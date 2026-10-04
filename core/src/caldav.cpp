#include "reminders/caldav.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "file_util.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/merge.hpp"
#include "reminders/vtodo.hpp"

namespace rem {

namespace {

using namespace std::chrono;
using namespace detail;

constexpr std::string_view kEmptyList = "---\nreminders: 1\n---\n";

fs::path caldav_dir(const fs::path& state_dir) { return state_dir / "caldav"; }

// ── Records ─────────────────────────────────────────────────────────────

struct CalendarEntry {
		std::string list;  // the list's name now
		std::string
			synced_list;  // … as of the last sync (differs after a rename here)
		std::string href;
		std::string
			ctag;  // as of the last complete sync; empty: pull next time
		std::string remote_name;  // display name as of the last sync
		std::string remote_color;
};

std::vector<CalendarEntry> load_calendars(const fs::path& dir) {
	std::vector<CalendarEntry> out;
	for (auto& line : read_lines(dir / "calendars.tsv")) {
		auto f = split(line, '\t');
		if (f.size() < 6) {
			continue;
		}
		out.push_back({f[0], f[1], f[2], f[3], f[4], f[5]});
	}
	return out;
}

void save_calendars(const fs::path& dir,
					const std::vector<CalendarEntry>& entries) {
	std::string text;
	for (auto& e : entries) {
		text += std::format("{}\t{}\t{}\t{}\t{}\t{}\n", field(e.list),
							field(e.synced_list), field(e.href), field(e.ctag),
							field(e.remote_name), field(e.remote_color));
	}
	write_atomic(dir / "calendars.tsv", text);
}

struct ItemRecord {
		std::string uid, href, etag;
};
using Items = std::map<std::string, ItemRecord>;  // by reminder id

Items load_items(const fs::path& cdir) {
	Items out;
	for (auto& line : read_lines(cdir / "items.tsv")) {
		auto f = split(line, '\t');
		if (f.size() >= 4) {
			out[f[0]] = {f[1], f[2], f[3]};
		}
	}
	return out;
}

void save_items(const fs::path& cdir, const Items& items) {
	std::string text;
	for (auto& [id, r] : items) {
		text += std::format("{}\t{}\t{}\t{}\n", id, field(r.uid), field(r.href),
							field(r.etag));
	}
	write_atomic(cdir / "items.tsv", text);
}

// A calendar's records folder, named by a fingerprint of its href.
fs::path calendar_dir(const fs::path& dir, std::string_view href) {
	return dir / fingerprint(href);
}

// ── Helpers ─────────────────────────────────────────────────────────────

std::string new_uid() {
	static thread_local std::mt19937_64 rng{std::random_device{}()};
	auto a = rng(), b = rng();
	return std::format("{:08x}-{:04x}-4{:03x}-{:04x}-{:012x}", a >> 32,
					   (a >> 16) & 0xffff, a & 0xfff,
					   0x8000 | ((b >> 48) & 0x3fff), b & 0xffffffffffffULL);
}

bool usable_id(std::string_view s) {
	return s.size() >= 6 && std::ranges::all_of(s, [](char c) {
			   return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		   });
}

std::string fresh_id(std::string_view uid, std::vector<std::string>& taken) {
	std::string id(uid);
	if (!usable_id(id) || std::ranges::find(taken, id) != taken.end()) {
		do {
			id = new_id();
		} while (std::ranges::find(taken, id) != taken.end());
	}
	taken.push_back(id);
	return id;
}

std::vector<std::string> ids_in(Document& doc) {
	std::vector<std::string> out;
	doc.walk([&](Reminder& r, Reminder*) {
		if (!r.id.empty()) {
			out.push_back(r.id);
		}
	});
	return out;
}

// A file name for a list from a calendar's display name, not clashing with
// `used` or an existing file.
std::string unique_list_name(const fs::path& folder, std::string name,
							 const std::vector<std::string>& used) {
	std::ranges::replace(name, '/', '-');
	std::ranges::replace(name, '\\', '-');
	std::erase_if(name,
				  [](char c) { return c == '\n' || c == '\r' || c == '\t'; });
	while (!name.empty() && (name.front() == '.' || name.front() == ' ')) {
		name.erase(0, 1);
	}
	while (!name.empty() && name.back() == ' ') {
		name.pop_back();
	}
	if (name.empty()) {
		name = "List";
	}
	auto taken = [&](const std::string& n) {
		std::error_code ec;
		return std::ranges::find(used, n) != used.end() ||
			   fs::exists(folder / (n + ".md"), ec);
	};
	auto out = name;
	for (int n = 2; taken(out); ++n) {
		out = std::format("{} {}", name, n);
	}
	return out;
}

// Sort orders for siblings in their new order: existing values are kept
// while they still increase; others get a value between their neighbours
// (so moving one reminder rewrites only that one, when there's room).
// Siblings none of which has a value stay without.
std::vector<std::optional<long long>> assign_orders(
	const std::vector<std::optional<long long>>& have) {
	if (std::ranges::none_of(have, [](auto& v) { return v.has_value(); })) {
		return have;
	}
	std::vector<std::optional<long long>> out(have.size());
	std::optional<long long> prev;
	for (std::size_t i = 0; i < have.size(); ++i) {
		if (have[i] && (!prev || *have[i] > *prev)) {
			out[i] = prev = have[i];
			continue;
		}
		std::optional<long long> hi;
		for (auto j = i + 1; j < have.size() && !hi; ++j) {
			if (have[j] && (!prev || *have[j] > *prev)) {
				hi = have[j];
			}
		}
		long long v = prev && hi ? (*hi - *prev > 1 ? *prev + (*hi - *prev) / 2
													: *prev + 1)
					: prev		 ? *prev + 1024
					: hi		 ? *hi - 1024
								 : 0;
		out[i] = prev = v;
	}
	return out;
}

// ── One calendar ────────────────────────────────────────────────────────

struct Ctx {
		const fs::path& folder;
		fs::path dir;
		Remote& remote;
		std::mutex& lock;
		const time_zone* zone;
		sys_seconds now;
		SyncResult& result;
};

// The calendar objects of one calendar as the server last sent them.
class RawStore {
	public:
		explicit RawStore(fs::path cdir) : cdir_(std::move(cdir)) {}
		IcalComponent* get(const std::string& id) {
			if (auto it = cache_.find(id); it != cache_.end()) {
				return it->second ? &*it->second : nullptr;
			}
			auto text = read_file(cdir_ / (id + ".ics"));
			auto& slot = cache_[id];
			if (text) {
				slot = parse_ical(*text);
			}
			return slot ? &*slot : nullptr;
		}
		void put(const std::string& id, IcalComponent cal) {
			cache_[id] = std::move(cal);
			dirty_.insert(id);
			removed_.erase(id);
		}
		void drop(const std::string& id) {
			cache_[id].reset();
			dirty_.erase(id);
			removed_.insert(id);
		}
		void save() {
			for (auto& id : dirty_) {
				write_atomic(cdir_ / (id + ".ics"),
							 serialize_ical(*cache_[id]));
			}
			std::error_code ec;
			for (auto& id : removed_) {
				fs::remove(cdir_ / (id + ".ics"), ec);
			}
		}

