// Keeping the app's per-device state out of Syncthing.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace rem {

namespace fs = std::filesystem;

// The folder's own per-device state lives in <folder>/.reminders/<device>/.
inline constexpr const char* kStateDirName = ".reminders";

fs::path state_dir(const fs::path& folder, const std::string& device);

// The root of the Syncthing folder containing `dir`: the nearest ancestor
// (or `dir` itself) holding Syncthing's ".stfolder" marker.
std::optional<fs::path> syncthing_root(const fs::path& dir);

// Adds "(?d).reminders" to the Syncthing root's .stignore (creating it if need
// be) so per-device state isn't synced. Does nothing if `folder` isn't in a
// Syncthing folder or the pattern is already there. Returns true if it
// changed .stignore.
bool ignore_state_in_syncthing(const fs::path& folder);

}  // namespace rem
