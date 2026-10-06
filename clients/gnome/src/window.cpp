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

Window* Window::create(AdwApplication* app,
					   std::optional<std::filesystem::path> folder) {
	auto* w = new Window(app, std::move(folder));
	attach(w->window_, "ui-window", std::unique_ptr<Window>(w));
	return w;
}

Window* Window::from(GtkWindow* window) {
	return window ? static_cast<Window*>(
						g_object_get_data(G_OBJECT(window), "ui-window"))
				  : nullptr;
}

// Opens settings.ini in the default app for text files, creating it (with
// the [general] line its settings go under) if needed. Saving it there is
// picked up by watch_settings().
void Window::open_settings() {
	auto path = rem::settings_file();
	try {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream(path) << "[general]\n";
		}
	} catch (const std::exception& e) {
		toast(std::format("Couldn't create the settings file: {}", e.what()));
		return;
	}
	if (!settings_monitor_) {
		watch_settings();
	}
	auto file = Obj<GFile>::adopt(g_file_new_for_path(path.c_str()));
	auto* launcher = gtk_file_launcher_new(file.get());
	auto keep = Obj<GtkWindow>::ref(GTK_WINDOW(window_));
	gtk_file_launcher_launch(
		launcher, GTK_WINDOW(window_), nullptr,
		[](GObject* source, GAsyncResult* result, gpointer data) {
			auto* holder = static_cast<Obj<GtkWindow>*>(data);
			GError* error = nullptr;
			if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source),
												 result, &error)) {
				bool dismissed = g_error_matches(error, GTK_DIALOG_ERROR,
												 GTK_DIALOG_ERROR_DISMISSED);
				if (auto* self = Window::from(holder->get());
					self && !dismissed) {
					self->toast(std::format(
						"Couldn't open the settings file: {}", error->message));
				}
				g_error_free(error);
			}
			delete holder;
		},
		new Obj<GtkWindow>(std::move(keep)));
	g_object_unref(launcher);
}

// Re-reads settings.ini when it changes (an editor, the TUI's S, or this app),
// a moment after the last change.
void Window::watch_settings() {
	auto file =
		Obj<GFile>::adopt(g_file_new_for_path(rem::settings_file().c_str()));
	settings_monitor_ = Obj<GFileMonitor>::adopt(g_file_monitor_file(
		file.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr));
	if (!settings_monitor_) {
		return;
	}
	settings_handler_ =
		connect<void(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent)>(
			settings_monitor_.get(), "changed",
			[this](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent) {
				if (settings_timer_) {
					g_source_remove(settings_timer_);
				}
				settings_timer_ = timeout(300, [this] {
					settings_timer_ = 0;
					reload_settings();
					return false;
				});
			});
}

// row-buttons=always shows each reminder row's buttons (flag, Details, ⋮)
// all the time, dimmed; hover (the default) only on hover or keyboard focus.
void Window::apply_row_buttons() {
	auto value = rem::load_setting("row-buttons");
	if (value == "always") {
		gtk_widget_add_css_class(window_, "row-buttons-always");
	} else {
		gtk_widget_remove_css_class(window_, "row-buttons-always");
	}
}

void Window::reload_settings() {
	std::string text;
	{
		std::ifstream in(rem::settings_file());
		text.assign(std::istreambuf_iterator<char>(in), {});
	}
	if (text == settings_text_) {
		return;
	}
	settings_text_ = std::move(text);
	apply_row_buttons();
	if (auto lines = rem::load_note_lines(); lines != note_lines_) {
		note_lines_ = lines;
		if (store_) {
			rebuild_content();
		}
	}
	if (sidebar_) {
		sidebar_->reload();
	}
	if (show_hidden_action_) {
		g_simple_action_set_state(
			show_hidden_action_,
			g_variant_new_boolean(rem::load_hidden().show));
	}
	// Sources added, removed or changed (and this isn't a --folder session):
	// open them again.
	if (remember_view_) {
		auto configured = rem::load_sources();
		bool same = store_ && configured.size() == store_->sources().size();
		for (std::size_t i = 0; same && i < configured.size(); ++i) {
			auto& open = store_->sources()[i].config;
			same = configured[i].name == open.name &&
				   configured[i].folder == open.folder &&
				   configured[i].backend == open.backend &&
				   configured[i].title == open.title;
		}
		if (!same) {
			return open_sources();
		}
	}
	if (!store_) {
		return;
	}
	// The view may have just been hidden.
	if (sidebar_ && sidebar_->gone(view_)) {
		return select(home_view());
	}
	// Rebuilding replaces the sidebar's rows: keep keyboard focus on the same
	// one.
	std::optional<View> focused;
	std::optional<rem::SidebarGroup> focused_heading;
	for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w;
		 w = gtk_widget_get_parent(w)) {
		if (GTK_IS_LIST_BOX_ROW(w) &&
			gtk_widget_get_parent(w) == sidebar_list_) {
			if (auto* v = row_view(GTK_LIST_BOX_ROW(w))) {
				focused = *v;
			} else {
				focused_heading = row_group(GTK_LIST_BOX_ROW(w));
			}
			break;
		}
	}
	rebuild_sidebar();
	if (!focused && !focused_heading) {
		return;
	}
	for (int i = 0;; ++i) {
		auto* row =
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list_), i);
		if (!row) {
			break;
		}
		auto* v = row_view(row);
		if (focused ? v && *v == *focused
					: !v && row_group(row) == focused_heading) {
			gtk_widget_grab_focus(GTK_WIDGET(row));
			break;
		}
	}
}

