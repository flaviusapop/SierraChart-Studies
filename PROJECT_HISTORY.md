# SierraChart Studies — Project History & Working Record

*Reconstructed 2026-05-20 from the source files in this folder, after a Claude reinstall cleared the chat history. The code survived; this note rebuilds the context around it so work can continue in a fresh chat.*

---

## What this project is

A suite of **Sierra Chart ACSIL (C++) custom studies** built around an ES futures scalping workflow. The common thread is **order-flow confluence gated by trend state ("OTF") filters**, with a separate breadth oscillator providing a market-wide bias backdrop. Most studies are designed to overlay on a fast trading chart (range/renko) while reading data from companion charts at other timeframes.

The pieces fall into three groups:

1. **Order-flow signal engines** — score many order-flow triggers, fire arrows + confluence backgrounds.
2. **OTF / trend-state filters** — produce a +1 / 0 / −1 trend state that can gate the signal engines.
3. **Breadth bias filter** — a NYSE-breadth composite oscillator (separate sub-project with its own spec).

---

## Timeline (from file modification dates)

| Date | File | What happened |
|---|---|---|
| May 16, 18:30 | `BreadthCompositeOscillator.cpp` | Breadth oscillator implementation |
| May 16, 18:32 | `BREADTH_OSCILLATOR_PROJECT.md` | Spec doc for the breadth oscillator (now at v1.3) |
| May 16, 19:06 | `OrderflowConfluence.cpp` | First order-flow engine: 25 bull + 25 bear triggers in one study |
| May 18, 09:19 | `OTFStateFilter.cpp` | Trend state machine (strict Higher-Lows / Lower-Highs) |
| May 19, 08:01 | `OrderflowSignal.cpp` | Redesigned single-direction engine ("v4"), 50 weighted triggers, `AutoLoop = 1` |
| May 19, 12:43 | `OrderflowSignalV2.cpp` | Performance edition of the above: `AutoLoop = 0` + sub-panel mode |
| May 20, 10:17 | `MTFCloseFilter.cpp` | **Newest.** Multi-timeframe close-breakout status bar (Sierra Chart) |
| May 20, 10:36 | `MTFCloseFilter.pine` | TradingView Pine v5 port of the MTF Close Filter |

The two most recently touched threads — and the ones earmarked to continue in a new chat — are **MTF Close Filter** and **Orderflow Signal**.

---

## How the pieces fit together

```
                 ┌─────────────────────────┐
                 │  Order-flow trigger      │   (Sierra Chart's own
                 │  studies on the chart    │    order-flow / volume /
                 │  (IDs referenced by      │    delta studies)
                 │   index in the engines)  │
                 └───────────┬─────────────┘
                             │ subgraph values (non-zero = "fired")
                             ▼
        ┌────────────────────────────────────────┐
        │  ORDER-FLOW ENGINE                       │
        │  OrderflowConfluence  →  OrderflowSignal │
        │                       →  OrderflowSignalV2│
        │  scores triggers, fires arrows +         │
        │  confluence background                   │
        └───────────┬──────────────────────────────┘
                    │  optional OTF gate (AND)
                    ▼
        ┌────────────────────────────────────────┐
        │  OTF / TREND-STATE FILTER                │
        │  OTFStateFilter   (HL/LH state machine)  │
        │  MTFCloseFilter   (close-breakout state) │
        │  → expose +1 / 0 / -1 subgraphs the      │
        │    engine reads via "OTF Slot" inputs    │
        └──────────────────────────────────────────┘

  BreadthCompositeOscillator  —  independent market-wide bias backdrop
```

The order-flow engines have **"OTF Filter" inputs** (Arrow OTF Slot 1/2 and Confluence OTF Slot 1/2). Those slots point at a trend-state subgraph — produced by `OTFStateFilter` or `MTFCloseFilter` — so signals only fire when trend state agrees. That is the integration glue between groups 1 and 2.

---

## File-by-file state

### Order-flow engines

