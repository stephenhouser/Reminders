// Settings shared by every front end on a device (GUI, CLI, TUI), and the
// device's name for its per-device state folder.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

// How a sidebar group appears: shown (with a plain heading, except the group
// at the top, which has none), under a heading that folds the group, or not
// at all.
enum class GroupDisplay { Visible, Collapsible, Hidden };

// The sidebar's groups, in the order the settings give:
//   sidebar-order=smart-lists, my-lists, tags
// A group left out or misspelled goes after the others, in this default order.
enum class SidebarGroup { SmartLists, MyLists, Tags };
std::vector<SidebarGroup> load_sidebar_order();
void save_sidebar_order(const std::vector<SidebarGroup>& order);
// "Smart Lists", "My Lists", "Tags".
const char* group_title(SidebarGroup group);
// Moves `group` past the next group showing before it (delta < 0) or after
// it (delta > 0), skipping groups not in `showing`. False if it's already
// first or last.
bool move_sidebar_group(std::vector<SidebarGroup>& order, SidebarGroup group, int delta,
                        const std::vector<SidebarGroup>& showing);

// The smart lists (Today, Scheduled, All, Flagged, Completed), from the
// settings file:
//   smart-lists=today, scheduled, all, flagged, completed   (which, in order)
//   smart-lists-display=visible | collapsible | hidden
//   smart-lists-collapsed=true | false                      (set by the apps when folded)
struct SmartListsLayout {
    std::vector<std::string> shown{"today", "scheduled", "all", "all-reminders", "flagged", "completed"};
    GroupDisplay display = GroupDisplay::Visible;
    bool collapsed = false;  // only meaningful when Collapsible

    bool hidden() const { return display == GroupDisplay::Hidden || shown.empty(); }
    bool foldable() const { return display == GroupDisplay::Collapsible && !hidden(); }
    bool folded() const { return foldable() && collapsed; }
};
SmartListsLayout load_smart_lists_layout();

// The My Lists and Tags groups:
//   my-lists-display=visible | collapsible      (your lists can't be hidden)
//   tags-display=visible | collapsible | hidden
//   my-lists-collapsed, tags-collapsed=true | false   (set by the apps when folded)
struct GroupLayout {
    GroupDisplay display = GroupDisplay::Visible;
    bool collapsed = false;

    bool hidden() const { return display == GroupDisplay::Hidden; }
    bool foldable() const { return display == GroupDisplay::Collapsible; }
    bool folded() const { return foldable() && collapsed; }
};
GroupLayout load_my_lists_layout();
GroupLayout load_tags_layout();

// Remembers whether a group is folded (smart-lists-collapsed, …).
void save_group_collapsed(SidebarGroup group, bool collapsed);
// Sets how a group appears (smart-lists-display, …).
void save_group_display(SidebarGroup group, GroupDisplay display);

// A comma-separated list of names; a name with a comma in it is quoted:
//   lists-hidden=Work, "Smith, Jo"
std::vector<std::string> load_names_setting(const std::string& key);
void save_names_setting(const std::string& key, const std::vector<std::string>& names);

// Hidden sidebar entries. They show (dimmed) only with show-hidden=true,
// which the app's main menu toggles:
//   lists-hidden=Work, Home     tags-hidden=errands, frontend
// Smart lists are hidden by leaving them out of smart-lists.
struct HiddenEntries {
    std::vector<std::string> lists, tags;
    bool show = false;  // show-hidden

    bool list_hidden(std::string_view name) const;
    bool tag_hidden(std::string_view tag) const;
};
HiddenEntries load_hidden();
void set_list_hidden(const std::string& name, bool hidden);
void set_tag_hidden(const std::string& tag, bool hidden);
// Leaves a smart list ("today", …) out of smart-lists, or puts it back at the end.
void set_smart_list_hidden(const std::string& name, bool hidden);
void save_show_hidden(bool show);

// Writes smart-lists (which smart lists show, in order; "none" if empty).
void save_smart_lists(const std::vector<std::string>& shown);

// The sidebar's tag order: those in tags-order first, in that order, then the
// rest alphabetically.   tags-order=work, errands
std::vector<std::string> order_tags(std::vector<std::string> tags);

// The sidebar's order for your lists, on this device: those in lists-order
// first, in that order, then the rest in `names`' order (the files' own
// order: field, then name).   lists-order=Groceries, "Smith, Jo", Work
std::vector<std::string> order_lists(const std::vector<std::string>& names);

// Moves `name` past the next entry in `showing` before it (delta < 0) or
// after it, skipping entries not showing. False if it's already at that end.
bool move_in_order(std::vector<std::string>& order, const std::string& name, int delta,
                   const std::vector<std::string>& showing);

// A tag's colour and icon in the sidebar (one of kColors / kIcons):
//   tag-color.errands=orange    tag-icon.errands=cart
struct TagStyle {
    std::string color = "gray";
    std::string icon = "tag";
};
TagStyle load_tag_style(const std::string& tag);
void save_tag_style(const std::string& tag, const TagStyle& style);

// The folder chosen in the app ("folder" setting), if it is set and exists.
std::optional<fs::path> saved_folder();

// This device's name for <folder>/.reminders/<device>/: the host name plus 4
// hex digits from the machine id (so two machines both called "fedora" differ).
std::string device_name();

}  // namespace rem
