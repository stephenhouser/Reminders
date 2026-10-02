#include "reminders/settings.hpp"

#include <unistd.h>

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
