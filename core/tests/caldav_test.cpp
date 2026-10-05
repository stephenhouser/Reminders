#include "reminders/caldav.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>

#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/vtodo.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

#define MARK "---\nreminders: 1\n---\n"

std::string read(const fs::path& p) {
	std::ifstream in(p);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void write(const fs::path& p, const std::string& text) {
	std::ofstream(p) << text;
}

std::string todo(std::string_view uid, std::string_view summary,
				 std::string_view extra = "") {
	return std::format(
		"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Other//"
		"EN\r\nBEGIN:VTODO\r\nUID:{}\r\n"
		"SUMMARY:{}\r\n{}END:VTODO\r\nEND:VCALENDAR\r\n",
		uid, summary, extra);
}

// A CalDAV server in memory.
struct FakeRemote : Remote {
		struct Object {
				std::string etag, data;
		};
		struct Calendar {
				std::string name, color;
				int version = 1;
				std::map<std::string, Object> objects;	// by href
		};
		std::map<std::string, Calendar> cals;  // by href
		int next_etag = 1;
		int puts = 0, removes = 0;
		bool offline = false;
		std::function<void()> before_put;  // to change things mid-sync

		std::string add_calendar(const std::string& href,
								 const std::string& name,
								 const std::string& color = "") {
			cals[href] = Calendar{name, color, 1, {}};
			return href;
		}
		void server_put(const std::string& cal, const std::string& href,
						const std::string& data) {
			cals[cal].objects[href] = {std::format("\"e{}\"", next_etag++),
									   data};
			cals[cal].version++;
		}
		Object* find(const std::string& href) {
			for (auto& [ch, c] : cals) {
				if (auto it = c.objects.find(href); it != c.objects.end()) {
					return &it->second;
				}
			}
			return nullptr;
		}
		std::string cal_of(const std::string& href) {
			return href.substr(0, href.rfind('/') + 1);
		}
		std::optional<Todo> read_back(const std::string& href) {
			auto* o = find(href);
			if (!o) {
				return std::nullopt;
			}
			return read_todo(*todo_of(*parse_ical(o->data)),
							 locate_zone("UTC"));
		}
		std::optional<Todo> by_summary(const std::string& cal,
									   const std::string& title) {
			for (auto& [h, o] : cals[cal].objects) {
				auto t = read_todo(*todo_of(*parse_ical(o.data)),
								   locate_zone("UTC"));
				if (t.reminder.title == title) {
					return t;
				}
			}
			return std::nullopt;
		}

		std::vector<RemoteCalendar> calendars() override {
			if (offline) {
				throw SyncError("offline");
			}
			std::vector<RemoteCalendar> out;
			for (auto& [h, c] : cals) {
				out.push_back({h, c.name, c.color, std::to_string(c.version)});
			}
			return out;
		}
		std::vector<RemoteItem> items(const std::string& cal) override {
			std::vector<RemoteItem> out;
			for (auto& [h, o] : cals.at(cal).objects) {
				out.push_back({h, o.etag});
			}
			return out;
		}
		std::vector<RemoteObject> fetch(
			const std::string& cal,
			const std::vector<std::string>& hrefs) override {
			std::vector<RemoteObject> out;
			for (auto& h : hrefs) {
				if (auto it = cals.at(cal).objects.find(h);
					it != cals.at(cal).objects.end()) {
					out.push_back({h, it->second.etag, it->second.data});
				}
			}
			return out;
		}
		std::optional<std::string> put(const std::string& href,
									   const std::string& data,
									   const std::string& if_match) override {
			if (before_put) {
				std::exchange(before_put, nullptr)();
			}
			++puts;
			auto* o = find(href);
			if (if_match.empty() ? o != nullptr : (!o || o->etag != if_match)) {
				return std::nullopt;
			}
			server_put(cal_of(href), href, data);
			return find(href)->etag;
		}
		bool remove(const std::string& href, const std::string& etag) override {
			++removes;
			auto& c = cals.at(cal_of(href));
			auto it = c.objects.find(href);
			if (it == c.objects.end()) {
				return true;
			}
			if (it->second.etag != etag) {
				return false;
			}
			c.objects.erase(it);
			c.version++;
			return true;
		}
		std::string create_calendar(const std::string& name,
									const std::string& color) override {
			auto href = std::format("/cal/new{}/", cals.size());
			add_calendar(href, name, color);
			return href;
		}
		void update_calendar(const std::string& href, const std::string& name,
							 const std::string& color) override {
			cals.at(href).name = name;
			cals.at(href).color = color;
			cals.at(href).version++;
		}
		void delete_calendar(const std::string& href) override {
			cals.erase(href);
		}
};

struct Fixture {
		fs::path dir =
			fs::temp_directory_path() / ("reminders-caldav-" + new_id());
		fs::path folder = dir / "lists";
		fs::path state = dir / "state";
		FakeRemote server;
		ServerBackend backend{"caldav", state};

		Fixture() { fs::create_directories(folder); }
		~Fixture() {
			std::error_code ec;
			fs::remove_all(dir, ec);
		}
		SyncResult sync() {
			SyncOptions o;
			o.zone = locate_zone("UTC");
			o.now = sys_days{2026y / 10 / 2} + 9h;
			return caldav_sync(folder, state, server, backend.lock(), o);
		}
		Document list(const std::string& name) {
			return parse(read(folder / (name + ".md")));
		}
};

}  // namespace

