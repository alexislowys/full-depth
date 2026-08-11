"""Best-level Order Flow Imbalance (Cont-Kukanov-Stoikov 2014) from L1 change stream.

Usage: python studies/ofi.py /Users/alexislow/Desktop/projects/full-depth/data/export
Nasdaq 2020-01-30. Regular hours 09:30-16:00 ET for all aggregates.
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

RTH_START = 34_200_000_000_000  # 09:30 ET in ns since midnight
RTH_END = 57_600_000_000_000    # 16:00
N_MIN = 390
TICK_FP = 100                   # $0.01 in fixed-point 1e-4 units

OUT = Path("/Users/alexislow/Desktop/projects/full-depth/analytics/output")
FIG = Path("/Users/alexislow/Desktop/projects/full-depth/analytics/figures")


def compute_symbol(l1_path: Path) -> dict:
    """OFI events + 1-min aggregation for one symbol. Returns dict of arrays/stats."""
    t = pq.read_table(
        l1_path, columns=["ts_ns", "bid_px", "ask_px", "bid_sz", "ask_sz"]
    )
    ts = t["ts_ns"].to_numpy().astype(np.int64)
    bp = t["bid_px"].to_numpy().astype(np.int64)
    ap = t["ask_px"].to_numpy().astype(np.int64)
    bs = t["bid_sz"].to_numpy().astype(np.int64)
    az = t["ask_sz"].to_numpy().astype(np.int64)

    two_sided = (bp > 0) & (ap > 0)
    crossed = two_sided & (bp >= ap)          # auction-pending locked/crossed
    valid = two_sided & (bp < ap)
    n_empty = int((~two_sided).sum())         # empty or one-sided book states
    n_crossed = int(crossed.sum())

    # adjacent state pairs; chain breaks whenever either endpoint is invalid
    pair_ok = valid[:-1] & valid[1:]
    # OFI per CKS: bid contribution - ask contribution
    bp0, bp1 = bp[:-1], bp[1:]
    ap0, ap1 = ap[:-1], ap[1:]
    bs0, bs1 = bs[:-1], bs[1:]
    az0, az1 = az[:-1], az[1:]
    bid_c = np.where(bp1 > bp0, bs1, np.where(bp1 < bp0, -bs0, bs1 - bs0))
    ask_c = np.where(ap1 < ap0, az1, np.where(ap1 > ap0, -az0, az1 - az0))
    e = np.where(pair_ok, bid_c - ask_c, 0)
    ets = ts[1:]                              # event timestamped at new state

    rth_pair = (ets >= RTH_START) & (ets < RTH_END)
    ev_mask = pair_ok & rth_pair
    skipped_rth = int((~pair_ok & rth_pair).sum())
    e_rth = e[ev_mask]
    ets_rth = ets[ev_mask]

    # 1-min buckets over regular hours
    minute = ((ets_rth - RTH_START) // 60_000_000_000).astype(np.int64)
    ofi_sum = np.bincount(minute, weights=e_rth, minlength=N_MIN)
    ev_cnt = np.bincount(minute, minlength=N_MIN).astype(np.int64)

    # last valid mid at each bucket close (mid in fixed-point 1e-4 units)
    vts = ts[valid]
    vmid = (bp[valid] + ap[valid]) / 2.0
    edges = RTH_START + 60_000_000_000 * np.arange(N_MIN + 1)  # bucket boundaries
    idx = np.searchsorted(vts, edges, side="left") - 1          # last state ts < edge
    mid_at_edge = np.where(idx >= 0, vmid[np.clip(idx, 0, None)], np.nan)
    mid_prev, mid_last = mid_at_edge[:-1], mid_at_edge[1:]
    chg_fp = mid_last - mid_prev
    chg_ticks = chg_fp / TICK_FP
    with np.errstate(invalid="ignore", divide="ignore"):
        chg_bps = chg_fp / mid_prev * 1e4

    # contemporaneous OLS: bps ~ ofi_sum, buckets with defined mid change
    fin = np.isfinite(chg_bps)
    x, y = ofi_sum[fin], chg_bps[fin]
    if len(x) > 2 and x.std() > 0 and y.std() > 0:
        r2 = float(np.corrcoef(x, y)[0, 1] ** 2)
        n_minutes = int(len(x))
        slope = float(np.cov(x, y, ddof=0)[0, 1] / x.var())
        icept = float(y.mean() - slope * x.mean())
    else:
        r2, slope, icept = np.nan, np.nan, np.nan
        n_minutes = 0

    return dict(
        ofi_sum=ofi_sum, ev_cnt=ev_cnt, chg_ticks=chg_ticks, chg_bps=chg_bps,
        e_rth=e_rth, ets_rth=ets_rth, r2=r2, slope=slope, icept=icept,
        n_minutes=n_minutes,
        events=int(ev_cnt.sum()), skipped=skipped_rth, total_ofi=float(e_rth.sum()),
        n_empty=n_empty, n_crossed=n_crossed, rows=len(ts),
    )


def main(export_dir: str) -> None:
    sub = Path(export_dir) / "subsets"
    syms = sub.joinpath("symbols.txt").read_text().split()
    OUT.mkdir(parents=True, exist_ok=True)
    FIG.mkdir(parents=True, exist_ok=True)

    res: dict[str, dict] = {}
    for s in syms:
        res[s] = compute_symbol(sub / f"{s}_l1.parquet")

    # ---- parquet deliverable -------------------------------------------------
    rows = []
    for s in syms:
        r = res[s]
        for m in range(N_MIN):
            rows.append((s, m, RTH_START + m * 60_000_000_000,
                         r["ofi_sum"][m], int(r["ev_cnt"][m]),
                         r["chg_ticks"][m], r["chg_bps"][m]))
    cols = list(zip(*rows))
    tbl = pa.table({
        "symbol": pa.array(cols[0], pa.string()),
        "minute": pa.array(cols[1], pa.int32()),
        "bucket_start_ns": pa.array(cols[2], pa.int64()),
        "ofi_sum": pa.array(cols[3], pa.float64()),
        "event_count": pa.array(cols[4], pa.int64()),
        "mid_chg_ticks": pa.array(cols[5], pa.float64()),
        "mid_chg_bps": pa.array(cols[6], pa.float64()),
    })
    pq.write_table(tbl, OUT / "ofi_1min.parquet")

    date_tag = "Nasdaq 2020-01-30, regular hours 09:30-16:00 ET"
    mins = np.arange(N_MIN) / 60.0 + 9.5  # hours ET

    # ---- (a) intraday |OFI| profile -----------------------------------------
    abs_mat = np.vstack([np.abs(res[s]["ofi_sum"]) for s in syms])
    med = np.median(abs_mat, axis=0)
    q1, q3 = np.percentile(abs_mat, [25, 75], axis=0)
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.fill_between(mins, q1, q3, alpha=0.3, color="tab:blue", label="IQR across 20 symbols")
    ax.plot(mins, med, color="tab:blue", lw=1.2, label="median")
    ax.set_yscale("log")
    ax.set_xlabel("time of day (hours ET)")
    ax.set_ylabel("|1-min OFI| (shares, log scale)")
    ax.set_title(f"Intraday |OFI| per 1-min bucket, 20 symbols\n{date_tag}")
    ax.legend()
    fig.tight_layout()
    fig.savefig(FIG / "ofi_intraday.png", dpi=150)
    plt.close(fig)

    # ---- (b) per-event OFI histograms ---------------------------------------
    fig, axes = plt.subplots(1, 3, figsize=(13, 4.2), sharey=True)
    for ax, s in zip(axes, ["AAPL", "TSLA", "SPY"]):
        e = res[s]["e_rth"]
        mx = max(np.abs(e).max(), 10)
        pos = np.logspace(0, np.log10(mx), 40)
        edges = np.r_[-pos[::-1], [0], pos]
        ax.hist(e, bins=edges, color="tab:blue", alpha=0.8)
        ax.set_xscale("symlog", linthresh=1)
        ax.set_yscale("log")
        ax.set_xlabel("OFI per event (shares, symlog)")
        ax.set_title(f"{s} ({len(e):,} events)")
    axes[0].set_ylabel("count (log)")
    fig.suptitle(f"Per-event OFI distribution\n{date_tag}", y=1.02)
    fig.tight_layout()
    fig.savefig(FIG / "ofi_hist.png", dpi=150, bbox_inches="tight")
    plt.close(fig)

    # ---- (c) OFI vs contemporaneous mid change ------------------------------
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8))
    for ax, s in zip(axes, ["AAPL", "SPY"]):
        r = res[s]
        fin = np.isfinite(r["chg_bps"])
        x, y = r["ofi_sum"][fin], r["chg_bps"][fin]
        ax.scatter(x, y, s=9, alpha=0.5, color="tab:blue")
        xs = np.linspace(x.min(), x.max(), 2)
        ax.plot(xs, r["icept"] + r["slope"] * xs, color="tab:red", lw=1.3)
        ax.text(0.03, 0.95,
                f"$R^2$ = {r['r2']:.3f} (n={r['n_minutes']} 1-min buckets)",
                transform=ax.transAxes,
                va="top", fontsize=11)
        ax.axhline(0, color="gray", lw=0.5)
        ax.axvline(0, color="gray", lw=0.5)
        ax.set_xlabel("1-min OFI (shares)")
        ax.set_ylabel("same-minute mid change (bps)")
        ax.set_title(s)
    fig.suptitle(
        f"1-min OFI vs SAME-minute mid change - CONTEMPORANEOUS DESCRIPTIVE, "
        f"not predictive (predictive study: week 9)\n{date_tag}", fontsize=10)
    fig.tight_layout(rect=(0, 0, 1, 0.90))
    fig.savefig(FIG / "ofi_vs_ret_preview.png", dpi=150)
    plt.close(fig)

    # ---- summary table -------------------------------------------------------
    hdr = (f"{'sym':<6}{'events':>9}{'skipped':>9}{'sum_OFI':>13}{'R2_1min':>9}"
           f"{'n_min':>7}{'fd_empty/1side':>15}{'fd_crossed':>11}")
    print(f"OFI study - {date_tag}")
    print(hdr)
    print("-" * len(hdr))
    for s in sorted(syms, key=lambda k: -res[k]["r2"] if np.isfinite(res[k]["r2"]) else 0):
        r = res[s]
        print(f"{s:<6}{r['events']:>9,}{r['skipped']:>9,}{r['total_ofi']:>13,.0f}"
              f"{r['r2']:>9.3f}{r['n_minutes']:>7}{r['n_empty']:>15,}"
              f"{r['n_crossed']:>11,}")
    print(f"\nwrote {OUT / 'ofi_1min.parquet'} ({tbl.num_rows} rows) + 3 figures in {FIG}")


if __name__ == "__main__":
    main(sys.argv[1])
