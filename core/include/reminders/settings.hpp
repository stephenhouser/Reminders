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

// The same in another section ("source.personal" is [source.personal]); a
// missing section is added at the end of the file.
std::string load_section_setting(const std::string& section, const std::string& key);
void save_section_setting(const std::string& section, const std::string& key, const std::string& value);
// Every [section] in the file, in order ("general", "source.personal", …).
std::vector<std::string> section_names();
// Removes a section and everything in it.
void remove_section(const std::string& section);

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

// A sidebar group: the smart lists, one source's lists, or the tags.
struct SidebarGroup {
    enum Kind { SmartLists, Lists, Tags };
    Kind kind = SmartLists;
    std::string source;  // Lists: the source's name

    bool operator==(const SidebarGroup&) const = default;
    static SidebarGroup smart_lists() { return {SmartLists, {}}; }
    static SidebarGroup tags() { return {Tags, {}}; }
    static SidebarGroup lists(std::string source) { return {Lists, std::move(source)}; }
};

// The sidebar's groups, in the order the settings give:
//   sidebar-order=smart-lists, lists:home, lists:work, tags
// "my-lists" stands for every source not named on its own (with one source,
// it's the only lists group). A group left out or unknown goes after the
// others, in the default order: smart lists, each source, tags. `sources`
// are the sources' names, in order.
std::vector<SidebarGroup> load_sidebar_order(const std::vector<std::string>& sources);
void save_sidebar_order(const std::vector<SidebarGroup>& order);
// "Smart Lists", "Tags", and for a lists group `lists_title` ("My Lists" with
// one source, else the source's title).
std::string group_title(const SidebarGroup& group, const std::string& lists_title = "My Lists");
// Moves `group` past the next group showing before it (delta < 0) or after
// it (delta > 0), skipping groups not in `showing`. False if it's already
// first or last.
bool move_sidebar_group(std::vector<SidebarGroup>& order, const SidebarGroup& group, int delta,
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

// The lists groups and the Tags group:
//   my-lists-display=visible | collapsible      (every source's lists; can't be hidden)
//   tags-display=visible | collapsible | hidden
//   lists-collapsed.NAME, tags-collapsed=true | false   (set by the apps when folded)
struct GroupLayout {
    GroupDisplay display = GroupDisplay::Visible;
    bool collapsed = false;

    bool hidden() const { return display == GroupDisplay::Hidden; }
    bool foldable() const { return display == GroupDisplay::Collapsible; }
    bool folded() const { return foldable() && collapsed; }
};
GroupLayout load_lists_layout(const std::string& source);
GroupLayout load_tags_layout();

// Remembers whether a group is folded (smart-lists-collapsed,
// lists-collapsed.NAME, tags-collapsed).
void save_group_collapsed(const SidebarGroup& group, bool collapsed);
// Sets how a group appears (smart-lists-display, my-lists-display for every
// source's lists, tags-display).
void save_group_display(const SidebarGroup& group, GroupDisplay display);

// A comma-separated list of names; a name with a comma in it is quoted:
//   lists-hidden=Work, "Smith, Jo"
std::vector<std::string> load_names_setting(const std::string& key);
void save_names_setting(const std::string& key, const std::vector<std::string>& names);

// Lists in settings are named "source/list". A bare "list" (as written
// before sources) still matches a list of that name in any source.
bool list_entry_matches(std::string_view entry, std::string_view key);

// Hidden sidebar entries. They show (dimmed) only with show-hidden=true,
// which the app's main menu toggles:
//   lists-hidden=Work, Home     tags-hidden=errands, frontend
// Smart lists are hidden by leaving them out of smart-lists.
struct HiddenEntries {
    std::vector<std::string> lists, tags;
    bool show = false;  // show-hidden

    bool list_hidden(std::string_view key) const;  // key: "source/list"
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

// show-sidebar=true | false: whether the sidebar is shown (Ctrl+B), saved by
// both apps and read at start-up (load_bool_setting("show-sidebar", true)).


// This device's name for <folder>/.reminders/<device>/: the host name plus 4
// hex digits from the machine id (so two machines both called "fedora" differ).
std::string device_name();

}  // namespace rem
