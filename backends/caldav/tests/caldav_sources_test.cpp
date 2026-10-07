// CalDAV sources in settings.ini: naming, the default local copy.
#include <cstdlib>
#include <fstream>

#include "reminders/backend_module.hpp"
#include "reminders/caldav_client.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;

TEST(sources_caldav) {
	register_caldav_backend();
	auto dir = fs::temp_directory_path() / ("reminders-sources-" + new_id());
	setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
	setenv("XDG_DATA_HOME", (dir / "data").c_str(), 1);
	fs::create_directories(dir / "config" / "reminders");
	std::ofstream(dir / "config" / "reminders" / "settings.ini")
		<< "[general]\n";

	// Named after the server; the local copy goes in the data folder.
	auto dav_source = [](std::string backend, DavSettings c,
						 std::string title) {
		SourceConfig s{"", std::move(backend), {}, std::move(title)};
		set_dav_settings(s, c);
		return s;
	};
	auto caldav = [&](DavSettings c, std::string title) {
		return add_source(Profile(),
						  dav_source("caldav", std::move(c), std::move(title)));
	};
	CHECK_EQ(
		new_source_name(dav_source(
			"caldav", {"https://caldav.fastmail.com/dav/", "", "", 15}, "")),
		"fastmail");
	auto a = caldav({"https://caldav.fastmail.com/dav/", "me@example.com",
					 "pass show fm", 15},
					"");
	CHECK_EQ(a.name, "fastmail");
	CHECK(a.folder == dir / "data" / "reminders" / "caldav" / "fastmail");
	CHECK_EQ(load_setting(Profile(), "default-source"), "fastmail");
	// … or after its title, and made unique.
	auto b = caldav({"http://localhost:5232/", "", "", 5}, "Fastmail");
	CHECK_EQ(b.name, "fastmail-2");
	auto c = caldav(
		{"https://nextcloud.example.org/remote.php/dav", "", "", 15}, "");
	CHECK_EQ(new_source_name(SourceConfig{"", "local", dir / "Home Lists", ""}),
			 "home-lists");
	CHECK_EQ(new_source_name(SourceConfig{"", "local", dir / "x", "My Stuff"}),
			 "my-stuff");
	CHECK_EQ(c.name, "example");

	auto all = load_sources(Profile());
	CHECK_EQ(all.size(), 3u);
	CHECK(all[0].backend == "caldav");
	CHECK(dav_settings(all[0]) == dav_settings(a));
	CHECK_EQ(dav_settings(all[1]).interval, 5);
	CHECK(all[1].folder == dir / "data" / "reminders" / "caldav" /
							   "fastmail-2");  // no folder= needed

	// `reminders folder PATH` doesn't turn the CalDAV default into a folder.
	fs::create_directories(dir / "Notes");
	auto f = set_default_folder(Profile(), dir / "Notes");
	CHECK_EQ(f.name, "notes");
	CHECK(f.backend == "local");
	CHECK_EQ(load_setting(Profile(), "default-source"), "notes");
	CHECK(load_sources(Profile())[0].backend == "caldav");
	unsetenv("XDG_DATA_HOME");
	std::error_code ec;
	fs::remove_all(dir, ec);
}
