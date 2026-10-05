#include "reminders/backend_module.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>

namespace rem {

namespace {

std::string lower(std::string_view s) {
	std::string out;
	for (char c : s) {
		out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

struct Registry {
		std::mutex mutex;
		std::vector<std::unique_ptr<BackendModule>> modules;
};

Registry& registry() {
	static Registry r;
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

// For a back end not built in: the folder's files as they are.
const BackendModule& plain_files() {
	static const BackendModule m = [] {
		BackendModule p;
		p.id = "local";
		p.title = "Local Folder";
		p.problem = [](const SourceConfig& s) {
			return folder_problem(s, true);
		};
		p.make_backend = [](const fs::path&) { return make_local_backend(); };
		return p;
	}();
	return m;
}

const BackendModule& backend_of(const SourceConfig& source) {
	if (auto* m = find_backend(source.backend)) {
		return *m;
	}
	return plain_files();
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
	return m && m->make_backend ? m->make_backend(state_dir)
								: make_local_backend();
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

}  // namespace rem
