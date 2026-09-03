# Failed Aggression Map — Build Spec v1.0

Standalone Sierra Chart ACSIL study. New file only: `FailedAggressionMap.cpp`.
Do NOT modify `TrappedTraders.cpp` or `LiquidityZones.cpp` (read for VAP/drawing precedent only).

## 1. Identity

- File: `FailedAggressionMap.cpp`
- `SCDLLName("FailedAggressionMap")`
- `SCSFExport scsf_FailedAggressionMap(SCStudyInterfaceRef sc)`
- `GraphName = "Failed Aggression Map v1.0"`
- `AutoLoop = 0`, `GraphRegion = 0`, `sc.MaintainVolumeAtPriceData = 1`

## 2. Purpose and epistemic limit

Detect directional aggression at bar/window extremes, confirm price did not reward
it and displaced against it, then visualize the inferred failed inventory as a
volume-scaled bubble at its delta-weighted centroid plus a thin horizontal ribbon.

VAP shows executed bid/ask volume, not trader identity or open positions.
"Failed buyers/sellers" is an inference. The ribbon is labeled as inferred
inventory, not exact trader break-even. This is a separate study, not Trapped
Traders v2.

## 3. Evidence state machine (kept separate, never conflated)

`ATTEMPT -> FAILURE_CONFIRMED -> FIRST_RETEST -> RETEST_FAILED or ACCEPTED_THROUGH`

- A raw footprint cluster is an ATTEMPT, not rejection.
- A touch is a FIRST_RETEST, not a failed retest.
- RETEST_FAILED requires exit back in the displacement direction (Sec. 5).
- ACCEPTED_THROUGH requires a closed bar beyond the far edge + tolerance (Sec. 5).

## 4. Core detection (closed bars only)

- Source: this chart's closed-bar VAP (`sc.VolumeAtPriceForBars`). Forming bar
  (`sc.ArraySize - 1`) is never used for detection or state transitions.
- Window: rolling `Detection Lookback Bars` (In:1, default 2), window `[w-L+1, w]`
  ending at closed bar `w`.
- Aggregate Ask-Bid delta by integer price tick (`tick = round(price / TickSize)`)
  using `int64` math (`AskVolume - BidVolume` as `int64` before subtract/accumulate).
- Rolling window high/low converted to ticks via `round(high / TickSize)`.
- High side: levels with `tick >= highTick - ProximityTicks` (In:2, default 3) and
  `delta >= blockThresh`, need `>= MinimumBlocks` (In:3, default 3) such levels,
  cluster total (sum of qualifying levels only) `>= zoneThresh` → ATTEMPT,
  side = aggressive buyers (+1, expected failure = displacement DOWN).
- Low side mirror: `tick <= lowTick + ProximityTicks`, `delta <= -blockThresh`,
  total magnitude `>= zoneThresh` → ATTEMPT, side = aggressive sellers (-1).
- Manual defaults: 95 per block (In:4), 350 cluster total (In:5).
- Geometry stored as integer ticks (`TopTick`, `BotTick`, `CentroidTick`).
  Centroid = `round(sum(tick * |delta|) / sum(|delta|))` over qualifying levels
  (delta-weighted, integer math, `int64` accumulator).
- Overlap/merge/dedupe tests use inclusive integer-tick intersection
  (touching ranges merge). No float equality on prices anywhere.

## 5. Confirmation / retest / acceptance (closed bars only)

All comparisons in integer ticks; `closeTick = round(Close / TickSize)`, etc.

- Failure confirmation (In:6 `Confirmation Bars` default 3, In:7 `Adverse
  Displacement Ticks` default 4): high-side ATTEMPT detected at bar `w` confirms
  on the first closed bar `c` in `(w, w+M]` with `closeTick <= BotTick - advTicks`.
  Low-side mirror: `closeTick >= TopTick + advTicks`. No confirmation by deadline
  bar close → discard (pruned, never drawn).
- First retest (only after confirmation, from the displaced side, overlapping the
  cluster): high-side event retests on the first closed bar after `confirmBar`
  with `LowTick < BotTick && HighTick >= BotTick` (came from below, overlaps).
  Low-side mirror: `HighTick > TopTick && LowTick <= TopTick`.
