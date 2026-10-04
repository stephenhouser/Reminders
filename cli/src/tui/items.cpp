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

// The reminders a smart, tag or search view shows (empty for a list).
std::vector<rem::Ref> Tui::view_refs() {
	auto today = rem::local_today();
	std::vector<rem::Ref> refs;
	switch (view_.kind) {
		case View::Today:
			refs = store_.today(today);
			break;
		case View::Scheduled:
			refs = store_.scheduled();
			break;
		case View::All:
			refs = store_.all();
			break;
		case View::AllReminders:
			refs = store_.everything();
			break;
		case View::Flagged:
			refs = store_.flagged();
			break;
		case View::Completed:
			refs = store_.completed();
			break;
		case View::Tag:
			refs = store_.tagged(view_.name);
			break;
		case View::Search:
			refs = store_.search(view_.name);
			break;
		case View::List:
			break;
	}
	return refs;
}

std::pair<std::string, std::string> Tui::view_count() {
	int total = 0, done = 0;
	auto style = rem::CountStyle::OpenOnly;
	if (view_.kind == View::List) {
		if (auto* l = store_.list(view_.name)) {
			l->doc.walk([&](rem::Reminder& r, rem::Reminder*) {
				++total;
				done += r.done;
			});
		}
		style = rem::CountStyle::WithComplete;
	} else {
		auto refs = view_refs();
		total = static_cast<int>(refs.size());
		done = static_cast<int>(std::ranges::count_if(
			refs, [](auto& r) { return r.reminder->done; }));
		style = view_.kind == View::Search	  ? rem::CountStyle::Results
			  : view_.kind == View::Completed ? rem::CountStyle::Completed
			  : view_.kind == View::Tag || view_.kind == View::AllReminders
				  ? rem::CountStyle::WithComplete
				  : rem::CountStyle::OpenOnly;
	}
	return {rem::count_label(style, total, done),
			rem::count_short(style, total, done)};
}

std::vector<Line> Tui::lines() {
	std::vector<Line> out;
	auto today = rem::local_today();
	auto item = [&](const rem::Ref& ref, int depth, bool show_list) {
		auto& r = *ref.reminder;
		auto md = term::markdown_line(r);
		Line line{Line::Item, r.id, md.before, ref.list->color(), depth};
		line.due = md.due;
		line.after = md.after;
		if (show_list) {
			line.where = "(" + store_.label(*ref.list) +
						 (ref.parent ? " > " + ref.parent->title : "") + ")";
		}
		line.done = r.done;
		line.overdue = term::is_overdue(r, today);
		out.push_back(std::move(line));
		for (std::size_t s = 0; !r.notes.empty();) {
			auto nl = r.notes.find('\n', s);
			out.push_back(
				{Line::Note, "", r.notes.substr(s, nl - s), "", depth + 2});
			if (nl == std::string::npos) {
				break;
			}
			s = nl + 1;
		}
	};

	if (view_.kind == View::List) {
		auto* l = store_.list(view_.name);
		if (!l) {
			return out;
		}
		for (auto& section : l->doc.sections()) {
			if (section.name) {
				if (!out.empty()) {
					out.push_back({Line::Note, "", "", "",
								   0});	 // blank line, as in the file
				}
				out.push_back(
					{Line::Heading, "", "## " + *section.name, l->color(), 0});
			}
			for (auto* r : section.reminders) {
				if (r->done && !show_completed_) {
					continue;
				}
				item({l, r, nullptr}, 0, false);
				if (hide_subtasks_) {
					continue;
				}
				for (auto& s : r->subtasks) {
					if (!s.done || show_completed_) {
						item({l, &s, r}, 2, false);
					}
				}
			}
		}
		return out;
	}

	auto refs = view_refs();
	bool by_date = view_.kind == View::Today || view_.kind == View::Scheduled;
	if (by_date) {
		std::ranges::stable_sort(refs, [](auto& a, auto& b) {
			auto key = [](const rem::Ref& r) {
				auto t = r.reminder->due_time.value_or(rem::TimeOfDay{-1, 0});
				return std::pair{*r.reminder->due_date, t.hour * 60 + t.minute};
			};
			return key(a) < key(b);
		});
	}
	std::string group = "\x01";
	for (auto& ref : refs) {
		std::string g, color;
		if (by_date) {
			g = *ref.reminder->due_date < today
				  ? "Overdue"
				  : rem::relative_date(*ref.reminder->due_date, today);
		} else if (view_.kind != View::Flagged) {
			g = store_.label(*ref.list), color = ref.list->color();
		}
		if (g != group) {
			if (!g.empty()) {
				if (!out.empty()) {
					out.push_back({Line::Note, "", "", "", 0});
				}
				out.push_back({Line::Heading, "", "## " + g, color, 0});
			}
			group = g;
		}
		item(ref, 0,
			 by_date || view_.kind == View::Flagged ||
				 view_.kind == View::Search);
	}
	return out;
}

