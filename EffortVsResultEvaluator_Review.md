# EffortVsResultEvaluator — Independent Final Review

Date: 2026-09-04. Scope: `EffortVsResultEvaluator_BuildSpec.md` (v1.0, 296 lines),
`EffortVsResultEvaluator.cpp` (1514 lines), `tests/test_effort_vs_result_evaluator.cpp`
(460 lines), `EffortVsResult.cpp` (830 lines, read-only reference), `.agent-context/`
headers plus exact Zander headers at `/home/ubuntu/.hermes/cache/`.
No production, test, spec, or history file was modified. No commit made.

## Verdict: PASS — no blocking defects

The implementation conforms to the build spec on every audited dimension.
Portable suite and repo-local gate both pass cleanly (evidence below).
Two LOW-severity observations are recorded; neither blocks use, and neither
requires a code change. **There is no blocking defect; no further work is
required by this review.** (Sierra F5 compile + chart replay remain
orchestrator-owned, as with every study in this repo — not a finding.)

## Evidence (fresh runs, unmodified tree)

```
checks=103 fails=0
ALL EVR EVALUATOR TESTS PASSED
PORTABLE_EXIT=0
BUILD_EXIT=0 (g++ -std=c++17 -Wall -Wextra -O2, warning-free)

PASS exactly one SCSFExport
PASS braces balanced
PASS no static/thread_local locals
PASS no stray '#' comments
PASS no float == on price arrays
PASS no sc.Stochastic(
PASS all inputs defaulted
PASS all inputs read in logic
PASS no duplicate input slots
PASS S-> fields all in struct
PASS SG max <= 58
PASS persist-ptr paired w/ LastCallToFunction

12/12 checks pass | 1514 lines
GATE_EXIT=0
```

## Findings (severity-ranked)

### BLOCKER — none

Explicitly: no lookahead, no timing error, no stop/target inversion, no
censoring misclassification, no CSV equivocation, no unsafe API, and no
memory-safety defect was found. Do not invent work from this review.

### HIGH — none

### MEDIUM — none

### LOW-1 — Cumulative SGs not carried onto the last closed bar (cosmetic)

- Lines: main loop bound `EffortVsResultEvaluator.cpp:974`
  (`for (int j = 1; j <= lastClosed - 1; j++)`); deterministic clear
  `911-937` zeroes SG 15/16/17 on every bar; carry writes at
  `1035-1038`, `1044-1047`, `1069-1072`, `1285-1288` cover only loop bars;
  forming-bar carry at `1310-1313` covers `sc.ArraySize-1`.
- Effect: SG 15 (Cumulative Included Events), SG 16 (Cumulative Average R),
  SG 17 (Cumulative Win Rate Percent) read `0` at index `lastClosed`
  (`sc.ArraySize-2`) even when `cumN > 0`. All SGs are `DRAWSTYLE_IGNORE`
  and the CSV export does not consume cumulative SGs, so there is no
  measurement or export impact — only a one-bar display gap for any
  consumer reading cumulative state at the newest closed bar.
- Optional (not required): write the same three carry values at
  `lastClosed` after the loop. Left to orchestrator discretion.

### LOW-2 — Persistent slot 1 is write-only; header comment overstates it (docs)

- Lines: `586` (slot constant), writes at `824`, `946`, `1316`; no
  `GetPersistentInt(EVRE_INT_BARS)` read anywhere in the study body.
  Header block `75-79` describes slot 1 as a "new-bar / intrabar-tick
  guard", but the full-recalc-only design (`818-819`: any non-full-recalc
  call returns before touching state) needs no bar guard — the slot is
  informational only.
- Effect: none at runtime. Optional one-line comment correction.
  Slots 2-3 are correctly unused (no alerts per spec §1) and slots 4-6
  hold the structural fingerprint (`784-807`); no slot conflicts.

### INFO (not a defect) — CSV `entry_datetime` under Confirmation-Close entry

- Lines: entry selection `1077-1078` (Confirmation Close uses `Close[j]`
  while traversal still starts at `j+1`); CSV timestamps `1414-1416`
  (`confirmation = j`, `entry = j+1` start in both modes).
- Using the `j+1` bar start as the entry datetime for a close-priced entry
  is defensible (the close is known when `j+1` opens) and the spec (§11)
  does not define per-mode entry-datetime semantics. No action needed;
  recorded only so external tooling does not misread it as the traversal
  start in one mode and the fill time in the other.

## Audit detail (dimension-by-dimension)

- **Event timing / no signal lookahead.** Signals come only from source
  arrays at confirmation bar `j` (`976-987` via `EvrSelectEvent`);
  candidate bar is exactly `j-1` (`1079-1080`, `1091-1093`); loop bound
  `974` guarantees `j+1` is closed; forming bar is never a signal or
  outcome source (`lastClosed = ArraySize-2` at `821`, explicit forming
  zeros at `1292-1314`). No future-bar data enters signal selection.
