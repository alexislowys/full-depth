"""Price-impact coefficient vs market depth — cross-sectional CKS replication.

Cont-Kukanov-Stoikov (2014) stylized fact: the impact coefficient beta_i from
    mid_change = beta_i * OFI + eps        (per symbol, contemporaneous)
scales inversely with average depth AD_i: log10(beta_i) vs log10(AD_i) is
approximately linear with slope near -1.

Usage: python studies/impact_depth.py /Users/alexislow/Desktop/projects/full-depth/data/export
Run from /Users/alexislow/Desktop/projects/full-depth/analytics.

Scope: ONE day (2020-01-30), ONE venue (Nasdaq TotalView-ITCH), in-sample.
Descriptive microstructure only.

Reuse: OFI events come from studies/ofi.py `compute_symbol` (imported, not
re-implemented) — identical CKS sign conventions, equal-price case, and chain
breaks at invalid book states. Mid-at-bucket-close logic mirrors ofi.py's
valid-state definition (both sides nonzero, bid < ask).

Buckets: half-open [start, end), aligned to 09:30:00 ET. Bucket OFI = sum of
events inside. Bucket mid change = last valid mid in bucket minus last valid
mid in previous bucket, in TICKS (1 tick = $0.01 = 100 fixed-point units).
A bucket with no valid mid inside contributes nothing (no carry-forward): the
affected pairs are dropped and counted. Contemporaneous regression: OFI in
bucket k vs mid change in bucket k (no lookahead claims made — descriptive).

Newey-West: implemented MANUALLY (no statsmodels), Bartlett kernel,
lag L = ceil(1.5 * n**(1/3)), for the per-symbol time-series slope t-stats.
Cross-sectional (n=20 / n=16) slope CI uses classic OLS SE with Student-t.

Depth: liquidity_summary.parquet `tw_depth_shares` is time-weighted touch
depth SUMMED OVER BOTH SIDES (regular hours); we halve it to get per-side
average depth AD_i in shares.
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
from scipy import stats

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ofi  # week-8 study: reuse its OFI event computation verbatim

RTH_START = ofi.RTH_START
RTH_END = ofi.RTH_END
TICK_FP = ofi.TICK_FP
RTH_NS = RTH_END - RTH_START

OUT = Path("/Users/alexislow/Desktop/projects/full-depth/analytics/output")
FIG = Path("/Users/alexislow/Desktop/projects/full-depth/analytics/figures")

HORIZONS_S = [10, 30, 60]

GROUPS = {
    "ETF": ["SPY", "QQQ", "IWM", "TQQQ"],
    "VOL_ETP": ["VXX", "UVXY", "TVIX"],
    "WIDE_TICK": ["AMZN", "TSLA"],
}
# validated categorical palette (all-pairs CVD + normal-vision pass; aqua's
# sub-3:1 contrast relieved by direct symbol labels on every point)
GROUP_COLOR = {
    "ordinary": "#2a78d6",
    "ETF": "#eb6834",
    "VOL_ETP": "#1baf7a",
    "WIDE_TICK": "#4a3aa7",
}
INK, MUTED, GRID = "#0b0b0b", "#898781", "#e1e0d9"

DATE_TAG = "Nasdaq TotalView-ITCH 2020-01-30, regular hours 09:30-16:00 ET"
FOOTNOTE = "Single day, single venue, in-sample. Descriptive microstructure."

# hand-set label offsets (points) where the default (6, 5) collides
LABEL_OFFSETS = {
    "FB": (6, 7), "PYPL": (-30, -4), "QCOM": (7, 1), "TVIX": (-31, -10),
    "MSFT": (8, -4), "MU": (-22, 3), "IWM": (1, -14), "INTC": (7, 5),
    "SPY": (-26, -9), "TQQQ": (-34, 1), "LK": (5, 7), "MDLZ": (7, -11),
    "SBUX": (-33, 3), "QQQ": (7, 4), "AMD": (5, -14),
}


def symbol_group(sym: str) -> str:
    for g, members in GROUPS.items():
        if sym in members:
            return g
    return "ordinary"


def valid_mid_series(l1_path: Path) -> tuple[np.ndarray, np.ndarray]:
    """(ts, mid_fp) at every valid book state — same validity rule as ofi.py."""
    t = pq.read_table(l1_path, columns=["ts_ns", "bid_px", "ask_px"])
    ts = t["ts_ns"].to_numpy().astype(np.int64)
    bp = t["bid_px"].to_numpy().astype(np.int64)
    ap = t["ask_px"].to_numpy().astype(np.int64)
    valid = ((bp > 0) & (ap > 0) & (bp < ap) & (ts >= RTH_START)
             & (ts < RTH_START + RTH_NS))
    return ts[valid], (bp[valid] + ap[valid]) / 2.0


def bucketize(e_rth, ets_rth, vts, vmid, h_ns: int):
    """Per-bucket OFI sums and mid changes (ticks) at horizon h_ns.

    Mid at bucket close = last valid RTH mid with an update inside the
    bucket [edge-h, edge). Bucket 0 has no in-session baseline and is
    dropped, matching ofi_horizons.py exactly. Pairs missing either
    endpoint are dropped (no carry-forward) and counted.
    """
    n_b = RTH_NS // h_ns
    ofi_sum = np.bincount(
        (ets_rth - RTH_START) // h_ns, weights=e_rth, minlength=n_b
    )
    edges = RTH_START + h_ns * np.arange(n_b + 1)
    idx = np.searchsorted(vts, edges, side="left") - 1  # last state ts < edge
    in_bucket = (idx >= 0) & (vts[np.clip(idx, 0, None)] >= edges - h_ns)
    mid_edge = np.where(in_bucket, vmid[np.clip(idx, 0, None)], np.nan)
    chg_ticks = (mid_edge[1:] - mid_edge[:-1]) / TICK_FP
    ok = np.isfinite(chg_ticks)
    return ofi_sum[ok], chg_ticks[ok], int(n_b - ok.sum())


def nw_tstat(x: np.ndarray, y: np.ndarray, slope: float, icept: float) -> float:
    """Newey-West (Bartlett) t-stat for the OLS slope, manual implementation.

    Lag L = ceil(1.5 * n**(1/3)). Score u_t = (x_t - xbar) * resid_t;
    Var(beta) = S / (sum (x-xbar)^2)^2 with S the Bartlett-weighted LRV sum.
    """
    n = len(x)
    lag = math.ceil(1.5 * n ** (1 / 3))
    xd = x - x.mean()
    u = xd * (y - icept - slope * x)
    s = float(u @ u)
    for l in range(1, lag + 1):
        s += 2.0 * (1.0 - l / (lag + 1)) * float(u[l:] @ u[:-l])
    sxx = float(xd @ xd)
    return slope / math.sqrt(s / sxx**2)


def ols(x: np.ndarray, y: np.ndarray):
    """Plain OLS: slope, intercept, R^2, classic slope SE."""
    n = len(x)
    xd = x - x.mean()
    sxx = float(xd @ xd)
    slope = float(xd @ (y - y.mean()) / sxx)
    icept = float(y.mean() - slope * x.mean())
    resid = y - icept - slope * x
    r2 = 1.0 - float(resid @ resid) / float((y - y.mean()) @ (y - y.mean()))
    se = math.sqrt(float(resid @ resid) / (n - 2) / sxx)
    return slope, icept, r2, se


def cross_section(logb: np.ndarray, logd: np.ndarray, label: str) -> dict:
    slope, icept, r2, se = ols(logd, logb)
    n = len(logb)
    tcrit = stats.t.ppf(0.975, n - 2)
    ci = (slope - tcrit * se, slope + tcrit * se)
    return dict(label=label, n=n, slope=slope, icept=icept, r2=r2,
                se=se, ci_lo=ci[0], ci_hi=ci[1])


def main(export_dir: str) -> None:
    sub = Path(export_dir) / "subsets"
    syms = sub.joinpath("symbols.txt").read_text().split()
    OUT.mkdir(parents=True, exist_ok=True)
    FIG.mkdir(parents=True, exist_ok=True)

    # depth: tw_depth_shares sums BOTH sides -> halve for per-side AD_i
    liq = pq.read_table(
        OUT / "liquidity_summary.parquet", columns=["symbol", "tw_depth_shares"]
    ).to_pandas().set_index("symbol")
    ad_shares = {s: float(liq.loc[s, "tw_depth_shares"]) / 2.0 for s in syms}

    rows = []
    for s in syms:
        r = ofi.compute_symbol(sub / f"{s}_l1.parquet")  # reused event calc
        vts, vmid = valid_mid_series(sub / f"{s}_l1.parquet")
        rec: dict = {"symbol": s, "group": symbol_group(s),
                     "AD_shares": ad_shares[s]}
        for h in HORIZONS_S:
            x, y, dropped = bucketize(r["e_rth"], r["ets_rth"], vts, vmid,
                                      h * 1_000_000_000)
            slope, icept, r2, _ = ols(x, y)
            rec[f"beta_{h}s"] = slope          # ticks per share of OFI
            rec[f"r2_{h}s"] = r2
            rec[f"n_{h}s"] = len(x)
            rec[f"dropped_{h}s"] = dropped
            rec[f"t_nw_{h}s"] = nw_tstat(x, y, slope, icept)
        rows.append(rec)

    # ---- parquet deliverable -------------------------------------------------
    cols = ["symbol", "group", "beta_10s", "beta_30s", "beta_60s", "AD_shares",
            "r2_10s", "t_nw_10s", "n_10s", "dropped_10s",
            "n_30s", "dropped_30s", "n_60s", "dropped_60s"]
    tbl = pa.table({c: pa.array([r[c] for r in rows]) for c in cols})
    tbl = tbl.replace_schema_metadata({b"scope": FOOTNOTE.encode()})
    pq.write_table(tbl, OUT / "impact_depth.parquet")

    # ---- cross-sectional regressions ----------------------------------------
    # Log-log fit demands beta > 0; exclude (with a count) rather than
    # abs/clip so a future negative-beta day degrades loudly.
    bpos = np.array([r["beta_10s"] for r in rows]) > 0
    if not bpos.all():
        print("excluded from log-log fit (beta_10s <= 0):",
              [r["symbol"] for r, p in zip(rows, bpos) if not p])
    rows = [r for r, p in zip(rows, bpos) if p]
    logb = np.log10([r["beta_10s"] for r in rows])
    logd = np.log10([r["AD_shares"] for r in rows])
    grp = np.array([r["group"] for r in rows])
    fit_all = cross_section(logb, logd, "all 20")
    m = grp != "ETF"
    fit_ex = cross_section(logb[m], logd[m], "ex-ETF (16)")

    # ---- money plot ----------------------------------------------------------
    fig, ax = plt.subplots(figsize=(9, 6.8))
    ax.set_facecolor("#fcfcfb")
    for g in ["ordinary", "ETF", "VOL_ETP", "WIDE_TICK"]:
        gm = grp == g
        ax.scatter(10 ** logd[gm], 10 ** logb[gm], s=58, zorder=3,
                   color=GROUP_COLOR[g], edgecolors="white", linewidths=0.9,
                   label={"ordinary": "ordinary single name", "ETF": "ETF",
                          "VOL_ETP": "vol ETP", "WIDE_TICK": "wide-tick"}[g])
    for r, lb, ld in zip(rows, logb, logd):
        ax.annotate(r["symbol"], (10 ** ld, 10 ** lb),
                    textcoords="offset points",
                    xytext=LABEL_OFFSETS.get(r["symbol"], (6, 5)),
                    fontsize=8.5, color=INK, zorder=4)
    xs = np.linspace(logd.min() - 0.15, logd.max() + 0.15, 2)
    ax.plot(10 ** xs, 10 ** (fit_all["icept"] + fit_all["slope"] * xs),
            color=INK, lw=1.4, zorder=2,
            label=(f"fit, all 20: slope {fit_all['slope']:.2f} "
                   f"[{fit_all['ci_lo']:.2f}, {fit_all['ci_hi']:.2f}]"))
    ax.plot(10 ** xs, 10 ** (fit_ex["icept"] + fit_ex["slope"] * xs),
            color=INK, lw=1.1, ls=":", zorder=2,
            label=f"fit, ex-ETF: slope {fit_ex['slope']:.2f}")
    # CKS reference slope -1 anchored at the all-20 centroid
    ax.plot(10 ** xs, 10 ** (logb.mean() - 1.0 * (xs - logd.mean())),
            color=MUTED, lw=1.2, ls="--", zorder=1, label="CKS reference slope $-1$")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.grid(True, which="both", color=GRID, lw=0.6, zorder=0)
    ax.tick_params(colors=MUTED, labelsize=9)
    for sp in ax.spines.values():
        sp.set_color(GRID)
    ax.set_xlabel("average touch depth $AD_i$ (shares per side, time-weighted, log scale)",
                  color=INK)
    ax.set_ylabel(r"impact coefficient $\beta_i$ at 10 s (ticks per share of OFI, log scale)",
                  color=INK)
    ax.set_title("Price impact vs depth: $\\log_{10}\\beta_i$ is linear in "
                 f"$\\log_{{10}}AD_i$ (Cont-Kukanov-Stoikov)\n{DATE_TAG}",
                 fontsize=11, color=INK)
    ax.legend(loc="lower left", fontsize=8.5, framealpha=0.9)
    fig.text(0.995, 0.005, FOOTNOTE, ha="right", va="bottom",
             fontsize=7.5, color=MUTED, style="italic")
    fig.tight_layout()
    fig.savefig(FIG / "impact_vs_depth.png", dpi=150)
    plt.close(fig)

    # ---- console report ------------------------------------------------------
    print(f"Impact-vs-depth study — {DATE_TAG}")
    print(f"{FOOTNOTE}\n")
    hdr = (f"{'sym':<6}{'group':<10}{'AD_sh/side':>11}{'beta_10s':>11}"
           f"{'t_NW':>8}{'R2_10s':>8}{'n_10s':>7}{'drop':>6}"
           f"{'beta_30s':>11}{'beta_60s':>11}")
    print(hdr)
    print("-" * len(hdr))
    for r in sorted(rows, key=lambda r: -r["AD_shares"]):
        print(f"{r['symbol']:<6}{r['group']:<10}{r['AD_shares']:>11,.0f}"
              f"{r['beta_10s']:>11.3e}{r['t_nw_10s']:>8.1f}{r['r2_10s']:>8.3f}"
              f"{r['n_10s']:>7}{r['dropped_10s']:>6}"
              f"{r['beta_30s']:>11.3e}{r['beta_60s']:>11.3e}")
    print("\nCross-section: log10(beta_10s) = a + b*log10(AD_shares)   "
          "[classic OLS SE, Student-t 95% CI]")
    for f in (fit_all, fit_ex):
        print(f"  {f['label']:<12} n={f['n']:>2}  slope={f['slope']:+.3f} "
              f"[{f['ci_lo']:+.3f}, {f['ci_hi']:+.3f}]  "
              f"intercept={f['icept']:+.3f}  R2={f['r2']:.3f}")
    print("\nNewey-West t-stats: manual Bartlett implementation, "
          "lag = ceil(1.5*n^(1/3)).")
    print(f"wrote {OUT / 'impact_depth.parquet'} + {FIG / 'impact_vs_depth.png'}")


if __name__ == "__main__":
    main(sys.argv[1])
