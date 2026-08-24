cmake_minimum_required(VERSION 3.24)
project(full_depth CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# libFuzzer target for the untrusted-input walkers (fuzz/decode_fuzz.cpp).
# CI-only: Apple clang ships no libFuzzer runtime — build with upstream
# clang (see the `fuzz` job in .github/workflows/ci.yml). When ON, the
# whole tree is instrumented so ASan sees into libitch, not just the
# fuzz driver's translation unit. OFF leaves normal builds untouched.
option(FULLDEPTH_FUZZ
  "Build decode_fuzz (needs clang with libFuzzer; Apple clang lacks it)"
  OFF)
if(FULLDEPTH_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(itch
  src/itch/scanner.cpp
  src/itch/messages.cpp
)
target_include_directories(itch PUBLIC src)

add_executable(itch_scan apps/itch_scan.cpp)
target_link_libraries(itch_scan PRIVATE itch)

add_executable(itch_decode apps/itch_decode.cpp)
target_link_libraries(itch_decode PRIVATE itch)

add_library(book
  src/book/book.cpp
  src/book/engine.cpp
)
target_link_libraries(book PUBLIC itch)

add_executable(itch_replay apps/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE book)

add_executable(itch_bench apps/itch_bench.cpp)
target_link_libraries(itch_bench PRIVATE book)

add_executable(itch_export apps/itch_export.cpp)
target_link_libraries(itch_export PRIVATE book)

if(FULLDEPTH_FUZZ)
  add_executable(decode_fuzz fuzz/decode_fuzz.cpp)
  target_link_libraries(decode_fuzz PRIVATE itch)
  target_compile_options(decode_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(decode_fuzz PRIVATE -fsanitize=fuzzer)
endif()

enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(itch_tests
  tests/scanner_test.cpp
  tests/messages_test.cpp
  tests/decoder_test.cpp
  tests/book_test.cpp
  tests/order_store_test.cpp
  tests/exporter_test.cpp
)
target_link_libraries(itch_tests PRIVATE book GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(itch_tests)
