#include "reminders/store.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <sstream>

#include "reminders/format.hpp"
#include "reminders/merge.hpp"
#include "reminders/recurrence.hpp"

namespace rem {

namespace {

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_atomic(const fs::path& p, const std::string& text) {
    auto tmp = p.parent_path() / ("." + p.filename().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) throw fs::filesystem_error("write failed", tmp, std::make_error_code(std::errc::io_error));
    }
    fs::rename(tmp, p);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

template <std::size_t N>
std::string one_of(std::optional<std::string> v, const std::string_view (&allowed)[N],
                   std::string_view fallback) {
    if (v && std::ranges::find(allowed, *v) != std::end(allowed)) return *v;
    return std::string(fallback);
}

// Minutes since midnight, or -1 for all-day reminders so they sort first.
int due_minutes(const Reminder& r) {
    return r.due_time ? r.due_time->hour * 60 + r.due_time->minute : -1;
}

// Fresh open copy for the next occurrence of a repeating reminder.
Reminder next_copy(const Reminder& r, Date due, std::vector<std::string>& taken) {
    Reminder copy = r;
    auto reset = [&](Reminder& x) {
        do x.id = new_id();
        while (std::ranges::find(taken, x.id) != taken.end());
        taken.push_back(x.id);
        x.done = false;
        x.completed.reset();
        x.source_line.reset();
    };
    reset(copy);
    copy.due_date = due;
    for (auto& s : copy.subtasks) reset(s);
    return copy;
}

}  // namespace

std::string ListFile::color() const { return one_of(doc.meta("color"), kColors, "blue"); }
std::string ListFile::icon() const { return one_of(doc.meta("icon"), kIcons, "list"); }

std::optional<int> ListFile::order() const {
    auto v = doc.meta("order");
    if (!v) return std::nullopt;
    int n = 0;
    auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), n);
    if (ec != std::errc{} || p != v->data() + v->size()) return std::nullopt;
    return n;
}

Store::Store(fs::path folder, fs::path state_dir, BackendKind backend)
    : folder_(std::move(folder)), state_dir_(std::move(state_dir)), backend_(make_backend(backend, state_dir_)) {}

fs::path Store::path_of(std::string_view name) const {
    return folder_ / (std::string(name) + ".md");
}

std::vector<std::string> Store::taken_ids() {
    std::vector<std::string> taken = other_ids_ ? other_ids_() : std::vector<std::string>{};
    for (auto& l : lists_)
        l->doc.walk([&](Reminder& r, Reminder*) {
            if (!r.id.empty()) taken.push_back(r.id);
        });
    return taken;
}

void Store::load_all() {
    std::vector<std::string> names;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(folder_, ec)) {
        if (!e.is_regular_file()) continue;
        if (auto n = backend_->list_name_for(e.path()); n && std::ranges::find(names, *n) == names.end())
            names.push_back(*n);
    }
    std::erase_if(lists_, [&](auto& l) { return std::ranges::find(names, l->name) == names.end(); });
    candidates_.clear();
    for (auto& n : names) reload(n);
}

bool Store::forget(const std::string& name) {
    auto before = lists_.size();
    std::erase_if(lists_, [&](auto& l) { return l->name == name; });
    return lists_.size() != before;
}

void Store::update_candidate(const std::string& name, const std::optional<std::string>& text) {
    std::erase(candidates_, name);
    if (!text) return;
    auto no = declined();
    if (std::ranges::find(no, name) != no.end()) return;
    if (parse(*text).reminders().empty()) return;  // no checklist: just a note
    candidates_.insert(std::ranges::upper_bound(candidates_, name), name);
}

std::vector<std::string> Store::declined() const {
    std::vector<std::string> out;
    std::ifstream in(state_dir_ / "declined.txt");
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) out.push_back(line);
    return out;
}

void Store::decline(const std::string& name) {
    fs::create_directories(state_dir_);
    std::ofstream(state_dir_ / "declined.txt", std::ios::app) << name << "\n";
    std::erase(candidates_, name);
}

void Store::adopt(const std::string& name) {
    auto text = read_file(path_of(name));
    if (!text) return;
    auto doc = parse(*text);
    doc.mark_as_list();
    write_atomic(path_of(name), serialize(doc, false));
    std::erase(candidates_, name);
    reload(name);
}

