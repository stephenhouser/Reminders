// Reading and writing list files. See docs/FORMAT.md.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "reminders/model.hpp"

namespace rem {

Document parse(std::string_view text);
// With `new_ids` false, ids assigned in memory to lines that had none on disk
// are left out, which tells whether a save would change anything else.
std::string serialize(const Document& doc, bool new_ids = true);

// One reminder line's content (everything after "- [ ] "), split into fields.
// The id is returned separately because it's not part of LineFields.
LineFields parse_fields(std::string_view text, std::string* id = nullptr);
// The line content for these fields (without the "- [ ] " marker or indent).
std::string format_fields(const LineFields& f, std::string_view id);

std::optional<Date> parse_date(std::string_view s);
std::string format_date(Date d);
std::optional<TimeOfDay> parse_time(std::string_view s);
std::string format_time(TimeOfDay t);

}  // namespace rem
