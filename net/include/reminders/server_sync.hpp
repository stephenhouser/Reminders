// Syncing the sources the app syncs itself (CalDAV, WebDAV, git).
#pragma once

#include <string>

#include "reminders/backend.hpp"
#include "reminders/sources.hpp"
#include "reminders/store.hpp"

namespace rem {

// The password from password-command= (the first line it prints). Throws
// SyncError if the command fails.
std::string run_password_command(const std::string& command);

// Syncs a CalDAV, WebDAV or git source with its server (sync_caldav_source,
// sync_webdav_source, sync_git_source). Only reads the Store's folder, state
// folder and back end, so it can run on another thread while the Store is in
// use: the lists it writes come back to the app as outside changes. Throws
// SyncError when the server can't be reached.
SyncResult sync_source(Store& store, const SourceConfig& source);

// Attaches the sync of caldav, webdav and git to their registry entries
// (backend_module.hpp), so they report syncs(). sync_source and SyncRunner
// do it themselves; the apps call it before asking the registry.
void register_network_backends();

}  // namespace rem
