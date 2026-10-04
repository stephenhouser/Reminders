#include "reminders/sync_runner.hpp"

#include <format>

#include "reminders/server_sync.hpp"

namespace rem {

namespace {

using namespace std::chrono;

constexpr auto kQuiet = seconds{2};	 // after a change, wait this long for more
constexpr auto kPoll = seconds{1};

}  // namespace

SyncRunner::SyncRunner(Library& library) {
	auto now = steady_clock::now();
	for (auto& s : library.sources()) {
		if (syncs(s.config.backend) && s.store) {
			jobs_.push_back({s.config, s.store.get(), {}, now});
		}
	}
	now_ = true;  // the first sync right away
	if (!jobs_.empty()) {
		thread_ = std::thread([this] { run(); });
	}
}

SyncRunner::~SyncRunner() {
	{
		std::lock_guard g(mutex_);
		stop_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) {
		thread_.join();
	}
}

void SyncRunner::sync_now() {
	{
		std::lock_guard g(mutex_);
		now_ = true;
	}
	wake_.notify_all();
}

void SyncRunner::sync_now(const std::string& source) {
	{
		std::lock_guard g(mutex_);
		now_sources_.insert(source);
	}
	wake_.notify_all();
}

SyncRunner::Status SyncRunner::take_status() {
	std::lock_guard g(mutex_);
	auto out = status_;
	status_.errors.clear();
	return out;
}

SyncRunner::Snapshot SyncRunner::snapshot(const fs::path& folder) {
	Snapshot out;
	std::error_code ec;
	for (auto& e : fs::directory_iterator(folder, ec)) {
		auto name = e.path().filename().string();
		if (!name.ends_with(".md") || name.starts_with('.')) {
			continue;
		}
		out[name] = {e.last_write_time(ec), e.file_size(ec)};
	}
	return out;
}

void SyncRunner::run() {
	std::unique_lock lock(mutex_);
	while (!stop_) {
		wake_.wait_for(lock, kPoll, [this] {
			return stop_ || now_ || !now_sources_.empty();
		});
		if (stop_) {
			break;
		}
		auto now = steady_clock::now();
		if (now_) {
			for (auto& j : jobs_) {
				j.next = now;
			}
			now_ = false;
		}
		for (auto& j : jobs_) {
			if (now_sources_.contains(j.config.name)) {
				j.next = now;
			}
		}
		now_sources_.clear();
		for (auto& j : jobs_) {
			// A change here since the last sync: sync once it has settled.
			auto files = snapshot(j.config.folder);
			if (files != j.seen && j.next > now + kQuiet) {
				j.next = now + kQuiet;
			}
			if (now < j.next || stop_) {
				continue;
			}

			status_.syncing = true;
			lock.unlock();
			std::vector<std::string> errors;
			// What the files looked like going in; the ones the sync wrote
			// are taken as they are after it, so edits made meanwhile to
			// other lists still count as changes.
			auto seen = snapshot(j.config.folder);
			try {
				auto result = sync_source(*j.store, j.config);
				for (auto& e : result.errors) {
					errors.push_back(
						std::format("{}: {}", source_title(j.config), e));
				}
				auto after = snapshot(j.config.folder);
				for (auto& list : result.changed) {
					auto file = list + ".md";
					if (auto it = after.find(file); it != after.end()) {
						seen[file] = it->second;
					} else {
						seen.erase(file);
					}
				}
			} catch (const std::exception& e) {
				errors.push_back(
					std::format("{}: {}", source_title(j.config), e.what()));
			}
			lock.lock();
			status_.syncing = false;
			for (auto& e : errors) {
				status_.errors.push_back(e);
			}
			if (errors.empty()) {
				status_.last_sync = system_clock::now();
			}
			j.seen = std::move(seen);
			// Retry sooner after a failure, but not in a tight loop.
			auto wait = errors.empty()
						  ? minutes{sync_interval(j.config)}
						  : minutes{std::min(sync_interval(j.config), 2)};
			j.next = steady_clock::now() + wait;
		}
	}
}

}  // namespace rem
