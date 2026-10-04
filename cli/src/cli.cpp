// reminders: the command-line interface (and, with no command, the TUI).

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "reminders/dates.hpp"
#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/importer.hpp"
#include "reminders/library.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/store.hpp"
#include "reminders/syncthing.hpp"
#ifdef REMINDERS_NETWORK
#include "reminders/server_sync.hpp"
#endif
#include "editfile.hpp"
#include "text.hpp"
#include "tui.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";

constexpr const char* kUsage =
	R"(Usage: reminders [--folder PATH] [--json] [--no-color] [COMMAND …]

With no command, opens the interactive (terminal) interface.

Commands:
  lists                         Lists, with how many reminders are open in each
  list [VIEW] [-a]              Reminders in VIEW: a list name, today,
                                scheduled, all, all-reminders, flagged,
                                completed or #tag.
                                Default: the list that last had focus in the
                                app or TUI. -a also shows completed reminders
  show NAME                     Everything about one reminder
  add TEXT… [FIELDS]            Add a reminder (inline fields like "#tag" or
                                "📅 2026-10-03" work in TEXT too). Goes to
                                --list, else the list that last had focus
  edit NAME [FIELDS]            Change a reminder (no FIELDS: edit them all
                                in $EDITOR)
  done NAME                     Complete (repeating reminders roll forward)
  undone NAME                   Mark as not completed
  move NAME --to LIST [--section S]
                                Move to another list
  delete NAME [--yes]           Delete
  search TEXT                   Search titles and notes
  new-list NAME [--color C] [--icon I]
  import FILE [--list LIST] [--source S] [--format F] [--duplicates]
                                Import reminders from a file into LIST,
                                made in S if missing (default: the
                                calendar's name, else the file's). F, found
                                from the file if not given: md, txt (a line
                                each), todo.txt, csv or ics. Ones already
                                here are skipped, or with --duplicates
                                added again as copies
  export [LIST] [--format F] [-o FILE] [-a]
                                Write LIST as F: md (the list file, the
                                default), txt (a line per open reminder; -a
                                adds completed ones), todo.txt, csv or ics.
                                Without --format, FILE's name says. To FILE
                                (a folder: LIST.EXT in it), else to the
                                terminal. Without LIST: every list, into
                                the folder -o names, or one .zip archive
                                if -o names a .zip file
  folder [PATH]                 Show or set the folder (shared with the app)
  sync [SOURCE]                 Sync CalDAV, WebDAV and git sources now
                                (other commands sync before and after, too)
  tui                           Open the interactive interface

NAME is a reminder's title, or enough of it: an exact title wins, then one
starting with NAME, then one containing it, then one containing all its words.
Open reminders win over completed ones. --in LIST looks in one list only. If
several still match, you're asked which one (or, in a script, they're listed).

Fields:
  --title TEXT      --list LIST (add: where; edit: move there)
  --section NAME    --parent NAME (add a subtask)    --in LIST (find NAME in LIST)
  --due DATE        DATE: today, tomorrow, fri, +3d, +2w, 2026-10-31
  --time HH:MM      --no-due
  --flag            --unflag
  --priority none|low|medium|high
  --tag TAG         --untag TAG      (repeatable)
  --repeat RULE     e.g. "every week", "every 2 months"   --no-repeat
  --notes TEXT      --url URL

Options:
  -f, --folder PATH  Use PATH instead of the saved folder
  --json             Machine-readable output
  --no-color         No colours (also when NO_COLOR is set or not a terminal)
  --offline          Don't sync CalDAV, WebDAV or git sources
  --show-key-numbers Label sidebar entries with their number key, e.g.
                     "(1)Today" (interactive interface; overrides the
                     show-key-numbers setting)
  --hide-key-numbers Don't label them
  -h, --help         This help
  --version          Show the version
)";

struct UsageError : std::runtime_error {
		using std::runtime_error::runtime_error;
};

