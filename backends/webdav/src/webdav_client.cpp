#include "reminders/webdav_client.hpp"

#include <format>

#include "reminders/backend_module.hpp"
#include "reminders/dav.hpp"

namespace rem {

namespace {

using namespace dav;

class CurlFiles : public FileRemote {
	public:
		CurlFiles(const DavSettings& s, std::string password)
			: http_(s.username, std::move(password)) {
			auto scheme = s.url.find("://");
			if (scheme == std::string::npos) {
				throw SyncError(
					"url= needs to start with https:// (or http://)");
			}
			auto path = s.url.find('/', scheme + 3);
			origin_ = s.url.substr(0, path);
			// The folder's path, each part escaped once (whether or not url=
			// was).
			folder_ = "/";
			if (path != std::string::npos) {
				std::string part;
				for (auto c : s.url.substr(path) + "/") {
					if (c != '/') {
						part += c;
						continue;
					}
					if (!part.empty()) {
						folder_ += url_encode_segment(url_decode(part)) + "/";
					}
					part.clear();
				}
			}
		}

		std::vector<RemoteFile> files() override {
			constexpr std::string_view body =
				R"(<?xml version="1.0" encoding="utf-8"?><d:propfind xmlns:d="DAV:">)"
				R"(<d:prop><d:resourcetype/><d:getetag/></d:prop></d:propfind>)";
			auto r = http_.request(
				"PROPFIND", origin_ + folder_, std::string(body),
				{"Content-Type: application/xml; charset=utf-8", "Depth: 1"});
			if (r.status == 404) {
				make_folder();
				return {};
			}
			if (r.status != 207) {
				throw SyncError(std::format("PROPFIND {} failed: HTTP {}",
											url_decode(folder_), r.status));
			}
			std::vector<RemoteFile> out;
			for (auto& resp : parse_multistatus(r.body)) {
				auto path = url_decode(path_of(resp.href));
				if (path.ends_with('/') ||
					std::ranges::find(resp.resourcetype, "collection") !=
						resp.resourcetype.end()) {
					continue;
				}
				auto name = path.substr(path.rfind('/') + 1);
				if (name.starts_with('.') || !name.ends_with(".md") ||
					name.size() <= 3) {
					continue;
				}
				out.push_back(
					{name.substr(0, name.size() - 3), resp.text["getetag"]});
			}
			return out;
		}

		std::optional<RemoteText> get(const std::string& name) override {
			auto r = http_.request("GET", url_of(name));
			if (r.status == 404) {
				return std::nullopt;
			}
			check(r, "GET", name);
			return RemoteText{std::move(r.body), std::move(r.etag)};
		}

		std::optional<std::string> put(const std::string& name,
									   const std::string& text,
									   const std::string& if_match) override {
			std::vector<std::string> headers{
				"Content-Type: text/markdown; charset=utf-8"};
			headers.push_back(if_match.empty() ? "If-None-Match: *"
											   : "If-Match: " + if_match);
			auto r = http_.request("PUT", url_of(name), text, headers);
			if (r.status == 412) {
				return std::nullopt;
			}
			check(r, "PUT", name);
			return r.etag;
		}

		bool remove(const std::string& name, const std::string& etag) override {
			std::vector<std::string> headers;
			if (!etag.empty()) {
				headers.push_back("If-Match: " + etag);
			}
			auto r = http_.request("DELETE", url_of(name), "", headers);
			if (r.status == 412) {
				return false;
			}
			if (r.status == 404) {
				return true;
			}
			check(r, "DELETE", name);
			return true;
		}

		bool move(const std::string& from, const std::string& to) override {
			auto r =
				http_.request("MOVE", url_of(from), "",
							  {"Destination: " + url_of(to), "Overwrite: F"});
			if (r.status == 412 || r.status == 404) {
				return false;
			}
			check(r, "MOVE", from);
			return true;
		}

	private:
		Http http_;
		std::string origin_, folder_;  // folder_: escaped, ending in '/'

		std::string url_of(const std::string& name) const {
			return origin_ + folder_ + url_encode_segment(name + ".md");
		}

		static void check(const Response& r, std::string_view method,
						  const std::string& name) {
			if (r.status < 200 || r.status >= 300) {
				throw SyncError(std::format("{} {}.md failed: HTTP {}", method,
											name, r.status));
			}
		}

		void make_folder() {
			auto r = http_.request("MKCOL", origin_ + folder_);
			if (r.status == 409) {
				throw SyncError(
					std::format("{} doesn't exist on the server, nor does the "
								"folder it would go in",
								url_decode(folder_)));
			}
			if (r.status != 405) {
				check(r, "MKCOL", url_decode(folder_));	 // 405: there after all
			}
		}
};

}  // namespace

std::unique_ptr<FileRemote> make_webdav_remote(const DavSettings& settings) {
	if (settings.url.empty()) {
		throw SyncError("the source has no url=");
	}
	return std::make_unique<CurlFiles>(
		settings, run_password_command(settings.password_command));
}

SyncResult sync_webdav_source(Store& store, const SourceConfig& source) {
	auto* backend = dynamic_cast<ServerBackend*>(&store.backend_object());
	if (!backend || backend->id() != "webdav") {
		throw SyncError(std::format("{} isn't a WebDAV source", source.name));
	}
	auto remote = make_webdav_remote(dav_settings(source));
	return webdav_sync(store.folder(), store.state_dir(), *remote,
					   backend->lock());
}

void register_webdav_backend() {
	BackendModule m;
	m.id = "webdav";
	m.title = "WebDAV";
	m.description =
		"List files in a folder on a WebDAV server, with a copy kept here";
	m.has_server = true;
	m.synced = true;
	m.owns_folder = true;
	m.fields = server_fields(
		"Folder Address",
		"The folder's address, e.g. "
		"https://cloud.example.com/remote.php/dav/files/you/Reminders/");
	m.settings_note =
		"The list files are kept in a folder on the server (Nextcloud, "
		"ownCloud, a NAS, …), made if it isn't there.";
	m.problem = server_problem;
	m.name_hint = server_name_hint;
	m.erase_note = [](const SourceConfig&, const std::string& where) {
		return std::format(
			"Deletes the local copy in {}. The lists on the server are "
			"unaffected.",
			where);
	};
	m.make_backend = [](const fs::path& state) {
		return std::make_unique<ServerBackend>("webdav", state);
	};
	m.sync = sync_webdav_source;
	register_backend(std::move(m));
}

}  // namespace rem
