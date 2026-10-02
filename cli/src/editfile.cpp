#include "editfile.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <functional>
#include <sstream>
#include <stdexcept>

#include "reminders/dates.hpp"
#include "reminders/format.hpp"
#include "text.hpp"

namespace editfile {

namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r");
    return std::string(s.substr(b, e - b + 1));
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}

bool parse_bool(const std::string& key, const std::string& v) {
    auto t = term::lower(v);
    if (t == "true" || t == "yes" || t == "y" || t == "x") return true;
    if (t == "false" || t == "no" || t == "n" || t.empty()) return false;
    throw std::runtime_error(std::format("{}: use true or false, not “{}”", key, v));
}

}  // namespace

std::string render(const rem::Ref& ref, const std::vector<std::string>& lists, rem::Date today) {
    auto& r = *ref.reminder;
    std::string list_names;
    for (auto& l : lists) list_names += (list_names.empty() ? "" : ", ") + l;
    std::string tags;
    for (auto& t : r.tags) tags += (tags.empty() ? "" : ", ") + t;

    std::string out = std::format(
        "# Editing “{}” in {}. Change values, save and quit; an empty value\n"
        "# clears a field. Quit without saving to cancel.\n"
        "#   due: today, tomorrow, fri, +3d, 2026-10-31     time: 17:30\n"
        "#   repeat: never, every day, every weekday, every week, every 2 weeks, every month, every year\n"
        "#   priority: none, low, medium, high              list: {}\n",
        r.title, ref.list->name, list_names);
    if (!ref.parent) out += "#   subtasks: Markdown lines; add, remove, or tick them with [x]\n";
    out += std::format("title: {}\n", r.title);
    out += std::format("done: {}\n", r.done ? "true" : "false");
    out += std::format("due: {}\n", r.due_date ? rem::format_date(*r.due_date) : "");
    out += std::format("time: {}\n", r.due_time ? rem::format_time(*r.due_time) : "");
    out += std::format("repeat: {}\n", r.repeat.value_or("never"));
    out += std::format("priority: {}\n", term::priority_name(r.priority));
    out += std::format("flagged: {}\n", r.flagged ? "true" : "false");
    out += std::format("tags: {}\n", tags);
    out += std::format("list: {}\n", ref.list->name);
    if (!ref.parent) out += std::format("section: {}\n", ref.list->doc.section_of(r).value_or(""));
    out += std::format("url: {}\n", r.url.value_or(""));
    if (r.notes.empty()) {
        out += "notes:\n";
    } else {
        out += "notes: |\n";
        for (auto& l : split_lines(r.notes)) out += (l.empty() ? "" : "  " + l) + "\n";
    }
    if (!ref.parent) {
        out += "subtasks:\n";
        for (auto& s : r.subtasks) out += "  " + term::markdown_line(s).text() + "\n";
    }
    (void)today;
    return out;
}