struct Global {
		std::optional<rem::fs::path> folder;
		bool json = false;
		std::optional<bool>
			key_numbers;  // --show-key-numbers / --hide-key-numbers
		bool color = false;
		bool offline = false;  // --offline: no CalDAV or WebDAV syncing
};

// Syncs the library's CalDAV and WebDAV sources (or just `only`); problems
// are reported on stderr. Returns false if any source failed.
// The parameters go unused in a build without network support.
bool sync_servers([[maybe_unused]] rem::Library& library,
				  [[maybe_unused]] const std::string& only = "",
				  [[maybe_unused]] bool verbose = false) {
	bool ok = true;
#ifdef REMINDERS_NETWORK
	for (auto& s : library.sources()) {
		if (!rem::syncs(s.config.backend) || !s.store) {
			continue;
		}
		if (!only.empty() && s.config.name != only) {
			continue;
		}
		try {
			auto r = rem::sync_source(*s.store, s.config);
			for (auto& e : r.errors) {
				std::cerr << std::format("reminders: sync {}: {}\n",
										 s.config.name, e);
			}
			ok = ok && r.errors.empty();
			if (verbose && r.errors.empty()) {
				std::cout << std::format(
					"Synced {}{}\n", rem::source_title(s.config),
					r.changed.empty()
						? ""
						: std::format(" ({} lists changed)", r.changed.size()));
			}
		} catch (const std::exception& e) {
			std::cerr << std::format("reminders: sync {}: {}\n", s.config.name,
									 e.what());
			ok = false;
		}
	}
#endif
	return ok;
}

// A folder from the command line: relative to the current folder, as the
// shell means it; "~" and "$VAR" work even when quoted.
rem::fs::path folder_arg(const std::string& arg) {
	if (arg.starts_with('~') || arg.find('$') != std::string::npos) {
		return rem::expand_path(arg);
	}
	return rem::fs::absolute(arg).lexically_normal();
}

bool has_servers(const rem::Library& library) {
	return std::ranges::any_of(library.sources(), [](auto& s) {
		return rem::syncs(s.config.backend);
	});
}

// --- output ---------------------------------------------------------------

struct Style {
		bool on;
		std::string fg(term::Rgb c) const {
			return on ? std::format("\033[38;2;{};{};{}m", c.r, c.g, c.b) : "";
		}
		std::string bold() const { return on ? "\033[1m" : ""; }
		std::string dim() const { return on ? "\033[2m" : ""; }
		std::string red() const { return on ? "\033[31m" : ""; }
		std::string reset() const { return on ? "\033[0m" : ""; }
};

std::string json_escape(std::string_view s) {
	std::string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\t':
				out += "\\t";
				break;
			case '\r':
				out += "\\r";
				break;
			default:
				if (c < 0x20) {
					out += std::format("\\u{:04x}", c);
				} else {
					out += static_cast<char>(c);
				}
		}
	}
	return out + "\"";
}

// The open library, for showing list names: a list's name, or "source/name"
// when another source has a list of that name (see Library::label).
const rem::Library* g_library = nullptr;

std::string list_label(const rem::ListFile& l) {
	return g_library ? g_library->label(l) : l.name;
}

std::string json_reminder(const rem::Ref& ref) {
	auto& r = *ref.reminder;
	std::string tags = "[";
	for (std::size_t i = 0; i < r.tags.size(); ++i) {
		tags += (i ? "," : "") + json_escape(r.tags[i]);
	}
	tags += "]";
	auto opt = [](const std::optional<std::string>& v) {
		return v ? json_escape(*v) : std::string("null");
	};
	auto section = ref.list->doc.section_of(ref.parent ? *ref.parent : r);
	return std::format(
		R"({{"id":{},"list":{},"section":{},"parent":{},"title":{},"done":{},"due":{},"time":{},)"
		R"("completed":{},"flagged":{},"priority":"{}","tags":{},"repeat":{},"url":{},"notes":{}}})",
		json_escape(r.id), json_escape(list_label(*ref.list)), opt(section),
		ref.parent ? json_escape(ref.parent->id) : "null", json_escape(r.title),
		r.done ? "true" : "false",
		r.due_date ? json_escape(rem::format_date(*r.due_date)) : "null",
		r.due_time ? json_escape(rem::format_time(*r.due_time)) : "null",
		r.completed ? json_escape(rem::format_date(*r.completed)) : "null",
		r.flagged ? "true" : "false", term::priority_name(r.priority), tags,
		opt(r.repeat), opt(r.url), json_escape(r.notes));
}

