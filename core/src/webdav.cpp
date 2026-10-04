#include "reminders/webdav.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "file_util.hpp"
#include "reminders/format.hpp"
#include "reminders/merge.hpp"

namespace rem {

namespace {

using namespace detail;

struct Entry {
		std::string list;	 // the list's name here
		std::string synced;	 // its name on the server (differs until a rename
							 // made here is sent)
		std::string etag;	 // as of the last sync; empty: not known
};

std::vector<Entry> load_entries(const fs::path& dir) {
	std::vector<Entry> out;
	for (auto& line : read_lines(dir / "files.tsv")) {
		auto f = split(line, '\t');
		if (f.size() >= 3 && !f[0].empty() && !f[1].empty()) {
			out.push_back({f[0], f[1], f[2]});
		}
	}
	return out;
}

void save_entries(const fs::path& dir, const std::vector<Entry>& entries) {
	std::string text;
	for (auto& e : entries) {
		text += std::format("{}\t{}\t{}\n", field(e.list), field(e.synced),
							field(e.etag));
	}
	write_atomic(dir / "files.tsv", text);
}

class Sync {
	public:
		Sync(const fs::path& folder, const fs::path& state_dir,
			 FileRemote& remote, std::mutex& lock)
			: folder_(folder),
			  dir_(state_dir / "webdav"),
			  remote_(remote),
			  lock_(lock) {}

		SyncResult run() {
			fs::create_directories(folder_);
			fs::create_directories(dir_);
			for (auto& f : remote_.files()) {
				on_server_[f.name] =
					f.etag;	 // throws if the server can't be reached
			}

			// Renames and deletions made in the app since the last sync.
			std::vector<std::string> deleted;
			{
				std::lock_guard guard(lock_);
				entries_ = load_entries(dir_);
				for (auto& line : read_lines(dir_ / "renamed.tsv")) {
					auto f = split(line, '\t');
					if (f.size() < 2) {
						continue;
					}
					for (auto& e : entries_) {
						if (e.list == f[0]) {
							e.list = f[1];
						}
					}
				}
				deleted = read_lines(dir_ / "deleted.txt");
				save_entries(dir_, entries_);
				std::error_code ec;
				fs::remove(dir_ / "renamed.tsv", ec);
				fs::remove(dir_ / "deleted.txt", ec);
			}

			send_renames();

			std::vector<Entry> kept;
			for (auto& e : entries_) {
				// A rename that couldn't be sent, or renamed or deleted in the
				// app while this ran: next time.
				if (unsent_.contains(e.list) || pending(e.list)) {
					kept.push_back(e);
					continue;
				}
				bool user_deleted =
					std::ranges::find(deleted, e.list) != deleted.end();
				try {
					if (e.list != e.synced ? sync_new(e)
										   : sync_entry(e, user_deleted)) {
						kept.push_back(e);
					}
				} catch (const SyncError& err) {
					result_.errors.push_back(e.list + ": " + err.what());
					kept.push_back(e);
					if (user_deleted) {
						std::lock_guard guard(lock_);
						std::ofstream(dir_ / "deleted.txt", std::ios::app)
							<< field(e.list) << '\n';
					}
				}
			}

			// Files new on the server, and lists new here.
			std::set<std::string> names;
			for (auto& [name, etag] : on_server_) {
				names.insert(name);
			}
			{
				std::lock_guard guard(lock_);
				std::error_code ec;
				for (auto& f : fs::directory_iterator(folder_, ec)) {
					auto fname = f.path().filename().string();
					if (f.is_regular_file() && !fname.starts_with('.') &&
						fname.ends_with(".md") && fname.size() > 3) {
						names.insert(fname.substr(0, fname.size() - 3));
					}
				}
			}
			for (auto& name : names) {
				if (std::ranges::any_of(kept,
										[&](auto& e) {
											return e.list == name ||
												   e.synced == name;
										}) ||
					pending(name)) {
					continue;
				}
				Entry e{name, name, ""};
				try {
					if (sync_new(e)) {
						kept.push_back(e);
					}
				} catch (const SyncError& err) {
					result_.errors.push_back(name + ": " + err.what());
				}
			}

			{
				std::lock_guard guard(lock_);
				save_entries(dir_, kept);
			}
			return result_;
		}

	private:
		const fs::path& folder_;
		fs::path dir_;
		FileRemote& remote_;
		std::mutex& lock_;
		std::map<std::string, std::string> on_server_;	// name → ETag
		std::vector<Entry> entries_;
		std::set<std::string>
			unsent_;  // lists whose rename couldn't be sent this time
		SyncResult result_;

		fs::path path_of(const std::string& name) const {
			return folder_ / (name + ".md");
		}
		fs::path base_path(const std::string& synced) const {
			return dir_ / "base" / (synced + ".md");
		}

