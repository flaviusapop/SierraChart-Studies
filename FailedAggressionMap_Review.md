# Failed Aggression Map — Independent ACSIL Code Review v1.0

Scope: `TASK_FAILED_AGGRESSION_MAP.md` (86 lines) + `FailedAggressionMap_BuildSpec.md`
(237 lines) vs `FailedAggressionMap.cpp` (1221 lines). No implementation file was
edited. Static gate was executed. Sierra compilation is NOT claimed (no Sierra
build system in this environment).

Gate result: **12/12 PASS** (`gate_check.py FailedAggressionMap.cpp`, 1221 lines):

- PASS exactly one SCSFExport / braces balanced / no static-thread_local locals /
  no stray '#' comments / no float == on price arrays / no sc.Stochastic( /
  all inputs defaulted / all inputs read in logic / no duplicate input slots /
  S-> fields all in struct / SG max <= 58 / persist-ptr paired w/ LastCallToFunction

Verdict: **no BLOCKERs — every requested feature is present and non-stubbed.**
Formula signs, closed-bar discipline, and the evidence-state ladder are correct.
2 HIGH findings need Sierra-side verification / a small logic fix; the rest are
MEDIUM/LOW robustness issues. Nothing below invalidates the design.

## 1. Acceptance-criteria matrix (task order)

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 1 | Identity: file, `SCDLLName("FailedAggressionMap")`, `scsf_FailedAggressionMap`, GraphName `Failed Aggression Map v1.0`, `AutoLoop=0`, region 0, `MaintainVolumeAtPriceData=1` | PASS | `FailedAggressionMap.cpp:111,308,356-365` |
| 2 | Bubble at event bar + centroid, volume-scaled, min/max bounded | PASS | `FailedAggressionMap.cpp:920-922,1057-1081` |
| 3 | States `ATTEMPT -> FAILURE_CONFIRMED -> FIRST_RETEST -> RETEST_FAILED or ACCEPTED_THROUGH`, cluster != rejection, touch != failed retest | PASS | `FailedAggressionMap.cpp:122-128,701-783` |
| 4 | Closed-bar VAP only; lookback dflt 2; prox 3; minblocks 3; 95/350; int-tick geometry + delta-weighted centroid; confirm 3 bars / 4 ticks; retest from displaced side + overlap; exit 2 ticks; accept far-edge + 1; no future data; replay == live | PASS | `FailedAggressionMap.cpp:556-576,786-1008` |
| 5 | Manual/Auto/Both dflt Manual; Auto = 5 prior COMPLETE days excl. current, no lookahead, once-per-day cache; per-day percentile (85 block / 80 zone), median of 5, clamp >= 1; warmup warning + Auto silent when short; Both keeps Manual; Manual precedence any-side inclusive intersect; same-side+same-source merge only; source stored+exported; Auto colors distinct | PASS | `FailedAggressionMap.cpp:582-694,790-918,1053-1055` |
| 6 | Mapped source via `SetChartStudySubgraphValues`, disabled dflt; nonzero on detection bar = confluence; never suppresses; halo + SG flag | PASS (with HIGH compile-risk note H-1) | `FailedAggressionMap.cpp:402-403,468-469,481-484,996-997,1083-1089` |
| 7 | 4 rendering modes, default Bubble + Ribbon | PASS | `FailedAggressionMap.cpp:405-407,1033-1035` |
| 8 | Buyers magenta/red, sellers cyan/blue, gold halo, ring on first retest, arrow on failed retest, X + terminate on accept, inferred-inventory label, object cap | PASS (arrow-position note M-4) | `FailedAggressionMap.cpp:412-414,1049-1171` |
| 9 | SG outputs < 60, all transitions downstream-readable, stale cleared | PASS with notes (M-1, M-2, M-6) | 14 SGs `FailedAggressionMap.cpp:336-349,367-382,537-553` |
| 10 | Append-only inputs/SGs, documented indexes | PASS (spec-heading note L-4) | 23 inputs `FailedAggressionMap.cpp:311-333,384-416` |
| 11 | Replay-safe persistent state, `LastCallToFunction` cleanup, fixed-capacity, no STL in persistent state | PASS (dead-counter note L-3) | `FailedAggressionMap.cpp:150-189,424-439,508-536` |
| 12 | Every missed closed bar processed after reconnect/backfill | PASS live tail; history-rewrite gap, see H-2 | `FailedAggressionMap.cpp:562-576` |
| 13 | Correct drawing namespace / deletion API | PASS | `FailedAggressionMap.cpp:195-214,244-302` |
| 14 | Settings fingerprint rebuilds immediately | PASS with truncation note M-3 | `FailedAggressionMap.cpp:489-506` |
| 15 | No float price equality, int64 delta math | PASS (precision notes L-1, L-2) | `FailedAggressionMap.cpp:224-227,630-639,809-820` |
| 16 | No history alerts; live watermark only | PASS (single-alert note L-5) | `FailedAggressionMap.cpp:1188-1219` |

## 2. Findings

### HIGH

#### H-1 — `SetChartStudySubgraphValues` is task-correct but unverifiable here; every repo precedent uses the other name
Location: `FailedAggressionMap.cpp:403`.

```cpp
In_MappedSrc.SetChartStudySubgraphValues(0, 0);
```

The task (`TASK_FAILED_AGGRESSION_MAP.md:53`) explicitly requires
`SetChartStudySubgraphValues`. The code complies. But no Sierra header exists
locally to confirm the symbol, and every in-repo precedent
(`OrderflowSignalV2.cpp:187,236`, `OrderflowSignalV3.cpp:220,269`,
`OrderflowConfluence.cpp:165`, `BreadthCompositeOscillator.cpp:200`) uses
`SetStudySubgraphValues` + `sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, ...)`
(the BuildSpec Sec. 7 as-built note admits this). If the target SC version only
exposes `SetStudySubgraphValues`, F5 compile fails on this one line.
Concrete fix (Sierra side, one line): if the compile errors on this symbol,
replace with `In_MappedSrc.SetStudySubgraphValues(0, 0);` — the read path
(`FailedAggressionMap.cpp:484`, same 4-arg
`GetStudyArrayFromChartUsingID(ChartNumber, studyID, subgraph, array)` shape as
`OrderflowSignalV3.cpp:342-345`) is already compatible with both spellings, and
the getters (`GetStudyID`/`GetSubgraphIndex`, lines 468-469) match repo usage.
Must be confirmed at F5 compile; do not "fix" blindly here since the task
mandates the current spelling.

#### H-2 — Incremental SG clear can wipe history that is never recomputed after a history rewrite/backfill
Locations: `FailedAggressionMap.cpp:544-567`.

```cpp
const int clrFrom = sc.UpdateStartIndex < 0 ? 0 : sc.UpdateStartIndex; // 547
for (int sgi = 0; sgi <= 13; sgi++)
    for (int bi = clrFrom; bi < sc.ArraySize; bi++)                     // 550-551
        sc.Subgraph[sgi][bi] = 0.0f;
...
const int procStart = isFullRecalc ? 0 : (pState->PrevLastClosed + 1 ...); // 567
```

`isFullRecalc` is only `(sc.UpdateStartIndex == 0)` or fingerprint/state-null
(lines 500-506). If SC rewrites history with `UpdateStartIndex = K > 0` (backfill
correcting bars `K..PrevLastClosed`), the code zeroes SGs on `[K, ArraySize)`
but reprocesses only `(PrevLastClosed, lastClosed]` — bars `[K, PrevLastClosed]`
lose their already-computed pulses permanently until the next full recalc. Live
tail processing (reconnect gap with no rewrite) is correct.
Concrete fix: treat a history rewrite as a full rebuild, e.g.

```cpp
if (!isFullRecalc && sc.UpdateStartIndex <= pState->PrevLastClosed)
    isFullRecalc = true;   // history under us changed: rebuild, don't patch
```

placed before the clear block. Cost is one full recalc on backfill, which is
exactly what replay-determinism requires.

### MEDIUM

#### M-1 — Merged candidates publish `SG_Top/SG_Bot` without their `SG_CandSide` pulse triple
Locations: `FailedAggressionMap.cpp:924-938` vs `999-1006`.

On the merge path the code updates `target` range/totals/centroid and writes
only `SG_Top[w]`/`SG_Bot[w]`, then `continue`s — `SG_CandSide/Centroid/Strength/
Mapped/Source/EventID[w]` stay 0 while Top/Bot are nonzero at bar `w`. A
downstream consumer gating on `SG_CandSide != 0` will miss a detection bar whose
Top/Bot moved. Concrete fix: on merge, also write the pulse triple for `w`
(side, merged centroid price, merged strength, mapped check for `w`, source,
target ID), or deliberately document that merges are silent and remove the two
Top/Bot writes so bar `w` stays fully zero.

#### M-2 — Merged `Strength10` reflects only the current window, not the merged total
Location: `FailedAggressionMap.cpp:920-934`.

`strength` is computed from the current window `total` (line 920), then assigned
to `target->Strength10` (line 934) after `TotalDelta`/`TickWeight` were already
accumulated (lines 929-930). Bubble radius (`1057`) and `SG_Strength` therefore
understate merged events. Concrete fix: recompute from the merged magnitude:

```cpp
const int64 magAll = target->TotalDelta >= 0 ? target->TotalDelta : -target->TotalDelta;
float mStr = (float)((double)magAll * 10.0 / ((double)zTh * 3.0));
if (mStr < 0.5f) mStr = 0.5f; if (mStr > 10.0f) mStr = 10.0f;
target->Strength10 = mStr;
```

using the `zTh` of the merging pass (same-mode merge implies same source, but in
`Both` mode Manual and Auto passes have different `zTh` — merging only happens
within one pass, so capture it).

#### M-3 — Settings fingerprint truncates `manBlock/manZone` to 16 bits; large threshold edits alias
Location: `FailedAggressionMap.cpp:491`.

```cpp
const int fp1 = (int)(manBlock & 0xFFFF) | ((int)(manZone & 0xFFFF) << 16);
```

`Manual Block Delta` allows up to 1,000,000 and `Manual Cluster Delta` up to
10,000,000, but only the low 16 bits participate. Edits differing by exactly
65536 (e.g. 350 -> 65886, both in range) do not trigger the immediate rebuild
the task requires. Practical likelihood is low, but the fix is trivial —
fold the high bits in:

```cpp
const int fp1 = (int)(manBlock ^ (manBlock >> 16)) * 31
              ^ (int)(manZone ^ (manZone >> 16));
```

(and similarly keep the other words). Display-only inputs remain excluded,
which is correct.

#### M-4 — Failed-retest arrow drawing disagrees with its own `SG_Marker` price
Locations: `FailedAggressionMap.cpp:726,773` vs `1135-1139`.

`SG_Marker[w]` correctly records `sc.Close[w]` at fail/accept bars, but the
drawn `FAIL SHORT/LONG` text is anchored at `(TopTick+1)` / `(BotTick-1)` —
a fixed cluster-edge offset, not the fail price. Either anchor the text at
`sc.Close[FailBar]` to match the exported marker, or document that the text is
a cluster-edge label while `SG13` is the price. Current state sends two prices
for one event.

#### M-5 — Auto calibration samples truncated day-open windows that live detection never emits
Locations: `FailedAggressionMap.cpp:619-626` vs `786-787`.

Live detection skips `w < lookback - 1`; the per-day Auto sampler instead
clamps `ws = max(we - lookback + 1, dayStarts)` and samples the short window.
Day-open truncated windows contribute to the `lvl`/`tot` percentile pools while
being undetectable live — a mild train/serve skew (largest with lookback >> 1
on short days). Concrete fix: in the sampler, `if (we - dayStarts[dd] + 1 <
lookback) continue;` so only full-length windows calibrate thresholds.

#### M-6 — SG12/SG13 draw styles contradict the BuildSpec table
Locations: `FailedAggressionMap.cpp:379-382` vs `FailedAggressionMap_BuildSpec.md:176-177`.

Spec table: SG12 `CIRCLE, hollow` at confirmation bar; SG13 `POINT` at fail/accept.
Code: SG12 `DRAWSTYLE_POINT`, SG13 `DRAWSTYLE_DIAMOND`. The task itself does not
dictate marker styles ("use standard subgraph marker styles where practical"),
so this is spec-vs-code drift, not a task violation. Fix by aligning the spec
table to the code (POINT/DIAMOND with `LineWidth 3` are the reasonable choices
— there is no hollow-circle point style that survives `DrawZeros=0` cleanly) or
vice versa before Sierra replay.

### LOW

#### L-1 — Centroid rounding via `double` loses exactness for very large accumulated volume
Locations: `FailedAggressionMap.cpp:880,933`.

```cpp
const int centroid = (int)((double)tickW / (double)total + 0.5);
```

`tickW` is `int64` tick*volume; beyond 2^53 (~9e15) the `double` division is
inexact. Realistic footprint volumes never approach this, and the result feeds
an integer tick, so impact is nil — but integer division with remainder is exact
and equally short: `(int)((tickW + total/2) / total)` for the positive-magnitude
convention used on both sides. Same at line 933 with `magT`.

#### L-2 — `FAMPriceToTick` (`FailedAggressionMap.cpp:224-227`) uses `floorf(p/t + 0.5f)`
Correct for all non-negative prices (all realistic futures charts), wrong for
negative prices (only the 2020 oil anomaly). Either document "non-negative
prices assumed" or use `floorf(p/t + 0.5f)` for `p >= 0` / `ceilf(p/t - 0.5f)`
otherwise. No action needed for the target instruments.

#### L-3 — `FAMState.Count` is maintained but never read
Locations: `FailedAggressionMap.cpp:176,522,969,994,1181`.

Incremented/decremented on insert/evict/prune, never consumed (draw phase
rescans `Used` flags into `liveIdx`). Harmless; remove the field or use it to
assert `Count == nLive` in the draw phase to catch state corruption early.

#### L-4 — BuildSpec Sec. 11 heading says "24" inputs; there are 23 (indexes 0-22)
Locations: `FailedAggressionMap_BuildSpec.md:179` vs `FailedAggressionMap.cpp:311-333`.

Count the table rows: In:1..In:23 = 23 inputs, matching `sc.Input[0..22]`
(state `S->` gate check passes, no duplicate slots). The "24" is a heading typo;
fix the number so a future append-only audit does not miscount the next free
slot (which is index 23).

#### L-5 — One alert per transition type per bar; acceptance has no alert
Locations: `FailedAggressionMap.cpp:1195-1218`.

The watermark loops `break` after the first event with `ConfirmBar/FailBar ==
lastClosed`, so co-confirming events on the same bar produce one alert. That is
arguably correct anti-spam behavior and satisfies "live watermark, never from
history" (guard `alertsOn && !isFullRecalc`, anchored `sc.ArraySize - 1`).
Acceptance-through intentionally has no alert — fine, but document it so Sierra
replay testers do not file it as a bug.

#### L-6 — Dead clamp `rb = max(endBar, eb)` in the draw phase
Locations: `FailedAggressionMap.cpp:1096,1162`.

`RibbonEndBar >= DetectBar` is invariant (init `RibbonEndBar = w` at insert,
only advanced afterwards), and both sides are clamped by `lastClosed` identically
(lines 1061-1062), so `endBar >= eb` always holds and `rb == endBar`. Harmless
defensive code; leave or assert the invariant instead.

## 3. What was verified clean (no finding)

- VAP iteration `INT_MIN` + `GetNextHigherVAPElement` matches the
  `TrappedTraders.cpp:408-416` precedent; per-tick `int64 Ask-Bid` accumulation
  (`FailedAggressionMap.cpp:638,819`) avoids the float-delta path TrappedTraders
  uses.
- Directional signs: high side `delta >= +bTh` -> buyers `+1`, failure is
  displacement DOWN (`close <= Bot - adv`, `113-...732-734`); low side mirror
  with magnitude compare (`861-863`), sellers `-1`, failure UP. Retest overlap
  predicates (`751-752`), exit predicates (`763-765`), far-edge acceptance
  (`717-719`, checked before exit) and `FAIL_ENTRY` polarity (short on failed
  buyers, long on failed sellers, `772`) are all correct per Sec. 5.
- No lookahead: engine order per bar is advance-then-detect (`701-1008`);
  detection window `[w-L+1, w]` and transitions use only bar `w`;
  forming bar `ArraySize - 1` never enters logic (`558-565`); Auto day scan
  starts at `w - 1` and skips `d >= dayW` (`595-599`).
- Merge/dedupe uses inclusive integer-tick intersection incl. touching ranges
  (`216-222`); cross-side never merges; Manual precedence suppresses Auto
  regardless of side against live (non-discarded/accepted) Manual events, and
  Manual pass `t = 0` runs before Auto `t = 1` so same-bar suppression works
  (`790-918`).
- Mapped read is once per call (`481-484`), closed-bar indexed (`996`), never
  suppresses (`999` always executes), halo + `SG9` only (`1083-1089,1004`).
- Drawings: single namespace bases 110000-180000 + ID, warn 199999
  (`135-143`); `UTAM_ADD_OR_ADJUST` adds, `DeleteACSChartDrawing(CHART,
  TOOL_DELETE_CHARTDRAWING, N)` deletes (`195-214`) — matches BigTradesTape
  precedent; text uses BeginIndex+price exactly like `BTLabel`
  (`BigTradesTape.cpp:271-293` vs `287-302`).
- Persistent struct: fixed `Events[128]`, no STL inside, all 19 fields
  cross-checked as used; freed + nulled on `LastCallToFunction` after
  collapsing drawings (`424-439`); capacity pressure evicts discarded, then
  oldest-accepted, else skips new (`946-965`) — bounded growth holds.
- Alerts: `alertsOn && !isFullRecalc`, transition-on-`lastClosed` only,
  two watermarks, anchored `sc.ArraySize - 1` (`1188-1219`) — matches the
  CLAUDE.md post-loop watermark pattern.

## 4. Sierra compile / replay checklist (exact, still required)

1. F5 compile `FailedAggressionMap.cpp` in Sierra; first confirm whether
   `SetChartStudySubgraphValues` (line 403) resolves — see H-1 for the one-line
   fallback. Resolve any `s_VolumeAtPriceV2::{Ask,Bid}Volume` type mismatch at
   lines 638/819 (code assumes integer volumes castable to `int64`).
2. Footprint chart with VAP enabled (`MaintainVolumeAtPriceData` path, line 365);
   Ctrl+Insert full recalc; confirm no warmup warning in Manual, warning visible
   in Auto until 5 complete days exist.
3. Replay Manual vs Auto vs Both on the same chart: Both must equal Manual plus
   non-intersecting Auto events; verify an Auto candidate intersecting a live
   Manual range is suppressed while same-side/same-source overlaps merge with
   preserved totals (M-2 changes this math — re-verify bubble sizes after fix).
4. Mapped-source halo check: attach a study-subgraph source, confirm gold halo +
   `SG9 = 1` on detection bars and zero suppression of raw detections.
5. Rendering-mode switch (display-only) + `Max Events Drawn` cap: no rebuild,
   older drawings collapse; structural edits (In:1-15) must visibly rebuild.
6. Backfill/reconnect: force a history rewrite and confirm SG pulses survive
   (H-2 fix) and `PrevLastClosed` resumes without double transitions.
7. Alerts enabled: confirm alerts fire only for live `lastClosed` transitions,
   one per type per bar, anchored to the current bar.

## 5. Deliverables / git state

- Review file: this document (`FailedAggressionMap_Review.md`).
- Implementation, spec, README, PROJECT_HISTORY commits are pre-existing on
  `feature/failed-aggression-map` (`e3f1df0 Add Failed Aggression Map v1.0
  standalone study + build spec`); `git status --short` is clean.
## 6. Fix-engineer dispositions (2026-09-03, post-review changes)

Gate after fixes: **12/12 PASS** (`gate_check.py FailedAggressionMap.cpp`, 1262 lines).
`TrappedTraders.cpp` / `LiquidityZones.cpp` untouched. No BLOCKERs existed.

- H-1 (mapped-input symbol): NO CODE CHANGE — finding is a Sierra-side
  compile risk, not a defect. The task mandates `SetChartStudySubgraphValues`
  and the code complies. The one-line `SetStudySubgraphValues(0, 0)` fallback
  is now documented in the code header and BuildSpec Sec. 7. Must be confirmed
  at F5 compile.
- H-2 (history-rewrite SG wipe): FIXED. `UpdateStartIndex <= PrevLastClosed`
  on a non-recalc call now forces a full rebuild before the SG clear block.
- M-1 (merge bar pulse triple): FIXED. Merge path now publishes side,
  merged centroid, Top/Bot, merged strength, mapped flag, source, and event ID
  for bar `w`, and ORs the current-bar mapped check into the target.
- M-2 (merged strength understated): FIXED. Strength recomputed from the
  merged magnitude with the merging pass `zTh`, clamped 0.5..10.
- M-3 (fingerprint 16-bit truncation): FIXED. Full 64-bit magnitudes folded
  (`b*31 ^ z` over `v ^ v>>16 ^ v>>32`); display-only inputs still excluded.
- M-4 (arrow vs SG13 price): FIXED. `FAIL SHORT/LONG` text anchored at
  `sc.Close[FailBar]`, matching `SG_Marker`.
- M-5 (Auto sampler skew): FIXED. Sampler skips truncated day-open windows
  (`we - dayStarts + 1 < lookback`), so calibration uses only full-length
  windows live detection can emit.
- M-6 (SG12/SG13 style drift): FIXED IN SPEC. BuildSpec Sec. 10 table aligned
  to the code (SG12 POINT w3, SG13 DIAMOND w3); task does not dictate styles.
- L-1 (double centroid rounding): FIXED. Exact integer
  `(tickW + total/2) / total` (positive-magnitude convention) at both sites.
- L-2 (negative-price rounding): FIXED. `FAMPriceToTick` rounds half away
  from zero on both sides.
- L-3 (`Count` never read): NO CHANGE — harmless dead counter maintained
  alongside `Used` flags; removal churn not justified.
- L-4 (spec "24 inputs"): FIXED. Heading now reads 23 (indexes 0-22; next
  free slot is index 23).
- L-5 (alert coalescing / no accept alert): DOCUMENTED in code header and
  BuildSpec Sec. 12; behavior unchanged (one alert per type per bar, live only).
- L-6 (dead `rb` clamp): NO CHANGE — invariant `RibbonEndBar >= DetectBar`
  holds; defensive clamp left as-is.

## 7. Header Audit disposition (2026-09-03, exact Zander headers, SC_DLL_VERSION 2927)

Method: every `sc.*`, input, subgraph, drawing-constant, `s_UseTool`-field,
VAP, and array API used by `FailedAggressionMap.cpp` was checked with exact
word-boundary search against `sierrachart_zander.h` + `scstructures_zander.h`
+ `scconstants_zander.h`. `VAPContainer.h` / `SCString.h` are included but not
copied locally, so those two symbols were verified via in-repo compiled-study
precedent instead. No design change; no other study touched.

Gate after fixes: **12/12 PASS** (`gate_check.py FailedAggressionMap.cpp`, 1266 lines).

- C-1 (BLOCKER, FIXED) — `DRAWING_ELLIPSE` does not exist. Exact header
  declares `DRAWING_ELLIPSEHIGHLIGHT = 13` (and `DRAWING_RECTANGLEHIGHLIGHT`,
  `DRAWING_TEXT`); word-boundary search for `DRAWING_ELLIPSE` returns 0 hits —
  the earlier substring match was a false positive on the longer name. No
  other study uses this constant (only `FailedAggressionMap.cpp:261` did).
  Fixed to `DRAWING_ELLIPSEHIGHLIGHT` in `FAMDrawEllipse`; code header and
  BuildSpec Sec. 8 now name the correct constant.
- C-2 (BLOCKER, FIXED) — `SetChartStudySubgraphValues(0, 0)` arity mismatch.
  Exact header (`scstructures_zander.h:2884`) declares the 3-arg form
  `SetChartStudySubgraphValues(int ChartNumber, int StudyID, int SubgraphIndex)`
  (`CHART_STUDY_SUBGRAPH_VALUES`); the 2-arg call cannot compile. Fixed to
  `SetChartStudySubgraphValues(0, 0, 0)` (disabled default preserved, task
  spelling preserved). The `SetStudySubgraphValues(0, 0)` fallback stays
  documented for older SC versions. This supersedes the H-1 "no change"
  disposition: the symbol exists, only the arity was wrong. Read path
  (`GetStudyArrayFromChartUsingID(ChartNumber, studyID, subgraph, array)`,
  4-arg overload confirmed at `sierrachart_zander.h:1801`) and getters
  (`GetStudyID`/`GetSubgraphIndex` both handle `CHART_STUDY_SUBGRAPH_VALUES`)
  are unchanged and compatible with both spellings.
- Verified clean (exact-header hits): `GetStudyArrayFromChartUsingID` (both
  overloads), `GetTradingDayDate(const SCDateTime&)`, `SetAlert(int, int,
  const SCString&)` (char-buffer callers convert implicitly, same as
  `OrderflowSignalV3.cpp:642`), `DeleteACSChartDrawing(int, int, int)`,
  `GetPersistentPointer`/`SetPersistentPointer`, `UseTool(s_UseTool&)`,
  `SetCustomInputStrings`/`SetCustomInputIndex`, `SetInt`/`SetFloat` +
  limits, `SetColor`/`GetColor`, `SetYesNo`/`GetYesNo`, `GetInt`/`GetFloat`/
  `GetIndex`, all `s_UseTool` fields used (`BeginIndex`, `Begin/EndDateTime`,
  `Begin/EndValue`, `SecondaryColor`, `TransparencyLevel`, `FontSize`,
  `AddMethod`, `Text`), `DRAWSTYLE_IGNORE/POINT/DIAMOND`,
  `SCALE_SAMEASREGION`, `TOOL_DELETE_CHARTDRAWING`, `UTAM_ADD_OR_ADJUST`,
  subgraph `operator[](int)`, `SC_SUBGRAPHS_AVAILABLE = 60` (14 SGs) /
  `SC_INPUTS_AVAILABLE = 128` (23 inputs), `High/Low/Close/BaseDateTimeIn/
  TickSize/ArraySize/UpdateStartIndex/ChartNumber/LastCallToFunction/
  SetDefaults/AutoLoop/GraphRegion/MaintainVolumeAtPriceData/
  VolumeAtPriceForBars`, `COLORREF` via `windows.h` (same as TrappedTraders),
  `T.Text = char*` (same as `TrappedTraders.cpp:714`).
- Precedent-verified (header include not copied locally, no change):
  `GetNextHigherVAPElement` + `s_VolumeAtPriceV2::{Ask,Bid}Volume` match
  `TrappedTraders.cpp:409-410` and `LiquidityZones.cpp:234` exactly
  (`INT_MIN` start, `(unsigned int)bar, tick, &pVAP`); `VAPContainer.h` is
  `#include`d by the exact `sierrachart.h`, so absence from the local cache
  is a copy gap, not a defect.
- Sierra checklist delta: F5 compile should now pass the two fixed lines;
  remaining Sierra-side confirmations are VAP-type behavior at runtime
  (unchanged code path) and the standard replay/mapped/halo/rendering checks
  in Sec. 4 (unchanged).
