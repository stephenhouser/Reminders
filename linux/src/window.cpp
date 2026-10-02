#include "window.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "dialogs.hpp"
#include "reminders/format.hpp"
#include "reminders/syncthing.hpp"
#include "support.hpp"

namespace ui {

namespace {

constexpr guint kCompleteDelayMs = 900;  // a checked reminder lingers before it disappears
constexpr double kContentWidthShare = 0.9;  // lists' width as a share of the content area

struct SmartInfo {
    View::Kind kind;
    const char* title;
    const char* icon;
    const char* color;
};

constexpr SmartInfo kSmart[] = {
    {View::Today, "Today", "x-office-calendar-symbolic", "blue"},
    {View::Scheduled, "Scheduled", "alarm-symbolic", "red"},
    {View::All, "All", "view-list-bullet-symbolic", "gray"},
    {View::Flagged, "Flagged", "sr-flag-symbolic", "orange"},
    {View::Completed, "Completed", "object-select-symbolic", "gray"},
};

const SmartInfo* smart_info(View::Kind k) {
    for (auto& s : kSmart)
        if (s.kind == k) return &s;
    return nullptr;
}

std::string color_class(std::string_view color) { return "color-" + std::string(color); }

// Up/Down at the end of one section's list continue into the next one.
void chain_listboxes(const std::vector<GtkWidget*>& boxes) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        GtkWidget* prev = i > 0 ? boxes[i - 1] : nullptr;
        GtkWidget* next = i + 1 < boxes.size() ? boxes[i + 1] : nullptr;
        connect<gboolean(GtkWidget*, GtkDirectionType)>(
            boxes[i], "keynav-failed", [prev, next](GtkWidget*, GtkDirectionType dir) -> gboolean {
                GtkListBoxRow* target = nullptr;
                if (dir == GTK_DIR_DOWN && next) {
                    target = gtk_list_box_get_row_at_index(GTK_LIST_BOX(next), 0);
                } else if (dir == GTK_DIR_UP && prev) {
                    for (int j = 0; auto* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(prev), j); ++j) target = r;
                }
                if (!target) return FALSE;
                gtk_widget_grab_focus(GTK_WIDGET(target));
                return TRUE;
            });
    }
}

// GtkEditableLabel shows its text in a GtkLabel that never wraps, so a long
// title would demand its full width and push the window wider. It doesn't
// expose that label, so find it and let it wrap.
void wrap_editable_label(GtkWidget* editable) {
    std::vector<GtkWidget*> todo{editable};
    while (!todo.empty()) {
        auto* w = todo.back();
        todo.pop_back();
        if (GTK_IS_LABEL(w)) {
            gtk_label_set_wrap(GTK_LABEL(w), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(w), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_xalign(GTK_LABEL(w), 0);
        }
        for (auto* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) todo.push_back(c);
    }
}

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\n");
    return std::string(s.substr(b, e - b + 1));
}

int open_count(rem::ListFile& l) {
    int n = 0;
    l.doc.walk([&](rem::Reminder& r, rem::Reminder*) { n += !r.done; });
    return n;
}

// Rows remember which view they open.
void set_row_view(GtkWidget* row, View v) {
    g_object_set_data_full(G_OBJECT(row), "view", new View(std::move(v)),
                           [](gpointer p) { delete static_cast<View*>(p); });
}

const View* row_view(GtkListBoxRow* row) {
    return static_cast<const View*>(g_object_get_data(G_OBJECT(row), "view"));
}

