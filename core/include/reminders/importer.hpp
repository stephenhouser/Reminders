// Importing reminders from a file: ☰ → Import… in the app and `reminders
// import`. Three kinds, told apart by their content (read_import):
//
//   iCalendar (.ics)  each VTODO becomes a reminder, read as the CalDAV back
//                     end reads them (vtodo.hpp): title, notes, done, due,
//                     priority, repeat, tags, URL, flag, section, and
//                     subtasks (RELATED-TO; deeper ones go under the top
//                     parent). Events and other components aren't tasks:
//                     they're counted and skipped.
//   Markdown          a checklist ("- [ ] …"), in this app's format or not:
//                     the reminders with their fields, notes, subtasks,
//                     sections and ids; the list's colour.
//   plain text        one reminder per line, with inline fields as in quick
//                     entry ("Pay rent #home 📅 2026-10-31"). Bullets and
//                     numbering are dropped, "# Heading" lines start a
//                     section, indented lines are subtasks of the line above.
//
// Reminders keep their ids: from the file (^id), or for a task from its UID
// (id_for_uid). Importing the same file again skips the reminders already
// imported instead of doubling them. Reminders without ids (plain text,
// hand-written Markdown) get new ones; one is skipped if the list already
// has an open reminder with the same title.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/library.hpp"
#include "reminders/model.hpp"

namespace rem {

struct Import {
    enum class Kind { Ics, Markdown, Text };
    Kind kind = Kind::Text;
    std::string name;   // the calendar's name (X-WR-CALNAME), may be empty
    std::string color;  // the app colour of the calendar or list, may be empty
    struct Item {
        Reminder reminder;  // with its id (may be empty) and subtasks
        std::optional<std::string> section;
    };
    std::vector<Item> items;  // top-level, in order
    int skipped = 0;          // .ics: events and other components that aren't tasks
};

// Reads an .ics file's text. Throws std::runtime_error if it isn't iCalendar.
Import read_ics(std::string_view text, const std::chrono::time_zone* local);
// A Markdown checklist's reminders (none if it has no "- [ ]" lines).
Import read_markdown(std::string_view text);
// One reminder per non-empty line.
Import read_plain_text(std::string_view text);
// Whichever of the three the text is: iCalendar if it starts with
// BEGIN:VCALENDAR, Markdown if it has checklist lines, else plain text.
Import read_import(std::string_view text, const std::chrono::time_zone* local);

// A reminder id for a task's UID: the UID itself when it's usable as an id
// (6 or more lower-case letters and digits), else one derived from it.
std::string id_for_uid(std::string_view uid);

// Its reminders, subtasks included.
int reminder_count(const Import& import);

struct ImportResult {
    int added = 0;    // reminders added, subtasks included
    int already = 0;  // skipped (with their subtasks): already in the library (by id) or the list (by title)
};

// Adds the reminders to the end of `list` (in their sections), skipping
// ones already there (see above), and saves it once. Reminders without an
// id get a new one.
ImportResult import_into(Library& library, ListFile& list, const Import& import);

}  // namespace rem
