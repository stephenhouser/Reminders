#include <adwaita.h>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "gtk_util.hpp"
#include "support.hpp"
#include "window.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";

void show_about(GtkApplication* app) {
    auto* about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Reminders");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), ui::kAppId);
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), kVersion);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
                                  "Reminders and to-do lists stored as Markdown files in a Syncthing folder.");
    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(gtk_application_get_active_window(app)));
}

void show_shortcuts(GtkApplication* app) {
    auto* dialog = adw_shortcuts_dialog_new();
    struct Item {
        const char* title;
        const char* accel;
    };
    auto section = [&](const char* title, std::initializer_list<Item> items) {
        auto* s = adw_shortcuts_section_new(title);
        for (auto& i : items) adw_shortcuts_section_add(s, adw_shortcuts_item_new(i.title, i.accel));
        adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dialog), s);
    };
    section("Reminders", {{"New Reminder", "<Control>n"},
                          {"Complete / Not Complete", "space"},
                          {"Edit Title", "Return"},
                          {"Save While Editing", "<Control>s"},
                          {"Cancel Editing", "Escape"},
                          {"Details", "<Control>i"},
                          {"Flag / Unflag", "<Control><Shift>f"},
                          {"Due Today", "<Control>t"},
                          {"Due Tomorrow", "<Control><Shift>t"},
                          {"Priority: None, Low, Medium, High", "<Alt>0 <Alt>1 <Alt>2 <Alt>3"},
                          {"Indent (Make Subtask)", "<Control>bracketright"},
                          {"Outdent", "<Control>bracketleft"},
                          {"Move Up", "<Alt>Up"},
                          {"Move Down", "<Alt>Down"},
                          {"Delete", "Delete"}});
    section("Lists", {{"Go To…", "<Control>k"},
                      {"Sidebar Entries 1–9 (Today, Scheduled, …, Your Lists)", "<Control>1...<Control>9"},
                      {"Sidebar Entry 10", "<Control>0"},
                      {"Next in Sidebar", "<Control>Page_Down"},
                      {"Previous in Sidebar", "<Control>Page_Up"},
                      {"New List", "<Control><Shift>n"},
                      {"Show / Hide Completed", "<Control>h"},
                      {"Show / Hide All Subtasks", "<Control>e"}});
    section("General", {{"Undo", "<Control>z"},
                        {"Redo", "<Control><Shift>z"},
                        {"Search", "<Control>f"},
                        {"Show / Hide Sidebar", "<Control>b"},
                        {"Main Menu", "F10"},
                        {"Keyboard Shortcuts", "<Control>question"},
                        {"Close Window", "<Control>w"},
                        {"Quit", "<Control>q"}});
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(gtk_application_get_active_window(app)));
}

// Developer aid: REMINDERS_SCREENSHOT=out.png renders the window to a
// PNG shortly after start-up and quits (screen capture is locked down on Wayland).
void schedule_screenshot(GtkWindow* win, std::string path) {
    auto keep = ui::Obj<GtkWindow>::ref(win);
    ui::timeout(1500, [keep, path] {
        auto* widget = GTK_WIDGET(keep.get());
        int w = gtk_widget_get_width(widget), h = gtk_widget_get_height(widget);
        auto paintable = ui::Obj<GdkPaintable>::adopt(gtk_widget_paintable_new(widget));
        auto* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable.get(), snapshot, w, h);
        if (auto* node = gtk_snapshot_free_to_node(snapshot)) {
            graphene_rect_t bounds;
            graphene_rect_init(&bounds, 0, 0, static_cast<float>(w), static_cast<float>(h));
            auto* renderer = gtk_native_get_renderer(GTK_NATIVE(widget));
            auto texture = ui::Obj<GdkTexture>::adopt(gsk_renderer_render_texture(renderer, node, &bounds));
            gdk_texture_save_to_png(texture.get(), path.c_str());
            gsk_render_node_unref(node);
        }
        g_application_quit(g_application_get_default());
        return false;
    });
}

}  // namespace

// Shows the window, creating it if needed; `folder` replaces the open folder,
// `key_numbers` overrides the show-key-numbers setting.
void present(AdwApplication* app, std::optional<std::filesystem::path> folder,
             std::optional<bool> key_numbers = std::nullopt) {
    if (auto* existing = gtk_application_get_active_window(GTK_APPLICATION(app))) {
        if (auto* w = ui::Window::from(existing)) {
            if (folder) w->open_folder(*folder, false);
            if (key_numbers) w->set_show_key_numbers(*key_numbers);
        }
        gtk_window_present(existing);
        return;
    }
    auto* window = ui::Window::create(app, std::move(folder));
    if (key_numbers) window->set_show_key_numbers(*key_numbers);
    auto* win = window->gtk();
    gtk_window_present(win);
    if (const char* shot = g_getenv("REMINDERS_SCREENSHOT")) schedule_screenshot(win, shot);
}

