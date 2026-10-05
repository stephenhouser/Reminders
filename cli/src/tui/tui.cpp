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

// Shows what went wrong in the last CalDAV and WebDAV syncs, if anything.
void Tui::check_sync() {
	if (!sync_) {
		return;
	}
	auto status = sync_->take_status();
	if (!status.errors.empty()) {
		message_ = "Sync: " + status.errors.back();
		sync_asked_.reset();
	} else if (sync_asked_ && !status.syncing && status.last_sync &&
			   *status.last_sync > *sync_asked_) {
		message_ = "Synced";
		sync_asked_.reset();
	}
}

// Reloads when files in the folder change (Syncthing, the GUI, an editor).
void Tui::check_folder() {
	std::map<std::string, std::pair<fs::file_time_type, std::uintmax_t>> now;
	std::error_code ec;
	for (auto& source : store_.sources()) {
		for (auto& e : fs::directory_iterator(source.config.folder, ec)) {
			auto name = e.path().filename().string();
			if (!name.ends_with(".md") || name.starts_with('.')) {
				continue;
			}
			now[e.path().string()] = {e.last_write_time(ec), e.file_size(ec)};
		}
	}
	if (now == seen_) {
		return;
	}
	bool first = seen_.empty();
	seen_ = std::move(now);
	if (first) {
		return;
	}
	try {
		store_.load_all();	// only lists whose files changed are re-read
	} catch (const std::exception& e) {
		message_ = std::format("Error reloading: {}", e.what());
	}
}

int Tui::run() {
	std::setlocale(LC_ALL, "");
	initscr();
	cbreak();
	// Ctrl+S / Ctrl+Q are XOFF / XON (pause / resume output) in a terminal;
	// turn that off so Ctrl+S reaches the app. ncurses restores it on exit.
	termios tio{};
	if (tcgetattr(STDIN_FILENO, &tio) == 0) {
		tio.c_iflag &= ~static_cast<tcflag_t>(IXON);
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}
	noecho();
	keypad(stdscr, TRUE);
	set_escdelay(25);
	curs_set(0);
	timeout(1000);	// wake up every second to look for changes
	// Modified keys that terminals send as escape sequences ncurses knows by
	// these capability names (kUP3 = Alt+↑, kNXT5 = Ctrl+Page Down, …).
	auto code = [](const char* cap) {
		const char* seq = tigetstr(cap);
		if (!seq || seq == reinterpret_cast<const char*>(-1)) {
			return 0;
		}
		int c = key_defined(seq);
		return c > 0 ? c : 0;
	};
	alt_up_ = code("kUP3");
	alt_down_ = code("kDN3");
	alt_shift_up_ = code("kUP4");
	alt_shift_down_ = code("kDN4");
	ctrl_page_down_ = code("kNXT5");
	ctrl_page_up_ = code("kPRV5");
	setup_colors();
	check_folder();
	sync_ = std::make_unique<rem::SyncRunner>(store_);
	restore_view();
	if (hide_sidebar_) {
		focus_items_ =
			true;  // started with the sidebar hidden (show-sidebar=false)
	}

	try {
		draw();
		while (true) {
			wint_t key;
			int kind = get_wch(&key);
			if (kind == ERR) {
				check_sync();
				check_folder();
				draw();
				continue;
			}
			bool alt = false;
			if (kind == OK &&
				key == 27) {  // Esc: on its own, or Alt+key (sent as Esc, key)
				nodelay(stdscr, TRUE);
				wint_t next;
				int next_kind = get_wch(&next);
				timeout(1000);
				if (next_kind != ERR) {
					key = next;
					kind = next_kind;
					alt = true;
				}
			}
			if (!handle_key(key, kind == KEY_CODE_YES, alt)) {
				break;
			}
			check_folder();
			draw();
		}
	} catch (...) {
		endwin();
		throw;
	}
	endwin();
	return 0;
}

}  // namespace tui

int run_tui(rem::Library& store, bool remember,
			std::optional<bool> key_numbers) {
	tui::Tui tui(store, remember);
	if (key_numbers) {
		tui.set_show_key_numbers(*key_numbers);
	}
	return tui.run();
}
