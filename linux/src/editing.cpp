#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include "dialogs.hpp"
#include "reminders/clipboard.hpp"
#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/importer.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/syncthing.hpp"
#include "support.hpp"
#include "window_internal.hpp"

namespace ui {

void Window::add_reminder(const std::string& list,
						  const std::optional<std::string>& section,
						  const std::string& text) {
	undoable("Add Reminder", [&] {
		auto* l = store_->list(list);
		if (!l) {
			toast(std::format("Couldn't find the list “{}”", list));
			return;
		}
		rem::Reminder r;
		r.fields() = rem::parse_fields(trim(text));
		r.created = today();
		try {
			store_->add(*l, std::move(r), nullptr, section);
		} catch (const std::exception& e) {
			toast(std::format("Couldn't save: {}", e.what()));
		}
		focus_new_row_ = list + '\x1f' + section.value_or("");
		refresh();
	});
}

void Window::edit_title(const std::string& id, const std::string& text) {
	undoable("Edit Reminder", [&] {
		auto ref = store_->find(id);
		if (!ref) {
			return;
		}
		if (trim(text).empty()) {
			return delete_reminders({id});
		}
		auto& r = *ref->reminder;
		// Typed fields ("#tag", "📅 2026-10-03", …) are applied, not kept in
		// the title.
		auto f = rem::parse_fields(trim(text));
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
			r.due_date = f.due_date;
			r.due_time = f.due_time;
		}
		if (f.url) {
			r.url = f.url;
		}
		try {
			store_->touch(id);
		} catch (const std::exception& e) {
			toast(std::format("Couldn't save: {}", e.what()));
		}
		refresh();
	});
}

void Window::toggle_done(const std::string& id, bool done) {
	undoable("Complete Reminder", [&] {
		try {
			store_->set_done(id, done, today());
		} catch (const std::exception& e) {
			toast(std::format("Couldn't save: {}", e.what()));
		}
		refresh_later(kCompleteDelayMs);
	});
}

void Window::complete_reminders(const std::vector<std::string>& ids) {
	bool all_done = std::ranges::all_of(ids, [this](auto& id) {
		auto ref = store_->find(id);
		return !ref || ref->reminder->done;
	});
	batch(ids.size() > 1 ? "Complete Reminders" : "Complete Reminder", [&] {
		for (auto& id : ids) {
			store_->set_done(id, !all_done, today());
		}
	});
	// Show the change at once, as a click on one circle does; the rows then
	// linger a moment before the view is rebuilt (and completed ones hide).
	syncing_checks_ = true;
	for (auto& [id, row] : reminder_rows_) {
		auto ref = store_->find(id);
		auto* check =
			static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(row), "check"));
		if (ref && check) {
			gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
										ref->reminder->done);
		}
	}
	syncing_checks_ = false;
	refresh_later(kCompleteDelayMs);
}

// Several land together, in their order: the first next to `target`, each
// other after the one before.
void Window::move_reminders(const std::vector<std::string>& ids,
							const std::string& target,
							rem::Document::Place place) {
	bool nested = false;
	batch(ids.size() > 1 ? "Move Reminders" : "Move Reminder", [&] {
		auto anchor = target;
		for (auto& id : ids) {
			auto ref = store_->find(id);
			auto target_ref = store_->find(anchor);
			if (!ref || !target_ref) {
				continue;
			}
			if (ref->list != target_ref->list) {
				store_->move_to_list(id, *target_ref->list);
			}
			auto* l = store_->find(anchor)->list;
			if (l->doc.move_next_to(id, anchor, place)) {
				anchor = id;
				place = rem::Document::Place::After;
			} else {
				nested = true;
			}
			store_->save(*l);  // also covers a move from another list
		}
	});
	if (nested) {
		toast("Subtasks can't have subtasks of their own");
	}
	refresh();
}

void Window::move_to_section_end(const std::vector<std::string>& ids,
								 const std::string& list,
								 const std::optional<std::string>& section) {
	batch(ids.size() > 1 ? "Move Reminders" : "Move Reminder", [&] {
		auto* l = store_->list(list);
		if (!l) {
			return;
		}
		for (auto& id : ids) {
			auto ref = store_->find(id);
			if (!ref) {
				continue;
			}
			if (ref->list != l) {
				store_->move_to_list(id, *l);
			}
			l->doc.move_to_end(id, section);
			store_->save(*l);
		}
	});
	refresh();
}

