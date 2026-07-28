// itch_decode: full-file decode validation for TotalView-ITCH 5.0.
//
// Every message is decoded field-level (not just framed) through the
// dispatch layer, with cross-message consistency checks:
//   - locate → symbol mapping from the Stock Directory spin must agree
//     with the symbol carried by every later symbol-bearing message
//   - timestamps must lie within [0, 24h)
//   - side must be 'B'/'S' on add orders and non-cross trades
//   - system-event lifecycle is printed with wall-clock times
//
// Exit 0 only if the whole day decodes with zero violations.

#include "itch/decoder.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kDayNs = 86'400ULL * 1'000'000'000ULL;

std::string fmt_time(std::uint64_t ns) {
    const auto s = ns / 1'000'000'000ULL;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>(s / 60 % 60),
                  static_cast<unsigned long long>(s % 60));
    return buf;
}

struct Validator : itch::NullHandler {
    std::array<std::uint64_t, 256> counts{};
    std::vector<std::array<char, 9>> locate_symbol =
        std::vector<std::array<char, 9>>(65536, {{0}});
    std::uint64_t ts_min = ~0ULL, ts_max = 0;
    std::uint64_t ts_out_of_range = 0;
    std::uint64_t bad_side = 0;
    std::uint64_t zero_price_adds = 0;
    std::uint64_t locate_checked = 0;
    std::uint64_t locate_mismatch = 0;
    std::vector<std::string> lifecycle;

    void note(const itch::Header& h, char type) {
        ++counts[static_cast<unsigned char>(type)];
        if (h.ts_ns >= kDayNs) ++ts_out_of_range;
        if (h.ts_ns < ts_min) ts_min = h.ts_ns;
        if (h.ts_ns > ts_max) ts_max = h.ts_ns;
    }
    // Symbol-bearing messages must agree with the directory spin.
    void check_locate(const itch::Header& h, const char* stock) {
        const auto& dir = locate_symbol[h.locate];
        if (dir[0] == 0) return; // no directory entry (e.g. locate 0)
        ++locate_checked;
        if (std::strncmp(dir.data(), stock, 8) != 0) ++locate_mismatch;
    }

    void on_system_event(const itch::SystemEvent& m) {
        note(m.h, 'S');
        lifecycle.push_back(fmt_time(m.h.ts_ns) + "  event '" +
                            m.event_code + "'");
    }
    void on_stock_directory(const itch::StockDirectory& m) {
        note(m.h, 'R');
        std::memcpy(locate_symbol[m.h.locate].data(), m.stock, 9);
    }
    void on_stock_trading_action(const itch::StockTradingAction& m) {
        note(m.h, 'H');
        check_locate(m.h, m.stock);
    }
    void on_reg_sho(const itch::RegSho& m) {
        note(m.h, 'Y');
        check_locate(m.h, m.stock);
    }
    void on_market_participant_position(
        const itch::MarketParticipantPosition& m) {
        note(m.h, 'L');
        check_locate(m.h, m.stock);
    }
    void on_mwcb_decline(const itch::MwcbDecline& m) { note(m.h, 'V'); }
    void on_mwcb_status(const itch::MwcbStatus& m) { note(m.h, 'W'); }
    void on_ipo_quoting(const itch::IpoQuoting& m) { note(m.h, 'K'); }
    void on_luld_collar(const itch::LuldCollar& m) {
        note(m.h, 'J');
        check_locate(m.h, m.stock);
    }
    void on_operational_halt(const itch::OperationalHalt& m) {
        note(m.h, 'h');
        check_locate(m.h, m.stock);
    }
    void on_add_order(const itch::AddOrder& m) {
        note(m.h, m.mpid[0] ? 'F' : 'A');
        check_locate(m.h, m.stock);
        if (m.side != 'B' && m.side != 'S') ++bad_side;
        if (m.price == 0) ++zero_price_adds;
    }
    void on_order_executed(const itch::OrderExecuted& m) { note(m.h, 'E'); }
    void on_order_executed_price(const itch::OrderExecutedPrice& m) {
        note(m.h, 'C');
    }
    void on_order_cancel(const itch::OrderCancel& m) { note(m.h, 'X'); }
    void on_order_delete(const itch::OrderDelete& m) { note(m.h, 'D'); }
    void on_order_replace(const itch::OrderReplace& m) { note(m.h, 'U'); }
    void on_trade(const itch::Trade& m) {
        note(m.h, 'P');
        check_locate(m.h, m.stock);
        if (m.side != 'B' && m.side != 'S') ++bad_side;
    }
    void on_cross_trade(const itch::CrossTrade& m) {
        note(m.h, 'Q');
        check_locate(m.h, m.stock);
    }
    void on_broken_trade(const itch::BrokenTrade& m) { note(m.h, 'B'); }
    void on_noii(const itch::Noii& m) {
        note(m.h, 'I');
        check_locate(m.h, m.stock);
    }
    void on_rpii(const itch::Rpii& m) {
        note(m.h, 'N');
        check_locate(m.h, m.stock);
    }
    void on_direct_listing(const itch::DirectListing& m) {
        note(m.h, 'O');
        check_locate(m.h, m.stock);
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <itch-file>\n", argv[0]);
        return 2;
    }
    const int fd = ::open(argv[1], O_RDONLY);
    if (fd < 0) {
        std::perror("open");
        return 1;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        std::perror("fstat");
        return 1;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    ::madvise(map, size, MADV_SEQUENTIAL);

    Validator v;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = itch::decode_stream(
        static_cast<const std::uint8_t*>(map), size, v);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::printf("messages        %llu\n",
                static_cast<unsigned long long>(r.messages));
    std::printf("stream status   %s\n",
                r.status == itch::DecodeStatus::Ok
                    ? (r.clean_eof ? "OK, clean EOF" : "OK, TRUNCATED")
                    : (r.status == itch::DecodeStatus::UnknownType
                           ? "UNKNOWN TYPE"
                           : "BAD LENGTH"));
    if (r.status != itch::DecodeStatus::Ok)
        std::printf("failed type     '%c'\n", r.failed_type);
    std::printf("decode time     %.3f s (%.1fM msgs/sec)\n", secs,
                r.messages / secs / 1e6);
    std::printf("ts range        %s – %s (out of range: %llu)\n",
                fmt_time(v.ts_min).c_str(), fmt_time(v.ts_max).c_str(),
                static_cast<unsigned long long>(v.ts_out_of_range));
    std::printf("locate checks   %llu checked, %llu mismatches\n",
                static_cast<unsigned long long>(v.locate_checked),
                static_cast<unsigned long long>(v.locate_mismatch));
    std::printf("side violations %llu\n",
                static_cast<unsigned long long>(v.bad_side));
    std::printf("zero-price adds %llu\n",
                static_cast<unsigned long long>(v.zero_price_adds));
    std::printf("\nsystem lifecycle:\n");
    for (const auto& l : v.lifecycle) std::printf("  %s\n", l.c_str());
    std::printf("\ntype  count\n");
    for (int t = 0; t < 256; ++t) {
        if (v.counts[t] == 0) continue;
        std::printf("  %c   %llu\n", t,
                    static_cast<unsigned long long>(v.counts[t]));
    }

    ::munmap(map, size);
    ::close(fd);

    const bool ok = r.status == itch::DecodeStatus::Ok && r.clean_eof &&
                    v.ts_out_of_range == 0 && v.bad_side == 0 &&
                    v.locate_mismatch == 0;
    std::printf("\nvalidation      %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