void Tui::draw_items(int x, int width, int height) {
	title_y_ = -1;
	auto ls = lines();
	// Marks only on reminders in view (completed ones hidden, gone elsewhere).
	std::erase_if(marked_, [&](const std::string& id) {
		return std::ranges::none_of(ls, [&](const Line& l) {
			return l.kind == Line::Item && l.id == id;
		});
	});
	// Title
	std::string title;
	if (view_.kind == View::List) {
		auto* l = store_.list(view_.name);
		title = l ? l->name : view_.name;
	} else if (view_.kind == View::Tag) {
		title = "#" + view_.name;
	} else if (view_.kind == View::Search) {
		title = std::format("Search: {}", view_.name);
	} else {
		static constexpr const char* smart_titles[] = {
			"Today",   "Scheduled", "All",
			"Flagged", "Completed", "All Reminders"};
		title = smart_titles[static_cast<int>(view_.kind)];
	}
	attron(A_BOLD);
	if (view_.kind == View::List) {
		if (auto* l = store_.list(view_.name)) {
			attron(list_color(l->color()));
		}
	}
	int room = width - 2;
	int used = put(0, x + 1, "# " + title, room);
	attroff(A_BOLD | A_COLOR);
	// The count, dimmed, against the right edge: "6 Reminders / 3 Complete",
	// or "6/3" if that doesn't fit beside the title, or nothing.
	auto [full, brief] = view_count();
	if (!marked_.empty()) {
		full = brief = std::format("{} marked", marked_.size());
	}
	for (auto& count : {full, brief}) {
		int w = static_cast<int>(count.size());	 // ASCII
		if (used + 2 + w > room) {
			continue;
		}
		attron(dim());
		put(0, x + 1 + room - w, count, w);
		attroff(dim());
		break;
	}

	// Keep a valid selection.
	std::vector<int> items;
	for (int i = 0; i < static_cast<int>(ls.size()); ++i) {
		if (ls[static_cast<std::size_t>(i)].kind == Line::Item) {
			items.push_back(i);
		}
	}

	auto sel = std::ranges::find_if(items, [&](int i) {
		return ls[static_cast<std::size_t>(i)].id == item_sel_;
	});
	if (sel == items.end()) {
		item_sel_ =
			items.empty() ? "" : ls[static_cast<std::size_t>(items.front())].id;
	}
	int sel_line = -1;
	for (int i : items) {
		if (ls[static_cast<std::size_t>(i)].id == item_sel_) {
			sel_line = i;
		}
	}

	int rows = height - 3;
	if (sel_line >= 0) {
		if (sel_line < item_scroll_) {
			item_scroll_ = sel_line;
		}
		if (sel_line >= item_scroll_ + rows) {
			item_scroll_ = sel_line - rows + 1;
		}
	}
	item_scroll_ = std::clamp(item_scroll_, 0,
							  std::max(0, static_cast<int>(ls.size()) - rows));

	if (ls.empty()) {
		attron(dim());
		put(2, x + 2, "Nothing here. Press n to add a reminder.", width - 3);
		attroff(dim());
	}
	for (int row = 0;
		 row < rows && item_scroll_ + row < static_cast<int>(ls.size());
		 ++row) {
		auto& l = ls[static_cast<std::size_t>(item_scroll_ + row)];
		int y = 2 + row;
		switch (l.kind) {
			case Line::Heading:
				attron(A_BOLD | list_color(l.color));
				put(y, x + 1, l.text, width - 2);
				attroff(A_BOLD | A_COLOR);
				break;
			case Line::Note:
				attron(dim());
				put(y, x + 2 + l.depth, l.text, width - 3 - l.depth);
				attroff(dim());
				break;
			case Line::Item:
				{
					bool selected = l.id == item_sel_;
					attr_t base =
						selected
							? (focus_items_ ? COLOR_PAIR(kSelected) | A_BOLD
											: A_BOLD)
							: A_NORMAL;
					attr_t faint = selected ? base : dim();
					if (selected) {
						attron(base);
						mvhline(y, x, ' ', width);
					}
					int col = x + 1 + l.depth, room = width - 2 - l.depth;
					if (selected) {	 // the title starts after "- [ ] "
						title_y_ = y;
						title_x_ = col + 6;
						title_width_ = std::max(1, room - 6);
					}
					auto part = [&](const std::string& text, attr_t a) {
						if (text.empty() || room <= 1) {
							return;
						}
						attrset(a);
						int n = put(y, col, text, room);
						col += n + 1, room -= n + 1;
					};
					part(l.text, l.done ? faint : base);
					part(l.due, l.overdue && !selected && has_colors()
									? COLOR_PAIR(kRed)
								: l.done ? faint
										 : base);
					part(l.after, faint);
					part(l.where, faint);
					if (marked_.contains(l.id)) {  // a * in the margin
						attrset((selected ? base : A_NORMAL) | A_BOLD |
								(has_colors() && !selected ? COLOR_PAIR(kMarked)
														   : 0));
						mvaddstr(y, x, "*");
					}
					attrset(A_NORMAL);
					break;
				}
		}
	}
}

