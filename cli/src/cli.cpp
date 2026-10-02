// reminders: the command-line interface (and, with no command, the TUI).

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "reminders/settings.hpp"
#include "reminders/store.hpp"
#include "reminders/syncthing.hpp"
#include "text.hpp"
#include "tui.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";

constexpr const char* kUsage = R"(Usage: reminders [--folder PATH] [--json] [--no-color] [COMMAND …]

With no command, opens the interactive (terminal) interface.

Commands:
  lists                         Lists, with how many reminders are open in each
  list [VIEW] [-a]              Reminders in VIEW: a list name, today (default),
                                scheduled, all, flagged, completed or #tag.
                                -a also shows completed reminders
  show REF                      Everything about one reminder
  add TEXT… [FIELDS]            Add a reminder (inline fields like "#tag" or
                                "📅 2026-10-03" work in TEXT too)
  edit REF [FIELDS]             Change a reminder
  done REF…                     Complete (repeating reminders roll forward)
  undone REF…                   Mark as not completed
  move REF LIST [--section S]   Move to another list
  delete REF… [--yes]           Delete
  search TEXT                   Search titles and notes
  new-list NAME [--color C] [--icon I]
  folder [PATH]                 Show or set the folder (shared with the app)
  tui                           Open the interactive interface

REF is a reminder's id (as shown by list, e.g. milk01) or part of its title.

Fields:
  --title TEXT      --list LIST      --section NAME   --parent REF (add only)
  --due DATE        DATE: today, tomorrow, fri, +3d, +2w, 2026-10-31
  --time HH:MM      --no-due
  --flag            --unflag
  --priority none|low|medium|high
  --tag TAG         --untag TAG      (repeatable)
  --repeat RULE     e.g. "every week", "every 2 months"   --no-repeat
  --notes TEXT      --url URL

Options:
  -f, --folder PATH  Use PATH instead of the saved folder
  --json             Machine-readable output
  --no-color         No colours (also when NO_COLOR is set or not a terminal)
  -h, --help         This help
  --version          Show the version
)";

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Global {
    std::optional<rem::fs::path> folder;
    bool json = false;
    bool color = false;
};

// --- output ---------------------------------------------------------------

struct Style {
    bool on;
    std::string fg(term::Rgb c) const { return on ? std::format("\033[38;2;{};{};{}m", c.r, c.g, c.b) : ""; }
    std::string bold() const { return on ? "\033[1m" : ""; }
    std::string dim() const { return on ? "\033[2m" : ""; }
    std::string red() const { return on ? "\033[31m" : ""; }
    std::string reset() const { return on ? "\033[0m" : ""; }
};

