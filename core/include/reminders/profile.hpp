// Profiles: separate sets of settings, each with its own sources, sidebar,
// saved view and per-device records, so that several can be open at once
// (a window each in the GNOME app) without sharing anything.
//
//   default  $XDG_CONFIG_HOME/reminders/settings.ini, and the records under
//            $XDG_STATE_HOME/reminders/, $XDG_DATA_HOME/reminders/ and
//            $XDG_CACHE_HOME/reminders/, as before profiles existed
//   NAME     $XDG_CONFIG_HOME/reminders/profiles/NAME.ini, and the records
//            under $XDG_STATE_HOME/reminders/profiles/NAME/ and the same
//            under XDG_DATA_HOME and XDG_CACHE_HOME
//
// Anything kept per profile is found through its Profile, never through
// paths.hpp's config_dir() and the rest, which are every profile's root.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rem {

namespace fs = std::filesystem;

class Profile {
	public:
		// The default profile.
		Profile() = default;
		// A profile by name; "" or "default" is the default profile. Throws
		// std::invalid_argument for a name valid_profile_name() refuses.
		explicit Profile(std::string_view name);

		// "default" for the default profile.
		std::string name() const;
		bool is_default() const { return name_.empty(); }
		// The default profile always; a named one when its file is there.
		bool exists() const;

		fs::path settings_file() const;
		fs::path data_dir() const;
		fs::path state_dir() const;
		fs::path cache_dir() const;

		bool operator==(const Profile&) const = default;

	private:
		std::string name_;	// empty: the default profile
};

// A name a profile can have: 1–64 letters, digits, '-' and '_', not
// starting with '-', and not "default" (the default profile's).
bool valid_profile_name(std::string_view name);

// The named profiles, from the .ini files in $XDG_CONFIG_HOME/reminders/
// profiles/, sorted; not the default profile.
std::vector<std::string> profile_names();

// A profile by name, which must exist: "default", or a named one. Throws
// std::runtime_error saying which profiles there are.
Profile existing_profile(std::string_view name);

// Makes a named profile: its settings file, with just a [general] line, or a
// copy of `from`'s. Throws std::invalid_argument for a bad name and
// std::runtime_error for one that's taken.
Profile create_profile(std::string_view name,
					   const std::optional<Profile>& from = std::nullopt);

// The profile an app opens when none is named (on its command line or in
// REMINDERS_PROFILE), from profile-on-start= in the default profile's
// settings:
//   default (or unset)  the default profile
//   last                the one an app last opened (last-profile=)
//   ask                 ask which, with `profile` (the last one) as the
//                       answer where nobody can be asked
//   NAME                that profile
// A profile that's gone is the default; there's no asking with only the
// default profile.
struct StartProfile {
		Profile profile;
		bool ask = false;
};
StartProfile profile_on_start();
// Remembers the profile an app opened (last-profile=, in the default
// profile's settings), for profile-on-start=last and ask.
void remember_profile(const Profile& profile);

}  // namespace rem
