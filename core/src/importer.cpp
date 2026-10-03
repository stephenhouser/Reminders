#include "reminders/importer.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>

#include "file_util.hpp"
#include "reminders/caldav.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/recurrence.hpp"
#include "reminders/vtodo.hpp"

namespace rem {

namespace {

bool usable_id(std::string_view s) {
    return s.size() >= 6 && std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string_view without_bom(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return text;
}

// "2026-10-03" exactly.
std::optional<Date> iso_date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    return parse_date(s);
}

bool truthy(std::string_view s) {
    auto v = lower(trim(s));
    for (auto t : {"x", "yes", "y", "true", "1", "done", "completed", "complete", "checked"})
        if (v == t) return true;
    return false;
}

void add_tag(Reminder& r, std::string_view raw) {
    while (!raw.empty() && (raw.front() == '#' || raw.front() == '@' || raw.front() == '+')) raw.remove_prefix(1);
    auto tag = tag_from_category(raw);
    if (!tag.empty() && std::ranges::find(r.tags, tag) == r.tags.end()) r.tags.push_back(tag);
}

// todo.txt's rec: ("1w", "+2m", "b") as a repeat rule.
std::optional<std::string> rule_from_rec(std::string_view rec) {
    if (rec.starts_with('+')) rec.remove_prefix(1);
    if (rec.empty()) return std::nullopt;
    int n = 1;
    if (rec.size() > 1) {
        auto [p, ec] = std::from_chars(rec.data(), rec.data() + rec.size() - 1, n);
        if (ec != std::errc{} || p != rec.data() + rec.size() - 1 || n < 1) return std::nullopt;
    }
    std::string_view unit;
    switch (std::tolower(static_cast<unsigned char>(rec.back()))) {
    case 'd': unit = "day"; break;
    case 'w': unit = "week"; break;
    case 'm': unit = "month"; break;
    case 'y': unit = "year"; break;
    case 'b': return n == 1 ? std::optional<std::string>("every weekday") : std::nullopt;
    default: return std::nullopt;
    }
    return n == 1 ? std::format("every {}", unit) : std::format("every {} {}s", n, unit);
}

Priority priority_from_letter(char c) {
    return c == 'A' ? Priority::High : c == 'B' ? Priority::Medium : Priority::Low;
}

// A reminder read from a line or row, with the reminder it's a subtask of
// (an id, or a title), if any.
struct Pending {
    Reminder reminder;
    std::optional<std::string> section;
    std::string parent;
};

// Puts subtasks under their parents: found by id, else by title; deeper
// ones under the top parent (this app has one level). Ones whose parent
// isn't there stay top-level.
std::vector<Import::Item> nest(std::vector<Pending> rows) {
    std::map<std::string, std::size_t> by_id, by_title;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].reminder.id.empty()) by_id.emplace(rows[i].reminder.id, i);
        by_title.emplace(lower(rows[i].reminder.title), i);
    }
    auto parent_of = [&](std::size_t i) -> std::optional<std::size_t> {
        auto& p = rows[i].parent;
        if (p.empty()) return std::nullopt;
        if (auto it = by_id.find(id_for_uid(p)); it != by_id.end() && it->second != i) return it->second;
        if (auto it = by_title.find(lower(p)); it != by_title.end() && it->second != i) return it->second;
        return std::nullopt;
    };
    auto top_of = [&](std::size_t i) -> std::optional<std::size_t> {
        std::optional<std::size_t> top;
        std::set<std::size_t> seen{i};
        for (auto at = i;;) {
            auto p = parent_of(at);
            if (!p || !seen.insert(*p).second) return top;
            top = at = *p;
        }
    };
    std::vector<Import::Item> out;
    std::map<std::size_t, std::size_t> item_of;
    std::vector<std::size_t> subtasks;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (top_of(i)) {
            subtasks.push_back(i);
            continue;
        }
        item_of[i] = out.size();
        out.push_back({rows[i].reminder, rows[i].section});
    }
    for (auto i : subtasks) out[item_of.at(*top_of(i))].reminder.subtasks.push_back(rows[i].reminder);
    return out;
}

