#include "reminders/profile.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "reminders/model.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;

namespace {

// XDG_CONFIG_HOME, XDG_STATE_HOME, XDG_DATA_HOME and XDG_CACHE_HOME in a
// scratch folder, for one test.
struct ScratchXdg {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-profile-" + new_id());
		ScratchXdg() {
			for (auto* v : {"XDG_CONFIG_HOME", "XDG_STATE_HOME",
							"XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
				setenv(v, (dir / v).c_str(), 1);
			}
		}
		~ScratchXdg() {
			for (auto* v : {"XDG_CONFIG_HOME", "XDG_STATE_HOME",
							"XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
				unsetenv(v);
			}
			fs::remove_all(dir);
		}
};

}  // namespace

TEST(profile_names_are_checked) {
	CHECK(valid_profile_name("work"));
	CHECK(valid_profile_name("Work_2-b"));
	CHECK(!valid_profile_name(""));
	CHECK(!valid_profile_name("default"));
	CHECK(!valid_profile_name("-x"));
	CHECK(!valid_profile_name("a/b"));
	CHECK(!valid_profile_name("a b"));
	CHECK(!valid_profile_name(".."));
	CHECK(!valid_profile_name(std::string(65, 'a')));
	CHECK(Profile("").is_default());
	CHECK(Profile("default").is_default());
	CHECK_EQ(Profile().name(), "default");
	CHECK_EQ(Profile("work").name(), "work");
	CHECK(Profile("default") == Profile());
	bool threw = false;
	try {
		Profile p("../etc");
	} catch (const std::invalid_argument&) {
		threw = true;
	}
	CHECK(threw);
}

// The default profile is where everything was before profiles; a named one
// is under profiles/NAME in each XDG folder.
TEST(profile_paths) {
	ScratchXdg xdg;
	Profile def, work("work");
	CHECK_EQ(def.settings_file(), config_dir() / "settings.ini");
	CHECK_EQ(def.state_dir(), state_dir());
	CHECK_EQ(def.data_dir(), data_dir());
	CHECK_EQ(def.cache_dir(), cache_dir());
	CHECK_EQ(work.settings_file(), config_dir() / "profiles" / "work.ini");
	CHECK_EQ(work.state_dir(), state_dir() / "profiles" / "work");
	CHECK_EQ(work.data_dir(), data_dir() / "profiles" / "work");
	CHECK_EQ(work.cache_dir(), cache_dir() / "profiles" / "work");

	CHECK(def.exists());
	CHECK(!work.exists());
	CHECK(profile_names().empty());
	fs::create_directories(config_dir() / "profiles");
	std::ofstream(work.settings_file()) << "[general]\n";
	std::ofstream(config_dir() / "profiles" / "home.ini") << "[general]\n";
	std::ofstream(config_dir() / "profiles" / "notes.txt") << "";
	std::ofstream(config_dir() / "profiles" / "bad name.ini") << "";
	CHECK(work.exists());
	CHECK((profile_names() == std::vector<std::string>{"home", "work"}));
}

// Each profile reads and writes its own file, and the cache keeps them
// apart.
TEST(profile_settings_are_separate) {
	ScratchXdg xdg;
	Profile def, work("work");
	save_setting(def, "view", "today");
	save_setting(work, "view", "flagged");
	CHECK_EQ(load_setting(def, "view"), "today");
	CHECK_EQ(load_setting(work, "view"), "flagged");
	save_setting(def, "view", "all");
	CHECK_EQ(load_setting(work, "view"), "flagged");
	CHECK_EQ(load_setting(def, "view"), "all");
	std::ifstream in(work.settings_file());
	std::string all((std::istreambuf_iterator<char>(in)), {});
	CHECK_EQ(all, "[general]\nview=flagged\n");
}

// Two profiles with a source of the same name keep their records, and a
// server source's local copy, apart.
TEST(profile_sources_are_separate) {
	ScratchXdg xdg;
	Profile def, work("work");
	auto folder = xdg.dir / "lists";
	fs::create_directories(folder);
	SourceConfig mine{"personal", "local", folder, ""};
	add_source(def, mine);
	add_source(work, mine);
	auto a = load_sources(def), b = load_sources(work);
	CHECK_EQ(a.size(), 1u);
	CHECK_EQ(b.size(), 1u);
	CHECK(a[0].profile == def);
	CHECK(b[0].profile == work);
	auto device = device_name();
	CHECK_EQ(source_state_dir(a[0], device), state_dir() / device / "personal");
	CHECK_EQ(source_state_dir(b[0], device),
			 state_dir() / "profiles" / "work" / device / "personal");
	CHECK_EQ(default_copy_folder(work, "caldav", "fm"),
			 data_dir() / "profiles" / "work" / "caldav" / "fm");
	CHECK(source_for_folder(work, folder).profile == work);
	CHECK(source_for_folder(work, xdg.dir).profile == work);

	remove_source(work, "personal");
	CHECK(load_sources(work).empty());
	CHECK_EQ(load_sources(def).size(), 1u);
	CHECK_EQ(load_setting(def, "default-source"), "personal");
	CHECK_EQ(load_setting(work, "default-source"), "");
}

TEST(profile_create_and_find) {
	ScratchXdg xdg;
	auto threw = [](auto&& f) {
		try {
			f();
		} catch (const std::exception&) {
			return true;
		}
		return false;
	};
	CHECK(existing_profile("default") == Profile());
	CHECK(existing_profile("") == Profile());
	CHECK(threw([] { existing_profile("work"); }));
	CHECK(threw([] { existing_profile("a/b"); }));

	save_setting(Profile(), "view", "flagged");
	auto work = create_profile("work");
	CHECK(work.exists());
	CHECK(existing_profile("work") == work);
	CHECK_EQ(load_setting(work, "view"), "");
	auto copy = create_profile("copy", Profile());
	CHECK_EQ(load_setting(copy, "view"), "flagged");
	CHECK(threw([] { create_profile("work"); }));
	CHECK(threw([] { create_profile("default"); }));
	CHECK(threw([] { create_profile("bad name"); }));
	CHECK((profile_names() == std::vector<std::string>{"copy", "work"}));
}

TEST(profile_on_start_follows_the_setting) {
	ScratchXdg xdg;
	Profile def;
	auto start = profile_on_start();
	CHECK(start.profile == def && !start.ask);
	save_setting(def, "profile-on-start", "ask");
	CHECK(!profile_on_start().ask);	 // nothing to choose from

	auto work = create_profile("work");
	save_setting(def, "profile-on-start", "work");
	CHECK(profile_on_start().profile == work);
	save_setting(def, "profile-on-start", "gone");
	CHECK(profile_on_start().profile == def);
	save_setting(def, "profile-on-start", "Default");
	CHECK(profile_on_start().profile == def);

	save_setting(def, "profile-on-start", "last");
	CHECK(profile_on_start().profile == def);  // nothing opened yet
	remember_profile(work);
	CHECK_EQ(load_setting(def, "last-profile"), "work");
	CHECK(profile_on_start().profile == work);
	CHECK(!profile_on_start().ask);

	save_setting(def, "profile-on-start", "ask");
	start = profile_on_start();
	CHECK(start.ask && start.profile == work);
	remember_profile(def);
	CHECK(profile_on_start().profile == def);
	CHECK_EQ(load_setting(work, "last-profile"), "");  // only the default's
}
