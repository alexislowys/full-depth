// itch_bench: honest throughput measurement over a full ITCH day.
//
// Usage: itch_bench <itch-file> [--runs N]
//                   [--stage scan|decode|book|latency|all]
//
// Methodology (see docs/benchmarks.md):
//   - the file is mmap'd once; every run walks the same mapping, so
//     run 1 pays page-cache misses and later runs show the warm rate —
//     both are reported, nothing is hidden
//   - stages are timed independently: framing scan (no field decode),
//     full decode (every field of every message, no-op handler), and
//     whole-market book build (decode + order book maintenance)
//   - engine construction happens outside the timed region
//   - medians are reported alongside every individual run
//   - the latency stage times every message individually and reports
//     percentiles per session phase; it is opt-in ("all" covers the
//     three throughput stages) and always a single pass — per-message
//     timer calls make it slower than the book stage by construction

#include "book/engine.hpp"
#include "itch/be.hpp"
#include "itch/decoder.hpp"
#include "itch/scanner.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

double now_between(const std::function<std::uint64_t()>& work,
                   std::uint64_t& messages) {
    const auto t0 = std::chrono::steady_clock::now();
    messages = work();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

void print_machine() {
#if defined(__APPLE__)
    char brand[256] = "unknown";
    std::size_t len = sizeof brand;
    ::sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0);
    std::uint64_t mem = 0;
    len = sizeof mem;
    ::sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
    std::uint32_t pcores = 0, ecores = 0;
    len = sizeof pcores;
    ::sysctlbyname("hw.perflevel0.physicalcpu", &pcores, &len, nullptr, 0);
    len = sizeof ecores;
    ::sysctlbyname("hw.perflevel1.physicalcpu", &ecores, &len, nullptr, 0);
    std::printf("machine    %s, %uP+%uE cores, %.0f GB RAM\n", brand, pcores,
                ecores, mem / 1073741824.0);
#else
    std::printf("machine    (unrecorded non-macOS host)\n");
#endif
}

struct StageResult {
    std::vector<double> secs;
    std::uint64_t messages = 0;
};

void report(const char* name, const StageResult& r, std::size_t bytes) {
    std::vector<double> sorted = r.secs;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    std::printf("\n%s: %llu messages\n", name,
                static_cast<unsigned long long>(r.messages));
    for (std::size_t i = 0; i < r.secs.size(); ++i)
        std::printf("  run %zu   %7.3f s   %6.1fM msgs/s   %5.2f GB/s\n",
                    i + 1, r.secs[i], r.messages / r.secs[i] / 1e6,
                    bytes / r.secs[i] / 1e9);
    std::printf("  median  %7.3f s   %6.1fM msgs/s   %5.2f GB/s\n", median,
                r.messages / median / 1e6, bytes / median / 1e9);
}

// --- latency stage ------------------------------------------------------

constexpr std::uint64_t kNsPerSec = 1'000'000'000ull;
// Session phase boundaries, nanoseconds since midnight ET.
constexpr std::uint64_t kOpen = 34'200 * kNsPerSec;        // 09:30
constexpr std::uint64_t kOpenPlus30 = 36'000 * kNsPerSec;  // 10:00
constexpr std::uint64_t kCloseLess30 = 55'800 * kNsPerSec; // 15:30
constexpr std::uint64_t kClose = 57'600 * kNsPerSec;       // 16:00

constexpr int kPhases = 5;
constexpr const char* kPhaseNames[kPhases] = {
    "pre-open", "first 30min", "mid-day", "last 30min", "post-close"};

int phase_of(std::uint64_t ts_ns) {
    if (ts_ns < kOpen) return 0;
    if (ts_ns < kOpenPlus30) return 1;
    if (ts_ns < kCloseLess30) return 2;
    if (ts_ns < kClose) return 3;
    return 4;
}

// Fixed-bin latency histogram: 128 linear 8 ns bins over [0, 1024) ns,
// one bin per power of two over [2^10, 2^30) ns, one overflow bin above
// (~1.07 s). O(1) record; percentiles are exact to bin resolution and
// reported at the bin's upper edge, clamped to the exact observed max.
struct LatencyHist {
    static constexpr int kLinear = 128;
    static constexpr int kPow2 = 20;
    static constexpr int kBins = kLinear + kPow2 + 1;

    std::array<std::uint64_t, kBins> bins{};
    std::uint64_t count = 0;
    std::uint64_t max_ns = 0;