// ── CSV ─────────────────────────────────────────────────────────────────

// The delimiter the header line uses most: comma, semicolon or tab.
char csv_delimiter(std::string_view text) {
    auto line = text.substr(0, text.find('\n'));
    char best = ',';
    std::ptrdiff_t most = 0;
    for (char c : {',', ';', '\t'})
        if (auto n = std::ranges::count(line, c); n > most) most = n, best = c;
    return best;
}

std::vector<std::vector<std::string>> parse_csv(std::string_view text, char delim) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') field += '"', ++i;
            else if (c == '"') quoted = false;
            else field += c;
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = any = true;
        } else if (c == delim) {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !field.empty()) {
                row.push_back(std::move(field));
                rows.push_back(std::move(row));
            }
            row.clear();
            field.clear();
            any = false;
        } else {
            field += c;
            any = true;
        }
    }
    if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

// What a CSV column holds, from its header ("Due Date", "due_date", "DUE").
enum class Col { None, Title, Done, Completed, Due, Time, Priority, Flagged, Tags, Repeat, Url, Notes, Section, Created, Id, Parent };

Col column_of(std::string_view header) {
    std::string h;
    for (char c : lower(trim(header)))
        if (c != ' ' && c != '_' && c != '-') h += c;
    auto is = [&](std::initializer_list<std::string_view> names) { return std::ranges::find(names, h) != names.end(); };
    if (is({"title", "name", "task", "content", "summary", "subject", "todo", "item", "reminder"})) return Col::Title;
    if (is({"done", "status", "complete", "iscompleted", "checked", "isdone"})) return Col::Done;
    if (is({"completed", "completedat", "completeddate", "datecompleted", "completiondate"})) return Col::Completed;
    if (is({"due", "duedate", "date", "deadline", "duedatetime"})) return Col::Due;
    if (is({"duetime", "time"})) return Col::Time;
    if (is({"priority", "pri", "importance"})) return Col::Priority;
    if (is({"flagged", "flag", "starred", "star", "important"})) return Col::Flagged;
    if (is({"tags", "tag", "labels", "label", "categories", "category"})) return Col::Tags;
    if (is({"repeat", "recurrence", "recurring", "repeats", "rrule"})) return Col::Repeat;
    if (is({"url", "link", "website"})) return Col::Url;
    if (is({"notes", "note", "description", "details", "comments", "comment"})) return Col::Notes;
    if (is({"section", "heading", "group"})) return Col::Section;
    if (is({"created", "createdat", "datecreated", "createddate", "added", "dateadded"})) return Col::Created;
    if (is({"id", "uid"})) return Col::Id;
    if (is({"parent", "parentid", "parenttask"})) return Col::Parent;
    return Col::None;
}

Priority priority_from_text(std::string_view text) {
    auto v = lower(trim(text));
    for (auto t : {"high", "h", "!!!", "1", "a", "urgent", "p1"})
        if (v == t) return Priority::High;
    for (auto t : {"medium", "med", "m", "!!", "2", "b", "p2", "normal"})
        if (v == t) return Priority::Medium;
    for (auto t : {"low", "l", "!", "3", "c", "p3"})
        if (v == t) return Priority::Low;
    return Priority::None;
}

// The header's columns, if it has a title column.
std::optional<std::vector<Col>> csv_header(const std::vector<std::string>& header) {
    std::vector<Col> cols;
    for (auto& h : header) cols.push_back(column_of(h));
    if (std::ranges::find(cols, Col::Title) == cols.end()) return std::nullopt;
    return cols;
}

// ── Telling the kinds apart ─────────────────────────────────────────────

