# Effort vs Result Evaluator — Build Spec v1.0

Standalone companion ACSIL study for objective historical evaluation of
`EffortVsResult.cpp`. It is an event-study evaluator, not a trading system and
never sends orders. It reads the existing source study so signal logic has one
canonical implementation.

## 1. Identity and boundaries

- File: `EffortVsResultEvaluator.cpp`
- Function: `scsf_EffortVsResultEvaluator`
- DLL name: `EffortVsResultEvaluator`
- Graph name: `Effort vs Result Evaluator v1.0`
- `AutoLoop = 0`; default lower region; all output SGs hidden by default.
- Same-chart source only. One evaluator instance belongs on every bar-type chart.
- Source: one Study/Subgraph input supplies the Effort vs Result Study ID; the
  evaluator reads the canonical internal SG indices from that study:
  - 2 Normalized Effort
  - 5 Raw Signed Failure
  - 6 Smoothed Signed Failure
  - 7 Buyer Failure Pulse
  - 8 Seller Failure Pulse
  - 9 Buyer Failure Arrow
  - 10 Seller Failure Arrow
- Do not copy or reimplement the source study's effort/reward/failure formulas.
- No order submission, alerts, chart drawings, networking, or external process
  execution.

## 2. Research contract

The evaluator scans already-loaded historical bars. Replay is not required for
initial ranking. For every eligible confirmation event it freezes the event,
entry, stop, target, and time horizon, then inspects later bars only to measure
that event's outcome.

This is intentionally an event study: every eligible event is evaluated
independently by default. Overlapping observations are identified because they
are correlated; the user may instead exclude a signal while a prior event is
active.

Signals are generated only from source arrays at confirmation bar `j`:

- Buyer-failure event = short (`side = -1`).
- Seller-failure event = long (`side = +1`).
- Candidate bar is exactly `j-1`.
- If both buyer and seller channels are nonzero on the same bar, mark a conflict
  and do not evaluate it.
- The current forming bar is never a signal or outcome source.

## 3. Event-source modes

Input `Event Source`:

0. **Strict Failure Pulses** (default): buyer SG7 / seller SG8.
1. **Price Confirmation Arrows**: buyer SG9 / seller SG10.
2. **Both, Deduplicated**: an event exists when either corresponding channel is
   nonzero; one same-bar/same-side pulse+arrow is one event, with pulse preferred
   for signal strength.

Arrow mode depends on `Show Price Arrows` being enabled in the source study.
An empty arrow source is not an error. CSV records `pulse`, `arrow`, or `both`.

## 4. Eligibility filters

- Source Study ID must be configured and all required arrays must be available.
  Log one clear Sierra message per invalid configuration, not one per bar.
- Optional date bounds are integer `YYYYMMDD`; 0 means unbounded. Reject an
  invalid nonzero date value with one message and no evaluation.
- Optional session filter uses chart-time-zone start/end inputs and supports
  windows crossing midnight. The confirmation bar time determines eligibility.
- Require `j >= 1`.
- Default next-open entry requires `j+1` to be closed and available.
- Require positive tick size and positive risk distance.
- Exclude events without enough future chart data to reach the full configured
  holding horizon. Do not misclassify right-edge censoring as a timeout.

## 5. Entry, stop, target, and horizon

### Entry mode

0. **Next Bar Open** (default): entry = Open[j+1], outcome traversal begins at
   `j+1`.
1. **Confirmation Close**: entry = Close[j], but outcome traversal still begins
   at `j+1`; the confirmation bar's earlier high/low must never be used after a
   close entry.

### Stop

- Buyer failure / short: `High[j-1] + StopBufferTicks * TickSize`.
- Seller failure / long: `Low[j-1] - StopBufferTicks * TickSize`.
- Default buffer: 2 ticks; limits 0..100.
- If the entry is already at or beyond the stop, mark `invalid_risk` and exclude
  from performance aggregates.

### Target

- Risk = absolute entry-to-stop distance.
- Long target = `entry + TargetR * risk`.
- Short target = `entry - TargetR * risk`.
- Default Target R = 1.0; limits 0.1..20.0.

### Holding horizon