- Failed retest (In:8 `Retest Exit Ticks` default 2): after FIRST_RETEST, a later
  closed bar exits again in the displacement direction — high-side:
  `closeTick <= BotTick - exitTicks` → RETEST_FAILED + short entry arrow.
  Low-side mirror: `closeTick >= TopTick + exitTicks` → RETEST_FAILED + long arrow.
  No exit without acceptance → stays in FIRST_RETEST.
- Acceptance-through (In:9 `Acceptance Ticks` default 1): closed bar beyond the
  FAR cluster edge — high-side: `closeTick >= TopTick + accTicks`; low-side:
  `closeTick <= BotTick - accTicks` → ACCEPTED_THROUGH, ribbon terminated/frozen,
  X mark. Checked before retest-exit on every bar (acceptance wins ties). Valid
  from FAILURE_CONFIRMED onward (straight-through without retest is allowed).
- Historical full recalculation runs one sequential closed-bar engine identical to
  live order (per bar: advance existing events, then detect), so replay reproduces
  live decisions. No future data.

## 6. Threshold modes (In:10, default Manual)

- `Manual`: In:4/In:5 fixed thresholds.
- `Auto`: thresholds derived from the same chart's five previous COMPLETE Sierra
  trading days (`sc.GetTradingDayDate`), excluding the current day, no lookahead.
  Computed once per trading day and cached in persistent state (`cachedDayDate`,
  `autoBlock`, `autoZone`, `autoReady`).
   - Per prior day: slide the Sec. 4 window over that day's closed bars
     (full-length windows only; truncated day-open windows are skipped so
     calibration matches live detection); collect
    (a) per-level absolute deltas of directionally valid near-extreme levels and
    (b) per-window side totals (sum of qualifying levels per side per window).
    Daily value = percentile ((a) at In:12 `Auto Block Percentile` default 85,
    (b) at In:13 `Auto Zone Percentile` default 80, nearest-rank). Final threshold
    = median of the (up to) five daily percentile values, min-clamped to 1.
    In:11 `Auto Days` default 5 (capped at 5 by fixed array sizing).
  - A prior day with fewer than 5 sampled windows is skipped. If fewer than 5
    usable days exist, Auto is inactive for the current day: emits no events and
    shows one fixed-line-number warning text (updated, never spammed).
  - `Both` continues Manual normally while Auto warms up.
- `Both`: Manual and Auto detectors run independently over the same windows.
  Manual precedence: an Auto candidate whose inclusive tick range intersects any
  live (not discarded/accepted) Manual event is suppressed, regardless of side.
  Same-mode overlap merges only within the same side AND same source (Manual/Manual
   or Auto/Auto): expand range, add `int64` totals and tick-weight sums, recompute
   centroid with exact integer math and recompute strength from the merged
   total, then publish the full pulse triple for the merge bar
   (fixes the v1 "merge drops delta" bug). Cross-side never merges.
- Every event stores `Source` (1 = Manual, 2 = Auto) and exports it (SG10).
  Auto drawings use distinct colors (In:19/20 inputs are the Manual pair; Auto
  uses fixed orange / light-blue documented in Sec. 9).

## 7. Mapped-location confluence (In:14, disabled by default)

- Input type Study-Subgraph via `SetChartStudySubgraphValues(0, 0)` (StudyID 0 =
  disabled), per the task. If the target SC version does not expose that
  symbol, the one-line fallback is `SetStudySubgraphValues(0, 0)` (the
  repo-proven spelling in OrderflowSignalV2/V3, OrderflowConfluence); the read
  path with `sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, ...)` is already
  compatible with both spellings. Must be confirmed at F5 compile.
  When enabled, the referenced array is fetched once per call via
  `sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, ...)`.
- Nonzero value (`!= 0.0f`) on the detection bar (a closed bar) = confluence.
- v1.0 NEVER suppresses raw detections on confluence; it adds a gold halo drawing
  and sets SG9 = 1 (research comparison preserved).

