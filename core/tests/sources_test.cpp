#include "reminders/sources.hpp"

#include <cstdlib>
#include <fstream>

#include "reminders/backend_module.hpp"
#include "reminders/settings.hpp"
#include "test.hpp"

using namespace rem;

TEST(sources_from_settings) {
	auto dir = fs::temp_directory_path() / ("reminders-sources-" + new_id());
	setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
	fs::create_directories(dir / "config" / "reminders");
	fs::create_directories(dir / "Sync" / "Reminders");
	fs::create_directories(dir / "Notes Todo");
	fs::create_directories(dir / "Sync" / ".stfolder");	 // a Syncthing folder
	std::ofstream(dir / "config" / "reminders" / "settings.ini")
		<< "[general]\nview=today\n";

	CHECK(load_sources().empty());
	CHECK(!default_source());
	CHECK(!saved_folder());

	// Choosing a folder creates the default source, named after it, with the
	// back end it needs.
	auto s = set_default_folder(dir / "Sync" / "Reminders");
	CHECK_EQ(s.name, "reminders");
	CHECK(s.backend == "syncthing");
	CHECK_EQ(load_setting("default-source"), "reminders");
	CHECK_EQ(load_section_setting("source.reminders", "backend"), "syncthing");
	CHECK(saved_folder() == dir / "Sync" / "Reminders");

	// A second source, set by hand; the default stays.
	save_section_setting("source.scratch", "backend", "Local");
	save_section_setting("source.scratch", "folder",
						 (dir / "Notes Todo").string());
	auto all = load_sources();
	CHECK_EQ(all.size(), 2u);
	CHECK(all[1].backend == "local");
	CHECK_EQ(default_source()->name, "reminders");

	// A folder from the command line: its configured source, else detected.
	CHECK_EQ(source_for_folder(dir / "Notes Todo").name, "scratch");
	auto other = source_for_folder(dir);
	CHECK(other.name.empty());
	CHECK(other.backend == "local");
	CHECK(detect_backend(dir / "Sync" / "Reminders") == "syncthing");

	// Changing the folder updates the default source, back end and all.
	s = set_default_folder(dir / "Notes Todo");
	CHECK_EQ(s.name, "reminders");
	CHECK(s.backend == "local");

	// Adding and removing sources.
	auto added = add_source(dir / "Notes Todo");
	CHECK_EQ(added.name, "notes-todo");
	CHECK_EQ(add_source(dir / "Notes Todo").name,
			 "notes-todo-2");  // names are unique
	remove_source("notes-todo-2");
	remove_source("scratch");
	CHECK_EQ(load_sources().size(), 2u);
	remove_source(added.name);
	CHECK_EQ(load_sources().size(), 1u);
	CHECK_EQ(default_source()->name, "reminders");

	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}

TEST(sources_caldav) {
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
		return add_source(dav_source("caldav", std::move(c), std::move(title)));
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
	CHECK_EQ(load_setting("default-source"), "fastmail");
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

	auto all = load_sources();
	CHECK_EQ(all.size(), 3u);
	CHECK(all[0].backend == "caldav");
	CHECK(dav_settings(all[0]) == dav_settings(a));
	CHECK_EQ(dav_settings(all[1]).interval, 5);
	CHECK(all[1].folder == dir / "data" / "reminders" / "caldav" /
							   "fastmail-2");  // no folder= needed

	// `reminders folder PATH` doesn't turn the CalDAV default into a folder.
	fs::create_directories(dir / "Notes");
	auto f = set_default_folder(dir / "Notes");
	CHECK_EQ(f.name, "notes");
	CHECK(f.backend == "local");
	CHECK_EQ(load_setting("default-source"), "notes");
	CHECK(load_sources()[0].backend == "caldav");
	unsetenv("XDG_DATA_HOME");
	std::error_code ec;
	fs::remove_all(dir, ec);
}

TEST(sources_webdav) {
	auto dir = fs::temp_directory_path() / ("reminders-sources-" + new_id());
	setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
	setenv("XDG_DATA_HOME", (dir / "data").c_str(), 1);
	fs::create_directories(dir / "config" / "reminders");
	std::ofstream(dir / "config" / "reminders" / "settings.ini")
		<< "[general]\n";

	DavSettings s{
		"https://cloud.example.com/remote.php/dav/files/me/Reminders/", "me",
		"pass show cloud", 10};
	SourceConfig source{"", "webdav", {}, ""};
	set_dav_settings(source, s);
	auto a = add_source(source);
	CHECK_EQ(a.name, "example");
	CHECK(a.folder == dir / "data" / "reminders" / "webdav" / "example");
	auto all = load_sources();
	CHECK_EQ(all.size(), 1u);
	CHECK(all[0].backend == "webdav");
	CHECK(dav_settings(all[0]) == s);
	CHECK(all[0].folder == a.folder);
	CHECK(find_backend("webdav")->has_server &&
		  find_backend("caldav")->has_server &&
		  !find_backend("local")->has_server);

	// Removing it can keep the local copy in the default place...
	fs::create_directories(a.folder);
	remove_source(a.name, true);
	CHECK(fs::exists(a.folder));
	CHECK(load_sources().empty());
	// ...or, by default, removes it too.
	auto b = add_source(source);
	fs::create_directories(b.folder);
	remove_source(b.name);
	CHECK(!fs::exists(b.folder));
	unsetenv("XDG_DATA_HOME");
	std::error_code ec;
	fs::remove_all(dir, ec);
}
