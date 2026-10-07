#include "reminders/sources.hpp"

#include <cstdlib>
#include <fstream>

#include "reminders/backend_module.hpp"
#include "reminders/settings.hpp"
#include "stand_in_backends.hpp"
#include "test.hpp"

using namespace rem;

TEST(sources_from_settings) {
	test::register_stand_in_syncthing();
	auto dir = fs::temp_directory_path() / ("reminders-sources-" + new_id());
	setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
	fs::create_directories(dir / "config" / "reminders");
	fs::create_directories(dir / "Sync" / "Reminders");
	fs::create_directories(dir / "Notes Todo");
	fs::create_directories(dir / "Sync" / ".stfolder");	 // a Syncthing folder
	std::ofstream(dir / "config" / "reminders" / "settings.ini")
		<< "[general]\nview=today\n";

	CHECK(load_sources(Profile()).empty());
	CHECK(!default_source(Profile()));
	CHECK(!saved_folder(Profile()));

	// Choosing a folder creates the default source, named after it, with the
	// back end it needs.
	auto s = set_default_folder(Profile(), dir / "Sync" / "Reminders");
	CHECK_EQ(s.name, "reminders");
	CHECK(s.backend == "syncthing");
	CHECK_EQ(load_setting(Profile(), "default-source"), "reminders");
	CHECK_EQ(load_section_setting(Profile(), "source.reminders", "backend"),
			 "syncthing");
	CHECK(saved_folder(Profile()) == dir / "Sync" / "Reminders");

	// A second source, set by hand; the default stays.
	save_section_setting(Profile(), "source.scratch", "backend", "Local");
	save_section_setting(Profile(), "source.scratch", "folder",
						 (dir / "Notes Todo").string());
	auto all = load_sources(Profile());
	CHECK_EQ(all.size(), 2u);
	CHECK(all[1].backend == "local");
	CHECK_EQ(default_source(Profile())->name, "reminders");

	// A folder from the command line: its configured source, else detected.
	CHECK_EQ(source_for_folder(Profile(), dir / "Notes Todo").name, "scratch");
	auto other = source_for_folder(Profile(), dir);
	CHECK(other.name.empty());
	CHECK(other.backend == "local");
	CHECK(detect_backend(dir / "Sync" / "Reminders") == "syncthing");

	// Changing the folder updates the default source, back end and all.
	s = set_default_folder(Profile(), dir / "Notes Todo");
	CHECK_EQ(s.name, "reminders");
	CHECK(s.backend == "local");

	// Adding and removing sources.
	auto added = add_source(Profile(), dir / "Notes Todo");
	CHECK_EQ(added.name, "notes-todo");
	CHECK_EQ(add_source(Profile(), dir / "Notes Todo").name,
			 "notes-todo-2");  // names are unique
	remove_source(Profile(), "notes-todo-2");
	remove_source(Profile(), "scratch");
	CHECK_EQ(load_sources(Profile()).size(), 2u);
	remove_source(Profile(), added.name);
	CHECK_EQ(load_sources(Profile()).size(), 1u);
	CHECK_EQ(default_source(Profile())->name, "reminders");

	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}
