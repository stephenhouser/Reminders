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

#include "internal.hpp"
#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
#include "reminders/settings.hpp"
#ifdef REMINDERS_NETWORK
#include "reminders/sync_runner.hpp"
#endif
#include "../editfile.hpp"
#include "../text.hpp"

namespace tui {

std::vector<SidebarEntry> Tui::smart_entries() {
	auto today = rem::local_today();
	std::vector<SidebarEntry> out;
	if (smart_.display == rem::GroupDisplay::Hidden) {
		return out;
	}
	auto names = smart_.shown;
	if (hidden_.show) {	 // the hidden ones after them
		for (auto name : {"today", "scheduled", "all", "all-reminders",
						  "flagged", "completed"}) {
			if (std::ranges::find(names, name) == names.end()) {
				names.push_back(name);
			}
		}
	}
	for (auto& name : names) {
		if (name == "today") {
			out.push_back({{View::Today, ""},
						   "Today",
						   "blue",
						   static_cast<int>(store_.today(today).size())});
		}
		if (name == "scheduled") {
			out.push_back({{View::Scheduled, ""},
						   "Scheduled",
						   "red",
						   static_cast<int>(store_.scheduled().size())});
		}
		if (name == "all") {
			out.push_back({{View::All, ""},
						   "All",
						   "gray",
						   static_cast<int>(store_.all().size())});
		}
		if (name == "all-reminders") {	// completed too
			out.push_back({{View::AllReminders, ""},
						   "All Reminders",
						   "gray",
						   static_cast<int>(store_.everything().size())});
		}
		if (name == "flagged") {
			out.push_back({{View::Flagged, ""},
						   "Flagged",
						   "orange",
						   static_cast<int>(store_.flagged().size())});
		}
		if (name == "completed") {
			out.push_back({{View::Completed, ""},
						   "Completed",
						   "gray",
						   static_cast<int>(store_.completed().size())});
		}
		out.back().hidden =
			std::ranges::find(smart_.shown, name) == smart_.shown.end();
	}
	return out;
}

// The groups with something to show, in order.
std::vector<rem::SidebarGroup> Tui::showing_groups() {
	std::vector<rem::SidebarGroup> out;
	for (auto g : order_) {
		if (g.kind == rem::SidebarGroup::SmartLists &&
			smart_entries().empty()) {
			continue;
		}
		if (g.kind == rem::SidebarGroup::Tags &&
			(tags_.hidden() ||
			 std::ranges::none_of(store_.tags(), [&](auto& t) {
				 return hidden_.show || !hidden_.tag_hidden(t);
			 }))) {
			continue;
		}
		out.push_back(g);
	}
	return out;
}

std::vector<std::string> Tui::source_names() {
	std::vector<std::string> out;
	for (auto& s : store_.sources()) {
		out.push_back(s.config.name);
	}
	return out;
}

std::string Tui::group_title(const rem::SidebarGroup& group) {
	if (group.kind != rem::SidebarGroup::Lists ||
		store_.sources().size() <= 1) {
		return rem::group_title(group);
	}
	for (auto& s : store_.sources()) {
		if (s.config.name == group.source) {
			return rem::group_title(group, rem::source_title(s.config));
		}
	}
	return rem::group_title(group);
}

std::vector<std::string> Tui::list_keys() {
	std::vector<std::string> out;
	for (auto* l : store_.lists()) {
		out.push_back(store_.key_of(*l));
	}
	return out;
}

rem::GroupLayout* Tui::layout_of(const rem::SidebarGroup& group) {
	if (group.kind == rem::SidebarGroup::Lists) {
		auto at = lists_layouts_.find(group.source);
		if (at == lists_layouts_.end()) {
			at =
				lists_layouts_
					.emplace(group.source, rem::load_lists_layout(group.source))
					.first;
		}
		return &at->second;
	}
	if (group.kind == rem::SidebarGroup::Tags) {
		return &tags_;
	}
	return nullptr;
}

bool Tui::folded(const rem::SidebarGroup& group) {
	auto* l = layout_of(group);
	return l ? l->folded() : smart_.folded();
}

void Tui::toggle_fold(const rem::SidebarGroup& group) {
	auto* l = layout_of(group);
	bool& collapsed = l ? l->collapsed : smart_.collapsed;
	collapsed = !collapsed;
	try {
		rem::save_group_collapsed(group, collapsed);
	} catch (const std::exception&) {
		// Folding still works; it just won't be remembered.
	}
}

// Every row in order. The group at the top has no heading unless it can be
// folded; the groups below it have one.
std::vector<SidebarEntry> Tui::sidebar() {
	std::vector<SidebarEntry> out;
	for (auto g : showing_groups()) {
		auto* l = layout_of(g);
		bool foldable = l ? l->foldable() : smart_.foldable();
		if (foldable || !out.empty()) {
			std::string title = group_title(g);
			if (folded(g)) {
				title += " (folded)";
			}
			SidebarEntry heading{{}, title, "", -1};
			heading.kind =
				foldable ? SidebarEntry::FoldHeading : SidebarEntry::Heading;
			heading.group = g;
			out.push_back(heading);
			if (folded(g)) {
				continue;
			}
		}
		std::vector<SidebarEntry> rows;
		switch (g.kind) {
			case rem::SidebarGroup::SmartLists:
				rows = smart_entries();
				break;
			case rem::SidebarGroup::Lists:
				{  // one source's lists, in lists-order
					std::vector<std::string> keys;
					for (auto* l : store_.lists(g.source)) {
						keys.push_back(store_.key_of(*l));
					}
					for (auto& key : rem::order_lists(keys)) {
						auto* list = store_.list(key);
						if (!list) {
							continue;
						}
						bool hidden = hidden_.list_hidden(key);
						if (hidden && !hidden_.show) {
							continue;
						}
						int open = 0;
						list->doc.walk([&](rem::Reminder& r, rem::Reminder*) {
							open += !r.done;
						});
						rows.push_back({{View::List, key},
										list->name,
										list->color(),
										open});
						rows.back().hidden = hidden;
					}
					break;
				}
			case rem::SidebarGroup::Tags:
				for (auto& t : rem::order_tags(store_.tags())) {
					bool hidden = hidden_.tag_hidden(t);
					if (hidden && !hidden_.show) {
						continue;
					}
					rows.push_back({{View::Tag, t},
									"#" + t,
									rem::load_tag_style(t).color,
									-1});
					rows.back().hidden = hidden;
				}
				break;
		}
		for (auto& r : rows) {
			r.group = g;
			out.push_back(r);
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

// Where to land when there's nothing better: Today, unless it's hidden.
View Tui::home_view() {
	auto smart = smart_entries();
	if (std::ranges::any_of(
			smart, [](auto& e) { return e.view.kind == View::Today; })) {
		return {View::Today, ""};
	}
	auto items = sidebar_items();
	if (!items.empty()) {
		return items.front().view;
	}
	return smart.empty() ? View{View::Today, ""} : smart.front().view;
}

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
	if (!rem::move_sidebar_group(order_, selected.group, delta,
								 showing_groups())) {
		return;
	}
	try {
		rem::save_sidebar_order(order_);
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
	std::vector<std::string>
		showing;  // the names of the group's rows, as drawn
	auto name_of = [](const SidebarEntry& e) {
		return e.view.kind <= View::AllReminders
				 ? std::string(
					   kViewSettings[static_cast<int>(e.view.kind)].second)
				 : e.view.name;
	};
	for (auto& e : entries) {
		if (e.kind == SidebarEntry::Item && e.group == selected.group) {
			showing.push_back(name_of(e));
		}
	}
	auto name = name_of(selected);
	try {
		switch (selected.group.kind) {
			case rem::SidebarGroup::SmartLists:
				{
					auto order =
						smart_
							.shown;	 // a hidden smart list has no place to move
					if (!rem::move_in_order(order, name, delta, order)) {
						return;
					}
					rem::save_smart_lists(order);
					break;
				}
			case rem::SidebarGroup::Lists:
				{  // every source's lists keep their places
					auto order = rem::order_lists(list_keys());
					if (!rem::move_in_order(order, name, delta, showing)) {
						return;
					}
					rem::save_names_setting("lists-order", order);
					break;
				}
			case rem::SidebarGroup::Tags:
				{
					auto order = rem::order_tags(store_.tags());
					if (!rem::move_in_order(order, name, delta, showing)) {
						return;
					}
					rem::save_names_setting("tags-order", order);
					break;
				}
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
	order_ = rem::load_sidebar_order(source_names());
	smart_ = rem::load_smart_lists_layout();
	lists_layouts_.clear();
	tags_ = rem::load_tags_layout();
	hidden_ = rem::load_hidden();
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
	bool smart = target.kind <= View::AllReminders;
	std::string name = smart
						 ? kViewSettings[static_cast<int>(target.kind)].second
						 : target.name;
	bool hidden =
		smart ? std::ranges::find(smart_.shown, name) == smart_.shown.end()
		: target.kind == View::List ? hidden_.list_hidden(name)
		: target.kind == View::Tag	? hidden_.tag_hidden(name)
									: false;
	if (target.kind == View::Search) {
		return;
	}
	try {
		if (smart) {
			rem::set_smart_list_hidden(name, !hidden);
		} else if (target.kind == View::List) {
			rem::set_list_hidden(name, !hidden);
		} else {
			rem::set_tag_hidden(name, !hidden);
		}
	} catch (const std::exception& e) {
		message_ = std::format("Couldn't save the setting: {}", e.what());
		return;
	}
	load_layout();
	if (title.empty()) {
		title = target.kind == View::Tag ? "#" + name : name;
	}
	if (!hidden && !hidden_.show) {
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

// H: shows the hidden smart lists, lists and tags (dimmed), or stops showing
// them, as the app's Show Hidden Lists does (show-hidden in settings.ini).
void Tui::toggle_show_hidden() {
	try {
		rem::save_show_hidden(!hidden_.show);
	} catch (const std::exception& e) {
		message_ = std::format("Couldn't save the setting: {}", e.what());
		return;
	}
	load_layout();
	message_ =
		hidden_.show ? "Showing hidden lists" : "Not showing hidden lists";
	bool smart = view_.kind <= View::AllReminders;
	bool gone = (smart && std::ranges::none_of(
							  smart_entries(),
							  [&](auto& e) { return e.view == view_; })) ||
				(view_.kind == View::List && hidden_.list_hidden(view_.name)) ||
				(view_.kind == View::Tag && hidden_.tag_hidden(view_.name));
	if (gone && !hidden_.show) {
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
