#include "reminders/caldav_client.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <random>

#include "dav.hpp"
#include "reminders/paths.hpp"
#include "reminders/server_sync.hpp"

namespace rem {

namespace {

using namespace net;

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
    CurlRemote(const DavSettings& s, std::string password, fs::path cache_file)
        : http_(s.username, std::move(password)), url_(s.url), username_(s.username), cache_file_(std::move(cache_file)) {
        auto scheme = url_.find("://");
        if (scheme == std::string::npos) throw SyncError("url= needs to start with https:// (or http://)");
        auto path = url_.find('/', scheme + 3);
        origin_ = url_.substr(0, path);
        if (path == std::string::npos) url_ += "/";
    }

    std::vector<RemoteCalendar> calendars() override {
        try {
            return list_calendars();
        } catch (const SyncError&) {
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
            throw SyncError(std::format("{} {} failed: HTTP {}", method, href, r.status));
    }

    Response dav(const std::string& method, const std::string& path, const std::string& body, const std::string& depth) {
        std::vector<std::string> headers{"Content-Type: application/xml; charset=utf-8"};
        if (!depth.empty()) headers.push_back("Depth: " + depth);
        auto r = http_.request(method, origin_ + path, body, headers);
        if (r.status != 207 && (r.status < 200 || r.status >= 300))
            throw SyncError(std::format("{} {} failed: HTTP {}", method, path, r.status));
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
        if (!home) throw SyncError(std::format("no CalDAV calendars found at {}", url_));
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

std::unique_ptr<Remote> make_caldav_remote(const DavSettings& settings, const fs::path& cache_file) {
    if (settings.url.empty()) throw SyncError("the source has no url=");
    return std::make_unique<CurlRemote>(settings, run_password_command(settings.password_command), cache_file);
}

SyncResult sync_caldav_source(Store& store, const SourceConfig& source) {
    auto* backend = dynamic_cast<ServerBackend*>(&store.backend_object());
    if (!backend || backend->kind() != BackendKind::Caldav) throw SyncError(std::format("{} isn't a CalDAV source", source.name));
    auto cache = source.name.empty() ? fs::path{} : cache_dir() / "caldav" / (source.name + ".home");
    auto remote = make_caldav_remote(source.dav, cache);
    return caldav_sync(store.folder(), store.state_dir(), *remote, backend->lock());
}

}  // namespace rem
