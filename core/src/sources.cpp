#include "reminders/sources.hpp"

#include <algorithm>
#include <cctype>

#include "reminders/settings.hpp"
#include "reminders/syncthing.hpp"

namespace rem {

namespace {

constexpr std::string_view kPrefix = "source.";

std::string section_of(const std::string& name) { return std::string(kPrefix) + name; }

SourceConfig read_source(const std::string& name) {
    SourceConfig s;
    s.name = name;
    s.backend = parse_backend(load_section_setting(section_of(name), "backend")).value_or(BackendKind::Syncthing);
    s.folder = load_section_setting(section_of(name), "folder");
    return s;
}

// A source name from a folder's name: "Reminders" → "reminders".
std::string name_for(const fs::path& folder) {
    std::string out;
    for (char c : folder.filename().string()) {
        auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out += static_cast<char>(std::tolower(u));
        else if ((c == '-' || c == '_' || c == ' ') && !out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "reminders" : out;
}

}  // namespace

std::vector<SourceConfig> load_sources() {
    std::vector<SourceConfig> out;
    for (auto& section : section_names())
        if (section.starts_with(kPrefix) && section.size() > kPrefix.size()) {
            auto s = read_source(section.substr(kPrefix.size()));
            if (!s.folder.empty()) out.push_back(std::move(s));
        }
    return out;
}

std::optional<SourceConfig> default_source() {
    auto sources = load_sources();
    if (sources.empty()) return std::nullopt;
    auto name = load_setting("default-source");
    for (auto& s : sources)
        if (s.name == name) return s;
    return sources.front();
}

std::optional<fs::path> saved_folder() {
    auto source = default_source();
    std::error_code ec;
    if (!source || !fs::is_directory(source->folder, ec)) return std::nullopt;
    return source->folder;
}

void save_source(const SourceConfig& source) {
    save_section_setting(section_of(source.name), "backend", std::string(backend_name(source.backend)));
    save_section_setting(section_of(source.name), "folder", source.folder.string());
}

BackendKind detect_backend(const fs::path& folder) {
    return syncthing_root(folder) ? BackendKind::Syncthing : BackendKind::Local;
}

SourceConfig source_for_folder(const fs::path& folder) {
    std::error_code ec;
    for (auto& s : load_sources())
        if (s.folder == folder || fs::equivalent(s.folder, folder, ec)) return s;
    return SourceConfig{"", detect_backend(folder), folder};
}

SourceConfig set_default_folder(const fs::path& folder) {
    auto source = default_source().value_or(SourceConfig{name_for(folder), BackendKind::Syncthing, folder});
    source.folder = folder;
    source.backend = detect_backend(folder);
    save_source(source);
    if (load_setting("default-source").empty()) save_setting("default-source", source.name);
    return source;
}

std::unique_ptr<Store> open_source(const SourceConfig& source, const std::string& device) {
    auto store = std::make_unique<Store>(source.folder, state_dir(source.folder, device), source.backend);
    store->prepare();
    return store;
}

}  // namespace rem
