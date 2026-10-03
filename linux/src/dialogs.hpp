// The reminder details dialog and the new/edit list dialog.
#pragma once

#include <adwaita.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <filesystem>

#include "reminders/backend.hpp"
#include "reminders/model.hpp"
#include "reminders/sources.hpp"

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

// "Tag Info": the same colour and icon choices, without a name to edit
// (`style.name` is shown as the subtitle, e.g. "#errands").
void show_tag_dialog(GtkWidget* parent, const std::string& tag, ListEdit style,
                     std::function<void(ListEdit)> on_done);

// "Source Info": a source's title, type (back end), folder and, for CalDAV,
// its server; and whether new lists go into it. Its name ([source.NAME]) is
// fixed: settings refer to it. With `is_new` it's "Add Source": the name
// comes from the title, and a CalDAV source's folder defaults to its place
// in $XDG_DATA_HOME. The rows change with the type.
struct SourceEdit {
    std::string name;
    std::string title;  // empty: from the name
    rem::BackendKind backend = rem::BackendKind::Syncthing;
    std::filesystem::path folder;
    bool is_default = false;
    rem::CaldavSettings caldav = {};
    bool is_new = false;
};
// `validate` returns an error to show, or "". `on_remove` runs when Remove
// Source… is pressed (the dialog closes first).
void show_source_dialog(GtkWidget* parent, SourceEdit source, std::function<std::string(const SourceEdit&)> validate,
                        std::function<void(SourceEdit)> on_done, std::function<void()> on_remove);

// "Sources": every source, each opening its Source Info; and Add Source….
struct SourceRow {
    std::string name, title, detail;
};
void show_sources_dialog(GtkWidget* parent, const std::vector<SourceRow>& rows, std::function<void(std::string)> on_open,
                         std::function<void()> on_add);

}  // namespace ui
