#include "dialogs.hpp"

#include <algorithm>
#include <format>
#include <memory>

#include "gtk_util.hpp"
#include "reminders/paths.hpp"
#include "support.hpp"

namespace ui {

namespace {

struct RepeatChoice {
    const char* label;
    const char* rule;  // nullptr = never
};

constexpr RepeatChoice kRepeats[] = {
    {"Never", nullptr},
    {"Daily", "every day"},
    {"Weekdays", "every weekday"},
    {"Weekends", "every weekend"},
    {"Weekly", "every week"},
    {"Every 2 Weeks", "every 2 weeks"},
    {"Monthly", "every month"},
    {"Every 3 Months", "every 3 months"},
    {"Every 6 Months", "every 6 months"},
    {"Yearly", "every year"},
};

// NULL-terminated for gtk_string_list_new().
constexpr const char* kPriorities[] = {"None", "Low", "Medium", "High", nullptr};

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\n");
    return std::string(s.substr(b, e - b + 1));
}

GtkWidget* header_with_buttons(const char* done_label, GtkWidget** cancel, GtkWidget** done) {
    auto* header = adw_header_bar_new();
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    *cancel = gtk_button_new_with_mnemonic("_Cancel");
    *done = gtk_button_new_with_mnemonic(done_label);
    gtk_widget_add_css_class(*done, "suggested-action");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), *cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), *done);
    return header;
}

GtkStringList* string_list(const std::vector<std::string>& items) {
    auto* list = gtk_string_list_new(nullptr);
    for (auto& s : items) gtk_string_list_append(list, s.c_str());
    return list;
}

GDateTime* to_gdatetime(rem::Date d) {
    return g_date_time_new_local(static_cast<int>(d.year()), static_cast<int>(static_cast<unsigned>(d.month())),
                                 static_cast<int>(static_cast<unsigned>(d.day())), 0, 0, 0);
}

rem::Date from_gdatetime(GDateTime* dt) {
    return rem::Date{std::chrono::year{g_date_time_get_year(dt)},
                     std::chrono::month{static_cast<unsigned>(g_date_time_get_month(dt))},
                     std::chrono::day{static_cast<unsigned>(g_date_time_get_day_of_month(dt))}};
}

// Widgets of an open reminder dialog. Owned by the dialog.
struct ReminderDialog {
    GtkWidget* dialog;
    GtkWidget* title;
    GtkWidget* notes;
    GtkWidget* url;
    GtkWidget* date_row;
    GtkWidget* calendar;
    GtkWidget* time_row;
    GtkWidget* hour;
    GtkWidget* minute;
    GtkWidget* repeat;
    GtkWidget* flag;
    GtkWidget* priority;
    GtkWidget* list;
    GtkWidget* tags;
    GtkWidget* subtasks_group = nullptr;
    std::vector<std::string> repeat_rules;  // index → rule ("" = never)
    std::vector<std::string> lists;
    std::string current_list;
    std::vector<std::string> new_subtasks;
    rem::LineFields original;
    std::function<void(ReminderEdit)> on_done;
    bool syncing = false;

    rem::Date selected_date() const {
        GDateTime* dt = gtk_calendar_get_date(GTK_CALENDAR(calendar));
        auto d = from_gdatetime(dt);
        g_date_time_unref(dt);
        return d;
    }

