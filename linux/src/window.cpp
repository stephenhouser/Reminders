#include "window.hpp"

#include <algorithm>
#include <format>
#include <chrono>
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

namespace ui {

namespace {

constexpr guint kCompleteDelayMs = 900;  // a checked reminder lingers before it disappears
constexpr double kContentWidthShare = 1.0; //0.98;  // lists' width as a share of the content area

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
    {View::AllReminders, "All Reminders", "edit-select-all-symbolic", "gray"},  // completed too
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

// The shortcut that jumps to sidebar entry `index` (0-based), as GTK shows
// accelerators ("Ctrl+1" … "Ctrl+0"), or "" past the tenth.
std::string jump_shortcut(std::size_t index) {
    if (index >= 10) return {};
    return take_string(gtk_accelerator_get_label(GDK_KEY_0 + static_cast<guint>((index + 1) % 10), GDK_CONTROL_MASK));
}

GtkWidget* sidebar_row(const char* icon_name, std::string_view color, const std::string& title,
                       std::optional<int> count, const std::string& shortcut = {}) {
    auto* row = gtk_list_box_row_new();
    auto* box = hbox(12);
    auto* img = icon(icon_name, {"list-icon"});
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(img, color_class(color).c_str());
    auto* name = label(title);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    append(box, {img, name});
    if (!shortcut.empty()) append(box, {label(shortcut, {"dim-label", "caption", "sidebar-shortcut"})});
    // A fixed-width, right-aligned count column (empty for tags), so the
    // shortcut labels line up whatever the counts are.
    auto* count_label = label(count ? std::to_string(*count) : "", {"dim-label", "numeric", "sidebar-count"});
    gtk_label_set_xalign(GTK_LABEL(count_label), 1);
    append(box, {count_label});
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

// Every sidebar row carries its group, for the menu and Alt+↑/↓.
void set_row_group(GtkWidget* row, const rem::SidebarGroup& group) {
    g_object_set_data_full(G_OBJECT(row), "sidebar-group", new rem::SidebarGroup(group),
                           [](gpointer p) { delete static_cast<rem::SidebarGroup*>(p); });
}

std::optional<rem::SidebarGroup> row_group(GtkListBoxRow* row) {
    auto* g = row ? static_cast<rem::SidebarGroup*>(g_object_get_data(G_OBJECT(row), "sidebar-group")) : nullptr;
    if (!g) return std::nullopt;
    return *g;
}

// A collapsible group's heading, which folds or unfolds the group when
// clicked. "fold-group" marks it for the click handler.
GtkWidget* fold_heading(const std::string& text, bool collapsed, const rem::SidebarGroup& group) {
    auto* row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    auto* box = hbox(6);
    auto* l = label(text.c_str(), {"heading", "dim-label"});
    gtk_widget_set_hexpand(l, TRUE);
    append(box, {l, icon(collapsed ? "pan-end-symbolic" : "pan-down-symbolic", {"dim-label"})});
    gtk_widget_add_css_class(box, "sidebar-heading");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(row, std::format("{} {}", collapsed ? "Show" : "Hide", text).c_str());
    g_object_set_data(G_OBJECT(row), "fold-group", GINT_TO_POINTER(1));
    set_row_group(row, group);
    return row;
}

GtkWidget* sidebar_heading(const std::string& text) {
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
    //gtk_widget_add_css_class(list, "boxed-list");
    gtk_widget_add_css_class(list, "reminder-list");
    return list;
}

GMenu* menu_section(GMenu* menu) {
    auto* section = g_menu_new();
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
    return section;
}

// What's wrong with a CalDAV or WebDAV account's settings, or "".
std::string dav_problem(const rem::DavSettings& c) {
    if (c.url.empty()) return "Enter the server's address";
    if (!c.url.starts_with("https://") && !c.url.starts_with("http://")) return "The address starts with https://";
    if (c.url.find('.', c.url.find("://")) == std::string::npos && c.url.find("localhost") == std::string::npos &&
        c.url.find("127.0.0.1") == std::string::npos)
        return "That doesn't look like a server's address";
    return {};
}

// What's wrong with a source in Add Source / Source Info, or "". `self` is
// the source being edited ("" for a new one).
std::string source_problem(const SourceEdit& e, const std::string& self) {
    std::error_code ec;
    if (rem::has_server(e.backend)) {
#ifndef REMINDERS_NETWORK
        return "This copy of Reminders was built without CalDAV and WebDAV";
#endif
        if (auto p = dav_problem(e.dav); !p.empty()) return p;
    } else if (e.backend == rem::BackendKind::Git) {
#ifndef REMINDERS_NETWORK
        return "This copy of Reminders was built without git support";
#endif
        if (e.folder.empty()) return "Choose a folder";
    } else {
        if (e.folder.empty()) return "Choose a folder";
        if (!std::filesystem::is_directory(e.folder, ec)) return "That folder doesn't exist";
    }
    for (auto& s : rem::load_sources())
        if (s.name != self && (s.folder == e.folder || std::filesystem::equivalent(s.folder, e.folder, ec)))
            return std::format("That folder is already the source “{}”", rem::source_title(s));
    return {};
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

// Dragged reminders' ids travel as this private type rather than as plain
// text, so text fields (the "New Reminder" entry, a title being edited)
// don't accept the drop and paste the ids into themselves. Several when a
// selection is dragged, top to bottom.
using Ids = std::vector<std::string>;

GType reminder_drag_type() {
    static GType type = g_boxed_type_register_static(
        "RemindersReminderIds", [](gpointer p) -> gpointer { return new Ids(*static_cast<Ids*>(p)); },
        [](gpointer p) { delete static_cast<Ids*>(p); });
    return type;
}

const Ids* dragged_ids(const GValue* value) {
    if (!value || !G_VALUE_HOLDS(value, reminder_drag_type())) return nullptr;
    return static_cast<const Ids*>(g_value_get_boxed(value));
}

// The widget a controller is attached to; null once that widget is gone
// (a drop rebuilds the view, destroying the dragged row before "drag-end").
GtkWidget* owner(gpointer controller) {
    return gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
}

// `image` with a count badge in its top left corner, for dragging several.
Obj<GdkPaintable> with_badge(GtkWidget* widget, GdkPaintable* image, std::size_t count) {
    int w = gdk_paintable_get_intrinsic_width(image), h = gdk_paintable_get_intrinsic_height(image);
    auto* snap = gtk_snapshot_new();
    gdk_paintable_snapshot(image, snap, w, h);
    auto layout = Obj<PangoLayout>::adopt(gtk_widget_create_pango_layout(widget, std::to_string(count).c_str()));
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout.get(), &lw, &lh);
    float bh = static_cast<float>(lh + 4), bw = std::max(static_cast<float>(lw + 12), bh);
    graphene_rect_t r = GRAPHENE_RECT_INIT(4, 4, bw, bh);
    GskRoundedRect round;
    gsk_rounded_rect_init_from_rect(&round, &r, bh / 2);
    gtk_snapshot_push_rounded_clip(snap, &round);
    auto* accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
    gtk_snapshot_append_color(snap, accent, &r);
    gdk_rgba_free(accent);
    gtk_snapshot_pop(snap);
    gtk_snapshot_save(snap);
    graphene_point_t at = GRAPHENE_POINT_INIT(4 + (bw - static_cast<float>(lw)) / 2, 6);
    gtk_snapshot_translate(snap, &at);
    GdkRGBA white{1, 1, 1, 1};
    gtk_snapshot_append_layout(snap, layout.get(), &white);
    gtk_snapshot_restore(snap);
    graphene_size_t size = GRAPHENE_SIZE_INIT(static_cast<float>(w), static_cast<float>(h));
    return Obj<GdkPaintable>::adopt(gtk_snapshot_free_to_paintable(snap, &size));
}

// Rows carry their reminders' ids when dragged: `ids` gives them when the
// drag starts (the row's own, or the selection it's part of), and `mark`
// fades those rows while they're dragged (false: no longer).
void make_draggable(GtkWidget* row, std::function<Ids()> ids, std::function<void(const Ids&, bool)> mark) {
    auto* source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    auto dragging = std::make_shared<Ids>();
    connect<GdkContentProvider*(GtkDragSource*, double, double)>(
        source, "prepare", [ids, dragging](GtkDragSource*, double, double) {
            *dragging = ids();
            GValue value = G_VALUE_INIT;
            g_value_init(&value, reminder_drag_type());
            g_value_set_boxed(&value, dragging.get());
            auto* provider = gdk_content_provider_new_for_value(&value);
            g_value_unset(&value);
            return provider;
        });
    connect<void(GtkDragSource*, GdkDrag*)>(source, "drag-begin", [dragging, mark](GtkDragSource* s, GdkDrag*) {
        auto* row = owner(s);
        if (!row) return;
        // A still image of the row (the rows themselves fade while dragged).
        auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
        auto still = Obj<GdkPaintable>::adopt(gdk_paintable_get_current_image(live.get()));
        if (dragging->size() > 1) still = with_badge(row, still.get(), dragging->size());
        gtk_drag_source_set_icon(s, still.get(), 24, 20);
        gtk_widget_add_css_class(row, "dragging");
        mark(*dragging, true);
    });
    connect<void(GtkDragSource*, GdkDrag*, gboolean)>(
        source, "drag-end", [dragging, mark](GtkDragSource* s, GdkDrag*, gboolean) {
            if (auto* row = owner(s)) gtk_widget_remove_css_class(row, "dragging");
            mark(*dragging, false);
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
}

enum class DropStyle { Halves, Above, Into };

// Accepts dropped reminders on `row`, showing where they'd land.
// `self` is the row's own reminder id (it can't be dropped on itself).
void make_drop_target(GtkWidget* row, DropStyle style, std::string self,
                      std::function<void(Ids, rem::Document::Place)> on_drop) {
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
            auto* ids = dragged_ids(gtk_drop_target_get_value(t));
            if (ids && std::ranges::find(*ids, self) != ids->end()) return GdkDragAction(0);
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
            auto* ids = dragged_ids(value);
            if (!w || !ids || ids->empty() || std::ranges::find(*ids, self) != ids->end()) return FALSE;
            auto place = place_at(w, y);
            // Rebuilding the view destroys this row; do it after the drop finishes.
            idle([on_drop, ids = *ids, place] { on_drop(ids, place); });
            return TRUE;
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

// The local files in a drop, if it holds files.
std::vector<std::filesystem::path> dropped_files(const GValue* value) {
    std::vector<std::filesystem::path> out;
    if (!value || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return out;
    auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value));
    auto* files = gdk_file_list_get_files(list);
    for (auto* f = files; f; f = f->next)
        if (auto path = take_string(g_file_get_path(G_FILE(f->data))); !path.empty()) out.emplace_back(path);
    g_slist_free(files);
    return out;
}

// Accepts files dropped on `widget`; with `highlight`, the widget shows it
// while they're over it (a sidebar list).
void make_file_drop_target(GtkWidget* widget, bool highlight,
                           std::function<void(std::vector<std::filesystem::path>)> on_drop) {
    auto* target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    if (highlight) {
        connect<GdkDragAction(GtkDropTarget*, double, double)>(target, "motion", [](GtkDropTarget* t, double, double) {
            if (auto* w = owner(t)) gtk_widget_add_css_class(w, "drop-into");
            return GDK_ACTION_COPY;
        });
        connect<void(GtkDropTarget*)>(target, "leave", [](GtkDropTarget* t) {
            if (auto* w = owner(t)) gtk_widget_remove_css_class(w, "drop-into");
        });
    }
    connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
        target, "drop", [on_drop](GtkDropTarget* t, const GValue* value, double, double) -> gboolean {
            if (auto* w = owner(t)) gtk_widget_remove_css_class(w, "drop-into");
            auto files = dropped_files(value);
            if (files.empty()) return FALSE;
            // The dialog opens after the drop has finished.
            idle([on_drop, files = std::move(files)] { on_drop(files); });
            return TRUE;
        });
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(target));
}

// Sidebar entries carry their View when dragged, to be put in another place
// in their group.
GType entry_drag_type() {
    static GType type = g_boxed_type_register_static(
        "RemSidebarEntry", [](gpointer p) -> gpointer { return new View(*static_cast<View*>(p)); },
        [](gpointer p) { delete static_cast<View*>(p); });
    return type;
}

const View* dragged_entry(const GValue* value) {
    if (!value || !G_VALUE_HOLDS(value, entry_drag_type())) return nullptr;
    return static_cast<const View*>(g_value_get_boxed(value));
}

void make_entry_draggable(GtkWidget* row, const View& view) {
    auto* source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    connect<GdkContentProvider*(GtkDragSource*, double, double)>(
        source, "prepare", [view](GtkDragSource*, double, double) {
            GValue value = G_VALUE_INIT;
            g_value_init(&value, entry_drag_type());
            g_value_set_boxed(&value, &view);
            auto* provider = gdk_content_provider_new_for_value(&value);
            g_value_unset(&value);
            return provider;
        });
    connect<void(GtkDragSource*, GdkDrag*)>(source, "drag-begin", [](GtkDragSource* s, GdkDrag*) {
        auto* row = owner(s);
        if (!row) return;
        auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
        auto still = Obj<GdkPaintable>::adopt(gdk_paintable_get_current_image(live.get()));
        gtk_drag_source_set_icon(s, still.get(), 24, 16);
        gtk_widget_add_css_class(row, "dragging");
    });
    connect<void(GtkDragSource*, GdkDrag*, gboolean)>(source, "drag-end", [](GtkDragSource* s, GdkDrag*, gboolean) {
        if (auto* row = owner(s)) gtk_widget_remove_css_class(row, "dragging");
    });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
}

// Accepts entries dropped on `row` when `accepts`
// says they can go next to it, showing a line above or below.
void make_entry_drop_target(GtkWidget* row, std::function<bool(const View&)> accepts,
                            std::function<void(View, bool)> on_drop) {
    auto* target = gtk_drop_target_new(entry_drag_type(), GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(target, TRUE);
    auto below = [](GtkWidget* w, double y) { return y > gtk_widget_get_height(w) / 2.0; };
    auto clear = [](GtkWidget* w) {
        if (!w) return;
        for (auto* c : {"drop-above", "drop-below"}) gtk_widget_remove_css_class(w, c);
    };
    connect<GdkDragAction(GtkDropTarget*, double, double)>(
        target, "motion", [accepts, below, clear](GtkDropTarget* t, double, double y) {
            auto* w = owner(t);
            if (!w) return GdkDragAction(0);
            clear(w);
            auto* v = dragged_entry(gtk_drop_target_get_value(t));
            if (!v || !accepts(*v)) return GdkDragAction(0);
            gtk_widget_add_css_class(w, below(w, y) ? "drop-below" : "drop-above");
            return GDK_ACTION_MOVE;
        });
    connect<void(GtkDropTarget*)>(target, "leave", [clear](GtkDropTarget* t) { clear(owner(t)); });
    connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
        target, "drop",
        [accepts, below, clear, on_drop](GtkDropTarget* t, const GValue* value, double, double y) -> gboolean {
            auto* w = owner(t);
            clear(w);
            auto* v = dragged_entry(value);
            if (!w || !v || !accepts(*v)) return FALSE;
            // Rebuilding the sidebar destroys this row; do it after the drop finishes.
            idle([on_drop, v = *v, after = below(w, y)] { on_drop(v, after); });
            return TRUE;
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

// Groups are dragged by their heading and dropped on any row of another
// group: above it when over the group's top half, below it when over the
// bottom half. The rows carry their group (set_row_group).
GType group_drag_type() {
    static GType type = g_boxed_type_register_static(
        "RemSidebarGroup",
        [](gpointer p) -> gpointer { return new rem::SidebarGroup(*static_cast<rem::SidebarGroup*>(p)); },
        [](gpointer p) { delete static_cast<rem::SidebarGroup*>(p); });
    return type;
}

const rem::SidebarGroup* dragged_group(const GValue* value) {
    if (!value || !G_VALUE_HOLDS(value, group_drag_type())) return nullptr;
    return static_cast<const rem::SidebarGroup*>(g_value_get_boxed(value));
}

// The rows of `group` in the list box holding `row`, top to bottom.
std::vector<GtkWidget*> group_rows(GtkWidget* row, const rem::SidebarGroup& group) {
    std::vector<GtkWidget*> out;
    auto* list = row ? gtk_widget_get_parent(row) : nullptr;
    if (!list || !GTK_IS_LIST_BOX(list)) return out;
    for (int i = 0;; ++i) {
        auto* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i);
        if (!r) break;
        if (row_group(r) == group) out.push_back(GTK_WIDGET(r));
    }
    return out;
}

void make_group_draggable(GtkWidget* heading, const rem::SidebarGroup& group) {
    auto* source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    connect<GdkContentProvider*(GtkDragSource*, double, double)>(
        source, "prepare", [group](GtkDragSource*, double, double) {
            GValue value = G_VALUE_INIT;
            g_value_init(&value, group_drag_type());
            g_value_set_boxed(&value, &group);
            auto* provider = gdk_content_provider_new_for_value(&value);
            g_value_unset(&value);
            return provider;
        });
    connect<void(GtkDragSource*, GdkDrag*)>(source, "drag-begin", [group](GtkDragSource* s, GdkDrag*) {
        auto* row = owner(s);
        if (!row) return;
        auto live = Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(row));
        auto still = Obj<GdkPaintable>::adopt(gdk_paintable_get_current_image(live.get()));
        gtk_drag_source_set_icon(s, still.get(), 24, 12);
        for (auto* r : group_rows(row, group)) gtk_widget_add_css_class(r, "dragging");
    });
    connect<void(GtkDragSource*, GdkDrag*, gboolean)>(source, "drag-end", [group](GtkDragSource* s, GdkDrag*, gboolean) {
        for (auto* r : group_rows(owner(s), group)) gtk_widget_remove_css_class(r, "dragging");
    });
    gtk_widget_add_controller(heading, GTK_EVENT_CONTROLLER(source));
}

// Accepts groups dropped on `row`, a row of `group`, showing a line above
// the group's first row or below its last.
void make_group_drop_target(GtkWidget* row, const rem::SidebarGroup& group,
                            std::function<void(rem::SidebarGroup, bool)> on_drop) {
    auto* target = gtk_drop_target_new(group_drag_type(), GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(target, TRUE);
    // Below when the pointer is over the bottom half of the group's rows.
    auto below = [group](GtkWidget* w, double y) {
        auto rows = group_rows(w, group);
        auto at = std::ranges::find(rows, w);
        if (rows.empty() || at == rows.end()) return false;
        double h = std::max(1, gtk_widget_get_height(w));
        return (static_cast<double>(at - rows.begin()) + y / h) / static_cast<double>(rows.size()) >= 0.5;
    };
    auto clear = [group](GtkWidget* w) {
        for (auto* r : group_rows(w, group))
            for (auto* c : {"drop-above", "drop-below"}) gtk_widget_remove_css_class(r, c);
    };
    connect<GdkDragAction(GtkDropTarget*, double, double)>(
        target, "motion", [group, below, clear](GtkDropTarget* t, double, double y) {
            auto* w = owner(t);
            if (!w) return GdkDragAction(0);
            clear(w);
            auto* g = dragged_group(gtk_drop_target_get_value(t));
            if (!g || *g == group) return GdkDragAction(0);
            auto rows = group_rows(w, group);
            if (rows.empty()) return GdkDragAction(0);
            if (below(w, y)) gtk_widget_add_css_class(rows.back(), "drop-below");
            else gtk_widget_add_css_class(rows.front(), "drop-above");
            return GDK_ACTION_MOVE;
        });
    connect<void(GtkDropTarget*)>(target, "leave", [clear](GtkDropTarget* t) { clear(owner(t)); });
    connect<gboolean(GtkDropTarget*, const GValue*, double, double)>(
        target, "drop",
        [group, below, clear, on_drop](GtkDropTarget* t, const GValue* value, double, double y) -> gboolean {
            auto* w = owner(t);
            if (!w) return FALSE;
            clear(w);
            auto* g = dragged_group(value);
            if (!g || *g == group) return FALSE;
            // Rebuilding the sidebar destroys this row; do it after the drop finishes.
            idle([on_drop, g = *g, after = below(w, y)] { on_drop(g, after); });
            return TRUE;
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

constexpr std::pair<View::Kind, std::string_view> kViewNames[] = {
    {View::Today, "today"}, {View::Scheduled, "scheduled"}, {View::All, "all"},
    {View::Flagged, "flagged"}, {View::Completed, "completed"}, {View::AllReminders, "all-reminders"},
    {View::List, "list"},
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

// An edit of several reminders: one undo step, with saves held so each list
// is written once.
template <class F>
std::uint64_t Window::batch(const char* label, F&& f) {
    return undoable(label, [&] {
        store_->hold_saves();
        try {
            f();
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
        try {
            store_->release_saves();
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
    });
}

Window* Window::create(AdwApplication* app, std::optional<std::filesystem::path> folder) {
    auto* w = new Window(app, std::move(folder));
    attach(w->window_, "ui-window", std::unique_ptr<Window>(w));
    return w;
}

Window* Window::from(GtkWindow* window) {
    return window ? static_cast<Window*>(g_object_get_data(G_OBJECT(window), "ui-window")) : nullptr;
}

void Window::set_show_key_numbers(bool on) {
    show_key_numbers_ = on;
    key_numbers_override_ = on;
    if (store_) rebuild_sidebar();
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
    if (!settings_monitor_) watch_settings();
    auto file = Obj<GFile>::adopt(g_file_new_for_path(path.c_str()));
    auto* launcher = gtk_file_launcher_new(file.get());
    auto keep = Obj<GtkWindow>::ref(GTK_WINDOW(window_));
    gtk_file_launcher_launch(
        launcher, GTK_WINDOW(window_), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* holder = static_cast<Obj<GtkWindow>*>(data);
            GError* error = nullptr;
            if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error)) {
                bool dismissed = g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED);
                if (auto* self = Window::from(holder->get()); self && !dismissed)
                    self->toast(std::format("Couldn't open the settings file: {}", error->message));
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
    auto file = Obj<GFile>::adopt(g_file_new_for_path(rem::settings_file().c_str()));
    settings_monitor_ = Obj<GFileMonitor>::adopt(g_file_monitor_file(file.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr));
    if (!settings_monitor_) return;
    settings_handler_ = connect<void(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent)>(
        settings_monitor_.get(), "changed", [this](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent) {
            if (settings_timer_) g_source_remove(settings_timer_);
            settings_timer_ = timeout(300, [this] {
                settings_timer_ = 0;
                reload_settings();
                return false;
            });
        });
}

void Window::reload_settings() {
    std::string text;
    {
        std::ifstream in(rem::settings_file());
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (text == settings_text_) return;
    settings_text_ = std::move(text);
    show_key_numbers_ = key_numbers_override_.value_or(rem::load_bool_setting("show-key-numbers"));
    order_ = rem::load_sidebar_order(source_names());
    smart_ = rem::load_smart_lists_layout();
    lists_layouts_.clear();
    tags_ = rem::load_tags_layout();
    hidden_ = rem::load_hidden();
    if (show_hidden_action_) g_simple_action_set_state(show_hidden_action_, g_variant_new_boolean(hidden_.show));
    // Sources added, removed or changed (and this isn't a --folder session):
    // open them again.
    if (remember_view_) {
        auto configured = rem::load_sources();
        bool same = store_ && configured.size() == store_->sources().size();
        for (std::size_t i = 0; same && i < configured.size(); ++i) {
            auto& open = store_->sources()[i].config;
            same = configured[i].name == open.name && configured[i].folder == open.folder &&
                   configured[i].backend == open.backend && configured[i].title == open.title;
        }
        if (!same) return open_sources();
    }
    if (!store_) return;
    // The view may have just been hidden.
    auto smart = smart_views();
    bool gone = (smart_info(view_.kind) && std::ranges::find(smart, view_) == smart.end()) ||
                (view_.kind == View::Tag && tags_.hidden()) || (!hidden_.show && entry_hidden(view_));
    if (gone) return select(home_view());
    // Rebuilding replaces the sidebar's rows: keep keyboard focus on the same one.
    std::optional<View> focused;
    std::optional<rem::SidebarGroup> focused_heading;
    for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w; w = gtk_widget_get_parent(w))
        if (GTK_IS_LIST_BOX_ROW(w) && gtk_widget_get_parent(w) == sidebar_list_) {
            if (auto* v = row_view(GTK_LIST_BOX_ROW(w))) focused = *v;
            else focused_heading = row_group(GTK_LIST_BOX_ROW(w));
            break;
        }
    rebuild_sidebar();
    if (!focused && !focused_heading) return;
    for (int i = 0;; ++i) {
        auto* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list_), i);
        if (!row) break;
        auto* v = row_view(row);
        if (focused ? v && *v == *focused : !v && row_group(row) == focused_heading) {
            gtk_widget_grab_focus(GTK_WIDGET(row));
            break;
        }
    }
}

void Window::show_reminder(const std::string& id) {
    if (!store_) return;
    auto ref = store_->find(id);
    if (!ref) return;
    select(View{View::List, store_->key_of(*ref->list)});
    show_content();
    show_details(id);
}

Window::Window(AdwApplication* app, std::optional<std::filesystem::path> folder)
    : app_(app), show_key_numbers_(rem::load_bool_setting("show-key-numbers")),
      smart_(rem::load_smart_lists_layout()), tags_(rem::load_tags_layout()), hidden_(rem::load_hidden()) {
    build();
    add_actions();
    // Ctrl+A and Escape for the reminder selection, wherever the focus is in
    // the window: caught before the focused widget sees them, except by text
    // being typed (a title, New Reminder, search), menus and dialogs, which
    // keep their own.
    auto* selection_keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(selection_keys, GTK_PHASE_CAPTURE);
    connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
        selection_keys, "key-pressed", [this](GtkEventControllerKey*, guint keyval, guint, GdkModifierType mods) -> gboolean {
            if (!store_ || adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window_))) return FALSE;
            if (auto* f = gtk_root_get_focus(GTK_ROOT(window_));
                f && (GTK_IS_TEXT(f) || GTK_IS_TEXT_VIEW(f) || gtk_widget_get_ancestor(f, GTK_TYPE_POPOVER)))
                return FALSE;
            auto mask = mods & gtk_accelerator_get_default_mod_mask();
            if (mask == GDK_CONTROL_MASK && gdk_keyval_to_lower(keyval) == GDK_KEY_a && !shown_ids_.empty()) {
                select_all();
                return TRUE;
            }
            if (mask == 0 && keyval == GDK_KEY_Escape && !selected_.empty()) {
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
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(outside), 0);  // any button
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(outside), GTK_PHASE_CAPTURE);
    connect<void(GtkGestureClick*, int, double, double)>(
        outside, "pressed", [this](GtkGestureClick* g, int, double x, double y) {
            if (!store_ || adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window_))) return;
            auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(g));
            if (!event || gdk_event_get_surface(event) != gtk_native_get_surface(GTK_NATIVE(window_))) return;  // a menu
            auto mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g)) &
                        gtk_accelerator_get_default_mod_mask();
            clicked(gtk_widget_pick(window_, x, y, GTK_PICK_DEFAULT), mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK));
        });
    gtk_widget_add_controller(window_, GTK_EVENT_CONTROLLER(outside));
    // A file dropped anywhere else is imported, into the list in view.
    make_file_drop_target(window_, false, [this](std::vector<std::filesystem::path> files) {
        if (!store_) return;
        import_files(std::move(files), view_.kind == View::List ? view_.name : std::string());
    });
    {
        std::ifstream in(rem::settings_file());  // as read at start-up
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
    for (auto id : {reload_timer_, refresh_timer_, notify_timer_, autoscroll_timer_, settings_timer_})
        if (id) g_source_remove(id);
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
    g_menu_append(s1, "_Import…", "win.import");
    g_menu_append(s1, "_Export…", "win.export");
    g_menu_append(s1, "S_ources…", "win.sources");
    auto* sync_item = g_menu_item_new("S_ync Now", "win.sync-now");
    g_menu_item_set_attribute(sync_item, "hidden-when", "s", "action-disabled");  // no CalDAV, WebDAV or git sources
    g_menu_append_item(s1, sync_item);
    g_object_unref(sync_item);
    g_menu_append(menu_section(primary_menu), "Show _Hidden Lists", "win.show-hidden");
    auto* s2 = menu_section(primary_menu);
    g_menu_append(s2, "_Settings…", "win.settings");
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
        else if (view_.kind == View::Search) select(home_view());
    });

    sidebar_list_ = gtk_list_box_new();
    gtk_widget_add_css_class(sidebar_list_, "navigation-sidebar");
    connect<void(GtkListBox*, GtkListBoxRow*)>(sidebar_list_, "row-activated",
                                               [this](GtkListBox*, GtkListBoxRow* row) {
                                                   if (updating_sidebar_) return;
                                                   if (g_object_get_data(G_OBJECT(row), "fold-group")) {
                                                       if (auto g = row_group(row)) toggle_fold(*g);
                                                       return;
                                                   }
                                                   if (auto* v = row_view(row)) {
                                                       select(*v);
                                                       show_content();
                                                   }
                                               });
    // Right-click or long-press: a menu to move the group up or down.
    auto* sidebar_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(sidebar_click), GDK_BUTTON_SECONDARY);
    connect<void(GtkGestureClick*, int, double, double)>(
        sidebar_click, "pressed", [this](GtkGestureClick*, int, double x, double y) {
            sidebar_menu(gtk_list_box_get_row_at_y(GTK_LIST_BOX(sidebar_list_), static_cast<int>(y)), x, y);
        });
    gtk_widget_add_controller(sidebar_list_, GTK_EVENT_CONTROLLER(sidebar_click));
    auto* sidebar_press = gtk_gesture_long_press_new();
    connect<void(GtkGestureLongPress*, double, double)>(
        sidebar_press, "pressed", [this](GtkGestureLongPress*, double x, double y) {
            sidebar_menu(gtk_list_box_get_row_at_y(GTK_LIST_BOX(sidebar_list_), static_cast<int>(y)), x, y);
        });
    gtk_widget_add_controller(sidebar_list_, GTK_EVENT_CONTROLLER(sidebar_press));
    // Alt+↑ / Alt+↓ (and with Shift) on a sidebar row move it or its group.
    auto* sidebar_keys = gtk_event_controller_key_new();
    connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
        sidebar_keys, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType mods) -> gboolean {
            // Alt+↑/↓ moves the entry within its group (on a heading, the
            // group); Alt+Shift+↑/↓ moves the group. The same keys as the TUI.
            auto mask = mods & gtk_accelerator_get_default_mod_mask();
            bool group_keys = mask == (GDK_ALT_MASK | GDK_SHIFT_MASK);
            if (mask != GDK_ALT_MASK && !group_keys) return FALSE;
            if (key != GDK_KEY_Up && key != GDK_KEY_Down) return FALSE;
            auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
            while (focus && !GTK_IS_LIST_BOX_ROW(focus)) focus = gtk_widget_get_parent(focus);
            int delta = key == GDK_KEY_Up ? -1 : 1;
            auto* v = focus ? row_view(GTK_LIST_BOX_ROW(focus)) : nullptr;
            if (v && !group_keys) {
                auto view = *v;
                idle([this, view, delta] { move_entry(view, delta); });
            } else if (auto g = row_group(GTK_LIST_BOX_ROW(focus))) {
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
        paste_keys, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType mods) -> gboolean {
            if ((mods & gtk_accelerator_get_default_mod_mask()) != GDK_CONTROL_MASK) return FALSE;
            if (gdk_keyval_to_lower(key) != GDK_KEY_v) return FALSE;
            auto* focus = gtk_root_get_focus(GTK_ROOT(window_));
            if (focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus))) return FALSE;
            paste_reminders();
            return TRUE;
        });
    gtk_widget_add_controller(window_, paste_keys);
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
    // An invisible menu button in the corner hosts the sidebar's context menu.
    sidebar_menu_button_ = gtk_menu_button_new();
    gtk_widget_set_halign(sidebar_menu_button_, GTK_ALIGN_START);
    gtk_widget_set_valign(sidebar_menu_button_, GTK_ALIGN_START);
    gtk_widget_set_opacity(sidebar_menu_button_, 0);
    gtk_widget_set_can_target(sidebar_menu_button_, FALSE);
    gtk_widget_set_can_focus(sidebar_menu_button_, FALSE);
    gtk_accessible_update_state(GTK_ACCESSIBLE(sidebar_menu_button_), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
    auto* sidebar_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(sidebar_overlay), sidebar_scroller);
    gtk_overlay_add_overlay(GTK_OVERLAY(sidebar_overlay), sidebar_menu_button_);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebar_view), sidebar_overlay);
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
    g_menu_append(m2, "_Export…", "win.export-list");
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
    // As over the sidebar, an invisible menu button hosts a reminder's
    // context menu, which opens where the row was clicked.
    content_menu_button_ = gtk_menu_button_new();
    gtk_widget_set_halign(content_menu_button_, GTK_ALIGN_START);
    gtk_widget_set_valign(content_menu_button_, GTK_ALIGN_START);
    gtk_widget_set_opacity(content_menu_button_, 0);
    gtk_widget_set_can_target(content_menu_button_, FALSE);
    gtk_widget_set_can_focus(content_menu_button_, FALSE);
    gtk_accessible_update_state(GTK_ACCESSIBLE(content_menu_button_), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
    auto* content_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(content_overlay), content_scroller_);
    gtk_overlay_add_overlay(GTK_OVERLAY(content_overlay), content_menu_button_);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_view), content_overlay);
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

    // Whether the sidebar is shown is saved (show-sidebar), shared with the
    // TUI. Only while the window is wide enough to show it beside the
    // content: on narrow windows it hides by itself and slides over.
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_), rem::load_bool_setting("show-sidebar", true));
    // ("notify" passes the property as well, so not on(), which is for signals
    // that pass only the emitter.)
    connect<void(GObject*, GParamSpec*)>(split_, "notify::show-sidebar", [this](GObject*, GParamSpec*) {
        auto* split = ADW_OVERLAY_SPLIT_VIEW(split_);
        if (adw_overlay_split_view_get_collapsed(split)) return;
        bool shown = adw_overlay_split_view_get_show_sidebar(split);
        if (rem::load_bool_setting("show-sidebar", true) == shown) return;
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
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), toasts_);
}

