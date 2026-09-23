# MultiLab — Build Spec

**File:** `MultiLab.cpp`
**Study name:** `Multi Lab`
**DLL:** `MultiLab`
**Status:** implemented (v1.4 — same-chart source reads + per-slot diagnostics; see git history for
the v1.1 5→6-trigger extension and the v1.0 5-trigger spec)
**Date:** 2026-08-02
**Reference implementation (READ ONLY, never modified):** `C:\SierraChart\ACS_Source\TriggerLab.cpp` ("Trigger Lab v1.2")

---

## 0. What this is and is not

TriggerLab measures ONE trigger (one long SG + one short SG) per run. Measuring 8 triggers today costs
8 chart configurations, 8 CSV files, and cross-trigger confluence has to be reconstructed after the
fact by fuzzy-matching timestamps across separate files.

MultiLab measures **6 triggers (12 source SGs: 6 long + 6 short) in a single run**, writing one shared
CSV. It is TriggerLab's entire measurement apparatus — fire test, delay sweep, horizon modes, session
gating, MFE/MAE, the single-barrier evaluator, the Barrier Sweep second export — unchanged, wrapped in
an outer loop over 6 trigger slots, plus two genuinely new things:

1. `source_name` — a human-readable label per trigger, written on every row, so `registry.md`'s whole
   reason for existing (decoding opaque `source_sg` integers) becomes unnecessary for MultiLab data.
2. `fired_mask` / `fired_count` — because the study now holds all 6 triggers' entry bars in memory at
   once, it can answer "how many OTHER triggers were active when this one fired" directly, instead of
   the current weak proxy of matching timestamps across 6 separate TriggerLab files within a fuzzy
   window.

Not a strategy engine. Not an auto-trader. No trading calls of any kind. Same non-negotiables as
TriggerLab (see its header comment) apply here unchanged.

---

## 1. Signal sources — 6 trigger slots, 12 source SGs

| Slot | Inputs |
|------|--------|
| Trigger 1 | Enable, Name, Long Chart Number, Long Study+Subgraph, Short Chart Number, Short Study+Subgraph |
| Trigger 2 | (same 6) |
| Trigger 3 | (same 6) |
| Trigger 4 | (same 6) |
| Trigger 5 | (same 6) |
| Trigger 6 | (same 6) |

**One Enable flag per trigger, not per side.** TriggerLab has independent Long/Short enable because it
tests one trigger's two directions. MultiLab's Enable answers "is trigger K in this run at all" — if
you want long-only for one trigger, set its Short Chart Number/Study+Subgraph to nothing (Study ID 0),
which is already TriggerLab's existing "slot not configured" convention (`studyID == 0` → skip). This
halves the enable-flag input cost (6 instead of 12) with no loss of capability.

**Chart Number = 0 means "this chart"**, same convention as TriggerLab. Study+Subgraph uses
`SetStudySubgraphValues`, same as TriggerLab. Cross-chart read reuses the identical
`GetStudyArrayFromChartUsingID` + `GetChartDateTimeArray` + monotonic merge-walk pattern — copied
verbatim per trigger per side, not reinvented.

**`source_name`** is a plain string input, default `"Trigger 1"` .. `"Trigger 6"`. Written into a new
`source_name` CSV column on every real-trade row. Baseline rows carry `source_name = "BASELINE"`.

### Same-chart vs cross-chart reads (v1.4)

Every slot resolves a `chartNum` (0 → `sc.ChartNumber`) and picks one of two read paths:

| | Same chart (`chartNum == sc.ChartNumber`) | Cross chart (`chartNum != sc.ChartNumber`) |
|---|---|---|
| Array fetch | `sc.GetStudyArrayUsingID(studyID, sgIndex, arr)` | `sc.GetStudyArrayFromChartUsingID(chartNum, studyID, sgIndex, arr)` |
| Source DateTime | `sc.BaseDateTimeIn` directly (identity — the source's bars ARE this chart's bars) | `sc.GetChartDateTimeArray(chartNum, srcDT)` |
| Merge-walk | Still runs, degenerates to `destOfSrc[s] == s` | Unchanged monotonic merge-walk |

