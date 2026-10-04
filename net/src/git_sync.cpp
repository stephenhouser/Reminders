#include "reminders/git_sync.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <format>
#include <fstream>
#include <optional>
#include <vector>

#include "reminders/format.hpp"
#include "reminders/merge.hpp"
#include "reminders/settings.hpp"

extern char** environ;

namespace rem {

namespace {

// The top-level list files, as a pathspec relative to the folder.
constexpr std::string_view kLists = ":(glob)*.md";

struct Run {
		int status = -1;
		std::string out, err;
		bool ok() const { return status == 0; }
};

// Runs `git -C dir ARGS…` (no shell), with prompts turned off and messages
// in English, and collects what it prints.
Run git(const fs::path& dir, const std::vector<std::string>& args) {
	std::vector<std::string> argv{"git", "-C", dir.string()};
	argv.insert(argv.end(), args.begin(), args.end());
	std::vector<char*> cargv;
	for (auto& a : argv) {
		cargv.push_back(a.data());
	}
	cargv.push_back(nullptr);

	std::vector<std::string> env;
	for (char** e = environ; *e; ++e) {
		std::string_view v(*e);
		if (v.starts_with("LC_ALL=") || v.starts_with("GIT_TERMINAL_PROMPT=") ||
			v.starts_with("GIT_MERGE_AUTOEDIT=")) {
			continue;
		}
		env.emplace_back(v);
	}
	env.emplace_back("LC_ALL=C");
	env.emplace_back(
		"GIT_TERMINAL_PROMPT=0");  // no asking for a password: fail instead
	env.emplace_back("GIT_MERGE_AUTOEDIT=no");
	if (!std::getenv("GIT_SSH_COMMAND")) {
		env.emplace_back("GIT_SSH_COMMAND=ssh -o BatchMode=yes");
	}
	std::vector<char*> cenv;
	for (auto& e : env) {
		cenv.push_back(e.data());
	}
	cenv.push_back(nullptr);

	int out[2], err[2];
	if (pipe2(out, O_CLOEXEC) != 0) {
		throw SyncError("couldn't run git");
	}
	if (pipe2(err, O_CLOEXEC) != 0) {
		close(out[0]), close(out[1]);
		throw SyncError("couldn't run git");
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	posix_spawn_file_actions_adddup2(&fa, err[1], 2);
	pid_t pid = 0;
	int rc = posix_spawnp(&pid, "git", &fa, nullptr, cargv.data(), cenv.data());
	posix_spawn_file_actions_destroy(&fa);
	close(out[1]);
	close(err[1]);
	if (rc != 0) {
		close(out[0]), close(err[0]);
		throw SyncError("git isn't installed, or can't be run");
	}
	Run r;
	pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
	std::string* into[2] = {&r.out, &r.err};
	for (int open = 2; open > 0;) {
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		for (int i = 0; i < 2; ++i) {
			if (fds[i].fd < 0 || !fds[i].revents) {
				continue;
			}
			char buf[4096];
			auto n = read(fds[i].fd, buf, sizeof buf);
			if (n > 0) {
				into[i]->append(buf, static_cast<std::size_t>(n));
			} else if (n == 0 || errno != EINTR) {
				close(fds[i].fd);
				fds[i].fd = -1;
				--open;
			}
		}
	}
	for (auto& f : fds) {
		if (f.fd >= 0) {
			close(f.fd);
		}
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return r;
}

std::vector<std::string> lines_of(const std::string& text) {
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start < text.size()) {
		auto nl = text.find('\n', start);
		auto line = text.substr(
			start, nl == std::string::npos ? std::string::npos : nl - start);
		if (!line.empty()) {
			out.push_back(line);
		}
		if (nl == std::string::npos) {
			break;
		}
		start = nl + 1;
	}
	return out;
}

// git's message, as one line: "fatal: x" → "x".
std::string message(const Run& r) {
	auto lines = lines_of(r.err.empty() ? r.out : r.err);
	for (auto& l : lines) {
		for (std::string_view p : {"fatal: ", "error: "}) {
			if (l.starts_with(p)) {
				return l.substr(p.size());
			}
		}
	}
	return lines.empty() ? std::format("git failed (exit {})", r.status)
						 : lines.front();
}

std::string trimmed(std::string s) {
	while (!s.empty() &&
		   (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
		s.pop_back();
	}
	return s;
}

class GitSync {
	public:
		GitSync(const fs::path& folder, const GitSettings& settings,
				std::mutex& lock, const std::string& device)
			: folder_(folder),
			  settings_(settings),
			  lock_(lock),
			  device_(device) {}

		SyncResult run() {
			open_repository();
			remote_ = settings_.remote.empty() ? "origin" : settings_.remote;
			branch_ = settings_.branch;
			if (branch_.empty()) {
				auto head =
					git(folder_, {"symbolic-ref", "--short", "-q", "HEAD"});
				if (!head.ok()) {
					throw SyncError("the repository has no branch checked out");
				}
				branch_ = trimmed(head.out);
			}
			// Commits need a name; without one set up, this app's.
			auto email = git(folder_, {"config", "user.email"});
			if (!email.ok() || trimmed(email.out).empty()) {
				identity_ = {"-c", "user.name=Reminders", "-c",
							 std::format("user.email=reminders@{}", device_)};
			}

			{
				std::lock_guard guard(lock_);
				commit_local();
			}
			if (!git(folder_, {"remote", "get-url", remote_}).ok()) {
				return result_;	 // history only
			}

			for (int attempt = 0; attempt < 3; ++attempt) {
				auto fetch = git(folder_, {"-c", "http.lowSpeedLimit=1000",
										   "-c", "http.lowSpeedTime=60",
										   "fetch", "-q", remote_, branch_});
				bool remote_has_branch = fetch.ok();
				if (!fetch.ok() && fetch.err.find("couldn't find remote ref") ==
									   std::string::npos) {
					throw SyncError(message(fetch));
				}
				if (remote_has_branch) {
					std::lock_guard guard(lock_);
					commit_local();
					merge_fetched();
				}
				// Anything to send?
				if (!git(folder_, {"rev-parse", "-q", "--verify", "HEAD"})
						 .ok()) {
					return result_;	 // nothing either side
				}
				if (remote_has_branch) {
					auto ahead = git(
						folder_, {"rev-list", "--count", "FETCH_HEAD..HEAD"});
					if (ahead.ok() && trimmed(ahead.out) == "0") {
						return result_;
					}
				}
				auto push = git(folder_, {"-c", "http.lowSpeedLimit=1000", "-c",
										  "http.lowSpeedTime=60", "push", "-q",
										  "--no-verify", remote_,
										  "HEAD:refs/heads/" + branch_});
				if (push.ok()) {
					return result_;
				}
				auto text = push.err;
				if (text.find("[rejected]") == std::string::npos &&
					text.find("fetch first") == std::string::npos &&
					text.find("non-fast-forward") == std::string::npos) {
					throw SyncError(message(push));
				}
				// Someone pushed meanwhile: fetch and merge that too.
			}
			result_.errors.push_back("the remote kept changing; will retry");
			return result_;
		}

	private:
		const fs::path& folder_;
		const GitSettings& settings_;
		std::mutex& lock_;
		std::string device_;
		std::string remote_, branch_, top_, prefix_;
		std::vector<std::string> identity_;
		SyncResult result_;

		Run git_id(const fs::path& dir, std::vector<std::string> args) {
			args.insert(args.begin(), identity_.begin(), identity_.end());
			return git(dir, args);
		}

		// The working tree and the folder's place in it. A folder that isn't in
		// one yet is cloned from url=, or without one becomes a new repository
		// of its own (history only, until a remote is given).
		void open_repository() {
			std::error_code ec;
			fs::create_directories(folder_, ec);
			auto top = git(folder_, {"rev-parse", "--show-toplevel"});
			if (!top.ok()) {
				if (settings_.url.empty()) {
					auto init = git(folder_, {"init", "-q"});
					if (!init.ok()) {
						throw SyncError(
							std::format("couldn't make a git repository: {}",
										message(init)));
					}
				} else if (fs::is_empty(folder_, ec)) {
					auto clone =
						git(folder_, {"clone", "-q", settings_.url, "."});
					if (!clone.ok()) {
						throw SyncError(std::format("couldn't clone {}: {}",
													settings_.url,
													message(clone)));
					}
				} else {
					// Lists made here before the first sync: a repository
					// around them, on the remote's branch; merged with it
					// below.
					git(folder_, {"init", "-q"});
					git(folder_,
						{"remote", "add", remote_name(), settings_.url});
					auto head = git(folder_, {"ls-remote", "--symref",
											  settings_.url, "HEAD"});
					for (auto& line : lines_of(head.out)) {
						if (line.starts_with("ref: refs/heads/")) {
							git(folder_, {"symbolic-ref", "HEAD",
										  line.substr(5, line.find('\t') - 5)});
						}
					}
				}
				top = git(folder_, {"rev-parse", "--show-toplevel"});
				if (!top.ok()) {
					throw SyncError(message(top));
				}
			}
			top_ = trimmed(top.out);
			prefix_ = trimmed(git(folder_, {"rev-parse", "--show-prefix"}).out);
			// url= given to a repository without that remote (say, one made
			// here first): add it. One already there keeps its own address.
			if (!settings_.url.empty() &&
				!git(folder_, {"remote", "get-url", remote_name()}).ok()) {
				auto add = git(folder_,
							   {"remote", "add", remote_name(), settings_.url});
				if (!add.ok()) {
					throw SyncError(message(add));
				}
			}
		}

		std::string remote_name() const {
			return settings_.remote.empty() ? "origin" : settings_.remote;
		}

		// Commits the changed list files (lock held).
		void commit_local() {
			auto add = git(folder_, {"add", "-A", "--", std::string(kLists)});
			if (!add.ok()) {
				if (add.err.find("did not match any files") !=
					std::string::npos) {
					return;	 // no lists, here or in git
				}
				throw SyncError(message(add));
			}
			auto staged =
				git(folder_, {"diff", "--cached", "--name-only", "--relative",
							  "--", std::string(kLists)});
			auto files = lines_of(staged.out);
			if (files.empty()) {
				return;
			}
			std::string names;
			for (std::size_t i = 0; i < files.size() && i < 5; ++i) {
				auto stem = files[i].substr(0, files[i].size() - 3);
				names += (names.empty() ? "" : ", ") + stem;
			}
			if (files.size() > 5) {
				names += std::format(" and {} more", files.size() - 5);
			}
			auto commit = git_id(
				folder_, {"commit", "-q", "--no-verify", "-m",
						  std::format("Reminders ({}): {}", device_, names),
						  "--", std::string(kLists)});
			if (!commit.ok()) {
				throw SyncError(message(commit));
			}
		}

		// Merges FETCH_HEAD into the branch (lock held), and notes the lists
		// that changed.
		void merge_fetched() {
			auto old = git(folder_, {"rev-parse", "-q", "--verify", "HEAD"});
			if (old.ok() && git(folder_, {"merge-base", "--is-ancestor",
										  "FETCH_HEAD", "HEAD"})
								.ok()) {
				return;	 // nothing new
			}
			auto merge =
				git_id(folder_, {"merge", "-q", "--no-edit", "--no-verify",
								 "--allow-unrelated-histories", "FETCH_HEAD"});
			if (!merge.ok()) {
				auto unmerged = lines_of(
					git(top_, {"diff", "--name-only", "--diff-filter=U"}).out);
				if (unmerged.empty()) {
					throw SyncError(message(
						merge));  // e.g. changes here it would overwrite
				}
				resolve(unmerged);
			}
			auto changed =
				old.ok()
					? git(folder_,
						  {"diff", "--name-only", "--relative",
						   trimmed(old.out), "HEAD", "--", std::string(kLists)})
					: git(folder_, {"ls-files", "--", std::string(kLists)});
			for (auto& f : lines_of(changed.out)) {
				result_.changed.push_back(f.substr(0, f.size() - 3));
			}
		}

		// Conflicts after a merge: list files merged this app's way; anything
		// else undoes the merge.
		void resolve(const std::vector<std::string>& unmerged) {
			for (auto& path : unmerged) {
				auto rest = std::string_view(path).substr(
					path.starts_with(prefix_) ? prefix_.size() : 0);
				if (!path.starts_with(prefix_) ||
					rest.find('/') != std::string_view::npos ||
					!rest.ends_with(".md")) {
					abort_merge(path);
				}
				auto stage = [&](int n) -> std::optional<std::string> {
					auto r =
						git(top_, {"show", std::format(":{}:{}", n, path)});
					return r.ok() ? std::optional{r.out} : std::nullopt;
				};
				auto ours = stage(2), theirs = stage(3), base = stage(1);
				if (!ours && !theirs) {
					git(top_, {"rm", "-q", "--cached", "--", path});
					continue;
				}
				std::string text;
				if (!ours || !theirs) {
					text =
						ours ? *ours : *theirs;	 // deleted on one side, changed
												 // on the other: kept
				} else {
					auto o = parse(*ours), t = parse(*theirs);
					if (!o.is_list() && !t.is_list()) {
						abort_merge(path);
					}
					auto b = base ? std::optional{parse(*base)} : std::nullopt;
					text = serialize(merge(
						o, t, b ? &*b : nullptr));	// this device wins a field
													// changed on both
				}
				{
					std::ofstream out(fs::path(top_) / path,
									  std::ios::binary | std::ios::trunc);
					out << text;
					if (!out) {
						abort_merge(path);
					}
				}
				git(top_, {"add", "--", path});
			}
			auto commit =
				git_id(folder_, {"commit", "-q", "--no-edit", "--no-verify"});
			if (!commit.ok()) {
				git(folder_, {"merge", "--abort"});
				throw SyncError(message(commit));
			}
		}

		[[noreturn]] void abort_merge(const std::string& path) {
			git(folder_, {"merge", "--abort"});
			throw SyncError(
				std::format("{} changed here and on the remote and can't be "
							"merged as a list: merge it with git",
							path));
		}
};

}  // namespace

SyncResult git_sync(const fs::path& folder, const GitSettings& settings,
					std::mutex& lock, const std::string& device) {
	return GitSync(folder, settings, lock, device).run();
}

SyncResult sync_git_source(Store& store, const SourceConfig& source) {
	auto* backend = dynamic_cast<ServerBackend*>(&store.backend_object());
	if (!backend || backend->kind() != BackendKind::Git) {
		throw SyncError(std::format("{} isn't a git source", source.name));
	}
	// git sees renames and deletions in the files themselves: the back
	// end's notes of them aren't needed.
	std::error_code ec;
	fs::remove(backend->records_dir() / "renamed.tsv", ec);
	fs::remove(backend->records_dir() / "deleted.txt", ec);
	return git_sync(store.folder(), source.git, backend->lock(), device_name());
}

}  // namespace rem
