#include "internal.hpp"

namespace cli {

const std::vector<std::string> kFlags = {
	"no-due", "flag", "unflag", "no-repeat", "yes", "all", "duplicates"};

// Options that take a value; anything else starting with "--" is a flag.
const std::vector<std::string> kValued = {
	"title", "list",  "section", "parent", "due",	"time",	 "priority",
	"tag",	 "untag", "repeat",	 "notes",  "url",	"color", "icon",
	"in",	 "to",	  "source",	 "format", "output"};

// The open library, for showing list names: a list's name, or "source/name"
// when another source has a list of that name (see Library::label).
const rem::Library* g_library = nullptr;

// Syncs the library's CalDAV and WebDAV sources (or just `only`); problems
// are reported on stderr. Returns false if any source failed.
// The parameters go unused in a build without network support.
bool sync_servers([[maybe_unused]] rem::Library& library,
				  [[maybe_unused]] const std::string& only,
				  [[maybe_unused]] bool verbose) {
	bool ok = true;
#ifdef REMINDERS_NETWORK
	for (auto& s : library.sources()) {
		if (!rem::syncs(s.config.backend) || !s.store) {
			continue;
		}
		if (!only.empty() && s.config.name != only) {
			continue;
		}
		try {
			auto r = rem::sync_source(*s.store, s.config);
			for (auto& e : r.errors) {
				std::cerr << std::format("reminders: sync {}: {}\n",
										 s.config.name, e);
			}
			ok = ok && r.errors.empty();
			if (verbose && r.errors.empty()) {
				std::cout << std::format(
					"Synced {}{}\n", rem::source_title(s.config),
					r.changed.empty()
						? ""
						: std::format(" ({} lists changed)", r.changed.size()));
			}
		} catch (const std::exception& e) {
			std::cerr << std::format("reminders: sync {}: {}\n", s.config.name,
									 e.what());
			ok = false;
		}
	}
#endif
	return ok;
}

// A folder from the command line: relative to the current folder, as the
// shell means it; "~" and "$VAR" work even when quoted.
rem::fs::path folder_arg(const std::string& arg) {
	if (arg.starts_with('~') || arg.find('$') != std::string::npos) {
		return rem::expand_path(arg);
	}
	return rem::fs::absolute(arg).lexically_normal();
}

bool has_servers(const rem::Library& library) {
	return std::ranges::any_of(library.sources(), [](auto& s) {
		return rem::syncs(s.config.backend);
	});
}

std::string json_escape(std::string_view s) {
	std::string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\t':
				out += "\\t";
				break;
			case '\r':
				out += "\\r";
				break;
			default:
				if (c < 0x20) {
					out += std::format("\\u{:04x}", c);
				} else {
					out += static_cast<char>(c);
				}
		}
	}
	return out + "\"";
}

std::string list_label(const rem::ListFile& l) {
	return g_library ? g_library->label(l) : l.name;
}

std::string json_reminder(const rem::Ref& ref) {
	auto& r = *ref.reminder;
	std::string tags = "[";
	for (std::size_t i = 0; i < r.tags.size(); ++i) {
		tags += (i ? "," : "") + json_escape(r.tags[i]);
	}
	tags += "]";
	auto opt = [](const std::optional<std::string>& v) {
		return v ? json_escape(*v) : std::string("null");
	};
	auto section = ref.list->doc.section_of(ref.parent ? *ref.parent : r);
	return std::format(
		R"({{"id":{},"list":{},"section":{},"parent":{},"title":{},"done":{},"due":{},"time":{},)"
		R"("completed":{},"flagged":{},"priority":"{}","tags":{},"repeat":{},"url":{},"notes":{}}})",
		json_escape(r.id), json_escape(list_label(*ref.list)), opt(section),
		ref.parent ? json_escape(ref.parent->id) : "null", json_escape(r.title),
		r.done ? "true" : "false",
		r.due_date ? json_escape(rem::format_date(*r.due_date)) : "null",
		r.due_time ? json_escape(rem::format_time(*r.due_time)) : "null",
		r.completed ? json_escape(rem::format_date(*r.completed)) : "null",
		r.flagged ? "true" : "false", term::priority_name(r.priority), tags,
		opt(r.repeat), opt(r.url), json_escape(r.notes));
}

