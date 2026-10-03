// Exporting a list to a file: ⋮ → Export… in the app and `reminders
// export`. The same three kinds the importer reads (importer.hpp), so an
// exported list imports again as it was:
//
//   Markdown    the list file itself (docs/FORMAT.md): everything, ids
//               included.
//   plain text  one line per open reminder (completed ones only when
//               asked), with its fields inline as in quick entry ("Pay rent
//               #home 📅 2026-10-31"); subtasks indented two spaces;
//               sections as "# Heading" lines. No ids or notes.
//   iCalendar   a VCALENDAR named after the list (X-WR-CALNAME, its colour
//               as X-APPLE-CALENDAR-COLOR), one VTODO per reminder, mapped
//               as the CalDAV back end maps them (vtodo.hpp). A reminder's
//               id is its UID, so importing it back finds the same ids.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "reminders/model.hpp"
#include "reminders/store.hpp"

namespace rem {

enum class ExportFormat { Markdown, Text, Ics };

// "md" / "markdown", "txt" / "text", "ics" (any case, with or without a
// dot), or nullopt.
std::optional<ExportFormat> export_format(std::string_view name);
// The format a file name's extension asks for, or nullopt.
std::optional<ExportFormat> export_format_for(const std::string& file_name);
// "md", "txt", "ics".
std::string_view export_extension(ExportFormat format);

struct ExportOptions {
    bool completed = false;  // plain text: completed reminders too
    std::chrono::sys_seconds now{};                // iCalendar timestamps; zero: now
    const std::chrono::time_zone* zone = nullptr;  // null: the local zone
};

std::string export_markdown(const Document& doc);
std::string export_text(const Document& doc, bool completed);
std::string export_ics(const Document& doc, std::string_view name, const ExportOptions& options = {});

// A list in the given format.
std::string export_list(const ListFile& list, ExportFormat format, const ExportOptions& options = {});

}  // namespace rem