		std::optional<std::string> read_local(const std::string& name) {
			std::lock_guard guard(lock_);
			return read_file(path_of(name));
		}

		// Writes a list file (or with nullopt deletes it), unless it changed
		// since it read `expected`: then the next sync takes it up.
		bool write_local(const std::string& name,
						 const std::optional<std::string>& expected,
						 const std::optional<std::string>& text) {
			std::lock_guard guard(lock_);
			if (read_file(path_of(name)) != expected) {
				return false;
			}
			std::error_code ec;
			if (text) {
				write_atomic(path_of(name), *text);
			} else {
				fs::remove(path_of(name), ec);
			}
			result_.changed.push_back(name);
			return true;
		}

		// Renamed or deleted in the app since this sync began.
		bool pending(const std::string& name) {
			std::lock_guard guard(lock_);
			for (auto& line : read_lines(dir_ / "renamed.tsv")) {
				if (std::ranges::contains(split(line, '\t'), name)) {
					return true;
				}
			}
			return std::ranges::contains(read_lines(dir_ / "deleted.txt"),
										 name);
		}

		void record(Entry& e, std::string etag, const std::string& text) {
			e.etag = std::move(etag);
			write_atomic(base_path(e.synced), text);
		}

		void drop_base(const std::string& synced) {
			std::error_code ec;
			fs::remove(base_path(synced), ec);
		}

		void retry(const std::string& name) {
			result_.errors.push_back(
				name + ": changed on the server meanwhile; will retry");
		}

		// Lists renamed here: their files on the server follow (MOVE). Done in
		// rounds, so a rename into a name another one is leaving waits for it;
		// a cycle (A↔B) goes through a temporary name. A file gone from the
		// server just takes the new name in the records (and is then treated as
		// gone). One whose new name is taken there by another file is synced as
		// a new list, merged with that file (sync_new, as e.list != e.synced).
		// One that fails is tried again next time (unsent_).
		void send_renames() {
			auto moving = [&](const Entry& e) {
				return e.list != e.synced && on_server_.contains(e.synced);
			};
			auto apply = [&](Entry& e, const std::string& to) {
				on_server_[to] = on_server_[e.synced];
				on_server_.erase(e.synced);
				std::error_code ec;
				if (fs::exists(base_path(e.synced), ec)) {
					fs::rename(base_path(e.synced), base_path(to), ec);
				}
				e.synced = to;
				std::lock_guard guard(lock_);
				save_entries(dir_, entries_);
			};
			std::set<std::string>
				aside;	// moved to a temporary name (once each)
			for (bool progress = true; progress;) {
				progress = false;
				for (auto& e : entries_) {
					if (!moving(e) || on_server_.contains(e.list) ||
						unsent_.contains(e.list)) {
						continue;
					}
					try {
						if (remote_.move(e.synced, e.list)) {
							apply(e, e.list);
							progress = true;
						} else {
							unsent_.insert(e.list);
						}
					} catch (const SyncError& err) {
						result_.errors.push_back(e.list + ": " + err.what());
						unsent_.insert(e.list);
					}
				}
				if (progress) {
					continue;
				}
				// Blocked only by another file that's leaving its name: move
				// one aside to break the cycle.
				for (auto& e : entries_) {
					if (!moving(e) || unsent_.contains(e.list) ||
						aside.contains(e.list) ||
						!std::ranges::any_of(entries_, [&](auto& o) {
							return moving(o) && o.synced == e.list;
						})) {
						continue;
					}
					auto temp = e.synced + " (renaming)";
					for (int n = 2; on_server_.contains(temp); ++n) {
						temp = std::format("{} (renaming {})", e.synced, n);
					}
					try {
						if (!remote_.move(e.synced, temp)) {
							continue;
						}
					} catch (const SyncError& err) {
						result_.errors.push_back(e.list + ": " + err.what());
						unsent_.insert(e.list);
						continue;
					}
					apply(e, temp);
					aside.insert(e.list);
					progress = true;
					break;
				}
			}
			for (auto& e : entries_) {
				if (e.list == e.synced || unsent_.contains(e.list) ||
					on_server_.contains(e.synced)) {
					continue;
				}
				// Gone from the server: the records follow the rename.
				std::error_code ec;
				if (fs::exists(base_path(e.synced), ec)) {
					fs::rename(base_path(e.synced), base_path(e.list), ec);
				}
				e.synced = e.list;
			}
			// The rest (the new name taken on the server) become new lists.
			for (auto& e : entries_) {
				if (e.list != e.synced && !unsent_.contains(e.list) &&
					on_server_.contains(e.list)) {
					drop_base(e.synced);
					e.etag.clear();
				}
			}
			std::lock_guard guard(lock_);
			save_entries(dir_, entries_);
		}

