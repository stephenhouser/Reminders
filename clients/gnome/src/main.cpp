#include <adwaita.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtk_util.hpp"
#include "reminders/backend_module.hpp"
#include "support.hpp"
#include "version.hpp"
#include "window.hpp"

namespace {

void show_about(GtkApplication* app) {
	auto* about = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Reminders");
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), ui::kAppId);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), ui::kVersion);
	adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
								  "Reminders and to-do lists stored as "
								  "Markdown files in a Syncthing folder.\n\n"
									"Copyright(c) 2026 Stephen Houser.");
	adw_dialog_present(ADW_DIALOG(about),
					   GTK_WIDGET(gtk_application_get_active_window(app)));
}

void show_shortcuts(GtkApplication* app) {
	auto* dialog = adw_shortcuts_dialog_new();
	struct Item {
			const char* title;
			const char* accel;
	};
	auto section = [&](const char* title, std::initializer_list<Item> items) {
		auto* s = adw_shortcuts_section_new(title);
		for (auto& i : items) {
			adw_shortcuts_section_add(s,
									  adw_shortcuts_item_new(i.title, i.accel));
		}
		adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), s);
	};
	section("General", {{"Undo", "<Control>z"},
						{"Redo", "<Control><Shift>z"},
						{"Search", "<Control>f"},
						{"Show / Hide Sidebar", "<Control>b F9"},
						{"Switch Between Sidebar and Reminders", "<Control>l"},
						{"Main Menu", "F10"},
						{"Sync This Source", "<Control>s"},
						{"Sync All", "<Control><Shift>s"},
						{"Import…", "<Control>o"},
						{"Settings", "<Control>comma"},
						{"Keyboard Shortcuts", "<Control>question"},
						{"Close Window", "<Control>w"},
						{"Quit", "<Control>q"}});
	section("Reminders",
			{{"New Reminder", "<Control>n"},
			 {"Complete / Not Complete", "space"},
			 {"Edit Title", "Return"},
			 {"Save While Editing", "<Control>s"},
			 {"Cancel Editing", "Escape"},
			 {"Details", "<Control>e <Alt>Return"},
			 {"Menu", "Menu <Shift>F10"},
			 {"Flag / Unflag", "<Control>d"},
			 {"Due Today", "<Control>t"},
			 {"Due Tomorrow", "<Control><Shift>t"},
			 {"Priority: None, Low, Medium, High",
			  "<Control>0 <Control>1 <Control>2 <Control>3"},
			 {"Indent (Make Subtask)", "<Control>bracketright"},
			 {"Outdent", "<Control>bracketleft"},
			 {"Move Up / Down", "<Control>Up <Control>Down"},
			 {"Cut", "<Control>x"},
			 {"Copy", "<Control>c"},
			 {"Paste as New Reminders", "<Control>v"},
			 {"Paste Special (One or One per Line)", "<Control><Shift>v"},
			 {"Delete", "Delete"}});
	{
		// Clicks can't be shown as keys, so they're told in a subtitle.
		auto* s = adw_shortcuts_section_new("Selecting Several Reminders");
		auto* all = adw_shortcuts_item_new("Select All", "<Control>a");
		adw_shortcuts_item_set_subtitle(
			all, "Ctrl+click adds or removes one; Shift+click selects a range");
		adw_shortcuts_section_add(s, all);
		adw_shortcuts_section_add(
			s, adw_shortcuts_item_new("Extend Selection Up / Down",
									  "<Shift>Up <Shift>Down"));
		adw_shortcuts_section_add(
			s, adw_shortcuts_item_new("Clear the Selection",
									  "Escape <Control><Shift>a"));
		adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), s);
	}
	section("Lists",
			{{"Go To…", "<Control>k"},
			 {"Open and Move Into (in the Sidebar)", "Return"},
			 {"Next in Sidebar", "<Control>Page_Down"},
			 {"Previous in Sidebar", "<Control>Page_Up"},
			 {"Move Up / Down in the Sidebar", "<Control>Up <Control>Down"},
			 {"Move Sidebar Group Up / Down",
			  "<Control><Shift>Up <Control><Shift>Down"},
			 {"New List", "<Control><Shift>n"},
			 {"Show / Hide Completed", "<Control>h"},
			 {"Show / Hide Hidden Lists", "<Control><Shift>h"},
			 {"Show / Hide Subtasks", "<Shift>Right <Shift>Left"}});
	adw_dialog_present(ADW_DIALOG(dialog),
					   GTK_WIDGET(gtk_application_get_active_window(app)));
}