std::optional<Edited> parse(const std::string& text, rem::Date today) {
    Edited e;
    bool any = false, has_list = false;
    auto lines = split_lines(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto& line = lines[i];
        if (trim(line).empty() || trim(line).starts_with('#')) continue;
        if (line[0] == ' ' || line[0] == '\t')
            throw std::runtime_error(std::format("line {}: indented, but not under notes: or subtasks:", i + 1));
        auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error(std::format("line {}: expected “name: value”", i + 1));
        auto key = term::lower(trim(line.substr(0, colon)));
        auto value = trim(line.substr(colon + 1));
        any = true;

        if (key == "notes" || key == "subtasks") {
            // A block of indented lines follows ("notes: |" or a bare key).
            std::vector<std::string> block;
            if (key == "notes" && !value.empty() && value != "|" && value != ">") block.push_back(value);
            while (i + 1 < lines.size() &&
                   (lines[i + 1].empty() || lines[i + 1][0] == ' ' || lines[i + 1][0] == '\t')) {
                auto& b = lines[++i];
                block.push_back(b.size() >= 2 && b.starts_with("  ") ? b.substr(2) : trim(b));
            }
            while (!block.empty() && trim(block.back()).empty()) block.pop_back();
            if (key == "notes") {
                for (std::size_t k = 0; k < block.size(); ++k) e.notes += (k ? "\n" : "") + block[k];
            } else {
                e.has_subtasks = true;
                for (auto& b : block)
                    if (!trim(b).empty()) e.subtasks.push_back(trim(b));
            }
            continue;
        }

        auto& f = e.fields;
        if (key == "title") {
            auto parsed = rem::parse_fields(value);  // "#tags" typed into the title become tags
            f.title = parsed.title;
            for (auto& t : parsed.tags)
                if (std::ranges::find(f.tags, t) == f.tags.end()) f.tags.push_back(t);
        } else if (key == "done") {
            f.done = parse_bool(key, value);
        } else if (key == "due") {
            if (!value.empty()) {
                auto d = rem::parse_human_date(value, today);
                if (!d) throw std::runtime_error(std::format("due: can't read “{}”", value));
                f.due_date = d;
            }
        } else if (key == "time") {
            if (!value.empty()) {
                auto t = rem::parse_time(value);
                if (!t) throw std::runtime_error(std::format("time: can't read “{}” (use HH:MM)", value));
                f.due_time = t;
            }
        } else if (key == "repeat") {
            if (!value.empty() && term::lower(value) != "never") f.repeat = value;
        } else if (key == "priority") {
            auto p = term::lower(value);
            if (p.empty() || p == "none") f.priority = rem::Priority::None;
            else if (p == "low") f.priority = rem::Priority::Low;
            else if (p == "medium") f.priority = rem::Priority::Medium;
            else if (p == "high") f.priority = rem::Priority::High;
            else throw std::runtime_error(std::format("priority: none, low, medium or high, not “{}”", value));
        } else if (key == "flagged") {
            f.flagged = parse_bool(key, value);
        } else if (key == "tags") {
            std::string word;
            for (char c : value + " ") {
                if (c == ' ' || c == ',') {
                    if (word.starts_with('#')) word.erase(0, 1);
                    if (!word.empty() && std::ranges::find(f.tags, word) == f.tags.end()) f.tags.push_back(word);
                    word.clear();
                } else {
                    word += c;
                }
            }
        } else if (key == "list") {
            e.list = value;
            has_list = true;
        } else if (key == "section") {
            if (!value.empty()) e.section = value;
        } else if (key == "url") {
            if (!value.empty()) f.url = value;
        } else {
            throw std::runtime_error(std::format("line {}: unknown field “{}”", i + 1, key));
        }
    }
    if (!any) return std::nullopt;
    if (e.fields.title.empty()) throw std::runtime_error("title: can't be empty");
    if (e.fields.due_time && !e.fields.due_date) e.fields.due_date = today;  // a time needs a date
    if (!has_list) throw std::runtime_error("list: missing");
    return e;
}