bool looks_like_todotxt(std::string_view text) {
    int lines = 0, like = 0;
    for (auto& raw : detail::split(text, '\n')) {
        auto line = trim(raw);
        if (line.empty()) continue;
        ++lines;
        bool match = (line.starts_with("x ") && iso_date(line.substr(2, 10))) ||
                     (line.size() > 4 && line[0] == '(' && std::isupper(static_cast<unsigned char>(line[1])) &&
                      line[2] == ')' && line[3] == ' ') ||
                     iso_date(line.substr(0, 10));
        for (auto& t : detail::split(line, ' ')) {
            if (match) break;
            match = (t.size() > 1 && (t[0] == '+' || t[0] == '@') && std::isalnum(static_cast<unsigned char>(t[1]))) ||
                    (t.starts_with("due:") && iso_date(t.substr(4))) || t.starts_with("rec:");
        }
        like += match;
    }
    return lines > 0 && like * 2 >= lines;
}

bool looks_like_csv(std::string_view text) {
    auto delim = csv_delimiter(text);
    auto first = text.substr(0, text.find('\n'));
    if (std::ranges::count(first, delim) == 0) return false;
    auto rows = parse_csv(first, delim);
    if (rows.empty()) return false;
    auto cols = csv_header(rows[0]);
    // A title column and another this app knows.
    return cols && std::ranges::count_if(*cols, [](Col c) { return c != Col::None && c != Col::Title; }) > 0;
}

}  // namespace

std::string id_for_uid(std::string_view uid) {
    if (usable_id(uid)) return std::string(uid);
    // Ten base-36 digits of the UID's fingerprint: the same UID, the same id.
    auto h = std::stoull(detail::fingerprint(uid), nullptr, 16);
    std::string id;
    for (int i = 0; i < 10; ++i, h /= 36) id += "abcdefghijklmnopqrstuvwxyz0123456789"[h % 36];
    return id;
}

Import read_ics(std::string_view text, const std::chrono::time_zone* local) {
    auto cal = parse_ical(text);
    if (!cal) throw std::runtime_error("that isn't an iCalendar (.ics) file");
    Import out;
    out.kind = Import::Kind::Ics;
    if (auto* p = cal->prop("X-WR-CALNAME")) out.name = ical_text(p->value);
    if (auto* p = cal->prop("X-APPLE-CALENDAR-COLOR")) out.color = color_from_hex(p->value);

    struct Task {
        Todo todo;
        std::string id;
    };
    std::vector<Task> tasks;
    for (auto& c : cal->children) {
        if (c.name == "VTIMEZONE") continue;
        if (c.name != "VTODO") {
            ++out.skipped;
            continue;
        }
        auto t = read_todo(c, local);
        auto id = id_for_uid(t.uid.empty() ? t.reminder.title : t.uid);
        tasks.push_back({std::move(t), std::move(id)});
    }
    // The calendar's order when every task has one.
    if (std::ranges::all_of(tasks, [](auto& t) { return t.todo.sort_order.has_value(); }))
        std::ranges::stable_sort(tasks, {}, [](auto& t) { return *t.todo.sort_order; });

    std::map<std::string, std::size_t> by_uid;
    for (std::size_t i = 0; i < tasks.size(); ++i)
        if (!tasks[i].todo.uid.empty()) by_uid.emplace(tasks[i].todo.uid, i);
    // The top-level task a subtask belongs under (deeper ones: the top
    // parent; this app has one level), or none.
    auto top_of = [&](std::size_t i) -> std::optional<std::size_t> {
        std::optional<std::size_t> top;
        std::set<std::size_t> seen{i};
        for (auto at = i;;) {
            auto& parent = tasks[at].todo.parent_uid;
            auto it = parent ? by_uid.find(*parent) : by_uid.end();
            if (it == by_uid.end() || !seen.insert(it->second).second) return top;
            top = at = it->second;
        }
    };

    std::map<std::size_t, std::size_t> item_of;  // task → its item
    std::vector<std::size_t> subtasks;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (top_of(i)) {
            subtasks.push_back(i);
            continue;
        }
        Reminder r = tasks[i].todo.reminder;
        r.id = tasks[i].id;
        item_of[i] = out.items.size();
        out.items.push_back({std::move(r), tasks[i].todo.section});
    }
    for (auto i : subtasks) {
        Reminder r = tasks[i].todo.reminder;
        r.id = tasks[i].id;
        out.items[item_of.at(*top_of(i))].reminder.subtasks.push_back(std::move(r));
    }
    return out;
}