void Window::add_actions() {
    add_action(window_, "change-folder", [this] { choose_folder(); });
    add_action(window_, "add-source", [this] { add_source(); });
    add_action(window_, "sources", [this] { show_sources(); });
    add_action(window_, "import", [this] {
        if (store_) import_file();
    });
    sync_action_ = add_action(window_, "sync-now", [this] {
#ifdef REMINDERS_NETWORK
        if (sync_) sync_->sync_now();
#endif
    });
    g_simple_action_set_enabled(sync_action_, FALSE);
    add_action(window_, "settings", [this] { open_settings(); });
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
    // Ctrl+E: hide every reminder's subtasks, or show them all if any are hidden.
    add_action(window_, "toggle-subtasks", [this] {
        if (!store_) return;
        if (!collapsed_.empty()) {
            collapsed_.clear();
        } else {
            for (auto* l : store_->lists())
                for (auto* r : l->doc.reminders())
                    if (!r->subtasks.empty()) collapsed_.insert(r->id);
        }
        rebuild_content();
    });
    // Main menu: show the lists, smart lists and tags hidden from the sidebar
    // (dimmed), so they can be opened or unhidden.
    show_hidden_action_ = add_toggle(window_, "show-hidden", hidden_.show, [this](bool on) {
        hidden_.show = on;
        try {
            rem::save_show_hidden(on);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save the setting: {}", e.what()));
        }
        if (!on && entry_hidden(view_)) select(home_view());
        rebuild_sidebar();
    });
    add_action(window_, "move-group-up", [this] { move_group(menu_group_, -1); });
    add_action(window_, "move-group-down", [this] { move_group(menu_group_, 1); });
    // The sidebar menu's "Collapsible" check item: visible <-> collapsible.
    collapsible_action_ = add_toggle(window_, "group-collapsible", false, [this](bool on) {
        auto* l = layout_of(menu_group_);
        auto& display = l ? l->display : smart_.display;
        bool& collapsed = l ? l->collapsed : smart_.collapsed;
        display = on ? rem::GroupDisplay::Collapsible : rem::GroupDisplay::Visible;
        collapsed = false;  // a group made collapsible starts unfolded
        try {
            rem::save_group_display(menu_group_, display);
            rem::save_group_collapsed(menu_group_, false);
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
    add_action(window_, "export", [this] {  // ☰: the list in view ticked, if any
        if (store_) export_lists(view_.kind == View::List ? std::vector{view_.name} : std::vector<std::string>{});
    });
    add_action(window_, "export-list", [this] {
        if (view_.kind == View::List) export_lists({view_.name});
    });
    show_completed_action_ = add_toggle(window_, "show-completed", false, [this](bool on) {
        show_completed_ = on;
        rebuild_content();
    });
}

// --- folder ----------------------------------------------------------------

void Window::choose_folder() {
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose Folder");
    if (store_ && !store_->sources().empty()) {
        auto current = Obj<GFile>::adopt(g_file_new_for_path(store_->sources().front().config.folder.c_str()));
        gtk_file_dialog_set_initial_folder(dialog, current.get());
    }
    gtk_file_dialog_select_folder(
        dialog, GTK_WINDOW(window_), nullptr,
        [](GObject* source, GAsyncResult* res, gpointer) {
            auto* self = static_cast<Window*>(g_object_get_data(G_OBJECT(source), "window"));
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
        nullptr);
    g_object_set_data(G_OBJECT(dialog), "window", this);
    g_object_unref(dialog);
}

void Window::open_folder(const std::filesystem::path& folder) {
    // A configured source's folder becomes the default; any other folder
    // becomes the default source's folder (created if there's none).
    try {
        auto source = rem::source_for_folder(folder);
        if (source.name.empty()) rem::set_default_folder(folder);
        else rem::save_setting("default-source", source.name);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't save the folder: {}", e.what()));
    }
    open_sources();
}

// Removes a source from the app (settings.ini), after asking. Its folder and
// files stay as they are.
void Window::remove_source(const std::string& name) {
    std::string title = name;
    for (auto& s : store_->sources())
        if (s.config.name == name) title = rem::source_title(s.config);
    auto* dialog = adw_alert_dialog_new(std::format("Remove “{}”?", title).c_str(),
                                        "Its lists leave the app. The folder and its files stay where they are, "
                                        "and you can add it again with Add Source….");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "remove", "_Remove", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    connect<void(AdwAlertDialog*, const char*)>(dialog, "response", [this, name](AdwAlertDialog*, const char* response) {
        if (std::string_view(response) != "remove") return;
        try {
            rem::remove_source(name);
        } catch (const std::exception& e) {
            toast(std::format("Couldn't remove the source: {}", e.what()));
            return;
        }
        idle([this] { open_sources(); });
    });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::source_info(const std::string& name) {
    std::optional<rem::SourceConfig> config;
    for (auto& s : rem::load_sources())
        if (s.name == name) config = s;
    if (!config) return;
    SourceEdit edit{config->name, config->title, config->backend, config->folder,
                    rem::load_setting("default-source") == name || (store_ && store_->default_source() == name),
                    config->dav, config->git, false};
    show_source_dialog(
        window_, edit, [name](const SourceEdit& e) { return source_problem(e, name); },
        [this](SourceEdit e) {
            try {
                rem::save_source(rem::SourceConfig{e.name, e.backend, e.folder, e.title, e.dav, e.git});
                if (e.title.empty()) rem::save_section_setting("source." + e.name, "title", "");
                if (e.is_default) rem::save_setting("default-source", e.name);
            } catch (const std::exception& err) {
                toast(std::format("Couldn't save the source: {}", err.what()));
                return;
            }
            open_sources();
        },
        [this, name] { idle([this, name] { remove_source(name); }); });
}

void Window::show_sources() {
    std::vector<SourceRow> rows;
    for (auto& s : rem::load_sources()) {
        auto folder = rem::contract_path(s.folder);  // ~/… for folders in the home folder
        auto detail = s.backend == rem::BackendKind::Caldav   ? std::format("CalDAV · {}", s.dav.url)
                    : s.backend == rem::BackendKind::Webdav ? std::format("WebDAV · {}", s.dav.url)
                    : s.backend == rem::BackendKind::Git    ? std::format("Git · {}", folder)
                    : std::format("{} · {}", s.backend == rem::BackendKind::Local ? "Local folder" : "Syncthing", folder);
        rows.push_back({s.name, rem::source_title(s), detail});
    }
    show_sources_dialog(
        window_, rows, [this](std::string name) { idle([this, name] { source_info(name); }); },
        [this] { idle([this] { add_source(); }); });
}

void Window::add_source() {
    SourceEdit edit;
    edit.is_new = true;
    edit.is_default = rem::load_sources().empty();
    show_source_dialog(
        window_, edit, [](const SourceEdit& e) { return source_problem(e, ""); },
        [this](SourceEdit e) {
            try {
                if (!rem::has_server(e.backend)) std::filesystem::create_directories(e.folder);
                auto config = rem::add_source(rem::SourceConfig{"", e.backend, e.folder, e.title, e.dav, e.git});
                if (e.is_default) rem::save_setting("default-source", config.name);
                toast(rem::syncs(e.backend)
                          ? std::format("Added “{}”; its lists appear once it has synced", rem::source_title(config))
                          : std::format("Added “{}”", rem::source_title(config)));
            } catch (const std::exception& err) {
                toast(std::format("Couldn't add the source: {}", err.what()));
                return;
            }
            open_sources();
        },
        nullptr);
}

void Window::start_sync() {
#ifdef REMINDERS_NETWORK
    stop_sync();
    if (!store_) return;
    sync_ = std::make_unique<rem::SyncRunner>(*store_);
    g_simple_action_set_enabled(sync_action_, sync_->active());
    if (!sync_->active()) return;
    sync_timer_ = timeout(1000, [this] {
        auto status = sync_->take_status();
        // The same problem every few minutes (offline, say) is shown once.
        if (!status.errors.empty() && status.errors.back() != last_sync_error_) {
            last_sync_error_ = status.errors.back();
            toast("Couldn't sync " + last_sync_error_);
        }
        if (status.errors.empty() && status.last_sync) last_sync_error_.clear();
        return true;
    });
#endif
}

void Window::stop_sync() {
#ifdef REMINDERS_NETWORK
    if (sync_timer_) g_source_remove(sync_timer_);
    sync_timer_ = 0;
    sync_.reset();  // waits for a sync under way
#endif
}

void Window::stop_watching() {
    for (auto& w : monitors_) {
        g_signal_handler_disconnect(w.monitor.get(), w.handler);
        g_file_monitor_cancel(w.monitor.get());
    }
    monitors_.clear();
}

void Window::open_sources(std::optional<std::filesystem::path> folder) {
    std::unique_ptr<rem::Library> library;
    try {
        library = rem::open_library(folder, device_name());
        library->load_all();
    } catch (const std::exception& e) {
        toast(std::format("Couldn't open the lists: {}", e.what()));
        return;
    }
    stop_sync();
    stop_watching();
    store_ = std::move(library);
    history_.clear();  // steps refer to the old lists
    update_undo_actions();
    order_ = rem::load_sidebar_order(source_names());
    lists_layouts_.clear();
    // A folder that isn't a configured source is for this session only: the
    // saved view belongs to the configured ones.
    bool remember = !folder || (!store_->sources().empty() && !store_->sources().front().config.name.empty());
    remember_view_ = remember;
    if (store_->sources().empty()) {
        gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "welcome");
        return;
    }

    // Changes made elsewhere (Syncthing, an editor, the TUI) show up.
    for (auto& source : store_->sources()) {
        auto dir = Obj<GFile>::adopt(g_file_new_for_path(source.config.folder.c_str()));
        GError* error = nullptr;
        FolderWatch w;
        w.monitor = Obj<GFileMonitor>::adopt(g_file_monitor_directory(dir.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, &error));
        if (error) {
            toast(std::format("Changes to “{}” from elsewhere won't show until restart: can't watch its folder",
                              rem::source_title(source.config)));
            g_error_free(error);
            continue;
        }
        w.handler = connect<void(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent)>(
            w.monitor.get(), "changed",
            [this](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent) { on_file_changed(file, other); });
        monitors_.push_back(std::move(w));
    }
    start_sync();

    gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "main");
    view_ = remember ? view_from_string(load_last_view()) : home_view();
    if (view_.kind == View::List) {  // "source/name", or a bare name only one source has
        auto* l = store_->list(view_.name);
        view_ = l ? View{View::List, store_->key_of(*l)} : home_view();
    }
    if (smart_info(view_.kind)) {  // a smart list the settings hide
        auto smart = smart_views();
        if (std::ranges::find(smart, view_) == smart.end()) view_ = home_view();
    }
    if (view_.kind == View::Tag && tags_.hidden()) view_ = home_view();
    if (!hidden_.show && entry_hidden(view_)) view_ = home_view();  // hidden in the sidebar
    refresh();
}

void Window::on_file_changed(GFile* file, GFile* other) {
    for (auto* f : {file, other}) {
        if (!f) continue;
        auto path = take_string(g_file_get_path(f));
        if (auto key = store_->key_for_path(path)) pending_reload_.insert(*key);
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
    if (view_.kind == View::List && !store_->list(view_.name)) view_ = home_view();
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
    if (!(v == view_)) {
        selected_.clear();
        anchor_.reset();
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
    // Optional "Ctrl+1"-style labels after each name (show-key-numbers),
    // numbered in display order over the entries that are showing.
    std::size_t index = 0;
    auto shortcut = [this](std::size_t i) { return show_key_numbers_ ? jump_shortcut(i) : std::string(); };

    // Entries are dragged to another place in their group.
    auto make_reorderable = [this](GtkWidget* row, const View& v) {
        make_entry_draggable(row, v);
        make_entry_drop_target(
            row,
            [this, v](const View& dragged) {
                if (!same_sidebar_group(dragged, v)) return false;
                auto t = entry_order(v);  // a hidden smart list has no place
                return t && std::ranges::find(t->order, t->name) != t->order.end();
            },
            [this, v](View dragged, bool after) { drop_entry(dragged, v, after); });
    };

    // The group at the top has no heading unless it can be folded; the
    // groups below it have one.
    bool first = true;
    for (auto g : showing_groups()) {
        // Every row of the group takes a dragged group; the heading drags it.
        auto add = [&](GtkWidget* row) {
            set_row_group(row, g);
            make_group_drop_target(row, g, [this, g](rem::SidebarGroup dragged, bool after) {
                drop_group(dragged, g, after);
            });
            gtk_list_box_append(list, row);
        };
        if (group_foldable(g)) {
            auto* heading = fold_heading(group_title(g), group_folded(g), g);
            make_group_draggable(heading, g);
            add(heading);
        } else if (!first) {
            auto* heading = sidebar_heading(group_title(g));
            make_group_draggable(heading, g);
            add(heading);
        }
        first = false;
        if (group_folded(g)) continue;
        switch (g.kind) {
            case rem::SidebarGroup::SmartLists:
                for (auto& v : smart_views()) {
                    auto* s = smart_info(v.kind);
                    std::size_t count = 0;
                    switch (v.kind) {
                        case View::Today: count = store_->today(day).size(); break;
                        case View::Scheduled: count = store_->scheduled().size(); break;
                        case View::All: count = store_->all().size(); break;
                        case View::AllReminders: count = store_->everything().size(); break;
                        case View::Flagged: count = store_->flagged().size(); break;
                        case View::Completed: count = store_->completed().size(); break;
                        default: break;
                    }
                    auto* row = sidebar_row(s->icon, s->color, s->title, static_cast<int>(count), shortcut(index++));
                    set_row_view(row, v);
                    if (entry_hidden(v)) gtk_widget_add_css_class(row, "hidden-entry");
                    make_reorderable(row, v);
                    add(row);
                }
                break;
            case rem::SidebarGroup::Lists:  // one source's lists
                for (auto* l : sidebar_lists(g.source)) {
                    auto key = store_->key_of(*l);
                    auto* row = sidebar_row(list_icon_name(l->icon()), l->color(), l->name, open_count(*l), shortcut(index++));
                    set_row_view(row, View{View::List, key});
                    if (hidden_.list_hidden(key)) gtk_widget_add_css_class(row, "hidden-entry");
                    make_reorderable(row, View{View::List, key});
                    make_drop_target(row, DropStyle::Into, "", [this, key](Ids dropped, rem::Document::Place) {
                        move_to_list(dropped, key);
                    });
                    make_file_drop_target(row, true, [this, key](std::vector<std::filesystem::path> files) {
                        import_files(std::move(files), key);
                    });
                    add(row);
                }
                break;
            case rem::SidebarGroup::Tags:
                for (auto& t : sidebar_tags()) {
                    auto style = rem::load_tag_style(t);
                    auto* row = sidebar_row(list_icon_name(style.icon), style.color, "#" + t, std::nullopt, shortcut(index++));
                    set_row_view(row, View{View::Tag, t});
                    if (hidden_.tag_hidden(t)) gtk_widget_add_css_class(row, "hidden-entry");
                    make_reorderable(row, View{View::Tag, t});
                    add(row);
                }
                break;
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
    shown_ids_.clear();  // build_reminder_row adds each row
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
        int total = 0, done = 0;
        l->doc.walk([&](rem::Reminder& r, rem::Reminder*) {
            ++total;
            done += r.done;
        });
        count_subtitle_ = rem::count_label(rem::CountStyle::WithComplete, total, done);
        body = build_list_view(*l);
    } else {
        if (auto* s = smart_info(view_.kind)) page_title = s->title;
        else if (view_.kind == View::Tag) page_title = "#" + view_.name;
        else page_title = "Search";
        // The same kind of count as a list's: what the view contains.
        auto refs = view_refs();
        int done = static_cast<int>(std::ranges::count_if(refs, [](auto& r) { return r.reminder->done; }));
        int total = static_cast<int>(refs.size());
        auto style = view_.kind == View::Search                                              ? rem::CountStyle::Results
                     : view_.kind == View::Completed                                         ? rem::CountStyle::Completed
                     : view_.kind == View::Tag || view_.kind == View::AllReminders ? rem::CountStyle::WithComplete
                                                                                             : rem::CountStyle::OpenOnly;
        count_subtitle_ = rem::count_label(style, total, done);
        body = build_smart_view();
    }
    adw_window_title_set_title(title, page_title.c_str());
    adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(content_page_), page_title.c_str());
    // Selected reminders no longer shown (completed and hidden, deleted
    // elsewhere) drop out of the selection.
    std::erase_if(selected_, [this](const std::string& id) { return !reminder_rows_.contains(id); });
    update_selection();  // also sets the subtitle

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
    auto list = store_->key_of(l);  // "source/name": a bare name is ambiguous when two sources have it
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
    for (auto& section : l.doc.sections()) {
        auto* listbox = boxed_list();
        for (auto* r : section.reminders) {
            if (r->done && !show_completed_) continue;
            gtk_list_box_append(GTK_LIST_BOX(listbox), build_reminder_row(rem::Ref{&l, r, nullptr}, false));
            if (collapsed_.contains(r->id)) continue;
            for (auto& s : r->subtasks) {
                if (s.done && !show_completed_) continue;
                gtk_list_box_append(GTK_LIST_BOX(listbox), build_reminder_row(rem::Ref{&l, &s, r}, false));
            }
        }
        gtk_list_box_append(GTK_LIST_BOX(listbox), build_new_row(store_->key_of(l), section.name));
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
    return clamp(page);
}

std::vector<rem::Ref> Window::view_refs() {
    auto day = today();
    std::vector<rem::Ref> refs;
    switch (view_.kind) {
        case View::Today: refs = store_->today(day); break;
        case View::Scheduled: refs = store_->scheduled(); break;
        case View::All: refs = store_->all(); break;
        case View::AllReminders: refs = store_->everything(); break;
        case View::Flagged: refs = store_->flagged(); break;
        case View::Completed: refs = store_->completed(); break;
        case View::Tag: refs = store_->tagged(view_.name); break;
        case View::Search: refs = store_->search(view_.name); break;
        case View::List: break;
    }
    return refs;
}

GtkWidget* Window::build_smart_view() {
    auto day = today();
    auto refs = view_refs();

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
            box = group_for(store_->label(*ref.list), ref.list->color());
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
    g_object_set_data_full(G_OBJECT(row), "reminder-id", g_strdup(id.c_str()), g_free);  // for paste
    shown_ids_.push_back(id);
    reminder_rows_[id] = row;
    if (selected_.contains(id)) gtk_widget_add_css_class(row, "selected-reminder");

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
        if (!syncing_checks_) toggle_done(id, gtk_check_button_get_active(c));
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
        // The URL itself, as in the terminal client; long ones are shortened
        // with "…" (the tooltip has the full one).
        auto* link = gtk_link_button_new_with_label(r.url->c_str(), r.url->c_str());
        if (auto* text = gtk_button_get_child(GTK_BUTTON(link)); GTK_IS_LABEL(text)) {
            gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(text), 50);
        }
        gtk_widget_add_css_class(link, "caption");
        gtk_widget_add_css_class(link, "inline-link");
        add_meta(link);
    }
    if (show_list || ref.parent) {
        std::string where = show_list ? store_->label(*ref.list) : "";
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

    // Per-row actions: the "more" menu, the context menu and keyboard
    // shortcuts. Most act on targets(id): the selection, when this row is
    // part of it.
    bool in_list = view_.kind == View::List;
    bool can_indent = in_list && !ref.parent && r.subtasks.empty(), can_outdent = in_list && ref.parent;
    auto* actions = g_simple_action_group_new();
    add_action(actions, "details", [this, id] { show_details(id); });
    add_action(actions, "complete", [this, id] {
        focus_reminder_ = id;
        idle([this, ids = targets(id)] { complete_reminders(ids); });
    });
    add_action(actions, "flag", [this, id] { idle([this, ids = targets(id)] { toggle_flag(ids); }); });
    add_action(actions, "copy", [this, id] { copy_reminders(targets(id)); });
    add_action(actions, "due-today", [this, id] { idle([this, ids = targets(id)] { set_due(ids, 0); }); });
    add_action(actions, "due-tomorrow", [this, id] { idle([this, ids = targets(id)] { set_due(ids, 1); }); });
    add_action(actions, "delete", [this, id] { idle([this, ids = targets(id)] { delete_reminders(ids); }); });
    auto* indent_action = add_action(actions, "indent", [this, id] { idle([this, id] { indent(id, true); }); });
    auto* outdent_action = add_action(actions, "outdent", [this, id] { idle([this, id] { indent(id, false); }); });
    g_simple_action_set_enabled(indent_action, can_indent);
    g_simple_action_set_enabled(outdent_action, can_outdent);
    {
        auto* move_to = g_simple_action_new("move-to", G_VARIANT_TYPE_STRING);
        connect<void(GSimpleAction*, GVariant*)>(move_to, "activate", [this, id](GSimpleAction*, GVariant* v) {
            idle([this, ids = outermost(targets(id)), key = std::string(g_variant_get_string(v, nullptr))] {
                move_to_list(ids, key);
            });
        });
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(move_to));
        g_object_unref(move_to);
    }
    gtk_widget_insert_action_group(row, "reminder", G_ACTION_GROUP(actions));
    // Kept for the context menu, whose popover isn't inside the row.
    g_object_set_data_full(G_OBJECT(row), "reminder-actions", actions, g_object_unref);

    // The menu is made as it opens, for what it will act on then.
    auto* more = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
    using MakeMenu = std::function<void(GtkMenuButton*)>;
    gtk_menu_button_set_create_popup_func(
        GTK_MENU_BUTTON(more), [](GtkMenuButton* b, gpointer d) { (*static_cast<MakeMenu*>(d))(b); },
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
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    connect<void(GtkGestureClick*, int, double, double)>(
        click, "pressed", [this, id, row, title, in_list](GtkGestureClick* g, int, double x, double y) {
            if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) return;
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            reminder_context_menu(row, id, in_list, x, y);
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    auto* press = gtk_gesture_long_press_new();
    connect<void(GtkGestureLongPress*, double, double)>(
        press, "pressed", [this, id, row, title, in_list](GtkGestureLongPress*, double x, double y) {
            if (!gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(title))) reminder_context_menu(row, id, in_list, x, y);
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(press));

    // Selecting: Ctrl+click adds or removes the row, Shift+click selects up
    // to it (Ctrl+Shift+click adds that range), anywhere on the row, before
    // its title or buttons see the click. A plain click outside the
    // selection clears it; on a selected row, when released without
    // dragging the selection (and not on its menu or Details buttons).
    auto* pick = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(pick), GTK_PHASE_CAPTURE);
    auto modifiers = [](GtkGestureClick* g) {
        return gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g)) &
               gtk_accelerator_get_default_mod_mask();
    };
    connect<void(GtkGestureClick*, int, double, double)>(
        pick, "pressed", [this, id, row, modifiers](GtkGestureClick* g, int, double, double) {
            auto mods = modifiers(g);
            if (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
                gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
                if (mods & GDK_SHIFT_MASK) select_range(id, mods & GDK_CONTROL_MASK);
                else toggle_selected(id);
                gtk_widget_grab_focus(row);
                return;
            }
            if (!selected_.contains(id)) clear_selection();
            anchor_ = id;
        });
    // A plain click (not a drag) on the row's empty space selects just this
    // row; on its title (which starts editing) or circle, nothing stays
    // selected; its ⋮ and Details buttons leave the selection as it is.
    connect<void(GtkGestureClick*, int, double, double)>(
        pick, "released", [this, id, row, modifiers](GtkGestureClick* g, int, double x, double y) {
            if (modifiers(g) & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) return;
            for (auto* w = gtk_widget_pick(row, x, y, GTK_PICK_DEFAULT); w && w != row; w = gtk_widget_get_parent(w)) {
                if (GTK_IS_BUTTON(w) || GTK_IS_MENU_BUTTON(w)) return;
                if (GTK_IS_CHECK_BUTTON(w) || GTK_IS_EDITABLE_LABEL(w)) return clear_selection();
            }
            selected_ = {id};
            anchor_ = id;
            update_selection();
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(pick));

    // Clicking a row's empty space focuses it, so its keyboard shortcuts apply.
    auto* select_click = gtk_gesture_click_new();
    connect<void(GtkGestureClick*, int, double, double)>(
        select_click, "pressed", [row](GtkGestureClick*, int, double x, double y) {
            for (auto* w = gtk_widget_pick(row, x, y, GTK_PICK_DEFAULT); w && w != row; w = gtk_widget_get_parent(w))
                if (GTK_IS_BUTTON(w) || GTK_IS_CHECK_BUTTON(w) || GTK_IS_EDITABLE_LABEL(w) || GTK_IS_MENU_BUTTON(w))
                    return;  // that widget handles the click itself
            gtk_widget_grab_focus(row);
        });
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(select_click));

    auto* focus = gtk_event_controller_focus_new();
    connect<void(GtkEventControllerFocus*)>(focus, "enter", [this, id](GtkEventControllerFocus*) {
        cursor_ = id;
        if (follow_focus_) {  // moved here with ↑/↓ from a selection
            selected_ = {id};
            anchor_ = id;
            update_selection();
        }
    });
    gtk_widget_add_controller(row, focus);

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
                    case GDK_KEY_KP_Delete: return later([this, ids = targets(id)] { delete_reminders(ids); });
                    case GDK_KEY_space:
                        if (selected_.contains(id)) {
                            focus_reminder_ = id;
                            return later([this, ids = targets(id)] { complete_reminders(ids); });
                        }
                        gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
                                                    !gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
                        return TRUE;
                    case GDK_KEY_Return:
                    case GDK_KEY_KP_Enter:
                    case GDK_KEY_F2:
                        gtk_editable_label_start_editing(GTK_EDITABLE_LABEL(title));
                        return TRUE;
                    case GDK_KEY_Up:
                    case GDK_KEY_Down:
                        // With a selection, the row the focus moves to becomes
                        // the selection (none, if it's a New Reminder row).
                        if (!selected_.empty()) {
                            clear_selection();
                            follow_focus_ = true;
                            idle([this, id] {
                                // Nowhere to go (the top or bottom): it stays selected.
                                if (follow_focus_ && cursor_ == id && reminder_rows_.contains(id)) {
                                    selected_ = {id};
                                    update_selection();
                                }
                                follow_focus_ = false;
                            });
                        }
                        break;
                }
            } else if (mask == GDK_SHIFT_MASK && (key == GDK_KEY_Up || key == GDK_KEY_Down)) {
                extend_selection(id, key == GDK_KEY_Up);
                return TRUE;
            } else if (mask == GDK_CONTROL_MASK) {
                switch (key) {
                    case GDK_KEY_t: return later([this, ids = targets(id)] { set_due(ids, 0); });
                    case GDK_KEY_i: return later([this, id] { show_details(id); });
                    case GDK_KEY_c: copy_reminders(targets(id)); return TRUE;
                    case GDK_KEY_bracketright:
                        if (in_list) return later([this, id] { indent(id, true); });
                        break;
                    case GDK_KEY_bracketleft:
                        if (in_list) return later([this, id] { indent(id, false); });
                        break;
                }
            } else if (mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && key == GDK_KEY_f) {
                return later([this, ids = targets(id)] { toggle_flag(ids); });
            } else if (mask == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && key == GDK_KEY_t) {
                return later([this, ids = targets(id)] { set_due(ids, 1); });
            } else if (mask == GDK_ALT_MASK && key >= GDK_KEY_0 && key <= GDK_KEY_3) {
                auto p = static_cast<rem::Priority>(key - GDK_KEY_0);
                return later([this, ids = targets(id), p] { set_priority(ids, p); });
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
    // Dragging a selected row drags the whole selection.
    make_draggable(
        row, [this, id] { return outermost(targets(id)); },
        [this](const Ids& ids, bool on) {
            for (auto& i : ids)
                if (auto it = reminder_rows_.find(i); it != reminder_rows_.end()) {
                    if (on) gtk_widget_add_css_class(it->second, "dragging");
                    else gtk_widget_remove_css_class(it->second, "dragging");
                }
        });
    if (view_.kind == View::List)
        make_drop_target(row, DropStyle::Halves, id, [this, id](Ids dropped, rem::Document::Place place) {
            move_reminders(dropped, id, place);
        });
    return row;
}

void Window::reminder_context_menu(GtkWidget* row, const std::string& id, bool in_list, double x, double y) {
    // The menu belongs to the invisible button over the content (rows are
    // rebuilt, taking their popovers with them); the row's actions go along.
    auto* button = GTK_MENU_BUTTON(content_menu_button_);
    gtk_widget_insert_action_group(content_menu_button_, "reminder",
                                   G_ACTION_GROUP(g_object_get_data(G_OBJECT(row), "reminder-actions")));
    auto menu = Obj<GMenuModel>::adopt(reminder_menu(id, in_list));
    gtk_menu_button_set_menu_model(button, menu.get());
    auto* popover = GTK_POPOVER(gtk_menu_button_get_popover(button));
    graphene_point_t in_row{static_cast<float>(x), static_cast<float>(y)}, point{};
    if (!gtk_widget_compute_point(row, content_menu_button_, &in_row, &point)) point = in_row;
    GdkRectangle at{static_cast<int>(point.x), static_cast<int>(point.y), 1, 1};
    gtk_popover_set_pointing_to(popover, &at);
    gtk_popover_set_has_arrow(popover, FALSE);
    select_for_menu(id, popover);
    gtk_menu_button_popup(button);
}

// A click in the window on `hit` (before it sees the click).
void Window::clicked(GtkWidget* hit, bool modified) {
    auto inside = [hit](GtkWidget* w) { return hit && (hit == w || gtk_widget_is_ancestor(hit, w)); };
    for (auto* f = gtk_root_get_focus(GTK_ROOT(window_)); f; f = gtk_widget_get_parent(f))
        if (GTK_IS_EDITABLE_LABEL(f)) {
            if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(f)) && !inside(f))
                gtk_editable_label_stop_editing(GTK_EDITABLE_LABEL(f), TRUE);  // keeps the text
            break;
        }
    for (auto* w = hit; w; w = gtk_widget_get_parent(w))
        if (GTK_IS_LIST_BOX_ROW(w) && g_object_get_data(G_OBJECT(w), "reminder-id")) return;  // its own rules
    if (!modified) clear_selection();
}

