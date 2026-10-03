#include "reminders/ics_import.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "file_util.hpp"
#include "reminders/caldav.hpp"
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

IcsImport read_ics(std::string_view text, const std::chrono::time_zone* local) {
    auto cal = parse_ical(text);
    if (!cal) throw std::runtime_error("that isn't an iCalendar (.ics) file");
    IcsImport out;
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

int reminder_count(const IcsImport& import) {
    int n = 0;
    for (auto& item : import.items) n += 1 + static_cast<int>(item.reminder.subtasks.size());
    return n;
}

ImportResult import_into(Library& library, ListFile& list, const IcsImport& import) {
    ImportResult result;
    std::set<std::string> used;  // ids given out by this import
    auto taken = [&](const std::string& id) { return used.contains(id) || library.find(id).has_value(); };
    for (auto& item : import.items) {
        auto count = 1 + static_cast<int>(item.reminder.subtasks.size());
        if (taken(item.reminder.id)) {
            result.already += count;
            continue;
        }
        Reminder r = item.reminder;
        used.insert(r.id);
        // A subtask whose id is taken (it was moved out, say) gets a new one.
        for (auto& s : r.subtasks) {
            while (taken(s.id)) s.id = new_id();
            used.insert(s.id);
        }
        list.doc.insert(std::move(r), nullptr, item.section);
        result.added += count;
    }
    if (result.added > 0) library.save(list);
    return result;
}

}  // namespace rem
