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

// The selection (top to bottom) when `id` is in it, else just `id`.
std::vector<std::string> Window::targets(const std::string& id) {
	if (!selected_.contains(id)) {
		return {id};
	}
	std::vector<std::string> out;
	for (auto& s : shown_ids_) {
		if (selected_.contains(s)) {
			out.push_back(s);
		}
	}
	return out;
}

// For moving, deleting and copying: a subtask goes along with its parent.
std::vector<std::string> Window::outermost(
	const std::vector<std::string>& ids) {
	std::vector<std::string> out;
	for (auto& id : ids) {
		auto ref = store_->find(id);
		if (ref && ref->parent &&
			std::ranges::find(ids, ref->parent->id) != ids.end()) {
			continue;
		}
		out.push_back(id);
	}
	return out;
}

void Window::toggle_selected(const std::string& id) {
	if (!selected_.erase(id)) {
		selected_.insert(id);
	}
	anchor_ = id;
	update_selection();
}

// From the anchor (the last reminder clicked, else the focused one) to `to`;
// `add` keeps what was already selected.
void Window::select_range(const std::string& to, bool add) {
	auto from = anchor_.value_or(cursor_.value_or(to));
	auto a = std::ranges::find(shown_ids_, from),
		 b = std::ranges::find(shown_ids_, to);
	if (b == shown_ids_.end()) {
		return;
	}
	if (a == shown_ids_.end()) {
		a = b;
		from = to;
	}
	if (a > b) {
		std::swap(a, b);
	}
	if (!add) {
		selected_.clear();
	}
	selected_.insert(a, b + 1);
	anchor_ = from;
	update_selection();
}

// Shift+↑/↓: selects from the anchor to the row above or below `from`, and
// moves the focus there.
void Window::extend_selection(const std::string& from, bool up) {
	auto at = std::ranges::find(shown_ids_, from);
	if (at == shown_ids_.end() ||
		(up ? at == shown_ids_.begin() : at + 1 == shown_ids_.end())) {
		return;
	}
	auto next = up ? *(at - 1) : *(at + 1);
	if (selected_.empty() || !anchor_) {
		anchor_ = from;
	}
	select_range(next, false);
	if (auto row = reminder_rows_.find(next); row != reminder_rows_.end()) {
		gtk_widget_grab_focus(row->second);
	}
}

void Window::select_all() {
	if (shown_ids_.empty()) {
		return;
	}
	selected_ = {shown_ids_.begin(), shown_ids_.end()};
	if (!anchor_) {
		anchor_ = shown_ids_.front();
	}
	update_selection();
	// The keys that act on the selection work from a reminder row: focus the
	// first one unless one has the focus already (say, just after opening
	// the list from the sidebar).
	for (auto* w = gtk_root_get_focus(GTK_ROOT(window_)); w;
		 w = gtk_widget_get_parent(w)) {
		if (GTK_IS_LIST_BOX_ROW(w) &&
			g_object_get_data(G_OBJECT(w), "reminder-id")) {
			return;
		}
	}
	gtk_widget_grab_focus(reminder_rows_[shown_ids_.front()]);
}

void Window::clear_selection() {
	if (selected_.empty()) {
		return;
	}
	selected_.clear();
	update_selection();
}

void Window::update_selection() {
	for (auto& [id, row] : reminder_rows_) {
		if (selected_.contains(id)) {
			gtk_widget_add_css_class(row, "selected-reminder");
		} else {
			gtk_widget_remove_css_class(row, "selected-reminder");
		}
	}
	auto subtitle = selected_.size() < 2
					  ? count_subtitle_
					  : std::format("{} Selected", selected_.size());
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(content_title_),
								  subtitle.c_str());
}

void Window::keep_focus(const std::vector<std::string>& ids) {
	if (ids.empty()) {
		return;
	}
	focus_reminder_ = cursor_ && std::ranges::find(ids, *cursor_) != ids.end()
						? *cursor_
						: ids.front();
}

}  // namespace ui