void Window::move_to_list(const std::vector<std::string>& ids,
						  const std::string& list) {
	auto* l = store_->list(list);
	if (!l) {
		return;
	}
	int moved = 0;
	batch(ids.size() > 1 ? "Move Reminders" : "Move Reminder", [&] {
		for (auto& id : ids) {
			auto ref = store_->find(id);
			if (!ref || ref->list == l) {
				continue;
			}
			store_->move_to_list(id, *l);
			++moved;
		}
	});
	if (moved == 0) {
		return;
	}
	refresh();
	auto name = store_->label(*l);
	toast(moved == 1 ? std::format("Moved to “{}”", name)
					 : std::format("Moved {} reminders to “{}”", moved, name));
}

void Window::set_priority(const std::vector<std::string>& ids,
						  rem::Priority priority) {
	batch("Set Priority", [&] {
		for (auto& id : ids) {
			if (auto ref = store_->find(id);
				ref && ref->reminder->priority != priority) {
				ref->reminder->priority = priority;
				store_->touch(id);
			}
		}
	});
	keep_focus(ids);
	refresh();
}

void Window::toggle_flag(const std::vector<std::string>& ids) {
	bool all_flagged = std::ranges::all_of(ids, [this](auto& id) {
		auto ref = store_->find(id);
		return !ref || ref->reminder->flagged;
	});
	batch(ids.size() > 1 ? "Flag Reminders" : "Flag Reminder", [&] {
		for (auto& id : ids) {
			if (auto ref = store_->find(id)) {
				ref->reminder->flagged = !all_flagged;
				store_->touch(id);
			}
		}
	});
	keep_focus(ids);
	refresh();
}

void Window::set_due(const std::vector<std::string>& ids, int days_from_today) {
	batch("Set Due Date", [&] {
		auto due = rem::Date{std::chrono::sys_days{today()} +
							 std::chrono::days{days_from_today}};
		for (auto& id : ids) {
			if (auto ref = store_->find(id)) {
				ref->reminder->due_date = due;
				store_->touch(id);	// keeps any time already set
			}
		}
	});
	keep_focus(ids);
	refresh();
}

// The reminders as Markdown text (a subtask goes with its parent), as
// copied, and as other apps get them when they're dragged out.
std::string Window::reminders_text(const std::vector<std::string>& ids) {
	std::string text;
	for (auto& id : outermost(ids)) {
		if (auto ref = store_->find(id)) {
			text += rem::to_clipboard_text(*ref->reminder);
		}
	}
	return text;
}

// The reminders as Markdown text, so they also paste into other apps.
void Window::copy_reminders(const std::vector<std::string>& ids) {
	if (!store_) {
		return;
	}
	std::string first;
	int count = 0;
	for (auto& id : outermost(ids)) {
		if (auto ref = store_->find(id)) {
			if (count++ == 0) {
				first = ref->reminder->title;
			}
		}
	}
	if (count == 0) {
		return;
	}
	gdk_clipboard_set_text(gtk_widget_get_clipboard(window_),
						   reminders_text(ids).c_str());
	toast(count == 1 ? std::format("Copied “{}”", first)
					 : std::format("Copied {} reminders", count));
}

void Window::delete_reminders(const std::vector<std::string>& ids) {
	auto gone = outermost(ids);
	std::erase_if(gone, [this](auto& id) { return !store_->find(id); });
	if (gone.empty()) {
		return;
	}
	auto step =
		batch(gone.size() > 1 ? "Delete Reminders" : "Delete Reminder", [&] {
			for (auto& id : gone) {
				store_->remove(id);
			}
		});
	refresh();
	// The toast's Undo only applies while this deletion is still the latest
	// step.
	auto text = gone.size() > 1
				  ? std::format("{} reminders deleted", gone.size())
				  : std::string("Reminder deleted");
	if (step) {
		toast(text, "_Undo", [this, step] {
			if (history_.next_undo() == step) {
				undo();
			}
		});
	}
}

void Window::move_step(const std::string& id, bool up) {
	undoable("Move Reminder", [&] {
		auto ref = store_->find(id);
		if (!ref) {
			return;
		}
		// Hidden completed reminders are skipped, so each step moves past a
		// visible one.
		bool show_done = show_completed_;
		auto visible = [show_done](const rem::Reminder& r) {
			return show_done || !r.done;
		};
		if (!ref->list->doc.move_step(id, up, visible)) {
			return;
		}
		try {
			store_->save(*ref->list);
		} catch (const std::exception& e) {
			toast(std::format("Couldn't save: {}", e.what()));
		}
		focus_reminder_ = id;
		refresh();
	});
}

