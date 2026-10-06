// The main window: sidebar of smart lists, lists and tags; the selected view
// on the right.
#pragma once

#include <adwaita.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "gtk_util.hpp"
#include "reminders/clipboard.hpp"
#include "reminders/history.hpp"
#include "reminders/importer.hpp"
#include "reminders/library.hpp"
#include "reminders/preferences.hpp"
#include "reminders/selection.hpp"
#include "reminders/sidebar.hpp"
#include "reminders/sync_runner.hpp"
#include "reminders/view.hpp"

namespace ui {

using View = rem::View;	 // what the window shows (reminders/view.hpp)

class Window {
	public:
		// Creates the window; it owns this object and deletes it when
		// destroyed. `folder` (from the command line) is opened instead of the
		// saved one, without replacing the saved one.
		static Window* create(
			AdwApplication* app,
			std::optional<std::filesystem::path> folder = std::nullopt);
		~Window();
		GtkWindow* gtk() const { return GTK_WINDOW(window_); }
		static Window* from(GtkWindow* window);
		// Opens a reminder's list and its details dialog (e.g. from a
		// notification).
		void show_reminder(const std::string& id);
		// Overrides the show-key-numbers setting (from the command line).
		void set_show_key_numbers(bool on);
		void
		open_settings();  // Settings…: settings.ini in the default text editor
		void reload_settings();	 // applies settings.ini as it is now
		void watch_settings();
		void apply_row_buttons();  // row-buttons=hover | always

	private:
		Window(AdwApplication* app,
			   std::optional<std::filesystem::path> folder);
		void build();
		void add_actions();

		// Sources. choose_folder: the welcome page's Choose Folder… (the
		// default source's folder).
		void choose_folder();
		void remove_source(const std::string& name);
		void source_info(const std::string& name);	// Source Info…
		void show_sources();						// Sources… (main menu)
		void add_source();							// Add Source…
	public:
		// With `folder` (from the command line), just that folder for this
		// session; otherwise every configured source, remembered.
		void open_sources(
			std::optional<std::filesystem::path> folder = std::nullopt);
		// Change Folder… / a folder chosen on the welcome page: it becomes the
		// default source's folder, then every source is opened.
		void open_folder(const std::filesystem::path& folder);

	private:
		void on_file_changed(GFile* file, GFile* other);
		void reload_pending();

		// Rendering. Everything is rebuilt from the store; lists are small.
		void refresh();
		void refresh_later(guint ms);
		void rebuild_sidebar();
		void rebuild_content();
		void select(View v);
		GtkWidget* build_list_view(rem::ListFile& list);
		GtkWidget* build_smart_view();
		std::vector<rem::Ref>
		view_refs();  // a smart, tag or search view's reminders
		GtkWidget* build_reminder_row(const rem::Ref& ref, bool show_list);
		GtkWidget* build_new_row(const std::string& list,
								 const std::optional<std::string>& section);
		GtkWidget* group(const std::string& title, const char* color,
						 GtkWidget* listbox);
		GtkWidget* clamp(GtkWidget* child);
		void update_clamp();
		GtkWidget* empty_state(const char* icon, const char* title,
							   const char* description);

		// Editing.
		void add_reminder(const std::string& list,
						  const std::optional<std::string>& section,
						  const std::string& text);
		void edit_title(const std::string& id, const std::string& text);
		void toggle_done(const std::string& id, bool done);	 // its check button
		// Edits of several reminders (a selection, or just one), each one undo
		// step that writes each list once.
		void complete_reminders(const std::vector<std::string>&
									ids);  // all done, or not if they all were
		void toggle_flag(const std::vector<std::string>&
							 ids);	// all flagged, or not if they all were
		void set_priority(const std::vector<std::string>& ids,
						  rem::Priority priority);
		void indent(const std::string& id, bool in);  // false: outdent
		std::vector<View> smart_views();
		// Your lists and tags the sidebar shows: hidden ones only with Show
		// Hidden.
		std::vector<rem::ListFile*> sidebar_lists(
			const std::string& source);	 // in lists-order
		std::vector<std::string> sidebar_tags();
		bool entry_hidden(
			const View& v);	 // hidden by the settings (shown or not)
		void set_entry_hidden(const View& v, bool hidden);
		// An entry's place among the others in its group (smart-lists,
		// lists-order, tags-order): Move Up / Down, Alt+↑/↓, or dragged onto
		// another entry of the group (`after`: below it).

