# Full-Depth

Reconstructing the complete Nasdaq limit order book from raw binary TotalView-ITCH 5.0 feed data in C++, with a Python analytics layer for market microstructure research.

## Headline results

> Work in progress — numbers land here as milestones complete.

| Metric | Result |
|---|---|
| Framing scan (week 1) | **423,285,709 messages, byte accounting EXACT, 18.6M msgs/sec** (mmap cold, I/O included, Apple M-series) |
| Full decode (week 2) | **17.2M msgs/sec decoding every field of all 23 message types; 192.7M locate↔symbol cross-checks, 0 mismatches; 0 timestamp/side violations** |
| Book replay (weeks 3–4) | **Whole-market book (8,915 symbols, 186.6M adds, peak 1.93M live orders): 0 invariant violations, all 8,915 end-of-day audits pass, book fully empties at end of session** |
| Benchmarks (week 5) | scan **20.2M**, full decode **17.5M** msgs/sec medians — I/O included; methodology in [docs/benchmarks.md](docs/benchmarks.md) |
| Optimization (week 6) | whole-market book build **3.0M → 7.4M msgs/sec (2.5×)** — profile-driven: sorted-vector ladders + open-addressing order store with backshift deletion; every experiment (including the failed ones) measured and documented |
| Correctness | byte-exact framing; field-level decode validated; zero book violations with auction-aware crossed-book invariant |

**Microstructure find:** a naive "book never crosses while trading" invariant fires 458 times on this day — every one traced to 4 symbols: ANPC and BDTX (IPO'd that very day) and RKDA and DTSS (volatility-halt reopenings, DTSS crossed 5.4 minutes through repeated LULD collar extensions). Crossed displayed books are *legitimate* between a resumption and the auction's cross print. The invariant is auction-aware; details in [docs/spec-notes.md](docs/spec-notes.md).
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
