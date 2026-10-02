#include "reminders/merge.hpp"

#include <algorithm>
#include <string>

namespace rem {

namespace {

std::string key(const Reminder& r) { return r.id.empty() ? "t:" + r.title : r.id; }

const Reminder* find_key(const std::vector<Block>& v, const std::string& k) {
    for (auto& b : v)
        if (auto* r = std::get_if<Reminder>(&b); r && key(*r) == k) return r;
    return nullptr;
}

std::ptrdiff_t index_of_key(const std::vector<Block>& v, const std::string& k) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (auto* r = std::get_if<Reminder>(&v[i]); r && key(*r) == k)
            return static_cast<std::ptrdiff_t>(i);
    return -1;
}

std::ptrdiff_t index_of_heading(const std::vector<Block>& v, const std::string& name) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (auto* raw = std::get_if<RawLine>(&v[i]); raw && raw->section() == name)
            return static_cast<std::ptrdiff_t>(i);
    return -1;
}

// Three-way pick for one value: keep main's unless only the conflict side changed it.
template <class T>
void pick(T& m, const T& c, const T* b) {
    if (b && m == *b) m = c;
}

std::vector<Block> as_blocks(const std::vector<Reminder>& v) {
    return {v.begin(), v.end()};
}

std::vector<Reminder> as_reminders(std::vector<Block>&& v) {
    std::vector<Reminder> out;
    for (auto& b : v)
        if (auto* r = std::get_if<Reminder>(&b)) out.push_back(std::move(*r));
    return out;
}

void merge_blocks(std::vector<Block>& m, const std::vector<Block>& c, const std::vector<Block>* b);

void merge_reminder(Reminder& m, const Reminder& c, const Reminder* b) {
    pick(m.title, c.title, b ? &b->title : nullptr);
    pick(m.done, c.done, b ? &b->done : nullptr);
    pick(m.tags, c.tags, b ? &b->tags : nullptr);
    pick(m.priority, c.priority, b ? &b->priority : nullptr);
    pick(m.flagged, c.flagged, b ? &b->flagged : nullptr);
    pick(m.repeat, c.repeat, b ? &b->repeat : nullptr);
    pick(m.due_date, c.due_date, b ? &b->due_date : nullptr);
    pick(m.due_time, c.due_time, b ? &b->due_time : nullptr);
    pick(m.completed, c.completed, b ? &b->completed : nullptr);
    pick(m.created, c.created, b ? &b->created : nullptr);
    pick(m.url, c.url, b ? &b->url : nullptr);
    pick(m.notes, c.notes, b ? &b->notes : nullptr);
    if (m.id.empty()) m.id = c.id;

    auto subs = as_blocks(m.subtasks);
    auto base_subs = b ? std::optional{as_blocks(b->subtasks)} : std::nullopt;
    merge_blocks(subs, as_blocks(c.subtasks), base_subs ? &*base_subs : nullptr);
    m.subtasks = as_reminders(std::move(subs));
}

// Where to put a reminder that only exists in the conflict copy: after the
// nearest earlier reminder that main also has, else at the top of its
// section, else before main's first reminder.
std::size_t insert_position(std::vector<Block>& m, const std::vector<Block>& c, std::size_t ci) {
    for (auto i = ci; i-- > 0;) {
        if (auto* raw = std::get_if<RawLine>(&c[i])) {
            if (auto name = raw->section()) {
                auto h = index_of_heading(m, *name);
                if (h < 0) {
                    m.push_back(RawLine{""});
                    m.push_back(RawLine{"## " + *name});
                    return m.size();
                }
                return static_cast<std::size_t>(h) + 1;
            }
            continue;
        }
        auto at = index_of_key(m, key(std::get<Reminder>(c[i])));
        if (at >= 0) return static_cast<std::size_t>(at) + 1;
    }
    auto first = std::ranges::find_if(m, [](const Block& b) {
        auto* raw = std::get_if<RawLine>(&b);
        return !raw || raw->section();
    });
    return static_cast<std::size_t>(first - m.begin());
}

void merge_blocks(std::vector<Block>& m, const std::vector<Block>& c, const std::vector<Block>* b) {
    // Reminders main has: merge with the conflict side, or drop if the
    // conflict side deleted them and main didn't touch them.
    for (auto it = m.begin(); it != m.end();) {
        auto* mr = std::get_if<Reminder>(&*it);
        if (!mr) {
            ++it;
            continue;
        }
        auto k = key(*mr);
        auto* cr = find_key(c, k);
        auto* br = b ? find_key(*b, k) : nullptr;
        if (cr) {
            merge_reminder(*mr, *cr, br);
            ++it;
        } else if (br && mr->same_content(*br)) {
            it = m.erase(it);
        } else {
            ++it;
        }
    }
    // Reminders only the conflict copy has: new ones are added, ones main
    // deleted stay deleted unless the conflict side changed them.
    for (std::size_t ci = 0; ci < c.size(); ++ci) {
        auto* cr = std::get_if<Reminder>(&c[ci]);
        if (!cr) continue;
        auto k = key(*cr);
        if (find_key(m, k)) continue;
        auto* br = b ? find_key(*b, k) : nullptr;
        if (br && cr->same_content(*br)) continue;
        auto at = insert_position(m, c, ci);
        m.insert(m.begin() + static_cast<std::ptrdiff_t>(at), *cr);
    }
}

}  // namespace

Document merge(const Document& main, const Document& conflict, const Document* base) {
    Document out = main;

    for (auto& [k, v] : conflict.front) {
        if (k.empty()) continue;
        auto bv = base ? base->meta(k) : std::nullopt;
        auto mv = main.meta(k);
        if (!mv) {
            if (!bv || *bv != conflict.meta(k)) out.set_meta(k, v);  // new, or changed after main deleted it
        } else if (bv && *mv == *bv) {
            out.set_meta(k, v);
        }
    }
    if (base) {
        for (auto& [k, v] : base->front)
            if (!k.empty() && !conflict.meta(k) && main.meta(k) == base->meta(k)) out.set_meta(k, std::nullopt);
    }

    merge_blocks(out.blocks, conflict.blocks, base ? &base->blocks : nullptr);
    return out;
}

}  // namespace rem
