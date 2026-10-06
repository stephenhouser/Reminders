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
#include "support.hpp"
#include "window_internal.hpp"

namespace ui {

void Window::rebuild_content() {
	if (!store_) {
		return;
	}
	first_new_entry_ = nullptr;
	shown_ids_.clear();	 // build_reminder_row adds each row
	reminder_rows_.clear();
	auto* title = ADW_WINDOW_TITLE(content_title_);
	bool is_list = view_.kind == View::List;
	gtk_widget_set_visible(new_button_, is_list);
	gtk_widget_set_visible(list_menu_button_, is_list);

	GtkWidget* body = nullptr;
	clamp_ = nullptr;
	first_row_ = nullptr;
	std::string page_title;
	if (is_list) {
		auto* l = store_->list(view_.name);
		if (!l) {
			view_ = home_view();
			return refresh();
		}
		page_title = l->name;
		// "6 Reminders / 3 Complete": every reminder (subtasks included), and
		// how many of them are done (shown with Ctrl+H or ⋮ → Show Completed).
		count_subtitle_ = rem::view_count(*store_, view_, today()).label();
		body = build_list_view(*l);
	} else {
		if (auto* s = smart_info(view_.kind)) {
			page_title = s->title;
		} else if (view_.kind == View::Tag) {
			page_title = "#" + view_.name;
		} else {
			page_title = "Search";
		}
		// The same kind of count as a list's: what the view contains.
		count_subtitle_ = rem::view_count(*store_, view_, today()).label();
		body = build_smart_view();
	}
	adw_window_title_set_title(title, page_title.c_str());
	adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(content_page_),
								  page_title.c_str());
	// Selected reminders no longer shown (completed and hidden, deleted
	// elsewhere) drop out of the selection.
	selection_.prune(shown_ids_);
	update_selection();	 // also sets the subtitle

	// Keep the scroll position across rebuilds.
	auto* adj = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(content_scroller_));
	double scroll = gtk_adjustment_get_value(adj);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(content_scroller_), body);
	auto keep = Obj<GtkAdjustment>::ref(adj);
	idle([keep, scroll] { gtk_adjustment_set_value(keep.get(), scroll); });
}

GtkWidget* Window::group(const std::string& title, const char* color,
						 GtkWidget* listbox) {
	auto* box = vbox(6);
	if (!title.empty()) {
		auto* heading = label(title, {"heading"});
		gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END);
		if (color) {
			gtk_widget_add_css_class(heading, "list-heading");
			gtk_widget_add_css_class(heading, color_class(color).c_str());
		}
		gtk_widget_set_margin_start(heading, 6);
		append(box, {heading});
	}
	append(box, {listbox});
	return box;
}

// A named section: its heading, with a menu to rename or delete it.
GtkWidget* Window::section_group(rem::ListFile& l, const std::string& name,
								 int count, GtkWidget* listbox) {
	auto list = store_->key_of(
		l);	 // "source/name": a bare name is ambiguous when two sources have it
	auto* actions = g_simple_action_group_new();
	add_action(actions, "rename",
			   [this, list, name] { rename_section(list, name); });
	add_action(actions, "delete", [this, list, name, count] {
		delete_section(list, name, count);
	});

	auto* menu = g_menu_new();
	g_menu_append(menu, "_Rename Section…", "section.rename");
	g_menu_append(menu, "_Delete Section…", "section.delete");
	auto* button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button),
								  "view-more-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "circular");
	gtk_widget_set_tooltip_text(button, "Section Menu");
	g_object_unref(menu);

	auto* heading = label(
		name, {"heading", "list-heading", color_class(l.color()).c_str()});
	gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(heading, TRUE);
	gtk_widget_set_margin_start(heading, 6);
	auto* header = hbox(6);
	append(header, {heading, button});
	gtk_widget_insert_action_group(header, "section", G_ACTION_GROUP(actions));
	g_object_unref(actions);

	auto* box = vbox(6);
	append(box, {header, listbox});
	return box;
}

