#include "reminders/history.hpp"

#include <set>

#include "reminders/format.hpp"
#include "reminders/merge.hpp"

namespace rem {

std::uint64_t History::record(std::string label, const Snapshot& before, const Snapshot& after) {
    std::set<std::string> names;
    for (auto& [k, _] : before) names.insert(k);
    for (auto& [k, _] : after) names.insert(k);

    Step step{0, std::move(label), {}};
    for (auto& name : names) {
        auto b = before.find(name), a = after.find(name);
        std::optional<std::string> bt = b == before.end() ? std::nullopt : std::optional{b->second};
        std::optional<std::string> at = a == after.end() ? std::nullopt : std::optional{a->second};
        if (bt != at) step.changes.push_back(Change{name, std::move(bt), std::move(at)});
    }
    if (step.changes.empty()) return 0;

    step.id = next_id_++;
    undo_.push_back(std::move(step));
    if (undo_.size() > kLimit) undo_.erase(undo_.begin());
    redo_.clear();
    return undo_.back().id;
}

History::Result History::apply(Store& store, const Step& step, bool backwards) {
    Result result{true, {}};
    for (auto& c : step.changes) {
        const auto& target = backwards ? c.before : c.after;
        const auto& source = backwards ? c.after : c.before;
        auto current = store.current_text(c.list);
        if (current == source) {
            // Untouched since: put the old version back exactly.
            store.restore(c.list, target);
        } else if (current && target && source) {
            // Changed elsewhere since: reverse just this step's edits, keeping
            // the newer changes (the three-way merge used for sync conflicts,
            // with the step's own result as the base).
            auto base = parse(*source);
            store.restore(c.list, serialize(merge(parse(*current), parse(*target), &base)));
        } else {
            result.skipped.push_back(c.list);
        }
    }
    return result;
}

History::Result History::undo(Store& store) {
    if (undo_.empty()) return {};
    auto step = std::move(undo_.back());
    undo_.pop_back();
    auto result = apply(store, step, true);
    redo_.push_back(std::move(step));
    return result;
}

History::Result History::redo(Store& store) {
    if (redo_.empty()) return {};
    auto step = std::move(redo_.back());
    redo_.pop_back();
    auto result = apply(store, step, false);
    undo_.push_back(std::move(step));
    return result;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
}

}  // namespace rem