    void update_subtitles() {
        bool has_date = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(date_row));
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(date_row),
                                      has_date ? relative_date(selected_date(), today()).c_str() : "");
        bool has_time = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(time_row));
        auto t = rem::TimeOfDay{static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(hour))),
                                static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(minute)))};
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(time_row), has_time ? format_time(t).c_str() : "");
    }

    ReminderEdit result() const {
        ReminderEdit e;
        auto& f = e.fields;
        f = original;
        f.title = trim(gtk_editable_get_text(GTK_EDITABLE(title)));
        auto u = trim(gtk_editable_get_text(GTK_EDITABLE(url)));
        f.url = u.empty() ? std::nullopt : std::optional{u};

        if (adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(date_row))) {
            f.due_date = selected_date();
            if (adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(time_row)))
                f.due_time = rem::TimeOfDay{static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(hour))),
                                            static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(minute)))};
            else
                f.due_time.reset();
        } else {
            f.due_date.reset();
            f.due_time.reset();
        }

        auto ri = adw_combo_row_get_selected(ADW_COMBO_ROW(repeat));
        auto rule = ri < repeat_rules.size() ? repeat_rules[ri] : std::string();
        f.repeat = rule.empty() ? std::nullopt : std::optional{rule};
        f.flagged = adw_switch_row_get_active(ADW_SWITCH_ROW(flag));
        auto pi = adw_combo_row_get_selected(ADW_COMBO_ROW(priority));
        f.priority = pi <= 3 ? static_cast<rem::Priority>(pi) : rem::Priority::None;

        f.tags.clear();
        std::string tag_text = gtk_editable_get_text(GTK_EDITABLE(tags));
        for (std::size_t i = 0; i < tag_text.size();) {
            auto start = tag_text.find_first_not_of(" ,#", i);
            if (start == std::string::npos) break;
            auto end = tag_text.find_first_of(" ,", start);
            auto t = tag_text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (std::ranges::find(f.tags, t) == f.tags.end()) f.tags.push_back(t);
            i = end == std::string::npos ? tag_text.size() : end;
        }

        GtkTextIter a, b;
        auto* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notes));
        gtk_text_buffer_get_bounds(buf, &a, &b);
        e.notes = take_string(gtk_text_buffer_get_text(buf, &a, &b, FALSE));
        while (!e.notes.empty() && (e.notes.back() == '\n' || e.notes.back() == ' ')) e.notes.pop_back();

        auto li = adw_combo_row_get_selected(ADW_COMBO_ROW(list));
        e.list = li < lists.size() ? lists[li] : current_list;
        e.new_subtasks = new_subtasks;
        return e;
    }
};

}  // namespace

