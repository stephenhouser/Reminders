#include <fstream>

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

TEST(todotxt_reads_the_format) {
    auto imp = read_import("(A) 2026-09-01 Call Mom +family @phone due:2026-10-03 time:17:30 id:mom001\n"
                           "x 2026-09-30 2026-09-01 Pay rent pri:B rec:1m\n"
                           "  Ask about the boiler p:mom001 flag:yes\n"
                           "Water plants rec:+2w url:https://example.com/plants note:keep\n",
                           locate_zone("UTC"), "todo.txt");
    CHECK(imp.kind == Import::Kind::Todotxt);
    CHECK_EQ(imp.items.size(), std::size_t{3});
    auto& mom = imp.items[0].reminder;
    CHECK_EQ(mom.title, std::string("Call Mom"));
    CHECK(mom.priority == Priority::High);
    CHECK_EQ(mom.tags.size(), std::size_t{2});
    CHECK(mom.created.has_value() && mom.due_date.has_value() && mom.due_time.has_value());
    CHECK_EQ(mom.id, std::string("mom001"));
    CHECK_EQ(mom.subtasks.size(), std::size_t{1});
    CHECK(mom.subtasks[0].flagged);
    auto& rent = imp.items[1].reminder;
    CHECK(rent.done && rent.completed.has_value() && rent.created.has_value());
    CHECK(rent.priority == Priority::Medium);
    CHECK_EQ(rent.repeat.value_or(""), std::string("every month"));
    auto& plants = imp.items[2].reminder;
    CHECK_EQ(plants.repeat.value_or(""), std::string("every 2 weeks"));
    CHECK_EQ(plants.url.value_or(""), std::string("https://example.com/plants"));
    CHECK_EQ(plants.title, std::string("Water plants note:keep"));  // unknown keys stay in the title
}

TEST(todotxt_round_trip) {
    auto doc = parse(kList);
    auto text = export_todotxt(doc);
    CHECK(text.starts_with("(A) 2026-09-01 Milk +dairy due:2026-10-03 time:17:30 flag:yes id:milk01\n"));
    CHECK(text.find("x 2026-09-30 Whole id:whol01 p:milk01\n") != std::string::npos);
    CHECK(text.find("Bread due:2026-10-04 rec:1w id:brea01\n") != std::string::npos);
    auto imp = read_import(text, locate_zone("UTC"), "Groceries.todo.txt");
    CHECK(imp.kind == Import::Kind::Todotxt);
    CHECK_EQ(imp.items.size(), std::size_t{3});
    CHECK_EQ(imp.items[0].reminder.subtasks.size(), std::size_t{2});
    CHECK_EQ(imp.items[2].reminder.repeat.value_or(""), std::string("every week"));
    // Recognised without the name, too.
    CHECK(detect_kind(text) == Import::Kind::Todotxt);
    CHECK(detect_kind("Milk\nBread\nEggs\n") == Import::Kind::Text);
}

TEST(csv_reads_other_apps_columns) {
    // Semicolons, a quoted title with a comma, notes over two lines, a
    // parent named by title, and columns this app doesn't know.
    auto imp = read_import("\xEF\xBB\xBFTYPE;CONTENT;DESCRIPTION;PRIORITY;DUE DATE;Labels;Parent\r\n"
                           "task;\"Pack, then lock up\";\"Keys\r\nPassport\";1;2026-10-20 08:15;travel, home;\r\n"
                           "task;Charger;;4;;;\"Pack, then lock up\"\r\n"
                           ";;;;;;\r\n",
                           locate_zone("UTC"), "export.csv");
    CHECK(imp.kind == Import::Kind::Csv);
    CHECK_EQ(imp.items.size(), std::size_t{1});
    auto& pack = imp.items[0].reminder;
    CHECK_EQ(pack.title, std::string("Pack, then lock up"));
    CHECK_EQ(pack.notes, std::string("Keys\nPassport"));
    CHECK(pack.priority == Priority::High);
    CHECK(pack.due_date.has_value() && pack.due_time.has_value());
    CHECK_EQ(pack.tags.size(), std::size_t{2});
    CHECK_EQ(pack.subtasks.size(), std::size_t{1});
    CHECK(pack.subtasks[0].priority == Priority::None);  // Todoist's 4 is no priority
    bool threw = false;
    try {
        read_csv("a,b\n1,2\n");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(csv_round_trip) {
    auto doc = parse(kList);
    auto text = export_csv(doc, "Groceries");
    CHECK(text.starts_with("List,Section,Title,Done,Due Date,"));
    CHECK(text.find("Groceries,,Milk,,2026-10-03,17:30,high,yes,dairy,,,2% if they have it,,2026-09-01,milk01,\r\n") !=
          std::string::npos);
    CHECK(text.find("Groceries,Bakery,Bread,,2026-10-04,,,,,every week,,,,,brea01,\r\n") != std::string::npos);
    CHECK(detect_kind(text) == Import::Kind::Csv);  // by its header
    auto imp = read_import(text, locate_zone("UTC"));
    CHECK_EQ(imp.items.size(), std::size_t{3});
    auto& milk = imp.items[0].reminder;
    CHECK_EQ(milk.id, std::string("milk01"));
    CHECK_EQ(milk.notes, std::string("2% if they have it"));
    CHECK(milk.flagged && milk.priority == Priority::High && milk.due_time.has_value());
    CHECK_EQ(milk.subtasks.size(), std::size_t{2});
    CHECK(milk.subtasks[1].done && milk.subtasks[1].completed.has_value());
    CHECK_EQ(imp.items[2].section.value_or(""), std::string("Bakery"));
    auto quoted = export_csv(parse("---\nreminders: 1\n---\n- [ ] Say \"hi\", then go ^say001\n"), "L");
    CHECK(quoted.ends_with("\r\nL,,\"Say \"\"hi\"\", then go\",,,,,,,,,,,,say001,\r\n"));
}

TEST(export_all_lists) {
    auto dir = fs::temp_directory_path() / ("reminders-export-" + new_id());
    Library lib;
    for (auto name : {"home", "work"}) {
        fs::create_directories(dir / name);
        std::ofstream(dir / name / "Todo.md") << "---\nreminders: 1\n---\n- [ ] " << name << "\n";
        lib.add(SourceConfig{name, BackendKind::Local, dir / name, {}},
                std::make_unique<Store>(dir / name, dir / "state" / name, BackendKind::Local));
    }
    std::ofstream(dir / "home" / "Garden.md") << "---\nreminders: 1\n---\n- [ ] Weed\n";
    lib.load_all();
    auto files = export_all(lib, dir / "out", ExportFormat::Todotxt);
    CHECK_EQ(files.size(), std::size_t{3});
    CHECK(fs::exists(dir / "out" / "Garden.todo.txt"));
    CHECK(fs::exists(dir / "out" / "home-Todo.todo.txt"));  // two sources have a Todo
    CHECK(fs::exists(dir / "out" / "work-Todo.todo.txt"));
    fs::remove_all(dir);
}