bool Store::reload(const std::string& name) {
    auto path = path_of(name);
    auto conflicts = backend_->conflict_copies(folder_, name);
    auto text = read_file(path);

    // Only Markdown files carrying the marker are lists. A conflict copy can
    // carry it when the marker was added on one side.
    auto marked = [](const std::optional<std::string>& t) { return t && parse(*t).is_list(); };
    std::erase_if(conflicts, [&](auto& c) { return !marked(read_file(c)) && !marked(text); });

    if (!text) {
        if (conflicts.empty()) {
            // Deleted (here or on another device): its state goes too.
            std::erase(candidates_, name);
            backend_->drop_state(name);
            return forget(name);
        }
        // Only a conflict copy is left: promote it.
        fs::rename(conflicts.front(), path);
        conflicts.erase(conflicts.begin());
        text = read_file(path);
        if (!text) return false;
    }
    if (conflicts.empty() && !marked(text)) {
        update_candidate(name, text);
        return forget(name);
    }
    std::erase(candidates_, name);

    auto* lf = list(name);
    if (lf && conflicts.empty() && *text == lf->disk_text) return false;  // our own write coming back

    // Other lists' ids, so a fresh id is unique across the folder.
    if (lf) lf->doc = Document{};
    auto taken = taken_ids();
    auto ensure_list = [&] {
        if (!lf) {
            lists_.push_back(std::make_unique<ListFile>());
            lf = lists_.back().get();
            lf->name = name;
        }
    };

    if (!conflicts.empty()) {
        auto base_text = backend_->read_base(name);
        auto base = base_text ? std::optional{parse(*base_text)} : std::nullopt;
        auto doc = parse(*text);
        for (auto& c : conflicts)
            if (auto ct = read_file(c)) doc = merge(doc, parse(*ct), base ? &*base : nullptr);
        doc.ensure_ids(taken);
        auto merged = serialize(doc);
        write_atomic(path, merged);
        for (auto& c : conflicts) fs::remove(c);
        backend_->write_base(name, merged);
        backend_->remember_written(name, merged);
        if (!doc.is_list()) {  // the marker was removed on one side
            update_candidate(name, merged);
            return forget(name);
        }
        ensure_list();
        lf->doc = parse(merged);
        lf->disk_text = merged;
        return true;
    }

    // A version we didn't write ourselves is the latest one shared with the
    // other devices: it becomes the base for future merges.
    if (!backend_->is_own_write(name, *text)) backend_->write_base(name, *text);
    ensure_list();
    lf->doc = parse(*text);
    lf->doc.ensure_ids(taken);  // in memory only; written with the next save
    lf->disk_text = *text;
    return true;
}

std::vector<ListFile*> Store::lists() {
    std::vector<ListFile*> out;
    for (auto& l : lists_) out.push_back(l.get());
    std::ranges::stable_sort(out, [](ListFile* a, ListFile* b) {
        auto oa = a->order(), ob = b->order();
        if (oa.has_value() != ob.has_value()) return oa.has_value();
        if (oa && *oa != *ob) return *oa < *ob;
        return lower(a->name) < lower(b->name);
    });
    return out;
}

ListFile* Store::list(std::string_view name) {
    for (auto& l : lists_)
        if (l->name == name) return l.get();
    return nullptr;
}

void Store::write_file(ListFile& list, const std::string& text) {
    fs::create_directories(folder_);
    write_atomic(path_of(list.name), text);
    backend_->remember_written(list.name, text);
    list.disk_text = text;
}

void Store::save(ListFile& list) {
    // Ids given to hand-written lines only get written along with a real change.
    if (serialize(list.doc, false) == list.disk_text) return;
    write_file(list, serialize(list.doc));
}

ListFile& Store::create_list(const std::string& name, std::string_view color, std::string_view icon) {
    lists_.push_back(std::make_unique<ListFile>());
    auto& lf = *lists_.back();
    lf.name = name;
    lf.doc.mark_as_list();
    lf.doc.set_meta("color", std::string(color));
    lf.doc.set_meta("icon", std::string(icon));
    save(lf);
    return lf;
}

bool Store::rename_list(ListFile& list, const std::string& new_name) {
    if (new_name.empty() || new_name == list.name || this->list(new_name) ||
        fs::exists(path_of(new_name)))
        return false;
    fs::rename(path_of(list.name), path_of(new_name));
    backend_->move_state(list.name, new_name);
    list.name = new_name;
    return true;
}

void Store::delete_list(const std::string& name) {
    std::error_code ec;
    fs::remove(path_of(name), ec);
    backend_->drop_state(name);
    std::erase_if(lists_, [&](auto& l) { return l->name == name; });
}

Snapshot Store::snapshot() const {
    Snapshot out;
    for (auto& l : lists_) out.emplace(l->name, l->disk_text);
    return out;
}

std::optional<std::string> Store::current_text(const std::string& name) const {
    for (auto& l : lists_)
        if (l->name == name) return l->disk_text;
    return std::nullopt;
}

void Store::restore(const std::string& name, const std::optional<std::string>& text) {
    if (!text) {
        delete_list(name);
        return;
    }
    auto* lf = list(name);
    if (!lf) {
        lists_.push_back(std::make_unique<ListFile>());
        lf = lists_.back().get();
        lf->name = name;
    }
    lf->doc = Document{};
    auto taken = taken_ids();
    lf->doc = parse(*text);
    lf->doc.ensure_ids(taken);
    write_file(*lf, *text);
}

std::optional<Ref> Store::find(std::string_view id) {
    for (auto& l : lists_) {
        Reminder* parent = nullptr;
        if (auto* r = l->doc.find(id, &parent)) return Ref{l.get(), r, parent};
    }
    return std::nullopt;
}