// Developer aid: REMINDERS_SCREENSHOT=out.png renders the window to a
// PNG shortly after start-up and quits (screen capture is locked down on
// Wayland). REMINDERS_SCREENSHOT_DELAY=ms waits longer than the 1.5 s
// default, for a test that types first (tools/gui-keys.py).
void schedule_screenshot(GtkWindow* win, std::string path) {
	auto keep = ui::Obj<GtkWindow>::ref(win);
	unsigned delay = 1500;
	if (const char* ms = g_getenv("REMINDERS_SCREENSHOT_DELAY")) {
		delay = static_cast<unsigned>(std::max(0, std::atoi(ms)));
	}
	ui::timeout(delay, [keep, path] {
		auto* widget = GTK_WIDGET(keep.get());
		int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
		auto paintable =
			ui::Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(widget));
		auto* snapshot = gtk_snapshot_new();
		gdk_paintable_snapshot(paintable.get(), snapshot, w, h);
		if (auto* node = gtk_snapshot_free_to_node(snapshot)) {
			graphene_rect_t bounds;
			graphene_rect_init(&bounds, 0, 0, static_cast<float>(w),
							   static_cast<float>(h));
			auto* renderer = gtk_native_get_renderer(GTK_NATIVE(widget));
			auto texture = ui::Obj<GdkTexture>::adopt(
				gsk_renderer_render_texture(renderer, node, &bounds));
			gdk_texture_save_to_png(texture.get(), path.c_str());
			gsk_render_node_unref(node);
		}
		g_application_quit(g_application_get_default());
		return false;
	});
}

}  // namespace

// The window showing `profile`, if one is open: each profile has one.
ui::Window* window_for(GtkApplication* app, const rem::Profile& profile) {
	for (auto* l = gtk_application_get_windows(app); l; l = l->next) {
		if (auto* w = ui::Window::from(GTK_WINDOW(l->data));
			w && w->profile() == profile) {
			return w;
		}
	}
	return nullptr;
}

// Shows `profile`'s window, creating it if needed; `folder` replaces its
// open folder.
ui::Window* present(AdwApplication* app, const rem::Profile& profile,
					std::optional<std::filesystem::path> folder) {
	if (auto* w = window_for(GTK_APPLICATION(app), profile)) {
		if (folder) {
			w->open_sources(*folder);
		}
		gtk_window_present(w->gtk());
		return w;
	}
	auto* window = ui::Window::create(app, profile, std::move(folder));
	auto* win = window->gtk();
	gtk_window_present(win);
	if (const char* shot = g_getenv("REMINDERS_SCREENSHOT")) {
		schedule_screenshot(win, shot);
	}
	try {
		rem::remember_profile(profile);	 // for profile-on-start=last
	} catch (const std::exception&) {
		// It just won't be remembered.
	}
	return window;
}

// With no profile named: the window in front, if there is one, else the
// profile profile-on-start= picks (with ask, the last one, until there's a
// picker).
void present_any(AdwApplication* app,
				 std::optional<std::filesystem::path> folder) {
	if (auto* w = ui::Window::from(
			gtk_application_get_active_window(GTK_APPLICATION(app)))) {
		present(app, w->profile(), std::move(folder));
		return;
	}
	present(app, rem::profile_on_start().profile, std::move(folder));
}

// "Reminders [--profile NAME] [FOLDER]". Runs in the main instance;
// messages go to the terminal the command was typed in, even when the app
// was already running.
int handle_command_line(AdwApplication* app, GApplicationCommandLine* cmd) {
	// The profile: --profile, else the caller's REMINDERS_PROFILE, else none
	// named (the window in front, or profile-on-start=).
	std::optional<rem::Profile> profile;
	const char* named = nullptr;
	g_variant_dict_lookup(g_application_command_line_get_options_dict(cmd),
						  "profile", "&s", &named);
	if (!named || !*named) {
		named = g_application_command_line_getenv(cmd, "REMINDERS_PROFILE");
	}
	if (named && *named) {
		try {
			profile = rem::existing_profile(named);
		} catch (const std::exception& e) {
			g_application_command_line_printerr(cmd, "Reminders: %s\n",
												e.what());
			return 1;
		}
	}

	int all_argc = 0;
	char** all_argv = g_application_command_line_get_arguments(cmd, &all_argc);
	// Options are in the options dictionary; keep just the folder argument
	// (not --profile's value either).
	std::vector<char*> args;
	for (int i = 0; i < all_argc; ++i) {
		std::string_view a = all_argv[i];
		if ((a == "-P" || a == "--profile") && i + 1 < all_argc) {
			++i;
		} else if (i == 0 || !a.starts_with('-')) {
			args.push_back(all_argv[i]);
		}
	}
	auto show = [&](std::optional<std::filesystem::path> folder) {
		if (profile) {
			present(app, *profile, std::move(folder));
		} else {
			present_any(app, std::move(folder));
		}
	};
	int argc = static_cast<int>(args.size());
	char** argv = args.data();

	int status = 0;
	if (argc > 2) {
		g_application_command_line_printerr(
			cmd, "Usage: Reminders [--profile NAME] [FOLDER]\n");
		status = 1;
	} else if (argc == 2) {
		// Relative paths are resolved against the caller's directory.
		auto file = ui::Obj<GFile>::adopt(
			g_application_command_line_create_file_for_arg(cmd, argv[1]));
		auto path = ui::take_string(g_file_get_path(file.get()));
		if (path.empty() || !std::filesystem::is_directory(path)) {
			g_application_command_line_printerr(
				cmd, "Reminders: “%s” is not a folder\n", argv[1]);
			status = 1;
		} else {
			show(std::filesystem::path(path));
		}
	} else {
		show(std::nullopt);
	}
	g_strfreev(all_argv);
	return status;
}