std::string json_escape(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string json_reminder(const rem::Ref& ref) {
    auto& r = *ref.reminder;
    std::string tags = "[";
    for (std::size_t i = 0; i < r.tags.size(); ++i) tags += (i ? "," : "") + json_escape(r.tags[i]);
    tags += "]";
    auto opt = [](const std::optional<std::string>& v) { return v ? json_escape(*v) : std::string("null"); };
    auto section = ref.list->doc.section_of(ref.parent ? *ref.parent : r);
    return std::format(
        R"({{"id":{},"list":{},"section":{},"parent":{},"title":{},"done":{},"due":{},"time":{},)"
        R"("completed":{},"flagged":{},"priority":"{}","tags":{},"repeat":{},"url":{},"notes":{}}})",
        json_escape(r.id), json_escape(ref.list->name), opt(section),
        ref.parent ? json_escape(ref.parent->id) : "null", json_escape(r.title), r.done ? "true" : "false",
        r.due_date ? json_escape(rem::format_date(*r.due_date)) : "null",
        r.due_time ? json_escape(rem::format_time(*r.due_time)) : "null",
        r.completed ? json_escape(rem::format_date(*r.completed)) : "null", r.flagged ? "true" : "false",
        term::priority_name(r.priority), tags, opt(r.repeat), opt(r.url), json_escape(r.notes));
}

void print_json(const std::vector<rem::Ref>& refs) {
    std::cout << "[";
    for (std::size_t i = 0; i < refs.size(); ++i) std::cout << (i ? ",\n " : "") << json_reminder(refs[i]);
    std::cout << "]\n";
}

// One reminder line: "  ○ !!! Milk  Today 17:30  #errands  🚩   milk01"
void print_reminder(const rem::Ref& ref, const Style& st, int indent, bool show_list, rem::Date today) {
    auto& r = *ref.reminder;
    auto color = term::color_rgb(ref.list->color());
    std::string line(static_cast<std::size_t>(indent), ' ');
    line += st.fg(color) + (r.done ? "●" : "○") + st.reset() + " ";
    if (r.priority != rem::Priority::None) line += st.fg(color) + term::priority_marks(r.priority) + st.reset() + " ";
    line += r.done ? st.dim() + r.title + st.reset() : r.title;
    if (r.due_date)
        line += "  " + (term::is_overdue(r, today) ? st.red() : st.dim()) + term::due_label(r, today) + st.reset();
    if (r.repeat) line += st.dim() + " ⟳" + st.reset();
    for (auto& t : r.tags) line += "  " + st.fg(color) + "#" + t + st.reset();
    if (r.flagged) line += "  " + st.fg(term::color_rgb("orange")) + "⚑" + st.reset();
    if (show_list) line += "  " + st.dim() + ref.list->name + (ref.parent ? " › " + ref.parent->title : "") + st.reset();
    line += "  " + st.dim() + r.id + st.reset();
    std::cout << line << "\n";
    if (!r.notes.empty()) {
        auto first = r.notes.substr(0, r.notes.find('\n'));
        std::cout << std::string(static_cast<std::size_t>(indent) + 2, ' ') << st.dim() << first << st.reset()
                  << "\n";
    }
}

void print_heading(const std::string& text, std::optional<term::Rgb> color, const Style& st) {
    std::cout << st.bold() << (color ? st.fg(*color) : "") << text << st.reset() << "\n";
}

// --- argument parsing -------------------------------------------------------

struct Args {
    std::vector<std::string> positional;
    std::multimap<std::string, std::string> options;  // name → value ("" for flags)

    bool has(const std::string& k) const { return options.contains(k); }
    std::optional<std::string> get(const std::string& k) const {
        auto it = options.find(k);
        return it == options.end() ? std::nullopt : std::optional{it->second};
    }
    std::vector<std::string> all(const std::string& k) const {
        std::vector<std::string> out;
        for (auto [a, b] = options.equal_range(k); a != b; ++a) out.push_back(a->second);
        return out;
    }
};

// Options that take a value; anything else starting with "--" is a flag.
const std::vector<std::string> kValued = {"title", "list", "section", "parent", "due",    "time",  "priority",
                                          "tag",   "untag", "repeat", "notes",  "url",   "color", "icon"};
const std::vector<std::string> kFlags = {"no-due", "flag", "unflag", "no-repeat", "yes", "all"};

Args parse_args(std::span<const std::string> in) {
    Args a;
    for (std::size_t i = 0; i < in.size(); ++i) {
        auto& s = in[i];
        if (s == "--") {
            a.positional.insert(a.positional.end(), in.begin() + static_cast<long>(i) + 1, in.end());
            break;
        }
        if (s == "-a" || s == "-y") {
            a.options.emplace(s == "-a" ? "all" : "yes", "");
        } else if (s.starts_with("--")) {
            auto name = s.substr(2);
            std::string value;
            if (auto eq = name.find('='); eq != std::string::npos) {
                value = name.substr(eq + 1);
                name.resize(eq);
            } else if (std::ranges::find(kValued, name) != kValued.end()) {
                if (i + 1 >= in.size()) throw UsageError(std::format("--{} needs a value", name));
                value = in[++i];
            }
            if (std::ranges::find(kValued, name) == kValued.end() && std::ranges::find(kFlags, name) == kFlags.end())
                throw UsageError(std::format("unknown option --{}", name));
            a.options.emplace(name, value);
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

std::string join(const std::vector<std::string>& v, std::size_t from = 0) {
    std::string out;
    for (auto i = from; i < v.size(); ++i) out += (out.empty() ? "" : " ") + v[i];
    return out;
}

// --- the folder -----------------------------------------------------------------

class App {
public:
    App(Global g, rem::fs::path folder)
        : g_(g), st_{g.color}, store_(folder, rem::state_dir(folder, rem::device_name())), today_(rem::local_today()) {
        try {
            rem::ignore_state_in_syncthing(folder);
        } catch (const std::exception&) {
        }
        store_.load_all();
    }

    int run(const std::string& cmd, const Args& a);
    rem::Store& store() { return store_; }

private:
    Global g_;
    Style st_;
    rem::Store store_;
    rem::Date today_;

    rem::ListFile& list_named(const std::string& name);
    rem::ListFile& default_list();
    rem::Ref resolve(const std::string& ref);
    void apply_fields(rem::Reminder& r, const Args& a);
    void report(const std::string& verb, const rem::Ref& ref);

    int cmd_lists();
    int cmd_list(const Args& a);
    int cmd_show(const Args& a);
    int cmd_add(const Args& a);
    int cmd_edit(const Args& a);
    int cmd_done(const Args& a, bool done);
    int cmd_move(const Args& a);
    int cmd_delete(const Args& a);
    int cmd_search(const Args& a);
    int cmd_new_list(const Args& a);
};

rem::ListFile& App::list_named(const std::string& name) {
    if (auto* l = store_.list(name)) return *l;
    for (auto* l : store_.lists())
        if (term::lower(l->name) == term::lower(name)) return *l;
    throw std::runtime_error(std::format("no list called “{}”", name));
}

rem::ListFile& App::default_list() {
    auto lists = store_.lists();
    if (lists.empty()) throw std::runtime_error("there are no lists yet: create one with `reminders new-list NAME`");
    return *lists.front();
}

rem::Ref App::resolve(const std::string& ref) {
    auto id = ref.starts_with('^') ? ref.substr(1) : ref;
    if (auto r = store_.find(id)) return *r;
    // Part of a title: prefer an exact title, then a unique match, open ones first.
    auto q = term::lower(ref);
    std::vector<rem::Ref> exact, partial;
    for (auto* l : store_.lists())
        l->doc.walk([&](rem::Reminder& r, rem::Reminder* parent) {
            auto t = term::lower(r.title);
            if (t == q) exact.push_back({l, &r, parent});
            else if (t.find(q) != std::string::npos) partial.push_back({l, &r, parent});
        });
    for (auto* candidates : {&exact, &partial}) {
        if (candidates->size() > 1) {
            std::vector<rem::Ref> open;
            for (auto& c : *candidates)
                if (!c.reminder->done) open.push_back(c);
            if (open.size() == 1) return open.front();
        }
        if (candidates->size() == 1) return candidates->front();
        if (candidates->size() > 1) {
            std::string msg = std::format("“{}” matches several reminders; use an id:\n", ref);
            for (std::size_t k = 0; k < candidates->size() && k < 10; ++k) {
                auto& c = (*candidates)[k];
                msg += std::format("  {}  {}  ({})\n", c.reminder->id, c.reminder->title, c.list->name);
            }
            if (candidates->size() > 10) msg += std::format("  … and {} more\n", candidates->size() - 10);
            msg.pop_back();
            throw std::runtime_error(msg);
        }
    }
    throw std::runtime_error(std::format("no reminder matches “{}”", ref));
}

void App::apply_fields(rem::Reminder& r, const Args& a) {
    if (auto t = a.get("title")) {
        auto f = rem::parse_fields(*t);
        r.title = f.title;
    }
    if (a.has("no-due")) {
        r.due_date.reset();
        r.due_time.reset();
    }
    if (auto d = a.get("due")) {
        auto date = rem::parse_human_date(*d, today_);
        if (!date) throw UsageError(std::format("can't read the date “{}”", *d));
        r.due_date = date;
    }
    if (auto t = a.get("time")) {
        auto time = rem::parse_time(*t);
        if (!time) throw UsageError(std::format("can't read the time “{}” (use HH:MM)", *t));
        if (!r.due_date) r.due_date = today_;
        r.due_time = time;
    }
    if (a.has("flag")) r.flagged = true;
    if (a.has("unflag")) r.flagged = false;
    if (auto p = a.get("priority")) {
        static const std::pair<const char*, rem::Priority> names[] = {
            {"none", rem::Priority::None}, {"low", rem::Priority::Low},
            {"medium", rem::Priority::Medium}, {"high", rem::Priority::High}};
        auto it = std::ranges::find_if(names, [&](auto& n) { return term::lower(*p) == n.first; });
        if (it == std::end(names)) throw UsageError("--priority is none, low, medium or high");
        r.priority = it->second;
    }
    for (auto t : a.all("tag")) {
        if (t.starts_with('#')) t.erase(0, 1);
        if (!t.empty() && std::ranges::find(r.tags, t) == r.tags.end()) r.tags.push_back(t);
    }
    for (auto t : a.all("untag")) {
        if (t.starts_with('#')) t.erase(0, 1);
        std::erase(r.tags, t);
    }
    if (a.has("no-repeat")) r.repeat.reset();
    if (auto rule = a.get("repeat")) r.repeat = *rule;
    if (auto n = a.get("notes")) r.notes = *n;
    if (auto u = a.get("url")) r.url = u->empty() ? std::nullopt : std::optional{*u};
}

void App::report(const std::string& verb, const rem::Ref& ref) {
    if (g_.json) {
        std::cout << json_reminder(ref) << "\n";
        return;
    }
    std::cout << verb << ": ";
    print_reminder(ref, st_, 0, true, today_);
}

int App::cmd_lists() {
    auto lists = store_.lists();
    if (g_.json) {
        std::cout << "[";
        for (std::size_t i = 0; i < lists.size(); ++i) {
            int open = 0;
            lists[i]->doc.walk([&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
            std::cout << (i ? ",\n " : "")
                      << std::format(R"({{"name":{},"color":"{}","icon":"{}","open":{}}})", json_escape(lists[i]->name),
                                     lists[i]->color(), lists[i]->icon(), open);
        }
        std::cout << "]\n";
        return 0;
    }
    for (auto* l : lists) {
        int open = 0;
        l->doc.walk([&](rem::Reminder& r, rem::Reminder*) { open += !r.done; });
        std::cout << st_.fg(term::color_rgb(l->color())) << "● " << st_.reset() << l->name << st_.dim() << "  " << open
                  << st_.reset() << "\n";
    }
    if (!store_.candidates().empty() && !g_.json)
        std::cout << st_.dim() << "\nNot lists yet (no “reminders: 1”): " << join(store_.candidates()) << st_.reset()
                  << "\n";
    return 0;
}

int App::cmd_list(const Args& a) {
    auto view = a.positional.empty() ? std::string("today") : join(a.positional);
    bool with_done = a.has("all");
    auto v = term::lower(view);

    std::vector<rem::Ref> refs;
    std::string title;
    bool by_date = false, flat = false;
    if (v == "today") {
        refs = store_.today(today_), title = "Today", by_date = true;
    } else if (v == "scheduled") {
        refs = store_.scheduled(), title = "Scheduled", by_date = true;
    } else if (v == "all") {
        title = "All";
        if (with_done) {
            for (auto* l : store_.lists())
                l->doc.walk([&](rem::Reminder& r, rem::Reminder* p) { refs.push_back({l, &r, p}); });
        } else {
            refs = store_.all();
        }
    } else if (v == "flagged") {
        refs = store_.flagged(), title = "Flagged", flat = true;
    } else if (v == "completed") {
        refs = store_.completed(), title = "Completed";
    } else if (v.starts_with('#')) {
        refs = store_.tagged(view.substr(1)), title = view;
        if (!with_done) std::erase_if(refs, [](auto& r) { return r.reminder->done; });
    } else {
        // A list, shown with its sections.
        auto& l = list_named(view);
        if (g_.json) {
            std::vector<rem::Ref> all;
            l.doc.walk([&](rem::Reminder& r, rem::Reminder* p) {
                if (with_done || !r.done) all.push_back({&l, &r, p});
            });
            print_json(all);
            return 0;
        }
        print_heading(l.name, term::color_rgb(l.color()), st_);
        int hidden = 0;
        for (auto& section : l.doc.sections()) {
            if (section.name) std::cout << "\n" << st_.fg(term::color_rgb(l.color())) << "  " << *section.name << st_.reset() << "\n";
            for (auto* r : section.reminders) {
                if (r->done && !with_done) {
                    ++hidden;
                    continue;
                }
                print_reminder({&l, r, nullptr}, st_, 2, false, today_);
                for (auto& s : r->subtasks) {
                    if (s.done && !with_done) {
                        ++hidden;
                        continue;
                    }
                    print_reminder({&l, &s, r}, st_, 4, false, today_);
                }
            }
        }
        if (hidden) std::cout << st_.dim() << "\n  " << hidden << " completed (show with -a)" << st_.reset() << "\n";
        return 0;
    }

    if (g_.json) {
        print_json(refs);
        return 0;
    }
    print_heading(title, std::nullopt, st_);
    if (refs.empty()) {
        std::cout << st_.dim() << "  Nothing here." << st_.reset() << "\n";
        return 0;
    }
    if (by_date) {
        std::ranges::stable_sort(refs, [](auto& x, auto& y) {
            auto key = [](const rem::Ref& r) {
                auto t = r.reminder->due_time.value_or(rem::TimeOfDay{-1, 0});
                return std::pair{*r.reminder->due_date, t.hour * 60 + t.minute};
            };
            return key(x) < key(y);
        });
    }
    std::string group;
    for (auto& ref : refs) {
        std::string g;
        if (by_date) g = *ref.reminder->due_date < today_ ? "Overdue" : rem::relative_date(*ref.reminder->due_date, today_);
        else if (!flat) g = ref.list->name;
        if (g != group) {
            std::cout << "\n" << st_.dim() << "  " << g << st_.reset() << "\n";
            group = g;
        }
        print_reminder(ref, st_, 2, by_date || flat, today_);
    }
    return 0;
}

int App::cmd_show(const Args& a) {
    if (a.positional.size() != 1) throw UsageError("show needs one REF");
    auto ref = resolve(a.positional[0]);
    if (g_.json) {
        std::cout << json_reminder(ref) << "\n";
        return 0;
    }
    auto& r = *ref.reminder;
    auto row = [&](const char* label, const std::string& value) {
        if (!value.empty()) std::cout << st_.dim() << std::format("{:<10}", label) << st_.reset() << value << "\n";
    };
    std::cout << st_.bold() << r.title << st_.reset() << "\n";
    row("id", r.id);
    row("list", ref.list->name + (ref.parent ? " › " + ref.parent->title : ""));
    row("section", ref.list->doc.section_of(ref.parent ? *ref.parent : r).value_or(""));
    row("status", r.done ? "completed" + (r.completed ? " " + rem::format_date(*r.completed) : std::string()) : "open");
    if (r.due_date)
        row("due", rem::format_date(*r.due_date) + (r.due_time ? " " + rem::format_time(*r.due_time) : "") + "  (" +
                       term::due_label(r, today_) + ")");
    row("repeat", r.repeat.value_or(""));
    row("priority", r.priority == rem::Priority::None ? "" : term::priority_name(r.priority));
    row("flagged", r.flagged ? "yes" : "");
    std::string tags;
    for (auto& t : r.tags) tags += (tags.empty() ? "#" : " #") + t;
    row("tags", tags);
    row("url", r.url.value_or(""));
    if (!r.notes.empty()) {
        std::cout << st_.dim() << "notes" << st_.reset() << "\n";
        for (std::size_t s = 0;;) {
            auto nl = r.notes.find('\n', s);
            std::cout << "  " << r.notes.substr(s, nl - s) << "\n";
            if (nl == std::string::npos) break;
            s = nl + 1;
        }
    }
    if (!r.subtasks.empty()) {
        std::cout << st_.dim() << "subtasks" << st_.reset() << "\n";
        for (auto& s : r.subtasks) print_reminder({ref.list, &s, &r}, st_, 2, false, today_);
    }
    return 0;
}

int App::cmd_add(const Args& a) {
    auto text = join(a.positional);
    if (text.empty() && !a.has("title")) throw UsageError("add needs the reminder's text");
    rem::Reminder r;
    r.fields() = rem::parse_fields(text);
    r.created = today_;
    apply_fields(r, a);

    if (auto parent_ref = a.get("parent")) {
        auto parent = resolve(*parent_ref);
        if (parent.parent) throw std::runtime_error("subtasks can't have subtasks of their own");
        auto id = rem::new_id();
        r.id = id;
        parent.reminder->subtasks.push_back(std::move(r));
        store_.save(*parent.list);
        report("Added", *store_.find(id));
        return 0;
    }
    auto& l = a.get("list") ? list_named(*a.get("list")) : default_list();
    auto& added = store_.add(l, std::move(r), nullptr, a.get("section"));
    report("Added", *store_.find(added.id));
    return 0;
}

int App::cmd_edit(const Args& a) {
    if (a.positional.size() != 1) throw UsageError("edit needs one REF (then the fields to change)");
    auto ref = resolve(a.positional[0]);
    auto id = ref.reminder->id;
    apply_fields(*ref.reminder, a);
    store_.touch(id);
    if (auto l = a.get("list")) {
        auto& dest = list_named(*l);
        if (&dest != ref.list) store_.move_to_list(id, dest);
    }
    report("Updated", *store_.find(id));
    return 0;
}

int App::cmd_done(const Args& a, bool done) {
    if (a.positional.empty()) throw UsageError(done ? "done needs a REF" : "undone needs a REF");
    for (auto& ref_text : a.positional) {
        auto ref = resolve(ref_text);
        auto id = ref.reminder->id;
        store_.set_done(id, done, today_);
        report(done ? "Completed" : "Reopened", *store_.find(id));
    }
    return 0;
}

int App::cmd_move(const Args& a) {
    if (a.positional.size() < 2) throw UsageError("move needs a REF and a LIST");
    auto ref = resolve(a.positional[0]);
    auto id = ref.reminder->id;
    auto& dest = list_named(join(a.positional, 1));
    if (&dest != ref.list) store_.move_to_list(id, dest);
    if (auto section = a.get("section")) {
        dest.doc.move_to_end(id, *section);
        store_.save(dest);
    }
    report("Moved", *store_.find(id));
    return 0;
}

int App::cmd_delete(const Args& a) {
    if (a.positional.empty()) throw UsageError("delete needs a REF");
    std::vector<rem::Ref> refs;
    for (auto& r : a.positional) refs.push_back(resolve(r));
    if (!a.has("yes") && isatty(STDIN_FILENO)) {
        for (auto& r : refs) print_reminder(r, st_, 2, true, today_);
        std::cout << (refs.size() == 1 ? "Delete this reminder?" : "Delete these reminders?") << " [y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (term::lower(answer) != "y" && term::lower(answer) != "yes") return 1;
    }
    std::vector<std::string> ids;
    for (auto& r : refs) ids.push_back(r.reminder->id);
    for (auto& id : ids) store_.remove(id);
    if (!g_.json) std::cout << "Deleted " << ids.size() << (ids.size() == 1 ? " reminder" : " reminders") << "\n";
    return 0;
}

int App::cmd_search(const Args& a) {
    auto q = join(a.positional);
    if (q.empty()) throw UsageError("search needs some text");
    auto refs = store_.search(q);
    if (g_.json) {
        print_json(refs);
        return 0;
    }
    if (refs.empty()) {
        std::cout << st_.dim() << "No results." << st_.reset() << "\n";
        return 1;
    }
    for (auto& r : refs) print_reminder(r, st_, 0, true, today_);
    return 0;
}

int App::cmd_new_list(const Args& a) {
    auto name = join(a.positional);
    if (name.empty() || name.front() == '.' || name.find_first_of("/\\<>:\"|?*") != std::string::npos ||
        name.find(".sync-conflict-") != std::string::npos)
        throw UsageError("list names can't be empty, start with a dot, or contain / \\ < > : \" | ? *");
    for (auto* l : store_.lists())
        if (term::lower(l->name) == term::lower(name)) throw std::runtime_error("a list with that name already exists");
    auto color = a.get("color").value_or("blue");
    auto icon = a.get("icon").value_or("list");
    if (std::ranges::find(rem::kColors, color) == std::end(rem::kColors))
        throw UsageError("colours: red orange yellow green cyan blue indigo purple pink brown gray");
    if (std::ranges::find(rem::kIcons, icon) == std::end(rem::kIcons))
        throw UsageError("unknown icon; see docs/FORMAT.md for the names");
    store_.create_list(name, color, icon);
    if (!g_.json) std::cout << "Created " << name << "\n";
    return 0;
}

int App::run(const std::string& cmd, const Args& a) {
    if (cmd == "lists") return cmd_lists();
    if (cmd == "list" || cmd == "ls") return cmd_list(a);
    if (cmd == "show") return cmd_show(a);
    if (cmd == "add") return cmd_add(a);
    if (cmd == "edit") return cmd_edit(a);
    if (cmd == "done") return cmd_done(a, true);
    if (cmd == "undone") return cmd_done(a, false);
    if (cmd == "move" || cmd == "mv") return cmd_move(a);
    if (cmd == "delete" || cmd == "rm") return cmd_delete(a);
    if (cmd == "search") return cmd_search(a);
    if (cmd == "new-list") return cmd_new_list(a);
    throw UsageError(std::format("unknown command “{}” (see reminders --help)", cmd));
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    Global g;
    g.color = isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");

    // Global options come before the command.
    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        auto& s = args[i];
        if (s == "-h" || s == "--help") {
            std::cout << kUsage;
            return 0;
        }
        if (s == "--version") {
            std::cout << "reminders " << kVersion << "\n";
            return 0;
        }
        if (s == "--json") g.json = true;
        else if (s == "--no-color") g.color = false;
        else if (s == "-f" || s == "--folder") {
            if (i + 1 >= args.size()) {
                std::cerr << "reminders: --folder needs a path\n";
                return 2;
            }
            g.folder = args[++i];
        } else if (s.starts_with("--folder=")) g.folder = s.substr(9);
        else break;
    }
    std::string cmd = i < args.size() ? args[i++] : "tui";
    std::vector<std::string> rest(args.begin() + static_cast<long>(i), args.end());

    try {
        if (cmd == "folder") {
            if (rest.empty()) {
                auto f = rem::saved_folder();
                if (!f) {
                    std::cerr << "reminders: no folder set (use `reminders folder PATH`)\n";
                    return 1;
                }
                std::cout << f->string() << "\n";
                return 0;
            }
            std::error_code ec;
            auto path = rem::fs::absolute(rest[0], ec);
            if (ec || !rem::fs::is_directory(path)) throw std::runtime_error(std::format("“{}” is not a folder", rest[0]));
            rem::save_setting("folder", path.lexically_normal().string());
            std::cout << "Folder set to " << path.lexically_normal().string() << "\n";
            return 0;
        }

        auto folder = g.folder ? std::optional{rem::fs::absolute(*g.folder)} : rem::saved_folder();
        if (!folder) throw std::runtime_error("no folder: pass --folder PATH, or set one with `reminders folder PATH`");
        if (!rem::fs::is_directory(*folder)) throw std::runtime_error(std::format("“{}” is not a folder", folder->string()));

        App app(g, *folder);
        if (cmd == "tui") {
            if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
                throw UsageError("the interactive interface needs a terminal; see reminders --help for commands");
            return run_tui(app.store(), *folder);
        }
        return app.run(cmd, parse_args(rest));
    } catch (const UsageError& e) {
        std::cerr << "reminders: " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "reminders: " << e.what() << "\n";
        return 1;
    }
}
