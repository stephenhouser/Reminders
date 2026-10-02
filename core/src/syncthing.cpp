#include "reminders/syncthing.hpp"

#include <fstream>
#include <sstream>

namespace rem {

fs::path state_dir(const fs::path& folder, const std::string& device) {
    return folder / kStateDirName / device;
}

std::optional<fs::path> syncthing_root(const fs::path& dir) {
    std::error_code ec;
    for (auto p = fs::weakly_canonical(dir, ec); !p.empty(); p = p.parent_path()) {
        if (fs::is_directory(p / ".stfolder", ec)) return p;
        if (p == p.parent_path()) break;
    }
    return std::nullopt;
}

bool ignore_state_in_syncthing(const fs::path& folder) {
    auto root = syncthing_root(folder);
    if (!root) return false;
    auto file = *root / ".stignore";

    std::string existing;
    if (std::ifstream in(file); in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        existing = ss.str();
        std::istringstream lines(existing);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == kStateDirName || line == std::string("(?d)") + kStateDirName)
                return false;  // already ignored
        }
    }

    std::ofstream out(file, std::ios::app);
    if (!existing.empty() && existing.back() != '\n') out << '\n';
    // Unanchored, so it matches wherever the Reminders folder sits inside
    // the Syncthing folder. "(?d)" lets Syncthing delete it when it would
    // otherwise stop the Reminders folder itself from being deleted.
    out << "// Reminders app: per-device state, not to be synced\n(?d)" << kStateDirName << '\n';
    return static_cast<bool>(out);
}

}  // namespace rem
