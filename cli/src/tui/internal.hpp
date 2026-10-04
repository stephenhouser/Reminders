// Internal to the terminal interface's files (cli/src/tui/): its types, the
// Tui class and the text and colour helpers they share.
#pragma once

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

#include "reminders/actions.hpp"
#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
#include "reminders/selection.hpp"
#include "reminders/settings.hpp"
#include "reminders/sidebar.hpp"
#include "reminders/view.hpp"
#include "reminders/view_model.hpp"
#ifdef REMINDERS_NETWORK
#include "reminders/sync_runner.hpp"
#endif

#include "../editfile.hpp"
#include "../text.hpp"
#include "../tui.hpp"

namespace tui {

using namespace std::chrono_literals;

namespace fs = std::filesystem;

enum Pair : short {
	kDim = 1,
	kRed,
	kSelected,
	kHeading,
	kStatus,
	kMarked,
	kFirstListColor
};

using View = rem::View;	 // what the interface shows (reminders/view.hpp)

// Alt+Shift+↑ / ↓ (move the sidebar group), as internal keys outside the
// range of characters.
constexpr wint_t kGroupUp = 0x110010, kGroupDown = 0x110011;

constexpr wint_t kDelete =
	0x110000;  // the Delete key, outside the range of characters

struct SidebarEntry {
		View view;
		std::string title;
		std::string color;
		int count = -1;
		// Headings: a plain Heading is just a label; a FoldHeading (a
		// collapsible group's) can be selected, and Enter/Space folds or
		// unfolds the group.
		enum Kind { Item, Heading, FoldHeading } kind = Item;
		rem::SidebarGroup group =
			rem::SidebarGroup::smart_lists();  // the group the row belongs to
		bool hidden = false;  // hidden in settings.ini, showing because of
							  // show-hidden (dimmed)
};

struct Line {
		enum Kind { Heading, Item, Note } kind;
		std::string id;	 // for items
		std::string text;
		std::string color;
		int depth = 0;
		// Items: the Markdown line in parts, so the date can be coloured.
		std::string due{}, after{}, where{};
		bool done = false, overdue = false;
};

class Tui {
	public:
		Tui(rem::Library& store, bool remember)
			: store_(store), remember_(remember) {}
		int run();
		void set_show_key_numbers(bool on) {
			show_key_numbers_ = on, key_numbers_override_ = on;
		}

	private:
		rem::Library& store_;  // every source
#ifdef REMINDERS_NETWORK
		std::unique_ptr<rem::SyncRunner>
			sync_;	// CalDAV and WebDAV sources, in the background
#endif
		void check_sync();
		bool remember_;
		rem::History history_;
		View view_;
		bool focus_items_ = false;
		bool show_completed_ = false;
		bool hide_subtasks_ = false;  // Ctrl+E
		bool hide_sidebar_ = !rem::load_bool_setting(
			"show-sidebar", true);	// Ctrl+B; shared with the app
		bool show_key_numbers_ = rem::load_bool_setting("show-key-numbers");
		std::optional<bool>
			key_numbers_override_;	// --show-key-numbers / --hide-key-numbers
		rem::Sidebar sidebar_{store_};	// the sidebar's layout (settings.ini)
		// Extended key codes for the GUI's modified keys, 0 if the terminal
		// lacks them.
		int alt_up_ = 0, alt_down_ = 0, alt_shift_up_ = 0, alt_shift_down_ = 0,
			ctrl_page_down_ = 0, ctrl_page_up_ = 0;
		int side_sel_ = 0;
		std::string item_sel_;	// selected reminder id
		// Marked reminders (v, * marks all, Esc clears): while any are marked,
		// the editing keys act on all of them instead of the selected one.
		rem::Selection marked_;
		int item_scroll_ = 0;
		std::string message_;
		std::map<std::string, std::pair<fs::file_time_type, std::uintmax_t>>
			seen_;	// folder signature

		std::vector<SidebarEntry> sidebar();  // every row, headings included
		std::vector<SidebarEntry>
		sidebar_items();  // the selectable lists, in order (numbered)
		SidebarEntry entry_for(
			const View& v);	 // a row for an entry: title, colour, count
		std::vector<SidebarEntry>
		smart_entries();  // the smart lists the settings show
		void toggle_fold(const rem::SidebarGroup& group);
		void move_group(int delta);
		void move_entry(int delta);
		void edit_settings();
		void toggle_hidden();
		void toggle_show_hidden();
		void load_layout();
		View home_view();
		std::vector<Line> lines();
		std::vector<rem::Ref> view_refs();
		// As under the app's title, long and short: {"6 Reminders / 3
		// Complete", "6/3"}.
		std::pair<std::string, std::string> view_count();
		void draw();
		void draw_sidebar(int width, int height);
		void draw_items(int x, int width, int height);
		void draw_status();
		std::optional<std::string> prompt(const std::string& label,
										  const std::string& initial = "");
		std::optional<std::string> edit_line(int y, int x, int width,
											 const std::string& initial,
											 attr_t attr);
		void edit_title_in_place(const std::string& id);
		// Where the selected reminder's title is on screen (set while drawing).
		int title_y_ = -1, title_x_ = 0, title_width_ = 0;
		bool confirm(const std::string& question);
		void show_help();
		// Edits every field of a reminder in the user's editor.
		void edit_in_editor(const std::string& id);
		bool handle_key(wint_t key, bool is_function_key, bool alt = false);
		std::vector<std::string>
		shown_items();	// the reminders in view, top to bottom
		std::vector<std::string> marked_items();  // marked_, in that order
		void step_off(
			const std::vector<std::string>&
				going);	 // selects the nearest reminder not in `going`
		bool act_on_marked(
			wint_t key);  // false: not a key that applies to marks
		void step_sidebar(int delta);
		void move_selection(int delta);
		void select_view(const View& v);
		void restore_view();
		void remember_view();
		void check_folder();

		template <class F>
		void undoable(const char* label, F&& f) {
			auto before = store_.snapshot();
			try {
				f();
			} catch (const std::exception& e) {
				message_ = std::format("Error: {}", e.what());
			}
			history_.record(label, before, store_.snapshot());
		}
		// An edit of several reminders: one undo step, each list written once.
		template <class F>
		void batch(const char* label, F&& f) {
			undoable(label, [&] {
				store_.hold_saves();
				try {
					f();
				} catch (...) {
					try {
						store_.release_saves();
					} catch (...) {
					}
					throw;
				}
				store_.release_saves();
			});
		}
};

std::wstring widen(std::string_view s);
std::string narrow(const std::wstring& w);
int cell_width(wchar_t c);
int text_width(const std::wstring& w);
int put(int y, int x, std::string_view s, int width);
short xterm256(term::Rgb c);
void setup_colors();
attr_t list_color(const std::string& color);
attr_t dim();

}  // namespace tui