	private:
		fs::path cdir_;
		std::map<std::string, std::optional<IcalComponent>> cache_;
		std::set<std::string> dirty_, removed_;
};

// Adds or updates the pulled tasks in `doc` (the server's version of the
// list). New tasks go where their sort order puts them; subtasks under
// their parent (deeper ones under the top parent: this app has one level).
// Existing reminders keep their place: moves aren't merged.
void apply_pulled(Document& doc,
				  std::vector<std::pair<std::string, Todo>>& changes,
				  const Items& items, RawStore& raw, const time_zone* zone) {
	std::map<std::string, std::string> id_of_uid;
	for (auto& [id, r] : items) {
		id_of_uid[r.uid] = id;
	}
	auto order_of = [&](const std::string& id) -> std::optional<long long> {
		auto* cal = raw.get(id);
		auto* t = cal ? todo_of(*cal) : nullptr;
		return t ? read_todo(*t, zone).sort_order : std::nullopt;
	};
	auto parent_id = [&](const Todo& t) -> std::optional<std::string> {
		if (!t.parent_uid) {
			return std::nullopt;
		}
		auto it = id_of_uid.find(*t.parent_uid);
		return it == id_of_uid.end() ? std::nullopt : std::optional{it->second};
	};
	// Parents first, so their subtasks have somewhere to go.
	std::ranges::stable_sort(changes, [&](auto& a, auto& b) {
		return !parent_id(a.second).has_value() &&
			   parent_id(b.second).has_value();
	});

	for (auto& [id, t] : changes) {
		if (auto* existing = doc.find(id)) {
			existing->fields() = t.reminder.fields();
			existing->notes = t.reminder.notes;
			continue;
		}
		Reminder r = t.reminder;
		r.id = id;
		Reminder* parent = nullptr;
		if (auto pid = parent_id(t)) {
			Reminder* top = nullptr;
			parent = doc.find(*pid, &top);
			if (top) {
				parent = top;
			}
		}
		auto mine = t.sort_order;
		auto before = [&](const Reminder& other) {
			auto o = order_of(other.id);
			return mine && o && *o > *mine;
		};
		if (parent) {
			auto& subs = parent->subtasks;
			auto at = std::ranges::find_if(subs, before);
			subs.insert(at, std::move(r));
			continue;
		}
		std::optional<std::string> next;
		for (auto& s : doc.sections()) {
			if (s.name == t.section) {
				for (auto* other : s.reminders) {
					if (!next && before(*other)) {
						next = other->id;
					}
				}
			}
		}
		doc.insert(std::move(r), nullptr, t.section);
		if (next) {
			doc.move_before(id, *next);
		}
	}
}

void sync_calendar(Ctx& cx, CalendarEntry& e, const RemoteCalendar& rc) {
	auto cdir = calendar_dir(cx.dir, e.href);
	auto items = load_items(cdir);
	auto base_text = read_file(cdir / "base.md");
	std::optional<std::string> local_text;
	{
		std::lock_guard guard(cx.lock);
		local_text = read_file(cx.folder / (e.list + ".md"));
	}

	bool remote_renamed = rc.name != e.remote_name;
	bool local_renamed = e.list != e.synced_list;
	bool pull = !base_text || rc.ctag.empty() || rc.ctag != e.ctag;
	bool push = local_text && local_text != base_text;
	if (!pull && !push && !remote_renamed && !local_renamed &&
		rc.color == e.remote_color) {
		return;
	}

	RawStore raw(cdir);
	Document theirs = parse(base_text.value_or(std::string(kEmptyList)));
	Document ours = local_text ? parse(*local_text) : theirs;
	std::vector<std::string> taken = ids_in(theirs);
	for (auto& id : ids_in(ours)) {
		taken.push_back(id);
	}
	for (auto& [id, r] : items) {
		taken.push_back(id);
	}

	// Pull: the server's version of the list.
	if (pull) {
		auto listed = cx.remote.items(e.href);
		std::map<std::string, std::string> id_of_href;
		for (auto& [id, r] : items) {
			id_of_href[r.href] = id;
		}
		std::set<std::string> listed_hrefs;
		std::vector<std::string> wanted;
		for (auto& it : listed) {
			listed_hrefs.insert(it.href);
			auto f = id_of_href.find(it.href);
			if (f == id_of_href.end() || it.etag.empty() ||
				items[f->second].etag != it.etag) {
				wanted.push_back(it.href);
			}
		}
		for (auto it = items.begin(); it != items.end();) {
			if (listed_hrefs.contains(it->second.href)) {
				++it;
				continue;
			}
			theirs.remove(it->first);
			raw.drop(it->first);
			it = items.erase(it);
		}
		std::vector<std::pair<std::string, Todo>> changes;
		if (!wanted.empty()) {
			for (auto& o : cx.remote.fetch(e.href, wanted)) {
				auto cal = parse_ical(o.data);
				auto* vtodo = cal ? todo_of(*cal) : nullptr;
				if (!vtodo) {
					continue;
				}
				auto t = read_todo(*vtodo, cx.zone);
				std::string id;
				if (auto f = id_of_href.find(o.href); f != id_of_href.end()) {
					id = f->second;
				} else {
					id = fresh_id(t.uid, taken);
				}
				items[id] = {t.uid, o.href, o.etag};
				raw.put(id, std::move(*cal));
				changes.emplace_back(id, std::move(t));
			}
		}
		apply_pulled(theirs, changes, items, raw, cx.zone);
	}
	if (!rc.color.empty()) {
		theirs.set_meta("color", color_from_hex(rc.color));
	}
	auto theirs_text = serialize(theirs);

	// Merge with what changed here since the last sync.
	Document merged = theirs;
	if (local_text) {
		auto base = base_text ? std::optional{parse(*base_text)} : std::nullopt;
		merged = merge(ours, theirs, base ? &*base : nullptr);
	}
	merged.ensure_ids(taken);
	auto merged_text = serialize(merged);

	// Write the list (renamed first if the calendar was renamed on the
	// server and not here), unless it changed while we were busy.
	{
		std::lock_guard guard(cx.lock);
		auto path = cx.folder / (e.list + ".md");
		if (read_file(path) != local_text) {
			e.ctag.clear();	 // pull again next time
			return;
		}
		if (remote_renamed && !local_renamed) {
			std::vector<std::string> used;
			auto name = rc.name == e.list
						  ? e.list
						  : unique_list_name(cx.folder, rc.name, used);
			if (name != e.list) {
				std::error_code ec;
				if (local_text) {
					fs::rename(path, cx.folder / (name + ".md"), ec);
				}
				cx.result.changed.push_back(e.list);
				e.list = e.synced_list = name;
				path = cx.folder / (name + ".md");
			}
		}
		if (merged_text != local_text) {
			write_atomic(path, merged_text);
			cx.result.changed.push_back(e.list);
		}
	}

	// Push what differs from the server's version.
	bool failed = false;
	std::set<std::string> present;
	auto push_one = [&](const Reminder& r, const Reminder* parent,
						const std::optional<std::string>& section,
						std::optional<long long> order) {
		present.insert(r.id);
		Todo want;
		want.reminder = r;
		want.reminder.subtasks.clear();
		want.section = section;
		want.sort_order = order;
		if (parent) {
			auto pit = items.find(parent->id);
			if (pit ==
				items.end()) {	// the parent couldn't be sent: wait for it
				failed = true;
				return;
			}
			want.parent_uid = pit->second.uid;
		}
		auto it = items.find(r.id);
		auto* cal = it != items.end() ? raw.get(r.id) : nullptr;
		if (cal && todo_of(*cal)) {
			IcalComponent copy = *cal;
			if (!write_todo(*todo_of(copy), want, cx.now, cx.zone)) {
				return;
			}
			auto etag = cx.remote.put(it->second.href, serialize_ical(copy),
									  it->second.etag);
			if (!etag) {
				failed = true;
				return;
			}
			it->second.etag = *etag;
			raw.put(r.id, std::move(copy));
			return;
		}
		want.uid = it != items.end() ? it->second.uid : new_uid();
		auto href =
			it != items.end() ? it->second.href : e.href + want.uid + ".ics";
		auto created = new_todo_calendar(want, cx.now, cx.zone);
		auto etag = cx.remote.put(href, serialize_ical(created),
								  it != items.end() ? it->second.etag : "");
		if (!etag) {
			failed = true;
			return;
		}
		items[r.id] = {want.uid, href, *etag};
		raw.put(r.id, std::move(created));
	};
	auto stored_order = [&](const std::string& id) -> std::optional<long long> {
		auto* cal = items.contains(id) ? raw.get(id) : nullptr;
		auto* t = cal ? todo_of(*cal) : nullptr;
		return t ? read_todo(*t, cx.zone).sort_order : std::nullopt;
	};
	std::vector<Reminder*> tops = merged.reminders();
	std::vector<std::optional<long long>> have;
	for (auto* r : tops) {
		have.push_back(stored_order(r->id));
	}
	auto orders = assign_orders(have);
	for (std::size_t i = 0; i < tops.size(); ++i) {
		auto& r = *tops[i];
		push_one(r, nullptr, merged.section_of(r), orders[i]);
		std::vector<std::optional<long long>> sub_have;
		for (auto& s : r.subtasks) {
			sub_have.push_back(stored_order(s.id));
		}
		auto sub_orders = assign_orders(sub_have);
		for (std::size_t j = 0; j < r.subtasks.size(); ++j) {
			push_one(r.subtasks[j], &r, std::nullopt, sub_orders[j]);
		}
	}
	for (auto it = items.begin(); it != items.end();) {
		if (present.contains(it->first)) {
			++it;
			continue;
		}
		if (!cx.remote.remove(it->second.href, it->second.etag)) {
			failed = true;
			++it;
			continue;
		}
		raw.drop(it->first);
		it = items.erase(it);
	}

	// The calendar's name and colour.
	auto color = merged.meta("color").value_or("");
	bool color_changed = !color.empty() && color != color_from_hex(rc.color);
	auto remote_name = rc.name, remote_color = rc.color;
	if ((local_renamed && e.list != rc.name) || color_changed) {
		remote_name = local_renamed ? e.list : rc.name;
		remote_color = color_changed ? hex_from_color(color) : rc.color;
		cx.remote.update_calendar(e.href, remote_name, remote_color);
	}

	raw.save();
	save_items(cdir, items);
	write_atomic(cdir / "base.md", failed ? theirs_text : merged_text);
	e.ctag = failed ? "" : rc.ctag;
	e.remote_name = remote_name;
	e.remote_color = remote_color;
	e.synced_list = e.list;
	if (failed) {
		cx.result.errors.push_back(
			e.list + ": changed on the server meanwhile; will retry");
	}
}

// ── Colours ─────────────────────────────────────────────────────────────

struct Rgb {
		std::string_view name;
		int r, g, b;
};

// Close to Apple's system colours, which many CalDAV clients use too.
constexpr std::array<Rgb, 11> kRgb{{{"red", 0xFF, 0x3B, 0x30},
									{"orange", 0xFF, 0x95, 0x00},
									{"yellow", 0xFF, 0xCC, 0x00},
									{"green", 0x34, 0xC7, 0x59},
									{"cyan", 0x32, 0xAD, 0xE6},
									{"blue", 0x00, 0x7A, 0xFF},
									{"indigo", 0x58, 0x56, 0xD6},
									{"purple", 0xAF, 0x52, 0xDE},
									{"pink", 0xFF, 0x2D, 0x55},
									{"brown", 0xA2, 0x84, 0x5E},
									{"gray", 0x8E, 0x8E, 0x93}}};

}  // namespace

std::string color_from_hex(std::string_view hex) {
	if (hex.starts_with('#')) {
		hex.remove_prefix(1);
	}
	if (hex.size() < 6) {
		return "";
	}
	auto byte = [&](std::size_t at) {
		return std::stoi(std::string(hex.substr(at, 2)), nullptr, 16);
	};
	int r = 0, g = 0, b = 0;
	try {
		r = byte(0), g = byte(2), b = byte(4);
	} catch (const std::exception&) {
		return "";
	}
	auto best = kRgb[0].name;
	long best_d = -1;
	for (auto& c : kRgb) {
		long d = (r - c.r) * (r - c.r) + (g - c.g) * (g - c.g) +
				 (b - c.b) * (b - c.b);
		if (best_d < 0 || d < best_d) {
			best_d = d, best = c.name;
		}
	}
	return std::string(best);
}

std::string hex_from_color(std::string_view color) {
	for (auto& c : kRgb) {
		if (c.name == color) {
			return std::format("#{:02X}{:02X}{:02X}", c.r, c.g, c.b);
		}
	}
	return "";
}

SyncResult caldav_sync(const fs::path& folder, const fs::path& state_dir,
					   Remote& remote, std::mutex& lock,
					   const SyncOptions& options) {
	SyncResult result;
	auto dir = caldav_dir(state_dir);
	fs::create_directories(folder);
	fs::create_directories(dir);
	Ctx cx{folder,
		   dir,
		   remote,
		   lock,
		   options.zone ? options.zone : current_zone(),
		   options.now != sys_seconds{} ? options.now
										: floor<seconds>(system_clock::now()),
		   result};

	auto remote_cals =
		remote.calendars();	 // throws if the server can't be reached

	// Renames and deletions made in the app since the last sync.
	std::vector<CalendarEntry> entries;
	std::vector<std::string> deleted;
	{
		std::lock_guard guard(lock);
		entries = load_calendars(dir);
		for (auto& line : read_lines(dir / "renamed.tsv")) {
			auto f = split(line, '\t');
			if (f.size() < 2) {
				continue;
			}
			for (auto& e : entries) {
				if (e.list == f[0]) {
					e.list = f[1];
				}
			}
		}
		deleted = read_lines(dir / "deleted.txt");
		save_calendars(dir, entries);
		std::error_code ec;
		fs::remove(dir / "renamed.tsv", ec);
		fs::remove(dir / "deleted.txt", ec);
	}
	auto find_remote = [&](const std::string& href) -> const RemoteCalendar* {
		for (auto& rc : remote_cals) {
			if (rc.href == href) {
				return &rc;
			}
		}
		return nullptr;
	};
	auto run = [&](CalendarEntry& e, const RemoteCalendar& rc) {
		try {
			sync_calendar(cx, e, rc);
		} catch (const SyncError& err) {
			e.ctag.clear();
			result.errors.push_back(e.list + ": " + err.what());
		}
	};

	std::vector<CalendarEntry> kept;
	for (auto& e : entries) {
		auto path = folder / (e.list + ".md");
		auto cdir = calendar_dir(dir, e.href);
		std::error_code ec;
		auto* rc = find_remote(e.href);
		if (!rc) {
			// Gone from the server: the list goes too, unless it has changes
			// made here since (then it becomes a new calendar, below).
			std::lock_guard guard(lock);
			auto local = read_file(path);
			if (local && local == read_file(cdir / "base.md")) {
				fs::remove(path, ec);
				result.changed.push_back(e.list);
			}
			fs::remove_all(cdir, ec);
			continue;
		}
		if (!fs::exists(path, ec)) {
			if (std::ranges::find(deleted, e.list) != deleted.end()) {
				try {
					remote.delete_calendar(e.href);
					std::erase_if(remote_cals,
								  [&](auto& c) { return c.href == e.href; });
					fs::remove_all(cdir, ec);
					continue;
				} catch (const SyncError& err) {
					result.errors.push_back(e.list + ": " + err.what());
					kept.push_back(e);
					std::lock_guard guard(lock);
					std::ofstream(dir / "deleted.txt", std::ios::app)
						<< field(e.list) << '\n';
					continue;
				}
			}
			// The file went missing without the app deleting it: fetch it
			// again.
			fs::remove_all(cdir, ec);
			e.ctag.clear();
			e.synced_list = e.list;
		}
		run(e, *rc);
		kept.push_back(e);
	}

	// Calendars new on the server.
	for (auto& rc : remote_cals) {
		if (std::ranges::any_of(kept,
								[&](auto& e) { return e.href == rc.href; })) {
			continue;
		}
		std::vector<std::string> used;
		for (auto& e : kept) {
			used.push_back(e.list);
		}
		std::string name;
		{
			std::lock_guard guard(lock);
			name = unique_list_name(folder, rc.name, used);
		}
		CalendarEntry e{name, name, rc.href, "", rc.name, ""};
		run(e, rc);
		kept.push_back(e);
	}

	// Lists new here.
	std::vector<std::pair<std::string, std::string>> fresh;	 // name, colour
	{
		std::lock_guard guard(lock);
		// Lists renamed in the app while this ran aren't new.
		std::vector<std::string> renamed_to;
		for (auto& line : read_lines(dir / "renamed.tsv")) {
			if (auto f = split(line, '\t'); f.size() >= 2) {
				renamed_to.push_back(f[1]);
			}
		}
		std::error_code ec;
		for (auto& f : fs::directory_iterator(folder, ec)) {
			auto fname = f.path().filename().string();
			if (!f.is_regular_file() || fname.starts_with('.') ||
				!fname.ends_with(".md")) {
				continue;
			}
			auto name = fname.substr(0, fname.size() - 3);
			if (std::ranges::any_of(kept,
									[&](auto& e) { return e.list == name; }) ||
				std::ranges::find(renamed_to, name) != renamed_to.end()) {
				continue;
			}
			auto text = read_file(f.path());
			if (!text) {
				continue;
			}
			auto doc = parse(*text);
			if (doc.is_list()) {
				fresh.emplace_back(name, doc.meta("color").value_or(""));
			}
		}
	}
	for (auto& [name, color] : fresh) {
		try {
			auto hex = hex_from_color(color);
			auto href = remote.create_calendar(name, hex);
			CalendarEntry e{name, name, href, "", name, hex};
			run(e, RemoteCalendar{href, name, hex, ""});
			kept.push_back(e);
		} catch (const SyncError& err) {
			result.errors.push_back(name + ": " + err.what());
		}
	}

	{
		// Renames made in the app while this ran are applied next time.
		std::lock_guard guard(lock);
		save_calendars(dir, kept);
	}
	return result;
}

}  // namespace rem