void apply(rem::Store& store, const std::string& id, const Edited& e, rem::Date today) {
    auto ref = store.find(id);
    if (!ref) throw std::runtime_error("the reminder was changed elsewhere");
    auto* dest = store.list(e.list);
    if (!dest)
        for (auto* l : store.lists())
            if (term::lower(l->name) == term::lower(e.list)) dest = l;
    if (!dest) throw std::runtime_error(std::format("list: there's no list called “{}”", e.list));

    auto& r = *ref->reminder;
    bool was_done = r.done;
    auto created = r.created;
    auto completed = r.completed;
    r.fields() = e.fields;
    r.done = was_done;  // completion goes through set_done below (repeats, subtasks)
    r.completed = completed;
    r.created = created;
    r.notes = e.notes;

    if (e.has_subtasks && !ref->parent) {
        // Match lines to existing subtasks by title so they keep their notes,
        // ids and dates; the rest are new, and unlisted ones are removed.
        auto old = std::move(r.subtasks);
        r.subtasks.clear();
        std::vector<bool> used(old.size());
        for (auto& line : e.subtasks) {
            auto doc = rem::parse(line.starts_with("- [") || line.starts_with("* [") ? line : "- [ ] " + line);
            auto parsed = doc.reminders();
            if (parsed.empty()) continue;
            auto& p = *parsed.front();
            rem::Reminder s;
            for (std::size_t k = 0; k < old.size(); ++k)
                if (!used[k] && old[k].title == p.title) {
                    s = old[k];
                    used[k] = true;
                    break;
                }
            bool existing = !s.id.empty();
            auto keep_created = s.created;
            bool was = s.done;
            s.fields() = p.fields();
            s.created = existing ? keep_created : std::optional{today};  // only new ones get a date
            if (s.done && !was) s.completed = today;
            if (!s.done) s.completed.reset();
            if (s.id.empty()) s.id = rem::new_id();
            s.source_line.reset();
            r.subtasks.push_back(std::move(s));
        }
    }

    store.touch(id);
    if (dest != ref->list) store.move_to_list(id, *dest);
    if (!ref->parent) {
        auto now = store.find(id);
        if (now->list->doc.section_of(*now->reminder) != e.section) {
            now->list->doc.move_to_end(id, e.section);
            store.save(*now->list);
        }
    }
    if (e.fields.done != was_done) store.set_done(id, e.fields.done, today);
}

std::optional<std::string> run_editor(const std::string& text) {
    std::string editor;
    for (auto* var : {"VISUAL", "EDITOR"})
        if (const char* v = std::getenv(var); v && *v) {
            editor = v;
            break;
        }
    if (editor.empty()) editor = std::system("command -v nano >/dev/null 2>&1") == 0 ? "nano" : "vi";

    auto path = fs::temp_directory_path() / std::format("reminder-{}.yaml", rem::new_id());
    { std::ofstream(path) << text; }
    int status = std::system(std::format("{} '{}'", editor, path.string()).c_str());
    std::optional<std::string> out;
    if (status == 0) {
        std::ifstream in(path);
        out = std::string((std::istreambuf_iterator<char>(in)), {});
    }
    std::error_code ec;
    fs::remove(path, ec);
    return out;
}

namespace {

// After a bad edit: true to edit again, false to revert.
bool ask_edit_again(const std::string& problem) {
    std::cerr << "\nThat edit has a problem: " << problem << "\n";
    while (true) {
        std::cerr << "Edit it again, or revert to how it was? [E/r] " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) return false;
        auto a = term::lower(trim(answer));
        if (a.empty() || a == "e" || a == "edit") return true;
        if (a == "r" || a == "revert") return false;
    }
}

}  // namespace

Outcome edit(rem::Store& store, const std::string& id,
             const std::function<void(const std::function<void()>&)>& apply_fn) {
    auto ref = store.find(id);
    if (!ref) return Outcome::Unchanged;
    auto today = rem::local_today();
    std::vector<std::string> lists;
    for (auto* l : store.lists()) lists.push_back(l->name);

    auto original = render(*ref, lists, today);
    auto text = original;
    while (true) {
        auto edited = run_editor(text);
        if (!edited || *edited == original) return Outcome::Unchanged;  // editor failed, or nothing changed
        try {
            auto e = parse(*edited, today);
            if (!e) return Outcome::Unchanged;  // emptied: cancel
            if (apply_fn) apply_fn([&] { apply(store, id, *e, today); });
            else apply(store, id, *e, today);
            return Outcome::Saved;
        } catch (const std::exception& err) {
            if (!ask_edit_again(err.what())) return Outcome::Reverted;
            // Back to the editor with their text, and the problem noted at the top.
            std::string kept;
            for (auto& l : split_lines(*edited))
                if (!l.starts_with("# Error:")) kept += l + "\n";
            text = std::format("# Error: {}\n{}", err.what(), kept);
        }
    }
}

}  // namespace editfile
