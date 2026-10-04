// iCalendar (RFC 5545) as far as CalDAV task lists need it: components and
// properties kept close to the text, so a task read from a server and
// written back keeps every property this app doesn't understand.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reminders/model.hpp"

namespace rem {

struct IcalProperty {
		std::string name;  // upper case
		std::vector<std::pair<std::string, std::string>>
			params;			// names upper case, values unquoted
		std::string value;	// as written (still escaped); see ical_text()

		std::optional<std::string> param(std::string_view name) const;
};

struct IcalComponent {
		std::string name;  // "VCALENDAR", "VTODO", …
		std::vector<IcalProperty> props;
		std::vector<IcalComponent> children;

		const IcalProperty* prop(std::string_view name) const;
		std::vector<const IcalProperty*> all(std::string_view name) const;
		// Replaces every property called `name` with one holding `value`, in
		// the first one's place (else at the end).
		void set(std::string_view name, std::string value,
				 std::vector<std::pair<std::string, std::string>> params = {});
		void remove(std::string_view name);

		IcalComponent* child(std::string_view name);
		const IcalComponent* child(std::string_view name) const;
};

// The top component (VCALENDAR) of an iCalendar object, or nullopt if it
// isn't one. Accepts LF as well as CRLF line ends.
std::optional<IcalComponent> parse_ical(std::string_view text);
// CRLF line ends, lines folded at 75 octets without splitting a UTF-8
// character.
std::string serialize_ical(const IcalComponent& c);

// TEXT values: unescaping (\n \, \; \\) and escaping.
std::string ical_text(std::string_view value);
std::string ical_escape(std::string_view text);
// A comma-separated list of TEXT values (CATEGORIES), unescaped.
std::vector<std::string> ical_text_list(std::string_view value);

// A DATE or DATE-TIME property in local time: UTC ("…Z") and TZID times are
// converted to `local`; floating times are taken as they are.
struct IcalWhen {
		Date date;
		std::optional<TimeOfDay> time;	// nullopt for a DATE
};
std::optional<IcalWhen> ical_when(const IcalProperty& p,
								  const std::chrono::time_zone* local);

// "20261003T173000Z": a UTC timestamp for DTSTAMP, LAST-MODIFIED, COMPLETED.
std::string ical_utc(std::chrono::sys_seconds t);
// "20261003" and "20261003T173000" (floating).
std::string ical_date(Date d);
std::string ical_local(Date d, TimeOfDay t);

}  // namespace rem
