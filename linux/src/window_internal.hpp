// Internal to the main window's files (window*.cpp, sidebar.cpp, …): the
// widget, row and drag-and-drop helpers they share, and Window's edit
// templates. Not for other parts of the app.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dialogs.hpp"
#include "reminders/actions.hpp"
#include "reminders/backend_module.hpp"
#include "reminders/import_file.hpp"
#include "reminders/sources.hpp"
#include "reminders/view_model.hpp"
#include "support.hpp"
#include "window.hpp"

namespace ui {

namespace detail {

constexpr guint kCompleteDelayMs =
	900;  // a checked reminder lingers before it disappears

constexpr double kContentWidthShare =
	1.0;  // 0.98;  // lists' width as a share of the content area

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
	{View::AllReminders, "All Reminders", "edit-select-all-symbolic",
	 "gray"},  // completed too
	{View::Flagged, "Flagged", "sr-flag-symbolic", "orange"},
	{View::Completed, "Completed", "object-select-symbolic", "gray"},
};

// Dragged reminders' ids travel as this private type rather than as plain
// text, so text fields (the "New Reminder" entry, a title being edited)
// don't accept the drop and paste the ids into themselves. Several when a
// selection is dragged, top to bottom.
using Ids = std::vector<std::string>;

enum class DropStyle { Halves, Above, Into };

const SmartInfo* smart_info(View::Kind k);
std::string color_class(std::string_view color);
void chain_listboxes(const std::vector<GtkWidget*>& boxes);
void wrap_editable_label(GtkWidget* editable);
std::string trim(std::string_view s);
int open_count(rem::ListFile& l);
void set_row_view(GtkWidget* row, View v);
const View* row_view(GtkListBoxRow* row);
std::string jump_shortcut(std::size_t index);
GtkWidget* sidebar_row(const char* icon_name, std::string_view color,
					   const std::string& title, std::optional<int> count,
					   const std::string& shortcut = {});
void set_row_group(GtkWidget* row, const rem::SidebarGroup& group);
std::optional<rem::SidebarGroup> row_group(GtkListBoxRow* row);
GtkWidget* fold_heading(const std::string& text, bool collapsed,
						const rem::SidebarGroup& group);
GtkWidget* sidebar_heading(const std::string& text);
GtkWidget* boxed_list();
GMenu* menu_section(GMenu* menu);
std::string source_problem(const SourceEdit& e, const std::string& self);
using rem::list_name_error;
GType reminder_drag_type();
const Ids* dragged_ids(const GValue* value);
GtkWidget* owner(gpointer controller);
Obj<GdkPaintable> with_badge(GtkWidget* widget, GdkPaintable* image,
							 std::size_t count);
void make_draggable(GtkWidget* row, std::function<Ids()> ids,
					std::function<void(const Ids&, bool)> mark,
					std::function<std::string(const Ids&)> text);
void make_drop_target(GtkWidget* row, DropStyle style, std::string self,
					  std::function<void(Ids, rem::Document::Place)> on_drop);
std::vector<std::filesystem::path> dropped_files(const GValue* value);
void make_file_drop_target(
	GtkWidget* widget, bool highlight,
	std::function<void(std::vector<std::filesystem::path>)> on_drop);
GType entry_drag_type();
const View* dragged_entry(const GValue* value);
void make_entry_draggable(GtkWidget* row, const View& view);
void make_entry_drop_target(GtkWidget* row,
							std::function<bool(const View&)> accepts,
							std::function<void(View, bool)> on_drop);
GType group_drag_type();
const rem::SidebarGroup* dragged_group(const GValue* value);
std::vector<GtkWidget*> group_rows(GtkWidget* row,
								   const rem::SidebarGroup& group);
void make_group_draggable(GtkWidget* heading, const rem::SidebarGroup& group);
void make_group_drop_target(
	GtkWidget* row, const rem::SidebarGroup& group,
	std::function<void(rem::SidebarGroup, bool)> on_drop);
bool drop_offers(GdkDrop* drop, GType type);
bool dnd_debug();
std::string from_utf16(std::string_view data);
std::string decode_text(std::string data);
std::string html_text(const std::string& html);
std::string text_from(std::string_view mime, std::string data);
bool is_file_drop(GdkDrop* drop);
void make_text_drop_target(
	GtkWidget* widget, DropStyle style,
	std::function<void(std::optional<std::string>, rem::Document::Place)>
		on_drop);
std::string lower(std::string_view s);

}  // namespace detail

using namespace detail;

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

}  // namespace ui
