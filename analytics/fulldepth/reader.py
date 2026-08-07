"""Loaders for a C++ exporter output directory.

Layout: trades.bin (TradeRecord x N), l1.bin (L1Record x N),
symbols.csv ("locate,symbol" header), meta.txt (key=value lines).
"""

import csv
from pathlib import Path

import numpy as np

from .records import L1_DTYPE, TRADE_DTYPE


def _load_bin(path: Path, dtype: np.dtype) -> np.ndarray:
    size = path.stat().st_size
    if size % dtype.itemsize != 0:
        raise ValueError(
            f"{path}: size {size} not a multiple of record size {dtype.itemsize}"
        )
    return np.fromfile(path, dtype=dtype)


def load_trades(export_dir: str | Path) -> np.ndarray:
    """Load trades.bin as a TRADE_DTYPE structured array."""
    return _load_bin(Path(export_dir) / "trades.bin", TRADE_DTYPE)


def load_l1(export_dir: str | Path) -> np.ndarray:
    """Load l1.bin as an L1_DTYPE structured array."""
    return _load_bin(Path(export_dir) / "l1.bin", L1_DTYPE)


def load_symbols(export_dir: str | Path) -> dict[int, str]:
    """Load symbols.csv -> {locate: symbol}. Symbols are stripped of padding."""
    out: dict[int, str] = {}
    with open(Path(export_dir) / "symbols.csv", newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        if header != ["locate", "symbol"]:
            raise ValueError(f"symbols.csv: unexpected header {header!r}")
        for row in reader:
            if not row:
                continue
            out[int(row[0])] = row[1].strip()
    return out


def load_meta(export_dir: str | Path) -> dict[str, str]:
    """Load meta.txt key=value lines -> dict. Values may contain '='."""
    out: dict[str, str] = {}
    with open(Path(export_dir) / "meta.txt") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            key, _, value = line.partition("=")
            out[key] = value
    return out
