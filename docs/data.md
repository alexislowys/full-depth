# Data provenance

## Source

Nasdaq publishes free full-day TotalView-ITCH 5.0 sample files at
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. No registration, no API key.

## File in use

| Field | Value |
|---|---|
| File | `01302020.NASDAQ_ITCH50.gz` |
| Trading day | Thursday, January 30, 2020 |
| Compressed size | 5,597,158,940 bytes (listed on server) |
| Decompressed size | 12,952,050,754 bytes |
| Messages | 423,285,709 (framing scan, byte accounting exact) |
| MD5 | not available — the server's `.md5sum` link for this file 404s (stale directory listing, checked 2026-07-27). Integrity verified instead by exact size match against the server listing plus byte-exact framing scan (`itch_scan` byte accounting). |

Chosen because it is the most recent sample day on the server.

## Format

- TotalView-ITCH 5.0, big-endian binary.
- Each message framed by a 2-byte big-endian length prefix (length excludes
  the prefix itself); first payload byte is the message type.
- Timestamps: 48-bit nanoseconds since midnight ET.
- Spec: "Nasdaq TotalView-ITCH 5.0" specification PDF, nasdaqtrader.com →
  Technical Support → Specifications → Data Products.

## Local layout

Data lives in `data/` (gitignored — never commit multi-GB files):

```
data/01302020.NASDAQ_ITCH50.gz  # as downloaded
data/01302020.NASDAQ_ITCH50     # gunzip -k output, mmap'd by tools
```
