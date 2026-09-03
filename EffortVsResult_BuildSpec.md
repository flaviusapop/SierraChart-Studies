# Effort vs Result — Build Spec v1.0

Standalone Sierra Chart ACSIL study. Filter/context study, not a trade entry
system and not a per-price trapped-zone detector. New files only; no existing
study is modified.

## 1. Identity

- File: `EffortVsResult.cpp`
- `SCDLLName("EffortVsResult")`
- Function: `scsf_EffortVsResult`
- GraphName: `Effort vs Result v1.0`
- `AutoLoop = 0`, default separate lower region (`GraphRegion = 1`)
- Direct VAP source on the SAME chart: `sc.MaintainVolumeAtPriceData = 1`,
  read via `sc.VolumeAtPriceForBars->GetNextHigherVAPElement`. No external
  Numbers Bars study dependency.

## 2. Per-bar computation (closed bars only)

Notation: `O/H/L/C[i]` = open/high/low/close of bar `i`, `TS` = `sc.TickSize`.

### 2.1 BarDelta (effort numerator)

`BarDelta[i] = sum over the chart bar's VAP levels of (AskVolume - BidVolume)`,
accumulated in `int64_t` (`(int64_t)Ask - (int64_t)Bid` per level, as in
TrappedTraders) then stored as float. Bars with no VAP elements yield 0.
If `sc.VolumeAtPriceForBars == nullptr`, all outputs are zero (no fallback
source; the study requires VAP on its own chart).

### 2.2 Effort normalization (no mean subtraction)

- `EffortScale` = EWMA of `abs(BarDelta)` over `Effort Length` (default 50):
  `S += alpha * (absDelta - S)`, `alpha = 2 / (N + 1)`, seeded with the first
  observed `abs(BarDelta)`. Floor: `max(S, 1.0)` (one-contract epsilon floor
  so division is always safe).
- `Effort[i] = BarDelta[i] / EffortScale`, then optionally capped:
  if `Max Normalized Value > 0`, clamp to `[-MaxNorm, +MaxNorm]` (default 5);
  `0` disables the cap.
- House convention: normalize directional effort WITHOUT mean subtraction so
  sustained aggression does not decay toward zero merely because its mean
  shifted (same rationale as FlowConviction v2 change 7).

### 2.3 Result normalization

- `ResultRaw[i]` per `Result Mode` (default 0 = Close-Open):
  - Mode 0: `C[i] - O[i]`
  - Mode 1: `C[i] - C[i-1]` (`i == 0` falls back to `C[0] - O[0]`)
  - Mode 2 (excursion/close-location composite):
    `0.5 * (C[i] - O[i]) + 0.5 * (C[i] - Mid[i])`, `Mid[i] = (H[i]+L[i]) * 0.5`
- `TR[i]` (True Range): `i == 0` gives `H[0] - L[0]`; otherwise
  `max(H-L, |H - C[i-1]|, |L - C[i-1]|)`.
- `TRScale` = EWMA of `TR` over `Result Length` (default 50), seeded with the
  first TR, floored at `max(TRScale, TS)` with `TS` guarded to a small epsilon
  if non-positive.
- `ResultNorm[i] = ResultRaw[i] / TRScale` (dimensionless, in range units).

### 2.4 Reward and failure

- `SignedReward[i] = sign(Effort[i]) * ResultNorm[i]` with
  `sign(x) = +1 / -1 / 0`. Positive means aggressors were rewarded by price;
  negative means price moved against them.
- `FailureMagnitude[i] = abs(Effort[i]) * max(0, Stall - SignedReward[i])`,
  computed ONLY when `abs(Effort[i]) >= Minimum Effort` (default 1.25),
  else 0. Default `Stall Allowance` 0.20, so strong aggression with almost no
  progress registers as failure, not only outright reversal.
- `SignedFailure[i] = -sign(Effort[i]) * FailureMagnitude[i]`:
  - buyers fail (positive delta, poor/down result) -> NEGATIVE (magenta, below zero)
  - sellers fail (negative delta, poor/up result) -> POSITIVE (cyan, above zero)
  - rewarded aggression (`SignedReward >= Stall`) -> exactly 0, never failure
