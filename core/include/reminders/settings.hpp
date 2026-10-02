// Settings shared by every front end on a device (GUI, CLI, TUI), and the
// device's name for its per-device state folder.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace rem {

namespace fs = std::filesystem;

// $XDG_CONFIG_HOME/reminders/settings.ini, or ~/.config/reminders/settings.ini.
fs::path settings_file();

// A value from the [general] section, or "" if unset.
std::string load_setting(const std::string& key);
// Sets a value in [general], keeping every other line of the file as it was.
void save_setting(const std::string& key, const std::string& value);

// The folder chosen in the app ("folder" setting), if it is set and exists.
std::optional<fs::path> saved_folder();

// This device's name for <folder>/.reminders/<device>/: the host name plus 4
// hex digits from the machine id (so two machines both called "fedora" differ).
std::string device_name();

}  // namespace rem
