// itch_scan: framing-level scan of a raw TotalView-ITCH 5.0 file.
//
// Prints per-type message counts, total messages, byte accounting, and
// throughput. First correctness gate of the project: the sum of framed
// bytes must equal the file size exactly.
//
// Usage: itch_scan <path/to/file.NASDAQ_ITCH50>

#include "itch/scanner.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

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

    const auto t0 = std::chrono::steady_clock::now();
    const auto result =
        itch::scan_buffer(static_cast<const std::uint8_t*>(map), size);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::printf("file            %s\n", argv[1]);
    std::printf("file bytes      %zu\n", size);
    std::printf("framed bytes    %llu\n",
                static_cast<unsigned long long>(result.total_bytes));
    std::printf("byte accounting %s\n",
                (result.clean_eof && result.total_bytes == size) ? "EXACT"
                                                                 : "MISMATCH");
    std::printf("messages        %llu\n",
                static_cast<unsigned long long>(result.total_messages));
    std::printf("scan time       %.3f s (%.1fM msgs/sec)\n", secs,
                result.total_messages / secs / 1e6);
    std::printf("\ntype  count\n");
    for (int t = 0; t < 256; ++t) {
        if (result.counts[t] == 0) continue;
        std::printf("  %c   %llu\n", std::isprint(t) ? t : '?',
                    static_cast<unsigned long long>(result.counts[t]));
    }

    ::munmap(map, size);
    ::close(fd);
    return (result.clean_eof && result.total_bytes == size) ? 0 : 1;
}
