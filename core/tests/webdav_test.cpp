#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

#include "reminders/format.hpp"
#include "reminders/store.hpp"
#include "reminders/webdav.hpp"
#include "test.hpp"

using namespace rem;

namespace {

#define MARK "---\nreminders: 1\n---\n"

std::string read(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write(const fs::path& p, const std::string& text) { std::ofstream(p) << text; }

// A WebDAV folder in memory.
struct FakeFiles : FileRemote {
    struct File {
        std::string etag, text;
    };
    std::map<std::string, File> files_;  // by name
    int next_etag = 1;
    int puts = 0, gets = 0, removes = 0, moves = 0;
    bool offline = false;
    std::function<void()> before_put;  // to change things mid-sync

    void server_put(const std::string& name, const std::string& text) {
        files_[name] = {std::format("\"e{}\"", next_etag++), text};
    }
    std::string text(const std::string& name) { return files_.contains(name) ? files_[name].text : ""; }

    std::vector<RemoteFile> files() override {
        if (offline) throw SyncError("offline");
        std::vector<RemoteFile> out;
        for (auto& [name, f] : files_) out.push_back({name, f.etag});
        return out;
    }
    std::optional<RemoteText> get(const std::string& name) override {
        ++gets;
        auto it = files_.find(name);
        if (it == files_.end()) return std::nullopt;
        return RemoteText{it->second.text, it->second.etag};
    }
    std::optional<std::string> put(const std::string& name, const std::string& text, const std::string& if_match) override {
        if (before_put) std::exchange(before_put, nullptr)();
        auto it = files_.find(name);
        if (if_match.empty() ? it != files_.end()
                             : it == files_.end() || (if_match != "*" && it->second.etag != if_match))
            return std::nullopt;
        ++puts;
        server_put(name, text);
        return files_[name].etag;
    }
    bool remove(const std::string& name, const std::string& etag) override {
        auto it = files_.find(name);
        if (it == files_.end()) return true;
        if (!etag.empty() && it->second.etag != etag) return false;
        ++removes;
        files_.erase(it);
        return true;
    }
    bool move(const std::string& from, const std::string& to) override {
        if (!files_.contains(from) || files_.contains(to)) return false;
        ++moves;
        files_[to] = files_[from];
        files_.erase(from);
        return true;
    }
};

struct Fixture {
    fs::path dir = fs::temp_directory_path() / ("reminders-webdav-" + new_id());
    fs::path folder = dir / "lists";
    fs::path state = dir / "state";
    FakeFiles server;
    Store store{folder, state, BackendKind::Webdav};

    Fixture() { fs::create_directories(folder); }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::mutex& lock() { return static_cast<ServerBackend&>(store.backend_object()).lock(); }
    SyncResult sync() { return webdav_sync(folder, state, server, lock()); }
    std::string local(const std::string& name) { return read(folder / (name + ".md")); }
    bool has(const std::string& name) { return fs::exists(folder / (name + ".md")); }
};

}  // namespace

TEST(webdav_pulls_and_pushes_files) {
    Fixture f;
    f.server.server_put("Groceries", MARK "- [ ] Milk\n");
    f.server.server_put("Notes", "Just some notes\n");  // not a list: copied all the same
    auto r = f.sync();
    CHECK(r.errors.empty());
    CHECK_EQ(f.local("Groceries"), std::string(MARK "- [ ] Milk\n"));
    CHECK_EQ(f.local("Notes"), std::string("Just some notes\n"));
    CHECK_EQ(r.changed.size(), std::size_t{2});

    // Nothing changed: nothing fetched or sent.
    f.server.gets = 0;
    r = f.sync();
    CHECK(r.changed.empty());
    CHECK_EQ(f.server.gets + f.server.puts, 0);

    // Edited here: sent.
    write(f.folder / "Groceries.md", MARK "- [x] Milk\n");
    f.sync();
    CHECK_EQ(f.server.text("Groceries"), std::string(MARK "- [x] Milk\n"));
    CHECK_EQ(f.server.puts, 1);

    // Edited there: fetched.
    f.server.server_put("Groceries", MARK "- [x] Milk\n- [ ] Eggs\n");
    r = f.sync();
    CHECK_EQ(f.local("Groceries"), std::string(MARK "- [x] Milk\n- [ ] Eggs\n"));
    CHECK_EQ(f.server.puts, 1);
}

TEST(webdav_new_lists_here_are_sent_other_files_stay) {
    Fixture f;
    write(f.folder / "Work.md", MARK "- [ ] Report\n");
    write(f.folder / "scratch.md", "not a list\n");
    auto r = f.sync();
    CHECK(r.errors.empty());
    CHECK_EQ(f.server.text("Work"), std::string(MARK "- [ ] Report\n"));
    CHECK(!f.server.files_.contains("scratch"));
    CHECK(f.has("scratch"));
}

TEST(webdav_merges_changes_on_both_sides) {
    Fixture f;
    f.server.server_put("Home", MARK "- [ ] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n");
    f.sync();
    write(f.folder / "Home.md", MARK "- [x] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n");
    f.server.server_put("Home", MARK "- [ ] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n- [ ] Dust ^cccccc\n");
    auto r = f.sync();
    CHECK(r.errors.empty());
    auto want = std::string(MARK "- [x] Paint ^aaaaaa\n- [ ] Sweep ^bbbbbb\n- [ ] Dust ^cccccc\n");
    CHECK_EQ(f.local("Home"), want);
    CHECK_EQ(f.server.text("Home"), want);
    // And settled: the next sync does nothing.
    auto puts = f.server.puts;
    r = f.sync();
    CHECK(r.changed.empty());
    CHECK_EQ(f.server.puts, puts);
}

TEST(webdav_both_changed_first_sync_keeps_both) {
    Fixture f;
    write(f.folder / "Trip.md", MARK "- [ ] Tickets ^aaaaaa\n");
    f.server.server_put("Trip", MARK "- [ ] Passport ^bbbbbb\n");
    f.sync();
    auto text = f.local("Trip");
    CHECK(text.find("Tickets") != std::string::npos);
    CHECK(text.find("Passport") != std::string::npos);
    CHECK_EQ(f.server.text("Trip"), text);
}

TEST(webdav_files_that_arent_lists_keep_this_devices_copy) {
    Fixture f;
    f.server.server_put("Notes", "one\n");
    f.sync();
    write(f.folder / "Notes.md", "one, edited here\n");
    f.server.server_put("Notes", "one, edited there\n");
    f.sync();
    CHECK_EQ(f.local("Notes"), std::string("one, edited there\n"));
    CHECK_EQ(f.local("Notes (this device)"), std::string("one, edited here\n"));
    CHECK(!f.server.files_.contains("Notes (this device)"));
}

TEST(webdav_deleted_on_server) {
    Fixture f;
    f.server.server_put("Old", MARK "- [ ] a\n");
    f.server.server_put("Edited", MARK "- [ ] b\n");
    f.sync();
    write(f.folder / "Edited.md", MARK "- [ ] b\n- [ ] c\n");
    f.server.files_.clear();
    auto r = f.sync();
    CHECK(r.errors.empty());
    CHECK(!f.has("Old"));  // unchanged here: goes
    CHECK(f.has("Edited"));  // changed here since: sent again
    CHECK_EQ(f.server.text("Edited"), std::string(MARK "- [ ] b\n- [ ] c\n"));
}

TEST(webdav_deleted_in_app_or_vanished) {
    Fixture f;
    f.server.server_put("Gone", MARK "- [ ] a\n");
    f.server.server_put("Lost", MARK "- [ ] b\n");
    f.sync();
    f.store.load_all();
    f.store.delete_list("Gone");
    fs::remove(f.folder / "Lost.md");  // not by the app: fetched again
    auto r = f.sync();
    CHECK(r.errors.empty());
    CHECK(!f.server.files_.contains("Gone"));
    CHECK(f.server.files_.contains("Lost"));
    CHECK(f.has("Lost"));
}

TEST(webdav_deleted_here_but_changed_there_comes_back) {
    Fixture f;
    f.server.server_put("Plans", MARK "- [ ] a\n");
    f.sync();
    f.store.load_all();
    f.store.delete_list("Plans");
    f.server.server_put("Plans", MARK "- [ ] a\n- [ ] b\n");
    f.sync();
    CHECK(f.server.files_.contains("Plans"));
    CHECK_EQ(f.local("Plans"), std::string(MARK "- [ ] a\n- [ ] b\n"));
}

TEST(webdav_renames_follow) {
    Fixture f;
    f.server.server_put("Errands", MARK "- [ ] Post\n");
    f.sync();
    f.store.load_all();
    CHECK(f.store.rename_list(*f.store.list("Errands"), "Chores"));
    auto r = f.sync();
    CHECK(r.errors.empty());
    CHECK_EQ(f.server.moves, 1);
    CHECK(!f.server.files_.contains("Errands"));
    CHECK_EQ(f.server.text("Chores"), std::string(MARK "- [ ] Post\n"));
    // Settled: nothing fetched again, nothing new here.
    auto gets = f.server.gets;
    r = f.sync();
    CHECK(r.changed.empty());
    CHECK_EQ(f.server.gets, gets);
    CHECK(!f.has("Errands"));
}

TEST(webdav_swapped_names) {
    Fixture f;
    f.server.server_put("A", MARK "- [ ] from a\n");
    f.server.server_put("B", MARK "- [ ] from b\n");
    f.sync();
    f.store.load_all();
    CHECK(f.store.rename_list(*f.store.list("A"), "T"));
    CHECK(f.store.rename_list(*f.store.list("B"), "A"));
    CHECK(f.store.rename_list(*f.store.list("T"), "B"));
    auto r = f.sync();
    CHECK(r.errors.empty());
    CHECK_EQ(f.server.text("A"), std::string(MARK "- [ ] from b\n"));
    CHECK_EQ(f.server.text("B"), std::string(MARK "- [ ] from a\n"));
    CHECK_EQ(f.server.files_.size(), std::size_t{2});
    CHECK_EQ(f.local("A"), std::string(MARK "- [ ] from b\n"));
    CHECK_EQ(f.local("B"), std::string(MARK "- [ ] from a\n"));
}

TEST(webdav_rename_onto_a_name_taken_there) {
    Fixture f;
    f.server.server_put("Old", MARK "- [ ] mine ^aaaaaa\n");
    f.sync();
    f.store.load_all();
    CHECK(f.store.rename_list(*f.store.list("Old"), "New"));
    f.server.server_put("New", MARK "- [ ] theirs ^bbbbbb\n");  // another device made one
    auto r = f.sync();
    CHECK(r.errors.empty());
    auto text = f.server.text("New");
    CHECK(text.find("mine") != std::string::npos);
    CHECK(text.find("theirs") != std::string::npos);
    // Nothing lost: the old file is still on the server, and comes back here.
    CHECK(f.server.files_.contains("Old"));
    CHECK(f.has("Old"));
}

TEST(webdav_changed_on_server_meanwhile_is_merged_next_time) {
    Fixture f;
    f.server.server_put("Work", MARK "- [ ] a ^aaaaaa\n");
    f.sync();
    write(f.folder / "Work.md", MARK "- [x] a ^aaaaaa\n");
    f.server.before_put = [&] { f.server.server_put("Work", MARK "- [ ] a ^aaaaaa\n- [ ] b ^bbbbbb\n"); };
    auto r = f.sync();
    CHECK_EQ(r.errors.size(), std::size_t{1});
    r = f.sync();
    CHECK(r.errors.empty());
    auto want = std::string(MARK "- [x] a ^aaaaaa\n- [ ] b ^bbbbbb\n");
    CHECK_EQ(f.server.text("Work"), want);
    CHECK_EQ(f.local("Work"), want);
}

TEST(webdav_offline_changes_nothing) {
    Fixture f;
    write(f.folder / "Work.md", MARK "- [ ] a\n");
    f.server.offline = true;
    bool threw = false;
    try {
        f.sync();
    } catch (const SyncError&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(f.local("Work"), std::string(MARK "- [ ] a\n"));
    f.server.offline = false;
    f.sync();
    CHECK(f.server.files_.contains("Work"));
}
