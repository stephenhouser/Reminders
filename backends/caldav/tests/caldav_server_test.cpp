// The CalDAV client over HTTP against fake_dav.py.
#include <thread>

#include "dav_test.hpp"
#include "reminders/caldav_client.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/library.hpp"
#include "reminders/sync_runner.hpp"
#include "reminders/vtodo.hpp"
#include "test.hpp"

using namespace rem;
using namespace davtest;
using namespace std::chrono;

// The tests use the real back end from the registry.
static const bool registered = (register_caldav_backend(), true);

TEST(net_password_command) {
	CHECK_EQ(run_password_command("printf 'pw one\\nignored\\n'"),
			 std::string("pw one"));
	CHECK_EQ(run_password_command(""), std::string(""));
	bool threw = false;
	try {
		run_password_command("exit 3");
	} catch (const SyncError&) {
		threw = true;
	}
	CHECK(threw);
}

TEST(net_wrong_password) {
	auto remote = make_caldav_remote(settings("nope"));
	bool threw = false;
	try {
		remote->calendars();
	} catch (const SyncError& e) {
		threw = std::string_view(e.what()).find("password") !=
				std::string_view::npos;
	}
	CHECK(threw);
}

TEST(net_discovery_and_calendars) {
	// From the server root (via /.well-known/caldav) and from the principal.
	for (auto path : {"/", "/dav/principals/alice/"}) {
		auto remote = make_caldav_remote(settings("secret", path));
		auto href =
			remote->create_calendar(std::string("Errands ") + path, "#FF9500");
		bool found = false;
		for (auto& c : remote->calendars()) {
			if (c.href == href) {
				found = true;
				CHECK_EQ(c.color, std::string("#FF9500"));	// alpha dropped
				CHECK(!c.ctag.empty());
			}
		}
		CHECK(found);
		remote->update_calendar(href, "Renamed", "");
		for (auto& c : remote->calendars()) {
			if (c.href == href) {
				CHECK_EQ(c.name, std::string("Renamed"));
			}
		}
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
	CHECK(other->put(href + "milk.ics", todo("milk-uid", "Milk"), "")
			  .has_value());

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
	for (auto& i : items) {
		hrefs.push_back(i.href);
	}
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
	CHECK(other->put(href + "bins.ics", todo("bins-uid", "Bins out"), "")
			  .has_value());

	SourceConfig config{"chores", "caldav", dir.path / "lists", ""};
	set_dav_settings(config, settings());
	fs::create_directories(config.folder);
	Library library;
	library.add(config, std::make_unique<Store>(config.folder,
												dir.path / "state", "caldav"));
	auto file = config.folder / "Chores.md";
	auto wait_for = [](auto&& cond) {
		for (int i = 0; i < 100 && !cond(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
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
	} catch (const SyncError&) {
		ok = false;
	}
	CHECK(ok);
	CHECK_EQ(read(cache), server_url + "/\nalice\n/dav/calendars/alice/\n");
	// Another account's cache isn't used.
	std::ofstream(cache) << server_url << "/\nbob\n/dav/old/home/\n";
	make_caldav_remote(settings(), cache)->calendars();
	CHECK_EQ(read(cache), server_url + "/\nalice\n/dav/calendars/alice/\n");
}
