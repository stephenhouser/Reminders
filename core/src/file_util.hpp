// Small file helpers shared by the back ends (internal to the core library).
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rem::detail {

namespace fs = std::filesystem;

inline std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes through a temporary file, so readers see the old text or the new.
inline void write_atomic(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    auto tmp = p.parent_path() / ("." + p.filename().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) throw fs::filesystem_error("write failed", tmp, std::make_error_code(std::errc::io_error));
    }
    fs::rename(tmp, p);
}

inline std::vector<std::string> split(std::string_view line, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i)
        if (i == line.size() || line[i] == sep) {
            out.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

// A value for one field of a .tsv record line.
inline std::string field(std::string s) {
    std::ranges::replace(s, '\t', ' ');
    std::ranges::replace(s, '\n', ' ');
    std::ranges::replace(s, '\r', ' ');
    return s;
}

// The file's non-empty lines.
inline std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) out.push_back(line);
    return out;
}

// 64-bit FNV-1a, as 16 hex digits.
inline std::string fingerprint(std::string_view text) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return std::format("{:016x}", h);
}

}  // namespace rem::detail
