// The reminder details dialog and the new/edit list dialog.
#pragma once

#include <adwaita.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "reminders/model.hpp"

namespace ui {

struct ReminderEdit {
    rem::LineFields fields;
    std::string notes;
    std::string list;                      // possibly a different list: move it there
    std::vector<std::string> new_subtasks;  // titles to add
    bool deleted = false;
};

// `lists` is every list name, in sidebar order. `on_done` runs on "Done"
// (or "Delete"); nothing happens on "Cancel".
// Subtasks can't have subtasks of their own (`is_subtask`).
void show_reminder_dialog(GtkWidget* parent, const rem::Reminder& reminder, bool is_subtask,
                          const std::string& list, const std::vector<std::string>& lists,
                          std::function<void(ReminderEdit)> on_done);

struct ListEdit {
    std::string name;
    std::string color = "blue";
    std::string icon = "list";
};

// `existing` empty → "New List". `validate` returns an error to show, or "".
void show_list_dialog(GtkWidget* parent, std::optional<ListEdit> existing,
                      std::function<std::string(const ListEdit&)> validate,
                      std::function<void(ListEdit)> on_done);

}  // namespace ui