Import read_markdown(std::string_view text) {
    Import out;
    out.kind = Import::Kind::Markdown;
    auto doc = parse(text);
    auto color = doc.meta("color").value_or("");
    if (std::ranges::find(kColors, color) != std::end(kColors)) out.color = color;
    for (auto& section : doc.sections())
        for (auto* r : section.reminders) out.items.push_back({*r, section.name});
    return out;
}

Import read_plain_text(std::string_view text) {
    Import out;
    std::optional<std::string> section;
    std::size_t top_indent = 0;  // the last top-level line's
    bool first = true, front_matter = false;
    for (auto& raw : detail::split(text, '\n')) {
        auto line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Front matter ("---" … "---" at the top) isn't reminders.
        if (first && line == "---") {
            first = false;
            front_matter = true;
            continue;
        }
        first = false;
        if (front_matter) {
            front_matter = line != "---";
            continue;
        }
        std::size_t indent = 0, at = 0;
        for (; at < line.size() && (line[at] == ' ' || line[at] == '\t'); ++at) indent += line[at] == '\t' ? 4 : 1;
        std::string_view rest = std::string_view(line).substr(at);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.remove_suffix(1);
        if (rest.empty()) continue;
        // "# Heading" (any level) starts a section.
        if (auto hashes = rest.find_first_not_of('#'); hashes > 0 && hashes != std::string_view::npos && rest[hashes] == ' ') {
            auto name = rest.substr(hashes + 1);
            while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
            if (!name.empty()) section = std::string(name);
            continue;
        }
        // Bullets and numbering.
        for (std::string_view bullet : {"- ", "* ", "+ ", "• "})
            if (rest.starts_with(bullet)) {
                rest.remove_prefix(bullet.size());
                break;
            }
        if (auto digits = rest.find_first_not_of("0123456789"); digits > 0 && digits != std::string_view::npos &&
                                                                 digits + 1 < rest.size() &&
                                                                 (rest[digits] == '.' || rest[digits] == ')') &&
                                                                 rest[digits + 1] == ' ')
            rest.remove_prefix(digits + 2);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (rest.empty()) continue;

        Reminder r;
        r.fields() = parse_fields(rest, &r.id);
        if (r.title.empty()) r.title = std::string(rest);
        if (indent > top_indent && !out.items.empty()) {
            out.items.back().reminder.subtasks.push_back(std::move(r));
            continue;
        }
        top_indent = indent;
        out.items.push_back({std::move(r), section});
    }
    return out;
}

Import read_todotxt(std::string_view text) {
    Import out;
    out.kind = Import::Kind::Todotxt;
    std::vector<Pending> rows;
    for (auto& raw : detail::split(without_bom(text), '\n')) {
        std::vector<std::string> tokens;
        for (auto& t : detail::split(trim(raw), ' '))
            if (!t.empty()) tokens.push_back(t);
        if (tokens.empty()) continue;
        Pending row;
        auto& r = row.reminder;
        std::size_t i = 0, n = tokens.size();
        if (tokens[0] == "x") {
            r.done = true;
            ++i;
            if (i < n && (r.completed = iso_date(tokens[i]))) {
                ++i;
                if (i < n && (r.created = iso_date(tokens[i]))) ++i;
            }
        } else {
            auto& t = tokens[0];
            if (t.size() == 3 && t[0] == '(' && t[2] == ')' && std::isupper(static_cast<unsigned char>(t[1]))) {
                r.priority = priority_from_letter(t[1]);
                ++i;
            }
            if (i < n && (r.created = iso_date(tokens[i]))) ++i;
        }
        std::string title;
        for (; i < n; ++i) {
            auto& t = tokens[i];
            if (t.size() > 1 && (t[0] == '+' || t[0] == '@')) {
                add_tag(r, t);
                continue;
            }
            if (auto colon = t.find(':'); colon != std::string::npos && colon > 0 && colon + 1 < t.size()) {
                auto key = lower(t.substr(0, colon));
                std::string_view value = std::string_view(t).substr(colon + 1);
                if (key == "due" && iso_date(value)) {
                    r.due_date = iso_date(value);
                    continue;
                }
                if (key == "time" && parse_time(value)) {
                    r.due_time = parse_time(value);
                    continue;
                }
                if (key == "rec") {
                    if (auto rule = rule_from_rec(value)) {
                        r.repeat = rule;
                        continue;
                    }
                }
                if (key == "id") {
                    r.id = id_for_uid(value);
                    continue;
                }
                if (key == "p") {
                    row.parent = std::string(value);
                    continue;
                }
                if (key == "pri" && value.size() == 1 && std::isupper(static_cast<unsigned char>(value[0]))) {
                    r.priority = priority_from_letter(value[0]);
                    continue;
                }
                if (key == "flag") {
                    r.flagged = truthy(value);
                    continue;
                }
                if (key == "url") {
                    r.url = std::string(value);
                    continue;
                }
            }
            title += (title.empty() ? "" : " ") + t;
        }
        if (title.empty()) continue;
        r.title = std::move(title);
        rows.push_back(std::move(row));
    }
    out.items = nest(std::move(rows));
    return out;
}

