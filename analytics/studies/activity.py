"""Order-activity study: cancel ratios, trade sizes, intraday volume, auctions, earnings.

Usage: python studies/activity.py /Users/alexislow/Desktop/projects/full-depth/data/export
Nasdaq TotalView-ITCH, Thursday 2020-01-30. All headline stats regular hours
(09:30-16:00 ET) unless labeled otherwise.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

RTH_OPEN = 34_200_000_000_000  # 09:30:00 ET in ns since midnight
RTH_CLOSE = 57_600_000_000_000  # 16:00:00
FIRST30_END = 36_000_000_000_000  # 10:00:00
OPEN_CROSS_CUT = 36_000_000_000_000  # cross before 10:00 -> opening
CLOSE_CROSS_CUT = 56_700_000_000_000  # cross after 15:45 -> closing

EARNINGS_PRIOR = ("TSLA", "FB", "MSFT")  # reported evening 01-29
EARNINGS_TONIGHT = ("AMZN",)              # reports after today's close
EARNINGS = EARNINGS_PRIOR + EARNINGS_TONIGHT
ETFS = ("SPY", "QQQ", "IWM", "TQQQ")

C_NEUTRAL = "#7f7f7f"
C_EARN = "#d62728"
C_ETF = "#1f77b4"


def load_ops(export: Path) -> dict[str, np.ndarray]:
    """ops.csv -> column arrays keyed by header name."""
    with open(export / "ops.csv") as f:
        rows = list(csv.reader(f))
    hdr, data = rows[0], np.array(rows[1:], dtype=np.int64)
    return {name: data[:, i] for i, name in enumerate(hdr)}


def load_locate_map(export: Path) -> dict[int, str]:
    with open(export / "symbols.csv") as f:
        rows = list(csv.reader(f))[1:]
    return {int(loc): sym for loc, sym in rows}


def vw_median_size(shares: np.ndarray) -> float:
    """Trade size below which half of executed volume printed."""
    if shares.size == 0:
        return float("nan")
    s = np.sort(shares.astype(np.int64))
    cum = np.cumsum(s)
    return float(s[np.searchsorted(cum, cum[-1] / 2)])


def read_trades(export: Path, sym: str) -> dict[str, np.ndarray]:
    t = pq.read_table(export / "subsets" / f"{sym}_trades.parquet",
                      columns=["ts_ns", "price", "shares", "kind"])
    return {c: t.column(c).to_numpy() for c in t.column_names}


def fig_cancel_hist(ratios: np.ndarray, subset: dict[str, float],
                    median: float, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 5.5))
    pos = ratios[ratios > 0]
    bins = np.logspace(np.log10(max(pos.min(), 0.1)), np.log10(pos.max()), 60)
    ax.hist(pos, bins=bins, color=C_NEUTRAL, alpha=0.75, edgecolor="white", lw=0.3)
    ax.set_xscale("log")
    ax.axvline(median, color="black", ls="--", lw=1.2,
               label=f"market median = {median:.1f}")
    ymax = ax.get_ylim()[1]
    for i, (sym, r) in enumerate(sorted(subset.items(), key=lambda kv: kv[1])):
        c = C_EARN if sym in EARNINGS else (C_ETF if sym in ETFS else "#2ca02c")
        ax.axvline(r, color=c, lw=0.9, alpha=0.85)
        ax.text(r, ymax * (0.97 - 0.045 * (i % 8)), sym, rotation=90,
                fontsize=6.5, ha="right", va="top", color=c)
    ax.set_xlabel("cancel-to-execute ratio  (cancels + deletes) / executes  [log]")
    ax.set_ylabel("symbols")
    ax.set_title("Cancel-to-execute ratio, all Nasdaq symbols with >=1 execute\n"
                 "2020-01-30 full day, whole market; 20 subset symbols marked "
                 "(red=earnings, blue=ETF, green=other)")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_intraday(minutes: np.ndarray, frac: np.ndarray, out: Path) -> None:
    """frac: (n_sym, n_min) share of each symbol's RTH volume per minute."""
    med = np.nanmedian(frac, axis=0)
    q1, q3 = np.nanpercentile(frac, [25, 75], axis=0)
    hrs = minutes / 60.0
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.fill_between(hrs, np.maximum(q1, 1e-6), q3, color=C_ETF, alpha=0.25,
                    label="IQR across 20 symbols")
    ax.plot(hrs, np.maximum(med, 1e-6), color=C_ETF, lw=1.4,
            label="median across 20 symbols")
    ax.set_yscale("log")
    ax.set_xlabel("hour of day (ET)")
    ax.set_ylabel("share of symbol's RTH volume per minute  [log]")
    ax.set_xticks(np.arange(9.5, 16.5, 0.5))
    ax.set_xlim(9.5, 16.0)
    ax.set_title("Intraday executed volume curve (kinds exec/exec_px), 1-min bins\n"
                 "20 subset symbols, 2020-01-30 regular hours 09:30-16:00 ET")
    ax.legend(fontsize=8)
    ax.grid(True, which="both", alpha=0.25, lw=0.4)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def fig_first30(syms: list[str], shares: list[float], out: Path) -> None:
    order = np.argsort(shares)[::-1]
    syms_o = [syms[i] for i in order]
    vals_o = [shares[i] * 100 for i in order]
    colors = [C_EARN if s in EARNINGS else (C_ETF if s in ETFS else C_NEUTRAL)
              for s in syms_o]
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.bar(syms_o, vals_o, color=colors)
    ax.axhline(30 / 390 * 100, color="black", ls=":", lw=1,
               label="uniform pace (7.7%)")
    ax.set_ylabel("first 30 min share of RTH displayed volume  [%]")
    ax.set_title("First-30-minutes (09:30-10:00) share of regular-hours volume\n"
                 "20 subset symbols, 2020-01-30; red = earnings names "
                 "(TSLA/FB/MSFT prior eve, AMZN tonight), blue = ETF")
    ax.tick_params(axis="x", rotation=60, labelsize=8)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)
    plt.close(fig)


