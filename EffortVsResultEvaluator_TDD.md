# EffortVsResultEvaluator — TDD Record v1.0

Date: 2026-09-04. Method: vertical RED→GREEN slices. `EffortVsResult.cpp`
was not modified. Portable tests compile with the system C++ compiler only
(`g++ -std=c++17 -Wall -Wextra -O2`), no external packages.

## RED evidence (slice 0 — behavior absent)

The first test was a minimal long-target probe including
`../EffortVsResultEvaluator.cpp` behind `EVR_EVALUATOR_TEST`, run via
`tests/run_effort_vs_result_evaluator_tests.sh` BEFORE the evaluator file
existed. Exact terminal output:

```
tests/test_effort_vs_result_evaluator.cpp:5:10: fatal error: ../EffortVsResultEvaluator.cpp: No such file or directory
    5 | #include "../EffortVsResultEvaluator.cpp"
      |          ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
compilation terminated.
EXIT=1
```

This is the retained proof that the first test failed because evaluator
behavior was absent (no implementation file at all), not because of a
test-harness defect.

## Slice 1 — portable core + first GREEN

Implemented `EffortVsResultEvaluator.cpp` with the pure core outside the
ACSIL body (`EvrSelectEvent`, `EvrComputeTrade`, `EvrEvaluateEvent`,
`EvrOverlapCheck`, `EvrValidYYYYMMDD`, `EvrDateEligible`,
`EvrSessionEligible`, `EvrSanitize`, `EvrBuildCsvName`) plus the full
`scsf_EffortVsResultEvaluator` study body. The minimal probe then exposed a
fixture defect, not an implementation defect: a 2-bar tape with a 900 s
horizon correctly reports `EVRE_ST_INSUFFICIENT` (status -4) because the
chart cannot cover the full horizon:

```
RED: EvrEvaluateEvent long-target absent/broken rc=1 status=-4
```

Per spec §4/§5 this is right-edge censoring, never a timeout — the helper
was correct and the fixture needed a horizon-covering tape (20 bars at 60 s
spacing). The probe was expanded into the full 12-area suite (slice 2).

## Slice 2 — full 12-area suite (one intermediate fixture fix)

First full-suite run:

```
FAIL tests/test_effort_vs_result_evaluator.cpp:162: long MAE_R nonpositive (got -0.500000 want -0.250000)
FAIL tests/test_effort_vs_result_evaluator.cpp:172: short MAE_R normalized (got -0.500000 want -0.250000)
checks=94 fails=2
EVR EVALUATOR TESTS FAILED
EXIT=1
```

Root cause: test-fixture background bars (`FlatTape` lows at 99.0) dominated
the excursion minimum; the evaluator correctly scans every bar from the
traversal start through the exit bar. Fixture tightened to a 100.1/99.9
background so the carved extremes dominate. No production change.

Final run:

```
checks=94 fails=0
ALL EVR EVALUATOR TESTS PASSED
EXIT=0
```

Build is warning-free (`BUILD_EXIT=0` under `-Wall -Wextra`).

## Coverage map (spec §13 items → test functions)

1. Long target/stop/timeout/ambiguous → `T1_LongOutcomes` (incl. Stop-First
   and Target-First policies on the same ambiguous bar).
2. Mirrored short outcomes → `T2_ShortOutcomes`.
3. MFE/MAE sign and R normalization → `T3_MfeMae`.
4. Confirmation-close traversal begins on the following bar →
   `T4_ConfirmCloseTraversal` (also asserts scanning FROM the confirmation
   bar would stop out, so the test is sensitive to the start index; plus
   `EvrComputeTrade` stop/target/invalid-risk checks).
5. Right-edge insufficient-data exclusion → `T5_RightEdge`.
6. Pulse/arrow/both selection and deduplication → `T6_EventSelection`.
7. Conflicting same-bar sides → `T7_Conflict` (pulse, arrow, and
   cross-channel both-mode conflicts).
8. Session filters including crossing midnight → `T8_Session`.
9. Date bounds (+ leap-day/invalid validation) → `T9_Dates`.
10. Include+flag and exclude-overlap policies (incl. entry==exit boundary) →
    `T10_Overlap`.
11. Safe prefix/symbol sanitization (+ fallback prefix, CSV file name) →
    `T11_Sanitize`.
12. Determinism across repeated evaluation → `T12_Determinism`.

## Static self-checks (for the orchestrator ACSIL gate)

- Exactly one `SCSFExport` (`rg -c 'SCSFExport'` → 1).
- Highest subgraph index 17 (≤ 58); 18 SGs, all `DRAWSTYLE_IGNORE`.
- All 15 inputs defaulted (`Set*`) and read (`Get*`): source StudyID,
  event/entry/ambiguity/overlap indices, buffer/hold ints, target float,
  session yes/no + times, date ints, CSV yes/no + string.
