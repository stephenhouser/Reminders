#include "tui.hpp"

#include <ncurses.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <clocale>
#include <cwchar>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
#include "reminders/settings.hpp"
#ifdef REMINDERS_NETWORK
#include "reminders/sync_runner.hpp"
#endif
#include "editfile.hpp"
#include "text.hpp"

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// --- UTF-8 text in fixed-width cells ---------------------------------------

std::wstring widen(std::string_view s) {
    std::wstring out;
    std::mbstate_t st{};
    const char* p = s.data();
    const char* end = s.data() + s.size();
    while (p < end) {
        wchar_t wc;
        auto n = std::mbrtowc(&wc, p, static_cast<std::size_t>(end - p), &st);
        if (n == static_cast<std::size_t>(-1) || n == static_cast<std::size_t>(-2)) {
            wc = L'?';
            n = 1;
            st = {};
        } else if (n == 0) {
            n = 1;
        }
        if (wc != 0xFE0F) out += wc;  // variation selectors confuse terminals' widths
        p += n;
    }
    return out;
}

std::string narrow(const std::wstring& w) {
    std::string out;
    std::mbstate_t st{};
    char buf[MB_LEN_MAX];
    for (wchar_t wc : w) {
        auto n = std::wcrtomb(buf, wc, &st);
        if (n != static_cast<std::size_t>(-1)) out.append(buf, n);
    }
    return out;
}

int cell_width(wchar_t c) {
    int w = wcwidth(c);
    return w < 0 ? 1 : w;
}

int text_width(const std::wstring& w) {
    int n = 0;
    for (auto c : w) n += cell_width(c);
    return n;
}

// Draws `s` at (y, x) in at most `width` cells, ending in "…" if cut. Returns
// the cells used.
int put(int y, int x, std::string_view s, int width) {
    if (width <= 0) return 0;
    auto w = widen(s);
    std::wstring out;
    int used = 0;
    for (auto c : w) {
        int cw = cell_width(c);
        if (used + cw > width) {
            while (!out.empty() && used + 1 > width) {
                used -= cell_width(out.back());
                out.pop_back();
            }
            out += L'…';
            ++used;
            break;
        }
        out += c;
        used += cw;
    }
    mvaddnwstr(y, x, out.c_str(), static_cast<int>(out.size()));
    return used;
}

// --- colours -------------------------------------------------------------------

// Nearest xterm-256 colour to an RGB value (from the 6×6×6 cube).
short xterm256(term::Rgb c) {
    auto level = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    return static_cast<short>(16 + 36 * level(c.r) + 6 * level(c.g) + level(c.b));
}

enum Pair : short { kDim = 1, kRed, kSelected, kHeading, kStatus, kMarked, kFirstListColor };

std::map<std::string, short> g_color_pairs;

void setup_colors() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(kDim, COLORS >= 256 ? 244 : COLOR_WHITE, -1);
    init_pair(kRed, COLOR_RED, -1);
    init_pair(kSelected, -1, COLORS >= 256 ? 237 : COLOR_BLUE);
    init_pair(kHeading, -1, -1);
    init_pair(kStatus, COLORS >= 256 ? 250 : COLOR_WHITE, COLORS >= 256 ? 236 : COLOR_BLACK);
    init_pair(kMarked, COLORS >= 256 ? 75 : COLOR_CYAN, -1);  // the * beside a marked reminder
    short next = kFirstListColor;
    for (auto c : rem::kColors) {
        auto rgb = term::color_rgb(c);
        init_pair(next, COLORS >= 256 ? xterm256(rgb) : COLOR_CYAN, -1);
        g_color_pairs[std::string(c)] = next++;
    }
}

attr_t list_color(const std::string& color) {
    if (!has_colors()) return A_BOLD;
    auto it = g_color_pairs.find(color);
    return it == g_color_pairs.end() ? 0 : COLOR_PAIR(it->second);
}

attr_t dim() { return has_colors() ? COLOR_PAIR(kDim) : A_DIM; }

// --- the interface -------------------------------------------------------------

struct View {
    enum Kind { Today, Scheduled, All, Flagged, Completed, AllReminders, List, Tag, Search } kind = Today;
    std::string name;
    bool operator==(const View&) const = default;
};

// The smart lists' names in settings.ini, by View::Kind.
constexpr std::pair<int, const char*> kViewSettings[] = {
    {0, "today"}, {1, "scheduled"}, {2, "all"}, {3, "flagged"}, {4, "completed"}, {5, "all-reminders"}};

// Alt+Shift+↑ / ↓ (move the sidebar group), as internal keys outside the
// range of characters.
constexpr wint_t kGroupUp = 0x110010, kGroupDown = 0x110011;
constexpr wint_t kDelete = 0x110000;  // the Delete key, outside the range of characters

struct SidebarEntry {
    View view;
    std::string title;
    std::string color;
    int count = -1;
    // Headings: a plain Heading is just a label; a FoldHeading (a collapsible
    // group's) can be selected, and Enter/Space folds or unfolds the group.
    enum Kind { Item, Heading, FoldHeading } kind = Item;
    rem::SidebarGroup group = rem::SidebarGroup::smart_lists();  // the group the row belongs to
    bool hidden = false;  // hidden in settings.ini, showing because of show-hidden (dimmed)
};

struct Line {
    enum Kind { Heading, Item, Note } kind;
    std::string id;  // for items
    std::string text;
    std::string color;
    int depth = 0;
    // Items: the Markdown line in parts, so the date can be coloured.
    std::string due{}, after{}, where{};
    bool done = false, overdue = false;
};

class Tui {
public:
    Tui(rem::Library& store, bool remember)
        : store_(store), remember_(remember) {}
    int run();
    void set_show_key_numbers(bool on) { show_key_numbers_ = on, key_numbers_override_ = on; }

private:
    rem::Library& store_;  // every source
#ifdef REMINDERS_NETWORK
    std::unique_ptr<rem::SyncRunner> sync_;  // CalDAV and WebDAV sources, in the background
#endif
    void check_sync();
    bool remember_;
    rem::History history_;
    View view_;
    bool focus_items_ = false;
    bool show_completed_ = false;
    bool hide_subtasks_ = false;   // Ctrl+E
    bool hide_sidebar_ = !rem::load_bool_setting("show-sidebar", true);  // Ctrl+B; shared with the app
    bool show_key_numbers_ = rem::load_bool_setting("show-key-numbers");
    std::optional<bool> key_numbers_override_;  // --show-key-numbers / --hide-key-numbers
    std::vector<rem::SidebarGroup> order_ = rem::load_sidebar_order(source_names());
    rem::SmartListsLayout smart_ = rem::load_smart_lists_layout();
    std::map<std::string, rem::GroupLayout> lists_layouts_;  // by source; loaded as needed
    rem::GroupLayout tags_ = rem::load_tags_layout();
    rem::HiddenEntries hidden_ = rem::load_hidden();  // lists-hidden, tags-hidden, show-hidden
    // Extended key codes for the GUI's modified keys, 0 if the terminal lacks them.
    int alt_up_ = 0, alt_down_ = 0, alt_shift_up_ = 0, alt_shift_down_ = 0, ctrl_page_down_ = 0,
        ctrl_page_up_ = 0;
    int side_sel_ = 0;
    std::string item_sel_;  // selected reminder id
    // Marked reminders (v, * marks all, Esc clears): while any are marked,
    // the editing keys act on all of them instead of the selected one.
    std::set<std::string> marked_;
    int item_scroll_ = 0;
    std::string message_;
    std::map<std::string, std::pair<fs::file_time_type, std::uintmax_t>> seen_;  // folder signature

    std::vector<SidebarEntry> sidebar();        // every row, headings included
    std::vector<SidebarEntry> sidebar_items();  // the selectable lists, in order (numbered)
    std::vector<SidebarEntry> smart_entries();  // the smart lists the settings show
    std::vector<rem::SidebarGroup> showing_groups();
    std::vector<std::string> source_names();
    std::string group_title(const rem::SidebarGroup& group);  // "My Lists" with one source, else its title
    std::vector<std::string> list_keys();                     // every list, as "source/name"
    rem::GroupLayout* layout_of(const rem::SidebarGroup& group);  // nullptr for the smart lists
    bool folded(const rem::SidebarGroup& group);
    void toggle_fold(const rem::SidebarGroup& group);
    void move_group(int delta);
    void move_entry(int delta);
    void edit_settings();
    void toggle_hidden();
    void toggle_show_hidden();
    void load_layout();
    View home_view();
    std::vector<Line> lines();
    std::vector<rem::Ref> view_refs();
    // As under the app's title, long and short: {"6 Reminders / 3 Complete", "6/3"}.
    std::pair<std::string, std::string> view_count();
    void draw();
    void draw_sidebar(int width, int height);
    void draw_items(int x, int width, int height);
    void draw_status();
    std::optional<std::string> prompt(const std::string& label, const std::string& initial = "");
    std::optional<std::string> edit_line(int y, int x, int width, const std::string& initial, attr_t attr);
    void edit_title_in_place(const std::string& id);
    // Where the selected reminder's title is on screen (set while drawing).
    int title_y_ = -1, title_x_ = 0, title_width_ = 0;
    bool confirm(const std::string& question);
    void show_help();
    // Edits every field of a reminder in the user's editor.
    void edit_in_editor(const std::string& id);
    bool handle_key(wint_t key, bool is_function_key, bool alt = false);
    std::vector<std::string> shown_items();   // the reminders in view, top to bottom
    std::vector<std::string> marked_items();  // marked_, in that order
    std::vector<std::string> outermost(const std::vector<std::string>& ids);  // without subtasks whose parent is there
    void step_off(const std::vector<std::string>& going);  // selects the nearest reminder not in `going`
    bool act_on_marked(wint_t key);  // false: not a key that applies to marks
    void step_sidebar(int delta);
    void move_selection(int delta);
    void select_view(const View& v);
    void restore_view();
    void remember_view();
    void check_folder();

