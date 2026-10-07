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

namespace ui::detail {

const SmartInfo* smart_info(View::Kind k) {
	for (auto& s : kSmart) {
		if (s.kind == k) {
			return &s;
		}
	}
	return nullptr;
}

std::string color_class(std::string_view color) {
	return "color-" + std::string(color);
}

// Up/Down at the end of one section's list continue into the next one.
void chain_listboxes(const std::vector<GtkWidget*>& boxes) {
	for (std::size_t i = 0; i < boxes.size(); ++i) {
		GtkWidget* prev = i > 0 ? boxes[i - 1] : nullptr;
		GtkWidget* next = i + 1 < boxes.size() ? boxes[i + 1] : nullptr;
		connect<gboolean(GtkWidget*, GtkDirectionType)>(
			boxes[i], "keynav-failed",
			[prev, next](GtkWidget*, GtkDirectionType dir) -> gboolean {
				GtkListBoxRow* target = nullptr;
				if (dir == GTK_DIR_DOWN && next) {
					target =
						gtk_list_box_get_row_at_index(GTK_LIST_BOX(next), 0);
				} else if (dir == GTK_DIR_UP && prev) {
					for (int j = 0; auto* r = gtk_list_box_get_row_at_index(
										GTK_LIST_BOX(prev), j);
						 ++j) {
						target = r;
					}
				}
				if (!target) {
					return FALSE;
				}
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
		for (auto* c = gtk_widget_get_first_child(w); c;
			 c = gtk_widget_get_next_sibling(c)) {
			todo.push_back(c);
		}
	}
}

std::string trim(std::string_view s) {
	auto b = s.find_first_not_of(" \t\n");
	if (b == std::string_view::npos) {
		return {};
	}
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

GtkWidget* sidebar_row(const char* icon_name, std::string_view color,
					   const std::string& title, std::optional<int> count) {
	auto* row = gtk_list_box_row_new();
	auto* box = hbox(12);
	auto* img = icon(icon_name, {"list-icon"});
	gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(img, color_class(color).c_str());
	auto* name = label(title);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(name, TRUE);
	append(box, {img, name});
	// A fixed-width, right-aligned count column (empty for tags).
	auto* count_label = label(count ? std::to_string(*count) : "",
							  {"dim-label", "numeric", "sidebar-count"});
	gtk_label_set_xalign(GTK_LABEL(count_label), 1);
	append(box, {count_label});
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	return row;
}

// Every sidebar row carries its group, for the menu and Alt+↑/↓.
void set_row_group(GtkWidget* row, const rem::SidebarGroup& group) {
	g_object_set_data_full(
		G_OBJECT(row), "sidebar-group", new rem::SidebarGroup(group),
		[](gpointer p) { delete static_cast<rem::SidebarGroup*>(p); });
}

std::optional<rem::SidebarGroup> row_group(GtkListBoxRow* row) {
	auto* g = row ? static_cast<rem::SidebarGroup*>(
						g_object_get_data(G_OBJECT(row), "sidebar-group"))
				  : nullptr;
	if (!g) {
		return std::nullopt;
	}
	return *g;
}

// A collapsible group's heading, which folds or unfolds the group when
// clicked. "fold-group" marks it for the click handler.
GtkWidget* fold_heading(const std::string& text, bool collapsed,
						const rem::SidebarGroup& group) {
	auto* row = gtk_list_box_row_new();
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
	auto* box = hbox(6);
	auto* l = label(text.c_str(), {"heading", "dim-label"});
	gtk_widget_set_hexpand(l, TRUE);
	append(box, {l, icon(collapsed ? "pan-end-symbolic" : "pan-down-symbolic",
						 {"dim-label"})});
	gtk_widget_add_css_class(box, "sidebar-heading");
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_widget_set_tooltip_text(
		row, std::format("{} {}", collapsed ? "Show" : "Hide", text).c_str());
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
	// gtk_widget_add_css_class(list, "boxed-list");
	gtk_widget_add_css_class(list, "reminder-list");
	return list;
}

GMenu* menu_section(GMenu* menu) {
	auto* section = g_menu_new();
	g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
	g_object_unref(section);
	return section;
}

void menu_append_accel(GMenu* menu, const char* label, const char* action,
					   const char* accel) {
	auto* item = g_menu_item_new(label, action);
	g_menu_item_set_attribute(item, "accel", "s", accel);
	g_menu_append_item(menu, item);
	g_object_unref(item);
}

// What's wrong with a CalDAV or WebDAV account's settings, or "".
// What's wrong with a source in Add Source / Source Info, or "". `self` is
// the source being edited ("" for a new one).
std::string source_problem(const SourceEdit& e, const std::string& self) {
	auto config = e.config();
	auto& module = rem::backend_of(config);
	if (module.synced && !module.sync) {
		return std::format(
			"This copy of Reminders was built without {} support",
			module.title);
	}
	if (module.problem) {
		if (auto p = module.problem(config); !p.empty()) {
			return p;
		}
	}
	std::error_code ec;
	for (auto& s : rem::load_sources(e.profile)) {
		if (s.name != self &&
			(s.folder == e.folder ||
			 std::filesystem::equivalent(s.folder, e.folder, ec))) {
			return std::format("That folder is already the source “{}”",
							   rem::source_title(s));
		}
	}
	return {};
}

// Validates a list name for a file that must work on every synced platform.
std::string lower(std::string_view s) {
	std::string out(s);
	for (auto& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

}  // namespace ui::detail
