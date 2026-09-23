# OrderflowSignal V5 — Configuration Worksheet

Companion to `OrderflowSignalV5.cpp`. Purpose: carry the V3/V4 trigger defaults over
to V5's three-part addressing, one trigger at a time, without losing track of what
was configured before.

---

## 1. How addressing changed

| | V3 / V4 | V5 |
|---|---|---|
| Input type | `SetStudySubgraphValues(study, sg)` | `SetChartStudySubgraphValues(chart, study, sg)` |
| What it points at | a **Study Overlay instance on the main chart** | the **trigger study on its own source chart** |
| Parts | 2 — study ID, subgraph | 3 — chart number, study ID, subgraph |

**The critical point: study IDs are scoped per chart.** The ID you see today is the ID
of the *overlay* on the main chart. The same trigger has a **different ID** on the chart
it actually runs on. `trigger_reference_table.md` states this directly: *"IDs are
chart-specific: same number = different study on a different chart window."*

So the numbers in the "Current (V3/V4)" column below are **not reusable as-is** — they
are the starting inventory, not the answer. Every row needs its source chart's ID
looked up.

**You do not type numeric IDs.** V5's input uses SC's native Chart + Study + Subgraph
picker: choose the chart, and the study dropdown repopulates with *that chart's*
studies by name. That is the usability win the overlays were originally there to
provide, now handled natively.

### Subgraph indexing — off-by-one warning

The API's subgraph index is **0-based**; the SC UI and `trigger_reference_table.md`
label subgraphs **1-based**. So:

```
API sgIdx 0  ==  SG1 in the UI / reference table
API sgIdx 4  ==  SG5
```

The "Current SG idx" column below is the raw API value from the code. The "= UI SG"
column is what you will actually see in the picker.

---

## 2. Chart roster — from `OF-Triggers-For V5.docx`

Chart numbers come straight from the document, so the defaults should resolve without
editing. Verify each against your chartbook (chart number is in the SC window title).

| Chart # | Bar size | vs main (2.0 range) | Long triggers | V5 benefit |
|---|---|---|---|---|
| **#2** | 2.5 range | coarser | 8 | low |
| **#4** | 2.0 range | **same — likely the main chart** | 2 | identity map |
| **#5** | 3.5 range | coarser | 5 | low |
| **#7** | 2.5 range | coarser | 6 | low |
| **#9** | 8t renko | ≈ same (2.0 pt) | 3 | low |
| **#10** | 4.0 range | coarser | 4 | low |
| **#12** | 3.0 range | coarser | 2 | low |
| **#13** | 6.5 range | coarser (~3 bars/bar) | 5 | low — but **the Hold case** |
| | | | **35 total** | |

### Read this before configuring

**Every source chart is the same size or coarser than the main 2.0-range chart.**
Nothing in this set is *faster* than the main chart. That means:

- **The pulse-loss problem barely applies to this trigger set.** Many-to-one collapse
  only happens when a source produces more bars than the destination. Here it doesn't,
  so V5's accumulation fix has little to do.
- **What V5 buys you instead is placement control** — the Open / Close / **Hold**
  choice, which is exactly the higher-timeframe question you raised. Chart #13
  (6.5 range) spans ~3 main bars per source bar, so it is the clearest Hold test.
- If you were expecting a big jump in arrow count from V5 alone, **expect a small one**.
  The gain here is in *where* HTF triggers land and how long they stay live, not in
  recovering lost events.

If a chart *finer* than 2.0 range exists in your chartbook and has triggers on it
(the earlier Renko 6t = 1.5 pt set), adding it here is where accumulation would
actually pay off.

All source charts must be **open, in the same chartbook, on matching session times**,
with days-to-load ≥ the main chart. A closed chart contributes zero and shows as a dip
in SG6 "Debug: Connected".

---

## 3. Migration strategy — chart 0 is your A/B switch

V5 treats **chart number 0 as "this chart"**, which builds an identity time-map and
reproduces the old overlay read exactly.

```
All triggers left at chart 0   ->  V5 behaves like V4 (reads the overlays)
One trigger set to its source  ->  that trigger reads directly, no pulse loss
```

Migrate one at a time. Each row becomes a controlled A/B on the same chart: set the
chart number, watch whether that trigger's contribution to SG5/SG7 changes. Renko 6t
rows should change the most (fastest source = most collapsed events); Renko 8t rows
should barely change (slower than the main chart, so little to collapse).

Order to migrate: **Renko 6t first** — biggest expected delta, so it validates the
whole mechanism fastest.

---

## 4. Trigger slots — LONG set, compiled in as defaults

These are already in `OrderflowSignalV5.cpp`; nothing to fill unless a chart number or
study ID disagrees with your chartbook. **SG column shown 1-based as in the document**
(the code stores 0-based — `SG5` in the doc is `sgIdx 4` in the source).

