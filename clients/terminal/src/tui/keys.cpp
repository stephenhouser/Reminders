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
#include "reminders/backend_module.hpp"
#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
#include "reminders/import_file.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/sync_runner.hpp"

namespace tui {

std::string Tui::selected_source() {
	auto entries = sidebar();
	if (!focus_items_ && side_sel_ >= 0 &&
		side_sel_ < static_cast<int>(entries.size()) &&
		entries[static_cast<std::size_t>(side_sel_)].group.kind ==
			rem::SidebarGroup::Lists) {
		return entries[static_cast<std::size_t>(side_sel_)].group.source;
	}
	if (auto* l =
			view_.kind == View::List ? store_.list(view_.name) : nullptr) {
		return store_.source_of(*l)->config.name;
	}
	return store_.default_source();
}

// The file's path, then the list: the list showing, else one named after the
// calendar (or the file). A name finds a list in the selected source first,
// then in any (or "source/name"); one that isn't there is made in the
// selected source.
void Tui::import_file() {
	auto file = prompt("Import file:");
	if (!file || file->empty()) {
		return;
	}
	auto path = file->starts_with('~') || file->find('$') != std::string::npos
				  ? rem::expand_path(*file)
				  : fs::absolute(*file).lexically_normal();
	rem::Import imp;
	try {
		imp = rem::read_import_file(path);
	} catch (const std::exception& e) {
		message_ = std::format("Can't import: {}", e.what());
		return;
	}
	auto source = selected_source();
	auto* showing =
		view_.kind == View::List ? store_.list(view_.name) : nullptr;
	auto name =
		prompt(std::format(
				   "Import {} into list:",
				   rem::reminder_count(imp) == 1
					   ? std::string("1 reminder")
					   : std::format("{} reminders", rem::reminder_count(imp))),
			   showing ? showing->name : rem::import_list_name(imp, path));
	if (!name || name->empty()) {
		return;
	}
	auto found = rem::lists_called(store_, *name, source);
	if (found.empty()) {
		found = rem::lists_called(store_, *name);
	}
	if (found.size() > 1) {
		message_ = std::format(
			"Several lists are called “{}”: type source/name", *name);
		return;
	}
	auto* list = found.empty() ? nullptr : found.front();
	if (!list) {
		if (auto err = rem::list_name_error(*name); !err.empty()) {
			message_ = err;
			return;
		}
	}
	rem::ImportDone done;
	undoable("Import", [&] {
		done = rem::import_to(store_, list, source, *name, imp);
		message_ = rem::import_summary(store_, done, imp);
	});
	if (done.created) {
		select_view({View::List, done.key});
		message_ = rem::import_summary(store_, done, imp);
	}
}

bool Tui::handle_key(wint_t key, bool fn, bool alt) {
	message_.clear();
	auto today = rem::local_today();
	auto id = item_sel_;
	auto ref = id.empty() ? std::nullopt : store_.find(id);
	bool in_list = view_.kind == View::List;

	// The GNOME app's shortcuts, as far as a terminal can send them; they map
	// onto the TUI's own keys below. (Ctrl+Shift+letter arrives as
	// Ctrl+letter, Ctrl+I as Tab and Ctrl+[ as Esc, so those keep their
	// letter keys; Ctrl+, arrives as a comma. Ctrl+0…3 can't all be told
	// apart, so priority is 0…3 alone.) Like the app, no Alt keys.
	if (alt) {
		return true;
	} else if (fn) {
		if (ctrl_up_ && static_cast<int>(key) == ctrl_up_) {
			key = 'K', fn = false;
		} else if (ctrl_down_ && static_cast<int>(key) == ctrl_down_) {
			key = 'J', fn = false;
		} else if (ctrl_shift_up_ && static_cast<int>(key) == ctrl_shift_up_) {
			key = kGroupUp, fn = false;
		} else if (ctrl_shift_down_ &&
				   static_cast<int>(key) == ctrl_shift_down_) {
			key = kGroupDown, fn = false;
		} else if (ctrl_page_down_ &&
				   static_cast<int>(key) == ctrl_page_down_) {
			return step_sidebar(1), true;
		} else if (ctrl_page_up_ && static_cast<int>(key) == ctrl_page_up_) {
			return step_sidebar(-1), true;
		} else if (key == KEY_SRIGHT) {
			key = kShowSubtasks, fn = false;
		} else if (key == KEY_SLEFT) {
			key = kHideSubtasks, fn = false;
		} else if (key == KEY_F(1)) {
			key = '?', fn = false;
		} else if (key == KEY_F(2)) {
			key = '\n', fn = false;
		}
	} else {
		switch (key) {
			case 14:
				key = 'n';
				break;	// Ctrl+N: new reminder
			case 20:
				key = 't';
				break;	// Ctrl+T: due today
			case 11:
				key = 'g';
				break;	// Ctrl+K: go to
			case 8:
				key = 'c';
				break;	// Ctrl+H: show/hide completed
			case 6:
				key = '/';
				break;	// Ctrl+F: search
			case 17:
			case 23:
				key = 'q';
				break;	// Ctrl+Q, Ctrl+W: quit
			case 5:
				key = 'e';
				break;	// Ctrl+E: edit (the app's Details)
			case 4:
				key = 'f';
				break;	// Ctrl+D: flag
			case 2:		// Ctrl+B: show/hide sidebar
				hide_sidebar_ = !hide_sidebar_;
				if (hide_sidebar_) {
					focus_items_ = true;
				}
				try {
					rem::save_setting("show-sidebar",
									  hide_sidebar_ ? "false" : "true");
				} catch (const std::exception&) {
					// It just won't be remembered.
				}
				return true;
			case 15:
				key = 'O';
				break;	// Ctrl+O: import
			case 12:	// Ctrl+L: redraw the whole screen, as terminal apps do
				clearok(curscr, TRUE);
				return true;
		}
	}

	// Tab switches between the sidebar and the reminders. (A terminal sends
	// Ctrl+I as Tab.)
	if (!fn && key == '\t') {
		focus_items_ = !focus_items_;
		return true;
	}

	// Esc on its own unmarks everything.
	if (!fn && key == 27) {
		if (!marked_.empty()) {
			marked_.clear();
			message_ = "Unmarked";
		}
		return true;
	}

	// Keys that work anywhere.
	if (!fn) {
		switch (key) {
			case 'q':
				return false;
			case '?':
				show_help();
				return true;
			case 'h':
				toggle_hidden();
				return true;
			case 'j':
				move_selection(1);
				return true;
			case 'k':
				move_selection(-1);
				return true;
			case 'c':
				show_completed_ = !show_completed_;
				return true;
			case 's':
				{  // sync the selected source now (Ctrl+S in the app): the
				   // selected sidebar entry's, else the list showing's; a
				   // smart list, tag or search syncs every source
					std::optional<std::string> source;
					auto entries = sidebar();
					if (!focus_items_ && side_sel_ >= 0 &&
						side_sel_ < static_cast<int>(entries.size())) {
						auto& g =
							entries[static_cast<std::size_t>(side_sel_)].group;
						if (g.kind == rem::SidebarGroup::Lists) {
							source = g.source;
						}
					} else if (auto* l = view_.kind == View::List
										   ? store_.list(view_.name)
										   : nullptr) {
						source = store_.source_of(*l)->config.name;
					}
					if (!source) {
						return handle_key('S', false);
					}
					auto name = *source;
					const rem::SourceConfig* config = nullptr;
					for (auto& src : store_.sources()) {
						if (src.config.name == name) {
							config = &src.config;
						}
					}
					if (!config) {
						return true;
					}
					auto title = rem::source_title(*config);
					if (!sync_ || !rem::syncs(*config)) {
						message_ =
							std::format("“{}” isn't synced by the app", title);
						return true;
					}
					sync_->sync_now(name);
					sync_asked_ = std::chrono::system_clock::now();
					message_ = std::format("Syncing “{}”…", title);
					return true;
				}
			case 'S':  // sync every source now (Sync All in the app)
				if (!sync_ || !sync_->active()) {
					message_ = "No sources the app syncs";
					return true;
				}
				sync_->sync_now();
				sync_asked_ = std::chrono::system_clock::now();
				message_ = "Syncing all sources…";
				return true;
			case 'H':
				toggle_show_hidden();
				return true;
			case 'u':
				{
					auto r = history_.undo(store_);
					message_ = r.applied ? "Undone" : "Nothing to undo";
					for (auto& s : r.skipped) {
						message_ = std::format(
							"“{}” changed elsewhere; left as it is", s);
					}
					return true;
				}
			case 'r':
				{  // redo
					auto r = history_.redo(store_);
					message_ = r.applied ? "Redone" : "Nothing to redo";
					return true;
				}
			case '/':
				if (auto q = prompt("Search:"); q && !q->empty()) {
					marked_.clear();
					view_ = {View::Search, *q};
					item_sel_.clear();
					focus_items_ = true;
				}
				return true;
			case 'g':
				if (auto q = prompt("Go to:"); q && !q->empty()) {
					// Best match: a name starting with it, else containing it.
					std::optional<View> best;
					int best_score = 3;
					std::vector<SidebarEntry>
						findable;  // folded groups' entries too
					for (auto& v : sidebar_.all(true)) {
						findable.push_back(entry_for(v));
					}
					for (auto& e : findable) {
						auto t = term::lower(e.title), s = term::lower(*q);
						if (t.starts_with('#') && !s.starts_with('#')) {
							t.erase(0, 1);
						}
						int score = t.starts_with(s)			   ? 0
								  : t.find(s) != std::string::npos ? 1
																   : 3;
						if (score < best_score) {
							best_score = score, best = e.view;
						}
					}
					if (best) {
						select_view(*best);
					} else {
						message_ = std::format("Nothing called “{}”", *q);
					}
				}
				return true;
			case 'O':
				import_file();
				return true;
			case ',':  // and Ctrl+, (sent as a comma), as in the app
				edit_settings();
				return true;
			case 'N':
				// Into the source whose group is selected, else the default
				// one.
				if (auto name = prompt("New list name:");
					name && !name->empty()) {
					auto source = selected_source();
					if (auto err = rem::list_name_error(*name); !err.empty()) {
						message_ = err;
					} else if (store_.list(rem::Library::key(source, *name))) {
						message_ = "A list with that name already exists";
					} else {
						undoable("New List", [&] {
							store_.create_list(source, *name, "blue", "list");
						});
						select_view(
							{View::List, rem::Library::key(source, *name)});
					}
				}
				return true;
		}
	}
	if (!fn && key >= '0' && key <= '9' &&
		!focus_items_) {  // 1…9, then 0 for the 10th
		auto entries = sidebar_items();
		auto n = static_cast<std::size_t>(key == '0' ? 9 : key - '1');
		if (n < entries.size()) {
			select_view(entries[n].view);
		}
		return true;
	}
	if (fn) {
		switch (key) {
			case KEY_UP:
				move_selection(-1);
				return true;
			case KEY_DOWN:
				move_selection(1);
				return true;
			case KEY_PPAGE:
				move_selection(-(LINES - 4));
				return true;
			case KEY_NPAGE:
				move_selection(LINES - 4);
				return true;
			case KEY_RESIZE:
				return true;
			case KEY_RIGHT:
				focus_items_ = true;
				return true;
			case KEY_LEFT:
				focus_items_ = false;
				return true;
		}
	}

	if (!focus_items_) {
		auto entries = sidebar();
		bool on_row =
			side_sel_ >= 0 && side_sel_ < static_cast<int>(entries.size());
		auto heading = on_row
						 ? entries[static_cast<std::size_t>(side_sel_)].kind
						 : SidebarEntry::Item;
		bool activate = (!fn && (key == '\n' || key == '\r' || key == ' ')) ||
						(fn && key == KEY_ENTER);
		if (activate && heading == SidebarEntry::FoldHeading) {
			toggle_fold(entries[static_cast<std::size_t>(side_sel_)].group);
			return true;
		}
		// Ctrl+↑/↓ (J / K) move the selected entry within its group (on a
		// heading, the group); Ctrl+Shift+↑/↓ move its group. As in the app.
		if (!fn && (key == 'J' || key == 'K')) {
			if (heading == SidebarEntry::Item) {
				move_entry(key == 'K' ? -1 : 1);
			} else {
				move_group(key == 'K' ? -1 : 1);
			}
			return true;
		}
		if (!fn && (key == kGroupUp || key == kGroupDown)) {
			move_group(key == kGroupUp ? -1 : 1);
			return true;
		}
		if ((!fn && (key == '\n' || key == '\r' || key == 'l')) ||
			(fn && key == KEY_ENTER)) {
			focus_items_ = true;
		} else if (!fn && key == 'n') {
			focus_items_ = true;
			return handle_key('n', false);
		}
		return true;
	}

	// Adding works without a selection.
	if (!fn && key == 'n') {
		auto text = prompt("New reminder:");
		if (!text || text->empty()) {
			return true;
		}
		rem::ListFile* l = in_list ? store_.list(view_.name) : nullptr;
		if (!l && ref) {
			l = ref->list;
		}
		if (!l && !store_.lists().empty()) {
			l = store_.lists().front();
		}
		if (!l) {
			message_ = "Create a list first (N)";
			return true;
		}
		rem::Reminder r;
		r.fields() = rem::parse_fields(*text);
		r.created = today;
		if (view_.kind == View::Today && !r.due_date) {
			r.due_date = today;
		}
		if (view_.kind == View::Flagged) {
			r.flagged = true;
		}
		undoable("Add Reminder", [&] {
			std::optional<std::string> section =
				ref && in_list
					? ref->list->doc.section_of(ref->parent ? *ref->parent
															: *ref->reminder)
					: std::nullopt;
			item_sel_ = store_.add(*l, std::move(r), nullptr, section).id;
		});
		return true;
	}
	// Marking, and the keys that then act on every marked reminder.
	if (!fn && key == 'v' && ref) {
		marked_.toggle(id);
		move_selection(1);
		return true;
	}
	if (!fn && key == '*') {
		auto all = shown_items();
		marked_.select_all(all);
		if (!all.empty()) {
			message_ = std::format("{} marked", all.size());
		}
		return true;
	}
	if (!marked_.empty()) {
		auto k = fn && key == KEY_DC ? kDelete : key;
		if ((!fn || k == kDelete) && act_on_marked(k)) {
			return true;
		}
	}

	if (!ref) {
		return true;
	}
	auto& r = *ref->reminder;

	if (fn && key == KEY_DC) {
		key = kDelete, fn = false;
	}
	if (fn && key == KEY_ENTER) {
		key = '\n', fn = false;
	}
	if (fn) {
		return true;
	}

	switch (key) {
		case 'x':
		case ' ':
			{
				bool hides = !r.done && !show_completed_ &&
							 view_.kind != View::Completed &&
							 view_.kind != View::AllReminders;
				if (hides) {  // it's about to disappear: keep the place by
							  // selecting its neighbour
					move_selection(1);
					if (item_sel_ == id) {
						move_selection(-1);
					}
				}
				undoable("Complete",
						 [&] { rem::complete(store_, {id}, today); });
				break;
			}
		case '\n':
		case '\r':
			edit_title_in_place(id);
			break;
		case 'e':
			edit_in_editor(id);
			break;
		case 'd':
			if (auto d =
					prompt("Due (today, tomorrow, fri, +3d, 2026-10-31, none):",
						   r.due_date ? rem::format_date(*r.due_date) : "")) {
				if (term::lower(*d) == "none" || d->empty()) {
					undoable("Clear Due Date",
							 [&] { rem::set_due(store_, {id}, std::nullopt); });
				} else {
					auto words = *d;
					std::optional<rem::TimeOfDay> time;
					if (auto sp = words.rfind(' '); sp != std::string::npos) {
						if ((time = rem::parse_time(words.substr(sp + 1)))) {
							words.resize(sp);
						}
					}
					if (auto date = rem::parse_human_date(words, today)) {
						undoable("Set Due Date", [&] {
							rem::set_due(store_, {id}, date, time);
						});
					} else {
						message_ = std::format("Can't read “{}”", *d);
					}
				}
			}
			break;
		case 't':
			undoable("Due Today", [&] { rem::set_due(store_, {id}, today); });
			break;
		case 'T':
			undoable("Due Tomorrow", [&] {
				rem::set_due(store_, {id},
							 rem::Date{std::chrono::sys_days{today} +
									   std::chrono::days{1}});
			});
			break;
		case 'f':
			undoable("Flag", [&] { rem::toggle_flag(store_, {id}); });
			break;
		case '0':
		case '1':
		case '2':
		case '3':
			undoable("Priority", [&] {
				rem::set_priority(store_, {id},
								  static_cast<rem::Priority>(key - '0'));
			});
			break;
		case '#':
			if (auto t = prompt("Tag (-tag removes):"); t && !t->empty()) {
				auto tag = *t;
				bool remove = tag.starts_with('-');
				if (remove) {
					tag.erase(0, 1);
				}
				if (tag.starts_with('#')) {
					tag.erase(0, 1);
				}
				undoable("Tag",
						 [&] { rem::set_tag(store_, {id}, tag, !remove); });
			}
			break;
		case 'm':
			if (auto name = prompt("Move to list:"); name && !name->empty()) {
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
				} else {
					undoable("Move",
							 [&] { rem::move_to_list(store_, {id}, *dest); });
				}
			}
			break;
		case 'J':
		case 'K':
			if (!in_list) {
				message_ = "Reorder in a list view";
				break;
			}
			undoable("Move", [&] {
				auto visible = [this](const rem::Reminder& x) {
					return show_completed_ || !x.done;
				};
				if (ref->list->doc.move_step(id, key == 'K', visible)) {
					store_.save(*ref->list);
				}
			});
			break;
		case ']':
		case '[':
			if (!in_list) {
				message_ = "Indent in a list view";
				break;
			}
			undoable(key == ']' ? "Indent" : "Outdent", [&] {
				auto visible = [this](const rem::Reminder& x) {
					return show_completed_ || !x.done;
				};
				bool ok = key == ']' ? ref->list->doc.indent(id, visible)
									 : ref->list->doc.outdent(id);
				if (ok) {
					store_.save(*ref->list);
				} else {
					message_ =
						key == ']' ? "Can't indent this one" : "Not a subtask";
				}
			});
			break;
		case '+':  // fold / unfold
		case kShowSubtasks:
		case kHideSubtasks:
			{
				if (!in_list) {
					break;
				}
				// On a subtask, its parent's (as in the app).
				auto target = !r.subtasks.empty() ? id
							: ref->parent		  ? ref->parent->id
												  : std::string();
				if (target.empty()) {
					break;
				}
				bool hide = key == '+' ? !collapsed_.contains(target)
									   : key == kHideSubtasks;
				if (hide && target != id && collapsed_.contains(target)) {
					break;
				}
				if (hide) {
					collapsed_.insert(target);
					item_sel_ = target;	 // its subtasks are gone
				} else {
					collapsed_.erase(target);
				}
				break;
			}
		case kDelete:
			if (confirm(std::format("Delete “{}”?", r.title))) {
				move_selection(1);
				if (item_sel_ == id) {
					move_selection(-1);
				}
				undoable("Delete", [&] { store_.remove(id); });
				message_ = "Deleted (u to undo)";
			}
			break;
	}
	return true;
}

}  // namespace tui
