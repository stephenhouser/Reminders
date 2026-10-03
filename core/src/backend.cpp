#include "reminders/backend.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>

#include "reminders/caldav.hpp"
#include "reminders/syncthing.hpp"

namespace rem {

namespace {

constexpr std::string_view kConflictMarker = ".sync-conflict-";

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_atomic(const fs::path& p, const std::string& text) {
    auto tmp = p.parent_path() / ("." + p.filename().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) throw fs::filesystem_error("write failed", tmp, std::make_error_code(std::errc::io_error));
    }
    fs::rename(tmp, p);
}

// 64-bit FNV-1a, as 16 hex digits: enough to recognise our own last write
// without keeping a copy of it.
std::string fingerprint(std::string_view text) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return std::format("{:016x}", h);
}

class LocalBackend : public Backend {
public:
    BackendKind kind() const override { return BackendKind::Local; }
};

class SyncthingBackend : public Backend {
public:
    explicit SyncthingBackend(fs::path state_dir) : state_dir_(std::move(state_dir)) {}
    BackendKind kind() const override { return BackendKind::Syncthing; }

    void prepare(const fs::path& folder) override { ignore_state_in_syncthing(folder); }

    std::optional<std::string> list_name_for(const fs::path& file) const override {
        auto name = Backend::list_name_for(file);
        if (!name) return name;
        if (auto at = name->find(kConflictMarker); at != std::string::npos) name->resize(at);
        if (name->empty()) return std::nullopt;
        return name;
    }

    std::vector<fs::path> conflict_copies(const fs::path& folder, std::string_view name) const override {
        std::vector<fs::path> out;
        std::error_code ec;
        auto prefix = std::string(name) + std::string(kConflictMarker);
        for (auto& e : fs::directory_iterator(folder, ec)) {
            auto fname = e.path().filename().string();
            if (fname.starts_with(prefix) && fname.ends_with(".md")) out.push_back(e.path());
        }
        std::ranges::sort(out);
        return out;
    }

    // Per list, in the per-device state folder:
    //   base/<list>.md  the last version that came from another device (merge base)
    //   written/<list>  a fingerprint of the last version this device wrote
    std::optional<std::string> read_base(std::string_view name) const override { return read_file(base_path(name)); }

    void write_base(std::string_view name, const std::string& text) const override {
        fs::create_directories(base_path(name).parent_path());
        write_atomic(base_path(name), text);
    }

    void remember_written(std::string_view name, const std::string& text) const override {
        fs::create_directories(written_path(name).parent_path());
        write_atomic(written_path(name), fingerprint(text));
    }

    bool is_own_write(std::string_view name, const std::string& text) const override {
        return read_file(written_path(name)) == fingerprint(text);
    }

    void move_state(std::string_view from, std::string_view to) const override {
        std::error_code ec;
        for (auto [a, b] : {std::pair{base_path(from), base_path(to)}, std::pair{written_path(from), written_path(to)}})
            if (fs::exists(a, ec)) fs::rename(a, b, ec);
    }

    void drop_state(std::string_view name) const override {
        std::error_code ec;
        fs::remove(base_path(name), ec);
        fs::remove(written_path(name), ec);
    }

private:
    fs::path state_dir_;
    fs::path base_path(std::string_view name) const { return state_dir_ / "base" / (std::string(name) + ".md"); }
    fs::path written_path(std::string_view name) const { return state_dir_ / "written" / std::string(name); }
};

}  // namespace

std::string_view backend_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::Local: return "local";
    case BackendKind::Caldav: return "caldav";
    case BackendKind::Syncthing: break;
    }
    return "syncthing";
}

std::optional<BackendKind> parse_backend(std::string_view name) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n == "syncthing") return BackendKind::Syncthing;
    if (n == "local") return BackendKind::Local;
    if (n == "caldav") return BackendKind::Caldav;
    return std::nullopt;
}

std::optional<std::string> Backend::list_name_for(const fs::path& file) const {
    auto fname = file.filename().string();
    if (fname.empty() || fname[0] == '.' || !fname.ends_with(".md")) return std::nullopt;
    auto stem = fname.substr(0, fname.size() - 3);
    if (stem.empty()) return std::nullopt;
    return stem;
}

std::vector<fs::path> Backend::conflict_copies(const fs::path&, std::string_view) const { return {}; }
std::optional<std::string> Backend::read_base(std::string_view) const { return std::nullopt; }
void Backend::write_base(std::string_view, const std::string&) const {}
void Backend::remember_written(std::string_view, const std::string&) const {}
bool Backend::is_own_write(std::string_view, const std::string&) const { return false; }
void Backend::move_state(std::string_view, std::string_view) const {}
void Backend::drop_state(std::string_view) const {}

std::unique_ptr<Backend> make_backend(BackendKind kind, fs::path state_dir) {
    if (kind == BackendKind::Local) return std::make_unique<LocalBackend>();
    if (kind == BackendKind::Caldav) return std::make_unique<CaldavBackend>(std::move(state_dir));
    return std::make_unique<SyncthingBackend>(std::move(state_dir));
}

}  // namespace rem
