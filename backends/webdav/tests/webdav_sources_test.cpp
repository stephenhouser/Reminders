// WebDAV sources in settings.ini: naming, the default local copy, removing.
#include <cstdlib>
#include <fstream>

#include "reminders/backend_module.hpp"
#include "reminders/settings.hpp"
#include "reminders/sources.hpp"
#include "reminders/webdav_client.hpp"
#include "test.hpp"

using namespace rem;

TEST(sources_webdav) {
	register_webdav_backend();
	auto dir = fs::temp_directory_path() / ("reminders-sources-" + new_id());
	setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
	setenv("XDG_DATA_HOME", (dir / "data").c_str(), 1);
	fs::create_directories(dir / "config" / "reminders");
	std::ofstream(dir / "config" / "reminders" / "settings.ini")
		<< "[general]\n";

	DavSettings s{
		"https://cloud.example.com/remote.php/dav/files/me/Reminders/", "me",
		"pass show cloud", 10};
	SourceConfig source{"", "webdav", {}, ""};
	set_dav_settings(source, s);
	auto a = add_source(source);
	CHECK_EQ(a.name, "example");
	CHECK(a.folder == dir / "data" / "reminders" / "webdav" / "example");
	auto all = load_sources();
	CHECK_EQ(all.size(), 1u);
	CHECK(all[0].backend == "webdav");
	CHECK(dav_settings(all[0]) == s);
	CHECK(all[0].folder == a.folder);
	CHECK(find_backend("webdav")->has_server &&
		  find_backend("webdav")->owns_folder);

	// Removing it can keep the local copy in the default place...
	fs::create_directories(a.folder);
	remove_source(a.name, true);
	CHECK(fs::exists(a.folder));
	CHECK(load_sources().empty());
	// ...or, by default, removes it too.
	auto b = add_source(source);
	fs::create_directories(b.folder);
	remove_source(b.name);
	CHECK(!fs::exists(b.folder));
	unsetenv("XDG_DATA_HOME");
	std::error_code ec;
	fs::remove_all(dir, ec);
}
