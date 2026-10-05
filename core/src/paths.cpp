#include "reminders/paths.hpp"

#include <pwd.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>

namespace rem {

namespace {

fs::path xdg(const char* var, const fs::path& fallback) {
	if (const char* v = std::getenv(var);
		v && *v && fs::path(v).is_absolute()) {
		return fs::path(v) / "reminders";
	}
	return home_dir() / fallback / "reminders";
}

}  // namespace

fs::path home_dir() {
	if (const char* h = std::getenv("HOME"); h && *h) {
		return h;
	}
	if (auto* pw = getpwuid(getuid()); pw && pw->pw_dir) {
		return pw->pw_dir;
	}
	return fs::current_path();
}

fs::path config_dir() { return xdg("XDG_CONFIG_HOME", ".config"); }
fs::path data_dir() { return xdg("XDG_DATA_HOME", ".local/share"); }
fs::path state_dir() { return xdg("XDG_STATE_HOME", ".local/state"); }
fs::path cache_dir() { return xdg("XDG_CACHE_HOME", ".cache"); }

fs::path expand_path(std::string_view text) {
	std::string out;
	std::size_t i = 0;
	if (text == "~" || text.starts_with("~/")) {
		out = home_dir().string();
		i = 1;
	}
	auto name_char = [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	};
	while (i < text.size()) {
		if (text[i] != '$' || i + 1 >= text.size()) {
			out += text[i++];
			continue;
		}
		std::string name;
		if (text[i + 1] == '{') {
			auto close = text.find('}', i + 2);
			if (close ==
				std::string_view::npos) {  // "${" without an end: as written
				out += text[i++];
				continue;
			}
			name = text.substr(i + 2, close - i - 2);
			i = close + 1;
		} else {
			auto j = i + 1;
			while (j < text.size() && name_char(text[j])) {
				++j;
			}
			if (j == i + 1) {  // a lone "$"
				out += text[i++];
				continue;
			}
			name = text.substr(i + 1, j - i - 1);
			i = j;
		}
		if (name == "HOME") {
			out += home_dir().string();
		} else if (const char* v = std::getenv(name.c_str())) {
			out += v;
		}
	}
	if (out.empty()) {
		return {};
	}
	fs::path p(out);
	if (p.is_relative()) {
		p = home_dir() / p;
	}
	return p.lexically_normal();
}

std::string contract_path(const fs::path& path) {
	auto home = home_dir().lexically_normal();
	auto p = path.lexically_normal();
	auto rel = p.lexically_relative(home);
	if (!p.is_absolute() || rel.empty() || *rel.begin() == "..") {
		return p.string();
	}
	if (rel == ".") {
		return "~";
	}
	return "~/" + rel.string();
}

fs::path state_dir(const fs::path& folder, const std::string& device) {
	return folder / kStateDirName / device;
}

}  // namespace rem
