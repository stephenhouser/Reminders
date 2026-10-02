// The main window: sidebar of smart lists, lists and tags; the selected view
// on the right.
#pragma once

#include <adwaita.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "gtk_util.hpp"
#include "reminders/history.hpp"
#include "reminders/store.hpp"

namespace ui {

struct View {
    enum Kind { Today, Scheduled, All, Flagged, Completed, List, Tag, Search } kind = Today;
    std::string name;  // list name, tag, or search text
    bool operator==(const View&) const = default;
};

class Window {
public:
    // Creates the window; it owns this object and deletes it when destroyed.
    // `folder` (from the command line) is opened instead of the saved one,
    // without replacing the saved one.
    static Window* create(AdwApplication* app, std::optional<std::filesystem::path> folder = std::nullopt);
    ~Window();
    GtkWindow* gtk() const { return GTK_WINDOW(window_); }
    static Window* from(GtkWindow* window);
    // Opens a reminder's list and its details dialog (e.g. from a notification).
    void show_reminder(const std::string& id);

private:
    Window(AdwApplication* app, std::optional<std::filesystem::path> folder);
    void build();
    void add_actions();

    // Folder handling.
    void choose_folder();
public:
    // `remember` makes it the folder opened on the next start.
    void open_folder(const std::filesystem::path& folder, bool remember = true);

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
    GtkWidget* build_reminder_row(const rem::Ref& ref, bool show_list);
    GtkWidget* build_new_row(const std::string& list, const std::optional<std::string>& section);
    GtkWidget* group(const std::string& title, const char* color, GtkWidget* listbox);
    GtkWidget* clamp(GtkWidget* child);
    void update_clamp();
    GtkWidget* empty_state(const char* icon, const char* title, const char* description);

    // Editing.
    void add_reminder(const std::string& list, const std::optional<std::string>& section,
                      const std::string& text);
    void edit_title(const std::string& id, const std::string& text);
    void toggle_done(const std::string& id, bool done);
    void toggle_flag(const std::string& id);
    void set_priority(const std::string& id, rem::Priority priority);
    void indent(const std::string& id, bool in);  // false: outdent
    std::vector<View> sidebar_views();             // in sidebar order
    struct ViewInfo {
        View view;
        std::string title;
        const char* icon;
        std::string color;
    };
    ViewInfo view_info(const View& v);
    void quick_switcher();
    void step_view(int delta);
    void set_due(const std::string& id, int days_from_today);
    void toggle_subtasks(const std::string& id);
    void show_content();  // on narrow windows, hides the overlaid sidebar
    void focus_results();  // from the search entry into the search results
    // Drag and drop.
    void move_reminder(const std::string& id, const std::string& target, rem::Document::Place place);
    void move_to_section_end(const std::string& id, const std::string& list,
                             const std::optional<std::string>& section);
    void move_to_list(const std::string& id, const std::string& list);
    void move_step(const std::string& id, bool up);
    void setup_autoscroll();
    void delete_reminder(const std::string& id);
    void show_details(const std::string& id);
    void new_list();
    void edit_list(const std::string& name);
    void delete_list(const std::string& name);
    void add_section(const std::string& list);
    GtkWidget* section_group(rem::ListFile& l, const std::string& name, int count, GtkWidget* listbox);
    void rename_section(const std::string& list, const std::string& name);
    void delete_section(const std::string& list, const std::string& name, int count);
    void update_banner();
    template <class F> std::uint64_t undoable(const char* label, F&& f);
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
    GtkWidget* search_bar_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    GtkWidget* content_page_ = nullptr;
    GtkWidget* content_title_ = nullptr;
    GtkWidget* content_scroller_ = nullptr;
    GtkWidget* list_menu_button_ = nullptr;
    GtkWidget* new_button_ = nullptr;
    GtkWidget* banner_ = nullptr;
    GtkWidget* clamp_ = nullptr;   // width limit of the current page
    GtkWidget* first_row_ = nullptr;  // first reminder row of the current page
    bool clamp_pending_ = false;  // offers Markdown checklists that aren't lists yet
    GSimpleAction* show_completed_action_ = nullptr;

    std::unique_ptr<rem::Store> store_;
    Obj<GFileMonitor> monitor_;
    gulong monitor_handler_ = 0;
    GtkWidget* first_new_entry_ = nullptr;  // "New Reminder" entry of the current list
    std::set<std::string> pending_reload_;
    guint reload_timer_ = 0;
    guint refresh_timer_ = 0;
    guint notify_timer_ = 0;
    guint autoscroll_timer_ = 0;
    double autoscroll_y_ = -1;  // pointer height over the content while dragging
    gint64 last_notify_check_ = 0;  // unix seconds
    bool updating_sidebar_ = false;
    bool show_completed_ = false;
    std::set<std::string> collapsed_;  // reminders whose subtasks are hidden (this session)
    bool remember_view_ = true;  // false for a folder opened just for this session
    View view_;
    std::optional<std::string> focus_new_row_;
    std::optional<std::string> focus_reminder_;  // reminder row that gets focus after a rebuild  // list whose "new reminder" entry gets focus

    rem::History history_;
    int undo_depth_ = 0;
    GSimpleAction* undo_action_ = nullptr;
    GSimpleAction* redo_action_ = nullptr;

};

}  // namespace ui