    template <class F>
    void undoable(const char* label, F&& f) {
        auto before = store_.snapshot();
        try {
            f();
        } catch (const std::exception& e) {
            message_ = std::format("Error: {}", e.what());
        }
        history_.record(label, before, store_.snapshot());
    }
    // An edit of several reminders: one undo step, each list written once.
    template <class F>
    void batch(const char* label, F&& f) {
        undoable(label, [&] {
            store_.hold_saves();
            try {
                f();
            } catch (...) {
                try {
                    store_.release_saves();
                } catch (...) {
                }
                throw;
            }
            store_.release_saves();
        });
    }
};

std::vector<SidebarEntry> Tui::smart_entries() {
    auto today = rem::local_today();
    std::vector<SidebarEntry> out;
    if (smart_.display == rem::GroupDisplay::Hidden) return out;
    auto names = smart_.shown;
    if (hidden_.show)  // the hidden ones after them
        for (auto name : {"today", "scheduled", "all", "all-reminders", "flagged", "completed"})
            if (std::ranges::find(names, name) == names.end()) names.push_back(name);
    for (auto& name : names) {
        if (name == "today") out.push_back({{View::Today, ""}, "Today", "blue", static_cast<int>(store_.today(today).size())});
        if (name == "scheduled") out.push_back({{View::Scheduled, ""}, "Scheduled", "red", static_cast<int>(store_.scheduled().size())});
        if (name == "all") out.push_back({{View::All, ""}, "All", "gray", static_cast<int>(store_.all().size())});
        if (name == "all-reminders")  // completed too
            out.push_back({{View::AllReminders, ""}, "All Reminders", "gray", static_cast<int>(store_.everything().size())});
        if (name == "flagged") out.push_back({{View::Flagged, ""}, "Flagged", "orange", static_cast<int>(store_.flagged().size())});
        if (name == "completed") out.push_back({{View::Completed, ""}, "Completed", "gray", static_cast<int>(store_.completed().size())});
        out.back().hidden = std::ranges::find(smart_.shown, name) == smart_.shown.end();
    }
    return out;
}

// The groups with something to show, in order.
std::vector<rem::SidebarGroup> Tui::showing_groups() {
    std::vector<rem::SidebarGroup> out;
    for (auto g : order_) {
        if (g.kind == rem::SidebarGroup::SmartLists && smart_entries().empty()) continue;
        if (g.kind == rem::SidebarGroup::Tags && (tags_.hidden() || std::ranges::none_of(store_.tags(), [&](auto& t) {
                                                 return hidden_.show || !hidden_.tag_hidden(t);
                                             })))
            continue;
        out.push_back(g);
    }
    return out;
}

std::vector<std::string> Tui::source_names() {
    std::vector<std::string> out;
    for (auto& s : store_.sources()) out.push_back(s.config.name);
    return out;
}

std::string Tui::group_title(const rem::SidebarGroup& group) {
    if (group.kind != rem::SidebarGroup::Lists || store_.sources().size() <= 1) return rem::group_title(group);
    for (auto& s : store_.sources())
        if (s.config.name == group.source) return rem::group_title(group, rem::source_title(s.config));
    return rem::group_title(group);
}

std::vector<std::string> Tui::list_keys() {
    std::vector<std::string> out;
    for (auto* l : store_.lists()) out.push_back(store_.key_of(*l));
    return out;
}

rem::GroupLayout* Tui::layout_of(const rem::SidebarGroup& group) {
    if (group.kind == rem::SidebarGroup::Lists) {
        auto at = lists_layouts_.find(group.source);
        if (at == lists_layouts_.end()) at = lists_layouts_.emplace(group.source, rem::load_lists_layout(group.source)).first;
        return &at->second;
    }
    if (group.kind == rem::SidebarGroup::Tags) return &tags_;
    return nullptr;
}

bool Tui::folded(const rem::SidebarGroup& group) {
    auto* l = layout_of(group);
    return l ? l->folded() : smart_.folded();
}

void Tui::toggle_fold(const rem::SidebarGroup& group) {
    auto* l = layout_of(group);
    bool& collapsed = l ? l->collapsed : smart_.collapsed;
    collapsed = !collapsed;
    try {
        rem::save_group_collapsed(group, collapsed);
    } catch (const std::exception&) {
        // Folding still works; it just won't be remembered.
    }
}

