#include "tui.hpp"

#include <ncurses.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <clocale>
#include <cwchar>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/history.hpp"
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

enum Pair : short { kDim = 1, kRed, kSelected, kHeading, kStatus, kFirstListColor };

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
    enum Kind { Today, Scheduled, All, Flagged, Completed, List, Tag, Search } kind = Today;
    std::string name;
    bool operator==(const View&) const = default;
};

struct SidebarEntry {
    View view;
    std::string title;
    std::string color;
    int count = -1;
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
    Tui(rem::Store& store, fs::path folder) : store_(store), folder_(std::move(folder)) {}
    int run();

private:
    rem::Store& store_;
    fs::path folder_;
    rem::History history_;
    View view_;
    bool focus_items_ = false;
    bool show_completed_ = false;
    int side_sel_ = 0;
    std::string item_sel_;  // selected reminder id
    int item_scroll_ = 0;
    std::string message_;
    std::map<std::string, std::pair<fs::file_time_type, std::uintmax_t>> seen_;  // folder signature

    std::vector<SidebarEntry> sidebar();
    std::vector<Line> lines();
    void draw();
    void draw_sidebar(int width, int height);
    void draw_items(int x, int width, int height);
    void draw_status();
    std::optional<std::string> prompt(const std::string& label, const std::string& initial = "");
    bool confirm(const std::string& question);
    void show_help();
    void show_details(const std::string& id);
    bool handle_key(wint_t key, bool is_function_key);
    void move_selection(int delta);
    void select_view(const View& v);
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
};

std::vector<SidebarEntry> Tui::sidebar() {
    auto today = rem::local_today();
    std::vector<SidebarEntry> out = {
        {{View::Today, ""}, "Today", "blue", static_cast<int>(store_.today(today).size())},
        {{View::Scheduled, ""}, "Scheduled", "red", static_cast<int>(store_.scheduled().size())},
        {{View::All, ""}, "All", "gray", static_cast<int>(store_.all().size())},
        {{View::Flagged, ""}, "Flagged", "orange", static_cast<int>(store_.flagged().size())},
        {{View::Completed, ""}, "Completed", "gray", static_cast<int>(store_.completed().size())},
    };
    for (auto* l : store_.lists()) {
        int open = 0;
        l->doc.walk([&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
        out.push_back({{View::List, l->name}, l->name, l->color(), open});
    }
    for (auto& t : store_.tags()) out.push_back({{View::Tag, t}, "#" + t, "gray", -1});
    return out;
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
        if (show_list) line.where = "(" + ref.list->name + (ref.parent ? " > " + ref.parent->title : "") + ")";
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
                for (auto& s : r->subtasks)
                    if (!s.done || show_completed_) item({l, &s, r}, 2, false);
            }
        }
        return out;
    }

    std::vector<rem::Ref> refs;
    switch (view_.kind) {
        case View::Today: refs = store_.today(today); break;
        case View::Scheduled: refs = store_.scheduled(); break;
        case View::All: refs = store_.all(); break;
        case View::Flagged: refs = store_.flagged(); break;
        case View::Completed: refs = store_.completed(); break;
        case View::Tag: refs = store_.tagged(view_.name); break;
        case View::Search: refs = store_.search(view_.name); break;
        case View::List: break;
    }
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
        else if (view_.kind != View::Flagged) g = ref.list->name, color = ref.list->color();
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
    int y = 1;
    for (int i = 0; i < static_cast<int>(entries.size()) && y < height - 1; ++i, ++y) {
        if (i == 5) {
            attron(dim());
            put(y++, 1, "My Lists", width - 2);
            attroff(dim());
        }
        if (entries[i].view.kind == View::Tag && entries[i - 1].view.kind != View::Tag) {
            attron(dim());
            put(y++, 1, "Tags", width - 2);
            attroff(dim());
        }
        if (y >= height - 1) break;
        bool selected = i == side_sel_;
        bool current = entries[i].view == view_;
        if (selected && !focus_items_) attron(COLOR_PAIR(kSelected) | A_BOLD);
        else if (current) attron(A_BOLD);
        mvhline(y, 0, ' ', width - 1);
        bool colored = entries[i].view.kind == View::List;
        if (colored) attron(list_color(entries[i].color));
        put(y, 1, entries[i].title, width - 8);
        if (colored) attroff(list_color(entries[i].color));
        if (entries[i].count >= 0) {
            auto count = std::to_string(entries[i].count);
            attron(dim());
            put(y, width - 2 - static_cast<int>(count.size()), count, static_cast<int>(count.size()));
            attroff(dim());
        }
        attroff(COLOR_PAIR(kSelected) | A_BOLD);
    }
    mvvline_set(0, width - 1, WACS_VLINE, height - 1);
}

