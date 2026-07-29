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

Apple M4 (4P+6E), 16 GB RAM, macOS; Apple clang 17, `-O2` Release;
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
interface; this table is the before picture.