- **Confirmation-Close isolation.** Entry price `Close[j]` (`1077`) but
  the forward scan always starts at `firstBar = j+1` (`1076`, `1138-1147`,
  `1151`), so the confirmation bar's high/low can never stop out or
  target-qualify the trade. Portable test `T4` proves sensitivity (scan
  from bar 0 would stop out; scan from bar 1 times out).
- **Serial-day conversion.** Every ACSIL horizon/holding computation goes
  through `EvrSecondsBetween` (day-difference × 86400.0, `531-534`) at
  `1132`, `1141-1143`, `1186-1187`, `1215-1216`, `1227-1228`, `1245-1246`;
  portable core works purely in seconds (`345-494`). No raw day-unit
  comparison against second-unit horizons. Matches `BigTradesTape.cpp`
  (`GetAsDouble` + 86400.0) precedent.
- **Directional stops.** `EvrComputeTrade` (`295-336`) rejects long stops
  at/above entry (`315`) and short stops at/below entry (`321`), plus
  non-positive tick size / target-R / risk distance. Spec §5
  `invalid_risk` semantics implemented exactly; covered by `T13`.
- **Long/short symmetry.** Scan branches mirror exactly (`1159-1166`
  long vs `1168-1176` short); MFE/MAE, timeout-R sign, and ambiguity
  handling are symmetric; `T1`/`T2` assert mirrored outcomes.
- **Right-edge censoring.** Chart must cover the full horizon
  (`1132-1135` ACSIL; `380-385` portable) or status is `-4`
  `insufficient` with `evaluated = 0`, never a timeout. Only `evaluated`
  events touch cumulative aggregates (`1276-1284`).
- **First-touch ambiguity.** Dual-touch is tested before single touches
  (`1181` vs `1208`/`1220`; portable `429` vs `455`/`466`) with
  `>=` / `<=` touches only — no float equality on prices. Mark/Exclude
  reports MFE/MAE with `R = 0` and excludes from aggregates; Stop-First
  and Target-First resolve deterministically. Covered by `T1`/`T2`.
- **Overlap.** `EvrOverlapCheck` (`502-519`): entry at or before the prior
  *included* event's exit bar flags/excludes (`505`), matching spec §8's
  "prior included event" wording; gate runs before the forward scan
  (`1114-1123`) so excluded events cost no scan and never touch
  aggregates. Boundary `entry == exit` counts as overlap; covered by
  `T10`.
- **Date/session population.** Confirmation-bar eligibility via
  `GetTradingDayDate` (`1007`) + `GetTimeInSeconds` (`1008`) against
  `EvrDateEligible`/`EvrSessionEligible` (`1013-1016`); midnight-crossing
  windows and start==end full-day semantics (`171-189`) covered by `T8`;
  leap-aware `YYYYMMDD` validation (`123-166`) covered by `T9`; invalid
  bounds and start>end each log one message with no evaluation
  (`881-899`). Population gate runs before conflict handling
  (`1042-1049`), so out-of-population bars stay omitted/zero per spec.
- **Source SG mapping.** `EVRE_SRC_*` constants (`592-598`: 2/5/6/7/8/9/10)
  match `EffortVsResult.cpp` subgraphs exactly — SG2 effort (`189`), SG5
  raw failure (`192`), SG6 smoothed (`193`), SG7 buyer pulse (`194`), SG8
  seller pulse (`195`), SG9 buyer arrow (`196`), SG10 seller arrow
  (`197`). Read-only; source formulas never reimplemented
  (`25` in spec; no effort/reward math in this file).
- **Full-recalc behavior.** Evaluation + CSV run only when `isFullRecalc`
  (`809-819`: `UpdateStartIndex == 0`, `IsFullRecalculation`, or
  structural fingerprint change). Fingerprint (`787-802`) covers all
  structural inputs and excludes CSV enable/prefix per spec §10. All 18
  SGs are deterministically cleared on rebuild (`911-937`).
- **CSV SG-serialization equivalence.** The export (`1318-1510`) is a
  read-only pass over computed evaluator SGs; the region contains exactly
  one `EvrSelectEvent` (kind-string recovery only, `1401`) and zero
  references to `EvrComputeTrade` / `EvrEvaluateEvent` / `EvrOverlapCheck`
  / `horizonSec` / `ambP` / `targetR` / `bufTicks` / `entMode` (verified
  by region grep). Status `-5` emits `event_source "conflict"`; `risk_ticks`
  derives solely from SG stop/entry and tick size (`1425-1427`), staying
  `0` for invalid-risk/conflict rows. All configured-population statuses
  (`1/-1/2/3/-2/-3/-4/-5`) are exported; `status 0` rows skipped
  (`1390-1392`); date/session gating applied before export (`1387-1389`).