Applied at both read sites: the 12 trigger source slots (section 1) and the 5 directional-filter slots
(`MultiLab_Filters_BuildSpec.md`). The filter slots already special-cased the DateTime half of this
(`MLSampleFilterValue`'s `chartNum == sc.ChartNumber` branch predates v1.4 and samples `mainBar - 1`
directly); v1.4 only added the matching split on the array-fetch call itself, for consistency with the
trigger path and with `RenkoFlipAutoTrader.cpp` / `OTFMultiTrader.cpp`, which already branch the same
way for their own external/internal study references.

**Why this was investigated, and what it did and did not fix.** A same-chart configuration
(`Trigger Aggregator V2` sourcing into MultiLab on the same chart) was reported producing a
baseline-only export — zero trades, no error. The working hypothesis going in was that
`GetStudyArrayFromChartUsingID` is a cross-chart-only API and silently misbehaves when
`ChartNumber == sc.ChartNumber`. That hypothesis is **refuted** by direct evidence already in this
codebase:

- `TriggerAggregatorV2.cpp:741-758` reads its own V5 / V5 Sell sources — which sit on the same chart as
  itself — via `sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, bullID, ...)`, and this is a working,
  currently-deployed study.
- `OrderflowSignalV5.cpp:671` documents the identical merge-walk explicitly: "For `chartNum ==
  sc.ChartNumber` this yields the identity map, reproducing the old same-chart read exactly."
- This file's OWN filter-read code (pre-v1.4) already called `GetStudyArrayFromChartUsingID`
  unconditionally, including for same-chart filter sources, with no reported defect.

So `GetStudyArrayUsingID` for the same-chart case is a real hardening (it matches the more defensive
convention used by `RenkoFlipAutoTrader.cpp` / `OTFMultiTrader.cpp`, and skips a pointless merge-walk
over a chart's own bars) but it is **not** what caused the reported symptom. See
`Testing/ta-v2-multilab-sourcing-diagnosis.md` (2026-07-30) for the actual ranked causes — in short,
`TriggerAggregatorV2` can return before writing any subgraph if its own upstream sources are not
connected (Sierra still allocates the array at full size, so it reads as "connected" while every value
is 0), a `SetStudySubgraphValues` input's stored Study ID can go stale after a study is deleted and
re-added, or the wrong subgraph can be selected (`Debug: Sources Connected` is a hardcoded constant 2
and can never satisfy a rising-edge fire test). None of those are fixable from MultiLab's side by
choosing a different read API — they need the new per-slot diagnostics below to be distinguishable at
all, and section 11's checklist to rule out on the chart.

### Study-wide settings are shared across all 6 triggers — deliberate, not an oversight

Fire Test, Fire Threshold, Entry Delay, Entry Price, Overlapping Signals, Session Start/End, Flatten At
End, Horizon Mode/Bars/Minutes, Evaluate Stop/Target, Target, Stop, Baseline Mode, Delay Sweep, Barrier
Sweep and all its sub-inputs: **one set of values, applied identically to all 6 triggers.** Same
principle TriggerLab already uses for its long/short pair ("Do not add per-side delay/horizon
inputs"), extended to per-trigger. Reasons, not just budget:

- `fired_mask` only means something if every trigger's notion of "entry bar" is built the same way —
  same delay, same session gate, same horizon. Six triggers each running their own private Entry
  Delay would make "trigger B was active when trigger A fired" a comparison between two different
  clocks.
- The single shared baseline (section 3) is compared against all 6 triggers' expectancy; it can only
  be a fair control group if the thing it is a control FOR (delay, horizon, entry price convention) is
  the same for everyone.
- Input budget (section 8) is already tight.

If a future need arises to vary Entry Delay per trigger, that is a new spec, not a silent extension of
this one.

---

## 2. Everything from TriggerLab, preserved exactly

Copied verbatim, per trigger per side, unless stated otherwise below:

- Fire test (Non-Zero Rising Edge / Cross Above / Cross Below), evaluated in SOURCE time on the source
  chart's own bars.
- `knowableBar = destOfSrc[s+1]`, never the source bar's own open.
- `entryBar = knowableBar + Entry Delay`, Entry Delay 0–20, Delay Sweep (0..Max, ignores Entry Delay
  when ON) — **per-delay watermark arrays now needed per (trigger, side)**, not just per side, since
  six triggers each running Overlap = Skip While In Trade must not let one trigger's open trade
  suppress another trigger's signal. See section 6.
- Entry Price Close (default) / Open, with the Entry Delay=0 + Open → Close lookahead-guard fallback,
  computed once (config-level, not per-trigger — it only depends on Entry Delay and Entry Price mode,
  both shared).
- Session gate applied to the ENTRY bar (midnight-wrap aware), Flatten At Session End truncation.
- Horizon Mode (Bars/Minutes), the minutes-cutoff-reached logic, the last-closed-bar completeness
  clamp, the `complete` flag definition.
- MFE/MAE over `entryBar+1 .. horizonEnd`, entry bar excluded, same-bar-both-touched pessimism rule
  (stop wins).
- The single-barrier `st_*` evaluator (`TL_WIN`/`TL_LOSS`/`TL_TIMEOUT`), same pessimism rule.
- Barrier Sweep: first-touch-bars method, ascending stop/target level lists, min R:R floor, second
  narrow `*_barriers.csv` export, joined on `(run_chart, run_bars, run_delay, trade_id)`. **Barrier
  file schema is UNCHANGED** — no `fired_mask` column there; it joins to the main file if that
  information is ever needed per trade_id, exactly as `source_sg` and entry times already do.
- Baseline Mode: deterministic per-(chart, day, side) xorshift64* draw, no delay, no overlap policy,
  same `MLBuildTradeAtEntry` code path as real trades (not a parallel implementation).
- All `run_*` self-describing columns, unchanged in meaning: `run_chart, run_bars, run_delay,
  run_horizon_mode, run_horizon, run_fire_test, run_entry_price, run_eval_st, run_target, run_stop`.
- `sc.AutoLoop = 0`, single full pass, rebuild only on full recalc or bar-count change.
- 60-subgraph limit respected (section 5).
- Freshness canary, bumped: `sc.GraphName = "Multi Lab v1.1"`.

---

## 3. The baseline is emitted ONCE, shared across all 6 triggers

TriggerLab's Baseline Mode block runs once per side already (it has nothing to do with which trigger
is configured — it draws random in-session bars). MultiLab keeps it **exactly that way**: the baseline
loop runs once per side (long/short), NOT once per trigger. This was already true in TriggerLab's code
structure; the risk for MultiLab is accidentally moving it inside the per-trigger loop and generating
6 baselines. The spec calls it out explicitly because getting this wrong is the single easiest mistake
in this whole port: it would silently 6x the baseline row count and make the CSV's implicit "one
control group" assumption (which `barrier_grid.py`'s `build_grid()` relies on when it joins baseline
by `(run_bars, dir, stop, target)` without `source_sg`) wrong in a way that would not throw an error —
it would just quietly inflate baseline `n` and make `base_exp` an average over 6 correlated redraws of
the same days instead of one.

Concretely: the baseline block sits AFTER the per-trigger loop, reads no trigger-specific input, and
writes `sourceStudyID = 0, sourceSG = 0, source_name = "BASELINE"` — the same sentinel TriggerLab
already uses, now doubling as the "this row has no owning trigger" marker for `fired_mask` purposes
(section 4).

---

## 4. `fired_mask` / `fired_count`

### 4.1 What "active at the entry bar" means

**Configurable lookback window, input `Fired Mask Lookback (bars)`, default 0.** Bit i is set if
trigger i (same side as this trade) had a recorded entry bar anywhere in
`[thisEntryBar - lookback, thisEntryBar]` — inclusive on both ends, never forward. Default 0 means
"same bar only, exact alignment"; the spec explicitly asks for this to be defensible-configurable
because a trigger that fired 2 bars ago is arguably still confirming, and 0 is too strict a
requirement for triggers whose knowable-bar timing differs by a bar or two even when they are
conceptually reacting to the same move.

**Never forward.** A trigger that fires 2 bars AFTER this entry cannot be said to have confirmed it at
entry time — that information did not exist yet. Restricting the window to the past only keeps
`fired_mask` computable in real time (not just in hindsight on historical data), which matters because
this column's whole purpose is eventually informing a live decision ("do I take this signal given
what else just fired"), not just post-hoc analysis.

### 4.2 Evaluated at the entry bar, not the signal bar

TriggerLab defines "when did this trade happen" as `entryBar` (knowable bar + delay) for every other
measurement in the study — MFE/MAE, the session gate, the horizon, the `complete` flag are all keyed
off `entryBar`, never `signalBar`. `fired_mask` uses the same anchor for consistency: two triggers
whose SIGNAL bars are far apart but whose (delayed) ENTRY bars land close together are the ones that
actually matter for "did I have two reasons to take this trade at the same moment I could act on it."
Anchoring to signal bar instead would conflate a trigger's raw detection latency with actual
confluence.

**Under Delay Sweep**, a real-trade row's `run_delay` varies per row (0..sweepMax). Its `fired_mask` is
computed against OTHER triggers' entry-bar lists **at that same delay** — delay-0 rows are compared
against other triggers' delay-0 entry bars, delay-3 rows against delay-3 entry bars, never mixed.
Comparing across different delays would compare bars that were never simultaneously knowable under a
single, consistent delay assumption.

**Baseline rows are the one exception.** A baseline entry has no delay of its own (`delayBars = 0`
always, `run_delay` is either the run's single Entry Delay or the sweep's `-1` broadcast sentinel — see
TriggerLab's existing baseline comment, unchanged). Its `fired_mask` is computed against each
trigger's entry-bar list **at the study's single configured Entry Delay input**, regardless of
whether Delay Sweep is ON. This is a deliberate, documented simplification: baseline is delay-invariant
by construction (one draw, broadcast to every delay cell in the analysis pipeline), so it cannot have
one "true" per-delay confluence reading. Anchoring it to the base Entry Delay keeps it consistent with
everywhere else in the study that treats that input as "the" delay when a single delay-invariant
answer is needed (the on-chart panel does the same — see TriggerLab's `t.runDelay != entryDelay`
panel-statistics filter). **Consequence for analysis:** baseline's `fired_count` at swept delays other
than the anchor is not a like-for-like confluence comparison; this is acceptable because baseline is
used for expectancy comparison (`barrier_grid.py`'s `excess` column), not for confluence analysis, and
its `fired_mask`/`fired_count` are a bonus null-rate reference, not the study's primary output.

### 4.3 Bit order and own-bit

`fired_mask` is a 6-bit integer (values 0–63). **Bit i (i=0..5) corresponds to Trigger (i+1) in input
order** — Trigger 1 → bit 0 (value 1), Trigger 2 → bit 1 (value 2), ... Trigger 6 → bit 5 (value 32).
This must be documented next to the CSV header (and is, in the code) since it is meaningless without
this key.

**A trigger's own bit IS set in its own mask** (spec's recommendation, adopted). This falls out of the
implementation for free rather than needing a special case: the same "does trigger i have an entry bar
in `[E-lookback, E]`" check, applied to `i = the trade's own trigger`, always finds the trade's own
entry bar at exactly `E` (since `lookback >= 0`, `E` is always inside `[E-lookback, E]`). So the loop
is uniform over all 6 triggers with no `if (i == ownTrigger) continue` special case — simpler code,
and it guarantees `fired_count >= 1` for every real-trade row and `"solo"` reads naturally as
`fired_count == 1`.

