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
#include "support.hpp"
#include "window_internal.hpp"

namespace ui {

// A new list, starting in `source`; by default the source of the list
// showing, else the default source. With several sources the dialog can
// choose another.
void Window::new_list(std::string source) {
	if (source.empty()) {
		auto* l = view_.kind == View::List ? store_->list(view_.name) : nullptr;
		source =
			l ? store_->source_of(*l)->config.name : store_->default_source();
	}
	std::vector<SourceChoice> choices;
	for (auto& s : store_->sources()) {
		choices.push_back({s.config.name, rem::source_title(s.config)});
	}
	show_list_dialog(
		window_, std::nullopt,
		[this](const ListEdit& e) -> std::string {
			if (auto err = list_name_error(e.name); !err.empty()) {
				return err;
			}
			for (auto* l : store_->lists(e.source)) {
				if (lower(l->name) == lower(e.name)) {
					return "A list with that name already exists";
				}
			}
			return {};
		},
		[this](ListEdit e) {
			bool ok = true;
			undoable("New List", [&] {
				try {
					store_->create_list(e.source, e.name, e.color, e.icon);
				} catch (const std::exception& ex) {
					toast(
						std::format("Couldn't create the list: {}", ex.what()));
					ok = false;
				}
			});
			if (!ok) {
				return;
			}
			select(View{View::List, rem::Library::key(e.source, e.name)});
			show_content();
		},
		std::move(choices), source);
}

// List Info… for the list with key `key` ("source/name").
void Window::edit_list(const std::string& key) {
	auto* l = store_->list(key);
	if (!l) {
		return;
	}
	auto name = l->name;
	auto source = store_->source_of(*l)->config.name;
	show_list_dialog(
		window_, ListEdit{l->name, l->color(), l->icon(), {}},
		[this, name, source](const ListEdit& e) -> std::string {
			if (auto err = list_name_error(e.name); !err.empty()) {
				return err;
			}
			for (auto* other :
				 store_->lists(source)) {  // names are unique within a source
				if (other->name != name &&
					lower(other->name) == lower(e.name)) {
					return "A list with that name already exists";
				}
			}
			return {};
		},
		[this, key, name, source](ListEdit e) {
			auto* l = store_->list(key);
			if (!l) {
				return;
			}
			bool ok = true;
			auto new_key = rem::Library::key(source, e.name);
			undoable("Edit List", [&] {
				try {
					if (e.name != name && !store_->rename_list(*l, e.name)) {
						toast("Couldn't rename the list");
						ok = false;
						return;
					}
					if (e.name != name) {  // keeps its place in lists-order
										   // under its new name
						auto order = rem::load_names_setting("lists-order");
						for (auto& entry : order) {
							if (rem::list_entry_matches(entry, key)) {
								entry = new_key;
							}
						}
						rem::save_names_setting("lists-order", order);
					}
					if (e.name != name &&
						entry_hidden(
							View{View::List,
								 key})) {  // stays hidden under its new name
						rem::set_list_hidden(key, false);
						rem::set_list_hidden(new_key, true);
						sidebar_->reload();
					}
					l->doc.set_meta("color", e.color);
					l->doc.set_meta("icon", e.icon);
					store_->save(*l);
				} catch (const std::exception& ex) {
					toast(std::format("Couldn't save: {}", ex.what()));
				}
			});
			if (!ok) {
				return;
			}
			if (view_.kind == View::List && view_.name == key) {
				view_.name = new_key;
				if (remember_view_) {
					save_last_view(view_to_string(view_));
				}
			}
			refresh();
		});
}

// Deletes the list with key `name` ("source/name"), after asking.
void Window::delete_list(const std::string& name) {
	auto* list = store_->list(name);
	auto shown = list ? list->name : name;
	auto* dialog = adw_alert_dialog_new(
		std::format("Delete “{}”?", shown).c_str(),
		"The list and all its reminders will be deleted on every synced "
		"device. "
		"On this computer the file is moved to the Trash.");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
								   "_Cancel", "delete", "_Delete", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete",
											 ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, name, shown](AdwAlertDialog*, const char* response) {
			if (std::string_view(response) != "delete") {
				return;
			}
			auto step = undoable("Delete List", [&] {
				auto file = Obj<GFile>::adopt(
					g_file_new_for_path(store_->path_of(name).c_str()));
				GError* error = nullptr;
				if (!g_file_trash(file.get(), nullptr, &error)) {
					// No trash on this filesystem: delete outright.
					g_error_free(error);
				}
				store_->delete_list(name);
			});
			if (view_.kind == View::List && view_.name == name) {
				view_ = home_view();
			}
			refresh();
			toast(std::format("“{}” deleted", shown), "_Undo",
				  [this, step, name] {
					  if (history_.next_undo() != step) {
						  return;
					  }
					  undo();
					  select(View{View::List, name});
				  });
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

void Window::add_section(const std::string& list) {
	auto* dialog = adw_alert_dialog_new("Add Section", nullptr);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
								   "_Cancel", "add", "_Add", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "add",
											 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "add");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	auto* entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Section Name");
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, list, entry](AdwAlertDialog*, const char* response) {
			if (std::string_view(response) != "add") {
				return;
			}
			auto name = trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
			auto* l = store_->list(list);
			if (name.empty() || !l) {
				return;
			}
			for (auto& s : l->doc.sections()) {
				if (s.name == name) {
					return;
				}
			}
			auto& blocks = l->doc.blocks;
			if (!blocks.empty()) {
				auto* last = std::get_if<rem::RawLine>(&blocks.back());
				if (!last || !trim(last->text).empty()) {
					blocks.push_back(rem::RawLine{""});
				}
			}
			undoable("Add Section", [&] {
				blocks.push_back(rem::RawLine{"## " + name});
				try {
					store_->save(*l);
				} catch (const std::exception& e) {
					toast(std::format("Couldn't save: {}", e.what()));
				}
			});
			focus_new_row_ = list + '\x1f' + name;
			refresh();
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

}  // namespace ui