void Window::indent(const std::string& id, bool in) {
	auto ref = store_->find(id);
	if (!ref) {
		return;
	}
	bool show_done = show_completed_;
	auto visible = [show_done](const rem::Reminder& r) {
		return show_done || !r.done;
	};
	auto* list = ref->list;
	bool moved = false;
	undoable(in ? "Indent" : "Outdent", [&] {
		moved = in ? list->doc.indent(id, visible) : list->doc.outdent(id);
		if (!moved) {
			return;
		}
		try {
			store_->save(*list);
		} catch (const std::exception& e) {
			toast(std::format("Couldn't save: {}", e.what()));
		}
	});
	if (!moved && in && !ref->reminder->subtasks.empty()) {
		toast("Subtasks can't have subtasks of their own");
	}
	if (auto now = store_->find(id); moved && now && now->parent) {
		collapsed_.erase(now->parent->id);
	}
	focus_reminder_ = id;
	refresh();
}

void Window::paste_reminders() {
	if (!store_) {
		return;
	}
	auto keep = Obj<GtkWindow>::ref(GTK_WINDOW(window_));
	gdk_clipboard_read_text_async(
		gtk_widget_get_clipboard(window_), nullptr,
		[](GObject* source, GAsyncResult* result, gpointer data) {
			auto* holder = static_cast<Obj<GtkWindow>*>(data);
			char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source),
														result, nullptr);
			if (auto* self = Window::from(holder->get()); self && text) {
				self->add_pasted(text);
			}
			g_free(text);
			delete holder;
		},
		new Obj<GtkWindow>(std::move(keep)));
}

// Ctrl+V: after the focused reminder (in its list and section), else at the
// end of the list being shown (see add_text).
void Window::add_pasted(const std::string& text, rem::TextSplit split,
						bool offer_switch) {
	std::optional<std::string> anchor;
	for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w;
		 w = gtk_widget_get_parent(w)) {
		if (auto* id = static_cast<const char*>(
				g_object_get_data(G_OBJECT(w), "reminder-id"))) {
			anchor = id;
			break;
		}
	}
	add_text(text, "Paste", {}, anchor, rem::Document::Place::After, split,
			 offer_switch);
}

// Ctrl+Shift+V: for text of several lines, asks whether they're one
// reminder (the first line its title, the rest its notes) or one each.
void Window::paste_special() {
	if (!store_) {
		return;
	}
	auto keep = Obj<GtkWindow>::ref(GTK_WINDOW(window_));
	gdk_clipboard_read_text_async(
		gtk_widget_get_clipboard(window_), nullptr,
		[](GObject* source, GAsyncResult* result, gpointer data) {
			auto* holder = static_cast<Obj<GtkWindow>*>(data);
			auto text = take_string(gdk_clipboard_read_text_finish(
				GDK_CLIPBOARD(source), result, nullptr));
			auto* self = Window::from(holder->get());
			delete holder;
			if (!self || text.empty()) {
				return;
			}
			auto lines =
				rem::from_clipboard_text(text, rem::TextSplit::Lines).size();
			if (lines < 2) {
				return self->add_pasted(text);
			}
			auto* dialog = adw_alert_dialog_new(
				std::format("Paste {} Lines", lines).c_str(),
				"As one reminder, with the first line its title and the rest "
				"its notes, or a reminder for each line?");
			adw_alert_dialog_add_responses(
				ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "one",
				"_One Reminder", "lines",
				std::format("{} _Reminders", lines).c_str(), nullptr);
			// The one Ctrl+V would choose is the default.
			adw_alert_dialog_set_default_response(
				ADW_ALERT_DIALOG(dialog),
				rem::is_list_text(text) ? "lines" : "one");
			adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog),
												"cancel");
			connect<void(AdwAlertDialog*, const char*)>(
				dialog, "response",
				[self, text](AdwAlertDialog*, const char* r) {
					std::string_view response = r;
					if (response == "cancel") {
						return;
					}
					self->add_pasted(text,
									 response == "one" ? rem::TextSplit::One
													   : rem::TextSplit::Lines,
									 false);
				});
			adw_dialog_present(ADW_DIALOG(dialog), self->window_);
		},
		new Obj<GtkWindow>(std::move(keep)));
}

