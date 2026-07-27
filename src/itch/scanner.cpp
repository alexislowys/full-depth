#include "itch/scanner.hpp"

namespace itch {

ScanResult scan_buffer(const std::uint8_t* data, std::size_t size) {
    ScanResult r;
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::uint16_t len =
            static_cast<std::uint16_t>(data[pos] << 8 | data[pos + 1]);
        if (len == 0 || pos + 2 + len > size) {
            return r; // zero-length or truncated frame: not clean
        }
        const std::uint8_t type = data[pos + 2];
        ++r.counts[type];
        ++r.total_messages;
        pos += 2 + len;
        r.total_bytes = pos;
    }
    r.clean_eof = (pos == size);
    return r;
}

} // namespace itch
