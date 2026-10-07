// ":" — a command line, as in vi: the CLI's commands (commands.hpp) run on
// the library that's open, plus the ones only the terminal interface has
// (quit, go, set, undo…). A command given no NAME acts on the marked
// reminders, else the selected one.
#include <charconv>
#include <fstream>
#include <sstream>

#include "../commands.hpp"
#include "internal.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"

namespace tui {

namespace {

constexpr std::size_t kHistorySize = 500;

fs::path history_file() { return rem::state_dir() / "command-history"; }

// "show-completed=on" style values for :set.
std::optional<bool> parse_bool(const std::string& v) {
	auto s = term::lower(v);
	if (s == "on" || s == "true" || s == "yes" || s == "1") {
		return true;
	}
	if (s == "off" || s == "false" || s == "no" || s == "0") {
		return false;
	}
	return std::nullopt;
}

const std::vector<std::string> kSettings = {"completed", "sidebar",
											"key-numbers", "note-lines"};

}  // namespace

// `names` that start with `word`, ignoring case, each once, in order.
std::vector<std::string> starting_with(const std::vector<std::string>& names,
									   const std::string& word) {
	std::vector<std::string> out;
	auto w = term::lower(word);
	for (auto& n : names) {
		if (term::lower(n).starts_with(w) &&
			std::ranges::find(out, n) == out.end()) {
			out.push_back(n);
		}
	}
	return out;
}

// Files and folders for a path being typed ("~/Doc", "../x"); folders end
// in "/". As typed: "~" stays "~".
std::vector<std::string> files_for(const std::string& word) {
	auto slash = word.rfind('/');
	auto dir =
		slash == std::string::npos ? std::string() : word.substr(0, slash + 1);
	auto base = word.substr(dir.size());
	fs::path where = dir.empty() ? fs::current_path()
				   : dir.starts_with('~') || dir.find('$') != std::string::npos
					   ? rem::expand_path(dir)
					   : fs::path(dir);
	std::vector<std::string> out;
	std::error_code ec;
	for (auto& e : fs::directory_iterator(where, ec)) {
		auto name = e.path().filename().string();
		if (!name.starts_with(base) ||
			(name.starts_with('.') && !base.starts_with('.'))) {
			continue;
		}
		out.push_back(dir + name + (e.is_directory(ec) ? "/" : ""));
	}
	std::ranges::sort(out);
	return out;
}

bool Tui::command_line() {
	if (!history_read_) {
		history_read_ = true;
		std::ifstream in(history_file());
		for (std::string line; std::getline(in, line);) {
			if (!line.empty()) {
				command_history_.push_back(line);
			}
		}
	}
	LineHooks hooks;
	hooks.history = &command_history_;
	hooks.complete = [this](const std::string& before, std::size_t& begin) {
		return complete_command(before, begin);
	};
	attron(COLOR_PAIR(kStatus));
	mvhline(LINES - 1, 0, ' ', COLS);
	mvaddstr(LINES - 1, 0, ":");
	attroff(COLOR_PAIR(kStatus));
	auto line = edit_line(LINES - 1, 1, std::max(1, COLS - 2), "",
						  COLOR_PAIR(kStatus), &hooks);
	if (!line || line->find_first_not_of(" \t") == std::string::npos) {
		return true;
	}
	line->erase(line->find_last_not_of(" \t") + 1);
	std::erase(command_history_, *line);
	command_history_.push_back(*line);
	if (command_history_.size() > kHistorySize) {
		command_history_.erase(
			command_history_.begin(),
			command_history_.end() - static_cast<long>(kHistorySize));
	}
	try {
		fs::create_directories(history_file().parent_path());
		std::ofstream out(history_file(), std::ios::trunc);
		for (auto& l : command_history_) {
			out << l << "\n";
		}
	} catch (const std::exception&) {
		// It just won't be remembered.
	}
	return run_command(*line);
}

bool Tui::run_command(const std::string& line) {
	std::vector<std::string> words;
	try {
		words = rem::split_command_line(line);
	} catch (const std::exception& e) {
		message_ = e.what();
		return true;
	}
	if (words.empty()) {
		return true;
	}
	auto* c = cmd::find(words.front());
	if (!c) {
		message_ =
			std::format("Not a command: {} (:help lists them)", words.front());
		return true;
	}
	auto name = std::string(c->names.front());
	auto rest = [&] {
		std::vector<std::string> r(words.begin() + 1, words.end());
		return r;
	}();
	auto text = [&] {  // the words after the command, as one
		std::string t;
		for (auto& w : rest) {
			t += (t.empty() ? "" : " ") + w;
		}
		return t;
	};

	// The terminal interface's own, and the shared ones it runs its own way.
	if (name == "quit") {
		return false;
	}
	if (c->where == cmd::Where::Cli) {
		message_ = std::format("“{}” is for the command line (reminders {})",
							   name, name);
		return true;
	}
	if (name == "help") {
		if (!rest.empty()) {
			auto* h = cmd::find(rest.front());
			message_ = h ? std::format(":{} {} — {}", h->names.front(),
									   h->usage, h->summary)
						 : std::format("Not a command: {}", rest.front());
			return true;
		}
		// Each as it's typed, then what it does.
		std::vector<std::string> lines;
		for (auto& x : cmd::commands()) {
			if (x.where == cmd::Where::Cli) {
				continue;
			}
			lines.push_back(std::format("  :{} {}", x.names.front(), x.usage));
			lines.push_back(std::format("      {}", x.summary));
		}
		lines.emplace_back();
		lines.emplace_back(
			"  NAME left out: the marked reminders, else the selected one.");
		lines.emplace_back(
			"  Fields (add, edit): --due DATE --time HH:MM --flag --priority "
			"P");
		lines.emplace_back(
			"  --tag T --untag T --repeat RULE --notes TEXT --url URL --list "
			"L");
		lines.emplace_back(
			"  Tab completes; Up / Down go back through earlier commands.");
		show_text("Commands", lines);
		return true;
	}
	if (name == "undo" || name == "redo") {
		return handle_key(name == "undo" ? 'u' : 'r', false);
	}
	if (name == "sync") {
		if (rest.empty()) {
			return handle_key('s', false);
		}
		if (term::lower(rest.front()) == "all") {
			sync_all();
		} else {
			sync_source(rest.front());
		}
		return true;
	}
	if (name == "list" || name == "go" || name == "search") {
		bool all = std::erase(rest, "-a") > 0;
		auto q = text();
		if (q.empty()) {
			message_ = std::format(":{} {}", name, c->usage);
			return true;
		}
		if (all) {
			show_completed_ = true;
		}
		if (name == "search") {
			marked_.clear();
			view_ = {View::Search, q};
			item_sel_.clear();
			focus_items_ = true;
		} else if (auto v = find_view(q)) {
			select_view(*v);
			focus_items_ = true;
		} else {
			message_ = std::format("Nothing called “{}”", q);
		}
		return true;
	}
	if (name == "set") {
		if (rest.empty()) {
			rest = kSettings;
			for (auto& s : rest) {
				s += '?';
			}
		}
		std::string shown;
		for (auto& w : rest) {
			message_.clear();
			set_option(w);
			shown += (shown.empty() ? "" : "  ") + message_;
		}
		message_ = shown;
		return true;
	}

	// The commands the CLI has: on this library, with what they print
	// collected, and an edit one undo step (each list written once). The
	// background sync sends the changes on.
	cmd::Hooks hooks;
	hooks.remember = remember_;
	hooks.list = view_.kind == View::List ? view_.name : "";
	if (!marked_.empty()) {
		hooks.selected = marked_items();
	} else if (!item_sel_.empty() && store_.find(item_sel_)) {
		hooks.selected = {item_sel_};
	}
	hooks.confirm = [this](const std::string& q) { return confirm(q); };
	hooks.choose = [this](const std::string& q,
						  const std::vector<std::string>& options) {
		std::vector<std::string> numbered;
		for (std::size_t i = 0; i < options.size(); ++i) {
			numbered.push_back(i < 9 ? std::format("{}  {}", i + 1, options[i])
									 : "   " + options[i]);
		}
		return pick(q, numbered);
	};
	hooks.edit = [this](const std::string& id) {
		def_prog_mode();
		endwin();
		auto outcome = editfile::Outcome::Unchanged;
		try {
			outcome = editfile::edit(store_, id);
		} catch (...) {
			reset_prog_mode();
			refresh();
			throw;
		}
		reset_prog_mode();
		refresh();
		return outcome;
	};
	hooks.edit_list = [this](const std::string& key) {
		def_prog_mode();
		endwin();
		auto outcome = editfile::Outcome::Unchanged;
		try {
			outcome = editfile::edit_list(store_, key);
		} catch (...) {
			reset_prog_mode();
			refresh();
			throw;
		}
		reset_prog_mode();
		refresh();
		return outcome;
	};
	std::ostringstream out;
	std::string error;
	bool cancelled = false;
	auto run = [&] {
		try {
			cmd::run(store_, words, out, hooks);
		} catch (const cmd::Cancelled&) {
			cancelled = true;
		} catch (const std::exception& e) {
			error = e.what();
		}
	};
	auto shown = shown_items();
	if (c->edits) {
		auto label = ":" + name;
		batch(label.c_str(), run);
	} else {
		run();
	}
	if (cancelled) {
		return true;
	}
	if (c->edits && error.empty()) {
		marked_.clear();
		// The selected reminder went (completed, moved, deleted): keep the
		// place by selecting the nearest one still there, as x does.
		auto now = shown_items();
		auto still = [&](const std::string& id) {
			return std::ranges::find(now, id) != now.end();
		};
		auto at = std::ranges::find(shown, item_sel_);
		if (at != shown.end() && !still(item_sel_)) {
			if (auto next = std::find_if(at, shown.end(), still);
				next != shown.end()) {
				item_sel_ = *next;
			} else if (auto prev = std::find_if(std::make_reverse_iterator(at),
												shown.rend(), still);
					   prev != shown.rend()) {
				item_sel_ = *prev;
			}
		}
		// Done and undone say nothing, like x; other edits say what they
		// did in the status bar (its first line: not the notes after it).
		if (name == "done" || name == "undone") {
			return true;
		}
		std::string first;
		std::getline(std::istringstream(out.str()) >> std::ws, first);
		message_ = first;
		return true;
	}
	// What it printed: a line in the status bar, more in a box.
	std::vector<std::string> lines;
	std::istringstream in(out.str());
	for (std::string l; std::getline(in, l);) {
		lines.push_back(l);
	}
	if (!error.empty()) {
		std::istringstream err(error);
		for (std::string l; std::getline(err, l);) {
			lines.push_back(l);
		}
		if (lines.size() == 1) {
			message_ = "Error: " + lines.front();
			return true;
		}
	}
	if (lines.size() == 1) {
		message_ = lines.front();
	} else if (!lines.empty()) {
		show_text(":" + line, lines);
	}
	return true;
}

void Tui::set_option(const std::string& word) {
	auto w = word;
	bool ask = w.ends_with('?'), toggle = w.ends_with('!');
	if (ask || toggle) {
		w.pop_back();
	}
	std::optional<std::string> value;
	if (auto eq = w.find('='); eq != std::string::npos) {
		value = w.substr(eq + 1);
		w.resize(eq);
	}
	bool no = false;
	if (std::ranges::find(kSettings, w) == kSettings.end() &&
		w.starts_with("no")) {
		w.erase(0, 2), no = true;
	}
	auto show = [&](const std::string& n, const std::string& v) {
		message_ = std::format("{}={}", n, v);
	};
	auto flag = [&](bool now) -> std::optional<bool> {
		if (ask) {
			return std::nullopt;
		}
		if (value) {
			auto b = parse_bool(*value);
			if (!b) {
				message_ = std::format("{} is on or off", w);
			}
			return b;
		}
		return toggle ? !now : !no;
	};
	auto save = [&](const char* key, const std::string& v) {
		try {
			rem::save_setting(key, v);
		} catch (const std::exception&) {
			// It just won't be remembered.
		}
	};
	if (w == "completed") {
		if (auto b = flag(show_completed_)) {
			show_completed_ = *b;
		}
		if (message_.empty()) {
			show(w, show_completed_ ? "on" : "off");
		}
	} else if (w == "sidebar") {
		if (auto b = flag(!hide_sidebar_)) {
			hide_sidebar_ = !*b;
			if (hide_sidebar_) {
				focus_items_ = true;
			}
			save("show-sidebar", *b ? "true" : "false");
		}
		if (message_.empty()) {
			show(w, hide_sidebar_ ? "off" : "on");
		}
	} else if (w == "key-numbers") {
		if (auto b = flag(show_key_numbers_)) {
			show_key_numbers_ = *b, key_numbers_override_ = *b;
			save("show-key-numbers", *b ? "true" : "false");
		}
		if (message_.empty()) {
			show(w, show_key_numbers_ ? "on" : "off");
		}
	} else if (w == "note-lines") {
		if (value && !ask) {
			std::size_t n = 0;
			auto [p, ec] = std::from_chars(value->data(),
										   value->data() + value->size(), n);
			if (ec != std::errc{} || p != value->data() + value->size()) {
				message_ = "note-lines is a number (0: all of them)";
				return;
			}
			note_lines_ = n;
			save("note-lines", *value);
		}
		show(w, note_lines_ == 0 ? "0 (all)" : std::to_string(note_lines_));
	} else {
		message_ = std::format("No setting called “{}” ({})", w, [] {
			std::string all;
			for (auto& s : kSettings) {
				all += (all.empty() ? "" : ", ") + s;
			}
			return all;
		}());
	}
}

std::vector<std::string> Tui::complete_command(const std::string& before,
											   std::size_t& begin) {
	auto words = rem::split_words(before, true);
	// The word being typed, or a new one after a space.
	bool fresh = words.empty() || words.back().end < before.size();
	if (fresh) {
		words.push_back({"", before.size(), before.size()});
	}
	begin = words.back().begin;
	auto& word = words.back().text;
	auto index = words.size() - 1;

	if (index == 0) {
		std::vector<std::string> names;
		for (auto& c : cmd::commands()) {
			if (c.where != cmd::Where::Cli) {
				for (auto n : c.names) {
					if (n.size() > 2) {
						names.emplace_back(n);
					}
				}
			}
		}
		return starting_with(names, word);
	}
	auto* c = cmd::find(words.front().text);
	if (!c) {
		return {};
	}
	auto kind = c->arg;
	// After an option that takes a value: what that value is.
	auto& prev = words[index - 1].text;
	if (prev == "-o") {
		kind = cmd::Arg::File;
	} else if (prev.starts_with("--") && cmd::takes_value(prev.substr(2))) {
		kind = cmd::option_arg(prev.substr(2));
	} else if (word.starts_with("-")) {
		std::vector<std::string> opts;
		for (auto& o : cmd::option_names()) {
			opts.push_back("--" + o);
		}
		return starting_with(opts, word);
	}

	auto list_names = [&] {
		std::vector<std::string> out;
		for (auto* l : store_.lists()) {
			out.push_back(store_.label(*l));
		}
		return out;
	};
	switch (kind) {
		case cmd::Arg::None:
			return {};
		case cmd::Arg::File:
			return files_for(word);
		case cmd::Arg::List:
			return starting_with(list_names(), word);
		case cmd::Arg::Source:
			{
				std::vector<std::string> names;
				if (c->names.front() == "sync") {
					names.push_back("all");
				}
				for (auto& s : store_.sources()) {
					names.push_back(s.config.name);
				}
				return starting_with(names, word);
			}
		case cmd::Arg::View:
			{
				std::vector<std::string> names;
				for (auto& v : sidebar_.all(true)) {
					names.push_back(entry_for(v).title);
				}
				return starting_with(names, word);
			}
		case cmd::Arg::Reminder:
			{
				// Titles; for move, the list it goes to too.
				std::vector<std::string> names;
				if (c->names.front() == "move") {
					names = list_names();
				}
				for (auto* l : store_.lists()) {
					l->doc.walk([&](rem::Reminder& r, rem::Reminder*) {
						if (!r.done) {
							names.push_back(r.title);
						}
					});
				}
				return starting_with(names, word);
			}
		case cmd::Arg::Command:
			{
				std::vector<std::string> names;
				for (auto& x : cmd::commands()) {
					if (x.where != cmd::Where::Cli) {
						names.emplace_back(x.names.front());
					}
				}
				return starting_with(names, word);
			}
		case cmd::Arg::Setting:
			{
				auto names = kSettings;
				for (auto& s : kSettings) {
					if (s != "note-lines") {
						names.push_back("no" + s);
					}
				}
				return starting_with(names, word);
			}
	}
	return {};
}

}  // namespace tui
