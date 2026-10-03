#include "zip.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <stdexcept>

#ifdef REMINDERS_ZLIB
#include <zlib.h>
#endif

namespace rem::detail {

namespace {

void put16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>(v >> 8);
}

void put32(std::string& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v & 0xffff));
    put16(out, static_cast<std::uint16_t>(v >> 16));
}

#ifdef REMINDERS_ZLIB
// Raw deflate (no zlib header), as zip wants.
std::string deflate_raw(std::string_view data) {
    z_stream z{};
    if (deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("couldn't start compressing");
    std::string out(deflateBound(&z, static_cast<uLong>(data.size())), '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    if (rc != Z_STREAM_END) throw std::runtime_error("couldn't compress");
    return out;
}
#endif

}  // namespace

std::uint32_t crc32_of(std::string_view data) {
#ifdef REMINDERS_ZLIB
    return static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
#else
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
#endif
}

ZipWriter::ZipWriter(std::chrono::system_clock::time_point when) {
    auto t = std::chrono::system_clock::to_time_t(when);
    std::tm tm{};
    localtime_r(&t, &tm);
    // MS-DOS date and time: from 1980, two-second steps.
    time_ = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    date_ = static_cast<std::uint16_t>((std::max(tm.tm_year - 80, 0) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

void ZipWriter::add(std::string_view name, std::string_view data) {
    auto crc = crc32_of(data);
#ifdef REMINDERS_ZLIB
    auto packed = deflate_raw(data);
    std::uint16_t method = 8;
    if (packed.size() >= data.size()) packed = std::string(data), method = 0;  // tiny files don't shrink
#else
    std::string packed(data);
    std::uint16_t method = 0;
#endif
    auto offset = static_cast<std::uint32_t>(out_.size());
    auto header = [&](std::string& to, bool central) {
        put32(to, central ? 0x02014b50 : 0x04034b50);
        if (central) put16(to, 0x0314);  // made by: Unix, zip 2.0
        put16(to, 20);                    // needed to extract: 2.0
        put16(to, 0x0800);                // names are UTF-8
        put16(to, method);
        put16(to, time_);
        put16(to, date_);
        put32(to, crc);
        put32(to, static_cast<std::uint32_t>(packed.size()));
        put32(to, static_cast<std::uint32_t>(data.size()));
        put16(to, static_cast<std::uint16_t>(name.size()));
        put16(to, 0);  // extra field
        if (central) {
            put16(to, 0);                   // comment
            put16(to, 0);                   // disk
            put16(to, 0);                   // internal attributes
            put32(to, 0100644u << 16);      // external: a regular file, rw-r--r--
            put32(to, offset);
        }
        to += name;
    };
    header(out_, false);
    out_ += packed;
    header(central_, true);
    ++count_;
}

std::string ZipWriter::finish() {
    auto cd_offset = static_cast<std::uint32_t>(out_.size());
    out_ += central_;
    put32(out_, 0x06054b50);
    put16(out_, 0);
    put16(out_, 0);
    put16(out_, count_);
    put16(out_, count_);
    put32(out_, static_cast<std::uint32_t>(central_.size()));
    put32(out_, cd_offset);
    put16(out_, 0);  // comment
    return std::move(out_);
}

}  // namespace rem::detail
