# Full-Depth

[![CI](https://github.com/alexislowys/full-depth/actions/workflows/ci.yml/badge.svg)](https://github.com/alexislowys/full-depth/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**TL;DR — I rebuilt the entire Nasdaq limit order book, for every one of 8,915 symbols, from a raw 13 GB binary exchange feed (423 million messages), in C++ at 7.4M messages/second — with zero integrity violations, and end-of-day books that match Nasdaq's official closing prices to the cent. On top of it, a Python analytics layer reproduces a published price-impact law from the microstructure literature.**

Reconstructing the complete Nasdaq limit order book from raw binary TotalView-ITCH 5.0 feed data in C++, with a Python analytics layer for market microstructure research. Ten milestones, built in evenings July–August 2026 (planned as a 10-week roadmap, compressed — see the [commit history](https://github.com/alexislowys/full-depth/commits/main)). Development was AI-assisted (Claude Code, directed and reviewed by me); every headline number below is reproduced by a committed [clean-room verification script](analytics/verification/verify_headlines.py) and anchored against official market data — check the claims, not the author.

## Headline results

| Metric | Result |
|---|---|
| External validation | **15/15 Nasdaq-listed closing-cross prints match official closing prices to the cent**; every analytics headline number reproduced by an independent committed recomputation |
| Framing scan (week 1) | **423,285,709 messages, byte accounting EXACT, 18.6M msgs/sec** (mmap cold, I/O included, Apple M-series) |
| Full decode (week 2) | **17.2M msgs/sec decoding every field of all 23 message types; 192.7M locate↔symbol cross-checks, 0 mismatches; 0 timestamp/side violations** |
| Book replay (weeks 3–4) | **Whole-market book (8,915 symbols, 186.6M adds, peak 1.93M live orders): 0 invariant violations, all 8,915 end-of-day audits pass, book fully empties at end of session** |
| Benchmarks (week 5) | scan **20.2M**, full decode **17.5M** msgs/sec medians — I/O included; methodology in [docs/benchmarks.md](docs/benchmarks.md) |
| Optimization (week 6) | whole-market book build **3.0M → 7.4M msgs/sec (2.5×)** — profile-driven: sorted-vector ladders + open-addressing order store with backshift deletion; every experiment (including the failed ones) measured and documented |
| Export layer (week 7) | **10.3M trade + 208.8M L1 records from the full day, byte-frozen C++↔Python contract ([docs/export-format.md](docs/export-format.md)), 19/19 Python-side integrity checks, Parquet via zstd** |
| Correctness | byte-exact framing; field-level decode validated; zero book violations with auction-aware crossed-book invariant |
| Analytics (week 8) | **OFI, liquidity, activity studies on top-20 subsets — headline numbers independently recomputed to full float precision; [docs/analytics-wk8.md](docs/analytics-wk8.md)** |
| OFI–price impact study (week 9) | **CKS inverse depth–impact law reproduced: log-log slope −1.26 [−1.54, −0.98], R² 0.83 across 20 symbols; contemporaneous R² ~0.5 vs one-step-ahead ≤0.012 — described, honestly not forecast; [docs/analytics-wk9.md](docs/analytics-wk9.md)** |
| Reproducibility (week 10) | fresh-clone build 9 s, 75/75 tests, full replay PASS following the README alone |

![Price impact vs depth](analytics/figures/impact_vs_depth.png)

Full story: [docs/writeup.md](docs/writeup.md).

**Microstructure find:** a naive "book never crosses while trading" invariant fires 458 times on this day — every one traced to 4 symbols: ANPC and BDTX (IPO'd that very day) and RKDA and DTSS (volatility-halt reopenings, DTSS crossed 5.4 minutes through repeated LULD collar extensions). Crossed displayed books are *legitimate* between a resumption and the auction's cross print. The invariant is auction-aware; details in [docs/spec-notes.md](docs/spec-notes.md).

Benchmark methodology: mmap'd decompressed file, I/O included, every run reported plus median, machine spec stated. No benchmark theater — see [docs/benchmarks.md](docs/benchmarks.md).

## Data

Free full-day TotalView-ITCH 5.0 sample files from Nasdaq's public server: <https://emi.nasdaq.com/ITCH/>. See [docs/data.md](docs/data.md). Data files are not committed (~5.6 GB gzipped, ~13 GB raw).

## Layout

```
src/itch/     ITCH 5.0 decoder (framing + message decode)
src/book/     order book engine + open-addressing order store
src/export/   binary export layer (frozen C++/Python contract)
apps/         scan / decode / replay / bench / export binaries
tests/        GoogleTest unit tests (75)
analytics/    Python: readers, studies, verification, figures
docs/         spec notes, methodology, studies, writeup
```

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Requires: CMake ≥ 3.24, a C++20 compiler, and network on first configure
(GoogleTest arrives via FetchContent). Tested with Apple clang 17 on
macOS (arm64); fresh clone builds in ~9 s.

## Run

Binaries land in `build/`. With a decompressed sample day in `data/`:

```
build/itch_scan   data/01302020.NASDAQ_ITCH50            # framing scan + byte accounting
build/itch_decode data/01302020.NASDAQ_ITCH50            # field-level decode validation
build/itch_replay data/01302020.NASDAQ_ITCH50 [SYMBOL]   # book replay + invariants + audits
build/itch_bench  data/01302020.NASDAQ_ITCH50            # throughput benchmarks
build/itch_export data/01302020.NASDAQ_ITCH50 data/export  # binary export for analytics
```

Python analytics setup and commands: [analytics/README.md](analytics/README.md).

## Scope

Market-data engineering and descriptive microstructure only. No trading signals, no price prediction, no backtesting.

## Author

Built by **Alexis Low** — data science undergraduate, Monash University.
[LinkedIn](https://www.linkedin.com/in/alexislow10) · [GitHub](https://github.com/alexislowys) ·
related: [insider-tracker](https://github.com/alexislowys/insider-tracker) (SEC Form 4 pipeline, Next.js + Postgres)
