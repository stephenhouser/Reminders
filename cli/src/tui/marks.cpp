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

std::vector<std::string> Tui::shown_items() {
	std::vector<std::string> out;
	for (auto& l : lines()) {
		if (l.kind == Line::Item) {
			out.push_back(l.id);
		}
	}
	return out;
}

std::vector<std::string> Tui::marked_items() {
	return marked_.in_order(shown_items());
}

// Before reminders leave the view: keeps the place by selecting the nearest
// one staying, below the selection if there is one, else above.
void Tui::step_off(const std::vector<std::string>& going) {
	if (std::ranges::find(going, item_sel_) == going.end()) {
		return;
	}
	auto all = shown_items();
	auto at = std::ranges::find(all, item_sel_);
	if (at == all.end()) {
		return;
	}
	auto stays = [&](const std::string& id) {
		return std::ranges::find(going, id) == going.end();
	};
	if (auto next = std::find_if(at, all.end(), stays); next != all.end()) {
		item_sel_ = *next;
	} else if (auto prev = std::find_if(std::make_reverse_iterator(at),
										all.rend(), stays);
			   prev != all.rend()) {
		item_sel_ = *prev;
	}
}

// The editing keys while reminders are marked, on all of them, as one undo
// step. Completing and flagging set them all alike: done (flagged), or not
// if they all were already.
bool Tui::act_on_marked(wint_t key) {
	auto ids = marked_items();
	if (ids.empty()) {
		return false;
	}
	auto today = rem::local_today();
	auto n = ids.size();
	switch (key) {
		case 'x':
		case ' ':
			{
				bool done = !rem::all_done(store_, ids);
				bool hides = done && !show_completed_ &&
							 view_.kind != View::Completed &&
							 view_.kind != View::AllReminders;
				if (hides) {
					step_off(ids);
				}
				batch("Complete", [&] { rem::complete(store_, ids, today); });
				message_ = std::format("{} {}", n, done ? "done" : "not done");
				return true;
			}
		case 'f':
			{
				bool flag = false;
				batch("Flag", [&] { flag = rem::toggle_flag(store_, ids); });
				message_ =
					std::format("{} {}", n, flag ? "flagged" : "unflagged");
				return true;
			}
		case 't':
			batch("Due Today", [&] { rem::set_due(store_, ids, today); });
			return true;
		case 'T':
			batch("Due Tomorrow", [&] {
				rem::set_due(store_, ids,
							 rem::Date{std::chrono::sys_days{today} +
									   std::chrono::days{1}});
			});
			return true;
		case 'd':
			if (auto d = prompt(std::format("Due date for {} (today, tomorrow, "
											"fri, +3d, 2026-10-31, none):",
											n))) {
				if (term::lower(*d) == "none" || d->empty()) {
					batch("Clear Due Date",
						  [&] { rem::set_due(store_, ids, std::nullopt); });
				} else {
					auto words = *d;
					std::optional<rem::TimeOfDay> time;
					if (auto sp = words.rfind(' '); sp != std::string::npos) {
						if ((time = rem::parse_time(words.substr(sp + 1)))) {
							words.resize(sp);
						}
					}
					if (auto date = rem::parse_human_date(words, today)) {
						batch("Set Due Date",
							  [&] { rem::set_due(store_, ids, date, time); });
					} else {
						message_ = std::format("Can't read “{}”", *d);
					}
				}
			}
			return true;
		case '0':
		case '1':
		case '2':
		case '3':
			batch("Priority", [&] {
				rem::set_priority(store_, ids,
								  static_cast<rem::Priority>(key - '0'));
			});
			return true;
		case '#':
			if (auto t = prompt(std::format("Tag for {} (-tag removes):", n));
				t && !t->empty()) {
				auto tag = *t;
				bool remove = tag.starts_with('-');
				if (remove) {
					tag.erase(0, 1);
				}
				if (tag.starts_with('#')) {
					tag.erase(0, 1);
				}
				batch("Tag", [&] { rem::set_tag(store_, ids, tag, !remove); });
			}
			return true;
		case 'm':
			if (auto name = prompt(std::format("Move {} to list:", n));
				name && !name->empty()) {
				rem::ListFile* dest = nullptr;
				for (auto* l : store_.lists()) {
					if (term::lower(store_.label(*l))
							.starts_with(term::lower(*name)) ||
						term::lower(store_.key_of(*l))
							.starts_with(term::lower(*name))) {
						dest = l;
						break;
					}
				}
				if (!dest) {
					message_ = std::format("No list called “{}”", *name);
					return true;
				}
				if (view_.kind == View::List) {
					step_off(ids);
				}
				int moved = 0;
				batch("Move",
					  [&] { moved = rem::move_to_list(store_, ids, *dest); });
				message_ =
					std::format("Moved {} to “{}”", moved, store_.label(*dest));
			}
			return true;
		case kDelete:
			{
				if (!confirm(std::format("Delete {} reminders?", n))) {
					return true;
				}
				step_off(ids);
				batch("Delete", [&] { rem::remove(store_, ids); });
				message_ = std::format("Deleted {} (u to undo)", n);
				return true;
			}
	}
	return false;
}

}  // namespace tui
