#include "reminders/vtodo.hpp"

#include "reminders/ical.hpp"
#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {

Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}

const time_zone* ny() { return locate_zone("America/New_York"); }

sys_seconds at(Date date, int h, int m) {
	return sys_days{date} + hours{h} + minutes{m};
}

constexpr std::string_view kTodo =
	"BEGIN:VCALENDAR\r\n"
	"VERSION:2.0\r\n"
	"PRODID:-//Other//Client//EN\r\n"
	"BEGIN:VTODO\r\n"
	"UID:4F1C-77AB@example.com\r\n"
	"DTSTAMP:20261001T120000Z\r\n"
	"SUMMARY:Milk\\, eggs\r\n"
	"DESCRIPTION:2% if they have it\\nor whole\r\n"
	"DUE;TZID=America/New_York:20261003T173000\r\n"
	"PRIORITY:2\r\n"
	"CATEGORIES:errands,Grocery list\r\n"
	"X-OTHER-CLIENT:keep me\r\n"
	"RELATED-TO;RELTYPE=PARENT:PARENT-1\r\n"
	"X-APPLE-SORT-ORDER:12\r\n"
	"BEGIN:VALARM\r\n"
	"ACTION:DISPLAY\r\n"
	"TRIGGER:-PT15M\r\n"
	"END:VALARM\r\n"
	"END:VTODO\r\n"
	"END:VCALENDAR\r\n";

}  // namespace

TEST(ical_round_trip) {
	auto cal = parse_ical(kTodo);
	CHECK(cal.has_value());
	CHECK_EQ(serialize_ical(*cal), std::string(kTodo));
	auto* todo = todo_of(*cal);
	CHECK(todo != nullptr);
	CHECK_EQ(todo->children.size(), std::size_t{1});
	CHECK_EQ(todo->prop("DUE")->param("tzid").value_or(""),
			 std::string("America/New_York"));
}

TEST(ical_folding) {
	IcalComponent c{"VCALENDAR", {}, {}};
	std::string long_text(100, 'x');
	long_text += "é";  // two bytes: must not be split
	long_text += std::string(60, 'y');
	c.set("X-LONG", long_text);
	auto text = serialize_ical(c);
	for (std::size_t i = 0, nl;
		 (nl = text.find("\r\n", i)) != std::string::npos; i = nl + 2) {
		CHECK(nl - i <= 75);
	}
	// LF-only input with folding works too.
	auto back = parse_ical("BEGIN:VCALENDAR\nX-A:one\n  two\nEND:VCALENDAR\n");
	CHECK_EQ(back->prop("X-A")->value, std::string("one two"));
	auto again = parse_ical(text);
	CHECK_EQ(again->prop("X-LONG")->value, long_text);
}

TEST(ical_text_escapes) {
	CHECK_EQ(ical_text("a\\, b\\; c\\\\ d\\ne"), std::string("a, b; c\\ d\ne"));
	CHECK_EQ(ical_escape("a, b; c\\ d\ne"),
			 std::string("a\\, b\\; c\\\\ d\\ne"));
	CHECK((ical_text_list("one,two\\,three,") ==
		   std::vector<std::string>{"one", "two,three"}));
}

TEST(ical_times) {
	IcalProperty utc{"DUE", {}, "20261003T213000Z"};
	auto w = ical_when(utc, ny());
	CHECK((w && w->date == d(2026, 10, 3) && w->time == TimeOfDay{17, 30}));
	IcalProperty date{"DUE", {{"VALUE", "DATE"}}, "20261003"};
	w = ical_when(date, ny());
	CHECK(w && w->date == d(2026, 10, 3) && !w->time);
	IcalProperty paris{"DUE", {{"TZID", "Europe/Paris"}}, "20261004T010000"};
	w = ical_when(paris, ny());
	CHECK((w && w->date == d(2026, 10, 3) && w->time == TimeOfDay{19, 0}));
	IcalProperty odd{"DUE", {{"TZID", "My Server Zone"}}, "20261004T010000"};
	w = ical_when(odd, ny());
	CHECK((w && w->date == d(2026, 10, 4) && w->time == TimeOfDay{1, 0}));
	CHECK_EQ(ical_utc(at(d(2026, 1, 2), 3, 4)),
			 std::string("20260102T030400Z"));
}

TEST(vtodo_read) {
	auto cal = parse_ical(kTodo);
	auto t = read_todo(*todo_of(*cal), ny());
	CHECK_EQ(t.uid, std::string("4F1C-77AB@example.com"));
	CHECK_EQ(t.reminder.title, std::string("Milk, eggs"));
	CHECK_EQ(t.reminder.notes, std::string("2% if they have it\nor whole"));
	CHECK((t.reminder.due_date == d(2026, 10, 3) &&
		   t.reminder.due_time == TimeOfDay{17, 30}));
	CHECK(t.reminder.priority == Priority::High);
	CHECK((t.reminder.tags ==
		   std::vector<std::string>{"errands", "Grocery-list"}));
	CHECK(!t.reminder.done);
	CHECK_EQ(t.parent_uid.value_or(""), std::string("PARENT-1"));
	CHECK_EQ(t.sort_order.value_or(0), 12LL);
}

