#include "reminders/history.hpp"

#include <fstream>
#include <sstream>

#include "test.hpp"

using namespace rem;

namespace {

struct Folder {
		fs::path path;
		Folder() {
			path =
				fs::temp_directory_path() / ("reminders-history-" + new_id());
			fs::create_directories(path / "sync");
		}
		~Folder() { fs::remove_all(path); }
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

#define MARK "---\nreminders: 1\n---\n"

// Runs `f` against the store and records it as one step.
template <class F>
std::uint64_t act(History& h, Store& s, const char* label, F&& f) {
	auto before = s.snapshot();
	f();
	return h.record(label, before, s.snapshot());
}

}  // namespace

TEST(undo_and_redo_an_edit) {
	Folder t;
	write(t.sync() / "A.md", MARK "- [ ] milk ^milk01\n");
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	CHECK(!h.can_undo());
	act(h, s, "Flag", [&] {
		s.find("milk01")->reminder->flagged = true;
		s.touch("milk01");
	});
	CHECK_EQ(read(t.sync() / "A.md"), MARK "- [ ] milk 🚩 ^milk01\n");
	CHECK_EQ(h.undo_label(), "Flag");

	CHECK(h.undo(s).applied);
	CHECK_EQ(read(t.sync() / "A.md"), MARK "- [ ] milk ^milk01\n");
	CHECK(!s.find("milk01")->reminder->flagged);
	CHECK(h.can_redo());

	CHECK(h.redo(s).applied);
	CHECK_EQ(read(t.sync() / "A.md"), MARK "- [ ] milk 🚩 ^milk01\n");
	CHECK(!h.can_redo());
}

TEST(nothing_changed_records_nothing) {
	Folder t;
	write(t.sync() / "A.md", MARK "- [ ] milk ^milk01\n");
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	CHECK_EQ(act(h, s, "Nothing", [] {}), 0u);
	CHECK(!h.can_undo());
}

TEST(new_action_clears_redo) {
	Folder t;
	write(t.sync() / "A.md", MARK "- [ ] milk ^milk01\n");
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	act(h, s, "Done", [&] {
		s.set_done("milk01", true,
				   Date{std::chrono::year{2026}, std::chrono::October,
						std::chrono::day{1}});
	});
	h.undo(s);
	act(h, s, "Delete", [&] { s.remove("milk01"); });
	CHECK(!h.can_redo());
}

TEST(undo_move_between_lists_and_reorder) {
	Folder t;
	write(t.sync() / "A.md", MARK "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
	write(t.sync() / "B.md", MARK "- [ ] c ^cccccc\n");
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	act(h, s, "Move", [&] { s.move_to_list("aaaaaa", *s.list("B")); });
	act(h, s, "Reorder", [&] {
		s.list("B")->doc.move_next_to("aaaaaa", "cccccc",
									  Document::Place::Before);
		s.save(*s.list("B"));
	});
	CHECK_EQ(read(t.sync() / "B.md"),
			 MARK "- [ ] a ^aaaaaa\n- [ ] c ^cccccc\n");
	h.undo(s);
	CHECK_EQ(read(t.sync() / "B.md"),
			 MARK "- [ ] c ^cccccc\n- [ ] a ^aaaaaa\n");
	h.undo(s);
	CHECK_EQ(read(t.sync() / "A.md"),
			 MARK "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
	CHECK_EQ(read(t.sync() / "B.md"), MARK "- [ ] c ^cccccc\n");
}

TEST(undo_create_rename_and_delete_list) {
	Folder t;
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	act(h, s, "New List", [&] { s.create_list("Trip", "blue", "travel"); });
	act(h, s, "Rename", [&] { s.rename_list(*s.list("Trip"), "Holiday"); });
	act(h, s, "Delete", [&] { s.delete_list("Holiday"); });
	CHECK(s.lists().empty());

	h.undo(s);	// delete
	CHECK(fs::exists(t.sync() / "Holiday.md"));
	h.undo(s);	// rename
	CHECK(fs::exists(t.sync() / "Trip.md"));
	CHECK(!fs::exists(t.sync() / "Holiday.md"));
	CHECK_EQ(s.lists().size(), 1u);
	h.undo(s);	// create
	CHECK(!fs::exists(t.sync() / "Trip.md"));
	CHECK(s.lists().empty());
}

TEST(undo_keeps_changes_from_other_devices) {
	Folder t;
	write(t.sync() / "A.md", MARK "- [ ] milk ^milk01\n- [ ] eggs ^eggs01\n");
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	act(h, s, "Done", [&] {
		s.set_done("milk01", true,
				   Date{std::chrono::year{2026}, std::chrono::October,
						std::chrono::day{1}});
	});
	// Another device then flags eggs and adds bread.
	write(t.sync() / "A.md", MARK
		  "- [x] milk ✅ 2026-10-01 ^milk01\n- [ ] eggs 🚩 ^eggs01\n- [ ] "
		  "bread ^brea01\n");
	s.reload("A");

	auto r = h.undo(s);
	CHECK(r.applied);
	CHECK(r.skipped.empty());
	CHECK_EQ(
		read(t.sync() / "A.md"), MARK
		"- [ ] milk ^milk01\n- [ ] eggs 🚩 ^eggs01\n- [ ] bread ^brea01\n");
}

TEST(undo_skips_a_list_deleted_elsewhere) {
	Folder t;
	write(t.sync() / "A.md", MARK "- [ ] milk ^milk01\n");
	Store s(t.sync(), t.state());
	s.load_all();
	History h;
	act(h, s, "Flag", [&] {
		s.find("milk01")->reminder->flagged = true;
		s.touch("milk01");
	});
	fs::remove(t.sync() / "A.md");
	s.reload("A");
	auto r = h.undo(s);
	CHECK_EQ(r.skipped.size(), 1u);
	CHECK(!fs::exists(t.sync() / "A.md"));	// not resurrected
}
