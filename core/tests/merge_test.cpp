#include "reminders/merge.hpp"

#include "reminders/format.hpp"
#include "test.hpp"

using namespace rem;

namespace {

std::string merged(std::string_view main, std::string_view conflict,
				   std::optional<std::string_view> base) {
	auto b = base ? std::optional{parse(*base)} : std::nullopt;
	return serialize(merge(parse(main), parse(conflict), b ? &*b : nullptr));
}

constexpr std::string_view kBase =
	"- [ ] Milk ^milk01\n"
	"- [ ] Eggs ^eggs01\n"
	"- [ ] Bread ^brea01\n";

}  // namespace

TEST(three_way_takes_each_sides_changes) {
	// Main completed Milk; conflict flagged Bread.
	auto out = merged(
		"- [x] Milk ✅ 2026-10-01 ^milk01\n- [ ] Eggs ^eggs01\n- [ ] Bread "
		"^brea01\n",
		"- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n- [ ] Bread 🚩 ^brea01\n",
		kBase);
	CHECK_EQ(out,
			 "- [x] Milk ✅ 2026-10-01 ^milk01\n- [ ] Eggs ^eggs01\n- [ ] "
			 "Bread 🚩 ^brea01\n");
}

TEST(both_changed_same_field_main_wins) {
	auto out = merged("- [ ] Whole milk ^milk01\n", "- [ ] Oat milk ^milk01\n",
					  "- [ ] Milk ^milk01\n");
	CHECK_EQ(out, "- [ ] Whole milk ^milk01\n");
}

TEST(different_fields_of_same_reminder) {
	auto out = merged("- [ ] Whole milk ^milk01\n", "- [ ] Milk ⏫ ^milk01\n",
					  "- [ ] Milk ^milk01\n");
	CHECK_EQ(out, "- [ ] Whole milk ⏫ ^milk01\n");
}

TEST(deletions) {
	// Conflict deleted Eggs (untouched in main) → gone.
	// Main deleted Bread (untouched in conflict) → stays gone.
	auto out = merged("- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n",
					  "- [ ] Milk ^milk01\n- [ ] Bread ^brea01\n", kBase);
	CHECK_EQ(out, "- [ ] Milk ^milk01\n");
}

TEST(edit_beats_delete) {
	auto out = merged(
		"- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n",
		"- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n- [ ] Rye bread ^brea01\n",
		kBase);
	CHECK_EQ(
		out,
		"- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n- [ ] Rye bread ^brea01\n");
}

TEST(additions_keep_relative_position) {
	auto out = merged(
		"- [ ] Milk ^milk01\n- [ ] Eggs ^eggs01\n- [ ] Bread ^brea01\n- [ ] "
		"Jam ^jam001\n",
		"- [ ] Milk ^milk01\n- [ ] Butter ^butt01\n- [ ] Eggs ^eggs01\n- [ ] "
		"Bread ^brea01\n",
		kBase);
	CHECK_EQ(out,
			 "- [ ] Milk ^milk01\n- [ ] Butter ^butt01\n- [ ] Eggs ^eggs01\n- "
			 "[ ] Bread ^brea01\n"
			 "- [ ] Jam ^jam001\n");
}

TEST(addition_into_section_and_new_section) {
	auto out = merged("- [ ] Milk ^milk01\n\n## Party\n- [ ] Cake ^cake01\n",
					  "- [ ] Milk ^milk01\n\n## Party\n- [ ] Hats ^hats01\n- [ "
					  "] Cake ^cake01\n\n## Gifts\n- [ ] Card ^card01\n",
					  "- [ ] Milk ^milk01\n\n## Party\n- [ ] Cake ^cake01\n");
	CHECK_EQ(out,
			 "- [ ] Milk ^milk01\n\n## Party\n- [ ] Hats ^hats01\n- [ ] Cake "
			 "^cake01\n\n## Gifts\n- [ ] Card ^card01\n");
}

TEST(no_base_is_a_union_main_wins) {
	auto out =
		merged("- [ ] Whole milk ^milk01\n- [ ] Eggs ^eggs01\n",
			   "- [ ] Oat milk ^milk01\n- [ ] Bread ^brea01\n", std::nullopt);
	CHECK_EQ(
		out,
		"- [ ] Whole milk ^milk01\n- [ ] Bread ^brea01\n- [ ] Eggs ^eggs01\n");
}

TEST(lines_without_ids_match_by_title) {
	auto out = merged("- [x] Milk\n- [ ] Eggs\n",
					  "- [ ] Milk ⏫\n- [ ] Eggs\n- [ ] Tea\n",
					  "- [ ] Milk\n- [ ] Eggs\n");
	CHECK_EQ(out, "- [x] Milk ⏫\n- [ ] Eggs\n- [ ] Tea\n");
}

TEST(subtasks_and_notes_merge) {
	auto out =
		merged("- [ ] Trip ^trip01\n  new note\n  - [x] Passport ^pass01\n",
			   "- [ ] Trip ^trip01\n  - [ ] Passport ^pass01\n  - [ ] Tickets "
			   "^tick01\n",
			   "- [ ] Trip ^trip01\n  - [ ] Passport ^pass01\n");
	CHECK_EQ(out,
			 "- [ ] Trip ^trip01\n  new note\n  - [x] Passport ^pass01\n  - [ "
			 "] Tickets ^tick01\n");
}

TEST(front_matter_merges) {
	auto out = merged("---\ncolor: red\nicon: cart\n---\n",
					  "---\ncolor: blue\nicon: gift\norder: 3\n---\n",
					  "---\ncolor: blue\nicon: cart\n---\n");
	CHECK_EQ(out, "---\ncolor: red\nicon: gift\norder: 3\n---\n");
}