- Measured in elapsed chart time, never a fixed number of bars.
- Default maximum hold = 15 minutes; limits 1..1440.
- Horizon begins at the outcome traversal bar's start time.
- Scan closed bars whose start timestamps are within the horizon.
- If no stop/target occurs, exit at the close of the final eligible bar and
  calculate directional timeout R.

## 6. Intrabar first-touch ambiguity

For each scanned bar:

- Long target touch: `High >= target`; long stop touch: `Low <= stop`.
- Short target touch: `Low <= target`; short stop touch: `High >= stop`.

When both occur in one bar, OHLC cannot prove sequence. Input `Ambiguous Bar
Policy`:

0. **Mark and Exclude** (default): outcome status `ambiguous`, excluded from
   performance aggregates.
1. **Stop First**: outcome = -1R.
2. **Target First**: outcome = +TargetR.

MFE and MAE remain reported for ambiguous events.

## 7. Excursion calculations

From the first outcome bar through the exit/horizon bar:

- Long favorable excursion = max(0, High-entry); adverse = min(0, Low-entry).
- Short favorable excursion = max(0, entry-Low); adverse = min(0, entry-High).
- `MFE_R = favorable / risk` (nonnegative).
- `MAE_R = adverse magnitude represented as a nonpositive R value`.
- Outcome R:
  - target: `+TargetR`
  - stop: `-1`
  - timeout: directional `(exit-entry)/risk`
  - ambiguous/invalid/insufficient/conflict/excluded-overlap: 0 plus distinct
    status code, excluded from performance aggregates.

## 8. Overlap policy

Input `Overlapping Event Policy`:

0. **Include and Flag** (default): evaluate every event; flag an event when its
   entry begins no later than the prior included event's exit bar.
1. **Exclude While Prior Active**: status `excluded_overlap`; do not evaluate or
   aggregate the later event.

Overlap state is chronological and applies across both directions. CSV makes
this explicit so external analysis can cluster or filter observations.

## 9. Outputs (all `DRAWSTYLE_IGNORE` by default)

0. Event Side (`-1` buyer failure/short, `+1` seller failure/long)
1. Evaluated Flag (`1` included in aggregates)
2. Outcome R
3. MFE R
4. MAE R
5. Entry Price
6. Stop Price
7. Target Price
8. Holding Seconds
9. Status Code
10. Signal Strength (pulse absolute value preferred; otherwise absolute arrow
    channel value is not used as strength and output is 0)
11. Candidate Normalized Effort
12. Candidate Raw Signed Failure
13. Candidate Smoothed Signed Failure
14. Overlap Flag
15. Cumulative Included Events
16. Cumulative Average R
17. Cumulative Win Rate Percent

Status codes are stable and documented in the source banner:

- `1` target
- `-1` stop
- `2` timeout
- `3` ambiguous
- `-2` invalid risk
- `-3` excluded overlap
- `-4` insufficient future data
- `-5` conflicting sides

Every SG value and DataColor used by the study is cleared deterministically on
full recalculation and settings changes. No stale historical results.

## 10. Inputs

0. Effort vs Result Source (Study/Subgraph; Study ID used, default SG7)
1. Event Source (Pulse; Arrow; Both Deduplicated), default Pulse
2. Entry Mode (Next Bar Open; Confirmation Close), default Next Bar Open
3. Stop Buffer Ticks, int default 2, 0..100
4. Target R Multiple, float default 1.0, 0.1..20
5. Maximum Hold Minutes, int default 15, 1..1440
6. Ambiguous Bar Policy (Mark/Exclude; Stop First; Target First), default Mark
7. Overlapping Event Policy (Include+Flag; Exclude), default Include+Flag
8. Enable Session Filter, yes/no default No
9. Session Start, time default 09:30:00
10. Session End, time default 16:00:00
11. Evaluation Start Date YYYYMMDD (0=all), int default 0
12. Evaluation End Date YYYYMMDD (0=all), int default 0
13. Export CSV on Full Recalculation, yes/no default No
14. CSV File Prefix, string default `EVR_Evaluator`

All inputs are structural except CSV enable/prefix. A structural settings
fingerprint forces a deterministic full rebuild.