TEST(caldav_pulls_new_calendar) {
	Fixture f;
	f.server.add_calendar("/cal/groc/", "Groceries", "#FF9500");
	f.server.server_put(
		"/cal/groc/", "/cal/groc/a.ics",
		todo("A-1", "Milk", "PRIORITY:1\r\nX-APPLE-SORT-ORDER:2\r\n"));
	f.server.server_put("/cal/groc/", "/cal/groc/b.ics",
						todo("B-1", "Bread", "X-APPLE-SORT-ORDER:1\r\n"));
	f.server.server_put("/cal/groc/", "/cal/groc/c.ics",
						todo("C-1", "Skim", "RELATED-TO:A-1\r\n"));
	auto r = f.sync();
	CHECK(r.errors.empty());
	auto doc = f.list("Groceries");
	CHECK(doc.is_list());
	CHECK_EQ(doc.meta("color").value_or(""), std::string("orange"));
	auto tops = doc.reminders();
	CHECK_EQ(tops.size(), std::size_t{2});
	CHECK_EQ(tops[0]->title, std::string("Bread"));	 // by sort order
	CHECK_EQ(tops[1]->title, std::string("Milk"));
	CHECK(tops[1]->priority == Priority::High);
	CHECK_EQ(tops[1]->subtasks.size(), std::size_t{1});
	// Nothing changed here, so nothing is sent back.
	CHECK_EQ(f.server.puts, 0);
	f.sync();
	CHECK_EQ(f.server.puts, 0);
}

TEST(caldav_pushes_local_edits_keeping_unknown_properties) {
	Fixture f;
	f.server.add_calendar("/cal/w/", "Work");
	f.server.server_put("/cal/w/", "/cal/w/a.ics",
						todo("A-1", "Report", "X-OTHER:keep\r\n"));
	f.sync();
	auto text = read(f.folder / "Work.md");
	auto at = text.find("Report");
	text.insert(at + 6, " 📅 2026-10-05 🚩");
	text = std::regex_replace(text, std::regex("- \\[ \\]"), "- [x]");
	write(f.folder / "Work.md", text);
	f.sync();
	auto t = f.server.read_back("/cal/w/a.ics");
	CHECK(t && t->reminder.done && t->reminder.flagged);
	CHECK((t->reminder.due_date == Date{2026y / 10 / 5}));
	CHECK(f.server.find("/cal/w/a.ics")->data.find("X-OTHER:keep") !=
		  std::string::npos);
}

TEST(caldav_merges_both_sides) {
	Fixture f;
	f.server.add_calendar("/cal/w/", "Work");
	f.server.server_put("/cal/w/", "/cal/w/a.ics", todo("A-1", "Report"));
	f.server.server_put("/cal/w/", "/cal/w/b.ics", todo("B-1", "Slides"));
	f.sync();
	// Here: flag Slides and add a reminder. There: rename Report, add one.
	auto text = read(f.folder / "Work.md");
	text.insert(text.find("Slides") + 6, " 🚩");
	text += "- [ ] Expenses\n";
	write(f.folder / "Work.md", text);
	f.server.server_put("/cal/w/", "/cal/w/a.ics",
						todo("A-1", "Quarterly report"));
	f.server.server_put("/cal/w/", "/cal/w/c.ics", todo("C-1", "Call Sam"));
	auto r = f.sync();
	CHECK(r.errors.empty());
	std::vector<std::string> titles;
	for (auto* x : f.list("Work").reminders()) {
		titles.push_back(x->title);
	}
	CHECK((titles == std::vector<std::string>{"Quarterly report", "Slides",
											  "Call Sam", "Expenses"}));
	CHECK(f.server.by_summary("/cal/w/", "Slides")->reminder.flagged);
	CHECK(f.server.by_summary("/cal/w/", "Expenses").has_value());
	// The new one has a fresh UID and its id in the file.
	auto doc = f.list("Work");
	CHECK(!doc.reminders().back()->id.empty());
}

TEST(caldav_deletions_both_ways) {
	Fixture f;
	f.server.add_calendar("/cal/w/", "Work");
	f.server.server_put("/cal/w/", "/cal/w/a.ics", todo("A-1", "Report"));
	f.server.server_put("/cal/w/", "/cal/w/b.ics", todo("B-1", "Slides"));
	f.sync();
	// Deleted there.
	f.server.cals["/cal/w/"].objects.erase("/cal/w/a.ics");
	f.server.cals["/cal/w/"].version++;
	f.sync();
	CHECK_EQ(f.list("Work").reminders().size(), std::size_t{1});
	// Deleted here.
	write(f.folder / "Work.md", MARK);
	f.sync();
	CHECK(f.server.cals["/cal/w/"].objects.empty());
}