// Every row in order. The group at the top has no heading unless it can be
// folded; the groups below it have one.
std::vector<SidebarEntry> Tui::sidebar() {
    std::vector<SidebarEntry> out;
    for (auto g : showing_groups()) {
        auto* l = layout_of(g);
        bool foldable = l ? l->foldable() : smart_.foldable();
        if (foldable || !out.empty()) {
            std::string title = group_title(g);
            if (folded(g)) title += " (folded)";
            SidebarEntry heading{{}, title, "", -1};
            heading.kind = foldable ? SidebarEntry::FoldHeading : SidebarEntry::Heading;
            heading.group = g;
            out.push_back(heading);
            if (folded(g)) continue;
        }
        std::vector<SidebarEntry> rows;
        switch (g.kind) {
            case rem::SidebarGroup::SmartLists: rows = smart_entries(); break;
            case rem::SidebarGroup::Lists: {  // one source's lists, in lists-order
                std::vector<std::string> keys;
                for (auto* l : store_.lists(g.source)) keys.push_back(store_.key_of(*l));
                for (auto& key : rem::order_lists(keys)) {
                    auto* list = store_.list(key);
                    if (!list) continue;
                    bool hidden = hidden_.list_hidden(key);
                    if (hidden && !hidden_.show) continue;
                    int open = 0;
                    list->doc.walk([&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
                    rows.push_back({{View::List, key}, list->name, list->color(), open});
                    rows.back().hidden = hidden;
                }
                break;
            }
            case rem::SidebarGroup::Tags:
                for (auto& t : rem::order_tags(store_.tags())) {
                    bool hidden = hidden_.tag_hidden(t);
                    if (hidden && !hidden_.show) continue;
                    rows.push_back({{View::Tag, t}, "#" + t, rem::load_tag_style(t).color, -1});
                    rows.back().hidden = hidden;
                }
                break;
        }
        for (auto& r : rows) {
            r.group = g;
            out.push_back(r);
        }
    }
    return out;
}

std::vector<SidebarEntry> Tui::sidebar_items() {
    std::vector<SidebarEntry> out;
    for (auto& e : sidebar())
        if (e.kind == SidebarEntry::Item) out.push_back(e);
    return out;
}

// Where to land when there's nothing better: Today, unless it's hidden.
View Tui::home_view() {
    auto smart = smart_entries();
    if (std::ranges::any_of(smart, [](auto& e) { return e.view.kind == View::Today; })) return {View::Today, ""};
    auto items = sidebar_items();
    if (!items.empty()) return items.front().view;
    return smart.empty() ? View{View::Today, ""} : smart.front().view;
}
// The reminders a smart, tag or search view shows (empty for a list).
std::vector<rem::Ref> Tui::view_refs() {
    auto today = rem::local_today();
    std::vector<rem::Ref> refs;
    switch (view_.kind) {
        case View::Today: refs = store_.today(today); break;
        case View::Scheduled: refs = store_.scheduled(); break;
        case View::All: refs = store_.all(); break;
        case View::AllReminders: refs = store_.everything(); break;
        case View::Flagged: refs = store_.flagged(); break;
        case View::Completed: refs = store_.completed(); break;
        case View::Tag: refs = store_.tagged(view_.name); break;
        case View::Search: refs = store_.search(view_.name); break;
        case View::List: break;
    }
    return refs;
}

std::pair<std::string, std::string> Tui::view_count() {
    int total = 0, done = 0;
    auto style = rem::CountStyle::OpenOnly;
    if (view_.kind == View::List) {
        if (auto* l = store_.list(view_.name))
            l->doc.walk([&](rem::Reminder& r, rem::Reminder*) {
                ++total;
                done += r.done;
            });
        style = rem::CountStyle::WithComplete;
    } else {
        auto refs = view_refs();
        total = static_cast<int>(refs.size());
        done = static_cast<int>(std::ranges::count_if(refs, [](auto& r) { return r.reminder->done; }));
        style = view_.kind == View::Search                                              ? rem::CountStyle::Results
                : view_.kind == View::Completed                                         ? rem::CountStyle::Completed
                : view_.kind == View::Tag || view_.kind == View::AllReminders ? rem::CountStyle::WithComplete
                                                                                        : rem::CountStyle::OpenOnly;
    }
    return {rem::count_label(style, total, done), rem::count_short(style, total, done)};
}

std::vector<Line> Tui::lines() {
    std::vector<Line> out;
    auto today = rem::local_today();
    auto item = [&](const rem::Ref& ref, int depth, bool show_list) {
        auto& r = *ref.reminder;
        auto md = term::markdown_line(r);
        Line line{Line::Item, r.id, md.before, ref.list->color(), depth};
        line.due = md.due;
        line.after = md.after;
        if (show_list) line.where = "(" + store_.label(*ref.list) + (ref.parent ? " > " + ref.parent->title : "") + ")";
        line.done = r.done;
        line.overdue = term::is_overdue(r, today);
        out.push_back(std::move(line));
        for (std::size_t s = 0; !r.notes.empty();) {
            auto nl = r.notes.find('\n', s);
            out.push_back({Line::Note, "", r.notes.substr(s, nl - s), "", depth + 2});
            if (nl == std::string::npos) break;
            s = nl + 1;
        }
    };

    if (view_.kind == View::List) {
        auto* l = store_.list(view_.name);
        if (!l) return out;
        for (auto& section : l->doc.sections()) {
            if (section.name) {
                if (!out.empty()) out.push_back({Line::Note, "", "", "", 0});  // blank line, as in the file
                out.push_back({Line::Heading, "", "## " + *section.name, l->color(), 0});
            }
            for (auto* r : section.reminders) {
                if (r->done && !show_completed_) continue;
                item({l, r, nullptr}, 0, false);
                if (hide_subtasks_) continue;
                for (auto& s : r->subtasks)
                    if (!s.done || show_completed_) item({l, &s, r}, 2, false);
            }
        }
        return out;
    }

    auto refs = view_refs();
    bool by_date = view_.kind == View::Today || view_.kind == View::Scheduled;
    if (by_date)
        std::ranges::stable_sort(refs, [](auto& a, auto& b) {
            auto key = [](const rem::Ref& r) {
                auto t = r.reminder->due_time.value_or(rem::TimeOfDay{-1, 0});
                return std::pair{*r.reminder->due_date, t.hour * 60 + t.minute};
            };
            return key(a) < key(b);
        });
    std::string group = "\x01";
    for (auto& ref : refs) {
        std::string g, color;
        if (by_date) g = *ref.reminder->due_date < today ? "Overdue" : rem::relative_date(*ref.reminder->due_date, today);
        else if (view_.kind != View::Flagged) g = store_.label(*ref.list), color = ref.list->color();
        if (g != group) {
            if (!g.empty()) {
                if (!out.empty()) out.push_back({Line::Note, "", "", "", 0});
                out.push_back({Line::Heading, "", "## " + g, color, 0});
            }
            group = g;
        }
        item(ref, 0, by_date || view_.kind == View::Flagged || view_.kind == View::Search);
    }
    return out;
}

void Tui::draw_sidebar(int width, int height) {
    auto entries = sidebar();
    side_sel_ = std::clamp(side_sel_, 0, static_cast<int>(entries.size()) - 1);
    attron(A_BOLD);
    put(0, 1, "Reminders", width - 2);
    attroff(A_BOLD);
    int y = 2;  // a blank line under the title
    std::size_t number = 0;  // key numbers follow the lists that are showing
    for (int i = 0; i < static_cast<int>(entries.size()) && y < height - 1; ++i, ++y) {
        auto& e = entries[static_cast<std::size_t>(i)];
        bool selected = i == side_sel_ && !focus_items_;
        if (e.kind != SidebarEntry::Item) {
            if (i > 0) ++y;  // a blank line above each group
            if (y >= height - 1) break;
            if (selected) attron(COLOR_PAIR(kSelected) | A_BOLD);
            else attron(dim());
            mvhline(y, 0, ' ', width - 1);
            put(y, 1, e.title, width - 2);
            attroff(COLOR_PAIR(kSelected) | A_BOLD);
            attroff(dim());
            continue;
        }
        bool current = e.view == view_;
        if (selected) attron(COLOR_PAIR(kSelected) | A_BOLD);
        else if (current) attron(A_BOLD);
        mvhline(y, 0, ' ', width - 1);
        // Lists in their colour, tags too once given one (Tag Info… in the app).
        bool colored = e.view.kind == View::List || (e.view.kind == View::Tag && e.color != "gray");
        if (e.hidden && !selected) attron(dim());
        if (colored) attron(list_color(e.color));
        put(y, 1, rem::with_key_number(e.title, number++, show_key_numbers_), width - 8);
        if (colored) attroff(list_color(e.color));
        if (e.hidden && !selected) attroff(dim());
        if (e.hidden) mvaddstr(y, 0, "-");  // hidden, showing because of show-hidden (H)
        if (e.count >= 0) {
            auto count = std::to_string(e.count);
            attron(dim());
            put(y, width - 2 - static_cast<int>(count.size()), count, static_cast<int>(count.size()));
            attroff(dim());
        }
        attroff(COLOR_PAIR(kSelected) | A_BOLD);
    }
    mvvline_set(0, width - 1, WACS_VLINE, height - 1);
}
void Tui::draw_items(int x, int width, int height) {
    title_y_ = -1;
    auto ls = lines();
    // Marks only on reminders in view (completed ones hidden, gone elsewhere).
    std::erase_if(marked_, [&](const std::string& id) {
        return std::ranges::none_of(ls, [&](const Line& l) { return l.kind == Line::Item && l.id == id; });
    });
    // Title
    std::string title;
    if (view_.kind == View::List) {
        auto* l = store_.list(view_.name);
        title = l ? l->name : view_.name;
    }
    else if (view_.kind == View::Tag) title = "#" + view_.name;
    else if (view_.kind == View::Search) title = std::format("Search: {}", view_.name);
    else {
        static constexpr const char* smart_titles[] = {"Today", "Scheduled", "All", "Flagged", "Completed",
                                                       "All Reminders"};
        title = smart_titles[static_cast<int>(view_.kind)];
    }
    attron(A_BOLD);
    if (view_.kind == View::List)
        if (auto* l = store_.list(view_.name)) attron(list_color(l->color()));
    int room = width - 2;
    int used = put(0, x + 1, "# " + title, room);
    attroff(A_BOLD | A_COLOR);
    // The count, dimmed, against the right edge: "6 Reminders / 3 Complete",
    // or "6/3" if that doesn't fit beside the title, or nothing.
    auto [full, brief] = view_count();
    if (!marked_.empty()) full = brief = std::format("{} marked", marked_.size());
    for (auto& count : {full, brief}) {
        int w = static_cast<int>(count.size());  // ASCII
        if (used + 2 + w > room) continue;
        attron(dim());
        put(0, x + 1 + room - w, count, w);
        attroff(dim());
        break;
    }

    // Keep a valid selection.
    std::vector<int> items;
    for (int i = 0; i < static_cast<int>(ls.size()); ++i)
        if (ls[static_cast<std::size_t>(i)].kind == Line::Item) items.push_back(i);

    auto sel = std::ranges::find_if(items, [&](int i) { return ls[static_cast<std::size_t>(i)].id == item_sel_; });
    if (sel == items.end()) item_sel_ = items.empty() ? "" : ls[static_cast<std::size_t>(items.front())].id;
    int sel_line = -1;
    for (int i : items)
        if (ls[static_cast<std::size_t>(i)].id == item_sel_) sel_line = i;

    int rows = height - 3;
    if (sel_line >= 0) {
        if (sel_line < item_scroll_) item_scroll_ = sel_line;
        if (sel_line >= item_scroll_ + rows) item_scroll_ = sel_line - rows + 1;
    }
    item_scroll_ = std::clamp(item_scroll_, 0, std::max(0, static_cast<int>(ls.size()) - rows));

    if (ls.empty()) {
        attron(dim());
        put(2, x + 2, "Nothing here. Press a to add a reminder.", width - 3);
        attroff(dim());
    }
    for (int row = 0; row < rows && item_scroll_ + row < static_cast<int>(ls.size()); ++row) {
        auto& l = ls[static_cast<std::size_t>(item_scroll_ + row)];
        int y = 2 + row;
        switch (l.kind) {
            case Line::Heading:
                attron(A_BOLD | list_color(l.color));
                put(y, x + 1, l.text, width - 2);
                attroff(A_BOLD | A_COLOR);
                break;
            case Line::Note:
                attron(dim());
                put(y, x + 2 + l.depth, l.text, width - 3 - l.depth);
                attroff(dim());
                break;
            case Line::Item: {
                bool selected = l.id == item_sel_;
                attr_t base = selected ? (focus_items_ ? COLOR_PAIR(kSelected) | A_BOLD : A_BOLD) : A_NORMAL;
                attr_t faint = selected ? base : dim();
                if (selected) {
                    attron(base);
                    mvhline(y, x, ' ', width);
                }
                int col = x + 1 + l.depth, room = width - 2 - l.depth;
                if (selected) {  // the title starts after "- [ ] "
                    title_y_ = y;
                    title_x_ = col + 6;
                    title_width_ = std::max(1, room - 6);
                }
                auto part = [&](const std::string& text, attr_t a) {
                    if (text.empty() || room <= 1) return;
                    attrset(a);
                    int n = put(y, col, text, room);
                    col += n + 1, room -= n + 1;
                };
                part(l.text, l.done ? faint : base);
                part(l.due, l.overdue && !selected && has_colors() ? COLOR_PAIR(kRed) : l.done ? faint : base);
                part(l.after, faint);
                part(l.where, faint);
                if (marked_.contains(l.id)) {  // a * in the margin
                    attrset((selected ? base : A_NORMAL) | A_BOLD | (has_colors() && !selected ? COLOR_PAIR(kMarked) : 0));
                    mvaddstr(y, x, "*");
                }
                attrset(A_NORMAL);
                break;
            }
        }
    }
}

void Tui::draw_status() {
    int h = LINES;
    attron(COLOR_PAIR(kStatus));
    mvhline(h - 1, 0, ' ', COLS);
    auto text = !message_.empty() ? " " + message_
                : !marked_.empty()
                    ? std::format(" {} marked: x done  f flag  t/T/d due  0-3 priority  # tag  m move  del delete  esc unmark",
                                  marked_.size())
                : focus_items_ ? std::string(" x done  v mark  n new  enter title  e edit  d due  f flag  del delete  u undo  ? help  q quit")
                               : std::string(" ↑↓ choose  enter open  tab switch  g go to  n new  N new list  ? help  q quit");
    put(h - 1, 0, text, COLS);
    attroff(COLOR_PAIR(kStatus));
}

void Tui::draw() {
    erase();
    int extra = show_key_numbers_ ? 4 : 0;  // room for "(1) "
    int side = hide_sidebar_ ? 0 : std::min(28 + extra, std::max(18 + extra, COLS / 4));
    if (!hide_sidebar_) draw_sidebar(side, LINES);
    draw_items(side, COLS - side, LINES);
    draw_status();
    refresh();
}

// Edits one line of text in `width` cells at (y, x), with a cursor: ←/→,
// Home/End (Ctrl+A/E), Backspace/Delete, Ctrl+U clears, Ctrl+K cuts to the
// end. Enter or Ctrl+S accepts, Esc cancels (nullopt).
std::optional<std::string> Tui::edit_line(int y, int x, int width, const std::string& initial, attr_t attr) {
    auto text = widen(initial);
    std::size_t cur = text.size();
    curs_set(1);
    std::optional<std::string> result;
    while (true) {
        // Scroll sideways so the cursor stays visible.
        std::size_t start = 0;
        while (start < cur && text_width(text.substr(start, cur - start)) > width - 1) ++start;
        attrset(attr);
        mvhline(y, x, ' ', width);
        std::wstring shown;
        int used = 0;
        for (auto i = start; i < text.size(); ++i) {
            int cw = cell_width(text[i]);
            if (used + cw > width) break;
            shown += text[i];
            used += cw;
        }
        mvaddnwstr(y, x, shown.c_str(), static_cast<int>(shown.size()));
        attrset(A_NORMAL);
        move(y, x + text_width(text.substr(start, cur - start)));
        refresh();

        wint_t ch;
        int kind = get_wch(&ch);
        if (kind == ERR) continue;
        if (kind == KEY_CODE_YES) {
            switch (ch) {
                case KEY_LEFT: if (cur > 0) --cur; break;
                case KEY_RIGHT: if (cur < text.size()) ++cur; break;
                case KEY_HOME: cur = 0; break;
                case KEY_END: cur = text.size(); break;
                case KEY_BACKSPACE: if (cur > 0) text.erase(--cur, 1); break;
                case KEY_DC: if (cur < text.size()) text.erase(cur, 1); break;
                case KEY_ENTER: result = narrow(text); break;
            }
            if (result) break;
            continue;
        }
        if (ch == 27) break;  // Esc: cancel
        if (ch == '\n' || ch == '\r' || ch == 19) {  // Enter, or Ctrl+S as in the GNOME app
            result = narrow(text);
            break;
        }
        switch (ch) {
            case 127:
            case 8: if (cur > 0) text.erase(--cur, 1); break;
            case 1: cur = 0; break;            // Ctrl+A
            case 5: cur = text.size(); break;  // Ctrl+E
            case 21: text.clear(), cur = 0; break;  // Ctrl+U
            case 11: text.erase(cur); break;   // Ctrl+K
            default:
                if (ch >= 32) text.insert(cur++, 1, static_cast<wchar_t>(ch));
        }
    }
    curs_set(0);
    return result;
}

std::optional<std::string> Tui::prompt(const std::string& label, const std::string& initial) {
    attron(COLOR_PAIR(kStatus));
    mvhline(LINES - 1, 0, ' ', COLS);
    int used = put(LINES - 1, 0, " " + label + " ", COLS);
    attroff(COLOR_PAIR(kStatus));
    return edit_line(LINES - 1, used, std::max(1, COLS - used - 1), initial, COLOR_PAIR(kStatus));
}

// Enter / F2: edit the selected reminder's title where it's shown. As in the
// GNOME app, fields typed into it (#tag, 📅 date…) are applied, and clearing
// it deletes the reminder.
void Tui::edit_title_in_place(const std::string& id) {
    draw();  // makes sure title_y_ etc. describe the selected row
    auto ref = store_.find(id);
    if (!ref || title_y_ < 0) return;
    auto text = edit_line(title_y_, title_x_, title_width_, ref->reminder->title,
                          focus_items_ ? COLOR_PAIR(kSelected) | A_BOLD : A_BOLD);
    if (!text || *text == ref->reminder->title) return;

    auto trimmed = *text;
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(0, 1);
    if (trimmed.empty()) {
        move_selection(1);
        if (item_sel_ == id) move_selection(-1);
        undoable("Delete", [&] { store_.remove(id); });
        message_ = "Deleted (u to undo)";
        return;
    }
    undoable("Edit Title", [&] {
        auto& r = *ref->reminder;
        auto f = rem::parse_fields(trimmed);
        r.title = f.title;
        for (auto& t : f.tags)
            if (std::ranges::find(r.tags, t) == r.tags.end()) r.tags.push_back(t);
        if (f.priority != rem::Priority::None) r.priority = f.priority;
        if (f.flagged) r.flagged = true;
        if (f.repeat) r.repeat = f.repeat;
        if (f.due_date) r.due_date = f.due_date, r.due_time = f.due_time;
        if (f.url) r.url = f.url;
        store_.touch(id);
    });
}

bool Tui::confirm(const std::string& question) {
    auto answer = prompt(question + " [y/N]");
    return answer && (term::lower(*answer) == "y" || term::lower(*answer) == "yes");
}

void Tui::show_help() {
    static const char* text[] = {
        "Reminders: keys",
        "",
        "Anywhere",
        "  ↑↓ / j k     move           tab         switch sidebar / reminders",
        "  1-9, 0       sidebar entry  g / Ctrl+K  go to a list by name",
        "  enter        on a collapsible group's heading: fold / unfold it",
        "  J / K        in the sidebar: move the entry down / up (Alt+Shift+↑↓: its group)",
        "  S            edit settings.ini in your $EDITOR",
        "  h            hide the selected list, smart list or tag (shows a hidden one)",
        "  H            show / stop showing hidden lists, smart lists and tags",
        "  /            search         c           show/hide completed",
        "  N            new list       u / r       undo / redo",
        "  ?            this help      q           quit",
        "",
        "On a reminder",
        "  x / space    done / not done  n         new reminder (inline fields work)",
        "  enter / F2   edit the title in place (esc cancels)",
        "  e / i        edit every field in your $EDITOR",
        "  d            due date (today, tomorrow, fri, +3d, 2026-10-31, none)",
        "  t / T        due today / tomorrow",
        "  f            flag           0-3         priority none-high",
        "  #            add tag (-tag removes)     m  move to list",
        "  J / K        move down/up   ] / [       indent / outdent",
        "  Delete       delete (asks first)",
        "",
        "Several reminders",
        "  v            mark / unmark (and go to the next)   *  mark all   esc  unmark all",
        "  While some are marked, x / space, f, t / T, d, 0-3, #, m and Delete",
        "  act on all of them, as one undo step.",
        "",
        "Same as the GNOME app",
        "  Ctrl+N new   Ctrl+T today   Ctrl+K go to   Ctrl+F search   Ctrl+H completed",
        "  Ctrl+E subtasks   Ctrl+B sidebar   F2 title",
        "  Alt+0-3 priority   Alt+↑↓ move   Ctrl+PgUp/PgDn previous/next   Ctrl+Q quit",
        "",
        "Press any key to close.",
    };
    int h = static_cast<int>(std::size(text)) + 2, w = 72;
    int y = std::max(0, (LINES - h) / 2), x = std::max(0, (COLS - w) / 2);
    auto* win = newwin(std::min(h, LINES), std::min(w, COLS), y, x);
    box_set(win, WACS_VLINE, WACS_HLINE);
    for (int i = 0; i < static_cast<int>(std::size(text)) && i + 1 < LINES - 1; ++i) mvwaddstr(win, i + 1, 2, text[i]);
    wrefresh(win);
    wint_t ch;
    while (get_wch(&ch) == ERR) {
    }
    delwin(win);
}

// Moves the selected sidebar entry's group up (delta < 0) or down, and saves
// the order. The selection stays on the same entry.
void Tui::move_group(int delta) {
    auto entries = sidebar();
    if (side_sel_ < 0 || side_sel_ >= static_cast<int>(entries.size())) return;
    auto selected = entries[static_cast<std::size_t>(side_sel_)];
    if (!rem::move_sidebar_group(order_, selected.group, delta, showing_groups())) return;
    try {
        rem::save_sidebar_order(order_);
    } catch (const std::exception& e) {
        message_ = std::format("Couldn't save the order: {}", e.what());
    }
    entries = sidebar();
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        auto& e = entries[static_cast<std::size_t>(i)];
        bool same = selected.kind == SidebarEntry::Item ? e.kind == SidebarEntry::Item && e.view == selected.view
                                                        : e.kind != SidebarEntry::Item && e.group == selected.group;
        if (same) side_sel_ = i;
    }
}

// Moves the selected smart list, list or tag up (delta < 0) or down within
// its group, skipping entries not showing, and saves the order (smart-lists,
// lists-order, tags-order). The selection stays on it.
void Tui::move_entry(int delta) {
    auto entries = sidebar();
    if (side_sel_ < 0 || side_sel_ >= static_cast<int>(entries.size())) return;
    auto selected = entries[static_cast<std::size_t>(side_sel_)];
    if (selected.kind != SidebarEntry::Item) return;
    std::vector<std::string> showing;  // the names of the group's rows, as drawn
    auto name_of = [](const SidebarEntry& e) {
        return e.view.kind <= View::AllReminders ? std::string(kViewSettings[static_cast<int>(e.view.kind)].second)
                                                 : e.view.name;
    };
    for (auto& e : entries)
        if (e.kind == SidebarEntry::Item && e.group == selected.group) showing.push_back(name_of(e));
    auto name = name_of(selected);
    try {
        switch (selected.group.kind) {
            case rem::SidebarGroup::SmartLists: {
                auto order = smart_.shown;  // a hidden smart list has no place to move
                if (!rem::move_in_order(order, name, delta, order)) return;
                rem::save_smart_lists(order);
                break;
            }
            case rem::SidebarGroup::Lists: {  // every source's lists keep their places
                auto order = rem::order_lists(list_keys());
                if (!rem::move_in_order(order, name, delta, showing)) return;
                rem::save_names_setting("lists-order", order);
                break;
            }
            case rem::SidebarGroup::Tags: {
                auto order = rem::order_tags(store_.tags());
                if (!rem::move_in_order(order, name, delta, showing)) return;
                rem::save_names_setting("tags-order", order);
                break;
            }
        }
    } catch (const std::exception& e) {
        message_ = std::format("Couldn't save the order: {}", e.what());
        return;
    }
    load_layout();
    entries = sidebar();
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        if (entries[static_cast<std::size_t>(i)].kind == SidebarEntry::Item &&
            entries[static_cast<std::size_t>(i)].view == selected.view)
            side_sel_ = i;
}

void Tui::load_layout() {
    show_key_numbers_ = key_numbers_override_.value_or(rem::load_bool_setting("show-key-numbers"));
    order_ = rem::load_sidebar_order(source_names());
    smart_ = rem::load_smart_lists_layout();
    lists_layouts_.clear();
    tags_ = rem::load_tags_layout();
    hidden_ = rem::load_hidden();
}

// Opens settings.ini in $EDITOR, then applies what changed.
void Tui::edit_settings() {
    auto file = rem::settings_file();
    try {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            fs::create_directories(file.parent_path());
            std::ofstream(file) << "[general]\n";  // settings are read from this section
        }
    } catch (const std::exception& e) {
        message_ = std::format("Error: {}", e.what());
        return;
    }
    def_prog_mode();
    endwin();
    bool ok = editfile::run_editor_on(file);
    reset_prog_mode();
    refresh();
    load_layout();
    // The view may have just been hidden.
    bool smart = view_.kind != View::List && view_.kind != View::Tag && view_.kind != View::Search;
    if ((smart && std::ranges::none_of(smart_entries(), [&](auto& e) { return e.view == view_; })) ||
        (view_.kind == View::Tag && (tags_.hidden() || (!hidden_.show && hidden_.tag_hidden(view_.name)))) ||
        (view_.kind == View::List && !hidden_.show && hidden_.list_hidden(view_.name)))
        select_view(home_view());
    else {
        auto keep = std::pair{item_sel_, item_scroll_};
        select_view(view_);  // finds it in the sidebar again
        std::tie(item_sel_, item_scroll_) = keep;
    }
    message_ = ok ? "Settings reloaded" : "The editor failed; settings reloaded";
}