TEST(vtodo_write_keeps_unknown_properties) {
	auto cal = *parse_ical(kTodo);
	auto& todo = *todo_of(cal);
	auto want = read_todo(todo, ny());
	// Unchanged: nothing is touched.
	CHECK(!write_todo(todo, want, at(d(2026, 10, 2), 9, 0), ny()));
	CHECK_EQ(serialize_ical(cal), std::string(kTodo));

	want.reminder.done = true;
	want.reminder.completed = d(2026, 10, 2);
	want.reminder.flagged = true;
	CHECK(write_todo(todo, want, at(d(2026, 10, 2), 9, 0), ny()));
	CHECK_EQ(todo.prop("STATUS")->value, std::string("COMPLETED"));
	CHECK_EQ(todo.prop("COMPLETED")->value,
			 std::string("20261002T160000Z"));	// local noon
	CHECK_EQ(todo.prop("X-REMINDERS-FLAGGED")->value, std::string("TRUE"));
	CHECK_EQ(todo.prop("SEQUENCE")->value, std::string("0"));
	CHECK_EQ(todo.prop("X-OTHER-CLIENT")->value, std::string("keep me"));
	CHECK_EQ(todo.prop("DUE")->param("TZID").value_or(""),
			 std::string("America/New_York"));
	CHECK(todo.child("VALARM") != nullptr);
	CHECK(read_todo(todo, ny()).reminder.done);
}

TEST(vtodo_new_and_back) {
	Todo t;
	t.uid = "abc";
	t.reminder.title = "Pay rent";
	t.reminder.notes = "by cheque";
	t.reminder.due_date = d(2026, 10, 31);
	t.reminder.repeat = "every month";
	t.reminder.tags = {"home"};
	t.reminder.priority = Priority::Low;
	t.reminder.url = "https://example.com/a,b";
	t.section = "Bills";
	t.sort_order = 3;
	t.parent_uid = "p1";
	auto cal = new_todo_calendar(t, at(d(2026, 10, 2), 9, 0), ny());
	auto back = read_todo(*todo_of(*parse_ical(serialize_ical(cal))), ny());
	CHECK_EQ(back.uid, std::string("abc"));
	CHECK(back.reminder.fields() == t.reminder.fields());
	CHECK_EQ(back.reminder.notes, t.reminder.notes);
	CHECK(back.section == t.section);
	CHECK(back.sort_order == t.sort_order);
	CHECK(back.parent_uid == t.parent_uid);
	CHECK_EQ(todo_of(cal)->prop("DUE")->param("VALUE").value_or(""),
			 std::string("DATE"));
}

TEST(vtodo_rrules) {
	CHECK_EQ(repeat_from_rrule("FREQ=WEEKLY").value_or(""),
			 std::string("every week"));
	CHECK_EQ(repeat_from_rrule("FREQ=DAILY;INTERVAL=3").value_or(""),
			 std::string("every 3 days"));
	CHECK_EQ(repeat_from_rrule("FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR").value_or(""),
			 std::string("every weekday"));
	CHECK(!repeat_from_rrule("FREQ=MONTHLY;BYMONTHDAY=15"));
	CHECK(!repeat_from_rrule("FREQ=DAILY;COUNT=3"));
	CHECK_EQ(rrule_from_repeat("every 2 weeks").value_or(""),
			 std::string("FREQ=WEEKLY;INTERVAL=2"));
	CHECK_EQ(rrule_from_repeat("every year").value_or(""),
			 std::string("FREQ=YEARLY"));
	CHECK(!rrule_from_repeat("every blue moon"));

	// A rule this app can't express survives an edit of another field.
	auto cal = *parse_ical(
		"BEGIN:VCALENDAR\r\nBEGIN:VTODO\r\nUID:x\r\nSUMMARY:Bins\r\n"
		"RRULE:FREQ=MONTHLY;BYDAY=1MO\r\nEND:VTODO\r\nEND:VCALENDAR\r\n");
	auto want = read_todo(*todo_of(cal), ny());
	CHECK(!want.reminder.repeat);
	want.reminder.title = "Bins out";
	write_todo(*todo_of(cal), want, at(d(2026, 10, 2), 9, 0), ny());
	CHECK_EQ(todo_of(cal)->prop("RRULE")->value,
			 std::string("FREQ=MONTHLY;BYDAY=1MO"));
}