Import read_csv(std::string_view text) {
    text = without_bom(text);
    auto rows = parse_csv(text, csv_delimiter(text));
    std::optional<std::vector<Col>> cols;
    if (!rows.empty()) cols = csv_header(rows[0]);
    if (!cols) throw std::runtime_error("a CSV file needs a header row with a Title (or Name, Task) column");
    Import out;
    out.kind = Import::Kind::Csv;
    std::vector<Pending> pending;
    for (std::size_t at = 1; at < rows.size(); ++at) {
        Pending row;
        auto& r = row.reminder;
        auto& cells = rows[at];
        for (std::size_t c = 0; c < cells.size() && c < cols->size(); ++c) {
            auto value = trim(cells[c]);
            if (value.empty()) continue;
            switch ((*cols)[c]) {
            case Col::Title: r.title = std::string(value); break;
            case Col::Done: r.done = r.done || truthy(value); break;
            case Col::Completed:
                if (auto d = iso_date(value.substr(0, 10))) r.completed = d, r.done = true;
                else r.done = r.done || truthy(value);
                break;
            case Col::Due:
                r.due_date = iso_date(value.substr(0, 10));
                if (r.due_date && value.size() >= 16 && (value[10] == ' ' || value[10] == 'T'))
                    if (auto t = parse_time(value.substr(11, 5))) r.due_time = t;
                break;
            case Col::Time:
                if (auto t = parse_time(value.substr(0, 5))) r.due_time = t;
                break;
            case Col::Priority: r.priority = priority_from_text(value); break;
            case Col::Flagged: r.flagged = truthy(value); break;
            case Col::Tags:
                for (auto& part : detail::split(value, ','))
                    for (auto& semi : detail::split(part, ';'))
                        for (auto& word : detail::split(semi, ' '))
                            if (!word.empty()) add_tag(r, word);
                break;
            case Col::Repeat: {
                auto rule = lower(value);
                if (next_occurrence(rule, Date{std::chrono::year{2026}, std::chrono::month{1}, std::chrono::day{1}}))
                    r.repeat = rule;
                else if (auto from_rec = rule_from_rec(value))
                    r.repeat = from_rec;
                break;
            }
            case Col::Url: r.url = std::string(value); break;
            case Col::Notes: r.notes = std::string(value); break;
            case Col::Section: row.section = std::string(value); break;
            case Col::Created: r.created = iso_date(value.substr(0, 10)); break;
            case Col::Id: r.id = id_for_uid(value); break;
            case Col::Parent: row.parent = std::string(value); break;
            case Col::None: break;
            }
        }
        // Notes keep their line breaks, without Windows' \r.
        std::erase(r.notes, '\r');
        if (r.title.empty()) continue;
        if (r.due_time && !r.due_date) r.due_time.reset();
        pending.push_back(std::move(row));
    }
    out.items = nest(std::move(pending));
    return out;
}