GtkWidget* sidebar_row(const char* icon_name, std::string_view color, const std::string& title,
                       std::optional<int> count) {
    auto* row = gtk_list_box_row_new();
    auto* box = hbox(12);
    auto* img = icon(icon_name, {"list-icon"});
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(img, color_class(color).c_str());
    auto* name = label(title);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    append(box, {img, name});
    if (count) append(box, {label(std::to_string(*count), {"dim-label", "numeric"})});
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

GtkWidget* sidebar_heading(const char* text) {
    auto* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    auto* l = label(text, {"heading", "dim-label", "sidebar-heading"});
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
    return row;
}

GtkWidget* boxed_list() {
    auto* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    return list;
}

GMenu* menu_section(GMenu* menu) {
    auto* section = g_menu_new();
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
    return section;
}

// Validates a list name for a file that must work on every synced platform.
std::string list_name_error(const std::string& name) {
    if (name.empty()) return "Enter a name";
    if (name.front() == '.') return "Names can't start with a dot";
    if (name.find_first_of("/\\<>:\"|?*") != std::string::npos)
        return "Names can't contain / \\ < > : \" | ? *";
    if (name.find(".sync-conflict-") != std::string::npos) return "That name is reserved";
    return {};
}

// A dragged reminder's id travels as this private type rather than as plain
// text, so text fields (the "New Reminder" entry, a title being edited)
// don't accept the drop and paste the id into themselves.
GType reminder_drag_type() {
    static GType type = g_boxed_type_register_static(
        "RemindersReminderId",
        [](gpointer p) -> gpointer { return new std::string(*static_cast<std::string*>(p)); },
        [](gpointer p) { delete static_cast<std::string*>(p); });
    return type;
}

const std::string* dragged_id(const GValue* value) {
    if (!value || !G_VALUE_HOLDS(value, reminder_drag_type())) return nullptr;
    return static_cast<const std::string*>(g_value_get_boxed(value));
}

// The widget a controller is attached to; null once that widget is gone
// (a drop rebuilds the view, destroying the dragged row before "drag-end").
GtkWidget* owner(gpointer controller) {
    return gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
}

// Rows carry their reminder's id when dragged.
void make_draggable(GtkWidget* row, const std::string& id) {
    auto* source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    connect<GdkContentProvider*(GtkDragSource*, double, double)>(
        source, "prepare", [id](GtkDragSource*, double, double) {
            GValue value = G_VALUE_INIT;
            g_value_init(&value, reminder_drag_type());
            g_value_set_boxed(&value, &id);
            auto* provider = gdk_content_provider_new_for_value(&value);
            g_value_unset(&value);
            return provider;
        });
    connect<void(GtkDragSource*, GdkDrag*)>(source, "drag-begin", [](GtkDragSource* s, GdkDrag*) {
        auto* row = owner(s);
        if (!row) return;
        // A still image of the row (the row itself fades while dragged).
        auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
        auto still = Obj<GdkPaintable>::adopt(gdk_paintable_get_current_image(live.get()));
        gtk_drag_source_set_icon(s, still.get(), 24, 20);
        gtk_widget_add_css_class(row, "dragging");
    });
    connect<void(GtkDragSource*, GdkDrag*, gboolean)>(source, "drag-end", [](GtkDragSource* s, GdkDrag*, gboolean) {
        if (auto* row = owner(s)) gtk_widget_remove_css_class(row, "dragging");
    });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
}

enum class DropStyle { Halves, Above, Into };

// Accepts dropped reminders on `row`, showing where they'd land.
// `self` is the row's own reminder id (it can't be dropped on itself).
void make_drop_target(GtkWidget* row, DropStyle style, std::string self,
                      std::function<void(std::string, rem::Document::Place)> on_drop) {
    auto* target = gtk_drop_target_new(reminder_drag_type(), GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(target, TRUE);
    auto place_at = [style](GtkWidget* w, double y) {
        if (style == DropStyle::Halves && y > gtk_widget_get_height(w) / 2.0) return rem::Document::Place::After;
        return rem::Document::Place::Before;
    };
    auto clear = [](GtkWidget* w) {
        if (!w) return;
        for (auto* c : {"drop-above", "drop-below", "drop-into"}) gtk_widget_remove_css_class(w, c);
    };
    connect<GdkDragAction(GtkDropTarget*, double, double)>(
        target, "motion", [style, self, place_at, clear](GtkDropTarget* t, double, double y) {
            auto* w = owner(t);
            if (!w) return GdkDragAction(0);
            clear(w);
            auto* id = dragged_id(gtk_drop_target_get_value(t));
            if (id && *id == self) return GdkDragAction(0);
            if (style == DropStyle::Into) gtk_widget_add_css_class(w, "drop-into");
            else if (place_at(w, y) == rem::Document::Place::Before) gtk_widget_add_css_class(w, "drop-above");
            else gtk_widget_add_css_class(w, "drop-below");
            return GDK_ACTION_MOVE;
        });
    connect<void(GtkDropTarget*)>(target, "leave", [clear](GtkDropTarget* t) { clear(owner(t)); });
    connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
        target, "drop",
        [self, place_at, clear, on_drop](GtkDropTarget* t, const GValue* value, double, double y) -> gboolean {
            auto* w = owner(t);
            clear(w);
            auto* id = dragged_id(value);
            if (!w || !id || *id == self) return FALSE;
            auto place = place_at(w, y);
            // Rebuilding the view destroys this row; do it after the drop finishes.
            idle([on_drop, id = *id, place] { on_drop(id, place); });
            return TRUE;
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

constexpr std::pair<View::Kind, std::string_view> kViewNames[] = {
    {View::Today, "today"}, {View::Scheduled, "scheduled"}, {View::All, "all"},
    {View::Flagged, "flagged"}, {View::Completed, "completed"}, {View::List, "list"},
    {View::Tag, "tag"}};

std::string view_to_string(const View& v) {
    for (auto& [k, n] : kViewNames)
        if (k == v.kind) return v.name.empty() ? std::string(n) : std::format("{}:{}", n, v.name);
    return "today";
}

View view_from_string(std::string_view s) {
    auto colon = s.find(':');
    auto kind = s.substr(0, colon);
    for (auto& [k, n] : kViewNames)
        if (n == kind) return View{k, colon == std::string_view::npos ? "" : std::string(s.substr(colon + 1))};
    return View{View::Today, ""};
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

// --- setup ---------------------------------------------------------------

// Runs an edit and records what it changed as one undo step. Nested calls
// (an edit that deletes, say) belong to the outermost step.
template <class F>
std::uint64_t Window::undoable(const char* label, F&& f) {
    if (undo_depth_ > 0 || !store_) {
        f();
        return 0;
    }
    auto before = store_->snapshot();
    ++undo_depth_;
    struct Leave {
        int& depth;
        ~Leave() { --depth; }
    } leave{undo_depth_};
    f();
    auto step = history_.record(label, before, store_->snapshot());
    update_undo_actions();
    return step;
}

Window* Window::create(AdwApplication* app, std::optional<std::filesystem::path> folder) {
    auto* w = new Window(app, std::move(folder));
    attach(w->window_, "ui-window", std::unique_ptr<Window>(w));
    return w;
}

Window* Window::from(GtkWindow* window) {
    return window ? static_cast<Window*>(g_object_get_data(G_OBJECT(window), "ui-window")) : nullptr;
}

void Window::show_reminder(const std::string& id) {
    if (!store_) return;
    auto ref = store_->find(id);
    if (!ref) return;
    select(View{View::List, ref->list->name});
    show_content();
    show_details(id);
}

Window::Window(AdwApplication* app, std::optional<std::filesystem::path> folder) : app_(app) {
    build();
    add_actions();
    if (folder) {
        open_folder(*folder, false);
    } else if (auto saved = load_folder(); saved && std::filesystem::is_directory(*saved)) {
        open_folder(*saved);
    }
    last_notify_check_ = g_get_real_time() / G_USEC_PER_SEC;
    notify_timer_ = timeout(30'000, [this] {
        check_notifications();
        return true;
    });
}

Window::~Window() {
    for (auto id : {reload_timer_, refresh_timer_, notify_timer_, autoscroll_timer_})
        if (id) g_source_remove(id);
    if (monitor_) {
        g_signal_handler_disconnect(monitor_.get(), monitor_handler_);
        g_file_monitor_cancel(monitor_.get());
    }
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
    adw_status_page_set_title(ADW_STATUS_PAGE(welcome_status), "Welcome to Reminders");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(welcome_status),
        "Choose the folder that Syncthing keeps in sync. Each list is stored there as a Markdown file "
        "you can also open in any text editor.");
    auto* choose = gtk_button_new_with_mnemonic("_Choose Folder…");
    gtk_widget_add_css_class(choose, "pill");
    gtk_widget_add_css_class(choose, "suggested-action");
    gtk_widget_set_halign(choose, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(choose), "win.change-folder");
    adw_status_page_set_child(ADW_STATUS_PAGE(welcome_status), choose);
    auto* welcome = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(welcome), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(welcome), welcome_status);

    // Sidebar.
    auto* primary_menu = g_menu_new();
    auto* s1 = menu_section(primary_menu);
    g_menu_append(s1, "_New List…", "win.new-list");
    g_menu_append(s1, "_Change Folder…", "win.change-folder");
    auto* s2 = menu_section(primary_menu);
    g_menu_append(s2, "_Keyboard Shortcuts", "app.shortcuts");
    g_menu_append(s2, "_About Reminders", "app.about");
    auto* menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(primary_menu));
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
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry_), "Search Reminders");
    search_bar_ = gtk_search_bar_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(search_bar_), search_entry_);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(search_bar_), GTK_EDITABLE(search_entry_));
    gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(search_bar_), window_);
    g_object_bind_property(search_toggle, "active", search_bar_, "search-mode-enabled",
                           GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
    // Enter (or ↓) moves into the results, to go through them with the arrow keys.
    on(search_entry_, "activate", [this] { focus_results(); });
    auto* search_keys = gtk_event_controller_key_new();
    connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
        search_keys, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType mods) -> gboolean {
            if (key != GDK_KEY_Down || (mods & gtk_accelerator_get_default_mod_mask())) return FALSE;
            focus_results();
            return TRUE;
        });
    gtk_widget_add_controller(search_entry_, search_keys);
    on(search_entry_, "search-changed", [this] {
        if (updating_sidebar_) return;  // cleared by select()
        auto text = trim(gtk_editable_get_text(GTK_EDITABLE(search_entry_)));
        // Already showing these results (Enter got there first): don't rebuild
        // them, which would take focus away from the selected result.
        if (!text.empty() && view_ == View{View::Search, text}) return;
        if (!text.empty()) select(View{View::Search, text});
        else if (view_.kind == View::Search) select(View{View::Today, ""});
    });

    sidebar_list_ = gtk_list_box_new();
    gtk_widget_add_css_class(sidebar_list_, "navigation-sidebar");
    connect<void(GtkListBox*, GtkListBoxRow*)>(sidebar_list_, "row-activated",
                                               [this](GtkListBox*, GtkListBoxRow* row) {
                                                   if (updating_sidebar_) return;
                                                   if (auto* v = row_view(row)) {
                                                       select(*v);
                                                       show_content();
                                                   }
                                               });
    auto* sidebar_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroller), sidebar_list_);

    auto* new_list_button = gtk_button_new();
    auto* new_list_content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(new_list_content), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(new_list_content), "New List");
    gtk_button_set_child(GTK_BUTTON(new_list_button), new_list_content);
    gtk_widget_add_css_class(new_list_button, "flat");
    gtk_widget_set_margin_start(new_list_button, 6);
    gtk_widget_set_margin_end(new_list_button, 6);
    gtk_widget_set_margin_top(new_list_button, 6);
    gtk_widget_set_margin_bottom(new_list_button, 6);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(new_list_button), "win.new-list");

    auto* sidebar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebar_view), sidebar_header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebar_view), search_bar_);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebar_view), sidebar_scroller);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(sidebar_view), new_list_button);
    auto* sidebar_page = adw_navigation_page_new(sidebar_view, "Reminders");

    // Content.
    content_title_ = adw_window_title_new("", "");
    auto* list_menu = g_menu_new();
    auto* m1 = menu_section(list_menu);
    g_menu_append(m1, "_Show Completed", "win.show-completed");
    auto* m2 = menu_section(list_menu);
    g_menu_append(m2, "Add _Section…", "win.add-section");
    g_menu_append(m2, "List _Info…", "win.list-info");
    auto* m3 = menu_section(list_menu);
    g_menu_append(m3, "_Delete List…", "win.delete-list");
    list_menu_button_ = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(list_menu_button_), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(list_menu_button_), G_MENU_MODEL(list_menu));
    gtk_widget_set_tooltip_text(list_menu_button_, "List Menu");
    g_object_unref(list_menu);

    new_button_ = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(new_button_, "New Reminder");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(new_button_), "win.new-reminder");

    auto* content_header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(content_header), content_title_);
    auto* sidebar_toggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebar_toggle), "sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(sidebar_toggle, "Show Sidebar");
    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), sidebar_toggle);
    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), new_button_);
    adw_header_bar_pack_end(ADW_HEADER_BAR(content_header), list_menu_button_);

    content_scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(content_scroller_), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(content_scroller_, TRUE);
    setup_autoscroll();
    connect<void(GObject*, GParamSpec*)>(
        gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(content_scroller_)), "notify::page-size",
        [this](GObject*, GParamSpec*) {
            // Resizing from inside the layout pass would only take effect a frame late anyway.
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
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_view), content_header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_view), banner_);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_view), content_scroller_);
    content_page_ = GTK_WIDGET(adw_navigation_page_new(content_view, "Today"));

    // An overlay split view can hide its sidebar at any width (Ctrl+B); on
    // narrow windows the sidebar slides over the content.
    split_ = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), GTK_WIDGET(sidebar_page));
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_), content_page_);
    adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split_), 300);
    g_object_bind_property(split_, "show-sidebar", sidebar_toggle, "active",
                           GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));

    auto* bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 560sp"));
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

    main_stack_ = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(main_stack_), welcome, "welcome");
    gtk_stack_add_named(GTK_STACK(main_stack_), split_, "main");
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "welcome");

    toasts_ = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toasts_), main_stack_);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), toasts_);
}

