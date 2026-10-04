// The CalDAV back end: task lists on a CalDAV server (Nextcloud, Fastmail,
// Radicale, …), one calendar per list.
//
// The lists live in a local folder like any other source's (by default
// $XDG_DATA_HOME/reminders/caldav/<source>/, see paths.hpp), so the apps, undo
// and hand editing work as usual. caldav_sync() brings that folder and the
// server in step: it pulls changed tasks, merges them three-way with local
// edits (the base being the last synced version), writes the merged list, and
// pushes what changed locally. Offline edits simply wait for the next sync.
//
// Per-device records, in <state_dir>/caldav/:
//   calendars.tsv         list name, list name at the last sync, calendar
//                         href, its CTag, its display name and colour
//   renamed.tsv,          lists renamed or deleted in the app (ServerBackend),
//   deleted.txt           applied by the next sync
//   <calendar>/base.md    the list as of the last sync (merge base)
//   <calendar>/items.tsv  id, UID, href and ETag of every task
//   <calendar>/<id>.ics   the task as the server last sent it (so properties
//                         this app doesn't know are sent back untouched)
//
// The network side is the Remote interface; the apps use the libcurl one
// (net/), the tests a fake.
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "reminders/backend.hpp"

namespace rem {

namespace fs = std::filesystem;

struct RemoteCalendar {
		std::string href;	// collection URL path, ending in '/'
		std::string name;	// displayname
		std::string color;	// "#RRGGBB" (calendar-color), may be empty
		std::string ctag;  // getctag or sync-token: changes when anything in it
						   // does; may be empty
};

struct RemoteItem {
		std::string href;
		std::string etag;
};

struct RemoteObject {
		std::string href;
		std::string etag;
		std::string data;  // iCalendar text
};

class Remote {
	public:
		virtual ~Remote() = default;
		// The calendars that can hold tasks (VTODO).
		virtual std::vector<RemoteCalendar> calendars() = 0;
		// The tasks in a calendar, without their data.
		virtual std::vector<RemoteItem> items(const std::string& calendar) = 0;
		virtual std::vector<RemoteObject> fetch(
			const std::string& calendar,
			const std::vector<std::string>& hrefs) = 0;
		// Stores a task. `if_match` empty: create only (If-None-Match: *).
		// Returns the new ETag ("" when the server doesn't say), or nullopt
		// when the precondition failed (someone else changed it first).
		virtual std::optional<std::string> put(const std::string& href,
											   const std::string& data,
											   const std::string& if_match) = 0;
		// False when the ETag no longer matches. A task already gone counts as
		// removed.
		virtual bool remove(const std::string& href,
							const std::string& etag) = 0;
		// Makes a task calendar under the user's calendar home; returns its
		// href.
		virtual std::string create_calendar(const std::string& name,
											const std::string& color) = 0;
		virtual void update_calendar(const std::string& href,
									 const std::string& name,
									 const std::string& color) = 0;
		virtual void delete_calendar(const std::string& href) = 0;
};

struct SyncOptions {
		const std::chrono::time_zone* zone = nullptr;  // null: the local zone
		std::chrono::sys_seconds now{};				   // zero: the current time
};

// Syncs `folder` with the server. `lock` is the back end's write lock (see
// ServerBackend), held while files are written so the app's own saves can't
// interleave. Throws SyncError if the server can't be reached at all.
SyncResult caldav_sync(const fs::path& folder, const fs::path& state_dir,
					   Remote& remote, std::mutex& lock,
					   const SyncOptions& options = {});

// The nearest of the app's colours to "#RRGGBB", and a colour's "#RRGGBB".
std::string color_from_hex(std::string_view hex);
std::string hex_from_color(std::string_view color);

}  // namespace rem
