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

// Reminders for pasted text, without ids (Store::add assigns them).
// - Checklist lines ("- [ ] …", "- [x] …") become reminders with all their
//   fields, notes and subtasks; other lines are ignored.
// - Otherwise each non-empty line becomes a reminder titled with it, inline
//   fields applied as when typing ("Call Sam #work 📅 2026-10-05"). A leading
//   bullet ("- ", "* ", "• ") is dropped.
std::vector<Reminder> from_clipboard_text(std::string_view text);

}  // namespace rem
