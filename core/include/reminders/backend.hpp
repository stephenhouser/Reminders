// What keeps a source's folder in step with other devices, and the special
// handling that needs. The Store does the loading, saving and queries; a
// back end only adds what its kind of syncing requires.
//
//   syncthing  Syncthing syncs the folder. Conflict copies ("X.sync-conflict-
//              …md") are merged three-way, using a per-device record of the
//              last version that came from another device (the merge base)
//              and of the last version this device wrote, kept in
//              <folder>/.reminders/<device>/, which .stignore keeps out of
//              Syncthing.
//   local      Just the folder: files are read and written as they are,
//              nothing else.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rem {

namespace fs = std::filesystem;

enum class BackendKind { Syncthing, Local };

// "syncthing" / "local"; parse_backend is case-insensitive.
std::string_view backend_name(BackendKind kind);
std::optional<BackendKind> parse_backend(std::string_view name);

class Backend {
public:
    virtual ~Backend() = default;
    virtual BackendKind kind() const = 0;

    // Run once when the source is opened (Syncthing: keep the per-device
    // state out of the sync).
    virtual void prepare(const fs::path& folder) { (void)folder; }

    // The list a file in the folder belongs to, or nullopt if it can't be a
    // list file. (Syncthing: a conflict copy belongs to its list.)
    virtual std::optional<std::string> list_name_for(const fs::path& file) const;
    // Other files with changes for list `name`, to merge into it.
    virtual std::vector<fs::path> conflict_copies(const fs::path& folder, std::string_view name) const;

    // The version to merge conflicting changes against, and recognising this
    // device's own writes when they come back.
    virtual std::optional<std::string> read_base(std::string_view name) const;
    virtual void write_base(std::string_view name, const std::string& text) const;
    virtual void remember_written(std::string_view name, const std::string& text) const;
    virtual bool is_own_write(std::string_view name, const std::string& text) const;
    // A list was renamed or deleted: its records follow.
    virtual void move_state(std::string_view from, std::string_view to) const;
    virtual void drop_state(std::string_view name) const;
};

// `state_dir` is the per-device folder (Syncthing's records live there).
std::unique_ptr<Backend> make_backend(BackendKind kind, fs::path state_dir);

}  // namespace rem
