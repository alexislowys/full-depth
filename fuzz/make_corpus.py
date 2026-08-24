#!/usr/bin/env python3
"""Seed corpus generator for decode_fuzz.

Builds framed ITCH 5.0 streams from the spec offset tables — same oracle
as tests/messages_test.cpp: 2-byte big-endian length prefix, payload
starting with the type byte. Payload lengths are asserted against
expected_length() in src/itch/messages.cpp. Writes fuzz/corpus/ next to
this file; run from anywhere.
"""

import struct
from pathlib import Path

OUT = Path(__file__).resolve().parent / "corpus"

# type byte -> payload length (incl. type byte), from expected_length().
EXPECTED = {"S": 12, "A": 36, "D": 19, "E": 31, "U": 35}

TS = 34_200_000_000_007  # 09:30:00.000000007 ET, needs all 48 bits' width


def u16(v):
    return struct.pack(">H", v)


def u32(v):
    return struct.pack(">I", v)


def u48(v):
    return struct.pack(">Q", v)[2:]


def u64(v):
    return struct.pack(">Q", v)


def alpha(s, n):
    """Alpha field: right-padded with spaces to n bytes."""
    return s.encode("ascii").ljust(n, b" ")


def header(t):
    return t.encode("ascii") + u16(0x1234) + u16(0x5678) + u48(TS)


def framed(payload):
    return u16(len(payload)) + payload


def check(t, payload):
    assert len(payload) == EXPECTED[t], (t, len(payload))
    return framed(payload)


def system_event(code):
    return check("S", header("S") + code.encode("ascii"))


def add_order(ref, side, shares, stock, price):
    return check("A", header("A") + u64(ref) + side.encode("ascii")
                 + u32(shares) + alpha(stock, 8) + u32(price))


def order_executed(ref, shares, match):
    return check("E", header("E") + u64(ref) + u32(shares) + u64(match))


def order_replace(orig, new, shares, price):
    return check("U", header("U") + u64(orig) + u64(new) + u32(shares)
                 + u32(price))


def order_delete(ref):
    return check("D", header("D") + u64(ref))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    seeds = {
        # Order lifecycle across all five seeded types.
        "seed_session.bin":
            system_event("Q")
            + add_order(42, "B", 300, "NVDA", 2504500)
            + order_executed(42, 100, 7)
            + order_replace(42, 43, 200, 2504600)
            + order_delete(43),
        "seed_single_add.bin": add_order(1, "S", 100, "AAPL", 3180000),
        "seed_deletes.bin":
            order_delete(0) + order_delete(0xFFFF_FFFF_FFFF_FFFF),
        # Prefix promises a 36-byte payload; stream ends 18 bytes in.
        "seed_truncated.bin":
            system_event("O") + add_order(9, "B", 1, "T", 1)[:20],
        # 'Z' is not an ITCH 5.0 type; decode must stop, not crash.
        "seed_unknown_type.bin": framed(header("Z") + b"Q"),
    }
    for name, blob in seeds.items():
        (OUT / name).write_bytes(blob)
        print(f"{name}: {len(blob)} bytes")


if __name__ == "__main__":
    main()