void print_json(const std::vector<rem::Ref>& refs) {
	std::cout << "[";
	for (std::size_t i = 0; i < refs.size(); ++i) {
		std::cout << (i ? ",\n " : "") << json_reminder(refs[i]);
	}
	std::cout << "]\n";
}

// One reminder as it looks in its file: "- [ ] Milk #errands 📅 2026-10-03".
// Notes follow on indented lines.
void print_reminder(const rem::Ref& ref, const Style& st, int indent,
					bool show_list, rem::Date today) {
	auto& r = *ref.reminder;
	auto md = term::markdown_line(r);
	std::string line(static_cast<std::size_t>(indent), ' ');
	line += r.done ? st.dim() + md.before + st.reset() : md.before;
	if (!md.due.empty()) {
		line += " " + (term::is_overdue(r, today) ? st.red() : std::string()) +
				md.due + st.reset();
	}
	if (!md.after.empty()) {
		line += " " + st.dim() + md.after + st.reset();
	}
	if (show_list) {
		line += "  " + st.dim() + "(" + list_label(*ref.list) +
				(ref.parent ? " > " + ref.parent->title : "") + ")" +
				st.reset();
	}
	std::cout << line << "\n";
	for (std::size_t s = 0; !r.notes.empty();) {
		auto nl = r.notes.find('\n', s);
		std::cout << std::string(static_cast<std::size_t>(indent) + 2, ' ')
				  << st.dim() << r.notes.substr(s, nl - s) << st.reset()
				  << "\n";
		if (nl == std::string::npos) {
			break;
		}
		s = nl + 1;
	}
}

// "# Groceries" / "## Party", in the list's colour when there is one.
void print_heading(int level, const std::string& text,
				   std::optional<term::Rgb> color, const Style& st) {
	std::cout << st.bold() << (color ? st.fg(*color) : "")
			  << std::string(static_cast<std::size_t>(level), '#') << " "
			  << text << st.reset() << "\n";
}

// --- argument parsing -------------------------------------------------------

struct Args {
		std::vector<std::string> positional;
		std::multimap<std::string, std::string>
			options;  // name → value ("" for flags)

		bool has(const std::string& k) const { return options.contains(k); }
		std::optional<std::string> get(const std::string& k) const {
			auto it = options.find(k);
			return it == options.end() ? std::nullopt
									   : std::optional{it->second};
		}
		std::vector<std::string> all(const std::string& k) const {
			std::vector<std::string> out;
			for (auto [a, b] = options.equal_range(k); a != b; ++a) {
				out.push_back(a->second);
			}
			return out;
		}
};

// Options that take a value; anything else starting with "--" is a flag.
const std::vector<std::string> kValued = {
	"title", "list",  "section", "parent", "due",	"time",	 "priority",
	"tag",	 "untag", "repeat",	 "notes",  "url",	"color", "icon",
	"in",	 "to",	  "source",	 "format", "output"};
const std::vector<std::string> kFlags = {
	"no-due", "flag", "unflag", "no-repeat", "yes", "all", "duplicates"};