def main() -> None:
    export = Path(sys.argv[1])
    base = Path(__file__).resolve().parent.parent
    out_dir, fig_dir = base / "output", base / "figures"
    out_dir.mkdir(parents=True, exist_ok=True)
    fig_dir.mkdir(parents=True, exist_ok=True)

    subset_syms = (export / "subsets" / "symbols.txt").read_text().split()

    # ---- 1. ops.csv: cancel-to-execute + adds-to-execute, market-wide -------
    ops = load_ops(export)
    loc2sym = load_locate_map(export)
    execs = ops["executes"]
    zero_exec = execs == 0
    n_zero = int(zero_exec.sum())
    with np.errstate(divide="ignore", invalid="ignore"):
        cancel_ratio = np.where(zero_exec, np.nan,
                                (ops["cancels"] + ops["deletes"]) / execs)
        adds_ratio = np.where(zero_exec, np.nan, ops["adds"] / execs)
    valid = ~zero_exec
    mkt_med_cancel = float(np.nanmedian(cancel_ratio[valid]))
    mkt_med_adds = float(np.nanmedian(adds_ratio[valid]))

    sym2row = {loc2sym[loc]: i for i, loc in enumerate(ops["locate"])
               if loc in loc2sym}
    sub_cancel = {s: float(cancel_ratio[sym2row[s]]) for s in subset_syms}
    sub_adds = {s: float(adds_ratio[sym2row[s]]) for s in subset_syms}
    fig_cancel_hist(cancel_ratio[valid], sub_cancel, mkt_med_cancel,
                    fig_dir / "cancel_ratios.png")

    # ---- 2-4. per-symbol trade stats ----------------------------------------
    n_min = 390
    minute_edges = RTH_OPEN + np.arange(n_min + 1, dtype=np.int64) * 60_000_000_000
    frac = np.full((len(subset_syms), n_min), np.nan)
    rows: list[dict] = []
    admin_crosses_total = 0

    for i, sym in enumerate(subset_syms):
        tr = read_trades(export, sym)
        ts, shares, kind = tr["ts_ns"], tr["shares"].astype(np.int64), tr["kind"]

        disp = (kind == 0) | (kind == 1)  # displayed + printable executions
        hidden = kind == 2  # non-displayed executions (P prints)
        rth = (ts >= RTH_OPEN) & (ts < RTH_CLOSE)

        # 1-min volume + intraday fraction (RTH, kinds 0/1)
        m = disp & rth
        vol_1m, _ = np.histogram(ts[m], bins=minute_edges, weights=shares[m])
        rth_vol = int(vol_1m.sum())
        if rth_vol > 0:
            frac[i] = vol_1m / rth_vol
        day_vol = int(shares[disp].sum())  # full day incl. pre/post

        med_sz = vw_median_size(shares[m])

        # auctions: kind 3, shares>0 real; shares==0 administrative
        cross = kind == 3
        admin = cross & (shares == 0)
        admin_crosses_total += int(admin.sum())
        real = cross & (shares > 0)
        open_sh = int(shares[real & (ts < OPEN_CROSS_CUT)].sum())
        close_sh = int(shares[real & (ts >= CLOSE_CROSS_CUT)].sum())
        other_sh = int(shares[real].sum()) - open_sh - close_sh
        # Denominator: all Nasdaq executions (displayed + hidden) plus real
        # cross volume — narrower denominators overstate the close.
        day_exec_all = int(shares[disp | hidden].sum()) + int(
            shares[cross & (shares > 0)].sum())
        close_pct = (100.0 * close_sh / day_exec_all
                     if day_exec_all else float("nan"))

        rth_hidden = shares[hidden & (ts >= RTH_OPEN) & (ts < RTH_CLOSE)].sum()

        # earnings fingerprint: first 30 min share of RTH volume
        f30 = shares[disp & (ts >= RTH_OPEN) & (ts < FIRST30_END)].sum()
        f30_share = f30 / rth_vol if rth_vol else float("nan")
        # robustness: hidden flow included in both numerator and denominator
        f30_all = shares[(disp | hidden) & (ts >= RTH_OPEN) & (ts < FIRST30_END)].sum()
        rth_all = rth_vol + rth_hidden
        f30_share_incl_hidden = f30_all / rth_all if rth_all else float("nan")

        rows.append(dict(symbol=sym,
                         cancel_ratio=sub_cancel[sym],
                         adds_to_exec=sub_adds[sym],
                         vw_median_trade_sz=med_sz,
                         rth_displayed_vol=rth_vol,
                         day_displayed_vol=day_vol,
                         open_cross_sh=open_sh,
                         rth_hidden_vol=int(rth_hidden),
                         f30_share_incl_hidden=f30_share_incl_hidden,
                         close_cross_sh=close_sh,
                         other_cross_sh=other_sh,
                         close_cross_pct=close_pct,
                         first30_share=f30_share,
                         is_earnings=sym in EARNINGS,
                         is_etf=sym in ETFS))

    minutes_mid = (np.arange(n_min) + 0.5) + RTH_OPEN / 60_000_000_000
    fig_intraday(minutes_mid, frac, fig_dir / "volume_intraday.png")
    fig_first30([r["symbol"] for r in rows], [r["first30_share"] for r in rows],
                fig_dir / "first30_share.png")

    # ---- summary parquet ----------------------------------------------------
    table = pa.Table.from_pylist(rows)
    pq.write_table(table, out_dir / "activity_summary.parquet")

    # ---- stdout report ------------------------------------------------------
    earn = [r for r in rows if r["is_earnings"]]
    single = [r for r in rows if not r["is_earnings"] and not r["is_etf"]]
    etf = [r for r in rows if r["is_etf"]]

    print(f"ops.csv: {len(execs)} symbols, {n_zero} with 0 executes "
          f"(excluded from ratio stats)")
    print(f"market median cancel-to-execute: {mkt_med_cancel:.2f}   "
          f"adds-to-execute: {mkt_med_adds:.2f}")
    print(f"administrative crosses (shares==0) filtered in subsets: "
          f"{admin_crosses_total}")
    print()
    hdr = (f"{'symbol':<6} {'cancel':>8} {'adds/ex':>8} {'medTrdSz':>9} "
           f"{'RTHvol':>12} {'openX':>10} {'closeX':>10} {'closeX%':>8} "
           f"{'f30%':>6}")
    print(hdr)
    print("-" * len(hdr))
    for r in sorted(rows, key=lambda r: -r["rth_displayed_vol"]):
        tag = "*" if r["is_earnings"] else ("E" if r["is_etf"] else " ")
        print(f"{r['symbol']:<5}{tag} {r['cancel_ratio']:>8.1f} "
              f"{r['adds_to_exec']:>8.1f} {r['vw_median_trade_sz']:>9.0f} "
              f"{r['rth_displayed_vol']:>12,} {r['open_cross_sh']:>10,} "
              f"{r['close_cross_sh']:>10,} {r['close_cross_pct']:>8.2f} "
              f"{100 * r['first30_share']:>6.1f}")
    print("\n(* earnings name, E = ETF; closeX% = closing cross / all Nasdaq "
          "executions incl. hidden + crosses; f30% = 09:30-10:00 share of RTH "
          "displayed volume)")
    print("(SPY/IWM/VXX/UVXY are NYSE Arca-listed: no Nasdaq opening/closing "
          "cross exists for them - cross columns are n/a, not zero "
          "participation)")
    f = lambda g: float(np.mean([r["first30_share"] for r in g]))
    prior = [r for r in rows if r["symbol"] in EARNINGS_PRIOR]
    tonight = [r for r in rows if r["symbol"] in EARNINGS_TONIGHT]
    print(f"first-30-min share means: reported-prior-evening {100 * f(prior):.1f}%  "
          f"reports-tonight {100 * f(tonight):.1f}%  ")
    print(f"first-30-min share means (legacy grouping): earnings {100 * f(earn):.1f}%  "
          f"other single names {100 * f(single):.1f}%  ETFs {100 * f(etf):.1f}%")


if __name__ == "__main__":
    main()