## 8. Rendering modes (In:15, default Bubble + Ribbon)

- 0 `Bubble + Ribbon` (default): Sec. 9 bubble + centroid ribbon.
- 1 `Lifecycle Glyphs`: attempt dot (hollow small ellipse) → filled failure
  bubble → gold ring (first retest) → text arrow (failed retest) → X (accepted).
- 2 `Legacy Rectangle`: full cluster-range rectangle (Trapped-Traders style) only.
- 3 `Bubble + Ribbon + Rectangle`: both.
- Bubble size from normalized cluster delta
  (`strength10 = clamp(10 * total / (zoneThresh * 3), 0.5, 10)`),
  radius `= minR + (strength10 / 10) * (maxR - minR)` ticks, bounded by
  In:16/In:17 (min/max radius in ticks, defaults 1/4).
- Ribbon: 1-tick-tall rectangle centered on centroid tick, from detection bar to
  last closed bar (live) or acceptance bar (terminated). Text label marks it as
  inferred inventory.
- All drawings are ordinary (non-user-drawn) `sc.UseTool` objects in one
  namespace (bases Sec. 11); deleted by collapsing with `UTAM_ADD_OR_ADJUST`
  (same namespace — avoids the v1 user-drawing delete bug). Active ribbons refresh
  each processed bar; everything display-only (colors, sizes) takes effect without
  a structural rebuild.
- Object cap: In:18 `Max Events Drawn` (default 50); only newest N events keep
  drawings, older drawings are collapsed. Fixed capacity `MAX_EVENTS = 128`;
  discarded/accepted-old records pruned on insert (no unbounded growth).

## 9. Colors

- Failed buyers (high-side, +1): magenta/red, In:19 default `RGB(255,0,200)`.
- Failed sellers (low-side, -1): cyan/blue, In:20 default `RGB(0,220,255)`.
- Mapped confluence halo + first-retest ring: gold, In:21 default `RGB(255,200,40)`.
- Auto source: fixed distinct orange `RGB(255,110,40)` (buyers) /
  light blue `RGB(110,150,255)` (sellers).
- Acceptance X: gray `RGB(160,160,160)`.

## 10. Subgraph outputs (14 total, hard limit < 60)

All set per closed bar; cleared from `sc.UpdateStartIndex` forward every call
(no stale ghost values). Pulses live on their transition bar:

| # | Name | Style | Value |
|---|---|---|---|
| SG0 | Candidate Side | IGNORE | +1 buyer-aggr / -1 seller-aggr at detection bar |
| SG1 | Failure Confirmed Side | IGNORE | side sign at confirmation bar |
| SG2 | Centroid Price | IGNORE | centroid price at detection bar |
| SG3 | Cluster Top | IGNORE | top price at detection bar |
| SG4 | Cluster Bottom | IGNORE | bottom price at detection bar |
| SG5 | Normalized Strength 0-10 | IGNORE | strength10 at detection bar |
| SG6 | First Retest Pulse | IGNORE | side sign at first-retest bar |
| SG7 | Failed Retest Entry | IGNORE | -1 short (failed buyers) / +1 long (failed sellers) at fail bar |
| SG8 | Accepted Through | IGNORE | side sign at acceptance bar |
| SG9 | Mapped Confluence Flag | IGNORE | 1 at detection bar if confluence |
| SG10 | Threshold Source | IGNORE | 1 Manual / 2 Auto at detection bar |
| SG11 | Event ID | IGNORE | stable float ID at detection bar |
| SG12 | Failure Bubble | POINT, width 3 | centroid price at confirmation bar |
| SG13 | Retest/Accept Marker | DIAMOND, width 3 | retest price at fail bar / accept price at accept bar |

## 11. Inputs (23, append-only, documented)