| Slot | Chart | Ref | Trigger | Pts |
|---|---|---|---|---|
| 1 | #2 | ID1.SG1 | Long Delta Smart Triggers | 1 |
| 2 | #2 | ID7.SG1 | Long Volume Momentum | 1 |
| 3 | #2 | ID7.SG3 | Long Exhaustion Reversal | 1 |
| 4 | #2 | ID10.SG3 | Long Slingshot 3-Bar | 2 |
| 5 | #2 | ID10.SG5 | Long FADE Trigger | 2 |
| 6 | #2 | ID10.SG7 | Long ABS | 1 |
| 7 | #2 | ID6.SG1 | Bullish Delta Trap | 1 |
| 8 | #2 | ID6.SG6 | Bullish Reversal | 1 |
| 9 | #4 | ID49.SG1 | Long DeltaUp+ | 1 |
| 10 | #4 | ID16.SG1 | Long BUY 8 | 1 |
| 11 | #5 | ID30.SG1 | Long ABS | 1 |
| 12 | #5 | ID24.SG5 | Long OF trigger A | 1 |
| 13 | #5 | ID24.SG6 | Long OF trigger B | 1 |
| 14 | #5 | ID33.SG6 | Delta Flip Up | 1 |
| 15 | #5 | ID31.SG6 | Exhaustion Reversal Long | 1 |
| 16 | #7 | ID32.SG1 | Delta Trap Signal Long | 1 |
| 17 | #7 | ID31.SG1 | Long POCS 10 | 2 |
| 18 | #7 | ID49.SG1 | Long DeltaUp+ | 1 |
| 19 | #7 | ID16.SG1 | Long BUY 10 | 2 |
| 20 | #7 | ID64.SG1 | Long POC WAVE | 1 |
| 21 | #7 | ID21.SG1 | Bullish Vol Seq Div | 1 |
| 22 | #9 | ID27.SG1 | Reversal Long | 2 |
| 23 | #9 | ID34.SG1 | Long T Buy Renko | 1 |
| 24 | #9 | ID25.SG1 | Long Trigger Renko | 1 |
| 25 | #10 | ID64.SG1 | Long POC Wave 16 | 1 |
| 26 | #10 | ID69.SG5 | Long OF L1 | 2 |
| 27 | #10 | ID69.SG6 | Long OF L2 | 2 |
| 28 | #10 | ID32.SG1 | Long Trap 16 | 2 |
| 29 | #12 | ID32.SG1 | Long delta trap | 1 |
| 30 | #12 | ID31.SG1 | Long POCS 12 | 2 |
| 31 | #13 | ID9.SG5 | Long OF L1 | 2 |
| 32 | #13 | ID9.SG6 | Long OF L2 | 2 |
| 33 | #13 | ID1.SG5 | Long FADE Trigger | 2 |
| 34 | #13 | ID1.SG3 | Long Slingshot 3-Bar | 2 |
| 35 | #13 | ID1.SG7 | Long ABS | 1 |
| 36–50 | — | — | free | 0 |

**Total weight if every trigger fired at once: 50 points.** Current thresholds are
3 / 5 / 8 — worth revisiting against this set, since it is a different population than
the V3/V4 defaults these numbers were tuned on.

