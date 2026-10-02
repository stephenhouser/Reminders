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

    unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(dir);
}