void Window::rename_section(const std::string& list, const std::string& name) {
	auto* dialog = adw_alert_dialog_new("Rename Section", nullptr);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
								   "_Cancel", "rename", "_Rename", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "rename",
											 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "rename");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	auto* entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(entry), name.c_str());
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, list, name, entry](AdwAlertDialog*, const char* response) {
			if (std::string_view(response) != "rename") {
				return;
			}
			auto new_name = trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
			auto* l = store_->list(list);
			if (!l || new_name.empty() || new_name == name) {
				return;
			}
			bool ok = false;
			undoable("Rename Section", [&] {
				ok = l->doc.rename_section(name, new_name);
				if (!ok) {
					return;
				}
				try {
					store_->save(*l);
				} catch (const std::exception& e) {
					toast(std::format("Couldn't save: {}", e.what()));
				}
			});
			if (!ok) {
				toast(std::format("There's already a section called “{}”",
								  new_name));
			}
			refresh();
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::delete_section(const std::string& list, const std::string& name,
							int count) {
	auto body = count == 0 ? std::string("The section is empty.")
						   : std::format(
								 "It has {} reminder{}. Keep them by moving "
								 "them to the section above, or "
								 "delete them with the section.",
								 count, count == 1 ? "" : "s");
	auto* dialog = adw_alert_dialog_new(
		std::format("Delete “{}”?", name).c_str(), body.c_str());
	if (count == 0) {
		adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
									   "_Cancel", "keep", "_Delete", nullptr);
		adw_alert_dialog_set_response_appearance(
			ADW_ALERT_DIALOG(dialog), "keep", ADW_RESPONSE_DESTRUCTIVE);
	} else {
		adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
									   "_Cancel", "keep", "_Keep Reminders",
									   "delete", "_Delete Reminders", nullptr);
		adw_alert_dialog_set_response_appearance(
			ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
	}
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, list, name](AdwAlertDialog*, const char* response) {
			std::string_view r = response;
			if (r != "keep" && r != "delete") {
				return;
			}
			auto* l = store_->list(list);
			if (!l) {
				return;
			}
			undoable("Delete Section", [&] {
				if (!l->doc.delete_section(name, r == "keep")) {
					return;
				}
				try {
					store_->save(*l);
				} catch (const std::exception& e) {
					toast(std::format("Couldn't save: {}", e.what()));
				}
			});
			refresh();
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

GtkWidget* Window::empty_state(const char* icon_name, const char* title,
							   const char* description) {
	auto* page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon_name);
	adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
	if (description) {
		adw_status_page_set_description(ADW_STATUS_PAGE(page), description);
	}
	gtk_widget_set_vexpand(page, TRUE);
	return page;
}

// The page's lists take a share of the content area's width (see
// update_clamp), so they widen with the window and when the sidebar hides.
GtkWidget* Window::clamp(GtkWidget* child) {
	auto* c = adw_clamp_new();
	adw_clamp_set_child(ADW_CLAMP(c), child);
	clamp_ = c;
	update_clamp();
	gtk_widget_set_margin_top(child, 18);
	gtk_widget_set_margin_bottom(child, 24);
	gtk_widget_set_margin_start(child, 12);
	gtk_widget_set_margin_end(child, 12);
	return c;
}

GtkWidget* Window::build_list_view(rem::ListFile& l) {
	auto* page = vbox(24);
	std::vector<GtkWidget*> listboxes;
	for (auto& section : l.doc.sections()) {
		auto* listbox = boxed_list();
		for (auto* r : section.reminders) {
			if (r->done && !show_completed_) {
				continue;
			}
			gtk_list_box_append(
				GTK_LIST_BOX(listbox),
				build_reminder_row(rem::Ref{&l, r, nullptr}, false));
			if (collapsed_.contains(r->id)) {
				continue;
			}
			for (auto& s : r->subtasks) {
				if (s.done && !show_completed_) {
					continue;
				}
				gtk_list_box_append(
					GTK_LIST_BOX(listbox),
					build_reminder_row(rem::Ref{&l, &s, r}, false));
			}
		}
		gtk_list_box_append(GTK_LIST_BOX(listbox),
							build_new_row(store_->key_of(l), section.name));
		if (section.name) {
			int count = 0;
			for (auto* r : section.reminders) {
				count += 1 + static_cast<int>(r->subtasks.size());
			}
			append(page, {section_group(l, *section.name, count, listbox)});
		} else {
			append(page, {group("", nullptr, listbox)});
		}
		listboxes.push_back(listbox);
	}
	chain_listboxes(listboxes);
	return clamp(page);
}

std::vector<rem::Ref> Window::view_refs() {
	return rem::view_refs(*store_, view_, today());
}

GtkWidget* Window::build_smart_view() {
	auto day = today();
	auto refs = view_refs();

	if (refs.empty()) {
		switch (view_.kind) {
			case View::Today:
				return empty_state("object-select-symbolic",
								   "Nothing Due Today", nullptr);
			case View::Scheduled:
				return empty_state("alarm-symbolic", "No Scheduled Reminders",
								   nullptr);
			case View::Flagged:
				return empty_state("sr-flag-symbolic", "No Flagged Reminders",
								   nullptr);
			case View::Completed:
				return empty_state("object-select-symbolic",
								   "No Completed Reminders", nullptr);
			case View::Search:
				return empty_state("edit-find-symbolic", "No Results",
								   "Try a different search");
			default:
				return empty_state("view-list-bullet-symbolic", "No Reminders",
								   nullptr);
		}
	}

	// Each view groups its reminders: by date, by list, or in one group.
	auto* page = vbox(24);
	std::vector<GtkWidget*> listboxes;
	bool show_list = rem::shows_list_name(view_);
	for (auto& g : rem::grouped(*store_, view_, day)) {
		auto* box = boxed_list();
		for (auto& ref : g.refs) {
			gtk_list_box_append(GTK_LIST_BOX(box),
								build_reminder_row(ref, show_list));
		}
		std::string title =
			g.kind == rem::RefGroup::Overdue ? "Overdue"
			: g.kind == rem::RefGroup::Day	 ? relative_date(g.day, day)
			: g.kind == rem::RefGroup::List	 ? store_->label(*g.list)
											 : "";
		std::string color =
			g.kind == rem::RefGroup::List ? g.list->color() : "";
		append(page,
			   {group(title, color.empty() ? nullptr : color.c_str(), box)});
		listboxes.push_back(box);
	}
	chain_listboxes(listboxes);
	return clamp(page);
}

