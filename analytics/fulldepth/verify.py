"""Integrity checks for a binary export directory.

Hard failures (FAIL) exit nonzero from main. Crossed L1 (bid_px >= ask_px with
both sides nonzero) is INFO — legitimate around reopening auctions — unless it
exceeds 0.5% of rows.
"""

import sys
from pathlib import Path

import numpy as np

from .reader import load_meta, load_symbols
from .records import L1_DTYPE, TRADE_DTYPE

_DAY_NS = 86_400_000_000_000  # 24h in ns
_CROSSED_FAIL_SHARE = 0.005

Check = dict[str, str]  # {"status": PASS|FAIL|INFO, "detail": str}


def _check(ok: bool, detail: str) -> Check:
    return {"status": "PASS" if ok else "FAIL", "detail": detail}


def verify(export_dir: str | Path) -> dict[str, Check]:
    """Run all checks; return {check_name: {"status", "detail"}}."""
    export_dir = Path(export_dir)
    meta = load_meta(export_dir)
    symbols = load_symbols(export_dir)
    results: dict[str, Check] = {}

    streams = {
        "trades": (export_dir / "trades.bin", TRADE_DTYPE),
        "l1": (export_dir / "l1.bin", L1_DTYPE),
    }
    arrays: dict[str, np.ndarray] = {}
    for name, (path, dtype) in streams.items():
        size = path.stat().st_size
        results[f"{name}_size"] = _check(
            size % dtype.itemsize == 0,
            f"{size} bytes, record size {dtype.itemsize}",
        )
        arr = np.fromfile(path, dtype=dtype)  # reads whole records only
        arrays[name] = arr
        expected = meta.get(name, "<missing>")
        results[f"{name}_count_meta"] = _check(
            expected == str(len(arr)),
            f"file {len(arr)} vs meta {expected}",
        )
        in_day = bool(np.all(arr["ts_ns"] < _DAY_NS)) if len(arr) else True
        results[f"{name}_ts_range"] = _check(in_day, f"ts_ns < {_DAY_NS}")
        monotonic = bool(np.all(np.diff(arr["ts_ns"].astype(np.int64)) >= 0))
        results[f"{name}_ts_monotonic"] = _check(monotonic, "non-decreasing ts_ns")
        unknown = np.setdiff1d(np.unique(arr["locate"]), np.array(sorted(symbols), dtype=np.uint16))
        results[f"{name}_locates"] = _check(
            unknown.size == 0,
            "all in symbols.csv" if unknown.size == 0 else f"unknown locates {unknown.tolist()[:10]}",
        )

    t = arrays["trades"]
    kind_ok = np.isin(t["kind"], (0, 1, 2, 3))
    results["trade_kind"] = _check(bool(kind_ok.all()), "kind in {0,1,2,3}")
    displayed = np.isin(t["kind"], (0, 1))
    side_bs = np.isin(t["side"], (0x42, 0x53))
    side_ok = np.where(displayed, side_bs, side_bs | (t["side"] == 0))
    results["trade_side"] = _check(
        bool(side_ok.all()), "B/S for kind 0/1; 0/B/S for kind 2/3"
    )
    # Kind 3 (cross) is exempt: ITCH disseminates administrative Q messages
    # with shares=0 and price=0 (no-volume and halt-resumption crosses) —
    # 12,060 of 17,835 crosses on the 2020-01-30 sample day.
    nz = t["kind"] != 3
    results["trade_price"] = _check(
        bool((t["price"][nz] > 0).all()), "price > 0 (kind 0/1/2)"
    )
    results["trade_shares"] = _check(
        bool((t["shares"][nz] > 0).all()), "shares > 0 (kind 0/1/2)"
    )
    zero_crosses = int(((~nz) & (t["shares"] == 0)).sum())
    results["trade_zero_share_crosses"] = {
        "status": "INFO",
        "detail": f"{zero_crosses} administrative kind-3 records with shares == 0",
    }
    results["trade_pad"] = _check(bool((t["pad"] == 0).all()), "pad == 0")

    l1 = arrays["l1"]
    results["l1_pad"] = _check(bool((l1["pad"] == 0).all()), "pad == 0")
    # All-zero rows are legitimate book-went-empty snapshots (the change-only
    # emission contract requires them); the initial all-zero state is never
    # emitted, so each one records a real transition.
    empty_rows = int((((l1["bid_px"] == 0) & (l1["ask_px"] == 0))).sum())
    results["l1_empty_book_rows"] = {
        "status": "INFO",
        "detail": f"{empty_rows} book-went-empty snapshots",
    }
    both = (l1["bid_px"] != 0) & (l1["ask_px"] != 0)
    crossed = int((both & (l1["bid_px"] >= l1["ask_px"])).sum())
    share = crossed / len(l1) if len(l1) else 0.0
    results["l1_crossed_rows"] = {
        "status": "FAIL" if share > _CROSSED_FAIL_SHARE else "INFO",
        "detail": f"{crossed} rows, {share:.4%} of {len(l1)} (fail > {_CROSSED_FAIL_SHARE:.1%})",
    }
    return results


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: python -m fulldepth.verify <export_dir>", file=sys.stderr)
        return 2
    results = verify(argv[1])
    width = max(len(k) for k in results)
    for name, r in results.items():
        print(f"{r['status']:<4}  {name:<{width}}  {r['detail']}")
    failed = sum(r["status"] == "FAIL" for r in results.values())
    print(f"{len(results)} checks, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
