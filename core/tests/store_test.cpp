#include "reminders/store.hpp"

#include <fstream>
#include <sstream>

#include "reminders/backend_module.hpp"
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

TEST(create_add_and_ignore_own_writes) {
	TempDir t;
	Store s(t.sync(), t.state());
	auto& list = s.create_list("Groceries", "orange", "cart");
	Reminder r;
	r.title = "Milk";
	r.id = "milk01";
	s.add(list, r);
	CHECK_EQ(read(t.sync() / "Groceries.md"),
			 "---\nreminders: 1\ncolor: orange\nicon: cart\n---\n- [ ] Milk "
			 "^milk01\n");
	CHECK(!s.reload("Groceries"));	// our own write
	CHECK_EQ(s.lists().size(), 1u);
}

TEST(load_assigns_ids_in_memory_only) {
	TempDir t;
	write(t.sync() / "Todo.md", MARK "- [ ] hand written\n");
	write(t.sync() / "notes.txt", "not a list");
	write(t.sync() / ".hidden.md", "- [ ] nope");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK_EQ(s.lists().size(), 1u);
	auto& r = *s.list("Todo")->doc.reminders()[0];
	CHECK_EQ(r.id.size(), 6u);
	CHECK_EQ(read(t.sync() / "Todo.md"),
			 MARK "- [ ] hand written\n");	// not rewritten
	s.touch(r.id);							// unchanged → still not rewritten
	CHECK_EQ(read(t.sync() / "Todo.md"), MARK "- [ ] hand written\n");
}

