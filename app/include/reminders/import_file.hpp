// Importing a file into a list, as the terminal client's `import` command and
// the TUI's I key do: reading it, the list it goes into (one already there,
// or a new one named after the calendar or the file), importing, and a line
// saying what happened. The reading and merging are core importer.hpp's.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/importer.hpp"
#include "reminders/library.hpp"

namespace rem {

// The file's reminders: told apart by its content and name, or read as
// `kind`. Throws std::runtime_error when it can't be read or has no
// reminders to import.
Import read_import_file(const std::filesystem::path& path,
						std::optional<Import::Kind> kind = {});

// The name of the list an import makes when none is chosen: the calendar's,
// else the file's without its extension ("/" becomes "-").
std::string import_list_name(const Import& import,
							 const std::filesystem::path& path);

// The lists called `name` (case aside), or whose key ("source/name") it is;
// in `source`, or in every source when that's empty.
std::vector<ListFile*> lists_called(Library& library, std::string_view name,
									const std::string& source = {});

// Why `name` can't be a list's name ("Enter a name", "Names can't start
// with a dot", …), or "" when it can.
std::string list_name_error(std::string_view name);

struct ImportDone {
		std::string key;  // the list's; "" when nothing was new for a new list
		bool created = false;  // a new list
		ImportResult result;
};

// Imports into `list`, or, when that's null, into a new list `name` in
// `source` (in the import's colour). A new list nothing was added to is
// removed again.
ImportDone import_to(Library& library, ListFile* list,
					 const std::string& source, const std::string& name,
					 const Import& import, bool duplicates = false);

// What happened, in one line: "Imported 3 reminders (iCalendar) into
// Groceries (a new list); 1 was already there", or "Nothing to import: all 4
// reminders are already here".
std::string import_summary(Library& library, const ImportDone& done,
						   const Import& import);

}  // namespace rem