void Window::add_actions() {
    add_action(window_, "change-folder", [this] { choose_folder(); });
    // "go-1" … "go-10": the sidebar's entries in order (Ctrl+1 … Ctrl+9, Ctrl+0).
    for (int n = 1; n <= 10; ++n)
        add_action(window_, std::format("go-{}", n).c_str(), [this, n] {
            if (!store_) return;
            auto views = sidebar_views();
            if (n <= static_cast<int>(views.size())) {
                select(views[static_cast<std::size_t>(n - 1)]);
                show_content();
            }
        });
    add_action(window_, "go-to", [this] {
        if (store_) quick_switcher();
    });
    add_action(window_, "toggle-sidebar", [this] {
        auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
        adw_overlay_split_view_set_show_sidebar(split, !adw_overlay_split_view_get_show_sidebar(split));
    });
    add_action(window_, "show-subtasks", [this] {
        collapsed_.clear();
        rebuild_content();
    });
    add_action(window_, "hide-subtasks", [this] {
        if (!store_) return;
        for (auto* l : store_->lists())
            for (auto* r : l->doc.reminders())
                if (!r->subtasks.empty()) collapsed_.insert(r->id);
        rebuild_content();
    });
    add_action(window_, "next-view", [this] { step_view(1); });
    add_action(window_, "previous-view", [this] { step_view(-1); });
    undo_action_ = add_action(window_, "undo", [this] { undo(); });
    redo_action_ = add_action(window_, "redo", [this] { redo(); });
    update_undo_actions();
    add_action(window_, "new-list", [this] {
        if (store_) new_list();
    });
    add_action(window_, "new-reminder", [this] {
        if (first_new_entry_) gtk_widget_grab_focus(first_new_entry_);
    });
    add_action(window_, "search", [this] {
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(search_bar_), TRUE);
        adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), TRUE);
        gtk_widget_grab_focus(search_entry_);
    });
    add_action(window_, "review-lists", [this] {
        if (store_) review_candidates();
    });
    add_action(window_, "add-section", [this] {
        if (view_.kind == View::List) add_section(view_.name);
    });
    add_action(window_, "list-info", [this] {
        if (view_.kind == View::List) edit_list(view_.name);
    });
    add_action(window_, "delete-list", [this] {
        if (view_.kind == View::List) delete_list(view_.name);
    });
    show_completed_action_ = add_toggle(window_, "show-completed", false, [this](bool on) {
        show_completed_ = on;
        rebuild_content();
    });
}

// --- folder ----------------------------------------------------------------

void Window::choose_folder() {
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose Syncthing Folder");
    if (store_) {
        auto current = Obj<GFile>::adopt(g_file_new_for_path(store_->folder().c_str()));
        gtk_file_dialog_set_initial_folder(dialog, current.get());
    }
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(window_), nullptr,
        [](GObject* source, GAsyncResult* res, gpointer data) {
            auto* self = static_cast<Window*>(data);
            GError* error = nullptr;
            auto file = Obj<GFile>::adopt(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &error));
            if (error) {
                g_error_free(error);  // cancelled
                return;
            }
            auto path = take_string(g_file_get_path(file.get()));
            if (path.empty()) {
                self->toast("That folder isn't on a local disk");
                return;
            }
            self->open_folder(path);
        },
        this);
    g_object_unref(dialog);
}

void Window::open_folder(const std::filesystem::path& folder, bool remember) {
    try {
        auto store = std::make_unique<rem::Store>(folder, rem::state_dir(folder, device_name()));
        store->load_all();
        store_ = std::move(store);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't open the folder: {}", e.what()));
        return;
    }
    if (remember) save_folder(folder);
    try {
        rem::ignore_state_in_syncthing(folder);
    } catch (const std::exception&) {
        // Not fatal: the per-device state would just be synced too.
    }
    history_.clear();  // steps refer to the old folder's lists
    update_undo_actions();
    // The saved view belongs to the saved folder.
    remember_view_ = remember;

    if (monitor_) {
        g_signal_handler_disconnect(monitor_.get(), monitor_handler_);
        g_file_monitor_cancel(monitor_.get());
        monitor_.reset();
    }
    auto dir = Obj<GFile>::adopt(g_file_new_for_path(folder.c_str()));
    GError* error = nullptr;
    monitor_ = Obj<GFileMonitor>::adopt(g_file_monitor_directory(dir.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, &error));
    if (error) {
        toast("Changes from other devices won't show until restart: can't watch the folder");
        g_error_free(error);
    } else {
        monitor_handler_ = connect<void(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent)>(
            monitor_.get(), "changed",
            [this](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent) { on_file_changed(file, other); });
    }

    gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "main");
    view_ = remember ? view_from_string(load_last_view()) : View{View::Today, ""};
    if (view_.kind == View::List && !store_->list(view_.name)) view_ = View{View::Today, ""};
    refresh();
}

void Window::on_file_changed(GFile* file, GFile* other) {
    for (auto* f : {file, other}) {
        if (!f) continue;
        auto path = take_string(g_file_get_path(f));
        if (auto name = rem::Store::list_name_for(path)) pending_reload_.insert(*name);
    }
    if (pending_reload_.empty()) return;
    // Wait for a burst of changes (Syncthing renames, writes, conflict copies) to settle.
    if (reload_timer_) g_source_remove(reload_timer_);
    reload_timer_ = timeout(400, [this] {
        reload_timer_ = 0;
        reload_pending();
        return false;
    });
}

void Window::reload_pending() {
    auto candidates_before = store_->candidates();
    bool changed = false;
    for (auto& name : pending_reload_) {
        try {
            changed |= store_->reload(name);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't read “{}”: {}", name, e.what()));
        }
    }
    pending_reload_.clear();
    if (!changed) {
        // A non-list Markdown file may have gained or lost a checklist.
        if (store_->candidates() != candidates_before) update_banner();
        return;
    }
    if (view_.kind == View::List && !store_->list(view_.name)) view_ = View{View::Today, ""};
    refresh();
}

// --- rendering -------------------------------------------------------------

void Window::refresh() {
    if (refresh_timer_) {
        g_source_remove(refresh_timer_);
        refresh_timer_ = 0;
    }
    if (!store_) return;
    rebuild_sidebar();
    rebuild_content();
    update_banner();
}

void Window::refresh_later(guint ms) {
    rebuild_sidebar();  // counts update straight away
    if (refresh_timer_) g_source_remove(refresh_timer_);
    refresh_timer_ = timeout(ms, [this] {
        refresh_timer_ = 0;
        rebuild_content();
        return false;
    });
}

void Window::select(View v) {
    if (v.kind != View::Search && gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(search_bar_))) {
        updating_sidebar_ = true;
        gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
        updating_sidebar_ = false;
    }
    view_ = std::move(v);
    if (view_.kind != View::Search && remember_view_) save_last_view(view_to_string(view_));
    refresh();
}

void Window::rebuild_sidebar() {
    updating_sidebar_ = true;
    auto* list = GTK_LIST_BOX(sidebar_list_);
    gtk_list_box_remove_all(list);
    auto day = today();

    for (auto& s : kSmart) {
        std::size_t count = 0;
        switch (s.kind) {
            case View::Today: count = store_->today(day).size(); break;
            case View::Scheduled: count = store_->scheduled().size(); break;
            case View::All: count = store_->all().size(); break;
            case View::Flagged: count = store_->flagged().size(); break;
            case View::Completed: count = store_->completed().size(); break;
            default: break;
        }
        auto* row = sidebar_row(s.icon, s.color, s.title, static_cast<int>(count));
        set_row_view(row, View{s.kind, ""});
        gtk_list_box_append(list, row);
    }

    gtk_list_box_append(list, sidebar_heading("My Lists"));
    for (auto* l : store_->lists()) {
        auto* row = sidebar_row(list_icon_name(l->icon()), l->color(), l->name, open_count(*l));
        set_row_view(row, View{View::List, l->name});
        make_drop_target(row, DropStyle::Into, "", [this, name = l->name](std::string dropped, rem::Document::Place) {
            move_to_list(dropped, name);
        });
        gtk_list_box_append(list, row);
    }

    auto tags = store_->tags();
    if (!tags.empty()) {
        gtk_list_box_append(list, sidebar_heading("Tags"));
        for (auto& t : tags) {
            auto* row = sidebar_row("sr-tag-symbolic", "gray", "#" + t, std::nullopt);
            set_row_view(row, View{View::Tag, t});
            gtk_list_box_append(list, row);
        }
    }

    gtk_list_box_unselect_all(list);
    for (int i = 0;; ++i) {
        auto* row = gtk_list_box_get_row_at_index(list, i);
        if (!row) break;
        if (auto* v = row_view(row); v && *v == view_) {
            gtk_list_box_select_row(list, row);
            break;
        }
    }
    updating_sidebar_ = false;
}

