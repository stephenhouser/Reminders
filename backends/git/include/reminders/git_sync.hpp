// The git back end: lists in a folder of a git working tree, committed,
// pulled and pushed by running git (so SSH keys, credential helpers and
// the rest of the user's git set-up apply; git never asks for anything:
// a sync that needs a password fails instead).
//
// A sync:
//   1. commits the folder's changed list files (its top-level *.md, and
//      nothing else in the repository) as "Reminders (DEVICE): Groceries, …";
//   2. fetches the branch from the remote;
//   3. merges it. A list file changed on both sides is merged by this
//      app's three-way merge (docs/FORMAT.md, "Conflicts"), reminder by
//      reminder, with git's base, ours and theirs, not by git's line merge;
//      a conflict anywhere else stops the merge (it's undone) and the
//      error says where;
//   4. pushes, and if someone pushed meanwhile, fetches and merges again.
// A repository without the remote just gets the commits: history alone.
// A folder that isn't a repository yet is cloned from url=, if there is one.
// The Store's write lock is held while git changes files (commit, merge),
// not while it talks to the remote.
#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include "reminders/backend.hpp"
#include "reminders/sources.hpp"
#include "reminders/store.hpp"

namespace rem {

namespace fs = std::filesystem;

// A git source's settings, from its options. Signing in is git's business
// (SSH keys, a credential helper), as on the command line.
struct GitSettings {
		std::string url;  // to clone from (or push to) when the folder isn't a
						  // repository yet
		std::string remote;	 // empty: origin
		std::string branch;	 // empty: the branch checked out
		int interval = 15;	 // minutes between syncs

		bool operator==(const GitSettings&) const = default;
};
GitSettings git_settings(const SourceConfig& source);
void set_git_settings(SourceConfig& source, const GitSettings& settings);

// Whether `folder` is in a git working tree (a .git at or above it).
bool in_git_repo(const fs::path& folder);

// Throws SyncError when git can't be run, the folder isn't a repository
// (and there's no url= to clone), or the remote can't be reached.
SyncResult git_sync(const fs::path& folder, const GitSettings& settings,
					std::mutex& lock, const std::string& device);

// Syncs a git source (see sync_source).
SyncResult sync_git_source(Store& store, const SourceConfig& source);

// Adds the back end to the registry (backend_module.hpp).
void register_git_backend();

}  // namespace rem
