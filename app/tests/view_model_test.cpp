#include "reminders/view_model.hpp"

#include <fstream>

#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

#define MARK "---\nreminders: 1\n---\n"

Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}

struct Fixture {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-view-" + new_id());
		Library lib;
		Fixture() {
			fs::create_directories(dir / "home");
			fs::create_directories(dir / "state");
			std::ofstream(dir / "home" / "A.md") << MARK
				"- [ ] Late 📅 2026-10-01 ^a00001\n"
				"- [ ] Later today 📅 2026-10-04 17:00 🚩 ^a00002\n"
				"- [ ] Morning 📅 2026-10-04 09:00 #x ^a00003\n"
				"- [x] Done 📅 2026-10-04 ✅ 2026-10-04 ^a00004\n";
			std::ofstream(dir / "home" / "B.md") << MARK
				"- [ ] Tomorrow 📅 2026-10-05 🚩 #x ^b00001\n- [ ] Undated "
				"^b00002\n";
			lib.add(
				SourceConfig{"home", "local", dir / "home", ""},
				std::make_unique<Store>(dir / "home", dir / "state", "local"));
			lib.load_all();
		}
		~Fixture() {
			std::error_code ec;
			fs::remove_all(dir, ec);
		}
};

std::vector<std::string> titles(const RefGroup& g) {
	std::vector<std::string> out;
	for (auto& r : g.refs) {
		out.push_back(r.reminder->title);
	}
	return out;
}

}  // namespace

TEST(view_model_groups_and_counts) {
	Fixture f;
	auto today = d(2026, 10, 4);
	// Scheduled: overdue, then each day, by time.
	auto sched = grouped(f.lib, {View::Scheduled, ""}, today);
	CHECK_EQ(sched.size(), 3u);
	CHECK(sched[0].kind == RefGroup::Overdue);
	CHECK((titles(sched[1]) ==
		   std::vector<std::string>{"Morning", "Later today"}));
	CHECK(sched[2].kind == RefGroup::Day && sched[2].day == d(2026, 10, 5));
	// Flagged: one group, no heading; a tag: by list.
	auto flagged = grouped(f.lib, {View::Flagged, ""}, today);
	CHECK(flagged.size() == 1u && flagged[0].kind == RefGroup::None &&
		  flagged[0].refs.size() == 2u);
	auto tag = grouped(f.lib, {View::Tag, "x"}, today);
	CHECK(tag.size() == 2u && tag[0].kind == RefGroup::List &&
		  tag[0].list->name == "A");
	CHECK(view_refs(f.lib, {View::List, "home/A"}, today).empty());
	// Counts.
	CHECK_EQ(view_count(f.lib, {View::List, "home/A"}, today).label(),
			 "4 Reminders / 1 Complete");
	CHECK_EQ(view_count(f.lib, {View::Today, ""}, today).label(),
			 "3 Reminders");
	CHECK_EQ(view_count(f.lib, {View::Search, "late"}, today).label(),
			 "2 Results");
	CHECK(shows_list_name({View::Today, ""}) &&
		  !shows_list_name({View::Tag, "x"}));
}
