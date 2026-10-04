#include "reminders/syncthing.hpp"

#include <fstream>
#include <sstream>

#include "reminders/model.hpp"
#include "test.hpp"

using namespace rem;

namespace {

struct Dir {
		fs::path path;
		Dir() {
			path = fs::temp_directory_path() / ("reminders-st-" + new_id());
			fs::create_directories(path);
		}
		~Dir() { fs::remove_all(path); }
};

std::string read(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

}  // namespace

TEST(no_syncthing_folder_no_changes) {
	Dir d;
	CHECK(!syncthing_root(d.path));
	CHECK(!ignore_state_in_syncthing(d.path));
	CHECK(!fs::exists(d.path / ".stignore"));
}

TEST(adds_ignore_at_the_syncthing_root_once) {
	Dir d;
	fs::create_directories(d.path / ".stfolder");
	fs::create_directories(d.path / "Notes" / "Reminders");
	{
		std::ofstream(d.path / ".stignore") << "*.bak";
	}  // no trailing newline

	auto lists = d.path / "Notes" / "Reminders";
	CHECK(syncthing_root(lists) == fs::weakly_canonical(d.path));
	CHECK(ignore_state_in_syncthing(lists));
	CHECK_EQ(read(d.path / ".stignore"),
			 "*.bak\n// Reminders app: per-device state, not to be "
			 "synced\n(?d).reminders\n");
	CHECK(!ignore_state_in_syncthing(lists));  // already there
	CHECK(!fs::exists(
		lists /
		".stignore"));	// a subfolder's .stignore would be ignored by Syncthing
}

TEST(existing_ignore_line_is_respected) {
	Dir d;
	fs::create_directories(d.path / ".stfolder");
	{
		std::ofstream(d.path / ".stignore") << ".reminders\r\n";
	}
	CHECK(!ignore_state_in_syncthing(d.path));
	CHECK_EQ(read(d.path / ".stignore"), ".reminders\r\n");
}

TEST(state_dir_is_inside_the_folder) {
	CHECK(state_dir("/x/Notes", "laptop-3f9a") ==
		  fs::path("/x/Notes/.reminders/laptop-3f9a"));
}