void show_reminder_dialog(GtkWidget* parent, const rem::Reminder& r, bool is_subtask, const std::string& list,
                          const std::vector<std::string>& lists, std::function<void(ReminderEdit)> on_done) {
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(ADW_DIALOG(dialog), "Details");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 480);
    adw_dialog_set_content_height(ADW_DIALOG(dialog), 680);
    auto* d = attach(dialog, "state", std::make_unique<ReminderDialog>());
    d->dialog = GTK_WIDGET(dialog);
    d->lists = lists;
    d->current_list = list;
    d->original = r.fields();
    d->on_done = std::move(on_done);

    GtkWidget *cancel, *done;
    auto* header = header_with_buttons("_Done", &cancel, &done);
    auto* page = adw_preferences_page_new();

    // Title, notes, URL.
    auto* g1 = adw_preferences_group_new();
    d->title = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->title), "Title");
    gtk_editable_set_text(GTK_EDITABLE(d->title), r.title.c_str());
    d->url = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->url), "URL");
    gtk_editable_set_text(GTK_EDITABLE(d->url), r.url.value_or("").c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), d->title);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), d->url);

    auto* notes_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(notes_group), "Notes");
    d->notes = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->notes), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(d->notes), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(d->notes), 12);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d->notes), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(d->notes), 12);
    gtk_widget_set_size_request(d->notes, -1, 96);
    gtk_widget_add_css_class(d->notes, "card");
    gtk_widget_add_css_class(d->notes, "notes-view");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->notes)), r.notes.c_str(), -1);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(notes_group), d->notes);

    // Date, time, repeat.
    auto* g2 = adw_preferences_group_new();
    d->date_row = adw_expander_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->date_row), "Date");
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(d->date_row), TRUE);
    d->calendar = gtk_calendar_new();
    gtk_widget_set_margin_top(d->calendar, 6);
    gtk_widget_set_margin_bottom(d->calendar, 6);
    gtk_widget_set_halign(d->calendar, GTK_ALIGN_CENTER);
    {
        GDateTime* dt = to_gdatetime(r.due_date.value_or(today()));
        gtk_calendar_set_date(GTK_CALENDAR(d->calendar), dt);
        g_date_time_unref(dt);
    }
    auto* calendar_row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(calendar_row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(calendar_row), d->calendar);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(d->date_row), calendar_row);

    d->time_row = adw_expander_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->time_row), "Time");
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(d->time_row), TRUE);
    auto t = r.due_time.value_or(rem::TimeOfDay{9, 0});
    d->hour = adw_spin_row_new_with_range(0, 23, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->hour), "Hour");
    adw_spin_row_set_value(ADW_SPIN_ROW(d->hour), t.hour);
    adw_spin_row_set_wrap(ADW_SPIN_ROW(d->hour), TRUE);
    d->minute = adw_spin_row_new_with_range(0, 59, 5);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->minute), "Minute");
    adw_spin_row_set_value(ADW_SPIN_ROW(d->minute), t.minute);
    adw_spin_row_set_wrap(ADW_SPIN_ROW(d->minute), TRUE);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(d->time_row), d->hour);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(d->time_row), d->minute);

    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(d->date_row), r.due_date.has_value());
    adw_expander_row_set_expanded(ADW_EXPANDER_ROW(d->date_row), FALSE);
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(d->time_row), r.due_time.has_value());
    adw_expander_row_set_expanded(ADW_EXPANDER_ROW(d->time_row), FALSE);

    std::vector<std::string> repeat_labels;
    std::size_t repeat_selected = 0;
    for (auto& c : kRepeats) {
        repeat_labels.emplace_back(c.label);
        d->repeat_rules.emplace_back(c.rule ? c.rule : "");
        if (r.repeat && c.rule && *r.repeat == c.rule) repeat_selected = repeat_labels.size() - 1;
    }
    if (r.repeat && repeat_selected == 0) {  // a rule written by hand
        repeat_labels.push_back("Custom: " + *r.repeat);
        d->repeat_rules.push_back(*r.repeat);
        repeat_selected = repeat_labels.size() - 1;
    }
    d->repeat = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->repeat), "Repeat");
    auto* repeat_model = string_list(repeat_labels);
    adw_combo_row_set_model(ADW_COMBO_ROW(d->repeat), G_LIST_MODEL(repeat_model));
    g_object_unref(repeat_model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(d->repeat), static_cast<guint>(repeat_selected));

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), d->date_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), d->time_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), d->repeat);

    // A time needs a date; no date means no time.
    connect<void(GObject*, GParamSpec*)>(d->time_row, "notify::enable-expansion", [d](GObject*, GParamSpec*) {
        if (adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(d->time_row)))
            adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(d->date_row), TRUE);
        d->update_subtitles();
    });
    connect<void(GObject*, GParamSpec*)>(d->date_row, "notify::enable-expansion", [d](GObject*, GParamSpec*) {
        if (!adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(d->date_row)))
            adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(d->time_row), FALSE);
        d->update_subtitles();
    });
    on(d->calendar, "day-selected", [d] { d->update_subtitles(); });
    connect<void(GObject*, GParamSpec*)>(d->hour, "notify::value", [d](GObject*, GParamSpec*) { d->update_subtitles(); });
    connect<void(GObject*, GParamSpec*)>(d->minute, "notify::value", [d](GObject*, GParamSpec*) { d->update_subtitles(); });
    d->update_subtitles();

    // Flag, priority, list, tags.
    auto* g3 = adw_preferences_group_new();
    d->flag = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->flag), "Flag");
    adw_switch_row_set_active(ADW_SWITCH_ROW(d->flag), r.flagged);
    d->priority = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->priority), "Priority");
    auto* priority_model = gtk_string_list_new(kPriorities);
    adw_combo_row_set_model(ADW_COMBO_ROW(d->priority), G_LIST_MODEL(priority_model));
    g_object_unref(priority_model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(d->priority), static_cast<guint>(r.priority));
    d->list = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->list), "List");
    auto* list_model = string_list(lists);
    adw_combo_row_set_model(ADW_COMBO_ROW(d->list), G_LIST_MODEL(list_model));
    g_object_unref(list_model);
    auto at = std::ranges::find(lists, list);
    adw_combo_row_set_selected(ADW_COMBO_ROW(d->list), static_cast<guint>(at - lists.begin()));
    d->tags = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->tags), "Tags");
    std::string tag_text;
    for (auto& tag : r.tags) tag_text += (tag_text.empty() ? "#" : " #") + tag;
    gtk_editable_set_text(GTK_EDITABLE(d->tags), tag_text.c_str());
    for (auto* w : {d->flag, d->priority, d->list, d->tags}) adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), w);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g1));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(notes_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g2));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g3));

    // Subtasks: existing ones are listed; new ones are added on Done.
    if (!is_subtask) {
        auto* g4 = adw_preferences_group_new();
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g4), "Subtasks");
        d->subtasks_group = g4;
        auto add_subtask_row = [g4](const std::string& title, bool done_) {
            auto* row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.empty() ? " " : title.c_str());
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_action_row_add_prefix(ADW_ACTION_ROW(row),
                                      icon(done_ ? "object-select-symbolic" : "radio-symbolic", {"dim-label"}));
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), row);
        };
        for (auto& s : r.subtasks) add_subtask_row(s.title, s.done);
        auto* entry = adw_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), "Add Subtask");
        adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(entry), TRUE);
        auto add = [d, entry, g4, add_subtask_row] {
            auto text = trim(gtk_editable_get_text(GTK_EDITABLE(entry)));
            if (text.empty()) return;
            d->new_subtasks.push_back(text);
            // Keep the entry last.
            g_object_ref(entry);
            adw_preferences_group_remove(ADW_PREFERENCES_GROUP(g4), entry);
            add_subtask_row(text, false);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), entry);
            g_object_unref(entry);
            gtk_editable_set_text(GTK_EDITABLE(entry), "");
            gtk_widget_grab_focus(entry);
        };
        on(entry, "apply", add);
        on(entry, "entry-activated", add);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), entry);
        adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g4));
    }

    auto* g5 = adw_preferences_group_new();
    auto* del = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(del), "Delete Reminder");
    gtk_widget_add_css_class(del, "destructive-action");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), del);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g5));

    on(cancel, "clicked", [dialog] { adw_dialog_close(ADW_DIALOG(dialog)); });
    on(done, "clicked", [d, dialog] {
        auto e = d->result();
        auto cb = d->on_done;
        adw_dialog_close(ADW_DIALOG(dialog));
        cb(std::move(e));
    });
    on(del, "activated", [d, dialog] {
        ReminderEdit e;
        e.deleted = true;
        auto cb = d->on_done;
        adw_dialog_close(ADW_DIALOG(dialog));
        cb(std::move(e));
    });

    auto* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(ADW_DIALOG(dialog), view);
    adw_dialog_set_default_widget(ADW_DIALOG(dialog), done);
    add_shortcut(GTK_WIDGET(dialog), "<Control>s", [done] { gtk_widget_activate(done); });
    adw_dialog_present(ADW_DIALOG(dialog), parent);
}

