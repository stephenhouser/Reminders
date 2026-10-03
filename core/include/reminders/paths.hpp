// Where the app keeps its files (the XDG base directories), and paths as
// people write them in settings.ini.
//
//   config  $XDG_CONFIG_HOME/reminders  (~/.config/reminders)       settings.ini
//   data    $XDG_DATA_HOME/reminders    (~/.local/share/reminders)  CalDAV and WebDAV sources' local copies
//   state   $XDG_STATE_HOME/reminders   (~/.local/state/reminders)  per-device records: merge bases, sync records
//   cache   $XDG_CACHE_HOME/reminders   (~/.cache/reminders)        what can be found again (CalDAV calendar homes)
//
// An XDG variable that is unset, empty or not an absolute path is ignored,
// as the specification says.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rem {

namespace fs = std::filesystem;

fs::path home_dir();
fs::path config_dir();
fs::path data_dir();
fs::path state_dir();
fs::path cache_dir();

// A path from settings.ini or the command line: a leading "~" or "~/" is
// the home folder; "$VAR" and "${VAR}" are environment variables ($HOME,
// $XDG_DATA_HOME, …; unset ones are empty); what's still relative after
// that is taken from the home folder.
fs::path expand_path(std::string_view text);
// The other way, for writing settings.ini: "~/…" for a path in the home
// folder (so the file works for another user or a different home), else as is.
std::string contract_path(const fs::path& path);

}  // namespace rem
