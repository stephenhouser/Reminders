// The local back end: a folder on this computer; its files are read and
// written as they are (the core's plain-files back end). See README.md.
#include <format>

#include "reminders/backend_module.hpp"

namespace rem {

void register_local_backend() {
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
	register_backend(std::move(m));
}

}  // namespace rem
