// Exporting lists to files: ☰ → Export… (any lists) and a list's ⋮ →
// Export… in the app, and `reminders export`. The kinds the importer reads
// (importer.hpp), so an exported list imports again as it was (less what a
// format can't hold):
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
//   todo.txt    a line per reminder, completed ones too ("x 2026-09-30 …"):
//               "(A)"–"(C)" priority, +tags, due:, and the extensions
//               time:, rec: (repeat; "every weekend" has none), flag:yes,
//               url:, id: and p: (a subtask's parent). No notes or
//               sections.
//   CSV         a header row (List, Section, Title, Done, Due Date, Due
//               Time, Priority, Flagged, Tags, Repeat, URL, Notes,
//               Completed, Created, ID, Parent ID), then a row per reminder
//               and subtask. Comma-separated, quoted as RFC 4180 says.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/library.hpp"
#include "reminders/model.hpp"
#include "reminders/store.hpp"

namespace rem {

enum class ExportFormat { Markdown, Text, Ics, Todotxt, Csv };

// "md" / "markdown", "txt" / "text", "ics", "todo.txt" / "todotxt", "csv"
// (any case, with or without a dot), or nullopt.
std::optional<ExportFormat> export_format(std::string_view name);
// The format a file name asks for (todo.txt, *.todo.txt; else by its
// extension), or nullopt.
std::optional<ExportFormat> export_format_for(const std::string& file_name);
// "md", "txt", "ics", "todo.txt", "csv".
std::string_view export_extension(ExportFormat format);

struct ExportOptions {
		bool completed = false;			 // plain text: completed reminders too
		std::chrono::sys_seconds now{};	 // iCalendar timestamps; zero: now
		const std::chrono::time_zone* zone = nullptr;  // null: the local zone
};

std::string export_markdown(const Document& doc);
std::string export_text(const Document& doc, bool completed);
std::string export_ics(const Document& doc, std::string_view name,
					   const ExportOptions& options = {});
std::string export_todotxt(const Document& doc);
// `list_name` fills the List column.
std::string export_csv(const Document& doc, std::string_view list_name);

// A list in the given format.
std::string export_list(const ListFile& list, ExportFormat format,
						const ExportOptions& options = {});

// Lists into `folder` (made if missing), a file each: NAME.EXT, or
// SOURCE-NAME.EXT where two sources have a list of that name. Existing
// files are replaced. Returns the files written.
std::vector<fs::path> export_lists(Library& library,
								   const std::vector<ListFile*>& lists,
								   const fs::path& folder, ExportFormat format,
								   const ExportOptions& options = {});
// The same lists as one .zip archive (the files as export_lists names
// them, compressed when the library has zlib), to write where you like.
std::string export_zip(Library& library, const std::vector<ListFile*>& lists,
					   ExportFormat format, const ExportOptions& options = {});
// Every list of the library, into a folder.
std::vector<fs::path> export_all(Library& library, const fs::path& folder,
								 ExportFormat format,
								 const ExportOptions& options = {});

}  // namespace rem
