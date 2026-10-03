// The CalDAV back end: task lists on a CalDAV server (Nextcloud, Fastmail,
// Radicale, …), one calendar per list.
//
// The lists live in a local folder like any other source's (by default
// ~/.local/share/reminders/caldav/<source>/), so the apps, undo and hand
// editing work as usual. caldav_sync() brings that folder and the server in
// step: it pulls changed tasks, merges them three-way with local edits (the
// base being the last synced version), writes the merged list, and pushes
// what changed locally. Offline edits simply wait for the next sync.
//
// Per-device records, in <state_dir>/caldav/:
//   calendars.tsv         list name, list name at the last sync, calendar
//                         href, its CTag, its display name and colour
//   deleted.txt           lists the user deleted, whose calendars go next sync
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
#include <stdexcept>
#include <string>
#include <vector>

#include "reminders/backend.hpp"

namespace rem {

namespace fs = std::filesystem;

// A network or server failure; the sync stops and is retried later.
struct CaldavError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct RemoteCalendar {
    std::string href;   // collection URL path, ending in '/'
    std::string name;   // displayname
    std::string color;  // "#RRGGBB" (calendar-color), may be empty
    std::string ctag;   // getctag or sync-token: changes when anything in it does; may be empty
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
    virtual std::vector<RemoteObject> fetch(const std::string& calendar, const std::vector<std::string>& hrefs) = 0;
    // Stores a task. `if_match` empty: create only (If-None-Match: *).
    // Returns the new ETag ("" when the server doesn't say), or nullopt when
    // the precondition failed (someone else changed it first).
    virtual std::optional<std::string> put(const std::string& href, const std::string& data,
                                           const std::string& if_match) = 0;
    // False when the ETag no longer matches. A task already gone counts as removed.
    virtual bool remove(const std::string& href, const std::string& etag) = 0;
    // Makes a task calendar under the user's calendar home; returns its href.
    virtual std::string create_calendar(const std::string& name, const std::string& color) = 0;
    virtual void update_calendar(const std::string& href, const std::string& name, const std::string& color) = 0;
    virtual void delete_calendar(const std::string& href) = 0;
};

struct SyncResult {
    std::vector<std::string> changed;  // lists whose file was written, created or deleted
    std::vector<std::string> errors;   // per calendar; the rest still synced
};

struct SyncOptions {
    const std::chrono::time_zone* zone = nullptr;  // null: the local zone
    std::chrono::sys_seconds now{};                // zero: the current time
};

// Syncs `folder` with the server. `lock` is the back end's write lock (see
// CaldavBackend), held while files are written so the app's own saves can't
// interleave. Throws CaldavError if the server can't be reached at all.
SyncResult caldav_sync(const fs::path& folder, const fs::path& state_dir, Remote& remote, std::mutex& lock,
                       const SyncOptions& options = {});

// The back end the Store uses for a CalDAV source's folder: plain files
// (like local), plus the records above kept in step when a list is renamed
// or deleted in the app.
class CaldavBackend : public Backend {
public:
    explicit CaldavBackend(fs::path state_dir) : state_dir_(std::move(state_dir)) {}
    BackendKind kind() const override { return BackendKind::Caldav; }
    std::mutex* write_lock() override { return &lock_; }
    void move_state(std::string_view from, std::string_view to) const override;
    void deleted_by_user(std::string_view name) const override;

    std::mutex& lock() { return lock_; }
    const fs::path& state_dir() const { return state_dir_; }

private:
    fs::path state_dir_;
    mutable std::mutex lock_;
};

// The nearest of the app's colours to "#RRGGBB", and a colour's "#RRGGBB".
std::string color_from_hex(std::string_view hex);
std::string hex_from_color(std::string_view color);

}  // namespace rem
