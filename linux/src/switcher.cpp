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

namespace {

// How well `query` matches `title` (lower is better), or -1 for no match:
// a prefix, then the start of a word, then anywhere, then letters in order.
int match_score(const std::string& title, const std::string& query) {
	auto t = lower(title), q = lower(query);
	if (!t.empty() && t[0] == '#' && !q.empty() && q[0] != '#') {
		t.erase(0, 1);
	}
	if (q.empty() || t.starts_with(q)) {
		return 0;
	}
	if (auto at = t.find(" " + q); at != std::string::npos) {
		return 1;
	}
	if (t.find(q) != std::string::npos) {
		return 2;
	}
	std::size_t i = 0;
	for (char c : t) {
		if (i < q.size() && c == q[i]) {
			++i;
		}
	}
	return i == q.size() ? 3 : -1;
}

}  // namespace

Window::ViewInfo Window::view_info(const View& v) {
	if (auto* s = smart_info(v.kind)) {
		return {v, s->title, s->icon, s->color};
	}
	if (v.kind == View::List) {
		auto* l = store_->list(v.name);
		return {v, l ? store_->label(*l) : v.name,
				l ? list_icon_name(l->icon()) : "view-list-bullet-symbolic",
				l ? l->color() : "gray"};
	}
	if (v.kind == View::Tag) {
		auto style = rem::load_tag_style(v.name);
		return {v, "#" + v.name, list_icon_name(style.icon), style.color};
	}
	return {v, std::format("Search for “{}”", v.name), "edit-find-symbolic",
			"gray"};
}

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
	for (auto& v : sidebar_views(true)) {
		st->all.push_back(view_info(v));
	}

	st->entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(st->entry),
										  "Go to a list or tag…");
	gtk_widget_set_hexpand(st->entry, TRUE);
	st->list = gtk_list_box_new();
	gtk_widget_add_css_class(st->list, "navigation-sidebar");
	auto* scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
								   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), st->list);
	gtk_widget_set_vexpand(scroller, TRUE);

	auto fill = [this, st] {
		gtk_list_box_remove_all(GTK_LIST_BOX(st->list));
		st->shown.clear();
		std::string query =
			trim(gtk_editable_get_text(GTK_EDITABLE(st->entry)));
		std::vector<std::pair<int, const ViewInfo*>> hits;
		for (auto& info : st->all) {
			if (int score = match_score(info.title, query); score >= 0) {
				hits.emplace_back(score, &info);
			}
		}
		std::ranges::stable_sort(hits, {},
								 &std::pair<int, const ViewInfo*>::first);
		std::vector<ViewInfo> rows;
		for (auto& [_, info] : hits) {
			rows.push_back(*info);
		}
		if (!query.empty()) {
			rows.push_back(
				view_info(View{View::Search, query}));	// always offer a search
		}
		for (auto& info : rows) {
			gtk_list_box_append(
				GTK_LIST_BOX(st->list),
				sidebar_row(info.icon, info.color, info.title, std::nullopt));
			st->shown.push_back(info.view);
		}
		if (auto* first =
				gtk_list_box_get_row_at_index(GTK_LIST_BOX(st->list), 0)) {
			gtk_list_box_select_row(GTK_LIST_BOX(st->list), first);
		}
	};
	auto go = [this, st](int index) {
		if (index < 0 || index >= static_cast<int>(st->shown.size())) {
			return;
		}
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
	on(st->entry, "stop-search",
	   [st] { adw_dialog_close(ADW_DIALOG(st->dialog)); });
	on(st->entry, "activate", [go, selected] { go(selected()); });
	connect<void(GtkListBox*, GtkListBoxRow*)>(
		st->list, "row-activated", [go](GtkListBox*, GtkListBoxRow* row) {
			go(gtk_list_box_row_get_index(row));
		});
	// ↑/↓ move the selection while typing continues in the entry.
	auto* keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	connect<gboolean(GtkEventControllerKey*, guint, guint, GdkModifierType)>(
		keys, "key-pressed",
		[st, selected](GtkEventControllerKey*, guint key, guint,
					   GdkModifierType) -> gboolean {
			int delta = key == GDK_KEY_Down ? 1 : key == GDK_KEY_Up ? -1 : 0;
			if (!delta) {
				return FALSE;
			}
			if (auto* row = gtk_list_box_get_row_at_index(
					GTK_LIST_BOX(st->list), selected() + delta)) {
				gtk_list_box_select_row(GTK_LIST_BOX(st->list), row);
				gtk_widget_grab_focus(st->entry);  // selecting can move focus;
												   // keep typing in the entry
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

}  // namespace ui
