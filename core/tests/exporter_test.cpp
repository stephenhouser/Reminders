#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/importer.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

constexpr std::string_view kList =
    "---\nreminders: 1\ncolor: orange\nicon: cart\n---\n"
    "- [ ] Milk #dairy ⏫ 🚩 📅 2026-10-03 17:30 ➕ 2026-09-01 ^milk01\n"
    "  2% if they have it\n"
    "  - [ ] Skim ^skim01\n"
    "  - [x] Whole ✅ 2026-09-30 ^whol01\n"
    "- [x] Eggs ✅ 2026-09-30 ^eggs01\n"
    "\n"
    "## Bakery\n"
    "- [ ] Bread 🔁 every week 📅 2026-10-04 ^brea01\n";

ExportOptions fixed() {
    ExportOptions o;
    o.now = sys_days{2026y / 10 / 3} + 9h;
    o.zone = locate_zone("UTC");
    return o;
}

}  // namespace

TEST(export_formats_by_name) {
    CHECK(export_format("MD") == ExportFormat::Markdown);
    CHECK(export_format(".txt") == ExportFormat::Text);
    CHECK(export_format("ics") == ExportFormat::Ics);
    CHECK(!export_format("pdf").has_value());
    CHECK(export_format_for("~/Out/Groceries.ics") == ExportFormat::Ics);
    CHECK(!export_format_for("Groceries").has_value());
    CHECK(!export_format_for("a.b/Groceries").has_value());
    CHECK_EQ(export_extension(ExportFormat::Text), std::string_view("txt"));
}

TEST(export_markdown_is_the_list) {
    auto doc = parse(kList);
    CHECK_EQ(export_markdown(doc), std::string(kList));
}

TEST(export_text_lines) {
    auto doc = parse(kList);
    CHECK_EQ(export_text(doc, false),
             std::string("Milk #dairy ⏫ 🚩 📅 2026-10-03 17:30\n"
                         "  Skim\n"
                         "\n"
                         "# Bakery\n"
                         "Bread 🔁 every week 📅 2026-10-04\n"));
    auto all = export_text(doc, true);
    CHECK(all.find("  Whole ✅ 2026-09-30\n") != std::string::npos);
    CHECK(all.find("Eggs ✅ 2026-09-30\n") != std::string::npos);
    // Back in: the same reminders, sections and subtasks.
    auto imp = read_import(export_text(doc, false), locate_zone("UTC"));
    CHECK(imp.kind == Import::Kind::Text);
    CHECK_EQ(imp.items.size(), std::size_t{2});
    CHECK(imp.items[0].reminder.flagged);
    CHECK_EQ(imp.items[0].reminder.subtasks.size(), std::size_t{1});
    CHECK_EQ(imp.items[1].section.value_or(""), std::string("Bakery"));
    CHECK_EQ(imp.items[1].reminder.repeat.value_or(""), std::string("every week"));
}

TEST(export_ics_round_trip) {
    auto doc = parse(kList);
    auto text = export_ics(doc, "Groceries", fixed());
    CHECK(text.find("X-WR-CALNAME:Groceries\r\n") != std::string::npos);
    CHECK(text.find("X-APPLE-CALENDAR-COLOR:#FF9500\r\n") != std::string::npos);
    CHECK(text.find("UID:milk01\r\n") != std::string::npos);
    auto imp = read_import(text, locate_zone("UTC"));
    CHECK(imp.kind == Import::Kind::Ics);
    CHECK_EQ(imp.name, std::string("Groceries"));
    CHECK_EQ(imp.color, std::string("orange"));
    CHECK_EQ(imp.items.size(), std::size_t{3});
    auto& milk = imp.items[0].reminder;
    CHECK_EQ(milk.id, std::string("milk01"));
    CHECK_EQ(milk.title, std::string("Milk"));
    CHECK_EQ(milk.notes, std::string("2% if they have it"));
    CHECK(milk.flagged && milk.priority == Priority::High);
    CHECK(milk.due_time.has_value());
    CHECK_EQ(milk.subtasks.size(), std::size_t{2});
    CHECK(milk.subtasks[1].done);
    CHECK(imp.items[1].reminder.done);
    CHECK_EQ(imp.items[2].section.value_or(""), std::string("Bakery"));
    CHECK_EQ(imp.items[2].reminder.repeat.value_or(""), std::string("every week"));
}

TEST(export_ics_gives_ids_to_lines_without) {
    auto doc = parse("---\nreminders: 1\n---\n- [ ] Parent\n  - [ ] Child\n");
    auto imp = read_import(export_ics(doc, "X", fixed()), locate_zone("UTC"));
    CHECK_EQ(imp.items.size(), std::size_t{1});
    CHECK_EQ(imp.items[0].reminder.subtasks.size(), std::size_t{1});  // still its parent's
}
