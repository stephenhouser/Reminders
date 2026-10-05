// Syncing the sources the app syncs itself (backend_module.hpp: syncs()).
//
// SyncRunner keeps a Library's synced sources in step in the background:
// each one when the runner starts, every `interval=` minutes, and two seconds
// after one of its list files changes (an edit in the app, or by hand). The
// lists it writes reach the app the usual way, as changed files.
#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "reminders/library.hpp"
#include "reminders/sources.hpp"

namespace rem {

// Syncs a source with its server or remote, through its back end. Only reads
// the Store's folder, state folder and back end, so it can run on another
// thread while the Store is in use: the lists it writes come back to the app
// as outside changes. Throws SyncError when it can't be reached, or when the
// source isn't one the app syncs.
SyncResult sync_source(Store& store, const SourceConfig& source);

class SyncRunner {
	public:
		// Picks up the library's CalDAV, WebDAV and git sources; does nothing
		// if there are none.
		// The library's stores must outlive the runner.
		explicit SyncRunner(Library& library);
		~SyncRunner();
		SyncRunner(const SyncRunner&) = delete;
		SyncRunner& operator=(const SyncRunner&) = delete;

		bool active() const { return !jobs_.empty(); }
		// Syncs every source as soon as possible.
		void sync_now();
		// Syncs one source (by name) as soon as possible.
		void sync_now(const std::string& source);

		struct Status {
				bool syncing = false;
				std::vector<std::string>
					errors;	 // new since the last take_status(), "source:
							 // message"
				std::optional<std::chrono::system_clock::time_point>
					last_sync;	// last one without errors
		};
		Status take_status();

	private:
		using Snapshot =
			std::map<std::string, std::pair<std::filesystem::file_time_type,
											std::uintmax_t>>;
		struct Job {
				SourceConfig config;
				Store* store;
				Snapshot seen;	// list files as of the last sync
				std::chrono::steady_clock::time_point
					next;  // when to sync next
		};

		void run();
		static Snapshot snapshot(const std::filesystem::path& folder);

		std::vector<Job> jobs_;
		std::mutex mutex_;
		std::condition_variable wake_;
		bool stop_ = false, now_ = false;
		std::set<std::string>
			now_sources_;  // sync_now(source), not yet started
		Status status_;
		std::thread thread_;
};

}  // namespace rem
