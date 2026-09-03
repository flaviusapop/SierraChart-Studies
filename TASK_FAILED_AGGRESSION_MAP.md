# Build task: standalone Failed Aggression Map

Create a NEW Sierra Chart ACSIL study. Do not modify `TrappedTraders.cpp` or `LiquidityZones.cpp`.

Deliverables in this worktree:
- `FailedAggressionMap_BuildSpec.md`
- `FailedAggressionMap.cpp`
- update `README.md` and `PROJECT_HISTORY.md`
- commit all work to this branch

Read first:
- `CLAUDE.md`
- `TrappedTraders.cpp` for VAP access and drawing precedents only
- `/home/ubuntu/ai-workspace/Trading/Futures_Day_Trading/TrappedTraders_v2_Guidelines.md`
- `/home/ubuntu/ai-workspace/Trading/Futures_Day_Trading/research/Trapped_Traders_Visualization_Research.md`
- `/home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/references/adaptive-vap-thresholds.md`
- `/home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/references/trapped-participant-visualizations.md`
- mockup: `/home/ubuntu/.hermes/artifacts/trapped_traders_visual_mockup.png`

Identity:
- file `FailedAggressionMap.cpp`
- `SCDLLName("FailedAggressionMap")` near the top
- function `scsf_FailedAggressionMap`
- GraphName `Failed Aggression Map v1.0`
- `AutoLoop=0`, region 0, `sc.MaintainVolumeAtPriceData=1`

Purpose: detect directional aggression at bar/window extremes, confirm that price did not reward it and displaced against it, then visualize the inferred failed inventory as a volume-scaled bubble at its delta-weighted centroid plus a thin horizontal ribbon. This is a separate study, not Trapped Traders v2.

Evidence states must remain separate:
`ATTEMPT -> FAILURE_CONFIRMED -> FIRST_RETEST -> RETEST_FAILED or ACCEPTED_THROUGH`.
A raw footprint cluster is not rejection; a touch is not a failed retest.

Core detection:
- Work only from this chart's CLOSED-bar VAP data.
- Aggregate Ask-Bid delta by integer price tick over configurable rolling `Detection Lookback Bars` (default 2).
- High side: positive directional delta within `Proximity Ticks` (default 3) of rolling high, at least `Minimum Blocks` (default 3), manual defaults 95 per block / 350 cluster total. Candidate = aggressive buyers.
- Low side mirror with negative delta. Candidate = aggressive sellers.
- Store all geometry as integer ticks. Compute delta-weighted centroid tick from qualifying levels.
- Failure confirmation: within `Confirmation Bars` default 3, high-side candidate must close `Adverse Displacement Ticks` default 4 below cluster bottom; low-side mirror above cluster top. If not confirmed by deadline, discard.
- First retest starts only after confirmation when price approaches from the displaced side and overlaps cluster range. Retest fails only when price exits again in the displacement direction by `Retest Exit Ticks` default 2 without a closed-bar acceptance-through.
- Acceptance-through: closed bar beyond far cluster edge plus `Acceptance Ticks` default 1. This invalidates and terminates the ribbon.
- No future data or forming-bar state transitions. Historical full recalculation must reproduce live closed-bar decisions.

Threshold modes:
- `Manual`, `Auto`, `Both`, default Manual.
- Auto uses the same chart's five previous COMPLETE Sierra trading days, excluding current day, with no lookahead. Calculate once per trading day and cache.
- Per prior day: derive percentile of directionally valid near-extreme per-level absolute delta and percentile of per-window side totals; final thresholds are median of five daily percentile values. Inputs: days=5, block percentile=85, zone percentile=80.
- Insufficient five-day history: auto emits no events and shows a non-spamming visible warmup warning. Both continues Manual.
- Both runs independently. Manual has precedence: suppress any Auto candidate whose inclusive tick range intersects an active Manual event, regardless of side. Same-mode overlap merges only within same side/source.
- Manual/Auto source must be stored and exported. Auto colors distinct.

Optional mapped-location confluence:
- Input `Mapped Location Source` using `SetChartStudySubgraphValues`, disabled by default.
- Treat nonzero source value on detection bar as confluence. It must NOT suppress raw detections in v1.0; it adds a gold halo and SG flag only, preserving research comparison.

Rendering modes:
- `Bubble + Ribbon` default
- `Lifecycle Glyphs`
- `Legacy Rectangle`
- `Bubble + Ribbon + Rectangle`

Bubble/ribbon:
- Bubble at event bar and centroid price. Size comes from normalized cluster delta, bounded by min/max marker size inputs.
- Use standard subgraph marker styles where practical; use `sc.UseTool` for persistent multi-object lifecycle. Avoid custom graphics callbacks unless required.
- Failed buyers magenta/red; failed sellers cyan/blue. Gold outline/halo for mapped confluence.
- Ribbon is thin and centered on inferred centroid, extends until accepted-through. Label it as inferred inventory—not exact trader break-even.
- First retest gets gold ring/state color; failed retest prints directional arrow; accepted-through terminates/fades and marks X.
- Aggregate or cap visible objects to avoid unreadable/performance-heavy charts.

Append explicit SG outputs (under 60 total): candidate side/pulse, failure-confirmed side/pulse, centroid price, cluster top, cluster bottom, normalized strength, first-retest pulse, failed-retest entry pulse, accepted-through pulse, mapped-confluence flag, threshold source, stable event ID. Each event/state must be downstream-readable and stale values must be cleared.

Engineering requirements:
- Append inputs/SGs only within this new study; document every index.
- Persistent state must be replay-safe, cleaned on `LastCallToFunction`, and avoid unbounded growth. Prefer fixed-capacity arrays over STL containers in persistent state.
- Process every missed closed bar after reconnect/backfill.
- Use correct drawing namespace/deletion API consistently.
- Settings fingerprint must rebuild immediately.
- Avoid float equality on prices and overflow-prone delta math.
- No alerts from historical bars. If alerts are included, use a live watermark.

Verification available on Linux:
- run `python3 /home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/scripts/gate_check.py FailedAggressionMap.cpp`
- run syntax/static checks possible without Sierra headers
- self-review every input is read, every struct field exists, SG count <60, no Python-style # comments, no forming-bar decisions, no stale SGs

Do not claim Sierra compilation because this environment has no Sierra build system. In the final response, report files, commit SHA, gate output, and exact remaining Sierra compile/replay checklist. Then stop.