// Edits the reminder in $EDITOR as YAML-style fields (see editfile.hpp).
void Tui::edit_in_editor(const std::string& id) {
    def_prog_mode();
    endwin();
    auto outcome = editfile::Outcome::Unchanged;
    try {
        outcome = editfile::edit(store_, id, [&](const std::function<void()>& apply) {
            auto before = store_.snapshot();
            apply();  // errors go back to the editor, so don't swallow them here
            history_.record("Edit Reminder", before, store_.snapshot());
        });
    } catch (const std::exception& e) {
        message_ = std::format("Error: {}", e.what());
    }
    reset_prog_mode();
    refresh();
    if (message_.empty())
        message_ = outcome == editfile::Outcome::Saved      ? "Saved (u to undo)"
                   : outcome == editfile::Outcome::Reverted ? "Reverted; the reminder is as it was"
                                                            : "No changes";
}


// h: hides the selected sidebar entry (the open view, from the reminders
// pane), or shows it again if it's hidden (visible with show-hidden). Saved
// in settings.ini, as the app's Hide / Show does.
void Tui::toggle_hidden() {
    auto target = view_;
    std::string title;  // as the sidebar shows it
    for (auto& e : sidebar())
        if (e.kind == SidebarEntry::Item && e.view == target) title = e.title;
    if (!focus_items_) {
        auto entries = sidebar();
        if (side_sel_ < 0 || side_sel_ >= static_cast<int>(entries.size())) return;
        auto& e = entries[static_cast<std::size_t>(side_sel_)];
        if (e.kind != SidebarEntry::Item) return;
        target = e.view;
        title = e.title;
    }
    bool smart = target.kind <= View::AllReminders;
    std::string name = smart ? kViewSettings[static_cast<int>(target.kind)].second : target.name;
    bool hidden = smart                         ? std::ranges::find(smart_.shown, name) == smart_.shown.end()
                  : target.kind == View::List ? hidden_.list_hidden(name)
                  : target.kind == View::Tag  ? hidden_.tag_hidden(name)
                                              : false;
    if (target.kind == View::Search) return;
    try {
        if (smart) rem::set_smart_list_hidden(name, !hidden);
        else if (target.kind == View::List) rem::set_list_hidden(name, !hidden);
        else rem::set_tag_hidden(name, !hidden);
    } catch (const std::exception& e) {
        message_ = std::format("Couldn't save the setting: {}", e.what());
        return;
    }
    load_layout();
    if (title.empty()) title = target.kind == View::Tag ? "#" + name : name;
    if (!hidden && !hidden_.show) {
        message_ = std::format("Hid “{}” (H shows hidden entries; h on it shows it again)", title);
        if (view_ == target) select_view(home_view());
        else select_view(view_);  // the selection follows the rows that remain
    } else {
        message_ = std::format("{} “{}”", hidden ? "Showing" : "Hid", title);
    }
}

