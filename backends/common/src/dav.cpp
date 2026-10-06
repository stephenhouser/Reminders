#include "reminders/dav.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cctype>
#include <format>

#include "reminders/backend.hpp"

namespace rem::dav {

namespace {

constexpr std::string_view kDav = "DAV:";

bool is(const xmlNode* n, std::string_view name, std::string_view ns = kDav) {
	return n && n->type == XML_ELEMENT_NODE && n->name &&
		   name == reinterpret_cast<const char*>(n->name) &&
		   (ns.empty() || (n->ns && n->ns->href &&
						   ns == reinterpret_cast<const char*>(n->ns->href)));
}

std::string content(const xmlNode* n) {
	auto* c = xmlNodeGetContent(n);
	std::string out = c ? reinterpret_cast<const char*>(c) : "";
	xmlFree(c);
	return out;
}

template <class F>
void each_child(const xmlNode* n, F&& f) {
	for (auto* c = n ? n->children : nullptr; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE) {
			f(c);
		}
	}
}

}  // namespace

std::string trim(std::string s) {
	auto ws = [](char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	};
	while (!s.empty() && ws(s.back())) {
		s.pop_back();
	}
	std::size_t i = 0;
	while (i < s.size() && ws(s[i])) {
		++i;
	}
	return s.substr(i);
}

std::string xml_escape(std::string_view s) {
	std::string out;
	for (char c : s) {
		switch (c) {
			case '&':
				out += "&amp;";
				break;
			case '<':
				out += "&lt;";
				break;
			case '>':
				out += "&gt;";
				break;
			case '"':
				out += "&quot;";
				break;
			default:
				out += c;
		}
	}
	return out;
}

std::string lower(std::string s) {
	for (auto& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string url_decode(std::string_view s) {
	std::string out;
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size() &&
			std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
			std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
			out += static_cast<char>(
				std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::string url_encode_segment(std::string_view s) {
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += std::format("%{:02X}", c);
		}
	}
	return out;
}

std::string path_of(const std::string& href) {
	if (href.starts_with("http://") || href.starts_with("https://")) {
		auto path = href.find('/', href.find("://") + 3);
		return path == std::string::npos ? "/" : href.substr(path);
	}
	return href;
}

// ── HTTP ────────────────────────────────────────────────────────────────

Http::Http(std::string user, std::string password)
	: user_(std::move(user)), password_(std::move(password)) {
	[[maybe_unused]] static const bool initialised = [] {
		return curl_global_init(CURL_GLOBAL_DEFAULT) == 0;
	}();
	curl_ = curl_easy_init();
	if (!curl_) {
		throw SyncError("couldn't start libcurl");
	}
}

Http::~Http() { curl_easy_cleanup(curl_); }

Response Http::request(const std::string& method, const std::string& url,
					   const std::string& body,
					   std::vector<std::string> headers) {
	curl_easy_reset(curl_);
	Response r;
	curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());
	curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 5L);
	// Keep the method and body across a 301/302/303: without this curl drops
	// the PROPFIND body when /.well-known/caldav redirects (Fastmail does).
	curl_easy_setopt(curl_, CURLOPT_POSTREDIR,
					 static_cast<long>(CURL_REDIR_POST_ALL));
	curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl_, CURLOPT_USERAGENT, "Reminders");
	curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
	if (!user_.empty()) {
		curl_easy_setopt(curl_, CURLOPT_USERNAME, user_.c_str());
		curl_easy_setopt(curl_, CURLOPT_PASSWORD, password_.c_str());
		curl_easy_setopt(curl_, CURLOPT_HTTPAUTH,
						 CURLAUTH_BASIC | CURLAUTH_DIGEST);
	}
	// A PUT sends its body even when it's empty (an empty file).
	if (!body.empty() || method == "PUT") {
		curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
		curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE,
						 static_cast<curl_off_t>(body.size()));
	}
	headers.push_back("Expect:");
	curl_slist* list = nullptr;
	for (auto& h : headers) {
		list = curl_slist_append(list, h.c_str());
	}
	curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(
		curl_, CURLOPT_WRITEFUNCTION,
		+[](char* p, size_t size, size_t n, void* out) -> size_t {
			static_cast<std::string*>(out)->append(p, size * n);
			return size * n;
		});
	curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &r.body);
	curl_easy_setopt(
		curl_, CURLOPT_HEADERFUNCTION,
		+[](char* p, size_t size, size_t n, void* out) -> size_t {
			std::string_view line(p, size * n);
			if (line.size() > 5 &&
				lower(std::string(line.substr(0, 5))) == "etag:") {
				*static_cast<std::string*>(out) =
					trim(std::string(line.substr(5)));
			}
			return size * n;
		});
	curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &r.etag);
	auto rc = curl_easy_perform(curl_);
	curl_slist_free_all(list);
	if (rc != CURLE_OK) {
		throw SyncError(std::format("{}: {}", url, curl_easy_strerror(rc)));
	}
	curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &r.status);
	if (r.status == 401) {
		throw SyncError("the server didn't accept the username or password");
	}
	return r;
}

// ── WebDAV multistatus ──────────────────────────────────────────────────

std::vector<DavResponse> parse_multistatus(const std::string& body) {
	std::vector<DavResponse> out;
	auto* doc = xmlReadMemory(
		body.data(), static_cast<int>(body.size()), "response.xml", nullptr,
		XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
	if (!doc) {
		throw SyncError("the server sent XML that couldn't be read");
	}
	if (auto* root = xmlDocGetRootElement(doc); is(root, "multistatus")) {
		each_child(root, [&](const xmlNode* resp) {
			if (!is(resp, "response")) {
				return;
			}
			DavResponse r;
			each_child(resp, [&](const xmlNode* c) {
				if (is(c, "href")) {
					r.href = trim(content(c));
				}
				if (!is(c, "propstat")) {
					return;
				}
				bool ok = false;
				each_child(c, [&](const xmlNode* s) {
					if (is(s, "status")) {
						ok = content(s).find(" 200") != std::string::npos;
					}
				});
				if (!ok) {
					return;
				}
				each_child(c, [&](const xmlNode* prop) {
					if (!is(prop, "prop")) {
						return;
					}
					each_child(prop, [&](const xmlNode* p) {
						std::string name =
							reinterpret_cast<const char*>(p->name);
						r.text[name] = name == "calendar-data"
										 ? content(p)
										 : trim(content(p));
						each_child(p, [&](const xmlNode* inner) {
							std::string iname =
								reinterpret_cast<const char*>(inner->name);
							if (name == "resourcetype") {
								r.resourcetype.push_back(iname);
							}
							if (iname == "href" && !r.hrefs.contains(name)) {
								r.hrefs[name] = trim(content(inner));
							}
							if (iname == "comp") {
								if (auto* a = xmlGetProp(
										inner, reinterpret_cast<const xmlChar*>(
												   "name"))) {
									r.components.push_back(
										reinterpret_cast<const char*>(a));
									xmlFree(a);
								}
							}
						});
					});
				});
			});
			out.push_back(std::move(r));
		});
	}
	xmlFreeDoc(doc);
	return out;
}

}  // namespace rem::dav
