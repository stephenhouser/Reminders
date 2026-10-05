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

GtkWidget* Window::build_reminder_row(const rem::Ref& ref, bool show_list) {
	auto& r = *ref.reminder;
	auto id = r.id;
	auto day = today();

	auto* row = gtk_list_box_row_new();
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_widget_add_css_class(row, "reminder-row");
	gtk_widget_add_css_class(row, color_class(ref.list->color()).c_str());
	g_object_set_data_full(G_OBJECT(row), "reminder-id", g_strdup(id.c_str()),
						   g_free);	 // for paste
	shown_ids_.push_back(id);
	reminder_rows_[id] = row;
	if (selection_.contains(id)) {
		gtk_widget_add_css_class(row, "selected-reminder");
	}

	auto* box = hbox(12);
	gtk_widget_set_margin_top(box, 8);
	gtk_widget_set_margin_bottom(box, 8);
	gtk_widget_set_margin_start(box, ref.parent && !show_list ? 44 : 12);
	gtk_widget_set_margin_end(box, 6);

	auto* check = gtk_check_button_new();
	gtk_widget_add_css_class(check, "selection-mode");
	gtk_widget_set_valign(check, GTK_ALIGN_START);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check), r.done);
	gtk_widget_set_tooltip_text(
		check, r.done ? "Mark as Not Completed" : "Mark as Completed");
	connect<void(GtkCheckButton*)>(
		check, "toggled", [this, id](GtkCheckButton* c) {
			if (!syncing_checks_) {
				toggle_done(id, gtk_check_button_get_active(c));
			}
		});
	g_object_set_data(G_OBJECT(row), "check", check);

	auto* text = vbox(2);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_widget_set_valign(text, GTK_ALIGN_CENTER);

	auto* title_line = hbox(6);
	if (r.priority != rem::Priority::None) {
		auto marks = std::string(static_cast<std::size_t>(r.priority), '!');
		append(title_line, {label(marks, {"priority"})});
	}
	auto* title = gtk_editable_label_new(r.title.c_str());
	gtk_widget_set_hexpand(title, TRUE);
	gtk_editable_set_width_chars(GTK_EDITABLE(title), 1);
	wrap_editable_label(title);
	if (r.done) {
		gtk_widget_add_css_class(title, "dim-label");
	}
	connect<void(GObject*, GParamSpec*)>(
		title, "notify::editing",
		[this, id, old = r.title](GObject* o, GParamSpec*) {
			if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(o))) {
				return;
			}
			std::string now = gtk_editable_get_text(GTK_EDITABLE(o));
			if (now != old) {
				idle([this, id, now] { edit_title(id, now); });
			}
		});
	add_shortcut(title, "<Control>s", [title] {
		if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) {
			gtk_editable_label_stop_editing(GTK_EDITABLE_LABEL(title), TRUE);
		}
	});
	append(title_line, {title});
	append(text, {title_line});

	// Second line: due date, repeat, tags, list.
	auto* meta = hbox(8);
	bool has_meta = false;
	auto add_meta = [&](GtkWidget* w) {
		gtk_box_append(GTK_BOX(meta), w);
		has_meta = true;
	};
	if (r.due_date) {
		auto* due = label(due_label(r, day), {"caption"});
		gtk_widget_add_css_class(due,
								 is_overdue(r, day) ? "error" : "dim-label");
		add_meta(due);
	}
	if (r.repeat) {
		auto* rep = icon("media-playlist-repeat-symbolic", {"dim-label"});
		gtk_widget_set_tooltip_text(rep, r.repeat->c_str());
		add_meta(rep);
	}
	if (!r.tags.empty()) {
		std::string tags;
		for (auto& t : r.tags) {
			tags += (tags.empty() ? "#" : " #") + t;
		}
		auto* tag_label = label(tags, {"caption", "tags"});
		gtk_label_set_ellipsize(GTK_LABEL(tag_label), PANGO_ELLIPSIZE_END);
		add_meta(tag_label);
	}
	if (r.url) {
		// The URL itself, as in the terminal client; long ones are shortened
		// with "…" (the tooltip has the full one).
		auto* link =
			gtk_link_button_new_with_label(r.url->c_str(), r.url->c_str());
		if (auto* text = gtk_button_get_child(GTK_BUTTON(link));
			GTK_IS_LABEL(text)) {
			gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
			gtk_label_set_max_width_chars(GTK_LABEL(text), 50);
		}
		gtk_widget_add_css_class(link, "caption");
		gtk_widget_add_css_class(link, "inline-link");
		add_meta(link);
	}
	if (show_list || ref.parent) {
		std::string where = show_list ? store_->label(*ref.list) : "";
		if (ref.parent && show_list) {
			where += " › " + ref.parent->title;
		}
		if (!where.empty()) {
			auto* where_label = label(where, {"caption", "dim-label"});
			gtk_label_set_ellipsize(GTK_LABEL(where_label),
									PANGO_ELLIPSIZE_END);
			add_meta(where_label);
		}
	}
	if (has_meta) {
		append(text, {meta});
	}

	if (!r.notes.empty()) {
		auto* notes = label(rem::first_lines(r.notes, note_lines_),
							{"caption", "dim-label"});
		gtk_label_set_wrap(GTK_LABEL(notes), TRUE);
		if (note_lines_ > 0) {
			// A long line wraps to at most as many lines again (GTK's limit
			// is per line of text, not for the whole label).
			gtk_label_set_ellipsize(GTK_LABEL(notes), PANGO_ELLIPSIZE_END);
			gtk_label_set_lines(GTK_LABEL(notes),
								static_cast<int>(note_lines_));
		}
		append(text, {notes});
	}

	append(box, {check, text});

	// Disclosure button for a reminder's subtasks (list views only).
	if (view_.kind == View::List && !ref.parent && !r.subtasks.empty()) {
		bool collapsed = collapsed_.contains(id);
		auto* toggle = gtk_button_new();
		auto* content = hbox(4);
		if (collapsed) {
			// How many would appear when expanded.
			auto n = std::ranges::count_if(r.subtasks, [this](auto& s) {
				return show_completed_ || !s.done;
			});
			if (n > 0) {
				append(content, {label(std::to_string(n),
									   {"caption", "dim-label", "numeric"})});
			}
		}
		append(content,
			   {icon(collapsed ? "pan-end-symbolic" : "pan-down-symbolic")});
		gtk_button_set_child(GTK_BUTTON(toggle), content);
		gtk_widget_add_css_class(toggle, "flat");
		gtk_widget_add_css_class(toggle, "subtask-toggle");
		gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(
			toggle, collapsed ? "Show Subtasks" : "Hide Subtasks");
		on(toggle, "clicked",
		   [this, id] { idle([this, id] { toggle_subtasks(id); }); });
		append(box, {toggle});
	}

	// The flag: shown when flagged, else on hover (dimmed); clicking it
	// flags or unflags the row (with the selection, when it's part of it).
	auto* flag = gtk_button_new_from_icon_name("sr-flag-symbolic");
	gtk_widget_add_css_class(flag, "flat");
	gtk_widget_add_css_class(flag, "circular");
	gtk_widget_add_css_class(flag, r.flagged ? "flag-icon" : "row-button");
	if (!r.flagged) {
		gtk_widget_add_css_class(flag, "flag-off");
	}
	gtk_widget_set_valign(flag, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(flag, r.flagged ? "Unflag" : "Flag");
	on(flag, "clicked",
	   [this, id] { idle([this, ids = targets(id)] { toggle_flag(ids); }); });
	append(box, {flag});

	auto* details = gtk_button_new_from_icon_name("document-edit-symbolic");
	gtk_widget_add_css_class(details, "flat");
	gtk_widget_add_css_class(details, "circular");
	gtk_widget_add_css_class(details, "row-button");
	gtk_widget_set_valign(details, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(details, "Details");
	on(details, "clicked", [this, id] { show_details(id); });

	// Per-row actions: the "more" menu, the context menu and keyboard
	// shortcuts. Most act on targets(id): the selection, when this row is
	// part of it.
	bool in_list = view_.kind == View::List;
	bool can_indent = in_list && !ref.parent && r.subtasks.empty(),
		 can_outdent = in_list && ref.parent;
	auto* actions = g_simple_action_group_new();
	add_action(actions, "details", [this, id] { show_details(id); });
	add_action(actions, "complete", [this, id] {
		focus_reminder_ = id;
		idle([this, ids = targets(id)] { complete_reminders(ids); });
	});
	add_action(actions, "flag", [this, id] {
		idle([this, ids = targets(id)] { toggle_flag(ids); });
	});
	add_action(actions, "copy", [this, id] { copy_reminders(targets(id)); });
	add_action(actions, "due-today", [this, id] {
		idle([this, ids = targets(id)] { set_due(ids, 0); });
	});
	add_action(actions, "due-tomorrow", [this, id] {
		idle([this, ids = targets(id)] { set_due(ids, 1); });
	});
	add_action(actions, "delete", [this, id] {
		idle([this, ids = targets(id)] { delete_reminders(ids); });
	});
	auto* indent_action = add_action(actions, "indent", [this, id] {
		idle([this, id] { indent(id, true); });
	});
	auto* outdent_action = add_action(actions, "outdent", [this, id] {
		idle([this, id] { indent(id, false); });
	});
	g_simple_action_set_enabled(indent_action, can_indent);
	g_simple_action_set_enabled(outdent_action, can_outdent);
	{
		auto* move_to = g_simple_action_new("move-to", G_VARIANT_TYPE_STRING);
		connect<void(GSimpleAction*, GVariant*)>(
			move_to, "activate", [this, id](GSimpleAction*, GVariant* v) {
				idle([this, ids = outermost(targets(id)),
					  key = std::string(g_variant_get_string(v, nullptr))] {
					move_to_list(ids, key);
				});
			});
		g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(move_to));
		g_object_unref(move_to);
	}
	gtk_widget_insert_action_group(row, "reminder", G_ACTION_GROUP(actions));
	// Kept for the context menu, whose popover isn't inside the row.
	g_object_set_data_full(G_OBJECT(row), "reminder-actions", actions,
						   g_object_unref);

	// The menu is made as it opens, for what it will act on then.
	auto* more = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
	using MakeMenu = std::function<void(GtkMenuButton*)>;
	gtk_menu_button_set_create_popup_func(
		GTK_MENU_BUTTON(more),
		[](GtkMenuButton* b, gpointer d) { (*static_cast<MakeMenu*>(d))(b); },
		new MakeMenu([this, id, in_list](GtkMenuButton* b) {
			auto menu = Obj<GMenuModel>::adopt(reminder_menu(id, in_list));
			gtk_menu_button_set_menu_model(b, menu.get());
			select_for_menu(id, gtk_menu_button_get_popover(b));
		}),
		[](gpointer d) { delete static_cast<MakeMenu*>(d); });
	gtk_widget_add_css_class(more, "flat");
	gtk_widget_add_css_class(more, "circular");
	gtk_widget_add_css_class(more, "row-button");
	gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(more, "More");
	append(box, {details, more});

	// Right-click and long-press anywhere on the row open the same menu,
	// where it was clicked; on a row outside the selection, for that row
	// only. Caught before the title, so it doesn't start editing (a title
	// being edited keeps its own text menu).
	auto* click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
								  GDK_BUTTON_SECONDARY);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
											   GTK_PHASE_CAPTURE);
	connect<void(GtkGestureClick*, int, double, double)>(
		click, "pressed",
		[this, id, row, title, in_list](GtkGestureClick* g, int, double x,
										double y) {
			if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) {
				return;
			}
			gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
			reminder_context_menu(row, id, in_list, x, y);
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
	auto* press = gtk_gesture_long_press_new();
	connect<void(GtkGestureLongPress*, double, double)>(
		press, "pressed",
		[this, id, row, title, in_list](GtkGestureLongPress*, double x,
										double y) {
			if (!gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) {
				reminder_context_menu(row, id, in_list, x, y);
			}
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(press));

	// Selecting: Ctrl+click adds or removes the row, Shift+click selects up
	// to it (Ctrl+Shift+click adds that range), anywhere on the row, before
	// its title or buttons see the click. A plain click outside the
	// selection clears it; on a selected row, when released without
	// dragging the selection (and not on its menu or Details buttons).
	auto* pick = gtk_gesture_click_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(pick),
											   GTK_PHASE_CAPTURE);
	auto modifiers = [](GtkGestureClick* g) {
		return gtk_event_controller_get_current_event_state(
				   GTK_EVENT_CONTROLLER(g)) &
			   gtk_accelerator_get_default_mod_mask();
	};
	connect<void(GtkGestureClick*, int, double, double)>(
		pick, "pressed",
		[this, id, row, modifiers](GtkGestureClick* g, int, double, double) {
			auto mods = modifiers(g);
			if (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
				gtk_gesture_set_state(GTK_GESTURE(g),
									  GTK_EVENT_SEQUENCE_CLAIMED);
				if (mods & GDK_SHIFT_MASK) {
					select_range(id, mods & GDK_CONTROL_MASK);
				} else {
					toggle_selected(id);
				}
				gtk_widget_grab_focus(row);
				return;
			}
			if (!selection_.contains(id)) {
				clear_selection();
			}
			selection_.set_anchor(id);
		});
	// A plain click (not a drag) on the row's empty space selects just this
	// row; on its title (which starts editing) or circle, nothing stays
	// selected; its ⋮ and Details buttons leave the selection as it is.
	connect<void(GtkGestureClick*, int, double, double)>(
		pick, "released",
		[this, id, row, modifiers](GtkGestureClick* g, int, double x,
								   double y) {
			if (modifiers(g) & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
				return;
			}
			for (auto* w = gtk_widget_pick(row, x, y, GTK_PICK_DEFAULT);
				 w && w != row; w = gtk_widget_get_parent(w)) {
				if (GTK_IS_BUTTON(w) || GTK_IS_MENU_BUTTON(w)) {
					return;
				}
				if (GTK_IS_CHECK_BUTTON(w) || GTK_IS_EDITABLE_LABEL(w)) {
					return clear_selection();
				}
			}
			selection_.select_only(id);
			update_selection();
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(pick));

	// Clicking a row's empty space focuses it, so its keyboard shortcuts apply.
	auto* select_click = gtk_gesture_click_new();
	connect<void(GtkGestureClick*, int, double, double)>(
		select_click, "pressed",
		[row](GtkGestureClick*, int, double x, double y) {
			for (auto* w = gtk_widget_pick(row, x, y, GTK_PICK_DEFAULT);
				 w && w != row; w = gtk_widget_get_parent(w)) {
				if (GTK_IS_BUTTON(w) || GTK_IS_CHECK_BUTTON(w) ||
					GTK_IS_EDITABLE_LABEL(w) || GTK_IS_MENU_BUTTON(w)) {
					return;	 // that widget handles the click itself
				}
			}
			gtk_widget_grab_focus(row);
		});
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(select_click));

	auto* focus = gtk_event_controller_focus_new();
	connect<void(GtkEventControllerFocus*)>(
		focus, "enter", [this, id](GtkEventControllerFocus*) {
			cursor_ = id;
			if (follow_focus_) {  // moved here with ↑/↓ from a selection
				selection_.select_only(id);
				update_selection();
			}
		});
	gtk_widget_add_controller(row, focus);

	auto* keys = gtk_event_controller_key_new();
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		keys, "key-pressed",
		[this, id, title, check, in_list](GtkEventControllerKey*, guint keyval,
										  guint,
										  GdkModifierType mods) -> gboolean {
			if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) {
				return FALSE;
			}
			auto mask = mods & gtk_accelerator_get_default_mod_mask();
			auto key = gdk_keyval_to_lower(keyval);
			auto later = [](std::function<void()> f) {
				idle(std::move(f));	 // rebuilding destroys this row: not from
									 // inside its handler
				return TRUE;
			};
			if (mask == 0) {
				switch (key) {
					case GDK_KEY_Delete:
					case GDK_KEY_KP_Delete:
						return later([this, ids = targets(id)] {
							delete_reminders(ids);
						});
					case GDK_KEY_space:
						if (selection_.contains(id)) {
							focus_reminder_ = id;
							return later([this, ids = targets(id)] {
								complete_reminders(ids);
							});
						}
						gtk_check_button_set_active(
							GTK_CHECK_BUTTON(check),
							!gtk_check_button_get_active(
								GTK_CHECK_BUTTON(check)));
						return TRUE;
					case GDK_KEY_Return:
					case GDK_KEY_KP_Enter:
					case GDK_KEY_F2:
						gtk_editable_label_start_editing(
							GTK_EDITABLE_LABEL(title));
						return TRUE;
					case GDK_KEY_Up:
					case GDK_KEY_Down:
						// With a selection, the row the focus moves to becomes
						// the selection (none, if it's a New Reminder row).
						if (!selection_.empty()) {
							clear_selection();
							follow_focus_ = true;
							idle([this, id] {
								// Nowhere to go (the top or bottom): it stays
								// selected.
								if (follow_focus_ && cursor_ == id &&
									reminder_rows_.contains(id)) {
									selection_.select_only(id);
									update_selection();
								}
								follow_focus_ = false;
							});
						}
						break;
				}
			} else if (mask == GDK_SHIFT_MASK &&
					   (key == GDK_KEY_Up || key == GDK_KEY_Down)) {
				extend_selection(id, key == GDK_KEY_Up);
				return TRUE;
			} else if (mask == GDK_CONTROL_MASK) {
				switch (key) {
					case GDK_KEY_t:
						return later(
							[this, ids = targets(id)] { set_due(ids, 0); });
					case GDK_KEY_i:
						return later([this, id] { show_details(id); });
					case GDK_KEY_c:
						copy_reminders(targets(id));
						return TRUE;
					case GDK_KEY_bracketright:
						if (in_list) {
							return later([this, id] { indent(id, true); });
						}
						break;
					case GDK_KEY_bracketleft:
						if (in_list) {
							return later([this, id] { indent(id, false); });
						}
						break;
				}
			} else if (mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
					   key == GDK_KEY_f) {
				return later([this, ids = targets(id)] { toggle_flag(ids); });
			} else if (mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
					   key == GDK_KEY_t) {
				return later([this, ids = targets(id)] { set_due(ids, 1); });
			} else if (mask == GDK_ALT_MASK && key >= GDK_KEY_0 &&
					   key <= GDK_KEY_3) {
				auto p = static_cast<rem::Priority>(key - GDK_KEY_0);
				return later(
					[this, ids = targets(id), p] { set_priority(ids, p); });
			} else if (mask == GDK_ALT_MASK && in_list &&
					   (key == GDK_KEY_Up || key == GDK_KEY_Down)) {
				bool up = key == GDK_KEY_Up;
				return later([this, id, up] { move_step(id, up); });
			}
			return FALSE;
		});
	gtk_widget_add_controller(row, keys);

	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

	if (focus_reminder_ == id) {
		focus_reminder_.reset();
		auto keep = Obj<GtkWidget>::ref(row);
		idle([keep] { gtk_widget_grab_focus(keep.get()); });
	}

	if (!first_row_) {
		first_row_ = row;
	}
	// Dragging a selected row drags the whole selection.
	make_draggable(
		row, [this, id] { return outermost(targets(id)); },
		[this](const Ids& ids, bool on) {
			for (auto& i : ids) {
				if (auto it = reminder_rows_.find(i);
					it != reminder_rows_.end()) {
					if (on) {
						gtk_widget_add_css_class(it->second, "dragging");
					} else {
						gtk_widget_remove_css_class(it->second, "dragging");
					}
				}
			}
		},
		[this](const Ids& ids) { return reminders_text(ids); });
	if (view_.kind == View::List) {
		make_drop_target(row, DropStyle::Halves, id,
						 [this, id](Ids dropped, rem::Document::Place place) {
							 move_reminders(dropped, id, place);
						 });
		make_text_drop_target(row, DropStyle::Halves,
							  [this, id](std::optional<std::string> text,
										 rem::Document::Place place) {
								  if (!text) {
									  toast("Couldn't read the dropped text");
								  } else {
									  add_text(*text, "Drop", {}, id, place);
								  }
							  });
	}
	return row;
}