// H: shows the hidden smart lists, lists and tags (dimmed), or stops showing
// them, as the app's Show Hidden Lists does (show-hidden in settings.ini).
void Tui::toggle_show_hidden() {
    try {
        rem::save_show_hidden(!hidden_.show);
    } catch (const std::exception& e) {
        message_ = std::format("Couldn't save the setting: {}", e.what());
        return;
    }
    load_layout();
    message_ = hidden_.show ? "Showing hidden lists" : "Not showing hidden lists";
    bool smart = view_.kind <= View::AllReminders;
    bool gone = (smart && std::ranges::none_of(smart_entries(), [&](auto& e) { return e.view == view_; })) ||
                (view_.kind == View::List && hidden_.list_hidden(view_.name)) ||
                (view_.kind == View::Tag && hidden_.tag_hidden(view_.name));
    if (gone && !hidden_.show) {
        select_view(home_view());
    } else {
        auto keep = std::pair{item_sel_, item_scroll_};
        select_view(view_);  // finds it in the sidebar again
        std::tie(item_sel_, item_scroll_) = keep;
    }
}

// Opens on the list that last had focus here, in the GNOME app or via the CLI.
void Tui::restore_view() {
    if (!remember_) {
        select_view(home_view());
        return;
    }
    auto saved = term::parse_view_setting(rem::load_setting("view"));
    for (auto [kind, name] : kViewSettings)
        if (saved.kind == name) view_ = {static_cast<View::Kind>(kind), ""};
    if (saved.kind == "list")  // "source/name" (or a name only one source has)
        if (auto* l = store_.list(saved.name)) view_ = {View::List, store_.key_of(*l)};
    auto tags = store_.tags();
    if (saved.kind == "tag" && !tags_.hidden() && std::ranges::find(tags, saved.name) != tags.end())
        view_ = {View::Tag, saved.name};
    bool smart = view_.kind != View::List && view_.kind != View::Tag && view_.kind != View::Search;
    if (smart && std::ranges::none_of(smart_entries(), [&](auto& e) { return e.view == view_; })) view_ = home_view();
    if (!hidden_.show && ((view_.kind == View::List && hidden_.list_hidden(view_.name)) ||
                          (view_.kind == View::Tag && hidden_.tag_hidden(view_.name))))
        view_ = home_view();  // hidden in the sidebar
    select_view(view_);
}