// "reminders [FOLDER]". Runs in the main instance; messages go to the
// terminal the command was typed in, even when the app was already running.
int handle_command_line(AdwApplication* app, GApplicationCommandLine* cmd) {
    int all_argc = 0;
    char** all_argv = g_application_command_line_get_arguments(cmd, &all_argc);
    // Options are in the options dictionary; keep just the folder argument.
    std::vector<char*> args;
    for (int i = 0; i < all_argc; ++i)
        if (i == 0 || all_argv[i][0] != '-') args.push_back(all_argv[i]);
    int argc = static_cast<int>(args.size());
    char** argv = args.data();

    std::optional<bool> key_numbers;
    auto* opts = g_application_command_line_get_options_dict(cmd);
    bool show = g_variant_dict_contains(opts, "show-key-numbers");
    bool hide = g_variant_dict_contains(opts, "hide-key-numbers");
    if (show || hide) key_numbers = show;

    int status = 0;
    if (show && hide) {
        g_application_command_line_printerr(cmd, "Reminders: use --show-key-numbers or --hide-key-numbers, not both\n");
        status = 1;
    } else if (argc > 2) {
        g_application_command_line_printerr(cmd, "Usage: Reminders [FOLDER]\n");
        status = 1;
    } else if (argc == 2) {
        // Relative paths are resolved against the caller's directory.
        auto file = ui::Obj<GFile>::adopt(g_application_command_line_create_file_for_arg(cmd, argv[1]));
        auto path = ui::take_string(g_file_get_path(file.get()));
        if (path.empty() || !std::filesystem::is_directory(path)) {
            g_application_command_line_printerr(cmd, "Reminders: “%s” is not a folder\n", argv[1]);
            status = 1;
        } else {
            present(app, std::filesystem::path(path), key_numbers);
        }
    } else {
        present(app, std::nullopt, key_numbers);
    }
    g_strfreev(all_argv);
    return status;
}

int main(int argc, char** argv) {
    g_set_application_name("Reminders");
    auto* app = adw_application_new(ui::kAppId, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_set_option_context_parameter_string(G_APPLICATION(app), "[FOLDER]");
    // Registering an option also turns on GApplication's --help handling.
    g_application_add_main_option(G_APPLICATION(app), "show-key-numbers", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                  "Show each sidebar entry’s Ctrl+number shortcut after its name", nullptr);
    g_application_add_main_option(G_APPLICATION(app), "hide-key-numbers", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                  "Don't label them (overrides the show-key-numbers setting)", nullptr);
    g_application_add_main_option(G_APPLICATION(app), "version", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                  "Show the version and exit", nullptr);
    ui::connect<int(GApplication*, GVariantDict*)>(app, "handle-local-options", [](GApplication*, GVariantDict* opts) {
        if (g_variant_dict_contains(opts, "version")) {
            g_print("Reminders %s\n", kVersion);
            return 0;
        }
        return -1;  // carry on
    });
    g_application_set_option_context_summary(
        G_APPLICATION(app),
        "Open the lists in FOLDER (a Syncthing folder of Markdown files). Without FOLDER,\n"
        "the folder chosen in the app is opened. FOLDER is only used for this session.");

    ui::on(app, "startup", [app] {
        auto* gapp = GTK_APPLICATION(app);
        ui::add_action(app, "quit", [app] { g_application_quit(G_APPLICATION(app)); });
        ui::add_action(app, "about", [gapp] { show_about(gapp); });
        ui::add_action(app, "shortcuts", [gapp] { show_shortcuts(gapp); });
        // Target: a reminder id. Used by notifications.
        auto* show = g_simple_action_new("show-reminder", G_VARIANT_TYPE_STRING);
        ui::connect<void(GSimpleAction*, GVariant*)>(show, "activate", [gapp](GSimpleAction*, GVariant* id) {
            g_application_activate(G_APPLICATION(gapp));
            if (auto* w = ui::Window::from(gtk_application_get_active_window(gapp)))
                w->show_reminder(g_variant_get_string(id, nullptr));
        });
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(show));
        g_object_unref(show);
        auto accel = [gapp](const char* action, const char* key) {
            const char* accels[] = {key, nullptr};
            gtk_application_set_accels_for_action(gapp, action, accels);
        };
        accel("app.quit", "<Control>q");
        accel("app.shortcuts", "<Control>question");
        accel("window.close", "<Control>w");
        accel("win.new-reminder", "<Control>n");
        accel("win.new-list", "<Control><Shift>n");
        accel("win.search", "<Control>f");
        accel("win.show-completed", "<Control>h");
        accel("win.toggle-sidebar", "<Control>b");
        accel("win.toggle-subtasks", "<Control>e");
        for (int n = 1; n <= 10; ++n) {
            auto action = std::format("win.go-{}", n);
            auto key = std::format("<Control>{}", n % 10);  // Ctrl+1 … Ctrl+9, then Ctrl+0
            accel(action.c_str(), key.c_str());
        }
        accel("win.go-to", "<Control>k");
        accel("win.next-view", "<Control>Page_Down");
        accel("win.previous-view", "<Control>Page_Up");
        accel("win.undo", "<Control>z");
        accel("win.redo", "<Control><Shift>z");
    });

    ui::on(app, "activate", [app] { present(app, std::nullopt); });
    ui::connect<int(GApplication*, GApplicationCommandLine*)>(
        app, "command-line", [app](GApplication*, GApplicationCommandLine* cmd) { return handle_command_line(app, cmd); });

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
