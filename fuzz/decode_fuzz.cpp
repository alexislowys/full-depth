// libFuzzer entry point for the two untrusted-input walkers: the framing
// scanner and the full decode dispatch. Both take arbitrary bytes off
// disk/wire and must never crash or read out of bounds.
//
// CI-only: Apple clang ships no libFuzzer runtime, so this target builds
// under -DFULLDEPTH_FUZZ=ON with upstream clang (see the `fuzz` job in
// .github/workflows/ci.yml). Seed corpus: fuzz/make_corpus.py.

#include "itch/decoder.hpp"
#include "itch/scanner.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    itch::scan_buffer(data, size);
    itch::NullHandler h;
    itch::decode_stream(data, size, h);
    return 0;
}
