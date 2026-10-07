// reminders: the command-line interface (and, with no command, the TUI).
//
// Internal to the CLI's files (clients/terminal/src/cli/): the shared types,
// the App class with a method per command, and the helpers they use.
#pragma once

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

#include "../commands.hpp"
#include "../editfile.hpp"
#include "../text.hpp"
#include "../tui.hpp"
#include "../version.hpp"
#include "reminders/actions.hpp"
#include "reminders/backend_module.hpp"
#include "reminders/dates.hpp"
#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/import_file.hpp"
#include "reminders/importer.hpp"
#include "reminders/library.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/store.hpp"
#include "reminders/sync_runner.hpp"

namespace cli {

extern const rem::Library* g_library;
extern const std::vector<std::string> kValued;
extern const std::vector<std::string> kFlags;

// --help: this, the commands (from cmd::commands()), then kUsageEnd.
constexpr const char* kUsage =
	R"(Usage: reminders [--folder PATH] [--json] [--no-color] [COMMAND …]

With no command, opens the interactive (terminal) interface.

Commands:
)";

// After the commands (cmd::commands()) in --help.
constexpr const char* kUsageEnd =
	R"(
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

class App {
	public:
		// `library`: every source, or the --folder one (see rem::open_library),
		// loaded. Output goes to `out`; `hooks` say how to ask questions and
		// what a command without a NAME acts on (see commands.hpp).
		App(Global g, rem::Library& library, std::ostream& out,
			cmd::Hooks hooks)
			: g_(g),
			  st_{g.color},
			  store_(library),
			  out_(out),
			  hooks_(std::move(hooks)),
			  today_(rem::local_today()) {
			g_library = &store_;
		}

		int run(const std::string& cmd, const Args& a);

	private:
		Global g_;
		Style st_;
		rem::Library& store_;
		std::ostream& out_;
		cmd::Hooks hooks_;
		rem::Date today_;

		rem::ListFile& list_named(const std::string& name);
		term::SavedView saved_view();
		rem::ListFile& default_list();
		// Finds a reminder by name (or id); `in` limits the search to one list.
		rem::Ref resolve(const std::string& text,
						 const std::optional<std::string>& in);
		// The reminders `name` means: the one it names, else (no name) the
		// ones the caller has selected. `what` is for the error without
		// either: "done needs the name of a reminder".
		std::vector<rem::Ref> targets(const std::string& name, const Args& a,
									  const std::string& what);
		void apply_fields(rem::Reminder& r, const Args& a);
		void report(const std::string& verb, const rem::Ref& ref);
		// One reminder as report() shows it; several, as how many.
		void report_all(const std::string& verb,
						const std::vector<std::string>& ids);

		int cmd_lists();
		int cmd_list(const Args& a);
		int cmd_show(const Args& a);
		int cmd_add(const Args& a);
		int cmd_edit(const Args& a);
		int cmd_edit_list(const Args& a);
		int cmd_done(const Args& a, bool done);
		int cmd_move(const Args& a);
		int cmd_delete(const Args& a);
		int cmd_search(const Args& a);
		int cmd_new_list(const Args& a);
		int cmd_import(const Args& a);
		int cmd_export(const Args& a);
};

bool sync_servers([[maybe_unused]] rem::Library& library,
				  [[maybe_unused]] const std::string& only = "",
				  [[maybe_unused]] bool verbose = false);
rem::fs::path folder_arg(const std::string& arg);
bool has_servers(const rem::Library& library);
std::string json_escape(std::string_view s);
std::string list_label(const rem::ListFile& l);
std::string json_reminder(const rem::Ref& ref);
void print_json(std::ostream& out, const std::vector<rem::Ref>& refs);
void print_reminder(std::ostream& out, const rem::Ref& ref, const Style& st,
					int indent, bool show_list, rem::Date today);
void print_heading(std::ostream& out, int level, const std::string& text,
				   std::optional<term::Rgb> color, const Style& st);
Args parse_args(std::span<const std::string> in);
std::string join(const std::vector<std::string>& v, std::size_t from = 0);
// The whole of --help.
std::string usage();

}  // namespace cli
