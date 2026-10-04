#include "reminders/library.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>

#include "reminders/settings.hpp"
#include "reminders/syncthing.hpp"

namespace rem {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

int due_minutes(const Reminder& r) { return r.due_time ? r.due_time->hour * 60 + r.due_time->minute : -1; }

}  // namespace

void Library::add(SourceConfig config, std::unique_ptr<Store> store) {
    auto* added = store.get();
    sources_.push_back(Source{std::move(config), std::move(store)});
    // New ids must not clash with the other sources' ids.
    added->set_other_ids([this, added] {
        std::vector<std::string> ids;
        for (auto& s : sources_) {
            if (s.store.get() == added) continue;
            for (auto* l : s.store->lists())
                l->doc.walk([&](Reminder& r, Reminder*) {
                    if (!r.id.empty()) ids.push_back(r.id);
                });
        }
        return ids;
    });
}

void Library::load_all() {
    for (auto& s : sources_) s.store->load_all();
}

Store* Library::store(std::string_view source) {
    for (auto& s : sources_)
        if (s.config.name == source) return s.store.get();
    return nullptr;
}

Library::Source* Library::owner(const ListFile& list) {
    for (auto& s : sources_)
        if (s.store->list(list.name) == &list) return &s;
    return nullptr;
}

const Library::Source* Library::source_of(const ListFile& list) const {
    return const_cast<Library*>(this)->owner(list);
}

std::string Library::key(std::string_view source, std::string_view list) {
    return std::string(source) + "/" + std::string(list);
}

std::string Library::key_of(const ListFile& list) const {
    auto* s = source_of(list);
    return key(s ? s->config.name : "", list.name);
}

std::string Library::label(const ListFile& list) const {
    int same = 0;
    for (auto& s : sources_)
        if (s.store->list(list.name)) ++same;
    return same > 1 ? key_of(list) : list.name;
}

std::string Library::default_source() const {
    auto name = load_setting("default-source");
    for (auto& s : sources_)
        if (s.config.name == name) return name;
    return sources_.empty() ? std::string() : sources_.front().config.name;
}

std::pair<Store*, std::string> Library::split(std::string_view key) {
    auto slash = key.find('/');
    if (slash == std::string_view::npos) return {nullptr, std::string(key)};
    return {store(key.substr(0, slash)), std::string(key.substr(slash + 1))};
}

ListFile* Library::list(std::string_view key) {
    if (key.find('/') != std::string_view::npos) {
        auto [st, name] = split(key);
        return st ? st->list(name) : nullptr;
    }
    ListFile* found = nullptr;  // a bare name: only if it's unambiguous
    for (auto& s : sources_)
        if (auto* l = s.store->list(key)) {
            if (found) return nullptr;
            found = l;
        }
    return found;
}

std::vector<ListFile*> Library::lists() {
    std::vector<ListFile*> out;
    for (auto& s : sources_)
        for (auto* l : s.store->lists()) out.push_back(l);
    return out;
}

std::vector<ListFile*> Library::lists(std::string_view source) {
    auto* st = store(source);
    return st ? st->lists() : std::vector<ListFile*>{};
}

std::optional<Ref> Library::find(std::string_view id) {
    for (auto& s : sources_)
        if (auto r = s.store->find(id)) return r;
    return std::nullopt;
}

std::optional<std::string> Library::key_for_path(const fs::path& file) {
    std::error_code ec;
    for (auto& s : sources_) {
        if (!fs::equivalent(file.parent_path(), s.config.folder, ec)) continue;
        if (auto name = s.store->list_name_for(file)) return key(s.config.name, *name);
    }
    return std::nullopt;
}

bool Library::reload(std::string_view key) {
    auto [st, name] = split(key);
    return st && st->reload(name);
}

fs::path Library::path_of(std::string_view key) {
    auto [st, name] = split(key);
    return st ? st->path_of(name) : fs::path();
}

std::vector<std::string> Library::candidates() {
    std::vector<std::string> out;
    for (auto& s : sources_)
        for (auto& c : s.store->candidates()) out.push_back(key(s.config.name, c));
    return out;
}

void Library::adopt(std::string_view key) {
    auto [st, name] = split(key);
    if (st) st->adopt(name);
}

void Library::decline(std::string_view key) {
    auto [st, name] = split(key);
    if (st) st->decline(name);
}

void Library::save(ListFile& list) {
    if (auto* s = owner(list)) s->store->save(list);
}

void Library::hold_saves() {
    for (auto& s : sources_) s.store->hold_saves();
}

void Library::release_saves() {
    std::exception_ptr error;
    for (auto& s : sources_) {
        try {
            s.store->release_saves();
        } catch (...) {
            if (!error) error = std::current_exception();
        }
    }
    if (error) std::rethrow_exception(error);
}

ListFile& Library::create_list(std::string_view source, const std::string& name, std::string_view color,
                               std::string_view icon) {
    auto* st = store(source);
    if (!st) throw std::runtime_error("no source called “" + std::string(source) + "”");
    return st->create_list(name, color, icon);
}

bool Library::rename_list(ListFile& list, const std::string& new_name) {
    auto* s = owner(list);
    return s && s->store->rename_list(list, new_name);
}

