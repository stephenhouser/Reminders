// For the CalDAV and WebDAV tests against fake_dav.py, which dav_test_main.cpp
// starts on a free port.
#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "reminders/dav_source.hpp"
#include "reminders/store.hpp"

namespace davtest {

using namespace rem;

// The fake server's address ("http://127.0.0.1:PORT").
extern std::string server_url;

inline std::string read(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

inline DavSettings settings(std::string password = "secret",
							std::string path = "/") {
	return DavSettings{server_url + path, "alice",
					   "printf '%s\\n' '" + password + "'", 15};
}

struct Dir {
		fs::path path =
			fs::temp_directory_path() / ("reminders-net-" + new_id());
		~Dir() {
			std::error_code ec;
			fs::remove_all(path, ec);
		}
};

inline std::string todo(std::string_view uid, std::string_view summary) {
	return std::format(
		"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Other//"
		"EN\r\nBEGIN:VTODO\r\nUID:{}\r\n"
		"SUMMARY:{}\r\nX-OTHER:kept\r\nEND:VTODO\r\nEND:VCALENDAR\r\n",
		uid, summary);
}

}  // namespace davtest