void Window::rebuild_content() {
    if (!store_) return;
    first_new_entry_ = nullptr;
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
            view_ = View{View::Today, ""};
            return refresh();
        }
        page_title = l->name;
        int open = open_count(*l);
        adw_window_title_set_subtitle(title, open == 1 ? "1 reminder" : std::format("{} reminders", open).c_str());
        body = build_list_view(*l);
    } else {
        if (auto* s = smart_info(view_.kind)) page_title = s->title;
        else if (view_.kind == View::Tag) page_title = "#" + view_.name;
        else page_title = "Search";
        adw_window_title_set_subtitle(title, view_.kind == View::Search ? std::format("“{}”", view_.name).c_str() : "");
        body = build_smart_view();
    }
    adw_window_title_set_title(title, page_title.c_str());
    adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(content_page_), page_title.c_str());

    // Keep the scroll position across rebuilds.
    auto* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(content_scroller_));
    double scroll = gtk_adjustment_get_value(adj);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(content_scroller_), body);
    auto keep = Obj<GtkAdjustment>::ref(adj);
    idle([keep, scroll] { gtk_adjustment_set_value(keep.get(), scroll); });
}

GtkWidget* Window::group(const std::string& title, const char* color, GtkWidget* listbox) {
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
GtkWidget* Window::section_group(rem::ListFile& l, const std::string& name, int count, GtkWidget* listbox) {
    auto list = l.name;
    auto* actions = g_simple_action_group_new();
    add_action(actions, "rename", [this, list, name] { rename_section(list, name); });
    add_action(actions, "delete", [this, list, name, count] { delete_section(list, name, count); });

    auto* menu = g_menu_new();
    g_menu_append(menu, "_Rename Section…", "section.rename");
    g_menu_append(menu, "_Delete Section…", "section.delete");
    auto* button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "circular");
    gtk_widget_set_tooltip_text(button, "Section Menu");
    g_object_unref(menu);

    auto* heading = label(name, {"heading", "list-heading", color_class(l.color()).c_str()});
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
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "rename", "_Rename", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "rename");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    auto* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), name.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [this, list, name, entry](AdwAlertDialog*, const char* response) {
            if (std::string_view(response) != "rename") return;
            auto new_name = trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
            auto* l = store_->list(list);
            if (!l || new_name.empty() || new_name == name) return;
            bool ok = false;
            undoable("Rename Section", [&] {
                ok = l->doc.rename_section(name, new_name);
                if (!ok) return;
                try {
                    store_->save(*l);
                } catch (const std::exception& e) {
                    toast(std::format("Couldn't save: {}", e.what()));
                }
            });
            if (!ok) toast(std::format("There's already a section called “{}”", new_name));
            refresh();
        });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::delete_section(const std::string& list, const std::string& name, int count) {
    auto body = count == 0 ? std::string("The section is empty.")
                           : std::format("It has {} reminder{}. Keep them by moving them to the section above, or "
                                         "delete them with the section.",
                                         count, count == 1 ? "" : "s");
    auto* dialog = adw_alert_dialog_new(std::format("Delete “{}”?", name).c_str(), body.c_str());
    if (count == 0) {
        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "keep", "_Delete", nullptr);
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "keep", ADW_RESPONSE_DESTRUCTIVE);
    } else {
        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "keep", "_Keep Reminders",
                                       "delete", "_Delete Reminders", nullptr);
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    }
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [this, list, name](AdwAlertDialog*, const char* response) {
            std::string_view r = response;
            if (r != "keep" && r != "delete") return;
            auto* l = store_->list(list);
            if (!l) return;
            undoable("Delete Section", [&] {
                if (!l->doc.delete_section(name, r == "keep")) return;
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

GtkWidget* Window::empty_state(const char* icon_name, const char* title, const char* description) {
    auto* page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon_name);
    adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
    if (description) adw_status_page_set_description(ADW_STATUS_PAGE(page), description);
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
    int hidden_done = 0;
    for (auto& section : l.doc.sections()) {
        auto* listbox = boxed_list();
        for (auto* r : section.reminders) {
            if (r->done && !show_completed_) {
                ++hidden_done;
                continue;
            }
            gtk_list_box_append(GTK_LIST_BOX(listbox), build_reminder_row(rem::Ref{&l, r, nullptr}, false));
            if (collapsed_.contains(r->id)) continue;
            for (auto& s : r->subtasks) {
                if (s.done && !show_completed_) {
                    ++hidden_done;
                    continue;
                }
                gtk_list_box_append(GTK_LIST_BOX(listbox), build_reminder_row(rem::Ref{&l, &s, r}, false));
            }
        }
        gtk_list_box_append(GTK_LIST_BOX(listbox), build_new_row(l.name, section.name));
        if (section.name) {
            int count = 0;
            for (auto* r : section.reminders) count += 1 + static_cast<int>(r->subtasks.size());
            append(page, {section_group(l, *section.name, count, listbox)});
        } else {
            append(page, {group("", nullptr, listbox)});
        }
        listboxes.push_back(listbox);
    }
    chain_listboxes(listboxes);

    if (hidden_done > 0) {
        auto* footer = hbox(6);
        gtk_widget_set_halign(footer, GTK_ALIGN_CENTER);
        auto* show = gtk_button_new_with_label("Show");
        gtk_widget_add_css_class(show, "flat");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(show), "win.show-completed");
        append(footer, {label(hidden_done == 1 ? "1 completed" : std::format("{} completed", hidden_done),
                              {"dim-label"}),
                        show});
        append(page, {footer});
    }
    return clamp(page);
}

GtkWidget* Window::build_smart_view() {
    auto day = today();
    std::vector<rem::Ref> refs;
    switch (view_.kind) {
        case View::Today: refs = store_->today(day); break;
        case View::Scheduled: refs = store_->scheduled(); break;
        case View::All: refs = store_->all(); break;
        case View::Flagged: refs = store_->flagged(); break;
        case View::Completed: refs = store_->completed(); break;
        case View::Tag: refs = store_->tagged(view_.name); break;
        case View::Search: refs = store_->search(view_.name); break;
        case View::List: break;
    }

    if (refs.empty()) {
        switch (view_.kind) {
            case View::Today: return empty_state("object-select-symbolic", "Nothing Due Today", nullptr);
            case View::Scheduled: return empty_state("alarm-symbolic", "No Scheduled Reminders", nullptr);
            case View::Flagged: return empty_state("sr-flag-symbolic", "No Flagged Reminders", nullptr);
            case View::Completed: return empty_state("object-select-symbolic", "No Completed Reminders", nullptr);
            case View::Search:
                return empty_state("edit-find-symbolic", "No Results", "Try a different search");
            default: return empty_state("view-list-bullet-symbolic", "No Reminders", nullptr);
        }
    }

    // Each view groups its reminders: by date, or by list.
    struct Group {
        std::string title;
        std::string color_name;
        GtkWidget* listbox;
    };
    std::vector<Group> groups;
    auto group_for = [&](const std::string& title, const std::string& color) -> GtkWidget* {
        for (auto& g : groups)
            if (g.title == title) return g.listbox;
        groups.push_back(Group{title, color, boxed_list()});
        return groups.back().listbox;
    };

    bool by_date = view_.kind == View::Today || view_.kind == View::Scheduled;
    bool flat = view_.kind == View::Flagged;
    if (by_date) {
        auto key = [](const rem::Ref& r) {
            auto t = r.reminder->due_time.value_or(rem::TimeOfDay{-1, 0});
            return std::pair{*r.reminder->due_date, t.hour * 60 + t.minute};
        };
        std::ranges::stable_sort(refs, [&](auto& a, auto& b) { return key(a) < key(b); });
    }
    for (auto& ref : refs) {
        GtkWidget* box;
        if (by_date) {
            auto& due = *ref.reminder->due_date;
            box = group_for(due < day ? "Overdue" : relative_date(due, day), "");
        } else if (flat) {
            box = group_for("", "");
        } else {
            box = group_for(ref.list->name, ref.list->color());
        }
        gtk_list_box_append(GTK_LIST_BOX(box), build_reminder_row(ref, by_date || flat));
    }

    auto* page = vbox(24);
    std::vector<GtkWidget*> listboxes;
    for (auto& g : groups) {
        append(page, {group(g.title, g.color_name.empty() ? nullptr : g.color_name.c_str(), g.listbox)});
        listboxes.push_back(g.listbox);
    }
    chain_listboxes(listboxes);
    return clamp(page);
}

GtkWidget* Window::build_reminder_row(const rem::Ref& ref, bool show_list) {
    auto& r = *ref.reminder;
    auto id = r.id;
    auto day = today();

    auto* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_add_css_class(row, "reminder-row");
    gtk_widget_add_css_class(row, color_class(ref.list->color()).c_str());

    auto* box = hbox(12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, ref.parent && !show_list ? 44 : 12);
    gtk_widget_set_margin_end(box, 6);

    auto* check = gtk_check_button_new();
    gtk_widget_add_css_class(check, "selection-mode");
    gtk_widget_set_valign(check, GTK_ALIGN_START);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), r.done);
    gtk_widget_set_tooltip_text(check, r.done ? "Mark as Not Completed" : "Mark as Completed");
    connect<void(GtkCheckButton*)>(check, "toggled", [this, id](GtkCheckButton* c) {
        toggle_done(id, gtk_check_button_get_active(c));
    });

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
    if (r.done) gtk_widget_add_css_class(title, "dim-label");
    connect<void(GObject*, GParamSpec*)>(title, "notify::editing", [this, id, old = r.title](GObject* o, GParamSpec*) {
        if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(o))) return;
        std::string now = gtk_editable_get_text(GTK_EDITABLE(o));
        if (now != old) idle([this, id, now] { edit_title(id, now); });
    });
    add_shortcut(title, "<Control>s", [title] {
        if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title)))
            gtk_editable_label_stop_editing(GTK_EDITABLE_LABEL(title), TRUE);
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
        gtk_widget_add_css_class(due, is_overdue(r, day) ? "error" : "dim-label");
        add_meta(due);
    }
    if (r.repeat) {
        auto* rep = icon("media-playlist-repeat-symbolic", {"dim-label"});
        gtk_widget_set_tooltip_text(rep, r.repeat->c_str());
        add_meta(rep);
    }
    if (!r.tags.empty()) {
        std::string tags;
        for (auto& t : r.tags) tags += (tags.empty() ? "#" : " #") + t;
        auto* tag_label = label(tags, {"caption", "tags"});
        gtk_label_set_ellipsize(GTK_LABEL(tag_label), PANGO_ELLIPSIZE_END);
        add_meta(tag_label);
    }
    if (r.url) {
        auto* link = gtk_link_button_new_with_label(r.url->c_str(), "Link");
        gtk_widget_add_css_class(link, "caption");
        gtk_widget_add_css_class(link, "inline-link");
        add_meta(link);
    }
    if (show_list || ref.parent) {
        std::string where = show_list ? ref.list->name : "";
        if (ref.parent && show_list) where += " › " + ref.parent->title;
        if (!where.empty()) {
            auto* where_label = label(where, {"caption", "dim-label"});
            gtk_label_set_ellipsize(GTK_LABEL(where_label), PANGO_ELLIPSIZE_END);
            add_meta(where_label);
        }
    }
    if (has_meta) append(text, {meta});

    if (!r.notes.empty()) {
        auto* notes = label(r.notes, {"caption", "dim-label"});
        gtk_label_set_wrap(GTK_LABEL(notes), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(notes), PANGO_ELLIPSIZE_END);
        gtk_label_set_lines(GTK_LABEL(notes), 2);
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
            auto n = std::ranges::count_if(r.subtasks, [this](auto& s) { return show_completed_ || !s.done; });
            if (n > 0) append(content, {label(std::to_string(n), {"caption", "dim-label", "numeric"})});
        }
        append(content, {icon(collapsed ? "pan-end-symbolic" : "pan-down-symbolic")});
        gtk_button_set_child(GTK_BUTTON(toggle), content);
        gtk_widget_add_css_class(toggle, "flat");
        gtk_widget_add_css_class(toggle, "subtask-toggle");
        gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(toggle, collapsed ? "Show Subtasks" : "Hide Subtasks");
        on(toggle, "clicked", [this, id] { idle([this, id] { toggle_subtasks(id); }); });
        append(box, {toggle});
    }

    if (r.flagged) {
        auto* flag = icon("sr-flag-symbolic", {"flag-icon"});
        gtk_widget_set_valign(flag, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(flag, "Flagged");
        append(box, {flag});
    }

    auto* details = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_add_css_class(details, "flat");
    gtk_widget_add_css_class(details, "circular");
    gtk_widget_add_css_class(details, "row-button");
    gtk_widget_set_valign(details, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(details, "Details");
    on(details, "clicked", [this, id] { show_details(id); });

    // Per-row actions: the "more" menu, the context menu and keyboard shortcuts.
    bool in_list = view_.kind == View::List;
    auto* actions = g_simple_action_group_new();
    add_action(actions, "details", [this, id] { show_details(id); });
    add_action(actions, "flag", [this, id] { toggle_flag(id); });
    add_action(actions, "due-today", [this, id] { idle([this, id] { set_due(id, 0); }); });
    add_action(actions, "due-tomorrow", [this, id] { idle([this, id] { set_due(id, 1); }); });
    add_action(actions, "delete", [this, id] { idle([this, id] { delete_reminder(id); }); });
    auto* indent_action = add_action(actions, "indent", [this, id] { idle([this, id] { indent(id, true); }); });
    auto* outdent_action = add_action(actions, "outdent", [this, id] { idle([this, id] { indent(id, false); }); });
    g_simple_action_set_enabled(indent_action, in_list && !ref.parent && r.subtasks.empty());
    g_simple_action_set_enabled(outdent_action, in_list && ref.parent);
    gtk_widget_insert_action_group(row, "reminder", G_ACTION_GROUP(actions));
    g_object_unref(actions);

    // Menu items show their shortcut.
    auto item = [](GMenu* m, const char* text, const char* action, const char* accel) {
        auto* i = g_menu_item_new(text, action);
        g_menu_item_set_attribute(i, "accel", "s", accel);
        g_menu_append_item(m, i);
        g_object_unref(i);
    };
    auto* menu = g_menu_new();
    item(menu, "_Details…", "reminder.details", "<Control>i");
    item(menu, r.flagged ? "_Unflag" : "_Flag", "reminder.flag", "<Control><Shift>f");
    auto* dates = menu_section(menu);
    item(dates, "Due _Today", "reminder.due-today", "<Control>t");
    item(dates, "Due To_morrow", "reminder.due-tomorrow", "<Control><Shift>t");
    if (in_list) {
        auto* structure = menu_section(menu);
        item(structure, "_Indent", "reminder.indent", "<Control>bracketright");
        item(structure, "_Outdent", "reminder.outdent", "<Control>bracketleft");
    }
    auto* danger = menu_section(menu);
    item(danger, "_Delete", "reminder.delete", "Delete");
    auto* more = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(menu));
    gtk_widget_add_css_class(more, "flat");
    gtk_widget_add_css_class(more, "circular");
    gtk_widget_add_css_class(more, "row-button");
    gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(more, "More");
    g_object_unref(menu);
    append(box, {details, more});

    // Right-click and long-press open the same menu.
    auto* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    connect<void(GtkGestureClick*, int, double, double)>(
        click, "pressed", [more](GtkGestureClick*, int, double, double) { gtk_menu_button_popup(GTK_MENU_BUTTON(more)); });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    auto* press = gtk_gesture_long_press_new();
    connect<void(GtkGestureLongPress*, double, double)>(
        press, "pressed", [more](GtkGestureLongPress*, double, double) { gtk_menu_button_popup(GTK_MENU_BUTTON(more)); });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(press));

    // Clicking a row's empty space selects it, so its keyboard shortcuts apply.
    auto* select_click = gtk_gesture_click_new();
    connect<void(GtkGestureClick*, int, double, double)>(
        select_click, "pressed", [row](GtkGestureClick*, int, double x, double y) {
            for (auto* w = gtk_widget_pick(row, x, y, GTK_PICK_DEFAULT); w && w != row; w = gtk_widget_get_parent(w))
                if (GTK_IS_BUTTON(w) || GTK_IS_CHECK_BUTTON(w) || GTK_IS_EDITABLE_LABEL(w) || GTK_IS_MENU_BUTTON(w))
                    return;  // that widget handles the click itself
            gtk_widget_grab_focus(row);
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(select_click));

    auto* keys = gtk_event_controller_key_new();
    connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
        keys, "key-pressed",
        [this, id, title, check, in_list](GtkEventControllerKey*, guint keyval, guint, GdkModifierType mods) -> gboolean {
            if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) return FALSE;
            auto mask = mods & gtk_accelerator_get_default_mod_mask();
            auto key = gdk_keyval_to_lower(keyval);
            auto later = [](std::function<void()> f) {
                idle(std::move(f));  // rebuilding destroys this row: not from inside its handler
                return TRUE;
            };
            if (mask == 0) {
                switch (key) {
                    case GDK_KEY_Delete:
                    case GDK_KEY_KP_Delete: return later([this, id] { delete_reminder(id); });
                    case GDK_KEY_space:
                        gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
                                                    !gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
                        return TRUE;
                    case GDK_KEY_Return:
                    case GDK_KEY_KP_Enter:
                    case GDK_KEY_F2:
                        gtk_editable_label_start_editing(GTK_EDITABLE_LABEL(title));
                        return TRUE;
                }
            } else if (mask == GDK_CONTROL_MASK) {
                switch (key) {
                    case GDK_KEY_t: return later([this, id] { set_due(id, 0); });
                    case GDK_KEY_i: return later([this, id] { show_details(id); });
                    case GDK_KEY_bracketright:
                        if (in_list) return later([this, id] { indent(id, true); });
                        break;
                    case GDK_KEY_bracketleft:
                        if (in_list) return later([this, id] { indent(id, false); });
                        break;
                }
            } else if (mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && key == GDK_KEY_f) {
                return later([this, id] { toggle_flag(id); });
            } else if (mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && key == GDK_KEY_t) {
                return later([this, id] { set_due(id, 1); });
            } else if (mask == GDK_ALT_MASK && key >= GDK_KEY_0 && key <= GDK_KEY_3) {
                auto p = static_cast<rem::Priority>(key - GDK_KEY_0);
                return later([this, id, p] { set_priority(id, p); });
            } else if (mask == GDK_ALT_MASK && in_list && (key == GDK_KEY_Up || key == GDK_KEY_Down)) {
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

    if (!first_row_) first_row_ = row;
    make_draggable(row, id);
    if (view_.kind == View::List)
        make_drop_target(row, DropStyle::Halves, id, [this, id](std::string dropped, rem::Document::Place place) {
            move_reminder(dropped, id, place);
        });
    return row;
}

GtkWidget* Window::build_new_row(const std::string& list, const std::optional<std::string>& section) {
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
    gtk_widget_set_tooltip_text(entry, "Fields work here too, e.g. “Milk #errands 🚩 📅 2026-10-03”");
    append(box, {icon("list-add-symbolic", {"dim-label"}), entry});
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    add_shortcut(entry, "<Control>s", [entry] { g_signal_emit_by_name(entry, "activate"); });
    add_shortcut(entry, "Escape", [entry] { gtk_editable_set_text(GTK_EDITABLE(entry), ""); });
    connect<void(GtkText*)>(entry, "activate", [this, list, section](GtkText* t) {
        std::string text = gtk_editable_get_text(GTK_EDITABLE(t));
        if (trim(text).empty()) return;
        idle([this, list, section, text] { add_reminder(list, section, text); });
    });

    make_drop_target(row, DropStyle::Above, "", [this, list, section](std::string dropped, rem::Document::Place) {
        move_to_section_end(dropped, list, section);
    });

    auto key = list + '\x1f' + section.value_or("");
    if (!first_new_entry_) first_new_entry_ = entry;
    if (focus_new_row_ == key) {
        focus_new_row_.reset();
        auto keep = Obj<GtkWidget>::ref(entry);
        idle([keep] { gtk_widget_grab_focus(keep.get()); });
    }
    return row;
}

// --- editing ---------------------------------------------------------------

void Window::toast(const std::string& text, const char* button, std::function<void()> on_button) {
    auto* t = adw_toast_new(text.c_str());
    if (button) {
        adw_toast_set_button_label(t, button);
        on(t, "button-clicked", std::move(on_button));
    }
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toasts_), t);
}

