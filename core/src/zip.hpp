// Writing .zip archives (internal to the core library): enough for an
// export of several lists. Files are compressed (deflate) when the library
// is built with zlib, else stored as they are; either way any unzip tool
// reads them. Names are UTF-8 (flag bit 11).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace rem::detail {

class ZipWriter {
	public:
		// `when`: the files' modification time (local time, as zip keeps it).
		explicit ZipWriter(std::chrono::system_clock::time_point when =
							   std::chrono::system_clock::now());
		void add(std::string_view name, std::string_view data);
		// The whole archive; the writer is spent.
		std::string finish();

	private:
		std::string out_, central_;
		std::uint16_t time_ = 0, date_ = 0, count_ = 0;
};

std::uint32_t crc32_of(std::string_view data);

}  // namespace rem::detail
