#include "reminders/model.hpp"

#include <algorithm>
#include <random>

namespace rem {

bool Reminder::same_content(const Reminder& o) const {
    if (fields() != o.fields() || notes != o.notes ||
        subtasks.size() != o.subtasks.size())
        return false;
    for (std::size_t i = 0; i < subtasks.size(); ++i)
        if (!subtasks[i].same_content(o.subtasks[i])) return false;
    return true;
}

std::optional<std::string> RawLine::section() const {
    if (text.starts_with("## ")) {
        auto s = text.substr(3);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    }
    return std::nullopt;
}

std::string new_id() {
    static constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string id(6, ' ');
    for (auto& c : id) c = alphabet[pick(rng)];
    return id;
}

std::optional<std::string> Document::meta(std::string_view key) const {
    for (auto& [k, v] : front)
        if (k == key) {
            auto s = v;
            auto b = s.find_first_not_of(" \t\n");
            auto e = s.find_last_not_of(" \t\n");
            return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
        }
    return std::nullopt;
}

void Document::set_meta(std::string_view key, std::optional<std::string> value) {
    auto it = std::ranges::find_if(front, [&](auto& kv) { return kv.first == key; });
    if (it != front.end()) {
        if (value) it->second = *value;
        else front.erase(it);
    } else if (value) {
        front.emplace_back(std::string(key), *value);
    }
    has_front = has_front || !front.empty();
}

void Document::mark_as_list() {
    if (is_list()) return;
    front.emplace(front.begin(), std::string(kMarker), std::string(kFormatVersion));
    has_front = true;
}

std::vector<Reminder*> Document::reminders() {
    std::vector<Reminder*> out;
    for (auto& b : blocks)
        if (auto* r = std::get_if<Reminder>(&b)) out.push_back(r);
    return out;
}

std::vector<Section> Document::sections() {
    std::vector<Section> out{Section{}};
    for (auto& b : blocks) {
        if (auto* raw = std::get_if<RawLine>(&b)) {
            if (auto name = raw->section()) out.push_back(Section{name, {}});
        } else {
            out.back().reminders.push_back(&std::get<Reminder>(b));
        }
    }
    if (out.size() > 1 && out.front().reminders.empty()) out.erase(out.begin());
    return out;
}

Reminder* Document::find(std::string_view id, Reminder** parent) {
    for (auto& b : blocks) {
        auto* r = std::get_if<Reminder>(&b);
        if (!r) continue;
        if (r->id == id) {
            if (parent) *parent = nullptr;
            return r;
        }
        for (auto& s : r->subtasks)
            if (s.id == id) {
                if (parent) *parent = r;
                return &s;
            }
    }
    return nullptr;
}

std::optional<std::string> Document::section_of(const Reminder& target) const {
    std::optional<std::string> current;
    for (auto& b : blocks) {
        if (auto* raw = std::get_if<RawLine>(&b)) {
            if (auto name = raw->section()) current = name;
        } else {
            auto& r = std::get<Reminder>(b);
            if (&r == &target) return current;
            for (auto& s : r.subtasks)
                if (&s == &target) return current;
        }
    }
    return std::nullopt;
}

void Document::ensure_ids(std::vector<std::string>& taken) {
    walk([&](Reminder& r, Reminder*) {
        if (!r.id.empty()) taken.push_back(r.id);
    });
    walk([&](Reminder& r, Reminder*) {
        if (!r.id.empty()) return;
        do r.id = new_id();
        while (std::ranges::find(taken, r.id) != taken.end());
        taken.push_back(r.id);
    });
}

static bool is_blank(const Block& b) {
    auto* raw = std::get_if<RawLine>(&b);
    return raw && raw->text.find_first_not_of(" \t") == std::string::npos;
}

std::size_t Document::section_end(const std::optional<std::string>& section) {
    std::size_t start = 0, end = blocks.size();
    if (section) {
        auto it = std::ranges::find_if(blocks, [&](const Block& b) {
            auto* raw = std::get_if<RawLine>(&b);
            return raw && raw->section() == section;
        });
        if (it == blocks.end()) {
            if (!blocks.empty() && !is_blank(blocks.back())) blocks.push_back(RawLine{""});
            blocks.push_back(RawLine{"## " + *section});
            return blocks.size();
        }
        start = static_cast<std::size_t>(it - blocks.begin()) + 1;
    }
    for (auto i = start; i < blocks.size(); ++i) {
        auto* raw = std::get_if<RawLine>(&blocks[i]);
        if (raw && raw->section()) {
            end = i;
            break;
        }
    }
    std::optional<std::size_t> last;
    for (auto i = start; i < end; ++i)
        if (std::holds_alternative<Reminder>(blocks[i])) last = i;
    if (last) return *last + 1;
    // Empty section: go after any blank lines below its heading. The empty
    // top area starts at the very top, keeping blank lines before a heading.
    if (!section) return start;
    auto i = start;
    while (i < end && is_blank(blocks[i])) ++i;
    return i;
}

Reminder& Document::insert(Reminder r, const Reminder* anchor,
                           const std::optional<std::string>& section) {
    if (anchor) {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            auto* top = std::get_if<Reminder>(&blocks[i]);
            if (!top) continue;
            if (top == anchor) {
                blocks.insert(blocks.begin() + static_cast<long>(i) + 1, std::move(r));
                return std::get<Reminder>(blocks[i + 1]);
            }
            for (std::size_t j = 0; j < top->subtasks.size(); ++j)
                if (&top->subtasks[j] == anchor) {
                    auto it = top->subtasks.insert(
                        top->subtasks.begin() + static_cast<long>(j) + 1, std::move(r));
                    return *it;
                }
        }
    }
    auto at = section_end(section);
    blocks.insert(blocks.begin() + static_cast<long>(at), std::move(r));
    return std::get<Reminder>(blocks[at]);
}

