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

// A sidebar row for an entry: its title, colour, count and whether it's
// hidden (showing because of show-hidden).
SidebarEntry Tui::entry_for(const View& v) {
	struct Smart {
			View::Kind kind;
			const char* title;
			const char* color;
	};
	static constexpr Smart kSmart[] = {
		{View::Today, "Today", "blue"},
		{View::Scheduled, "Scheduled", "red"},
		{View::All, "All", "gray"},
		{View::AllReminders, "All Reminders", "gray"},
		{View::Flagged, "Flagged", "orange"},
		{View::Completed, "Completed", "gray"}};
	SidebarEntry e{v, "", "gray",
				   sidebar_.count(v, rem::local_today()).value_or(-1)};
	for (auto& sm : kSmart) {
		if (sm.kind == v.kind) {
			e.title = sm.title, e.color = sm.color;
		}
	}
	if (v.kind == View::List) {
		if (auto* l = store_.list(v.name)) {
			e.title = l->name, e.color = l->color();
		}
	} else if (v.kind == View::Tag) {
		e.title = "#" + v.name, e.color = rem::load_tag_style(v.name).color;
	}
	e.hidden = sidebar_.hidden(v);
	return e;
}

std::vector<SidebarEntry> Tui::smart_entries() {
	std::vector<SidebarEntry> out;
	for (auto& v : sidebar_.smart_views()) {
		out.push_back(entry_for(v));
	}
	return out;
}

void Tui::toggle_fold(const rem::SidebarGroup& group) {
	sidebar_.toggle_fold(group);
}

// Every row in order. The group at the top has no heading unless it can be
// folded; the groups below it have one.
std::vector<SidebarEntry> Tui::sidebar() {
	std::vector<SidebarEntry> out;
	for (auto& g : sidebar_.groups()) {
		bool foldable = sidebar_.foldable(g), folded = sidebar_.folded(g);
		if (foldable || !out.empty()) {
			SidebarEntry heading{
				{}, sidebar_.title(g) + (folded ? " (folded)" : ""), "", -1};
			heading.kind =
				foldable ? SidebarEntry::FoldHeading : SidebarEntry::Heading;
			heading.group = g;
			out.push_back(heading);
			if (folded) {
				continue;
			}
		}
		for (auto& v : sidebar_.entries(g)) {
			auto e = entry_for(v);
			e.group = g;
			out.push_back(e);
		}
	}
	return out;
}

std::vector<SidebarEntry> Tui::sidebar_items() {
	std::vector<SidebarEntry> out;
	for (auto& e : sidebar()) {
		if (e.kind == SidebarEntry::Item) {
			out.push_back(e);
		}
	}
	return out;
}

View Tui::home_view() { return sidebar_.home(); }

void Tui::draw_sidebar(int width, int height) {
	auto entries = sidebar();
	side_sel_ = std::clamp(side_sel_, 0, static_cast<int>(entries.size()) - 1);
	attron(A_BOLD);
	put(0, 1, "Reminders", width - 2);
	attroff(A_BOLD);
	int y = 2;				 // a blank line under the title
	std::size_t number = 0;	 // key numbers follow the lists that are showing
	for (int i = 0; i < static_cast<int>(entries.size()) && y < height - 1;
		 ++i, ++y) {
		auto& e = entries[static_cast<std::size_t>(i)];
		bool selected = i == side_sel_ && !focus_items_;
		if (e.kind != SidebarEntry::Item) {
			if (i > 0) {
				++y;  // a blank line above each group
			}
			if (y >= height - 1) {
				break;
			}
			if (selected) {
				attron(COLOR_PAIR(kSelected) | A_BOLD);
			} else {
				attron(dim());
			}
			mvhline(y, 0, ' ', width - 1);
			put(y, 1, e.title, width - 2);
			attroff(COLOR_PAIR(kSelected) | A_BOLD);
			attroff(dim());
			continue;
		}
		bool current = e.view == view_;
		if (selected) {
			attron(COLOR_PAIR(kSelected) | A_BOLD);
		} else if (current) {
			attron(A_BOLD);
		}
		mvhline(y, 0, ' ', width - 1);
		// Lists in their colour, tags too once given one (Tag Info… in the
		// app).
		bool colored = e.view.kind == View::List ||
					   (e.view.kind == View::Tag && e.color != "gray");
		if (e.hidden && !selected) {
			attron(dim());
		}
		if (colored) {
			attron(list_color(e.color));
		}
		put(y, 1, rem::with_key_number(e.title, number++, show_key_numbers_),
			width - 8);
		if (colored) {
			attroff(list_color(e.color));
		}
		if (e.hidden && !selected) {
			attroff(dim());
		}
		if (e.hidden) {
			mvaddstr(y, 0, "-");  // hidden, showing because of show-hidden (H)
		}
		if (e.count >= 0) {
			auto count = std::to_string(e.count);
			attron(dim());
			put(y, width - 2 - static_cast<int>(count.size()), count,
				static_cast<int>(count.size()));
			attroff(dim());
		}
		attroff(COLOR_PAIR(kSelected) | A_BOLD);
	}
	mvvline_set(0, width - 1, WACS_VLINE, height - 1);
}

