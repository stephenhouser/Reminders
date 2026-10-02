#include <cstdlib>
#include <fstream>

#include "reminders/dates.hpp"
#include "reminders/settings.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {
Date d(int y, unsigned m, unsigned day) { return Date{year{y}, month{m}, std::chrono::day{day}}; }
}  // namespace

TEST(human_dates) {
    auto today = d(2026, 10, 1);  // a Thursday
    CHECK(parse_human_date("today", today) == today);
    CHECK(parse_human_date("Tomorrow", today) == d(2026, 10, 2));
    CHECK(parse_human_date("2026-12-25", today) == d(2026, 12, 25));
    CHECK(parse_human_date("+3d", today) == d(2026, 10, 4));
    CHECK(parse_human_date("+2w", today) == d(2026, 10, 15));
    CHECK(parse_human_date("+1m", today) == d(2026, 11, 1));
    CHECK(parse_human_date("fri", today) == d(2026, 10, 2));
    CHECK(parse_human_date("thursday", today) == d(2026, 10, 8));  // next week, never today
    CHECK(!parse_human_date("someday", today));
    CHECK(!parse_human_date("fr", today));
    CHECK(!parse_human_date("+x", today));
}

TEST(relative_dates) {
    auto today = d(2026, 10, 1);
    CHECK_EQ(relative_date(today, today), "Today");
    CHECK_EQ(relative_date(d(2026, 10, 2), today), "Tomorrow");
    CHECK_EQ(relative_date(d(2026, 10, 5), today), "Monday");
    CHECK_EQ(relative_date(d(2026, 12, 25), today), "Dec 25");
    CHECK_EQ(relative_date(d(2027, 1, 3), today), "Jan 3, 2027");
}