Duplicated triggers across charts are intentional and additive: `Long DeltaUp+`
(#4 + #7), `Long BUY` (#4 + #7), `Long POC WAVE` (#7 + #10), `Long delta trap` /
`Trap 16` (#10 + #12), `Long POCS` (#7 + #12), `Long ABS` (#2 + #5 + #13),
`Long OF L1/L2` (#10 + #13). The same signal agreeing on several timeframes is real
confluence — but it also means one market event can contribute 4–6 points on its own,
so watch SG5 before setting Level 3.

### The short set

The document's short/bearish triggers (32 of them) are **not** loaded. For both
directions, add a second V5 instance with the short set and
`Trigger Position = Above Candle`, `Draw Style = Arrow Down`.

---

## 4b. Original V3/V4 defaults — for reference only

Weights are carried over from the V3/V4 defaults and should not need changing.
`Current study ID` = the main-chart overlay ID, for identification only.

| Slot | Current study ID | Current SG idx | = UI SG | Weight | → Source chart | → Study ID on that chart | → SG idx | Trigger name / notes |
|---|---|---|---|---|---|---|---|---|
| 1 | 60 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 2 | 84 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 3 | 82 | 0 | SG1 | 1 | ______ | ______ | ____ | **duplicate of slot 6 — resolve** |
| 4 | 59 | 4 | SG5 | 1 | ______ | ______ | ____ | |
| 5 | 59 | 5 | SG6 | 1 | ______ | ______ | ____ | same study as slot 4, different SG |
| 6 | 82 | 0 | SG1 | 1 | ______ | ______ | ____ | **duplicate of slot 3 — resolve** |
| 7 | 58 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 8 | 57 | 2 | SG3 | 1 | ______ | ______ | ____ | |
| 9 | 56 | 2 | SG3 | 2 | ______ | ______ | ____ | |
| 10 | 56 | 4 | SG5 | 2 | ______ | ______ | ____ | same study as 9, 11 |
| 11 | 56 | 6 | SG7 | 1 | ______ | ______ | ____ | same study as 9, 10 |
| 12 | 55 | 4 | SG5 | 2 | ______ | ______ | ____ | |
| 13 | 55 | 5 | SG6 | 2 | ______ | ______ | ____ | same study as 12 |
| 14 | 28 | 4 | SG5 | 2 | ______ | ______ | ____ | |
| 15 | 34 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 16 | 70 | 4 | SG5 | 2 | ______ | ______ | ____ | |
| 17 | 70 | 5 | SG6 | 2 | ______ | ______ | ____ | same study as 16 |
| 18 | 68 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 19 | 71 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 20 | 64 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 21 | 64 | 5 | SG6 | 1 | ______ | ______ | ____ | same study as 20 |
| 22 | 53 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 23 | 76 | 0 | SG1 | 2 | ______ | ______ | ____ | |
| 24 | 73 | 0 | SG1 | 2 | ______ | ______ | ____ | |
| 25 | 90 | 0 | SG1 | 2 | ______ | ______ | ____ | |
| 26 | 62 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 27 | 79 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 28 | 67 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 29 | 52 | 0 | SG1 | 2 | ______ | ______ | ____ | |
| 30 | 29 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 31 | 49 | 0 | SG1 | 1 | ______ | ______ | ____ | |
| 32 | 19 | 5 | SG6 | 1 | ______ | ______ | ____ | |
| 33 | 47 | 2 | SG3 | 1 | ______ | ______ | ____ | |
| 34–50 | — | — | — | 0 (disabled) | | | | free slots |

**33 configured, 17 free.** SG6 "Debug: Connected" must read **33** once V5 is fully
wired. Anything lower means a slot failed to resolve — wrong chart, wrong study, or a
source chart that is closed.

---

## 5. Trigger inventory by source chart

From `trigger_reference_table.md`. Use this to identify which trigger each slot is and
therefore which chart it belongs to. ★★ = highest priority, ★ = used, — = not in the
active set.

### Range bar chart
| # | Trigger | Dir | Priority |
|---|---|---|---|
| 3 | Delta Rise | Bull | ★ |
| 4 | Delta Drop | Bear | ★ |
| 9 | VOL SEQ + Delta DIV | Bull | ★ |
| 10 | VOL SEQ + Delta DIV | Bear | ★ |
| 13 | Delta Slingshot Buy | Bull | ★★ |
| 14 | Delta Slingshot Sell | Bear | ★★ |
| 17 | MPOC + NYSE Tick | Bull | ★ |
| 18 | MPOC + NYSE Tick | Bear | ★ |
| 19 | Delta Trap (ΔT) | Bull | ★★ |
| 20 | Delta Trap (ΔT) | Bear | ★★ |
| 21 | Continuous POC Long | Bull | ★★ |
| 22 | Continuous POC Short | Bear | ★★ |
| 39 | POC Wave | Bull | ★★ |
| 40 | POC Wave | Bear | ★★ |
| 1, 2, 5–8, 11, 12, 15, 16, 23–26 | (not in active set) | | — |

### Renko 6t chart — migrate these first
| # | Trigger | Dir | Priority |
|---|---|---|---|
| 27 | OF Long v1 | Bull | ★★ |
| 28 | OF Long v2 | Bull | ★★ |
| 29 | OF Short v1 | Bear | ★★ |
| 30 | OF Short v2 | Bear | ★★ |
| 31 | FA+ | Bull | — |
| 32 | FA− | Bear | — |

### Renko 8t chart
| # | Trigger | Dir | Priority |
|---|---|---|---|
| 33 | Long Trigger | Bull | ★★ |
| 34 | Short Trigger | Bear | ★★ |
| 35 | T BUY | Bull | — |
| 36 | T SELL | Bear | — |
| 37 | R BUY | Bull | — |
| 38 | R SELL | Bear | — |

Note: the reference table's ID columns (ID4, ID13, ID23…) are the studies the **CBBOAC
formulas reference as inputs** — not the IDs of the trigger studies themselves. What
V5 needs is the ID of each **CBBOAC alert-condition study** on its source chart.

---

## 6. Non-trigger settings

Carried from V3/V4 unless noted.

| Input | Value | Note |
|---|---|---|
| [100] Level 1 Threshold | 3 | **expect to recalibrate — see §8** |
| [101] Level 2 Threshold | 5 | **expect to recalibrate** |
| [102] Level 3 Threshold | 8 | **expect to recalibrate** |
| [103] Lookback Window | 5 | bars for the rolling confluence sum |
| [104] Confluence Threshold | 6 | **expect to recalibrate** |
| [105] Signal Offset (ticks) | 2.0 | |
| [106] Trigger Position | Below Candle | match the arrow Draw Style |
| [107] Signal Alert | (sound #) | set one to exercise the new alert dedup |
| [108] Confluence Alert | (sound #) | |
| [109]–[114] OTF filters | as V4 | still read from the **main** chart in V5 |
| [115] Sub-panel Mode | No | |
| [116] Level 1/2/3 Window | 1 | |
| [117] Count Forming Source Bar | **No** | No = deterministic; Yes = faster but can retract |
| [118] Live Bar Latch | **Yes** | forming main bar's arrow score never decreases |
| [119] Source Fire Time | **Close** | Open / Close / **Hold** — see §6b |

Move SG5 / SG6 / SG7 / SG8 to region 2. SG6, SG7 and SG8 are the diagnostics you will
actually watch.

### 6b. Source Fire Time — the Hold test

A source bar slower than the main chart spans several main bars (a 6-point range bar
covers ~3 bars of a 2-point chart). One event, several candidate bars:

| Mode | Where the weight lands | Use |
|---|---|---|
| **Open** | first main bar of the span | anchors to where the move began; arrow lands in the past |
| **Close** | the bar where it became knowable | **default** — arrow lands where you can act, full lookback runs forward |
| **Hold** | every main bar of the span | HTF context stays alive so faster triggers can confluence with it anywhere inside the span |

**Hold uses two channels so confluence is not inflated.** The event counts **once**
(feeds the confluence sum, `Arrays[0]`, plotted as SG5); the held weight spans (feeds
the arrow threshold, `Arrays[1]`, plotted as SG8). Counting a held event per-bar in
the confluence sum is the V2 inflation V3 was written to kill — and raising the
confluence threshold cannot compensate, because the inflation scales with each source
bar's span and therefore varies by chart and by market speed.

Hold also **dedupes the arrow** — one arrow per episode rather than one per held bar,
with an upgrade to a higher tier still allowed to print.

**To see it:** put SG8 (Held Score) over SG5 (Bar Score) in region 2.
- Open / Close → the two lines are **identical**. That is your check that nothing else changed.
- Hold → SG5 keeps spiking on single bars, SG8 forms **plateaus** across each span.

Leave `[116] Level 1/2/3 Window` at **1** while testing Hold. Input 116 is the blunt
version of the same idea (a fixed bar count applied to every trigger); Hold is the
adaptive version (each trigger held for its own source bar's span). They compose, so
changing both at once makes the result unreadable.

---

## 7. Acceptance test

The reason V5 exists. Run it once the triggers are migrated.

1. Fix a date range with a decent sample of arrows.
2. Record the total arrow count on the **8-range** main chart.
3. Switch the main chart to **6-range**. Record again.
4. Switch to **flex renko 6-3-1**. Record again.

**All three counts must be identical.** Under V3/V4 they are not — that discrepancy is
the original bug report. V5 detects rising edges in source time and accumulates
`score[mainBar] += weight`, so every source event is credited to exactly one main bar
regardless of how the main chart slices time. A mapping choice can shift an event by
one bar; it cannot delete or duplicate one.

If the counts still differ, the remaining suspects are: a trigger still left at chart 0,
a source chart with different session times, or a source chart with fewer days loaded
than the main chart.

---

## 8. Expect the arrow count to go UP

Thresholds 3 / 5 / 8 and confluence 6 were tuned against **undercounted** scores — the
overlay was dropping events, so historical scores were systematically low. With those
events restored, scores rise and more arrows clear each threshold.

**This is the fix working, not a regression.** Plan to recalibrate rather than treating
a higher count as a bug. Use SG5 (Bar Score) and SG7 (Debug: Firing) over a few days to
see the new score distribution before changing the thresholds.

Related: `OFSignalAutoTrader` consumes these arrows. More arrows means more trades.
**Retest the trader in Replay before it trades V5's output on the live account.**

---

## 9. Known limits in V5

- Source fire time is mapped by the source bar's **open** timestamp. For the fine
  source timeframes in use, open vs close differs by at most one main bar.
- **Dead-tape confluence**: on a quiet 8-range chart a bar can span hours, so two
  unrelated triggers hours apart can share a bar and read as confluence. Independent of
  V5; a max-bar-duration guard is backlog.
- OTF filter slots still read from the main chart only — not migrated to cross-chart.
- The source→main map is rebuilt per pass. Bounded by the tail window, but on a very
  large chart this is the first thing to optimize (persistent buffer).
