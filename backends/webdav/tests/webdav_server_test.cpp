// The WebDAV client over HTTP against fake_dav.py.
#include <thread>

#include "dav_test.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/library.hpp"
#include "reminders/sync_runner.hpp"
#include "reminders/vtodo.hpp"
#include "reminders/webdav_client.hpp"
#include "test.hpp"

using namespace rem;
using namespace davtest;
using namespace std::chrono;

// The tests use the real back end from the registry.
static const bool registered = (register_webdav_backend(), true);

TEST(net_webdav_files) {
	// A folder that isn't there yet is made; names are escaped.
	auto remote = make_webdav_remote(
		settings("secret", "/files/alice/Reminders%20Here/"));
	CHECK(remote->files().empty());
	auto etag = remote->put("Shops & Errands",
							"---\nreminders: 1\n---\n- [ ] Milk\n", "");
	CHECK(etag.has_value() && !etag->empty());
	CHECK(!remote->put("Shops & Errands", "again", "").has_value());  // exists
	auto files = remote->files();
	CHECK_EQ(files.size(), std::size_t{1});
	CHECK_EQ(files[0].name, std::string("Shops & Errands"));
	CHECK_EQ(files[0].etag, *etag);
	auto got = remote->get("Shops & Errands");
	CHECK(got && got->text.find("- [ ] Milk") != std::string::npos);
	CHECK_EQ(got->etag, *etag);
	CHECK(!remote->get("Nothing").has_value());
	CHECK(!remote->put("Shops & Errands", "x", "\"stale\"").has_value());
	auto etag2 = remote->put("Shops & Errands", "y", *etag);
	CHECK(etag2.has_value());
	// The same folder written unescaped in url= is the same folder.
	auto same =
		make_webdav_remote(settings("secret", "/files/alice/Reminders Here"));
	CHECK_EQ(same->files().size(), std::size_t{1});
	CHECK(remote->put("Other", "z", "").has_value());
	CHECK(!remote->move("Shops & Errands", "Other"));  // never replaces
	CHECK(remote->move("Shops & Errands", "Errands"));
	CHECK(!remote->move("Shops & Errands", "Errands 2"));  // gone
	CHECK(!remote->remove("Errands", "\"stale\""));
	CHECK(remote->remove("Errands", *etag2));
	CHECK(remote->remove("Errands", ""));  // already gone
	CHECK(remote->remove("Other", ""));
}

TEST(net_webdav_folder_needs_a_parent) {
	auto remote =
		make_webdav_remote(settings("secret", "/files/alice/no/such/"));
	bool threw = false;
	try {
		remote->files();
	} catch (const SyncError& e) {
		threw = std::string_view(e.what()).find("doesn't exist") !=
				std::string_view::npos;
	}
	CHECK(threw);
}

TEST(net_webdav_two_devices) {
	Dir dir;
	auto s = settings("secret", "/files/alice/Shared/");
	auto a = make_webdav_remote(s), b = make_webdav_remote(s);
	auto fa = dir.path / "a", fb = dir.path / "b";
	std::mutex la, lb;
	fs::create_directories(fa);
	std::ofstream(fa / "Home.md")
		<< "---\nreminders: 1\n---\n- [ ] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n";
	CHECK(webdav_sync(fa, dir.path / "sa", *a, la).errors.empty());
	CHECK(webdav_sync(fb, dir.path / "sb", *b, lb).errors.empty());
	CHECK_EQ(read(fb / "Home.md"), read(fa / "Home.md"));
	// Each edits a different reminder.
	std::ofstream(fa / "Home.md")
		<< "---\nreminders: 1\n---\n- [x] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n";
	std::ofstream(fb / "Home.md")
		<< "---\nreminders: 1\n---\n- [ ] Paint ^aaaaaa\n- [x] Sweep ^bbbbbb\n";
	CHECK(webdav_sync(fa, dir.path / "sa", *a, la).errors.empty());
	CHECK(webdav_sync(fb, dir.path / "sb", *b, lb).errors.empty());
	CHECK(webdav_sync(fa, dir.path / "sa", *a, la).errors.empty());
	auto want = std::string(
		"---\nreminders: 1\n---\n- [x] Paint ^aaaaaa\n- [x] Sweep ^bbbbbb\n");
	CHECK_EQ(read(fa / "Home.md"), want);
	CHECK_EQ(read(fb / "Home.md"), want);
	a->remove("Home", "");
}

TEST(net_webdav_sync_runner) {
	Dir dir;
	auto s = settings("secret", "/files/alice/Runner/");
	auto other = make_webdav_remote(s);
	other->files();	 // makes the folder
	CHECK(other->put("Chores", "---\nreminders: 1\n---\n- [ ] Bins out\n", "")
			  .has_value());

	SourceConfig config{"cloud", "webdav", dir.path / "lists", ""};
	set_dav_settings(config, s);
	fs::create_directories(config.folder);
	Library library;
	library.add(config, std::make_unique<Store>(config.folder,
												dir.path / "state", "webdav"));
	auto file = config.folder / "Chores.md";
	auto wait_for = [](auto&& cond) {
		for (int i = 0; i < 100 && !cond(); ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return cond();
	};
	SyncRunner runner(library);
	CHECK(runner.active());
	CHECK(wait_for([&] { return fs::exists(file); }));
	std::ofstream(file, std::ios::app) << "- [ ] Sweep\n";
	CHECK(wait_for([&] {
		auto got = other->get("Chores");
		return got && got->text.find("Sweep") != std::string::npos;
	}));
	CHECK(runner.take_status().errors.empty());
	other->remove("Chores", "");
}
