// Repeat rules ("every week", "every 2 months", ...). See docs/FORMAT.md.
#pragma once

#include <optional>
#include <string_view>

#include "reminders/model.hpp"

namespace rem {

// The next due date after `from`, or nullopt if the rule isn't understood.
std::optional<Date> next_occurrence(std::string_view rule, Date from);

}  // namespace rem