void Window::show_reminder(const std::string& id) {
	if (!store_) {
		return;
	}
	auto ref = store_->find(id);
	if (!ref) {
		return;
	}
	select(View{View::List, store_->key_of(*ref->list)});
	show_content();
	show_details(id);
}

Window::Window(AdwApplication* app, std::optional<std::filesystem::path> folder)
	: app_(app) {
	build();
	add_actions();
	// Ctrl+A and Escape for the reminder selection, wherever the focus is in
	// the window: caught before the focused widget sees them, except by text
	// being typed (a title, New Reminder, search), menus and dialogs, which
	// keep their own.
	auto* selection_keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(selection_keys,
											   GTK_PHASE_CAPTURE);
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		selection_keys, "key-pressed",
		[this](GtkEventControllerKey*, guint keyval, guint,
			   GdkModifierType mods) -> gboolean {
			if (!store_ || adw_application_window_get_visible_dialog(
							   ADW_APPLICATION_WINDOW(window_))) {
				return FALSE;
			}
			if (auto* f = gtk_root_get_focus(GTK_ROOT(window_));
				f && (GTK_IS_TEXT(f) || GTK_IS_TEXT_VIEW(f) ||
					  gtk_widget_get_ancestor(f, GTK_TYPE_POPOVER))) {
				return FALSE;
			}
			auto mask = mods & gtk_accelerator_get_default_mod_mask();
			if (mask == GDK_CONTROL_MASK &&
				gdk_keyval_to_lower(keyval) == GDK_KEY_a &&
				!shown_ids_.empty()) {
				select_all();
				return TRUE;
			}
			// Escape, or the HIG's Deselect All (Shift+Ctrl+A).
			bool deselect = (mask == 0 && keyval == GDK_KEY_Escape) ||
							(mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) &&
							 gdk_keyval_to_lower(keyval) == GDK_KEY_a);
			if (deselect && !selection_.empty()) {
				clear_selection();
				return TRUE;
			}
			return FALSE;
		});
	gtk_widget_add_controller(window_, selection_keys);
	// A click outside a title being edited finishes the edit (keeping the
	// text, as Enter does); a click outside every reminder row clears the
	// selection. Seen before the widget clicked (which still gets it);
	// clicks in menus and dialogs are left alone.
	auto* outside = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(outside),
								  0);  // any button
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(outside),
											   GTK_PHASE_CAPTURE);
	connect<void(GtkGestureClick*, int, double, double)>(
		outside, "pressed",
		[this](GtkGestureClick* g, int, double x, double y) {
			if (!store_ || adw_application_window_get_visible_dialog(
							   ADW_APPLICATION_WINDOW(window_))) {
				return;
			}
			auto* event =
				gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(g));
			if (!event || gdk_event_get_surface(event) !=
							  gtk_native_get_surface(GTK_NATIVE(window_))) {
				return;	 // a menu
			}
			auto mods = gtk_event_controller_get_current_event_state(
							GTK_EVENT_CONTROLLER(g)) &
						gtk_accelerator_get_default_mod_mask();
			clicked(gtk_widget_pick(window_, x, y, GTK_PICK_DEFAULT),
					mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK));
		});
	gtk_widget_add_controller(window_, GTK_EVENT_CONTROLLER(outside));
	// A file dropped anywhere else is imported, into the list in view.
	make_file_drop_target(
		window_, false, [this](std::vector<std::filesystem::path> files) {
			if (!store_) {
				return;
			}
			import_files(std::move(files),
						 view_.kind == View::List ? view_.name : std::string());
		});
	// Text from another app dropped anywhere else becomes reminders, at the
	// end of the list in view (in a smart list, as pasting does).
	make_text_drop_target(
		window_, DropStyle::Above,
		[this](std::optional<std::string> text, rem::Document::Place) {
			if (!text) {
				toast("Couldn't read the dropped text");
			} else if (store_) {
				add_text(*text, "Drop", {}, std::nullopt,
						 rem::Document::Place::After);
			}
		});
	{
		std::ifstream in(rem::settings_file());	 // as read at start-up
		settings_text_.assign(std::istreambuf_iterator<char>(in), {});
	}
	watch_settings();
	open_sources(folder);
	last_notify_check_ = g_get_real_time() / G_USEC_PER_SEC;
	notify_timer_ = timeout(30'000, [this] {
		check_notifications();
		return true;
	});
}

Window::~Window() {
	stop_sync();
	for (auto id : {reload_timer_, refresh_timer_, notify_timer_,
					autoscroll_timer_, settings_timer_}) {
		if (id) {
			g_source_remove(id);
		}
	}
	if (settings_monitor_) {
		g_signal_handler_disconnect(settings_monitor_.get(), settings_handler_);
		g_file_monitor_cancel(settings_monitor_.get());
	}
	stop_watching();
}

