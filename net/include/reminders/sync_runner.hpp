// Keeps a Library's CalDAV sources in sync in the background: each one when
// the runner starts, every `interval=` minutes, and two seconds after one of
// its list files changes (an edit in the app, or by hand). The lists it
// writes reach the app the usual way, as changed files.
#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "reminders/library.hpp"

namespace rem {

class SyncRunner {
public:
    // Picks up the library's CalDAV sources; does nothing if there are none.
    // The library's stores must outlive the runner.
    explicit SyncRunner(Library& library);
    ~SyncRunner();
    SyncRunner(const SyncRunner&) = delete;
    SyncRunner& operator=(const SyncRunner&) = delete;

    bool active() const { return !jobs_.empty(); }
    // Syncs every source as soon as possible.
    void sync_now();

    struct Status {
        bool syncing = false;
        std::vector<std::string> errors;  // new since the last take_status(), "source: message"
        std::optional<std::chrono::system_clock::time_point> last_sync;  // last one without errors
    };
    Status take_status();

private:
    using Snapshot = std::map<std::string, std::pair<std::filesystem::file_time_type, std::uintmax_t>>;
    struct Job {
        SourceConfig config;
        Store* store;
        Snapshot seen;  // list files as of the last sync
        std::chrono::steady_clock::time_point next;  // when to sync next
    };

    void run();
    static Snapshot snapshot(const std::filesystem::path& folder);

    std::vector<Job> jobs_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false, now_ = false;
    Status status_;
    std::thread thread_;
};

}  // namespace rem
