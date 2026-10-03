// Sources: where lists come from. Each is a back end and its settings, in a
// [source.NAME] section of settings.ini:
//
//   [general]
//   default-source=personal
//
//   [source.personal]
//   backend=syncthing        (or local, caldav or webdav; see backend.hpp)
//   folder=~/Sync/Reminders  (~, $HOME and ${VAR} work; see expand_path)
//
//   [source.fastmail]
//   backend=caldav
//   url=https://caldav.fastmail.com/
//   username=you@fastmail.com
//   password-command=secret-tool lookup service reminders-caldav
//   interval=15              (minutes between syncs)
//
//   [source.cloud]
//   backend=webdav
//   url=https://cloud.example.com/remote.php/dav/files/you/Reminders/
//   username=you             (password-command= and interval= as above)
//
//   [source.notes]
//   backend=git
//   folder=~/notes/todo      (in a git working tree; a subfolder is fine)
//   url=git@github.com:you/notes.git   (optional: cloned if folder= isn't
//                            a repository yet; default folder: as for DAV)
//   remote=origin            (optional; default origin)
//   branch=main              (optional; default the one checked out)
//   interval=15
//
// A CalDAV or WebDAV source's folder is its local copy, by default
// $XDG_DATA_HOME/reminders/BACKEND/NAME/. Each source's per-device records
// are in source_state_dir().
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

// A CalDAV or WebDAV source's server settings.
struct DavSettings {
    std::string url;               // CalDAV: the server, or its calendar home; WebDAV: the folder
    std::string username;
    std::string password_command;  // run by the shell; the first line it prints is the password
    int interval = 15;             // minutes between syncs

    bool operator==(const DavSettings&) const = default;
};

// A git source's settings. Signing in is git's business (SSH keys, a
// credential helper), as on the command line.
struct GitSettings {
    std::string url;     // to clone from when the folder isn't a repository yet
    std::string remote;  // empty: origin
    std::string branch;  // empty: the branch checked out
    int interval = 15;   // minutes between syncs

    bool operator==(const GitSettings&) const = default;
};

struct SourceConfig {
    std::string name;  // [source.NAME]; empty for a folder used only this session
    BackendKind backend = BackendKind::Syncthing;
    fs::path folder;
    std::string title;  // title=, shown on its sidebar group; empty: from the name
    DavSettings dav = {};  // CalDAV and WebDAV only (has_server)
    GitSettings git = {};  // git only
};

// Minutes between a source's syncs (CalDAV, WebDAV, git).
int sync_interval(const SourceConfig& source);

// Whether `folder` is in a git working tree (a .git at or above it).
bool in_git_repo(const fs::path& folder);

// Where a CalDAV or WebDAV source keeps its local copy, and a git source
// its clone, unless folder= says otherwise:
// $XDG_DATA_HOME/reminders/BACKEND/NAME (~/.local/share/…).
fs::path default_copy_folder(BackendKind backend, const std::string& name);

// A source's per-device records. Syncthing: <folder>/.reminders/DEVICE/
// (merge bases; .stignore keeps it out of the sync). Others:
// $XDG_STATE_HOME/reminders/DEVICE/NAME, or …/DEVICE/folder-HASH for a
// folder that isn't a configured source. The device is part of the path
// because a folder is shared between devices, and a home folder can be.
fs::path source_state_dir(const SourceConfig& source, const std::string& device);

// The source's title: title=, else its name capitalised ("personal" →
// "Personal"), else the folder's name.
std::string source_title(const SourceConfig& source);

// Every [source.NAME] with a folder (CalDAV and WebDAV ones: with a url), in
// file order. A missing or unknown backend= is syncthing.
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
// folder, if there is none. If the default is on a server (CalDAV, WebDAV),
// the folder becomes a source of its own (the default) instead. Returns it.
SourceConfig set_default_folder(const fs::path& folder);

// The [source.NAME] a new source would get: from its title, else (CalDAV,
// WebDAV) the server ("https://caldav.fastmail.com/" → "fastmail"), (git)
// the repository ("git@github.com:you/notes.git" → "notes"), else its
// folder's name; lower case, made unique among the configured sources.
std::string new_source_name(const SourceConfig& source);
// Adds a source (Add Source…). With no name it gets new_source_name(); a
// CalDAV or WebDAV one, or a git one with a url=, with no folder gets
// default_copy_folder().
// It becomes the default if there was none. Returns it as saved.
SourceConfig add_source(SourceConfig source);
// Adds a source for `folder`, with the back end it needs.
SourceConfig add_source(const fs::path& folder);
// Removes a source from settings.ini, and this device's records for it.
// Its folder and files stay as they are, except a CalDAV or WebDAV source's
// local copy in the default place, which is only a copy of the server's
// (a git clone stays: it may hold commits not pushed yet).
void remove_source(const std::string& name);

// Opens a source: its Store, with per-device state in source_state_dir()
// (for a source other than Syncthing, moved there from <folder>/.reminders/,
// where older versions kept it), and the back end set up (Syncthing:
// .stignore). Doesn't load the lists.
std::unique_ptr<Store> open_source(const SourceConfig& source, const std::string& device);

}  // namespace rem