void Window::build() {
	window_ = adw_application_window_new(GTK_APPLICATION(app_));
	gtk_window_set_title(GTK_WINDOW(window_), "Reminders");
	gtk_window_set_icon_name(GTK_WINDOW(window_), kAppId);
	gtk_window_set_default_size(GTK_WINDOW(window_), 900, 640);
	gtk_widget_set_size_request(window_, 360, 300);

	// Welcome page, shown until a folder is chosen.
	auto* welcome_status = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(welcome_status), kAppId);
	adw_status_page_set_title(ADW_STATUS_PAGE(welcome_status),
							  "Welcome to Reminders");
	adw_status_page_set_description(
		ADW_STATUS_PAGE(welcome_status),
		"Choose the folder that Syncthing keeps in sync. Each list is stored "
		"there as a Markdown file "
		"you can also open in any text editor.");
	auto* choose = gtk_button_new_with_mnemonic("_Choose Folder…");
	gtk_widget_add_css_class(choose, "pill");
	gtk_widget_add_css_class(choose, "suggested-action");
	gtk_widget_set_halign(choose, GTK_ALIGN_CENTER);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(choose), "win.change-folder");
	adw_status_page_set_child(ADW_STATUS_PAGE(welcome_status), choose);
	auto* welcome = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(welcome),
								 adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(welcome), welcome_status);

	// Sidebar.
	auto* primary_menu = g_menu_new();
	auto* s1 = menu_section(primary_menu);
	g_menu_append(s1, "_New List…", "win.new-list");
	g_menu_append(s1, "_Import…", "win.import");
	g_menu_append(s1, "_Export…", "win.export");
	g_menu_append(s1, "S_ources…", "win.sources");
	auto* sync_item = g_menu_item_new("S_ync All", "win.sync-all");
	g_menu_item_set_attribute(sync_item, "accel", "s", "<Control><Shift>s");
	g_menu_item_set_attribute(
		sync_item, "hidden-when", "s",
		"action-disabled");	 // no CalDAV, WebDAV or git sources
	g_menu_append_item(s1, sync_item);
	g_object_unref(sync_item);
	g_menu_append(menu_section(primary_menu), "Show _Hidden Lists",
				  "win.show-hidden");
	auto* s2 = menu_section(primary_menu);
	g_menu_append(s2, "_Settings…", "win.settings");
	g_menu_append(s2, "_Keyboard Shortcuts", "app.shortcuts");
	g_menu_append(s2, "_About Reminders", "app.about");
	auto* menu_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
								  "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
								   G_MENU_MODEL(primary_menu));
	gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
	gtk_widget_set_tooltip_text(menu_button, "Main Menu");
	g_object_unref(primary_menu);

	auto* search_toggle = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(search_toggle), "edit-find-symbolic");
	gtk_widget_set_tooltip_text(search_toggle, "Search");

	auto* sidebar_header = adw_header_bar_new();
	adw_header_bar_pack_start(ADW_HEADER_BAR(sidebar_header), search_toggle);
	adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), menu_button);

	search_entry_ = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry_),
										  "Search Reminders");
	search_bar_ = gtk_search_bar_new();
	gtk_search_bar_set_child(GTK_SEARCH_BAR(search_bar_), search_entry_);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(search_bar_),
								 GTK_EDITABLE(search_entry_));
	gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(search_bar_), window_);
	g_object_bind_property(
		search_toggle, "active", search_bar_, "search-mode-enabled",
		GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
	// Enter (or ↓, or Tab) moves into the results, to go through them with
	// the arrow keys.
	on(search_entry_, "activate", [this] { focus_results(); });
	auto* search_keys = gtk_event_controller_key_new();
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		search_keys, "key-pressed",
		[this](GtkEventControllerKey*, guint key, guint,
			   GdkModifierType mods) -> gboolean {
			if ((key != GDK_KEY_Down && key != GDK_KEY_Tab &&
				 key != GDK_KEY_KP_Tab) ||
				(mods & gtk_accelerator_get_default_mod_mask())) {
				return FALSE;
			}
			// With nothing to go to (no text, no matches), Tab moves on
			// as usual.
			return focus_results() || key == GDK_KEY_Down;
		});
	gtk_widget_add_controller(search_entry_, search_keys);
	on(search_entry_, "search-changed", [this] {
		if (updating_sidebar_) {
			return;	 // cleared by select()
		}
		auto text = trim(gtk_editable_get_text(GTK_EDITABLE(search_entry_)));
		// Already showing these results (Enter got there first): don't rebuild
		// them, which would take focus away from the selected result.
		if (!text.empty() && view_ == View{View::Search, text}) {
			return;
		}
		if (!text.empty()) {
			select(View{View::Search, text});
		} else if (view_.kind == View::Search) {
			select(home_view());
		}
	});

	sidebar_list_ = gtk_list_box_new();
	gtk_widget_add_css_class(sidebar_list_, "navigation-sidebar");
	connect<void(GtkListBox*, GtkListBoxRow*)>(
		sidebar_list_, "row-activated",
		[this](GtkListBox*, GtkListBoxRow* row) {
			if (updating_sidebar_) {
				return;
			}
			if (g_object_get_data(G_OBJECT(row), "fold-group")) {
				if (auto g = row_group(row)) {
					toggle_fold(*g);
				}
				return;
			}
			if (auto* v = row_view(row)) {
				select(*v);
				show_content();
			}
		});
	// Enter on an entry also moves the focus into its reminders (a click
	// leaves it be). Caught before the row's own Enter, which activates it.
	auto* sidebar_enter = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(sidebar_enter,
											   GTK_PHASE_CAPTURE);
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		sidebar_enter, "key-pressed",
		[this](GtkEventControllerKey*, guint key, guint,
			   GdkModifierType mods) -> gboolean {
			if ((key != GDK_KEY_Return && key != GDK_KEY_KP_Enter) ||
				(mods & gtk_accelerator_get_default_mod_mask())) {
				return FALSE;
			}
			auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
			if (!focus || !GTK_IS_LIST_BOX_ROW(focus) || updating_sidebar_) {
				return FALSE;
			}
			auto* v = row_view(GTK_LIST_BOX_ROW(focus));
			if (!v) {
				return FALSE;  // a heading: folds as usual
			}
			auto view = *v;	 // select() can rebuild the sidebar
			select(view);
			show_content();
			focus_content();
			return TRUE;
		});
	gtk_widget_add_controller(sidebar_list_, sidebar_enter);
	// Right-click or long-press: a menu to move the group up or down.
	auto* sidebar_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(sidebar_click),
								  GDK_BUTTON_SECONDARY);
	connect<void(GtkGestureClick*, int, double, double)>(
		sidebar_click, "pressed",
		[this](GtkGestureClick*, int, double x, double y) {
			sidebar_menu(gtk_list_box_get_row_at_y(GTK_LIST_BOX(sidebar_list_),
												   static_cast<int>(y)),
						 x, y);
		});
	gtk_widget_add_controller(sidebar_list_,
							  GTK_EVENT_CONTROLLER(sidebar_click));
	auto* sidebar_press = gtk_gesture_long_press_new();
	connect<void(GtkGestureLongPress*, double, double)>(
		sidebar_press, "pressed",
		[this](GtkGestureLongPress*, double x, double y) {
			sidebar_menu(gtk_list_box_get_row_at_y(GTK_LIST_BOX(sidebar_list_),
												   static_cast<int>(y)),
						 x, y);
		});
	gtk_widget_add_controller(sidebar_list_,
							  GTK_EVENT_CONTROLLER(sidebar_press));
	// Ctrl+↑ / Ctrl+↓ (and with Shift) on a sidebar row move it or its group.
	auto* sidebar_keys = gtk_event_controller_key_new();
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		sidebar_keys, "key-pressed",
		[this](GtkEventControllerKey*, guint key, guint,
			   GdkModifierType mods) -> gboolean {
			// Ctrl+↑/↓ moves the entry within its group (on a heading, the
			// group); Ctrl+Shift+↑/↓ moves the group. (The terminal client
			// uses Alt.)
			auto mask = mods & gtk_accelerator_get_default_mod_mask();
			bool group_keys = mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK);
			if (mask != GDK_CONTROL_MASK && !group_keys) {
				return FALSE;
			}
			if (key != GDK_KEY_Up && key != GDK_KEY_Down) {
				return FALSE;
			}
			auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
			while (focus && !GTK_IS_LIST_BOX_ROW(focus)) {
				focus = gtk_widget_get_parent(focus);
			}
			int delta = key == GDK_KEY_Up ? -1 : 1;
			auto* v = focus ? row_view(GTK_LIST_BOX_ROW(focus)) : nullptr;
			if (v && !group_keys) {
				auto view = *v;
				idle([this, view, delta] { move_entry(view, delta); });
			} else if (auto g = focus ? row_group(GTK_LIST_BOX_ROW(focus))
									  : std::nullopt) {
				auto group = *g;
				idle([this, group, delta] { move_group(group, delta); });
			}
			return TRUE;
		});
	gtk_widget_add_controller(sidebar_list_, sidebar_keys);
	// Ctrl+V outside a text field pastes reminders. Text fields handle it
	// first (this runs as the key bubbles up to the window), so pasting
	// into them works as usual.
	auto* paste_keys = gtk_event_controller_key_new();
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		paste_keys, "key-pressed",
		[this](GtkEventControllerKey*, guint key, guint,
			   GdkModifierType mods) -> gboolean {
			auto mask = mods & gtk_accelerator_get_default_mod_mask();
			if (mask != GDK_CONTROL_MASK &&
				mask != (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
				return FALSE;
			}
			if (gdk_keyval_to_lower(key) != GDK_KEY_v) {
				return FALSE;
			}
			auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
			if (focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus))) {
				return FALSE;
			}
			if (mask & GDK_SHIFT_MASK) {
				paste_special();  // Ctrl+Shift+V
			} else {
				paste_reminders();
			}
			return TRUE;
		});
	gtk_widget_add_controller(window_, paste_keys);
	// Ctrl+S syncs this source, also as the key bubbles up: a title or
	// dialog being edited takes it first, as Save.
	auto* sync_keys = gtk_event_controller_key_new();
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		sync_keys, "key-pressed",
		[this](GtkEventControllerKey*, guint key, guint,
			   GdkModifierType mods) -> gboolean {
			auto mask = mods & gtk_accelerator_get_default_mod_mask();
			if (mask != GDK_CONTROL_MASK ||
				gdk_keyval_to_lower(key) != GDK_KEY_s) {
				return FALSE;
			}
			g_action_activate(G_ACTION(sync_one_action_), nullptr);
			return TRUE;
		});
	gtk_widget_add_controller(window_, sync_keys);
	auto* sidebar_scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroller),
								   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroller),
								  sidebar_list_);

	auto* new_list_button = gtk_button_new();
	auto* new_list_content = adw_button_content_new();
	adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(new_list_content),
									 "list-add-symbolic");
	adw_button_content_set_label(ADW_BUTTON_CONTENT(new_list_content),
								 "New List");
	gtk_button_set_child(GTK_BUTTON(new_list_button), new_list_content);
	gtk_widget_add_css_class(new_list_button, "flat");
	gtk_widget_set_margin_start(new_list_button, 6);
	gtk_widget_set_margin_end(new_list_button, 6);
	gtk_widget_set_margin_top(new_list_button, 6);
	gtk_widget_set_margin_bottom(new_list_button, 6);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(new_list_button),
								   "win.new-list");

	auto* sidebar_view = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebar_view),
								 sidebar_header);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebar_view), search_bar_);
	// An invisible menu button in the corner hosts the sidebar's context menu.
	sidebar_menu_button_ = gtk_menu_button_new();
	gtk_widget_set_halign(sidebar_menu_button_, GTK_ALIGN_START);
	gtk_widget_set_valign(sidebar_menu_button_, GTK_ALIGN_START);
	gtk_widget_set_opacity(sidebar_menu_button_, 0);
	gtk_widget_set_can_target(sidebar_menu_button_, FALSE);
	gtk_widget_set_can_focus(sidebar_menu_button_, FALSE);
	gtk_accessible_update_state(GTK_ACCESSIBLE(sidebar_menu_button_),
								GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	auto* sidebar_overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(sidebar_overlay), sidebar_scroller);
	gtk_overlay_add_overlay(GTK_OVERLAY(sidebar_overlay), sidebar_menu_button_);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebar_view),
								 sidebar_overlay);
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(sidebar_view),
									new_list_button);
	auto* sidebar_page = adw_navigation_page_new(sidebar_view, "Reminders");

	// Content.
	content_title_ = adw_window_title_new("", "");
	auto* list_menu = g_menu_new();
	auto* m1 = menu_section(list_menu);
	g_menu_append(m1, "_Show Completed", "win.show-completed");
	g_menu_append(m1, "Show _All Subtasks", "win.show-all-subtasks");
	g_menu_append(m1, "_Hide All Subtasks", "win.hide-all-subtasks");
	auto* m2 = menu_section(list_menu);
	g_menu_append(m2, "Add _Section…", "win.add-section");
	g_menu_append(m2, "List _Info…", "win.list-info");
	g_menu_append(m2, "_Export…", "win.export-list");
	auto* m3 = menu_section(list_menu);
	g_menu_append(m3, "_Delete List…", "win.delete-list");
	list_menu_button_ = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(list_menu_button_),
								  "view-more-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(list_menu_button_),
								   G_MENU_MODEL(list_menu));
	gtk_widget_set_tooltip_text(list_menu_button_, "List Menu");
	g_object_unref(list_menu);

	new_button_ = gtk_button_new_from_icon_name("list-add-symbolic");
	gtk_widget_set_tooltip_text(new_button_, "New Reminder");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(new_button_),
								   "win.new-reminder");

	auto* content_header = adw_header_bar_new();
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(content_header),
									content_title_);
	auto* sidebar_toggle = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(sidebar_toggle),
							 "sidebar-show-symbolic");
	gtk_widget_set_tooltip_text(sidebar_toggle, "Show Sidebar");
	adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), sidebar_toggle);
	adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), new_button_);
	adw_header_bar_pack_end(ADW_HEADER_BAR(content_header), list_menu_button_);

	content_scroller_ = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(content_scroller_),
								   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(content_scroller_, TRUE);
	setup_autoscroll();
	connect<void(GObject*, GParamSpec*)>(
		gtk_scrolled_window_get_hadjustment(
			GTK_SCROLLED_WINDOW(content_scroller_)),
		"notify::page-size", [this](GObject*, GParamSpec*) {
			// Resizing from inside the layout pass would only take effect a
			// frame late anyway.
			if (!clamp_pending_) {
				clamp_pending_ = true;
				idle([this] {
					clamp_pending_ = false;
					update_clamp();
				});
			}
		});

	banner_ = adw_banner_new("");
	adw_banner_set_button_label(ADW_BANNER(banner_), "_Review…");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(banner_), "win.review-lists");

	auto* content_view = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_view),
								 content_header);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_view), banner_);
	// As over the sidebar, an invisible menu button hosts a reminder's
	// context menu, which opens where the row was clicked.
	content_menu_button_ = gtk_menu_button_new();
	gtk_widget_set_halign(content_menu_button_, GTK_ALIGN_START);
	gtk_widget_set_valign(content_menu_button_, GTK_ALIGN_START);
	gtk_widget_set_opacity(content_menu_button_, 0);
	gtk_widget_set_can_target(content_menu_button_, FALSE);
	gtk_widget_set_can_focus(content_menu_button_, FALSE);
	gtk_accessible_update_state(GTK_ACCESSIBLE(content_menu_button_),
								GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
	auto* content_overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(content_overlay), content_scroller_);
	gtk_overlay_add_overlay(GTK_OVERLAY(content_overlay), content_menu_button_);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_view),
								 content_overlay);
	content_page_ = GTK_WIDGET(adw_navigation_page_new(content_view, "Today"));

	// An overlay split view can hide its sidebar at any width (Ctrl+B); on
	// narrow windows the sidebar slides over the content.
	split_ = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_),
									   GTK_WIDGET(sidebar_page));
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_),
									   content_page_);
	adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_),
												 300);
	g_object_bind_property(
		split_, "show-sidebar", sidebar_toggle, "active",
		GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));

	auto* bp =
		adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 560sp"));
	GValue collapsed = G_VALUE_INIT;
	g_value_init(&collapsed, G_TYPE_BOOLEAN);
	g_value_set_boolean(&collapsed, TRUE);
	adw_breakpoint_add_setter(bp, G_OBJECT(split_), "collapsed", &collapsed);
	g_value_unset(&collapsed);
	GValue hidden = G_VALUE_INIT;
	g_value_init(&hidden, G_TYPE_BOOLEAN);
	g_value_set_boolean(&hidden, FALSE);
	adw_breakpoint_add_setter(bp, G_OBJECT(split_), "show-sidebar", &hidden);
	g_value_unset(&hidden);
	adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window_), bp);

	// Whether the sidebar is shown is saved (show-sidebar), shared with the
	// TUI. Only while the window is wide enough to show it beside the
	// content: on narrow windows it hides by itself and slides over.
	adw_overlay_split_view_set_show_sidebar(
		ADW_OVERLAY_SPLIT_VIEW(split_),
		rem::load_bool_setting("show-sidebar", true));
	apply_row_buttons();
	// ("notify" passes the property as well, so not on(), which is for signals
	// that pass only the emitter.)
	connect<void(GObject*, GParamSpec*)>(
		split_, "notify::show-sidebar", [this](GObject*, GParamSpec*) {
			auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
			if (adw_overlay_split_view_get_collapsed(split)) {
				return;
			}
			bool shown = adw_overlay_split_view_get_show_sidebar(split);
			if (rem::load_bool_setting("show-sidebar", true) == shown) {
				return;
			}
			try {
				rem::save_setting("show-sidebar", shown ? "true" : "false");
			} catch (const std::exception&) {
				// Not worth interrupting for; it just won't be remembered.
			}
		});

	main_stack_ = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(main_stack_), welcome, "welcome");
	gtk_stack_add_named(GTK_STACK(main_stack_), split_, "main");
	gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "welcome");

	toasts_ = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toasts_), main_stack_);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_),
									   toasts_);
}