void Window::select_for_menu(const std::string& id, GtkPopover* popover) {
    if (selected_.contains(id)) return;  // the menu acts on the selection
    selected_ = {id};
    anchor_ = id;
    menu_selected_ = id;
    update_selection();
    if (!popover || g_object_get_data(G_OBJECT(popover), "unselects")) return;
    g_object_set_data(G_OBJECT(popover), "unselects", GINT_TO_POINTER(1));  // connected once per popover
    connect<void(GtkPopover*)>(popover, "closed", [this](GtkPopover*) {
        if (menu_selected_ && selected_ == std::set{*menu_selected_}) clear_selection();
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
    auto item = [](GMenu* m, const std::string& text, const char* action, const char* accel) {
        auto* i = g_menu_item_new(text.c_str(), action);
        if (accel) g_menu_item_set_attribute(i, "accel", "s", accel);
        g_menu_append_item(m, i);
        g_object_unref(i);
    };
    auto* menu = g_menu_new();
    item(menu, all_done ? "Mark as Not _Completed" : "Mark as _Completed", "reminder.complete", "space");
    if (!several) item(menu, "_Details…", "reminder.details", "<Control>i");
    item(menu, all_flagged ? "_Unflag" : "_Flag", "reminder.flag", "<Control><Shift>f");
    auto* dates = menu_section(menu);
    item(dates, "Due _Today", "reminder.due-today", "<Control>t");
    item(dates, "Due To_morrow", "reminder.due-tomorrow", "<Control><Shift>t");
    // Move To: every list but the one they're all in already.
    std::set<rem::ListFile*> in;
    for (auto& i : ids)
        if (auto ref = store_->find(i)) in.insert(ref->list);
    auto* lists = g_menu_new();
    for (auto* l : store_->lists()) {
        if (in.size() == 1 && in.contains(l)) continue;
        auto* i = g_menu_item_new(store_->label(*l).c_str(), nullptr);
        g_menu_item_set_action_and_target_value(i, "reminder.move-to", g_variant_new_string(store_->key_of(*l).c_str()));
        g_menu_append_item(lists, i);
        g_object_unref(i);
    }
    auto* place = menu_section(menu);
    item(place, "_Copy", "reminder.copy", "<Control>c");
    if (g_menu_model_get_n_items(G_MENU_MODEL(lists)) > 0) g_menu_append_submenu(place, "_Move To", G_MENU_MODEL(lists));
    g_object_unref(lists);
    if (in_list && !several) {
        auto* structure = menu_section(menu);
        item(structure, "_Indent", "reminder.indent", "<Control>bracketright");
        item(structure, "_Outdent", "reminder.outdent", "<Control>bracketleft");
    }
    item(menu_section(menu), several ? std::format("_Delete {} Reminders", ids.size()) : "_Delete",
         "reminder.delete", "Delete");
    return G_MENU_MODEL(menu);
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

    make_drop_target(row, DropStyle::Above, "", [this, list, section](Ids dropped, rem::Document::Place) {
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
        if (!ref) return;
        if (trim(text).empty()) return delete_reminders({id});
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

void Window::complete_reminders(const std::vector<std::string>& ids) {
    bool all_done = std::ranges::all_of(ids, [this](auto& id) {
        auto ref = store_->find(id);
        return !ref || ref->reminder->done;
    });
    batch(ids.size() > 1 ? "Complete Reminders" : "Complete Reminder", [&] {
        for (auto& id : ids) store_->set_done(id, !all_done, today());
    });
    // Show the change at once, as a click on one circle does; the rows then
    // linger a moment before the view is rebuilt (and completed ones hide).
    syncing_checks_ = true;
    for (auto& [id, row] : reminder_rows_) {
        auto ref = store_->find(id);
        auto* check = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(row), "check"));
        if (ref && check) gtk_check_button_set_active(GTK_CHECK_BUTTON(check), ref->reminder->done);
    }
    syncing_checks_ = false;
    refresh_later(kCompleteDelayMs);
}

// Several land together, in their order: the first next to `target`, each
// other after the one before.
void Window::move_reminders(const std::vector<std::string>& ids, const std::string& target,
                            rem::Document::Place place) {
    bool nested = false;
    batch(ids.size() > 1 ? "Move Reminders" : "Move Reminder", [&] {
        auto anchor = target;
        for (auto& id : ids) {
            auto ref = store_->find(id);
            auto target_ref = store_->find(anchor);
            if (!ref || !target_ref) continue;
            if (ref->list != target_ref->list) store_->move_to_list(id, *target_ref->list);
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
    if (nested) toast("Subtasks can't have subtasks of their own");
    refresh();
}

void Window::move_to_section_end(const std::vector<std::string>& ids, const std::string& list,
                                 const std::optional<std::string>& section) {
    batch(ids.size() > 1 ? "Move Reminders" : "Move Reminder", [&] {
        auto* l = store_->list(list);
        if (!l) return;
        for (auto& id : ids) {
            auto ref = store_->find(id);
            if (!ref) continue;
            if (ref->list != l) store_->move_to_list(id, *l);
            l->doc.move_to_end(id, section);
            store_->save(*l);
        }
    });
    refresh();
}

void Window::move_to_list(const std::vector<std::string>& ids, const std::string& list) {
    auto* l = store_->list(list);
    if (!l) return;
    int moved = 0;
    batch(ids.size() > 1 ? "Move Reminders" : "Move Reminder", [&] {
        for (auto& id : ids) {
            auto ref = store_->find(id);
            if (!ref || ref->list == l) continue;
            store_->move_to_list(id, *l);
            ++moved;
        }
    });
    if (moved == 0) return;
    refresh();
    auto name = store_->label(*l);
    toast(moved == 1 ? std::format("Moved to “{}”", name) : std::format("Moved {} reminders to “{}”", moved, name));
}

void Window::set_priority(const std::vector<std::string>& ids, rem::Priority priority) {
    batch("Set Priority", [&] {
        for (auto& id : ids)
            if (auto ref = store_->find(id); ref && ref->reminder->priority != priority) {
                ref->reminder->priority = priority;
                store_->touch(id);
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
        for (auto& id : ids)
            if (auto ref = store_->find(id)) {
                ref->reminder->flagged = !all_flagged;
                store_->touch(id);
            }
    });
    keep_focus(ids);
    refresh();
}

void Window::set_due(const std::vector<std::string>& ids, int days_from_today) {
    batch("Set Due Date", [&] {
        auto due = rem::Date{std::chrono::sys_days{today()} + std::chrono::days{days_from_today}};
        for (auto& id : ids)
            if (auto ref = store_->find(id)) {
                ref->reminder->due_date = due;
                store_->touch(id);  // keeps any time already set
            }
    });
    keep_focus(ids);
    refresh();
}

// The reminders as Markdown text, so they also paste into other apps.
void Window::copy_reminders(const std::vector<std::string>& ids) {
    if (!store_) return;
    std::string text, first;
    int count = 0;
    for (auto& id : outermost(ids))
        if (auto ref = store_->find(id)) {
            text += rem::to_clipboard_text(*ref->reminder);
            if (count++ == 0) first = ref->reminder->title;
        }
    if (count == 0) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(window_), text.c_str());
    toast(count == 1 ? std::format("Copied “{}”", first) : std::format("Copied {} reminders", count));
}

void Window::delete_reminders(const std::vector<std::string>& ids) {
    auto gone = outermost(ids);
    std::erase_if(gone, [this](auto& id) { return !store_->find(id); });
    if (gone.empty()) return;
    auto step = batch(gone.size() > 1 ? "Delete Reminders" : "Delete Reminder", [&] {
        for (auto& id : gone) store_->remove(id);
    });
    refresh();
    // The toast's Undo only applies while this deletion is still the latest step.
    auto text = gone.size() > 1 ? std::format("{} reminders deleted", gone.size()) : std::string("Reminder deleted");
    if (step) toast(text, "_Undo", [this, step] {
        if (history_.next_undo() == step) undo();
    });
}

// --- selection -------------------------------------------------------------

// The selection (top to bottom) when `id` is in it, else just `id`.
std::vector<std::string> Window::targets(const std::string& id) {
    if (!selected_.contains(id)) return {id};
    std::vector<std::string> out;
    for (auto& s : shown_ids_)
        if (selected_.contains(s)) out.push_back(s);
    return out;
}

// For moving, deleting and copying: a subtask goes along with its parent.
std::vector<std::string> Window::outermost(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    for (auto& id : ids) {
        auto ref = store_->find(id);
        if (ref && ref->parent && std::ranges::find(ids, ref->parent->id) != ids.end()) continue;
        out.push_back(id);
    }
    return out;
}

void Window::toggle_selected(const std::string& id) {
    if (!selected_.erase(id)) selected_.insert(id);
    anchor_ = id;
    update_selection();
}

// From the anchor (the last reminder clicked, else the focused one) to `to`;
// `add` keeps what was already selected.
void Window::select_range(const std::string& to, bool add) {
    auto from = anchor_.value_or(cursor_.value_or(to));
    auto a = std::ranges::find(shown_ids_, from), b = std::ranges::find(shown_ids_, to);
    if (b == shown_ids_.end()) return;
    if (a == shown_ids_.end()) {
        a = b;
        from = to;
    }
    if (a > b) std::swap(a, b);
    if (!add) selected_.clear();
    selected_.insert(a, b + 1);
    anchor_ = from;
    update_selection();
}

// Shift+↑/↓: selects from the anchor to the row above or below `from`, and
// moves the focus there.
void Window::extend_selection(const std::string& from, bool up) {
    auto at = std::ranges::find(shown_ids_, from);
    if (at == shown_ids_.end() || (up ? at == shown_ids_.begin() : at + 1 == shown_ids_.end())) return;
    auto next = up ? *(at - 1) : *(at + 1);
    if (selected_.empty() || !anchor_) anchor_ = from;
    select_range(next, false);
    if (auto row = reminder_rows_.find(next); row != reminder_rows_.end()) gtk_widget_grab_focus(row->second);
}

void Window::select_all() {
    if (shown_ids_.empty()) return;
    selected_ = {shown_ids_.begin(), shown_ids_.end()};
    if (!anchor_) anchor_ = shown_ids_.front();
    update_selection();
    // The keys that act on the selection work from a reminder row: focus the
    // first one unless one has the focus already (say, just after opening
    // the list from the sidebar).
    for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w; w = gtk_widget_get_parent(w))
        if (GTK_IS_LIST_BOX_ROW(w) && g_object_get_data(G_OBJECT(w), "reminder-id")) return;
    gtk_widget_grab_focus(reminder_rows_[shown_ids_.front()]);
}

void Window::clear_selection() {
    if (selected_.empty()) return;
    selected_.clear();
    update_selection();
}

void Window::update_selection() {
    for (auto& [id, row] : reminder_rows_) {
        if (selected_.contains(id)) gtk_widget_add_css_class(row, "selected-reminder");
        else gtk_widget_remove_css_class(row, "selected-reminder");
    }
    auto subtitle = selected_.size() < 2 ? count_subtitle_ : std::format("{} Selected", selected_.size());
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(content_title_), subtitle.c_str());
}

void Window::keep_focus(const std::vector<std::string>& ids) {
    if (ids.empty()) return;
    focus_reminder_ = cursor_ && std::ranges::find(ids, *cursor_) != ids.end() ? *cursor_ : ids.front();
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

void Window::paste_reminders() {
    if (!store_) return;
    auto keep = Obj<GtkWindow>::ref(GTK_WINDOW(window_));
    gdk_clipboard_read_text_async(
        gtk_widget_get_clipboard(window_), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* holder = static_cast<Obj<GtkWindow>*>(data);
            char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, nullptr);
            if (auto* self = Window::from(holder->get()); self && text) self->add_pasted(text);
            g_free(text);
            delete holder;
        },
        new Obj<GtkWindow>(std::move(keep)));
}

// Adds pasted text as reminders: after the focused reminder (in its list and
// section), else at the end of the list being shown. In a smart list they go
// into the first list, set up to show there (due today in Today, flagged in
// Flagged, tagged in a tag's view).
void Window::add_pasted(const std::string& text) {
    auto pasted = rem::from_clipboard_text(text);
    if (pasted.empty() || !store_) return;

    std::optional<rem::Ref> anchor;
    for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w; w = gtk_widget_get_parent(w))
        if (auto* id = static_cast<const char*>(g_object_get_data(G_OBJECT(w), "reminder-id"))) {
            anchor = store_->find(id);
            break;
        }
    rem::ListFile* list = anchor ? anchor->list : view_.kind == View::List ? store_->list(view_.name) : nullptr;
    if (!list && !store_->lists().empty()) list = store_->lists().front();
    if (!list) {
        toast("Create a list first");
        return;
    }
    // A subtask's paste goes after its parent.
    const rem::Reminder* after = anchor ? (anchor->parent ? anchor->parent : anchor->reminder) : nullptr;
    std::optional<std::string> section = after ? list->doc.section_of(*after) : std::nullopt;
    std::string after_id = after ? after->id : "";

    auto day = today();
    undoable("Paste", [&] {
        std::string first;
        try {
            for (auto& r : pasted) {
                if (!r.created) r.created = day;
                if (!r.done && !r.due_date && (view_.kind == View::Today || view_.kind == View::Scheduled))
                    r.due_date = day;
                if (view_.kind == View::Flagged) r.flagged = true;
                if (view_.kind == View::Tag && std::ranges::find(r.tags, view_.name) == r.tags.end())
                    r.tags.push_back(view_.name);
                const rem::Reminder* at = after_id.empty() ? nullptr : list->doc.find(after_id);
                after_id = store_->add(*list, std::move(r), at, section).id;
                if (first.empty()) first = after_id;
            }
        } catch (const std::exception& e) {
            toast(std::format("Couldn't save: {}", e.what()));
        }
        if (!first.empty()) focus_reminder_ = first;
        refresh();
    });
    if (pasted.size() > 1) toast(std::format("Pasted {} reminders", pasted.size()));
}

void Window::show_details(const std::string& id) {
    auto ref = store_->find(id);
    if (!ref) return;
    std::vector<std::string> names;  // labels: "Name", or "source/Name" when names clash
    for (auto* l : store_->lists()) names.push_back(store_->label(*l));
    show_reminder_dialog(window_, *ref->reminder, ref->parent != nullptr, store_->label(*ref->list), names,
                         [this, id](ReminderEdit e) {
        if (e.deleted) return delete_reminders({id});
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
            if (e.list != store_->label(*ref->list))
                if (auto* dest = list_by_label(e.list)) store_->move_to_list(id, *dest);
        } catch (const std::exception& ex) {
            toast(std::format("Couldn't save: {}", ex.what()));
        }
        });
        refresh();
    });
}