		bool can_move_entry(const View& v, int delta);
		void move_entry(const View& v, int delta);
		bool same_sidebar_group(const View& a, const View& b);
		void drop_entry(const View& v, const View& target, bool after);
		void focus_entry(const View& v);
		void edit_tag(const std::string& tag);
		std::vector<View> sidebar_views(
			bool include_folded = false);  // in sidebar order
		std::vector<rem::SidebarGroup>
		showing_groups();  // the groups with something to show
		std::string group_title(
			const rem::SidebarGroup& group);  // "My Lists" with one source
		bool group_foldable(const rem::SidebarGroup& group);
		bool group_folded(const rem::SidebarGroup& group);
		void toggle_fold(const rem::SidebarGroup& group);
		void move_group(const rem::SidebarGroup& group, int delta);
		void drop_group(const rem::SidebarGroup& group,
						const rem::SidebarGroup& target,
						bool after);		   // dragged there
		std::vector<std::string> list_keys();  // every list, as "source/name"
		std::string list_label(
			const rem::ListFile&
				list);	// its name, or "source/name" if names clash
		rem::ListFile* list_by_label(const std::string& label);
		void sidebar_menu(GtkListBoxRow* row, double x, double y);
		View home_view();
		struct ViewInfo {
				View view;
				std::string title;
				const char* icon;
				std::string color;
		};
		ViewInfo view_info(const View& v);
		void quick_switcher();
		void step_view(int delta);
		void set_due(const std::vector<std::string>& ids, int days_from_today);
		void toggle_subtasks(const std::string& id);
		void show_content();   // on narrow windows, hides the overlaid sidebar
		void focus_results();  // from the search entry into the search results
		// Drag and drop (several reminders land together, in order).
		void move_reminders(const std::vector<std::string>& ids,
							const std::string& target,
							rem::Document::Place place);
		void move_to_section_end(const std::vector<std::string>& ids,
								 const std::string& list,
								 const std::optional<std::string>& section);
		void move_to_list(const std::vector<std::string>& ids,
						  const std::string& list);
		void move_step(const std::string& id, bool up);
		void setup_autoscroll();
		// Delete; with `cut`, Ctrl+X: copied first, the same single undo step.
		void delete_reminders(const std::vector<std::string>& ids,
							  bool cut = false);
		// Ctrl+C on reminders; Ctrl+V outside text fields (see clipboard.hpp).
		void copy_reminders(const std::vector<std::string>& ids);
		// Selecting several reminders: Ctrl+click, Shift+click, Shift+↑/↓,
		// Ctrl+A; Escape or a plain click clears it.
		std::vector<std::string> targets(
			const std::string& id);	 // what an action on id's row applies to
		std::vector<std::string> outermost(
			const std::vector<std::string>&
				ids);  // without subtasks whose parent is there
		void toggle_selected(const std::string& id);
		void select_range(const std::string& to, bool add);
		void extend_selection(const std::string& from, bool up);
		void select_all();
		void clear_selection();
		void
		update_selection();	 // row highlights and the header's "N Selected"
		void keep_focus(const std::vector<std::string>&
							ids);  // on the focused one of them after a rebuild
		// While a row's menu is open the row shows as selected, unless it's in
		// the selection already; it's unselected again when the menu closes.
		void select_for_menu(const std::string& id, GtkPopover* popover);
		// Any click in the window, on `hit`: one outside a title being edited
		// finishes the edit; one outside the reminder rows clears the
		// selection.
		void clicked(GtkWidget* hit, bool modified);
		GMenuModel* reminder_menu(
			const std::string& id,
			bool in_list);	// ⋮ / right-click, made as it opens
		// The same menu at the pointer (x, y in `row`), for a right-click or
		// long-press.
		void reminder_context_menu(GtkWidget* row, const std::string& id,
								   bool in_list, double x, double y);
		// After `anchor` (a reminder's menu), else after the focused one.
		void paste_reminders(std::optional<std::string> anchor = {});
		void add_pasted(const std::string& text,
						rem::TextSplit split = rem::TextSplit::Auto,
						bool offer_switch = true,
						std::optional<std::string> anchor = {});
		void paste_special();  // Ctrl+Shift+V: asks whether to split the lines
		// Pasted or dropped text. `offer_switch`: the message after several
		// lines offers to add them the other way (split / combined).
		void add_text(const std::string& text, const char* label,
					  const std::string& list_key,
					  const std::optional<std::string>& anchor,
					  rem::Document::Place place,
					  rem::TextSplit split = rem::TextSplit::Auto,
					  bool offer_switch = true);
		std::string reminders_text(
			const std::vector<std::string>&
				ids);  // as Markdown, for copying and dragging out
		void show_details(const std::string& id);
		void new_list(std::string source = {});
		// ☰ → Import…: a file of reminders (.ics, Markdown, text), into a new
		// or existing list.
		void import_file();
		// Files dropped on the window (into: the list in view) or on a list in
		// the sidebar (into: that list): an import dialog each, in turn.
		void import_files(std::vector<std::filesystem::path> files,
						  std::string into);
		// The import dialog for a file's text; `into` (a list key) is chosen
		// under Into to begin with. `then` runs once the dialog is answered.
		void import_tasks(const std::filesystem::path& file, std::string text,
						  std::string into = {},
						  std::function<void()> then = {});
		// ☰ → Export… and a list's ⋮ → Export…: lists (`chosen` ticked to
		// begin with) as Markdown, plain text, todo.txt, CSV or iCalendar.
		void export_lists(std::vector<std::string> chosen);
		void edit_list(const std::string& name);
		void delete_list(const std::string& name);
		void add_section(const std::string& list);
		GtkWidget* section_group(rem::ListFile& l, const std::string& name,
								 int count, GtkWidget* listbox);
		void rename_section(const std::string& list, const std::string& name);
		void delete_section(const std::string& list, const std::string& name,
							int count);
		void update_banner();
		template <class F>
		std::uint64_t undoable(const char* label, F&& f);
		template <class F>
		std::uint64_t batch(const char* label, F&& f);	// undoable, saves held
		void undo();
		void redo();
		void after_history(const rem::History::Result& result);
		void update_undo_actions();
		void review_candidates();
		void toast(const std::string& text, const char* button = nullptr,
				   std::function<void()> on_button = {});
		void check_notifications();