namespace {

struct ListDialog {
    GtkWidget* name;
    GtkWidget* error;
    GtkWidget* preview;
    GtkWidget* done;
    ListEdit edit;
    std::function<std::string(const ListEdit&)> validate;
    std::function<void(ListEdit)> on_done;

    void update() {
        edit.name = trim(gtk_editable_get_text(GTK_EDITABLE(name)));
        auto err = validate(edit);
        // Don't nag about an empty name before anything was typed.
        bool show = !err.empty() && !edit.name.empty();
        gtk_label_set_text(GTK_LABEL(error), err.c_str());
        gtk_widget_set_visible(error, show);
        gtk_widget_set_sensitive(done, err.empty());

        for (auto c : rem::kColors) gtk_widget_remove_css_class(preview, ("color-" + std::string(c)).c_str());
        gtk_widget_add_css_class(preview, ("color-" + edit.color).c_str());
        gtk_image_set_from_icon_name(GTK_IMAGE(preview), list_icon_name(edit.icon));
    }
};

}  // namespace

namespace {

// The list dialog, or with `tag` set the tag dialog (no name row).
void present_style_dialog(GtkWidget* parent, std::optional<ListEdit> existing, const std::string* tag,
                          std::function<std::string(const ListEdit&)> validate,
                          std::function<void(ListEdit)> on_done) {
    bool is_new = !existing;
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(ADW_DIALOG(dialog), tag ? "Tag Info" : is_new ? "New List" : "List Info");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 440);
    auto* d = attach(dialog, "state", std::make_unique<ListDialog>());
    d->edit = existing.value_or(ListEdit{});
    d->validate = std::move(validate);
    d->on_done = std::move(on_done);

    GtkWidget* cancel;
    auto* header = header_with_buttons(is_new ? "_Create" : "_Done", &cancel, &d->done);
    auto* page = adw_preferences_page_new();

    auto* top = adw_preferences_group_new();
    d->preview = icon(list_icon_name(d->edit.icon), {"list-icon", "list-icon-large"});
    gtk_widget_set_halign(d->preview, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(d->preview, 18);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(top), d->preview);

