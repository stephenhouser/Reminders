#include "reminders/backend.hpp"

#include <algorithm>
#include <fstream>

#include "reminders/file_util.hpp"

namespace rem {

namespace {

using namespace detail;

class LocalBackend : public Backend {
	public:
		std::string_view id() const override { return "local"; }
};

}  // namespace

std::optional<std::string> Backend::list_name_for(const fs::path& file) const {
	auto fname = file.filename().string();
	if (fname.empty() || fname[0] == '.' || !fname.ends_with(".md")) {
		return std::nullopt;
	}
	auto stem = fname.substr(0, fname.size() - 3);
	if (stem.empty()) {
		return std::nullopt;
	}
	return stem;
}

std::vector<fs::path> Backend::conflict_copies(const fs::path&,
											   std::string_view) const {
	return {};
}
std::optional<std::string> Backend::read_base(std::string_view) const {
	return std::nullopt;
}
void Backend::write_base(std::string_view, const std::string&) const {}
void Backend::remember_written(std::string_view, const std::string&) const {}
bool Backend::is_own_write(std::string_view, const std::string&) const {
	return false;
}
void Backend::move_state(std::string_view, std::string_view) const {}
void Backend::drop_state(std::string_view) const {}

fs::path ServerBackend::records_dir() const { return state_dir_ / id_; }

void ServerBackend::move_state(std::string_view from,
							   std::string_view to) const {
	// Called with the write lock held; the next sync applies it.
	fs::create_directories(records_dir());
	std::ofstream(records_dir() / "renamed.tsv", std::ios::app)
		<< field(std::string(from)) << '\t' << field(std::string(to)) << '\n';
}

void ServerBackend::deleted_by_user(std::string_view name) const {
	fs::create_directories(records_dir());
	std::ofstream(records_dir() / "deleted.txt", std::ios::app)
		<< field(std::string(name)) << '\n';
}

std::unique_ptr<Backend> make_local_backend() {
	return std::make_unique<LocalBackend>();
}

}  // namespace rem
