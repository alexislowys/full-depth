"""Quoted-liquidity study from L1 streams: time-weighted spread, touch depth,
intraday 1-min structure, and book-state occupancy. Regular hours 09:30-16:00 ET,
Nasdaq 2020-01-30.

Usage: python studies/liquidity.py /Users/alexislow/Desktop/projects/full-depth/data/export
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

RTH_START_NS = 34_200_000_000_000  # 09:30:00 ET
RTH_END_NS = 57_600_000_000_000    # 16:00:00 ET
MIN_NS = 60_000_000_000
N_MIN = (RTH_END_NS - RTH_START_NS) // MIN_NS  # 390

ETFS = {"SPY", "QQQ", "IWM", "TQQQ"}
VOL_ETPS = {"TVIX", "VXX", "UVXY"}

DATE = "2020-01-30"
SESSION = "09:30-16:00 ET"


def weighted_quantile(values: np.ndarray, weights: np.ndarray, q: float) -> float:
    """Duration-weighted quantile: value where cumulative weight fraction first reaches q."""
    order = np.argsort(values)
    v, w = values[order], weights[order]
    cw = np.cumsum(w)
    return float(v[np.searchsorted(cw, q * cw[-1])])


def split_to_minutes(t0: np.ndarray, t1: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Split intervals [t0,t1) (already clipped to RTH) across 1-min bins.

    Returns (interval_idx, minute_idx, sub_duration_ns) with sub-intervals never
    crossing a minute boundary."""
    b0 = ((t0 - RTH_START_NS) // MIN_NS).astype(np.int64)
    b1 = ((t1 - 1 - RTH_START_NS) // MIN_NS).astype(np.int64)  # bin of last covered ns
    counts = b1 - b0 + 1
    idx = np.repeat(np.arange(len(t0)), counts)
    # within-group offset 0..counts[i]-1
    total = int(counts.sum())
    grp_start = np.zeros(len(t0), dtype=np.int64)
    grp_start[1:] = np.cumsum(counts)[:-1]
    offs = np.arange(total) - np.repeat(grp_start, counts)
    bins = b0[idx] + offs
    edge_lo = RTH_START_NS + bins * MIN_NS
    edge_hi = edge_lo + MIN_NS
    sub0 = np.maximum(t0[idx], edge_lo)
    sub1 = np.minimum(t1[idx], edge_hi)
    return idx, bins, (sub1 - sub0).astype(np.float64)


def analyze(sym: str, path: Path) -> tuple[dict, dict]:
    """Per-symbol liquidity stats. Returns (summary_row, per_minute_arrays)."""
    t = pq.read_table(path, columns=["ts_ns", "bid_px", "ask_px", "bid_sz", "ask_sz"])
    ts = t["ts_ns"].to_numpy().astype(np.int64)
    bid = t["bid_px"].to_numpy().astype(np.int64)
    ask = t["ask_px"].to_numpy().astype(np.int64)
    bsz = t["bid_sz"].to_numpy().astype(np.float64)
    asz = t["ask_sz"].to_numpy().astype(np.float64)

    # State i prevails over [ts[i], ts[i+1]); last state runs to end of feed day (20:00).
    nxt = np.empty_like(ts)
    nxt[:-1] = ts[1:]
    nxt[-1] = 72_000_000_000_000  # 20:00 ET
    # Duration weight = overlap of the state's lifetime with regular hours. This is
    # THE weight for every statistic below: change-only stream => row counts are
    # meaningless, seconds-in-state are the measure.
    t0 = np.clip(ts, RTH_START_NS, RTH_END_NS)
    t1 = np.clip(nxt, RTH_START_NS, RTH_END_NS)
    w = (t1 - t0).astype(np.float64)  # ns in RTH
    live = w > 0
    ts, nxt, bid, ask, bsz, asz, t0, t1, w = (
        a[live] for a in (ts, nxt, bid, ask, bsz, asz, t0, t1, w)
    )
    total_w = w.sum()  # == 6.5h in ns unless feed starts after 09:30

    # Book-state classification.
    two = (bid > 0) & (ask > 0) & (bid < ask)
    crossed = (bid > 0) & (ask > 0) & (bid >= ask)
    empty = (bid == 0) & (ask == 0)
    one = ~two & ~crossed & ~empty
    pct = {k: 100.0 * w[m].sum() / total_w for k, m in
           [("two_sided", two), ("one_sided", one), ("empty", empty), ("crossed", crossed)]}
    n_excluded = int(crossed.sum() + empty.sum() + one.sum())

    # Spread stats: two-sided states only. Fixed-point cancels in the ratio.
    spr_bps = (ask[two] - bid[two]) / ((ask[two] + bid[two]) / 2.0) * 1e4
    w2 = w[two]
    tw_spread = float(np.sum(spr_bps * w2) / w2.sum())
    med_spread = weighted_quantile(spr_bps, w2, 0.50)
    p95_spread = weighted_quantile(spr_bps, w2, 0.95)

    # Touch depth (two-sided states): shares and dollars at best bid + best ask.
    depth_sh = bsz[two] + asz[two]
    depth_usd = bsz[two] * bid[two] / 1e4 + asz[two] * ask[two] / 1e4
    tw_depth_sh = float(np.sum(depth_sh * w2) / w2.sum())
    tw_depth_usd = float(np.sum(depth_usd * w2) / w2.sum())

    tw_mid = float(np.sum((ask[two] + bid[two]) / 2e4 * w2) / w2.sum())

    # 1-min time-weighted series (two-sided states, split at minute boundaries).
    idx, bins, subw = split_to_minutes(t0[two], t1[two])
    m_w = np.bincount(bins, weights=subw, minlength=N_MIN)
    m_spr = np.bincount(bins, weights=subw * spr_bps[idx], minlength=N_MIN)
    m_usd = np.bincount(bins, weights=subw * depth_usd[idx], minlength=N_MIN)
    m_sh = np.bincount(bins, weights=subw * depth_sh[idx], minlength=N_MIN)
    with np.errstate(invalid="ignore", divide="ignore"):
        min_spread = np.where(m_w > 0, m_spr / m_w, np.nan)
        min_usd = np.where(m_w > 0, m_usd / m_w, np.nan)
        min_sh = np.where(m_w > 0, m_sh / m_w, np.nan)

    summary = {
        "symbol": sym,
        "tw_spread_bps": tw_spread,
        "med_spread_bps": med_spread,
        "p95_spread_bps": p95_spread,
        "tw_depth_shares": tw_depth_sh,
        "tw_depth_usd": tw_depth_usd,
        "tw_mid": tw_mid,
        "pct_two_sided": pct["two_sided"],
        "pct_one_sided": pct["one_sided"],
        "pct_empty": pct["empty"],
        "pct_crossed": pct["crossed"],
        "n_states_rth": int(len(w)),
        "n_states_excluded": n_excluded,
    }
    minutes = {"spread_bps": min_spread, "depth_usd": min_usd, "depth_shares": min_sh}
    return summary, minutes


def main() -> None:
    export = Path(sys.argv[1])
    root = Path("/Users/alexislow/Desktop/projects/full-depth/analytics")
    out_dir, fig_dir = root / "output", root / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    fig_dir.mkdir(parents=True, exist_ok=True)

    symbols = (export / "subsets" / "symbols.txt").read_text().split()
    rows, per_min = [], {}
    for sym in symbols:
        s, m = analyze(sym, export / "subsets" / f"{sym}_l1.parquet")
        rows.append(s)
        per_min[sym] = m

    rows.sort(key=lambda r: r["tw_spread_bps"])

    # --- output parquet ---
    pq.write_table(
        pa.Table.from_pylist(rows), out_dir / "liquidity_summary.parquet")
    minute_labels = np.arange(N_MIN)
    min_rows = {
        "symbol": [], "minute": [], "min_of_day": [],
        "tw_spread_bps": [], "tw_depth_usd": [], "tw_depth_shares": [],
    }
    for sym in symbols:
        m = per_min[sym]
        min_rows["symbol"].extend([sym] * N_MIN)
        min_rows["minute"].extend(minute_labels.tolist())
        min_rows["min_of_day"].extend((570 + minute_labels).tolist())  # 570 = 09:30
        min_rows["tw_spread_bps"].extend(m["spread_bps"].tolist())
        min_rows["tw_depth_usd"].extend(m["depth_usd"].tolist())
        min_rows["tw_depth_shares"].extend(m["depth_shares"].tolist())
    pq.write_table(pa.table(min_rows), out_dir / "liquidity_1min.parquet")

    # --- figures ---
    hours = 9.5 + minute_labels / 60.0

    fig, axi = plt.subplots(figsize=(10, 6))
    for sym, c in [("AAPL", "tab:blue"), ("TSLA", "tab:red"),
                   ("SPY", "tab:green"), ("LK", "tab:orange")]:
        axi.plot(hours, per_min[sym]["spread_bps"], label=sym, color=c, lw=1.0)
    axi.set_yscale("log")
    axi.set_xlabel("time of day (ET, decimal hours)")
    axi.set_ylabel("1-min time-weighted quoted spread (bps)")
    axi.set_title(f"Intraday quoted spread — AAPL/TSLA/SPY/LK, Nasdaq {DATE}, {SESSION}")
    axi.legend()
    axi.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "spread_intraday.png", dpi=150)
    plt.close(fig)

    fig, axs = plt.subplots(figsize=(9, 7))
    for r in rows:
        sym = r["symbol"]
        grp = ("ETF", "tab:green") if sym in ETFS else \
              ("vol ETP", "tab:purple") if sym in VOL_ETPS else \
              ("single name", "tab:blue")
        axs.scatter(r["tw_spread_bps"], r["tw_depth_usd"], color=grp[1], s=40,
                    label=grp[0])
        axs.annotate(sym, (r["tw_spread_bps"], r["tw_depth_usd"]),
                     textcoords="offset points", xytext=(5, 4), fontsize=8)
    axs.set_xscale("log")
    axs.set_yscale("log")
    axs.set_xlabel("time-weighted quoted spread (bps)")
    axs.set_ylabel("time-weighted touch depth, bid+ask ($)")
    axs.set_title(f"Spread vs touch depth — 20 symbols, Nasdaq {DATE}, {SESSION}")
    h, l = axs.get_legend_handles_labels()
    uniq = dict(zip(l, h))
    axs.legend(uniq.values(), uniq.keys())
    axs.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "spread_vs_depth.png", dpi=150)
    plt.close(fig)

    fig, axd = plt.subplots(figsize=(10, 6))
    for sym, c in [("SPY", "tab:green"), ("QQQ", "tab:olive"),
                   ("AAPL", "tab:blue"), ("TSLA", "tab:red")]:
        axd.plot(hours, per_min[sym]["depth_usd"], label=sym, color=c, lw=1.0)
    axd.set_yscale("log")
    axd.set_xlabel("time of day (ET, decimal hours)")
    axd.set_ylabel("1-min time-weighted touch depth, bid+ask ($)")
    axd.set_title(f"Intraday touch depth — SPY/QQQ vs AAPL/TSLA, Nasdaq {DATE}, {SESSION}")
    axd.legend()
    axd.grid(True, which="both", alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "depth_intraday.png", dpi=150)
    plt.close(fig)

    # --- stdout summary ---
    hdr = (f"{'sym':<6}{'tw_sprd':>9}{'med':>8}{'p95':>9}{'depth_sh':>10}"
           f"{'depth_$':>12}{'%2side':>8}{'%1side':>8}{'%empty':>8}{'%cross':>8}{'excl':>6}")
    print(f"Liquidity summary — Nasdaq {DATE}, regular hours {SESSION}, time-weighted")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['symbol']:<6}{r['tw_spread_bps']:>9.2f}{r['med_spread_bps']:>8.2f}"
              f"{r['p95_spread_bps']:>9.2f}{r['tw_depth_shares']:>10.0f}"
              f"{r['tw_depth_usd']:>12,.0f}{r['pct_two_sided']:>8.3f}"
              f"{r['pct_one_sided']:>8.4f}{r['pct_empty']:>8.4f}"
              f"{r['pct_crossed']:>8.4f}{r['n_states_excluded']:>6}")


if __name__ == "__main__":
    main()
