#include "reminders/settings.hpp"

#include "reminders/model.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

namespace rem {

namespace {

std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string trimmed(std::string_view s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t");
    return std::string(s.substr(b, e - b + 1));
}

// Locates `key` in [section]: the line's index, or where to insert it.
struct Found {
    std::optional<std::size_t> line;
    std::optional<std::size_t> section_end;  // insert position if the key is missing
};

Found find_key(const std::vector<std::string>& lines, const std::string& section, const std::string& key) {
    Found f;
    bool in_section = false;
    auto header = "[" + section + "]";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto t = trimmed(lines[i]);
        if (t.starts_with('[')) {
            in_section = t == header;
            if (in_section) f.section_end = i + 1;
            continue;
        }
        if (!in_section) continue;
        if (!t.empty() && !t.starts_with('#') && !t.starts_with(';')) f.section_end = i + 1;
        auto eq = t.find('=');
        if (eq != std::string::npos && trimmed(t.substr(0, eq)) == key) f.line = i;
    }
    return f;
}

}  // namespace

fs::path settings_file() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "reminders" / "settings.ini";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".config" / "reminders" / "settings.ini";
}

std::string load_setting(const std::string& key) { return load_section_setting("general", key); }

void save_setting(const std::string& key, const std::string& value) { save_section_setting("general", key, value); }

std::string load_section_setting(const std::string& section, const std::string& key) {
    auto lines = read_lines(settings_file());
    auto f = find_key(lines, section, key);
    if (!f.line) return {};
    auto& l = lines[*f.line];
    return trimmed(std::string_view(l).substr(l.find('=') + 1));
}

std::vector<std::string> section_names() {
    std::vector<std::string> out;
    for (auto& l : read_lines(settings_file())) {
        auto t = trimmed(l);
        if (t.size() > 2 && t.front() == '[' && t.back() == ']') {
            auto name = t.substr(1, t.size() - 2);
            if (std::ranges::find(out, name) == out.end()) out.push_back(name);
        }
    }
    return out;
}

namespace {

void write_lines(const fs::path& file, const std::vector<std::string>& lines) {
    fs::create_directories(file.parent_path());
    auto tmp = file.parent_path() / ".settings.ini.tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (auto& l : lines) out << l << '\n';
    }
    fs::rename(tmp, file);
}

}  // namespace

void remove_section(const std::string& section) {
    auto file = settings_file();
    std::vector<std::string> kept;
    bool in_section = false;
    for (auto& l : read_lines(file)) {
        auto t = trimmed(l);
        if (t.starts_with('[')) in_section = t == "[" + section + "]";
        if (!in_section) kept.push_back(l);
    }
    while (kept.size() >= 2 && kept.back().empty() && kept[kept.size() - 2].empty()) kept.pop_back();
    write_lines(file, kept);
}

void save_section_setting(const std::string& section, const std::string& key, const std::string& value) {
    auto file = settings_file();
    auto lines = read_lines(file);
    auto f = find_key(lines, section, key);
    auto entry = key + "=" + value;
    if (f.line) {
        lines[*f.line] = entry;
    } else if (f.section_end) {
        lines.insert(lines.begin() + static_cast<long>(*f.section_end), entry);
    } else {  // a new section at the end
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.push_back("[" + section + "]");
        lines.push_back(entry);
    }
    fs::create_directories(file.parent_path());
    auto tmp = file.parent_path() / ".settings.ini.tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (auto& l : lines) out << l << '\n';
    }
    fs::rename(tmp, file);
}

bool load_bool_setting(const std::string& key, bool fallback) {
    std::string v;
    for (char c : load_setting(key)) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "true" || v == "yes" || v == "1" || v == "on") return true;
    if (v == "false" || v == "no" || v == "0" || v == "off") return false;
    return fallback;
}

std::string with_key_number(const std::string& title, std::size_t index, bool show) {
    if (!show || index >= 10) return title;
    return std::format("({}) {}", (index + 1) % 10, title);
}