// Adds text (pasted, or dropped from another app) as reminders, parsed as
// clipboard.hpp says. Into `list` (a key), or with none into the list in
// view, or in a smart list into the first list, set up to show there (due
// today in Today or Scheduled, flagged in Flagged, tagged in a tag's view).
// Next to reminder `anchor` (`place`: before or after it; a subtask's
// parent), else at the end.
void Window::add_text(const std::string& text, const char* label,
					  const std::string& list_key,
					  const std::optional<std::string>& anchor_id,
					  rem::Document::Place place, rem::TextSplit split,
					  bool offer_switch) {
	auto added = rem::from_clipboard_text(text, split);
	if (added.empty() || !store_) {
		return;
	}
	auto anchor = anchor_id ? store_->find(*anchor_id) : std::nullopt;
	bool for_view = list_key.empty();  // made to show in the view
	rem::ListFile* list = !for_view				   ? store_->list(list_key)
						: anchor				   ? anchor->list
						: view_.kind == View::List ? store_->list(view_.name)
												   : nullptr;
	if (!list && for_view && !store_->lists().empty()) {
		list = store_->lists().front();
	}
	if (!list) {
		toast("Create a list first");
		return;
	}
	if (anchor && anchor->list != list) {
		anchor.reset();
	}
	const rem::Reminder* next_to =
		anchor ? (anchor->parent ? anchor->parent : anchor->reminder) : nullptr;
	std::optional<std::string> section =
		next_to ? list->doc.section_of(*next_to) : std::nullopt;
	bool before = next_to && place == rem::Document::Place::Before;
	std::string before_id = before ? next_to->id : "";
	std::string after_id = next_to && !before ? next_to->id : "";

	auto day = today();
	auto count = added.size();
	auto title = added.front().title;
	auto step = undoable(label, [&] {
		std::string first;
		try {
			for (auto& r : added) {
				if (!r.created) {
					r.created = day;
				}
				if (for_view) {
					if (!r.done && !r.due_date &&
						(view_.kind == View::Today ||
						 view_.kind == View::Scheduled)) {
						r.due_date = day;
					}
					if (view_.kind == View::Flagged) {
						r.flagged = true;
					}
					if (view_.kind == View::Tag &&
						std::ranges::find(r.tags, view_.name) == r.tags.end()) {
						r.tags.push_back(view_.name);
					}
				}
				if (before) {  // each just above the anchor, so they keep their
							   // order
					auto id = store_
								  ->add(*list, std::move(r),
										list->doc.find(before_id), section)
								  .id;
					list->doc.move_next_to(id, before_id,
										   rem::Document::Place::Before);
					store_->save(*list);
					if (first.empty()) {
						first = id;
					}
				} else {
					const rem::Reminder* at =
						after_id.empty() ? nullptr : list->doc.find(after_id);
					after_id = store_->add(*list, std::move(r), at, section).id;
					if (first.empty()) {
						first = after_id;
					}
				}
			}
		} catch (const std::exception& e) {
			toast(std::format("Couldn't save: {}", e.what()));
		}
		if (!first.empty()) {
			focus_reminder_ = first;
		}
		refresh();
	});
	// Several lines: say how they came in, and offer the other way (it
	// undoes this and adds them again, split or combined).
	auto as_lines =
		rem::from_clipboard_text(text, rem::TextSplit::Lines).size();
	if (as_lines > 1) {
		bool combined = count == 1;
		auto message = combined ? std::format("Added “{}” with notes", title)
								: std::format("Added {} reminders", count);
		if (!offer_switch || !step) {
			return toast(message);
		}
		auto other = combined ? rem::TextSplit::Lines : rem::TextSplit::One;
		auto button = combined ? std::format("_Split into {}", as_lines)
							   : std::string("_Combine into One");
		toast(message, button.c_str(),
			  [this, step, text, label = std::string(label), list_key,
			   anchor_id, place, other] {
				  if (history_.next_undo() != step) {
					  return;  // something else changed since
				  }
				  undo();
				  add_text(text, label.c_str(), list_key, anchor_id, place,
						   other, false);
			  });
	} else if (!for_view &&
			   (view_.kind != View::List || view_.name != list_key)) {
		toast(std::format("Added to “{}”", store_->label(*list)));
	}
}

void Window::show_details(const std::string& id) {
	auto ref = store_->find(id);
	if (!ref) {
		return;
	}
	std::vector<std::string>
		names;	// labels: "Name", or "source/Name" when names clash
	for (auto* l : store_->lists()) {
		names.push_back(store_->label(*l));
	}
	show_reminder_dialog(
		window_, *ref->reminder, ref->parent != nullptr,
		store_->label(*ref->list), names, [this, id](ReminderEdit e) {
			if (e.deleted) {
				return delete_reminders({id});
			}
			auto ref = store_->find(id);
			if (!ref) {
				toast("This reminder was changed on another device");
				return;
			}
			undoable("Edit Reminder", [&] {
				auto& r = *ref->reminder;
				bool done = r.done;
				r.fields() = e.fields;
				r.done = done;
				r.notes = e.notes;
				for (auto& title : e.new_subtasks) {
					rem::Reminder s;
					s.title = title;
					s.id = rem::new_id();
					s.created = today();
					r.subtasks.push_back(std::move(s));
				}
				try {
					store_->touch(id);
					if (e.list != store_->label(*ref->list)) {
						if (auto* dest = list_by_label(e.list)) {
							store_->move_to_list(id, *dest);
						}
					}
				} catch (const std::exception& ex) {
					toast(std::format("Couldn't save: {}", ex.what()));
				}
			});
			refresh();
		});
}

}  // namespace ui
