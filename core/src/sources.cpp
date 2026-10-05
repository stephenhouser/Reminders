#include "reminders/sources.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <format>

#include "reminders/backend_module.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/syncthing.hpp"

namespace rem {

namespace {

constexpr std::string_view kPrefix = "source.";

std::string section_of(const std::string& name) {
	return std::string(kPrefix) + name;
}

SourceConfig read_source(const std::string& name) {
	SourceConfig s;
	s.name = name;
	auto section = section_of(name);
	for (auto& [key, value] : section_settings(section)) {
		if (key == "backend") {
			if (auto* m = find_backend(value)) {
				s.backend = m->id;
			}
		} else if (key == "folder") {
			if (!value.empty()) {
				s.folder = expand_path(value);
			}
		} else if (key == "title") {
			s.title = value;
		} else {
			s.options[key] = value;
		}
	}
	if (s.folder.empty() && backend_of(s).owns_folder) {
		s.folder = default_copy_folder(s.backend, name);
	}
	return s;
}

// A positive whole number of minutes, or `fallback`.
int minutes(const std::string& text, int fallback) {
	int n = 0;
	auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
	return ec == std::errc{} && p == text.data() + text.size() && n > 0
			 ? n
			 : fallback;
}
// A source name from a folder's name: "Reminders" → "reminders".
std::string name_for(const fs::path& folder) {
	std::string out;
	for (char c : folder.filename().string()) {
		auto u = static_cast<unsigned char>(c);
		if (std::isalnum(u)) {
			out += static_cast<char>(std::tolower(u));
		} else if ((c == '-' || c == '_' || c == ' ') && !out.empty() &&
				   out.back() != '-') {
			out += '-';
		}
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	return out.empty() ? "reminders" : out;
}

// Moves a source's per-device records from `from` to `to`, unless `to`
// already has some.
void move_state_dir(const fs::path& from, const fs::path& to) {
	std::error_code ec;
	if (from == to || !fs::is_directory(from, ec) || fs::exists(to, ec)) {
		return;
	}
	fs::create_directories(to.parent_path(), ec);
	fs::rename(from, to, ec);
	if (ec) {  // another file system
		ec.clear();
		fs::copy(from, to, fs::copy_options::recursive, ec);
		if (!ec) {
			fs::remove_all(from, ec);
		}
	}
	if (fs::is_empty(from.parent_path(), ec)) {
		fs::remove(from.parent_path(), ec);
	}
}

// Records in the other place a source's could be: <folder>/.reminders/<device>
// (where every source's were before 2026-10-03, and Syncthing's still are) or
// $XDG_STATE_HOME/reminders/<device>/<name> (where a build of 2026-10-03 put
// Syncthing's too). They move to where they belong.
void move_misplaced_state(const SourceConfig& source, const std::string& device,
						  const fs::path& state) {
	auto in_folder = source.folder / kStateDirName / device;
	if (backend_of(source).state_in_folder) {
		if (!source.name.empty()) {
			move_state_dir(state_dir() / device / source.name, in_folder);
		}
	} else {
		move_state_dir(in_folder, state);
	}
}

}  // namespace

fs::path default_copy_folder(std::string_view backend,
							 const std::string& name) {
	return data_dir() / std::string(backend) / name;
}

std::string SourceConfig::option(const std::string& key) const {
	auto at = options.find(key);
	return at == options.end() ? std::string() : at->second;
}

DavSettings dav_settings(const SourceConfig& source) {
	return {source.option("url"), source.option("username"),
			source.option("password-command"), sync_interval(source)};
}

void set_dav_settings(SourceConfig& source, const DavSettings& settings) {
	source.options["url"] = settings.url;
	source.options["username"] = settings.username;
	source.options["password-command"] = settings.password_command;
	source.options["interval"] = std::to_string(settings.interval);
}

GitSettings git_settings(const SourceConfig& source) {
	return {source.option("url"), source.option("remote"),
			source.option("branch"), sync_interval(source)};
}

void set_git_settings(SourceConfig& source, const GitSettings& settings) {
	source.options["url"] = settings.url;
	source.options["remote"] = settings.remote;
	source.options["branch"] = settings.branch;
	source.options["interval"] = std::to_string(settings.interval);
}
fs::path source_state_dir(const SourceConfig& source,
						  const std::string& device) {
	if (backend_of(source).state_in_folder) {
		return state_dir(source.folder, device);
	}
	auto base = state_dir() / device;
	if (!source.name.empty()) {
		return base / source.name;
	}
	std::error_code ec;
	auto folder = fs::weakly_canonical(source.folder, ec).string();
	std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a
	for (unsigned char c : folder) {
		h ^= c;
		h *= 0x100000001b3ULL;
	}
	return base / std::format("folder-{:016x}", h);
}

std::vector<SourceConfig> load_sources() {
	std::vector<SourceConfig> out;
	for (auto& section : section_names()) {
		if (section.starts_with(kPrefix) && section.size() > kPrefix.size()) {
			auto s = read_source(section.substr(kPrefix.size()));
			if (!s.folder.empty()) {
				out.push_back(std::move(s));
			}
		}
	}
	return out;
}

std::optional<SourceConfig> default_source() {
	auto sources = load_sources();
	if (sources.empty()) {
		return std::nullopt;
	}
	auto name = load_setting("default-source");
	for (auto& s : sources) {
		if (s.name == name) {
			return s;
		}
	}
	return sources.front();
}

std::string source_title(const SourceConfig& source) {
	if (!source.title.empty()) {
		return source.title;
	}
	auto t =
		source.name.empty() ? source.folder.filename().string() : source.name;
	if (!t.empty()) {
		t[0] =
			static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
	}
	return t;
}

std::optional<fs::path> saved_folder() {
	auto source = default_source();
	std::error_code ec;
	if (!source || !fs::is_directory(source->folder, ec)) {
		return std::nullopt;
	}
	return source->folder;
}

void save_source(const SourceConfig& source) {
	// Records kept for another folder (or server) don't apply any more.
	auto section = section_of(source.name);
	auto old_folder = load_section_setting(section, "folder");
	auto old_url = load_section_setting(section, "url");
	if ((!old_folder.empty() && expand_path(old_folder) != source.folder) ||
		(!old_url.empty() && old_url != source.option("url"))) {
		std::error_code ec;
		fs::remove_all(source_state_dir(source, device_name()), ec);
	}
	save_section_setting(section, "backend", source.backend);
	save_section_setting(section, "folder", contract_path(source.folder));
	if (!source.title.empty()) {
		save_section_setting(section, "title", source.title);
	}
	for (auto& [key, value] : source.options) {
		save_section_setting(section, key, value);
	}
}
int sync_interval(const SourceConfig& source) {
	return minutes(source.option("interval"), 15);
}
bool in_git_repo(const fs::path& folder) {
	std::error_code ec;
	for (auto p = fs::weakly_canonical(folder, ec); !p.empty();
		 p = p.parent_path()) {
		if (fs::exists(p / ".git", ec)) {
			return true;
		}
		if (p == p.parent_path()) {
			break;
		}
	}
	return false;
}

std::string detect_backend(const fs::path& folder) {
	for (auto* m : backends()) {
		if (m->detect_by_default && m->detect && m->detect(folder)) {
			return m->id;
		}
	}
	return "local";
}
SourceConfig source_for_folder(const fs::path& folder) {
	std::error_code ec;
	for (auto& s : load_sources()) {
		if (s.folder == folder || fs::equivalent(s.folder, folder, ec)) {
			return s;
		}
	}
	return SourceConfig{"", detect_backend(folder), folder, ""};
}

SourceConfig set_default_folder(const fs::path& folder) {
	// A server account stays one: the folder becomes a source of its own.
	if (auto d = default_source(); d && has_server(*d)) {
		auto source = source_for_folder(folder);
		if (source.name.empty()) {
			source = add_source(folder);
		}
		save_setting("default-source", source.name);
		return source;
	}
	auto source = default_source().value_or(
		SourceConfig{name_for(folder), "syncthing", folder, ""});
	source.folder = folder;
	source.backend = detect_backend(folder);
	save_source(source);
	if (load_setting("default-source").empty()) {
		save_setting("default-source", source.name);
	}
	return source;
}

namespace {

std::string unique_source_name(const std::string& base,
							   const std::vector<SourceConfig>& existing) {
	auto name = base;
	for (int n = 2;
		 std::ranges::any_of(existing, [&](auto& s) { return s.name == name; });
		 ++n) {
		name = std::format("{}-{}", base, n);
	}
	return name;
}

}  // namespace

std::string new_source_name(const SourceConfig& source) {
	std::string base = source.title;
	auto& module = backend_of(source);
	if (base.empty() && module.name_hint) {
		base = module.name_hint(source);
	}
	if (base.empty() && !source.folder.empty()) {
		base = source.folder.filename().string();
	}
	if (base.empty()) {
		base = module.has_server ? module.id : "reminders";
	}
	return unique_source_name(name_for(fs::path(base)), load_sources());
}

SourceConfig add_source(SourceConfig source) {
	bool first = load_sources().empty();
	if (source.name.empty()) {
		source.name = new_source_name(source);
	}
	if (source.folder.empty() && backend_of(source).owns_folder) {
		source.folder = default_copy_folder(source.backend, source.name);
	}
	save_source(source);
	if (first) {
		save_setting("default-source", source.name);
	}
	return source;
}

SourceConfig add_source(const fs::path& folder) {
	return add_source(SourceConfig{"", detect_backend(folder), folder, ""});
}

void remove_source(const std::string& name, bool keep_copy) {
	std::error_code ec;
	for (auto& s : load_sources()) {
		if (s.name == name) {
			fs::remove_all(source_state_dir(s, device_name()), ec);
			if (!keep_copy && has_server(s) &&
				s.folder == default_copy_folder(s.backend, name)) {
				fs::remove_all(s.folder, ec);
			}
		}
	}
	remove_section(section_of(name));
	if (load_setting("default-source") == name) {
		auto rest = load_sources();
		save_setting("default-source", rest.empty() ? "" : rest.front().name);
	}
}

std::unique_ptr<Store> open_source(const SourceConfig& source,
								   const std::string& device) {
	// A local copy, or a git folder (a clone or a new repository to be):
	// made now, filled (or made a repository) by the first sync.
	if (backend_of(source).owns_folder) {
		fs::create_directories(source.folder);
	}
	auto state = source_state_dir(source, device);
	move_misplaced_state(source, device, state);
	auto store = std::make_unique<Store>(source.folder, state, source.backend);
	store->prepare();
	return store;
}

}  // namespace rem