// Ctrl+S: the source of the focused sidebar row, else of the list shown.
// What spans sources (smart lists, tags, search) syncs them all.
void Window::sync_current() {
	if (!sync_ || !store_) {
		return;
	}
	std::optional<std::string> source;
	auto list_source = [](const View& v) -> std::optional<std::string> {
		if (v.kind != View::List) {
			return std::nullopt;
		}
		return v.name.substr(0, v.name.find('/'));
	};
	auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
	auto* row = focus && gtk_widget_is_ancestor(focus, sidebar_list_)
				  ? gtk_widget_get_ancestor(focus, GTK_TYPE_LIST_BOX_ROW)
				  : nullptr;
	if (row && row_view(GTK_LIST_BOX_ROW(row))) {
		source = list_source(*row_view(GTK_LIST_BOX_ROW(row)));
	} else if (auto g = row ? row_group(GTK_LIST_BOX_ROW(row)) : std::nullopt;
			   g && g->kind == rem::SidebarGroup::Lists) {
		source = g->source;
	} else if (!row) {
		source = list_source(view_);
	}
	if (!source) {
		sync_->sync_now();
		return;
	}
	for (auto& s : store_->sources()) {
		if (s.config.name == *source) {
			if (rem::syncs(s.config)) {
				sync_->sync_now(*source);
			} else {
				toast("“" + *source + "” isn’t synced by Reminders");
			}
			return;
		}
	}
}