Args parse_args(std::span<const std::string> in) {
	Args a;
	for (std::size_t i = 0; i < in.size(); ++i) {
		auto& s = in[i];
		if (s == "--") {
			a.positional.insert(a.positional.end(),
								in.begin() + static_cast<long>(i) + 1,
								in.end());
			break;
		}
		if (s == "-a" || s == "-y") {
			a.options.emplace(s == "-a" ? "all" : "yes", "");
		} else if (s == "-o") {
			if (i + 1 >= in.size()) {
				throw UsageError("-o needs a file name");
			}
			a.options.emplace("output", in[++i]);
		} else if (s.starts_with("--")) {
			auto name = s.substr(2);
			std::string value;
			if (auto eq = name.find('='); eq != std::string::npos) {
				value = name.substr(eq + 1);
				name.resize(eq);
			} else if (std::ranges::find(kValued, name) != kValued.end()) {
				if (i + 1 >= in.size()) {
					throw UsageError(std::format("--{} needs a value", name));
				}
				value = in[++i];
			}
			if (std::ranges::find(kValued, name) == kValued.end() &&
				std::ranges::find(kFlags, name) == kFlags.end()) {
				throw UsageError(std::format("unknown option --{}", name));
			}
			a.options.emplace(name, value);
		} else {
			a.positional.push_back(s);
		}
	}
	return a;
}

std::string join(const std::vector<std::string>& v, std::size_t from = 0) {
	std::string out;
	for (auto i = from; i < v.size(); ++i) {
		out += (out.empty() ? "" : " ") + v[i];
	}
	return out;
}

// --- the folder
// -----------------------------------------------------------------

class App {
	public:
		// `own_folder`: this is the saved folder, so the saved view applies to
		// it. `library`: every source, or the --folder one (see
		// rem::open_library).
		App(Global g, std::unique_ptr<rem::Library> library, bool own_folder)
			: g_(g),
			  st_{g.color},
			  owned_(std::move(library)),
			  store_(*owned_),
			  today_(rem::local_today()),
			  own_folder_(own_folder) {
			store_.load_all();
			g_library = owned_.get();
		}

		int run(const std::string& cmd, const Args& a);
		rem::Library& store() { return store_; }

	private:
		Global g_;
		Style st_;
		std::unique_ptr<rem::Library> owned_;
		rem::Library& store_;
		rem::Date today_;
		bool own_folder_;

		rem::ListFile& list_named(const std::string& name);
		term::SavedView saved_view();
		rem::ListFile& default_list();
		// Finds a reminder by name (or id); `in` limits the search to one list.
		rem::Ref resolve(const std::string& text,
						 const std::optional<std::string>& in);
		void apply_fields(rem::Reminder& r, const Args& a);
		void report(const std::string& verb, const rem::Ref& ref);

		int cmd_lists();
		int cmd_list(const Args& a);
		int cmd_show(const Args& a);
		int cmd_add(const Args& a);
		int cmd_edit(const Args& a);
		int cmd_done(const Args& a, bool done);
		int cmd_move(const Args& a);
		int cmd_delete(const Args& a);
		int cmd_search(const Args& a);
		int cmd_new_list(const Args& a);
		int cmd_import(const Args& a);
		int cmd_export(const Args& a);
};

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

