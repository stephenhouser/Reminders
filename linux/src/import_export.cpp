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

void Window::import_file() {
	auto* dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Import Reminders");
	auto* filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Calendar, Markdown, text and CSV files");
	for (auto suffix : {"ics", "md", "markdown", "txt", "csv"}) {
		gtk_file_filter_add_suffix(filter, suffix);
	}
	for (auto mime :
		 {"text/calendar", "text/markdown", "text/plain", "text/csv"}) {
		gtk_file_filter_add_mime_type(filter, mime);
	}
	auto* all = gtk_file_filter_new();
	gtk_file_filter_set_name(all, "All files");
	gtk_file_filter_add_pattern(all, "*");
	auto* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	g_list_store_append(filters, all);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dialog, filter);
	g_object_unref(filter);
	g_object_unref(all);
	g_object_unref(filters);
	gtk_file_dialog_open(
		dialog, GTK_WINDOW(window_), nullptr,
		[](GObject* source, GAsyncResult* res, gpointer) {
			auto* self = static_cast<Window*>(
				g_object_get_data(G_OBJECT(source), "window"));
			GError* error = nullptr;
			auto file = Obj<GFile>::adopt(gtk_file_dialog_open_finish(
				GTK_FILE_DIALOG(source), res, &error));
			if (error) {
				g_error_free(error);  // cancelled
				return;
			}
			auto path =
				std::filesystem::path(take_string(g_file_get_path(file.get())));
			if (path.empty() || !self->store_) {
				return;
			}
			self->import_files({path}, {});
		},
		nullptr);
	g_object_set_data(G_OBJECT(dialog), "window", this);
	g_object_unref(dialog);
}

void Window::import_files(std::vector<std::filesystem::path> files,
						  std::string into) {
	if (files.empty() || !store_) {
		return;
	}
	auto path = files.front();
	files.erase(files.begin());
	auto next = [this, files, into] {
		idle([this, files, into] { import_files(files, into); });
	};
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec)) {
		toast(std::format("“{}” is a folder: drop the files in it",
						  path.filename().string()));
		return next();
	}
	std::ifstream in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	if (!in.good() && !in.eof()) {
		toast(std::format("Couldn't read “{}”", path.filename().string()));
		return next();
	}
	import_tasks(path, text.str(), into, next);
}