void Tui::remember_view() {
    if (!remember_ || view_.kind == View::Search) return;
    std::string value = view_.kind == View::List ? "list:" + view_.name
                        : view_.kind == View::Tag ? "tag:" + view_.name
                                                  : kViewSettings[static_cast<int>(view_.kind)].second;
    try {
        if (rem::load_setting("view") != value) rem::save_setting("view", value);
    } catch (const std::exception&) {
        // Not being able to save the last view isn't worth interrupting for.
    }
}

void Tui::select_view(const View& v) {
    if (!(v == view_)) marked_.clear();
    view_ = v;
    remember_view();
    item_scroll_ = 0;
    item_sel_.clear();
    auto entries = sidebar();
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        if (entries[static_cast<std::size_t>(i)].kind == SidebarEntry::Item && entries[static_cast<std::size_t>(i)].view == v)
            side_sel_ = i;
}
void Tui::move_selection(int delta) {
    if (!focus_items_) {
        // Steps over plain headings; stops on a collapsible group's heading
        // (Enter folds it) and on lists, which open as you go.
        auto entries = sidebar();
        int n = static_cast<int>(entries.size()), step = delta < 0 ? -1 : 1;
        for (int moved = 0, i = side_sel_; moved < std::abs(delta);) {
            i += step;
            if (i < 0 || i >= n) break;
            auto k = entries[static_cast<std::size_t>(i)].kind;
            if (k == SidebarEntry::Heading) continue;
            side_sel_ = i;
            ++moved;
        }
        auto& e = entries[static_cast<std::size_t>(std::clamp(side_sel_, 0, n - 1))];
        if (e.kind == SidebarEntry::Item && !(e.view == view_)) {
            view_ = e.view;
            remember_view();
            item_scroll_ = 0;
            item_sel_.clear();
        }
        return;
    }
    auto ls = lines();
    std::vector<std::string> ids;
    for (auto& l : ls)
        if (l.kind == Line::Item) ids.push_back(l.id);
    if (ids.empty()) return;
    auto at = std::ranges::find(ids, item_sel_);
    long i = at == ids.end() ? 0 : at - ids.begin();
    i = std::clamp<long>(i + delta, 0, static_cast<long>(ids.size()) - 1);
    item_sel_ = ids[static_cast<std::size_t>(i)];
}

// Shows what went wrong in the last CalDAV and WebDAV syncs, if anything.
void Tui::check_sync() {
#ifdef REMINDERS_NETWORK
    if (!sync_) return;
    auto status = sync_->take_status();
    if (!status.errors.empty()) message_ = "Sync: " + status.errors.back();
#endif
}

// Reloads when files in the folder change (Syncthing, the GUI, an editor).
void Tui::check_folder() {
    std::map<std::string, std::pair<fs::file_time_type, std::uintmax_t>> now;
    std::error_code ec;
    for (auto& source : store_.sources())
        for (auto& e : fs::directory_iterator(source.config.folder, ec)) {
            auto name = e.path().filename().string();
            if (!name.ends_with(".md") || name.starts_with('.')) continue;
            now[e.path().string()] = {e.last_write_time(ec), e.file_size(ec)};
        }
    if (now == seen_) return;
    bool first = seen_.empty();
    seen_ = std::move(now);
    if (first) return;
    try {
        store_.load_all();  // only lists whose files changed are re-read
    } catch (const std::exception& e) {
        message_ = std::format("Error reloading: {}", e.what());
    }
}

