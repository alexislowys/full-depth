"""Multi-horizon OFI vs mid-price change regressions (contemporaneous + one-step-ahead).

Usage: python studies/ofi_horizons.py /Users/alexislow/Desktop/projects/full-depth/data/export
Nasdaq TotalView-ITCH 2020-01-30, regular hours 09:30-16:00 ET.

Event-level OFI is REUSED from the trusted week-8 study (studies/ofi.py,
Cont-Kukanov-Stoikov best-level events) via import: same sign conventions,
equal-price size differencing, chain breaks at empty/one-sided/crossed states.

Conventions:
- Half-open buckets [start, end) aligned to 09:30:00; horizons 10s/30s/60s/300s
  (all divide the 23,400 s session evenly).
- Bucket OFI = sum of events timestamped inside the bucket.
- Bucket mid close = last valid mid (both sides nonzero, bid < ask) with a state
  update timestamped INSIDE the bucket; buckets with no valid in-bucket update
  carry forward nothing -> mid close is NaN, pairs touching it are dropped and
  counted. Mid change in TICKS (1 tick = $0.01 = 100 fixed-point units).
- Contemporaneous: mid change in bucket k ~ OFI in bucket k.
  Predictive (lagged): mid change in bucket k+1 ~ OFI in bucket k. Zero overlap.
- OLS with intercept, plain R^2; Newey-West (HAC) t-stat on the slope via
  statsmodels (already installed, v0.14.4), maxlags = ceil(1.5 * n^(1/3)).

One day, one venue, in-sample. Descriptive microstructure only.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import statsmodels.api as sm

# trusted week-8 event computation (studies/ dir is sys.path[0] when script-run)
from ofi import RTH_START, RTH_END, TICK_FP, compute_symbol

SESSION_S = (RTH_END - RTH_START) // 1_000_000_000  # 23,400
HORIZONS = [10, 30, 60, 300]

OUT = Path("/Users/alexislow/Desktop/projects/full-depth/analytics/output")
FIG = Path("/Users/alexislow/Desktop/projects/full-depth/analytics/figures")

GROUPS = {
    "ETF": ["SPY", "QQQ", "IWM", "TQQQ"],
    "VOL_ETP": ["VXX", "UVXY", "TVIX"],
    "WIDE_TICK": ["AMZN", "TSLA"],
}
GROUP_COLOR = {
    "ETF": "tab:blue",
    "VOL_ETP": "tab:orange",
    "WIDE_TICK": "tab:red",
    "ORDINARY": "tab:gray",
}
DATE_TAG = "Nasdaq TotalView-ITCH 2020-01-30, regular hours 09:30-16:00 ET"
FOOTNOTE = f"{DATE_TAG}. One day, one venue, in-sample; descriptive microstructure only."


def group_of(sym: str) -> str:
    for g, members in GROUPS.items():
        if sym in members:
            return g
    return "ORDINARY"


def valid_mid_states(l1_path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Timestamps + mids (fixed-point 1e-4) of valid book states inside RTH."""
    t = pq.read_table(l1_path, columns=["ts_ns", "bid_px", "ask_px"])
    ts = t["ts_ns"].to_numpy().astype(np.int64)
    bp = t["bid_px"].to_numpy().astype(np.int64)
    ap = t["ask_px"].to_numpy().astype(np.int64)
    m = (bp > 0) & (ap > 0) & (bp < ap) & (ts >= RTH_START) & (ts < RTH_END)
    return ts[m], (bp[m] + ap[m]) / 2.0


