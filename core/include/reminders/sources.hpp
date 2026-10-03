// Sources: where lists come from. Each is a back end and its settings, in a
// [source.NAME] section of settings.ini:
//
//   [general]
//   default-source=personal
//
//   [source.personal]
//   backend=syncthing        (or local or caldav; see backend.hpp)
//   folder=/home/you/Sync/Reminders
//
//   [source.fastmail]
//   backend=caldav
//   url=https://caldav.fastmail.com/
//   username=you@fastmail.com
//   password-command=secret-tool lookup service reminders-caldav
//   interval=15              (minutes between syncs)
//
// A CalDAV source's folder is its local copy, by default
// ~/.local/share/reminders/caldav/NAME/.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "reminders/backend.hpp"
#include "reminders/store.hpp"

namespace rem {

namespace fs = std::filesystem;

// A CalDAV source's server settings.
struct CaldavSettings {
    std::string url;               // the server, or its calendar home
    std::string username;
    std::string password_command;  // run by the shell; the first line it prints is the password
    int interval = 15;             // minutes between syncs

    bool operator==(const CaldavSettings&) const = default;
};

struct SourceConfig {
    std::string name;  // [source.NAME]; empty for a folder used only this session
    BackendKind backend = BackendKind::Syncthing;
    fs::path folder;
    std::string title;  // title=, shown on its sidebar group; empty: from the name
    CaldavSettings caldav = {};  // backend=caldav only
};

// Where a CalDAV source keeps its lists unless folder= says otherwise:
// $XDG_DATA_HOME/reminders/caldav/NAME (~/.local/share/…).
fs::path default_caldav_folder(const std::string& name);

// The source's title: title=, else its name capitalised ("personal" →
// "Personal"), else the folder's name.
std::string source_title(const SourceConfig& source);

// Every [source.NAME] with a folder (CalDAV ones: with a url), in file order. A missing or unknown
// backend= is syncthing.
std::vector<SourceConfig> load_sources();
std::optional<SourceConfig> default_source();
void save_source(const SourceConfig& source);

// The default source's folder, if there is one and it exists.
std::optional<fs::path> saved_folder();

// The back end a folder needs when nothing says otherwise: syncthing inside a
// Syncthing folder (one with .stfolder at or above it), else local.
BackendKind detect_backend(const fs::path& folder);

// The source for a folder (from the command line, say): the configured one
// with that folder, else an unnamed one with the detected back end.
SourceConfig source_for_folder(const fs::path& folder);

// Points the default source at `folder` (Change Folder…, `reminders folder
// PATH`), with the back end it needs; creates the source, named after the
// folder, if there is none. If the default is a CalDAV account, the folder
// becomes a source of its own (the default) instead. Returns it.
SourceConfig set_default_folder(const fs::path& folder);

// Adds a source for `folder` (Add Source…): named after it (made unique),
// with the back end it needs. It becomes the default if there was none.
SourceConfig add_source(const fs::path& folder);
// Adds a CalDAV source (Add CalDAV Account…): named after `title`, else the
// server ("https://caldav.fastmail.com/" → "fastmail"), made unique, with
// its lists in default_caldav_folder(name). It becomes the default if there
// was none.
SourceConfig add_caldav_source(const CaldavSettings& caldav, const std::string& title);
// Removes a source from settings.ini (its folder and files stay as they are).
void remove_source(const std::string& name);

// Opens a source: its Store, with per-device state in
// <folder>/.reminders/<device>/, and the back end set up (Syncthing:
// .stignore). Doesn't load the lists.
std::unique_ptr<Store> open_source(const SourceConfig& source, const std::string& device);

}  // namespace rem
