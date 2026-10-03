#include <fstream>
#include <sstream>

#include "reminders/format.hpp"
#include "reminders/ics_import.hpp"
#include "test.hpp"

using namespace rem;

namespace {

#define MARK "---\nreminders: 1\n---\n"

const auto* utc() { return std::chrono::locate_zone("UTC"); }

// Three tasks (one a subtask of another, one done, one in a section), an
// event, and a time zone definition.
constexpr std::string_view kCalendar =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Other//EN\r\n"
    "X-WR-CALNAME:Trip\r\nX-APPLE-CALENDAR-COLOR:#34C759FF\r\n"
    "BEGIN:VTIMEZONE\r\nTZID:Europe/Paris\r\nEND:VTIMEZONE\r\n"
    "BEGIN:VTODO\r\nUID:6F1C2D3E-AAAA-4BBB-8CCC-1234567890AB\r\nSUMMARY:Book flights\r\n"
    "DUE;VALUE=DATE:20261020\r\nPRIORITY:1\r\nCATEGORIES:travel\r\nX-APPLE-SORT-ORDER:1\r\nEND:VTODO\r\n"
    "BEGIN:VEVENT\r\nUID:ev-1\r\nSUMMARY:Flight\r\nDTSTART:20261101T080000Z\r\nEND:VEVENT\r\n"
    "BEGIN:VTODO\r\nUID:packing01\r\nSUMMARY:Pack\r\nX-REMINDERS-SECTION:Before\r\nX-APPLE-SORT-ORDER:2\r\n"
    "END:VTODO\r\n"
    "BEGIN:VTODO\r\nUID:sub-uid\r\nSUMMARY:Passport\r\nRELATED-TO:packing01\r\nSTATUS:COMPLETED\r\n"
    "COMPLETED:20260930T120000Z\r\nX-APPLE-SORT-ORDER:3\r\nEND:VTODO\r\n"
    "END:VCALENDAR\r\n";

struct Folder {
    fs::path dir = fs::temp_directory_path() / ("reminders-ics-" + new_id());
    Library lib;
    Folder() {
        fs::create_directories(dir / "lists");
        std::ofstream(dir / "lists" / "Trip.md") << MARK "- [ ] Visa ^visa01\n";
        lib.add(SourceConfig{"home", BackendKind::Local, dir / "lists", {}},
                std::make_unique<Store>(dir / "lists", dir / "state", BackendKind::Local));
        lib.load_all();
    }
    ~Folder() { fs::remove_all(dir); }
    std::string text() {
        std::ifstream in(dir / "lists" / "Trip.md");
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

}  // namespace

TEST(ics_reads_tasks) {
    auto imp = read_ics(kCalendar, utc());
    CHECK_EQ(imp.name, std::string("Trip"));
    CHECK_EQ(imp.color, std::string("green"));
    CHECK_EQ(imp.skipped, 1);  // the event
    CHECK_EQ(imp.items.size(), std::size_t{2});
    auto& flights = imp.items[0].reminder;
    CHECK_EQ(flights.title, std::string("Book flights"));
    CHECK(flights.priority == Priority::High);
    CHECK(flights.due_date.has_value());
    CHECK_EQ(flights.tags.size(), std::size_t{1});
    CHECK_EQ(flights.id, id_for_uid("6F1C2D3E-AAAA-4BBB-8CCC-1234567890AB"));
    auto& pack = imp.items[1];
    CHECK_EQ(pack.reminder.id, std::string("packing01"));  // a usable UID is the id
    CHECK_EQ(pack.section.value_or(""), std::string("Before"));
    CHECK_EQ(pack.reminder.subtasks.size(), std::size_t{1});
    CHECK(pack.reminder.subtasks[0].done);
}

TEST(ics_ids_from_uids) {
    auto a = id_for_uid("6F1C2D3E-AAAA-4BBB-8CCC-1234567890AB");
    CHECK_EQ(a, id_for_uid("6F1C2D3E-AAAA-4BBB-8CCC-1234567890AB"));
    CHECK(a != id_for_uid("6F1C2D3E-AAAA-4BBB-8CCC-1234567890AC"));
    CHECK_EQ(a.size(), std::size_t{10});
    CHECK_EQ(id_for_uid("abc123"), std::string("abc123"));
    CHECK_EQ(id_for_uid("ABC123").size(), std::size_t{10});  // upper case isn't an id
}

TEST(ics_rejects_other_files) {
    bool threw = false;
    try {
        read_ics("# Just Markdown\n", utc());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(ics_imports_into_a_list_once) {
    Folder f;
    auto imp = read_ics(kCalendar, utc());
    auto* list = f.lib.list("Trip");
    auto r = import_into(f.lib, *list, imp);
    CHECK_EQ(reminder_count(imp), 3);
    CHECK_EQ(r.added, 3);  // the subtask too
    CHECK_EQ(r.already, 0);
    auto text = f.text();
    CHECK(text.starts_with(MARK "- [ ] Visa ^visa01\n- [ ] Book flights"));
    CHECK(text.find("\n## Before\n- [ ] Pack") != std::string::npos);
    CHECK(text.find("  - [x] Passport") != std::string::npos);

    // Again: nothing new, nothing written.
    auto before = text;
    r = import_into(f.lib, *f.lib.list("Trip"), imp);
    CHECK_EQ(r.added, 0);
    CHECK_EQ(r.already, 3);
    CHECK_EQ(f.text(), before);
}

TEST(ics_tasks_without_order_keep_the_files) {
    auto imp = read_ics("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
                        "BEGIN:VTODO\r\nUID:zzzzzz\r\nSUMMARY:First\r\nEND:VTODO\r\n"
                        "BEGIN:VTODO\r\nUID:aaaaaa\r\nSUMMARY:Second\r\nX-APPLE-SORT-ORDER:1\r\nEND:VTODO\r\n"
                        "END:VCALENDAR\r\n",
                        utc());
    CHECK_EQ(imp.items.size(), std::size_t{2});
    CHECK_EQ(imp.items[0].reminder.title, std::string("First"));
    CHECK(imp.name.empty());
}