void Window::reminder_context_menu(GtkWidget* row, const std::string& id,
								   bool in_list, double x, double y) {
	// The menu belongs to the invisible button over the content (rows are
	// rebuilt, taking their popovers with them); the row's actions go along.
	auto* button = GTK_MENU_BUTTON(content_menu_button_);
	gtk_widget_insert_action_group(
		content_menu_button_, "reminder",
		G_ACTION_GROUP(g_object_get_data(G_OBJECT(row), "reminder-actions")));
	auto menu = Obj<GMenuModel>::adopt(reminder_menu(id, in_list));
	gtk_menu_button_set_menu_model(button, menu.get());
	auto* popover = GTK_POPOVER(gtk_menu_button_get_popover(button));
	graphene_point_t in_row{static_cast<float>(x), static_cast<float>(y)},
		point{};
	if (!gtk_widget_compute_point(row, content_menu_button_, &in_row, &point)) {
		point = in_row;
	}
	GdkRectangle at{static_cast<int>(point.x), static_cast<int>(point.y), 1, 1};
	gtk_popover_set_pointing_to(popover, &at);
	gtk_popover_set_has_arrow(popover, FALSE);
	select_for_menu(id, popover);
	gtk_menu_button_popup(button);
}

// A click in the window on `hit` (before it sees the click).
void Window::clicked(GtkWidget* hit, bool modified) {
	auto inside = [hit](GtkWidget* w) {
		return hit && (hit == w || gtk_widget_is_ancestor(hit, w));
	};
	for (auto* f = gtk_root_get_focus(GTK_ROOT(window_)); f;
		 f = gtk_widget_get_parent(f)) {
		if (GTK_IS_EDITABLE_LABEL(f)) {
			if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(f)) &&
				!inside(f)) {
				gtk_editable_label_stop_editing(GTK_EDITABLE_LABEL(f),
												TRUE);	// keeps the text
			}
			break;
		}
	}
	for (auto* w = hit; w; w = gtk_widget_get_parent(w)) {
		if (GTK_IS_LIST_BOX_ROW(w) &&
			g_object_get_data(G_OBJECT(w), "reminder-id")) {
			return;	 // its own rules
		}
	}
	if (!modified) {
		clear_selection();
	}
}

