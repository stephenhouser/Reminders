#include "reminders/format.hpp"

#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

// The example from docs/FORMAT.md.
constexpr std::string_view kExample =
	"---\n"
	"color: orange\n"
	"icon: cart\n"
	"order: 2\n"
	"---\n"
	"\n"
	"- [ ] Buy milk #errands ⏫ 🚩 📅 2026-10-03 09:00 ^k3x9qa\n"
	"  2% if they have it\n"
	"  second line of notes\n"
	"  - [ ] Check expiry date ^p0d2mf\n"
	"- [ ] Bread ^b81zzc\n"
	"\n"
	"## Party\n"
	"- [ ] Balloons 🔁 every week 📅 2026-10-04 ^h2n7aa\n"
	"- [x] Cake ✅ 2026-09-30 ^c4ke00\n";

Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}

}  // namespace

TEST(parses_spec_example) {
	auto doc = parse(kExample);
	CHECK_EQ(doc.meta("color").value_or(""), "orange");
	CHECK_EQ(doc.meta("order").value_or(""), "2");

	auto rs = doc.reminders();
	CHECK_EQ(rs.size(), 4u);
	auto& milk = *rs[0];
	CHECK_EQ(milk.title, "Buy milk");
	CHECK_EQ(milk.id, "k3x9qa");
	CHECK_EQ(milk.tags.size(), 1u);
	CHECK_EQ(milk.tags[0], "errands");
	CHECK(milk.priority == Priority::High);
	CHECK(milk.flagged);
	CHECK(milk.due_date == d(2026, 10, 3));
	CHECK((milk.due_time == TimeOfDay{9, 0}));
	CHECK_EQ(milk.notes, "2% if they have it\nsecond line of notes");
	CHECK_EQ(milk.subtasks.size(), 1u);
	CHECK_EQ(milk.subtasks[0].title, "Check expiry date");

	auto& balloons = *rs[2];
	CHECK_EQ(balloons.repeat.value_or(""), "every week");
	CHECK(balloons.due_date == d(2026, 10, 4));
	CHECK(!balloons.due_time);

	CHECK(rs[3]->done);
	CHECK(rs[3]->completed == d(2026, 9, 30));

	auto sections = doc.sections();
	CHECK_EQ(sections.size(), 2u);
	CHECK(!sections[0].name);
	CHECK_EQ(sections[0].reminders.size(), 2u);
	CHECK_EQ(sections[1].name.value_or(""), "Party");
	CHECK_EQ(sections[1].reminders.size(), 2u);
}

TEST(round_trips_byte_for_byte) {
	CHECK_EQ(serialize(parse(kExample)), std::string(kExample));
}

TEST(canonical_line_when_edited) {
	auto doc = parse(kExample);
	auto& milk = *doc.reminders()[0];
	milk.flagged = false;
	milk.done = true;
	milk.completed = d(2026, 10, 1);
	auto out = serialize(doc);
	CHECK(out.find("- [x] Buy milk #errands ⏫ 📅 2026-10-03 09:00 ✅ "
				   "2026-10-01 ^k3x9qa\n") != std::string::npos);
	// Everything else untouched.
	CHECK(out.find("- [ ] Bread ^b81zzc\n") != std::string::npos);
}

TEST(hand_written_line_keeps_formatting_and_gains_id) {
	auto doc = parse("* [X]   Call   mom 📅2026-10-05\n");
	auto& r = *doc.reminders()[0];
	CHECK_EQ(r.title, "Call mom");
	CHECK(r.done);
	CHECK(r.due_date == d(2026, 10, 5));
	CHECK(r.id.empty());
	CHECK_EQ(serialize(doc), "* [X]   Call   mom 📅2026-10-05\n");
	r.id = "abc123";
	CHECK_EQ(serialize(doc), "* [X]   Call   mom 📅2026-10-05 ^abc123\n");
}

