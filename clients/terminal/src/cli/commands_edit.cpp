#include "internal.hpp"

namespace cli {

int App::cmd_add(const Args& a) {
	auto text = join(a.positional);
	if (text.empty() && !a.has("title")) {
		throw UsageError("add needs the reminder's text");
	}
	rem::Reminder r;
	r.fields() = rem::parse_fields(text);
	r.created = today_;
	apply_fields(r, a);

	if (auto parent_ref = a.get("parent")) {
		auto parent = resolve(*parent_ref, a.get("in"));
		if (parent.parent) {
			throw std::runtime_error(
				"subtasks can't have subtasks of their own");
		}
		auto id = rem::new_id();
		r.id = id;
		parent.reminder->subtasks.push_back(std::move(r));
		store_.save(*parent.list);
		report("Added", *store_.find(id));
		return 0;
	}
	auto& l = a.get("list") ? list_named(*a.get("list")) : default_list();
	auto& added = store_.add(l, std::move(r), nullptr, a.get("section"));
	report("Added", *store_.find(added.id));
	return 0;
}

int App::cmd_edit(const Args& a) {
	if (a.positional.empty()) {
		throw UsageError(
			"edit needs the name of a reminder (then the fields to change)");
	}
	auto ref = resolve(join(a.positional), a.get("in"));
	auto id = ref.reminder->id;
	bool only_lookup =
		std::ranges::all_of(a.options, [](auto& o) { return o.first == "in"; });
	if (only_lookup) {
		// No fields given: edit them all in $EDITOR.
		if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
			throw UsageError(
				"edit needs fields to change (e.g. --due fri), or a terminal "
				"to open an editor in");
		}
		switch (editfile::edit(store_, id)) {
			case editfile::Outcome::Saved:
				break;
			case editfile::Outcome::Unchanged:
				if (!g_.json) {
					std::cout << "No changes\n";
				}
				return 0;
			case editfile::Outcome::Reverted:
				if (!g_.json) {
					std::cout << "Reverted; the reminder is as it was\n";
				}
				return 1;
		}
		report("Updated", *store_.find(id));
		return 0;
	}
	apply_fields(*ref.reminder, a);
	store_.touch(id);
	if (auto l = a.get("list")) {
		auto& dest = list_named(*l);
		if (&dest != ref.list) {
			store_.move_to_list(id, dest);
		}
	}
	report("Updated", *store_.find(id));
	return 0;
}

int App::cmd_done(const Args& a, bool done) {
	if (a.positional.empty()) {
		throw UsageError(done ? "done needs the name of a reminder"
							  : "undone needs the name of a reminder");
	}
	// Several names: each word is one reminder only if quoted separately, so
	// try the whole text first.
	std::vector<std::string> names =
		a.positional.size() == 1 ? a.positional
								 : std::vector<std::string>{join(a.positional)};
	for (auto& ref_text : names) {
		auto ref = resolve(ref_text, a.get("in"));
		auto id = ref.reminder->id;
		store_.set_done(id, done, today_);
		report(done ? "Completed" : "Reopened", *store_.find(id));
	}
	return 0;
}

int App::cmd_move(const Args& a) {
	auto to = a.get("to");
	if (a.positional.empty() || (!to && a.positional.size() < 2)) {
		throw UsageError(
			"move needs a reminder and a list: reminders move NAME --to LIST");
	}
	auto name =
		to ? join(a.positional)
		   : join(std::vector(a.positional.begin(), a.positional.end() - 1));
	auto ref = resolve(name, a.get("in"));
	auto id = ref.reminder->id;
	auto& dest = list_named(to ? *to : a.positional.back());
	if (&dest != ref.list) {
		store_.move_to_list(id, dest);
	}
	if (auto section = a.get("section")) {
		dest.doc.move_to_end(id, *section);
		store_.save(dest);
	}
	report("Moved", *store_.find(id));
	return 0;
}

int App::cmd_delete(const Args& a) {
	if (a.positional.empty()) {
		throw UsageError("delete needs the name of a reminder");
	}
	std::vector<rem::Ref> refs{resolve(join(a.positional), a.get("in"))};
	if (!a.has("yes") && isatty(STDIN_FILENO)) {
		for (auto& r : refs) {
			print_reminder(r, st_, 0, true, today_);
		}
		std::cout << "Delete this reminder? [y/N] " << std::flush;
		std::string answer;
		std::getline(std::cin, answer);
		if (term::lower(answer) != "y" && term::lower(answer) != "yes") {
			return 1;
		}
	}
	std::vector<std::string> ids;
	for (auto& r : refs) {
		ids.push_back(r.reminder->id);
	}
	for (auto& id : ids) {
		store_.remove(id);
	}
	if (!g_.json) {
		std::cout << "Deleted " << ids.size()
				  << (ids.size() == 1 ? " reminder" : " reminders") << "\n";
	}
	return 0;
}

int App::cmd_new_list(const Args& a) {
	auto name = join(a.positional);
	if (name.empty() || name.front() == '.' ||
		name.find_first_of("/\\<>:\"|?*") != std::string::npos ||
		name.find(".sync-conflict-") != std::string::npos) {
		throw UsageError(
			"list names can't be empty, start with a dot, or contain / \\ < > "
			": \" | ? *");
	}
	auto source = a.get("source").value_or(store_.default_source());
	if (!store_.store(source)) {
		throw std::runtime_error(std::format("no source called “{}”", source));
	}
	for (auto* l : store_.lists(source)) {
		if (term::lower(l->name) == term::lower(name)) {
			throw std::runtime_error("a list with that name already exists");
		}
	}
	auto color = a.get("color").value_or("blue");
	auto icon = a.get("icon").value_or("list");
	if (std::ranges::find(rem::kColors, color) == std::end(rem::kColors)) {
		throw UsageError(
			"colours: red orange yellow green cyan blue indigo purple pink "
			"brown gray");
	}
	if (std::ranges::find(rem::kIcons, icon) == std::end(rem::kIcons)) {
		throw UsageError("unknown icon; see docs/FORMAT.md for the names");
	}
	store_.create_list(source, name, color, icon);
	if (!g_.json) {
		std::cout << "Created " << name
				  << (store_.sources().size() > 1 ? " in " + source : "")
				  << "\n";
	}
	return 0;
}

}  // namespace cli
