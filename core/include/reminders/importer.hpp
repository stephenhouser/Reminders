// Importing reminders from a file: ☰ → Import… in the app and `reminders
// import`. Five kinds, told apart by the file's name and content
// (detect_kind), or chosen (read_as):
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
//   todo.txt          one task per line (todotxt.org): "x" and dates, "(A)"
//                     priority, +project and @context as tags, due:, and the
//                     common extensions rec: (repeat), id: and p: (parent,
//                     as topydo writes it), plus this app's time:, flag:
//                     and url:.
//   CSV               a header row, then a reminder per row. Columns are
//                     found by name, as this app and others call them
//                     (Title / Name / Task / Content, Due / Due Date, Tags /
//                     Labels, Notes / Description, …); the delimiter (comma,
//                     semicolon or tab) is found from the header.
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
    enum class Kind { Ics, Markdown, Text, Todotxt, Csv };
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
// todo.txt lines.
Import read_todotxt(std::string_view text);
// A CSV file with a header row. Throws std::runtime_error if no column
// holds titles.
Import read_csv(std::string_view text);

// "iCalendar", "Markdown", "plain text", "todo.txt", "CSV".
std::string_view kind_name(Import::Kind kind);
// What a file is: iCalendar if it starts with BEGIN:VCALENDAR; then by its
// name (.csv; todo.txt, done.txt, *.todo.txt; .md); then by content:
// Markdown with checklist lines, CSV with a title column in its header,
// todo.txt when most lines look like it, else plain text.
Import::Kind detect_kind(std::string_view text, std::string_view file_name = {});
Import read_as(std::string_view text, Import::Kind kind, const std::chrono::time_zone* local);
// read_as(text, detect_kind(text, file_name)).
Import read_import(std::string_view text, const std::chrono::time_zone* local, std::string_view file_name = {});

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