// A new list in `source`; by default the source of the list showing, else
// the default source.
void Window::new_list(std::string source) {
    if (source.empty()) {
        auto* l = view_.kind == View::List ? store_->list(view_.name) : nullptr;
        source = l ? store_->source_of(*l)->config.name : store_->default_source();
    }
    show_list_dialog(
        window_, std::nullopt,
        [this, source](const ListEdit& e) -> std::string {
            if (auto err = list_name_error(e.name); !err.empty()) return err;
            for (auto* l : store_->lists(source))
                if (lower(l->name) == lower(e.name)) return "A list with that name already exists";
            return {};
        },
        [this, source](ListEdit e) {
            bool ok = true;
            undoable("New List", [&] {
                try {
                    store_->create_list(source, e.name, e.color, e.icon);
                } catch (const std::exception& ex) {
                    toast(std::format("Couldn't create the list: {}", ex.what()));
                    ok = false;
                }
            });
            if (!ok) return;
            select(View{View::List, rem::Library::key(source, e.name)});
            show_content();
        });
}

void Window::import_file() {
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Import Reminders");
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Calendar, Markdown, text and CSV files");
    for (auto suffix : {"ics", "md", "markdown", "txt", "csv"}) gtk_file_filter_add_suffix(filter, suffix);
    for (auto mime : {"text/calendar", "text/markdown", "text/plain", "text/csv"}) gtk_file_filter_add_mime_type(filter, mime);
    auto* all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    auto* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    g_object_unref(all);
    g_object_unref(filters);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(window_), nullptr,
        [](GObject* source, GAsyncResult* res, gpointer) {
            auto* self = static_cast<Window*>(g_object_get_data(G_OBJECT(source), "window"));
            GError* error = nullptr;
            auto file = Obj<GFile>::adopt(gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error));
            if (error) {
                g_error_free(error);  // cancelled
                return;
            }
            auto path = std::filesystem::path(take_string(g_file_get_path(file.get())));
            if (path.empty() || !self->store_) return;
            self->import_files({path}, {});
        },
        nullptr);
    g_object_set_data(G_OBJECT(dialog), "window", this);
    g_object_unref(dialog);
}

