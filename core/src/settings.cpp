#include "reminders/settings.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <vector>

#include "reminders/paths.hpp"

namespace rem {

namespace {

std::vector<std::string> read_lines(const fs::path& p) {
	std::vector<std::string> lines;
	std::ifstream in(p);
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		lines.push_back(line);
	}
	return lines;
}

std::string trimmed(std::string_view s) {
	auto b = s.find_first_not_of(" \t");
	if (b == std::string_view::npos) {
		return {};
	}
	auto e = s.find_last_not_of(" \t");
	return std::string(s.substr(b, e - b + 1));
}

// Locates `key` in [section]: the line's index, or where to insert it.
struct Found {
		std::optional<std::size_t> line;
		std::optional<std::size_t>
			section_end;  // insert position if the key is missing
};

Found find_key(const std::vector<std::string>& lines,
			   const std::string& section, const std::string& key) {
	Found f;
	bool in_section = false;
	auto header = "[" + section + "]";
	for (std::size_t i = 0; i < lines.size(); ++i) {
		auto t = trimmed(lines[i]);
		if (t.starts_with('[')) {
			in_section = t == header;
			if (in_section) {
				f.section_end = i + 1;
			}
			continue;
		}
		if (!in_section) {
			continue;
		}
		if (!t.empty() && !t.starts_with('#') && !t.starts_with(';')) {
			f.section_end = i + 1;
		}
		auto eq = t.find('=');
		if (eq != std::string::npos && trimmed(t.substr(0, eq)) == key) {
			f.line = i;
		}
	}
	return f;
}

}  // namespace

fs::path settings_file() { return config_dir() / "settings.ini"; }

std::string load_setting(const std::string& key) {
	return load_section_setting("general", key);
}

void save_setting(const std::string& key, const std::string& value) {
	save_section_setting("general", key, value);
}

std::string load_section_setting(const std::string& section,
								 const std::string& key) {
	auto lines = read_lines(settings_file());
	auto f = find_key(lines, section, key);
	if (!f.line) {
		return {};
	}
	auto& l = lines[*f.line];
	return trimmed(std::string_view(l).substr(l.find('=') + 1));
}

std::vector<std::pair<std::string, std::string>> section_settings(
	const std::string& section) {
	std::vector<std::pair<std::string, std::string>> out;
	bool in = false;
	for (auto& l : read_lines(settings_file())) {
		auto t = trimmed(l);
		if (t.size() > 2 && t.front() == '[' && t.back() == ']') {
			in = t.substr(1, t.size() - 2) == section;
			continue;
		}
		if (!in || t.empty() || t.front() == '#' || t.front() == ';') {
			continue;
		}
		if (auto eq = t.find('='); eq != std::string::npos) {
			out.emplace_back(trimmed(std::string_view(t).substr(0, eq)),
							 trimmed(std::string_view(t).substr(eq + 1)));
		}
	}
	return out;
}

std::vector<std::string> section_names() {
	std::vector<std::string> out;
	for (auto& l : read_lines(settings_file())) {
		auto t = trimmed(l);
		if (t.size() > 2 && t.front() == '[' && t.back() == ']') {
			auto name = t.substr(1, t.size() - 2);
			if (std::ranges::find(out, name) == out.end()) {
				out.push_back(name);
			}
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
		for (auto& l : lines) {
			out << l << '\n';
		}
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
		if (t.starts_with('[')) {
			in_section = t == "[" + section + "]";
		}
		if (!in_section) {
			kept.push_back(l);
		}
	}
	while (kept.size() >= 2 && kept.back().empty() &&
		   kept[kept.size() - 2].empty()) {
		kept.pop_back();
	}
	write_lines(file, kept);
}

void save_section_setting(const std::string& section, const std::string& key,
						  const std::string& value) {
	auto file = settings_file();
	auto lines = read_lines(file);
	auto f = find_key(lines, section, key);
	auto entry = key + "=" + value;
	if (f.line) {
		lines[*f.line] = entry;
	} else if (f.section_end) {
		lines.insert(lines.begin() + static_cast<long>(*f.section_end), entry);
	} else {  // a new section at the end
		if (!lines.empty() && !lines.back().empty()) {
			lines.emplace_back();
		}
		lines.push_back("[" + section + "]");
		lines.push_back(entry);
	}
	write_lines(file, lines);
}

bool load_bool_setting(const std::string& key, bool fallback) {
	std::string v;
	for (char c : load_setting(key)) {
		v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (v == "true" || v == "yes" || v == "1" || v == "on") {
		return true;
	}
	if (v == "false" || v == "no" || v == "0" || v == "off") {
		return false;
	}
	return fallback;
}

std::string device_name() {
	char buf[256] = {};
	gethostname(buf, sizeof buf - 1);
	std::string host;
	for (char c : std::string_view(buf)) {
		auto u = static_cast<unsigned char>(c);
		if (std::isalnum(u) || c == '-') {
			host += static_cast<char>(std::tolower(u));
		}
		if (host.size() == 32) {
			break;
		}
	}
	if (host.empty()) {
		host = "device";
	}

	std::string seed;
	std::getline(std::ifstream("/etc/machine-id"), seed);
	if (seed.empty()) {
		seed = host;
	}
	std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a
	for (unsigned char c : seed) {
		h ^= c;
		h *= 0x100000001b3ULL;
	}
	return std::format("{}-{:04x}", host, static_cast<unsigned>(h & 0xffff));
}

}  // namespace rem
