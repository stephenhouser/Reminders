#include "reminders/syncthing.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#include "reminders/backend_module.hpp"
#include "reminders/file_util.hpp"
#include "reminders/paths.hpp"

namespace rem {

std::optional<fs::path> syncthing_root(const fs::path& dir) {
	std::error_code ec;
	for (auto p = fs::weakly_canonical(dir, ec); !p.empty();
		 p = p.parent_path()) {
		if (fs::is_directory(p / ".stfolder", ec)) {
			return p;
		}
		if (p == p.parent_path()) {
			break;
		}
	}
	return std::nullopt;
}

bool ignore_state_in_syncthing(const fs::path& folder) {
	auto root = syncthing_root(folder);
	if (!root) {
		return false;
	}
	auto file = *root / ".stignore";

	std::string existing;
	if (std::ifstream in(file); in) {
		std::ostringstream ss;
		ss << in.rdbuf();
		existing = ss.str();
		std::istringstream lines(existing);
		for (std::string line; std::getline(lines, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line == kStateDirName ||
				line == std::string("(?d)") + kStateDirName) {
				return false;  // already ignored
			}
		}
	}

	std::ofstream out(file, std::ios::app);
	if (!existing.empty() && existing.back() != '\n') {
		out << '\n';
	}
	// Unanchored, so it matches wherever the Reminders folder sits inside
	// the Syncthing folder. "(?d)" lets Syncthing delete it when it would
	// otherwise stop the Reminders folder itself from being deleted.
	out << "// Reminders app: per-device state, not to be synced\n(?d)"
		<< kStateDirName << '\n';
	return static_cast<bool>(out);
}

namespace {

using namespace detail;

constexpr std::string_view kConflictMarker = ".sync-conflict-";

class SyncthingBackend : public Backend {
	public:
		explicit SyncthingBackend(fs::path state_dir)
			: state_dir_(std::move(state_dir)) {}
		std::string_view id() const override { return "syncthing"; }

		void prepare(const fs::path& folder) override {
			ignore_state_in_syncthing(folder);
		}

		std::optional<std::string> list_name_for(
			const fs::path& file) const override {
			auto name = Backend::list_name_for(file);
			if (!name) {
				return name;
			}
			if (auto at = name->find(kConflictMarker);
				at != std::string::npos) {
				name->resize(at);
			}
			if (name->empty()) {
				return std::nullopt;
			}
			return name;
		}

		std::vector<fs::path> conflict_copies(
			const fs::path& folder, std::string_view name) const override {
			std::vector<fs::path> out;
			std::error_code ec;
			auto prefix = std::string(name) + std::string(kConflictMarker);
			for (auto& e : fs::directory_iterator(folder, ec)) {
				auto fname = e.path().filename().string();
				if (fname.starts_with(prefix) && fname.ends_with(".md")) {
					out.push_back(e.path());
				}
			}
			std::ranges::sort(out);
			return out;
		}

		// Per list, in the per-device state folder:
		//   base/<list>.md  the last version that came from another device
		//   (merge base) written/<list>  a fingerprint of the last version this
		//   device wrote
		std::optional<std::string> read_base(
			std::string_view name) const override {
			return read_file(base_path(name));
		}

		void write_base(std::string_view name,
						const std::string& text) const override {
			fs::create_directories(base_path(name).parent_path());
			write_atomic(base_path(name), text);
		}

		void remember_written(std::string_view name,
							  const std::string& text) const override {
			fs::create_directories(written_path(name).parent_path());
			write_atomic(written_path(name), fingerprint(text));
		}

		bool is_own_write(std::string_view name,
						  const std::string& text) const override {
			return read_file(written_path(name)) == fingerprint(text);
		}

		void move_state(std::string_view from,
						std::string_view to) const override {
			std::error_code ec;
			for (auto [a, b] :
				 {std::pair{base_path(from), base_path(to)},
				  std::pair{written_path(from), written_path(to)}}) {
				if (fs::exists(a, ec)) {
					fs::rename(a, b, ec);
				}
			}
		}

		void drop_state(std::string_view name) const override {
			std::error_code ec;
			fs::remove(base_path(name), ec);
			fs::remove(written_path(name), ec);
		}

	private:
		fs::path state_dir_;
		fs::path base_path(std::string_view name) const {
			return state_dir_ / "base" / (std::string(name) + ".md");
		}
		fs::path written_path(std::string_view name) const {
			return state_dir_ / "written" / std::string(name);
		}
};

}  // namespace

void register_syncthing_backend() {
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
		return std::make_unique<SyncthingBackend>(state);
	};
	register_backend(std::move(m));
}

}  // namespace rem
