// Profiles from the command line: which one a run uses, `reminders
// profiles` and `reminders new-profile`.
#include "internal.hpp"

namespace cli {

namespace {

// Asks on the terminal which profile to open, `fallback` being the answer
// to Enter.
rem::Profile ask_profile(const rem::Profile& fallback) {
	std::vector<rem::Profile> all{rem::Profile()};
	for (auto& n : rem::profile_names()) {
		all.emplace_back(n);
	}
	std::cerr << "Which profile?\n";
	for (std::size_t i = 0; i < all.size(); ++i) {
		std::cerr << std::format("  {}) {}\n", i + 1, all[i].name());
	}
	std::cerr << std::format("Which one? [1-{}, Enter: {}] ", all.size(),
							 fallback.name())
			  << std::flush;
	std::string answer;
	if (!std::getline(std::cin, answer)) {
		throw std::runtime_error("nothing chosen");
	}
	if (answer.find_first_not_of(" \t") == std::string::npos) {
		return fallback;
	}
	std::size_t n = 0;
	auto [p, ec] =
		std::from_chars(answer.data(), answer.data() + answer.size(), n);
	if (ec == std::errc{} && n >= 1 && n <= all.size()) {
		return all[n - 1];
	}
	// A name works too.
	for (auto& profile : all) {
		if (profile.name() == answer) {
			return profile;
		}
	}
	throw std::runtime_error("nothing chosen");
}

}  // namespace

rem::Profile choose_profile(const std::optional<std::string>& named,
							bool interactive) {
	if (named) {
		return rem::existing_profile(*named);
	}
	if (const char* env = std::getenv("REMINDERS_PROFILE"); env && *env) {
		return rem::existing_profile(env);
	}
	auto start = rem::profile_on_start();
	if (start.ask && interactive && isatty(STDIN_FILENO) &&
		isatty(STDERR_FILENO)) {
		return ask_profile(start.profile);
	}
	return start.profile;
}

int cmd_profiles(const Global& g, const rem::Profile& current) {
	std::vector<rem::Profile> all{rem::Profile()};
	for (auto& n : rem::profile_names()) {
		all.emplace_back(n);
	}
	auto sources = [](const rem::Profile& p) {
		std::vector<std::string> out;
		for (auto& s : rem::load_sources(p)) {
			out.push_back(s.name);
		}
		return out;
	};
	if (g.json) {
		std::string out = "[";
		for (auto& p : all) {
			std::string names;
			for (auto& s : sources(p)) {
				names += (names.empty() ? "" : ", ") + json_escape(s);
			}
			out += std::format(
				"{}\n  {{\"name\": {}, \"current\": {}, \"file\": {}, "
				"\"sources\": [{}]}}",
				out.size() > 1 ? "," : "", json_escape(p.name()),
				p == current ? "true" : "false",
				json_escape(p.settings_file().string()), names);
		}
		std::cout << out << "\n]\n";
		return 0;
	}
	std::size_t width = 0;
	for (auto& p : all) {
		width = std::max(width, p.name().size());
	}
	Style st{g.color};
	for (auto& p : all) {
		std::string names;
		for (auto& s : sources(p)) {
			names += (names.empty() ? "" : ", ") + s;
		}
		std::cout << std::format(
			"{} {}{:<{}}{}  {}{}{}\n", p == current ? "*" : " ", st.bold(),
			p.name(), width, st.reset(), st.dim(),
			names.empty() ? "(no sources)" : names, st.reset());
	}
	return 0;
}

int cmd_new_profile(const Args& a) {
	if (a.positional.size() != 1) {
		throw UsageError("usage: reminders new-profile NAME [--from PROFILE]");
	}
	std::optional<rem::Profile> from;
	if (auto f = a.get("from")) {
		from = rem::existing_profile(*f);
	}
	auto profile = rem::create_profile(a.positional[0], from);
	std::cout << std::format("Created profile “{}” ({})\n", profile.name(),
							 profile.settings_file().string());
	if (!from) {
		std::cout << std::format(
			"Give it a folder with: reminders --profile {} folder PATH\n",
			profile.name());
	}
	return 0;
}

}  // namespace cli
