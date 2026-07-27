# ITCH 5.0 spec notes

Working notes distilled from the Nasdaq TotalView-ITCH 5.0 specification.
Payload length includes the 1-byte message type. Everything big-endian.
Prices are unsigned fixed-point with 4 decimal places unless noted.
Timestamps are 48-bit nanoseconds since midnight ET.

These notes are hand-written from the spec PDF; week-2 decoder fixtures are
built from this table and then cross-checked against the real file.

## Message type table

| Type | Name | Payload bytes |
|---|---|---|
| `S` | System Event | 12 |
| `R` | Stock Directory | 39 |
| `H` | Stock Trading Action | 25 |
| `Y` | Reg SHO Restriction | 20 |
| `L` | Market Participant Position | 26 |
| `V` | MWCB Decline Level | 35 |
| `W` | MWCB Status | 12 |
| `K` | IPO Quoting Period Update | 28 |
| `J` | LULD Auction Collar | 35 |
| `h` | Operational Halt | 21 |
| `A` | Add Order (no MPID) | 36 |
| `F` | Add Order (MPID attribution) | 40 |
| `E` | Order Executed | 31 |
| `C` | Order Executed With Price | 36 |
| `X` | Order Cancel (partial) | 23 |
| `D` | Order Delete | 19 |
| `U` | Order Replace | 35 |
| `P` | Trade (non-cross) | 44 |
| `Q` | Cross Trade | 40 |
| `B` | Broken Trade | 19 |
| `I` | Net Order Imbalance Indicator | 50 |
| `N` | Retail Price Improvement Indicator | 20 |
| `O` | Direct Listing With Capital Raise | 48 |

## Field offsets — book-critical messages first

Offsets are within the payload; offset 0 is the message type byte.

### `A` — Add Order (36)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type `A` |
| 1 | 2 | stock locate |
| 3 | 2 | tracking number |
| 5 | 6 | timestamp (ns since midnight) |
| 11 | 8 | order reference number |
| 19 | 1 | buy/sell indicator (`B`/`S`) |
| 20 | 4 | shares |
| 24 | 8 | stock symbol (ASCII, right-padded with spaces) |
| 32 | 4 | price |

`F` is `A` plus a 4-byte MPID attribution at offset 36.

### `E` — Order Executed (31)

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | order reference number |
| 19 | 4 | executed shares |
| 23 | 8 | match number |

`C` is `E` plus printable flag (1 byte, offset 31) and execution price
(4 bytes, offset 32); book updates use the *original* order's price, the
execution price is for trade reporting.

### `X` — Order Cancel (23)

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | order reference number |
| 19 | 4 | cancelled shares |

Partial cancel: order stays on book with reduced shares.

### `D` — Order Delete (19)

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | order reference number |

### `U` — Order Replace (35)

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | original order reference number |
| 19 | 8 | new order reference number |
| 27 | 4 | shares |
| 31 | 4 | price |

Replace = delete original + add new (new ref, same side/symbol as
original; side and symbol are *not* in the message — engine must carry
them over from the original order).

## Engine-relevant gotchas

- Order reference numbers are unique per day across all symbols.
- `E`/`C`/`X`/`D`/`U` never repeat the symbol or side — the book engine
  must resolve them via the order reference number.
- Stock locate code in every message is a per-day dense symbol index —
  usable as a direct array index for per-symbol books.
- Cross trades (`Q`) and non-cross trades (`P`) do not mutate the book
  (P executes against non-displayable orders). P's order reference number
  is zero-filled since Dec 2010 — never resolve it against the book.
- Shares in `Q` (cross) are 8 bytes, not 4.
