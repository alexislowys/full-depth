"""Extract per-symbol subset Parquet files for the most active symbols.

Selection: top N symbols by displayed-execution trade records (kinds 0/1).
One streaming pass over l1.parquet / trades.parquet with a ParquetWriter
per symbol, so peak memory stays at one row-group regardless of file size.
"""

import sys
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq


def top_symbols(export_dir: Path, n: int) -> list[str]:
    """Top-n symbols by count of kind-0/1 trade records."""
    t = pq.read_table(export_dir / "trades.parquet", columns=["symbol", "kind"])
    displayed = t.filter(pc.is_in(t["kind"], value_set=pa.array([0, 1])))
    counts = displayed.group_by("symbol").aggregate([("kind", "count")])
    counts = counts.sort_by([("kind_count", "descending")])
    return counts["symbol"].slice(0, n).to_pylist()


def _split_stream(src: Path, out_dir: Path, stem: str,
                  symbols: list[str]) -> dict[str, int]:
    keep = set(symbols)
    writers: dict[str, pq.ParquetWriter] = {}
    rows: dict[str, int] = {s: 0 for s in symbols}
    f = pq.ParquetFile(src)
    for batch in f.iter_batches(batch_size=1_000_000):
        table = pa.Table.from_batches([batch])
        for sym in keep:
            part = table.filter(pc.equal(table["symbol"], sym))
            if part.num_rows == 0:
                continue
            if sym not in writers:
                writers[sym] = pq.ParquetWriter(
                    out_dir / f"{sym}_{stem}.parquet", part.schema,
                    compression="zstd")
            writers[sym].write_table(part)
            rows[sym] += part.num_rows
    for w in writers.values():
        w.close()
    return rows


def extract(export_dir: str | Path, n: int = 20,
            out: str | Path | None = None) -> dict[str, dict[str, int]]:
    """Write <SYM>_l1.parquet / <SYM>_trades.parquet subsets; return row counts."""
    export_dir = Path(export_dir)
    out_dir = Path(out) if out else export_dir / "subsets"
    out_dir.mkdir(parents=True, exist_ok=True)
    symbols = top_symbols(export_dir, n)
    l1_rows = _split_stream(export_dir / "l1.parquet", out_dir, "l1", symbols)
    trade_rows = _split_stream(export_dir / "trades.parquet", out_dir,
                               "trades", symbols)
    (out_dir / "symbols.txt").write_text("\n".join(symbols) + "\n")
    return {"l1": l1_rows, "trades": trade_rows}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: python -m fulldepth.extract <export_dir> [n] [out_dir]",
              file=sys.stderr)
        return 2
    n = int(argv[2]) if len(argv) > 2 else 20
    out = argv[3] if len(argv) > 3 else None
    rows = extract(argv[1], n, out)
    for sym in rows["l1"]:
        print(f"{sym:<8} l1 {rows['l1'][sym]:>12,}  trades {rows['trades'].get(sym, 0):>10,}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
