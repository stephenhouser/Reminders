#include "reminders/server_sync.hpp"

#include <cstdio>
#include <format>
#include <sys/wait.h>

#include "reminders/caldav_client.hpp"
#include "reminders/git_sync.hpp"
#include "reminders/webdav_client.hpp"

namespace rem {

std::string run_password_command(const std::string& command) {
    if (command.empty()) return "";
    auto* pipe = popen(command.c_str(), "r");
    if (!pipe) throw SyncError("couldn't run password-command");
    std::string out;
    char buf[256];
    while (auto n = std::fread(buf, 1, sizeof buf, pipe)) out.append(buf, n);
    int status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw SyncError("password-command failed");
    if (auto nl = out.find('\n'); nl != std::string::npos) out.resize(nl);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return out;
}

SyncResult sync_source(Store& store, const SourceConfig& source) {
    switch (source.backend) {
    case BackendKind::Caldav: return sync_caldav_source(store, source);
    case BackendKind::Webdav: return sync_webdav_source(store, source);
    case BackendKind::Git: return sync_git_source(store, source);
    default: throw SyncError(std::format("{} isn't synced by the app", source.name));
    }
}

}  // namespace rem
