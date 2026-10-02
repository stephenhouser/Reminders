#include "reminders/settings.hpp"

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

// Locates `key` in [general]: the line's index, or where to insert it.
struct Found {
    std::optional<std::size_t> line;
    std::optional<std::size_t> section_end;  // insert position if the key is missing
};

Found find_key(const std::vector<std::string>& lines, const std::string& key) {
    Found f;
    bool in_general = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto t = trimmed(lines[i]);
        if (t.starts_with('[')) {
            if (in_general) f.section_end = i;
            in_general = t == "[general]";
            if (in_general) f.section_end = i + 1;
            continue;
        }
        if (!in_general) continue;
        f.section_end = i + 1;
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

std::string load_setting(const std::string& key) {
    auto lines = read_lines(settings_file());
    auto f = find_key(lines, key);
    if (!f.line) return {};
    auto& l = lines[*f.line];
    return trimmed(std::string_view(l).substr(l.find('=') + 1));
}

void save_setting(const std::string& key, const std::string& value) {
    auto file = settings_file();
    auto lines = read_lines(file);
    auto f = find_key(lines, key);
    auto entry = key + "=" + value;
    if (f.line) {
        lines[*f.line] = entry;
    } else if (f.section_end) {
        lines.insert(lines.begin() + static_cast<long>(*f.section_end), entry);
    } else {
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.emplace_back("[general]");
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
    static const std::vector<std::string> known = {"today", "scheduled", "all", "flagged", "completed"};
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

GroupLayout load_my_lists_layout() {
    auto display = load_display("my-lists-display");
    if (display == GroupDisplay::Hidden) display = GroupDisplay::Visible;
    return GroupLayout{display, load_bool_setting("my-lists-collapsed")};
}

GroupLayout load_tags_layout() {
    return GroupLayout{load_display("tags-display"), load_bool_setting("tags-collapsed")};
}

namespace {

constexpr SidebarGroup kGroups[] = {SidebarGroup::SmartLists, SidebarGroup::MyLists, SidebarGroup::Tags};

const char* group_key(SidebarGroup group) {
    switch (group) {
        case SidebarGroup::SmartLists: return "smart-lists";
        case SidebarGroup::MyLists: return "my-lists";
        case SidebarGroup::Tags: return "tags";
    }
    return "";
}

}  // namespace

const char* group_title(SidebarGroup group) {
    switch (group) {
        case SidebarGroup::SmartLists: return "Smart Lists";
        case SidebarGroup::MyLists: return "My Lists";
        case SidebarGroup::Tags: return "Tags";
    }
    return "";
}

void save_group_collapsed(SidebarGroup group, bool collapsed) {
    save_setting(std::string(group_key(group)) + "-collapsed", collapsed ? "true" : "false");
}

void save_group_display(SidebarGroup group, GroupDisplay display) {
    const char* value = display == GroupDisplay::Collapsible ? "collapsible"
                        : display == GroupDisplay::Hidden    ? "hidden"
                                                             : "visible";
    save_setting(std::string(group_key(group)) + "-display", value);
}

std::vector<SidebarGroup> load_sidebar_order() {
    std::vector<SidebarGroup> order;
    std::string word;
    for (char c : load_setting("sidebar-order") + ",") {
        if (c != ',' && c != ' ' && c != '\t') {
            if (c != '-' && c != '_') word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            continue;
        }
        std::optional<SidebarGroup> g;
        if (word == "smartlists" || word == "smart") g = SidebarGroup::SmartLists;
        else if (word == "mylists" || word == "lists") g = SidebarGroup::MyLists;
        else if (word == "tags") g = SidebarGroup::Tags;
        if (g && std::ranges::find(order, *g) == order.end()) order.push_back(*g);
        word.clear();
    }
    for (auto g : kGroups)
        if (std::ranges::find(order, g) == order.end()) order.push_back(g);
    return order;
}

void save_sidebar_order(const std::vector<SidebarGroup>& order) {
    std::string value;
    for (auto g : order) value += (value.empty() ? "" : ", ") + std::string(group_key(g));
    save_setting("sidebar-order", value);
}

bool move_sidebar_group(std::vector<SidebarGroup>& order, SidebarGroup group, int delta,
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

std::optional<fs::path> saved_folder() {
    auto folder = load_setting("folder");
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return std::nullopt;
    return fs::path(folder);
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
