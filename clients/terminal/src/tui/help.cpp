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

// The keys, one per line, in a box (show_text).
void Tui::show_help() {
	// {key, description}; a heading has no description, a blank line neither.
	struct Entry {
			const char* key;
			const char* text;
	};
	static const Entry entries[] = {
		{"Anywhere", nullptr},
		{"Up / Down, j / k", "move"},
		{"Page Up / Down", "move a page"},
		{"tab", "switch between the sidebar and the reminders"},
		{"Ctrl+L", "redraw the screen"},
		{"1-9, 0", "go to sidebar entry 1-10"},
		{"Ctrl+PgUp / PgDn", "previous / next sidebar entry"},
		{"g, Ctrl+K", "go to a list by name"},
		{":", "type a command (:help lists them, tab completes)"},
		{"/, Ctrl+F", "search (esc, or tab back to the sidebar, ends it)"},
		{"enter", "on a collapsible group's heading: fold / unfold"},
		{"J / K, Ctrl+Up / Down", "in the sidebar: move the entry down / up"},
		{"Ctrl+Shift+Up / Down", "in the sidebar: move its group"},
		{"h", "hide the selected list, smart list or tag"},
		{"H", "show / stop showing hidden ones"},
		{"c, Ctrl+H", "show / hide completed"},
		{"Ctrl+B", "show / hide the sidebar"},
		{"N", "new list"},
		{"O, Ctrl+O", "import a file (.ics, .md, .txt, todo.txt, .csv)"},
		{"s", "sync the selected source now (on a smart list, all)"},
		{"S", "sync every source now"},
		{"Ctrl+, (or just ,)", "edit settings.ini in your $EDITOR"},
		{"u", "undo"},
		{"r", "redo"},
		{"?, F1", "this help"},
		{"q, Ctrl+Q", "quit"},
		{"", ""},
		{"On a reminder", nullptr},
		{"x / space", "done / not done"},
		{"n, Ctrl+N", "new reminder (inline fields work)"},
		{"enter, F2", "edit the title in place (esc cancels)"},
		{"e, Ctrl+E", "edit every field in your $EDITOR"},
		{"d", "due date: today, tomorrow, fri, +3d, 2026-10-31, none"},
		{"t, Ctrl+T", "due today"},
		{"T", "due tomorrow"},
		{"f, Ctrl+D", "flag / unflag"},
		{"0-3", "priority none / low / medium / high"},
		{"#", "add a tag (-tag removes it)"},
		{"m", "move to another list"},
		{"J / K, Ctrl+Up / Down", "move down / up in the list"},
		{"] / [", "indent / outdent"},
		{"+, Shift+Right / Left", "show / hide its subtasks (in a list)"},
		{"Delete", "delete (asks first)"},
		{"", ""},
		{"Several reminders", nullptr},
		{"v", "mark / unmark, and go to the next"},
		{"*", "mark all"},
		{"esc", "unmark all"},
		{"", "while some are marked, x / space, f, t / T, d, 0-3,"},
		{"", "#, m and Delete act on all of them (one undo step)"},
	};
	// One column: the keys, then their descriptions lined up after them.
	int key_w = 0;
	for (auto& e : entries) {
		if (e.text) {
			key_w = std::max(key_w, text_width(widen(e.key)));
		}
	}
	std::vector<std::string> lines;
	for (auto& e : entries) {
		if (!e.text) {
			lines.push_back(e.key);	 // a heading
		} else if (!*e.key && !*e.text) {
			lines.emplace_back();
		} else {
			auto key = std::string("  ") + e.key;
			key.append(
				static_cast<std::size_t>(key_w - text_width(widen(e.key)) + 3),
				' ');
			lines.push_back(key + e.text);
		}
	}
	show_text("Reminders: keys", lines);
}

// `lines` in a box as wide as them (as far as the screen allows), centred,
// from 1 row below the top of the screen to 2 above the bottom. When they're
// taller than the box they scroll: Up / Down, j / k, Page Up / Down, Space;
// any other key closes it.
void Tui::show_text(const std::string& title,
					const std::vector<std::string>& lines) {
	std::vector<const char*> text;
	for (auto& l : lines) {
		text.push_back(l.c_str());
	}
	const int n = static_cast<int>(text.size());
	int text_w = 0;
	for (auto* line : text) {
		text_w = std::max(text_w, text_width(widen(line)));
	}
	// As wide as the text (with a space each side), centred; from 1 row
	// below the top of the screen to 2 rows above the bottom (all of it on a
	// very short terminal).
	int w = std::min(COLS, text_w + 4);
	int above = LINES >= 10 ? 1 : 0, below = LINES >= 10 ? 2 : 0;
	int h = LINES - above - below, y = above, x = (COLS - w) / 2;
	int inner = w - 2;	// between the borders
	int rows = h - 4;  // a blank line under the top border and above the bottom
	int left = std::max(1, (inner - text_w) / 2);  // the text block, centred
	int top = 0, last = std::max(0, n - rows);
	auto* win = newwin(h, w, y, x);
	while (true) {
		// Every row is written in full, so nothing of what was there before
		// (the lists behind, an earlier page) is left in the box.
		for (int i = 1; i < h - 1; ++i) {
			std::wstring row(static_cast<std::size_t>(inner), L' ');
			int at = i - 2 + top;
			if (i >= 2 && i < 2 + rows && at < n) {
				auto line = widen(text[at]);
				while (!line.empty() && left + text_width(line) > inner) {
					line.pop_back();
				}
				row = std::wstring(static_cast<std::size_t>(left), L' ') + line;
				row.append(static_cast<std::size_t>(
							   std::max(0, inner - left - text_width(line))),
						   L' ');
			}
			mvwaddnwstr(win, i, 1, row.c_str(), static_cast<int>(row.size()));
		}
		box_set(win, WACS_VLINE, WACS_HLINE);
		wattron(win, A_BOLD);
		mvwaddnwstr(win, 0, 2, widen(" " + title + " ").c_str(),
					std::max(0, w - 4));
		wattroff(win, A_BOLD);
		auto hint =
			last > 0
				? std::format(
					  " {}-{} of {}  Up / Down scroll, any other key closes ",
					  top + 1, std::min(n, top + rows), n)
				: std::string(" Press any key to close ");
		wattron(win, A_DIM);
		mvwaddstr(win, h - 1, std::max(1, (w - text_width(widen(hint))) / 2),
				  hint.c_str());
		wattroff(win, A_DIM);
		redrawwin(
			win);  // repaint all of it, not just what ncurses thinks changed
		wrefresh(win);
		wint_t ch;
		int kind;
		while ((kind = get_wch(&ch)) == ERR) {
		}
		bool fn = kind == KEY_CODE_YES;
		int step = fn && ch == KEY_UP	 ? -1
				 : fn && ch == KEY_DOWN	 ? 1
				 : fn && ch == KEY_PPAGE ? -rows
				 : fn && ch == KEY_NPAGE ? rows
				 : !fn && ch == 'k'		 ? -1
				 : !fn && ch == 'j'		 ? 1
				 : !fn && ch == ' '		 ? rows
										 : 0;
		if (fn && ch == KEY_RESIZE) {
			break;
		}
		if (step == 0 || last == 0) {
			break;
		}
		top = std::clamp(top + step, 0, last);
	}
	delwin(win);
	// The next draw clears the terminal and repaints all of it, rather than
	// only what ncurses thinks the box changed, which leaves pieces of it
	// behind when the terminal drew some character wider than ncurses
	// expected.
	clearok(stdscr, TRUE);
}

}  // namespace tui