    d->name = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->name), "Name");
    gtk_editable_set_text(GTK_EDITABLE(d->name), d->edit.name.c_str());
    adw_entry_row_set_activates_default(ADW_ENTRY_ROW(d->name), TRUE);
    auto* name_group = adw_preferences_group_new();
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(name_group), d->name);
    if (tag) {  // the tag's name, not editable
        gtk_widget_set_visible(name_group, FALSE);
        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(top), ("#" + *tag).c_str());
    }
    d->error = label("", {"error", "caption"});
    gtk_widget_set_margin_top(d->error, 6);
    gtk_widget_set_margin_start(d->error, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(name_group), d->error);
    on(d->name, "changed", [d] { d->update(); });

    auto* colors_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(colors_group), "Color");
    auto* colors = adw_wrap_box_new();
    adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(colors), 10);
    adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(colors), 10);
    GtkWidget* color_group_leader = nullptr;
    for (auto c : rem::kColors) {
        auto* b = gtk_toggle_button_new();
        gtk_widget_add_css_class(b, "color-swatch");
        gtk_widget_add_css_class(b, ("color-" + std::string(c)).c_str());
        gtk_widget_set_tooltip_text(b, color_label(c).c_str());
        if (color_group_leader) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(color_group_leader));
        else color_group_leader = b;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), d->edit.color == c);
        connect<void(GtkToggleButton*)>(b, "toggled", [d, c = std::string(c)](GtkToggleButton* t) {
            if (!gtk_toggle_button_get_active(t)) return;
            d->edit.color = c;
            d->update();
        });
        adw_wrap_box_append(ADW_WRAP_BOX(colors), b);
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(colors_group), colors);

    auto* icons_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(icons_group), "Icon");
    auto* icons = adw_wrap_box_new();
    adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(icons), 6);
    adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(icons), 6);
    GtkWidget* icon_group_leader = nullptr;
    for (auto i : rem::kIcons) {
        auto* b = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(b), list_icon_name(i));
        gtk_widget_add_css_class(b, "icon-swatch");
        gtk_widget_add_css_class(b, "circular");
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_tooltip_text(b, color_label(i).c_str());
        if (icon_group_leader) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(icon_group_leader));
        else icon_group_leader = b;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), d->edit.icon == i);
        connect<void(GtkToggleButton*)>(b, "toggled", [d, i = std::string(i)](GtkToggleButton* t) {
            if (!gtk_toggle_button_get_active(t)) return;
            d->edit.icon = i;
            d->update();
        });
        adw_wrap_box_append(ADW_WRAP_BOX(icons), b);
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(icons_group), icons);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(top));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(name_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(colors_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(icons_group));

    on(cancel, "clicked", [dialog] { adw_dialog_close(ADW_DIALOG(dialog)); });
    on(d->done, "clicked", [d, dialog] {
        d->update();
        if (!gtk_widget_get_sensitive(d->done)) return;
        auto e = d->edit;
        auto cb = d->on_done;
        adw_dialog_close(ADW_DIALOG(dialog));
        cb(std::move(e));
    });

    auto* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(ADW_DIALOG(dialog), view);
    adw_dialog_set_default_widget(ADW_DIALOG(dialog), d->done);
    add_shortcut(GTK_WIDGET(dialog), "<Control>s", [d] { gtk_widget_activate(d->done); });
    if (!tag) adw_dialog_set_focus(ADW_DIALOG(dialog), d->name);
    d->update();
    adw_dialog_present(ADW_DIALOG(dialog), parent);
}

}  // namespace

void show_list_dialog(GtkWidget* parent, std::optional<ListEdit> existing,
                      std::function<std::string(const ListEdit&)> validate, std::function<void(ListEdit)> on_done) {
    present_style_dialog(parent, std::move(existing), nullptr, std::move(validate), std::move(on_done));
}

void show_tag_dialog(GtkWidget* parent, const std::string& tag, ListEdit style, std::function<void(ListEdit)> on_done) {
    present_style_dialog(parent, std::move(style), &tag, [](const ListEdit&) { return std::string(); },
                         std::move(on_done));
}

namespace {

// The back ends offered, in the Type row's order.
constexpr rem::BackendKind kSourceTypes[] = {rem::BackendKind::Syncthing, rem::BackendKind::Local,
                                             rem::BackendKind::Caldav, rem::BackendKind::Webdav, rem::BackendKind::Git};

struct SourceDialog {
    SourceEdit edit;
    std::function<std::string(const SourceEdit&)> validate;
    GtkWidget* done = nullptr;
    GtkWidget* error = nullptr;
    GtkWidget* type_row = nullptr;
    GtkWidget* folder_row = nullptr;
    GtkWidget* server_group = nullptr;  // CalDAV and WebDAV only
    GtkWidget* repo_group = nullptr;    // git only
    GtkWidget* address_row = nullptr;
    // A new server source's folder (or a git clone's) follows its name and
    // type (the default place in $XDG_DATA_HOME) until one is chosen.
    bool folder_chosen = false;

