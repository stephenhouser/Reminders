// Editing a reminder in the user's text editor, as YAML-style fields.
// Shared by `reminders edit NAME` and the TUI.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "reminders/library.hpp"

namespace editfile {

// The editable text for a reminder. `lists` are the lists' labels
// (Library::label) and `list` this one's.
std::string render(const rem::Ref& ref, const std::vector<std::string>& lists,
				   const std::string& list);

struct Edited {
		rem::LineFields fields;
		std::string notes;
		std::string list;
		std::optional<std::string> section;	 // nullopt: no section
		std::vector<std::string> subtasks;	 // Markdown lines ("- [ ] Title …")
		bool has_subtasks = false;			 // the file had a subtasks: key
};

// Parses edited text; throws std::runtime_error with a message for the user.
// Returns nullopt if the text has no fields left (the user emptied it: cancel).
std::optional<Edited> parse(const std::string& text, rem::Date today);

// Applies an edit to reminder `id` (fields, notes, subtasks, list, section,
// completion). Throws if the list doesn't exist.
void apply(rem::Library& store, const std::string& id, const Edited& e,
		   rem::Date today);

// Runs $VISUAL / $EDITOR (nano, else vi) on `text`; nullopt if the editor
// failed. The caller must have released the terminal.
std::optional<std::string> run_editor(const std::string& text);

// Runs the editor on a file in place; false if the editor failed.
bool run_editor_on(const std::filesystem::path& file);

enum class Outcome { Saved, Unchanged, Reverted };

// The whole loop: render, edit, parse, apply. If the edited text has a
// problem, asks on the terminal whether to edit it again (the default) or
// revert to how the reminder was. `apply_fn` wraps the apply (e.g. to record
// an undo step). Must run with the terminal in normal (not curses) mode.
Outcome edit(
	rem::Library& store, const std::string& id,
	const std::function<void(const std::function<void()>&)>& apply_fn = {});

}  // namespace editfile
