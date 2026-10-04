#include "reminders/sidebar.hpp"

#include <cstdlib>
#include <fstream>

#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

#define MARK "---\nreminders: 1\n---\n"

// Sources "home" (A, B) and "work" (C), tags x and y, settings in a temporary
// XDG_CONFIG_HOME.
struct Fixture {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-sidebar-" + new_id());
		Library lib;
		Fixture() {
			setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
			fs::create_directories(dir / "config" / "reminders");
			std::ofstream(dir / "config" / "reminders" / "settings.ini")
				<< "[general]\n";
			for (auto name : {"home", "work"}) {
				fs::create_directories(dir / name);
				fs::create_directories(dir / "state" / name);
			}
			std::ofstream(dir / "home" / "A.md") << MARK
				"- [ ] One #x ^a00001\n- [x] Two ✅ 2026-10-01 ^a00002\n";
			std::ofstream(dir / "home" / "B.md")
				<< MARK "- [ ] Three #y 🚩 ^b00001\n";
			std::ofstream(dir / "work" / "C.md") << MARK "- [ ] Four ^c00001\n";
			for (auto name : {"home", "work"}) {
				lib.add(
					SourceConfig{name, BackendKind::Local, dir / name, ""},
					std::make_unique<Store>(dir / name, dir / "state" / name,
											BackendKind::Local));
			}
			lib.load_all();
		}
		~Fixture() {
			unsetenv("XDG_CONFIG_HOME");
			std::error_code ec;
			fs::remove_all(dir, ec);
		}
};

std::vector<std::string> names(const std::vector<View>& views) {
	std::vector<std::string> out;
	for (auto& v : views) {
		out.push_back(view_to_string(v));
	}
	return out;
}

}  // namespace

TEST(sidebar_groups_entries_and_counts) {
	Fixture f;
	Sidebar s(f.lib);
	auto groups = s.groups();
	CHECK_EQ(groups.size(), 4u);  // smart lists, home, work, tags
	CHECK_EQ(s.title(groups[1]), "Home");
	CHECK((names(s.entries(groups[1])) ==
		   std::vector<std::string>{"list:home/A", "list:home/B"}));
	CHECK((names(s.entries(groups[3])) ==
		   std::vector<std::string>{"tag:x", "tag:y"}));
	CHECK_EQ(s.all().size(), 6u + 3u + 2u);
	CHECK(s.home() == (View{View::Today, ""}));
	CHECK(s.count({View::List, "home/A"}, Date{}) == std::optional<int>(1));
	CHECK(s.count({View::Flagged, ""}, Date{}) == std::optional<int>(1));
	CHECK(!s.count({View::Tag, "x"}, Date{}));
}

TEST(sidebar_hiding_folding_and_moving_are_saved) {
	Fixture f;
	{
		Sidebar s(f.lib);
		s.set_hidden({View::List, "home/A"}, true);
		s.set_hidden({View::Today, ""}, true);
		CHECK(s.gone({View::List, "home/A"}));
		CHECK((names(s.entries({SidebarGroup::Lists, "home"})) ==
			   std::vector<std::string>{"list:home/B"}));
		CHECK(s.home() == (View{View::Scheduled, ""}));	 // Today is hidden
		s.set_show_hidden(true);
		CHECK(!s.gone({View::List, "home/A"}));
		CHECK_EQ(s.entries({SidebarGroup::Lists, "home"}).size(), 2u);
		// Lists move within their source only.
		CHECK(s.same_group({View::List, "home/A"}, {View::List, "home/B"}));
		CHECK(!s.same_group({View::List, "home/A"}, {View::List, "work/C"}));
		CHECK(s.move_next_to({View::List, "home/B"}, {View::List, "home/A"},
							 false));
		CHECK(!s.move_next_to({View::List, "home/B"}, {View::List, "work/C"},
							  false));
		CHECK(s.move({View::Tag, "y"}, -1));
		CHECK(!s.can_move({View::Tag, "y"}, -1));
		// Groups.
		SidebarGroup tags{SidebarGroup::Tags, {}},
			smart{SidebarGroup::SmartLists, {}};
		CHECK(s.move_group_next_to(tags, smart, false));
		s.set_foldable(tags, true);
		s.toggle_fold(tags);
		CHECK(s.folded(tags));
	}
	// A new sidebar reads it all back from settings.ini.
	Sidebar s(f.lib);
	CHECK(s.show_hidden() && s.hidden({View::List, "home/A"}) &&
		  s.hidden({View::Today, ""}));
	CHECK((names(s.entries({SidebarGroup::Lists, "home"})) ==
		   std::vector<std::string>{"list:home/B", "list:home/A"}));
	CHECK((names(s.entries({SidebarGroup::Tags, {}})) ==
		   std::vector<std::string>{"tag:y", "tag:x"}));
	CHECK(s.groups().front().kind == SidebarGroup::Tags);
	CHECK(s.folded({SidebarGroup::Tags, {}}));
	CHECK(s.all().size() == 6u + 3u);  // the folded tags left out
	CHECK(s.all(true).size() == 6u + 3u + 2u);
}
