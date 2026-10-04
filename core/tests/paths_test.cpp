#include "reminders/paths.hpp"

#include <cstdlib>
#include <fstream>

#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;

namespace {

// Sets environment variables for one test and puts them back after.
struct Env {
		std::vector<std::pair<std::string, std::optional<std::string>>> saved;
		void set(const std::string& name, const char* value) {
			const char* old = std::getenv(name.c_str());
			saved.emplace_back(
				name, old ? std::optional<std::string>(old) : std::nullopt);
			if (value) {
				setenv(name.c_str(), value, 1);
			} else {
				unsetenv(name.c_str());
			}
		}
		~Env() {
			for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
				if (it->second) {
					setenv(it->first.c_str(), it->second->c_str(), 1);
				} else {
					unsetenv(it->first.c_str());
				}
			}
		}
};

}  // namespace

TEST(paths_xdg_dirs) {
	Env env;
	env.set("HOME", "/home/jo");
	for (auto v : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
				   "XDG_CACHE_HOME"}) {
		env.set(v, nullptr);
	}
	CHECK(config_dir() == fs::path("/home/jo/.config/reminders"));
	CHECK(data_dir() == fs::path("/home/jo/.local/share/reminders"));
	CHECK(state_dir() == fs::path("/home/jo/.local/state/reminders"));
	CHECK(cache_dir() == fs::path("/home/jo/.cache/reminders"));
	CHECK(settings_file() ==
		  fs::path("/home/jo/.config/reminders/settings.ini"));
	env.set("XDG_STATE_HOME", "/var/x/state");
	env.set("XDG_CACHE_HOME", "relative/cache");  // not absolute: ignored
	env.set("XDG_DATA_HOME", "");
	CHECK(state_dir() == fs::path("/var/x/state/reminders"));
	CHECK(cache_dir() == fs::path("/home/jo/.cache/reminders"));
	CHECK(data_dir() == fs::path("/home/jo/.local/share/reminders"));
}

TEST(paths_expand_and_contract) {
	Env env;
	env.set("HOME", "/home/jo");
	env.set("SYNC", "/mnt/sync");
	env.set("UNSET_FOR_TEST", nullptr);
	CHECK(expand_path("~") == fs::path("/home/jo"));
	CHECK(expand_path("~/Sync/Reminders") ==
		  fs::path("/home/jo/Sync/Reminders"));
	CHECK(expand_path("$HOME/Sync") == fs::path("/home/jo/Sync"));
	CHECK(expand_path("${HOME}/Sync") == fs::path("/home/jo/Sync"));
	CHECK(expand_path("$SYNC/Reminders") == fs::path("/mnt/sync/Reminders"));
	CHECK(expand_path("${SYNC}Lists") == fs::path("/mnt/syncLists"));
	CHECK(expand_path("Sync/Reminders") ==
		  fs::path("/home/jo/Sync/Reminders"));	 // relative: from home
	CHECK(expand_path("/srv/lists/") == fs::path("/srv/lists/"));
	CHECK(expand_path("$UNSET_FOR_TEST/x") == fs::path("/x"));
	CHECK(expand_path("/a/~b/$/c") ==
		  fs::path("/a/~b/$/c"));  // ~ only at the start; lone $
	CHECK(expand_path("~/a/../b") == fs::path("/home/jo/b"));
	CHECK(expand_path("").empty());

	CHECK_EQ(contract_path("/home/jo/Sync/Reminders"),
			 std::string("~/Sync/Reminders"));
	CHECK_EQ(contract_path("/home/jo"), std::string("~"));
	CHECK_EQ(contract_path("/home/joanne/x"), std::string("/home/joanne/x"));
	CHECK_EQ(contract_path("/srv/lists"), std::string("/srv/lists"));
	CHECK(expand_path(contract_path("/home/jo/A B/c")) ==
		  fs::path("/home/jo/A B/c"));
}

TEST(paths_in_settings_and_state) {
	auto dir = fs::temp_directory_path() / ("reminders-paths-" + new_id());
	Env env;
	env.set("HOME", (dir / "home").c_str());
	env.set("XDG_CONFIG_HOME", nullptr);
	env.set("XDG_STATE_HOME", nullptr);
	env.set("XDG_DATA_HOME", nullptr);
	fs::create_directories(dir / "home" / ".config" / "reminders");
	fs::create_directories(dir / "home" / "Lists");
	std::ofstream(dir / "home" / ".config" / "reminders" / "settings.ini")
		<< "[general]\n\n[source.mine]\nbackend=local\nfolder=~/Lists\n";

	auto sources = load_sources();
	CHECK_EQ(sources.size(), 1u);
	CHECK(sources[0].folder == dir / "home" / "Lists");
	// Saved back with ~ for the home folder.
	save_source(sources[0]);
	CHECK_EQ(load_section_setting("source.mine", "folder"),
			 std::string("~/Lists"));

	// State: per device and source, under ~/.local/state; old state in the
	// folder is moved there.
	auto device = device_name();
	auto state = source_state_dir(sources[0], device);
	CHECK(state ==
		  dir / "home" / ".local" / "state" / "reminders" / device / "mine");
	fs::create_directories(dir / "home" / "Lists" / ".reminders" / device /
						   "base");
	std::ofstream(dir / "home" / "Lists" / ".reminders" / device / "base" /
				  "Todo.md")
		<< "old base";
	open_source(sources[0], device);
	CHECK(fs::exists(state / "base" / "Todo.md"));
	CHECK(!fs::exists(dir / "home" / "Lists" / ".reminders"));

	// A folder that isn't a configured source gets one of its own.
	auto loose = source_state_dir(
		SourceConfig{"", BackendKind::Local, dir / "elsewhere"}, device);
	CHECK(loose.parent_path() == state.parent_path());
	CHECK(loose.filename().string().starts_with("folder-"));

	// A Syncthing source keeps them in its folder, as .reminders/DEVICE, and
	// gets them back from $XDG_STATE_HOME if they were moved there.
	auto st = sources[0];
	st.backend = BackendKind::Syncthing;
	CHECK(source_state_dir(st, device) ==
		  dir / "home" / "Lists" / ".reminders" / device);
	open_source(st, device);
	CHECK(fs::exists(dir / "home" / "Lists" / ".reminders" / device / "base" /
					 "Todo.md"));
	CHECK(!fs::exists(state));
	open_source(sources[0], device);  // local again: out it goes
	CHECK(fs::exists(state / "base" / "Todo.md"));

	// Removing the source removes this device's records for it.
	remove_source("mine");
	CHECK(!fs::exists(state));
	CHECK(fs::exists(dir / "home" / "Lists"));
	std::error_code ec;
	fs::remove_all(dir, ec);
}