void Library::delete_list(std::string_view key) {
    auto [st, name] = split(key);
    if (st) st->delete_list(name);
}

Reminder& Library::add(ListFile& list, Reminder r, const Reminder* after, const std::optional<std::string>& section) {
    auto* s = owner(list);
    if (!s) throw std::runtime_error("that list isn't in any source");
    return s->store->add(list, std::move(r), after, section);
}

namespace {

// The store holding reminder `id`.
template <class Sources>
Store* store_with(Sources& sources, std::string_view id) {
    for (auto& s : sources)
        if (s.store->find(id)) return s.store.get();
    return nullptr;
}

}  // namespace

void Library::set_done(std::string_view id, bool done, Date today) {
    if (auto* st = store_with(sources_, id)) st->set_done(id, done, today);
}

void Library::remove(std::string_view id) {
    if (auto* st = store_with(sources_, id)) st->remove(id);
}

void Library::touch(std::string_view id) {
    if (auto* st = store_with(sources_, id)) st->touch(id);
}

void Library::move_to_list(std::string_view id, ListFile& dest) {
    auto ref = find(id);
    if (!ref || ref->list == &dest) return;
    auto* from = owner(*ref->list);
    auto* to = owner(dest);
    if (!from || !to) return;
    if (from == to) return from->store->move_to_list(id, dest);
    // Between sources: out of one file, into the other. Ids are unique across
    // sources, so it keeps its id.
    auto& src = *ref->list;
    auto r = src.doc.remove(id);
    if (!r) return;
    dest.doc.insert(std::move(*r), nullptr);
    to->store->save(dest);
    from->store->save(src);
}

template <class Query>
std::vector<Ref> Library::gather(Query&& query) {
    std::vector<Ref> out;
    for (auto& s : sources_) {
        auto refs = query(*s.store);
        out.insert(out.end(), refs.begin(), refs.end());
    }
    return out;
}

std::vector<Ref> Library::today(Date today) {
    return gather([&](Store& s) { return s.today(today); });
}

std::vector<Ref> Library::scheduled() {
    auto out = gather([](Store& s) { return s.scheduled(); });
    std::ranges::stable_sort(out, [](const Ref& a, const Ref& b) {
        return std::pair{*a.reminder->due_date, due_minutes(*a.reminder)} <
               std::pair{*b.reminder->due_date, due_minutes(*b.reminder)};
    });
    return out;
}

std::vector<Ref> Library::all() {
    return gather([](Store& s) { return s.all(); });
}

std::vector<Ref> Library::everything() {
    return gather([](Store& s) { return s.everything(); });
}

std::vector<Ref> Library::flagged() {
    return gather([](Store& s) { return s.flagged(); });
}

std::vector<Ref> Library::completed() {
    auto out = gather([](Store& s) { return s.completed(); });
    std::ranges::stable_sort(out, [](const Ref& a, const Ref& b) {
        return a.reminder->completed > b.reminder->completed;  // undated last
    });
    return out;
}

std::vector<Ref> Library::tagged(std::string_view tag) {
    return gather([&](Store& s) { return s.tagged(tag); });
}

std::vector<Ref> Library::search(std::string_view query) {
    return gather([&](Store& s) { return s.search(query); });
}

std::vector<std::string> Library::tags() {
    std::vector<std::string> out;
    for (auto& s : sources_)
        for (auto& t : s.store->tags())
            if (std::ranges::find(out, t) == out.end()) out.push_back(t);
    std::ranges::sort(out, [](auto& a, auto& b) { return lower(a) < lower(b); });
    return out;
}

Snapshot Library::snapshot() const {
    Snapshot out;
    for (auto& s : sources_)
        for (auto& [name, text] : s.store->snapshot()) out.emplace(key(s.config.name, name), text);
    return out;
}

std::optional<std::string> Library::current_text(const std::string& key) const {
    auto [st, name] = const_cast<Library*>(this)->split(key);
    return st ? st->current_text(name) : std::nullopt;
}

void Library::restore(const std::string& key, const std::optional<std::string>& text) {
    auto [st, name] = split(key);
    if (st) st->restore(name, text);
}

void Library::add(const SourceConfig& config, const std::string& device) {
    std::unique_ptr<Store> store;
    try {
        store = open_source(config, device);
    } catch (const std::exception&) {  // the back end's set-up failed: open it anyway
        store = std::make_unique<Store>(config.folder, source_state_dir(config, device), config.backend);
    }
    add(config, std::move(store));
}

std::unique_ptr<Library> open_library(const std::string& device) {
    auto library = std::make_unique<Library>();
    for (auto& source : load_sources()) {
        std::error_code ec;
        // A CalDAV or WebDAV source's local copy, or a git source's folder,
        // is made on first use.
        if (has_server(source.backend) || source.backend == BackendKind::Git || fs::is_directory(source.folder, ec))
            library->add(source, device);
    }
    return library;
}

std::unique_ptr<Library> open_library(const std::optional<fs::path>& folder, const std::string& device) {
    if (!folder) return open_library(device);
    auto library = std::make_unique<Library>();
    library->add(source_for_folder(*folder), device);
    return library;
}

}  // namespace rem