    bool on_server() const { return rem::has_server(edit.backend); }
    bool git() const { return edit.backend == rem::BackendKind::Git; }
    // The folder can be the app's own: a local copy, or a clone.
    bool own_folder() const { return on_server() || (git() && !edit.git.url.empty()); }

    void update() {
        if (edit.is_new && !folder_chosen)
            edit.folder = own_folder() ? rem::default_copy_folder(
                                             edit.backend, rem::new_source_name(rem::SourceConfig{
                                                               "", edit.backend, {}, edit.title, edit.dav, edit.git}))
                                       : std::filesystem::path{};
        auto err = validate(edit);
        gtk_label_set_text(GTK_LABEL(error), err.c_str());
        gtk_widget_set_visible(error, !err.empty());
        gtk_widget_set_sensitive(done, err.empty());
        gtk_widget_set_visible(server_group, on_server());
        gtk_widget_set_visible(repo_group, git());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row), on_server() ? "Local Copy" : "Folder");
        auto folder = edit.folder.empty() ? std::string("None chosen") : rem::contract_path(edit.folder);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(folder_row), folder.c_str());
        bool webdav = edit.backend == rem::BackendKind::Webdav;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(type_row),
                                    edit.backend == rem::BackendKind::Syncthing ? "A folder Syncthing keeps in step; conflict copies are merged"
                                    : edit.backend == rem::BackendKind::Local   ? "A folder on this computer; files are read and saved as they are"
                                    : git()  ? "A folder in a git repository; changes are committed, pulled and pushed"
                                    : webdav ? "List files in a folder on a WebDAV server, with a copy kept here"
                                             : "Task lists on a CalDAV server, with a copy kept here");
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(server_group),
            webdav ? "The list files are kept in a folder on the server (Nextcloud, ownCloud, a NAS, …), made if "
                     "it isn't there."
                   : "Each of the account's task lists becomes a list here (Nextcloud, Fastmail, Radicale, …).");
        gtk_widget_set_tooltip_text(address_row,
                                    webdav ? "The folder's address, e.g. "
                                             "https://cloud.example.com/remote.php/dav/files/you/Reminders/"
                                           : "The server's address, e.g. https://caldav.fastmail.com/");
    }
};

}  // namespace

