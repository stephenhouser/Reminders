// Dates as people type and read them, for the terminal front ends.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "reminders/model.hpp"

namespace rem {

// Today's date in the local time zone.
Date local_today();
// Minutes since local midnight, now.
int local_minutes_now();

// "today", "tomorrow", "yesterday", weekday names ("fri", "friday": the next
// one, never today), "+3d" / "+2w" / "+1m", or YYYY-MM-DD. Case-insensitive.
std::optional<Date> parse_human_date(std::string_view text, Date today);

// "Today", "Tomorrow", "Yesterday", "Friday" (within a week), "Oct 3",
// "Oct 3, 2027".
std::string relative_date(Date d, Date today);

}  // namespace rem
