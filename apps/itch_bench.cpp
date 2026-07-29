// itch_bench: honest throughput measurement over a full ITCH day.
//
// Usage: itch_bench <itch-file> [--runs N] [--stage scan|decode|book|all]
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

#include "book/engine.hpp"
#include "itch/scanner.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
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
                     "[--stage scan|decode|book|all]\n",
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

    ::munmap(map, size);
    ::close(fd);
    return 0;
}