    static int index_of(std::uint64_t ns) {
        if (ns < 1024) return static_cast<int>(ns >> 3);
        const int log2 = 63 - __builtin_clzll(ns);
        return log2 < 30 ? kLinear + (log2 - 10) : kBins - 1;
    }

    static std::uint64_t upper_edge(int i) {
        return i < kLinear ? (static_cast<std::uint64_t>(i) + 1) * 8
                           : 1ull << (i - kLinear + 11);
    }

    void record(std::uint64_t ns) {
        ++bins[static_cast<std::size_t>(index_of(ns))];
        ++count;
        if (ns > max_ns) max_ns = ns;
    }

    void merge(const LatencyHist& o) {
        for (int i = 0; i < kBins; ++i) bins[i] += o.bins[i];
        count += o.count;
        max_ns = std::max(max_ns, o.max_ns);
    }

    std::uint64_t percentile(double p) const {
        if (count == 0) return 0;
        const auto rank = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(
                   std::ceil(p / 100.0 * static_cast<double>(count))));
        std::uint64_t cum = 0;
        for (int i = 0; i < kBins; ++i) {
            cum += bins[i];
            if (cum >= rank)
                return i == kBins - 1 ? max_ns
                                      : std::min(upper_edge(i), max_ns);
        }
        return max_ns;
    }

    // Exact when ns falls on a bin edge (1000 does: 1000 = 125 * 8).
    std::uint64_t count_at_least(std::uint64_t ns) const {
        std::uint64_t c = 0;
        for (int i = index_of(ns); i < kBins; ++i) c += bins[i];
        return c;
    }
};

struct TimerCost {
    std::uint64_t mean_ns; // ≈ cost of one now() call
    std::uint64_t tick_ns; // clock granularity (min nonzero gap)
};

TimerCost timer_cost() {
    // Back-to-back now() gaps: the mean spreads the clock's tick
    // quantization into a per-call cost; the min nonzero gap is the tick.
    std::array<std::uint64_t, 4096> gaps{};
    auto prev = std::chrono::steady_clock::now();
    for (auto& g : gaps) {
        const auto t = std::chrono::steady_clock::now();
        g = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t - prev)
                .count());
        prev = t;
    }
    std::uint64_t sum = 0, tick = ~0ull;
    for (const auto g : gaps) {
        sum += g;
        if (g != 0 && g < tick) tick = g;
    }
    return {sum / gaps.size(), tick};
}

