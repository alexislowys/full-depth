"""Convert a binary export directory to Parquet.

trades.parquet: pad dropped, symbol joined from symbols.csv, kind kept as int
plus dictionary-encoded kind_name, side as 'B'/'S'/'' string.
l1.parquet: pad dropped, symbol joined. Both written with zstd compression.
"""

import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from .reader import load_l1, load_symbols, load_trades
from .records import KIND_NAMES

_SIDE_NAMES = {0: "", 0x42: "B", 0x53: "S"}


def _symbol_column(locates: np.ndarray, symbols: dict[int, str]) -> pa.Array:
    """Map locate codes to symbol strings via a lookup table (unknown -> '')."""
    table = np.full(int(locates.max(initial=0)) + 1, "", dtype=object)
    for locate, symbol in symbols.items():
        if locate < table.shape[0]:
            table[locate] = symbol
    return pa.array(table[locates], type=pa.string()).dictionary_encode()


def _map_u8(values: np.ndarray, mapping: dict[int, str]) -> pa.Array:
    table = np.full(256, "", dtype=object)
    for k, v in mapping.items():
        table[k] = v
    return pa.array(table[values], type=pa.string()).dictionary_encode()


def to_parquet(export_dir: str | Path, out_dir: str | Path | None = None) -> None:
    """Write trades.parquet and l1.parquet (zstd) to out_dir (default export_dir)."""
    export_dir = Path(export_dir)
    out_dir = Path(out_dir) if out_dir is not None else export_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    symbols = load_symbols(export_dir)

    trades = load_trades(export_dir)
    trades_table = pa.table(
        {
            "ts_ns": trades["ts_ns"],
            "locate": trades["locate"],
            "symbol": _symbol_column(trades["locate"], symbols),
            "price": trades["price"],
            "shares": trades["shares"],
            "side": _map_u8(trades["side"], _SIDE_NAMES),
            "kind": trades["kind"],
            "kind_name": _map_u8(trades["kind"], KIND_NAMES),
        }
    )
    pq.write_table(trades_table, out_dir / "trades.parquet", compression="zstd")

    l1 = load_l1(export_dir)
    l1_table = pa.table(
        {
            "ts_ns": l1["ts_ns"],
            "locate": l1["locate"],
            "symbol": _symbol_column(l1["locate"], symbols),
            "bid_px": l1["bid_px"],
            "ask_px": l1["ask_px"],
            "bid_sz": l1["bid_sz"],
            "ask_sz": l1["ask_sz"],
            "bid_ct": l1["bid_ct"],
            "ask_ct": l1["ask_ct"],
        }
    )
    pq.write_table(l1_table, out_dir / "l1.parquet", compression="zstd")


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print("usage: python -m fulldepth.convert <export_dir> [out_dir]", file=sys.stderr)
        return 2
    to_parquet(argv[1], argv[2] if len(argv) == 3 else None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