GtkWidget* Window::build_new_row(const std::string& list,
								 const std::optional<std::string>& section) {
	auto* row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_widget_add_css_class(row, "new-reminder-row");
	auto* box = hbox(12);
	gtk_widget_set_margin_top(box, 10);
	gtk_widget_set_margin_bottom(box, 10);
	gtk_widget_set_margin_start(box, 14);
	gtk_widget_set_margin_end(box, 12);
	auto* entry = gtk_text_new();
	gtk_text_set_placeholder_text(GTK_TEXT(entry), "New Reminder");
	gtk_widget_set_hexpand(entry, TRUE);
	gtk_widget_set_tooltip_text(
		entry, "Fields work here too, e.g. “Milk #errands 🚩 📅 2026-10-03”");
	append(box, {icon("list-add-symbolic", {"dim-label"}), entry});
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

	add_shortcut(entry, "<Control>s",
				 [entry] { g_signal_emit_by_name(entry, "activate"); });
	add_shortcut(entry, "Escape",
				 [entry] { gtk_editable_set_text(GTK_EDITABLE(entry), ""); });
	connect<void(GtkText*)>(
		entry, "activate", [this, list, section](GtkText* t) {
			std::string text = gtk_editable_get_text(GTK_EDITABLE(t));
			if (trim(text).empty()) {
				return;
			}
			idle([this, list, section, text] {
				add_reminder(list, section, text);
			});
		});

	make_drop_target(row, DropStyle::Above, "",
					 [this, list, section](Ids dropped, rem::Document::Place) {
						 move_to_section_end(dropped, list, section);
					 });

	auto key = list + '\x1f' + section.value_or("");
	if (!first_new_entry_) {
		first_new_entry_ = entry;
	}
	if (focus_new_row_ == key) {
		focus_new_row_.reset();
		auto keep = Obj<GtkWidget>::ref(entry);
		idle([keep] { gtk_widget_grab_focus(keep.get()); });
	}
	return row;
}

// GTK doesn't scroll during drag and drop on its own: scroll when the
// pointer is near the top or bottom edge of the content.
void Window::setup_autoscroll() {
	auto* motion = gtk_drop_controller_motion_new();
	connect<void(GtkDropControllerMotion*, double, double)>(
		motion, "motion", [this](GtkDropControllerMotion*, double, double y) {
			autoscroll_y_ = y;
			if (autoscroll_timer_) {
				return;
			}
			autoscroll_timer_ = timeout(16, [this] {
				if (autoscroll_y_ < 0) {
					autoscroll_timer_ = 0;
					return false;
				}
				constexpr double edge = 56, max_step = 14;
				double h = gtk_widget_get_height(content_scroller_);
				double step = 0;
				if (autoscroll_y_ < edge) {
					step = -max_step * (1 - autoscroll_y_ / edge);
				} else if (autoscroll_y_ > h - edge) {
					step = max_step * (1 - (h - autoscroll_y_) / edge);
				}
				if (step != 0) {
					auto* adj = gtk_scrolled_window_get_vadjustment(
						GTK_SCROLLED_WINDOW(content_scroller_));
					gtk_adjustment_set_value(
						adj, gtk_adjustment_get_value(adj) + step);
				}
				return true;
			});
		});
	auto stop = [this] { autoscroll_y_ = -1; };
	connect<void(GtkDropControllerMotion*)>(
		motion, "leave", [stop](GtkDropControllerMotion*) { stop(); });
	gtk_widget_add_controller(content_scroller_, GTK_EVENT_CONTROLLER(motion));
}

void Window::toggle_subtasks(const std::string& id) {
	if (!collapsed_.erase(id)) {
		collapsed_.insert(id);
	}
	focus_reminder_ = id;
	rebuild_content();
}

void Window::focus_results() {
	// The entry waits a moment before reporting changes; catch up first.
	auto text = trim(gtk_editable_get_text(GTK_EDITABLE(search_entry_)));
	if (text.empty()) {
		return;
	}
	if (!(view_ == View{View::Search, text})) {
		select(View{View::Search, text});
	}
	show_content();
	if (!first_row_) {
		return;
	}
	auto keep = Obj<GtkWidget>::ref(first_row_);
	idle([keep] { gtk_widget_grab_focus(keep.get()); });
}

void Window::focus_content() {
	auto* target = first_row_ ? first_row_ : first_new_entry_;
	if (!target) {
		return;	 // nothing to focus (an empty smart list)
	}
	auto keep = Obj<GtkWidget>::ref(target);
	idle([keep] { gtk_widget_grab_focus(keep.get()); });
}

}  // namespace ui
