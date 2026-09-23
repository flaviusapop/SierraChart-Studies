// =============================================================================
// OrderflowSignalV4.cpp
// Sierra Chart ACSIL Custom Study  —  v4 (Low-Latency Trigger Scoring)
//
// V4 = V3 with the TIMING layer rebuilt. The data layer (how trigger arrays
// are sourced from other charts) is UNCHANGED in this revision and is marked
// below with "DATA LAYER" for the cross-chart rework that follows.
//
// Run V4 alongside V3 on the same chart, different subgraph colors, to compare
// arrow-for-arrow. V3 is the reference; do not delete it.
//
// =============================================================================
// WHAT CHANGED FROM V3, AND WHY
// =============================================================================
//
// (1) THE BAR-CLOSE GATE IS GONE  — this is the multi-hour latency fix.
//
//     V3 lines 401-405:
//         if (!isFullRecalc && !isNewBar) return;
//
//     V3 fetched all trigger arrays FIRST (paying the full cost), saw the
//     freshly backfilled values, and then discarded them on every intrabar
//     tick. Output was a function of MAIN-CHART BAR FORMATION while the input
//     changed on TICKS. On an 8-range ES chart a new bar needs ~2.00 points of
//     travel; in a quiet tape that is 30+ minutes, overnight it is hours. Arrow
//     latency therefore had no upper bound — it was "time until ES next moves
//     2 points", not any property of the signal.
//
//     This also explains why lowering the Lookback Window never helped:
//     lookback only sizes the rolling confluence sum, it cannot touch the gate.
//
//     V4 never returns early on a tick. It recomputes a bounded TAIL WINDOW of
//     recent bars on every call, and does the full pass from bar 0 only on a
//     full recalculate, a bar count change, or a change in connected triggers.
//
//     Cost: a full pass on a 100k-bar chart is ~24M ops (25-50ms) — not
//     affordable at a 500ms or faster update interval. The tail window is
//     ~264 bars = ~63k ops = ~30-60us per tick, roughly 1/400th. That is what
//     makes per-tick recomputation viable. "Just delete the return" is NOT the
//     fix; the window is the fix.
//
//     Scores for bars below the tail window are read back from Arrays[0],
//     which persists across calls, so the rolling sums stay correct.
//     PersistentInt 5 (everFullPass) guarantees history has been scored at
//     least once before any tail-only pass trusts those persisted values.
//
// (2) LIVE BAR MODE  — removes the remaining one-bar latency floor.
//
//     V3's isLast branch read trigData[i-1] and trigData[i-2] as a proxy for
//     the forming bar. It NEVER read trigData[lastBar]. A trigger that fires on
//     the currently forming bar — exactly what a lower-timeframe trigger does
//     in real time — was structurally invisible until the next bar opened.
//     After fix (1) that is the only remaining unbounded wait.
//
//     The proxy had a second, separate defect: it made one trigger event score
//     on TWO bars in the same pass. A rising edge at lastBar-1 was counted by
//     the normal branch at i = lastBar-1 AND by the proxy branch at i = lastBar.
//     The rolling sum at lastBar therefore double-counted it — a one-bar
//     residue of the exact V2 inflation that V3 was written to eliminate.
//
//     Input[117] selects:
//       0 Proxy  — V3 behavior exactly (for A/B comparison)
//       1 Live   — read the forming bar's own value; no double-count  [DEFAULT]
//       2 Both   — union of the two; earliest possible fire, but re-introduces
//                  the double-count on the live bar
//
//     LATCH: in Live mode a source array that flickers non-zero -> zero
//     intrabar would make the arrow blink. Arrays[2] (unused since V3.2) holds
//     a running max of the forming bar's score so it never decreases within the
//     bar, and is reset when the bar count changes. Input[118] disables it.
//     The trade-off is real and is the user's to make: without the latch a live
//     arrow can evaporate; with it, an arrow persists even if the source
//     retracts. There is no version of "faster" that is also "never repaints".
//
// (3) BAR COUNT COMPARISON USES != , NOT >
//
//     V3 line 402 used (totalBars > lastKnownBars). lastKnownBars is only
//     written after the gate, so if ArraySize ever SHRANK (history trim,
//     session rollover, reload with fewer days) the gate never passed and the
//     chart had to grow back past the old high before updates resumed — the
//     study could stall for N bars. Also, V3 consumed the new-bar event BEFORE
//     the pass: if any source study had not yet published its value for that
//     bar, the event was spent and nothing re-ran until the NEXT bar. With 33
//     triggers across 7 charts and unspecified evaluation order, that was a
//     second, independent one-bar-loss channel. Fix (1) dissolves both.
//
// (4) STARTUP / CONNECTIVITY GUARD ACTUALLY DOES SOMETHING
//
//     The V3 header claimed sc.ResetStudyToCalculateFromBarOne was set when
//     trigger arrays came back empty. It was not — that identifier appears
//     nowhere in V3, and no such member exists in sierrachart.h. V3 line 359
//     was a bare return. So on the startup race the study did nothing and then
//     waited for a new range bar to get its first full pass.
//
//     V4 uses sc.FlagFullRecalculate (sierrachart.h:3020, the real member),
//     bounded by a retry counter so a permanently-closed source chart cannot
//     recalc-loop.
//
//     V3's guard was also all-or-nothing: it required ALL configured triggers
//     to be empty. If 5 of 33 connected, the guard passed and every bar was
//     scored with 5 triggers; when the other 28 arrived, nothing re-scored
//     history. V4 tracks the connected count and forces a full pass whenever it
//     changes, so late-arriving source charts trigger a re-score.
//
// (5) ALERT DEDUP BY MEMBERSHIP, NOT BY WATERMARK
//
//     V3 tracked the newest alerted bar index and scanned only forward of it.
//     Against a producer that rewrites its own history — which this study is —
//     an arrow that materialized AT OR BELOW the watermark was invisible
//     forever, at any ALERT_SCAN_BARS value, because the scan window started
//     ahead of the backfilled bar. The break statement made it worse by pushing
//     the watermark to the newest arrow found, permanently disqualifying every
//     older un-alerted arrow in the same pass.
//
//     The V3 comment justified this as "an arrow that later moves back one bar
//     never re-alerts". That conflates THE SAME ARROW RELOCATING with A
//     DIFFERENT, OLDER ARROW APPEARING. The code cannot distinguish them and
//     discarded both.
//
//     V4 keeps a 32-bar consumed-bitmap anchored to a base bar index. Bits
//     shift out as the chart advances. Any un-consumed arrow in the window
//     alerts, regardless of whether it appeared behind a newer one. On a full
//     recalculate the window is SWALLOWED (all bits set), not cleared —
//     clearing would re-alert everything after any settings edit.
//
//     NOTE: every persistent value here is a BAR INDEX. If SC prepends history
//     (scrolling back, downloading more data) all indices shift and these
//     become meaningless. SC normally issues a full recalc then, which resets
//     them. Keying on sc.BaseDateTimeIn[b] would be structurally robust and is
//     a candidate for the next revision.
//
// =============================================================================
// KNOWN, NOT YET FIXED IN V4  (cross-chart data layer — next revision)
// =============================================================================
//   The DATA LAYER below still calls GetStudyArrayFromChartUsingID with
//   sc.ChartNumber, i.e. the MAIN chart. The trigger studies actually live on
//   6-7 other charts and reach this one through Study Overlay studies.
//
//   Study Overlay POINT-SAMPLES. This is VERIFIED from SC's own documentation
//   for the Study/Price Overlay study, not inferred: its "Data Copy Mode" input
//   offers exactly two options — "Use Latest Value from Corresponding
//   Timeframe" (rightmost source bar) or "Use Earliest Value from Corresponding
//   Timeframe" (leftmost source bar) — and the doc states plainly that when the
//   source timeframe is lower, "multiple source bars may exist within one
//   destination bar's time range. The Data Copy Mode determines selection."
//
//   There is NO max / any-non-zero / OR copy mode. So NO OVERLAY SETTING CAN
//   FIX THIS. Do not spend time tuning Data Copy Mode or Bar Time Matching
//   Method; neither can preserve a pulse train.
//
//   Point-sampling is correct for a continuous series (a moving average, a
//   VWAP) but these triggers are ONE-BAR PULSES, and point-sampling a pulse
//   train drops every pulse the sampler does not land on. Consequences:
//     - lower-TF source: k source bars per main bar -> a pulse survives with
//       probability ~1/k, and multiple events inside one main bar collapse to a
//       single rising edge (scored once, not k times)
//     - higher-TF source: two adjacent firings become 1,1 with no zero gap on a
//       coarse main chart -> one arrow instead of two
//     - therefore 8-range and 6-range disagree in BOTH directions at once,
//       which is why the arrow count changes with the main chart's bar type
//     - HTF values are only knowable at source-bar CLOSE but the rising edge
//       lands on the first destination bar of the span (source-bar OPEN) —
//       a systematic backward displacement equal to the HTF bar's duration,
//       which is the "2-3 bars back" the alert logic compensates for
//
//   The fix is to detect the rising edge in SOURCE time, on the chart where the
//   trigger is actually defined, and get a COUNT to this chart rather than a
//   pulse. The recommended transport is a per-source-chart collector study
//   publishing a MONOTONE CUMULATIVE counter, which V4 differences over a
//   source-bar -> destination-bar time map:
//
//       score[i] = sum over source charts of
//                  ( cum[srcEnd(i)] - cum[srcEnd(i-1)] )
//
//   A cumulative counter is monotone non-decreasing, so the sum telescopes and
//   the event total is CONSERVED for any index mapping. A mapping error can
//   shift an event by one bar; it can never delete or duplicate one. That is
//   what makes the arrow count invariant to the main chart's bar size — the
//   8-range vs 6-range test becomes a hard invariant instead of a symptom.
//   A pulse has no such property, which is exactly why the overlay destroys it.
//
//   This file is structured so that rework replaces exactly one block. See the
//   DATA LAYER markers.
//
//   Input budget: SC_INPUTS_AVAILABLE is 128 (scconstants.h:954) and V4 uses
//   119, leaving 9. A third input per trigger (50 x 3 + 19) does not fit.
//   But note SetStudySubgraphValues and SetChartStudySubgraphValues write the
//   SAME union member at identical offsets (scstructures.h:2884-2907) — only
//   ValueType differs — so widening a trigger ref to chart+study+subgraph costs
//   ZERO extra inputs. Whether SC's chartbook serializer preserves a saved
//   input across a ValueType change is NOT verified (that code is not in
//   ACS_Source); assume the 50 saved refs are lost and screenshot settings
//   first. The collector design needs only ~2 refs per source chart, which
//   sidesteps this entirely.
//
// =============================================================================
// SUBGRAPHS
//   SG1  Level 1 Signal   arrow (low conviction)    — main chart
//   SG2  Level 2 Signal   arrow (medium)            — main chart
//   SG3  Level 3 Signal   arrow (high conviction)   — main chart
//   SG4  Confluence        background               — main chart
//   SG5  Bar Score         histogram                — move to region 2
//   SG6  Debug: Connected  line                     — move to region 2
//   SG7  Debug: Firing     line                     — move to region 2
//
//   All subgraph colors and draw styles are set as defaults only.
//   Changing them in Study Settings persists permanently.
//
//   SG6 "Debug: Connected" is the single best diagnostic in this file: it must
//   read the full configured trigger count within a second or two of chart load
//   and stay there. Any dip means a source study or source chart went dark.
//
// INPUT LAYOUT  (119 total):
//   [0  .. 99]   Trigger 1-50   Input[2n]=Study SG ref   Input[2n+1]=Weight
//   [100]  Level 1 Threshold
//   [101]  Level 2 Threshold
//   [102]  Level 3 Threshold
//   [103]  Lookback Window
//   [104]  Confluence Threshold
//   [105]  Signal Offset (ticks)
//   [106]  Trigger Position
//   [107]  Signal Alert
//   [108]  Confluence Alert
//   [109]  Enable Arrow OTF Filter
//   [110]  Arrow OTF Slot 1
//   [111]  Arrow OTF Slot 2
//   [112]  Enable Confluence OTF Filter
//   [113]  Confluence OTF Slot 1
//   [114]  Confluence OTF Slot 2
//   [115]  Sub-panel Mode
//   [116]  Level 1/2/3 Window
//   [117]  Live Bar Mode                (V4)
//   [118]  Live Bar Latch               (V4)
//
// AUXILIARY ARRAYS (sg_Level1.Arrays[]):
//   [0]  Score per bar          (persists across calls — the tail window
//                                depends on this)
//   [1]  (unused)
//   [2]  Live bar score latch   (V4)
//
// PERSISTENT INTS:
//   1  lastKnownBars       bar count at last pass
//   5  everFullPass        has history been scored at least once
//   6  alertBaseBar        bar index that bit 0 of the alert masks refers to
//   7  sigAlertMask        32-bar consumed bitmap, signal alerts
//   8  confAlertMask       32-bar consumed bitmap, confluence alerts
//   9  lastConnCount       connected trigger count at last pass
//   10 recalcTries         bounded retry counter for the startup guard
//   (2, 3, 4 deliberately left free — they held V3's watermarks)
// =============================================================================

