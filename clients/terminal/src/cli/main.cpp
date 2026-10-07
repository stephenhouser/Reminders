#include "internal.hpp"

using namespace cli;

int main(int argc, char** argv) {
	rem::register_backends();  // the back ends built in
	std::vector<std::string> args(argv + 1, argv + argc);
	Global g;
	g.color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
	// --json and --no-color work anywhere on the line, not just before the
	// command.
	std::erase_if(args, [&](const std::string& s) {
		if (s == "--json") {
			return g.json = true;
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
	std::optional<std::string> profile_name;  // --profile
	std::size_t i = 0;
	for (; i < args.size(); ++i) {
		auto& s = args[i];
		if (s == "-h" || s == "--help") {
			std::cout << usage();
			return 0;
		}
		if (s == "--version") {
			std::cout << "reminders " << term::kVersion << "\n";
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
		} else if (s == "-P" || s == "--profile") {
			if (i + 1 >= args.size()) {
				std::cerr << "reminders: --profile needs a name\n";
				return 2;
			}
			profile_name = args[++i];
		} else if (s.starts_with("--profile=")) {
			profile_name = s.substr(10);
		} else {
			break;
		}
	}
	std::string cmd = i < args.size() ? args[i++] : "tui";
	std::vector<std::string> rest(args.begin() + static_cast<long>(i),
								  args.end());

	try {
		if (cmd == "new-profile") {
			return cmd_new_profile(parse_args(rest));
		}
		// Only the interactive interface asks which profile (with
		// profile-on-start=ask); a command uses the one last opened.
		auto profile = choose_profile(profile_name, cmd == "tui");
		if (cmd == "profiles") {
			return cmd_profiles(g, profile);
		}
		if (cmd == "folder") {
			if (rest.empty()) {
				auto f = rem::saved_folder(profile);
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
			auto source = rem::set_default_folder(profile, path);
			std::cout << std::format("Folder set to {} (source “{}”, {})\n",
									 source.folder.string(), source.name,
									 source.backend);
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
		auto library = rem::open_library(profile, folder, rem::device_name());
		if (library->sources().empty()) {
			throw std::runtime_error(std::format(
				"no folder: pass --folder PATH, or set one with `reminders "
				"{}folder PATH`",
				profile.is_default() ? ""
									 : "--profile " + profile.name() + " "));
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
		library->load_all();
		if (cmd == "tui") {
			if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
				throw UsageError(
					"the interactive interface needs a terminal; see reminders "
					"--help for commands");
			}
			try {
				rem::remember_profile(profile);	 // for profile-on-start=last
			} catch (const std::exception&) {
				// It just won't be remembered.
			}
			return run_tui(*library, own_folder);
		}
		if (auto* c = cmd::find(cmd); c && c->where == cmd::Where::Tui) {
			throw UsageError(std::format(
				"“{}” is a command of the interactive interface (type : "
				"there)",
				cmd));
		}
		cmd::Hooks hooks;
		hooks.interactive = true;
		hooks.remember = own_folder;
		App app(g, *library, std::cout, hooks);
		auto status = app.run(cmd, parse_args(rest));
		// A command that changes lists sends them back to the servers.
		if (auto* c = cmd::find(cmd); sync && c && c->edits) {
			sync_servers(*library);
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
