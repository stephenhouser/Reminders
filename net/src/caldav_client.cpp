#include "reminders/caldav_client.hpp"

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <sys/wait.h>

#include "reminders/paths.hpp"

namespace rem {

namespace {

constexpr std::string_view kDav = "DAV:";

std::string trim(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && ws(s[i])) ++i;
    return s.substr(i);
}

std::string xml_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ── HTTP ────────────────────────────────────────────────────────────────

struct Response {
    long status = 0;
    std::string body;
    std::string etag;
};

class Http {
public:
    Http(std::string user, std::string password) : user_(std::move(user)), password_(std::move(password)) {
        static const bool initialised = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == 0; }();
        (void)initialised;
        curl_ = curl_easy_init();
        if (!curl_) throw CaldavError("couldn't start libcurl");
    }
    ~Http() { curl_easy_cleanup(curl_); }
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    Response request(const std::string& method, const std::string& url, const std::string& body = "",
                     std::vector<std::string> headers = {}) {
        curl_easy_reset(curl_);
        Response r;
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "Reminders (CalDAV)");
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        if (!user_.empty()) {
            curl_easy_setopt(curl_, CURLOPT_USERNAME, user_.c_str());
            curl_easy_setopt(curl_, CURLOPT_PASSWORD, password_.c_str());
            curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, CURLAUTH_BASIC | CURLAUTH_DIGEST);
        }
        if (!body.empty()) {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
        headers.push_back("Expect:");
        curl_slist* list = nullptr;
        for (auto& h : headers) list = curl_slist_append(list, h.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, +[](char* p, size_t size, size_t n, void* out) -> size_t {
            static_cast<std::string*>(out)->append(p, size * n);
            return size * n;
        });
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &r.body);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, +[](char* p, size_t size, size_t n, void* out) -> size_t {
            std::string_view line(p, size * n);
            if (line.size() > 5 && lower(std::string(line.substr(0, 5))) == "etag:")
                *static_cast<std::string*>(out) = trim(std::string(line.substr(5)));
            return size * n;
        });
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &r.etag);
        auto rc = curl_easy_perform(curl_);
        curl_slist_free_all(list);
        if (rc != CURLE_OK) throw CaldavError(std::format("{}: {}", url, curl_easy_strerror(rc)));
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &r.status);
        if (r.status == 401) throw CaldavError("the server didn't accept the username or password");
        return r;
    }

private:
    CURL* curl_ = nullptr;
    std::string user_, password_;
};

// ── WebDAV multistatus ──────────────────────────────────────────────────

struct DavResponse {
    std::string href;
    std::map<std::string, std::string> text;    // by property name (local name)
    std::map<std::string, std::string> hrefs;   // current-user-principal etc.: the href inside
    std::vector<std::string> resourcetype;      // child element names
    std::vector<std::string> components;        // supported-calendar-component-set
};

bool is(const xmlNode* n, std::string_view name, std::string_view ns = kDav) {
    return n && n->type == XML_ELEMENT_NODE && n->name && name == reinterpret_cast<const char*>(n->name) &&
           (ns.empty() || (n->ns && n->ns->href && ns == reinterpret_cast<const char*>(n->ns->href)));
}

std::string content(const xmlNode* n) {
    auto* c = xmlNodeGetContent(n);
    std::string out = c ? reinterpret_cast<const char*>(c) : "";
    xmlFree(c);
    return out;
}

template <class F> void each_child(const xmlNode* n, F&& f) {
    for (auto* c = n ? n->children : nullptr; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE) f(c);
}