#include "sierrachart.h"

SCDLLName("OrderflowSignalV4")

// Tail window sizing. TAIL_BASE is how far back a trigger array can be
// backfilled and still be picked up on a tick-rate pass. TAIL_MAX clamps the
// cost, since the arrow window input allows values up to 1,000,000.
static const int OFS4_TAIL_BASE = 256;
static const int OFS4_TAIL_MAX  = 4096;
static const int OFS4_TAIL_EDGE = 8;    // margin for the i-1 / i-2 edge reads

// =============================================================================
SCSFExport scsf_OrderflowSignalV4(SCStudyInterfaceRef sc)
{
    // -------------------------------------------------------------------------
    // SUBGRAPHS
    // -------------------------------------------------------------------------
    SCSubgraphRef sg_Level1     = sc.Subgraph[0];
    SCSubgraphRef sg_Level2     = sc.Subgraph[1];
    SCSubgraphRef sg_Level3     = sc.Subgraph[2];
    SCSubgraphRef sg_Confluence = sc.Subgraph[3];
    SCSubgraphRef sg_Score      = sc.Subgraph[4];
    SCSubgraphRef sg_DbgConn    = sc.Subgraph[5];
    SCSubgraphRef sg_DbgFire    = sc.Subgraph[6];

    // -------------------------------------------------------------------------
    // INPUT ALIASES
    // -------------------------------------------------------------------------
    SCInputRef in_Level1Thresh     = sc.Input[100];
    SCInputRef in_Level2Thresh     = sc.Input[101];
    SCInputRef in_Level3Thresh     = sc.Input[102];
    SCInputRef in_LookbackBars     = sc.Input[103];
    SCInputRef in_ConfluenceThresh = sc.Input[104];
    SCInputRef in_OffsetTicks      = sc.Input[105];
    SCInputRef in_TriggerPosition  = sc.Input[106];
    SCInputRef in_AlertSignal      = sc.Input[107];
    SCInputRef in_AlertConfluence  = sc.Input[108];
    SCInputRef in_EnableOTFArrow   = sc.Input[109];
    SCInputRef in_OTFArrowSlot1    = sc.Input[110];
    SCInputRef in_OTFArrowSlot2    = sc.Input[111];
    SCInputRef in_EnableOTFConf    = sc.Input[112];
    SCInputRef in_OTFConfSlot1     = sc.Input[113];
    SCInputRef in_OTFConfSlot2     = sc.Input[114];
    SCInputRef in_SubpanelMode     = sc.Input[115];
    SCInputRef in_ArrowWindow      = sc.Input[116];
    SCInputRef in_LiveBarMode      = sc.Input[117];
    SCInputRef in_LiveBarLatch     = sc.Input[118];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Orderflow Signal V4";
        sc.StudyDescription =
            "V4: tick-rate tail-window recalculation removes the bar-close latency gate. "
            "Live bar mode reads the forming bar's own trigger values. "
            "Membership-based alert dedup. AutoLoop=0.";
        sc.AutoLoop     = 0;
        sc.GraphRegion  = 1;
        sc.FreeDLL      = 0;
        sc.DrawZeros    = 0;
        sc.UpdateAlways = 1;

        // Run AFTER everything this study reads on its own chart — the Study
        // Overlay instances, and any OTF filter studies. SC's documentation
        // names this as the fix for "the dependent study calculated before the
        // overlay did", which otherwise costs one update cycle of stale data on
        // every pass. sierrachart.h:2554, scconstants.h:830-832.
        sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL;

        sg_Level1.Name         = "Level 1 Signal  (Low Conviction)";
        sg_Level1.DrawStyle    = DRAWSTYLE_ARROWUP;
        sg_Level1.LineWidth    = 1;
        sg_Level1.PrimaryColor = RGB(220, 220, 0);
        sg_Level1.DrawZeros    = 0;

        sg_Level2.Name         = "Level 2 Signal  (Medium Conviction)";
        sg_Level2.DrawStyle    = DRAWSTYLE_ARROWUP;
        sg_Level2.LineWidth    = 2;
        sg_Level2.PrimaryColor = RGB(220, 140, 0);
        sg_Level2.DrawZeros    = 0;

        sg_Level3.Name         = "Level 3 Signal  (High Conviction)";
        sg_Level3.DrawStyle    = DRAWSTYLE_ARROWUP;
        sg_Level3.LineWidth    = 3;
        sg_Level3.PrimaryColor = RGB(0, 220, 80);
        sg_Level3.DrawZeros    = 0;

        sg_Confluence.Name         = "Confluence  (Lookback Background)";
        sg_Confluence.DrawStyle    = DRAWSTYLE_BACKGROUND;
        sg_Confluence.PrimaryColor = RGB(100, 160, 255);
        sg_Confluence.DrawZeros    = 0;
        sg_Confluence.DisplayNameValueInWindowsFlags = 0;

        sg_Score.Name         = "Bar Score";
        sg_Score.DrawStyle    = DRAWSTYLE_BAR;
        sg_Score.LineWidth    = 2;
        sg_Score.PrimaryColor = RGB(120, 120, 120);
        sg_Score.DrawZeros    = 0;

        sg_DbgConn.Name         = "Debug: Connected Triggers";
        sg_DbgConn.DrawStyle    = DRAWSTYLE_LINE;
        sg_DbgConn.LineWidth    = 2;
        sg_DbgConn.PrimaryColor = RGB(200, 200, 0);
        sg_DbgConn.DrawZeros    = 1;

        sg_DbgFire.Name         = "Debug: Firing Triggers";
        sg_DbgFire.DrawStyle    = DRAWSTYLE_LINE;
        sg_DbgFire.LineWidth    = 2;
        sg_DbgFire.PrimaryColor = RGB(0, 200, 200);
        sg_DbgFire.DrawZeros    = 1;

        // -- Trigger defaults -------------------------------------------------
        struct s_TrigDef { int studyID; int sgIdx; int weight; };
        static const s_TrigDef td[50] = {
            {60,  0, 1}, {84,  0, 1}, {82,  0, 1}, {59,  4, 1}, {59,  5, 1},
            {82,  0, 1}, {58,  0, 1}, {57,  2, 1}, {56,  2, 2}, {56,  4, 2},
            {56,  6, 1}, {55,  4, 2}, {55,  5, 2}, {28,  4, 2}, {34,  0, 1},
            {70,  4, 2}, {70,  5, 2}, {68,  0, 1}, {71,  0, 1}, {64,  0, 1},
            {64,  5, 1}, {53,  0, 1}, {76,  0, 2}, {73,  0, 2}, {90,  0, 2},
            {62,  0, 1}, {79,  0, 1}, {67,  0, 1}, {52,  0, 2}, {29,  0, 1},
            {49,  0, 1}, {19,  5, 1}, {47,  2, 1},
            { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0},
            { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0},
            { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0}, { 0,  0, 0},
            { 0,  0, 0}, { 0,  0, 0},
        };

        for (int i = 0; i < 50; ++i)
        {
            SCString refName; refName.Format("Trigger %d", i + 1);
            sc.Input[i * 2].Name = refName;
            sc.Input[i * 2].SetStudySubgraphValues(td[i].studyID, td[i].sgIdx);

            SCString wtName; wtName.Format("Trigger %d Weight", i + 1);
            sc.Input[i * 2 + 1].Name = wtName;
            sc.Input[i * 2 + 1].SetCustomInputStrings("0 - Disabled;1 Point;2 Points;3 Points");
            sc.Input[i * 2 + 1].SetCustomInputIndex(td[i].weight);
        }

        in_Level1Thresh.Name = "Level 1 Threshold  (min score for any arrow)";
        in_Level1Thresh.SetInt(3);
        in_Level1Thresh.SetIntLimits(1, 150);

        in_Level2Thresh.Name = "Level 2 Threshold";
        in_Level2Thresh.SetInt(5);
        in_Level2Thresh.SetIntLimits(1, 150);

        in_Level3Thresh.Name = "Level 3 Threshold  (highest conviction)";
        in_Level3Thresh.SetInt(8);
        in_Level3Thresh.SetIntLimits(1, 150);

        in_LookbackBars.Name = "Lookback Window  (bars for rolling sum)";
        in_LookbackBars.SetInt(5);
        in_LookbackBars.SetIntLimits(1, 200);

        in_ConfluenceThresh.Name = "Confluence Threshold  (rolling sum to fire background)";
        in_ConfluenceThresh.SetInt(6);
        in_ConfluenceThresh.SetIntLimits(1, 450);

        in_OffsetTicks.Name = "Signal Offset  (ticks from High or Low)";
        in_OffsetTicks.SetFloat(2.0f);
        in_OffsetTicks.SetFloatLimits(0.0f, 200.0f);

        in_TriggerPosition.Name = "Trigger Position  (also update arrow Draw Style to match)";
        in_TriggerPosition.SetCustomInputStrings(
            "Below Candle  (arrow up,   offset from Low);"
            "Above Candle  (arrow down, offset from High)");
        in_TriggerPosition.SetCustomInputIndex(0);

        in_AlertSignal.Name = "Signal Alert  (fires when Level 1 / 2 / 3 arrow prints)";
        in_AlertSignal.SetAlertSoundNumber(0);

        in_AlertConfluence.Name = "Confluence Alert  (fires on onset of a new confluence zone)";
        in_AlertConfluence.SetAlertSoundNumber(0);

        in_EnableOTFArrow.Name = "Enable OTF Filter for Arrows";
        in_EnableOTFArrow.SetCustomInputStrings("No;Yes");
        in_EnableOTFArrow.SetCustomInputIndex(0);

        in_OTFArrowSlot1.Name = "Arrow OTF Slot 1";
        in_OTFArrowSlot1.SetStudySubgraphValues(0, 0);

        in_OTFArrowSlot2.Name = "Arrow OTF Slot 2  (optional - AND with Slot 1)";
        in_OTFArrowSlot2.SetStudySubgraphValues(0, 0);

        in_EnableOTFConf.Name = "Enable OTF Filter for Confluence";
        in_EnableOTFConf.SetCustomInputStrings("No;Yes");
        in_EnableOTFConf.SetCustomInputIndex(0);

        in_OTFConfSlot1.Name = "Confluence OTF Slot 1";
        in_OTFConfSlot1.SetStudySubgraphValues(0, 0);

        in_OTFConfSlot2.Name = "Confluence OTF Slot 2  (optional - AND with Slot 1)";
        in_OTFConfSlot2.SetStudySubgraphValues(0, 0);

        in_SubpanelMode.Name = "Sub-panel Mode  (arrows at level 1/2/3 instead of price)";
        in_SubpanelMode.SetCustomInputStrings(
            "No  (price chart - arrows at Low / High + offset);"
            "Yes  (sub-panel - arrows at Y=1 / 2 / 3)");
        in_SubpanelMode.SetCustomInputIndex(0);

        in_ArrowWindow.Name = "Level 1/2/3 Window  (bars to accumulate arrow score; 1 = current bar only)";
        in_ArrowWindow.SetInt(1);
        in_ArrowWindow.SetIntLimits(1, 1000000);

        in_LiveBarMode.Name = "Live Bar Mode  (how the forming bar is scored)";
        in_LiveBarMode.SetCustomInputStrings(
            "Proxy  (V3 behavior - reads bar i-1, one bar late);"
            "Live  (reads the forming bar - fastest, no double-count);"
            "Both  (union - earliest fire, but double-counts on the live bar)");
        in_LiveBarMode.SetCustomInputIndex(1);

        in_LiveBarLatch.Name = "Live Bar Latch  (forming bar score never decreases)";
        in_LiveBarLatch.SetCustomInputStrings(
            "No  (live arrow can disappear if the source retracts);"
            "Yes  (live arrow is sticky until the bar closes)");
        in_LiveBarLatch.SetCustomInputIndex(1);

        return;
    }

    // =========================================================================
    // GUARD: nothing to process
    // =========================================================================
    const int totalBars = sc.ArraySize;
    if (totalBars < 2)
        return;

    const int lastBar = totalBars - 1;

    // =========================================================================
    // READ SETTINGS (once per call, outside the bar loop)
    // =========================================================================
    const float l1       = static_cast<float>(in_Level1Thresh.GetInt());
    const float l2       = static_cast<float>(in_Level2Thresh.GetInt());
    const float l3       = static_cast<float>(in_Level3Thresh.GetInt());
    const float ct       = static_cast<float>(in_ConfluenceThresh.GetInt());
    const float offset   = in_OffsetTicks.GetFloat() * sc.TickSize;
    const int   lookback = in_LookbackBars.GetInt();
    int         arrowWindowRaw = in_ArrowWindow.GetInt();
    const int   arrowWindow = (arrowWindowRaw < 1) ? 1 : arrowWindowRaw;
    const int   position = in_TriggerPosition.GetIndex();
    const int   sigSound  = in_AlertSignal.GetInt();
    const int   confSound = in_AlertConfluence.GetInt();
    const bool  subpanel  = (in_SubpanelMode.GetIndex() == 1);

    // V4: 0 Proxy, 1 Live, 2 Both
    const int   liveBarMode  = in_LiveBarMode.GetIndex();
    const bool  useLiveLatch = (in_LiveBarLatch.GetIndex() == 1);

    // =========================================================================
    // DATA LAYER  >>> BEGIN <<<
    //
    // Trigger arrays are fetched ONCE per update cycle here, then the bar loop
    // indexes into these local references. With AutoLoop=1 this would be one
    // call per trigger PER BAR; hoisting it out is what keeps the cost at
    // exactly N calls per cycle regardless of bar count.
    //
    // NOTE: sc.ChartNumber means these come from the MAIN chart only — i.e.
    // whatever a Study Overlay projected onto this chart's bar timeline. See
    // "KNOWN, NOT YET FIXED" in the header. The cross-chart rework replaces
    // this block and the rising-edge detection in the bar loop; everything
    // between them is unaffected.
    // =========================================================================
    SCFloatArray trigData[50];
    int          trigWeight[50]        = {};
    bool         trigActive[50]        = {};
    bool         hasConfiguredTriggers = false;
    int          totalConnected        = 0;

    for (int t = 0; t < 50; ++t)
    {
        const int weight  = sc.Input[t * 2 + 1].GetIndex();
        const int studyID = sc.Input[t * 2].GetStudyID();

        if (weight == 0 || studyID == 0)
            continue;

        hasConfiguredTriggers = true;
        trigWeight[t] = weight;

        sc.GetStudyArrayFromChartUsingID(
            sc.ChartNumber, studyID,
            sc.Input[t * 2].GetSubgraphIndex(),
            trigData[t]);

        if (trigData[t].GetArraySize() > 0)
        {
            trigActive[t] = true;
            ++totalConnected;
        }
    }
    // ========================= DATA LAYER  >>> END <<< ========================

    // -------------------------------------------------------------------------
    // STARTUP / CONNECTIVITY GUARD  (V4 fix 4)
    //
    // Nothing connected at all: the trigger studies have not finished
    // calculating. Ask SC for a full recalculate rather than silently waiting
    // for the next bar. Bounded, so a source chart that is closed for good
    // cannot put us in a recalculate loop.
    //
    // Do NOT update lastKnownBars on this path — the pass never ran, so the
    // new-bar event must stay unconsumed.
    // -------------------------------------------------------------------------
    int& lastConnCount = sc.GetPersistentInt(9);
    int& recalcTries   = sc.GetPersistentInt(10);

    if (hasConfiguredTriggers && totalConnected == 0)
    {
        if (recalcTries < 3)
        {
            ++recalcTries;
            sc.FlagFullRecalculate = 1;
        }
        return;
    }

    // Partial connection that later completes must re-score history: a source
    // chart arriving late would otherwise leave every already-scored bar
    // missing its triggers.
    const bool connChanged = (totalConnected != lastConnCount);
    lastConnCount = totalConnected;
    recalcTries   = 0;

    // =========================================================================
    // PRE-FETCH OTF ARRAYS  (also once per call)
    // =========================================================================
    SCFloatArray otfArrow1, otfArrow2, otfConf1, otfConf2;
    const bool useOTFArrow = (in_EnableOTFArrow.GetIndex() == 1);
    const bool useOTFConf  = (in_EnableOTFConf.GetIndex()  == 1);

    if (useOTFArrow)
    {
        const int id1 = in_OTFArrowSlot1.GetStudyID();
        if (id1 > 0)
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, id1,
                in_OTFArrowSlot1.GetSubgraphIndex(), otfArrow1);

        const int id2 = in_OTFArrowSlot2.GetStudyID();
        if (id2 > 0)
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, id2,
                in_OTFArrowSlot2.GetSubgraphIndex(), otfArrow2);
    }

    if (useOTFConf)
    {
        const int id1 = in_OTFConfSlot1.GetStudyID();
        if (id1 > 0)
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, id1,
                in_OTFConfSlot1.GetSubgraphIndex(), otfConf1);

        const int id2 = in_OTFConfSlot2.GetStudyID();
        if (id2 > 0)
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, id2,
                in_OTFConfSlot2.GetSubgraphIndex(), otfConf2);
    }

    // =========================================================================
    // PASS WINDOW  (V4 fixes 1 and 3 — replaces V3's early return)
    //
    // There is no early return. Every call recomputes at least the tail.
    // =========================================================================
    int& lastKnownBars = sc.GetPersistentInt(1);
    int& everFullPass  = sc.GetPersistentInt(5);

    const bool isFullRecalc = (sc.UpdateStartIndex == 0) || sc.IsFullRecalculation;
    const bool barsChanged  = (totalBars != lastKnownBars);   // != , not > (fix 3)

    // The tail must cover every window the bar loop reads backwards over, plus
    // however far back a trigger array can be backfilled.
    int tail = OFS4_TAIL_BASE;
    if (lookback    > tail) tail = lookback;
    if (arrowWindow > tail) tail = arrowWindow;
    if (tail > OFS4_TAIL_MAX) tail = OFS4_TAIL_MAX;
    tail += OFS4_TAIL_EDGE;

    const bool doFullPass  = isFullRecalc || barsChanged || connChanged || (everFullPass == 0);
    const int  windowStart = doFullPass
                           ? 0
                           : ((totalBars - tail > 0) ? (totalBars - tail) : 0);

    lastKnownBars = totalBars;
    everFullPass  = 1;

    // Live bar latch resets when the forming bar changes identity.
    if (barsChanged && useLiveLatch)
        sg_Level1.Arrays[2][lastBar] = 0.0f;

    // =========================================================================
    // BAR LOOP
    // =========================================================================
    for (int i = windowStart; i < totalBars; ++i)
    {
        const bool isLast = (i == lastBar && i > 0);

        // ---------------------------------------------------------------------
        // 1. Score this bar
        // ---------------------------------------------------------------------
        float score   = 0.0f;
        int   dbgConn = 0;
        int   dbgFire = 0;

        for (int t = 0; t < 50; ++t)
        {
            if (!trigActive[t])
                continue;

            // Bounds guard — trigger array may be shorter than current chart.
            // NOTE this fails CLOSED and silently: a source study lagging one
            // bar behind the main chart makes its newest bars unreadable, which
            // is exactly where a live arrow would print. dbgConn is counted
            // after the guard so SG6 does surface it.
            if (i >= trigData[t].GetArraySize())
                continue;

            ++dbgConn;

            // -----------------------------------------------------------------
            // RISING EDGE DETECTION
            //
            // Count a trigger only on the first bar it fires (0 -> nonzero).
            // Bars where it stays nonzero are reprints — ignored. This is what
            // stops a higher-timeframe trigger, held across many chart bars on
            // live data, from inflating the confluence sum (the V2 defect).
            //
            // V4: which bar the forming bar reads is now Live Bar Mode.
            //   Proxy — trigData[i-1] vs [i-2]: V3 behavior, one bar late, and
            //           double-counts an edge at lastBar-1 with the normal
            //           branch at i = lastBar-1
            //   Live  — trigData[i] vs [i-1]: same rule as every other bar,
            //           sees the forming bar, no double-count
            //   Both  — union; earliest possible fire, accepts the double-count
            // -----------------------------------------------------------------
            bool fired = false;

            const bool readLive  = (!isLast) || (liveBarMode != 0);
            const bool readProxy = isLast && (liveBarMode != 1);

            if (readLive && trigData[t][i] != 0.0f)
                fired = (i == 0 || trigData[t][i - 1] == 0.0f);

            if (!fired && readProxy && i >= 1 && trigData[t][i - 1] != 0.0f)
                fired = (i < 2 || trigData[t][i - 2] == 0.0f);

            if (fired)
            {
                score += static_cast<float>(trigWeight[t]);
                ++dbgFire;
            }
        }

        // ---------------------------------------------------------------------
        // 1b. Live bar latch  (V4)
        //     A source array that flickers non-zero -> zero within the forming
        //     bar would make the arrow blink. Hold the running max instead.
        // ---------------------------------------------------------------------
        if (isLast && useLiveLatch)
        {
            if (score > sg_Level1.Arrays[2][i])
                sg_Level1.Arrays[2][i] = score;
            score = sg_Level1.Arrays[2][i];
        }

        sg_Level1.Arrays[0][i] = score;

        // ---------------------------------------------------------------------
        // 2. Rolling confluence sum  (reads persisted scores from Arrays[0])
        //
        //    Arrays[0] persists across calls, so on a tail-only pass the bars
        //    below windowStart still contribute correctly. everFullPass
        //    guarantees they were scored at least once.
        // ---------------------------------------------------------------------
        const int sumStart = (i - lookback + 1 > 0) ? (i - lookback + 1) : 0;
        float rollingSum = 0.0f;
        for (int b = sumStart; b <= i; ++b)
            rollingSum += sg_Level1.Arrays[0][b];

        // ---------------------------------------------------------------------
        // 2b. Windowed arrow score
        //     Accumulate bar scores over the arrow window so a strong move
        //     whose triggers spread across consecutive bars during volatility
        //     can still reach a fire threshold. arrowWindow == 1 sums only the
        //     current bar -> identical to pre-window behavior.
        // ---------------------------------------------------------------------
        const int awStart = (i - arrowWindow + 1 > 0) ? (i - arrowWindow + 1) : 0;
        float arrowScore = 0.0f;
        for (int b = awStart; b <= i; ++b)
            arrowScore += sg_Level1.Arrays[0][b];

        // ---------------------------------------------------------------------
        // 3a. OTF filter — arrows
        // ---------------------------------------------------------------------
        bool otfArrowPasses = true;
        if (useOTFArrow)
        {
            auto checkOTF = [&](SCFloatArray& arr) -> bool {
                if (arr.GetArraySize() == 0) return false;
                if (i >= arr.GetArraySize())  return false;
                return (arr[i] != 0.0f) ||
                       (isLast && i > 0 && arr[i - 1] != 0.0f);
            };

            if (in_OTFArrowSlot1.GetStudyID() > 0 && !checkOTF(otfArrow1))
                otfArrowPasses = false;
            if (otfArrowPasses &&
                in_OTFArrowSlot2.GetStudyID() > 0 && !checkOTF(otfArrow2))
                otfArrowPasses = false;
        }

        // ---------------------------------------------------------------------
        // 3b. OTF filter — confluence
        // ---------------------------------------------------------------------
        bool otfConfPasses = true;
        if (useOTFConf)
        {
            auto checkOTF = [&](SCFloatArray& arr) -> bool {
                if (arr.GetArraySize() == 0) return false;
                if (i >= arr.GetArraySize())  return false;
                return (arr[i] != 0.0f) ||
                       (isLast && i > 0 && arr[i - 1] != 0.0f);
            };

            if (in_OTFConfSlot1.GetStudyID() > 0 && !checkOTF(otfConf1))
                otfConfPasses = false;
            if (otfConfPasses &&
                in_OTFConfSlot2.GetStudyID() > 0 && !checkOTF(otfConf2))
                otfConfPasses = false;
        }

        // ---------------------------------------------------------------------
        // 4. Fire level and confluence
        // ---------------------------------------------------------------------
        int fireLevel = 0;
        if (otfArrowPasses)
        {
            if      (arrowScore >= l3) fireLevel = 3;
            else if (arrowScore >= l2) fireLevel = 2;
            else if (arrowScore >= l1) fireLevel = 1;
        }

        const bool fireConfluence = otfConfPasses && (rollingSum >= ct);

        // ---------------------------------------------------------------------
        // 5. Arrow Y placement
        //    Price chart mode : Low - offset  /  High + offset
        //    Sub-panel mode   : fire level (1 / 2 / 3)
        //
        //    On the forming bar sc.Low / sc.High still move, so a live arrow's
        //    Y drifts until the bar closes. That is inherent to printing before
        //    bar close, not a defect.
        // ---------------------------------------------------------------------
        float arrowPrice = 0.0f;
        if (fireLevel > 0)
        {
            if (subpanel)
                arrowPrice = static_cast<float>(fireLevel);
            else
                arrowPrice = (position == 0) ? (sc.Low[i]  - offset)
                                             : (sc.High[i] + offset);
        }

        // ---------------------------------------------------------------------
        // 6. Write subgraphs
        // ---------------------------------------------------------------------
        sg_Level1[i]     = (fireLevel == 1) ? arrowPrice : 0.0f;
        sg_Level2[i]     = (fireLevel == 2) ? arrowPrice : 0.0f;
        sg_Level3[i]     = (fireLevel == 3) ? arrowPrice : 0.0f;
        sg_Confluence[i] = fireConfluence ? 1.0f : 0.0f;

        sg_Score[i]   = score;
        sg_DbgConn[i] = static_cast<float>(dbgConn);
        sg_DbgFire[i] = static_cast<float>(dbgFire);

        // Score bar tier colors follow the Level subgraph primary colors and the
        // windowed arrow score, so the histogram color matches the arrow tier
        // that actually fired (bar height still shows this bar's raw score).
        if      (arrowScore >= l3) sg_Score.DataColor[i] = sg_Level3.PrimaryColor;
        else if (arrowScore >= l2) sg_Score.DataColor[i] = sg_Level2.PrimaryColor;
        else if (arrowScore >= l1) sg_Score.DataColor[i] = sg_Level1.PrimaryColor;
        else                       sg_Score.DataColor[i] = sg_Score.PrimaryColor;
    }

    // =========================================================================
    // 7. ALERTS  (V4 fix 5 — membership dedup, not a watermark)
    //
    // Arrows appear retroactively: a higher-timeframe trigger backfills onto
    // bars that are already historical by the time the value is knowable. Any
    // dedup scheme based on "newest bar alerted" therefore loses arrows that
    // materialize BEHIND that mark — permanently, at any scan depth.
    //
    // Instead: a 32-bar bitmap of bars already alerted on, anchored to
    // alertBaseBar. Bits shift out as the chart advances. An arrow appearing
    // behind a newer one still alerts, because membership does not care about
    // order.
    //
    // Alerts are anchored to the CURRENT last bar so SC treats them as live
    // (it suppresses alerts raised against stale bar indexes during real-time
    // updates — the reason the pre-V3.2 per-bar SetAlert calls never fired).
    // The message states how many bars back the arrow actually printed.
    // =========================================================================
    int& alertBaseBar  = sc.GetPersistentInt(6);
    int& sigAlertMask  = sc.GetPersistentInt(7);
    int& confAlertMask = sc.GetPersistentInt(8);

    if (lastBar > alertBaseBar)
    {
        // Slide the window forward, shifting consumed bits out the top.
        const int shift = lastBar - alertBaseBar;
        if (shift >= 32)
        {
            sigAlertMask  = 0;
            confAlertMask = 0;
        }
        else
        {
            sigAlertMask  = static_cast<int>(static_cast<unsigned int>(sigAlertMask)  >> shift);
            confAlertMask = static_cast<int>(static_cast<unsigned int>(confAlertMask) >> shift);
        }
        alertBaseBar = lastBar;
    }
    else if (lastBar < alertBaseBar)
    {
        // Chart shrank (reload / history trim). Indices are meaningless now.
        alertBaseBar  = lastBar;
        sigAlertMask  = -1;
        confAlertMask = -1;
    }

    if (isFullRecalc)
    {
        // Swallow the window, do not clear it: clearing would re-alert every
        // arrow still in range after any settings edit or chart load.
        sigAlertMask  = -1;
        confAlertMask = -1;
        return;
    }

    const int ALERT_SCAN_BARS = 32;   // must not exceed the bitmap width
    int scanFloor = totalBars - ALERT_SCAN_BARS;
    if (scanFloor < 0) scanFloor = 0;

    // ---- Signal alert: newest un-consumed arrow in the window ---------------
    if (sigSound > 0)
    {
        for (int b = lastBar; b >= scanFloor; --b)
        {
            const int k = alertBaseBar - b;
            if (k < 0 || k >= 32)          continue;
            if (sigAlertMask & (1 << k))   continue;   // already alerted on this bar

            int level = 0;
            if      (sg_Level3[b] != 0.0f) level = 3;
            else if (sg_Level2[b] != 0.0f) level = 2;
            else if (sg_Level1[b] != 0.0f) level = 1;

            if (level > 0)
            {
                SCString msg;
                msg.Format("Orderflow Signal L%d (%d bar(s) back)", level, lastBar - b);
                sc.SetAlert(sigSound, lastBar, msg);
                sigAlertMask |= (1 << k);
                break;  // one alert per pass; at tick rate a cluster drains in ~a second
            }
        }
    }

    // ---- Confluence alert: newest un-consumed zone ONSET ---------------------
    if (confSound > 0)
    {
        for (int b = lastBar; b >= scanFloor; --b)
        {
            const int k = alertBaseBar - b;
            if (k < 0 || k >= 32)          continue;
            if (confAlertMask & (1 << k))  continue;

            const bool onset = (sg_Confluence[b] > 0.5f) &&
                               (b == 0 || sg_Confluence[b - 1] < 0.5f);
            if (onset)
            {
                SCString msg;
                msg.Format("Orderflow Confluence Zone (%d bar(s) back)", lastBar - b);
                sc.SetAlert(confSound, lastBar, msg);
                confAlertMask |= (1 << k);
                break;
            }
        }
    }
}
