// A profile's settings file, shared by every front end on a device (GUI,
// CLI, TUI): reading and writing its keys and sections, keeping every other
// line as it was; and the device's name for its per-device state folders.
// Each profile has a file of its own (profile.hpp). A parsed file is kept in
// memory and read again when it changes on disk (a stat() per call). What the
// apps keep in it about the sidebar is in app/ preferences.hpp; sources'
// sections are read by sources.hpp.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/profile.hpp"

namespace rem {

namespace fs = std::filesystem;

// A value from the [general] section of the profile's file, or "" if unset.
std::string load_setting(const Profile& profile, const std::string& key);
// Sets a value in [general], keeping every other line of the file as it was.
void save_setting(const Profile& profile, const std::string& key,
				  const std::string& value);

// The same in another section ("source.personal" is [source.personal]); a
// missing section is added at the end of the file.
std::string load_section_setting(const Profile& profile,
								 const std::string& section,
								 const std::string& key);
void save_section_setting(const Profile& profile, const std::string& section,
						  const std::string& key, const std::string& value);
// Every [section] in the file, in order ("general", "source.personal", …).
std::vector<std::string> section_names(const Profile& profile);
// Every key=value in a section, in file order (comments left out).
std::vector<std::pair<std::string, std::string>> section_settings(
	const Profile& profile, const std::string& section);
// Removes a section and everything in it.
void remove_section(const Profile& profile, const std::string& section);

// A true/false setting ("true", "yes", "1" / "false", "no", "0"), or
// `fallback` if it's unset or unreadable.
bool load_bool_setting(const Profile& profile, const std::string& key,
					   bool fallback = false);

// This device's name for its state folders (<folder>/.reminders/<device>/ for
// Syncthing, $XDG_STATE_HOME/reminders/<device>/ otherwise; different per
// computer even with a shared home folder): the host name plus 4
// hex digits from the machine id (so two machines both called "fedora" differ).
std::string device_name();

}  // namespace rem