- **Repeat Windows export preservation.** Overwrite-only (`"w"`, never
  append) deterministic filename (`1341-1342`,
  `<prefix>_<symbol>_Chart<chart>_Study<instance>.csv` per spec §11);
  temp-file write with `fflush`/`ferror`/`fclose` checks (`1443-1448`);
  any failure preserves the prior CSV and logs one message (`1449-1457`);
  dest→`.bak` staging before replace handles Windows `rename`
  semantics, with restore on failure (`1460-1508`).
- **CSV safety.** Symbol and prefix sanitized to `[A-Za-z0-9_.-]`
  (`EvrSanitize`, `538-557`); empty prefix → `EVR_Evaluator`, empty
  symbol → `NOSYM` (`EvrBuildCsvName`, `561-576`); path confined under
  `sc.DataFilesFolder()` with backslash join (`1344-1350`); fixed header,
  dot-decimal `%f` numerics, serial-double datetimes (no locale dates);
  `event_source` drawn from a fixed four-string set, so chart-symbol
  text can never inject formulas or separators.
- **Unsupported ACSIL APIs — none.** Verified against exact Zander
  headers: `GetStudyArrayFromChartUsingID` (4-arg,
  `sierrachart_zander.h:1801`), `GetTradingDayDate` (`:2837`),
  `GetBarPeriodParameters` (`:3461`) with `n_ACSIL::s_BarPeriod` fields
  (`scstructures_zander.h:3258+`), `DataFilesFolder` (`:3291`),
  `StudyGraphInstanceID` (`:2869`), input getters/setters incl.
  2-arg `SetStudySubgraphValues` (`scstructures_zander.h:2900`) and
  `SetTime(int)` (`:2755`); `GetAsDouble`/`GetTimeInSeconds` match
  repo-wide precedent (`BigTradesTape`, `BreadthCompositeOscillator`,
  `InterestMap`); `SCString::Format` `%s/%d/%f` matches `BigTradesTape`
  precedent. No orders, alerts, drawings, networking, or `GetMovAvgType`.
- **Local/static memory safety.** No heap, no STL, no persistent
  pointers; the sole function-local `static` is the immutable
  `kDays[12]` lookup table (`125`); file-scope `static`s are functions
  and `const int` configuration constants (`586-601`) — no mutable
  shared state, satisfying spec §12. All stack buffers are
  size-guarded (`snprintf` at `862-904`, `1572`; `EvrSanitize`
  truncation at `546-554`; `EvrBuildCsvName` terminator at `574`).

## Acceptance checklist (spec §14)

| Gate | Result |
|---|---|
| Portable test script passes cleanly | PASS — `checks=103 fails=0`, `BUILD_EXIT=0` |
| ACSIL static gate 12/12 (repo-local `gate_check.py`) | PASS — `12/12`, `GATE_EXIT=0` |
| Exactly one `SCSFExport`; SG index ≤ 58; every input defaulted and read | PASS — 1 export; max SG 17; 15/15 inputs defaulted+read |
| Header audit vs Zander headers, no unsupported API/enum | PASS — all calls verified above, zero mismatches |
| Independent review of timing, symmetry, censoring, ambiguity, overlap, CSV, SG mapping | PASS — this document; no blockers |
| `PROJECT_HISTORY.md` update | Intentionally NOT done — review instructions forbid editing history |
| `EffortVsResult.cpp` unmodified | PASS — untouched (read-only reference) |

## Verdict restated

**PASS. No blocking defects found; no further work is required by this
review.** The two LOW observations (cumulative-SG gap at the last closed
bar; write-only slot-1 comment) are cosmetic/documentation and may be
addressed or explicitly deferred at orchestrator discretion. Sierra F5
compile and chart replay validation remain with the orchestrator, as for
all studies in this repo.

## Resolved findings note (2026-09-04, LOW cleanups applied)

- LOW-1 resolved: after the event loop, cumulative SG15/16/17 values are
  carried onto `lastClosed` before the forming-bar carry, closing the
  zero gap for consumers reading cumulative state at the newest closed bar.
- LOW-2 resolved: unused persistent slot `EVRE_INT_BARS` (slot 1),
  its writes, and the inaccurate header comment were removed; header
  now documents only fingerprint ints 4-6. Full-recalc-only behavior kept.
- Verification (fresh, repo-local only): portable suite `checks=103
  fails=0`, `g++ -std=c++17 -Wall -Wextra -O2` warning-free; gate 12/12
  on `EffortVsResultEvaluator.cpp`. `EffortVsResult.cpp` untouched.
  No commit made.
