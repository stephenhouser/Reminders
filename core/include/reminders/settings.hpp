// Settings shared by every front end on a device (GUI, CLI, TUI), and the
// device's name for its per-device state folder.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rem {

namespace fs = std::filesystem;

// $XDG_CONFIG_HOME/reminders/settings.ini, or ~/.config/reminders/settings.ini.
fs::path settings_file();

// A value from the [general] section, or "" if unset.
std::string load_setting(const std::string& key);
// Sets a value in [general], keeping every other line of the file as it was.
void save_setting(const std::string& key, const std::string& value);

// A true/false setting ("true", "yes", "1" / "false", "no", "0"), or
// `fallback` if it's unset or unreadable.
bool load_bool_setting(const std::string& key, bool fallback = false);

// With the "show-key-numbers" setting, the terminal client shows sidebar
// entries 1–10 with the number key that jumps to them: "(1) Today" … "(0) …".
// `index` is 0-based.
std::string with_key_number(const std::string& title, std::size_t index, bool show);

// How a sidebar group appears: shown (smart lists without a heading, tags
// with a plain one), under a heading that folds the group, or not at all.
enum class GroupDisplay { Visible, Collapsible, Hidden };

// The smart lists (Today, Scheduled, All, Flagged, Completed), from the
// settings file:
//   smart-lists=today, scheduled, all, flagged, completed   (which, in order)
//   smart-lists-display=visible | collapsible | hidden
//   smart-lists-position=top | bottom                       (bottom: after lists and tags)
//   smart-lists-collapsed=true | false                      (set by the apps when folded)
struct SmartListsLayout {
    std::vector<std::string> shown{"today", "scheduled", "all", "flagged", "completed"};
    GroupDisplay display = GroupDisplay::Visible;
    bool at_bottom = false;
    bool collapsed = false;  // only meaningful when Collapsible

    bool hidden() const { return display == GroupDisplay::Hidden || shown.empty(); }
    bool has_heading() const { return display == GroupDisplay::Collapsible && !hidden(); }
    bool folded() const { return has_heading() && collapsed; }
};
SmartListsLayout load_smart_lists_layout();
void save_smart_lists_collapsed(bool collapsed);

// The Tags group:
//   tags-display=visible | collapsible | hidden
//   tags-collapsed=true | false   (set by the apps when folded)
struct TagsLayout {
    GroupDisplay display = GroupDisplay::Visible;
    bool collapsed = false;

    bool hidden() const { return display == GroupDisplay::Hidden; }
    bool foldable() const { return display == GroupDisplay::Collapsible; }
    bool folded() const { return foldable() && collapsed; }
};
TagsLayout load_tags_layout();
void save_tags_collapsed(bool collapsed);

// The folder chosen in the app ("folder" setting), if it is set and exists.
std::optional<fs::path> saved_folder();

// This device's name for <folder>/.reminders/<device>/: the host name plus 4
// hex digits from the machine id (so two machines both called "fedora" differ).
std::string device_name();

}  // namespace rem
