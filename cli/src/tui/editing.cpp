#include <ncurses.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cwchar>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../editfile.hpp"
#include "../text.hpp"
#include "internal.hpp"
#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
#include "reminders/settings.hpp"
#include "reminders/sync_runner.hpp"

namespace tui {

// Edits one line of text in `width` cells at (y, x), with a cursor: ←/→,
// Home/End (Ctrl+A/E), Backspace/Delete, Ctrl+U clears, Ctrl+K cuts to the
// end. Enter or Ctrl+S accepts, Esc cancels (nullopt).
std::optional<std::string> Tui::edit_line(int y, int x, int width,
										  const std::string& initial,
										  attr_t attr) {
	auto text = widen(initial);
	std::size_t cur = text.size();
	curs_set(1);
	std::optional<std::string> result;
	while (true) {
		// Scroll sideways so the cursor stays visible.
		std::size_t start = 0;
		while (start < cur &&
			   text_width(text.substr(start, cur - start)) > width - 1) {
			++start;
		}
		attrset(attr);
		mvhline(y, x, ' ', width);
		std::wstring shown;
		int used = 0;
		for (auto i = start; i < text.size(); ++i) {
			int cw = cell_width(text[i]);
			if (used + cw > width) {
				break;
			}
			shown += text[i];
			used += cw;
		}
		mvaddnwstr(y, x, shown.c_str(), static_cast<int>(shown.size()));
		attrset(A_NORMAL);
		move(y, x + text_width(text.substr(start, cur - start)));
		refresh();

		wint_t ch;
		int kind = get_wch(&ch);
		if (kind == ERR) {
			continue;
		}
		if (kind == KEY_CODE_YES) {
			switch (ch) {
				case KEY_LEFT:
					if (cur > 0) {
						--cur;
					}
					break;
				case KEY_RIGHT:
					if (cur < text.size()) {
						++cur;
					}
					break;
				case KEY_HOME:
					cur = 0;
					break;
				case KEY_END:
					cur = text.size();
					break;
				case KEY_BACKSPACE:
					if (cur > 0) {
						text.erase(--cur, 1);
					}
					break;
				case KEY_DC:
					if (cur < text.size()) {
						text.erase(cur, 1);
					}
					break;
				case KEY_ENTER:
					result = narrow(text);
					break;
			}
			if (result) {
				break;
			}
			continue;
		}
		if (ch == 27) {
			break;	// Esc: cancel
		}
		if (ch == '\n' || ch == '\r' ||
			ch == 19) {	 // Enter, or Ctrl+S as in the GNOME app
			result = narrow(text);
			break;
		}
		switch (ch) {
			case 127:
			case 8:
				if (cur > 0) {
					text.erase(--cur, 1);
				}
				break;
			case 1:
				cur = 0;
				break;	// Ctrl+A
			case 5:
				cur = text.size();
				break;	// Ctrl+E
			case 21:
				text.clear(), cur = 0;
				break;	// Ctrl+U
			case 11:
				text.erase(cur);
				break;	// Ctrl+K
			default:
				if (ch >= 32) {
					text.insert(cur++, 1, static_cast<wchar_t>(ch));
				}
		}
	}
	curs_set(0);
	return result;
}

std::optional<std::string> Tui::prompt(const std::string& label,
									   const std::string& initial) {
	attron(COLOR_PAIR(kStatus));
	mvhline(LINES - 1, 0, ' ', COLS);
	int used = put(LINES - 1, 0, " " + label + " ", COLS);
	attroff(COLOR_PAIR(kStatus));
	return edit_line(LINES - 1, used, std::max(1, COLS - used - 1), initial,
					 COLOR_PAIR(kStatus));
}

// Enter / F2: edit the selected reminder's title where it's shown. As in the
// GNOME app, fields typed into it (#tag, 📅 date…) are applied, and clearing
// it deletes the reminder.
void Tui::edit_title_in_place(const std::string& id) {
	draw();	 // makes sure title_y_ etc. describe the selected row
	auto ref = store_.find(id);
	if (!ref || title_y_ < 0) {
		return;
	}
	auto text =
		edit_line(title_y_, title_x_, title_width_, ref->reminder->title,
				  focus_items_ ? COLOR_PAIR(kSelected) | A_BOLD : A_BOLD);
	if (!text || *text == ref->reminder->title) {
		return;
	}

	auto trimmed = *text;
	while (!trimmed.empty() && trimmed.back() == ' ') {
		trimmed.pop_back();
	}
	while (!trimmed.empty() && trimmed.front() == ' ') {
		trimmed.erase(0, 1);
	}
	if (trimmed.empty()) {
		move_selection(1);
		if (item_sel_ == id) {
			move_selection(-1);
		}
		undoable("Delete", [&] { store_.remove(id); });
		message_ = "Deleted (u to undo)";
		return;
	}
	undoable("Edit Title", [&] {
		auto& r = *ref->reminder;
		auto f = rem::parse_fields(trimmed);
		r.title = f.title;
		for (auto& t : f.tags) {
			if (std::ranges::find(r.tags, t) == r.tags.end()) {
				r.tags.push_back(t);
			}
		}
		if (f.priority != rem::Priority::None) {
			r.priority = f.priority;
		}
		if (f.flagged) {
			r.flagged = true;
		}
		if (f.repeat) {
			r.repeat = f.repeat;
		}
		if (f.due_date) {
			r.due_date = f.due_date, r.due_time = f.due_time;
		}
		if (f.url) {
			r.url = f.url;
		}
		store_.touch(id);
	});
}

bool Tui::confirm(const std::string& question) {
	auto answer = prompt(question + " [y/N]");
	return answer &&
		   (term::lower(*answer) == "y" || term::lower(*answer) == "yes");
}

// Opens settings.ini in $EDITOR, then applies what changed.
void Tui::edit_settings() {
	auto file = rem::settings_file();
	try {
		std::error_code ec;
		if (!fs::exists(file, ec)) {
			fs::create_directories(file.parent_path());
			std::ofstream(file)
				<< "[general]\n";  // settings are read from this section
		}
	} catch (const std::exception& e) {
		message_ = std::format("Error: {}", e.what());
		return;
	}
	def_prog_mode();
	endwin();
	bool ok = editfile::run_editor_on(file);
	reset_prog_mode();
	refresh();
	load_layout();
	// The view may have just been hidden.
	if (sidebar_.gone(view_)) {
		select_view(home_view());
	} else {
		auto keep = std::pair{item_sel_, item_scroll_};
		select_view(view_);	 // finds it in the sidebar again
		std::tie(item_sel_, item_scroll_) = keep;
	}
	message_ =
		ok ? "Settings reloaded" : "The editor failed; settings reloaded";
}

// Edits the reminder in $EDITOR as YAML-style fields (see editfile.hpp).
void Tui::edit_in_editor(const std::string& id) {
	def_prog_mode();
	endwin();
	auto outcome = editfile::Outcome::Unchanged;
	try {
		outcome =
			editfile::edit(store_, id, [&](const std::function<void()>& apply) {
				auto before = store_.snapshot();
				apply();  // errors go back to the editor, so don't swallow them
						  // here
				history_.record("Edit Reminder", before, store_.snapshot());
			});
	} catch (const std::exception& e) {
		message_ = std::format("Error: {}", e.what());
	}
	reset_prog_mode();
	refresh();
	if (message_.empty()) {
		message_ = outcome == editfile::Outcome::Saved ? "Saved (u to undo)"
				 : outcome == editfile::Outcome::Reverted
					 ? "Reverted; the reminder is as it was"
					 : "No changes";
	}
}

}  // namespace tui