TEST(fields_in_any_order_and_obsidian_priorities) {
	std::string id;
	auto f = parse_fields(
		"📅 2026-01-02 #a Pay 🔺 rent #b/c 🔗 https://x.y/z ^zz9", &id);
	CHECK_EQ(f.title, "Pay rent");
	CHECK_EQ(f.tags.size(), 2u);
	CHECK_EQ(f.tags[1], "b/c");
	CHECK(f.priority == Priority::High);
	CHECK_EQ(f.url.value_or(""), "https://x.y/z");
	CHECK_EQ(id, "zz9");
	CHECK(parse_fields("Low ⏬").priority == Priority::Low);
	CHECK(parse_fields("Med 🔼").priority == Priority::Medium);
}

TEST(variation_selector_ignored) {
	auto f = parse_fields("Thing ✅️ 2026-02-03");
	CHECK_EQ(f.title, "Thing");
	CHECK(f.completed == d(2026, 2, 3));
}

TEST(not_quite_fields_stay_in_title) {
	auto f = parse_fields("Issue #123 costs 📅 soon, see ^ notes");
	CHECK_EQ(f.title, "Issue #123 costs 📅 soon, see ^ notes");
	CHECK(f.tags.empty());
	CHECK(!f.due_date);
	CHECK(!parse_date("2026-02-30"));
	CHECK(!parse_time("24:00"));
}

TEST(repeat_stops_at_next_field) {
	auto f = parse_fields("Water plants 🔁 every 2 days #home 📅 2026-03-01");
	CHECK_EQ(f.repeat.value_or(""), "every 2 days");
	CHECK_EQ(f.title, "Water plants");
	CHECK_EQ(f.tags.size(), 1u);
}

TEST(empty_title) {
	auto doc = parse("- [ ]\n- [ ] ^abcdef\n");
	CHECK_EQ(doc.reminders().size(), 2u);
	CHECK_EQ(doc.reminders()[1]->id, "abcdef");
	Reminder r;
	r.id = "xyz789";
	Document out;
	out.blocks.push_back(r);
	CHECK_EQ(serialize(out), "- [ ] ^xyz789\n");
}

TEST(notes_with_blank_lines_and_subtask_notes) {
	constexpr std::string_view text =
		"- [ ] Trip\n"
		"  pack bags\n"
		"\n"
		"  book hotel\n"
		"  - [ ] Passport\n"
		"    check expiry\n"
		"  back on parent\n"
		"\n"
		"Some prose.\n";
	auto doc = parse(text);
	auto& trip = *doc.reminders()[0];
	CHECK_EQ(trip.notes, "pack bags\n\nbook hotel\nback on parent");
	CHECK_EQ(trip.subtasks[0].notes, "check expiry");
	CHECK_EQ(doc.blocks.size(), 3u);  // reminder, blank, prose
}

TEST(regenerated_notes_reparse_identically) {
	Reminder r;
	r.title = "Trip";
	r.id = "trip01";
	r.notes = "a\n\nb\n";
	Reminder sub;
	sub.title = "Sub";
	sub.id = "sub001";
	sub.notes = "x\ny";
	r.subtasks.push_back(sub);
	Document doc;
	doc.blocks.push_back(r);
	auto text = serialize(doc);
	CHECK_EQ(
		text,
		"- [ ] Trip ^trip01\n  a\n\n  b\n  - [ ] Sub ^sub001\n    x\n    y\n");
	auto back = parse(text);
	CHECK(back.reminders()[0]->same_content(*parse(text).reminders()[0]));
	CHECK_EQ(back.reminders()[0]->notes, "a\n\nb");
	CHECK_EQ(back.reminders()[0]->subtasks[0].notes, "x\ny");
}

TEST(front_matter_unknown_and_nested_keys_preserved) {
	constexpr std::string_view text =
		"---\n"
		"color: red\n"
		"# a comment\n"
		"aliases:\n"
		"  - one\n"
		"  - two\n"
		"custom: value\n"
		"---\n"
		"- [ ] x ^aaaaaa\n";
	auto doc = parse(text);
	CHECK_EQ(serialize(doc), std::string(text));
	doc.set_meta("color", "green");
	doc.set_meta("icon", "cart");
	auto out = serialize(doc);
	CHECK(
		out.starts_with("---\ncolor: green\n# a comment\naliases:\n  - one\n  "
						"- two\ncustom: value\nicon: cart\n---\n"));
}