Baseline rows are not owned by any trigger, so no bit is guaranteed set for them; `fired_count == 0` is
a valid and informative baseline value ("no trigger was active near this random bar"), unlike real rows
where it cannot happen.

`fired_count` = popcount of `fired_mask` (0–6 for baseline, 1–6 for real trades).

**`run_fired_mask_lookback`** is added to the `run_*` column block (after `run_stop`) so a pooled CSV
stays self-describing about which lookback produced its `fired_mask` values, exactly like every other
`run_*` column exists to avoid needing the filename or a side channel to know a run's configuration.

### 4.4 Implementation shape (two-pass, per delay cell)

1. **Pass A (per trigger, per side, per delay in the swept range):** run the existing TriggerLab fire
   test / entry-building loop unchanged, but instead of writing straight into the final `trades`
   vector, accumulate into a temporary structure keyed by `(triggerIndex, sideIndex, delay)`: a
   `std::vector<int>` of accepted entry bars (ascending — signals are scanned in ascending source-bar
   order and `destOfSrc` is monotonic, so no explicit sort is needed) plus the actual `MLTrade` records
   built for that cell.
2. **Pass B (mask fill):** for every accumulated real-trade record, for `i = 0..5`, binary-search
   trigger i's `(sideIndex, thisRecord.runDelay)` entry-bar list for any value in
   `[E - lookback, E]` (`std::lower_bound` on the ascending vector, O(log n)); set bit i if found.
   Then append the record (now carrying its final `fired_mask`/`fired_count`) to the global `trades`
   vector, in a fixed deterministic order: trigger 0 long, trigger 0 short, trigger 1 long, trigger 1
   short, ... trigger 5 short — always the same order regardless of how many signals each trigger
   produced, so `trade_id` (section 6) stays deterministic pass-to-pass.
