// For core tests of logic that depends on what a back end is (detecting a
// folder's back end, where its state goes) without the back-end modules:
// stand-ins registered under their ids.
#pragma once

#include "reminders/backend_module.hpp"

namespace test {

// A "syncthing" entry: picked for a folder in a Syncthing folder (a
// .stfolder at or above it), state kept in the folder.
inline void register_stand_in_syncthing() {
	rem::BackendModule m;
	m.id = "syncthing";
	m.title = "Syncthing";
	m.state_in_folder = true;
	m.detect_by_default = true;
	m.detect = [](const rem::fs::path& folder) {
		std::error_code ec;
		for (auto p = rem::fs::weakly_canonical(folder, ec); !p.empty();
			 p = p.parent_path()) {
			if (rem::fs::exists(p / ".stfolder", ec)) {
				return true;
			}
			if (p == p.parent_path()) {
				break;
			}
		}
		return false;
	};
	m.make_backend = [](const rem::fs::path&) {
		return rem::make_local_backend();
	};
	rem::register_backend(std::move(m));
}

}  // namespace test
