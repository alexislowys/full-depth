# Full-Depth

Reconstructing the complete Nasdaq limit order book from raw binary TotalView-ITCH 5.0 feed data in C++, with a Python analytics layer for market microstructure research.

## Headline results

> Work in progress — numbers land here as milestones complete.

| Metric | Result |
|---|---|
| Framing scan (week 1) | **423,285,709 messages, byte accounting EXACT, 18.6M msgs/sec** (mmap cold, I/O included, Apple M-series) |
| Throughput (parse + book build, full-day replay) | _TBD — target ≥10M msgs/sec_ |
| Correctness | _byte-exact framing done; book-invariant sweep TBD_ |
| Analytics | _TBD — OFI–midprice stylized fact, top-20 symbols_ |

Benchmark methodology: mmap'd decompressed file, warm cache, parse-only rate reported separately from parse+book rate, median of 5 runs, machine spec stated. No benchmark theater.

## Data

Free full-day TotalView-ITCH 5.0 sample files from Nasdaq's public server: <https://emi.nasdaq.com/ITCH/>. See [docs/data.md](docs/data.md). Data files are not committed (~5.6 GB gzipped, ~13 GB raw).

## Layout

```
src/itch/     ITCH 5.0 decoder (framing + message decode)
src/book/     order book engine (arrives week 3)
apps/         replay / scan binaries
tests/        GoogleTest unit tests
bench/        benchmark harness (arrives week 5)
analytics/    Python: Parquet + microstructure notebooks (arrives week 7)
docs/         spec notes, data provenance, methodology
```

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Requires: CMake ≥ 3.24, a C++20 compiler. Tested with Apple clang 17 on macOS (arm64).

## Scope

Market-data engineering and descriptive microstructure only. No trading signals, no price prediction, no backtesting.