int App::cmd_lists() {
	auto lists = store_.lists();
	if (g_.json) {
		std::cout << "[";
		for (std::size_t i = 0; i < lists.size(); ++i) {
			int open = 0;
			lists[i]->doc.walk(
				[&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
			std::cout
				<< (i ? ",\n " : "")
				<< std::format(
					   R"({{"name":{},"source":{},"color":"{}","icon":"{}","open":{}}})",
					   json_escape(lists[i]->name),
					   json_escape(store_.source_of(*lists[i])->config.name),
					   lists[i]->color(), lists[i]->icon(), open);
		}
		std::cout << "]\n";
		return 0;
	}
	// With several sources, under a heading each.
	bool several = store_.sources().size() > 1;
	for (auto& source : store_.sources()) {
		if (several) {
			print_heading(1, rem::source_title(source.config), std::nullopt,
						  st_);
		}
		for (auto* l : store_.lists(source.config.name)) {
			int open = 0;
			l->doc.walk(
				[&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
			std::cout << st_.fg(term::color_rgb(l->color())) << l->name
					  << st_.reset() << st_.dim() << "  (" << open << " open)"
					  << st_.reset() << "\n";
		}
		if (several && &source != &store_.sources().back()) {
			std::cout << "\n";
		}
	}
	if (!store_.candidates().empty() && !g_.json) {
		std::cout << st_.dim() << "\nNot lists yet (no “reminders: 1”): "
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
			print_json(all);
			return 0;
		}
		print_heading(1, l.name, term::color_rgb(l.color()), st_);
		int hidden = 0;
		for (auto& section : l.doc.sections()) {
			if (section.name) {
				std::cout << "\n";
				print_heading(2, *section.name, term::color_rgb(l.color()),
							  st_);
			}
			for (auto* r : section.reminders) {
				if (r->done && !with_done) {
					++hidden;
					continue;
				}
				print_reminder({&l, r, nullptr}, st_, 0, false, today_);
				for (auto& s : r->subtasks) {
					if (s.done && !with_done) {
						++hidden;
						continue;
					}
					print_reminder({&l, &s, r}, st_, 2, false, today_);
				}
			}
		}
		if (hidden) {
			std::cout << st_.dim() << "\n"
					  << hidden << " completed (show with -a)" << st_.reset()
					  << "\n";
		}
		return 0;
	}

	if (g_.json) {
		print_json(refs);
		return 0;
	}
	print_heading(1, title, std::nullopt, st_);
	if (refs.empty()) {
		std::cout << st_.dim() << "Nothing here." << st_.reset() << "\n";
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
			std::cout << "\n";
			print_heading(
				2, g,
				by_date || flat
					? std::nullopt
					: std::optional{term::color_rgb(ref.list->color())},
				st_);
			group = g;
		}
		print_reminder(ref, st_, 0, by_date || flat, today_);
	}
	return 0;
}

int App::cmd_show(const Args& a) {
	if (a.positional.empty()) {
		throw UsageError("show needs the name of a reminder");
	}
	auto ref = resolve(join(a.positional), a.get("in"));
	if (g_.json) {
		std::cout << json_reminder(ref) << "\n";
		return 0;
	}
	auto& r = *ref.reminder;
	auto row = [&](const char* label, const std::string& value) {
		if (!value.empty()) {
			std::cout << st_.dim() << std::format("{:<10}", label)
					  << st_.reset() << value << "\n";
		}
	};
	print_reminder(ref, st_, 0, false, today_);
	std::cout << "\n";
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
		std::cout << st_.dim() << "subtasks" << st_.reset() << "\n";
		for (auto& s : r.subtasks) {
			print_reminder({ref.list, &s, &r}, st_, 2, false, today_);
		}
	}
	return 0;
}

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

int App::cmd_search(const Args& a) {
	auto q = join(a.positional);
	if (q.empty()) {
		throw UsageError("search needs some text");
	}
	auto refs = store_.search(q);
	if (g_.json) {
		print_json(refs);
		return 0;
	}
	if (refs.empty()) {
		std::cout << st_.dim() << "No results." << st_.reset() << "\n";
		return 1;
	}
	for (auto& r : refs) {
		print_reminder(r, st_, 0, true, today_);
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

int App::cmd_import(const Args& a) {
	if (a.positional.size() != 1) {
		throw UsageError(
			"usage: reminders import FILE [--list LIST] [--source SOURCE] "
			"[--format F] [--duplicates]");
	}
	auto path = folder_arg(a.positional[0]);
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error(std::format("can't read {}", path.string()));
	}
	std::ostringstream text;
	text << in.rdbuf();
	rem::Import imp;
	if (auto f = a.get("format")) {
		auto format = rem::export_format(*f);
		if (!format) {
			throw UsageError("--format is md, txt, todo.txt, csv or ics");
		}
		static constexpr rem::Import::Kind kKinds[] = {
			rem::Import::Kind::Markdown, rem::Import::Kind::Text,
			rem::Import::Kind::Ics, rem::Import::Kind::Todotxt,
			rem::Import::Kind::Csv};
		imp = rem::read_as(text.str(), kKinds[static_cast<int>(*format)],
						   std::chrono::current_zone());
	} else {
		imp = rem::read_import(text.str(), std::chrono::current_zone(),
							   path.filename().string());
	}
	if (imp.items.empty()) {
		throw std::runtime_error(
			std::format("there are no reminders to import in {}",
						path.filename().string()));
	}

	// The list: --list (a name, or source/name), else one named after the
	// calendar (or the file); made in --source (or the default source) if
	// there's none.
	auto name = a.get("list").value_or("");
	if (name.empty()) {
		name = imp.name.empty() ? path.stem().string() : imp.name;
		std::ranges::replace(name, '/', '-');
	}
	rem::ListFile* list = nullptr;
	auto source = a.get("source");
	if (source && !store_.store(*source)) {
		throw std::runtime_error(std::format("no source called “{}”", *source));
	}
	std::vector<rem::ListFile*> found;
	for (auto* l : source ? store_.lists(*source) : store_.lists()) {
		if (term::lower(l->name) == term::lower(name) ||
			term::lower(store_.key_of(*l)) == term::lower(name)) {
			found.push_back(l);
		}
	}
	if (found.size() == 1) {
		list = found.front();
	} else if (found.size() > 1) {
		list = &list_named(name);  // says which to choose
	}
	bool created = false;
	if (!list) {
		if (name.empty() || name.front() == '.' ||
			name.find_first_of("\\<>:\"|?*") != std::string::npos) {
			throw UsageError(std::format(
				"“{}” can't be a list name: choose one with --list", name));
		}
		auto& l =
			store_.create_list(source.value_or(store_.default_source()), name,
							   imp.color.empty() ? "blue" : imp.color, "list");
		list = &l;
		created = true;
	}
	auto r = rem::import_into(store_, *list, imp, a.has("duplicates"));
	if (created && r.added == 0) {	// nothing new: no empty list either
		store_.delete_list(store_.key_of(*list));
		if (g_.json) {
			std::cout
				<< std::format(
					   R"({{"list":null,"created":false,"added":0,"already":{},"skipped":{}}})",
					   r.already, imp.skipped)
				<< "\n";
			return 0;
		}
		std::cout << std::format(
			"Nothing to import: {} already here\n",
			r.already == 1 ? "the one reminder is"
						   : std::format("all {} reminders are", r.already));
		return 0;
	}
	if (g_.json) {
		std::cout
			<< std::format(
				   R"({{"list":{},"created":{},"added":{},"already":{},"skipped":{}}})",
				   json_escape(store_.key_of(*list)), created, r.added,
				   r.already, imp.skipped)
			<< "\n";
		return 0;
	}
	auto plural = [](int n, std::string_view one, std::string_view many) {
		return std::format("{} {}", n, n == 1 ? one : many);
	};
	std::cout << std::format("Imported {} ({}) into {}{}",
							 plural(r.added, "reminder", "reminders"),
							 rem::kind_name(imp.kind), store_.label(*list),
							 created ? " (a new list)" : "");
	std::vector<std::string> notes;
	if (r.already) {
		notes.push_back(
			plural(r.already, "was already there", "were already there"));
	}
	if (imp.skipped) {
		notes.push_back(plural(imp.skipped, "event or other item skipped",
							   "events or other items skipped"));
	}
	for (std::size_t i = 0; i < notes.size(); ++i) {
		std::cout << (i ? ", " : "; ") << notes[i];
	}
	std::cout << "\n";
	return 0;
}

int App::cmd_export(const Args& a) {
	auto out = a.get("output");
	std::optional<rem::ExportFormat> format;
	if (auto f = a.get("format")) {
		format = rem::export_format(*f);
		if (!format) {
			throw UsageError("--format is md, txt, todo.txt, csv or ics");
		}
	} else if (out && !a.positional.empty()) {
		format = rem::export_format_for(*out);
	}
	auto fmt = format.value_or(rem::ExportFormat::Markdown);
	rem::ExportOptions options;
	options.completed = a.has("all");

	// Every list, into a folder.
	if (a.positional.empty()) {
		if (!out || *out == "-") {
			throw UsageError(
				"usage: reminders export [LIST] [--format F] [-o FILE]; "
				"without LIST, -o names a folder");
		}
		auto folder = folder_arg(*out);
		if (term::lower(folder.extension().string()) == ".zip") {
			std::ofstream file(folder, std::ios::binary | std::ios::trunc);
			file << rem::export_zip(store_, store_.lists(), fmt, options);
			file.close();
			if (!file) {
				throw std::runtime_error(
					std::format("couldn't write {}", folder.string()));
			}
			auto n = store_.lists().size();
			if (g_.json) {
				std::cout << std::format(
								 R"({{"format":"{}","archive":{},"lists":{}}})",
								 rem::export_extension(fmt),
								 json_escape(folder.string()), n)
						  << "\n";
			} else {
				std::cout << std::format("Exported {} {} to {}\n", n,
										 n == 1 ? "list" : "lists",
										 folder.string());
			}
			return 0;
		}
		if (std::filesystem::exists(folder) &&
			!std::filesystem::is_directory(folder)) {
			throw std::runtime_error(
				std::format("{} isn't a folder", folder.string()));
		}
		auto files = rem::export_all(store_, folder, fmt, options);
		if (g_.json) {
			std::string list;
			for (auto& f : files) {
				list += (list.empty() ? "" : ",") + json_escape(f.string());
			}
			std::cout << std::format(R"({{"format":"{}","files":[{}]}})",
									 rem::export_extension(fmt), list)
					  << "\n";
		} else {
			std::cout << std::format("Exported {} {} to {}\n", files.size(),
									 files.size() == 1 ? "list" : "lists",
									 folder.string());
		}
		return 0;
	}
	auto& list = list_named(join(a.positional));
	auto text = rem::export_list(list, fmt, options);
	if (!out || *out == "-") {
		std::cout << text;
		return 0;
	}
	auto path = folder_arg(*out);
	if (std::filesystem::is_directory(path)) {
		path /= std::format("{}.{}", list.name, rem::export_extension(fmt));
	}
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << text;
	file.close();
	if (!file) {
		throw std::runtime_error(
			std::format("couldn't write {}", path.string()));
	}
	if (g_.json) {
		std::cout << std::format(R"({{"list":{},"format":"{}","file":{}}})",
								 json_escape(store_.key_of(list)),
								 rem::export_extension(fmt),
								 json_escape(path.string()))
				  << "\n";
	} else {
		std::cout << std::format("Exported {} to {}\n", store_.label(list),
								 path.string());
	}
	return 0;
}

int App::run(const std::string& cmd, const Args& a) {
	if (cmd == "lists") {
		return cmd_lists();
	}
	if (cmd == "list" || cmd == "ls") {
		return cmd_list(a);
	}
	if (cmd == "show") {
		return cmd_show(a);
	}
	if (cmd == "add") {
		return cmd_add(a);
	}
	if (cmd == "edit") {
		return cmd_edit(a);
	}
	if (cmd == "done") {
		return cmd_done(a, true);
	}
	if (cmd == "undone") {
		return cmd_done(a, false);
	}
	if (cmd == "move" || cmd == "mv") {
		return cmd_move(a);
	}
	if (cmd == "delete" || cmd == "rm") {
		return cmd_delete(a);
	}
	if (cmd == "search") {
		return cmd_search(a);
	}
	if (cmd == "new-list") {
		return cmd_new_list(a);
	}
	if (cmd == "import") {
		return cmd_import(a);
	}
	if (cmd == "export") {
		return cmd_export(a);
	}
	throw UsageError(
		std::format("unknown command “{}” (see reminders --help)", cmd));
}

}  // namespace

int main(int argc, char** argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	Global g;
	g.color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
	// --json and --no-color work anywhere on the line, not just before the
	// command.
	std::erase_if(args, [&](const std::string& s) {
		if (s == "--json") {
			return g.json = true;
		}
		if (s == "--show-key-numbers" || s == "--hide-key-numbers") {
			g.key_numbers = s == "--show-key-numbers";
			return true;
		}
		if (s == "--no-color") {
			return !(g.color = false);
		}
		if (s == "--offline") {
			return g.offline = true;
		}
		return false;
	});

	// Global options come before the command.
	std::size_t i = 0;
	for (; i < args.size(); ++i) {
		auto& s = args[i];
		if (s == "-h" || s == "--help") {
			std::cout << kUsage;
			return 0;
		}
		if (s == "--version") {
			std::cout << "reminders " << kVersion << "\n";
			return 0;
		}
		if (s == "--json") {
			g.json = true;
		} else if (s == "--no-color") {
			g.color = false;
		} else if (s == "-f" || s == "--folder") {
			if (i + 1 >= args.size()) {
				std::cerr << "reminders: --folder needs a path\n";
				return 2;
			}
			g.folder = args[++i];
		} else if (s.starts_with("--folder=")) {
			g.folder = s.substr(9);
		} else {
			break;
		}
	}
	std::string cmd = i < args.size() ? args[i++] : "tui";
	std::vector<std::string> rest(args.begin() + static_cast<long>(i),
								  args.end());

	try {
		if (cmd == "folder") {
			if (rest.empty()) {
				auto f = rem::saved_folder();
				if (!f) {
					std::cerr << "reminders: no folder set (use `reminders "
								 "folder PATH`)\n";
					return 1;
				}
				std::cout << f->string() << "\n";
				return 0;
			}
			auto path = folder_arg(rest[0]);
			if (!rem::fs::is_directory(path)) {
				throw std::runtime_error(
					std::format("“{}” is not a folder", rest[0]));
			}
			auto source = rem::set_default_folder(path);
			std::cout << std::format("Folder set to {} (source “{}”, {})\n",
									 source.folder.string(), source.name,
									 rem::backend_name(source.backend));
			return 0;
		}

		// Every configured source, or just the --folder one for this run.
		std::optional<rem::fs::path> folder;
		if (g.folder) {
			folder = folder_arg(g.folder->string());
			if (!rem::fs::is_directory(*folder)) {
				throw std::runtime_error(
					std::format("“{}” is not a folder", folder->string()));
			}
		}
		auto library = rem::open_library(folder, rem::device_name());
		if (library->sources().empty()) {
			throw std::runtime_error(
				"no folder: pass --folder PATH, or set one with `reminders "
				"folder PATH`");
		}

		// A --folder that isn't a configured source is for this run only: the
		// saved view belongs to the configured ones.
		bool own_folder =
			!g.folder || !library->sources().front().config.name.empty();

		if (cmd == "sync") {
			if (!has_servers(*library)) {
				throw std::runtime_error(
					"no CalDAV, WebDAV or git sources to sync");
			}
#ifndef REMINDERS_NETWORK
			throw std::runtime_error(
				"this build has no CalDAV, WebDAV or git support");
#endif
			return sync_servers(*library, rest.empty() ? "" : rest[0], !g.json)
					 ? 0
					 : 1;
		}
		// CalDAV and WebDAV sources: fresh from the server going in, and
		// changes sent back coming out (the TUI syncs in the background
		// instead).
		bool sync = !g.offline && cmd != "tui" && has_servers(*library);
		if (sync) {
			sync_servers(*library);
		}
		auto* lib = library.get();
		App app(g, std::move(library), own_folder);
		if (cmd == "tui") {
			if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
				throw UsageError(
					"the interactive interface needs a terminal; see reminders "
					"--help for commands");
			}
			return run_tui(app.store(), own_folder, g.key_numbers);
		}
		auto status = app.run(cmd, parse_args(rest));
		static constexpr std::string_view kEdits[] = {
			"add", "edit",	 "done", "undone",	 "move",
			"mv",  "delete", "rm",	 "new-list", "import"};
		if (sync && std::ranges::find(kEdits, cmd) != std::end(kEdits)) {
			sync_servers(*lib);
		}
		return status;
	} catch (const UsageError& e) {
		std::cerr << "reminders: " << e.what() << "\n";
		return 2;
	} catch (const std::exception& e) {
		std::cerr << "reminders: " << e.what() << "\n";
		return 1;
	}
}
