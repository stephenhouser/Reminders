// What the CalDAV and WebDAV modules share about their sources: the server
// settings in a source's options, the password command, checking and naming
// a server source, and the settings rows for the Add Source form.
#pragma once

#include <string>
#include <vector>

#include "reminders/backend_module.hpp"
#include "reminders/sources.hpp"

namespace rem {

// A CalDAV or WebDAV source's server settings, from its options.
struct DavSettings {
		std::string url;  // CalDAV: the server, or its calendar home; WebDAV:
						  // the folder
		std::string username;
		std::string password_command;  // run by the shell; the first line it
									   // prints is the password
		int interval = 15;			   // minutes between syncs

		bool operator==(const DavSettings&) const = default;
};
DavSettings dav_settings(const SourceConfig& source);
void set_dav_settings(SourceConfig& source, const DavSettings& settings);

// The password from password-command= (the first line it prints). Throws
// SyncError if the command fails.
std::string run_password_command(const std::string& command);

// What's wrong with a server source's address, or "".
std::string server_problem(const SourceConfig& source);
// A name from the server: https://caldav.fastmail.com/ → "fastmail".
std::string server_name_hint(const SourceConfig& source);
// The Server group of the Add Source form (url=, username=,
// password-command=, interval=); `url_label` / `url_hint` describe the
// address.
std::vector<SettingField> server_fields(const std::string& url_label,
										const std::string& url_hint);

}  // namespace rem
