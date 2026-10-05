// CalDAV tasks (VTODO) in this app's terms, and back.
//
//   SUMMARY              title          DESCRIPTION   notes
//   STATUS / COMPLETED   done, ✅        DUE           📅 (DATE = all day)
//   PRIORITY 1–4/5/6–9   ⏫ / 🔼 / 🔽    RRULE         🔁 (the rules FORMAT.md
//   lists) CATEGORIES           #tags          URL           🔗 CREATED ➕
//   RELATED-TO    parent (subtasks) X-REMINDERS-FLAGGED  🚩 X-REMINDERS-SECTION
//   "## Section" X-APPLE-SORT-ORDER   position in the list
//
// Writing back changes only the properties whose meaning changed, so
// properties this app doesn't know (alarms, DTSTART, other clients' X-
// properties, an RRULE it can't express) survive untouched.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "reminders/ical.hpp"
#include "reminders/model.hpp"

namespace rem {

struct Todo {
		std::string uid;
		Reminder reminder;	// line fields and notes; no id or subtasks
		std::optional<std::string> parent_uid;
		std::optional<std::string> section;
		std::optional<long long> sort_order;
};

// The first VTODO in a calendar object, or null.
const IcalComponent* todo_of(const IcalComponent& calendar);
IcalComponent* todo_of(IcalComponent& calendar);

Todo read_todo(const IcalComponent& vtodo, const std::chrono::time_zone* local);

// Brings `vtodo` in line with `want` (its uid is ignored), touching only the
// properties whose meaning differs. Returns true if anything changed; then
// DTSTAMP and LAST-MODIFIED are set to `now` and SEQUENCE goes up by one.
bool write_todo(IcalComponent& vtodo, const Todo& want,
				std::chrono::sys_seconds now,
				const std::chrono::time_zone* local);

// A new calendar object (VCALENDAR) holding one VTODO for `want`.
IcalComponent new_todo_calendar(const Todo& want, std::chrono::sys_seconds now,
								const std::chrono::time_zone* local);

// RRULE ↔ repeat rule, for the rules both can express; nullopt otherwise.
std::optional<std::string> repeat_from_rrule(std::string_view rrule);
std::optional<std::string> rrule_from_repeat(std::string_view repeat);

// A CATEGORIES value as a tag ("Grocery list" → "Grocery-list"), or empty
// if nothing usable is left.
std::string tag_from_category(std::string_view category);

// The nearest of the app's colours to "#RRGGBB" (a calendar's colour), and
// a colour's "#RRGGBB".
std::string color_from_hex(std::string_view hex);
std::string hex_from_color(std::string_view color);

}  // namespace rem