std::optional<Reminder> Document::remove(std::string_view id) {
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
        auto* r = std::get_if<Reminder>(&*it);
        if (!r) continue;
        if (r->id == id) {
            Reminder out = std::move(*r);
            blocks.erase(it);
            return out;
        }
        for (auto s = r->subtasks.begin(); s != r->subtasks.end(); ++s)
            if (s->id == id) {
                Reminder out = std::move(*s);
                r->subtasks.erase(s);
                return out;
            }
    }
    return std::nullopt;
}

bool Document::move_before(std::string_view id, std::optional<std::string_view> before_id) {
    Reminder* parent = nullptr;
    Reminder* r = find(id, &parent);
    if (!r) return false;
    if (before_id && *before_id == id) return true;

    if (parent) {
        auto& subs = parent->subtasks;
        auto from = std::ranges::find_if(subs, [&](auto& s) { return s.id == id; });
        Reminder moving = std::move(*from);
        subs.erase(from);
        auto to = before_id ? std::ranges::find_if(subs, [&](auto& s) { return s.id == *before_id; })
                            : subs.end();
        subs.insert(to, std::move(moving));
        return true;
    }

    auto section = section_of(*r);
    auto moving = remove(id);
    if (before_id) {
        for (std::size_t i = 0; i < blocks.size(); ++i)
            if (auto* b = std::get_if<Reminder>(&blocks[i]); b && b->id == *before_id) {
                blocks.insert(blocks.begin() + static_cast<long>(i), std::move(*moving));
                return true;
            }
    }
    blocks.insert(blocks.begin() + static_cast<long>(section_end(section)), std::move(*moving));
    return true;
}

bool Document::move_next_to(std::string_view id, std::string_view target, Place place) {
    if (id == target) return false;
    Reminder* parent = nullptr;
    Reminder* r = find(id, &parent);
    Reminder* target_parent = nullptr;
    Reminder* t = find(target, &target_parent);
    if (!r || !t) return false;
    if (target_parent) {
        if (target_parent == r) return false;          // into its own subtasks
        if (!r->subtasks.empty()) return false;        // would nest two levels
    }

    std::string target_parent_id = target_parent ? target_parent->id : "";
    auto moving = remove(id);  // invalidates pointers into blocks

    if (!target_parent_id.empty()) {
        auto& subs = find(target_parent_id)->subtasks;
        auto at = std::ranges::find_if(subs, [&](auto& s) { return s.id == target; });
        if (place == Place::After) ++at;
        subs.insert(at, std::move(*moving));
        return true;
    }
    for (std::size_t i = 0; i < blocks.size(); ++i)
        if (auto* b = std::get_if<Reminder>(&blocks[i]); b && b->id == target) {
            auto at = static_cast<long>(place == Place::After ? i + 1 : i);
            blocks.insert(blocks.begin() + at, std::move(*moving));
            return true;
        }
    return false;  // unreachable: target was found above
}