- `SignedFailureSmooth[i]` = EMA of `SignedFailure` over `Smoothing Length`
  (default 3, `alpha = 2/(N+1)`, `N = max(1, input)`; `N = 1` passes through).
  Both raw and smoothed values are exposed as subgraphs.

### 2.5 Sign check (must hold)

| Case | Expected |
|---|---|
| Positive delta + down/flat result | Negative (magenta) buyer failed-aggression evidence |
| Negative delta + up/flat result | Positive (cyan) seller failed-aggression evidence |
| Either sign + strong same-direction result | Zero failure; optionally shown in rewarded-aggression subgraph |

## 3. One-bar-delayed confirmation channel (no future leak)

- Candidate bar `i` requires `abs(Effort[i]) >= Minimum Effort` and ready flag.
- Evaluated when bar `i+1` closes; the pulse/arrow prints ON bar `i+1` using
  only bars `<= i+1`:
  - Failed buyers confirm when candidate `Effort[i] > 0`,
    `H[i+1] <= H[i] + FavTicks * TS` (no extension beyond candidate high),
    and `C[i+1] <= Mid[i] - AdvTicks * TS` (adverse close below midpoint).
    Defaults: `FavTicks = 2`, `AdvTicks = 2`.
  - Failed sellers mirror at candidate low/midpoint with `Effort[i] < 0`,
    `L[i+1] >= L[i] - FavTicks * TS`,
    `C[i+1] >= Mid[i] + AdvTicks * TS`.
- Outputs: buyer-failure confirmation pulse, seller-failure confirmation
  pulse (each holds the smoothed failure value on its bar, else 0), plus
  price-region arrows: buyer failure -> `ARROW_DOWN` at
  `H[i+1] + ArrowOffsetTicks * TS`; seller failure -> `ARROW_UP` at
  `L[i+1] - ArrowOffsetTicks * TS`. No persistent rectangles.
- Gated by `Confirmation Enabled` input; arrows additionally gated by
  `Show Price Arrows`.

## 4. Rendering

- Lower-panel signed histogram/ribbon around zero: smoothed signed failure as
  `DRAWSTYLE_BAR`. Cyan above zero = sellers failing (long evidence);
  magenta below zero = buyers failing (short evidence). `DataColor` blends
  from neutral gray to the user color by `min(1, |v| / MaxNorm)` magnitude.
- Optional rewarded-aggression subgraph (`DRAWSTYLE_BAR`, off by default):
  `SignedReward` when `abs(Effort) >= Minimum Effort` and `SignedReward > 0`,
  else 0, so the display distinguishes failure from directional participation.
- Labels/messages say "failed aggression evidence", never assert known open
  positions (VAP shows executed volume, not trader identity or inventory).

## 5. Inputs (20, index 0-based)

| Idx | Name | Type | Default | Structural? |
|---|---|---|---|---|
| 0 | Effort Length | int 5..500 | 50 | yes |
| 1 | Result Length | int 5..500 | 50 | yes |
| 2 | Smoothing Length | int 1..50 | 3 | yes |
| 3 | Minimum Effort | float 0..20 | 1.25 | yes |
| 4 | Stall Allowance | float 0..2 | 0.20 | yes |
| 5 | Result Mode | Close-Open;Close-PreviousClose;Excursion-CloseLocation | 0 | yes |
| 6 | Max Normalized Value (0 = no cap) | float 0..20 | 5.0 | yes |
| 7 | Confirmation Enabled | yes/no | 1 | yes |
| 8 | Max Favorable Extension Ticks | int 0..50 | 2 | yes |
| 9 | Minimum Adverse Close Ticks | int 0..50 | 2 | yes |
| 10 | Show Price Arrows | yes/no | 1 | yes (changes arrow SGs) |
| 11 | Arrow Offset Ticks | int 0..100 | 2 | yes (changes arrow prices) |
| 12 | Show Rewarded Aggression | yes/no | 0 | yes (changes rewarded SG) |
| 13 | Buyer Failure Color | color | magenta 255,0,255 | no |
| 14 | Seller Failure Color | color | cyan 0,220,220 | no |
| 15 | Buyer Arrow Color | color | magenta 255,0,255 | no |
| 16 | Seller Arrow Color | color | cyan 0,220,220 | no |
| 17 | Rewarded Aggression Color | color | gray-green 0,150,70 | no |
| 18 | Enable Alerts | yes/no | 1 | no (no SG effect) |
| 19 | Alert Sound Number | alert sound | 1 | no (no SG effect) |