void Window::add_actions() {
	add_action(window_, "change-folder", [this] { choose_folder(); });
	add_action(window_, "add-source", [this] { add_source(); });
	add_action(window_, "sources", [this] { show_sources(); });
	add_action(window_, "import", [this] {
		if (store_) {
			import_file();
		}
	});
	sync_action_ = add_action(window_, "sync-all", [this] {
		if (sync_) {
			sync_->sync_now();
		}
	});
	g_simple_action_set_enabled(sync_action_, FALSE);
	sync_one_action_ = add_action(window_, "sync", [this] { sync_current(); });
	g_simple_action_set_enabled(sync_one_action_, FALSE);
	add_action(window_, "settings", [this] { open_settings(); });
	add_action(window_, "go-to", [this] {
		if (store_) {
			quick_switcher();
		}
	});
	add_action(window_, "toggle-sidebar", [this] {
		auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
		adw_overlay_split_view_set_show_sidebar(
			split, !adw_overlay_split_view_get_show_sidebar(split));
	});
	// Ctrl+L: from the sidebar to the reminders, or from anywhere else to
	// the sidebar's selected entry (showing the sidebar if it's hidden).
	add_action(window_, "switch-focus", [this] {
		if (!store_) {
			return;
		}
		auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
		auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
		if (focus && gtk_widget_is_ancestor(focus, sidebar_list_) &&
			adw_overlay_split_view_get_show_sidebar(split)) {
			show_content();
			focus_content();
			return;
		}
		adw_overlay_split_view_set_show_sidebar(split, TRUE);
		auto* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(sidebar_list_));
		if (row) {
			gtk_widget_grab_focus(GTK_WIDGET(row));
		} else {
			gtk_widget_child_focus(sidebar_list_, GTK_DIR_TAB_FORWARD);
		}
	});
	// The list menu's Show / Hide All Subtasks.
	add_action(window_, "show-all-subtasks", [this] {
		collapsed_.clear();
		rebuild_content();
	});
	add_action(window_, "hide-all-subtasks", [this] {
		if (!store_) {
			return;
		}
		for (auto* l : store_->lists()) {
			for (auto* r : l->doc.reminders()) {
				if (!r->subtasks.empty()) {
					collapsed_.insert(r->id);
				}
			}
		}
		rebuild_content();
	});
	// Main menu: show the lists, smart lists and tags hidden from the sidebar
	// (dimmed), so they can be opened or unhidden.
	show_hidden_action_ = add_toggle(
		window_, "show-hidden", rem::load_hidden().show, [this](bool on) {
			try {
				if (sidebar_) {
					sidebar_->set_show_hidden(on);
				}
			} catch (const std::exception& e) {
				toast(std::format("Couldn't save the setting: {}", e.what()));
			}
			if (!on && entry_hidden(view_)) {
				select(home_view());
			}
			rebuild_sidebar();
		});
	add_action(window_, "move-group-up",
			   [this] { move_group(menu_group_, -1); });
	add_action(window_, "move-group-down",
			   [this] { move_group(menu_group_, 1); });
	// The sidebar menu's "Collapsible" check item: visible <-> collapsible.
	collapsible_action_ =
		add_toggle(window_, "group-collapsible", false, [this](bool on) {
			try {
				if (sidebar_) {
					sidebar_->set_foldable(
						menu_group_,
						on);  // a group made collapsible starts unfolded
				}
			} catch (const std::exception& e) {
				toast(std::format("Couldn't save the setting: {}", e.what()));
			}
			rebuild_sidebar();
		});
	add_action(window_, "next-view", [this] { step_view(1); });
	add_action(window_, "previous-view", [this] { step_view(-1); });
	undo_action_ = add_action(window_, "undo", [this] { undo(); });
	redo_action_ = add_action(window_, "redo", [this] { redo(); });
	update_undo_actions();
	add_action(window_, "new-list", [this] {
		if (store_) {
			new_list();
		}
	});
	add_action(window_, "new-reminder", [this] {
		if (first_new_entry_) {
			gtk_widget_grab_focus(first_new_entry_);
		}
	});
	add_action(window_, "search", [this] {
		gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(search_bar_), TRUE);
		adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_),
												TRUE);
		gtk_widget_grab_focus(search_entry_);
	});
	add_action(window_, "review-lists", [this] {
		if (store_) {
			review_candidates();
		}
	});
	add_action(window_, "add-section", [this] {
		if (view_.kind == View::List) {
			add_section(view_.name);
		}
	});
	add_action(window_, "list-info", [this] {
		if (view_.kind == View::List) {
			edit_list(view_.name);
		}
	});
	add_action(window_, "delete-list", [this] {
		if (view_.kind == View::List) {
			delete_list(view_.name);
		}
	});
	add_action(window_, "export",
			   [this] {	 // ☰: the list in view ticked, if any
				   if (store_) {
					   export_lists(view_.kind == View::List
										? std::vector{view_.name}
										: std::vector<std::string>{});
				   }
			   });
	add_action(window_, "export-list", [this] {
		if (view_.kind == View::List) {
			export_lists({view_.name});
		}
	});
	show_completed_action_ =
		add_toggle(window_, "show-completed", false, [this](bool on) {
			show_completed_ = on;
			rebuild_content();
		});
}