void Window::import_files(std::vector<std::filesystem::path> files, std::string into) {
    if (files.empty() || !store_) return;
    auto path = files.front();
    files.erase(files.begin());
    auto next = [this, files, into] { idle([this, files, into] { import_files(files, into); }); };
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        toast(std::format("“{}” is a folder: drop the files in it", path.filename().string()));
        return next();
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in.good() && !in.eof()) {
        toast(std::format("Couldn't read “{}”", path.filename().string()));
        return next();
    }
    import_tasks(path, text.str(), into, next);
}

// Asks which list the reminders go into: a new one named after the
// calendar or file (first), or any list; one already called that is
// chosen to begin with. "Read As" shows the kind of file found, and
// reads it again as another.
void Window::import_tasks(const std::filesystem::path& file, std::string text, std::string into,
                          std::function<void()> then) {
    static constexpr rem::Import::Kind kKinds[] = {rem::Import::Kind::Markdown, rem::Import::Kind::Text,
                                                   rem::Import::Kind::Todotxt, rem::Import::Kind::Csv,
                                                   rem::Import::Kind::Ics};
    struct State {
        std::string text, file_name;
        std::optional<rem::Import> imp;  // as read with the kind chosen; nullopt if it couldn't be
    };
    auto state = std::make_shared<State>(State{std::move(text), file.filename().string(), std::nullopt});
    auto detected = rem::detect_kind(state->text, state->file_name);

    auto* in_view = view_.kind == View::List ? store_->list(view_.name) : nullptr;
    auto source = in_view ? store_->source_of(*in_view)->config.name : store_->default_source();
    // The new list's name: the calendar's, else the file's.
    std::string name;
    try {
        name = rem::read_as(state->text, detected, std::chrono::current_zone()).name;
    } catch (const std::exception&) {
    }
    if (name.empty()) {
        name = file.filename().string();
        for (auto suffix : {".todo.txt", ".txt", ".md", ".markdown", ".csv", ".ics"})
            if (lower(name).ends_with(suffix)) {
                name.resize(name.size() - std::string_view(suffix).size());
                break;
            }
    }
    for (auto& c : name)
        if (std::string_view("/\\<>:\"|?*").find(c) != std::string_view::npos) c = '-';
    if (!list_name_error(name).empty()) name = "Imported";

    std::vector<std::string> keys{""};  // "": the new list
    std::vector<std::string> labels{std::format("New List “{}”", name)};
    guint selected = 0;
    for (auto* l : store_->lists()) {
        auto key = store_->key_of(*l);
        if (key == into || (into.empty() && lower(l->name) == lower(name) && selected == 0))
            selected = static_cast<guint>(keys.size());
        keys.push_back(store_->key_of(*l));
        labels.push_back(store_->label(*l));
    }

    auto* dialog = adw_alert_dialog_new("Import Reminders", nullptr);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "import", "_Import", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "import", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "import");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    auto* rows = boxed_list();
    auto* read_as = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(read_as), "Read As");
    auto* kinds = gtk_string_list_new(nullptr);
    for (auto label : {"Markdown", "Plain Text", "todo.txt", "CSV", "iCalendar"}) gtk_string_list_append(kinds, label);
    adw_combo_row_set_model(ADW_COMBO_ROW(read_as), G_LIST_MODEL(kinds));
    g_object_unref(kinds);
    adw_combo_row_set_selected(ADW_COMBO_ROW(read_as),
                               static_cast<guint>(std::ranges::find(kKinds, detected) - std::begin(kKinds)));
    auto* into_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(into_row), "Into");
    auto* model = gtk_string_list_new(nullptr);
    for (auto& l : labels) gtk_string_list_append(model, l.c_str());
    adw_combo_row_set_model(ADW_COMBO_ROW(into_row), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(into_row), selected);
    auto* duplicates = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(duplicates), "Import Duplicates");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(duplicates), "Add reminders that are already here again, as copies");
    gtk_list_box_append(GTK_LIST_BOX(rows), read_as);
    gtk_list_box_append(GTK_LIST_BOX(rows), into_row);
    gtk_list_box_append(GTK_LIST_BOX(rows), duplicates);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), rows);

    // Reads the file as the kind chosen, and says what it found.
    auto reread = [state, dialog, read_as] {
        auto i = std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(read_as)), std::size(kKinds) - 1);
        std::string body;
        try {
            state->imp = rem::read_as(state->text, kKinds[i], std::chrono::current_zone());
            auto count = rem::reminder_count(*state->imp);
            body = count == 0 ? std::format("There are no reminders in “{}” read as {}.", state->file_name,
                                            rem::kind_name(kKinds[i]))
                              : std::format("{} {} from “{}”.", count, count == 1 ? "reminder" : "reminders",
                                            state->file_name);
            if (state->imp->skipped > 0)
                body += std::format(" {} {} left out: only tasks are imported.", state->imp->skipped,
                                    state->imp->skipped == 1 ? "event or other item is" : "events or other items are");
            if (count > 0 && kKinds[i] == rem::Import::Kind::Text) body += " Each line of the file is a reminder.";
            if (count == 0) state->imp.reset();
        } catch (const std::exception& e) {
            state->imp.reset();
            body = std::format("“{}” can't be read as {}: {}.", state->file_name, rem::kind_name(kKinds[i]), e.what());
        }
        adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), body.c_str());
        adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "import", state->imp.has_value());
    };
    reread();
    connect<void(GObject*, GParamSpec*)>(read_as, "notify::selected", [reread](GObject*, GParamSpec*) { reread(); });

    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [this, into_row, duplicates, keys, source, name, state, then](AdwAlertDialog*, const char* response) {
            if (then) then();
            if (std::string_view(response) != "import" || !store_ || !state->imp) return;
            auto& imp = *state->imp;
            auto key = keys.at(std::min<std::size_t>(adw_combo_row_get_selected(ADW_COMBO_ROW(into_row)), keys.size() - 1));
            rem::ImportResult result;
            undoable("Import", [&] {
                try {
                    rem::ListFile* list = key.empty() ? nullptr : store_->list(key);
                    bool created = !list;
                    if (!list) {
                        // A new list, its name made unique in the source.
                        auto unique = name;
                        auto clash = [&](const std::string& n) {
                            return std::ranges::any_of(store_->lists(source),
                                                       [&](auto* l) { return lower(l->name) == lower(n); });
                        };
                        for (int n = 2; clash(unique); ++n) unique = std::format("{} {}", name, n);
                        list = &store_->create_list(source, unique, imp.color.empty() ? "blue" : imp.color, "list");
                    }
                    key = store_->key_of(*list);
                    result = rem::import_into(*store_, *list, imp,
                                              adw_switch_row_get_active(ADW_SWITCH_ROW(duplicates)));
                    if (result.added == 0 && created) {  // nothing new: no empty list either
                        store_->delete_list(key);
                        key = "-";
                    }
                } catch (const std::exception& e) {
                    toast(std::format("Couldn't import: {}", e.what()));
                    key.clear();
                }
            });
            if (key.empty()) return;
            if (key == "-") {
                toast(std::format("Nothing to import: {} already here",
                                  result.already == 1 ? "the one reminder is"
                                                      : std::format("all {} reminders are", result.already)));
                return;
            }
            auto text = std::format("Imported {} {}", result.added, result.added == 1 ? "reminder" : "reminders");
            if (result.already > 0)
                text += std::format("; {} {} already here", result.already, result.already == 1 ? "was" : "were");
            toast(text);
            select(View{View::List, key});
            show_content();
        });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