		AdwApplication* app_;
		GtkWidget* window_ = nullptr;
		GtkWidget* toasts_ = nullptr;
		GtkWidget* main_stack_ = nullptr;  // "welcome" / "main"
		GtkWidget* split_ = nullptr;
		GtkWidget* sidebar_list_ = nullptr;
		GtkWidget* sidebar_menu_button_ =
			nullptr;  // invisible; hosts the sidebar's context menu
		GtkWidget* content_menu_button_ =
			nullptr;  // invisible; hosts a reminder's context menu
		GtkWidget* search_bar_ = nullptr;
		GtkWidget* search_entry_ = nullptr;
		GtkWidget* content_page_ = nullptr;
		GtkWidget* content_title_ = nullptr;
		GtkWidget* content_scroller_ = nullptr;
		GtkWidget* list_menu_button_ = nullptr;
		GtkWidget* new_button_ = nullptr;
		GtkWidget* banner_ = nullptr;
		GtkWidget* clamp_ = nullptr;  // width limit of the current page
		GtkWidget* first_row_ =
			nullptr;  // first reminder row of the current page
		bool clamp_pending_ =
			false;	// offers Markdown checklists that aren't lists yet
		GSimpleAction* show_completed_action_ = nullptr;

		std::unique_ptr<rem::Library> store_;  // every source
		std::unique_ptr<rem::SyncRunner>
			sync_;	// CalDAV and WebDAV sources, synced in the background;
					// before store_ goes
		guint sync_timer_ = 0;	// shows its errors
		std::string last_sync_error_;
		void start_sync();
		void stop_sync();
		GSimpleAction* sync_action_ =
			nullptr;  // Sync All (enabled with CalDAV, WebDAV or git sources)
		struct FolderWatch {
				Obj<GFileMonitor> monitor;
				gulong handler = 0;
		};
		std::vector<FolderWatch> monitors_;	 // one per source's folder
		void stop_watching();
		Obj<GFileMonitor>
			settings_monitor_;	// settings.ini, to apply edits made elsewhere
		gulong settings_handler_ = 0;
		guint settings_timer_ = 0;
		std::string settings_text_;	 // settings.ini as last applied
		std::optional<bool>
			key_numbers_override_;	// --show-key-numbers / --hide-key-numbers
		GtkWidget* first_new_entry_ =
			nullptr;  // "New Reminder" entry of the current list
		std::set<std::string> pending_reload_;
		guint reload_timer_ = 0;
		guint refresh_timer_ = 0;
		guint notify_timer_ = 0;
		guint autoscroll_timer_ = 0;
		double autoscroll_y_ =
			-1;	 // pointer height over the content while dragging
		gint64 last_notify_check_ = 0;	// unix seconds
		bool updating_sidebar_ = false;
		bool show_completed_ = false;
		std::size_t note_lines_ = rem::load_note_lines();  // note-lines
		bool show_key_numbers_ = false;	 // settings.ini: show-key-numbers
		std::unique_ptr<rem::Sidebar>
			sidebar_;  // its layout (made with store_)
		GSimpleAction* show_hidden_action_ = nullptr;
		rem::SidebarGroup menu_group_;	// the sidebar menu's group
		GSimpleAction* collapsible_action_ =
			nullptr;  // the sidebar menu's "Collapsible" check item
		std::set<std::string>
			collapsed_;	 // reminders whose subtasks are hidden (this session)
		rem::Selection selection_;	// selected reminders, in the view shown
									// (and the anchor)
		std::optional<std::string>
			cursor_;  // the reminder row that last had focus
		std::vector<std::string>
			shown_ids_;	 // the content's reminder rows, top to bottom
		std::map<std::string, GtkWidget*>
			reminder_rows_;			  // by id, until the next rebuild
		std::string count_subtitle_;  // the header's count, shown when nothing
									  // is selected
		bool syncing_checks_ =
			false;	// setting rows' check buttons to match, not a click
		std::optional<std::string>
			menu_selected_;			 // selected just for its open menu
									 // (select_for_menu)
		bool follow_focus_ = false;	 // ↑/↓ with a selection: the row they move
									 // to becomes the selection
		bool remember_view_ =
			true;  // false for a folder opened just for this session
		View view_;
		std::optional<std::string> focus_new_row_;
		std::optional<std::string>
			focus_reminder_;  // reminder row that gets focus after a rebuild //
							  // list whose "new reminder" entry gets focus

		rem::History history_;
		int undo_depth_ = 0;
		GSimpleAction* undo_action_ = nullptr;
		GSimpleAction* redo_action_ = nullptr;
};

}  // namespace ui