void Window::refresh() {
	if (refresh_timer_) {
		g_source_remove(refresh_timer_);
		refresh_timer_ = 0;
	}
	if (!store_) {
		return;
	}
	rebuild_sidebar();
	rebuild_content();
	update_banner();
}

void Window::refresh_later(guint ms) {
	rebuild_sidebar();	// counts update straight away
	if (refresh_timer_) {
		g_source_remove(refresh_timer_);
	}
	refresh_timer_ = timeout(ms, [this] {
		refresh_timer_ = 0;
		rebuild_content();
		return false;
	});
}

void Window::select(View v) {
	if (v.kind != View::Search &&
		gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(search_bar_))) {
		updating_sidebar_ = true;
		gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
		updating_sidebar_ = false;
	}
	if (!(v == view_)) {
		selection_.clear();
		selection_.set_anchor(std::nullopt);
	}
	view_ = std::move(v);
	if (view_.kind != View::Search && remember_view_) {
		save_last_view(view_to_string(view_));
	}
	refresh();
}

void Window::toast(const std::string& text, const char* button,
				   std::function<void()> on_button) {
	auto* t = adw_toast_new(text.c_str());
	if (button) {
		adw_toast_set_button_label(t, button);
		on(t, "button-clicked", std::move(on_button));
	}
	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toasts_), t);
}