def bucketize(ets: np.ndarray, e: np.ndarray, vts: np.ndarray, vmid: np.ndarray,
              h_s: int) -> tuple[np.ndarray, np.ndarray]:
    """Per-bucket OFI sum and mid change (ticks; NaN where undefined)."""
    per = h_s * 1_000_000_000
    nb = SESSION_S // h_s
    b = ((ets - RTH_START) // per).astype(np.int64)
    ofi = np.bincount(b, weights=e, minlength=nb)

    edges = RTH_START + per * np.arange(nb + 1)
    idx = np.searchsorted(vts, edges[1:], side="left") - 1  # last update ts < bucket end
    in_bucket = (idx >= 0) & (vts[np.clip(idx, 0, None)] >= edges[:-1])
    mid_close = np.where(in_bucket, vmid[np.clip(idx, 0, None)], np.nan)

    dmid = np.full(nb, np.nan)
    dmid[1:] = (mid_close[1:] - mid_close[:-1]) / TICK_FP  # ticks
    return ofi, dmid


def nw_ols(x: np.ndarray, y: np.ndarray) -> tuple[int, float, float, float, int]:
    """OLS y ~ const + x. Returns n, slope, plain R^2, NW t-stat, NW lag."""
    n = len(x)
    if n < 10 or x.std() == 0 or y.std() == 0:
        return n, np.nan, np.nan, np.nan, 0
    lag = math.ceil(1.5 * n ** (1.0 / 3.0))
    res = sm.OLS(y, sm.add_constant(x)).fit(cov_type="HAC", cov_kwds={"maxlags": lag})
    return n, float(res.params[1]), float(res.rsquared), float(res.tvalues[1]), lag


def main(export_dir: str) -> None:
    sub = Path(export_dir) / "subsets"
    syms = sub.joinpath("symbols.txt").read_text().split()
    OUT.mkdir(parents=True, exist_ok=True)
    FIG.mkdir(parents=True, exist_ok=True)

    rows = []  # (symbol, group, horizon_s, design, n, beta, r2, nw_t, nw_lag, n_dropped)
    for s in syms:
        r = compute_symbol(sub / f"{s}_l1.parquet")  # trusted week-8 OFI events
        e, ets = r["e_rth"].astype(np.float64), r["ets_rth"]
        vts, vmid = valid_mid_states(sub / f"{s}_l1.parquet")
        g = group_of(s)
        for h in HORIZONS:
            ofi, dmid = bucketize(ets, e, vts, vmid, h)
            nb = len(ofi)
            # contemporaneous: k = 1..nb-1 (dmid[0] needs pre-session close -> undefined)
            fin_c = np.isfinite(dmid)
            n, beta, r2, t, lag = nw_ols(ofi[fin_c], dmid[fin_c])
            rows.append((s, g, h, "contemporaneous", n, beta, r2, t, lag, (nb - 1) - n))
            # predictive: OFI in k vs mid change in k+1, k = 0..nb-2
            fin_p = np.isfinite(dmid[1:])
            n, beta, r2, t, lag = nw_ols(ofi[:-1][fin_p], dmid[1:][fin_p])
            rows.append((s, g, h, "predictive", n, beta, r2, t, lag, (nb - 1) - n))

    cols = list(zip(*rows))
    tbl = pa.table({
        "symbol": pa.array(cols[0], pa.string()),
        "group": pa.array(cols[1], pa.string()),
        "horizon_s": pa.array(cols[2], pa.int32()),
        "design": pa.array(cols[3], pa.string()),
        "n": pa.array(cols[4], pa.int64()),
        "beta_ticks_per_share": pa.array([float(v) for v in cols[5]], pa.float64()),
        "r2": pa.array([float(v) for v in cols[6]], pa.float64()),
        "nw_t": pa.array([float(v) for v in cols[7]], pa.float64()),
        "nw_lag": pa.array(cols[8], pa.int32()),
        "n_dropped": pa.array(cols[9], pa.int64()),
    })
    tbl = tbl.replace_schema_metadata({b"scope": FOOTNOTE.encode()})
    pq.write_table(tbl, OUT / "ofi_regressions.parquet")

    def get(s, h, d):
        for row in rows:
            if row[0] == s and row[2] == h and row[3] == d:
                return row
        raise KeyError((s, h, d))

    # ---- stdout table --------------------------------------------------------
    print(f"OFI multi-horizon regressions - {DATE_TAG}")
    print("mid change (ticks) ~ OFI (shares); beta in ticks per 1,000 shares")
    hdr = (f"{'sym':<6}{'grp':<10}{'h(s)':>5} | {'n_c':>5}{'beta_c':>9}{'R2_c':>8}"
           f"{'tNW_c':>8} | {'n_p':>5}{'beta_p':>9}{'R2_p':>8}{'tNW_p':>8}{'drop':>6}")
    print(hdr)
    print("-" * len(hdr))
    for s in syms:
        for h in HORIZONS:
            c = get(s, h, "contemporaneous")
            p = get(s, h, "predictive")
            print(f"{s:<6}{c[1]:<10}{h:>5} | {c[4]:>5}{c[5]*1e3:>9.3f}{c[6]:>8.3f}"
                  f"{c[7]:>8.2f} | {p[4]:>5}{p[5]*1e3:>9.3f}{p[6]:>8.3f}{p[7]:>8.2f}"
                  f"{p[9]:>6}")
    med_c = {h: float(np.median([get(s, h, 'contemporaneous')[6] for s in syms]))
             for h in HORIZONS}
    med_p = {h: float(np.median([get(s, h, 'predictive')[6] for s in syms]))
             for h in HORIZONS}
    print("\nmedian R2 across 20 symbols:")
    print("horizon(s)      " + "".join(f"{h:>8}" for h in HORIZONS))
    print("contemporaneous " + "".join(f"{med_c[h]:>8.3f}" for h in HORIZONS))
    print("predictive      " + "".join(f"{med_p[h]:>8.3f}" for h in HORIZONS))
    print(f"\nNW lag rule ceil(1.5*n^(1/3)) via statsmodels HAC. {FOOTNOTE}")

    # ---- (a) contemporaneous R2 vs horizon, group medians --------------------
    fig, ax = plt.subplots(figsize=(8, 5))
    order = ["ETF", "VOL_ETP", "WIDE_TICK", "ORDINARY"]
    for g in order:
        members = [s for s in syms if group_of(s) == g]
        med = [float(np.median([get(s, h, "contemporaneous")[6] for s in members]))
               for h in HORIZONS]
        ax.plot(HORIZONS, med, "o-", color=GROUP_COLOR[g],
                label=f"{g} (n={len(members)})")
    ax.set_xscale("log")
    ax.set_xticks(HORIZONS)
    ax.set_xticklabels([str(h) for h in HORIZONS])
    ax.set_xlabel("bucket horizon (seconds, log scale)")
    ax.set_ylabel("contemporaneous $R^2$ (mid change ticks ~ bucket OFI)")
    ax.set_title(f"Contemporaneous OFI fit vs aggregation horizon (group medians)\n{DATE_TAG}")
    ax.legend()
    fig.text(0.5, 0.005, FOOTNOTE, ha="center", fontsize=7, color="gray")
    fig.tight_layout(rect=(0, 0.02, 1, 1))
    fig.savefig(FIG / "r2_vs_horizon.png", dpi=150)
    plt.close(fig)

    # ---- (b) contemporaneous vs predictive R2 at 60s -------------------------
    fig, ax = plt.subplots(figsize=(7, 6))
    lim = 0.0
    for s in syms:
        g = group_of(s)
        xc = get(s, 60, "contemporaneous")[6]
        yp = get(s, 60, "predictive")[6]
        lim = max(lim, xc, yp)
        ax.scatter(xc, yp, color=GROUP_COLOR[g], s=36, zorder=3)
        ax.annotate(s, (xc, yp), textcoords="offset points", xytext=(4, 3), fontsize=7)
    lim = lim * 1.08 + 0.02
    ax.plot([0, lim], [0, lim], color="gray", lw=0.8, ls="--", label="y = x")
    for g in order:
        ax.scatter([], [], color=GROUP_COLOR[g], label=g)
    ax.set_xlim(0, lim)
    ax.set_ylim(0, lim)
    ax.set_xlabel("contemporaneous $R^2$ (60 s buckets)")
    ax.set_ylabel("one-step-ahead predictive $R^2$ (60 s buckets)")
    ax.set_title(f"Same-bucket fit vs next-bucket fit, 60 s horizon\n{DATE_TAG}")
    ax.legend(loc="upper left", fontsize=8)
    fig.text(0.5, 0.005, FOOTNOTE, ha="center", fontsize=7, color="gray")
    fig.tight_layout(rect=(0, 0.02, 1, 1))
    fig.savefig(FIG / "contemp_vs_pred.png", dpi=150)
    plt.close(fig)

    # ---- (c) predictive NW t-stats at 60s ------------------------------------
    ordered = sorted(syms, key=lambda s: get(s, 60, "predictive")[7])
    tvals = [get(s, 60, "predictive")[7] for s in ordered]
    colors = [GROUP_COLOR[group_of(s)] for s in ordered]
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.bar(range(len(ordered)), tvals, color=colors)
    ax.axhline(2, color="black", lw=0.8, ls="--")
    ax.axhline(-2, color="black", lw=0.8, ls="--")
    ax.axhline(0, color="gray", lw=0.5)
    ax.text(len(ordered) - 0.5, 2.05, "|t| = 2", ha="right", va="bottom", fontsize=8)
    ax.set_xticks(range(len(ordered)))
    ax.set_xticklabels(ordered, rotation=60, fontsize=8)
    ax.set_ylabel("Newey-West t-stat, predictive slope (60 s buckets)")
    ax.set_title(f"One-step-ahead slope t-stats, mid change k+1 ~ OFI k, 60 s\n{DATE_TAG}")
    handles = [plt.Rectangle((0, 0), 1, 1, color=GROUP_COLOR[g]) for g in order]
    ax.legend(handles, order, fontsize=8)
    fig.text(0.5, 0.005, FOOTNOTE, ha="center", fontsize=7, color="gray")
    fig.tight_layout(rect=(0, 0.02, 1, 1))
    fig.savefig(FIG / "pred_tstats.png", dpi=150)
    plt.close(fig)

    print(f"wrote {OUT / 'ofi_regressions.parquet'} ({tbl.num_rows} rows) + "
          f"r2_vs_horizon.png, contemp_vs_pred.png, pred_tstats.png in {FIG}")


if __name__ == "__main__":
    main(sys.argv[1])