void Window::select_for_menu(const std::string& id, GtkPopover* popover) {
	if (selection_.contains(id)) {
		return;	 // the menu acts on the selection
	}
	selection_.select_only(id);
	menu_selected_ = id;
	update_selection();
	if (!popover || g_object_get_data(G_OBJECT(popover), "unselects")) {
		return;
	}
	g_object_set_data(G_OBJECT(popover), "unselects",
					  GINT_TO_POINTER(1));	// connected once per popover
	connect<void(GtkPopover*)>(popover, "closed", [this](GtkPopover*) {
		if (menu_selected_ && selection_.size() == 1 &&
			selection_.contains(*menu_selected_)) {
			clear_selection();
		}
		menu_selected_.reset();
	});
}

// A reminder's ⋮ / right-click menu, for targets(id) as it opens: one
// reminder, or the selection it's part of.
GMenuModel* Window::reminder_menu(const std::string& id, bool in_list) {
	auto ids = targets(id);
	bool several = ids.size() > 1;
	bool all_flagged = std::ranges::all_of(ids, [this](auto& i) {
		auto ref = store_->find(i);
		return !ref || ref->reminder->flagged;
	});
	bool all_done = std::ranges::all_of(ids, [this](auto& i) {
		auto ref = store_->find(i);
		return !ref || ref->reminder->done;
	});
	// Menu items show their shortcut.
	auto item = [](GMenu* m, const std::string& text, const char* action,
				   const char* accel) {
		auto* i = g_menu_item_new(text.c_str(), action);
		if (accel) {
			g_menu_item_set_attribute(i, "accel", "s", accel);
		}
		g_menu_append_item(m, i);
		g_object_unref(i);
	};
	auto* menu = g_menu_new();
	item(menu, all_done ? "Mark as Not _Completed" : "Mark as _Completed",
		 "reminder.complete", "space");
	if (!several) {
		item(menu, "_Details…", "reminder.details", "<Control>i");
	}
	item(menu, all_flagged ? "_Unflag" : "_Flag", "reminder.flag",
		 "<Control><Shift>f");
	auto* dates = menu_section(menu);
	item(dates, "Due _Today", "reminder.due-today", "<Control>t");
	item(dates, "Due To_morrow", "reminder.due-tomorrow", "<Control><Shift>t");
	// Move To: every list but the one they're all in already.
	std::set<rem::ListFile*> in;
	for (auto& i : ids) {
		if (auto ref = store_->find(i)) {
			in.insert(ref->list);
		}
	}
	auto* lists = g_menu_new();
	for (auto* l : store_->lists()) {
		if (in.size() == 1 && in.contains(l)) {
			continue;
		}
		auto* i = g_menu_item_new(store_->label(*l).c_str(), nullptr);
		g_menu_item_set_action_and_target_value(
			i, "reminder.move-to",
			g_variant_new_string(store_->key_of(*l).c_str()));
		g_menu_append_item(lists, i);
		g_object_unref(i);
	}
	auto* place = menu_section(menu);
	item(place, "_Copy", "reminder.copy", "<Control>c");
	if (g_menu_model_get_n_items(G_MENU_MODEL(lists)) > 0) {
		g_menu_append_submenu(place, "_Move To", G_MENU_MODEL(lists));
	}
	g_object_unref(lists);
	if (in_list && !several) {
		auto* structure = menu_section(menu);
		item(structure, "_Indent", "reminder.indent", "<Control>bracketright");
		item(structure, "_Outdent", "reminder.outdent", "<Control>bracketleft");
	}
	item(menu_section(menu),
		 several ? std::format("_Delete {} Reminders", ids.size()) : "_Delete",
		 "reminder.delete", "Delete");
	return G_MENU_MODEL(menu);
}

}  // namespace ui
