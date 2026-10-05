#include "reminders/actions.hpp"

#include <fstream>
#include <sstream>

#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

#define MARK "---\nreminders: 1\n---\n"

Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}

std::string body(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	auto t = ss.str();
	return t.substr(t.find("---\n", 4) + 4);
}

// Sources "home" (Groceries: Milk, Bread > Rye, Jam) and "work" (Todo).
struct Fixture {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-app-" + new_id());
		Library lib;
		Fixture() {
			for (auto name : {"home", "work"}) {
				fs::create_directories(dir / name);
				fs::create_directories(dir / "state" / name);
			}
			std::ofstream(dir / "home" / "Groceries.md") << MARK
				"- [ ] Milk ^g00001\n- [ ] Bread ^g00002\n  - [ ] Rye "
				"^g00003\n- [ ] Jam ^g00004\n";
			std::ofstream(dir / "work" / "Todo.md")
				<< MARK "- [ ] Report ^w00001\n";
			for (auto name : {"home", "work"}) {
				lib.add(SourceConfig{name, "local", dir / name, ""},
						std::make_unique<Store>(dir / name,
												dir / "state" / name, "local"));
			}
			lib.load_all();
		}
		~Fixture() {
			std::error_code ec;
			fs::remove_all(dir, ec);
		}
		std::string groceries() { return body(dir / "home" / "Groceries.md"); }
		std::string todo() { return body(dir / "work" / "Todo.md"); }
};

}  // namespace

TEST(actions_complete_and_flag_all_alike) {
	Fixture f;
	auto today = d(2026, 10, 4);
	CHECK(complete(f.lib, {"g00001", "g00004"}, today));
	CHECK(!complete(f.lib, {"g00001", "g00004"}, today));  // all were: undone
	CHECK(complete(f.lib, {"g00001"}, today));
	CHECK(
		complete(f.lib, {"g00001", "g00004"}, today));	// one wasn't: both done
	CHECK(all_done(f.lib, {"g00001", "g00004"}));
	CHECK(toggle_flag(f.lib, {"g00002", "g00004"}));
	CHECK(!toggle_flag(f.lib, {"g00002", "g00004"}));
	set_due(f.lib, {"g00002"}, d(2026, 10, 5), TimeOfDay{9, 30});
	set_due(f.lib, {"g00002"}, d(2026, 10, 6));	 // keeps the time
	CHECK(f.groceries().find("Bread 📅 2026-10-06 09:30") != std::string::npos);
	set_priority(f.lib, {"g00002"}, Priority::High);
	set_tag(f.lib, {"g00002", "g00004"}, "shop", true);
	set_tag(f.lib, {"g00004"}, "shop", false);
	CHECK(f.groceries().find("#shop") != std::string::npos);
	CHECK(f.groceries().find("Jam ✅") != std::string::npos);
}

TEST(actions_move_delete_and_copy_take_subtasks_along) {
	Fixture f;
	CHECK((outermost(f.lib, {"g00002", "g00003", "g00004"}) ==
		   Ids{"g00002", "g00004"}));
	CHECK_EQ(as_text(f.lib, {"g00002", "g00003"}),
			 "- [ ] Bread\n  - [ ] Rye\n");
	// Moved together after Report, in their order, into the other source.
	CHECK(move_next_to(f.lib, {"g00004", "g00001"}, "w00001",
					   Document::Place::After));
	CHECK_EQ(f.todo(),
			 "- [ ] Report ^w00001\n- [ ] Jam ^g00004\n- [ ] Milk ^g00001\n");
	CHECK_EQ(move_to_list(f.lib, {"g00002", "g00003", "w00001"},
						  *f.lib.list("home/Groceries")),
			 1);
	CHECK(f.groceries().find("- [ ] Report ^w00001") != std::string::npos);
	CHECK_EQ(remove(f.lib, {"g00002", "g00003"}), 1);
	CHECK(f.groceries().find("Rye") == std::string::npos);
}

TEST(actions_add_text_where_it_lands) {
	Fixture f;
	auto today = d(2026, 10, 4);
	View groceries{View::List, "home/Groceries"};
	auto above =
		add_text(f.lib, "- Apples\n- Pears", TextSplit::Auto, groceries,
				 {"", "g00002", Document::Place::Before}, today);
	CHECK_EQ(above.ids.size(), 2u);
	auto below_sub = add_text(f.lib, "Oranges", TextSplit::Auto, groceries,
							  {"", "g00003", Document::Place::After}, today);
	CHECK(f.groceries().starts_with("- [ ] Milk ^g00001\n- [ ] Apples"));
	auto g = f.groceries();
	CHECK(g.find("Pears") < g.find("Bread") &&
		  g.find("Rye") < g.find("Oranges") &&
		  g.find("Oranges") < g.find("Jam"));
	// Into a named list: no view set-up.
	add_text(f.lib, "Plan", TextSplit::Auto, View{View::Today, ""},
			 {"work/Todo", std::nullopt}, today);
	CHECK(f.todo().find("- [ ] Plan ➕") != std::string::npos);
	CHECK(f.todo().find("📅") == std::string::npos);
	// For a smart list: the first list, set up to show there.
	add_text(f.lib, "Call Sam", TextSplit::Auto, View{View::Flagged, ""}, {},
			 today);
	CHECK(f.groceries().find("Call Sam 🚩") != std::string::npos);
	add_text(f.lib, "Buy stamps", TextSplit::Auto, View{View::Tag, "errands"},
			 {}, today);
	CHECK(f.groceries().find("Buy stamps #errands") != std::string::npos);
	CHECK(add_text(f.lib, "  \n", TextSplit::Auto, groceries, {}, today)
			  .ids.empty());
}