TEST(settings_round_trip_keeps_other_lines) {
    auto dir = fs::temp_directory_path() / ("reminders-settings-" + new_id());
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    fs::create_directories(dir / "reminders");
    std::ofstream(dir / "reminders" / "settings.ini") << "# mine\n[other]\nfolder=x\n[general]\nview=all\n";
    CHECK_EQ(load_setting("folder"), "");
    CHECK_EQ(load_setting("view"), "all");
    save_setting("folder", "/tmp/lists");
    save_setting("view", "today");
    CHECK_EQ(load_setting("folder"), "/tmp/lists");
    std::ifstream in(dir / "reminders" / "settings.ini");
    std::string all((std::istreambuf_iterator<char>(in)), {});
    CHECK_EQ(all, "# mine\n[other]\nfolder=x\n[general]\nview=today\nfolder=/tmp/lists\n");
    CHECK(device_name().find('-') != std::string::npos);
    CHECK(!load_bool_setting("show-key-numbers"));
    save_setting("show-key-numbers", "Yes");
    CHECK(load_bool_setting("show-key-numbers"));
    CHECK_EQ(with_key_number("Today", 0, true), "(1) Today");
    CHECK_EQ(with_key_number("Tenth", 9, true), "(0) Tenth");
    CHECK_EQ(with_key_number("Eleventh", 10, true), "Eleventh");
    CHECK_EQ(with_key_number("Today", 0, false), "Today");

    // Group order: smart lists, my lists, tags by default; missing ones go last.
    using enum SidebarGroup;
    CHECK((load_sidebar_order() == std::vector{SmartLists, MyLists, Tags}));
    save_setting("sidebar-order", "tags, My Lists, bogus, tags");
    CHECK((load_sidebar_order() == std::vector{Tags, MyLists, SmartLists}));
    auto order = load_sidebar_order();
    CHECK((move_sidebar_group(order, SmartLists, -1, {SmartLists, MyLists, Tags})));
    CHECK((order == std::vector{Tags, SmartLists, MyLists}));
    CHECK((!move_sidebar_group(order, SmartLists, -1, {SmartLists, MyLists})));  // only hidden Tags above
    CHECK((move_sidebar_group(order, Tags, 1, {SmartLists, MyLists, Tags})));
    CHECK((move_sidebar_group(order, MyLists, -1, {SmartLists, MyLists})));  // past Tags, which isn't showing
    CHECK((order == std::vector{MyLists, Tags, SmartLists}));
    CHECK((!move_sidebar_group(order, SmartLists, 1, {SmartLists, MyLists, Tags})));
    save_sidebar_order(order);
    CHECK_EQ(load_setting("sidebar-order"), "my-lists, tags, smart-lists");
    CHECK(load_sidebar_order() == order);

    // My Lists: visible or collapsible, never hidden.
    CHECK(!load_my_lists_layout().foldable());
    save_setting("my-lists-display", "hidden");
    CHECK(!load_my_lists_layout().hidden());
    save_group_display(MyLists, GroupDisplay::Collapsible);
    CHECK_EQ(load_setting("my-lists-display"), "collapsible");
    save_group_collapsed(MyLists, true);
    CHECK(load_my_lists_layout().folded());

    // Smart lists: all six by default, not foldable.
    auto layout = load_smart_lists_layout();
    CHECK_EQ(layout.shown.size(), 6u);
    CHECK(layout.display == GroupDisplay::Visible);
    CHECK(!layout.foldable());
    CHECK(!layout.folded());
    save_setting("smart-lists", "Flagged, today,bogus, today");
    save_group_collapsed(SmartLists, true);
    CHECK(!load_smart_lists_layout().folded());  // folding only applies when collapsible
    save_setting("smart-lists-display", "Collapsable");
    layout = load_smart_lists_layout();
    CHECK(layout.foldable());
    CHECK(layout.folded());
    CHECK_EQ(layout.shown.size(), 2u);
    CHECK_EQ(layout.shown[0], "flagged");
    CHECK_EQ(layout.shown[1], "today");
    CHECK(layout.collapsed);
    save_setting("smart-lists", "none");
    CHECK(load_smart_lists_layout().hidden());
    save_setting("smart-lists", "today");
    save_setting("smart-lists-display", "hidden");
    CHECK(load_smart_lists_layout().hidden());

    // Tags: visible (with a plain heading) by default.
    CHECK(load_tags_layout().display == GroupDisplay::Visible);
    CHECK(!load_tags_layout().foldable());
    save_group_collapsed(Tags, true);
    CHECK(!load_tags_layout().folded());
    save_setting("tags-display", "collapsible");
    CHECK(load_tags_layout().folded());
    save_setting("tags-display", "hidden");
    CHECK(load_tags_layout().hidden());

    // Hidden lists and tags; a name with a comma is quoted.
    CHECK(load_hidden().lists.empty());
    set_list_hidden("Work", true);
    set_list_hidden("Smith, Jo", true);
    set_list_hidden("Work", true);  // already hidden
    CHECK_EQ(load_setting("lists-hidden"), "Work, \"Smith, Jo\"");
    auto hidden = load_hidden();
    CHECK_EQ(hidden.lists.size(), 2u);
    CHECK(hidden.list_hidden("Smith, Jo"));
    set_list_hidden("Work", false);
    CHECK(!load_hidden().list_hidden("Work"));
    set_tag_hidden("errands", true);
    CHECK(load_hidden().tag_hidden("errands"));
    CHECK(!load_hidden().show);
    save_show_hidden(true);
    CHECK(load_hidden().show);

    // Hiding smart lists edits smart-lists; hiding the last one stores "none".
    save_setting("smart-lists", "today, flagged");
    set_smart_list_hidden("today", true);
    CHECK_EQ(load_setting("smart-lists"), "flagged");
    set_smart_list_hidden("flagged", true);
    CHECK_EQ(load_setting("smart-lists"), "none");
    set_smart_list_hidden("all", false);
    CHECK_EQ(load_setting("smart-lists"), "all");

    // Tag order: tags-order first, the rest alphabetically; moving skips hidden.
    CHECK((order_tags({"work", "errands", "bakery"}) == std::vector<std::string>{"bakery", "errands", "work"}));
    save_names_setting("tags-order", {"work", "gone"});
    auto tag_order = order_tags({"work", "errands", "bakery"});
    CHECK((tag_order == std::vector<std::string>{"work", "bakery", "errands"}));
    CHECK((move_in_order(tag_order, "errands", -1, {"work", "errands"})));  // past hidden bakery
    CHECK((tag_order == std::vector<std::string>{"errands", "bakery", "work"}));
    CHECK((!move_in_order(tag_order, "errands", -1, {"work", "errands"})));
    // List order: lists-order first, then the store's order.
    CHECK((order_lists({"B", "A", "C"}) == std::vector<std::string>{"B", "A", "C"}));
    save_names_setting("lists-order", {"C", "Smith, Jo", "gone"});
    CHECK((order_lists({"B", "Smith, Jo", "C"}) == std::vector<std::string>{"C", "Smith, Jo", "B"}));
    save_smart_lists({});
    CHECK_EQ(load_setting("smart-lists"), "none");

    // Tag styles: gray tag icon by default; unknown values ignored.
    CHECK_EQ(load_tag_style("errands").color, "gray");
    CHECK_EQ(load_tag_style("errands").icon, "tag");
    save_tag_style("errands", {"orange", "cart"});
    CHECK_EQ(load_tag_style("errands").color, "orange");
    CHECK_EQ(load_tag_style("errands").icon, "cart");
    save_setting("tag-color.errands", "chartreuse");
    CHECK_EQ(load_tag_style("errands").color, "gray");
    unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(dir);
}
