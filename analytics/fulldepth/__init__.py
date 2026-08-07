"""Readers for the C++ full-depth binary export (trades.bin / l1.bin)."""

from typing import Any

from .reader import load_l1, load_meta, load_symbols, load_trades
from .records import KIND_NAMES, L1_DTYPE, PRICE_SCALE, TRADE_DTYPE

__all__ = [
    "TRADE_DTYPE",
    "L1_DTYPE",
    "KIND_NAMES",
    "PRICE_SCALE",
    "load_trades",
    "load_l1",
    "load_symbols",
    "load_meta",
    "to_parquet",
    "verify",
]


def __getattr__(name: str) -> Any:
    # Lazy: keeps `python -m fulldepth.verify/convert` free of runpy re-import
    # warnings and lets verify run without pyarrow installed.
    if name == "to_parquet":
        from .convert import to_parquet

        return to_parquet
    if name == "verify":
        from .verify import verify

        return verify
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