void print_json(const std::vector<rem::Ref>& refs) {
	std::cout << "[";
	for (std::size_t i = 0; i < refs.size(); ++i) {
		std::cout << (i ? ",\n " : "") << json_reminder(refs[i]);
	}
	std::cout << "]\n";
}

// One reminder as it looks in its file: "- [ ] Milk #errands 📅 2026-10-03".
// Notes follow on indented lines.
void print_reminder(const rem::Ref& ref, const Style& st, int indent,
					bool show_list, rem::Date today) {
	auto& r = *ref.reminder;
	auto md = term::markdown_line(r);
	std::string line(static_cast<std::size_t>(indent), ' ');
	line += r.done ? st.dim() + md.before + st.reset() : md.before;
	if (!md.due.empty()) {
		line += " " + (term::is_overdue(r, today) ? st.red() : std::string()) +
				md.due + st.reset();
	}
	if (!md.after.empty()) {
		line += " " + st.dim() + md.after + st.reset();
	}
	if (show_list) {
		line += "  " + st.dim() + "(" + list_label(*ref.list) +
				(ref.parent ? " > " + ref.parent->title : "") + ")" +
				st.reset();
	}
	std::cout << line << "\n";
	for (std::size_t s = 0; !r.notes.empty();) {
		auto nl = r.notes.find('\n', s);
		std::cout << std::string(static_cast<std::size_t>(indent) + 2, ' ')
				  << st.dim() << r.notes.substr(s, nl - s) << st.reset()
				  << "\n";
		if (nl == std::string::npos) {
			break;
		}
		s = nl + 1;
	}
}

// "# Groceries" / "## Party", in the list's colour when there is one.
void print_heading(int level, const std::string& text,
				   std::optional<term::Rgb> color, const Style& st) {
	std::cout << st.bold() << (color ? st.fg(*color) : "")
			  << std::string(static_cast<std::size_t>(level), '#') << " "
			  << text << st.reset() << "\n";
}

Args parse_args(std::span<const std::string> in) {
	Args a;
	for (std::size_t i = 0; i < in.size(); ++i) {
		auto& s = in[i];
		if (s == "--") {
			a.positional.insert(a.positional.end(),
								in.begin() + static_cast<long>(i) + 1,
								in.end());
			break;
		}
		if (s == "-a" || s == "-y") {
			a.options.emplace(s == "-a" ? "all" : "yes", "");
		} else if (s == "-o") {
			if (i + 1 >= in.size()) {
				throw UsageError("-o needs a file name");
			}
			a.options.emplace("output", in[++i]);
		} else if (s.starts_with("--")) {
			auto name = s.substr(2);
			std::string value;
			if (auto eq = name.find('='); eq != std::string::npos) {
				value = name.substr(eq + 1);
				name.resize(eq);
			} else if (std::ranges::find(kValued, name) != kValued.end()) {
				if (i + 1 >= in.size()) {
					throw UsageError(std::format("--{} needs a value", name));
				}
				value = in[++i];
			}
			if (std::ranges::find(kValued, name) == kValued.end() &&
				std::ranges::find(kFlags, name) == kFlags.end()) {
				throw UsageError(std::format("unknown option --{}", name));
			}
			a.options.emplace(name, value);
		} else {
			a.positional.push_back(s);
		}
	}
	return a;
}

std::string join(const std::vector<std::string>& v, std::size_t from) {
	std::string out;
	for (auto i = from; i < v.size(); ++i) {
		out += (out.empty() ? "" : " ") + v[i];
	}
	return out;
}

}  // namespace cli
