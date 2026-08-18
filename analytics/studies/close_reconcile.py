"""Closing-cross reconciliation: engine auction prints vs official Yahoo closes.

Usage: python studies/close_reconcile.py

For each subset symbol, pull the Nasdaq closing cross (kind-3 print, shares>0,
~16:00:00 ET) rebuilt by the C++ engine and compare against Yahoo Finance's
unadjusted daily close for 2020-01-30. For Nasdaq-listed names the official
close IS the closing cross price, so they should match to the cent.
SPY/IWM/VXX/UVXY are NYSE Arca-listed (no Nasdaq closing cross) -> n/a.
Yahoo responses are cached to data/export/yahoo_cache/ so reruns are offline.

Gotcha: Yahoo's indicators.quote[0].close is UNADJUSTED for dividends but IS
adjusted for later splits (AAPL 4:1 2020-08, AMZN 20:1 2022-06, ...). We fetch
each ticker's split events and multiply the close back to the raw 2020-01-30
print before comparing.
"""

from __future__ import annotations

import csv
import json
import time
import urllib.error
import urllib.request
from pathlib import Path

import pyarrow.parquet as pq

ROOT = Path("/Users/alexislow/Desktop/projects/full-depth")
SUBSETS = ROOT / "data/export/subsets"
CACHE = ROOT / "data/export/yahoo_cache"
OUT_CSV = ROOT / "analytics/output/close_reconcile.csv"

CROSS_LO = 57_590_000_000_000  # 15:59:50 ET in ns since midnight
CROSS_HI = 57_900_000_000_000  # 16:05:00
ARCA = {"SPY", "IWM", "VXX", "UVXY"}  # NYSE Arca-listed: no Nasdaq close
RENAMES = {"FB": "META"}  # tickers to retry under a new name

# 2020-01-30 00:00 ET .. 2020-01-31 00:00 ET
PERIOD1, PERIOD2 = 1_580_360_400, 1_580_446_800
YAHOO_URL = (
    "https://query1.finance.yahoo.com/v8/finance/chart/"
    "{sym}?period1={p1}&period2={p2}&interval=1d"
)
SPLITS_URL = (
    "https://query1.finance.yahoo.com/v8/finance/chart/"
    "{sym}?period1={p1}&period2={p2}&interval=3mo&events=split"
)


def engine_closing_cross(sym: str) -> tuple[float, int] | tuple[None, None]:
    """Last kind-3 print with shares>0 in the closing window -> ($, shares)."""
    t = pq.read_table(SUBSETS / f"{sym}_trades.parquet",
                      columns=["ts_ns", "price", "shares", "kind"])
    ts = t.column("ts_ns").to_numpy()
    kind = t.column("kind").to_numpy()
    shares = t.column("shares").to_numpy()
    mask = (kind == 3) & (shares > 0) & (ts >= CROSS_LO) & (ts <= CROSS_HI)
    if not mask.any():
        return None, None
    idx = mask.nonzero()[0]
    last = idx[ts[idx].argmax()]
    return t.column("price").to_numpy()[last] / 1e4, int(shares[last])


def fetch_yahoo_json(ticker: str, url: str, tag: str = "") -> dict | None:
    """Chart JSON for ticker, cache-first; None on failure (cached too)."""
    hit = CACHE / f"{ticker}{tag}.json"
    miss = CACHE / f"{ticker}{tag}.unavailable"
    if hit.exists():
        return json.loads(hit.read_text())
    if miss.exists():
        return None
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode())
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as e:
        print(f"  [yahoo] {ticker}{tag}: fetch failed ({e})")
        miss.write_text(str(e))
        return None
    if data.get("chart", {}).get("error") or not data.get("chart", {}).get("result"):
        print(f"  [yahoo] {ticker}{tag}: no result in response")
        miss.write_text(json.dumps(data))
        return None
    hit.write_text(json.dumps(data))
    return data


