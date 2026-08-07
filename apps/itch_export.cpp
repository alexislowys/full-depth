// itch_export: replay an ITCH day and export flat binary record files.
//
// Usage: itch_export <itch-file> <out-dir> [symbol]
//
// Writes trades.bin, l1.bin, symbols.csv, and meta.txt under <out-dir>
// (created if absent). With a symbol, only that security is exported.
// Exit 0 requires a clean stream, zero violations, and all writes ok.

#include "export/exporter.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>

namespace {

long long file_size(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0
               ? static_cast<long long>(st.st_size)
               : -1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: %s <itch-file> <out-dir> [symbol]\n",
                     argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const std::string out_dir = argv[2];
    const std::string symbol = argc == 4 ? argv[3] : "";

    if (::mkdir(out_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::perror("mkdir");
        return 1;
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

    fdexport::Exporter ex(out_dir, symbol);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = itch::decode_stream(
        static_cast<const std::uint8_t*>(map), size, ex);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    ex.write_symbols_and_meta(path, r.messages);
    ex.close();

    const auto& v = ex.engine().violations();

    std::printf("messages          %llu (%.1fM msgs/sec, %.3f s)\n",
                static_cast<unsigned long long>(r.messages),
                r.messages / secs / 1e6, secs);
    std::printf("stream            %s\n",
                r.status == itch::DecodeStatus::Ok && r.clean_eof
                    ? "OK, clean EOF"
                    : "FAILED");
    std::printf("trades written    %llu\n",
                static_cast<unsigned long long>(ex.trades_written()));
    std::printf("l1 written        %llu\n",
                static_cast<unsigned long long>(ex.l1_written()));
    std::printf("violations total  %llu\n",
                static_cast<unsigned long long>(v.total()));

    std::printf("\noutputs (%s):\n", out_dir.c_str());
    for (const char* name :
         {"trades.bin", "l1.bin", "symbols.csv", "meta.txt"}) {
        std::printf("  %-12s %lld bytes\n", name,
                    file_size(out_dir + "/" + name));
    }

    ::munmap(map, size);
    ::close(fd);

    const bool ok = r.status == itch::DecodeStatus::Ok && r.clean_eof &&
                    v.total() == 0 && ex.all_ok();
    std::printf("\nexport            %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