int main(int argc, char** argv) {
	rem::register_backends();  // the back ends built in
	g_set_application_name("Reminders");
	auto* app =
		adw_application_new(ui::kAppId, G_APPLICATION_HANDLES_COMMAND_LINE);
	g_application_set_option_context_parameter_string(G_APPLICATION(app),
													  "[FOLDER]");
	// Registering an option also turns on GApplication's --help handling.
	g_application_add_main_option(G_APPLICATION(app), "version", 0,
								  G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
								  "Show the version and exit", nullptr);
	g_application_add_main_option(
		G_APPLICATION(app), "profile", 'P', G_OPTION_FLAG_NONE,
		G_OPTION_ARG_STRING,
		"Open profile NAME's window: its own settings and sources (also "
		"REMINDERS_PROFILE)",
		"NAME");
	ui::connect<int(GApplication*, GVariantDict*)>(
		app, "handle-local-options", [](GApplication*, GVariantDict* opts) {
			if (g_variant_dict_contains(opts, "version")) {
				g_print("Reminders %s\n", ui::kVersion);
				return 0;
			}
			return -1;	// carry on
		});
	g_application_set_option_context_summary(
		G_APPLICATION(app),
		"Open the lists in FOLDER (a Syncthing folder of Markdown files). "
		"Without FOLDER,\n"
		"the folder chosen in the app is opened. FOLDER is only used for this "
		"session.\n"
		"Each profile opens in a window of its own.");

	ui::on(app, "startup", [app] {
		auto* gapp = GTK_APPLICATION(app);
		ui::add_action(app, "quit",
					   [app] { g_application_quit(G_APPLICATION(app)); });
		ui::add_action(app, "about", [gapp] { show_about(gapp); });
		ui::add_action(app, "shortcuts", [gapp] { show_shortcuts(gapp); });
		// Target: (profile, reminder id). Used by notifications: opens
		// that profile's window, if it was closed meanwhile.
		auto* show =
			g_simple_action_new("show-reminder", G_VARIANT_TYPE("(ss)"));
		ui::connect<void(GSimpleAction*, GVariant*)>(
			show, "activate", [app](GSimpleAction*, GVariant* target) {
				const char* name = nullptr;
				const char* id = nullptr;
				g_variant_get(target, "(&s&s)", &name, &id);
				try {
					present(app, rem::existing_profile(name), std::nullopt)
						->show_reminder(id);
				} catch (const std::exception&) {
					// The profile is gone.
				}
			});
		g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(show));
		g_object_unref(show);
		// One or more keys for an action (a second for the HIG's key, where
		// an older one stays).
		auto accel = [gapp](const char* action, const char* key,
							const char* also = nullptr) {
			const char* accels[] = {key, also, nullptr};
			gtk_application_set_accels_for_action(gapp, action, accels);
		};
		accel("app.quit", "<Control>q");
		accel("app.shortcuts", "<Control>question");
		accel("window.close", "<Control>w");
		accel("win.new-reminder", "<Control>n");
		accel("win.new-list", "<Control><Shift>n");
		accel("win.search", "<Control>f");
		accel("win.show-completed", "<Control>h");
		accel("win.show-hidden", "<Control><Shift>h");
		accel("win.toggle-sidebar", "<Control>b", "F9");
		accel("win.switch-focus", "<Control>l");
		accel("win.go-to", "<Control>k");
		accel("win.next-view", "<Control>Page_Down");
		accel("win.previous-view", "<Control>Page_Up");
		accel("win.undo", "<Control>z");
		accel("win.redo", "<Control><Shift>z");
		// The HIG's Preferences and Open. Ctrl+S (Sync) is the window's,
		// after a title or dialog being edited has had it as Save.
		accel("win.sync-all", "<Control><Shift>s");
		accel("win.settings", "<Control>comma");
		accel("win.import", "<Control>o");
	});

	ui::on(app, "activate", [app] { present_any(app, std::nullopt); });
	ui::connect<int(GApplication*, GApplicationCommandLine*)>(
		app, "command-line",
		[app](GApplication*, GApplicationCommandLine* cmd) {
			return handle_command_line(app, cmd);
		});

	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