// The lists to export (`chosen`: keys ticked to begin with) and the format.
// One list is saved as a file; several, a file each, into a folder.
void Window::export_lists(std::vector<std::string> chosen) {
    if (!store_) return;
    auto* dialog = adw_alert_dialog_new("Export Lists",
                                        "Exported lists can be imported again, here or on another computer.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "export", "_Export…", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "export", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "export");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    static constexpr rem::ExportFormat kFormats[] = {rem::ExportFormat::Markdown, rem::ExportFormat::Text,
                                                     rem::ExportFormat::Todotxt, rem::ExportFormat::Csv,
                                                     rem::ExportFormat::Ics};
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    auto* rows = boxed_list();
    auto* format = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(format), "Format");
    auto* model = gtk_string_list_new(nullptr);
    for (auto name : {"Markdown", "Plain Text", "todo.txt", "CSV", "iCalendar"}) gtk_string_list_append(model, name);
    adw_combo_row_set_model(ADW_COMBO_ROW(format), G_LIST_MODEL(model));
    g_object_unref(model);
    auto* completed = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(completed), "Include Completed");
    auto describe = [format, completed] {
        auto f = kFormats[std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(format)), std::size(kFormats) - 1)];
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(format),
            f == rem::ExportFormat::Text      ? "A line per reminder, as you'd type it (.txt)"
            : f == rem::ExportFormat::Todotxt ? "For todo.txt apps; without notes or sections (.todo.txt)"
            : f == rem::ExportFormat::Csv     ? "A row per reminder, for spreadsheets (.csv)"
            : f == rem::ExportFormat::Ics     ? "Tasks for calendar and reminders apps (.ics)"
                                              : "The list file itself, with everything (.md)");
        gtk_widget_set_visible(completed, f == rem::ExportFormat::Text);  // the others always have everything
    };
    describe();
    connect<void(GObject*, GParamSpec*)>(format, "notify::selected", [describe](GObject*, GParamSpec*) { describe(); });
    auto* archive = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(archive), "Compressed Archive");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(archive), "One .zip file instead of a folder of files");
    gtk_list_box_append(GTK_LIST_BOX(rows), format);
    gtk_list_box_append(GTK_LIST_BOX(rows), completed);
    gtk_list_box_append(GTK_LIST_BOX(rows), archive);
    gtk_box_append(GTK_BOX(box), rows);

    // The lists, with All Lists above them.
    struct Pick {
        std::string key;
        GtkWidget* check;
    };
    auto picks = std::make_shared<std::vector<Pick>>();
    auto* lists = boxed_list();
    auto* all_check = gtk_check_button_new();
    auto* all_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(all_row), "All Lists");
    adw_action_row_add_prefix(ADW_ACTION_ROW(all_row), all_check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(all_row), all_check);
    gtk_list_box_append(GTK_LIST_BOX(lists), all_row);
    for (auto* l : store_->lists()) {
        auto key = store_->key_of(*l);
        auto* check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), std::ranges::find(chosen, key) != chosen.end());
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), store_->label(*l).c_str());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
        gtk_list_box_append(GTK_LIST_BOX(lists), row);
        picks->push_back({key, check});
    }
    auto* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 280);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), lists);
    gtk_box_append(GTK_BOX(box), scroller);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);

    // All Lists shows whether all, some or none are ticked; ticking it ticks
    // them all, unticking it none. Export needs at least one.
    auto syncing = std::make_shared<bool>(false);
    auto update = [dialog, picks, all_check, syncing, archive] {
        auto n = std::ranges::count_if(*picks, [](auto& p) { return gtk_check_button_get_active(GTK_CHECK_BUTTON(p.check)); });
        gtk_widget_set_visible(archive, n > 1);  // one list is one file anyway
        *syncing = true;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(all_check), n > 0 && n == static_cast<long>(picks->size()));
        gtk_check_button_set_inconsistent(GTK_CHECK_BUTTON(all_check), n > 0 && n < static_cast<long>(picks->size()));
        *syncing = false;
        adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "export", n > 0);
    };
    for (auto& p : *picks) connect<void(GtkCheckButton*)>(p.check, "toggled", [update](GtkCheckButton*) { update(); });
    connect<void(GtkCheckButton*)>(all_check, "toggled", [picks, syncing, update](GtkCheckButton* b) {
        if (*syncing) return;
        bool on = gtk_check_button_get_active(b);
        for (auto& p : *picks) gtk_check_button_set_active(GTK_CHECK_BUTTON(p.check), on);
        update();
    });
    update();

    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [this, picks, format, completed, archive](AdwAlertDialog*, const char* response) {
            if (std::string_view(response) != "export" || !store_) return;
            auto fmt = kFormats[std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(format)), std::size(kFormats) - 1)];
            rem::ExportOptions options;
            options.completed = adw_switch_row_get_active(ADW_SWITCH_ROW(completed));
            std::vector<std::string> keys;
            for (auto& p : *picks)
                if (gtk_check_button_get_active(GTK_CHECK_BUTTON(p.check))) keys.push_back(p.key);
            if (keys.empty()) return;

            struct Job {
                Window* self;
                std::vector<std::string> keys;
                rem::ExportFormat format;
                rem::ExportOptions options;
            };
            auto* chooser = gtk_file_dialog_new();
            auto* job = new Job{this, keys, fmt, options};
            if (keys.size() > 1 && adw_switch_row_get_active(ADW_SWITCH_ROW(archive))) {
                gtk_file_dialog_set_title(chooser, "Export Lists");
                gtk_file_dialog_set_initial_name(chooser, "Reminders.zip");
                gtk_file_dialog_save(
                    chooser, GTK_WINDOW(window_), nullptr,
                    [](GObject* source, GAsyncResult* res, gpointer data) {
                        std::unique_ptr<Job> job(static_cast<Job*>(data));
                        GError* error = nullptr;
                        auto file = Obj<GFile>::adopt(gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &error));
                        if (error) {
                            g_error_free(error);  // cancelled
                            return;
                        }
                        auto* self = job->self;
                        auto path = std::filesystem::path(take_string(g_file_get_path(file.get())));
                        if (!self->store_ || path.empty()) return;
                        std::vector<rem::ListFile*> lists;
                        for (auto& k : job->keys)
                            if (auto* l = self->store_->list(k)) lists.push_back(l);
                        try {
                            std::ofstream out(path, std::ios::binary | std::ios::trunc);
                            out << rem::export_zip(*self->store_, lists, job->format, job->options);
                            out.close();
                            self->toast(out ? std::format("Exported {} lists to {}", lists.size(), rem::contract_path(path))
                                            : std::format("Couldn't write {}", path.string()));
                        } catch (const std::exception& e) {
                            self->toast(std::format("Couldn't export: {}", e.what()));
                        }
                    },
                    job);
            } else if (keys.size() > 1) {
                gtk_file_dialog_set_title(chooser, "Choose a Folder for the Lists");
                gtk_file_dialog_select_folder(
                    chooser, GTK_WINDOW(window_), nullptr,
                    [](GObject* source, GAsyncResult* res, gpointer data) {
                        std::unique_ptr<Job> job(static_cast<Job*>(data));
                        GError* error = nullptr;
                        auto file = Obj<GFile>::adopt(
                            gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &error));
                        if (error) {
                            g_error_free(error);  // cancelled
                            return;
                        }
                        auto* self = job->self;
                        auto folder = std::filesystem::path(take_string(g_file_get_path(file.get())));
                        if (!self->store_ || folder.empty()) return;
                        std::vector<rem::ListFile*> lists;
                        for (auto& k : job->keys)
                            if (auto* l = self->store_->list(k)) lists.push_back(l);
                        try {
                            auto files = rem::export_lists(*self->store_, lists, folder, job->format, job->options);
                            self->toast(std::format("Exported {} {} to {}", files.size(),
                                                    files.size() == 1 ? "list" : "lists", rem::contract_path(folder)));
                        } catch (const std::exception& e) {
                            self->toast(std::format("Couldn't export: {}", e.what()));
                        }
                    },
                    job);
            } else {
                auto* list = store_->list(keys.front());
                if (!list) {
                    delete job;
                    g_object_unref(chooser);
                    return;
                }
                gtk_file_dialog_set_title(chooser, "Export List");
                gtk_file_dialog_set_initial_name(chooser,
                                                 std::format("{}.{}", list->name, rem::export_extension(fmt)).c_str());
                gtk_file_dialog_save(
                    chooser, GTK_WINDOW(window_), nullptr,
                    [](GObject* source, GAsyncResult* res, gpointer data) {
                        std::unique_ptr<Job> job(static_cast<Job*>(data));
                        GError* error = nullptr;
                        auto file = Obj<GFile>::adopt(gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &error));
                        if (error) {
                            g_error_free(error);  // cancelled
                            return;
                        }
                        auto* self = job->self;
                        auto* list = self->store_ ? self->store_->list(job->keys.front()) : nullptr;
                        auto path = std::filesystem::path(take_string(g_file_get_path(file.get())));
                        if (!list || path.empty()) return;
                        std::ofstream out(path, std::ios::binary | std::ios::trunc);
                        out << rem::export_list(*list, job->format, job->options);
                        out.close();
                        self->toast(out ? std::format("Exported “{}” to {}", list->name, rem::contract_path(path))
                                        : std::format("Couldn't write {}", path.string()));
                    },
                    job);
            }
            g_object_unref(chooser);
        });
    adw_dialog_present(ADW_DIALOG(dialog), window_);
}

