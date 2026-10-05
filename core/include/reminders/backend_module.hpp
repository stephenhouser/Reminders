// The back ends there are, each described in one place: what a source of
// that kind is, its settings, how its folder is found and checked, the
// Backend that handles its files, and (for the ones the app syncs) how it
// syncs. Everything else asks the registry instead of knowing the back ends:
// the settings file, the Add Source form, the sources list, Remove, sync.
//
// The built-in back ends (syncthing, local, caldav, webdav, git) register
// themselves the first time the registry is used; the network library
// attaches the sync of caldav, webdav and git (register_network_backends).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/backend.hpp"
#include "reminders/sources.hpp"

namespace rem {

class Store;

// One of a back end's own settings, kept in its [source.NAME] section and
// shown as a row of the Add Source form.
struct SettingField {
		std::string key;	// "url" (url=)
		std::string label;	// "Address"
		std::string hint;	// tooltip / placeholder
		enum Kind { Text, Url, Command, Minutes } kind = Text;
		std::string group;	// the form's group for it: "Server", "Repository"
		int fallback = 0;	// Minutes: the value when unset
};

struct BackendModule {
		std::string id;			  // in settings.ini: backend=git
		std::string title;		  // "Git"
		std::string description;  // the line under the Type row

		bool has_server =
			false;	// the folder is a local copy of a server's (CalDAV, WebDAV)
		bool synced =
			false;	// synced by the app (sync attached by the network library)
		bool owns_folder =
			false;	// without folder=, $XDG_DATA_HOME/reminders/ID/NAME (made
					// when opened)
		bool state_in_folder =
			false;	// per-device records in <folder>/.reminders/<device>
					// (Syncthing)
		bool detect_by_default = false;	 // picked for a folder from the command
										 // line when `detect` matches
		std::vector<SettingField> fields;
		std::string
			settings_note;	// the line under its settings' group in the form

		// Whether a folder is one of this kind (a Syncthing folder, a git
		// repository): Add Source picks the type when one is chosen.
		std::function<bool(const fs::path& folder)> detect;
		// What's wrong with a source of this kind in Add Source, or "".
		std::function<std::string(const SourceConfig&)> problem;
		// A name for a new source's section, from its settings ("" if none).
		std::function<std::string(const SourceConfig&)> name_hint;
		// The Remove dialog's note for Erase all source data; `where` is the
		// folder as shown.
		std::function<std::string(const SourceConfig&,
								  const std::string& where)>
			erase_note;
		// The Backend for an open source of this kind.
		std::function<std::unique_ptr<Backend>(const fs::path& state_dir)>
			make_backend;
		// Syncs a source of this kind (network library); empty: not synced
		// in this build.
		std::function<SyncResult(Store& store, const SourceConfig& source)>
			sync;

		// Synced by the app, and this build can.
		bool syncs() const { return synced && bool(sync); }
};

// Adds a back end, or replaces the one with its id.
void register_backend(BackendModule module);
// By id, ignoring case; nullptr if there's none.
BackendModule* find_backend(std::string_view id);
// A source's back end (an unknown backend= is syncthing's).
const BackendModule& backend_of(const SourceConfig& source);
// Every back end, in the order the Add Source form lists them.
std::vector<const BackendModule*> backends();

// The Backend for a source of back end `id` (unknown: syncthing).
std::unique_ptr<Backend> make_backend(std::string_view id,
									  const fs::path& state_dir);

// Shorthands for a source's back end.
inline bool has_server(const SourceConfig& s) {
	return backend_of(s).has_server;
}
inline bool syncs(const SourceConfig& s) { return backend_of(s).syncs(); }

}  // namespace rem
