#include "reminders/recurrence.hpp"

#include <charconv>
#include <string>
#include <vector>

namespace rem {

namespace {

using namespace std::chrono;

Date add_months(Date d, int n) {
	year_month ym = year_month{d.year(), d.month()} + months{n};
	auto last =
		year_month_day_last{ym.year(), month_day_last{ym.month()}}.day();
	return Date{ym.year(), ym.month(), d.day() > last ? last : d.day()};
}

Date add_days(Date d, int n) { return Date{sys_days{d} + days{n}}; }

bool is_weekend(Date d) {
	auto wd = weekday{sys_days{d}};
	return wd == Saturday || wd == Sunday;
}

}  // namespace

std::optional<Date> next_occurrence(std::string_view rule, Date from) {
	std::vector<std::string> words;
	for (std::size_t i = 0; i < rule.size();) {
		while (i < rule.size() && rule[i] == ' ') {
			++i;
		}
		auto start = i;
		while (i < rule.size() && rule[i] != ' ') {
			++i;
		}
		if (i > start) {
			std::string w(rule.substr(start, i - start));
			for (auto& c : w) {
				c = static_cast<char>(
					std::tolower(static_cast<unsigned char>(c)));
			}
			words.push_back(std::move(w));
		}
	}
	if (words.empty() || words[0] != "every") {
		return std::nullopt;
	}

	int n = 1;
	std::size_t unit_at = 1;
	if (words.size() == 3) {
		auto& w = words[1];
		auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), n);
		if (ec != std::errc{} || p != w.data() + w.size() || n < 1) {
			return std::nullopt;
		}
		unit_at = 2;
	} else if (words.size() != 2) {
		return std::nullopt;
	}
	auto unit = words[unit_at];
	if (unit.size() > 1 && unit.back() == 's') {
		unit.pop_back();  // "weeks" → "week"
	}

	if (unit == "day") {
		return add_days(from, n);
	}
	if (unit == "week") {
		return add_days(from, 7 * n);
	}
	if (unit == "month") {
		return add_months(from, n);
	}
	if (unit == "year") {
		return add_months(from, 12 * n);
	}
	if (n == 1 && unit == "weekday") {
		auto d = add_days(from, 1);
		while (is_weekend(d)) {
			d = add_days(d, 1);
		}
		return d;
	}
	if (n == 1 && unit == "weekend") {
		auto d = add_days(from, 1);
		while (!is_weekend(d)) {
			d = add_days(d, 1);
		}
		return d;
	}
	return std::nullopt;
}

}  // namespace rem
