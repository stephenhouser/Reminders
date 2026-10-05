#include "reminders/server_sync.hpp"

#include <sys/wait.h>

#include <cstdio>
#include <format>
#include <mutex>

#include "reminders/backend_module.hpp"
#include "reminders/caldav_client.hpp"
#include "reminders/git_sync.hpp"
#include "reminders/webdav_client.hpp"

namespace rem {

std::string run_password_command(const std::string& command) {
	if (command.empty()) {
		return "";
	}
	auto* pipe = popen(command.c_str(), "r");
	if (!pipe) {
		throw SyncError("couldn't run password-command");
	}
	std::string out;
	char buf[256];
	while (auto n = std::fread(buf, 1, sizeof buf, pipe)) {
		out.append(buf, n);
	}
	int status = pclose(pipe);
	if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		throw SyncError("password-command failed");
	}
	if (auto nl = out.find('\n'); nl != std::string::npos) {
		out.resize(nl);
	}
	if (!out.empty() && out.back() == '\r') {
		out.pop_back();
	}
	return out;
}

void register_network_backends() {
	static std::once_flag once;
	std::call_once(once, [] {
		find_backend("caldav")->sync = sync_caldav_source;
		find_backend("webdav")->sync = sync_webdav_source;
		find_backend("git")->sync = sync_git_source;
	});
}

SyncResult sync_source(Store& store, const SourceConfig& source) {
	register_network_backends();
	auto& module = backend_of(source);
	if (!module.syncs()) {
		throw SyncError(std::format("{} isn't synced by the app", source.name));
	}
	return module.sync(store, source);
}

}  // namespace rem