def split_factor(ticker: str) -> float:
    """Cumulative split ratio applied by Yahoo since 2020-01-30 (1.0 if none).

    Yahoo divides historical closes by later split ratios; multiplying by this
    factor recovers the raw print. Kept as an integer ratio to dodge float drift.
    """
    url = SPLITS_URL.format(sym=ticker, p1=PERIOD2, p2=int(time.time()))
    data = fetch_yahoo_json(ticker, url, tag="_splits")
    if data is None:
        return 1.0  # unknown -> compare against adjusted close as-is
    splits = data["chart"]["result"][0].get("events", {}).get("splits", {})
    num = den = 1
    for s in splits.values():
        num *= int(s["numerator"])
        den *= int(s["denominator"])
    return num / den


def yahoo_close(sym: str) -> tuple[float | None, str, float]:
    """Unadjusted 2020-01-30 close -> ($, source_ticker, split_factor)."""
    tickers = [sym] + ([RENAMES[sym]] if sym in RENAMES else [])
    for ticker in tickers:
        url = YAHOO_URL.format(sym=ticker, p1=PERIOD1, p2=PERIOD2)
        data = fetch_yahoo_json(ticker, url)
        if data is None:
            continue
        result = data["chart"]["result"][0]
        stamps = result.get("timestamp", [])
        closes = result["indicators"]["quote"][0].get("close", [])
        for t, c in zip(stamps, closes):
            if PERIOD1 <= t < PERIOD2 and c is not None:
                factor = split_factor(ticker)
                return float(c) * factor, ticker, factor
    return None, "unavailable", 1.0


def main() -> None:
    CACHE.mkdir(parents=True, exist_ok=True)
    OUT_CSV.parent.mkdir(parents=True, exist_ok=True)
    symbols = sorted(
        p.name.removesuffix("_trades.parquet")
        for p in SUBSETS.glob("*_trades.parquet")
    )

    rows = []
    for sym in symbols:
        eng, shares = engine_closing_cross(sym)
        yah, src, factor = yahoo_close(sym)
        if sym in ARCA:
            verdict = "n/a"
        elif eng is None or yah is None:
            verdict = "unavailable"
        else:
            verdict = "MATCH" if abs(eng - yah) <= 0.005 else "MISMATCH"
        diff_c = (eng - yah) * 100 if (eng is not None and yah is not None) else None
        notes = []
        if src not in (sym, "unavailable"):
            notes.append(f"via {src}")
        if abs(factor - 1.0) > 1e-9:
            notes.append(f"de-split x{factor:g}")
        note = ", ".join(notes)
        rows.append((sym, eng, shares, yah, diff_c, verdict, note))

    hdr = ("symbol", "engine_cross_usd", "cross_shares", "yahoo_close_usd",
           "diff_cents", "verdict", "note")
    fmt = lambda v, spec: format(v, spec) if v is not None else "-"
    print(f"{'symbol':<7}{'engine cross $':>15}{'yahoo close $':>15}"
          f"{'diff cents':>12}  {'verdict':<12}{'note'}")
    for sym, eng, shares, yah, diff_c, verdict, note in rows:
        print(f"{sym:<7}{fmt(eng, '.4f'):>15}{fmt(yah, '.2f'):>15}"
              f"{fmt(diff_c, '+.2f'):>12}  {verdict:<12}{note}")

    with open(OUT_CSV, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(hdr)
        for sym, eng, shares, yah, diff_c, verdict, note in rows:
            w.writerow([sym,
                        fmt(eng, ".4f"), shares if shares is not None else "-",
                        fmt(yah, ".2f"), fmt(diff_c, "+.2f"), verdict, note])

    listed = [r for r in rows if r[0] not in ARCA]
    matched = sum(r[5] == "MATCH" for r in listed)
    avail = sum(r[5] in ("MATCH", "MISMATCH") for r in listed)
    print(f"\n{matched}/{avail} Nasdaq-listed symbols with Yahoo data MATCH "
          f"({len(listed) - avail} unavailable, {len(rows) - len(listed)} Arca n/a)")
    print(f"csv -> {OUT_CSV}")


if __name__ == "__main__":
    main()