// List Info… for the list with key `key` ("source/name").
void Window::edit_list(const std::string& key) {
    auto* l = store_->list(key);
    if (!l) return;
    auto name = l->name;
    auto source = store_->source_of(*l)->config.name;
    show_list_dialog(
        window_, ListEdit{l->name, l->color(), l->icon()},
        [this, name, source](const ListEdit& e) -> std::string {
            if (auto err = list_name_error(e.name); !err.empty()) return err;
            for (auto* other : store_->lists(source))  // names are unique within a source
                if (other->name != name && lower(other->name) == lower(e.name))
                    return "A list with that name already exists";
            return {};
        },
        [this, key, name, source](ListEdit e) {
            auto* l = store_->list(key);
            if (!l) return;
            bool ok = true;
            auto new_key = rem::Library::key(source, e.name);
            undoable("Edit List", [&] {
                try {
                    if (e.name != name && !store_->rename_list(*l, e.name)) {
                        toast("Couldn't rename the list");
                        ok = false;
                        return;
                    }
                    if (e.name != name) {  // keeps its place in lists-order under its new name
                        auto order = rem::load_names_setting("lists-order");
                        for (auto& entry : order)
                            if (rem::list_entry_matches(entry, key)) entry = new_key;
                        rem::save_names_setting("lists-order", order);
                    }
                    if (e.name != name && hidden_.list_hidden(key)) {  // stays hidden under its new name
                        rem::set_list_hidden(key, false);
                        rem::set_list_hidden(new_key, true);
                        hidden_ = rem::load_hidden();
                    }
                    l->doc.set_meta("color", e.color);
                    l->doc.set_meta("icon", e.icon);
                    store_->save(*l);
                } catch (const std::exception& ex) {
                    toast(std::format("Couldn't save: {}", ex.what()));
                }
            });
            if (!ok) return;
            if (view_.kind == View::List && view_.name == key) {
                view_.name = new_key;
                if (remember_view_) save_last_view(view_to_string(view_));
            }
            refresh();
        });
}

