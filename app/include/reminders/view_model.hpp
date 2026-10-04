// What a view contains, for both apps: the reminders of a smart list, tag
// or search, how they're grouped, and the count shown under the title. (A
// list shows its own document, section by section.)
#pragma once

#include <string>
#include <vector>

#include "reminders/library.hpp"
#include "reminders/view.hpp"

namespace rem {

// The reminders a smart list, a tag's view or a search shows, in the
// library's order; none for a list.
std::vector<Ref> view_refs(Library& library, const View& view, Date today);

// The count under a view's title: what kind, and of how many.
struct ViewCount {
		CountStyle style = CountStyle::OpenOnly;
		int total = 0, done = 0;
		std::string label() const {
			return count_label(style, total, done);
		}  // "6 Reminders / 3 Complete"
		std::string short_label() const {
			return count_short(style, total, done);
		}  // "6/3"
};
ViewCount view_count(Library& library, const View& view, Date today);

// A heading's worth of a smart view: Today and Scheduled group by due date
// (overdue first, then each day, by time), Flagged is one group without a
// heading, the rest group by list. The apps word the headings ("Overdue",
// "Tomorrow", the list's name).
struct RefGroup {
		enum Kind { None, Overdue, Day, List } kind = None;
		Date day{};				   // Day
		ListFile* list = nullptr;  // List
		std::vector<Ref> refs;
};
std::vector<RefGroup> grouped(Library& library, const View& view, Date today);

// Whether each reminder shows which list it's in (when the groups aren't
// lists: by date, or one group).
bool shows_list_name(const View& view);

}  // namespace rem