std::vector<DavResponse> parse_multistatus(const std::string& body) {
    std::vector<DavResponse> out;
    auto* doc = xmlReadMemory(body.data(), static_cast<int>(body.size()), "response.xml", nullptr,
                              XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) throw CaldavError("the server sent XML that couldn't be read");
    if (auto* root = xmlDocGetRootElement(doc); is(root, "multistatus")) {
        each_child(root, [&](const xmlNode* resp) {
            if (!is(resp, "response")) return;
            DavResponse r;
            each_child(resp, [&](const xmlNode* c) {
                if (is(c, "href")) r.href = trim(content(c));
                if (!is(c, "propstat")) return;
                bool ok = false;
                each_child(c, [&](const xmlNode* s) {
                    if (is(s, "status")) ok = content(s).find(" 200") != std::string::npos;
                });
                if (!ok) return;
                each_child(c, [&](const xmlNode* prop) {
                    if (!is(prop, "prop")) return;
                    each_child(prop, [&](const xmlNode* p) {
                        std::string name = reinterpret_cast<const char*>(p->name);
                        r.text[name] = name == "calendar-data" ? content(p) : trim(content(p));
                        each_child(p, [&](const xmlNode* inner) {
                            std::string iname = reinterpret_cast<const char*>(inner->name);
                            if (name == "resourcetype") r.resourcetype.push_back(iname);
                            if (iname == "href" && !r.hrefs.contains(name)) r.hrefs[name] = trim(content(inner));
                            if (iname == "comp")
                                if (auto* a = xmlGetProp(inner, reinterpret_cast<const xmlChar*>("name"))) {
                                    r.components.push_back(reinterpret_cast<const char*>(a));
                                    xmlFree(a);
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

// ── The Remote ──────────────────────────────────────────────────────────

constexpr std::string_view kNs =
    R"(xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav" xmlns:cs="http://calendarserver.org/ns/" )"
    R"(xmlns:a="http://apple.com/ns/ical/")";

std::string new_collection_name() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return std::format("{:016x}", rng());
}

class CurlRemote : public Remote {
public:
    CurlRemote(const CaldavSettings& s, std::string password, fs::path cache_file)
        : http_(s.username, std::move(password)), url_(s.url), username_(s.username), cache_file_(std::move(cache_file)) {
        auto scheme = url_.find("://");
        if (scheme == std::string::npos) throw CaldavError("url= needs to start with https:// (or http://)");
        auto path = url_.find('/', scheme + 3);
        origin_ = url_.substr(0, path);
        if (path == std::string::npos) url_ += "/";
    }

    std::vector<RemoteCalendar> calendars() override {
        try {
            return list_calendars();
        } catch (const CaldavError&) {
            if (!home_from_cache_) throw;
            // The remembered calendar home may be out of date: find it again.
            home_.clear();
            home_from_cache_ = false;
            std::error_code ec;
            fs::remove(cache_file_, ec);
            return list_calendars();
        }
    }

    std::vector<RemoteCalendar> list_calendars() {
        auto& home = calendar_home();
        auto body = std::format(R"(<?xml version="1.0" encoding="utf-8"?><d:propfind {}><d:prop>)"
                                R"(<d:resourcetype/><d:displayname/><cs:getctag/><d:sync-token/>)"
                                R"(<c:supported-calendar-component-set/><a:calendar-color/></d:prop></d:propfind>)",
                                kNs);
        auto r = dav("PROPFIND", home, body, "1");
        std::vector<RemoteCalendar> out;
        for (auto& resp : parse_multistatus(r.body)) {
            auto href = path_of(resp.href);
            if (href == home || std::ranges::find(resp.resourcetype, "calendar") == resp.resourcetype.end()) continue;
            if (!resp.components.empty() && std::ranges::find(resp.components, "VTODO") == resp.components.end())
                continue;
            if (!href.ends_with('/')) href += '/';
            auto color = resp.text["calendar-color"];
            if (color.size() > 7) color.resize(7);  // #RRGGBBAA
            auto ctag = resp.text["getctag"].empty() ? resp.text["sync-token"] : resp.text["getctag"];
            auto name = resp.text["displayname"];
            if (name.empty()) {
                auto trimmed = href.substr(0, href.size() - 1);
                name = trimmed.substr(trimmed.rfind('/') + 1);
            }
            out.push_back({href, name, color, ctag});
        }
        return out;
    }

    std::vector<RemoteItem> items(const std::string& calendar) override {
        auto body = std::format(R"(<?xml version="1.0" encoding="utf-8"?><c:calendar-query {}>)"
                                R"(<d:prop><d:getetag/></d:prop><c:filter><c:comp-filter name="VCALENDAR">)"
                                R"(<c:comp-filter name="VTODO"/></c:comp-filter></c:filter></c:calendar-query>)",
                                kNs);
        auto r = dav("REPORT", calendar, body, "1");
        std::vector<RemoteItem> out;
        for (auto& resp : parse_multistatus(r.body)) {
            auto href = path_of(resp.href);
            if (href == calendar || !resp.text.contains("getetag")) continue;
            out.push_back({href, resp.text["getetag"]});
        }
        return out;
    }

    std::vector<RemoteObject> fetch(const std::string& calendar, const std::vector<std::string>& hrefs) override {
        std::vector<RemoteObject> out;
        for (std::size_t start = 0; start < hrefs.size(); start += 50) {
            std::string list;
            for (auto i = start; i < std::min(hrefs.size(), start + 50); ++i)
                list += "<d:href>" + xml_escape(hrefs[i]) + "</d:href>";
            auto body = std::format(R"(<?xml version="1.0" encoding="utf-8"?><c:calendar-multiget {}>)"
                                    R"(<d:prop><d:getetag/><c:calendar-data/></d:prop>{}</c:calendar-multiget>)",
                                    kNs, list);
            auto r = dav("REPORT", calendar, body, "1");
            for (auto& resp : parse_multistatus(r.body))
                if (resp.text.contains("calendar-data"))
                    out.push_back({path_of(resp.href), resp.text["getetag"], resp.text["calendar-data"]});
        }
        return out;
    }

    std::optional<std::string> put(const std::string& href, const std::string& data,
                                   const std::string& if_match) override {
        std::vector<std::string> headers{"Content-Type: text/calendar; charset=utf-8"};
        headers.push_back(if_match.empty() ? "If-None-Match: *" : "If-Match: " + if_match);
        auto r = http_.request("PUT", origin_ + href, data, headers);
        if (r.status == 412) return std::nullopt;
        check(r, "PUT", href);
        return r.etag;
    }

    bool remove(const std::string& href, const std::string& etag) override {
        std::vector<std::string> headers;
        if (!etag.empty()) headers.push_back("If-Match: " + etag);
        auto r = http_.request("DELETE", origin_ + href, "", headers);
        if (r.status == 412) return false;
        if (r.status == 404) return true;
        check(r, "DELETE", href);
        return true;
    }

    std::string create_calendar(const std::string& name, const std::string& color) override {
        auto href = calendar_home() + new_collection_name() + "/";
        auto body = std::format(R"(<?xml version="1.0" encoding="utf-8"?><c:mkcalendar {}><d:set><d:prop>)"
                                R"(<d:displayname>{}</d:displayname>{})"
                                R"(<c:supported-calendar-component-set><c:comp name="VTODO"/>)"
                                R"(</c:supported-calendar-component-set></d:prop></d:set></c:mkcalendar>)",
                                kNs, xml_escape(name), color_prop(color));
        auto r = http_.request("MKCALENDAR", origin_ + href, body, {"Content-Type: application/xml; charset=utf-8"});
        check(r, "MKCALENDAR", href);
        return href;
    }

    void update_calendar(const std::string& href, const std::string& name, const std::string& color) override {
        auto body = std::format(R"(<?xml version="1.0" encoding="utf-8"?><d:propertyupdate {}><d:set><d:prop>)"
                                R"(<d:displayname>{}</d:displayname>{}</d:prop></d:set></d:propertyupdate>)",
                                kNs, xml_escape(name), color_prop(color));
        check(dav("PROPPATCH", href, body, ""), "PROPPATCH", href);
    }

    void delete_calendar(const std::string& href) override {
        auto r = http_.request("DELETE", origin_ + href);
        if (r.status != 404) check(r, "DELETE", href);
    }

private:
    Http http_;
    std::string url_, origin_, home_, username_;
    fs::path cache_file_;  // url, username and calendar home, a line each
    bool home_from_cache_ = false;

    static std::string color_prop(const std::string& color) {
        return color.empty() ? "" : "<a:calendar-color>" + xml_escape(color) + "</a:calendar-color>";
    }

    static void check(const Response& r, std::string_view method, std::string_view href) {
        if (r.status < 200 || r.status >= 300)
            throw CaldavError(std::format("{} {} failed: HTTP {}", method, href, r.status));
    }

    // A path on the server from an href (which may be a full URL).
    std::string path_of(const std::string& href) const {
        if (href.starts_with("http://") || href.starts_with("https://")) {
            auto path = href.find('/', href.find("://") + 3);
            return path == std::string::npos ? "/" : href.substr(path);
        }
        return href;
    }

    Response dav(const std::string& method, const std::string& path, const std::string& body, const std::string& depth) {
        std::vector<std::string> headers{"Content-Type: application/xml; charset=utf-8"};
        if (!depth.empty()) headers.push_back("Depth: " + depth);
        auto r = http_.request(method, origin_ + path, body, headers);
        if (r.status != 207 && (r.status < 200 || r.status >= 300))
            throw CaldavError(std::format("{} {} failed: HTTP {}", method, path, r.status));
        return r;
    }

    // calendar-home-set of `path`, or of its current-user-principal.
    std::optional<std::string> home_from(const std::string& path) {
        auto body = std::format(R"(<?xml version="1.0" encoding="utf-8"?><d:propfind {}><d:prop>)"
                                R"(<d:current-user-principal/><c:calendar-home-set/></d:prop></d:propfind>)",
                                kNs);
        auto r = http_.request("PROPFIND", origin_ + path, body,
                               {"Content-Type: application/xml; charset=utf-8", "Depth: 0"});
        if (r.status != 207) return std::nullopt;
        std::optional<std::string> principal;
        for (auto& resp : parse_multistatus(r.body)) {
            if (resp.hrefs.contains("calendar-home-set")) return path_of(resp.hrefs["calendar-home-set"]);
            if (resp.hrefs.contains("current-user-principal")) principal = path_of(resp.hrefs["current-user-principal"]);
        }
        if (principal && *principal != path) return home_from(*principal);
        return std::nullopt;
    }

    const std::string& calendar_home() {
        if (!home_.empty()) return home_;
        if (!cache_file_.empty()) {
            std::ifstream in(cache_file_);
            std::string url, user, home;
            if (std::getline(in, url) && std::getline(in, user) && std::getline(in, home) && url == url_ &&
                user == username_ && home.starts_with('/')) {
                home_from_cache_ = true;
                return home_ = home;
            }
        }
        auto path = path_of(url_);
        auto home = home_from(path);
        if (!home) home = home_from("/.well-known/caldav");
        if (!home) throw CaldavError(std::format("no CalDAV calendars found at {}", url_));
        home_ = *home;
        if (!home_.ends_with('/')) home_ += '/';
        if (!cache_file_.empty()) {
            std::error_code ec;
            fs::create_directories(cache_file_.parent_path(), ec);
            std::ofstream(cache_file_) << url_ << '\n' << username_ << '\n' << home_ << '\n';
        }
        return home_;
    }
};

}  // namespace

std::string run_password_command(const std::string& command) {
    if (command.empty()) return "";
    auto* pipe = popen(command.c_str(), "r");
    if (!pipe) throw CaldavError("couldn't run password-command");
    std::string out;
    char buf[256];
    while (auto n = std::fread(buf, 1, sizeof buf, pipe)) out.append(buf, n);
    int status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw CaldavError("password-command failed");
    if (auto nl = out.find('\n'); nl != std::string::npos) out.resize(nl);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return out;
}

std::unique_ptr<Remote> make_caldav_remote(const CaldavSettings& settings, const fs::path& cache_file) {
    if (settings.url.empty()) throw CaldavError("the source has no url=");
    return std::make_unique<CurlRemote>(settings, run_password_command(settings.password_command), cache_file);
}

SyncResult sync_caldav_source(Store& store, const SourceConfig& source) {
    auto* backend = dynamic_cast<CaldavBackend*>(&store.backend_object());
    if (!backend) throw CaldavError(std::format("{} isn't a CalDAV source", source.name));
    auto cache = source.name.empty() ? fs::path{} : cache_dir() / "caldav" / (source.name + ".home");
    auto remote = make_caldav_remote(source.caldav, cache);
    return caldav_sync(store.folder(), store.state_dir(), *remote, backend->lock());
}

}  // namespace rem