void Tui::draw_items(int x, int width, int height) {
    auto ls = lines();
    // Title
    std::string title;
    if (view_.kind == View::List) title = view_.name;
    else if (view_.kind == View::Tag) title = "#" + view_.name;
    else if (view_.kind == View::Search) title = std::format("Search: {}", view_.name);
    else title = sidebar()[static_cast<std::size_t>(view_.kind)].title;
    attron(A_BOLD);
    if (view_.kind == View::List)
        if (auto* l = store_.list(view_.name)) attron(list_color(l->color()));
    put(0, x + 1, "# " + title, width - 2);
    attroff(A_BOLD | A_COLOR);

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
    auto text = message_.empty()
                    ? std::string(focus_items_ ? " space done  a add  e edit  d due  f flag  x delete  u undo  / search  ? help  q quit"
                                               : " ↑↓ choose  enter open  tab switch  g go to  N new list  / search  ? help  q quit")
                    : " " + message_;
    put(h - 1, 0, text, COLS);
    attroff(COLOR_PAIR(kStatus));
}

void Tui::draw() {
    erase();
    int side = std::min(28, std::max(18, COLS / 4));
    draw_sidebar(side, LINES);
    draw_items(side, COLS - side, LINES);
    draw_status();
    refresh();
}

std::optional<std::string> Tui::prompt(const std::string& label, const std::string& initial) {
    auto text = widen(initial);
    curs_set(1);
    while (true) {
        attron(COLOR_PAIR(kStatus));
        mvhline(LINES - 1, 0, ' ', COLS);
        int used = put(LINES - 1, 0, " " + label + " ", COLS);
        // Show the end of the text if it's too long.
        std::wstring shown = text;
        int room = COLS - used - 1;
        while (text_width(shown) > room && !shown.empty()) shown.erase(0, 1);
        mvaddnwstr(LINES - 1, used, shown.c_str(), static_cast<int>(shown.size()));
        attroff(COLOR_PAIR(kStatus));
        move(LINES - 1, used + text_width(shown));
        refresh();

        wint_t ch;
        int kind = get_wch(&ch);
        if (kind == ERR) continue;
        if (kind == KEY_CODE_YES) {
            if (ch == KEY_BACKSPACE && !text.empty()) text.pop_back();
            else if (ch == KEY_ENTER) break;
            continue;
        }
        if (ch == 27) {  // Esc
            curs_set(0);
            return std::nullopt;
        }
        if (ch == '\n' || ch == '\r') break;
        if (ch == 127 || ch == 8) {
            if (!text.empty()) text.pop_back();
        } else if (ch == 21) {  // Ctrl+U
            text.clear();
        } else if (ch >= 32) {
            text += static_cast<wchar_t>(ch);
        }
    }
    curs_set(0);
    return narrow(text);
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
        "  ↑↓ / j k     move           tab         switch pane",
        "  1-9          sidebar entry  g           go to a list by name",
        "  /            search         c           show/hide completed",
        "  N            new list       u / Ctrl+R  undo / redo",
        "  ?            this help      q           quit",
        "",
        "On a reminder",
        "  space        complete       a           add (inline fields work)",
        "  e / enter    edit title     i           details",
        "  d            due date (today, tomorrow, fri, +3d, 2026-10-31, none)",
        "  t / T        due today / tomorrow",
        "  f            flag           0-3         priority none-high",
        "  #            add tag (-tag removes)     m  move to list",
        "  J / K        move down/up   > / <       indent / outdent",
        "  x / Delete   delete",
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

void Tui::show_details(const std::string& id) {
    auto ref = store_.find(id);
    if (!ref) return;
    auto& r = *ref->reminder;
    std::vector<std::string> rows = {term::markdown_line(r).text(), ""};
    auto add = [&](const char* label, const std::string& v) {
        if (!v.empty()) rows.push_back(std::format("{:<10}{}", label, v));
    };
    add("List", ref->list->name + (ref->parent ? " > " + ref->parent->title : ""));
    add("Status", r.done ? "completed" : "open");
    if (r.due_date) add("Due", rem::format_date(*r.due_date) + (r.due_time ? " " + rem::format_time(*r.due_time) : ""));
    add("Repeat", r.repeat.value_or(""));
    add("Priority", r.priority == rem::Priority::None ? "" : term::priority_name(r.priority));
    add("Flagged", r.flagged ? "yes" : "");
    std::string tags;
    for (auto& t : r.tags) tags += (tags.empty() ? "#" : " #") + t;
    add("Tags", tags);
    add("URL", r.url.value_or(""));
    if (!r.notes.empty()) {
        rows.push_back("");
        for (std::size_t s = 0;;) {
            auto nl = r.notes.find('\n', s);
            rows.push_back(r.notes.substr(s, nl - s));
            if (nl == std::string::npos) break;
            s = nl + 1;
        }
    }
    for (auto& s : r.subtasks) rows.push_back("  " + term::markdown_line(s).text());
    rows.push_back("");
    rows.push_back("Press any key to close.");

    int w = std::min(COLS - 4, 76), h = std::min(LINES - 2, static_cast<int>(rows.size()) + 2);
    auto* win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    box_set(win, WACS_VLINE, WACS_HLINE);
    for (int i = 0; i < static_cast<int>(rows.size()) && i < h - 2; ++i) {
        auto line = widen(rows[static_cast<std::size_t>(i)]);
        while (text_width(line) > w - 4) line.pop_back();
        mvwaddnwstr(win, i + 1, 2, line.c_str(), static_cast<int>(line.size()));
    }
    wrefresh(win);
    wint_t ch;
    while (get_wch(&ch) == ERR) {
    }
    delwin(win);
}

void Tui::select_view(const View& v) {
    view_ = v;
    item_scroll_ = 0;
    item_sel_.clear();
    auto entries = sidebar();
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        if (entries[static_cast<std::size_t>(i)].view == v) side_sel_ = i;
}

void Tui::move_selection(int delta) {
    if (!focus_items_) {
        side_sel_ = std::max(0, side_sel_ + delta);
        auto entries = sidebar();
        side_sel_ = std::min(side_sel_, static_cast<int>(entries.size()) - 1);
        view_ = entries[static_cast<std::size_t>(side_sel_)].view;
        item_scroll_ = 0;
        item_sel_.clear();
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

// Reloads when files in the folder change (Syncthing, the GUI, an editor).
void Tui::check_folder() {
    std::map<std::string, std::pair<fs::file_time_type, std::uintmax_t>> now;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(folder_, ec)) {
        auto name = e.path().filename().string();
        if (!name.ends_with(".md") || name.starts_with('.')) continue;
        now[name] = {e.last_write_time(ec), e.file_size(ec)};
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

bool Tui::handle_key(wint_t key, bool fn) {
    message_.clear();
    auto today = rem::local_today();
    auto id = item_sel_;
    auto ref = id.empty() ? std::nullopt : store_.find(id);
    bool in_list = view_.kind == View::List;

    // Keys that work anywhere.
    if (!fn) switch (key) {
            case 'q': return false;
            case '?': show_help(); return true;
            case '\t': focus_items_ = !focus_items_; return true;
            case 'j': move_selection(1); return true;
            case 'k': move_selection(-1); return true;
            case 'c': show_completed_ = !show_completed_; return true;
            case 'u': {
                auto r = history_.undo(store_);
                message_ = r.applied ? "Undone" : "Nothing to undo";
                for (auto& s : r.skipped) message_ = std::format("“{}” changed elsewhere; left as it is", s);
                return true;
            }
            case 18: {  // Ctrl+R
                auto r = history_.redo(store_);
                message_ = r.applied ? "Redone" : "Nothing to redo";
                return true;
            }
            case '/':
                if (auto q = prompt("Search:"); q && !q->empty()) {
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
                    for (auto& e : sidebar()) {
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
                if (auto name = prompt("New list name:"); name && !name->empty()) {
                    if (store_.list(*name)) {
                        message_ = "A list with that name already exists";
                    } else {
                        undoable("New List", [&] { store_.create_list(*name, "blue", "list"); });
                        select_view({View::List, *name});
                    }
                }
                return true;
        }
    if (!fn && key >= '1' && key <= '9' && !focus_items_) {
        auto entries = sidebar();
        auto n = static_cast<std::size_t>(key - '1');
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
        if ((!fn && (key == '\n' || key == '\r' || key == 'l')) || (fn && key == KEY_ENTER)) focus_items_ = true;
        else if (!fn && key == 'a') {
            focus_items_ = true;
            return handle_key('a', false);
        }
        return true;
    }

    // Adding works without a selection.
    if (!fn && key == 'a') {
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
    if (!ref) return true;
    auto& r = *ref->reminder;

    auto save = [&](const char* label, auto&& change) {
        undoable(label, [&] {
            change();
            store_.touch(id);
        });
    };
    if (fn && (key == KEY_DC)) key = 'x', fn = false;
    if (fn && key == KEY_ENTER) key = 'e', fn = false;
    if (fn) return true;

    switch (key) {
        case ' ':
            undoable("Complete", [&] { store_.set_done(id, !r.done, today); });
            break;
        case 'e':
        case '\n':
        case '\r':
            if (auto t = prompt("Title:", r.title); t && !t->empty() && *t != r.title)
                save("Edit Title", [&] { r.title = rem::parse_fields(*t).title; });
            break;
        case 'i': show_details(id); break;
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
                    if (term::lower(l->name).starts_with(term::lower(*name))) {
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
        case '>':
        case '<':
            if (!in_list) {
                message_ = "Indent in a list view";
                break;
            }
            undoable(key == '>' ? "Indent" : "Outdent", [&] {
                auto visible = [this](const rem::Reminder& x) { return show_completed_ || !x.done; };
                bool ok = key == '>' ? ref->list->doc.indent(id, visible) : ref->list->doc.outdent(id);
                if (ok) store_.save(*ref->list);
                else message_ = key == '>' ? "Can't indent this one" : "Not a subtask";
            });
            break;
        case 'x':
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

int Tui::run() {
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(25);
    curs_set(0);
    timeout(1000);  // wake up every second to look for changes
    setup_colors();
    check_folder();

    try {
        draw();
        while (true) {
            wint_t key;
            int kind = get_wch(&key);
            if (kind == ERR) {
                check_folder();
                draw();
                continue;
            }
            if (!handle_key(key, kind == KEY_CODE_YES)) break;
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

int run_tui(rem::Store& store, const std::filesystem::path& folder) { return Tui(store, folder).run(); }
