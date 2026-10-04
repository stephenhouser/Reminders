#include "reminders/actions.hpp"

#include <algorithm>

namespace rem {

Ids outermost(Library& library, const Ids& ids) {
	Ids out;
	for (auto& id : ids) {
		auto ref = library.find(id);
		if (ref && ref->parent &&
			std::ranges::find(ids, ref->parent->id) != ids.end()) {
			continue;
		}
		out.push_back(id);
	}
	return out;
}

bool all_done(Library& library, const Ids& ids) {
	return std::ranges::all_of(ids, [&](auto& id) {
		auto ref = library.find(id);
		return !ref || ref->reminder->done;
	});
}

bool all_flagged(Library& library, const Ids& ids) {
	return std::ranges::all_of(ids, [&](auto& id) {
		auto ref = library.find(id);
		return !ref || ref->reminder->flagged;
	});
}

bool complete(Library& library, const Ids& ids, Date today) {
	bool done = !all_done(library, ids);
	for (auto& id : ids) {
		library.set_done(id, done, today);
	}
	return done;
}

bool toggle_flag(Library& library, const Ids& ids) {
	bool flag = !all_flagged(library, ids);
	for (auto& id : ids) {
		if (auto ref = library.find(id)) {
			ref->reminder->flagged = flag;
			library.touch(id);
		}
	}
	return flag;
}

void set_due(Library& library, const Ids& ids, std::optional<Date> due,
			 std::optional<TimeOfDay> time) {
	for (auto& id : ids) {
		if (auto ref = library.find(id)) {
			ref->reminder->due_date = due;
			if (!due) {
				ref->reminder->due_time.reset();
			} else if (time) {
				ref->reminder->due_time = time;
			}
			library.touch(id);
		}
	}
}

void set_priority(Library& library, const Ids& ids, Priority priority) {
	for (auto& id : ids) {
		if (auto ref = library.find(id);
			ref && ref->reminder->priority != priority) {
			ref->reminder->priority = priority;
			library.touch(id);
		}
	}
}

void set_tag(Library& library, const Ids& ids, const std::string& tag,
			 bool add) {
	for (auto& id : ids) {
		if (auto ref = library.find(id)) {
			auto& tags = ref->reminder->tags;
			bool has = std::ranges::find(tags, tag) != tags.end();
			if (add && !has) {
				tags.push_back(tag);
			} else if (!add && has) {
				std::erase(tags, tag);
			} else {
				continue;
			}
			library.touch(id);
		}
	}
}

bool move_next_to(Library& library, const Ids& ids, const std::string& target,
				  Document::Place place) {
	bool all = true;
	auto anchor = target;
	for (auto& id : ids) {
		auto ref = library.find(id);
		auto target_ref = library.find(anchor);
		if (!ref || !target_ref) {
			continue;
		}
		if (ref->list != target_ref->list) {
			library.move_to_list(id, *target_ref->list);
		}
		auto* l = library.find(anchor)->list;
		if (l->doc.move_next_to(id, anchor, place)) {
			anchor = id;
			place = Document::Place::After;
		} else {
			all = false;
		}
		library.save(*l);  // also covers a move from another list
	}
	return all;
}

void move_to_section_end(Library& library, const Ids& ids, ListFile& list,
						 const std::optional<std::string>& section) {
	for (auto& id : ids) {
		auto ref = library.find(id);
		if (!ref) {
			continue;
		}
		if (ref->list != &list) {
			library.move_to_list(id, list);
		}
		list.doc.move_to_end(id, section);
		library.save(list);
	}
}

int move_to_list(Library& library, const Ids& ids, ListFile& list) {
	int moved = 0;
	for (auto& id : outermost(library, ids)) {
		auto ref = library.find(id);
		if (!ref || ref->list == &list) {
			continue;
		}
		library.move_to_list(id, list);
		++moved;
	}
	return moved;
}

int remove(Library& library, const Ids& ids) {
	int removed = 0;
	for (auto& id : outermost(library, ids)) {
		if (library.find(id)) {
			library.remove(id);
			++removed;
		}
	}
	return removed;
}

std::string as_text(Library& library, const Ids& ids) {
	std::string text;
	for (auto& id : outermost(library, ids)) {
		if (auto ref = library.find(id)) {
			text += to_clipboard_text(*ref->reminder);
		}
	}
	return text;
}

AddedText add_text(Library& library, const std::string& text, TextSplit split,
				   const View& view, const TextPlace& where, Date today) {
	AddedText out;
	auto added = from_clipboard_text(text, split);
	if (added.empty()) {
		return out;
	}
	auto anchor = where.anchor ? library.find(*where.anchor) : std::nullopt;
	bool for_view = where.list.empty();	 // made to show in the view
	ListFile* list = !for_view				 ? library.list(where.list)
				   : anchor					 ? anchor->list
				   : view.kind == View::List ? library.list(view.name)
											 : nullptr;
	if (!list && for_view && !library.lists().empty()) {
		list = library.lists().front();
	}
	if (!list) {
		return out;
	}
	out.list = list;
	if (anchor && anchor->list != list) {
		anchor.reset();
	}
	const Reminder* next_to =
		anchor ? (anchor->parent ? anchor->parent : anchor->reminder) : nullptr;
	auto section = next_to ? list->doc.section_of(*next_to) : std::nullopt;
	bool before = next_to && where.place == Document::Place::Before;
	std::string before_id = before ? next_to->id : "";
	std::string after_id = next_to && !before ? next_to->id : "";
	for (auto& r : added) {
		if (!r.created) {
			r.created = today;
		}
		if (for_view) {
			if (!r.done && !r.due_date &&
				(view.kind == View::Today || view.kind == View::Scheduled)) {
				r.due_date = today;
			}
			if (view.kind == View::Flagged) {
				r.flagged = true;
			}
			if (view.kind == View::Tag &&
				std::ranges::find(r.tags, view.name) == r.tags.end()) {
				r.tags.push_back(view.name);
			}
		}
		if (before) {  // each just above the anchor, so they keep their order
			auto id = library
						  .add(*list, std::move(r), list->doc.find(before_id),
							   section)
						  .id;
			list->doc.move_next_to(id, before_id, Document::Place::Before);
			library.save(*list);
			out.ids.push_back(id);
		} else {
			const Reminder* at =
				after_id.empty() ? nullptr : list->doc.find(after_id);
			after_id = library.add(*list, std::move(r), at, section).id;
			out.ids.push_back(after_id);
		}
	}
	return out;
}

}  // namespace rem