3. **Baseline** is built once per side exactly as TriggerLab does today, then mask-filled against each
   trigger's `(sideIndex, entryDelayInput)` cell per section 4.2's baseline exception, and appended to
   `trades` last.

Cost: triggers × sides × delays × trades-per-cell searches, each O(log n) — trivially cheap next to
the O(window) MFE/MAE walk already done per trade, and bounded overall by the same `ML_MAX_TRADES` cap
as everything else (section 7).

---

## 5. Subgraph budget — 18 of 60 used

The 60-subgraph limit (SC silently clips indices > 59 to slot 59, last write wins, earlier subgraphs
become invisible) is the tightest real constraint in this port given 12 source slots. Budget:

| SG (0-based) | Human # | Name | DrawStyle | Purpose |
|---|---|---|---|---|
| 0 | SG1 | Trigger 1 Long Entry | Arrow Up | entry marker, this trigger, long |
| 1 | SG2 | Trigger 1 Short Entry | Arrow Down | entry marker, this trigger, short |
| 2 | SG3 | Trigger 2 Long Entry | Arrow Up | " |
| 3 | SG4 | Trigger 2 Short Entry | Arrow Down | " |
| 4 | SG5 | Trigger 3 Long Entry | Arrow Up | " |
| 5 | SG6 | Trigger 3 Short Entry | Arrow Down | " |
| 6 | SG7 | Trigger 4 Long Entry | Arrow Up | " |
| 7 | SG8 | Trigger 4 Short Entry | Arrow Down | " |
| 8 | SG9 | Trigger 5 Long Entry | Arrow Up | " |
| 9 | SG10 | Trigger 5 Short Entry | Arrow Down | " |
| 10 | SG11 | Trigger 6 Long Entry | Arrow Up | " |
| 11 | SG12 | Trigger 6 Short Entry | Arrow Down | " |
| 12 | SG13 | MFE (ticks) | Ignore | debug/legacy-parity, shared last-write |
| 13 | SG14 | MAE (ticks) | Ignore | debug, shared last-write |
| 14 | SG15 | Horizon P&L (ticks) | Ignore | debug, shared last-write |
| 15 | SG16 | Fired Count (debug) | Ignore | last-write fired_count at its entry bar |
| 16 | SG17 | Debug: Sources Connected | Ignore | count of the up-to-12 source slots resolved live this pass |
| 17 | SG18 | Debug: Source Array Size (per slot, see Arrays[0-11]) | Ignore | v1.4 — 12 aux arrays, one per trigger/side slot (`slot = k*2 + sideIdx`), each holding that slot's resolved `GetArraySize()` this pass; 0 = unconfigured or unreadable |

