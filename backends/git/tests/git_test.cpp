// git tests against bare repositories made here. No user git configuration
// (so the app's own identity is used), and the same default branch
// everywhere.
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "reminders/backend_module.hpp"
#include "reminders/git_sync.hpp"
#include "reminders/library.hpp"
#include "test.hpp"

using namespace rem;

namespace {

std::string read(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

struct Dir {
		fs::path path =
			fs::temp_directory_path() / ("reminders-git-" + new_id());
		~Dir() {
			std::error_code ec;
			fs::remove_all(path, ec);
		}
};

// Runs once, before the tests.
const bool git_configured = [] {
	auto git_config =
		fs::temp_directory_path() / ("reminders-gitconfig-" + new_id());
	std::ofstream(git_config) << "[init]\n\tdefaultBranch = main\n";
	setenv("GIT_CONFIG_GLOBAL", git_config.c_str(), 1);
	setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
	return true;
}();

}  // namespace

namespace {

int sh(const std::string& cmd) {
	return std::system((cmd + " >/dev/null 2>&1").c_str());
}

std::string sh_out(const std::string& cmd) {
	std::string out;
	if (auto* p = popen(cmd.c_str(), "r")) {
		char buf[512];
		while (auto n = std::fread(buf, 1, sizeof buf, p)) {
			out.append(buf, n);
		}
		pclose(p);
	}
	return out;
}

#define LIST "---\nreminders: 1\n---\n"

}  // namespace

TEST(net_git_two_devices) {
	Dir dir;
	auto bare = dir.path / "remote.git";
	fs::create_directories(dir.path);
	CHECK_EQ(sh("git init -q --bare '" + bare.string() + "'"), 0);
	GitSettings s{bare.string(), "", "", 15};
	std::mutex la, lb;
	auto a = dir.path / "a", b = dir.path / "b";

	// A clones the empty repository, makes a list and sends it.
	auto r = git_sync(a, s, la, "dev-a");
	CHECK(r.errors.empty());
	std::ofstream(a / "Home.md")
		<< LIST "- [ ] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n";
	r = git_sync(a, s, la, "dev-a");
	CHECK(r.errors.empty());
	// B clones it.
	r = git_sync(b, s, lb, "dev-b");
	CHECK_EQ(read(b / "Home.md"), read(a / "Home.md"));

	// Both change the same line: git can't merge it, the list merge can.
	std::ofstream(a / "Home.md")
		<< LIST "- [x] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n";
	std::ofstream(b / "Home.md")
		<< LIST "- [ ] Paint #house ^aaaaaa\n- [ ] Sweep ^bbbbbb\n";
	CHECK(git_sync(a, s, la, "dev-a").errors.empty());
	r = git_sync(b, s, lb, "dev-b");
	CHECK(r.errors.empty());
	CHECK_EQ(r.changed.size(), std::size_t{1});
	r = git_sync(a, s, la, "dev-a");
	auto want =
		std::string(LIST "- [x] Paint #house ^aaaaaa\n- [ ] Sweep ^bbbbbb\n");
	CHECK_EQ(read(a / "Home.md"), want);
	CHECK_EQ(read(b / "Home.md"), want);
	CHECK(sh_out("git -C '" + b.string() + "' log --format=%s")
			  .find("Reminders (dev-b): Home") != std::string::npos);

	// Renames and deletions travel as they are.
	fs::rename(a / "Home.md", a / "House.md");
	CHECK(git_sync(a, s, la, "dev-a").errors.empty());
	CHECK(git_sync(b, s, lb, "dev-b").errors.empty());
	CHECK(!fs::exists(b / "Home.md"));
	CHECK_EQ(read(b / "House.md"), want);
}

TEST(net_git_only_the_lists) {
	Dir dir;
	auto repo = dir.path / "repo";
	fs::create_directories(repo / "todo" / "deeper");
	CHECK_EQ(sh("git -C '" + repo.string() + "' init -q"), 0);
	std::ofstream(repo / "notes.txt") << "mine\n";
	std::ofstream(repo / "todo" / "Work.md") << LIST "- [ ] Report\n";
	std::ofstream(repo / "todo" / "deeper" / "Other.md") << "not ours\n";
	std::mutex lock;
	// No remote: commits only, the history.
	auto r = git_sync(repo / "todo", GitSettings{}, lock, "dev");
	CHECK(r.errors.empty());
	auto tracked = sh_out("git -C '" + repo.string() + "' ls-files");
	CHECK_EQ(tracked, std::string("todo/Work.md\n"));
}

TEST(net_git_new_repository_then_a_remote) {
	Dir dir;
	fs::create_directories(dir.path);
	std::mutex lock;
	// Not a repository and nothing to clone: it becomes one (made if missing).
	auto plain = dir.path / "plain";
	CHECK(git_sync(plain, GitSettings{}, lock, "dev").errors.empty());
	CHECK(fs::exists(plain / ".git"));
	std::ofstream(plain / "Home.md") << LIST "- [ ] Paint ^aaaaaa\n";
	CHECK(git_sync(plain, GitSettings{}, lock, "dev").errors.empty());
	CHECK_EQ(sh_out("git -C '" + plain.string() + "' ls-files"),
			 std::string("Home.md\n"));
	// An address given later: the remote is added and the lists pushed.
	auto bare = dir.path / "remote.git";
	CHECK_EQ(sh("git init -q --bare '" + bare.string() + "'"), 0);
	GitSettings s{bare.string(), "", "", 15};
	CHECK(git_sync(plain, s, lock, "dev").errors.empty());
	CHECK_EQ(sh_out("git -C '" + plain.string() + "' remote get-url origin"),
			 bare.string() + "\n");
	auto other = dir.path / "other";
	std::mutex lock2;
	CHECK(git_sync(other, s, lock2, "dev2").errors.empty());
	CHECK_EQ(read(other / "Home.md"), read(plain / "Home.md"));
}

TEST(net_git_other_conflicts_stop_the_merge) {
	Dir dir;
	auto bare = dir.path / "remote.git";
	fs::create_directories(dir.path);
	sh("git init -q --bare '" + bare.string() + "'");
	GitSettings s{bare.string(), "", "", 15};
	std::mutex la, lb;
	auto a = dir.path / "a", b = dir.path / "b";
	git_sync(a, s, la, "dev-a");
	std::ofstream(a / "Notes.md") << "plain notes\n";
	git_sync(a, s, la, "dev-a");
	git_sync(b, s, lb, "dev-b");
	std::ofstream(a / "Notes.md") << "changed on a\n";
	std::ofstream(b / "Notes.md") << "changed on b\n";
	git_sync(a, s, la, "dev-a");
	bool threw = false;
	try {
		git_sync(b, s, lb, "dev-b");
	} catch (const SyncError& e) {
		threw = std::string_view(e.what()).find("Notes.md") !=
				std::string_view::npos;
	}
	CHECK(threw);
	CHECK(!fs::exists(b / ".git" / "MERGE_HEAD"));	// undone
	CHECK_EQ(read(b / "Notes.md"), std::string("changed on b\n"));
}

TEST(net_git_clone_into_lists_made_first) {
	Dir dir;
	auto bare = dir.path / "remote.git";
	fs::create_directories(dir.path);
	sh("git init -q --bare -b main '" + bare.string() + "'");
	GitSettings s{bare.string(), "", "", 15};
	std::mutex la, lc;
	auto a = dir.path / "a", c = dir.path / "c";
	git_sync(a, s, la, "dev-a");
	std::ofstream(a / "Home.md") << LIST "- [ ] Paint ^aaaaaa\n";
	git_sync(a, s, la, "dev-a");
	// C made lists before its first sync, one of them called Home too.
	fs::create_directories(c);
	std::ofstream(c / "Home.md") << LIST "- [ ] Hoover ^cccccc\n";
	std::ofstream(c / "Work.md") << LIST "- [ ] Report ^dddddd\n";
	auto r = git_sync(c, s, lc, "dev-c");
	CHECK(r.errors.empty());
	auto home = read(c / "Home.md");
	CHECK(home.find("Paint") != std::string::npos &&
		  home.find("Hoover") != std::string::npos);
	CHECK_EQ(sh_out("git -C '" + c.string() + "' symbolic-ref --short HEAD"),
			 std::string("main\n"));
	git_sync(a, s, la, "dev-a");
	CHECK(fs::exists(a / "Work.md"));
	CHECK_EQ(read(a / "Home.md"), home);
}

TEST(git_backend_entry) {
	register_git_backend();
	auto* git = find_backend("git");
	CHECK(git && git->owns_folder && !git->has_server && git->syncs());
	SourceConfig source{
		"notes", "git", {}, "", {{"url", "git@github.com:you/notes.git"}}};
	CHECK_EQ(git->name_hint(source), "notes");
	CHECK_EQ(git_settings(source).url, "git@github.com:you/notes.git");
	CHECK_EQ(git_settings(source).interval, 15);
}