// Asks which list the reminders go into: a new one named after the
// calendar or file (first), or any list; one already called that is
// chosen to begin with. "Read As" shows the kind of file found, and
// reads it again as another.
void Window::import_tasks(const std::filesystem::path& file, std::string text,
						  std::string into, std::function<void()> then) {
	static constexpr rem::Import::Kind kKinds[] = {
		rem::Import::Kind::Markdown, rem::Import::Kind::Text,
		rem::Import::Kind::Todotxt, rem::Import::Kind::Csv,
		rem::Import::Kind::Ics};
	struct State {
			std::string text, file_name;
			std::optional<rem::Import>
				imp;  // as read with the kind chosen; nullopt if it couldn't be
	};
	auto state = std::make_shared<State>(
		State{std::move(text), file.filename().string(), std::nullopt});
	auto detected = rem::detect_kind(state->text, state->file_name);

	auto* in_view =
		view_.kind == View::List ? store_->list(view_.name) : nullptr;
	auto source = in_view ? store_->source_of(*in_view)->config.name
						  : store_->default_source();
	// The new list's name: the calendar's, else the file's.
	std::string name;
	try {
		name = rem::read_as(state->text, detected, std::chrono::current_zone())
				   .name;
	} catch (const std::exception&) {
	}
	if (name.empty()) {
		name = file.filename().string();
		for (auto suffix :
			 {".todo.txt", ".txt", ".md", ".markdown", ".csv", ".ics"}) {
			if (lower(name).ends_with(suffix)) {
				name.resize(name.size() - std::string_view(suffix).size());
				break;
			}
		}
	}
	for (auto& c : name) {
		if (std::string_view("/\\<>:\"|?*").find(c) != std::string_view::npos) {
			c = '-';
		}
	}
	if (!list_name_error(name).empty()) {
		name = "Imported";
	}

	std::vector<std::string> keys{""};	// "": the new list
	std::vector<std::string> labels{std::format("New List “{}”", name)};
	guint selected = 0;
	for (auto* l : store_->lists()) {
		auto key = store_->key_of(*l);
		if (key == into ||
			(into.empty() && lower(l->name) == lower(name) && selected == 0)) {
			selected = static_cast<guint>(keys.size());
		}
		keys.push_back(store_->key_of(*l));
		labels.push_back(store_->label(*l));
	}

	auto* dialog = adw_alert_dialog_new("Import Reminders", nullptr);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
								   "_Cancel", "import", "_Import", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "import",
											 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "import");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	auto* rows = boxed_list();
	auto* read_as = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(read_as), "Read As");
	auto* kinds = gtk_string_list_new(nullptr);
	for (auto label :
		 {"Markdown", "Plain Text", "todo.txt", "CSV", "iCalendar"}) {
		gtk_string_list_append(kinds, label);
	}
	adw_combo_row_set_model(ADW_COMBO_ROW(read_as), G_LIST_MODEL(kinds));
	g_object_unref(kinds);
	adw_combo_row_set_selected(
		ADW_COMBO_ROW(read_as),
		static_cast<guint>(std::ranges::find(kKinds, detected) -
						   std::begin(kKinds)));
	auto* into_row = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(into_row), "Into");
	auto* model = gtk_string_list_new(nullptr);
	for (auto& l : labels) {
		gtk_string_list_append(model, l.c_str());
	}
	adw_combo_row_set_model(ADW_COMBO_ROW(into_row), G_LIST_MODEL(model));
	g_object_unref(model);
	adw_combo_row_set_selected(ADW_COMBO_ROW(into_row), selected);
	auto* duplicates = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(duplicates),
								  "Import Duplicates");
	adw_action_row_set_subtitle(
		ADW_ACTION_ROW(duplicates),
		"Add reminders that are already here again, as copies");
	gtk_list_box_append(GTK_LIST_BOX(rows), read_as);
	gtk_list_box_append(GTK_LIST_BOX(rows), into_row);
	gtk_list_box_append(GTK_LIST_BOX(rows), duplicates);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), rows);

	// Reads the file as the kind chosen, and says what it found.
	auto reread = [state, dialog, read_as] {
		auto i =
			std::min<guint>(adw_combo_row_get_selected(ADW_COMBO_ROW(read_as)),
							std::size(kKinds) - 1);
		std::string body;
		try {
			state->imp = rem::read_as(state->text, kKinds[i],
									  std::chrono::current_zone());
			auto count = rem::reminder_count(*state->imp);
			body = count == 0
					 ? std::format("There are no reminders in “{}” read as {}.",
								   state->file_name, rem::kind_name(kKinds[i]))
					 : std::format("{} {} from “{}”.", count,
								   count == 1 ? "reminder" : "reminders",
								   state->file_name);
			if (state->imp->skipped > 0) {
				body += std::format(" {} {} left out: only tasks are imported.",
									state->imp->skipped,
									state->imp->skipped == 1
										? "event or other item is"
										: "events or other items are");
			}
			if (count > 0 && kKinds[i] == rem::Import::Kind::Text) {
				body += " Each line of the file is a reminder.";
			}
			if (count == 0) {
				state->imp.reset();
			}
		} catch (const std::exception& e) {
			state->imp.reset();
			body =
				std::format("“{}” can't be read as {}: {}.", state->file_name,
							rem::kind_name(kKinds[i]), e.what());
		}
		adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), body.c_str());
		adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog),
											  "import", state->imp.has_value());
	};
	reread();
	connect<void(GObject*, GParamSpec*)>(
		read_as, "notify::selected",
		[reread](GObject*, GParamSpec*) { reread(); });

	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, into_row, duplicates, keys, source, name, state, then](
			AdwAlertDialog*, const char* response) {
			if (then) {
				then();
			}
			if (std::string_view(response) != "import" || !store_ ||
				!state->imp) {
				return;
			}
			auto& imp = *state->imp;
			auto key = keys.at(std::min<std::size_t>(
				adw_combo_row_get_selected(ADW_COMBO_ROW(into_row)),
				keys.size() - 1));
			rem::ImportResult result;
			undoable("Import", [&] {
				try {
					rem::ListFile* list =
						key.empty() ? nullptr : store_->list(key);
					bool created = !list;
					if (!list) {
						// A new list, its name made unique in the source.
						auto unique = name;
						auto clash = [&](const std::string& n) {
							return std::ranges::any_of(
								store_->lists(source), [&](auto* l) {
									return lower(l->name) == lower(n);
								});
						};
						for (int n = 2; clash(unique); ++n) {
							unique = std::format("{} {}", name, n);
						}
						list = &store_->create_list(
							source, unique,
							imp.color.empty() ? "blue" : imp.color, "list");
					}
					key = store_->key_of(*list);
					result = rem::import_into(
						*store_, *list, imp,
						adw_switch_row_get_active(ADW_SWITCH_ROW(duplicates)));
					if (result.added == 0 &&
						created) {	// nothing new: no empty list either
						store_->delete_list(key);
						key = "-";
					}
				} catch (const std::exception& e) {
					toast(std::format("Couldn't import: {}", e.what()));
					key.clear();
				}
			});
			if (key.empty()) {
				return;
			}
			if (key == "-") {
				toast(std::format(
					"Nothing to import: {} already here",
					result.already == 1
						? "the one reminder is"
						: std::format("all {} reminders are", result.already)));
				return;
			}
			auto text =
				std::format("Imported {} {}", result.added,
							result.added == 1 ? "reminder" : "reminders");
			if (result.already > 0) {
				text += std::format("; {} {} already here", result.already,
									result.already == 1 ? "was" : "were");
			}
			toast(text);
			select(View{View::List, key});
			show_content();
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

// The lists to export (`chosen`: keys ticked to begin with) and the format.
// One list is saved as a file; several, a file each, into a folder.
void Window::export_lists(std::vector<std::string> chosen) {
	if (!store_) {
		return;
	}
	auto* dialog = adw_alert_dialog_new(
		"Export Lists",
		"Exported lists can be imported again, here or on another computer.");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel",
								   "_Cancel", "export", "_Export…", nullptr);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "export",
											 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "export");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

	static constexpr rem::ExportFormat kFormats[] = {
		rem::ExportFormat::Markdown, rem::ExportFormat::Text,
		rem::ExportFormat::Todotxt, rem::ExportFormat::Csv,
		rem::ExportFormat::Ics};
	auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	auto* rows = boxed_list();
	auto* format = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(format), "Format");
	auto* model = gtk_string_list_new(nullptr);
	for (auto name :
		 {"Markdown", "Plain Text", "todo.txt", "CSV", "iCalendar"}) {
		gtk_string_list_append(model, name);
	}
	adw_combo_row_set_model(ADW_COMBO_ROW(format), G_LIST_MODEL(model));
	g_object_unref(model);
	auto* completed = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(completed),
								  "Include Completed");
	auto describe = [format, completed] {
		auto f = kFormats[std::min<guint>(
			adw_combo_row_get_selected(ADW_COMBO_ROW(format)),
			std::size(kFormats) - 1)];
		adw_action_row_set_subtitle(
			ADW_ACTION_ROW(format),
			f == rem::ExportFormat::Text
				? "A line per reminder, as you'd type it (.txt)"
			: f == rem::ExportFormat::Todotxt
				? "For todo.txt apps; without notes or sections (.todo.txt)"
			: f == rem::ExportFormat::Csv
				? "A row per reminder, for spreadsheets (.csv)"
			: f == rem::ExportFormat::Ics
				? "Tasks for calendar and reminders apps (.ics)"
				: "The list file itself, with everything (.md)");
		gtk_widget_set_visible(
			completed,
			f == rem::ExportFormat::Text);	// the others always have everything
	};
	describe();
	connect<void(GObject*, GParamSpec*)>(
		format, "notify::selected",
		[describe](GObject*, GParamSpec*) { describe(); });
	auto* archive = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(archive),
								  "Compressed Archive");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(archive),
								"One .zip file instead of a folder of files");
	gtk_list_box_append(GTK_LIST_BOX(rows), format);
	gtk_list_box_append(GTK_LIST_BOX(rows), completed);
	gtk_list_box_append(GTK_LIST_BOX(rows), archive);
	gtk_box_append(GTK_BOX(box), rows);

	// The lists, with All Lists above them.
	struct Pick {
			std::string key;
			GtkWidget* check;
	};
	auto picks = std::make_shared<std::vector<Pick>>();
	auto* lists = boxed_list();
	auto* all_check = gtk_check_button_new();
	auto* all_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(all_row), "All Lists");
	adw_action_row_add_prefix(ADW_ACTION_ROW(all_row), all_check);
	adw_action_row_set_activatable_widget(ADW_ACTION_ROW(all_row), all_check);
	gtk_list_box_append(GTK_LIST_BOX(lists), all_row);
	for (auto* l : store_->lists()) {
		auto key = store_->key_of(*l);
		auto* check = gtk_check_button_new();
		gtk_check_button_set_active(
			GTK_CHECK_BUTTON(check),
			std::ranges::find(chosen, key) != chosen.end());
		auto* row = adw_action_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
									  store_->label(*l).c_str());
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
		adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
		gtk_list_box_append(GTK_LIST_BOX(lists), row);
		picks->push_back({key, check});
	}
	auto* scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
								   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller),
											   280);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), lists);
	gtk_box_append(GTK_BOX(box), scroller);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);

	// All Lists shows whether all, some or none are ticked; ticking it ticks
	// them all, unticking it none. Export needs at least one.
	auto syncing = std::make_shared<bool>(false);
	auto update = [dialog, picks, all_check, syncing, archive] {
		auto n = std::ranges::count_if(*picks, [](auto& p) {
			return gtk_check_button_get_active(GTK_CHECK_BUTTON(p.check));
		});
		gtk_widget_set_visible(archive, n > 1);	 // one list is one file anyway
		*syncing = true;
		gtk_check_button_set_active(
			GTK_CHECK_BUTTON(all_check),
			n > 0 && n == static_cast<long>(picks->size()));
		gtk_check_button_set_inconsistent(
			GTK_CHECK_BUTTON(all_check),
			n > 0 && n < static_cast<long>(picks->size()));
		*syncing = false;
		adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog),
											  "export", n > 0);
	};
	for (auto& p : *picks) {
		connect<void(GtkCheckButton*)>(p.check, "toggled",
									   [update](GtkCheckButton*) { update(); });
	}
	connect<void(GtkCheckButton*)>(
		all_check, "toggled", [picks, syncing, update](GtkCheckButton* b) {
			if (*syncing) {
				return;
			}
			bool on = gtk_check_button_get_active(b);
			for (auto& p : *picks) {
				gtk_check_button_set_active(GTK_CHECK_BUTTON(p.check), on);
			}
			update();
		});
	update();

	connect<void(AdwAlertDialog*, const char*)>(
		dialog, "response",
		[this, picks, format, completed, archive](AdwAlertDialog*,
												  const char* response) {
			if (std::string_view(response) != "export" || !store_) {
				return;
			}
			auto fmt = kFormats[std::min<guint>(
				adw_combo_row_get_selected(ADW_COMBO_ROW(format)),
				std::size(kFormats) - 1)];
			rem::ExportOptions options;
			options.completed =
				adw_switch_row_get_active(ADW_SWITCH_ROW(completed));
			std::vector<std::string> keys;
			for (auto& p : *picks) {
				if (gtk_check_button_get_active(GTK_CHECK_BUTTON(p.check))) {
					keys.push_back(p.key);
				}
			}
			if (keys.empty()) {
				return;
			}

			struct Job {
					Window* self;
					std::vector<std::string> keys;
					rem::ExportFormat format;
					rem::ExportOptions options;
			};
			auto* chooser = gtk_file_dialog_new();
			auto* job = new Job{this, keys, fmt, options};
			if (keys.size() > 1 &&
				adw_switch_row_get_active(ADW_SWITCH_ROW(archive))) {
				gtk_file_dialog_set_title(chooser, "Export Lists");
				gtk_file_dialog_set_initial_name(chooser, "Reminders.zip");
				gtk_file_dialog_save(
					chooser, GTK_WINDOW(window_), nullptr,
					[](GObject* source, GAsyncResult* res, gpointer data) {
						std::unique_ptr<Job> job(static_cast<Job*>(data));
						GError* error = nullptr;
						auto file =
							Obj<GFile>::adopt(gtk_file_dialog_save_finish(
								GTK_FILE_DIALOG(source), res, &error));
						if (error) {
							g_error_free(error);  // cancelled
							return;
						}
						auto* self = job->self;
						auto path = std::filesystem::path(
							take_string(g_file_get_path(file.get())));
						if (!self->store_ || path.empty()) {
							return;
						}
						std::vector<rem::ListFile*> lists;
						for (auto& k : job->keys) {
							if (auto* l = self->store_->list(k)) {
								lists.push_back(l);
							}
						}
						try {
							std::ofstream out(
								path, std::ios::binary | std::ios::trunc);
							out << rem::export_zip(*self->store_, lists,
												   job->format, job->options);
							out.close();
							self->toast(
								out ? std::format("Exported {} lists to {}",
												  lists.size(),
												  rem::contract_path(path))
									: std::format("Couldn't write {}",
												  path.string()));
						} catch (const std::exception& e) {
							self->toast(
								std::format("Couldn't export: {}", e.what()));
						}
					},
					job);
			} else if (keys.size() > 1) {
				gtk_file_dialog_set_title(chooser,
										  "Choose a Folder for the Lists");
				gtk_file_dialog_select_folder(
					chooser, GTK_WINDOW(window_), nullptr,
					[](GObject* source, GAsyncResult* res, gpointer data) {
						std::unique_ptr<Job> job(static_cast<Job*>(data));
						GError* error = nullptr;
						auto file = Obj<GFile>::adopt(
							gtk_file_dialog_select_folder_finish(
								GTK_FILE_DIALOG(source), res, &error));
						if (error) {
							g_error_free(error);  // cancelled
							return;
						}
						auto* self = job->self;
						auto folder = std::filesystem::path(
							take_string(g_file_get_path(file.get())));
						if (!self->store_ || folder.empty()) {
							return;
						}
						std::vector<rem::ListFile*> lists;
						for (auto& k : job->keys) {
							if (auto* l = self->store_->list(k)) {
								lists.push_back(l);
							}
						}
						try {
							auto files =
								rem::export_lists(*self->store_, lists, folder,
												  job->format, job->options);
							self->toast(std::format(
								"Exported {} {} to {}", files.size(),
								files.size() == 1 ? "list" : "lists",
								rem::contract_path(folder)));
						} catch (const std::exception& e) {
							self->toast(
								std::format("Couldn't export: {}", e.what()));
						}
					},
					job);
			} else {
				auto* list = store_->list(keys.front());
				if (!list) {
					delete job;
					g_object_unref(chooser);
					return;
				}
				gtk_file_dialog_set_title(chooser, "Export List");
				gtk_file_dialog_set_initial_name(
					chooser,
					std::format("{}.{}", list->name, rem::export_extension(fmt))
						.c_str());
				gtk_file_dialog_save(
					chooser, GTK_WINDOW(window_), nullptr,
					[](GObject* source, GAsyncResult* res, gpointer data) {
						std::unique_ptr<Job> job(static_cast<Job*>(data));
						GError* error = nullptr;
						auto file =
							Obj<GFile>::adopt(gtk_file_dialog_save_finish(
								GTK_FILE_DIALOG(source), res, &error));
						if (error) {
							g_error_free(error);  // cancelled
							return;
						}
						auto* self = job->self;
						auto* list = self->store_
									   ? self->store_->list(job->keys.front())
									   : nullptr;
						auto path = std::filesystem::path(
							take_string(g_file_get_path(file.get())));
						if (!list || path.empty()) {
							return;
						}
						std::ofstream out(path,
										  std::ios::binary | std::ios::trunc);
						out << rem::export_list(*list, job->format,
												job->options);
						out.close();
						self->toast(out ? std::format("Exported “{}” to {}",
													  list->name,
													  rem::contract_path(path))
										: std::format("Couldn't write {}",
													  path.string()));
					},
					job);
			}
			g_object_unref(chooser);
		});
	adw_dialog_present(ADW_DIALOG(dialog), window_);
}

}  // namespace ui
