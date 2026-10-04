#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

#include "dialogs.hpp"
#include "reminders/clipboard.hpp"
#include "reminders/exporter.hpp"
#include "reminders/format.hpp"
#include "reminders/importer.hpp"
#include "reminders/paths.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/syncthing.hpp"
#include "support.hpp"
#include "window_internal.hpp"

namespace ui {

void Window::choose_folder() {
	auto* dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Choose Folder");
	if (store_ && !store_->sources().empty()) {
		auto current = Obj<GFile>::adopt(g_file_new_for_path(
			store_->sources().front().config.folder.c_str()));
		gtk_file_dialog_set_initial_folder(dialog, current.get());
	}
	gtk_file_dialog_select_folder(
		dialog, GTK_WINDOW(window_), nullptr,
		[](GObject* source, GAsyncResult* res, gpointer) {
			auto* self = static_cast<Window*>(
				g_object_get_data(G_OBJECT(source), "window"));
			GError* error = nullptr;
			auto file = Obj<GFile>::adopt(gtk_file_dialog_select_folder_finish(
				GTK_FILE_DIALOG(source), res, &error));
			if (error) {
				g_error_free(error);  // cancelled
				return;
			}
			auto path = take_string(g_file_get_path(file.get()));
			if (path.empty()) {
				self->toast("That folder isn't on a local disk");
				return;
			}
			self->open_folder(path);
		},
		nullptr);
	g_object_set_data(G_OBJECT(dialog), "window", this);
	g_object_unref(dialog);
}

void Window::open_folder(const std::filesystem::path& folder) {
	// A configured source's folder becomes the default; any other folder
	// becomes the default source's folder (created if there's none).
	try {
		auto source = rem::source_for_folder(folder);
		if (source.name.empty()) {
			rem::set_default_folder(folder);
		} else {
			rem::save_setting("default-source", source.name);
		}
	} catch (const std::exception& e) {
		toast(std::format("Couldn't save the folder: {}", e.what()));
	}
	open_sources();
}

// Removes a source from the app (settings.ini), after asking. Its folder and
// files stay as they are.
// Whether erasing `folder` would take more than one source's data with it:
// the filesystem root, the home folder, or a folder holding the home folder.
static bool unsafe_to_erase(const std::filesystem::path& folder) {
	std::error_code ec;
	auto target = std::filesystem::weakly_canonical(folder, ec);
	if (ec || target.empty() || target == target.root_path()) {
		return true;
	}
	auto home = std::filesystem::weakly_canonical(g_get_home_dir(), ec);
	for (auto p = home; !p.empty(); p = p.parent_path()) {
		if (p == target) {
			return true;
		}
		if (p == p.parent_path()) {
			break;
		}
	}
	return false;
}

// Removes a source, after asking. "Erase all source data" also erases what
// it has on this computer: its whole folder (a CalDAV / WebDAV local copy,
// a git clone, a list folder) and the app's records for it. Remote copies
// (the server, the git remote) are left as they are.
void Window::remove_source(const std::string& name) {
	const rem::Library::Source* source = nullptr;
	for (auto& s : store_->sources()) {
		if (s.config.name == name) {
			source = &s;
		}
	}
	if (!source) {
		return;
	}
	auto config = source->config;
	bool server = rem::has_server(config.backend);
	auto where = rem::contract_path(config.folder);
	std::string detail;
	switch (config.backend) {
		case rem::BackendKind::Caldav:
		case rem::BackendKind::Webdav:
			detail = std::format(
				"Deletes the local copy in {}. The lists on the server are "
				"unaffected.",
				where);
			break;
		case rem::BackendKind::Git:
			detail = std::format(
				"Deletes the local copy in {}. Any remote source is "
				"unaffected.",
				"Any changes not pushed yet are lost.", where);
			break;
		case rem::BackendKind::Syncthing:
			detail = std::format(
				"Deletes {} and everything in it from this computer.",
				"WARNING: If Syncthing still shares the folder, ",
				"it WILL BE DELETED EVERYWHERE.",
				"Remove it from Syncthing first.", where);
			break;
		default:
			detail = std::format(
				"Deletes {} and everything in it. This computer may have the "
				"only copy.",
				where);
			break;
	}

	auto* dialog = adw_alert_dialog_new(
		std::format("Remove “{}”?", rem::source_title(config)).c_str(),
		"The source and its lists will be removed. You can add it again with "
		"Add Source.");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
								   "_Cancel", "remove", "_Remove", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove",
											 ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	auto* box = vbox(4);
	auto* check =
		gtk_check_button_new_with_mnemonic("_Delete all source's data");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
								server);  // only a copy of the server's
	auto* note = label(detail, {"caption", "dim-label"});
	gtk_label_set_wrap(GTK_LABEL(note), TRUE);
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_widget_set_margin_start(note, 28);	// under the check box's label
	append(box, {check, note});
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, name, folder = config.folder, check](AdwAlertDialog*,
													const char* response) {
			if (std::string_view(response) != "remove") {
				return;
			}
			bool erase = gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
			if (erase && unsafe_to_erase(folder)) {
				toast(std::format(
					"Not erasing {}: it holds more than this source",
					rem::contract_path(folder)));
				return;
			}
			try {
				rem::remove_source(name,
								   !erase);	 // this computer's records; a DAV
											 // copy in the default place
			} catch (const std::exception& e) {
				toast(std::format("Couldn't remove the source: {}", e.what()));
				return;
			}
			// Erased once the source is closed and its syncing stopped.
			idle([this, folder, erase] {
				open_sources();
				if (!erase) {
					return;
				}
				std::error_code ec;
				std::filesystem::remove_all(folder, ec);
				if (ec) {
					toast(std::format("Couldn't erase all of {}: {}",
									  rem::contract_path(folder),
									  ec.message()));
				}
			});
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::source_info(const std::string& name) {
	std::optional<rem::SourceConfig> config;
	for (auto& s : rem::load_sources()) {
		if (s.name == name) {
			config = s;
		}
	}
	if (!config) {
		return;
	}
	SourceEdit edit{config->name,
					config->title,
					config->backend,
					config->folder,
					rem::load_setting("default-source") == name ||
						(store_ && store_->default_source() == name),
					config->dav,
					config->git,
					false};
	show_source_dialog(
		window_, edit,
		[name](const SourceEdit& e) { return source_problem(e, name); },
		[this](SourceEdit e) {
			try {
				rem::save_source(rem::SourceConfig{e.name, e.backend, e.folder,
												   e.title, e.dav, e.git});
				if (e.title.empty()) {
					rem::save_section_setting("source." + e.name, "title", "");
				}
				if (e.is_default) {
					rem::save_setting("default-source", e.name);
				}
			} catch (const std::exception& err) {
				toast(std::format("Couldn't save the source: {}", err.what()));
				return;
			}
			open_sources();
		},
		[this, name] { idle([this, name] { remove_source(name); }); });
}

void Window::show_sources() {
	std::vector<SourceRow> rows;
	for (auto& s : rem::load_sources()) {
		auto folder =
			rem::contract_path(s.folder);  // ~/… for folders in the home folder
		auto detail = s.backend == rem::BackendKind::Caldav
						? std::format("CalDAV · {}", s.dav.url)
					: s.backend == rem::BackendKind::Webdav
						? std::format("WebDAV · {}", s.dav.url)
					: s.backend == rem::BackendKind::Git
						? std::format("Git · {}", folder)
						: std::format("{} · {}",
									  s.backend == rem::BackendKind::Local
										  ? "Local folder"
										  : "Syncthing",
									  folder);
		rows.push_back({s.name, rem::source_title(s), detail});
	}
	show_sources_dialog(
		window_, rows,
		[this](std::string name) { idle([this, name] { source_info(name); }); },
		[this] { idle([this] { add_source(); }); });
}

void Window::add_source() {
	SourceEdit edit;
	edit.is_new = true;
	edit.is_default = rem::load_sources().empty();
	show_source_dialog(
		window_, edit,
		[](const SourceEdit& e) { return source_problem(e, ""); },
		[this](SourceEdit e) {
			try {
				if (!rem::has_server(e.backend)) {
					std::filesystem::create_directories(e.folder);
				}
				auto config = rem::add_source(rem::SourceConfig{
					"", e.backend, e.folder, e.title, e.dav, e.git});
				if (e.is_default) {
					rem::save_setting("default-source", config.name);
				}
				toast(
					rem::syncs(e.backend)
						? std::format(
							  "Added “{}”; its lists appear once it has synced",
							  rem::source_title(config))
						: std::format("Added “{}”", rem::source_title(config)));
			} catch (const std::exception& err) {
				toast(std::format("Couldn't add the source: {}", err.what()));
				return;
			}
			open_sources();
		},
		nullptr);
}

void Window::start_sync() {
#ifdef REMINDERS_NETWORK
	stop_sync();
	if (!store_) {
		return;
	}
	sync_ = std::make_unique<rem::SyncRunner>(*store_);
	g_simple_action_set_enabled(sync_action_, sync_->active());
	if (!sync_->active()) {
		return;
	}
	sync_timer_ = timeout(1000, [this] {
		auto status = sync_->take_status();
		// The same problem every few minutes (offline, say) is shown once.
		if (!status.errors.empty() &&
			status.errors.back() != last_sync_error_) {
			last_sync_error_ = status.errors.back();
			toast("Couldn't sync " + last_sync_error_);
		}
		if (status.errors.empty() && status.last_sync) {
			last_sync_error_.clear();
		}
		return true;
	});
#endif
}

void Window::stop_sync() {
#ifdef REMINDERS_NETWORK
	if (sync_timer_) {
		g_source_remove(sync_timer_);
	}
	sync_timer_ = 0;
	sync_.reset();	// waits for a sync under way
#endif
}

void Window::stop_watching() {
	for (auto& w : monitors_) {
		g_signal_handler_disconnect(w.monitor.get(), w.handler);
		g_file_monitor_cancel(w.monitor.get());
	}
	monitors_.clear();
}

void Window::open_sources(std::optional<std::filesystem::path> folder) {
	std::unique_ptr<rem::Library> library;
	try {
		library = rem::open_library(folder, device_name());
		library->load_all();
	} catch (const std::exception& e) {
		toast(std::format("Couldn't open the lists: {}", e.what()));
		return;
	}
	stop_sync();
	stop_watching();
	store_ = std::move(library);
	history_.clear();  // steps refer to the old lists
	update_undo_actions();
	sidebar_ = std::make_unique<rem::Sidebar>(*store_);
	// A folder that isn't a configured source is for this session only: the
	// saved view belongs to the configured ones.
	bool remember = !folder || (!store_->sources().empty() &&
								!store_->sources().front().config.name.empty());
	remember_view_ = remember;
	if (store_->sources().empty()) {
		gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "welcome");
		return;
	}

	// Changes made elsewhere (Syncthing, an editor, the TUI) show up.
	for (auto& source : store_->sources()) {
		auto dir = Obj<GFile>::adopt(
			g_file_new_for_path(source.config.folder.c_str()));
		GError* error = nullptr;
		FolderWatch w;
		w.monitor = Obj<GFileMonitor>::adopt(g_file_monitor_directory(
			dir.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, &error));
		if (error) {
			toast(
				std::format("Changes to “{}” from elsewhere won't show until "
							"restart: can't watch its folder",
							rem::source_title(source.config)));
			g_error_free(error);
			continue;
		}
		w.handler =
			connect<void(GFileMonitor*, GFile*, GFile*, GFileMonitorEvent)>(
				w.monitor.get(), "changed",
				[this](GFileMonitor*, GFile* file, GFile* other,
					   GFileMonitorEvent) { on_file_changed(file, other); });
		monitors_.push_back(std::move(w));
	}
	start_sync();

	gtk_stack_set_visible_child_name(GTK_STACK(main_stack_), "main");
	view_ = remember ? rem::view_from_string(load_last_view()) : home_view();
	if (view_.kind ==
		View::List) {  // "source/name", or a bare name only one source has
		auto* l = store_->list(view_.name);
		view_ = l ? View{View::List, store_->key_of(*l)} : home_view();
	}
	if (sidebar_->gone(view_)) {  // hidden in the sidebar, or not shown there
		view_ = home_view();
	}
	refresh();
}

