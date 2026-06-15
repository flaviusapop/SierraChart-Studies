# Breadth Composite Oscillator — Sierra Chart ACSIL Study
## Project Specification v1.3

---

## Version History

| Version | Change |
|---|---|
| v1.0 | Initial spec: $TICK + $VOLD + $ADD, dual Z-score normalization |
| v1.2 | Removed $TICK. Replaced with ROC derivatives of $VOLD and $ADD to capture breadth momentum (acceleration/deceleration) rather than point-in-time tick snapshots |
| v1.3 | Fixed sign/direction bug in pure ROC approach by blending absolute level Z with ROC Z per indicator via `LevelROC_Blend` input |

---

## Overview

A Sierra Chart ACSIL custom study that computes a **composite breadth oscillator** from two NYSE breadth indicators — $VOLD and $ADD — each normalized via a blend of absolute level and rate-of-change (ROC), processed through dual Z-scores (session-adaptive + multi-session regime), blended into a single oscillator line with individual component lines for divergence visibility.

Primary use case: **ES futures scalping bias filter** on a 10-point range bar chart.

### v1.3 Design Rationale
Pure ROC had a sign/direction bug: when VOLD or ADD is in negative territory (bearish) but its ROC is positive (rate of decline slowing), the composite incorrectly rose toward bullish bias. ROC alone loses absolute level context — you need to know both where you are and where you're going. The fix blends absolute level Z-score with ROC Z-score per indicator, controlled by `LevelROC_Blend`.

---

## Input Symbols

The study reads two external symbols as inputs:

| Symbol | Description |
|---|---|
| `$VOLD` | NYSE Volume Differential — advancing vs declining volume |
| `$ADD` | NYSE Advance/Decline Difference — advancing minus declining issues |

These are configured as Symbol studies on the same 1-min chart, accessed via study subgraph inputs (subgraph 4 = Last).

---

## User Inputs

| Input Name | Type | Default | Description |
|---|---|---|---|
| `VOLD Data Source` | StudySubgraph | — | Points to the VOLD Symbol study, subgraph 4 (Last) |
| `ADD Data Source` | StudySubgraph | — | Points to the ADD Symbol study, subgraph 4 (Last) |
| `LongZ_Sessions` | Integer | 5 | Number of complete past sessions used for Long Z normalization window |
| `SessionStartTime` | Time | 09:30 | RTH session start — resets Short Z and suppression logic |
| `SessionEndTime` | Time | 16:00 | RTH session end |
| `ChartIntervalMinutes` | Integer | 1 | Chart bar interval in minutes |
| `WeightMode` | Enum | DYNAMIC | Weighting mode: `FIXED` or `DYNAMIC` |
| `MinBarsForShortZ` | Integer | 10 | Minimum bars since session open before Short Z is valid |
| `StdDevFloor` | Float | 0.5 | Minimum std dev — prevents division by near-zero |
| `ConsistencyLookback` | Integer | 20 | Bar lookback for dynamic weight consistency |
| `DisplayMode` | Enum | ZSCORE | Normalization display: `ZSCORE` or `RATIO` |
| `ROC_Lookback` | Integer | 10 | Bars back for rate of change on both VOLD and ADD |
| `LevelROC_Blend` | Float | 0.50 | Weight for absolute level vs ROC per indicator. 0.0 = pure ROC, 1.0 = pure level, 0.5 = equal blend |

---

## Architecture

### Step 1 — ROC Computation

Before normalization, compute the rate of change for each indicator:

```
VOLD_ROC = VOLD_current - VOLD[ROC_Lookback bars ago]
ADD_ROC  = ADD_current  - ADD[ROC_Lookback bars ago]
```

**Note:** ROC is not computable if the lookback bar falls outside the current RTH session (prior session boundary). Those bars are treated as part of the suppression window.

**Sign behavior:** ROC values can go negative even when the indicator is positive (e.g. VOLD was +500k 10 bars ago, now +200k — ROC = -300k even though VOLD is still positive). This is intentional: ROC captures acceleration/deceleration. The level blend restores directional context.

### Step 2 — Dual Z-Score Normalization (four independent streams)

Each of the four streams — VOLD_level, VOLD_ROC, ADD_level, ADD_ROC — passes through the full dual Z-score / RATIO pipeline independently.

#### Short Z — Session Character
- Rolling mean and std dev from session open forward
- Resets at each `SessionStartTime`
- Not valid until `MinBarsForShortZ` bars have elapsed

#### Long Z — Regime Context
```
BarsPerSession = 390 / ChartIntervalMinutes
LongZ_Bars     = LongZ_Sessions × BarsPerSession
```

#### Dual Z Blend per stream
```
Z_blend = Short_Z * 0.6 + Long_Z * 0.4
```

### Step 3 — Per-Indicator Signal (Level + ROC blend)

```
VOLD_signal = (VOLD_level_Zblend * LevelROC_Blend) + (VOLD_ROC_Zblend * (1 - LevelROC_Blend))
ADD_signal  = (ADD_level_Zblend  * LevelROC_Blend) + (ADD_ROC_Zblend  * (1 - LevelROC_Blend))
```

When ROC is not yet computable (first ROC_Lookback bars of session), signal falls back to level only:
```
VOLD_signal = VOLD_level_Zblend
ADD_signal  = ADD_level_Zblend
```

---

## Weighting Modes

### Mode 1: FIXED (50 / 50)

```
VOLD_Weight = 0.50
ADD_Weight  = 0.50
```

### Mode 2: DYNAMIC (consistency-based)

