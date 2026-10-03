// The server end of a WebDAV source: the FileRemote interface over HTTP
// (libcurl, with libxml2 for PROPFIND replies), and syncing a source with it.
#pragma once

#include <memory>

#include "reminders/server_sync.hpp"
#include "reminders/sources.hpp"
#include "reminders/store.hpp"
#include "reminders/webdav.hpp"

namespace rem {

// A FileRemote for the folder at url= in `settings` (the password command
// is run here). The folder is made on first use if it doesn't exist; its
// parent must.
std::unique_ptr<FileRemote> make_webdav_remote(const DavSettings& settings);

// Syncs a WebDAV source's folder with the server's (see sync_source).
SyncResult sync_webdav_source(Store& store, const SourceConfig& source);

}  // namespace rem