bool Document::move_step(std::string_view id, bool up,
                         const std::function<bool(const Reminder&)>& visible) {
    Reminder* parent = nullptr;
    Reminder* r = find(id, &parent);
    if (!r) return false;

    if (parent) {
        auto& subs = parent->subtasks;
        auto at = std::ranges::find_if(subs, [&](auto& s) { return s.id == id; }) - subs.begin();
        for (auto i = at + (up ? -1 : 1); i >= 0 && i < static_cast<long>(subs.size()); i += up ? -1 : 1)
            if (visible(subs[static_cast<std::size_t>(i)]))
                return move_next_to(id, std::string(subs[static_cast<std::size_t>(i)].id),
                                    up ? Place::Before : Place::After);
        return false;
    }

    // Top level: walk the blocks, noting whether a section heading is crossed.
    auto at = std::ranges::find_if(blocks, [&](const Block& b) {
        auto* x = std::get_if<Reminder>(&b);
        return x && x->id == id;
    }) - blocks.begin();
    bool crossed = false;
    for (auto i = at + (up ? -1 : 1); i >= 0 && i < static_cast<long>(blocks.size()); i += up ? -1 : 1) {
        auto& b = blocks[static_cast<std::size_t>(i)];
        if (auto* raw = std::get_if<RawLine>(&b)) {
            crossed = crossed || raw->section().has_value();
            continue;
        }
        auto& other = std::get<Reminder>(b);
        if (!visible(other)) continue;
        // Within a section, swap places; across a heading, land at the near
        // edge of the other section.
        auto place = up == crossed ? Place::After : Place::Before;
        return move_next_to(id, std::string(other.id), place);
    }
    if (crossed) {
        // Only hidden reminders (or none) on the other side of a heading.
        // The area above the first heading counts as a section even when empty.
        std::vector<std::optional<std::string>> names{std::nullopt};
        for (auto& b : blocks)
            if (auto* raw = std::get_if<RawLine>(&b); raw && raw->section()) names.push_back(raw->section());
        auto here = std::ranges::find(names, section_of(*r)) - names.begin();
        auto other = here + (up ? -1 : 1);
        if (other < 0 || other >= static_cast<long>(names.size())) return false;
        auto name = names[static_cast<std::size_t>(other)];
        if (up) return move_to_end(id, name);
        auto moving = remove(id);
        // Top of the next section: right after its heading.
        for (std::size_t i = 0; i < blocks.size(); ++i)
            if (auto* raw = std::get_if<RawLine>(&blocks[i]); raw && raw->section() == name) {
                blocks.insert(blocks.begin() + static_cast<long>(i) + 1, std::move(*moving));
                return true;
            }
    }
    return false;
}

bool Document::indent(std::string_view id, const std::function<bool(const Reminder&)>& visible) {
    Reminder* parent = nullptr;
    Reminder* r = find(id, &parent);
    if (!r || parent || !r->subtasks.empty()) return false;
    std::string above;
    for (auto& b : blocks) {
        if (auto* raw = std::get_if<RawLine>(&b); raw && raw->section()) above.clear();
        auto* x = std::get_if<Reminder>(&b);
        if (!x) continue;
        if (x == r) break;
        if (visible(*x)) above = x->id;
    }
    if (above.empty()) return false;
    auto moving = remove(id);
    find(above)->subtasks.push_back(std::move(*moving));
    return true;
}

bool Document::outdent(std::string_view id) {
    Reminder* parent = nullptr;
    if (!find(id, &parent) || !parent) return false;
    std::string parent_id = parent->id;
    auto moving = remove(id);
    insert(std::move(*moving), find(parent_id));
    return true;
}

namespace {

std::ptrdiff_t heading_index(const std::vector<Block>& blocks, const std::string& name) {
    for (std::size_t i = 0; i < blocks.size(); ++i)
        if (auto* raw = std::get_if<RawLine>(&blocks[i]); raw && raw->section() == name)
            return static_cast<std::ptrdiff_t>(i);
    return -1;
}

}  // namespace

bool Document::rename_section(const std::string& name, const std::string& new_name) {
    auto at = heading_index(blocks, name);
    if (at < 0 || new_name.empty() || new_name.find('\n') != std::string::npos ||
        heading_index(blocks, new_name) >= 0)
        return false;
    std::get<RawLine>(blocks[static_cast<std::size_t>(at)]).text = "## " + new_name;
    return true;
}

bool Document::delete_section(const std::string& name, bool keep_reminders) {
    auto at = heading_index(blocks, name);
    if (at < 0) return false;
    auto begin = blocks.begin() + at;
    if (keep_reminders) {
        // Drop the heading and the blank line separating it from the section above.
        if (at > 0 && is_blank(blocks[static_cast<std::size_t>(at) - 1])) --begin;
        blocks.erase(begin, blocks.begin() + at + 1);
        return true;
    }
    // Everything up to the next heading, plus the blank line above this one;
    // the blank line before the next heading stays as its separator.
    auto end = std::find_if(begin + 1, blocks.end(), [](const Block& b) {
        auto* raw = std::get_if<RawLine>(&b);
        return raw && raw->section();
    });
    if (end != blocks.end() && end - 1 > begin && is_blank(*(end - 1))) --end;
    if (at > 0 && is_blank(blocks[static_cast<std::size_t>(at) - 1])) --begin;
    blocks.erase(begin, end);
    return true;
}

bool Document::move_to_end(std::string_view id, const std::optional<std::string>& section) {
    auto moving = remove(id);
    if (!moving) return false;
    insert(std::move(*moving), nullptr, section);
    return true;
}

}  // namespace rem
