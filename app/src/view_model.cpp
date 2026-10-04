#include "reminders/view_model.hpp"

#include <algorithm>

namespace rem {

namespace {

bool by_date(const View& v) {
	return v.kind == View::Today || v.kind == View::Scheduled;
}

}  // namespace

std::vector<Ref> view_refs(Library& library, const View& view, Date today) {
	switch (view.kind) {
		case View::Today:
			return library.today(today);
		case View::Scheduled:
			return library.scheduled();
		case View::All:
			return library.all();
		case View::AllReminders:
			return library.everything();
		case View::Flagged:
			return library.flagged();
		case View::Completed:
			return library.completed();
		case View::Tag:
			return library.tagged(view.name);
		case View::Search:
			return library.search(view.name);
		case View::List:
			break;
	}
	return {};
}

ViewCount view_count(Library& library, const View& view, Date today) {
	ViewCount out;
	if (view.kind == View::List) {
		// Every reminder, subtasks included, and how many are done.
		out.style = CountStyle::WithComplete;
		if (auto* l = library.list(view.name)) {
			l->doc.walk([&](Reminder& r, Reminder*) {
				++out.total;
				out.done += r.done;
			});
		}
		return out;
	}
	auto refs = view_refs(library, view, today);
	out.total = static_cast<int>(refs.size());
	out.done = static_cast<int>(
		std::ranges::count_if(refs, [](auto& r) { return r.reminder->done; }));
	out.style = view.kind == View::Search	 ? CountStyle::Results
			  : view.kind == View::Completed ? CountStyle::Completed
			  : view.kind == View::Tag || view.kind == View::AllReminders
				  ? CountStyle::WithComplete
				  : CountStyle::OpenOnly;
	return out;
}

std::vector<RefGroup> grouped(Library& library, const View& view, Date today) {
	auto refs = view_refs(library, view, today);
	if (by_date(view)) {
		auto key = [](const Ref& r) {
			auto t = r.reminder->due_time.value_or(TimeOfDay{-1, 0});
			return std::pair{*r.reminder->due_date, t.hour * 60 + t.minute};
		};
		std::ranges::stable_sort(
			refs, [&](auto& a, auto& b) { return key(a) < key(b); });
	}
	std::vector<RefGroup> out;
	auto group_for = [&](RefGroup::Kind kind, Date day,
						 ListFile* list) -> RefGroup& {
		for (auto& g : out) {
			if (g.kind == kind && (kind != RefGroup::Day || g.day == day) &&
				g.list == list) {
				return g;
			}
		}
		out.push_back({kind, day, list, {}});
		return out.back();
	};
	for (auto& ref : refs) {
		if (by_date(view)) {
			auto due = *ref.reminder->due_date;
			auto& g = due < today ? group_for(RefGroup::Overdue, {}, nullptr)
								  : group_for(RefGroup::Day, due, nullptr);
			g.refs.push_back(ref);
		} else if (view.kind == View::Flagged) {
			group_for(RefGroup::None, {}, nullptr).refs.push_back(ref);
		} else {
			group_for(RefGroup::List, {}, ref.list).refs.push_back(ref);
		}
	}
	return out;
}

bool shows_list_name(const View& view) {
	return by_date(view) || view.kind == View::Flagged;
}

}  // namespace rem
