# Benchmark methodology & results

No benchmark theater. Every number here is reproducible with
`itch_bench`, and the conditions that shaped it are stated.

## Methodology

- **Input:** the full 2020-01-30 TotalView-ITCH day — 12.95 GB
  decompressed, 423,285,709 messages. Real feed data, not synthetic.
- **Stages timed independently:**
  1. *framing scan* — walk the 2-byte length framing, count messages;
     no field decoding. Upper bound set by memory/I-O throughput.
  2. *full decode* — every field of every message decoded through the
     dispatch layer into typed structs; no-op handler.
  3. *whole-market book build* — full decode plus order-book maintenance
     for all 8,915 symbols (order store, price ladders, invariant
     bookkeeping). Engine construction is outside the timed region; the
     benchmark aborts if any invariant violation occurs during a run.
- **Runs:** every run is reported individually plus the median. Run 1
  may include cold page-cache misses — it is reported, not discarded.
- **I/O is included.** The file is mmap'd; on this 16 GB machine a
  12.95 GB file cannot stay resident in the page cache (the book stage
  alone peaks at ~2.8 GB RSS), so every pass re-faults pages from SSD.
  Run-to-run rates are flat, confirming there is no hidden warm-cache
  advantage in these numbers. A machine with more RAM would report
  higher, cache-warm rates.
- **Single-threaded.** One core does all the work; the P-core count is
  irrelevant to these numbers.

## Results — baseline (map-based book structures)

Apple M4 (4P+6E), 16 GB RAM, macOS; Apple clang 17, `-O3` (CMake Release);
medians below, per-run detail in the benchmark output.

| Stage | median msgs/s | median GB/s | vs previous stage |
|---|---|---|---|
| framing scan | 20.2M | 0.62 | — |
| full decode | 17.5M | 0.54 | field decoding costs ~14% |
| whole-market book build | 3.0M | 0.09 | book maintenance costs ~6.7× the decode |

Reading the table: scan and decode run at the machine's effective
single-thread mmap throughput (~0.6 GB/s here) — they are I/O-bound, and
their medians are within noise of each other. The book stage runs at
0.09 GB/s, far below that floor: it is **not** I/O-bound. The cost is
container overhead — node allocations and pointer-chasing in
`std::map`/`std::unordered_map` — which is exactly what the optimization
pass attacks. Three runs of the book stage landed within 1.1% of each
other (139.4–140.9 s), so the baseline is stable.

Baseline book structures are deliberately naive (`std::map` price
ladders, `std::unordered_map<u64, Order>` order store) — correctness
first. The optimization pass replaces the containers behind the same
interface; the table above is the before picture.

## Optimization pass — what worked and what didn't

Every step was profiled (`sample`), measured on the full-day replay, and
kept only if the full correctness gate still passed (57 unit tests, zero
violations, 8,915/8,915 end-of-day audits). Whole-market book build:

| Change | msgs/s | Verdict |
|---|---|---|
| baseline: `std::map` ladders + `std::unordered_map` store | 3.0M | profile: map ops ≈47% of samples, hash+allocator ≈15% |
| sorted-vector ladders + open-addressing store (fibonacci hash, backshift deletion, contiguous 12-byte orders) | 6.3M | **kept — 2.1×** |
| ladder orientation experiments (asks descending; both descending) | 3.7M / 2.5M | reverted — ascending-both is fastest; churn is asymmetric per side (bid churn skews touch-ward, ask churn deep) |
| O(1) fast path for back()-of-ladder hits | ~6.7M | kept (harmless), no measurable gain |
| structure-of-arrays ladders (prices in own u32 array, 6× search density) | 6.6M | kept (cleaner memory), no measurable gain |
| one-frame lookahead + order-store slot prefetch | 6.9M | kept, marginal |
| **final** | **7.4M median** (bench, 3 runs, 56.1–58.6 s) | **2.5× total** |

Why the 10M target wasn't reached (yet): at 7.4M msgs/s the engine
spends ~135 ns per message, and the order store — ~84 MB of
randomly-probed table, far beyond any cache level — costs roughly a DRAM
round-trip on nearly every book message. Three independent structural
rewrites of the *ladders* landed inside the same 6.6–6.9M band, which is
the signature of a memory-latency bound elsewhere. Next lever:
counter-level profiling (cache-miss rates via Instruments) and a deeper
prefetch pipeline (N-message lookahead queue rather than one frame).

A background-QoS trap worth recording: one replay measured 0.4M msgs/s
(16× slow) because macOS had demoted the process to background I/O
priority — mmap page faults crawl in that state. Benchmarks here are
foreground runs only.

## Latency percentiles

(Engine-processing latencies on file replay — no network jitter, no
queueing; the wire-side story lives in [transport.md](transport.md).)

Per-message latency of the `decode_message` dispatch into the book
engine (`itch_bench --stage latency`), same file, same machine, single
foreground pass: 423,285,709 messages in 65.8 s (6.4M msgs/s with
timers running, vs 7.4M for the untimed book stage — the difference is
the instrumentation).

**Instrumentation:** two `steady_clock::now()` calls per message,
measured at ~23 ns mean per call on this machine; the clock advances in
~41.67 ns ticks (mach_absolute_time timebase), so samples are quantized
to the tick and each carries roughly one call (~23 ns) of overhead —
reported, not subtracted. Framing walk, timestamp read, and the same
one-frame prefetch hint `decode_stream` issues sit outside the timed
span. **Histogram:** fixed bins, O(1) per message — 128 × 8 ns bins to
1.024 µs, then one bin per power of two to ~1 s; percentiles are
reported at the containing bin's upper edge, max is exact.

| phase (ET) | messages | p50 | p99 | p99.9 | p99.99 | max | ≥1 µs |
|---|---|---|---|---|---|---|---|
| pre-open (<09:30) | 13,911,880 | 48 ns | 168 ns | 424 ns | 2.0 µs | 96.5 µs | 3,175 |
| first 30 min | 45,093,814 | 48 ns | 376 ns | 672 ns | 2.0 µs | 14.1 ms | 11,051 |
| mid-day | 315,297,777 | 48 ns | 424 ns | 752 ns | 4.1 µs | 15.2 ms | 125,638 |
| last 30 min | 44,709,195 | 48 ns | 376 ns | 672 ns | 2.0 µs | 143.8 µs | 8,757 |
| post-close (≥16:00) | 4,273,043 | 48 ns | 296 ns | 504 ns | 880 ns | 13.5 µs | 290 |
| **overall** | **423,285,709** | **48 ns** | **424 ns** | **752 ns** | **2.0 µs** | **15.2 ms** | **148,911** |

Reading the table: p50 is one clock tick in every phase — the median
message completes faster than the timer can resolve. The tail does not
peak at the open/close bursts: mid-day carries the worst p99/p99.9/
p99.99. In file replay there is no queueing, so arrival-rate bursts
cannot back messages up; per-message latency tracks data-structure
state instead, and mid-day is when the order store is at peak occupancy
and probes miss cache hardest. The two ms-scale maxima (14.1 ms /
15.2 ms) are the size of a macOS scheduler quantum — preemption or an
mmap page-fault stall landing inside a timed span, not engine work;
0.035% of messages (148,911) took ≥1 µs.

Caveat: this is a file replay. There is no network jitter, NIC, kernel
receive path, or queueing delay in these numbers — they are
engine-processing latencies only, the component a transport stack would
add its own budget on top of.