TEST(crlf_and_no_trailing_newline) {
	auto doc = parse("- [ ] a ^aaaaaa\r\n- [x] b ^bbbbbb");
	CHECK_EQ(doc.reminders().size(), 2u);
	CHECK_EQ(doc.reminders()[1]->title, "b");
}

TEST(moved_subtask_gets_new_indentation) {
	auto doc = parse("- [ ] Parent ^pppppp\n  - [ ] Child ^cccccc\n");
	auto child = doc.remove("cccccc");
	doc.insert(std::move(*child), nullptr);
	CHECK_EQ(serialize(doc), "- [ ] Parent ^pppppp\n- [ ] Child ^cccccc\n");
}

TEST(insert_into_sections) {
	auto doc = parse(kExample);
	Reminder a;
	a.title = "Eggs";
	a.id = "eggs01";
	doc.insert(a, nullptr);	 // end of the unsectioned part
	Reminder b;
	b.title = "Plates";
	b.id = "plat01";
	doc.insert(b, nullptr, std::string("Party"));
	Reminder c;
	c.title = "Wrap";
	c.id = "wrap01";
	doc.insert(c, nullptr, std::string("Gifts"));
	auto out = serialize(doc);
	CHECK(out.find("- [ ] Bread ^b81zzc\n- [ ] Eggs ^eggs01\n\n## Party") !=
		  std::string::npos);
	CHECK(out.find("- [x] Cake ✅ 2026-09-30 ^c4ke00\n- [ ] Plates "
				   "^plat01\n\n## Gifts\n- [ ] Wrap ^wrap01\n") !=
		  std::string::npos);
}

TEST(drag_reorder_top_level_and_sections) {
	auto doc =
		parse("- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n\n## S\n- [ ] c ^cccccc\n");
	using P = Document::Place;
	CHECK(doc.move_next_to("aaaaaa", "bbbbbb", P::After));
	CHECK_EQ(serialize(doc),
			 "- [ ] b ^bbbbbb\n- [ ] a ^aaaaaa\n\n## S\n- [ ] c ^cccccc\n");
	CHECK(doc.move_next_to("bbbbbb", "cccccc", P::Before));	 // into section S
	CHECK_EQ(serialize(doc),
			 "- [ ] a ^aaaaaa\n\n## S\n- [ ] b ^bbbbbb\n- [ ] c ^cccccc\n");
	CHECK(doc.move_to_end("bbbbbb", std::nullopt));
	CHECK_EQ(serialize(doc),
			 "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n\n## S\n- [ ] c ^cccccc\n");
	CHECK(!doc.move_next_to("aaaaaa", "aaaaaa", P::Before));
	CHECK(!doc.move_next_to("aaaaaa", "nope00", P::Before));
}