void Tui::step_sidebar(int delta) {
    auto entries = sidebar_items();
    if (entries.empty()) return;
    auto at = std::ranges::find_if(entries, [&](auto& e) { return e.view == view_; });
    long n = static_cast<long>(entries.size());
    long i = at == entries.end() ? 0 : ((at - entries.begin()) + delta + n) % n;
    select_view(entries[static_cast<std::size_t>(i)].view);
}
bool Tui::handle_key(wint_t key, bool fn, bool alt) {
    message_.clear();
    auto today = rem::local_today();
    auto id = item_sel_;
    auto ref = id.empty() ? std::nullopt : store_.find(id);
    bool in_list = view_.kind == View::List;

    // The GNOME app's shortcuts, as far as a terminal can send them; they map
    // onto the TUI's own keys below. (Ctrl+Shift+letter arrives as
    // Ctrl+letter, Ctrl+I as Tab and Ctrl+[ as Esc, so those keep their
    // letter keys.)
    if (alt) {
        if (!fn && key >= '0' && key <= '3') {  // Alt+0…3: priority
            if (focus_items_ && !marked_.empty()) {
                act_on_marked(key);
            } else if (ref) {
                auto p = static_cast<rem::Priority>(key - '0');
                undoable("Priority", [&] {
                    ref->reminder->priority = p;
                    store_.touch(id);
                });
            }
            return true;
        }
        if (fn && key == KEY_UP) key = 'K', fn = false;         // Alt+↑ (sent as Esc, ↑)
        else if (fn && key == KEY_DOWN) key = 'J', fn = false;  // Alt+↓
        else if (fn && key == KEY_SR) key = kGroupUp, fn = false;    // Alt+Shift+↑ (Esc, Shift+↑)
        else if (fn && key == KEY_SF) key = kGroupDown, fn = false;  // Alt+Shift+↓
        else return true;
        // In the sidebar they move the selected group; J / K below.
    } else if (fn) {
        if (alt_up_ && static_cast<int>(key) == alt_up_) key = 'K', fn = false;
        else if (alt_down_ && static_cast<int>(key) == alt_down_) key = 'J', fn = false;
        else if (alt_shift_up_ && static_cast<int>(key) == alt_shift_up_) key = kGroupUp, fn = false;
        else if (alt_shift_down_ && static_cast<int>(key) == alt_shift_down_) key = kGroupDown, fn = false;
        else if (ctrl_page_down_ && static_cast<int>(key) == ctrl_page_down_) return step_sidebar(1), true;
        else if (ctrl_page_up_ && static_cast<int>(key) == ctrl_page_up_) return step_sidebar(-1), true;
        else if (key == KEY_F(1)) key = '?', fn = false;
        else if (key == KEY_F(2)) key = '\n', fn = false;
    } else {
        switch (key) {
            case 14: key = 'n'; break;            // Ctrl+N: new reminder
            case 20: key = 't'; break;            // Ctrl+T: due today
            case 11: key = 'g'; break;            // Ctrl+K: go to
            case 8: key = 'c'; break;             // Ctrl+H: show/hide completed
            case 6: key = '/'; break;             // Ctrl+F: search
            case 17: case 23: key = 'q'; break;   // Ctrl+Q, Ctrl+W: quit
            case 5:                               // Ctrl+E: show/hide subtasks
                hide_subtasks_ = !hide_subtasks_;
                message_ = hide_subtasks_ ? "Subtasks hidden" : "Subtasks shown";
                return true;
            case 2:                               // Ctrl+B: show/hide sidebar
                hide_sidebar_ = !hide_sidebar_;
                if (hide_sidebar_) focus_items_ = true;
                try {
                    rem::save_setting("show-sidebar", hide_sidebar_ ? "false" : "true");
                } catch (const std::exception&) {
                    // It just won't be remembered.
                }
                return true;
        }
    }

    // Tab switches between the sidebar and the reminders. (A terminal sends
    // Ctrl+I as Tab, so Ctrl+I can't also edit, as it does in the GNOME app.)
    if (!fn && key == '\t') {
        focus_items_ = !focus_items_;
        return true;
    }

    // Esc on its own unmarks everything.
    if (!fn && key == 27) {
        if (!marked_.empty()) {
            marked_.clear();
            message_ = "Unmarked";
        }
        return true;
    }

    // Keys that work anywhere.
    if (!fn) switch (key) {
            case 'q': return false;
            case '?': show_help(); return true;
            case 'h': toggle_hidden(); return true;
            case 'j': move_selection(1); return true;
            case 'k': move_selection(-1); return true;
            case 'c': show_completed_ = !show_completed_; return true;
            case 'S': edit_settings(); return true;
            case 'H': toggle_show_hidden(); return true;
            case 'u': {
                auto r = history_.undo(store_);
                message_ = r.applied ? "Undone" : "Nothing to undo";
                for (auto& s : r.skipped) message_ = std::format("“{}” changed elsewhere; left as it is", s);
                return true;
            }
            case 'r': {  // redo
                auto r = history_.redo(store_);
                message_ = r.applied ? "Redone" : "Nothing to redo";
                return true;
            }
            case '/':
                if (auto q = prompt("Search:"); q && !q->empty()) {
                    marked_.clear();
                    view_ = {View::Search, *q};
                    item_sel_.clear();
                    focus_items_ = true;
                }
                return true;
            case 'g':
                if (auto q = prompt("Go to:"); q && !q->empty()) {
                    // Best match: a name starting with it, else containing it.
                    std::optional<View> best;
                    int best_score = 3;
                    auto findable = sidebar_items();  // plus folded groups' entries
                    if (smart_.folded())
                        for (auto& e : smart_entries()) findable.push_back(e);
                    for (auto& s : store_.sources())
                        if (folded(rem::SidebarGroup::lists(s.config.name)))
                            for (auto* l : store_.lists(s.config.name))
                                findable.push_back({{View::List, store_.key_of(*l)}, l->name, l->color(), -1});
                    if (tags_.folded() && !tags_.hidden())
                        for (auto& t : store_.tags()) findable.push_back({{View::Tag, t}, "#" + t, "gray", -1});
                    for (auto& e : findable) {
                        auto t = term::lower(e.title), s = term::lower(*q);
                        if (t.starts_with('#') && !s.starts_with('#')) t.erase(0, 1);
                        int score = t.starts_with(s) ? 0 : t.find(s) != std::string::npos ? 1 : 3;
                        if (score < best_score) best_score = score, best = e.view;
                    }
                    if (best) select_view(*best);
                    else message_ = std::format("Nothing called “{}”", *q);
                }
                return true;
            case 'N':
                // Into the source whose group is selected, else the default one.
                if (auto name = prompt("New list name:"); name && !name->empty()) {
                    auto source = store_.default_source();
                    auto entries = sidebar();
                    if (!focus_items_ && side_sel_ >= 0 && side_sel_ < static_cast<int>(entries.size()) &&
                        entries[static_cast<std::size_t>(side_sel_)].group.kind == rem::SidebarGroup::Lists)
                        source = entries[static_cast<std::size_t>(side_sel_)].group.source;
                    else if (auto* l = view_.kind == View::List ? store_.list(view_.name) : nullptr)
                        source = store_.source_of(*l)->config.name;
                    if (store_.list(rem::Library::key(source, *name))) {
                        message_ = "A list with that name already exists";
                    } else {
                        undoable("New List", [&] { store_.create_list(source, *name, "blue", "list"); });
                        select_view({View::List, rem::Library::key(source, *name)});
                    }
                }
                return true;
        }
    if (!fn && key >= '0' && key <= '9' && !focus_items_) {  // 1…9, then 0 for the 10th
        auto entries = sidebar_items();
        auto n = static_cast<std::size_t>(key == '0' ? 9 : key - '1');
        if (n < entries.size()) select_view(entries[n].view);
        return true;
    }
    if (fn) switch (key) {
            case KEY_UP: move_selection(-1); return true;
            case KEY_DOWN: move_selection(1); return true;
            case KEY_PPAGE: move_selection(-(LINES - 4)); return true;
            case KEY_NPAGE: move_selection(LINES - 4); return true;
            case KEY_RESIZE: return true;
            case KEY_RIGHT:
                focus_items_ = true;
                return true;
            case KEY_LEFT:
                focus_items_ = false;
                return true;
        }

    if (!focus_items_) {
        auto entries = sidebar();
        bool on_row = side_sel_ >= 0 && side_sel_ < static_cast<int>(entries.size());
        auto heading = on_row ? entries[static_cast<std::size_t>(side_sel_)].kind : SidebarEntry::Item;
        bool activate = (!fn && (key == '\n' || key == '\r' || key == ' ')) || (fn && key == KEY_ENTER);
        if (activate && heading == SidebarEntry::FoldHeading) {
            toggle_fold(entries[static_cast<std::size_t>(side_sel_)].group);
            return true;
        }
        // Alt+↑/↓ (J / K) move the selected entry within its group (on a
        // heading, the group); Alt+Shift+↑/↓ move its group. As in the app.
        if (!fn && (key == 'J' || key == 'K')) {
            if (heading == SidebarEntry::Item) move_entry(key == 'K' ? -1 : 1);
            else move_group(key == 'K' ? -1 : 1);
            return true;
        }
        if (!fn && (key == kGroupUp || key == kGroupDown)) {
            move_group(key == kGroupUp ? -1 : 1);
            return true;
        }
        if ((!fn && (key == '\n' || key == '\r' || key == 'l')) || (fn && key == KEY_ENTER)) focus_items_ = true;
        else if (!fn && key == 'n') {
            focus_items_ = true;
            return handle_key('n', false);
        }
        return true;
    }

    // Adding works without a selection.
    if (!fn && key == 'n') {
        auto text = prompt("New reminder:");
        if (!text || text->empty()) return true;
        rem::ListFile* l = in_list ? store_.list(view_.name) : nullptr;
        if (!l && ref) l = ref->list;
        if (!l && !store_.lists().empty()) l = store_.lists().front();
        if (!l) {
            message_ = "Create a list first (N)";
            return true;
        }
        rem::Reminder r;
        r.fields() = rem::parse_fields(*text);
        r.created = today;
        if (view_.kind == View::Today && !r.due_date) r.due_date = today;
        if (view_.kind == View::Flagged) r.flagged = true;
        undoable("Add Reminder", [&] {
            std::optional<std::string> section = ref && in_list ? ref->list->doc.section_of(ref->parent ? *ref->parent : *ref->reminder) : std::nullopt;
            item_sel_ = store_.add(*l, std::move(r), nullptr, section).id;
        });
        return true;
    }
    // Marking, and the keys that then act on every marked reminder.
    if (!fn && key == 'v' && ref) {
        if (!marked_.erase(id)) marked_.insert(id);
        move_selection(1);
        return true;
    }
    if (!fn && key == '*') {
        auto all = shown_items();
        marked_ = {all.begin(), all.end()};
        if (!all.empty()) message_ = std::format("{} marked", all.size());
        return true;
    }
    if (!marked_.empty()) {
        auto k = fn && key == KEY_DC ? kDelete : key;
        if ((!fn || k == kDelete) && act_on_marked(k)) return true;
    }

    if (!ref) return true;
    auto& r = *ref->reminder;

    auto save = [&](const char* label, auto&& change) {
        undoable(label, [&] {
            change();
            store_.touch(id);
        });
    };
    if (fn && key == KEY_DC) key = kDelete, fn = false;
    if (fn && key == KEY_ENTER) key = '\n', fn = false;
    if (fn) return true;

    switch (key) {
        case 'x':
        case ' ': {
            bool hides = !r.done && !show_completed_ && view_.kind != View::Completed &&
                         view_.kind != View::AllReminders;
            if (hides) {  // it's about to disappear: keep the place by selecting its neighbour
                move_selection(1);
                if (item_sel_ == id) move_selection(-1);
            }
            undoable("Complete", [&] { store_.set_done(id, !r.done, today); });
            break;
        }
        case '\n':
        case '\r': edit_title_in_place(id); break;
        case 'e':
        case 'i': edit_in_editor(id); break;
        case 'd':
            if (auto d = prompt("Due (today, tomorrow, fri, +3d, 2026-10-31, none):",
                                r.due_date ? rem::format_date(*r.due_date) : "")) {
                if (term::lower(*d) == "none" || d->empty()) {
                    save("Clear Due Date", [&] { r.due_date.reset(), r.due_time.reset(); });
                } else {
                    auto words = *d;
                    std::optional<rem::TimeOfDay> time;
                    if (auto sp = words.rfind(' '); sp != std::string::npos)
                        if ((time = rem::parse_time(words.substr(sp + 1)))) words.resize(sp);
                    if (auto date = rem::parse_human_date(words, today))
                        save("Set Due Date", [&] {
                            r.due_date = date;
                            if (time) r.due_time = time;
                        });
                    else message_ = std::format("Can't read “{}”", *d);
                }
            }
            break;
        case 't': save("Due Today", [&] { r.due_date = today; }); break;
        case 'T':
            save("Due Tomorrow", [&] { r.due_date = rem::Date{std::chrono::sys_days{today} + std::chrono::days{1}}; });
            break;
        case 'f': save("Flag", [&] { r.flagged = !r.flagged; }); break;
        case '0':
        case '1':
        case '2':
        case '3': save("Priority", [&] { r.priority = static_cast<rem::Priority>(key - '0'); }); break;
        case '#':
            if (auto t = prompt("Tag (-tag removes):"); t && !t->empty()) {
                auto tag = *t;
                bool remove = tag.starts_with('-');
                if (remove) tag.erase(0, 1);
                if (tag.starts_with('#')) tag.erase(0, 1);
                save("Tag", [&] {
                    if (remove) std::erase(r.tags, tag);
                    else if (std::ranges::find(r.tags, tag) == r.tags.end()) r.tags.push_back(tag);
                });
            }
            break;
        case 'm':
            if (auto name = prompt("Move to list:"); name && !name->empty()) {
                rem::ListFile* dest = nullptr;
                for (auto* l : store_.lists())
                    if (term::lower(store_.label(*l)).starts_with(term::lower(*name)) ||
                        term::lower(store_.key_of(*l)).starts_with(term::lower(*name))) {
                        dest = l;
                        break;
                    }
                if (!dest) message_ = std::format("No list called “{}”", *name);
                else if (dest != ref->list) undoable("Move", [&] { store_.move_to_list(id, *dest); });
            }
            break;
        case 'J':
        case 'K':
            if (!in_list) {
                message_ = "Reorder in a list view";
                break;
            }
            undoable("Move", [&] {
                auto visible = [this](const rem::Reminder& x) { return show_completed_ || !x.done; };
                if (ref->list->doc.move_step(id, key == 'K', visible)) store_.save(*ref->list);
            });
            break;
        case ']':
        case '[':
            if (!in_list) {
                message_ = "Indent in a list view";
                break;
            }
            undoable(key == ']' ? "Indent" : "Outdent", [&] {
                auto visible = [this](const rem::Reminder& x) { return show_completed_ || !x.done; };
                bool ok = key == ']' ? ref->list->doc.indent(id, visible) : ref->list->doc.outdent(id);
                if (ok) store_.save(*ref->list);
                else message_ = key == ']' ? "Can't indent this one" : "Not a subtask";
            });
            break;
        case kDelete:
            if (confirm(std::format("Delete “{}”?", r.title))) {
                move_selection(1);
                if (item_sel_ == id) move_selection(-1);
                undoable("Delete", [&] { store_.remove(id); });
                message_ = "Deleted (u to undo)";
            }
            break;
    }
    return true;
}

