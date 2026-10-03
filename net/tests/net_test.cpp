// CalDAV over HTTP against fake_caldav.py (started here on a free port).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>

#include "reminders/caldav_client.hpp"
#include "reminders/library.hpp"
#include "reminders/sync_runner.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/vtodo.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

std::string server_url;

std::string read(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

CaldavSettings settings(std::string password = "secret", std::string path = "/") {
    return CaldavSettings{server_url + path, "alice", "printf '%s\\n' '" + password + "'", 15};
}

struct Dir {
    fs::path path = fs::temp_directory_path() / ("reminders-net-" + new_id());
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string todo(std::string_view uid, std::string_view summary) {
    return std::format("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Other//EN\r\nBEGIN:VTODO\r\nUID:{}\r\n"
                       "SUMMARY:{}\r\nX-OTHER:kept\r\nEND:VTODO\r\nEND:VCALENDAR\r\n",
                       uid, summary);
}

}  // namespace

TEST(net_password_command) {
    CHECK_EQ(run_password_command("printf 'pw one\\nignored\\n'"), std::string("pw one"));
    CHECK_EQ(run_password_command(""), std::string(""));
    bool threw = false;
    try {
        run_password_command("exit 3");
    } catch (const CaldavError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(net_wrong_password) {
    auto remote = make_caldav_remote(settings("nope"));
    bool threw = false;
    try {
        remote->calendars();
    } catch (const CaldavError& e) {
        threw = std::string_view(e.what()).find("password") != std::string_view::npos;
    }
    CHECK(threw);
}

TEST(net_discovery_and_calendars) {
    // From the server root (via /.well-known/caldav) and from the principal.
    for (auto path : {"/", "/dav/principals/alice/"}) {
        auto remote = make_caldav_remote(settings("secret", path));
        auto href = remote->create_calendar(std::string("Errands ") + path, "#FF9500");
        bool found = false;
        for (auto& c : remote->calendars())
            if (c.href == href) {
                found = true;
                CHECK_EQ(c.color, std::string("#FF9500"));  // alpha dropped
                CHECK(!c.ctag.empty());
            }
        CHECK(found);
        remote->update_calendar(href, "Renamed", "");
        for (auto& c : remote->calendars())
            if (c.href == href) CHECK_EQ(c.name, std::string("Renamed"));
        remote->delete_calendar(href);
    }
}

TEST(net_sync_round_trip) {
    Dir dir;
    auto folder = dir.path / "lists", state = dir.path / "state";
    std::mutex lock;
    // Another client makes a list with one task.
    auto other = make_caldav_remote(settings());
    auto href = other->create_calendar("Shopping", "#34C759");
    CHECK(other->put(href + "milk.ics", todo("milk-uid", "Milk"), "").has_value());

    auto mine = make_caldav_remote(settings());
    auto r = caldav_sync(folder, state, *mine, lock);
    CHECK(r.errors.empty());
    auto text = read(folder / "Shopping.md");
    CHECK(text.find("- [ ] Milk") != std::string::npos);
    CHECK(text.find("color: green") != std::string::npos);

    // Edit here: complete Milk, add Bread.
    text.replace(text.find("- [ ] Milk"), 5, "- [x]");
    text += "- [ ] Bread\n";
    std::ofstream(folder / "Shopping.md") << text;
    r = caldav_sync(folder, state, *mine, lock);
    CHECK(r.errors.empty());

    auto items = other->items(href);
    CHECK_EQ(items.size(), std::size_t{2});
    std::vector<std::string> hrefs;
    for (auto& i : items) hrefs.push_back(i.href);
    int done = 0, bread = 0;
    for (auto& o : other->fetch(href, hrefs)) {
        auto cal = parse_ical(o.data);
        auto t = read_todo(*todo_of(*cal), nullptr);
        if (t.reminder.title == "Milk") {
            done += t.reminder.done;
            CHECK(o.data.find("X-OTHER:kept") != std::string::npos);
        }
        bread += t.reminder.title == "Bread";
    }
    CHECK_EQ(done, 1);
    CHECK_EQ(bread, 1);

    // A second sync with nothing new sends nothing and changes nothing.
    r = caldav_sync(folder, state, *mine, lock);
    CHECK(r.changed.empty());
    other->delete_calendar(href);
}

TEST(net_sync_runner) {
    Dir dir;
    auto other = make_caldav_remote(settings());
    auto href = other->create_calendar("Chores", "");
    CHECK(other->put(href + "bins.ics", todo("bins-uid", "Bins out"), "").has_value());

    SourceConfig config{"chores", BackendKind::Caldav, dir.path / "lists", "", settings()};
    fs::create_directories(config.folder);
    Library library;
    library.add(config, std::make_unique<Store>(config.folder, dir.path / "state", BackendKind::Caldav));
    auto file = config.folder / "Chores.md";
    auto wait_for = [](auto&& cond) {
        for (int i = 0; i < 100 && !cond(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return cond();
    };
    SyncRunner runner(library);
    CHECK(runner.active());
    // Synced as soon as it starts …
    CHECK(wait_for([&] { return fs::exists(file); }));
    // … and two seconds or so after a list changes here.
    auto text = read(file);
    std::ofstream(file) << text << "- [ ] Sweep\n";
    auto pushed = [&] { return other->items(href).size() == 2; };
    CHECK(wait_for(pushed));
    CHECK(runner.take_status().errors.empty());
    other->delete_calendar(href);
}

TEST(net_calendar_home_cache) {
    Dir dir;
    auto cache = dir.path / "fm.home";
    make_caldav_remote(settings(), cache)->calendars();
    CHECK_EQ(read(cache), server_url + "/\nalice\n/dav/calendars/alice/\n");
    // A home that stopped working is found again (and the cache fixed).
    std::ofstream(cache) << server_url << "/\nalice\n/dav/old/home/\n";
    bool ok = true;
    try {
        make_caldav_remote(settings(), cache)->calendars();
    } catch (const CaldavError&) {
        ok = false;
    }
    CHECK(ok);
    CHECK_EQ(read(cache), server_url + "/\nalice\n/dav/calendars/alice/\n");
    // Another account's cache isn't used.
    std::ofstream(cache) << server_url << "/\nbob\n/dav/old/home/\n";
    make_caldav_remote(settings(), cache)->calendars();
    CHECK_EQ(read(cache), server_url + "/\nalice\n/dav/calendars/alice/\n");
}

int main(int argc, char** argv) {
    // net_tests PATH/fake_caldav.py
    if (argc < 2) {
        std::cerr << "usage: net_tests fake_caldav.py\n";
        return 2;
    }
    auto cmd = std::format("python3 '{}'", argv[1]);
    auto* server = popen(cmd.c_str(), "r");
    char port[32] = {};
    if (!server || !std::fgets(port, sizeof port, server)) {
        std::cerr << "couldn't start the fake CalDAV server\n";
        return 2;
    }
    server_url = std::format("http://127.0.0.1:{}", std::string(port, std::strcspn(port, "\n")));

    for (auto& c : test::registry()) {
        int before = test::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++test::failures();
            std::cerr << c.name << ": threw " << e.what() << "\n";
        }
        if (test::failures() != before) std::cerr << "FAILED: " << c.name << "\n";
    }
    std::cout << test::registry().size() << " tests, " << test::failures() << " failed checks\n";

    // Stop the server.
    if (std::system(std::format("curl -s '{}/_quit' > /dev/null", server_url).c_str()) != 0)
        std::cerr << "couldn't stop the fake server; it stops by itself after 120 s\n";
    pclose(server);
    return test::failures() == 0 ? 0 : 1;
}