std::string_view kind_name(Import::Kind kind) {
    switch (kind) {
    case Import::Kind::Ics: return "iCalendar";
    case Import::Kind::Markdown: return "Markdown";
    case Import::Kind::Todotxt: return "todo.txt";
    case Import::Kind::Csv: return "CSV";
    case Import::Kind::Text: break;
    }
    return "plain text";
}

Import::Kind detect_kind(std::string_view text, std::string_view file_name) {
    text = without_bom(text);
    auto start = text;
    while (!start.empty() && (start.front() == ' ' || start.front() == '\r' || start.front() == '\n'))
        start.remove_prefix(1);
    std::string head(start.substr(0, 15));
    for (auto& c : head) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (head == "BEGIN:VCALENDAR") return Import::Kind::Ics;

    auto name = lower(file_name.substr(file_name.find_last_of('/') == std::string_view::npos ? 0 : file_name.find_last_of('/') + 1));
    if (name.ends_with(".csv")) return Import::Kind::Csv;
    if (name == "todo.txt" || name == "done.txt" || name.ends_with(".todo.txt")) return Import::Kind::Todotxt;
    if (name.ends_with(".ics")) return Import::Kind::Ics;
    if (!read_markdown(text).items.empty()) return Import::Kind::Markdown;
    if (looks_like_csv(text)) return Import::Kind::Csv;
    if (!name.ends_with(".md") && !name.ends_with(".markdown") && looks_like_todotxt(text)) return Import::Kind::Todotxt;
    return Import::Kind::Text;
}

Import read_as(std::string_view text, Import::Kind kind, const std::chrono::time_zone* local) {
    text = without_bom(text);
    switch (kind) {
    case Import::Kind::Ics: {
        auto start = text;
        while (!start.empty() && (start.front() == ' ' || start.front() == '\r' || start.front() == '\n'))
            start.remove_prefix(1);
        return read_ics(start, local);
    }
    case Import::Kind::Markdown: return read_markdown(text);
    case Import::Kind::Todotxt: return read_todotxt(text);
    case Import::Kind::Csv: return read_csv(text);
    case Import::Kind::Text: break;
    }
    return read_plain_text(text);
}

Import read_import(std::string_view text, const std::chrono::time_zone* local, std::string_view file_name) {
    return read_as(text, detect_kind(text, file_name), local);
}

int reminder_count(const Import& import) {
    int n = 0;
    for (auto& item : import.items) n += 1 + static_cast<int>(item.reminder.subtasks.size());
    return n;
}

ImportResult import_into(Library& library, ListFile& list, const Import& import) {
    ImportResult result;
    std::set<std::string> used;  // ids given out by this import
    auto taken = [&](const std::string& id) { return used.contains(id) || library.find(id).has_value(); };
    // Reminders without ids (plain text, hand-written Markdown) are matched
    // by title instead: one the list has open already isn't added again.
    auto key = [](std::string_view title) {
        std::string k(title);
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return k;
    };
    std::set<std::string> open_titles;
    for (auto& section : list.doc.sections())
        for (auto* r : section.reminders)
            if (!r->done) open_titles.insert(key(r->title));
    auto give_id = [&](Reminder& r) {
        while (r.id.empty() || taken(r.id)) r.id = new_id();
        used.insert(r.id);
    };
    for (auto& item : import.items) {
        auto count = 1 + static_cast<int>(item.reminder.subtasks.size());
        if (item.reminder.id.empty() ? !item.reminder.done && open_titles.contains(key(item.reminder.title))
                                     : taken(item.reminder.id)) {
            result.already += count;
            continue;
        }
        Reminder r = item.reminder;
        give_id(r);
        // A subtask without an id, or whose id is taken (it was moved out,
        // say), gets a new one.
        for (auto& s : r.subtasks) give_id(s);
        list.doc.insert(std::move(r), nullptr, item.section);
        result.added += count;
    }
    if (result.added > 0) library.save(list);
    return result;
}

}  // namespace rem
