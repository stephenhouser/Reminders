#include "reminders/recurrence.hpp"

#include "test.hpp"

using namespace rem;
using namespace std::chrono;

namespace {
Date d(int y, unsigned m, unsigned day) {
	return Date{year{y}, month{m}, std::chrono::day{day}};
}
}  // namespace

TEST(simple_rules) {
	CHECK(next_occurrence("every day", d(2026, 12, 31)) == d(2027, 1, 1));
	CHECK(next_occurrence("every week", d(2026, 10, 1)) == d(2026, 10, 8));
	CHECK(next_occurrence("every 2 weeks", d(2026, 10, 1)) == d(2026, 10, 15));
	CHECK(next_occurrence("Every Month", d(2026, 1, 15)) == d(2026, 2, 15));
	CHECK(next_occurrence("every year", d(2026, 3, 1)) == d(2027, 3, 1));
	CHECK(next_occurrence("every 3 days", d(2026, 3, 1)) == d(2026, 3, 4));
}

TEST(month_end_clamps) {
	CHECK(next_occurrence("every month", d(2026, 1, 31)) == d(2026, 2, 28));
	CHECK(next_occurrence("every year", d(2028, 2, 29)) == d(2029, 2, 28));
	CHECK(next_occurrence("every 6 months", d(2026, 8, 31)) == d(2027, 2, 28));
}

TEST(weekdays_and_weekends) {
	// 2026-10-02 is a Friday.
	CHECK(next_occurrence("every weekday", d(2026, 10, 2)) == d(2026, 10, 5));
	CHECK(next_occurrence("every weekday", d(2026, 10, 5)) == d(2026, 10, 6));
	CHECK(next_occurrence("every weekend", d(2026, 10, 2)) == d(2026, 10, 3));
	CHECK(next_occurrence("every weekend", d(2026, 10, 4)) == d(2026, 10, 10));
}

TEST(unknown_rules) {
	CHECK(!next_occurrence("every blue moon", d(2026, 1, 1)));
	CHECK(!next_occurrence("weekly", d(2026, 1, 1)));
	CHECK(!next_occurrence("every 0 days", d(2026, 1, 1)));
	CHECK(!next_occurrence("every 2 weekdays", d(2026, 1, 1)));
}
