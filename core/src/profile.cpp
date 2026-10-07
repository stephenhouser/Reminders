#include "reminders/profile.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <stdexcept>

#include "reminders/paths.hpp"
#include "reminders/settings.hpp"

namespace rem {

namespace {

constexpr std::string_view kDefault = "default";
constexpr const char* kProfilesDir = "profiles";

}  // namespace

bool valid_profile_name(std::string_view name) {
	if (name.empty() || name.size() > 64 || name.front() == '-' ||
		name == kDefault) {
		return false;
	}
	return std::ranges::all_of(name, [](char c) {
		return std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
			   c == '_';
	});
}

Profile::Profile(std::string_view name) {
	if (name.empty() || name == kDefault) {
		return;
	}
	if (!valid_profile_name(name)) {
		throw std::invalid_argument(std::format(
			"“{}” can't be a profile's name: use letters, digits, - and _",
			name));
	}
	name_ = name;
}

std::string Profile::name() const {
	return is_default() ? std::string(kDefault) : name_;
}

bool Profile::exists() const {
	std::error_code ec;
	return is_default() || fs::is_regular_file(settings_file(), ec);
}

fs::path Profile::settings_file() const {
	return is_default() ? config_dir() / "settings.ini"
						: config_dir() / kProfilesDir / (name_ + ".ini");
}

fs::path Profile::data_dir() const {
	return is_default() ? rem::data_dir()
						: rem::data_dir() / kProfilesDir / name_;
}

fs::path Profile::state_dir() const {
	return is_default() ? rem::state_dir()
						: rem::state_dir() / kProfilesDir / name_;
}

fs::path Profile::cache_dir() const {
	return is_default() ? rem::cache_dir()
						: rem::cache_dir() / kProfilesDir / name_;
}

std::vector<std::string> profile_names() {
	std::vector<std::string> out;
	std::error_code ec;
	for (auto& entry :
		 fs::directory_iterator(config_dir() / kProfilesDir, ec)) {
		auto& p = entry.path();
		if (p.extension() == ".ini" && entry.is_regular_file(ec) &&
			valid_profile_name(p.stem().string())) {
			out.push_back(p.stem().string());
		}
	}
	std::ranges::sort(out);
	return out;
}

Profile existing_profile(std::string_view name) {
	std::string there = std::string(kDefault);
	for (auto& n : profile_names()) {
		there += ", " + n;
	}
	if (!name.empty() && name != kDefault && !valid_profile_name(name)) {
		throw std::runtime_error(std::format(
			"“{}” can't be a profile's name (profiles: {})", name, there));
	}
	Profile p(name);
	if (!p.exists()) {
		throw std::runtime_error(
			std::format("no profile “{}” (profiles: {})", name, there));
	}
	return p;
}

Profile create_profile(std::string_view name,
					   const std::optional<Profile>& from) {
	if (!valid_profile_name(name)) {
		Profile check(name);  // throws, saying why
		throw std::invalid_argument(
			std::format("“{}” is the default profile's name", name));
	}
	Profile p(name);
	if (p.exists()) {
		throw std::runtime_error(
			std::format("there's already a profile “{}”", name));
	}
	auto file = p.settings_file();
	fs::create_directories(file.parent_path());
	std::error_code ec;
	if (from && fs::is_regular_file(from->settings_file(), ec)) {
		fs::copy_file(from->settings_file(), file);
	} else {
		std::ofstream out(file);
		out << "[general]\n";
		if (!out) {
			throw std::runtime_error(
				std::format("couldn't write {}", file.string()));
		}
	}
	return p;
}

namespace {

// A profile by name, or the default if it's gone or the name is bad.
Profile profile_or_default(std::string_view name) {
	if (!valid_profile_name(name)) {
		return {};
	}
	Profile p(name);
	return p.exists() ? p : Profile();
}

}  // namespace

StartProfile profile_on_start() {
	Profile def;
	std::string choice;
	for (char c : load_setting(def, "profile-on-start")) {
		choice +=
			static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (choice.empty() || choice == kDefault) {
		return {};
	}
	auto last = profile_or_default(load_setting(def, "last-profile"));
	if (choice == "last") {
		return {last};
	}
	if (choice == "ask") {
		return {last, !profile_names().empty()};
	}
	return {profile_or_default(load_setting(def, "profile-on-start"))};
}

void remember_profile(const Profile& profile) {
	Profile def;
	auto name = profile.name();
	if (load_setting(def, "last-profile") != name) {
		save_setting(def, "last-profile", name);
	}
}

}  // namespace rem
