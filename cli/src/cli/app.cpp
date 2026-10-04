#include "internal.hpp"

namespace cli {

// A list by name ("Groceries", any case) or, when two sources have one of
// that name, "source/name".
rem::ListFile& App::list_named(const std::string& name) {
	if (auto* l = store_.list(name)) {
		return *l;
	}
	std::vector<rem::ListFile*> found;
	for (auto* l : store_.lists()) {
		if (term::lower(l->name) == term::lower(name) ||
			term::lower(store_.key_of(*l)) == term::lower(name)) {
			found.push_back(l);
		}
	}
	if (found.size() == 1) {
		return *found.front();
	}
	if (found.size() > 1) {
		std::string keys;
		for (auto* l : found) {
			keys += (keys.empty() ? "" : ", ") + store_.key_of(*l);
		}
		throw std::runtime_error(std::format(
			"more than one list called “{}”: say which ({})", name, keys));
	}
	throw std::runtime_error(std::format("no list called “{}”", name));
}

// The list or view that last had focus (in the GUI or TUI), if it still exists.
term::SavedView App::saved_view() {
	if (!own_folder_) {
		return {"today", ""};
	}
	auto v = term::parse_view_setting(rem::load_setting("view"));
	if (v.kind == "list" && !store_.list(v.name)) {
		return {"today", ""};
	}
	return v;
}

// For `add` without --list: the list that last had focus, else the first one.
rem::ListFile& App::default_list() {
	if (auto v = saved_view(); v.kind == "list") {
		return *store_.list(v.name);
	}
	auto lists = store_.lists();
	if (lists.empty()) {
		throw std::runtime_error(
			"there are no lists yet: create one with `reminders new-list "
			"NAME`");
	}
	return *lists.front();
}

rem::Ref App::resolve(const std::string& text,
					  const std::optional<std::string>& in) {
	rem::ListFile* only = in ? &list_named(*in) : nullptr;
	auto id = text.starts_with('^') ? text.substr(1) : text;
	if (auto r = store_.find(id); r && (!only || r->list == only)) {
		return *r;
	}

	// Rank titles: exact, then starting with the text, then containing it,
	// then containing all its words in any order.
	auto q = term::lower(text);
	std::vector<std::string> words;
	for (std::size_t i = 0; i < q.size();) {
		auto b = q.find_first_not_of(' ', i);
		if (b == std::string::npos) {
			break;
		}
		auto e = q.find(' ', b);
		words.push_back(
			q.substr(b, e == std::string::npos ? std::string::npos : e - b));
		i = e == std::string::npos ? q.size() : e;
	}
	auto score = [&](const std::string& title) {
		auto t = term::lower(title);
		if (t == q) {
			return 0;
		}
		if (t.starts_with(q)) {
			return 1;
		}
		if (t.find(q) != std::string::npos) {
			return 2;
		}
		if (!words.empty() && std::ranges::all_of(words, [&](auto& w) {
				return t.find(w) != std::string::npos;
			})) {
			return 3;
		}
		return -1;
	};
	std::vector<rem::Ref> best;
	int best_score = 4;
	for (auto* l : store_.lists()) {
		if (only && l != only) {
			continue;
		}
		l->doc.walk([&](rem::Reminder& r, rem::Reminder* parent) {
			int sc = score(r.title);
			if (sc < 0 || sc > best_score) {
				return;
			}
			if (sc < best_score) {
				best.clear(), best_score = sc;
			}
			best.push_back({l, &r, parent});
		});
	}
	if (best.size() > 1) {	// open reminders win over completed ones
		std::vector<rem::Ref> open;
		for (auto& c : best) {
			if (!c.reminder->done) {
				open.push_back(c);
			}
		}
		if (!open.empty()) {
			best = std::move(open);
		}
	}
	if (best.empty()) {
		throw std::runtime_error(
			only ? std::format("nothing in {} matches “{}”", only->name, text)
				 : std::format("nothing matches “{}”", text));
	}
	if (best.size() == 1) {
		return best.front();
	}

	// Several match: ask, when someone is there to answer.
	auto describe = [&](const rem::Ref& c) {
		return std::format(
			"{}  ({}{})", term::markdown_line(*c.reminder).text(),
			list_label(*c.list), c.parent ? " > " + c.parent->title : "");
	};
	if (isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
		std::cerr << std::format("“{}” matches {} reminders:\n", text,
								 best.size());
		for (std::size_t i = 0; i < best.size(); ++i) {
			std::cerr << std::format("  {}) {}\n", i + 1, describe(best[i]));
		}
		std::cerr << "Which one? [1-" << best.size() << "] " << std::flush;
		std::string answer;
		std::getline(std::cin, answer);
		int n = 0;
		auto [p, ec] =
			std::from_chars(answer.data(), answer.data() + answer.size(), n);
		if (ec == std::errc{} && n >= 1 && n <= static_cast<int>(best.size())) {
			return best[static_cast<std::size_t>(n - 1)];
		}
		throw std::runtime_error("nothing chosen");
	}
	std::string msg = std::format(
		"“{}” matches {} reminders; be more specific, or add --in LIST:\n",
		text, best.size());
	for (std::size_t i = 0; i < best.size() && i < 10; ++i) {
		msg += "  " + describe(best[i]) + "\n";
	}
	if (best.size() > 10) {
		msg += std::format("  … and {} more\n", best.size() - 10);
	}
	msg.pop_back();
	throw std::runtime_error(msg);
}

void App::apply_fields(rem::Reminder& r, const Args& a) {
	if (auto t = a.get("title")) {
		auto f = rem::parse_fields(*t);
		r.title = f.title;
	}
	if (a.has("no-due")) {
		r.due_date.reset();
		r.due_time.reset();
	}
	if (auto d = a.get("due")) {
		auto date = rem::parse_human_date(*d, today_);
		if (!date) {
			throw UsageError(std::format("can't read the date “{}”", *d));
		}
		r.due_date = date;
	}
	if (auto t = a.get("time")) {
		auto time = rem::parse_time(*t);
		if (!time) {
			throw UsageError(
				std::format("can't read the time “{}” (use HH:MM)", *t));
		}
		if (!r.due_date) {
			r.due_date = today_;
		}
		r.due_time = time;
	}
	if (a.has("flag")) {
		r.flagged = true;
	}
	if (a.has("unflag")) {
		r.flagged = false;
	}
	if (auto p = a.get("priority")) {
		static const std::pair<const char*, rem::Priority> names[] = {
			{"none", rem::Priority::None},
			{"low", rem::Priority::Low},
			{"medium", rem::Priority::Medium},
			{"high", rem::Priority::High}};
		auto it = std::ranges::find_if(
			names, [&](auto& n) { return term::lower(*p) == n.first; });
		if (it == std::end(names)) {
			throw UsageError("--priority is none, low, medium or high");
		}
		r.priority = it->second;
	}
	for (auto t : a.all("tag")) {
		if (t.starts_with('#')) {
			t.erase(0, 1);
		}
		if (!t.empty() && std::ranges::find(r.tags, t) == r.tags.end()) {
			r.tags.push_back(t);
		}
	}
	for (auto t : a.all("untag")) {
		if (t.starts_with('#')) {
			t.erase(0, 1);
		}
		std::erase(r.tags, t);
	}
	if (a.has("no-repeat")) {
		r.repeat.reset();
	}
	if (auto rule = a.get("repeat")) {
		r.repeat = *rule;
	}
	if (auto n = a.get("notes")) {
		r.notes = *n;
	}
	if (auto u = a.get("url")) {
		r.url = u->empty() ? std::nullopt : std::optional{*u};
	}
}

void App::report(const std::string& verb, const rem::Ref& ref) {
	if (g_.json) {
		std::cout << json_reminder(ref) << "\n";
		return;
	}
	std::cout << verb << ": ";
	print_reminder(ref, st_, 0, true, today_);
}

int App::run(const std::string& cmd, const Args& a) {
	// Each command, under its names.
	using Handler = int (*)(App&, const Args&);
	static const std::pair<std::vector<std::string_view>, Handler> kCommands[] =
		{
			{{"lists"}, [](App& app, const Args&) { return app.cmd_lists(); }},
			{{"list", "ls"},
			 [](App& app, const Args& a) { return app.cmd_list(a); }},
			{{"show"}, [](App& app, const Args& a) { return app.cmd_show(a); }},
			{{"add"}, [](App& app, const Args& a) { return app.cmd_add(a); }},
			{{"edit"}, [](App& app, const Args& a) { return app.cmd_edit(a); }},
			{{"done"},
			 [](App& app, const Args& a) { return app.cmd_done(a, true); }},
			{{"undone"},
			 [](App& app, const Args& a) { return app.cmd_done(a, false); }},
			{{"move", "mv"},
			 [](App& app, const Args& a) { return app.cmd_move(a); }},
			{{"delete", "rm"},
			 [](App& app, const Args& a) { return app.cmd_delete(a); }},
			{{"search"},
			 [](App& app, const Args& a) { return app.cmd_search(a); }},
			{{"new-list"},
			 [](App& app, const Args& a) { return app.cmd_new_list(a); }},
			{{"import"},
			 [](App& app, const Args& a) { return app.cmd_import(a); }},
			{{"export"},
			 [](App& app, const Args& a) { return app.cmd_export(a); }},
		};
	for (auto& [names, handler] : kCommands) {
		if (std::ranges::find(names, cmd) != names.end()) {
			return handler(*this, a);
		}
	}
	throw UsageError(
		std::format("unknown command “{}” (see reminders --help)", cmd));
}

}  // namespace cli
