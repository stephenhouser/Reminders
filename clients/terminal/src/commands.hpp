// The commands of `reminders COMMAND …`, shared by the CLI and the terminal
// interface's ":" prompt: one table of them, and a way to run one on a
// library that's already open, with its output going to a stream.
#pragma once

#include <functional>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "editfile.hpp"
#include "reminders/library.hpp"

namespace cmd {

// Where a command can be typed. The terminal interface runs some of the
// shared ones its own way (list and search change the view; sync starts
// the background sync).
enum class Where { Both, Cli, Tui };

// What a command's words are, for completing them on the ":" prompt.
enum class Arg { None, File, List, Source, View, Reminder, Command, Setting };

struct Command {
		std::vector<std::string_view> names;  // the first is its name
		std::string_view usage;				  // after the name
		std::string_view summary;			  // a line, for :help
		// For --help, when there's more to say than `summary`; "\n" starts
		// a new line.
		std::string_view help = "";
		bool edits = false;	 // it changes lists (the CLI syncs after it)
		Where where = Where::Both;
		Arg arg = Arg::None;
};

std::span<const Command> commands();
// The command `name` is one of the names of, or nullptr.
const Command* find(std::string_view name);
// Every --option a command takes, without the dashes.
std::vector<std::string> option_names();
// Options that take a value (`--list LIST`); the others are flags.
bool takes_value(std::string_view option);
// What the value of `--option` is, for completing it.
Arg option_arg(std::string_view option);

// What the caller provides when it isn't a plain terminal.
struct Hooks {
		// Asks on the terminal (stdin) when several reminders match a
		// NAME, before deleting, and opens $EDITOR for `edit NAME`.
		bool interactive = false;
		// The saved view (settings.ini) applies: this is the saved folder.
		bool remember = true;
		// Instead of the terminal: asks a yes / no question.
		std::function<bool(const std::string& question)> confirm;
		// Instead of the terminal: chooses one of several reminders a NAME
		// matches (an index into `options`), or nullopt to cancel.
		std::function<std::optional<std::size_t>(
			const std::string& question,
			const std::vector<std::string>& options)>
			choose;
		// Instead of the terminal: edits reminder `id` in $EDITOR.
		std::function<editfile::Outcome(const std::string& id)> edit;
		// Reminders (ids) that commands given no NAME act on: the marked
		// ones, else the selected one.
		std::vector<std::string> selected;
		// The list showing (a key), where `add` adds without --list.
		std::string list;
};

// Thrown when the user backs out of a question (Hooks::choose); nothing
// was changed, and there's nothing to say.
struct Cancelled : std::runtime_error {
		Cancelled() : std::runtime_error("cancelled") {}
};

// Runs `words` (the command, then its arguments, as on the command line).
// Returns the exit status; throws std::runtime_error for a mistake, with a
// message for the user.
int run(rem::Library& library, std::span<const std::string> words,
		std::ostream& out, const Hooks& hooks);

}  // namespace cmd
