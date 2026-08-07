"""Numpy dtypes mirroring the C++ packed export structs byte-for-byte.

TradeRecord: 24 bytes. L1Record: 32 bytes. Little-endian, no implicit padding
(structs are #pragma pack(1) on the C++ side; pad fields are explicit and 0).
"""

import numpy as np

TRADE_DTYPE = np.dtype(
    [
        ("ts_ns", "<u8"),  # nanoseconds since midnight ET
        ("price", "<u4"),  # fixed-point, 4 decimal places
        ("shares", "<u4"),
        ("locate", "<u2"),
        ("side", "u1"),  # 'B'=0x42 / 'S'=0x53 resting side; 0 for kind 2/3 without side
        ("kind", "u1"),  # 0=E 1=C(printable) 2=P(hidden) 3=Q(cross)
        ("pad", "<u4"),  # always 0
    ]
)

L1_DTYPE = np.dtype(
    [
        ("ts_ns", "<u8"),
        ("bid_px", "<u4"),  # 0 = bid side empty
        ("ask_px", "<u4"),  # 0 = ask side empty
        ("bid_sz", "<u4"),
        ("ask_sz", "<u4"),
        ("locate", "<u2"),
        ("bid_ct", "<u2"),
        ("ask_ct", "<u2"),
        ("pad", "<u2"),  # always 0
    ]
)

assert TRADE_DTYPE.itemsize == 24, TRADE_DTYPE.itemsize
assert L1_DTYPE.itemsize == 32, L1_DTYPE.itemsize

KIND_NAMES: dict[int, str] = {0: "exec", 1: "exec_px", 2: "hidden", 3: "cross"}

PRICE_SCALE = 10_000  # fixed-point denominator: price / 1e4 = dollars
