// The Syncthing back end: a folder Syncthing keeps in step with other
// devices. The app merges the conflict copies Syncthing makes when a list
// changed on two devices, three-way, using a per-device record of the last
// version that came from elsewhere (the merge base) and of the last version
// this device wrote, kept in <folder>/.reminders/<device>/, which .stignore
// keeps out of Syncthing. See README.md.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace rem {

namespace fs = std::filesystem;

// The root of the Syncthing folder containing `dir`: the nearest ancestor
// (or `dir` itself) holding Syncthing's ".stfolder" marker.
std::optional<fs::path> syncthing_root(const fs::path& dir);

// Adds "(?d).reminders" to the Syncthing root's .stignore (creating it if need
// be) so per-device state isn't synced. Does nothing if `folder` isn't in a
// Syncthing folder or the pattern is already there. Returns true if it
// changed .stignore.
bool ignore_state_in_syncthing(const fs::path& folder);

// Adds the back end to the registry (backend_module.hpp).
void register_syncthing_backend();

}  // namespace rem
