#include "internal.hpp"

namespace cli {

int App::cmd_lists() {
	auto lists = store_.lists();
	if (g_.json) {
		out_ << "[";
		for (std::size_t i = 0; i < lists.size(); ++i) {
			int open = 0;
			lists[i]->doc.walk(
				[&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
			out_
				<< (i ? ",\n " : "")
				<< std::format(
					   R"({{"name":{},"source":{},"color":"{}","icon":"{}","open":{}}})",
					   json_escape(lists[i]->name),
					   json_escape(store_.source_of(*lists[i])->config.name),
					   lists[i]->color(), lists[i]->icon(), open);
		}
		out_ << "]\n";
		return 0;
	}
	// With several sources, under a heading each.
	bool several = store_.sources().size() > 1;
	for (auto& source : store_.sources()) {
		if (several) {
			print_heading(out_, 1, rem::source_title(source.config),
						  std::nullopt, st_);
		}
		for (auto* l : store_.lists(source.config.name)) {
			int open = 0;
			l->doc.walk(
				[&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
			out_ << st_.fg(term::color_rgb(l->color())) << l->name
				 << st_.reset() << st_.dim() << "  (" << open << " open)"
				 << st_.reset() << "\n";
		}
		if (several && &source != &store_.sources().back()) {
			out_ << "\n";
		}
	}
	if (!store_.candidates().empty() && !g_.json) {
		out_ << st_.dim() << "\nNot lists yet (no “reminders: 1”): "
			 << join(store_.candidates()) << st_.reset() << "\n";
	}
	return 0;
}

int App::cmd_list(const Args& a) {
	std::string view;
	if (!a.positional.empty()) {
		view = join(a.positional);
	} else {  // the list that last had focus
		auto v = saved_view();
		view = v.kind == "list" ? v.name
			 : v.kind == "tag"	? "#" + v.name
								: v.kind;
	}
	bool with_done = a.has("all");
	auto v = term::lower(view);

	std::vector<rem::Ref> refs;
	std::string title;
	bool by_date = false, flat = false;
	if (v == "today") {
		refs = store_.today(today_), title = "Today", by_date = true;
	} else if (v == "scheduled") {
		refs = store_.scheduled(), title = "Scheduled", by_date = true;
	} else if (v == "all") {
		title = "All";
		if (with_done) {
			for (auto* l : store_.lists()) {
				l->doc.walk([&](rem::Reminder& r, rem::Reminder* p) {
					refs.push_back({l, &r, p});
				});
			}
		} else {
			refs = store_.all();
		}
	} else if (v == "all-reminders") {	// completed too, like "all -a"
		title = "All Reminders";
		for (auto* l : store_.lists()) {
			l->doc.walk([&](rem::Reminder& r, rem::Reminder* p) {
				refs.push_back({l, &r, p});
			});
		}
	} else if (v == "flagged") {
		refs = store_.flagged(), title = "Flagged", flat = true;
	} else if (v == "completed") {
		refs = store_.completed(), title = "Completed";
	} else if (v.starts_with('#')) {
		refs = store_.tagged(view.substr(1)), title = view;
		if (!with_done) {
			std::erase_if(refs, [](auto& r) { return r.reminder->done; });
		}
	} else {
		// A list, shown with its sections.
		auto& l = list_named(view);
		if (g_.json) {
			std::vector<rem::Ref> all;
			l.doc.walk([&](rem::Reminder& r, rem::Reminder* p) {
				if (with_done || !r.done) {
					all.push_back({&l, &r, p});
				}
			});
			print_json(out_, all);
			return 0;
		}
		print_heading(out_, 1, l.name, term::color_rgb(l.color()), st_);
		int hidden = 0;
		for (auto& section : l.doc.sections()) {
			if (section.name) {
				out_ << "\n";
				print_heading(out_, 2, *section.name,
							  term::color_rgb(l.color()), st_);
			}
			for (auto* r : section.reminders) {
				if (r->done && !with_done) {
					++hidden;
					continue;
				}
				print_reminder(out_, {&l, r, nullptr}, st_, 0, false, today_);
				for (auto& s : r->subtasks) {
					if (s.done && !with_done) {
						++hidden;
						continue;
					}
					print_reminder(out_, {&l, &s, r}, st_, 2, false, today_);
				}
			}
		}
		if (hidden) {
			out_ << st_.dim() << "\n"
				 << hidden << " completed (show with -a)" << st_.reset()
				 << "\n";
		}
		return 0;
	}

	if (g_.json) {
		print_json(out_, refs);
		return 0;
	}
	print_heading(out_, 1, title, std::nullopt, st_);
	if (refs.empty()) {
		out_ << st_.dim() << "Nothing here." << st_.reset() << "\n";
		return 0;
	}
	if (by_date) {
		std::ranges::stable_sort(refs, [](auto& x, auto& y) {
			auto key = [](const rem::Ref& r) {
				auto t = r.reminder->due_time.value_or(rem::TimeOfDay{-1, 0});
				return std::pair{*r.reminder->due_date, t.hour * 60 + t.minute};
			};
			return key(x) < key(y);
		});
	}
	std::string group;
	for (auto& ref : refs) {
		std::string g;
		if (by_date) {
			g = *ref.reminder->due_date < today_
				  ? "Overdue"
				  : rem::relative_date(*ref.reminder->due_date, today_);
		} else if (!flat) {
			g = list_label(*ref.list);
		}
		if (g != group) {
			out_ << "\n";
			print_heading(
				out_, 2, g,
				by_date || flat
					? std::nullopt
					: std::optional{term::color_rgb(ref.list->color())},
				st_);
			group = g;
		}
		print_reminder(out_, ref, st_, 0, by_date || flat, today_);
	}
	return 0;
}

int App::cmd_show(const Args& a) {
	auto ref =
		targets(join(a.positional), a, "show needs the name of a reminder")
			.front();
	if (g_.json) {
		out_ << json_reminder(ref) << "\n";
		return 0;
	}
	auto& r = *ref.reminder;
	auto row = [&](const char* label, const std::string& value) {
		if (!value.empty()) {
			out_ << st_.dim() << std::format("{:<10}", label) << st_.reset()
				 << value << "\n";
		}
	};
	print_reminder(out_, ref, st_, 0, false, today_);
	out_ << "\n";
	row("list",
		list_label(*ref.list) + (ref.parent ? " > " + ref.parent->title : ""));
	row("section",
		ref.list->doc.section_of(ref.parent ? *ref.parent : r).value_or(""));
	row("status",
		r.done
			? "completed" + (r.completed ? " " + rem::format_date(*r.completed)
										 : std::string())
			: "open");
	if (r.due_date) {
		row("due", rem::format_date(*r.due_date) +
					   (r.due_time ? " " + rem::format_time(*r.due_time) : "") +
					   "  (" + term::due_label(r, today_) + ")");
	}
	row("repeat", r.repeat.value_or(""));
	row("priority", r.priority == rem::Priority::None
						? ""
						: term::priority_name(r.priority));
	row("flagged", r.flagged ? "yes" : "");
	std::string tags;
	for (auto& t : r.tags) {
		tags += (tags.empty() ? "#" : " #") + t;
	}
	row("tags", tags);
	row("url", r.url.value_or(""));
	if (!r.subtasks.empty()) {
		out_ << st_.dim() << "subtasks" << st_.reset() << "\n";
		for (auto& s : r.subtasks) {
			print_reminder(out_, {ref.list, &s, &r}, st_, 2, false, today_);
		}
	}
	return 0;
}

int App::cmd_search(const Args& a) {
	auto q = join(a.positional);
	if (q.empty()) {
		throw UsageError("search needs some text");
	}
	auto refs = store_.search(q);
	if (g_.json) {
		print_json(out_, refs);
		return 0;
	}
	if (refs.empty()) {
		out_ << st_.dim() << "No results." << st_.reset() << "\n";
		return 1;
	}
	for (auto& r : refs) {
		print_reminder(out_, r, st_, 0, true, today_);
	}
	return 0;
}

}  // namespace cli
