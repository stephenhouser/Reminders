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
	if (!hooks_.remember) {
		return {"today", ""};
	}
	auto v = term::parse_view_setting(rem::load_setting("view"));
	if (v.kind == "list" && !store_.list(v.name)) {
		return {"today", ""};
	}
	return v;
}

// For `add` without --list: the list showing (in the terminal interface),
// else the one that last had focus, else the first one.
rem::ListFile& App::default_list() {
	if (auto* l = hooks_.list.empty() ? nullptr : store_.list(hooks_.list)) {
		return *l;
	}
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
	if (hooks_.choose) {
		std::vector<std::string> options;
		for (auto& c : best) {
			options.push_back(describe(c));
		}
		auto n = hooks_.choose(
			std::format("“{}” matches {} reminders", text, best.size()),
			options);
		if (!n || *n >= best.size()) {
			throw cmd::Cancelled();
		}
		return best[*n];
	}
	if (hooks_.interactive && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
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

std::vector<rem::Ref> App::targets(const std::string& name, const Args& a,
								   const std::string& what) {
	if (!name.empty()) {
		return {resolve(name, a.get("in"))};
	}
	std::vector<rem::Ref> refs;
	for (auto& id : hooks_.selected) {
		if (auto r = store_.find(id)) {
			refs.push_back(*r);
		}
	}
	if (refs.empty()) {
		throw UsageError(what);
	}
	return refs;
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
		out_ << json_reminder(ref) << "\n";
		return;
	}
	out_ << verb << ": ";
	print_reminder(out_, ref, st_, 0, true, today_);
}

void App::report_all(const std::string& verb,
					 const std::vector<std::string>& ids) {
	if (ids.size() == 1 || g_.json) {
		for (auto& id : ids) {
			if (auto r = store_.find(id)) {
				report(verb, *r);
			}
		}
		return;
	}
	out_ << std::format("{} {} reminders\n", verb, ids.size());
}

int App::run(const std::string& name, const Args& a) {
	auto* c = cmd::find(name);
	auto is = [&](std::string_view n) { return c && c->names.front() == n; };
	if (is("lists")) {
		return cmd_lists();
	} else if (is("list")) {
		return cmd_list(a);
	} else if (is("show")) {
		return cmd_show(a);
	} else if (is("add")) {
		return cmd_add(a);
	} else if (is("edit")) {
		return cmd_edit(a);
	} else if (is("edit-list")) {
		return cmd_edit_list(a);
	} else if (is("done") || is("undone")) {
		return cmd_done(a, is("done"));
	} else if (is("move")) {
		return cmd_move(a);
	} else if (is("delete")) {
		return cmd_delete(a);
	} else if (is("search")) {
		return cmd_search(a);
	} else if (is("new-list")) {
		return cmd_new_list(a);
	} else if (is("import")) {
		return cmd_import(a);
	} else if (is("export")) {
		return cmd_export(a);
	}
	throw UsageError(
		std::format("unknown command “{}” (see reminders --help)", name));
}

}  // namespace cli