int run_latency(const std::uint8_t* data, std::size_t size) {
    const TimerCost timer = timer_cost();
    std::printf("\nper-message latency: decode_message dispatch into "
                "book::Engine\n");
    std::printf("  timer      steady_clock, ~%llu ns mean per now() call, "
                "~%llu ns tick;\n"
                "             samples quantized to the tick and carry ~one "
                "call of\n"
                "             overhead (two calls/message), not "
                "subtracted\n",
                static_cast<unsigned long long>(timer.mean_ns),
                static_cast<unsigned long long>(timer.tick_ns));
    std::printf("  histogram  8 ns bins to 1.024 us, power-of-2 bins to "
                "~1 s;\n"
                "             percentiles reported at bin upper edge\n");
    std::printf("  note       measures latency, not throughput — timer "
                "calls make\n"
                "             this pass slower than the book stage; "
                "single pass\n");

    auto engine = std::make_unique<book::Engine>(); // untimed
    std::array<LatencyHist, kPhases> hists{};

    const auto wall0 = std::chrono::steady_clock::now();
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::uint16_t len = itch::be16(data + pos);
        if (len == 0 || pos + 2 + len > size) break;
        const std::uint8_t* payload = data + pos + 2;
        const std::size_t next = pos + 2u + len;
        // Same one-frame lookahead decode_stream issues, kept outside
        // the timed span so both stages run the same prefetch pipeline.
        if (next + 2 <= size) {
            const std::uint16_t next_len = itch::be16(data + next);
            if (next_len != 0 && next + 2 + next_len <= size)
                engine->prefetch_hint(data + next + 2, next_len);
        }
        const int phase = phase_of(itch::be48(payload + 5));
        const auto t0 = std::chrono::steady_clock::now();
        const itch::DecodeStatus s =
            itch::decode_message(payload, len, *engine);
        const auto t1 = std::chrono::steady_clock::now();
        if (s != itch::DecodeStatus::Ok) {
            std::fprintf(stderr, "decode failed (type 0x%02x) — abort\n",
                         payload[0]);
            return 1;
        }
        hists[static_cast<std::size_t>(phase)].record(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 -
                                                                     t0)
                    .count()));
        pos = next;
    }
    const auto wall1 = std::chrono::steady_clock::now();
    if (engine->violations().total() != 0) {
        std::fprintf(stderr, "latency stage saw violations — benchmark "
                             "void\n");
        return 1;
    }

    LatencyHist overall{};
    for (const auto& h : hists) overall.merge(h);
    const double wall = std::chrono::duration<double>(wall1 - wall0).count();
    std::printf("\n  pass       %llu messages in %.1f s (%.1fM msgs/s "
                "with timers)\n",
                static_cast<unsigned long long>(overall.count), wall,
                overall.count / wall / 1e6);

    std::printf("\n  %-12s %11s %6s %6s %7s %7s %10s %9s\n", "phase",
                "messages", "p50", "p99", "p99.9", "p99.99", "max",
                ">=1us");
    const auto row = [](const char* name, const LatencyHist& h) {
        std::printf("  %-12s %11llu %6llu %6llu %7llu %7llu %10llu %9llu\n",
                    name, static_cast<unsigned long long>(h.count),
                    static_cast<unsigned long long>(h.percentile(50.0)),
                    static_cast<unsigned long long>(h.percentile(99.0)),
                    static_cast<unsigned long long>(h.percentile(99.9)),
                    static_cast<unsigned long long>(h.percentile(99.99)),
                    static_cast<unsigned long long>(h.max_ns),
                    static_cast<unsigned long long>(
                        h.count_at_least(1000)));
    };
    for (int i = 0; i < kPhases; ++i)
        row(kPhaseNames[i], hists[static_cast<std::size_t>(i)]);
    row("overall", overall);
    std::printf("  (all values ns; phases ET: pre-open <09:30, first "
                "30min 09:30-10:00,\n   mid-day 10:00-15:30, last 30min "
                "15:30-16:00, post-close >=16:00)\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    int runs = 5;
    std::string stage = "all";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--stage") == 0 && i + 1 < argc) {
            stage = argv[++i];
        } else {
            path = argv[i];
        }
    }
    if (!path || runs < 1) {
        std::fprintf(stderr,
                     "usage: %s <itch-file> [--runs N] "
                     "[--stage scan|decode|book|latency|all]\n",
                     argv[0]);
        return 2;
    }

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
    const auto* data = static_cast<const std::uint8_t*>(map);

    print_machine();
    std::printf("file       %s (%.2f GB)\n", path, size / 1e9);
    std::printf("runs       %d per stage; run 1 may include page-cache "
                "misses (reported, not discarded)\n",
                runs);

    if (stage == "scan" || stage == "all") {
        StageResult r;
        for (int i = 0; i < runs; ++i) {
            std::uint64_t msgs = 0;
            r.secs.push_back(now_between(
                [&] { return itch::scan_buffer(data, size).total_messages; },
                msgs));
            r.messages = msgs;
        }
        report("framing scan", r, size);
    }

    if (stage == "decode" || stage == "all") {
        StageResult r;
        for (int i = 0; i < runs; ++i) {
            std::uint64_t msgs = 0;
            r.secs.push_back(now_between(
                [&] {
                    itch::NullHandler h;
                    return itch::decode_stream(data, size, h).messages;
                },
                msgs));
            r.messages = msgs;
        }
        report("full decode", r, size);
    }

    if (stage == "book" || stage == "all") {
        StageResult r;
        for (int i = 0; i < runs; ++i) {
            auto engine = std::make_unique<book::Engine>(); // untimed
            std::uint64_t msgs = 0;
            r.secs.push_back(now_between(
                [&] {
                    return itch::decode_stream(data, size, *engine).messages;
                },
                msgs));
            r.messages = msgs;
            if (engine->violations().total() != 0) {
                std::fprintf(stderr,
                             "book stage saw violations — benchmark void\n");
                return 1;
            }
        }
        report("whole-market book build", r, size);
    }

    // Opt-in: per-message timing answers a different question than the
    // throughput stages, so "all" does not include it.
    if (stage == "latency") {
        const int rc = run_latency(data, size);
        if (rc != 0) return rc;
    }

    ::munmap(map, size);
    ::close(fd);
    return 0;
}
