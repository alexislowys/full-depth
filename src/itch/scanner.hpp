#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace itch {

// Result of a framing-level scan over a raw TotalView-ITCH 5.0 stream.
//
// ITCH 5.0 files frame every message with a 2-byte big-endian length
// prefix; the first payload byte is the message type. A scan walks the
// framing only — it does not decode fields — and tallies message counts
// per type plus exact byte accounting.
struct ScanResult {
    std::uint64_t total_messages = 0;
    // Bytes consumed, including the 2-byte length prefixes. For a clean
    // file this must equal the input size exactly.
    std::uint64_t total_bytes = 0;
    // Message count indexed by type byte (e.g. counts['A'] = add orders).
    std::array<std::uint64_t, 256> counts{};
    // True iff the input ended exactly on a message boundary with no
    // truncated frame and no zero-length message.
    bool clean_eof = false;
};

// Scan a complete in-memory ITCH stream. Stops at the first malformed
// frame (truncated length prefix, truncated payload, or zero-length
// message); in that case clean_eof is false and total_bytes reports how
// far the scan got.
ScanResult scan_buffer(const std::uint8_t* data, std::size_t size);

} // namespace itch