// Deletes the list with key `name` ("source/name"), after asking.
void Window::delete_list(const std::string& name) {
    auto* list = store_->list(name);
    auto shown = list ? list->name : name;
    auto* dialog = adw_alert_dialog_new(std::format("Delete “{}”?", shown).c_str(),
                                        "The list and all its reminders will be deleted on every synced device. "
                                        "On this computer the file is moved to the Trash.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel", "delete", "_Delete", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    connect<void(AdwAlertDialog*, const char*)>(dialog, "response", [this, name, shown](AdwAlertDialog*, const char* response) {
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
        if (view_.kind == View::List && view_.name == name) view_ = home_view();
        refresh();
        toast(std::format("“{}” deleted", shown), "_Undo", [this, step, name] {
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
        return {v, l ? store_->label(*l) : v.name, l ? list_icon_name(l->icon()) : "view-list-bullet-symbolic",
                l ? l->color() : "gray"};
    }
    if (v.kind == View::Tag) {
        auto style = rem::load_tag_style(v.name);
        return {v, "#" + v.name, list_icon_name(style.icon), style.color};
    }
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
    for (auto& v : sidebar_views(true)) st->all.push_back(view_info(v));

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

// The smart lists the settings show, in their order.
std::vector<View> Window::smart_views() {
    std::vector<View> out;
    if (smart_.display == rem::GroupDisplay::Hidden) return out;
    for (auto& name : smart_.shown) {
        auto v = view_from_string(name);
        if (smart_info(v.kind)) out.push_back(v);
    }
    if (hidden_.show)  // the hidden ones after them
        for (auto& s : kSmart)
            if (std::ranges::find(out, View{s.kind, ""}) == out.end()) out.push_back(View{s.kind, ""});
    return out;
}

std::vector<rem::ListFile*> Window::sidebar_lists(const std::string& source) {
    std::vector<std::string> keys;
    for (auto* l : store_->lists(source)) keys.push_back(store_->key_of(*l));
    std::vector<rem::ListFile*> out;
    for (auto& key : rem::order_lists(keys))
        if (hidden_.show || !hidden_.list_hidden(key))
            if (auto* l = store_->list(key)) out.push_back(l);
    return out;
}

std::vector<std::string> Window::list_keys() {
    std::vector<std::string> keys;
    for (auto* l : store_->lists()) keys.push_back(store_->key_of(*l));
    return keys;
}

std::string Window::list_label(const rem::ListFile& list) { return store_->label(list); }

rem::ListFile* Window::list_by_label(const std::string& label) {
    if (auto* l = store_->list(label)) return l;
    for (auto* l : store_->lists())
        if (store_->label(*l) == label) return l;
    return nullptr;
}

std::vector<std::string> Window::source_names() {
    std::vector<std::string> out;
    if (store_)
        for (auto& s : store_->sources()) out.push_back(s.config.name);
    return out;
}

std::string Window::group_title(const rem::SidebarGroup& group) {
    if (group.kind != rem::SidebarGroup::Lists || !store_ || store_->sources().size() <= 1) return rem::group_title(group);
    for (auto& s : store_->sources())
        if (s.config.name == group.source) return rem::group_title(group, rem::source_title(s.config));
    return rem::group_title(group);
}

std::vector<std::string> Window::sidebar_tags() {
    std::vector<std::string> out;
    for (auto& t : rem::order_tags(store_->tags()))
        if (hidden_.show || !hidden_.tag_hidden(t)) out.push_back(t);
    return out;
}

bool Window::entry_hidden(const View& v) {
    if (smart_info(v.kind)) return std::ranges::find(smart_.shown, view_to_string(v)) == smart_.shown.end();
    if (v.kind == View::List) return hidden_.list_hidden(v.name);
    if (v.kind == View::Tag) return hidden_.tag_hidden(v.name);
    return false;
}

// Hides or unhides a sidebar entry (saved in settings.ini). Leaving the view
// that was just hidden goes to Today or the first entry showing.
void Window::set_entry_hidden(const View& v, bool hidden) {
    try {
        if (smart_info(v.kind)) rem::set_smart_list_hidden(view_to_string(v), hidden);
        else if (v.kind == View::List) rem::set_list_hidden(v.name, hidden);
        else if (v.kind == View::Tag) rem::set_tag_hidden(v.name, hidden);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't save the setting: {}", e.what()));
    }
    smart_ = rem::load_smart_lists_layout();
    hidden_ = rem::load_hidden();
    if (hidden && !hidden_.show && view_ == v) select(home_view());
    rebuild_sidebar();
}

// Tag Info…: the tag's colour and icon, kept in settings.ini.
void Window::edit_tag(const std::string& tag) {
    auto style = rem::load_tag_style(tag);
    show_tag_dialog(window_, tag, ListEdit{"#" + tag, style.color, style.icon}, [this, tag](ListEdit e) {
        try {
            rem::save_tag_style(tag, {e.color, e.icon});
        } catch (const std::exception& err) {
            toast(std::format("Couldn't save the setting: {}", err.what()));
        }
        rebuild_sidebar();
    });
}

// Sidebar entries in display order. `include_folded` adds the entries of
// collapsed groups (Go To finds them; numbers skip them).
std::vector<View> Window::sidebar_views(bool include_folded) {
    std::vector<View> out;
    for (auto g : showing_groups()) {
        if (group_folded(g) && !include_folded) continue;
        switch (g.kind) {
            case rem::SidebarGroup::SmartLists:
                for (auto& v : smart_views()) out.push_back(v);
                break;
            case rem::SidebarGroup::Lists:
                for (auto* l : sidebar_lists(g.source)) out.push_back(View{View::List, store_->key_of(*l)});
                break;
            case rem::SidebarGroup::Tags:
                for (auto& t : sidebar_tags()) out.push_back(View{View::Tag, t});
                break;
        }
    }
    return out;
}

std::vector<rem::SidebarGroup> Window::showing_groups() {
    std::vector<rem::SidebarGroup> out;
    for (auto g : order_) {
        if (g.kind == rem::SidebarGroup::SmartLists && smart_views().empty()) continue;
        if (g.kind == rem::SidebarGroup::Tags && (tags_.hidden() || sidebar_tags().empty())) continue;
        out.push_back(g);
    }
    return out;
}

rem::GroupLayout* Window::layout_of(const rem::SidebarGroup& group) {
    if (group.kind == rem::SidebarGroup::Lists) {
        auto at = lists_layouts_.find(group.source);
        if (at == lists_layouts_.end()) at = lists_layouts_.emplace(group.source, rem::load_lists_layout(group.source)).first;
        return &at->second;
    }
    if (group.kind == rem::SidebarGroup::Tags) return &tags_;
    return nullptr;
}

bool Window::group_foldable(const rem::SidebarGroup& group) {
    auto* l = layout_of(group);
    return l ? l->foldable() : smart_.foldable();
}

bool Window::group_folded(const rem::SidebarGroup& group) {
    auto* l = layout_of(group);
    return l ? l->folded() : smart_.folded();
}

void Window::toggle_fold(const rem::SidebarGroup& group) {
    auto* l = layout_of(group);
    bool& collapsed = l ? l->collapsed : smart_.collapsed;
    collapsed = !collapsed;
    try {
        rem::save_group_collapsed(group, collapsed);
    } catch (const std::exception&) {
        // Folding still works; it just won't be remembered.
    }
    rebuild_sidebar();
}

// Moves a group past its neighbour and saves the order. Focus stays on the
// row that had it (or the group's first row).
void Window::move_group(const rem::SidebarGroup& group, int delta) {
    if (!store_ || !rem::move_sidebar_group(order_, group, delta, showing_groups())) return;
    try {
        rem::save_sidebar_order(order_);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't save the sidebar order: {}", e.what()));
    }
    std::optional<View> focused;
    for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w; w = gtk_widget_get_parent(w))
        if (GTK_IS_LIST_BOX_ROW(w)) {
            if (auto* v = row_view(GTK_LIST_BOX_ROW(w))) focused = *v;
            break;
        }
    rebuild_sidebar();
    for (int i = 0;; ++i) {
        auto* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list_), i);
        if (!row) break;
        auto* v = row_view(row);
        if (focused ? v && *v == *focused : row_group(row) == group) {
            gtk_widget_grab_focus(GTK_WIDGET(row));
            break;
        }
    }
}

void Window::drop_group(const rem::SidebarGroup& group, const rem::SidebarGroup& target, bool after) {
    if (!store_ || !rem::move_sidebar_group_next_to(order_, group, target, after)) return;
    try {
        rem::save_sidebar_order(order_);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't save the sidebar order: {}", e.what()));
    }
    rebuild_sidebar();
}

std::optional<Window::EntryOrder> Window::entry_order(const View& v) {
    if (!store_) return std::nullopt;
    if (smart_info(v.kind)) {
        auto order = smart_.shown;  // a hidden smart list has no place to move
        return EntryOrder{order, order, view_to_string(v)};
    }
    if (v.kind == View::Tag) return EntryOrder{rem::order_tags(store_->tags()), sidebar_tags(), v.name};
    if (v.kind == View::List) {
        auto* l = store_->list(v.name);
        if (!l) return std::nullopt;
        std::vector<std::string> showing;  // this list's group
        for (auto* x : sidebar_lists(store_->source_of(*l)->config.name)) showing.push_back(store_->key_of(*x));
        // every source's, each keeping its place
        return EntryOrder{rem::order_lists(list_keys()), std::move(showing), v.name};
    }
    return std::nullopt;
}

void Window::save_entry_order(const View& v, const std::vector<std::string>& order) {
    if (smart_info(v.kind)) {
        rem::save_smart_lists(order);
        smart_ = rem::load_smart_lists_layout();
    } else if (v.kind == View::Tag) {
        rem::save_names_setting("tags-order", order);
    } else if (v.kind == View::List) {
        rem::save_names_setting("lists-order", order);
    }
}

bool Window::can_move_entry(const View& v, int delta) {
    auto e = entry_order(v);
    return e && rem::move_in_order(e->order, e->name, delta, e->showing);
}

void Window::move_entry(const View& v, int delta) {
    auto e = entry_order(v);
    if (!e || !rem::move_in_order(e->order, e->name, delta, e->showing)) return;
    try {
        save_entry_order(v, e->order);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't save the order: {}", e.what()));
        return;
    }
    rebuild_sidebar();
    focus_entry(v);
}

// Entries are dragged only within their group: smart lists among smart
// lists, a source's lists among that source's, tags among tags.
bool Window::same_sidebar_group(const View& a, const View& b) {
    if (a == b || !store_) return false;
    if (smart_info(a.kind) || smart_info(b.kind)) return smart_info(a.kind) && smart_info(b.kind);
    if (a.kind != b.kind) return false;
    if (a.kind == View::Tag) return true;
    if (a.kind != View::List) return false;
    auto *la = store_->list(a.name), *lb = store_->list(b.name);
    return la && lb && store_->source_of(*la) == store_->source_of(*lb);
}

void Window::drop_entry(const View& v, const View& target, bool after) {
    if (!same_sidebar_group(v, target)) return;
    auto e = entry_order(v);
    auto t = entry_order(target);
    if (!e || !t || !rem::move_next_to(e->order, e->name, t->name, after)) return;
    try {
        save_entry_order(v, e->order);
    } catch (const std::exception& e) {
        toast(std::format("Couldn't save the order: {}", e.what()));
        return;
    }
    rebuild_sidebar();
    focus_entry(v);
}

void Window::focus_entry(const View& v) {
    for (int i = 0;; ++i) {  // keep focus on the entry that moved
        auto* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(sidebar_list_), i);
        if (!row) break;
        if (auto* rv = row_view(row); rv && *rv == v) {
            gtk_widget_grab_focus(GTK_WIDGET(row));
            break;
        }
    }
}

// The sidebar's context menu, for the group of the row under the pointer.
void Window::sidebar_menu(GtkListBoxRow* row, double x, double y) {
    if (!store_ || !row) return;
    Obj<GMenuModel> menu;
    if (auto* v = row_view(row)) {
        // An entry, which stays as it is (not opened). A list gets the same
        // items as the header's ⋮ menu, but for this list (Show Completed is
        // the window's setting, as in the ⋮ menu); a tag gets Tag Info…; all
        // get Hidden.
        auto view = *v;
        auto name = v->name;
        auto* actions = g_simple_action_group_new();
        add_action(actions, "add-section", [this, name] { idle([this, name] { add_section(name); }); });
        add_action(actions, "list-info", [this, name] { idle([this, name] { edit_list(name); }); });
        add_action(actions, "delete-list", [this, name] { idle([this, name] { delete_list(name); }); });
        add_action(actions, "export-list", [this, name] { idle([this, name] { export_lists({name}); }); });
        add_action(actions, "tag-info", [this, name] { idle([this, name] { edit_tag(name); }); });
        auto* up = add_action(actions, "move-up", [this, view] { idle([this, view] { move_entry(view, -1); }); });
        auto* down = add_action(actions, "move-down", [this, view] { idle([this, view] { move_entry(view, 1); }); });
        g_simple_action_set_enabled(up, can_move_entry(view, -1));
        g_simple_action_set_enabled(down, can_move_entry(view, 1));
        bool hidden = entry_hidden(view);
        add_action(actions, "hide", [this, view, hidden] {  // Hide, or Show for a hidden one
            idle([this, view, hidden] { set_entry_hidden(view, !hidden); });
        });
        gtk_widget_insert_action_group(sidebar_menu_button_, "sidebar-entry", G_ACTION_GROUP(actions));
        g_object_unref(actions);
        auto* m = g_menu_new();
        if (view.kind == View::List) {
            g_menu_append(menu_section(m), "_Show Completed", "win.show-completed");
            auto* edit = menu_section(m);
            g_menu_append(edit, "Add _Section…", "sidebar-entry.add-section");
            g_menu_append(edit, "List _Info…", "sidebar-entry.list-info");
            g_menu_append(edit, "_Export…", "sidebar-entry.export-list");
        } else if (view.kind == View::Tag) {
            g_menu_append(menu_section(m), "Tag _Info…", "sidebar-entry.tag-info");
        }
        {  // the entry's place in its group
            auto* moves = menu_section(m);
            g_menu_append(moves, "Move _Up", "sidebar-entry.move-up");
            g_menu_append(moves, "Move _Down", "sidebar-entry.move-down");
        }
        g_menu_append(menu_section(m), hidden ? "_Show" : "_Hide", "sidebar-entry.hide");
        if (view.kind == View::List) g_menu_append(menu_section(m), "_Delete List…", "sidebar-entry.delete-list");
        menu = Obj<GMenuModel>::adopt(G_MENU_MODEL(m));
    } else {
        // A group heading: move the group, make it collapsible.
        auto group = row_group(row);
        if (!group) return;
        menu_group_ = *group;
        auto showing = showing_groups();
        auto can = [&](int delta) {
            auto order = order_;
            return rem::move_sidebar_group(order, *group, delta, showing);
        };
        // Every item goes in a section: loose items next to a section make the
        // popover size itself wrongly (clipped, with scrollbars).
        auto* m = g_menu_new();
        auto* moves = menu_section(m);
        auto title = group_title(*group);
        g_menu_append(moves, std::format("Move “{}” _Up", title).c_str(), "win.move-group-up");
        g_menu_append(moves, std::format("Move “{}” _Down", title).c_str(), "win.move-group-down");
        auto enable = [this](const char* name, bool on) {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(window_), name)), on);
        };
        enable("move-group-up", can(-1));
        enable("move-group-down", can(1));
        g_menu_append(menu_section(m), "_Collapsible", "win.group-collapsible");
        g_simple_action_set_state(collapsible_action_, g_variant_new_boolean(group_foldable(*group)));
        if (group->kind == rem::SidebarGroup::Lists) {  // a source's lists
            auto source = group->source;
            auto* actions = g_simple_action_group_new();
            add_action(actions, "new-list", [this, source] { idle([this, source] { new_list(source); }); });
            add_action(actions, "remove", [this, source] { idle([this, source] { remove_source(source); }); });
            gtk_widget_insert_action_group(sidebar_menu_button_, "sidebar-source", G_ACTION_GROUP(actions));
            g_object_unref(actions);
            add_action(actions, "info", [this, source] { idle([this, source] { source_info(source); }); });
            auto* items = menu_section(m);
            g_menu_append(items, "_New List…", "sidebar-source.new-list");
            g_menu_append(items, "Source _Info…", "sidebar-source.info");
        }
        menu = Obj<GMenuModel>::adopt(G_MENU_MODEL(m));
    }
    // The menu belongs to an invisible menu button over the sidebar, not to
    // the list box (rebuilding that removes all its children, popovers too),
    // and not to a plain widget, which never re-sizes a popover whose items
    // arrive after it opens (it came out clipped, with scrollbars).
    auto* button = GTK_MENU_BUTTON(sidebar_menu_button_);
    gtk_menu_button_set_menu_model(button, menu.get());
    auto* popover = GTK_POPOVER(gtk_menu_button_get_popover(button));
    graphene_point_t in_list{static_cast<float>(x), static_cast<float>(y)}, point{};
    if (!gtk_widget_compute_point(sidebar_list_, sidebar_menu_button_, &in_list, &point)) point = in_list;
    GdkRectangle at{static_cast<int>(point.x), static_cast<int>(point.y), 1, 1};
    gtk_popover_set_pointing_to(popover, &at);
    gtk_popover_set_has_arrow(popover, FALSE);
    gtk_menu_button_popup(button);
}

// Where to land when there's nothing better: Today, unless it's hidden.
View Window::home_view() {
    auto smart = smart_views();
    if (std::ranges::find(smart, View{View::Today, ""}) != smart.end()) return View{View::Today, ""};
    auto all = sidebar_views(true);
    return all.empty() ? View{View::Today, ""} : all.front();
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
    auto files = store_->candidates();
    if (store_->sources().size() == 1)  // just the file names
        for (auto& f : files) f = f.substr(f.find('/') + 1);
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
        auto shown = store_->sources().size() == 1 ? name.substr(name.find('/') + 1) : name;
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), (shown + ".md").c_str());
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
        auto body = store_->label(*ref.list);
        if (!r.notes.empty()) body += " — " + r.notes.substr(0, r.notes.find('\n'));
        g_notification_set_body(n, body.c_str());
        g_notification_set_default_action_and_target(n, "app.show-reminder", "s", r.id.c_str());
        g_application_send_notification(G_APPLICATION(app_), ("reminder-" + r.id).c_str(), n);
        g_object_unref(n);
    }
    last_notify_check_ = now;
}

}  // namespace ui
