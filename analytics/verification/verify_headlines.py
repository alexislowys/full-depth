"""Independent recomputation of the repo's headline analytics numbers.

Clean-room by construction: this file imports nothing from
analytics/studies/ and re-derives each quantity from the raw subset
parquets with its own (deliberately simple, scalar-flavoured) code. If a
refactor in the studies ever changes a headline number, this script
fails loudly.

Checks:
  1. AAPL regular-hours time-weighted quoted spread (bps)
     vs liquidity_summary.parquet
  2. AAPL OFI sum + event count for the 10:00-10:01 minute
     vs ofi_1min.parquet
  3. AAPL 60s contemporaneous beta/R^2 vs ofi_regressions.parquet
  4. MSFT 10s contemporaneous beta vs impact_depth.parquet

Run: python verification/verify_headlines.py ../data/export
"""

import sys
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq

RTH_START = 34_200 * 10**9
RTH_END = 57_600 * 10**9
OUT = Path(__file__).resolve().parents[1] / "output"


def load_l1(export_dir: Path, sym: str):
    t = pq.read_table(export_dir / "subsets" / f"{sym}_l1.parquet",
                      columns=["ts_ns", "bid_px", "ask_px", "bid_sz", "ask_sz"])
    return {c: t[c].to_numpy().astype(np.int64) for c in t.column_names}


def tw_spread_bps(d) -> float:
    """Duration-weighted quoted spread, session-clipped, scalar loop."""
    num = den = 0.0
    n = len(d["ts_ns"])
    for i in range(n):
        t0 = d["ts_ns"][i]
        t1 = d["ts_ns"][i + 1] if i + 1 < n else RTH_END
        a, b = max(t0, RTH_START), min(t1, RTH_END)
        if a >= b:
            continue
        bp, ap = d["bid_px"][i], d["ask_px"][i]
        if bp <= 0 or ap <= 0 or bp >= ap:
            continue
        w = b - a
        num += w * (ap - bp) / ((ap + bp) / 2) * 1e4
        den += w
    return num / den


def ofi_events(d):
    """CKS best-level OFI events over valid consecutive state pairs."""
    ts, ev = [], []
    prev = None
    n = len(d["ts_ns"])
    for i in range(n):
        bp, ap = d["bid_px"][i], d["ask_px"][i]
        if bp <= 0 or ap <= 0 or bp >= ap:
            prev = None  # chain break
            continue
        cur = (bp, d["bid_sz"][i], ap, d["ask_sz"][i], d["ts_ns"][i])
        if prev is not None:
            pb, pbs, pa, pas, _ = prev
            if bp > pb:
                bid = cur[1]
            elif bp < pb:
                bid = -pbs
            else:
                bid = cur[1] - pbs
            if ap < pa:
                ask = cur[3]
            elif ap > pa:
                ask = -pas
            else:
                ask = cur[3] - pas
            ts.append(cur[4])
            ev.append(bid - ask)
        prev = cur
    return np.array(ts), np.array(ev, dtype=float)


def bucket_series(d, ets, ev, h_ns):
    """Per-bucket OFI sums and mid changes (ticks); bucket 0 dropped."""
    n_b = (RTH_END - RTH_START) // h_ns
    ofi = np.zeros(n_b)
    rth = (ets >= RTH_START) & (ets < RTH_END)
    for t, e in zip(ets[rth], ev[rth]):
        ofi[(t - RTH_START) // h_ns] += e
    # last valid mid with an update inside each bucket
    close = np.full(n_b, np.nan)
    for i in range(len(d["ts_ns"])):
        t, bp, ap = d["ts_ns"][i], d["bid_px"][i], d["ask_px"][i]
        if t < RTH_START or t >= RTH_END or bp <= 0 or ap <= 0 or bp >= ap:
            continue
        close[(t - RTH_START) // h_ns] = (bp + ap) / 2.0
    chg = (close[1:] - close[:-1]) / 100.0  # ticks
    ok = np.isfinite(chg)
    return ofi[1:][ok], chg[ok]


def ols_beta_r2(x, y):
    xc, yc = x - x.mean(), y - y.mean()
    beta = (xc * yc).sum() / (xc * xc).sum()
    resid = yc - beta * xc
    return beta, 1 - (resid * resid).sum() / (yc * yc).sum()


def close_to(a, b, sig=3) -> bool:
    if b == 0:
        return a == 0
    return abs(a - b) / abs(b) < 10 ** (-sig)


def main(export_dir: str) -> int:
    export_dir = Path(export_dir)
    failures = 0

    def check(name, mine, theirs, exact=False):
        nonlocal failures
        ok = (mine == theirs) if exact else close_to(mine, theirs)
        if not ok:
            failures += 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}: recomputed {mine} "
              f"vs published {theirs}")

    aapl = load_l1(export_dir, "AAPL")

    liq = pq.read_table(OUT / "liquidity_summary.parquet").to_pydict()
    pub_spread = liq["tw_spread_bps"][liq["symbol"].index("AAPL")]
    check("AAPL tw spread bps", round(tw_spread_bps(aapl), 6),
          round(pub_spread, 6))

    ets, ev = ofi_events(aapl)
    lo, hi = 36_000 * 10**9, 36_060 * 10**9
    m = (ets >= lo) & (ets < hi)
    o1 = pq.read_table(OUT / "ofi_1min.parquet").to_pydict()
    idx = [i for i, (s, b) in enumerate(zip(o1["symbol"], o1["bucket_start_ns"]))
           if s == "AAPL" and b == lo][0]
    check("AAPL OFI 10:00 sum", float(ev[m].sum()), float(o1["ofi_sum"][idx]))
    check("AAPL OFI 10:00 events", int(m.sum()), int(o1["event_count"][idx]),
          exact=True)

    x, y = bucket_series(aapl, ets, ev, 60 * 10**9)
    beta, r2 = ols_beta_r2(x, y)
    reg = pq.read_table(OUT / "ofi_regressions.parquet").to_pydict()
    i = [k for k in range(len(reg["symbol"]))
         if reg["symbol"][k] == "AAPL" and reg["horizon_s"][k] == 60
         and reg["design"][k] == "contemporaneous"][0]
    check("AAPL 60s contemporaneous beta", beta,
          reg["beta_ticks_per_share"][i])
    check("AAPL 60s contemporaneous R2", r2, reg["r2"][i])

    msft = load_l1(export_dir, "MSFT")
    ets_m, ev_m = ofi_events(msft)
    xm, ym = bucket_series(msft, ets_m, ev_m, 10 * 10**9)
    beta_m, _ = ols_beta_r2(xm, ym)
    imp = pq.read_table(OUT / "impact_depth.parquet").to_pydict()
    j = imp["symbol"].index("MSFT")
    check("MSFT 10s beta", beta_m, imp["beta_10s"][j])

    print(f"\n{'ALL HEADLINE NUMBERS REPRODUCED' if failures == 0 else str(failures) + ' FAILURES'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1] if len(sys.argv) > 1 else "../data/export"))