TEST(caldav_lists_created_renamed_deleted_here) {
	Fixture f;
	write(f.folder / "Home.md",
		  "---\nreminders: 1\ncolor: green\n---\n- [ ] Mow ^mow001\n  - [ ] "
		  "Edges ^edg001\n");
	f.sync();
	CHECK_EQ(f.server.cals.size(), std::size_t{1});
	auto& [href, cal] = *f.server.cals.begin();
	CHECK_EQ(cal.name, std::string("Home"));
	CHECK_EQ(cal.color, std::string("#34C759"));
	CHECK_EQ(cal.objects.size(), std::size_t{2});
	auto mow = f.server.by_summary(href, "Mow");
	CHECK_EQ(f.server.by_summary(href, "Edges")->parent_uid.value_or(""),
			 mow->uid);

	// Renamed in the app.
	{
		std::lock_guard g(f.backend.lock());
		fs::rename(f.folder / "Home.md", f.folder / "House.md");
		f.backend.move_state("Home", "House");
	}
	f.sync();
	CHECK_EQ(f.server.cals.at(href).name, std::string("House"));
	CHECK_EQ(f.server.cals.size(), std::size_t{1});

	// A file that just went missing comes back from the server …
	fs::remove(f.folder / "House.md");
	f.sync();
	CHECK(fs::exists(f.folder / "House.md"));
	CHECK_EQ(f.list("House").reminders().size(), std::size_t{1});
	// … but one deleted in the app takes its calendar with it.
	{
		std::lock_guard g(f.backend.lock());
		fs::remove(f.folder / "House.md");
		f.backend.deleted_by_user("House");
	}
	f.sync();
	CHECK(f.server.cals.empty());
}

TEST(caldav_server_rename_and_removal) {
	Fixture f;
	f.server.add_calendar("/cal/w/", "Work");
	f.server.server_put("/cal/w/", "/cal/w/a.ics", todo("A-1", "Report"));
	f.sync();
	f.server.cals["/cal/w/"].name = "Office";
	f.server.cals["/cal/w/"].version++;
	auto r = f.sync();
	CHECK(fs::exists(f.folder / "Office.md"));
	CHECK(!fs::exists(f.folder / "Work.md"));
	CHECK(std::ranges::find(r.changed, "Work") != r.changed.end());
	// Calendar deleted on the server, list unchanged here: it goes.
	f.server.cals.erase("/cal/w/");
	f.sync();
	CHECK(!fs::exists(f.folder / "Office.md"));
}

TEST(caldav_conflicting_put_retries) {
	Fixture f;
	f.server.add_calendar("/cal/w/", "Work");
	f.server.server_put("/cal/w/", "/cal/w/a.ics", todo("A-1", "Report"));
	f.sync();
	auto text = read(f.folder / "Work.md");
	text.insert(text.find("Report") + 6, " 🚩");
	write(f.folder / "Work.md", text);
	// Someone else changes the due date just before our PUT.
	f.server.before_put = [&] {
		f.server.server_put(
			"/cal/w/", "/cal/w/a.ics",
			todo("A-1", "Report", "DUE;VALUE=DATE:20261009\r\n"));
	};
	auto r = f.sync();
	CHECK(!r.errors.empty());
	r = f.sync();
	CHECK(r.errors.empty());
	auto t = f.server.read_back("/cal/w/a.ics");
	CHECK(t->reminder.flagged);
	CHECK((t->reminder.due_date == Date{2026y / 10 / 9}));
	auto doc = f.list("Work");
	auto local = doc.reminders();
	CHECK(local[0]->flagged && local[0]->due_date == Date{2026y / 10 / 9});
}

TEST(caldav_sort_order_on_insert) {
	Fixture f;
	f.server.add_calendar("/cal/w/", "Work");
	f.server.server_put("/cal/w/", "/cal/w/a.ics",
						todo("A-1", "One", "X-APPLE-SORT-ORDER:100\r\n"));
	f.server.server_put("/cal/w/", "/cal/w/b.ics",
						todo("B-1", "Three", "X-APPLE-SORT-ORDER:200\r\n"));
	f.sync();
	auto text = read(f.folder / "Work.md");
	text.insert(text.find("- [ ] Three"), "- [ ] Two\n");
	write(f.folder / "Work.md", text);
	f.server.puts = 0;
	f.sync();
	CHECK_EQ(f.server.puts, 1);	 // only the new one
	CHECK_EQ(f.server.by_summary("/cal/w/", "Two")->sort_order.value_or(0),
			 150LL);
}

TEST(caldav_offline) {
	Fixture f;
	f.server.offline = true;
	bool threw = false;
	try {
		f.sync();
	} catch (const SyncError&) {
		threw = true;
	}
	CHECK(threw);
}

TEST(caldav_colors) {
	CHECK_EQ(color_from_hex("#FF2968"), std::string("pink"));
	CHECK_EQ(color_from_hex("#1BADF8"), std::string("cyan"));
	CHECK_EQ(color_from_hex("junk"), std::string(""));
	CHECK_EQ(color_from_hex(hex_from_color("indigo")), std::string("indigo"));
}