## 11. CSV contract

When export is enabled during full recalculation/settings rebuild:

- Overwrite one deterministic file; never append duplicate historical rows.
- Path is always under `sc.DataFilesFolder()`.
- Sanitize symbol and prefix to `[A-Za-z0-9_.-]`; replace all other characters
  with `_`; reject empty prefix by using `EVR_Evaluator`.
- Filename:
  `<prefix>_<symbol>_Chart<chart>_Study<instance>.csv`.
- Write through a temporary file in the same folder and replace the destination
  only after a complete successful write when practical with standard C file
  APIs. On failure, preserve any prior completed CSV and log one useful error.
- RFC-like CSV: fixed header, dot decimal, no locale-dependent formatted dates.
- Include all events inside the configured date/session evaluation population,
  including overlap-excluded, invalid, ambiguous, conflicting, and right-edge
  censored events, with status. Events outside the configured date/session
  population are intentionally omitted rather than treated as evaluated events.

Columns:

`schema_version,symbol,chart_number,study_instance,bar_period_type,bar_param1,bar_param2,bar_param3,bar_param4,confirmation_datetime,entry_datetime,event_source,side,signal_strength,candidate_effort,candidate_raw_failure,candidate_smoothed_failure,entry_price,stop_price,target_price,risk_ticks,status_code,outcome_r,mfe_r,mae_r,holding_seconds,overlap_flag`

Use `sc.GetBarPeriodParameters` and record the numeric intraday bar type plus its
four parameters. External tooling can map numeric types to names without making
this study depend on enum-string helpers.

## 12. Runtime behavior and performance

- Historical evaluation runs only on full recalculation or structural settings
  rebuild in v1.0. Ordinary intrabar and new-bar calls return without rescanning
  history. Run **Recalculate All Studies** to incorporate newly loaded bars.
  CSV is therefore never rewritten on an ordinary chart update.
- Use `AutoLoop=0` and fetch each source array once per call.
- Complexity may be `O(events * bars_within_horizon)`; do not scan the full
  future chart after a stop/target/horizon has ended.
- No C++ containers in persistent state. Fixed POD state only; clean up any
  allocated pointer on `sc.LastCallToFunction`.
- No static/thread-local mutable compute state.
- No float equality for price comparisons.
- Historical results must be deterministic across repeated full recalculations.

## 13. Portable test mode and TDD

The implementation must be one uploadable/self-contained `.cpp`. It may expose
pure evaluator helpers behind `#ifdef EVR_EVALUATOR_TEST` so Linux tests can
include the production file without Sierra headers or the ACSIL study body.

Create:

- `tests/test_effort_vs_result_evaluator.cpp`
- `tests/run_effort_vs_result_evaluator_tests.sh`

Muse must follow vertical RED→GREEN cycles and retain terminal evidence that the
first test failed because behavior was absent. Portable tests must cover:

1. Long target, stop, timeout, and ambiguous outcomes.
2. Mirrored short outcomes.
3. MFE/MAE sign and R normalization.
4. Confirmation-close traversal begins on the following bar.
5. Right-edge insufficient-data exclusion.
6. Pulse/arrow/both event selection and deduplication.
7. Conflicting same-bar sides.
8. Session filters including crossing midnight.
9. Date bounds.
10. Include+flag and exclude-overlap policies.
11. Safe prefix/symbol sanitization.
12. Determinism across repeated evaluation.

Tests must compile with the available system C++ compiler and use no external
packages.

## 14. Acceptance gates

- Portable test script passes cleanly.
- ACSIL static gate reports 12/12.
- Exactly one `SCSFExport`; SG index <= 58; every input defaulted and read.
- Header audit against `/home/ubuntu/.hermes/cache/sierrachart_zander.h` finds no
  unsupported ACSIL API or enum use.
- Independent fresh Muse review confirms event timing, no future leak in signal
  selection, correct long/short symmetry, right-edge censoring, ambiguity,
  overlap, CSV idempotence/path safety, and source SG mapping.
- Update `PROJECT_HISTORY.md`.
- Do not modify `EffortVsResult.cpp` unless an independently proven integration
  defect requires it; any such change requires separate approval.