**`OrderflowConfluence.cpp`** (May 16 — earliest engine)
- 25 bull + 25 bear triggers in a single study, weighted points (0–3 each).
- Two scoring modes: **Shared** (bull adds, bear subtracts → one combined score) and **Independent** (separate long/short pools, both can fire same bar).
- Fires immediate arrows (per-bar score ≥ threshold) and a lookback confluence background (rolling N-bar sum ≥ threshold).
- 106 inputs. `AutoLoop = 1`, overlays on price (region 0).
- Bull/bear trigger names are pre-labeled with order-flow concepts (Absorption, Stacked Imbalance, Delta Divergence, Sweep, Volume Climax, POC/VWAP reclaim, Trapped, Iceberg…).
- *Superseded in practice by the OrderflowSignal redesign, but kept for the bull-vs-bear-in-one-study model.*

**`OrderflowSignal.cpp`** (May 19 — labeled "v4")
- Redesign to a **single-direction** engine: 50 weighted triggers, 3 intensity levels (Level 1/2/3 arrows by score), lookback confluence background. **Add the study twice** for longs and shorts.
- 115 inputs (`[0..99]` = 50 trigger ref+weight pairs, `[100..114]` = thresholds, offsets, alerts, OTF filter slots).
- Ships with a **default trigger map already wired** to specific study IDs/subgraphs (e.g. `{60,0,1}`, `{84,0,1}`, `{59,4,1}`…). 33 of 50 slots are populated by default; the rest are off. *These IDs are specific to the chart they were authored on — they must be re-pointed on any other chart.*
- `AutoLoop = 1`. Live-bar logic checks both `idx` and `idx-1` to catch triggers that finalize bar N when bar N+1 opens.
- Optional OTF filter (AND of up to two slots) for arrows and confluence independently.

**`OrderflowSignalV2.cpp`** (May 19 — same day, performance edition)
- **Feature-identical to OrderflowSignal but `AutoLoop = 0`.** The motivation (documented in the header): with `AutoLoop = 1`, `GetStudyArrayFromChartUsingID` runs up to 50× per bar per tick. V2 fetches all 50 trigger arrays **once per update cycle**, then the manual bar loop indexes into those local references — cost drops to ~50 API calls per cycle regardless of bar count.
- Rescore window = `lookback * 2` back from `UpdateStartIndex`, so HTF triggers further back are still captured live.
- Startup race guard: if trigger arrays are empty on first run, it returns early and relies on `UpdateAlways` to retry; a manual Recalculate may still be needed once after Sierra Chart starts.
- **Adds one input V1 lacks: `Input[115]` Sub-panel Mode** — draws arrows at Y = 1/2/3 in a sub-panel instead of at price ± offset. (Note: the header comment still says "115 total" but V2 actually has 116 inputs.)
- *This is the version to run for performance; V1 is the simpler reference implementation.*

### OTF / trend-state filters

**`OTFStateFilter.cpp`** (May 18)
- Trend **state machine** from price structure, not order flow.
  - **Up entry:** two strict Higher Lows (`L[2] < L[1] < L[0]`). **Up exit:** current Low breaks below `LSHL` (running max of lows since entry — a ratcheting trailing stop on structure).
  - **Down entry:** two strict Lower Highs. **Down exit:** current High breaks above `LSLH` (running min of highs since entry).
  - State always passes through Neutral when flipping; same-bar opposite re-entry is possible.
- Output: SG0 State (+1/0/−1, stair-step), SG1 Up Context (green background), SG2 Down Context (red background).
- **Cross-chart capable:** `Source Chart = 0` runs native; set to chart N (e.g. a renko chart) to pull High/Low from there while living on the trading chart. Native mode runs an incremental per-bar machine; cross-chart mode re-runs the full state machine over the source history each call.
- This is one of the studies meant to feed the order-flow engine's OTF Slot inputs.

**`MTFCloseFilter.cpp`** (May 20 — newest)
- A **different** kind of OTF filter: multi-timeframe **close-breakout** state across six charts (1m · 2m · 5m · 30m · 60m · 120m).
- Per bar, per timeframe: find highest/lowest **close** over the previous N bars (current bar excluded, wicks never used).
  - **Mode 0 Breakout / Mode 1 Continuation:** UP if close > prev-N high close, DOWN if < prev-N low close, else Neutral (modes 0 and 1 currently produce identical zones).
  - **Mode 2 Stateful (default):** breakout flips state; inside the range carries prior state forward (no neutral once established). Capped at `kMaxScanBars = 10000` for performance.
