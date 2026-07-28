#pragma once

#include <cstdint>
#include <cstring>

namespace itch {

// Big-endian field loads. ITCH is big-endian throughout; timestamps are
// 48-bit nanoseconds since midnight ET.

inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(std::uint16_t(p[0]) << 8 | p[1]);
}

inline std::uint32_t be32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

inline std::uint64_t be48(const std::uint8_t* p) {
    return std::uint64_t(be16(p)) << 32 | be32(p + 2);
}

inline std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

} // namespace itch
