# Week 9 — Order flow imbalance and price impact: the study

The capstone analysis: does the relationship between order-flow
imbalance and price movement documented by Cont, Kukanov & Stoikov
(2014) reproduce on a single day of Nasdaq TotalView-ITCH data, rebuilt
from the raw feed by this project's own C++ engine?

Scope stated up front: **one day (2020-01-30), one venue, in-sample,
descriptive microstructure — not a trading signal.** Every table,
figure, and parquet in this study carries that caveat (parquets embed it
as schema metadata).

## Result 1 — Price impact scales inversely with depth

Per symbol, the 10-second contemporaneous impact coefficient β
(mid-price change in ticks per share of best-level OFI) against
time-weighted touch depth, cross-sectionally, log-log:

> **slope = −1.26 [95% CI −1.54, −0.98], R² = 0.83** (all 20 symbols)
> slope = −1.23 [−1.54, −0.92], R² = 0.84 (excluding index ETFs)

CKS's reference value of −1 sits inside the confidence interval in both
specifications. Doubling visible depth roughly halves price impact —
reproduced from scratch, feed bytes to regression, in one repo.
The deviations are informative, not noise:

- **AMZN, TSLA sit ~3–5× above the line** — multi-dollar spreads mean
  10s mid changes come from quote repositioning across ticks, not
  depletion of the thin visible touch.
- **SPY, IWM sit below** — Nasdaq-only touch depth understates their
  consolidated cross-venue liquidity, so measured β is small relative
  to *visible* depth.

Figure: `analytics/figures/impact_vs_depth.png`.

## Result 2 — Contemporaneous fit is strong; prediction is not

Multi-horizon regressions (10s/30s/60s/300s), contemporaneous vs
strictly one-step-ahead (bucket k OFI vs bucket k+1 mid change, zero
overlap):

| | median R² (20 symbols) |
|---|---|
| contemporaneous, any horizon | **0.48–0.53** |
| one-step-ahead, any horizon | **≤ 0.012** |

At 60s, 17 of 20 symbols show no significant predictive slope
(Newey–West |t| < 2), and the three that clear the bar (MSFT −3.36,
LK −2.06, FB −1.92) are *negative* — mild next-minute reversal on an
earnings-digestion day, not momentum. Same-bucket explanatory power near
0.5 coexisting with next-bucket power near zero is the honest headline:
best-level OFI describes price formation; it does not forecast it at
these horizons on this day.

The CKS aggregation effect (R² strengthening with bucket size) shows up
as instrument-dependent, not universal: single names and vol ETPs
plateau or improve toward 300s (VXX 0.86→0.91), while index ETFs decay
sharply (SPY 0.33→0.13) — consistent with their mids being driven by
cross-venue flow a single venue's L1 cannot see.

## Verification

An adversarial reviewer reproduced, from the raw subset parquets with
independent code: AAPL's 60s contemporaneous beta/R² and NW t
(match to machine precision), AAPL's 60s predictive R² (exact), MSFT's
10s beta and its position in the log-log plot (exact), and the
depth-halving convention. One convention inconsistency it caught
(bucket-0 baseline mid taken from pre-open in `impact_depth.py`) was
fixed; both studies now share the identical bucket convention and their
overlapping betas agree to machine precision. The headline slope moved
by less than 0.001 under the fix.

## Files

- `analytics/studies/ofi_horizons.py` → `output/ofi_regressions.parquet` (160 regressions), figures `r2_vs_horizon.png`, `contemp_vs_pred.png`, `pred_tstats.png`
- `analytics/studies/impact_depth.py` → `output/impact_depth.parquet`, figure `impact_vs_depth.png`