void Window::add_reminder(const std::string& list, const std::optional<std::string>& section,
                          const std::string& text) {
    undoable("Add Reminder", [&] {
        auto* l = store_->list(list);
        if (!l) return;
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
        if (!ref) return;
        if (trim(text).empty()) return delete_reminder(id);
        auto& r = *ref->reminder;
        // Typed fields ("#tag", "📅 2026-10-03", …) are applied, not kept in the title.
        auto f = rem::parse_fields(trim(text));
        r.title = f.title;
        for (auto& t : f.tags)
            if (std::ranges::find(r.tags, t) == r.tags.end()) r.tags.push_back(t);
        if (f.priority != rem::Priority::None) r.priority = f.priority;
        if (f.flagged) r.flagged = true;
        if (f.repeat) r.repeat = f.repeat;
        if (f.due_date) {
            r.due_date = f.due_date;
            r.due_time = f.due_time;
        }
        if (f.url) r.url = f.url;
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

void Window::move_reminder(const std::string& id, const std::string& target, rem::Document::Place place) {
    undoable("Move Reminder", [&] {
        auto ref = store_->find(id);
        auto target_ref = store_->find(target);
        if (!ref || !target_ref) return;
        try {
            if (ref->list != target_ref->list) store_->move_to_list(id, *target_ref->list);
            auto* l = store_->find(target)->list;
            if (!l->doc.move_next_to(id, target, place)) toast("Subtasks can't have subtasks of their own");
            store_->save(*l);  // also covers a move from another list
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
        refresh();
    });
}

void Window::move_to_section_end(const std::string& id, const std::string& list,
                                 const std::optional<std::string>& section) {
    undoable("Move Reminder", [&] {
        auto ref = store_->find(id);
        auto* l = store_->list(list);
        if (!ref || !l) return;
        try {
            if (ref->list != l) store_->move_to_list(id, *l);
            l->doc.move_to_end(id, section);
            store_->save(*l);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
        refresh();
    });
}

void Window::move_step(const std::string& id, bool up) {
    undoable("Move Reminder", [&] {
        auto ref = store_->find(id);
        if (!ref) return;
        // Hidden completed reminders are skipped, so each step moves past a visible one.
        bool show_done = show_completed_;
        auto visible = [show_done](const rem::Reminder& r) { return show_done || !r.done; };
        if (!ref->list->doc.move_step(id, up, visible)) return;
        try {
            store_->save(*ref->list);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
        focus_reminder_ = id;
        refresh();
    });
}

void Window::move_to_list(const std::string& id, const std::string& list) {
    undoable("Move Reminder", [&] {
        auto ref = store_->find(id);
        auto* l = store_->list(list);
        if (!ref || !l || ref->list == l) return;
        try {
            store_->move_to_list(id, *l);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
        refresh();
        toast(std::format("Moved to “{}”", list));
    });
}

// GTK doesn't scroll during drag and drop on its own: scroll when the
// pointer is near the top or bottom edge of the content.
void Window::setup_autoscroll() {
    auto* motion = gtk_drop_controller_motion_new();
    connect<void(GtkDropControllerMotion*, double, double)>(
        motion, "motion", [this](GtkDropControllerMotion*, double, double y) {
            autoscroll_y_ = y;
            if (autoscroll_timer_) return;
            autoscroll_timer_ = timeout(16, [this] {
                if (autoscroll_y_ < 0) {
                    autoscroll_timer_ = 0;
                    return false;
                }
                constexpr double edge = 56, max_step = 14;
                double h = gtk_widget_get_height(content_scroller_);
                double step = 0;
                if (autoscroll_y_ < edge) step = -max_step * (1 - autoscroll_y_ / edge);
                else if (autoscroll_y_ > h - edge) step = max_step * (1 - (h - autoscroll_y_) / edge);
                if (step != 0) {
                    auto* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(content_scroller_));
                    gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + step);
                }
                return true;
            });
        });
    auto stop = [this] { autoscroll_y_ = -1; };
    connect<void(GtkDropControllerMotion*)>(motion, "leave", [stop](GtkDropControllerMotion*) { stop(); });
    gtk_widget_add_controller(content_scroller_, GTK_EVENT_CONTROLLER(motion));
}

void Window::set_priority(const std::string& id, rem::Priority priority) {
    undoable("Set Priority", [&] {
        auto ref = store_->find(id);
        if (!ref || ref->reminder->priority == priority) return;
        ref->reminder->priority = priority;
        try {
            store_->touch(id);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
    });
    focus_reminder_ = id;
    refresh();
}

void Window::indent(const std::string& id, bool in) {
    auto ref = store_->find(id);
    if (!ref) return;
    bool show_done = show_completed_;
    auto visible = [show_done](const rem::Reminder& r) { return show_done || !r.done; };
    auto* list = ref->list;
    bool moved = false;
    undoable(in ? "Indent" : "Outdent", [&] {
        moved = in ? list->doc.indent(id, visible) : list->doc.outdent(id);
        if (!moved) return;
        try {
            store_->save(*list);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
    });
    if (!moved && in && !ref->reminder->subtasks.empty()) toast("Subtasks can't have subtasks of their own");
    if (auto now = store_->find(id); moved && now && now->parent) collapsed_.erase(now->parent->id);
    focus_reminder_ = id;
    refresh();
}

void Window::toggle_flag(const std::string& id) {
    undoable("Flag Reminder", [&] {
        auto ref = store_->find(id);
        if (!ref) return;
        ref->reminder->flagged = !ref->reminder->flagged;
        try {
            store_->touch(id);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
    });
    focus_reminder_ = id;
    refresh();
}

void Window::delete_reminder(const std::string& id) {
    if (!store_->find(id)) return;
    auto step = undoable("Delete Reminder", [&] {
        try {
            store_->remove(id);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
    });
    refresh();
    // The toast's Undo only applies while this deletion is still the latest step.
    if (step) toast("Reminder deleted", "_Undo", [this, step] {
        if (history_.next_undo() == step) undo();
    });
}

void Window::show_details(const std::string& id) {
    auto ref = store_->find(id);
    if (!ref) return;
    std::vector<std::string> names;
    for (auto* l : store_->lists()) names.push_back(l->name);
    show_reminder_dialog(window_, *ref->reminder, ref->parent != nullptr, ref->list->name, names, [this, id](ReminderEdit e) {
        if (e.deleted) return delete_reminder(id);
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
            if (e.list != ref->list->name)
                if (auto* dest = store_->list(e.list)) store_->move_to_list(id, *dest);
        } catch (const std::exception& ex) {
            toast(std::format("Couldn't save: {}", ex.what()));
        }
        });
        refresh();
    });
}

void Window::new_list() {
    show_list_dialog(
        window_, std::nullopt,
        [this](const ListEdit& e) -> std::string {
            if (auto err = list_name_error(e.name); !err.empty()) return err;
            for (auto* l : store_->lists())
                if (lower(l->name) == lower(e.name)) return "A list with that name already exists";
            return {};
        },
        [this](ListEdit e) {
            bool ok = true;
            undoable("New List", [&] {
                try {
                    store_->create_list(e.name, e.color, e.icon);
                } catch (const std::exception& ex) {
                    toast(std::format("Couldn't create the list: {}", ex.what()));
                    ok = false;
                }
            });
            if (!ok) return;
            select(View{View::List, e.name});
            show_content();
        });
}

void Window::edit_list(const std::string& name) {
    auto* l = store_->list(name);
    if (!l) return;
    show_list_dialog(
        window_, ListEdit{l->name, l->color(), l->icon()},
        [this, name](const ListEdit& e) -> std::string {
            if (auto err = list_name_error(e.name); !err.empty()) return err;
            for (auto* other : store_->lists())
                if (other->name != name && lower(other->name) == lower(e.name))
                    return "A list with that name already exists";
            return {};
        },
        [this, name](ListEdit e) {
            auto* l = store_->list(name);
            if (!l) return;
            bool ok = true;
            undoable("Edit List", [&] {
                try {
                    if (e.name != name && !store_->rename_list(*l, e.name)) {
                        toast("Couldn't rename the list");
                        ok = false;
                        return;
                    }
                    l->doc.set_meta("color", e.color);
                    l->doc.set_meta("icon", e.icon);
                    store_->save(*l);
                } catch (const std::exception& ex) {
                    toast(std::format("Couldn't save: {}", ex.what()));
                }
            });
            if (!ok) return;
            if (view_.kind == View::List && view_.name == name) {
                view_.name = l->name;
                if (remember_view_) save_last_view(view_to_string(view_));
            }
            refresh();
        });
}

void Window::delete_list(const std::string& name) {
    auto* dialog = adw_alert_dialog_new(std::format("Delete “{}”?", name).c_str(),
                                        "The list and all its reminders will be deleted on every synced device. "
                                        "On this computer the file is moved to the Trash.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "delete", "_Delete", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    connect<void(AdwAlertDialog*, const char*)>(dialog, "response", [this, name](AdwAlertDialog*, const char* response) {
        if (std::string_view(response) != "delete") return;
        auto step = undoable("Delete List", [&] {
            auto file = Obj<GFile>::adopt(g_file_new_for_path(store_->path_of(name).c_str()));
            GError* error = nullptr;
            if (!g_file_trash(file.get(), nullptr, &error)) {
                // No trash on this filesystem: delete outright.
                g_error_free(error);
            }
            store_->delete_list(name);
        });
        if (view_.kind == View::List && view_.name == name) view_ = View{View::Today, ""};
        refresh();
        toast(std::format("“{}” deleted", name), "_Undo", [this, step, name] {
            if (history_.next_undo() != step) return;
            undo();
            select(View{View::List, name});
        });
    });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::add_section(const std::string& list) {
    auto* dialog = adw_alert_dialog_new("Add Section", nullptr);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "add", "_Add", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "add", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "add");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    auto* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Section Name");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [this, list, entry](AdwAlertDialog*, const char* response) {
            if (std::string_view(response) != "add") return;
            auto name = trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
            auto* l = store_->list(list);
            if (name.empty() || !l) return;
            for (auto& s : l->doc.sections())
                if (s.name == name) return;
            auto& blocks = l->doc.blocks;
            if (!blocks.empty()) {
                auto* last = std::get_if<rem::RawLine>(&blocks.back());
                if (!last || !trim(last->text).empty()) blocks.push_back(rem::RawLine{""});
            }
            undoable("Add Section", [&] {
                blocks.push_back(rem::RawLine{"## " + name});
                try {
                    store_->save(*l);
                } catch (const std::exception& e) {
                    toast(std::format("Couldn't save: {}", e.what()));
                }
            });
            focus_new_row_ = list + '\x1f' + name;
            refresh();
        });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

Window::ViewInfo Window::view_info(const View& v) {
    if (auto* s = smart_info(v.kind)) return {v, s->title, s->icon, s->color};
    if (v.kind == View::List) {
        auto* l = store_->list(v.name);
        return {v, v.name, l ? list_icon_name(l->icon()) : "view-list-bullet-symbolic", l ? l->color() : "gray"};
    }
    if (v.kind == View::Tag) return {v, "#" + v.name, "sr-tag-symbolic", "gray"};
    return {v, std::format("Search for “{}”", v.name), "edit-find-symbolic", "gray"};
}

namespace {

// How well `query` matches `title` (lower is better), or -1 for no match:
// a prefix, then the start of a word, then anywhere, then letters in order.
int match_score(const std::string& title, const std::string& query) {
    auto t = lower(title), q = lower(query);
    if (!t.empty() && t[0] == '#' && !q.empty() && q[0] != '#') t.erase(0, 1);
    if (q.empty() || t.starts_with(q)) return 0;
    if (auto at = t.find(" " + q); at != std::string::npos) return 1;
    if (t.find(q) != std::string::npos) return 2;
    std::size_t i = 0;
    for (char c : t)
        if (i < q.size() && c == q[i]) ++i;
    return i == q.size() ? 3 : -1;
}

}  // namespace

// Ctrl+K: type part of a list's name, Enter to go there.
void Window::quick_switcher() {
    struct State {
        GtkWidget* dialog;
        GtkWidget* entry;
        GtkWidget* list;
        std::vector<ViewInfo> all;
        std::vector<View> shown;  // row index → view
    };
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(ADW_DIALOG(dialog), "Go To");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 420);
    adw_dialog_set_content_height(ADW_DIALOG(dialog), 460);
    auto* st = attach(dialog, "state", std::make_unique<State>());
    st->dialog = GTK_WIDGET(dialog);
    for (auto& v : sidebar_views()) st->all.push_back(view_info(v));

    st->entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(st->entry), "Go to a list or tag…");
    gtk_widget_set_hexpand(st->entry, TRUE);
    st->list = gtk_list_box_new();
    gtk_widget_add_css_class(st->list, "navigation-sidebar");
    auto* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), st->list);
    gtk_widget_set_vexpand(scroller, TRUE);

    auto fill = [this, st] {
        gtk_list_box_remove_all(GTK_LIST_BOX(st->list));
        st->shown.clear();
        std::string query = trim(gtk_editable_get_text(GTK_EDITABLE(st->entry)));
        std::vector<std::pair<int, const ViewInfo*>> hits;
        for (auto& info : st->all)
            if (int score = match_score(info.title, query); score >= 0) hits.emplace_back(score, &info);
        std::ranges::stable_sort(hits, {}, &std::pair<int, const ViewInfo*>::first);
        std::vector<ViewInfo> rows;
        for (auto& [_, info] : hits) rows.push_back(*info);
        if (!query.empty()) rows.push_back(view_info(View{View::Search, query}));  // always offer a search
        for (auto& info : rows) {
            gtk_list_box_append(GTK_LIST_BOX(st->list), sidebar_row(info.icon, info.color, info.title, std::nullopt));
            st->shown.push_back(info.view);
        }
        if (auto* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(st->list), 0))
            gtk_list_box_select_row(GTK_LIST_BOX(st->list), first);
    };
    auto go = [this, st](int index) {
        if (index < 0 || index >= static_cast<int>(st->shown.size())) return;
        auto view = st->shown[static_cast<std::size_t>(index)];
        adw_dialog_close(ADW_DIALOG(st->dialog));
        select(view);
        show_content();
    };
    auto selected = [st] {
        auto* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(st->list));
        return row ? gtk_list_box_row_get_index(row) : -1;
    };

    on(st->entry, "search-changed", fill);
    // The search entry takes Escape for itself ("stop-search"), so the
    // dialog never sees it: close from here.
    on(st->entry, "stop-search", [st] { adw_dialog_close(ADW_DIALOG(st->dialog)); });
    on(st->entry, "activate", [go, selected] { go(selected()); });
    connect<void(GtkListBox*, GtkListBoxRow*)>(st->list, "row-activated", [go](GtkListBox*, GtkListBoxRow* row) {
        go(gtk_list_box_row_get_index(row));
    });
    // ↑/↓ move the selection while typing continues in the entry.
    auto* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
        keys, "key-pressed", [st, selected](GtkEventControllerKey*, guint key, guint, GdkModifierType) -> gboolean {
            int delta = key == GDK_KEY_Down ? 1 : key == GDK_KEY_Up ? -1 : 0;
            if (!delta) return FALSE;
            if (auto* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(st->list), selected() + delta)) {
                gtk_list_box_select_row(GTK_LIST_BOX(st->list), row);
                gtk_widget_grab_focus(st->entry);  // selecting can move focus; keep typing in the entry
            }
            return TRUE;
        });
    gtk_widget_add_controller(st->entry, keys);

    auto* header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), st->entry);
    auto* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);
    adw_dialog_set_child(ADW_DIALOG(dialog), view);
    adw_dialog_set_focus(ADW_DIALOG(dialog), st->entry);
    fill();
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

