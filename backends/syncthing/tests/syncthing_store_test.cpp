// The Syncthing back end in a Store: merging conflict copies, and its
// per-list records (merge base, own writes).
#include <fstream>
#include <sstream>

#include "reminders/store.hpp"
#include "reminders/syncthing.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}

struct TempDir {
		fs::path path;
		TempDir() {
			path = fs::temp_directory_path() / ("reminders-test-" + new_id());
			fs::create_directories(path / "sync");
		}
		~TempDir() { fs::remove_all(path); }
		fs::path sync() const { return path / "sync"; }
		fs::path state() const { return path / "state"; }
};

std::string read(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void write(const fs::path& p, std::string_view text) {
	std::ofstream(p) << text;
}

}  // namespace

// Front matter marking a file as a list.
#define MARK "---\nreminders: 1\n---\n"

// Every test here uses the real Syncthing back end.
const bool registered = (register_syncthing_backend(), true);

TEST(syncthing_conflict_is_merged_with_base) {
	TempDir t;
	auto file = t.sync() / "Groceries.md";
	write(file, MARK "- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n");
	Store s(t.sync(), t.state());
	s.load_all();  // this version becomes the base

	// Local edit: complete Milk.
	s.set_done("milk01", true, d(2026, 10, 1));
	auto local = read(file);

	// Meanwhile another device flagged Eggs. Syncthing picks the remote
	// version and keeps ours as a conflict copy.
	fs::rename(file,
			   t.sync() / "Groceries.sync-conflict-20261001-120000-ABCDEFG.md");
	write(file, MARK "- [ ] Milk ^milk01\n- [ ] Eggs 🚩 ^eggs01\n");
	CHECK_EQ(
		s.list_name_for(t.sync() /
						"Groceries.sync-conflict-20261001-120000-ABCDEFG.md")
			.value_or(""),
		"Groceries");

	CHECK(s.reload("Groceries"));
	CHECK_EQ(read(file),
			 MARK "- [x] Milk ✅ 2026-10-01 ^milk01\n- [ ] Eggs 🚩 ^eggs01\n");
	CHECK(!fs::exists(t.sync() /
					  "Groceries.sync-conflict-20261001-120000-ABCDEFG.md"));
	CHECK(s.list("Groceries")->doc.find("milk01")->done);
}

TEST(conflict_found_at_startup) {
	TempDir t;
	write(t.sync() / "Work.md", MARK "- [ ] a ^aaaaaa\n");
	write(t.sync() / "Work.sync-conflict-20261001-120000-XYZ.md",
		  "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK_EQ(s.lists().size(), 1u);
	CHECK_EQ(read(t.sync() / "Work.md"),
			 MARK "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
}

TEST(marker_added_on_one_side_of_a_conflict) {
	TempDir t;
	write(t.sync() / "Todo.md", "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
	write(t.sync() / "Todo.sync-conflict-20261001-120000-XYZ.md",
		  MARK "- [ ] a ^aaaaaa\n");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK_EQ(s.lists().size(), 1u);
	CHECK(read(t.sync() / "Todo.md").starts_with(MARK));
	CHECK(!fs::exists(t.sync() / "Todo.sync-conflict-20261001-120000-XYZ.md"));
}

TEST(state_keeps_one_copy_per_list) {
	TempDir t;
	write(t.sync() / "Todo.md", MARK "- [ ] a ^aaaaaa\n");
	std::string first;
	{
		Store s(t.sync(), t.state());
		s.load_all();  // the file came from outside: it becomes the base
		first = read(t.state() / "base" / "Todo.md");
		CHECK_EQ(first, MARK "- [ ] a ^aaaaaa\n");
		s.set_done("aaaaaa", true, d(2026, 10, 1));
		// Our own write is remembered as a fingerprint, not a second copy.
		CHECK(!fs::exists(t.state() / "mine"));
		CHECK_EQ(read(t.state() / "written" / "Todo").size(), 16u);
	}
	// After a restart our unsynced edit is still recognised as our own, so
	// the base stays the last shared version.
	Store again(t.sync(), t.state());
	again.load_all();
	CHECK_EQ(read(t.state() / "base" / "Todo.md"), first);
}

TEST(state_follows_rename_and_delete) {
	TempDir t;
	write(t.sync() / "A.md", MARK "- [ ] a ^aaaaaa\n");
	Store s(t.sync(), t.state());
	s.load_all();
	s.set_done("aaaaaa", true, d(2026, 10, 1));
	CHECK(s.rename_list(*s.list("A"), "B"));
	CHECK(!fs::exists(t.state() / "base" / "A.md"));
	CHECK(!fs::exists(t.state() / "written" / "A"));
	CHECK(fs::exists(t.state() / "base" / "B.md"));
	CHECK(fs::exists(t.state() / "written" / "B"));
	s.delete_list("B");
	CHECK(!fs::exists(t.state() / "base" / "B.md"));
	CHECK(!fs::exists(t.state() / "written" / "B"));
}

TEST(state_goes_when_another_device_deletes_the_list) {
	TempDir t;
	write(t.sync() / "A.md", MARK "- [ ] a ^aaaaaa\n");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK(fs::exists(t.state() / "base" / "A.md"));
	fs::remove(t.sync() / "A.md");
	s.reload("A");
	CHECK(!fs::exists(t.state() / "base" / "A.md"));
}
