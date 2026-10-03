// The CalDAV server end of a CalDAV source: the Remote interface over HTTP
// (libcurl, with libxml2 for the WebDAV XML), and syncing a source with it.
#pragma once

#include <memory>
#include <string>

#include "reminders/caldav.hpp"
#include "reminders/sources.hpp"
#include "reminders/store.hpp"

namespace rem {

// The password from password-command= (the first line it prints). Throws
// CaldavError if the command fails.
std::string run_password_command(const std::string& command);

// A Remote for the server in `settings` (the password command is run here).
// The calendar home is found on first use: from url= itself, its
// current-user-principal, or /.well-known/caldav.
std::unique_ptr<Remote> make_caldav_remote(const CaldavSettings& settings);

// Syncs a CalDAV source's folder with its server. Only reads the Store's
// folder, state folder and back end, so it can run on another thread while
// the Store is in use: the lists it writes come back to the app as outside
// changes. Throws CaldavError when the server can't be reached.
SyncResult sync_caldav_source(Store& store, const SourceConfig& source);

}  // namespace rem
