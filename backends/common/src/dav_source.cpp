#include "reminders/dav_source.hpp"

#include <sys/wait.h>

#include <cstdio>

namespace rem {

DavSettings dav_settings(const SourceConfig& source) {
	return {source.option("url"), source.option("username"),
			source.option("password-command"), sync_interval(source)};
}

void set_dav_settings(SourceConfig& source, const DavSettings& settings) {
	source.options["url"] = settings.url;
	source.options["username"] = settings.username;
	source.options["password-command"] = settings.password_command;
	source.options["interval"] = std::to_string(settings.interval);
}

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

std::string server_problem(const SourceConfig& s) {
	auto url = s.option("url");
	if (url.empty()) {
		return "Enter the server's address";
	}
	if (!url.starts_with("https://") && !url.starts_with("http://")) {
		return "The address starts with https://";
	}
	if (url.find('.', url.find("://")) == std::string::npos &&
		url.find("localhost") == std::string::npos &&
		url.find("127.0.0.1") == std::string::npos) {
		return "That doesn't look like a server's address";
	}
	return {};
}

std::string server_name_hint(const SourceConfig& s) {
	// The host's second-to-last label: caldav.fastmail.com → fastmail.
	auto url = s.option("url");
	if (url.empty()) {
		return {};
	}
	auto start = url.find("://");
	auto host = url.substr(start == std::string::npos ? 0 : start + 3);
	host = host.substr(0, host.find_first_of("/:"));
	auto last = host.rfind('.');
	if (last != std::string::npos && last > 0) {
		auto prev = host.rfind('.', last - 1);
		auto from = prev == std::string::npos ? 0 : prev + 1;
		host = host.substr(from, last - from);
	}
	return host;
}

std::vector<SettingField> server_fields(const std::string& url_label,
										const std::string& url_hint) {
	return {
		{"url", url_label, url_hint, SettingField::Url, "Server"},
		{"username", "Username", "", SettingField::Text, "Server"},
		{"password-command", "Password Command",
		 "A command that prints the password, e.g. “secret-tool lookup service "
		 "reminders-caldav” or “pass show "
		 "caldav”",
		 SettingField::Command, "Server"},
		{"interval", "Sync Every",
		 "Minutes; changes made here are sent straight away",
		 SettingField::Minutes, "Server", 15},
	};
}

}  // namespace rem