// Where to land when there's nothing better: Today, unless it's hidden.
View Window::home_view() {
	return sidebar_ ? sidebar_->home() : View{View::Today, ""};
}

void Window::step_view(int delta) {
	if (!store_) {
		return;
	}
	auto views = sidebar_views();
	auto at = std::ranges::find(views, view_);
	auto n = static_cast<long>(views.size());
	long i = at == views.end() ? 0 : (at - views.begin() + delta + n) % n;
	select(views[static_cast<std::size_t>(i)]);
}

void Window::update_clamp() {
	if (!clamp_) {
		return;
	}
	int width = gtk_widget_get_width(content_scroller_);
	if (width <= 0) {
		width = 860;  // not laid out yet
	}
	int size = static_cast<int>(width * kContentWidthShare);
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp_), size);
	adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp_), size);
}

void Window::show_content() {
	auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
	if (adw_overlay_split_view_get_collapsed(split)) {
		adw_overlay_split_view_set_show_sidebar(split, FALSE);
	}
}

void Window::undo() {
	if (!store_) {
		return;
	}
	auto result = history_.undo(*store_);
	after_history(result);
}

void Window::redo() {
	if (!store_) {
		return;
	}
	auto result = history_.redo(*store_);
	after_history(result);
}

void Window::after_history(const rem::History::Result& result) {
	if (!result.applied) {
		return;
	}
	for (auto& name : result.skipped) {
		toast(std::format(
			"“{}” was changed on another device, so it was left as it is",
			name));
	}
	refresh();
	update_undo_actions();
}

