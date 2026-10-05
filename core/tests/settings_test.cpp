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
	CHECK(!load_bool_setting("show-key-numbers"));
	save_setting("show-key-numbers", "Yes");
	CHECK(load_bool_setting("show-key-numbers"));
	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}