void show_source_dialog(GtkWidget* parent, SourceEdit source, std::function<std::string(const SourceEdit&)> validate,
                        std::function<void(SourceEdit)> on_done, std::function<void()> on_remove) {
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(ADW_DIALOG(dialog), source.is_new ? "Add Source" : "Source Info");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 480);
    auto* d = attach(dialog, "state", std::make_unique<SourceDialog>());
    d->edit = std::move(source);
    d->validate = std::move(validate);
    d->folder_chosen = !d->edit.is_new || !d->edit.folder.empty();

    GtkWidget* cancel;
    auto* header = header_with_buttons(d->edit.is_new ? "_Add" : "_Done", &cancel, &d->done);
    auto* page = adw_preferences_page_new();

    auto* main = adw_preferences_group_new();
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(main),
        d->edit.is_new ? "Where lists come from: a folder, a git repository, or an account on a CalDAV or WebDAV server."
                       : std::format("Settings name it “{}” ([source.{}]).", d->edit.name, d->edit.name).c_str());
    auto* title = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(title), "Name");
    gtk_editable_set_text(GTK_EDITABLE(title), d->edit.title.c_str());
    on(title, "changed", [d, title] {
        d->edit.title = trim(gtk_editable_get_text(GTK_EDITABLE(title)));
        d->update();
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(main), title);

    d->type_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->type_row), "Type");
    auto* kinds = string_list({"Syncthing", "Local Folder", "CalDAV", "WebDAV", "Git"});
    adw_combo_row_set_model(ADW_COMBO_ROW(d->type_row), G_LIST_MODEL(kinds));
    g_object_unref(kinds);
    auto type_index = [](rem::BackendKind k) {
        return static_cast<guint>(std::ranges::find(kSourceTypes, k) - std::begin(kSourceTypes));
    };
    adw_combo_row_set_selected(ADW_COMBO_ROW(d->type_row), type_index(d->edit.backend));
    connect<void(GObject*, GParamSpec*)>(d->type_row, "notify::selected", [d](GObject*, GParamSpec*) {
        auto i = adw_combo_row_get_selected(ADW_COMBO_ROW(d->type_row));
        if (i >= std::size(kSourceTypes)) return;
        bool was_on_server = d->on_server();
        d->edit.backend = kSourceTypes[i];
        // A new source's default local copy isn't a folder to sync.
        if (d->edit.is_new && was_on_server && !d->on_server() && !d->folder_chosen) d->edit.folder.clear();
        d->update();
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(main), d->type_row);

    d->folder_row = adw_action_row_new();
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(d->folder_row), TRUE);
    auto* change = gtk_button_new_with_mnemonic(d->edit.is_new ? "_Choose…" : "_Change…");
    gtk_widget_set_valign(change, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(d->folder_row), change);
    on(change, "clicked", [d, dialog] {
        auto* chooser = gtk_file_dialog_new();
        gtk_file_dialog_set_title(chooser, d->on_server() ? "Choose Where to Keep the Lists" : "Choose the Source's Folder");
        if (!d->edit.folder.empty() && std::filesystem::is_directory(d->edit.folder)) {
            auto current = Obj<GFile>::adopt(g_file_new_for_path(d->edit.folder.c_str()));
            gtk_file_dialog_set_initial_folder(chooser, current.get());
        }
        auto keep = Obj<GtkWidget>::ref(GTK_WIDGET(dialog));
        gtk_file_dialog_select_folder(
            chooser, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(dialog))), nullptr,
            [](GObject* src, GAsyncResult* res, gpointer data) {
                std::unique_ptr<Obj<GtkWidget>> keep(static_cast<Obj<GtkWidget>*>(data));
                auto* d = static_cast<SourceDialog*>(g_object_get_data(G_OBJECT(keep->get()), "state"));
                GError* error = nullptr;
                auto file = Obj<GFile>::adopt(gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &error));
                if (error) {
                    g_error_free(error);  // cancelled
                    return;
                }
                if (auto* path = g_file_get_path(file.get())) {
                    d->edit.folder = path;
                    g_free(path);
                    d->folder_chosen = true;
                    // A new folder source: Syncthing inside a Syncthing folder,
                    // git inside a git repository, else local.
                    if (d->edit.is_new && (d->edit.backend == rem::BackendKind::Syncthing ||
                                           d->edit.backend == rem::BackendKind::Local)) {
                        d->edit.backend = rem::detect_backend(d->edit.folder);
                        if (d->edit.backend == rem::BackendKind::Local && rem::in_git_repo(d->edit.folder))
                            d->edit.backend = rem::BackendKind::Git;
                        adw_combo_row_set_selected(
                            ADW_COMBO_ROW(d->type_row),
                            static_cast<guint>(std::ranges::find(kSourceTypes, d->edit.backend) - std::begin(kSourceTypes)));
                    }
                    d->update();
                }
            },
            new Obj<GtkWidget>(std::move(keep)));
        g_object_unref(chooser);
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(main), d->folder_row);

    auto* is_default = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(is_default), "Default Source");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(is_default), "New lists go here unless you choose another");
    adw_switch_row_set_active(ADW_SWITCH_ROW(is_default), d->edit.is_default);
    gtk_widget_set_sensitive(is_default, d->edit.is_new || !d->edit.is_default);  // another source must take over by being chosen
    connect<void(GObject*, GParamSpec*)>(is_default, "notify::active", [d, is_default](GObject*, GParamSpec*) {
        d->edit.is_default = adw_switch_row_get_active(ADW_SWITCH_ROW(is_default));
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(main), is_default);

    // CalDAV and WebDAV: the server, shown only for those types (its
    // description and the address's hint are set by update()).
    d->server_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(d->server_group), "Server");
    auto entry = [&](const char* row_title, std::string* value, const char* hint, GtkWidget* group = nullptr) {
        auto* row = adw_entry_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), row_title);
        gtk_editable_set_text(GTK_EDITABLE(row), value->c_str());
        if (hint) gtk_widget_set_tooltip_text(row, hint);
        on(row, "changed", [d, row, value] {
            *value = trim(gtk_editable_get_text(GTK_EDITABLE(row)));
            d->update();
        });
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group ? group : d->server_group), row);
        return row;
    };
    d->address_row = entry("Address", &d->edit.dav.url, nullptr);
    entry("Username", &d->edit.dav.username, nullptr);
    entry("Password Command", &d->edit.dav.password_command,
          "A command that prints the password, e.g. “secret-tool lookup service reminders-caldav” or “pass show caldav”");
    auto* every = adw_spin_row_new_with_range(1, 1440, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(every), "Sync Every");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(every), "Minutes; changes made here are sent straight away");
    adw_spin_row_set_value(ADW_SPIN_ROW(every), d->edit.dav.interval);
    connect<void(GObject*, GParamSpec*)>(every, "notify::value", [d, every](GObject*, GParamSpec*) {
        d->edit.dav.interval = static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(every)));
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(d->server_group), every);

    // git: the repository, shown only for that type.
    d->repo_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(d->repo_group), "Repository");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(d->repo_group),
        "Changed lists are committed and pushed, and changes from elsewhere pulled and merged. Signing in uses your "
        "git set-up (SSH keys, a credential helper).");
    entry("Clone From", &d->edit.git.url,
          "Optional: the repository to clone when the folder isn't one yet, e.g. git@github.com:you/notes.git",
          d->repo_group);
    entry("Remote", &d->edit.git.remote, "Optional; origin if empty", d->repo_group);
    entry("Branch", &d->edit.git.branch, "Optional; the branch checked out if empty", d->repo_group);
    auto* git_every = adw_spin_row_new_with_range(1, 1440, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(git_every), "Sync Every");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(git_every), "Minutes; changes made here are sent straight away");
    adw_spin_row_set_value(ADW_SPIN_ROW(git_every), d->edit.git.interval);
    connect<void(GObject*, GParamSpec*)>(git_every, "notify::value", [d, git_every](GObject*, GParamSpec*) {
        d->edit.git.interval = static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(git_every)));
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(d->repo_group), git_every);

    d->error = label("", {"error", "caption"});
    gtk_widget_set_margin_start(d->error, 12);
    auto* errors = adw_preferences_group_new();
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(errors), d->error);

    auto* danger = adw_preferences_group_new();
    auto* remove = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remove), "_Remove Source…");
    adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(remove), TRUE);
    gtk_widget_add_css_class(remove, "destructive-action");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(danger), remove);
    connect<void(AdwButtonRow*)>(remove, "activated", [dialog, on_remove](AdwButtonRow*) {
        adw_dialog_close(ADW_DIALOG(dialog));
        if (on_remove) on_remove();
    });

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(main));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(d->server_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(d->repo_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(errors));
    if (!d->edit.is_new) adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(danger));

    on(cancel, "clicked", [dialog] { adw_dialog_close(ADW_DIALOG(dialog)); });
    on(d->done, "clicked", [d, dialog, on_done] {
        d->update();
        if (!gtk_widget_get_sensitive(d->done)) return;
        auto e = d->edit;
        adw_dialog_close(ADW_DIALOG(dialog));
        on_done(std::move(e));
    });

    auto* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(ADW_DIALOG(dialog), view);
    adw_dialog_set_default_widget(ADW_DIALOG(dialog), d->done);
    add_shortcut(GTK_WIDGET(dialog), "<Control>s", [d] { gtk_widget_activate(d->done); });
    d->update();
    adw_dialog_present(ADW_DIALOG(dialog), parent);
}

