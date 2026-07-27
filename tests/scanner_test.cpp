#include "itch/scanner.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

// Append one framed message: 2-byte big-endian length, then payload
// beginning with the type byte followed by `body` filler bytes.
void append_message(std::vector<std::uint8_t>& buf, char type,
                    std::size_t body) {
    const auto len = static_cast<std::uint16_t>(1 + body);
    buf.push_back(static_cast<std::uint8_t>(len >> 8));
    buf.push_back(static_cast<std::uint8_t>(len & 0xff));
    buf.push_back(static_cast<std::uint8_t>(type));
    buf.insert(buf.end(), body, 0x00);
}

TEST(Scanner, EmptyBufferIsCleanAndEmpty) {
    const auto r = itch::scan_buffer(nullptr, 0);
    EXPECT_TRUE(r.clean_eof);
    EXPECT_EQ(r.total_messages, 0u);
    EXPECT_EQ(r.total_bytes, 0u);
}

TEST(Scanner, CountsMessagesPerType) {
    std::vector<std::uint8_t> buf;
    append_message(buf, 'S', 11);  // system event: 12-byte payload
    append_message(buf, 'A', 35);  // add order: 36-byte payload
    append_message(buf, 'A', 35);
    append_message(buf, 'D', 18);  // order delete: 19-byte payload

    const auto r = itch::scan_buffer(buf.data(), buf.size());
    EXPECT_TRUE(r.clean_eof);
    EXPECT_EQ(r.total_messages, 4u);
    EXPECT_EQ(r.total_bytes, buf.size());
    EXPECT_EQ(r.counts['S'], 1u);
    EXPECT_EQ(r.counts['A'], 2u);
    EXPECT_EQ(r.counts['D'], 1u);
    EXPECT_EQ(r.counts['X'], 0u);
}

TEST(Scanner, TruncatedPayloadIsNotClean) {
    std::vector<std::uint8_t> buf;
    append_message(buf, 'A', 35);
    const auto full = buf.size();
    append_message(buf, 'A', 35);
    buf.resize(buf.size() - 10); // cut second message short

    const auto r = itch::scan_buffer(buf.data(), buf.size());
    EXPECT_FALSE(r.clean_eof);
    EXPECT_EQ(r.total_messages, 1u);
    EXPECT_EQ(r.total_bytes, full); // scan stopped at last good boundary
}

TEST(Scanner, TruncatedLengthPrefixIsNotClean) {
    std::vector<std::uint8_t> buf;
    append_message(buf, 'E', 30);
    buf.push_back(0x00); // lone half of a length prefix

    const auto r = itch::scan_buffer(buf.data(), buf.size());
    EXPECT_FALSE(r.clean_eof);
    EXPECT_EQ(r.total_messages, 1u);
}

TEST(Scanner, ZeroLengthMessageIsNotClean) {
    std::vector<std::uint8_t> buf{0x00, 0x00, 0x41};
    const auto r = itch::scan_buffer(buf.data(), buf.size());
    EXPECT_FALSE(r.clean_eof);
    EXPECT_EQ(r.total_messages, 0u);
}

} // namespace
