# Build task: standalone Effort vs Result

Create a NEW Sierra Chart ACSIL study. Do not modify `TrappedTraders.cpp`, `LiquidityZones.cpp`, or any other signal study.

Deliverables in this worktree:
- `EffortVsResult_BuildSpec.md`
- `EffortVsResult.cpp`
- update `README.md` and `PROJECT_HISTORY.md`
- commit all work to this branch

Read first:
- `CLAUDE.md`
- `/home/ubuntu/ai-workspace/Trading/Futures_Day_Trading/research/Trapped_Traders_Visualization_Research.md`
- `/home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/references/cbboac-feasibility.md`
- `/home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/references/trapped-participant-visualizations.md`
- mockup: `/home/ubuntu/.hermes/artifacts/trapped_traders_visual_mockup.png`

Identity:
- file `EffortVsResult.cpp`
- `SCDLLName("EffortVsResult")` near the top
- function `scsf_EffortVsResult`
- GraphName `Effort vs Result v1.0`
- `AutoLoop=0`, default separate lower region
- direct VAP source on the SAME chart with `sc.MaintainVolumeAtPriceData=1`; no external Numbers Bars study dependency

Purpose: a lightweight, closed-bar, bar-level measure of whether aggressive order flow was rewarded by price. This is a filter/context study, not a trade entry system and not a per-price trapped-zone detector.

Per closed bar:
- `BarDelta = sum(AskVolume - BidVolume)` across the chart bar's VAP levels using int64-safe math.
- `EffortScale = EWMA(abs(BarDelta), Effort Length default 50)` with a tick/epsilon floor. House convention: normalize directional effort without mean subtraction so sustained aggression does not decay toward zero merely because its mean shifted.
- `Effort = BarDelta / EffortScale`, optionally capped by `Max Normalized Value` default 5.
- `ResultRaw = Close - Open` by default, with selectable `Result Mode`: Close-Open, Close-PreviousClose, and directional excursion/close-location composite. Normalize by EWMA True Range over `Result Length` default 50 with TickSize floor.
- `SignedReward = sign(Effort) * ResultNormalized`: positive means aggressors were rewarded; negative means price moved against them.
- `FailureMagnitude = abs(Effort) * max(0, Stall Allowance - SignedReward)` only when abs(Effort) >= `Minimum Effort` default 1.25. Default Stall Allowance 0.20 so strong aggression with almost no progress can register as failure, not only outright reversal.
- `SignedFailure = -sign(Effort) * FailureMagnitude`: positive/cyan = sellers failed (long evidence); negative/magenta = buyers failed (short evidence).
- Smooth final signed failure with configurable EMA default 3, but expose raw and smoothed values.

Add a separate one-bar-delayed confirmation channel:
- Candidate bar i requires abs(Effort[i]) >= Minimum Effort.
- On closed bar i+1, failed buyers confirm when candidate effort was positive, price fails to extend above candidate high by more than `Max Favorable Extension Ticks` default 2, and closes at least `Minimum Adverse Close Ticks` default 2 below candidate midpoint.
- Failed sellers mirror at candidate low/midpoint.
- Confirmation prints on i+1 only; no future leak. Expose buyer-failure and seller-failure pulses/arrows.

Rendering:
- Lower-panel signed histogram/ribbon around zero. Cyan above zero for sellers failing; magenta below zero for buyers failing. Intensity may use `DataColor` by magnitude.
- Optional rewarded-aggression background/subgraph, off by default, so the display distinguishes failure from directional participation.
- Optional price-region arrows for confirmed failure events using subgraphs at price offsets; no persistent rectangles.
- Labels must say `failed aggression evidence`, not assert known open positions.

Inputs must include: Effort Length, Result Length, Smoothing Length, Minimum Effort, Stall Allowance, Result Mode, Max Normalized Value, confirmation on/off, Max Favorable Extension Ticks, Minimum Adverse Close Ticks, show price arrows, arrow offset ticks, colors, alert enable/sound. Defaults documented. Settings changes rebuild correctly.

Outputs under 60 SGs: raw bar delta, effort scale, normalized effort, normalized result, signed reward, raw signed failure, smoothed signed failure, buyer-failure confirmation pulse, seller-failure confirmation pulse, buyer/seller arrows, ready/warmup flag. Zero stale outputs on recalc.

Engineering requirements:
- Closed bars only; forming bar may display optional provisional value only if explicitly separated and default off. It must never fire confirmed outputs/alerts.
- Historical full recalculation equals sequential live results. Process every missed bar after reconnect/backfill.
- No STL containers in persistent state. No float equality on prices. No Python-style # comments.
- Correct alert watermark: never alert historical bars.
- SGs/inputs documented by index and every input read.

Verification available on Linux:
- run `python3 /home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/scripts/gate_check.py EffortVsResult.cpp`
- independently hand-check formula sign: positive delta + down/no progress -> negative/magenta buyer failure; negative delta + up/no progress -> positive/cyan seller failure; rewarded aggression should not appear as failure
- self-review SG count <60, no lookahead, warmup, settings rebuild, stale outputs, and VAP-null behavior

Do not claim Sierra compilation because this environment has no Sierra build system. In the final response, report files, commit SHA, gate output, and exact remaining Sierra compile/replay checklist. Then stop.