namespace cmd {

// Every command, with its names; in this order in --help and the terminal
// interface's :help. main() runs sync, folder and tui itself, the terminal
// interface the ones only it has.
std::span<const Command> commands() {
	static const std::vector<Command> kCommands = {
		{.names = {"lists"},
		 .summary = "the lists, with how many are open in each",
		 .help = "Lists, with how many reminders are open in each"},
		{.names = {"list", "ls"},
		 .usage = "[VIEW] [-a]",
		 .summary = "a list, today, scheduled, all, flagged, completed or #tag",
		 .help = "Reminders in VIEW: a list name, today, scheduled, all, "
				 "all-reminders, flagged, completed or #tag.\n"
				 "Default: the list that last had focus in the app or TUI. "
				 "-a also shows completed reminders",
		 .arg = Arg::View},
		{.names = {"show"},
		 .usage = "NAME",
		 .summary = "everything about one reminder",
		 .arg = Arg::Reminder},
		{.names = {"add"},
		 .usage = "TEXT… [FIELDS]",
		 .summary = "add a reminder",
		 .help =
			 "Add a reminder (inline fields like \"#tag\" or "
			 "\"📅\u00a02026-10-03\" work in TEXT too). Goes to --list, else "
			 "the list that last had focus",
		 .edits = true},
		{.names = {"edit"},
		 .usage = "NAME [FIELDS]",
		 .summary = "change a reminder (no FIELDS: all of them in $EDITOR)",
		 .help = "Change a reminder (no FIELDS: edit them all in $EDITOR)",
		 .edits = true,
		 .arg = Arg::Reminder},
		{.names = {"edit-list"},
		 .usage = "[LIST]",
		 .summary = "edit a list's Markdown file in $EDITOR",
		 .help = "Edit LIST's Markdown file, as it is, in $EDITOR (default: "
				 "the list that last had focus). Saved like any other "
				 "change, and merged with changes synced meanwhile",
		 .edits = true,
		 .arg = Arg::List},
		{.names = {"done"},
		 .usage = "NAME",
		 .summary = "complete",
		 .help = "Complete (repeating reminders roll forward)",
		 .edits = true,
		 .arg = Arg::Reminder},
		{.names = {"undone"},
		 .usage = "NAME",
		 .summary = "mark as not completed",
		 .edits = true,
		 .arg = Arg::Reminder},
		{.names = {"move", "mv"},
		 .usage = "NAME --to LIST [--section S]",
		 .summary = "move to another list",
		 .edits = true,
		 .arg = Arg::Reminder},
		{.names = {"delete", "rm"},
		 .usage = "NAME [--yes]",
		 .summary = "delete",
		 .edits = true,
		 .arg = Arg::Reminder},
		{.names = {"search"},
		 .usage = "TEXT",
		 .summary = "search titles and notes"},
		{.names = {"new-list"},
		 .usage = "NAME [--color C] [--icon I] [--source S]",
		 .summary = "make a list",
		 .help = "Make a list (in source S, when given)",
		 .edits = true},
		{.names = {"import"},
		 .usage = "FILE [--list LIST] [--source S] [--format F] [--duplicates]",
		 .summary =
			 "import reminders from a file (md, txt, todo.txt, csv, ics)",
		 .help = "Import reminders from a file into LIST, made in S if "
				 "missing (default: the calendar's name, else the file's). "
				 "F, found from the file if not given: md, txt (a line each), "
				 "todo.txt, csv or ics. Ones already here are skipped, or "
				 "with --duplicates added again as copies",
		 .edits = true,
		 .arg = Arg::File},
		{.names = {"export"},
		 .usage = "[LIST] [--format F] [-o FILE] [-a]",
		 .summary = "write a list to a file; without LIST, every list (-o a "
					"folder or .zip)",
		 .help = "Write LIST as F: md (the list file, the default), txt (a "
				 "line per open reminder; -a adds completed ones), todo.txt, "
				 "csv or ics. Without --format, FILE's name says. To FILE (a "
				 "folder: LIST.EXT in it), else to the terminal. Without "
				 "LIST: every list, into the folder -o names, or one .zip "
				 "archive if -o names a .zip file",
		 .arg = Arg::List},
		{.names = {"folder"},
		 .usage = "[PATH]",
		 .summary = "show or set the folder",
		 .help = "Show or set the folder (shared with the app)",
		 .where = Where::Cli,
		 .arg = Arg::File},
		{.names = {"sync"},
		 .usage = "[SOURCE]",
		 .summary = "sync CalDAV, WebDAV and git sources now",
		 .help = "Sync CalDAV, WebDAV and git sources now (other commands "
				 "sync before and after, too)",
		 .arg = Arg::Source},
		{.names = {"tui"},
		 .summary = "open the interactive interface",
		 .where = Where::Cli},
		{.names = {"go"},
		 .usage = "VIEW",
		 .summary = "show a list, smart list or tag",
		 .where = Where::Tui,
		 .arg = Arg::View},
		{.names = {"undo"},
		 .summary = "undo the last change",
		 .where = Where::Tui},
		{.names = {"redo"},
		 .summary = "redo what was undone",
		 .where = Where::Tui},
		{.names = {"set"},
		 .usage = "[NAME | noNAME | NAME! | NAME=VALUE]",
		 .summary = "show or change completed, sidebar, note-lines",
		 .where = Where::Tui,
		 .arg = Arg::Setting},
		{.names = {"help", "h"},
		 .usage = "[COMMAND]",
		 .summary = "the commands, or how to use one",
		 .where = Where::Tui,
		 .arg = Arg::Command},
		{.names = {"quit", "q", "wq", "x"},
		 .summary = "quit",
		 .where = Where::Tui},
	};
	return kCommands;
}

const Command* find(std::string_view name) {
	for (auto& c : commands()) {
		if (std::ranges::find(c.names, name) != c.names.end()) {
			return &c;
		}
	}
	return nullptr;
}

std::vector<std::string> option_names() {
	auto all = cli::kValued;
	all.insert(all.end(), cli::kFlags.begin(), cli::kFlags.end());
	return all;
}

bool takes_value(std::string_view option) {
	return std::ranges::find(cli::kValued, option) != cli::kValued.end();
}

Arg option_arg(std::string_view option) {
	if (option == "list" || option == "to" || option == "in") {
		return Arg::List;
	}
	if (option == "source") {
		return Arg::Source;
	}
	if (option == "output") {
		return Arg::File;
	}
	if (option == "parent") {
		return Arg::Reminder;
	}
	return Arg::None;
}

int run(rem::Library& library, std::span<const std::string> words,
		std::ostream& out, const Hooks& hooks) {
	if (words.empty()) {
		return 0;
	}
	cli::App app({}, library, out, hooks);
	return app.run(words.front(), cli::parse_args(words.subspan(1)));
}

}  // namespace cmd
