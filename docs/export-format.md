# Binary export format

Frozen cross-language contract between the C++ writer
([src/export/exporter.hpp](../src/export/exporter.hpp), static_asserts) and the
Python reader ([analytics/fulldepth/records.py](../analytics/fulldepth/records.py),
import-time asserts). Native-endian (little-endian on every machine this
project targets). Produced by `itch_export <itch-file> <out-dir> [symbol]`.

## trades.bin — TradeRecord, 24 bytes packed

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u64 | ts_ns | nanoseconds since midnight ET |
| 8 | u32 | price | fixed-point, 4 decimals |
| 12 | u32 | shares | Q's u64 shares saturate to u32 max |
| 16 | u16 | locate | |
| 18 | u8 | side | resting order side for kinds 0/1; message side for kind 2; 0 for kind 3 |
| 19 | u8 | kind | 0 = E at resting display price; 1 = C printable only, at execution price; 2 = P hidden-order trade; 3 = Q cross |
| 20 | u32 | pad | always 0 |

Non-printable C executions emit no trade record (they never print to the
tape) but still reduce the displayed book. Administrative Q messages with
shares = 0 are emitted (12,060 on the sample day) — filter `kind == 3 &&
shares == 0` before volume aggregation.

## l1.bin — L1Record, 32 bytes packed

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u64 | ts_ns | of the triggering message |
| 8 | u32 | bid_px | 0 = side empty |
| 12 | u32 | ask_px | 0 = side empty |
| 16 | u32 | bid_sz | shares at best bid, saturating u64→u32 |
| 20 | u32 | ask_sz | |
| 24 | u16 | locate | |
| 26 | u16 | bid_ct | orders at best bid, saturating →u16 |
| 28 | u16 | ask_ct | |
| 30 | u16 | pad | always 0 |

Emitted only when (bid_px, ask_px, bid_sz, ask_sz) changes, tracked per
locate; order counts are recorded but not part of the change test.
All-zero rows are legitimate book-went-empty transitions. Rows with
bid_px ≥ ask_px occur during reopening/IPO auction-pending windows
(215 rows = 0.0001% on the sample day).

## Sidecars

- `symbols.csv` — header `locate,symbol`, one row per directory (R) message, symbols trimmed.
- `meta.txt` — `source=`, `messages=`, `trades=`, `l1=` key=value lines.

## Sample-day results (2020-01-30)

423,285,709 messages → 10,344,031 trade records + 208,792,496 L1 records
(6.9 GB), zero violations. Python-side verification: 19 checks, 0 failed.
Parquet conversion (zstd): l1 2.8 GB, trades 89 MB, 64 s.
