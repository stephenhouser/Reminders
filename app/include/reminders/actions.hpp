// Edits of several reminders (a selection, the marked ones, or just one),
// with the rules both apps follow. They change the library and save; the
// apps make each call one undo step, with saves held (Library::hold_saves)
// so each list file is written once.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "reminders/clipboard.hpp"
#include "reminders/dates.hpp"
#include "reminders/library.hpp"
#include "reminders/view.hpp"

namespace rem {

using Ids = std::vector<std::string>;

// Without the subtasks whose parent is there too: moving, deleting or
// copying a reminder takes its subtasks along.
Ids outermost(Library& library, const Ids& ids);

bool all_done(Library& library, const Ids& ids);
bool all_flagged(Library& library, const Ids& ids);

// Completes them all, or, when they all were, makes them all not
// completed. Returns whether they're now completed.
bool complete(Library& library, const Ids& ids, Date today);
// Flags them all, or unflags them when they all were. Returns whether
// they're now flagged.
bool toggle_flag(Library& library, const Ids& ids);
// The due date (and the time, when given; else any time stays).
void set_due(Library& library, const Ids& ids, std::optional<Date> due,
			 std::optional<TimeOfDay> time = std::nullopt);
void set_priority(Library& library, const Ids& ids, Priority priority);
// Adds a tag to them all, or removes it.
void set_tag(Library& library, const Ids& ids, const std::string& tag,
			 bool add);

// Moves them together, keeping their order: the first next to `target`
// (in its list, before or after it), each other after the one before.
// False if one couldn't go there (one with subtasks next to a subtask).
bool move_next_to(Library& library, const Ids& ids, const std::string& target,
				  Document::Place place);
// To the end of a section of `list` (nullopt: its first, unnamed one).
void move_to_section_end(Library& library, const Ids& ids, ListFile& list,
						 const std::optional<std::string>& section);
// To the end of `list`; returns how many moved (those already there don't).
int move_to_list(Library& library, const Ids& ids, ListFile& list);
// Deletes them (outermost); returns how many.
int remove(Library& library, const Ids& ids);

// As Markdown (clipboard.hpp), as Copy gives them and other apps get them.
std::string as_text(Library& library, const Ids& ids);

// Text pasted or dropped, added as reminders (from_clipboard_text).
struct TextPlace {
		// The list (a key); empty: the list in `view`, else (a smart list)
		// the first one, and the reminders are set up to show in the view:
		// due today in Today or Scheduled, flagged in Flagged, tagged in a
		// tag's view.
		std::string list;
		// Next to this reminder (before or after it; a subtask: its parent),
		// else at the end.
		std::optional<std::string> anchor;
		Document::Place place = Document::Place::After;
};
struct AddedText {
		ListFile* list = nullptr;  // nullptr: no list to add to
		Ids ids;				   // the new reminders, in order
};
AddedText add_text(Library& library, const std::string& text, TextSplit split,
				   const View& view, const TextPlace& where, Date today);

}  // namespace rem