void Window::update_undo_actions() {
	g_simple_action_set_enabled(undo_action_, history_.can_undo());
	g_simple_action_set_enabled(redo_action_, history_.can_redo());
}

void Window::update_banner() {
	auto files = store_->candidates();
	if (store_->sources().size() == 1) {  // just the file names
		for (auto& f : files) {
			f = f.substr(f.find('/') + 1);
		}
	}
	if (files.empty()) {
		adw_banner_set_revealed(ADW_BANNER(banner_), FALSE);
		return;
	}
	auto title =
		files.size() == 1
			? std::format("“{}” has a checklist but isn't a list yet",
						  files.front())
			: std::format("{} Markdown files with checklists aren't lists yet",
						  files.size());
	adw_banner_set_title(ADW_BANNER(banner_), title.c_str());
	adw_banner_set_revealed(ADW_BANNER(banner_), TRUE);
}

void Window::review_candidates() {
	auto files = store_->candidates();
	if (files.empty()) {
		return;
	}
	auto* dialog = adw_alert_dialog_new(
		files.size() == 1 ? "Use as a List?" : "Use as Lists?",
		"Adding a file puts “reminders: 1” at the top of it, so every synced "
		"device shows it as a list. "
		"Files you switch off won't be suggested again.");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "later",
								   "_Not Now", "add", "_Add", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "add",
											 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "add");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "later");

	auto* rows = boxed_list();
	std::vector<std::pair<std::string, GtkWidget*>> switches;
	for (auto& name : files) {
		auto* row = adw_switch_row_new();
		auto shown = store_->sources().size() == 1
					   ? name.substr(name.find('/') + 1)
					   : name;
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
									  (shown + ".md").c_str());
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
		gtk_list_box_append(GTK_LIST_BOX(rows), row);
		switches.emplace_back(name, row);
	}
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), rows);

	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, switches](AdwAlertDialog*, const char* response) {
			if (std::string_view(response) != "add") {
				return;
			}
			for (auto& [name, row] : switches) {
				try {
					if (adw_switch_row_get_active(ADW_SWITCH_ROW(row))) {
						store_->adopt(name);
					} else {
						store_->decline(name);
					}
				} catch (const std::exception& e) {
					toast(std::format("Couldn't add “{}”: {}", name, e.what()));
				}
			}
			refresh();
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::check_notifications() {
	if (!store_) {
		return;
	}
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	for (auto& ref : store_->all()) {
		auto& r = *ref.reminder;
		if (!r.due_date) {
			continue;
		}
		// All-day reminders notify at 09:00, as on iOS.
		auto t = r.due_time.value_or(rem::TimeOfDay{9, 0});
		GDateTime* due = g_date_time_new_local(
			static_cast<int>(r.due_date->year()),
			static_cast<int>(static_cast<unsigned>(r.due_date->month())),
			static_cast<int>(static_cast<unsigned>(r.due_date->day())), t.hour,
			t.minute, 0);
		gint64 at = g_date_time_to_unix(due);
		g_date_time_unref(due);
		if (at <= last_notify_check_ || at > now) {
			continue;
		}
		auto* n =
			g_notification_new(r.title.empty() ? "Reminder" : r.title.c_str());
		auto body = store_->label(*ref.list);
		if (!r.notes.empty()) {
			body += " — " + r.notes.substr(0, r.notes.find('\n'));
		}
		g_notification_set_body(n, body.c_str());
		g_notification_set_default_action_and_target(n, "app.show-reminder",
													 "s", r.id.c_str());
		g_application_send_notification(G_APPLICATION(app_),
										("reminder-" + r.id).c_str(), n);
		g_object_unref(n);
	}
	last_notify_check_ = now;
}

}  // namespace ui