**42 subgraphs unused** — comfortable headroom, no risk of the 60-clip. SG13–18 are `DRAWSTYLE_IGNORE`
so they can never wreck the price scale, same rule as TriggerLab's SG3–SG6.

**SG18's aux arrays are the fix for a real blind spot** (`Testing/ta-v2-multilab-sourcing-diagnosis.md`
D1/D2): before v1.4, `sourcesConnected` only measured that a source array was ALLOCATED
(`GetArraySize() >= 2`), not that it had ever been WRITTEN TO — a source study that returns before
assigning any subgraph (Sierra still allocates the array at full chart length) counted as "connected"
while contributing nothing. SG18 makes each of the 12 slots' resolved sizes individually visible
instead of only the aggregate count in SG17, and a configured-but-unusable slot now also writes one
`sc.AddMessageToLog` line naming the trigger, side, chart, study ID and subgraph — gated to at most
once per full-recalculation pass by the same `isFullRecalc || barsChanged` guard the whole rebuild
already runs behind ("rebuild only on full recalc or bar-count change", section 2), not a separate
persistent-int flag.

**Design tradeoff, stated explicitly:** SG13–16 (MFE/MAE/HorizonPnl/FiredCount) are SHARED across all 6
triggers — whichever trade's entry bar is processed last on a given bar wins ("last write wins"), same
as any bar where two signals from different triggers would otherwise collide. This is acceptable
because these four subgraphs are **on-chart debug aids only** (visual sanity check that something is
firing and roughly what its MFE/MAE looked like) — the real per-trigger, per-trade data lives in the
CSV, which never loses a row to a collision. Giving each of the 6 triggers its own private MFE/MAE/
HorizonPnl (18 more subgraphs) was considered and rejected: it would not change any analysis (nothing
downstream reads subgraphs; the CSV is the pipeline's only input) and it eats budget for no benefit.
Entry markers (SG0–11) DO get one pair per trigger because visually distinguishing which trigger fired
where on the chart is the one thing the subgraphs are still useful for that the CSV can't show at a
glance.

---

## 6. `trade_id` uniqueness — guaranteed by construction, unchanged mechanism from TriggerLab

TriggerLab already guarantees `trade_id` uniqueness the simplest possible way: ONE `std::vector<Trade>`
for the whole file, rebuilt from scratch every pass, and `trade_id = (1-based position in that vector
at CSV-write time)`. MultiLab keeps this **exactly**, with no new uniqueness logic needed:

- There is still exactly one `std::vector<MLTrade> trades` (Persistent Pointer 1, same slot).
- It now accumulates rows from 12 source slots (6 triggers × 2 sides) instead of 2, plus baseline,
  in the fixed order given in section 4.4 step 2 — a deterministic, input-driven order that does not
  depend on how many signals fired, so it is identical pass-to-pass for the same inputs and the same
  bar history (the same property TriggerLab already relies on).
- `trade_id` is assigned once, at CSV-write time, as the row's 1-based index in this single vector —
  same as today's `(int)i + 1`.

Because it is one global position counter across the ENTIRE vector (all 6 triggers, both sides, every
swept delay, baseline included), `trade_id` is unique across the **whole file**, which is strictly
stronger than the join key's actual requirement (`trade_id` unique within `(run_chart, run_bars,
run_delay)`). Six triggers cannot collide because there is only ever one counter, not six restarting
at 0 — that failure mode (the one the task explicitly warns about) does not exist in this design; it
would only exist if each trigger were given its own trade vector or its own counter, which this spec
deliberately does not do.

---

## 7. Trade cap

`ML_MAX_TRADES = 120000` (TriggerLab's `TL_MAX_TRADES = 20000`, scaled 6x). With 6 source triggers
instead of 1, the same fixed cap would be reached roughly 6x sooner on the same chart history — a
long-history, tight-session, low-overlap-skip run across 6 active triggers could plausibly produce
6x TriggerLab's row count. Scaling the cap proportionally preserves TriggerLab's actual intent (the
cap exists to show a LIMIT marker rather than silently truncate, not to be a routinely-hit ceiling on
well-configured runs) rather than making MultiLab hit it 6x more often for runs that would have been
fine as 6 separate TriggerLab runs. Same behavior on hit: stop appending, show a LIMIT marker on the
panel, do not silently truncate.

**Barrier file size warning carries over, worse:** barrier rows are already ~50 bytes/row at ~390
pairs/trade in TriggerLab; MultiLab's trade count is up to 6x higher per run (6 triggers sharing one
run instead of 6 separate runs), so a `range1.5`-class run with Barrier Sweep ON can plausibly produce
**350–470 MB raw** before gzip. This is accepted, per the task's framing, not a defect — `Testing/
CLAUDE.md`'s existing gzip step (`barrier_grid.py --gzip-after`, ~19x measured) is exactly the
mitigation already in place, and it runs on the raw file regardless of how many triggers contributed to
it. Nothing about the barrier file format changes to accommodate this — see section 2, schema
unchanged.

---

## 8. Inputs — full list (0-based `sc.Input[]` index)

**70 inputs total (indices 0–69).** Flagged in section 10 as something to verify against the actual
installed ACSIL header before compiling — see that section for why.

### Per-trigger block (6 inputs × 6 triggers = indices 0–35)

| Idx | Name | Default |
|---|---|---|
| 0 | Trigger 1 Enable | Yes |
| 1 | Trigger 1 Name | `"Trigger 1"` |
| 2 | Trigger 1 Long Source Chart Number (0 = this chart) | 0 |
| 3 | Trigger 1 Long Source Study+Subgraph | (0,0) |
| 4 | Trigger 1 Short Source Chart Number (0 = this chart) | 0 |
| 5 | Trigger 1 Short Source Study+Subgraph | (0,0) |
| 6–11 | Trigger 2 (same 6 fields) | `"Trigger 2"`, else same |
| 12–17 | Trigger 3 | `"Trigger 3"` |
| 18–23 | Trigger 4 | `"Trigger 4"` |
| 24–29 | Trigger 5 | `"Trigger 5"` |
| 30–35 | Trigger 6 | `"Trigger 6"` |

### Study-wide block (indices 36–69), values/limits identical to TriggerLab's inputs of the same name

| Idx | Name | Default | Limits |
|---|---|---|---|
| 36 | Fire Test | Non-Zero Rising Edge | 3-way choice |
| 37 | Fire Threshold | 0.0 | float |
| 38 | Entry Delay (bars) | 2 | 0–20 |
| 39 | Entry Price | Close of Entry Bar | 2-way choice |
| 40 | Overlapping Signals | Skip While In Trade | 2-way choice |
| 41 | Session Start Time | 09:30:00 | time |
| 42 | Session End Time | 16:00:00 | time |
| 43 | Flatten At Session End | Yes | bool |
| 44 | Horizon Mode | Bars | 2-way choice |
| 45 | Horizon Bars | 20 | 1–2000 |
| 46 | Horizon Minutes | 30 | 1–2000 |
| 47 | MFE Hit Target (ticks) | 20 | 1–10000 |
| 48 | Evaluate Stop/Target | No | bool |
| 49 | Target (ticks) | 20 | 1–10000 |
| 50 | Stop (ticks) | 10 | 1–10000 |
| 51 | Show Panel | Yes | bool |
| 52 | Panel Text Size | 10 | 6–32 |
| 53 | CSV Export | No | bool |
| 54 | CSV Path | `C:\SierraChart\Data\MultiLab_{chart}_{bars}_{delay}.csv` | string |
| 55 | Baseline Mode | No | bool |
| 56 | Baseline Entries Per Day | 5 | 1–200 |
| 57 | Delay Sweep | No | bool |
| 58 | Delay Sweep Max | 3 | 0–20 |
| 59 | Barrier Sweep | No | bool |
| 60 | Barrier Sweep: Stop Min (ticks) | 10 | 1–1000 |
| 61 | Barrier Sweep: Stop Max (ticks) | 40 | 1–1000 |
| 62 | Barrier Sweep: Stop Step (ticks) | 2 | 1–1000 |
| 63 | Barrier Sweep: Target Min (ticks) | 10 | 1–5000 |
| 64 | Barrier Sweep: Target Max (ticks) | 120 | 1–5000 |
| 65 | Barrier Sweep: Target Step (ticks) | 4 | 1–1000 |
| 66 | Barrier Sweep: Min Reward:Risk | 1.0 | 0.1–100.0 |
| 67 | Barrier Sweep: Include Baseline Rows | Yes | bool |
| 68 | Barrier Sweep: Min Delay To Sweep | 1 | 0–20 |
| 69 | Fired Mask Lookback (bars) | 0 | 0–50 |

---

## 9. CSV column list — main export

40 columns (TriggerLab's 36 + 4 new: `run_fired_mask_lookback`, `source_name`, `fired_mask`,
`fired_count`). New columns are **appended after their nearest TriggerLab neighbor**, not inserted
mid-block, so a human diffing the two headers sees the delta immediately.

```
run_chart,run_bars,run_delay,run_horizon_mode,run_horizon,run_fire_test,run_entry_price,
run_eval_st,run_target,run_stop,run_fired_mask_lookback,
trade_id,side,source_chart,source_study_id,source_sg,source_name,
signal_dt,knowable_dt,entry_dt,weekday,session_minute,
entry_price,delay_bars,horizon_end_dt,horizon_bars_actual,
mfe_ticks,mae_ticks,bars_to_mfe,bars_to_mae,horizon_pnl_ticks,
st_outcome,st_pnl_ticks,truncated_by_session,complete,
minutes_to_mfe,minutes_to_mae,minutes_to_st_exit,
fired_mask,fired_count
```

`side` values unchanged: `LONG`, `SHORT`, `LONG_BASE`, `SHORT_BASE`.

### Barrier file (`*_barriers.csv`) — unchanged, verbatim TriggerLab schema

```
run_chart,run_bars,run_delay,trade_id,side,complete,stop_ticks,target_ticks,outcome,pnl_ticks,minutes_to_exit
```

No `fired_mask` here by design (section 2) — join on `trade_id` to the main file if ever needed.

---

## 10. Acceptance test, per trigger

`Testing/CLAUDE.md`'s 40/40 acceptance test — "the barrier pair equal to `(run_stop, run_target)` must
reproduce the main file's `st_outcome`/`st_pnl_ticks` exactly" — applies **per trade_id**, and since
every trade_id in the file (regardless of which of the 6 triggers produced it) went through the
identical `MLBuildTradeAtEntry` → `MLSweepTradeBarriers` code path with the same pessimism rule, the
test does not need to be run "per trigger" as a separate procedure. `barrier_grid.py`'s
`run_acceptance()` already checks every row in the barrier file against every row in the main file by
`trade_id` — with 6 triggers contributing rows to the same file, a single acceptance run over the
pooled file covers all 6 triggers simultaneously (a mismatch in trigger 3's rows fails the same way a
mismatch in trigger 1's rows would). **No changes to `barrier_grid.py` are required** for this reason.
The only column analysts should additionally check post-hoc is that `source_name` (or
`source_study_id`/`source_sg`) is populated correctly per trigger — that is a data-quality check, not
part of the 40/40 test itself.

---

## 11. What a human should verify before compiling

Listed here, and repeated in the reporting-back summary — this spec is not claiming certainty on these:

1. **Input array size.** MultiLab uses 70 inputs (indices 0–69). TriggerLab, the largest prior study
   in this codebase, uses 39 (indices 0–38). Neither the trimmed `sierrachart.h` nor `scstructures.h`
   available in this environment states the ACSIL `sc.Input[]` array's actual upper bound — it was not
   findable in the installed headers used for this port. Other studies in this codebase already use
   `sc.Input[119]`, so 69 is well within the demonstrated range, but if Sierra Chart's real limit were
   somehow lower, the fix is straightforward (fold the 6 per-trigger Name inputs into the
   Study+Subgraph's own naming, or drop per-trigger short-side chart number when it's almost always the
   long chart) but it needs a compile attempt to know whether it's needed at all.
2. **`SCInputRef` array indexing at 69.** Same uncertainty as #1 — confirm the compiler doesn't clip or
   reject indices this high.
3. Everything in TriggerLab's own header comment marked "NON-NEGOTIABLE" is assumed unchanged and
   copied verbatim; this spec did not re-derive any of that reasoning, only ported it.