TEST(drag_changes_nesting) {
	using P = Document::Place;
	auto doc = parse(
		"- [ ] p ^pppppp\n  - [ ] s1 ^s11111\n  - [ ] s2 ^s22222\n- [ ] q "
		"^qqqqqq\n");
	// A top-level reminder dropped on a subtask becomes a subtask.
	CHECK(doc.move_next_to("qqqqqq", "s11111", P::After));
	CHECK_EQ(serialize(doc),
			 "- [ ] p ^pppppp\n  - [ ] s1 ^s11111\n  - [ ] q ^qqqqqq\n  - [ ] "
			 "s2 ^s22222\n");
	// A subtask dropped on a top-level reminder becomes top-level.
	CHECK(doc.move_next_to("s22222", "pppppp", P::Before));
	CHECK_EQ(serialize(doc),
			 "- [ ] s2 ^s22222\n- [ ] p ^pppppp\n  - [ ] s1 ^s11111\n  - [ ] q "
			 "^qqqqqq\n");
	// Not allowed: a parent into its own subtasks, or a reminder with
	// subtasks into another's subtasks.
	CHECK(!doc.move_next_to("pppppp", "s11111", P::After));
	auto two = parse(
		"- [ ] a ^aaaaaa\n  - [ ] a1 ^a11111\n- [ ] b ^bbbbbb\n  - [ ] b1 "
		"^b11111\n");
	CHECK(!two.move_next_to("aaaaaa", "b11111", P::Before));
	CHECK_EQ(serialize(two),
			 "- [ ] a ^aaaaaa\n  - [ ] a1 ^a11111\n- [ ] b ^bbbbbb\n  - [ ] b1 "
			 "^b11111\n");
	// "After" a parent goes after its whole block.
	CHECK(two.move_next_to("a11111", "bbbbbb", P::After));
	CHECK_EQ(serialize(two),
			 "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n  - [ ] b1 ^b11111\n- [ ] a1 "
			 "^a11111\n");
}

TEST(keyboard_move_step) {
	auto all = [](const Reminder&) { return true; };
	auto open = [](const Reminder& r) { return !r.done; };
	auto doc = parse(
		"- [ ] a ^aaaaaa\n- [x] done ^dddddd\n- [ ] b ^bbbbbb\n\n## S\n- [ ] c "
		"^cccccc\n  - [ ] c1 ^c11111\n  - [ ] c2 ^c22222\n");
	// Hidden reminders are skipped.
	CHECK(doc.move_step("bbbbbb", true, open));
	CHECK_EQ(serialize(doc),
			 "- [ ] b ^bbbbbb\n- [ ] a ^aaaaaa\n- [x] done ^dddddd\n\n## S\n- "
			 "[ ] c ^cccccc\n  - [ ] c1 ^c11111\n  - [ ] c2 ^c22222\n");
	CHECK(!doc.move_step("bbbbbb", true, open));  // already first
	// Down across the heading: top of the next section.
	CHECK(doc.move_step("aaaaaa", false, open));
	CHECK_EQ(serialize(doc),
			 "- [ ] b ^bbbbbb\n- [x] done ^dddddd\n\n## S\n- [ ] a ^aaaaaa\n- "
			 "[ ] c ^cccccc\n  - [ ] c1 ^c11111\n  - [ ] c2 ^c22222\n");
	// Up across the heading: end of the previous section.
	CHECK(doc.move_step("aaaaaa", true, all));
	CHECK_EQ(serialize(doc),
			 "- [ ] b ^bbbbbb\n- [x] done ^dddddd\n- [ ] a ^aaaaaa\n\n## S\n- "
			 "[ ] c ^cccccc\n  - [ ] c1 ^c11111\n  - [ ] c2 ^c22222\n");
	// Down past a reminder with subtasks: after its whole block.
	CHECK(doc.move_step("aaaaaa", false, all));
	CHECK(doc.move_step("aaaaaa", false, all));
	CHECK_EQ(serialize(doc),
			 "- [ ] b ^bbbbbb\n- [x] done ^dddddd\n\n## S\n- [ ] c ^cccccc\n  "
			 "- [ ] c1 ^c11111\n  - [ ] c2 ^c22222\n- [ ] a ^aaaaaa\n");
	// Subtasks stay inside their parent.
	CHECK(doc.move_step("c22222", true, all));
	CHECK(!doc.move_step("c22222", true, all));
	CHECK_EQ(serialize(doc),
			 "- [ ] b ^bbbbbb\n- [x] done ^dddddd\n\n## S\n- [ ] c ^cccccc\n  "
			 "- [ ] c2 ^c22222\n  - [ ] c1 ^c11111\n- [ ] a ^aaaaaa\n");
}