Settings fingerprint (persistent int slots 4-6) covers every structural input;
any change forces a full rebuild. Colors and alert inputs are display-only and
excluded.

## 6. Outputs (13 subgraphs, 0-based, all under the 60-SG limit)

| SG | Name | Style | Content |
|---|---|---|---|
| 0 | Raw Bar Delta | LINE | `BarDelta` (contracts) |
| 1 | Effort Scale | LINE_SKIPZEROS | EWMA `abs(BarDelta)` |
| 2 | Normalized Effort | LINE | capped `Effort` |
| 3 | Normalized Result | LINE | `ResultNorm` (TR units) |
| 4 | Signed Reward | LINE | `sign(Effort) * ResultNorm` |
| 5 | Raw Signed Failure | BAR | `SignedFailure` |
| 6 | Smoothed Signed Failure | BAR + DataColor | EMA failure; main display |
| 7 | Buyer Failure Pulse | POINT | smoothed value on confirmed bars, else 0 |
| 8 | Seller Failure Pulse | POINT | smoothed value on confirmed bars, else 0 |
| 9 | Buyer Failure Arrow | ARROW_DOWN | price on confirmed bars, else 0 |
| 10 | Seller Failure Arrow | ARROW_UP | price on confirmed bars, else 0 |
| 11 | Rewarded Aggression | BAR | `SignedReward` when rewarded, else 0 (zeros hidden) |
| 12 | Ready Flag | LINE (1px) | 1 once `i >= max(EffortLen, ResultLen)`, else 0 |

Full recalculation overwrites every bar of every SG (zero stale outputs);
the forming bar is always written as 0 (no provisional values).

## 7. Engineering

- Closed bars only: `lastBar = ArraySize - 1` is forming and never processed;
  the last CLOSED bar is `ArraySize - 2`. Intrabar ticks return early via the
  slot-1 `lastKnownBars` guard. No provisional display (spec permits omitting
  it; forming bar outputs are zero and never fire confirmations/alerts).
- Live-sequential equivalence: full recalc loops `0..lastClosed` rebuilding
  EWMA state from scratch; incremental updates loop over ALL newly closed bars
  `(lastProcessed+1)..lastClosed`, so reconnect/backfill gaps are processed in
  order with identical arithmetic. EWMA/EMA state lives in a plain-C heap
  struct (persistent pointer slot 10; no STL in persistent state). Any
  shrink of `ArraySize` below `lastProcessed`, a null state, or a settings
  change forces a full rebuild.
- Persistent slots: 1 = `lastKnownBars` guard; 2/3 = buyer/seller alert
  watermarks; 4-6 = settings fingerprint words; 10 = state pointer (outside
  the int-slot convention range to avoid collision).
- Alerts: post-loop watermark scan (house pattern). Full recalc fast-forwards
  watermarks with no alerts (no historical alert storm). Incremental: scan the
  last 10 closed bars for the newest un-alerted buyer/seller pulse;
  `sc.SetAlert(sound, ArraySize - 1, msg)` anchored to the current bar, one
  alert per channel per cycle. Messages use "failed aggression evidence".
  Fires only when alerts enabled, sound > 0, and confirmations enabled.
- No float equality on prices (all comparisons are `>`/`>=`/`<`/`<=` on
  computed thresholds, never `==` on OHLC). No Python-style `#` comments.
- Warmup: `Ready[i] = (i >= max(EffortLen, ResultLen))`; confirmations require
  ready. Pre-warmup bars still get deterministic computed values so history
  and live match.
- `LastCallToFunction` frees the heap struct.

## 8. Verification (Linux, no Sierra build system here)

- `python3 gate_check.py EffortVsResult.cpp` must be fully green.
- Independent sign hand-check (§2.5 table) via a Python replica of the
  formulas (buyer-fail negative, seller-fail positive, rewarded zero).
- Self-review: SG count < 60, no lookahead (confirmation prints on `i+1`
  using only `<= i+1` data), warmup flag, settings rebuild, stale-output
  clearing, VAP-null guard.
- Remaining Sierra-side checklist (compile F5, replay): see final report.