// "visible" (the default), "collapsible" (or "collapsable") or "hidden".
GroupDisplay load_display(const std::string& key) {
    std::string v;
    for (char c : load_setting(key)) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "collapsible" || v == "collapsable") return GroupDisplay::Collapsible;
    if (v == "hidden" || v == "hide" || v == "none") return GroupDisplay::Hidden;
    return GroupDisplay::Visible;
}

SmartListsLayout load_smart_lists_layout() {
    static const std::vector<std::string> known = {"today", "scheduled", "all", "all-reminders", "flagged",
                                                   "completed"};
    SmartListsLayout layout;
    auto value = load_setting("smart-lists");
    if (!value.empty()) {
        layout.shown.clear();
        std::string word;
        for (char c : value + ",") {
            if (c == ',' || c == ' ' || c == '\t') {
                for (auto& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                bool ok = std::find(known.begin(), known.end(), word) != known.end();
                if (ok && std::find(layout.shown.begin(), layout.shown.end(), word) == layout.shown.end())
                    layout.shown.push_back(word);
                word.clear();
            } else {
                word += c;
            }
        }
    }
    layout.display = load_display("smart-lists-display");
    layout.collapsed = load_bool_setting("smart-lists-collapsed");
    return layout;
}

GroupLayout load_lists_layout(const std::string& source) {
    auto display = load_display("my-lists-display");
    if (display == GroupDisplay::Hidden) display = GroupDisplay::Visible;
    return GroupLayout{display, load_bool_setting("lists-collapsed." + source)};
}

GroupLayout load_tags_layout() {
    return GroupLayout{load_display("tags-display"), load_bool_setting("tags-collapsed")};
}

std::string group_title(const SidebarGroup& group, const std::string& lists_title) {
    switch (group.kind) {
        case SidebarGroup::SmartLists: return "Smart Lists";
        case SidebarGroup::Lists: return lists_title;
        case SidebarGroup::Tags: return "Tags";
    }
    return "";
}

void save_group_collapsed(const SidebarGroup& group, bool collapsed) {
    auto key = group.kind == SidebarGroup::SmartLists ? std::string("smart-lists-collapsed")
               : group.kind == SidebarGroup::Tags     ? std::string("tags-collapsed")
                                                      : "lists-collapsed." + group.source;
    save_setting(key, collapsed ? "true" : "false");
}

void save_group_display(const SidebarGroup& group, GroupDisplay display) {
    const char* value = display == GroupDisplay::Collapsible ? "collapsible"
                        : display == GroupDisplay::Hidden    ? "hidden"
                                                             : "visible";
    const char* key = group.kind == SidebarGroup::SmartLists ? "smart-lists-display"
                      : group.kind == SidebarGroup::Tags     ? "tags-display"
                                                             : "my-lists-display";
    save_setting(key, value);
}

std::vector<SidebarGroup> load_sidebar_order(const std::vector<std::string>& sources) {
    // The words, as written; "smart-lists", "smart_lists" and "SmartLists"
    // all count, but a source's name is kept exactly.
    std::vector<std::string> words;
    std::string word;
    for (char c : load_setting("sidebar-order") + ",") {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    auto keyword = [](std::string w) {
        std::string out;
        for (char c : w)
            if (c != '-' && c != '_') out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    std::vector<std::string> named;  // sources with a place of their own
    for (auto& w : words)
        if (keyword(w).starts_with("lists:")) named.push_back(w.substr(w.find(':') + 1));

    std::vector<SidebarGroup> order;
    auto put = [&](const SidebarGroup& g) {
        if (std::ranges::find(order, g) == order.end()) order.push_back(g);
    };
    for (auto& w : words) {
        auto k = keyword(w);
        if (k == "smartlists" || k == "smart") put(SidebarGroup::smart_lists());
        else if (k == "tags") put(SidebarGroup::tags());
        else if (k == "mylists" || k == "lists") {
            for (auto& s : sources)
                if (std::ranges::find(named, s) == named.end()) put(SidebarGroup::lists(s));
        } else if (k.starts_with("lists:")) {
            auto name = w.substr(w.find(':') + 1);
            if (std::ranges::find(sources, name) != sources.end()) put(SidebarGroup::lists(name));
        }
    }
    put(SidebarGroup::smart_lists());
    for (auto& s : sources) put(SidebarGroup::lists(s));
    put(SidebarGroup::tags());
    return order;
}

void save_sidebar_order(const std::vector<SidebarGroup>& order) {
    auto lists_groups = std::ranges::count_if(order, [](auto& g) { return g.kind == SidebarGroup::Lists; });
    std::string value;
    for (auto& g : order) {
        std::string w = g.kind == SidebarGroup::SmartLists ? "smart-lists"
                        : g.kind == SidebarGroup::Tags     ? "tags"
                        : lists_groups == 1                ? "my-lists"
                                                           : "lists:" + g.source;
        value += (value.empty() ? "" : ", ") + w;
    }
    save_setting("sidebar-order", value);
}

bool move_sidebar_group(std::vector<SidebarGroup>& order, const SidebarGroup& group, int delta,
                        const std::vector<SidebarGroup>& showing) {
    auto at = std::ranges::find(order, group);
    if (at == order.end() || delta == 0) return false;
    auto n = static_cast<long>(order.size());
    for (long i = (at - order.begin()) + (delta < 0 ? -1 : 1); i >= 0 && i < n; i += delta < 0 ? -1 : 1) {
        if (std::ranges::find(showing, order[static_cast<std::size_t>(i)]) == showing.end()) continue;
        std::swap(*at, order[static_cast<std::size_t>(i)]);
        return true;
    }
    return false;
}

std::vector<std::string> load_names_setting(const std::string& key) {
    std::vector<std::string> out;
    std::string value = load_setting(key), name;
    bool quoted = false, any = false;
    auto finish = [&] {
        auto t = trimmed(name);
        if (!t.empty() && std::ranges::find(out, t) == out.end()) out.push_back(t);
        name.clear();
        any = false;
    };
    for (char c : value) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (c == ',' && !quoted) {
            finish();
        } else {
            name += c;
        }
    }
    if (any || !name.empty()) finish();
    return out;
}

void save_names_setting(const std::string& key, const std::vector<std::string>& names) {
    std::string value;
    for (auto& n : names) {
        if (!value.empty()) value += ", ";
        bool quote = n.find_first_of(",\"") != std::string::npos || n != trimmed(n);
        if (quote) {
            std::string escaped;
            for (char c : n)
                if (c != '"') escaped += c;  // quotes can't be stored; list names rarely have them
            value += '"' + escaped + '"';
        } else {
            value += n;
        }
    }
    save_setting(key, value);
}

bool list_entry_matches(std::string_view entry, std::string_view key) {
    if (entry == key) return true;
    auto slash = key.find('/');
    return entry.find('/') == std::string_view::npos && slash != std::string_view::npos && key.substr(slash + 1) == entry;
}

bool HiddenEntries::list_hidden(std::string_view key) const {
    return std::ranges::any_of(lists, [&](auto& e) { return list_entry_matches(e, key); });
}
bool HiddenEntries::tag_hidden(std::string_view tag) const { return std::ranges::find(tags, tag) != tags.end(); }

HiddenEntries load_hidden() {
    return HiddenEntries{load_names_setting("lists-hidden"), load_names_setting("tags-hidden"),
                         load_bool_setting("show-hidden")};
}

namespace {

void set_in_names(const std::string& key, const std::string& name, bool present) {
    auto names = load_names_setting(key);
    auto at = std::ranges::find(names, name);
    if (present == (at != names.end())) return;
    if (present) names.push_back(name);
    else names.erase(at);
    save_names_setting(key, names);
}

}  // namespace

void set_list_hidden(const std::string& key, bool hidden) {
    auto names = load_names_setting("lists-hidden");
    if (hidden && std::ranges::find(names, key) != names.end()) return;  // already
    auto before = names;
    std::erase_if(names, [&](auto& e) { return list_entry_matches(e, key); });  // a bare "Work" too
    if (hidden) names.push_back(key);
    if (names != before) save_names_setting("lists-hidden", names);
}
void set_tag_hidden(const std::string& tag, bool hidden) { set_in_names("tags-hidden", tag, hidden); }

void set_smart_list_hidden(const std::string& name, bool hidden) {
    auto shown = load_smart_lists_layout().shown;
    auto at = std::ranges::find(shown, name);
    if (hidden == (at == shown.end())) return;
    if (hidden) shown.erase(at);
    else shown.push_back(name);
    save_smart_lists(shown);
}

void save_smart_lists(const std::vector<std::string>& shown) {
    std::string value;
    for (auto& s : shown) value += (value.empty() ? "" : ", ") + s;
    save_setting("smart-lists", value.empty() ? "none" : value);
}

std::vector<std::string> order_tags(std::vector<std::string> tags) {
    std::ranges::sort(tags);
    std::vector<std::string> out;
    for (auto& t : load_names_setting("tags-order"))
        if (std::ranges::find(tags, t) != tags.end()) out.push_back(t);
    for (auto& t : tags)
        if (std::ranges::find(out, t) == out.end()) out.push_back(t);
    return out;
}

std::vector<std::string> order_lists(const std::vector<std::string>& names) {
    std::vector<std::string> out;
    for (auto& entry : load_names_setting("lists-order"))
        for (auto& n : names)
            if (list_entry_matches(entry, n) && std::ranges::find(out, n) == out.end()) {
                out.push_back(n);
                break;
            }
    for (auto& n : names)
        if (std::ranges::find(out, n) == out.end()) out.push_back(n);
    return out;
}

bool move_in_order(std::vector<std::string>& order, const std::string& name, int delta,
                   const std::vector<std::string>& showing) {
    auto at = std::ranges::find(order, name);
    if (at == order.end() || delta == 0) return false;
    auto n = static_cast<long>(order.size());
    int step = delta < 0 ? -1 : 1;
    for (long i = (at - order.begin()) + step; i >= 0 && i < n; i += step) {
        if (std::ranges::find(showing, order[static_cast<std::size_t>(i)]) == showing.end()) continue;
        std::swap(*at, order[static_cast<std::size_t>(i)]);
        return true;
    }
    return false;
}

void save_show_hidden(bool show) { save_setting("show-hidden", show ? "true" : "false"); }

TagStyle load_tag_style(const std::string& tag) {
    TagStyle style;
    auto color = load_setting("tag-color." + tag), icon = load_setting("tag-icon." + tag);
    if (std::ranges::find(kColors, color) != std::end(kColors)) style.color = color;
    if (std::ranges::find(kIcons, icon) != std::end(kIcons)) style.icon = icon;
    return style;
}

void save_tag_style(const std::string& tag, const TagStyle& style) {
    save_setting("tag-color." + tag, style.color);
    save_setting("tag-icon." + tag, style.icon);
}

std::string device_name() {
    char buf[256] = {};
    gethostname(buf, sizeof buf - 1);
    std::string host;
    for (char c : std::string_view(buf)) {
        auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-') host += static_cast<char>(std::tolower(u));
        if (host.size() == 32) break;
    }
    if (host.empty()) host = "device";

    std::string seed;
    std::getline(std::ifstream("/etc/machine-id"), seed);
    if (seed.empty()) seed = host;
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a
    for (unsigned char c : seed) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return std::format("{}-{:04x}", host, static_cast<unsigned>(h & 0xffff));
}

}  // namespace rem
