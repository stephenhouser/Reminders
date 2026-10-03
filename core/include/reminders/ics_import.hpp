// Importing an iCalendar file (.ics) of tasks: ☰ → Import… in the app and
// `reminders import`. Each VTODO becomes a reminder, read as the CalDAV back
// end reads them (vtodo.hpp): title, notes, done, due, priority, repeat,
// tags, URL, flag, section, and subtasks (RELATED-TO; deeper ones go under
// the top parent). Events and other components aren't tasks: they're
// counted and skipped.
//
// A reminder's id comes from its task's UID (id_for_uid), so importing the
// same file again skips the tasks already imported instead of doubling them.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/library.hpp"
#include "reminders/model.hpp"

namespace rem {

struct IcsImport {
    std::string name;   // the calendar's name (X-WR-CALNAME), may be empty
    std::string color;  // the app colour nearest the calendar's, may be empty
    struct Item {
        Reminder reminder;  // with its id and subtasks
        std::optional<std::string> section;
    };
    std::vector<Item> items;  // top-level, in the calendar's order (X-APPLE-SORT-ORDER), else the file's
    int skipped = 0;          // events, journals and other components that aren't tasks
};

// Reads an .ics file's text. Throws std::runtime_error if it isn't iCalendar.
IcsImport read_ics(std::string_view text, const std::chrono::time_zone* local);

// A reminder id for a task's UID: the UID itself when it's usable as an id
// (6 or more lower-case letters and digits), else one derived from it.
std::string id_for_uid(std::string_view uid);

// Its reminders, subtasks included.
int reminder_count(const IcsImport& import);

struct ImportResult {
    int added = 0;    // reminders added, subtasks included
    int already = 0;  // skipped (with their subtasks): already in the library, by id
};

// Adds the reminders to the end of `list` (in their sections), skipping
// ones already in the library, and saves it once.
ImportResult import_into(Library& library, ListFile& list, const IcsImport& import);

}  // namespace rem
