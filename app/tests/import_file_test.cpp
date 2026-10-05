#include "reminders/import_file.hpp"

#include <fstream>

#include "reminders/sources.hpp"
#include "test.hpp"

using namespace rem;

namespace {

#define MARK "---\nreminders: 1\n---\n"

struct Fixture {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-import-" + new_id());
		Library lib;
		Fixture() {
			for (auto s : {"home", "work", "state"}) {
				fs::create_directories(dir / s);
			}
			std::ofstream(dir / "home" / "Groceries.md")
				<< MARK "- [ ] Milk ^g00001\n";
			std::ofstream(dir / "work" / "Groceries.md") << MARK;
			std::ofstream(dir / "work" / "Plans.md") << MARK;
			for (auto s : {"home", "work"}) {
				lib.add(
					SourceConfig{s, "local", dir / s, ""},
					std::make_unique<Store>(dir / s, dir / "state", "local"));
			}
			lib.load_all();
		}
		~Fixture() {
			std::error_code ec;
			fs::remove_all(dir, ec);
		}
};

bool fails(const fs::path& path) {
	try {
		read_import_file(path);
	} catch (const std::runtime_error&) {
		return true;
	}
	return false;
}

}  // namespace

TEST(import_file_reads_and_names) {
	Fixture f;
	CHECK(fails(f.dir / "missing.txt"));
	CHECK(fails(f.dir / "home"));  // a folder
	std::ofstream(f.dir / "empty.txt") << "\n\n";
	CHECK(fails(f.dir / "empty.txt"));

	std::ofstream(f.dir / "trip.txt") << "Passport\nTickets\n";
	auto imp = read_import_file(f.dir / "trip.txt");
	CHECK_EQ(reminder_count(imp), 2);
	CHECK_EQ(import_list_name(imp, f.dir / "trip.txt"), "trip");
	imp.name = "Away/Home";
	CHECK_EQ(import_list_name(imp, f.dir / "trip.txt"), "Away-Home");
}

TEST(import_file_finds_lists) {
	Fixture f;
	CHECK_EQ(lists_called(f.lib, "groceries").size(), 2u);
	CHECK_EQ(lists_called(f.lib, "Groceries", "work").size(), 1u);
	CHECK_EQ(lists_called(f.lib, "home/groceries").size(), 1u);
	CHECK_EQ(lists_called(f.lib, "plans", "home").size(), 0u);
	CHECK_EQ(lists_called(f.lib, "Nothing").size(), 0u);

	CHECK_EQ(list_name_error(""), "Enter a name");
	CHECK(!list_name_error(".hidden").empty());
	CHECK(!list_name_error("a/b").empty());
	CHECK(!list_name_error("A.sync-conflict-1").empty());
	CHECK_EQ(list_name_error("Trip"), "");
}

TEST(import_file_into_new_and_existing_lists) {
	Fixture f;
	std::ofstream(f.dir / "trip.md")
		<< "- [ ] Passport ^t00001\n- [ ] Tickets ^t00002\n";
	auto imp = read_import_file(f.dir / "trip.md");

	// A new list in the source given.
	auto done = import_to(f.lib, nullptr, "work", "Trip", imp);
	CHECK(done.created);
	CHECK_EQ(done.key, "work/Trip");
	CHECK_EQ(done.result.added, 2);
	CHECK_EQ(import_summary(f.lib, done, imp),
			 "Imported 2 reminders (Markdown) into Trip (a new list)");

	// Again (the same ids): nothing new, so no second list is left behind.
	auto again = import_to(f.lib, nullptr, "home", "Trip", imp);
	CHECK(!again.created);
	CHECK_EQ(again.key, "");
	CHECK(!f.lib.list("home/Trip"));
	CHECK_EQ(import_summary(f.lib, again, imp),
			 "Nothing to import: all 2 reminders are already here");

	// Into a list that's there.
	std::ofstream(f.dir / "more.txt") << "Milk\nEggs\n";
	auto more = read_import_file(f.dir / "more.txt");
	auto* list = f.lib.list("home/Groceries");
	auto into = import_to(f.lib, list, "home", "Groceries", more);
	CHECK(!into.created);
	CHECK_EQ(into.result.added, 1);
	CHECK_EQ(into.result.already, 1);
	CHECK_EQ(import_summary(f.lib, into, more),
			 "Imported 1 reminder (plain text) into home/Groceries; 1 was "
			 "already there");
}
