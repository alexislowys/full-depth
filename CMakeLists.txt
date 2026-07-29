cmake_minimum_required(VERSION 3.24)
project(full_depth CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

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
)
target_link_libraries(itch_tests PRIVATE book GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(itch_tests)