std::vector<View> Window::sidebar_views() {
    std::vector<View> out;
    for (auto& s : kSmart) out.push_back(View{s.kind, ""});
    for (auto* l : store_->lists()) out.push_back(View{View::List, l->name});
    for (auto& t : store_->tags()) out.push_back(View{View::Tag, t});
    return out;
}

void Window::step_view(int delta) {
    if (!store_) return;
    auto views = sidebar_views();
    auto at = std::ranges::find(views, view_);
    auto n = static_cast<long>(views.size());
    long i = at == views.end() ? 0 : (at - views.begin() + delta + n) % n;
    select(views[static_cast<std::size_t>(i)]);
}

void Window::toggle_subtasks(const std::string& id) {
    if (!collapsed_.erase(id)) collapsed_.insert(id);
    focus_reminder_ = id;
    rebuild_content();
}

void Window::update_clamp() {
    if (!clamp_) return;
    int width = gtk_widget_get_width(content_scroller_);
    if (width <= 0) width = 860;  // not laid out yet
    int size = static_cast<int>(width * kContentWidthShare);
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp_), size);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp_), size);
}

void Window::focus_results() {
    // The entry waits a moment before reporting changes; catch up first.
    auto text = trim(gtk_editable_get_text(GTK_EDITABLE(search_entry_)));
    if (text.empty()) return;
    if (!(view_ == View{View::Search, text})) select(View{View::Search, text});
    show_content();
    if (!first_row_) return;
    auto keep = Obj<GtkWidget>::ref(first_row_);
    idle([keep] { gtk_widget_grab_focus(keep.get()); });
}

