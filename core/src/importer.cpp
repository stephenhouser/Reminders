#include "reminders/importer.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>

#include "file_util.hpp"
#include "reminders/caldav.hpp"
#include "reminders/format.hpp"
#include "reminders/ical.hpp"
#include "reminders/vtodo.hpp"

namespace rem {

namespace {

bool usable_id(std::string_view s) {
    return s.size() >= 6 && std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
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

Import read_import(std::string_view text, const std::chrono::time_zone* local) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // a byte order mark
    auto start = text;
    while (!start.empty() && (start.front() == ' ' || start.front() == '\r' || start.front() == '\n'))
        start.remove_prefix(1);
    std::string head(start.substr(0, 15));
    for (auto& c : head) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (head == "BEGIN:VCALENDAR") return read_ics(start, local);
    auto md = read_markdown(text);
    if (!md.items.empty()) return md;
    return read_plain_text(text);
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
