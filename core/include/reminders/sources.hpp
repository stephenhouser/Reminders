// Sources: where lists come from. Each is a back end and its settings, in a
// [source.NAME] section of settings.ini:
//
//   [general]
//   default-source=personal
//
//   [source.personal]
//   backend=syncthing        (or local; see backend.hpp)
//   folder=/home/you/Sync/Reminders
//
// For now the apps open one source: the default one (default-source, else
// the first), or the folder given on the command line.
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

struct SourceConfig {
    std::string name;  // [source.NAME]; empty for a folder used only this session
    BackendKind backend = BackendKind::Syncthing;
    fs::path folder;
    std::string title;  // title=, shown on its sidebar group; empty: from the name
};

// The source's title: title=, else its name capitalised ("personal" →
// "Personal"), else the folder's name.
std::string source_title(const SourceConfig& source);

// Every [source.NAME] with a folder, in file order. A missing or unknown
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
// folder, if there is none. Returns it.
SourceConfig set_default_folder(const fs::path& folder);

// Adds a source for `folder` (Add Source…): named after it (made unique),
// with the back end it needs. It becomes the default if there was none.
SourceConfig add_source(const fs::path& folder);
// Removes a source from settings.ini (its folder and files stay as they are).
void remove_source(const std::string& name);

// Opens a source: its Store, with per-device state in
// <folder>/.reminders/<device>/, and the back end set up (Syncthing:
// .stignore). Doesn't load the lists.
std::unique_ptr<Store> open_source(const SourceConfig& source, const std::string& device);

}  // namespace rem