- ACSIL surface used is limited to repo-precedent or `.agent-context/`
  cached-header APIs: `GetStudyArrayFromChartUsingID`,
  `GetTradingDayDate`, `BaseDateTimeIn` (`GetAsDouble`,
  `GetTimeInSeconds`), `DataFilesFolder`, `GetBarPeriodParameters`
  (`n_ACSIL::s_BarPeriod`), `sc.Symbol`, `sc.ChartNumber`,
  `sc.StudyGraphInstanceID`, `GetPersistentInt`/`SetPersistentInt`,
  `AddMessageToLog`, standard C file APIs. No orders, alerts, drawings,
  networking, STL, or static mutable state. No float equality on prices
  (touches use `>=` / `<=`; `==` appears only on ints, chars, and
  pointers). No `HMS_TIME` (unverifiable locally) — session defaults use
  raw second literals with `SetTime(int)`.
- `EffortVsResult.cpp` untouched; source SG map (2/5/6/7/8/9/10) is
  read-only in the evaluator.

## Fix round (2026-09-04) — RED before repair

New failing tests `T13_DirectionalStopValidity` (wrong-side stops must be
invalid) and `T14_SerialDaySeconds` (ACSIL serial-day → seconds conversion
must exist as `86400.0`) were added to
`tests/test_effort_vs_result_evaluator.cpp`. Exact RED output before any
production change (`BUILD_EXIT=0`):

```
FAIL tests/test_effort_vs_result_evaluator.cpp:394: long stop above entry invalid
FAIL tests/test_effort_vs_result_evaluator.cpp:398: short stop below entry invalid
FAIL tests/test_effort_vs_result_evaluator.cpp:425: serial-day to seconds conversion present (86400.0)
checks=101 fails=3
EVR EVALUATOR TESTS FAILED
EXIT=1
```

Repo-local gate RED on the same tree
(`python3 .agent-context/gate_check.py EffortVsResultEvaluator.cpp`):

```
PASS exactly one SCSFExport
PASS braces balanced
FAIL no static/thread_local locals
PASS no stray '#' comments
PASS no float == on price arrays
PASS no sc.Stochastic(
PASS all inputs defaulted
FAIL all inputs read in logic
PASS no duplicate input slots
PASS S-> fields all in struct
PASS SG max <= 58
PASS persist-ptr paired w/ LastCallToFunction
  unread inputs: In_AmbP, In_BufTicks, In_CsvEn, In_CsvPre, In_EndDt, In_Entry, In_EvSrc, In_HoldMin, In_OvlP, In_SessEn, In_SessEn2, In_SessSt, In_Source, In_StartDt, In_TargetR

10/12 checks pass | 1442 lines
GATE_EXIT=1
```

GREEN after the repair is recorded by the runner output reported at the
end of this task (portable suite + repo-local gate).

## Refactor (2026-09-04) — CSV dedup + population-filter correction

Scope: `EffortVsResultEvaluator.cpp` only. `EffortVsResult.cpp` untouched,
no commit, no external paths.

- Main pass: date/session eligibility (`EvrDateEligible` /
  `EvrSessionEligible` → `inPopulation`) now runs before conflict
  handling. Pure no-event bars still write zeros + cumulative carry;
  events or conflicts outside the population write cumulative carry only
  and stay omitted/zero (status 0, no candidate SGs). In-population
  conflicts keep status -5 with candidate SGs. This is the explicit
  behavior change; everything else is deduplication.
- CSV pass: deleted the duplicated trade/outcome engine (second
  `EvrComputeTrade`, forward scan, `EvrOverlapCheck`, horizon/ambiguity
  branches, `csvHasPrior/csvPriorExit` overlap replay). The export is now
  a read-only serialization over the already-computed evaluator SGs
  (`SG_Status/Side/Entry/Stop/Target/Outcome/MFE/MAE/Hold/Overlap` plus
  candidate SGs and `SG_Strength`). Source arrays are re-selected via one
  `EvrSelectEvent` call per row only to recover the `event_source` kind
  string; status -5 emits `conflict`, otherwise pulse/arrow/both from the
  selector. `risk_ticks` derives only from SG stop/entry and `TickSize`
  (`fabs(stop-entry)/tickSize` when `TickSize > 0` and `SG_Stop != 0`,
  else 0, so invalid-risk rows stay 0). Date/session eligibility is
  applied before exporting, including conflicts; `SG_Status == 0` rows
  are skipped. Header, column order, temp + backup/restore replacement,
  and write-error handling (preserve prior completed CSV, one log line
  per failure) are unchanged.

Verification (repo-local only):

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

Static dedup checks on the refactored file: CSV region contains no
`EvrComputeTrade` / `EvrEvaluateEvent` / `EvrOverlapCheck` /
`horizonSec` / `ambP` / `targetR` / `bufTicks` / `entMode`, exactly one
`EvrSelectEvent` (kind-only), canonical `SG_Status/Side/Outcome/MFE/MAE/
Hold/Overlap/Cand` reads, `riskTicks` from `SG_Stop` + `TickSize`,
`"conflict"` kind string, and date/session gating; `SCSFExport` count 1;
`EffortVsResult.cpp` unmodified per `git status`.

## Open (orchestrator-owned)

External ACSIL static gate (12/12), Zander header audit, and Sierra F5
compile + replay validation were deliberately not attempted here.
