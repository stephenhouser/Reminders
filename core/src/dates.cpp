#include "reminders/dates.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <format>

#include "reminders/format.hpp"
#include "reminders/recurrence.hpp"

namespace rem {

using namespace std::chrono;

namespace {

constexpr std::array<std::string_view, 7> kWeekdays = {
	"sunday",	"monday", "tuesday", "wednesday",
	"thursday", "friday", "saturday"};
constexpr std::array<std::string_view, 12> kMonths = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

local_time<seconds> local_now() {
	return floor<seconds>(current_zone()->to_local(system_clock::now()));
}

}  // namespace

Date local_today() { return Date{floor<days>(local_now())}; }

int local_minutes_now() {
	auto now = local_now();
	return static_cast<int>(
		duration_cast<minutes>(now - floor<days>(now)).count());
}

std::optional<Date> parse_human_date(std::string_view text, Date today) {
	std::string t;
	for (char c : text) {
		t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (t == "today") {
		return today;
	}
	if (t == "tomorrow") {
		return Date{sys_days{today} + days{1}};
	}
	if (t == "yesterday") {
		return Date{sys_days{today} - days{1}};
	}
	if (auto d = parse_date(t)) {
		return d;
	}

	// +3d, +2w, +1m, +1y
	if (t.size() >= 3 && t[0] == '+') {
		int n = 0;
		auto [p, ec] =
			std::from_chars(t.data() + 1, t.data() + t.size() - 1, n);
		if (ec == std::errc{} && p == t.data() + t.size() - 1 && n >= 0) {
			switch (t.back()) {
				case 'd':
					return Date{sys_days{today} + days{n}};
				case 'w':
					return Date{sys_days{today} + days{7 * n}};
				case 'm':
					return n == 0
							 ? std::optional{today}
							 : next_occurrence(
								   std::format("every {} months", n), today);
				case 'y':
					return n == 0
							 ? std::optional{today}
							 : next_occurrence(std::format("every {} years", n),
											   today);
			}
		}
		return std::nullopt;
	}

	// Weekday names, at least 3 letters: the next such day after today.
	if (t.size() >= 3) {
		for (unsigned i = 0; i < kWeekdays.size(); ++i) {
			if (kWeekdays[i].starts_with(t)) {
				auto from = weekday{sys_days{today}}.c_encoding();
				auto ahead = (i + 7 - from) % 7;
				return Date{sys_days{today} + days{ahead == 0 ? 7 : ahead}};
			}
		}
	}
	return std::nullopt;
}

std::string relative_date(Date d, Date today) {
	auto diff = (sys_days{d} - sys_days{today}).count();
	if (diff == 0) {
		return "Today";
	}
	if (diff == 1) {
		return "Tomorrow";
	}
	if (diff == -1) {
		return "Yesterday";
	}
	if (diff > 1 && diff < 7) {
		auto name = std::string(kWeekdays[weekday{sys_days{d}}.c_encoding()]);
		name[0] = static_cast<char>(
			std::toupper(static_cast<unsigned char>(name[0])));
		return name;
	}
	auto month = kMonths[static_cast<unsigned>(d.month()) - 1];
	auto day = static_cast<unsigned>(d.day());
	if (d.year() == today.year()) {
		return std::format("{} {}", month, day);
	}
	return std::format("{} {}, {}", month, day, static_cast<int>(d.year()));
}

}  // namespace rem