void Window::on_file_changed(GFile* file, GFile* other) {
	for (auto* f : {file, other}) {
		if (!f) {
			continue;
		}
		auto path = take_string(g_file_get_path(f));
		if (auto key = store_->key_for_path(path)) {
			pending_reload_.insert(*key);
		}
	}
	if (pending_reload_.empty()) {
		return;
	}
	// Wait for a burst of changes (Syncthing renames, writes, conflict copies)
	// to settle.
	if (reload_timer_) {
		g_source_remove(reload_timer_);
	}
	reload_timer_ = timeout(400, [this] {
		reload_timer_ = 0;
		reload_pending();
		return false;
	});
}

void Window::reload_pending() {
	auto candidates_before = store_->candidates();
	bool changed = false;
	for (auto& name : pending_reload_) {
		try {
			changed |= store_->reload(name);
		} catch (const std::exception& e) {
			toast(std::format("Couldn't read “{}”: {}", name, e.what()));
		}
	}
	pending_reload_.clear();
	if (!changed) {
		// A non-list Markdown file may have gained or lost a checklist.
		if (store_->candidates() != candidates_before) {
			update_banner();
		}
		return;
	}
	if (view_.kind == View::List && !store_->list(view_.name)) {
		view_ = home_view();
	}
	refresh();
}

}  // namespace ui