Consistency is computed on the **final per-indicator signal** (post level+ROC blend), not on level or ROC separately:

```
VOLD_consistency = 1 / stddev(VOLD_signal, ConsistencyLookback)
ADD_consistency  = 1 / stddev(ADD_signal,  ConsistencyLookback)

Total  = VOLD_consistency + ADD_consistency
VOLD_W = VOLD_consistency / Total
ADD_W  = ADD_consistency  / Total
```

---

## Composite Calculation

```
Composite = (VOLD_signal * VOLD_W) + (ADD_signal * ADD_W)
```

---

## Display Modes

### Mode 1: ZSCORE (default)
- Output centered at 0, units in standard deviations
- Reference lines at 0, ±1, ±2

### Mode 2: RATIO
- Output centered at 1.0
- Reference lines at 1.0 (neutral), 1.5 (bull), 0.5 (bear), 2.0 / 0.0 (extremes)

| Reference Line | ZSCORE value | RATIO value |
|---|---|---|
| Strong bull extreme | +2.0 | 2.0 |
| Bull threshold | +1.0 | 1.5 |
| Neutral | 0.0 | 1.0 |
| Bear threshold | -1.0 | 0.5 |
| Strong bear extreme | -2.0 | 0.0 |

---

## Output Subgraphs

| Subgraph | Content | Visual Style |
|---|---|---|
| SG1 — Composite | Blended composite oscillator | Thick line, color-coded |
| SG2 — VOLD Signal | VOLD per-indicator signal (level+ROC blend) | Thin line, muted orange |
| SG3 — ADD Signal | ADD per-indicator signal (level+ROC blend) | Thin line, muted purple |
| SG4 — Zero Line | Constant 0 / 1.0 (RATIO) | Dashed gray |
| SG5 — Upper Threshold | +1.0 / 1.5 | Dotted green |
| SG6 — Lower Threshold | -1.0 / 0.5 | Dotted red |
| SG7 — Upper Extreme | +2.0 / 2.0 | Dashed green, lighter |
| SG8 — Lower Extreme | -2.0 / 0.0 | Dashed red, lighter |

---

## Signal Rules

### R1 — Time-Based Lookback
Long Z window = `LongZ_Sessions × (390 / ChartIntervalMinutes)` bars. Study designed to run on a 1-min chart overlaid on the range bar trading chart.

### R2 — Short Z Session Reset
Short Z resets at `SessionStartTime`. Minimum `MinBarsForShortZ` bars required before Short Z is valid.

### R3 — Std Dev Floor
If computed std dev < `StdDevFloor`, clamp to `StdDevFloor`. Applies to all four streams (level and ROC).

### R4 — Session Suppression
Composite is gray and signals unreliable until both conditions are met:
- `MinBarsForShortZ` bars have elapsed since session open
- `ROC_Lookback` bars have elapsed (ROC computable within current RTH session)

### R5 — Composite Color Logic
- Composite > bull threshold → **Green**
- Composite < bear threshold → **Red**
- Within thresholds → **Gray**
- During suppression → **Gray regardless**

### R6 — Bias Confirmation (2-bar hold)
Color commits only after 2 consecutive bars beyond the threshold. A single-bar spike reverts to previous state.

### R7 — Divergence (visual)
SG2 and SG3 on the same normalized scale enable visual divergence reading. Both aligned = high conviction. Scattered = weak conviction.

### R8 — Extreme Exhaustion Zone
Composite reaching ±2.0 (or 2.0/0.0 in RATIO) is a potential fade zone, not momentum continuation. Visual reference only.

### R9 — Outside RTH
All subgraphs output `sc.Unused_1` outside `SessionStartTime` to `SessionEndTime`.

---

## LevelROC_Blend Guidance

| Value | Behavior |
|---|---|
| 1.0 | Pure absolute level — directionally correct but lags turns |
| 0.5 | Equal blend — default, balances direction and momentum |
| 0.0 | Pure ROC — most responsive but loses directional context |
| 0.3–0.4 | ROC-leaning — responsive with light directional anchor |
| 0.6–0.7 | Level-leaning — directional with momentum confirmation |

---

## Interpretation Guide

### ZSCORE mode

| Composite Value | Meaning |
|---|---|
| Above +2.0 | Extreme bull breadth — exhaustion possible |
| +1.0 to +2.0 | Bullish bias confirmed |
| -1.0 to +1.0 | Neutral / chop — stand aside |
| -2.0 to -1.0 | Bearish bias confirmed |
| Below -2.0 | Extreme bear breadth — exhaustion possible |

**This study is a bias filter — not a standalone entry signal.**

---

## Implementation Notes

- Language: **ACSIL (C++)** for Sierra Chart
- External symbol data accessed via `sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, ...)` — always uses the current chart number to avoid the `GetChartNumber()==0` mismatch that occurs with same-chart inputs
- Persistent state (sessionOpenBar, prevBarDate, color counters) stored in `sc.Subgraph[3].Arrays[0..4]` — avoids `sc.PersistVars` which does not exist in this SC version
- ROC arrays store `sc.Unused_1` for bars where ROC is not computable; `RollingStats` skips these automatically
- Dynamic weights operate on the final per-indicator signal, not on level or ROC streams separately
- `StdDevFloor` applies to both level and ROC std dev calculations

---

## File Deliverables

| File | Description |
|---|---|
| `BreadthCompositeOscillator.cpp` | Main ACSIL study source file |

---

*Spec version 1.3 — updated from design session with Flavius*
*RATIO display mode inspired by MBRA normalization from Zaremba et al. (2019)*
