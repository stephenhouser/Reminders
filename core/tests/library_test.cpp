#include "reminders/library.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "reminders/history.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

#define MARK "---\nreminders: 1\n---\n"

Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}

std::string read(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// Two local sources, "home" and "work", each with a "Todo" list.
struct TwoSources {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-library-" + new_id());
		Library lib;
		TwoSources() {
			for (auto name : {"home", "work"}) {
				fs::create_directories(dir / name);
				fs::create_directories(dir / "state" / name);
			}
			std::ofstream(dir / "home" / "Todo.md") << MARK
				"- [ ] Milk 🚩 📅 2026-10-03 ^h00001\n- [x] Eggs ✅ 2026-09-30 "
				"^h00002\n";
			std::ofstream(dir / "home" / "Garden.md")
				<< MARK "- [ ] Weed #outside 📅 2026-10-01 ^h00003\n";
			std::ofstream(dir / "work" / "Todo.md")
				<< MARK "- [ ] Report 📅 2026-10-02 #outside ^w00001\n";
			for (auto name : {"home", "work"}) {
				lib.add(SourceConfig{name, "local", dir / name, ""},
						std::make_unique<Store>(dir / name,
												dir / "state" / name, "local"));
			}
			lib.load_all();
		}
		~TwoSources() { fs::remove_all(dir); }
};

}  // namespace

TEST(library_names_lists_by_source) {
	TwoSources t;
	auto& lib = t.lib;
	CHECK_EQ(lib.lists().size(), 3u);
	CHECK_EQ(lib.lists("work").size(), 1u);
	auto* home_todo = lib.list("home/Todo");
	auto* work_todo = lib.list("work/Todo");
	CHECK(home_todo && work_todo && home_todo != work_todo);
	CHECK_EQ(lib.key_of(*work_todo), "work/Todo");
	CHECK(lib.list("Todo") == nullptr);	 // two sources have one
	CHECK(lib.list("Garden") != nullptr);
	CHECK(lib.list("nope/Todo") == nullptr);
	CHECK_EQ(lib.source_of(*home_todo)->config.name, "home");
}

TEST(library_smart_lists_span_sources) {
	TwoSources t;
	auto& lib = t.lib;
	CHECK_EQ(lib.today(d(2026, 10, 2)).size(), 2u);	 // Weed (overdue), Report
	auto sched = lib.scheduled();
	CHECK_EQ(sched.size(), 3u);
	CHECK_EQ(sched[0].reminder->title, "Weed");	 // by date across sources
	CHECK_EQ(sched[1].reminder->title, "Report");
	CHECK_EQ(sched[2].reminder->title, "Milk");
	CHECK_EQ(lib.all().size(), 3u);
	CHECK_EQ(lib.everything().size(), 4u);
	CHECK_EQ(lib.flagged().size(), 1u);
	CHECK_EQ(lib.completed().size(), 1u);
	CHECK_EQ(lib.tagged("outside").size(), 2u);
	CHECK_EQ(lib.tags().size(), 1u);
	CHECK_EQ(lib.search("e").size(), 3u);  // Weed, Report, Eggs
}

TEST(library_edits_reach_the_right_source) {
	TwoSources t;
	auto& lib = t.lib;
	lib.set_done("w00001", true, d(2026, 10, 2));
	CHECK(read(t.dir / "work" / "Todo.md").find("- [x] Report") !=
		  std::string::npos);
	CHECK(read(t.dir / "home" / "Todo.md").find("Report") == std::string::npos);

	// A new reminder's id is unique across sources, not just its folder.
	auto& r = lib.add(*lib.list("work/Todo"), Reminder{});
	CHECK(!r.id.empty());
	CHECK(lib.find(r.id)->list == lib.list("work/Todo"));

	// New list in a chosen source.
	lib.create_list("work", "Errands", "green", "cart");
	CHECK(fs::exists(t.dir / "work" / "Errands.md"));
	CHECK(lib.list("Errands") != nullptr);
}

TEST(library_moves_between_sources_with_undo) {
	TwoSources t;
	auto& lib = t.lib;
	History history;
	auto before = lib.snapshot();
	CHECK(before.contains("home/Todo"));
	lib.move_to_list("h00001", *lib.list("work/Todo"));
	history.record("Move", before, lib.snapshot());
	CHECK(lib.find("h00001")->list == lib.list("work/Todo"));
	CHECK(read(t.dir / "work" / "Todo.md")
			  .find("Milk 🚩 📅 2026-10-03 ^h00001") != std::string::npos);
	CHECK(read(t.dir / "home" / "Todo.md").find("Milk") == std::string::npos);

	auto result = history.undo(lib);
	CHECK(result.applied);
	CHECK(lib.find("h00001")->list == lib.list("home/Todo"));
	CHECK(read(t.dir / "work" / "Todo.md").find("Milk") == std::string::npos);
}

TEST(library_opens_every_configured_source) {
	auto dir =
		fs::temp_directory_path() / ("reminders-library-cfg-" + new_id());
	fs::create_directories(dir / "config" / "reminders");
	fs::create_directories(dir / "a");
	fs::create_directories(dir / "b");
	setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
	std::ofstream(dir / "config" / "reminders" / "settings.ini")
		<< "[general]\ndefault-source=b\n\n[source.a]\nbackend=local\nfolder="
		<< (dir / "a").string()
		<< "\n\n[source.gone]\nfolder=" << (dir / "missing").string()
		<< "\n\n[source.b]\nbackend=local\nfolder=" << (dir / "b").string()
		<< "\n";
	std::ofstream(dir / "a" / "One.md") << MARK "- [ ] x\n";
	std::ofstream(dir / "b" / "Two.md") << MARK "- [ ] y\n";
	auto lib = open_library(Profile(), "test-device");
	lib->load_all();
	CHECK_EQ(lib->sources().size(), 2u);  // the missing folder is left out
	CHECK_EQ(lib->sources()[0].config.name, "a");
	CHECK(lib->list("a/One") && lib->list("b/Two"));
	unsetenv("XDG_CONFIG_HOME");
	fs::remove_all(dir);
}