- Two N settings: LTF N (default 3) for 1/2/5m, HTF N (default 5) for 30/60/120m.
- **Visual:** a six-cell colored status bar drawn at the bottom of the price chart via `sc.UseTool` (green/red/gray + dark background), using reserved drawing line numbers 11000–11015. Also publishes the six states to hidden subgraphs (DRAWSTYLE_IGNORE) for alerts/cross-study use.
- `AutoLoop = 0`, overlay (region 0), `UpdateAlways = 1`.
- **Setup dependency:** requires six chart windows open at those timeframes; the user enters their chart numbers in inputs `[3..8]` (defaults 2–7). One instance per chart only (drawing slots are shared).

**`MTFCloseFilter.pine`** (May 20 — TradingView port)
- Pine Script v5 port of the same logic, using `request.security()` per timeframe and `ta.highest(close,N)[1]` / `ta.lowest(close,N)[1]` for the prev-N-close window.
- Renders a 6-cell table (configurable position/size) instead of drawn rectangles, plus an optional transparent background tint when **all six** timeframes agree (all UP → green, all DOWN → red, else no tint).
- Uses `lookahead_off` (non-repainting; shows last confirmed TF close on the live bar) — commented as switchable to `lookahead_on`.

### Breadth bias filter

**`BreadthCompositeOscillator.cpp`** + **`BREADTH_OSCILLATOR_PROJECT.md`** (May 16, spec at v1.3)
- Composite breadth oscillator from two NYSE breadth indicators, **$VOLD** and **$ADD**, each normalized via a blend of absolute level and rate-of-change, run through dual Z-scores (session-adaptive Short Z + multi-session Long Z), blended into one oscillator line with per-indicator component lines for divergence reading.
- Primary use: **ES scalping bias filter** on a 10-point range-bar chart (study itself runs on a 1-min chart).
- **v1.3 fix** (per the spec): pure ROC had a sign/direction bug — when an indicator was negative but its ROC was positive, the composite wrongly turned bullish. Fixed by blending absolute-level Z with ROC Z per indicator via the `LevelROC_Blend` input. See the spec doc for the full architecture, inputs, signal rules (R1–R9), and interpretation tables.
- Independent of the order-flow / OTF stack — it's the macro bias backdrop.

---

## Where things were likely left off (pick-up points)

- **MTF Close Filter** is the freshest work (both `.cpp` and `.pine` written today, May 20). Likely **not yet validated on live charts** — natural next steps: compile the C++ in Sierra Chart, confirm the six chart-number inputs map correctly, and sanity-check that Mode 2 state carries as intended; on the Pine side, confirm the `request.security` states and the all-agree tint behave on a real symbol. Mode 0 vs Mode 1 are currently identical — an open design question is whether Mode 1 ("Continuation, lenient") should diverge from Mode 0.
- **Orderflow Signal**: the open thread is V1 vs **V2**. V2 is the performance build (`AutoLoop = 0`) and adds Sub-panel Mode. If continuing, V2 is the one to evolve. Watch items: the default trigger map (study IDs `60/84/82/59/...`) is **chart-specific** and must be re-pointed elsewhere; the V2 header comment's input count ("115") is stale (it's 116 with the sub-panel input).
- **OTF integration**: both `OTFStateFilter` and `MTFCloseFilter` expose a state subgraph that the order-flow engines can consume via their OTF Slot inputs — wiring/validating that handshake is a likely next task.

---

## Practical notes carried in the code (so they aren't rediscovered the hard way)

- External/companion-chart data is read with `sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, ...)` / `sc.GetChartBaseData(chartNum, ...)` — always using the current chart number to avoid the `GetChartNumber()==0` mismatch on same-chart inputs.
- Persistent per-bar state is stored in subgraph aux arrays (`sg.Arrays[0..n]`) rather than `sc.PersistVars`, which does not exist in this Sierra Chart version.
- Studies use `AutoLoop = 0` specifically when cross-chart array fetches would otherwise be called per-bar-per-tick (MTFCloseFilter, OrderflowSignalV2). `AutoLoop = 1` is fine for the simpler engines.
- Sierra Chart base price study (ID = 1): SG index 1 = High, SG index 2 = Low; `SC_LAST` = close.

---

*This note is a snapshot of file state as of 2026-05-20. Treat the code as authoritative — if anything here disagrees with the source, the source wins.*
