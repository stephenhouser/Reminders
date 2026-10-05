// The WebDAV back end: list files kept in a folder on a WebDAV server
// (Nextcloud, ownCloud, a NAS, Apache mod_dav, …), as they are: one
// Markdown file per list, in the format the folder sources use.
//
// The lists live in a local folder like any other source's (by default
// $XDG_DATA_HOME/reminders/webdav/<source>/, see paths.hpp), so the apps, undo
// and hand editing work as usual. webdav_sync() brings that folder and the
// server's in step, file by file: a file changed on one side is copied to the
// other; changed on both, the two are merged three-way (the base being the
// version of the last sync), as Syncthing conflict copies are. Writes are
// conditional on the server's ETag, so another device's change made
// meanwhile is merged next time rather than overwritten.
//
// What syncs: the folder's top-level *.md files. Files on the server are
// all copied here (one that isn't a list yet shows as one to review, as in
// any folder); files made here go to the server when they're lists.
//
// Per-device records, in <state_dir>/webdav/:
//   files.tsv          list name, its name on the server (they differ until
//                      a rename made here is sent), its ETag
//   base/<name>.md     each file as of the last sync (merge base), by its
//                      name on the server
//   renamed.tsv,       lists renamed or deleted in the app (ServerBackend),
//   deleted.txt        applied by the next sync
//
// The network side is the FileRemote interface; the apps use the libcurl
// one (net/), the tests a fake.
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "reminders/backend.hpp"

namespace rem {

namespace fs = std::filesystem;

// Files on the server are named by their list name: "Groceries" is
// Groceries.md in the server's folder.
struct RemoteFile {
		std::string name;
		std::string etag;  // may be empty if the server doesn't give one
};

struct RemoteText {
		std::string text;
		std::string etag;
};

class FileRemote {
	public:
		virtual ~FileRemote() = default;
		// The folder's *.md files (not hidden ones). A folder that doesn't
		// exist yet is made (empty).
		virtual std::vector<RemoteFile> files() = 0;
		// A file's text, or nullopt if it's gone.
		virtual std::optional<RemoteText> get(const std::string& name) = 0;
		// Stores a file. `if_match` empty: create only (If-None-Match: *).
		// Returns the new ETag ("" when the server doesn't say), or nullopt
		// when the precondition failed (someone else changed it first).
		virtual std::optional<std::string> put(const std::string& name,
											   const std::string& text,
											   const std::string& if_match) = 0;
		// False when the ETag no longer matches. A file already gone counts as
		// removed.
		virtual bool remove(const std::string& name,
							const std::string& etag) = 0;
		// Renames a file, never replacing one. False if `from` is gone or `to`
		// is taken.
		virtual bool move(const std::string& from, const std::string& to) = 0;
};

// Syncs `folder` with the server's folder. `lock` is the back end's write
// lock (ServerBackend::lock()), held while files are written so the app's
// own saves can't interleave. Throws SyncError if the server can't be
// reached at all; problems with single files are in the result's errors.
SyncResult webdav_sync(const fs::path& folder, const fs::path& state_dir,
					   FileRemote& remote, std::mutex& lock);

}  // namespace rem