Reminder& Store::add(ListFile& list, Reminder r, const Reminder* after,
                     const std::optional<std::string>& section) {
    // New ids for it and any subtasks (a pasted reminder brings some along).
    auto taken = taken_ids();
    auto give_id = [&](Reminder& x) {
        if (!x.id.empty()) return;
        do x.id = new_id();
        while (std::ranges::find(taken, x.id) != taken.end());
        taken.push_back(x.id);
    };
    give_id(r);
    for (auto& s : r.subtasks) give_id(s);
    auto id = r.id;
    list.doc.insert(std::move(r), after, section);
    save(list);
    return *list.doc.find(id);
}

void Store::set_done(std::string_view id, bool done, Date today) {
    auto ref = find(id);
    if (!ref) return;
    auto& r = *ref->reminder;
    auto& list = *ref->list;
    if (r.done == done) return;

    std::optional<Reminder> next;
    if (done && r.repeat) {
        if (auto due = next_occurrence(*r.repeat, r.due_date.value_or(today))) {
            auto taken = taken_ids();
            next = next_copy(r, *due, taken);
            r.repeat.reset();
        }
    }
    r.done = done;
    r.completed = done ? std::optional{today} : std::nullopt;
    if (done)
        for (auto& s : r.subtasks)
            if (!s.done) {
                s.done = true;
                s.completed = today;
            }

    if (next) {
        auto next_id = next->id;
        std::string done_id(id);
        list.doc.insert(std::move(*next), &r);  // invalidates r
        list.doc.move_before(next_id, done_id);
    }
    save(list);
}

void Store::remove(std::string_view id) {
    auto ref = find(id);
    if (!ref) return;
    ref->list->doc.remove(id);
    save(*ref->list);
}

void Store::move_to_list(std::string_view id, ListFile& dest) {
    auto ref = find(id);
    if (!ref || ref->list == &dest) return;
    auto& src = *ref->list;
    auto r = src.doc.remove(id);
    dest.doc.insert(std::move(*r), nullptr);
    save(src);
    save(dest);
}

void Store::touch(std::string_view id) {
    if (auto ref = find(id)) save(*ref->list);
}

template <class Pred>
std::vector<Ref> Store::collect(Pred&& pred) {
    std::vector<Ref> out;
    for (auto* l : lists())
        l->doc.walk([&](Reminder& r, Reminder* parent) {
            if (pred(r)) out.push_back(Ref{l, &r, parent});
        });
    return out;
}

std::vector<Ref> Store::today(Date today) {
    return collect([&](Reminder& r) { return !r.done && r.due_date && *r.due_date <= today; });
}

std::vector<Ref> Store::scheduled() {
    auto out = collect([](Reminder& r) { return !r.done && r.due_date; });
    std::ranges::stable_sort(out, [](const Ref& a, const Ref& b) {
        return std::pair{*a.reminder->due_date, due_minutes(*a.reminder)} <
               std::pair{*b.reminder->due_date, due_minutes(*b.reminder)};
    });
    return out;
}

std::vector<Ref> Store::all() {
    return collect([](Reminder& r) { return !r.done; });
}

std::string count_label(CountStyle style, int total, int done) {
    auto plural = [&](const char* one, const char* many) { return std::format("{} {}", total, total == 1 ? one : many); };
    switch (style) {
        case CountStyle::OpenOnly: return plural("Reminder", "Reminders");
        case CountStyle::Completed: return std::format("{} Completed", total);
        case CountStyle::Results: return plural("Result", "Results");
        case CountStyle::WithComplete: {
            auto s = plural("Reminder", "Reminders");
            if (done > 0) s += std::format(" / {} Complete", done);
            return s;
        }
    }
    return {};
}

std::string count_short(CountStyle style, int total, int done) {
    if (style == CountStyle::WithComplete && done > 0) return std::format("{}/{}", total, done);
    return std::to_string(total);
}

std::vector<Ref> Store::everything() {
    return collect([](Reminder&) { return true; });
}

std::vector<Ref> Store::flagged() {
    return collect([](Reminder& r) { return !r.done && r.flagged; });
}

std::vector<Ref> Store::completed() {
    auto out = collect([](Reminder& r) { return r.done; });
    std::ranges::stable_sort(out, [](const Ref& a, const Ref& b) {
        return a.reminder->completed > b.reminder->completed;  // undated last
    });
    return out;
}

std::vector<Ref> Store::tagged(std::string_view tag) {
    auto t = lower(tag);
    return collect([&](Reminder& r) {
        return std::ranges::any_of(r.tags, [&](auto& x) { return lower(x) == t; });
    });
}

std::vector<Ref> Store::search(std::string_view query) {
    auto q = lower(query);
    if (q.empty()) return {};
    return collect([&](Reminder& r) {
        return lower(r.title).find(q) != std::string::npos ||
               lower(r.notes).find(q) != std::string::npos;
    });
}

std::vector<std::string> Store::tags() {
    std::vector<std::string> out;
    for (auto& l : lists_)
        l->doc.walk([&](Reminder& r, Reminder*) {
            for (auto& t : r.tags)
                if (std::ranges::find(out, t) == out.end()) out.push_back(t);
        });
    std::ranges::sort(out, [](auto& a, auto& b) { return lower(a) < lower(b); });
    return out;
}

}  // namespace rem
