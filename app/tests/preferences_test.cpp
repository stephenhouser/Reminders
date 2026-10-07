#include "reminders/preferences.hpp"

#include <cstdlib>
#include <fstream>

#include "reminders/model.hpp"
#include "test.hpp"

using namespace rem;

TEST(preferences_sidebar_and_order) {
	auto dir = fs::temp_directory_path() / ("reminders-prefs-" + new_id());
	setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
	fs::create_directories(dir / "reminders");
	std::ofstream(dir / "reminders" / "settings.ini") << "[general]\n";

	// Group order: smart lists, each source's lists, tags by default;
	// missing ones go last.
	auto Smart = SidebarGroup::smart_lists(), Tags = SidebarGroup::tags();
	auto Mine = SidebarGroup::lists("mine");
	std::vector<std::string> one{"mine"};
	CHECK(
		(load_sidebar_order(Profile(), one) == std::vector{Smart, Mine, Tags}));
	save_setting(Profile(), "sidebar-order", "tags, My Lists, bogus, tags");
	CHECK(
		(load_sidebar_order(Profile(), one) == std::vector{Tags, Mine, Smart}));
	auto order = load_sidebar_order(Profile(), one);
	CHECK((move_sidebar_group(order, Smart, -1, {Smart, Mine, Tags})));
	CHECK((order == std::vector{Tags, Smart, Mine}));
	CHECK((!move_sidebar_group(order, Smart, -1,
							   {Smart, Mine})));  // only hidden Tags above
	CHECK((move_sidebar_group(order, Tags, 1, {Smart, Mine, Tags})));
	CHECK((move_sidebar_group(
		order, Mine, -1, {Smart, Mine})));	// past Tags, which isn't showing
	CHECK((order == std::vector{Mine, Tags, Smart}));
	CHECK((!move_sidebar_group(order, Smart, 1, {Smart, Mine, Tags})));
	// Dragged next to another group.
	CHECK((move_sidebar_group_next_to(order, Smart, Mine, false)));
	CHECK((order == std::vector{Smart, Mine, Tags}));
	CHECK((move_sidebar_group_next_to(order, Smart, Tags, true)));
	CHECK((order == std::vector{Mine, Tags, Smart}));
	CHECK((!move_sidebar_group_next_to(order, Tags, Smart,
									   false)));  // already there
	CHECK((!move_sidebar_group_next_to(order, Tags, Tags, true)));
	save_sidebar_order(Profile(), order);
	CHECK_EQ(load_setting(Profile(), "sidebar-order"),
			 "local-lists, tags, smart-lists");	 // one source: local-lists
	CHECK(load_sidebar_order(Profile(), one) == order);

	// Several sources: local-lists stands for the ones not named on their own.
	auto Home = SidebarGroup::lists("home"),
		 Work = SidebarGroup::lists("work-2"), Old = SidebarGroup::lists("old");
	std::vector<std::string> three{"home", "work-2", "old"};
	save_setting(Profile(), "sidebar-order",
				 "lists:work-2, smart-lists, local-lists, lists:gone");
	CHECK((load_sidebar_order(Profile(), three) ==
		   std::vector{Work, Smart, Home, Old, Tags}));
	save_sidebar_order(Profile(), {Work, Smart, Home, Old, Tags});
	CHECK_EQ(load_setting(Profile(), "sidebar-order"),
			 "lists:work-2, smart-lists, lists:home, lists:old, tags");
	CHECK_EQ(group_title(Home, "Home"), "Home");
	CHECK_EQ(group_title(Smart), "Smart Lists");

	// Lists groups: visible or collapsible (never hidden), folded per source.
	CHECK(!load_lists_layout(Profile(), "home").foldable());
	save_setting(Profile(), "local-lists-display", "hidden");
	CHECK(!load_lists_layout(Profile(), "home").hidden());
	save_group_display(Profile(), Home, GroupDisplay::Collapsible);
	CHECK_EQ(load_setting(Profile(), "local-lists-display"), "collapsible");
	save_group_collapsed(Profile(), Home, true);
	CHECK(load_lists_layout(Profile(), "home").folded());
	CHECK(!load_lists_layout(Profile(), "old").folded());

	// Smart lists: all six by default, not foldable.
	auto layout = load_smart_lists_layout(Profile());
	CHECK_EQ(layout.shown.size(), 6u);
	CHECK(layout.display == GroupDisplay::Visible);
	CHECK(!layout.foldable());
	CHECK(!layout.folded());
	save_setting(Profile(), "smart-lists", "Flagged, today,bogus, today");
	save_group_collapsed(Profile(), Smart, true);
	CHECK(!load_smart_lists_layout(Profile())
			   .folded());	// folding only applies when collapsible
	save_setting(Profile(), "smart-lists-display", "Collapsable");
	layout = load_smart_lists_layout(Profile());
	CHECK(layout.foldable());
	CHECK(layout.folded());
	CHECK_EQ(layout.shown.size(), 2u);
	CHECK_EQ(layout.shown[0], "flagged");
	CHECK_EQ(layout.shown[1], "today");
	CHECK(layout.collapsed);
	save_setting(Profile(), "smart-lists", "none");
	CHECK(load_smart_lists_layout(Profile()).hidden());
	save_setting(Profile(), "smart-lists", "today");
	save_setting(Profile(), "smart-lists-display", "hidden");
	CHECK(load_smart_lists_layout(Profile()).hidden());

	// Tags: visible (with a plain heading) by default.
	CHECK(load_tags_layout(Profile()).display == GroupDisplay::Visible);
	CHECK(!load_tags_layout(Profile()).foldable());
	save_group_collapsed(Profile(), Tags, true);
	CHECK(!load_tags_layout(Profile()).folded());
	save_setting(Profile(), "tags-display", "collapsible");
	CHECK(load_tags_layout(Profile()).folded());
	save_setting(Profile(), "tags-display", "hidden");
	CHECK(load_tags_layout(Profile()).hidden());

	// Hidden lists and tags; a name with a comma is quoted.
	CHECK(load_hidden(Profile()).lists.empty());
	set_list_hidden(Profile(), "Work", true);
	set_list_hidden(Profile(), "Smith, Jo", true);
	set_list_hidden(Profile(), "Work", true);  // already hidden
	CHECK_EQ(load_setting(Profile(), "lists-hidden"), "Work, \"Smith, Jo\"");
	auto hidden = load_hidden(Profile());
	CHECK_EQ(hidden.lists.size(), 2u);
	CHECK(hidden.list_hidden("Smith, Jo"));
	set_list_hidden(Profile(), "Work", false);
	CHECK(!load_hidden(Profile()).list_hidden("Work"));
	set_tag_hidden(Profile(), "errands", true);
	CHECK(load_hidden(Profile()).tag_hidden("errands"));
	CHECK(!load_hidden(Profile()).show);
	save_show_hidden(Profile(), true);
	CHECK(load_hidden(Profile()).show);

	// Hiding smart lists edits smart-lists; hiding the last one stores "none".
	save_setting(Profile(), "smart-lists", "today, flagged");
	set_smart_list_hidden(Profile(), "today", true);
	CHECK_EQ(load_setting(Profile(), "smart-lists"), "flagged");
	set_smart_list_hidden(Profile(), "flagged", true);
	CHECK_EQ(load_setting(Profile(), "smart-lists"), "none");
	set_smart_list_hidden(Profile(), "all", false);
	CHECK_EQ(load_setting(Profile(), "smart-lists"), "all");

	// Tag order: tags-order first, the rest alphabetically; moving skips
	// hidden.
	CHECK((order_tags(Profile(), {"work", "errands", "bakery"}) ==
		   std::vector<std::string>{"bakery", "errands", "work"}));
	save_names_setting(Profile(), "tags-order", {"work", "gone"});
	auto tag_order = order_tags(Profile(), {"work", "errands", "bakery"});
	CHECK((tag_order == std::vector<std::string>{"work", "bakery", "errands"}));
	CHECK((move_in_order(tag_order, "errands", -1,
						 {"work", "errands"})));  // past hidden bakery
	CHECK((tag_order == std::vector<std::string>{"errands", "bakery", "work"}));
	CHECK((!move_in_order(tag_order, "errands", -1, {"work", "errands"})));
	// Dragged next to another entry.
	std::vector<std::string> drag{"a", "b", "c", "d"};
	CHECK((move_next_to(drag, "a", "c", true)));
	CHECK((drag == std::vector<std::string>{"b", "c", "a", "d"}));
	CHECK((move_next_to(drag, "d", "b", false)));
	CHECK((drag == std::vector<std::string>{"d", "b", "c", "a"}));
	CHECK((!move_next_to(drag, "b", "c", false)));	// already there
	CHECK((!move_next_to(drag, "b", "b", true)));
	CHECK((!move_next_to(drag, "b", "gone", true)));
	CHECK((drag == std::vector<std::string>{"d", "b", "c", "a"}));
	// List order: lists-order first, then the store's order.
	CHECK((order_lists(Profile(), {"B", "A", "C"}) ==
		   std::vector<std::string>{"B", "A", "C"}));
	save_names_setting(Profile(), "lists-order", {"C", "Smith, Jo", "gone"});
	CHECK((order_lists(Profile(), {"B", "Smith, Jo", "C"}) ==
		   std::vector<std::string>{"C", "Smith, Jo", "B"}));
	save_smart_lists(Profile(), {});
	CHECK_EQ(load_setting(Profile(), "smart-lists"), "none");

	// Tag styles: gray tag icon by default; unknown values ignored.
	CHECK_EQ(load_tag_style(Profile(), "errands").color, "gray");
	CHECK_EQ(load_tag_style(Profile(), "errands").icon, "tag");
	save_tag_style(Profile(), "errands", {"orange", "cart"});
	CHECK_EQ(load_tag_style(Profile(), "errands").color, "orange");
	CHECK_EQ(load_tag_style(Profile(), "errands").icon, "cart");
	save_setting(Profile(), "tag-color.errands", "chartreuse");

	// How many lines of notes show: all unless note-lines is a number > 0.
	CHECK_EQ(load_note_lines(Profile()), 0u);
	save_setting(Profile(), "note-lines", "2");
	CHECK_EQ(load_note_lines(Profile()), 2u);
	save_setting(Profile(), "note-lines", "two");
	CHECK_EQ(load_note_lines(Profile()), 0u);
	CHECK_EQ(first_lines("a\nb\nc", 0), "a\nb\nc");
	CHECK_EQ(first_lines("a\nb\nc", 2), "a\nb…");
	CHECK_EQ(first_lines("a\nb\nc", 3), "a\nb\nc");
	CHECK_EQ(first_lines("a\nb\n", 2), "a\nb…");
	CHECK_EQ(first_lines("one line", 1), "one line");
	CHECK_EQ(load_tag_style(Profile(), "errands").color, "gray");
	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}
