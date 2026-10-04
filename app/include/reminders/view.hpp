// What a window shows: a smart list, one list, a tag's reminders or search
// results. Shared by the GNOME app and the terminal interface.
#pragma once

#include <string>
#include <string_view>

namespace rem {

struct View {
		enum Kind {
			Today,
			Scheduled,
			All,
			Flagged,
			Completed,
			AllReminders,
			List,
			Tag,
			Search
		} kind = Today;
		std::string name;  // List: its key ("source/name"); Tag: the tag;
						   // Search: the text
		bool operator==(const View&) const = default;
};

// A smart list's name in settings.ini (smart-lists=, view=): "today",
// "all-reminders", …; empty for a list, a tag or a search.
std::string_view smart_view_name(View::Kind kind);

// The view as settings.ini's view= keeps it: "today", "list:home/Todo",
// "tag:work". A search isn't kept: "today".
std::string view_to_string(const View& v);
// Back from view_to_string; anything else is Today.
View view_from_string(std::string_view s);

}  // namespace rem
