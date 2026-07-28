#include "itch/decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

struct CountingHandler : itch::NullHandler {
    int system_events = 0;
    int adds = 0;
    int deletes = 0;
    std::uint64_t last_add_ref = 0;
    char last_add_mpid0 = '?';

    void on_system_event(const itch::SystemEvent&) { ++system_events; }
    void on_add_order(const itch::AddOrder& m) {
        ++adds;
        last_add_ref = m.ref;
        last_add_mpid0 = m.mpid[0];
    }
    void on_order_delete(const itch::OrderDelete&) { ++deletes; }
};

// Frame one message: 2-byte big-endian length + payload.
void frame(std::vector<std::uint8_t>& buf,
           const std::vector<std::uint8_t>& payload) {
    buf.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    buf.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    buf.insert(buf.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> payload_of(char type, int fill_to) {
    std::vector<std::uint8_t> p(static_cast<std::size_t>(fill_to), 0x00);
    p[0] = static_cast<std::uint8_t>(type);
    return p;
}

TEST(Decoder, DispatchesToHandler) {
    std::vector<std::uint8_t> buf;
    frame(buf, payload_of('S', 12));
    auto add = payload_of('A', 36);
    add[18] = 42; // low byte of order ref (offset 11, 8 bytes)
    frame(buf, add);
    frame(buf, payload_of('D', 19));
    frame(buf, payload_of('F', 40));

    CountingHandler h;
    const auto r = itch::decode_stream(buf.data(), buf.size(), h);
    EXPECT_TRUE(r.clean_eof);
    EXPECT_EQ(r.status, itch::DecodeStatus::Ok);
    EXPECT_EQ(r.messages, 4u);
    EXPECT_EQ(r.bytes, buf.size());
    EXPECT_EQ(h.system_events, 1);
    EXPECT_EQ(h.adds, 2); // A and F both land on on_add_order
    EXPECT_EQ(h.deletes, 1);
    EXPECT_EQ(h.last_add_ref, 0u); // F fixture has zero ref
}

TEST(Decoder, UnknownTypeStopsStream) {
    std::vector<std::uint8_t> buf;
    frame(buf, payload_of('S', 12));
    frame(buf, payload_of('Z', 10));

    CountingHandler h;
    const auto r = itch::decode_stream(buf.data(), buf.size(), h);
    EXPECT_EQ(r.status, itch::DecodeStatus::UnknownType);
    EXPECT_EQ(r.failed_type, 'Z');
    EXPECT_EQ(r.messages, 1u);
    EXPECT_FALSE(r.clean_eof);
}

TEST(Decoder, WrongLengthStopsStream) {
    std::vector<std::uint8_t> buf;
    frame(buf, payload_of('A', 20)); // A must be 36

    CountingHandler h;
    const auto r = itch::decode_stream(buf.data(), buf.size(), h);
    EXPECT_EQ(r.status, itch::DecodeStatus::BadLength);
    EXPECT_EQ(r.failed_type, 'A');
    EXPECT_EQ(r.messages, 0u);
}

TEST(Decoder, TruncatedFrameIsNotClean) {
    std::vector<std::uint8_t> buf;
    frame(buf, payload_of('S', 12));
    buf.push_back(0x00);
    buf.push_back(0x24); // promises 36 bytes, delivers none

    CountingHandler h;
    const auto r = itch::decode_stream(buf.data(), buf.size(), h);
    EXPECT_EQ(r.status, itch::DecodeStatus::Ok);
    EXPECT_FALSE(r.clean_eof);
    EXPECT_EQ(r.messages, 1u);
}

} // namespace
