#include <cstdlib>
#include <fstream>

#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;

TEST(sources_from_settings) {
    auto dir = fs::temp_directory_path() / ("reminders-sources-" + new_id());
    setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
    fs::create_directories(dir / "config" / "reminders");
    fs::create_directories(dir / "Sync" / "Reminders");
    fs::create_directories(dir / "Notes Todo");
    fs::create_directories(dir / "Sync" / ".stfolder");  // a Syncthing folder
    std::ofstream(dir / "config" / "reminders" / "settings.ini") << "[general]\nview=today\n";

    CHECK(load_sources().empty());
    CHECK(!default_source());
    CHECK(!saved_folder());

    // Choosing a folder creates the default source, named after it, with the
    // back end it needs.
    auto s = set_default_folder(dir / "Sync" / "Reminders");
    CHECK_EQ(s.name, "reminders");
    CHECK(s.backend == BackendKind::Syncthing);
    CHECK_EQ(load_setting("default-source"), "reminders");
    CHECK_EQ(load_section_setting("source.reminders", "backend"), "syncthing");
    CHECK(saved_folder() == dir / "Sync" / "Reminders");

    // A second source, set by hand; the default stays.
    save_section_setting("source.scratch", "backend", "Local");
    save_section_setting("source.scratch", "folder", (dir / "Notes Todo").string());
    auto all = load_sources();
    CHECK_EQ(all.size(), 2u);
    CHECK(all[1].backend == BackendKind::Local);
    CHECK_EQ(default_source()->name, "reminders");

    // A folder from the command line: its configured source, else detected.
    CHECK_EQ(source_for_folder(dir / "Notes Todo").name, "scratch");
    auto other = source_for_folder(dir);
    CHECK(other.name.empty());
    CHECK(other.backend == BackendKind::Local);
    CHECK(detect_backend(dir / "Sync" / "Reminders") == BackendKind::Syncthing);

    // Changing the folder updates the default source, back end and all.
    s = set_default_folder(dir / "Notes Todo");
    CHECK_EQ(s.name, "reminders");
    CHECK(s.backend == BackendKind::Local);

    // Adding and removing sources.
    auto added = add_source(dir / "Notes Todo");
    CHECK_EQ(added.name, "notes-todo");
    CHECK_EQ(add_source(dir / "Notes Todo").name, "notes-todo-2");  // names are unique
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
    std::ofstream(dir / "config" / "reminders" / "settings.ini") << "[general]\n";

    // Named after the server; the local copy goes in the data folder.
    auto a = add_caldav_source({"https://caldav.fastmail.com/dav/", "me@example.com", "pass show fm", 15}, "");
    CHECK_EQ(a.name, "fastmail");
    CHECK(a.folder == dir / "data" / "reminders" / "caldav" / "fastmail");
    CHECK_EQ(load_setting("default-source"), "fastmail");
    // … or after its title, and made unique.
    auto b = add_caldav_source({"http://localhost:5232/", "", "", 5}, "Fastmail");
    CHECK_EQ(b.name, "fastmail-2");
    auto c = add_caldav_source({"https://nextcloud.example.org/remote.php/dav", "", "", 15}, "");
    CHECK_EQ(c.name, "example");

    auto all = load_sources();
    CHECK_EQ(all.size(), 3u);
    CHECK(all[0].backend == BackendKind::Caldav);
    CHECK(all[0].caldav == a.caldav);
    CHECK_EQ(all[1].caldav.interval, 5);
    CHECK(all[1].folder == dir / "data" / "reminders" / "caldav" / "fastmail-2");  // no folder= needed

    // `reminders folder PATH` doesn't turn the CalDAV default into a folder.
    fs::create_directories(dir / "Notes");
    auto f = set_default_folder(dir / "Notes");
    CHECK_EQ(f.name, "notes");
    CHECK(f.backend == BackendKind::Local);
    CHECK_EQ(load_setting("default-source"), "notes");
    CHECK(load_sources()[0].backend == BackendKind::Caldav);
    unsetenv("XDG_DATA_HOME");
    std::error_code ec;
    fs::remove_all(dir, ec);
}