void show_sources_dialog(GtkWidget* parent, const std::vector<SourceRow>& rows, std::function<void(std::string)> on_open,
                         std::function<void()> on_add) {
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(ADW_DIALOG(dialog), "Sources");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 460);
    auto* page = adw_preferences_page_new();
    auto* group = adw_preferences_group_new();
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          "Where your lists come from: folders kept in step with your other devices, "
                                          "and accounts on CalDAV and WebDAV servers.");
    for (auto& r : rows) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), r.title.c_str());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), r.detail.c_str());
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon("go-next-symbolic", {}));
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        connect<void(AdwActionRow*)>(row, "activated", [dialog, on_open, name = r.name](AdwActionRow*) {
            adw_dialog_close(ADW_DIALOG(dialog));
            on_open(name);
        });
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }
    auto* adds = adw_preferences_group_new();
    auto add_row = [&](const char* title, const char* icon_name, std::function<void()> fn) {
        auto* add = adw_button_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), title);
        adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(add), TRUE);
        adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(add), icon_name);
        connect<void(AdwButtonRow*)>(add, "activated", [dialog, fn](AdwButtonRow*) {
            adw_dialog_close(ADW_DIALOG(dialog));
            fn();
        });
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(adds), add);
    };
    add_row("_Add Source…", "list-add-symbolic", std::move(on_add));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(adds));
    auto* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
    adw_dialog_set_child(ADW_DIALOG(dialog), view);
    adw_dialog_present(ADW_DIALOG(dialog), parent);
}

}  // namespace ui
