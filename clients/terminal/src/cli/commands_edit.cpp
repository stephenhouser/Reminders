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
	auto refs = targets(
		join(a.positional), a,
		"edit needs the name of a reminder (then the fields to change)");
	bool only_lookup =
		std::ranges::all_of(a.options, [](auto& o) { return o.first == "in"; });
	if (only_lookup) {
		// No fields given: edit them all in $EDITOR.
		if (refs.size() > 1) {
			throw UsageError("edit in $EDITOR takes one reminder at a time");
		}
		auto id = refs.front().reminder->id;
		auto outcome = editfile::Outcome::Unchanged;
		if (hooks_.edit) {
			outcome = hooks_.edit(id);
		} else {
			if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
				throw UsageError(
					"edit needs fields to change (e.g. --due fri), or a "
					"terminal to open an editor in");
			}
			outcome = editfile::edit(store_, id);
		}
		switch (outcome) {
			case editfile::Outcome::Saved:
				break;
			case editfile::Outcome::Unchanged:
				if (!g_.json) {
					out_ << "No changes\n";
				}
				return 0;
			case editfile::Outcome::Reverted:
				if (!g_.json) {
					out_ << "Reverted; the reminder is as it was\n";
				}
				return 1;
		}
		report("Updated", *store_.find(id));
		return 0;
	}
	rem::ListFile* dest = a.get("list") ? &list_named(*a.get("list")) : nullptr;
	std::vector<std::string> ids;
	for (auto& ref : refs) {
		apply_fields(*ref.reminder, a);
		store_.touch(ref.reminder->id);
		ids.push_back(ref.reminder->id);
	}
	if (dest) {
		rem::move_to_list(store_, rem::outermost(store_, ids), *dest);
	}
	report_all("Updated", ids);
	return 0;
}

int App::cmd_done(const Args& a, bool done) {
	auto refs = targets(join(a.positional), a,
						done ? "done needs the name of a reminder"
							 : "undone needs the name of a reminder");
	std::vector<std::string> ids;
	for (auto& ref : refs) {
		ids.push_back(ref.reminder->id);
		store_.set_done(ref.reminder->id, done, today_);
	}
	report_all(done ? "Completed" : "Reopened", ids);
	return 0;
}

int App::cmd_move(const Args& a) {
	auto to = a.get("to");
	const char* usage =
		"move needs a reminder and a list: reminders move NAME --to LIST";
	if (!to && a.positional.empty()) {
		throw UsageError(usage);
	}
	// NAME --to LIST, or NAME LIST; without NAME, the selected reminders.
	auto name =
		to ? join(a.positional)
		   : join(std::vector(a.positional.begin(), a.positional.end() - 1));
	auto refs = targets(name, a, usage);
	auto& dest = list_named(to ? *to : a.positional.back());
	std::vector<std::string> ids;
	for (auto& ref : refs) {
		ids.push_back(ref.reminder->id);
	}
	ids = rem::outermost(store_, ids);
	rem::move_to_list(store_, ids, dest);
	if (auto section = a.get("section")) {
		rem::move_to_section_end(store_, ids, dest, *section);
	}
	report_all("Moved", ids);
	return 0;
}

int App::cmd_delete(const Args& a) {
	auto refs =
		targets(join(a.positional), a, "delete needs the name of a reminder");
	std::vector<std::string> ids;
	for (auto& r : refs) {
		ids.push_back(r.reminder->id);
	}
	ids = rem::outermost(store_, ids);
	if (!a.has("yes")) {
		if (hooks_.confirm) {
			auto question =
				ids.size() == 1
					? std::format("Delete “{}”?",
								  store_.find(ids[0])->reminder->title)
					: std::format("Delete {} reminders?", ids.size());
			if (!hooks_.confirm(question)) {
				return 1;
			}
		} else if (hooks_.interactive && isatty(STDIN_FILENO)) {
			for (auto& id : ids) {
				print_reminder(out_, *store_.find(id), st_, 0, true, today_);
			}
			out_ << "Delete this reminder? [y/N] " << std::flush;
			std::string answer;
			std::getline(std::cin, answer);
			if (term::lower(answer) != "y" && term::lower(answer) != "yes") {
				return 1;
			}
		}
	}
	auto n = rem::remove(store_, ids);
	if (!g_.json) {
		out_ << "Deleted " << n << (n == 1 ? " reminder" : " reminders")
			 << "\n";
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
		out_ << "Created " << name
			 << (store_.sources().size() > 1 ? " in " + source : "") << "\n";
	}
	return 0;
}

}  // namespace cli