| # | Name | Type / Default |
|---|---|---|
| In:1 [0] | Detection Lookback Bars | int 2 (structural) |
| In:2 [1] | Proximity Ticks | int 3 (structural) |
| In:3 [2] | Minimum Blocks | int 3 (structural) |
| In:4 [3] | Manual Block Delta | float 95 (structural) |
| In:5 [4] | Manual Cluster Delta | float 350 (structural) |
| In:6 [5] | Confirmation Bars | int 3 (structural) |
| In:7 [6] | Adverse Displacement Ticks | int 4 (structural) |
| In:8 [7] | Retest Exit Ticks | int 2 (structural) |
| In:9 [8] | Acceptance Ticks | int 1 (structural) |
| In:10 [9] | Threshold Mode | custom Manual/Auto/Both, Manual (structural) |
| In:11 [10] | Auto Days | int 5 (structural) |
| In:12 [11] | Auto Block Percentile | int 85 (structural) |
| In:13 [12] | Auto Zone Percentile | int 80 (structural) |
| In:14 [13] | Mapped Location Source | study-subgraph, disabled (structural: enable/disable only) |
| In:15 [14] | Rendering Mode | custom 4 modes, 0 (structural) |
| In:16 [15] | Bubble Min Radius (ticks) | int 1 (display) |
| In:17 [16] | Bubble Max Radius (ticks) | int 4 (display) |
| In:18 [17] | Max Events Drawn | int 50 (display/perf) |
| In:19 [18] | Failed Buyers Color | color magenta (display) |
| In:20 [19] | Failed Sellers Color | color cyan (display) |
| In:21 [20] | Confluence/Halo Color | color gold (display) |
| In:22 [21] | Enable Alerts | yes/no, No (display) |
| In:23 [22] | Alert Sound Number | int 0 (display) |

Drawing line bases: bubble 110000, halo 120000, ribbon 130000, ring 140000,
arrow-text 150000, X-text 160000, legacy rect 170000, label 180000, warmup 199999;
line number = base + event ID.

## 12. Engineering rules

- Persistent state: one heap struct via `GetPersistentPointer(1)` holding
  fixed-capacity arrays (`MAX_EVENTS = 128`, no STL in persistent state),
  counters, day cache, fingerprint words, alert watermarks, `prevLastClosed`.
  Freed + nulled on `LastCallToFunction` (drawings collapsed first).
- Missed bars (reconnect/backfill): incremental path processes every closed bar
  in `(prevLastClosed, lastClosed]` for both detection and transitions.
- Settings fingerprint over structural inputs (In:1..In:15 incl. mapped-source
  StudyID/Subgraph and threshold-mode indexes) forces immediate full rebuild
  (state reset + drawing purge by ID range + SG clear). Threshold magnitudes
  are folded whole (no 16-bit truncation) so any edit rebuilds.
- Intrabar guard on `ArraySize`; forming bar never read for logic.
- Alerts (if enabled): live-only watermark — fire only on non-full-recalc calls
  for transitions landing exactly on the newest closed bar, anchored with
  `sc.SetAlert(sound, sc.ArraySize - 1, ...)`. Never from history.
  One alert per transition type per bar; failure-confirm and failed-retest
  alert, acceptance-through intentionally has no alert.
- History rewrite (backfill with `UpdateStartIndex <= PrevLastClosed`) forces a
  full rebuild so SG pulses under the rewrite are recomputed, not left zeroed.
- Same-mode merge publishes the full pulse triple for the merge bar
  (side/centroid/strength/mapped/source/ID) and recomputes strength from the
  merged total. Failed-retest text is anchored at the fail-bar close to agree
  with SG13. Auto sampler uses full-length windows only.
- No `static`/`thread_local` function locals; no `#` comments; no float `==` on
  price arrays; SG count 14 < 60; every input defaulted AND read in logic.

## 13. Verification (Linux, no Sierra build here)

- `python3 gate_check.py FailedAggressionMap.cpp` must be 12/12.
- Static self-review: every input read, struct fields consistent, SG < 60,
  no forming-bar decisions, no stale SGs, no unbounded growth.
- Do NOT claim Sierra compilation. Report files, commit SHA, gate output, and the
  exact Sierra compile/replay checklist (F5 compile; footprint chart with VAP;
  Ctrl+Insert full recalc; Manual vs Auto vs Both replay comparison; mapped-source
  halo check; rendering-mode switch check).
