// Copying and pasting reminders as text: the Markdown a list file uses, so a
// copied reminder pastes into an editor or another app as a checklist line,
// and pasted text becomes reminders.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "reminders/model.hpp"

namespace rem {

// A reminder, its notes and its subtasks as list-file Markdown, without ids
// ("- [ ] Milk #errands 📅 2026-10-03\n  2% if they have it\n").
std::string to_clipboard_text(const Reminder& r);

// How pasted or dropped text of several lines becomes reminders.
enum class TextSplit {
	Auto,	// a reminder per line when it's a list (is_list_text), else One
	Lines,	// a reminder per line
	One,	// one reminder: the first line is its title, the rest its notes
};

// Reminders for pasted text, without ids (Store::add assigns them).
// - A reminder per line (Lines, or Auto for a list):
//   - Checklist lines ("- [ ] …", "- [x] …") become reminders with all their
//     fields, notes and subtasks; other lines are ignored.
//   - Otherwise each non-empty line becomes a reminder titled with it,
//     inline fields applied as when typing ("Call Sam #work 📅 2026-10-05").
//     A leading bullet ("- ", "* ", "+ ", "• ") or number ("1. ", "2) ") is
//     dropped.
// - One reminder (One, or Auto for anything else): the first non-empty line
//   (bullet dropped, fields applied) is its title, the lines after it its
//   notes, as they are (blank lines at either end dropped), except that
//   checklist lines become "☐ …" / "☑ …" so they stay notes when read back.
// One line of text is one reminder whichever way.
std::vector<Reminder> from_clipboard_text(std::string_view text,
										  TextSplit split = TextSplit::Auto);

// Whether text is a list (Auto splits it): checklist lines, or bulleted or
// numbered lines making up at least half of its non-empty lines (so a
// heading above the items still counts).
bool is_list_text(std::string_view text);

// How many non-empty lines text has.
std::size_t text_line_count(std::string_view text);

}  // namespace rem
