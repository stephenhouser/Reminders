// A command line typed as one string (the terminal interface's ":" prompt),
// split into words the way a shell would: spaces separate them, '…' and "…"
// quote, a backslash escapes the next character (inside "…" too). No
// globbing and no variables; "~" and "$VAR" are left for expand_path().
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rem {

struct CommandWord {
		std::string text;				 // unquoted
		std::size_t begin = 0, end = 0;	 // where it is in the line, as typed
};

// Throws std::runtime_error for an unclosed quote, unless `partial` (a line
// still being typed, for completion), which closes it at the end.
std::vector<CommandWord> split_words(std::string_view line,
									 bool partial = false);
std::vector<std::string> split_command_line(std::string_view line);

// `word` as typed on a command line: quoted when it has spaces, quotes or
// backslashes in it ("" for an empty one).
std::string quote_word(std::string_view word);

}  // namespace rem
