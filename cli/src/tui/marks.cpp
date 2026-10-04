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
	auto out = shown_items();
	std::erase_if(out, [this](auto& id) { return !marked_.contains(id); });
	return out;
}

// For moving and deleting: a subtask goes along with its parent.
std::vector<std::string> Tui::outermost(const std::vector<std::string>& ids) {
	std::vector<std::string> out;
	for (auto& id : ids) {
		auto ref = store_.find(id);
		if (ref && ref->parent &&
			std::ranges::find(ids, ref->parent->id) != ids.end()) {
			continue;
		}
		out.push_back(id);
	}
	return out;
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
	auto each = [&](const char* label, auto&& change) {
		batch(label, [&] {
			for (auto& id : ids) {
				if (auto ref = store_.find(id)) {
					change(*ref->reminder);
					store_.touch(id);
				}
			}
		});
	};
	auto all = [&](auto&& test) {
		return std::ranges::all_of(ids, [&](auto& id) {
			auto ref = store_.find(id);
			return !ref || test(*ref->reminder);
		});
	};
	switch (key) {
		case 'x':
		case ' ':
			{
				bool done = !all([](auto& r) { return r.done; });
				bool hides = done && !show_completed_ &&
							 view_.kind != View::Completed &&
							 view_.kind != View::AllReminders;
				if (hides) {
					step_off(ids);
				}
				batch("Complete", [&] {
					for (auto& id : ids) {
						store_.set_done(id, done, today);
					}
				});
				message_ = std::format("{} {}", n, done ? "done" : "not done");
				return true;
			}
		case 'f':
			{
				bool flag = !all([](auto& r) { return r.flagged; });
				each("Flag", [&](rem::Reminder& r) { r.flagged = flag; });
				message_ =
					std::format("{} {}", n, flag ? "flagged" : "unflagged");
				return true;
			}
		case 't':
			each("Due Today", [&](rem::Reminder& r) { r.due_date = today; });
			return true;
		case 'T':
			{
				auto tomorrow = rem::Date{std::chrono::sys_days{today} +
										  std::chrono::days{1}};
				each("Due Tomorrow",
					 [&](rem::Reminder& r) { r.due_date = tomorrow; });
				return true;
			}
		case 'd':
			if (auto d = prompt(std::format("Due date for {} (today, tomorrow, "
											"fri, +3d, 2026-10-31, none):",
											n))) {
				if (term::lower(*d) == "none" || d->empty()) {
					each("Clear Due Date", [](rem::Reminder& r) {
						r.due_date.reset(), r.due_time.reset();
					});
				} else {
					auto words = *d;
					std::optional<rem::TimeOfDay> time;
					if (auto sp = words.rfind(' '); sp != std::string::npos) {
						if ((time = rem::parse_time(words.substr(sp + 1)))) {
							words.resize(sp);
						}
					}
					if (auto date = rem::parse_human_date(words, today)) {
						each("Set Due Date", [&](rem::Reminder& r) {
							r.due_date = date;
							if (time) {
								r.due_time = time;
							}
						});
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
			{
				auto p = static_cast<rem::Priority>(key - '0');
				each("Priority", [&](rem::Reminder& r) { r.priority = p; });
				return true;
			}
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
				each("Tag", [&](rem::Reminder& r) {
					if (remove) {
						std::erase(r.tags, tag);
					} else if (std::ranges::find(r.tags, tag) == r.tags.end()) {
						r.tags.push_back(tag);
					}
				});
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
				auto moving = outermost(ids);
				if (view_.kind == View::List) {
					step_off(ids);
				}
				batch("Move", [&] {
					for (auto& id : moving) {
						store_.move_to_list(id, *dest);
					}
				});
				message_ = std::format("Moved {} to “{}”", moving.size(),
									   store_.label(*dest));
			}
			return true;
		case kDelete:
			{
				auto gone = outermost(ids);
				if (!confirm(std::format("Delete {} reminders?", n))) {
					return true;
				}
				step_off(ids);
				batch("Delete", [&] {
					for (auto& id : gone) {
						store_.remove(id);
					}
				});
				message_ = std::format("Deleted {} (u to undo)", n);
				return true;
			}
	}
	return false;
}

}  // namespace tui