		// Merges two versions of a file. Files that aren't lists can't be: the
		// server's version wins, and this one is kept beside it as
		// "NAME (this device).md" (which stays here, not being a list).
		std::string merge_texts(const std::string& name,
								const std::string& ours,
								const std::string& theirs,
								const std::optional<std::string>& base) {
			auto o = parse(ours), t = parse(theirs);
			if (o.is_list() || t.is_list()) {
				auto b = base ? std::optional{parse(*base)} : std::nullopt;
				return serialize(merge(o, t, b ? &*b : nullptr));
			}
			auto aside = name + " (this device)";
			{
				std::lock_guard guard(lock_);
				std::error_code ec;
				for (int n = 2; fs::exists(path_of(aside), ec); ++n) {
					aside = std::format("{} (this device {})", name, n);
				}
			}
			write_local(aside, std::nullopt, ours);
			return theirs;
		}

		// Writes the merged version here and sends it to the server.
		void finish(Entry& e, const std::optional<std::string>& local,
					const RemoteText& theirs, const std::string& merged) {
			if (merged != local && !write_local(e.list, local, merged)) {
				return;	 // changed meanwhile: next time
			}
			if (merged == theirs.text) {
				record(e, theirs.etag, theirs.text);
				return;
			}
			auto etag = remote_.put(e.synced, merged,
									theirs.etag.empty() ? "*" : theirs.etag);
			if (!etag) {
				// The base stays the server's version, to merge with again.
				record(e, theirs.etag, theirs.text);
				retry(e.list);
				return;
			}
			record(e, *etag, merged);
		}

		// A file without records: on one side only, or on both (first sync,
		// records lost, or a name taken on the server by a rename here).
		// Returns whether it's kept.
		bool sync_new(Entry& e) {
			e.synced = e.list;
			e.etag.clear();
			auto local = read_local(e.list);
			if (!on_server_.contains(e.list)) {
				if (!local || !parse(*local).is_list()) {
					return false;
				}
				auto etag = remote_.put(e.list, *local, "");
				if (!etag) {
					retry(e.list);
					return false;
				}
				record(e, *etag, *local);
				return true;
			}
			auto theirs = remote_.get(e.list);
			if (!theirs) {
				return false;  // gone meanwhile
			}
			if (!local) {
				if (!write_local(e.list, std::nullopt, theirs->text)) {
					return false;
				}
				record(e, theirs->etag, theirs->text);
				return true;
			}
			if (*local == theirs->text) {
				record(e, theirs->etag, theirs->text);
				return true;
			}
			finish(e, local, *theirs,
				   merge_texts(e.list, *local, theirs->text, std::nullopt));
			return true;
		}

		// A file synced before. Returns whether it's kept.
		bool sync_entry(Entry& e, bool user_deleted) {
			auto& name = e.list;
			auto base = read_file(base_path(name));
			if (!base) {
				return sync_new(e);
			}
			auto local = read_local(name);
			auto on = on_server_.find(name);

			if (on == on_server_.end()) {
				// Gone from the server: the list goes too, unless it changed
				// here since.
				if (!local || local == base) {
					if (local && !write_local(name, local, std::nullopt)) {
						return true;
					}
					drop_base(name);
					return false;
				}
				auto etag = remote_.put(name, *local, "");
				if (!etag) {
					retry(name);
					return true;
				}
				record(e, *etag, *local);
				return true;
			}

			bool remote_changed = on->second.empty() || on->second != e.etag;
			if (!local) {
				if (user_deleted && !remote_changed &&
					remote_.remove(name, e.etag)) {
					drop_base(name);
					return false;
				}
				// Gone from here without the app deleting it, or deleted here
				// but changed on the server since: fetch it again.
				auto theirs = remote_.get(name);
				if (!theirs) {
					drop_base(name);
					return false;
				}
				if (write_local(name, std::nullopt, theirs->text)) {
					record(e, theirs->etag, theirs->text);
				}
				return true;
			}

			bool local_changed = *local != *base;
			if (!remote_changed && !local_changed) {
				return true;
			}
			if (!remote_changed) {
				auto etag = remote_.put(name, *local, e.etag);
				if (!etag) {
					retry(name);  // changed there after all: merged next time
					return true;
				}
				record(e, *etag, *local);
				return true;
			}
			auto theirs = remote_.get(name);
			if (!theirs) {
				return true;  // gone meanwhile: next time
			}
			auto merged = local_changed
							? merge_texts(name, *local, theirs->text, base)
							: theirs->text;
			finish(e, local, *theirs, merged);
			return true;
		}
};

}  // namespace

SyncResult webdav_sync(const fs::path& folder, const fs::path& state_dir,
					   FileRemote& remote, std::mutex& lock) {
	return Sync(folder, state_dir, remote, lock).run();
}

}  // namespace rem
