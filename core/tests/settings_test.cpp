#include "reminders/settings.hpp"

#include <cstdlib>
#include <fstream>

#include "reminders/model.hpp"
#include "test.hpp"

using namespace rem;

TEST(settings_round_trip_keeps_other_lines) {
	auto dir = fs::temp_directory_path() / ("reminders-settings-" + new_id());
	setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
	fs::create_directories(dir / "reminders");
	std::ofstream(dir / "reminders" / "settings.ini")
		<< "# mine\n[other]\nfolder=x\n[general]\nview=all\n";
	CHECK_EQ(load_setting("folder"), "");
	CHECK_EQ(load_setting("view"), "all");
	save_setting("folder", "/tmp/lists");
	save_setting("view", "today");
	CHECK_EQ(load_setting("folder"), "/tmp/lists");
	std::ifstream in(dir / "reminders" / "settings.ini");
	std::string all((std::istreambuf_iterator<char>(in)), {});
	CHECK_EQ(
		all,
		"# "
		"mine\n[other]\nfolder=x\n[general]\nview=today\nfolder=/tmp/lists\n");
	CHECK(device_name().find('-') != std::string::npos);
	CHECK(!load_bool_setting("show-hidden"));
	save_setting("show-hidden", "Yes");
	CHECK(load_bool_setting("show-hidden"));
	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}

// The parsed file is kept between calls; a change on disk is still seen.
TEST(settings_cache_sees_changes_on_disk) {
	auto dir = fs::temp_directory_path() / ("reminders-settings-" + new_id());
	setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
	fs::create_directories(dir / "reminders");
	auto file = dir / "reminders" / "settings.ini";
	std::ofstream(file) << "[general]\nview=all\n";
	CHECK_EQ(load_setting("view"), "all");

	// Same size, written elsewhere and moved in (as another app saves).
	std::ofstream(dir / "new.ini") << "[general]\nview=day\n";
	fs::rename(dir / "new.ini", file);
	CHECK_EQ(load_setting("view"), "day");

	// Edited in place, as an editor might.
	std::ofstream(file, std::ios::trunc) << "[general]\nview=today\n";
	CHECK_EQ(load_setting("view"), "today");

	save_setting("view", "flagged");
	CHECK_EQ(load_setting("view"), "flagged");

	fs::remove(file);
	CHECK_EQ(load_setting("view"), "");

	// Another settings folder is another file.
	auto other = dir / "other";
	fs::create_directories(other / "reminders");
	std::ofstream(other / "reminders" / "settings.ini")
		<< "[general]\nview=all\n";
	setenv("XDG_CONFIG_HOME", other.c_str(), 1);
	CHECK_EQ(load_setting("view"), "all");

	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}