std::vector<std::string> Tui::shown_items() {
    std::vector<std::string> out;
    for (auto& l : lines())
        if (l.kind == Line::Item) out.push_back(l.id);
    return out;
}

std::vector<std::string> Tui::marked_items() {
    auto out = shown_items();
    std::erase_if(out, [this](auto& id) { return !marked_.contains(id); });
    return out;
}

// For moving and deleting: a subtask goes along with its parent.
std::vector<std::string> Tui::outermost(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    for (auto& id : ids) {
        auto ref = store_.find(id);
        if (ref && ref->parent && std::ranges::find(ids, ref->parent->id) != ids.end()) continue;
        out.push_back(id);
    }
    return out;
}

// Before reminders leave the view: keeps the place by selecting the nearest
// one staying, below the selection if there is one, else above.
void Tui::step_off(const std::vector<std::string>& going) {
    if (std::ranges::find(going, item_sel_) == going.end()) return;
    auto all = shown_items();
    auto at = std::ranges::find(all, item_sel_);
    if (at == all.end()) return;
    auto stays = [&](const std::string& id) { return std::ranges::find(going, id) == going.end(); };
    if (auto next = std::find_if(at, all.end(), stays); next != all.end()) item_sel_ = *next;
    else if (auto prev = std::find_if(std::make_reverse_iterator(at), all.rend(), stays); prev != all.rend())
        item_sel_ = *prev;
}

// The editing keys while reminders are marked, on all of them, as one undo
// step. Completing and flagging set them all alike: done (flagged), or not
// if they all were already.
bool Tui::act_on_marked(wint_t key) {
    auto ids = marked_items();
    if (ids.empty()) return false;
    auto today = rem::local_today();
    auto n = ids.size();
    auto each = [&](const char* label, auto&& change) {
        batch(label, [&] {
            for (auto& id : ids)
                if (auto ref = store_.find(id)) {
                    change(*ref->reminder);
                    store_.touch(id);
                }
        });
    };
    auto all = [&](auto&& test) {
        return std::ranges::all_of(ids, [&](auto& id) {
            auto ref = store_.find(id);
            return !ref || test(*ref->reminder);
        });
    };
    switch (key) {
        case 'x':
        case ' ': {
            bool done = !all([](auto& r) { return r.done; });
            bool hides = done && !show_completed_ && view_.kind != View::Completed && view_.kind != View::AllReminders;
            if (hides) step_off(ids);
            batch("Complete", [&] {
                for (auto& id : ids) store_.set_done(id, done, today);
            });
            message_ = std::format("{} {}", n, done ? "done" : "not done");
            return true;
        }
        case 'f': {
            bool flag = !all([](auto& r) { return r.flagged; });
            each("Flag", [&](rem::Reminder& r) { r.flagged = flag; });
            message_ = std::format("{} {}", n, flag ? "flagged" : "unflagged");
            return true;
        }
        case 't': each("Due Today", [&](rem::Reminder& r) { r.due_date = today; }); return true;
        case 'T': {
            auto tomorrow = rem::Date{std::chrono::sys_days{today} + std::chrono::days{1}};
            each("Due Tomorrow", [&](rem::Reminder& r) { r.due_date = tomorrow; });
            return true;
        }
        case 'd':
            if (auto d = prompt(std::format("Due date for {} (today, tomorrow, fri, +3d, 2026-10-31, none):", n))) {
                if (term::lower(*d) == "none" || d->empty()) {
                    each("Clear Due Date", [](rem::Reminder& r) { r.due_date.reset(), r.due_time.reset(); });
                } else {
                    auto words = *d;
                    std::optional<rem::TimeOfDay> time;
                    if (auto sp = words.rfind(' '); sp != std::string::npos)
                        if ((time = rem::parse_time(words.substr(sp + 1)))) words.resize(sp);
                    if (auto date = rem::parse_human_date(words, today))
                        each("Set Due Date", [&](rem::Reminder& r) {
                            r.due_date = date;
                            if (time) r.due_time = time;
                        });
                    else message_ = std::format("Can't read “{}”", *d);
                }
            }
            return true;
        case '0':
        case '1':
        case '2':
        case '3': {
            auto p = static_cast<rem::Priority>(key - '0');
            each("Priority", [&](rem::Reminder& r) { r.priority = p; });
            return true;
        }
        case '#':
            if (auto t = prompt(std::format("Tag for {} (-tag removes):", n)); t && !t->empty()) {
                auto tag = *t;
                bool remove = tag.starts_with('-');
                if (remove) tag.erase(0, 1);
                if (tag.starts_with('#')) tag.erase(0, 1);
                each("Tag", [&](rem::Reminder& r) {
                    if (remove) std::erase(r.tags, tag);
                    else if (std::ranges::find(r.tags, tag) == r.tags.end()) r.tags.push_back(tag);
                });
            }
            return true;
        case 'm':
            if (auto name = prompt(std::format("Move {} to list:", n)); name && !name->empty()) {
                rem::ListFile* dest = nullptr;
                for (auto* l : store_.lists())
                    if (term::lower(store_.label(*l)).starts_with(term::lower(*name)) ||
                        term::lower(store_.key_of(*l)).starts_with(term::lower(*name))) {
                        dest = l;
                        break;
                    }
                if (!dest) {
                    message_ = std::format("No list called “{}”", *name);
                    return true;
                }
                auto moving = outermost(ids);
                if (view_.kind == View::List) step_off(ids);
                batch("Move", [&] {
                    for (auto& id : moving) store_.move_to_list(id, *dest);
                });
                message_ = std::format("Moved {} to “{}”", moving.size(), store_.label(*dest));
            }
            return true;
        case kDelete: {
            auto gone = outermost(ids);
            if (!confirm(std::format("Delete {} reminders?", n))) return true;
            step_off(ids);
            batch("Delete", [&] {
                for (auto& id : gone) store_.remove(id);
            });
            message_ = std::format("Deleted {} (u to undo)", n);
            return true;
        }
    }
    return false;
}

int Tui::run() {
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    // Ctrl+S / Ctrl+Q are XOFF / XON (pause / resume output) in a terminal;
    // turn that off so Ctrl+S reaches the app. ncurses restores it on exit.
    termios tio{};
    if (tcgetattr(STDIN_FILENO, &tio) == 0) {
        tio.c_iflag &= ~static_cast<tcflag_t>(IXON);
        tcsetattr(STDIN_FILENO, TCSANOW, &tio);
    }
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    timeout(1000);  // wake up every second to look for changes
    // Modified keys that terminals send as escape sequences ncurses knows by
    // these capability names (kUP3 = Alt+↑, kNXT5 = Ctrl+Page Down, …).
    auto code = [](const char* cap) {
        const char* seq = tigetstr(cap);
        if (!seq || seq == reinterpret_cast<const char*>(-1)) return 0;
        int c = key_defined(seq);
        return c > 0 ? c : 0;
    };
    alt_up_ = code("kUP3");
    alt_down_ = code("kDN3");
    alt_shift_up_ = code("kUP4");
    alt_shift_down_ = code("kDN4");
    ctrl_page_down_ = code("kNXT5");
    ctrl_page_up_ = code("kPRV5");
    setup_colors();
    check_folder();
#ifdef REMINDERS_NETWORK
    sync_ = std::make_unique<rem::SyncRunner>(store_);
#endif
    restore_view();
    if (hide_sidebar_) focus_items_ = true;  // started with the sidebar hidden (show-sidebar=false)

    try {
        draw();
        while (true) {
            wint_t key;
            int kind = get_wch(&key);
            if (kind == ERR) {
                check_sync();
                check_folder();
                draw();
                continue;
            }
            bool alt = false;
            if (kind == OK && key == 27) {  // Esc: on its own, or Alt+key (sent as Esc, key)
                nodelay(stdscr, TRUE);
                wint_t next;
                int next_kind = get_wch(&next);
                timeout(1000);
                if (next_kind != ERR) {
                    key = next;
                    kind = next_kind;
                    alt = true;
                }
            }
            if (!handle_key(key, kind == KEY_CODE_YES, alt)) break;
            check_folder();
            draw();
        }
    } catch (...) {
        endwin();
        throw;
    }
    endwin();
    return 0;
}

}  // namespace

int run_tui(rem::Library& store, bool remember, std::optional<bool> key_numbers) {
    Tui tui(store, remember);
    if (key_numbers) tui.set_show_key_numbers(*key_numbers);
    return tui.run();
}
