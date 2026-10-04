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
//   caldav     The folder is a local copy of task lists on a CalDAV server,
//              kept in step by caldav_sync() (see caldav.hpp).
//   webdav     The folder is a local copy of list files kept in a folder on a
//              WebDAV server, kept in step by webdav_sync() (see webdav.hpp).
//   git        The folder is in a git working tree: changed lists are
//              committed, and pulled and pushed with its remote, by
//              git_sync() (net/, git_sync.hpp).
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rem {

namespace fs = std::filesystem;

enum class BackendKind { Syncthing, Local, Caldav, Webdav, Git };

// "syncthing" / "local" / "caldav" / "webdav" / "git"; parse_backend is
// case-insensitive.
std::string_view backend_name(BackendKind kind);
std::optional<BackendKind> parse_backend(std::string_view name);
// CalDAV and WebDAV: the folder is a local copy of what's on a server.
bool has_server(BackendKind kind);
// The back ends the app syncs itself (SyncRunner, `reminders sync`):
// CalDAV, WebDAV and git.
bool syncs(BackendKind kind);

// A network or server failure; the sync stops and is retried later.
struct SyncError : std::runtime_error {
		using std::runtime_error::runtime_error;
};

// What a sync with a server did.
struct SyncResult {
		std::vector<std::string>
			changed;  // lists whose file was written, created or deleted
		std::vector<std::string> errors;  // per list; the rest still synced
};

class Backend {
	public:
		virtual ~Backend() = default;
		virtual BackendKind kind() const = 0;

		// Run once when the source is opened (Syncthing: keep the per-device
		// state out of the sync).
		virtual void prepare([[maybe_unused]] const fs::path& folder) {}

		// The list a file in the folder belongs to, or nullopt if it can't be a
		// list file. (Syncthing: a conflict copy belongs to its list.)
		virtual std::optional<std::string> list_name_for(
			const fs::path& file) const;
		// Other files with changes for list `name`, to merge into it.
		virtual std::vector<fs::path> conflict_copies(
			const fs::path& folder, std::string_view name) const;

		// The version to merge conflicting changes against, and recognising
		// this device's own writes when they come back.
		virtual std::optional<std::string> read_base(
			std::string_view name) const;
		virtual void write_base(std::string_view name,
								const std::string& text) const;
		virtual void remember_written(std::string_view name,
									  const std::string& text) const;
		virtual bool is_own_write(std::string_view name,
								  const std::string& text) const;
		// A list was renamed or deleted: its records follow.
		virtual void move_state(std::string_view from,
								std::string_view to) const;
		virtual void drop_state(std::string_view name) const;
		// The user deleted a list in the app (not: its file disappeared).
		virtual void deleted_by_user(
			[[maybe_unused]] std::string_view name) const {}

		// Held while the Store writes, renames or deletes a list file, when the
		// back end changes files from another thread (CalDAV sync).
		virtual std::mutex* write_lock() { return nullptr; }
};

// The back end of a source the app syncs (CalDAV, WebDAV, git): plain files,
// like local, plus a note of each list renamed or deleted in the app, for
// the next sync to apply on the server. The notes are in
// <state_dir>/<backend name>/ (records_dir()), beside the sync's records:
//   renamed.tsv  old name, new name
//   deleted.txt  lists the user deleted
// Syncing writes the folder from another thread, so the Store's writes and
// the sync's take turns under write_lock().
class ServerBackend : public Backend {
	public:
		ServerBackend(BackendKind kind, fs::path state_dir)
			: kind_(kind), state_dir_(std::move(state_dir)) {}
		BackendKind kind() const override { return kind_; }
		std::mutex* write_lock() override { return &lock_; }
		void move_state(std::string_view from,
						std::string_view to) const override;
		void deleted_by_user(std::string_view name) const override;

		std::mutex& lock() { return lock_; }
		const fs::path& state_dir() const { return state_dir_; }
		fs::path records_dir() const;

	private:
		BackendKind kind_;
		fs::path state_dir_;
		mutable std::mutex lock_;
};

// `state_dir` is the per-device folder (Syncthing's records live there).
std::unique_ptr<Backend> make_backend(BackendKind kind, fs::path state_dir);

}  // namespace rem
