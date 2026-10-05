// HTTP and WebDAV plumbing shared by the CalDAV and WebDAV clients
// (internal to the net library): libcurl requests and multistatus replies.
#pragma once

#include <curl/curl.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace rem::dav {

std::string trim(std::string s);
std::string xml_escape(std::string_view s);
std::string lower(std::string s);

// "%20" → " ", and back for one path segment (everything but unreserved
// characters is escaped).
std::string url_decode(std::string_view s);
std::string url_encode_segment(std::string_view s);

// A path on the server from an href, which may be a full URL.
std::string path_of(const std::string& href);

struct Response {
		long status = 0;
		std::string body;
		std::string etag;
};

// One connection's worth of requests (libcurl keeps it open between them).
// Throws SyncError when the server can't be reached, and on HTTP 401.
class Http {
	public:
		Http(std::string user, std::string password);
		~Http();
		Http(const Http&) = delete;
		Http& operator=(const Http&) = delete;

		Response request(const std::string& method, const std::string& url,
						 const std::string& body = "",
						 std::vector<std::string> headers = {});

	private:
		CURL* curl_ = nullptr;
		std::string user_, password_;
};

// One <response> of a 207 Multi-Status: its properties with status 200.
struct DavResponse {
		std::string href;
		std::map<std::string, std::string>
			text;  // by property name (local name)
		std::map<std::string, std::string>
			hrefs;	// current-user-principal etc.: the href inside
		std::vector<std::string> resourcetype;	// child element names
		std::vector<std::string>
			components;	 // supported-calendar-component-set
};

std::vector<DavResponse> parse_multistatus(const std::string& body);

}  // namespace rem::dav