void Window::show_content() {
    auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
    if (adw_overlay_split_view_get_collapsed(split)) adw_overlay_split_view_set_show_sidebar(split, FALSE);
}

void Window::set_due(const std::string& id, int days_from_today) {
    undoable("Set Due Date", [&] {
        auto ref = store_->find(id);
        if (!ref) return;
        ref->reminder->due_date = rem::Date{std::chrono::sys_days{today()} + std::chrono::days{days_from_today}};
        try {
            store_->touch(id);  // keeps any time already set
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
    });
    focus_reminder_ = id;
    refresh();
}

void Window::undo() {
    if (!store_) return;
    auto result = history_.undo(*store_);
    after_history(result);
}

void Window::redo() {
    if (!store_) return;
    auto result = history_.redo(*store_);
    after_history(result);
}

void Window::after_history(const rem::History::Result& result) {
    if (!result.applied) return;
    for (auto& name : result.skipped)
        toast(std::format("“{}” was changed on another device, so it was left as it is", name));
    refresh();
    update_undo_actions();
}

void Window::update_undo_actions() {
    g_simple_action_set_enabled(undo_action_, history_.can_undo());
    g_simple_action_set_enabled(redo_action_, history_.can_redo());
}

void Window::update_banner() {
    auto& files = store_->candidates();
    if (files.empty()) {
        adw_banner_set_revealed(ADW_BANNER(banner_), FALSE);
        return;
    }
    auto title = files.size() == 1
                     ? std::format("“{}” has a checklist but isn't a list yet", files.front())
                     : std::format("{} Markdown files with checklists aren't lists yet", files.size());
    adw_banner_set_title(ADW_BANNER(banner_), title.c_str());
    adw_banner_set_revealed(ADW_BANNER(banner_), TRUE);
}

void Window::review_candidates() {
    auto files = store_->candidates();
    if (files.empty()) return;
    auto* dialog = adw_alert_dialog_new(
        files.size() == 1 ? "Use as a List?" : "Use as Lists?",
        "Adding a file puts “reminders: 1” at the top of it, so every synced device shows it as a list. "
        "Files you switch off won't be suggested again.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "later", "_Not Now", "add", "_Add", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "add", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "add");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "later");

    auto* rows = boxed_list();
    std::vector<std::pair<std::string, GtkWidget*>> switches;
    for (auto& name : files) {
        auto* row = adw_switch_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), (name + ".md").c_str());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
        gtk_list_box_append(GTK_LIST_BOX(rows), row);
        switches.emplace_back(name, row);
    }
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), rows);

    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [this, switches](AdwAlertDialog*, const char* response) {
            if (std::string_view(response) != "add") return;
            for (auto& [name, row] : switches) {
                try {
                    if (adw_switch_row_get_active(ADW_SWITCH_ROW(row))) store_->adopt(name);
                    else store_->decline(name);
                } catch (const std::exception& e) {
                    toast(std::format("Couldn't add “{}”: {}", name, e.what()));
                }
            }
            refresh();
        });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::check_notifications() {
    if (!store_) return;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    for (auto& ref : store_->all()) {
        auto& r = *ref.reminder;
        if (!r.due_date) continue;
        // All-day reminders notify at 09:00, as on iOS.
        auto t = r.due_time.value_or(rem::TimeOfDay{9, 0});
        GDateTime* due = g_date_time_new_local(static_cast<int>(r.due_date->year()),
                                               static_cast<int>(static_cast<unsigned>(r.due_date->month())),
                                               static_cast<int>(static_cast<unsigned>(r.due_date->day())), t.hour,
                                               t.minute, 0);
        gint64 at = g_date_time_to_unix(due);
        g_date_time_unref(due);
        if (at <= last_notify_check_ || at > now) continue;
        auto* n = g_notification_new(r.title.empty() ? "Reminder" : r.title.c_str());
        auto body = ref.list->name;
        if (!r.notes.empty()) body += " — " + r.notes.substr(0, r.notes.find('\n'));
        g_notification_set_body(n, body.c_str());
        g_notification_set_default_action_and_target(n, "app.show-reminder", "s", r.id.c_str());
        g_application_send_notification(G_APPLICATION(app_), ("reminder-" + r.id).c_str(), n);
        g_object_unref(n);
    }
    last_notify_check_ = now;
}

}  // namespace ui