TEST(external_change_reloads) {
	TempDir t;
	write(t.sync() / "Todo.md", MARK "- [ ] a ^aaaaaa\n");
	Store s(t.sync(), t.state());
	s.load_all();
	write(t.sync() / "Todo.md", MARK "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
	CHECK(s.reload("Todo"));
	CHECK_EQ(s.list("Todo")->doc.reminders().size(), 2u);
	fs::remove(t.sync() / "Todo.md");
	CHECK(s.reload("Todo"));
	CHECK(s.lists().empty());
}

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

TEST(completing_repeating_reminder) {
	TempDir t;
	write(t.sync() / "Home.md", MARK
		  "- [ ] Bins 🔁 every week 📅 2026-10-01 ^bins01\n  - [x] Recycling "
		  "^recy01\n- [ ] Other ^othe01\n");
	Store s(t.sync(), t.state());
	s.load_all();
	s.set_done("bins01", true, d(2026, 10, 2));
	auto doc = s.list("Home")->doc;
	auto rs = doc.reminders();
	CHECK_EQ(rs.size(), 3u);
	CHECK(!rs[0]->done);
	CHECK(rs[0]->due_date == d(2026, 10, 8));
	CHECK_EQ(rs[0]->repeat.value_or(""), "every week");
	CHECK(!rs[0]->subtasks[0].done);
	CHECK(rs[0]->id != "bins01");
	CHECK_EQ(rs[1]->id, "bins01");
	CHECK(rs[1]->done);
	CHECK(!rs[1]->repeat);
	CHECK(rs[1]->completed == d(2026, 10, 2));
	CHECK_EQ(rs[2]->id, "othe01");
}

TEST(completing_parent_completes_subtasks) {
	TempDir t;
	write(t.sync() / "A.md", MARK "- [ ] P ^pppppp\n  - [ ] c ^cccccc\n");
	Store s(t.sync(), t.state());
	s.load_all();
	s.set_done("pppppp", true, d(2026, 10, 1));
	CHECK_EQ(
		read(t.sync() / "A.md"), MARK
		"- [x] P ✅ 2026-10-01 ^pppppp\n  - [x] c ✅ 2026-10-01 ^cccccc\n");
}

TEST(smart_lists) {
	TempDir t;
	write(t.sync() / "A.md", MARK
		  "- [ ] overdue 📅 2026-09-01 ^a00001\n"
		  "- [ ] today 🚩 📅 2026-10-01 10:00 ^a00002\n"
		  "- [ ] later #x 📅 2026-12-01 ^a00003\n"
		  "- [ ] someday ^a00004\n"
		  "  - [ ] sub 🚩 ^a00005\n"
		  "- [x] done 🚩 ✅ 2026-09-15 ^a00006\n");
	write(t.sync() / "B.md",
		  "---\nreminders: 1\norder: 1\n---\n- [ ] b today 📅 2026-10-01 "
		  "^b00001\n- [x] b done #X ✅ 2026-09-20 ^b00002\n");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK_EQ(s.lists()[0]->name, "B");	// ordered lists first
	CHECK_EQ(s.today(d(2026, 10, 1)).size(), 3u);
	auto sched = s.scheduled();
	CHECK_EQ(sched.size(), 4u);
	CHECK_EQ(sched[0].reminder->title, "overdue");
	CHECK_EQ(sched[1].reminder->title, "b today");	// all-day before 10:00
	CHECK_EQ(s.all().size(), 6u);
	CHECK_EQ(s.everything().size(), 8u);  // the 6 open and the 2 completed
	CHECK_EQ(s.flagged().size(), 2u);
	auto done = s.completed();
	CHECK_EQ(done.size(), 2u);
	CHECK_EQ(done[0].reminder->title, "b done");
	CHECK_EQ(s.tagged("x").size(), 2u);
	CHECK_EQ(s.tags().size(), 2u);
	CHECK_EQ(s.search("SUB").size(), 1u);
	CHECK(s.search("SUB")[0].parent != nullptr);
}

TEST(move_and_rename) {
	TempDir t;
	write(t.sync() / "A.md", MARK "- [ ] one ^oneone\n");
	write(t.sync() / "B.md", MARK "- [ ] two ^twotwo\n");
	Store s(t.sync(), t.state());
	s.load_all();
	s.move_to_list("oneone", *s.list("B"));
	CHECK_EQ(read(t.sync() / "A.md"), MARK);
	CHECK_EQ(read(t.sync() / "B.md"),
			 MARK "- [ ] two ^twotwo\n- [ ] one ^oneone\n");
	CHECK(s.rename_list(*s.list("B"), "Bee"));
	CHECK(fs::exists(t.sync() / "Bee.md"));
	CHECK(!s.rename_list(*s.list("Bee"), "A"));
}

TEST(only_marked_files_are_lists) {
	TempDir t;
	write(t.sync() / "List.md", MARK "- [ ] a ^aaaaaa\n");
	write(t.sync() / "Checklist.md", "# Packing\n- [ ] socks\n");
	write(t.sync() / "Note.md", "Just a note.\n");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK_EQ(s.lists().size(), 1u);
	CHECK_EQ(s.lists()[0]->name, "List");
	// A note with a checklist is offered; a plain note isn't.
	CHECK_EQ(s.candidates().size(), 1u);
	CHECK_EQ(s.candidates()[0], "Checklist");
}

TEST(adopting_a_candidate_adds_the_marker_first) {
	TempDir t;
	write(t.sync() / "Packing.md",
		  "---\ntags: travel\n---\n# Packing\n- [ ] socks\n");
	Store s(t.sync(), t.state());
	s.load_all();
	s.adopt("Packing");
	CHECK(s.candidates().empty());
	CHECK_EQ(s.lists().size(), 1u);
	// Nothing else changes (no ids added just for this).
	CHECK_EQ(read(t.sync() / "Packing.md"),
			 "---\nreminders: 1\ntags: travel\n---\n# Packing\n- [ ] socks\n");
}

TEST(declined_candidates_stay_declined) {
	TempDir t;
	write(t.sync() / "Packing.md", "- [ ] socks\n");
	{
		Store s(t.sync(), t.state());
		s.load_all();
		s.decline("Packing");
		CHECK(s.candidates().empty());
	}
	Store again(t.sync(), t.state());
	again.load_all();
	CHECK(again.candidates().empty());
	CHECK(again.lists().empty());
}

TEST(removing_the_marker_unlists_the_file) {
	TempDir t;
	write(t.sync() / "Todo.md", MARK "- [ ] a ^aaaaaa\n");
	Store s(t.sync(), t.state());
	s.load_all();
	write(t.sync() / "Todo.md", "- [ ] a ^aaaaaa\n");
	CHECK(s.reload("Todo"));
	CHECK(s.lists().empty());
	CHECK_EQ(s.candidates().size(), 1u);
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

TEST(conflict_copies_of_ordinary_notes_are_left_alone) {
	TempDir t;
	write(t.sync() / "Note.md", "text\n");
	write(t.sync() / "Note.sync-conflict-20261001-120000-XYZ.md",
		  "other text\n");
	Store s(t.sync(), t.state());
	s.load_all();
	CHECK(s.lists().empty());
	CHECK(fs::exists(t.sync() / "Note.sync-conflict-20261001-120000-XYZ.md"));
	CHECK_EQ(read(t.sync() / "Note.md"), "text\n");
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

TEST(count_labels) {
	CHECK_EQ(count_label(CountStyle::OpenOnly, 1), "1 Reminder");
	CHECK_EQ(count_label(CountStyle::OpenOnly, 5), "5 Reminders");
	CHECK_EQ(count_label(CountStyle::WithComplete, 6, 3),
			 "6 Reminders / 3 Complete");
	CHECK_EQ(count_label(CountStyle::WithComplete, 6, 0), "6 Reminders");
	CHECK_EQ(count_label(CountStyle::Completed, 2), "2 Completed");
	CHECK_EQ(count_label(CountStyle::Results, 1), "1 Result");
	CHECK_EQ(count_label(CountStyle::Results, 0), "0 Results");
	CHECK_EQ(count_short(CountStyle::WithComplete, 8, 2), "8/2");
	CHECK_EQ(count_short(CountStyle::WithComplete, 8, 0), "8");
	CHECK_EQ(count_short(CountStyle::OpenOnly, 5), "5");
}

TEST(local_backend_has_no_sync_handling) {
	TempDir t;
	auto file = t.sync() / "Groceries.md";
	write(file, MARK "- [ ] Milk ^milk01\n");
	// A file named like a Syncthing conflict copy is just another file here.
	write(t.sync() / "Groceries.sync-conflict-20261001-120000-ABCDEFG.md",
		  MARK "- [ ] Eggs ^eggs01\n");
	Store s(t.sync(), t.state(), "local");
	s.load_all();
	CHECK(s.backend() == "local");
	CHECK_EQ(
		s.list_name_for(t.sync() /
						"Groceries.sync-conflict-20261001-120000-ABCDEFG.md")
			.value_or(""),
		"Groceries.sync-conflict-20261001-120000-ABCDEFG");
	CHECK_EQ(s.lists().size(), 2u);	 // not merged
	CHECK_EQ(read(file), MARK "- [ ] Milk ^milk01\n");

	// Saving writes the file and nothing else: no per-device records.
	s.set_done("milk01", true, d(2026, 10, 1));
	CHECK(s.list("Groceries")->doc.find("milk01")->done);
	CHECK(!fs::exists(t.state() / "base"));
	CHECK(!fs::exists(t.state() / "written"));

	// A change made by another program is picked up on reload.
	write(file, MARK "- [ ] Milk ^milk01\n- [ ] Bread ^brea01\n");
	CHECK(s.reload("Groceries"));
	CHECK(s.list("Groceries")->doc.find("brea01") != nullptr);
}

TEST(backend_registry) {
	CHECK_EQ(find_backend("Syncthing")->id, "syncthing");  // ignoring case
	CHECK(!find_backend("dropbox"));
	std::vector<std::string> ids;
	for (auto* m : backends()) {
		ids.push_back(m->id);
	}
	CHECK((ids == std::vector<std::string>{"syncthing", "local", "caldav",
										   "webdav", "git"}));
	CHECK(find_backend("git")->owns_folder && !find_backend("git")->has_server);
	CHECK(find_backend("syncthing")->state_in_folder);
	CHECK_EQ(backend_of(SourceConfig{"x", "nonsense", {}, ""}).id, "syncthing");
	CHECK_EQ(make_backend("local", {})->id(), "local");
	SourceConfig git{
		"notes", "git", {}, "", {{"url", "git@github.com:you/notes.git"}}};
	CHECK_EQ(find_backend("git")->name_hint(git), "notes");
	CHECK_EQ(git_settings(git).url, "git@github.com:you/notes.git");
	CHECK_EQ(git_settings(git).interval, 15);
}

TEST(held_saves_write_each_list_once) {
	TempDir t;
	write(t.sync() / "A.md", MARK "- [ ] one ^a00001\n- [ ] two ^a00002\n");
	write(t.sync() / "B.md", MARK "- [ ] three ^b00001\n");
	Store s(t.sync(), t.state());
	s.load_all();
	auto before = read(t.sync() / "A.md");
	s.hold_saves();
	s.hold_saves();	 // nested
	s.set_done("a00001", true, d(2026, 10, 4));
	s.set_done("a00002", true, d(2026, 10, 4));
	s.move_to_list("b00001", *s.list("A"));
	s.release_saves();
	CHECK_EQ(read(t.sync() / "A.md"), before);	// still held
	s.release_saves();
	CHECK_EQ(read(t.sync() / "A.md"), MARK
			 "- [x] one ✅ 2026-10-04 ^a00001\n- [x] two ✅ 2026-10-04 "
			 "^a00002\n- [ ] three ^b00001\n");
	CHECK_EQ(read(t.sync() / "B.md"), MARK);
	// A list deleted while held is skipped.
	s.hold_saves();
	s.set_done("b00001", true, d(2026, 10, 4));
	s.delete_list("B");
	s.release_saves();
	CHECK(!fs::exists(t.sync() / "B.md"));
	CHECK(read(t.sync() / "A.md").find("- [x] three") != std::string::npos);
}
