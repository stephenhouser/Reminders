#include "reminders/backend_module.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "reminders/syncthing.hpp"

namespace rem {

namespace {

std::string lower(std::string_view s) {
	std::string out;
	for (char c : s) {
		out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

std::string folder_problem(const SourceConfig& s, bool must_exist) {
	std::error_code ec;
	if (s.folder.empty()) {
		return "Choose a folder";
	}
	if (must_exist && !fs::is_directory(s.folder, ec)) {
		return "That folder doesn't exist";
	}
	return {};
}

std::string server_problem(const SourceConfig& s) {
	auto url = s.option("url");
	if (url.empty()) {
		return "Enter the server's address";
	}
	if (!url.starts_with("https://") && !url.starts_with("http://")) {
		return "The address starts with https://";
	}
	if (url.find('.', url.find("://")) == std::string::npos &&
		url.find("localhost") == std::string::npos &&
		url.find("127.0.0.1") == std::string::npos) {
		return "That doesn't look like a server's address";
	}
	return {};
}

// The host's second-to-last label: https://caldav.fastmail.com/ → fastmail.
std::string host_name(const SourceConfig& s) {
	auto url = s.option("url");
	if (url.empty()) {
		return {};
	}
	auto start = url.find("://");
	auto host = url.substr(start == std::string::npos ? 0 : start + 3);
	host = host.substr(0, host.find_first_of("/:"));
	auto last = host.rfind('.');
	if (last != std::string::npos && last > 0) {
		auto prev = host.rfind('.', last - 1);
		auto from = prev == std::string::npos ? 0 : prev + 1;
		host = host.substr(from, last - from);
	}
	return host;
}

// The repository's name: …/notes.git or …:you/notes → notes.
std::string repository_name(const SourceConfig& s) {
	auto url = s.option("url");
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	if (url.empty()) {
		return {};
	}
	auto cut = url.find_last_of("/:");
	auto name = url.substr(cut == std::string::npos ? 0 : cut + 1);
	if (name.ends_with(".git")) {
		name.resize(name.size() - 4);
	}
	return name;
}

std::vector<SettingField> server_fields(bool webdav) {
	return {
		{"url", webdav ? "Folder Address" : "Server",
		 webdav
			 ? "The folder's address, e.g. "
			   "https://cloud.example.com/remote.php/dav/files/you/Reminders/"
			 : "The server's address, e.g. https://caldav.fastmail.com/",
		 SettingField::Url, "Server"},
		{"username", "Username", "", SettingField::Text, "Server"},
		{"password-command", "Password Command",
		 "A command that prints the password, e.g. “secret-tool lookup service "
		 "reminders-caldav” or “pass show "
		 "caldav”",
		 SettingField::Command, "Server"},
		{"interval", "Sync Every",
		 "Minutes; changes made here are sent straight away",
		 SettingField::Minutes, "Server", 15},
	};
}

BackendModule syncthing() {
	BackendModule m;
	m.id = "syncthing";
	m.title = "Syncthing";
	m.description =
		"A folder Syncthing keeps in step; conflict copies are merged";
	m.state_in_folder = true;
	m.detect_by_default = true;
	m.detect = [](const fs::path& folder) {
		return syncthing_root(folder).has_value();
	};
	m.problem = [](const SourceConfig& s) { return folder_problem(s, true); };
	m.erase_note = [](const SourceConfig&, const std::string& where) {
		return std::format(
			"Deletes {} and everything in it from this computer. WARNING: If "
			"Syncthing still shares the folder, it "
			"WILL BE DELETED EVERYWHERE. Remove it from Syncthing first.",
			where);
	};
	m.make_backend = [](const fs::path& state) {
		return make_syncthing_backend(state);
	};
	return m;
}

BackendModule local() {
	BackendModule m;
	m.id = "local";
	m.title = "Local Folder";
	m.description =
		"A folder on this computer; files are read and saved as they are";
	m.problem = [](const SourceConfig& s) { return folder_problem(s, true); };
	m.erase_note = [](const SourceConfig&, const std::string& where) {
		return std::format(
			"Deletes {} and everything in it. This computer may have the only "
			"copy.",
			where);
	};
	m.make_backend = [](const fs::path&) { return make_local_backend(); };
	return m;
}

BackendModule dav(bool webdav) {
	BackendModule m;
	m.id = webdav ? "webdav" : "caldav";
	m.title = webdav ? "WebDAV" : "CalDAV";
	m.description =
		webdav
			? "List files in a folder on a WebDAV server, with a copy kept here"
			: "Task lists on a CalDAV server, with a copy kept here";
	m.has_server = true;
	m.synced = true;
	m.owns_folder = true;
	m.fields = server_fields(webdav);
	m.settings_note = webdav ? "The list files are kept in a folder on the "
							   "server (Nextcloud, ownCloud, a NAS, …), made "
							   "if it isn't there."
							 : "Each of the account's task lists becomes a "
							   "list here (Nextcloud, Fastmail, Radicale, …).";
	m.problem = server_problem;
	m.name_hint = host_name;
	m.erase_note = [](const SourceConfig&, const std::string& where) {
		return std::format(
			"Deletes the local copy in {}. The lists on the server are "
			"unaffected.",
			where);
	};
	m.make_backend = [id = m.id](const fs::path& state) {
		return std::make_unique<ServerBackend>(id, state);
	};
	return m;
}

BackendModule git() {
	BackendModule m;
	m.id = "git";
	m.title = "Git";
	m.description =
		"A folder in a git repository; changes are committed, pulled and "
		"pushed";
	m.synced = true;
	m.owns_folder = true;
	m.fields = {
		{"url", "Clone From",
		 "Optional: the repository to clone (or push to) when the folder isn't "
		 "one yet, e.g. "
		 "git@github.com:you/notes.git. Without it, a folder that isn't a "
		 "repository becomes a new one",
		 SettingField::Url, "Repository"},
		{"remote", "Remote", "Optional; origin if empty", SettingField::Text,
		 "Repository"},
		{"branch", "Branch", "Optional; the branch checked out if empty",
		 SettingField::Text, "Repository"},
		{"interval", "Sync Every",
		 "Minutes; changes made here are sent straight away",
		 SettingField::Minutes, "Repository", 15},
	};
	m.settings_note =
		"Changed lists are committed and pushed, and changes from elsewhere "
		"pulled and merged. Signing in uses your "
		"git set-up (SSH keys, a credential helper).";
	m.detect = [](const fs::path& folder) { return in_git_repo(folder); };
	m.problem = [](const SourceConfig& s) { return folder_problem(s, false); };
	m.name_hint = repository_name;
	m.erase_note = [](const SourceConfig&, const std::string& where) {
		return std::format(
			"Deletes the local copy in {}. Any remote source is unaffected. "
			"Any changes not pushed yet are lost.",
			where);
	};
	m.make_backend = [](const fs::path& state) {
		return std::make_unique<ServerBackend>("git", state);
	};
	return m;
}

struct Registry {
		std::mutex mutex;
		std::vector<std::unique_ptr<BackendModule>> modules;
};

Registry& registry() {
	static Registry r;
	static std::once_flag builtins;
	std::call_once(builtins, [] {
		for (auto m : {syncthing(), local(), dav(false), dav(true), git()}) {
			r.modules.push_back(std::make_unique<BackendModule>(std::move(m)));
		}
	});
	return r;
}

}  // namespace

void register_backend(BackendModule module) {
	auto& r = registry();
	std::lock_guard g(r.mutex);
	for (auto& m : r.modules) {
		if (m->id == module.id) {
			*m = std::move(module);
			return;
		}
	}
	r.modules.push_back(std::make_unique<BackendModule>(std::move(module)));
}

BackendModule* find_backend(std::string_view id) {
	auto& r = registry();
	std::lock_guard g(r.mutex);
	auto want = lower(id);
	for (auto& m : r.modules) {
		if (m->id == want) {
			return m.get();
		}
	}
	return nullptr;
}

const BackendModule& backend_of(const SourceConfig& source) {
	if (auto* m = find_backend(source.backend)) {
		return *m;
	}
	return *find_backend("syncthing");
}

std::vector<const BackendModule*> backends() {
	auto& r = registry();
	std::lock_guard g(r.mutex);
	std::vector<const BackendModule*> out;
	for (auto& m : r.modules) {
		out.push_back(m.get());
	}
	return out;
}

std::unique_ptr<Backend> make_backend(std::string_view id,
									  const fs::path& state_dir) {
	auto* m = find_backend(id);
	if (!m || !m->make_backend) {
		m = find_backend("syncthing");
	}
	return m->make_backend(state_dir);
}

}  // namespace rem
