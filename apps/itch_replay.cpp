// itch_replay: replay a full ITCH day through the order book engine.
//
// Usage: itch_replay <itch-file> [symbol]
//
// With a symbol, only that security's book is built (week-3 gate mode);
// without, every symbol's book is maintained. Exit 0 requires zero
// violations and a passing end-of-day audit (incremental book state ==
// state rebuilt from scratch from the live order store).

#include "book/engine.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <map>

namespace {

double dollars(std::uint32_t price) { return price / 10000.0; }

void print_top(const book::Engine& e, std::uint16_t locate, int depth) {
    const auto* b = e.book_for(locate);
    std::printf("\n%s book, top %d (EOD):\n", e.symbol_of(locate).c_str(),
                depth);
    std::printf("  %-22s | %s\n", "BID shares@price", "ASK shares@price");
    for (int i = 0; i < depth; ++i) {
        char left[64] = "", right[64] = "";
        if (static_cast<std::size_t>(i) < b->bid_levels()) {
            const auto q = b->bid_at(static_cast<std::size_t>(i));
            std::snprintf(left, sizeof left, "%6llu @ %9.4f (%u)",
                          static_cast<unsigned long long>(q.level.shares),
                          dollars(q.price), q.level.orders);
        }
        if (static_cast<std::size_t>(i) < b->ask_levels()) {
            const auto q = b->ask_at(static_cast<std::size_t>(i));
            std::snprintf(right, sizeof right, "%6llu @ %9.4f (%u)",
                          static_cast<unsigned long long>(q.level.shares),
                          dollars(q.price), q.level.orders);
        }
        if (!left[0] && !right[0]) break;
        std::printf("  %-22s | %s\n", left, right);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <itch-file> [symbol]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const std::string symbol = argc == 3 ? argv[2] : "";

    const int fd = ::open(path, O_RDONLY);
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

    book::Engine engine(symbol);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = itch::decode_stream(
        static_cast<const std::uint8_t*>(map), size, engine);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    const auto& s = engine.stats();
    const auto& v = engine.violations();

    std::printf("messages          %llu (%.1fM msgs/sec, %.3f s)\n",
                static_cast<unsigned long long>(r.messages),
                r.messages / secs / 1e6, secs);
    std::printf("stream            %s\n",
                r.status == itch::DecodeStatus::Ok && r.clean_eof
                    ? "OK, clean EOF"
                    : "FAILED");
    std::printf("book ops          adds %llu  execs %llu  cancels %llu  "
                "deletes %llu  replaces %llu\n",
                static_cast<unsigned long long>(s.adds),
                static_cast<unsigned long long>(s.executes),
                static_cast<unsigned long long>(s.cancels),
                static_cast<unsigned long long>(s.deletes),
                static_cast<unsigned long long>(s.replaces));
    std::printf("executed shares   %llu\n",
                static_cast<unsigned long long>(s.executed_shares));
    std::printf("live orders EOD   %llu (peak %llu)\n",
                static_cast<unsigned long long>(engine.live_orders()),
                static_cast<unsigned long long>(s.peak_live_orders));

    std::printf("\nviolations        total %llu\n",
                static_cast<unsigned long long>(v.total()));
    std::printf("  duplicate_ref          %llu\n",
                static_cast<unsigned long long>(v.duplicate_ref));
    std::printf("  unknown_ref            %llu\n",
                static_cast<unsigned long long>(v.unknown_ref));
    std::printf("  replace_unknown        %llu\n",
                static_cast<unsigned long long>(v.replace_unknown));
    std::printf("  over_reduction         %llu\n",
                static_cast<unsigned long long>(v.over_reduction));
    std::printf("  ladder_underflow       %llu\n",
                static_cast<unsigned long long>(v.ladder_underflow));
    std::printf("  crossed_while_trading  %llu\n",
                static_cast<unsigned long long>(v.crossed_while_trading));
    std::printf("  (cancel_to_zero oddity %llu)\n",
                static_cast<unsigned long long>(v.cancel_to_zero));
    std::printf("  (crossed w/ auction pending, legitimate: %llu)\n",
                static_cast<unsigned long long>(v.crossed_auction_pending));

    if (!engine.crossed_events().empty()) {
        struct PerSym {
            int n = 0;
            std::uint64_t first = ~0ULL, last = 0;
        };
        std::map<std::uint16_t, PerSym> by_sym;
        for (const auto& ev : engine.crossed_events()) {
            auto& p = by_sym[ev.locate];
            ++p.n;
            p.first = std::min(p.first, ev.ts_ns);
            p.last = std::max(p.last, ev.ts_ns);
        }
        std::printf("\ncrossed-while-trading breakdown (%zu symbols):\n",
                    by_sym.size());
        for (const auto& [loc, p] : by_sym) {
            const auto t0s = p.first / 1'000'000'000ULL;
            const auto t1s = p.last / 1'000'000'000ULL;
            std::printf(
                "  %-8s %4d events  %02llu:%02llu:%02llu.%03llu – "
                "%02llu:%02llu:%02llu.%03llu  (span %.3f s)\n",
                engine.symbol_of(loc).c_str(), p.n,
                static_cast<unsigned long long>(t0s / 3600),
                static_cast<unsigned long long>(t0s / 60 % 60),
                static_cast<unsigned long long>(t0s % 60),
                static_cast<unsigned long long>(p.first / 1'000'000ULL % 1000),
                static_cast<unsigned long long>(t1s / 3600),
                static_cast<unsigned long long>(t1s / 60 % 60),
                static_cast<unsigned long long>(t1s % 60),
                static_cast<unsigned long long>(p.last / 1'000'000ULL % 1000),
                (p.last - p.first) / 1e9);
        }
    }

    bool audit_ok = true;
    if (!symbol.empty()) {
        const auto locate = engine.target_locate();
        if (locate == 0) {
            std::printf("\nsymbol %s not found in directory spin\n",
                        symbol.c_str());
            audit_ok = false;
        } else {
            audit_ok = engine.audit(locate);
            std::printf("\nEOD audit (%s)   %s\n", symbol.c_str(),
                        audit_ok ? "PASS — incremental == rebuilt"
                                 : "FAIL — books diverge");
            print_top(engine, locate, 5);
        }
    } else {
        // Unfiltered: audit every symbol seen in the directory spin.
        int audited = 0, failed = 0;
        for (std::uint32_t loc = 0; loc < 65536; ++loc) {
            if (engine.symbol_of(static_cast<std::uint16_t>(loc)) == "?")
                continue;
            ++audited;
            if (!engine.audit(static_cast<std::uint16_t>(loc))) ++failed;
        }
        audit_ok = (failed == 0);
        std::printf("\nEOD audit         %d symbols, %d failures\n", audited,
                    failed);
    }

    ::munmap(map, size);
    ::close(fd);

    const bool ok = r.status == itch::DecodeStatus::Ok && r.clean_eof &&
                    v.total() == 0 && audit_ok;
    std::printf("\nreplay            %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