// Moves the selected sidebar entry's group up (delta < 0) or down, and saves
// the order. The selection stays on the same entry.
void Tui::move_group(int delta) {
	auto entries = sidebar();
	if (side_sel_ < 0 || side_sel_ >= static_cast<int>(entries.size())) {
		return;
	}
	auto selected = entries[static_cast<std::size_t>(side_sel_)];
	try {
		if (!sidebar_.move_group(selected.group, delta)) {
			return;
		}
	} catch (const std::exception& e) {
		message_ = std::format("Couldn't save the order: {}", e.what());
	}
	entries = sidebar();
	for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
		auto& e = entries[static_cast<std::size_t>(i)];
		bool same =
			selected.kind == SidebarEntry::Item
				? e.kind == SidebarEntry::Item && e.view == selected.view
				: e.kind != SidebarEntry::Item && e.group == selected.group;
		if (same) {
			side_sel_ = i;
		}
	}
}

// Moves the selected smart list, list or tag up (delta < 0) or down within
// its group, skipping entries not showing, and saves the order (smart-lists,
// lists-order, tags-order). The selection stays on it.
void Tui::move_entry(int delta) {
	auto entries = sidebar();
	if (side_sel_ < 0 || side_sel_ >= static_cast<int>(entries.size())) {
		return;
	}
	auto selected = entries[static_cast<std::size_t>(side_sel_)];
	if (selected.kind != SidebarEntry::Item) {
		return;
	}
	try {
		if (!sidebar_.move(selected.view, delta)) {
			return;
		}
	} catch (const std::exception& e) {
		message_ = std::format("Couldn't save the order: {}", e.what());
		return;
	}
	load_layout();
	entries = sidebar();
	for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
		if (entries[static_cast<std::size_t>(i)].kind == SidebarEntry::Item &&
			entries[static_cast<std::size_t>(i)].view == selected.view) {
			side_sel_ = i;
		}
	}
}

void Tui::load_layout() {
	show_key_numbers_ = key_numbers_override_.value_or(
		rem::load_bool_setting("show-key-numbers"));
	sidebar_.reload();
}

// h: hides the selected sidebar entry (the open view, from the reminders
// pane), or shows it again if it's hidden (visible with show-hidden). Saved
// in settings.ini, as the app's Hide / Show does.
void Tui::toggle_hidden() {
	auto target = view_;
	std::string title;	// as the sidebar shows it
	for (auto& e : sidebar()) {
		if (e.kind == SidebarEntry::Item && e.view == target) {
			title = e.title;
		}
	}
	if (!focus_items_) {
		auto entries = sidebar();
		if (side_sel_ < 0 || side_sel_ >= static_cast<int>(entries.size())) {
			return;
		}
		auto& e = entries[static_cast<std::size_t>(side_sel_)];
		if (e.kind != SidebarEntry::Item) {
			return;
		}
		target = e.view;
		title = e.title;
	}
	if (target.kind == View::Search) {
		return;
	}
	bool hidden = sidebar_.hidden(target);
	try {
		sidebar_.set_hidden(target, !hidden);
	} catch (const std::exception& e) {
		message_ = std::format("Couldn't save the setting: {}", e.what());
		return;
	}
	load_layout();
	if (title.empty()) {
		title = entry_for(target).title;
	}
	if (!hidden && !sidebar_.show_hidden()) {
		message_ = std::format(
			"Hid “{}” (H shows hidden entries; h on it shows it again)", title);
		if (view_ == target) {
			select_view(home_view());
		} else {
			select_view(view_);	 // the selection follows the rows that remain
		}
	} else {
		message_ = std::format("{} “{}”", hidden ? "Showing" : "Hid", title);
	}
}

void Tui::toggle_show_hidden() {
	try {
		sidebar_.set_show_hidden(!sidebar_.show_hidden());
	} catch (const std::exception& e) {
		message_ = std::format("Couldn't save the setting: {}", e.what());
		return;
	}
	load_layout();
	message_ = sidebar_.show_hidden() ? "Showing hidden lists"
									  : "Not showing hidden lists";
	if (sidebar_.gone(view_)) {
		select_view(home_view());
	} else {
		auto keep = std::pair{item_sel_, item_scroll_};
		select_view(view_);	 // finds it in the sidebar again
		std::tie(item_sel_, item_scroll_) = keep;
	}
}

void Tui::step_sidebar(int delta) {
	auto entries = sidebar_items();
	if (entries.empty()) {
		return;
	}
	auto at =
		std::ranges::find_if(entries, [&](auto& e) { return e.view == view_; });
	long n = static_cast<long>(entries.size());
	long i = at == entries.end() ? 0 : ((at - entries.begin()) + delta + n) % n;
	select_view(entries[static_cast<std::size_t>(i)].view);
}

}  // namespace tui