void Tui::draw_status() {
	int h = LINES;
	attron(COLOR_PAIR(kStatus));
	mvhline(h - 1, 0, ' ', COLS);
	auto text = !message_.empty() ? " " + message_
			  : !marked_.empty()
				  ? std::format(
						" {} marked: x done  f flag  t/T/d due  0-3 priority  "
						"# tag  m move  del delete  esc unmark",
						marked_.size())
			  : focus_items_
				  ? std::string(
						" x done  v mark  n new  enter title  e edit  d due  f "
						"flag  del delete  u undo  ? help  q quit")
				  : std::string(
						" ↑↓ choose  enter open  tab switch  g go to  n new  N "
						"new list  ? help  q quit");
	put(h - 1, 0, text, COLS);
	attroff(COLOR_PAIR(kStatus));
}

void Tui::draw() {
	erase();
	int extra = show_key_numbers_ ? 4 : 0;	// room for "(1) "
	int side = hide_sidebar_
				 ? 0
				 : std::min(28 + extra, std::max(18 + extra, COLS / 4));
	if (!hide_sidebar_) {
		draw_sidebar(side, LINES);
	}
	draw_items(side, COLS - side, LINES);
	draw_status();
	refresh();
}

// Opens on the list that last had focus here, in the GNOME app or via the CLI.
void Tui::restore_view() {
	if (!remember_) {
		select_view(home_view());
		return;
	}
	auto saved = term::parse_view_setting(rem::load_setting("view"));
	for (auto [kind, name] : kViewSettings) {
		if (saved.kind == name) {
			view_ = {static_cast<View::Kind>(kind), ""};
		}
	}
	if (saved.kind ==
		"list") {  // "source/name" (or a name only one source has)
		if (auto* l = store_.list(saved.name)) {
			view_ = {View::List, store_.key_of(*l)};
		}
	}
	auto tags = store_.tags();
	if (saved.kind == "tag" && !tags_.hidden() &&
		std::ranges::find(tags, saved.name) != tags.end()) {
		view_ = {View::Tag, saved.name};
	}
	bool smart = view_.kind != View::List && view_.kind != View::Tag &&
				 view_.kind != View::Search;
	if (smart && std::ranges::none_of(smart_entries(), [&](auto& e) {
			return e.view == view_;
		})) {
		view_ = home_view();
	}
	if (!hidden_.show &&
		((view_.kind == View::List && hidden_.list_hidden(view_.name)) ||
		 (view_.kind == View::Tag && hidden_.tag_hidden(view_.name)))) {
		view_ = home_view();  // hidden in the sidebar
	}
	select_view(view_);
}

void Tui::remember_view() {
	if (!remember_ || view_.kind == View::Search) {
		return;
	}
	std::string value = view_.kind == View::List ? "list:" + view_.name
					  : view_.kind == View::Tag
						  ? "tag:" + view_.name
						  : kViewSettings[static_cast<int>(view_.kind)].second;
	try {
		if (rem::load_setting("view") != value) {
			rem::save_setting("view", value);
		}
	} catch (const std::exception&) {
		// Not being able to save the last view isn't worth interrupting for.
	}
}

void Tui::select_view(const View& v) {
	if (!(v == view_)) {
		marked_.clear();
	}
	view_ = v;
	remember_view();
	item_scroll_ = 0;
	item_sel_.clear();
	auto entries = sidebar();
	for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
		if (entries[static_cast<std::size_t>(i)].kind == SidebarEntry::Item &&
			entries[static_cast<std::size_t>(i)].view == v) {
			side_sel_ = i;
		}
	}
}

void Tui::move_selection(int delta) {
	if (!focus_items_) {
		// Steps over plain headings; stops on a collapsible group's heading
		// (Enter folds it) and on lists, which open as you go.
		auto entries = sidebar();
		int n = static_cast<int>(entries.size()), step = delta < 0 ? -1 : 1;
		for (int moved = 0, i = side_sel_; moved < std::abs(delta);) {
			i += step;
			if (i < 0 || i >= n) {
				break;
			}
			auto k = entries[static_cast<std::size_t>(i)].kind;
			if (k == SidebarEntry::Heading) {
				continue;
			}
			side_sel_ = i;
			++moved;
		}
		auto& e =
			entries[static_cast<std::size_t>(std::clamp(side_sel_, 0, n - 1))];
		if (e.kind == SidebarEntry::Item && !(e.view == view_)) {
			view_ = e.view;
			remember_view();
			item_scroll_ = 0;
			item_sel_.clear();
		}
		return;
	}
	auto ls = lines();
	std::vector<std::string> ids;
	for (auto& l : ls) {
		if (l.kind == Line::Item) {
			ids.push_back(l.id);
		}
	}
	if (ids.empty()) {
		return;
	}
	auto at = std::ranges::find(ids, item_sel_);
	long i = at == ids.end() ? 0 : at - ids.begin();
	i = std::clamp<long>(i + delta, 0, static_cast<long>(ids.size()) - 1);
	item_sel_ = ids[static_cast<std::size_t>(i)];
}

}  // namespace tui
