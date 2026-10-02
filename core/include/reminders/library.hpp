// Every source open at once: one Store per source, lists named
// "source/list", and the smart lists, tags and search across all of them.
// Ids are unique across the sources, so a reminder id alone finds it.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "reminders/sources.hpp"
#include "reminders/store.hpp"

namespace rem {

class Library : public ListTexts {
public:
    // The stores refer back to it (for unique ids), so it stays put.
    Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    struct Source {
        SourceConfig config;
        std::unique_ptr<Store> store;
    };

    // Adds an opened source (see open_source); it's loaded by load_all().
    // Sources keep the order they were added in.
    void add(SourceConfig config, std::unique_ptr<Store> store);
    void load_all();

    const std::vector<Source>& sources() const { return sources_; }
    Store* store(std::string_view source);
    // The source a list belongs to.
    const Source* source_of(const ListFile& list) const;

    // A list's key: "source/name" (neither may contain "/").
    static std::string key(std::string_view source, std::string_view list);
    std::string key_of(const ListFile& list) const;
    // A list by key. A bare name finds the list when only one source has it.
    ListFile* list(std::string_view key);
    // Every list: sources in order, each in its own order (by "order", then name).
    std::vector<ListFile*> lists();
    std::vector<ListFile*> lists(std::string_view source);

    std::optional<Ref> find(std::string_view id);

    // As Store's, on the list's own source.
    void save(ListFile& list);
    ListFile& create_list(std::string_view source, const std::string& name, std::string_view color,
                          std::string_view icon);
    bool rename_list(ListFile& list, const std::string& new_name);
    void delete_list(std::string_view key);
    Reminder& add(ListFile& list, Reminder r, const Reminder* after = nullptr,
                  const std::optional<std::string>& section = std::nullopt);
    void set_done(std::string_view id, bool done, Date today);
    void remove(std::string_view id);
    void touch(std::string_view id);
    // Moves a reminder (with its subtasks) to the end of another list, which
    // may be in another source.
    void move_to_list(std::string_view id, ListFile& dest);

    // Smart lists, across every source (same order rules as Store's).
    std::vector<Ref> today(Date today);
    std::vector<Ref> scheduled();
    std::vector<Ref> all();
    std::vector<Ref> everything();
    std::vector<Ref> flagged();
    std::vector<Ref> completed();
    std::vector<Ref> tagged(std::string_view tag);
    std::vector<Ref> search(std::string_view query);
    std::vector<std::string> tags();

    // Undo (keys are "source/name").
    Snapshot snapshot() const override;
    std::optional<std::string> current_text(const std::string& key) const override;
    void restore(const std::string& key, const std::optional<std::string>& text) override;

private:
    std::vector<Source> sources_;

    Source* owner(const ListFile& list);
    // "source/name" → the source's store and the name; nullptr if unknown.
    std::pair<Store*, std::string> split(std::string_view key);
    template <class Query> std::vector<Ref> gather(Query&& query);
};

// Every configured source, opened (not loaded). A source whose folder is
// missing is left out.
std::unique_ptr<Library> open_library(const std::string& device);

}  // namespace rem