TEST(keyboard_move_into_section_with_only_hidden_items) {
	auto open = [](const Reminder& r) { return !r.done; };
	auto doc = parse("- [ ] a ^aaaaaa\n\n## S\n- [x] old ^oooooo\n");
	CHECK(doc.move_step("aaaaaa", false, open));
	CHECK_EQ(serialize(doc), "\n## S\n- [ ] a ^aaaaaa\n- [x] old ^oooooo\n");
	CHECK(doc.move_step("aaaaaa", true, open));
	CHECK_EQ(serialize(doc), "- [ ] a ^aaaaaa\n\n## S\n- [x] old ^oooooo\n");
}

TEST(indent_and_outdent) {
	auto all = [](const Reminder&) { return true; };
	auto open = [](const Reminder& r) { return !r.done; };
	auto doc = parse(
		"- [ ] a ^aaaaaa\n  - [ ] a1 ^a11111\n- [x] done ^dddddd\n- [ ] b "
		"^bbbbbb\n\n## S\n- [ ] c ^cccccc\n");
	// Under the nearest visible reminder above, as its last subtask.
	CHECK(doc.indent("bbbbbb", open));
	CHECK_EQ(serialize(doc),
			 "- [ ] a ^aaaaaa\n  - [ ] a1 ^a11111\n  - [ ] b ^bbbbbb\n- [x] "
			 "done ^dddddd\n\n## S\n- [ ] c ^cccccc\n");
	// Not across a section heading, not twice, not with subtasks of its own.
	CHECK(!doc.indent("cccccc", all));
	CHECK(!doc.indent("bbbbbb", all));
	CHECK(!doc.indent("aaaaaa", all));
	// Outdent lands right after the parent's block.
	CHECK(doc.outdent("a11111"));
	CHECK_EQ(serialize(doc),
			 "- [ ] a ^aaaaaa\n  - [ ] b ^bbbbbb\n- [ ] a1 ^a11111\n- [x] done "
			 "^dddddd\n\n## S\n- [ ] c ^cccccc\n");
	CHECK(!doc.outdent("cccccc"));
}

TEST(rename_and_delete_sections) {
	constexpr std::string_view text =
		"- [ ] a ^aaaaaa\n"
		"\n"
		"## Party\n"
		"- [ ] cake ^cake01\n"
		"\n"
		"## Gifts\n"
		"- [ ] card ^card01\n";

	auto doc = parse(text);
	CHECK(doc.rename_section("Party", "Birthday"));
	CHECK(!doc.rename_section("Birthday", "Gifts"));  // taken
	CHECK(!doc.rename_section("Nope", "X"));
	CHECK(!doc.rename_section("Birthday", ""));
	CHECK_EQ(serialize(doc),
			 "- [ ] a ^aaaaaa\n\n## Birthday\n- [ ] cake ^cake01\n\n## "
			 "Gifts\n- [ ] card ^card01\n");

	// Keeping the reminders: they join the section above.
	auto keep = parse(text);
	CHECK(keep.delete_section("Party", true));
	CHECK_EQ(serialize(keep),
			 "- [ ] a ^aaaaaa\n- [ ] cake ^cake01\n\n## Gifts\n- [ ] card "
			 "^card01\n");

	// Deleting them: the section goes, the next heading keeps its spacing.
	auto drop = parse(text);
	CHECK(drop.delete_section("Party", false));
	CHECK_EQ(serialize(drop),
			 "- [ ] a ^aaaaaa\n\n## Gifts\n- [ ] card ^card01\n");

	// The last section, deleted with its reminders.
	auto last = parse(text);
	CHECK(last.delete_section("Gifts", false));
	CHECK_EQ(serialize(last),
			 "- [ ] a ^aaaaaa\n\n## Party\n- [ ] cake ^cake01\n");
	CHECK(!last.delete_section("Gifts", false));
}

TEST(move_before) {
	auto doc = parse("- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n- [ ] c ^cccccc\n");
	doc.move_before("cccccc", std::string_view("aaaaaa"));
	CHECK_EQ(serialize(doc),
			 "- [ ] c ^cccccc\n- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
	doc.move_before("cccccc", std::nullopt);
	CHECK_EQ(serialize(doc),
			 "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n- [ ] c ^cccccc\n");
}
