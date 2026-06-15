// =============================================================================
// OrderflowSignalV3.cpp
// Sierra Chart ACSIL Custom Study  —  v3 (Rising Edge Trigger Detection)
//
// Identical to V2 with one change: trigger firing uses rising edge detection
// instead of raw non-zero detection.
//
// WHY RISING EDGE:
//   Higher timeframe triggers stay non-zero across all chart bars within the
//   HTF bar on live data (e.g. a 15-min trigger fires on bars 1-15 of that
//   period). V2 counted all 15 bars as separate events, inflating the rolling
//   confluence sum and producing phantom high-conviction signals live.
//
//   V3 counts a trigger only on the FIRST bar it goes non-zero (the 0->nonzero
//   transition). Subsequent bars where it stays non-zero are treated as
//   continuation/reprint and ignored.
//
//   On historical data this makes no difference — HTF triggers already show
//   on exactly one bar per event. The fix only changes live behavior.
//
// RISING EDGE LOGIC:
//   Normal bar:   fired = (trigData[i] != 0) && (trigData[i-1] == 0)
//   Last bar:     fired = (trigData[i-1] != 0) && (trigData[i-2] == 0)
//                 (i-1 used as proxy for the just-opened bar, same as V2)
//
// WHY AutoLoop = 0:
//   With AutoLoop = 1, GetStudyArrayFromChartUsingID is called once per
//   trigger PER BAR on every update tick — up to 50 calls * N bars.
//   With AutoLoop = 0 we control the loop. Trigger arrays are fetched ONCE
//   per update cycle (outside the bar loop), then the bar loop indexes into
//   those local references. Cost drops to exactly 50 API calls per cycle
//   regardless of how many bars we process.
//
// WINDOW LOGIC:
//   Processing is skipped on intrabar ticks and only runs on bar close
//   or an explicit full recalculate.
//
//   Bar close / new bar  (ArraySize grew since last call):
//     Full pass from bar 0. HTF triggers anywhere in history are always
//     captured. Persistent bar count (sc.GetPersistentInt(1)) is updated.
//
//   Intrabar tick  (ArraySize unchanged):
//     Return immediately — the last bar-close pass is still valid and
//     intrabar partial signals would vanish before bar close anyway.
//
//   Explicit full recalculate (sc.UpdateStartIndex == 0):
//     Same as bar-close path — full pass, persistent count updated.
//     Triggered by first add, Studies menu Recalculate, or startup guard.
//
// STARTUP AUTO-FIX:
//   If trigger arrays are empty when the study first runs (startup race
//   between SC loading multiple chart studies), sc.ResetStudyToCalculateFromBarOne
//   is set. SC re-queues a full recalculate; by the next pass the trigger
//   studies have finished and all bars score correctly.
//
// SUBGRAPHS:
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
//   Score bar tier colors follow the Level subgraph primary colors.
//
// INPUT LAYOUT  (115 total):
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
//   [116]  Level 1/2/3 Window  (bars to accumulate arrow score; 1 = current bar only)
//
// AUXILIARY ARRAYS (sg_Level1.Arrays[]):
//   [0]  Score per bar
//   [1]  (unused since V3.2 — was per-bar signal alert flag)
//   [2]  (unused since V3.2 — was per-bar confluence alert flag)
//
// V3.2 ALERT FIX:
//   Arrows print retroactively (HTF trigger backfill puts the rising edge
//   2-3 bars back), so per-bar SetAlert calls used stale bar indexes and SC
//   suppressed them. Alerts now run AFTER the bar loop: persistent watermarks
//   (PersistentInt 2/3) track the newest alerted signal/confluence bar; any
//   newer signal within the last 10 bars raises one alert anchored to the
//   current last bar, with "(N bar(s) back)" in the message. Watermarks are
//   fast-forwarded on full recalculation so chart loads never alert-storm.
// =============================================================================

#include "sierrachart.h"

SCDLLName("OrderflowSignalV3")

// =============================================================================
SCSFExport scsf_OrderflowSignalV3(SCStudyInterfaceRef sc)
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

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Orderflow Signal V3";
        sc.StudyDescription =
            "V3: rising edge trigger detection fixes HTF multi-bar reprint on live data. "
            "AutoLoop=0, full recalculate on every bar close.";
        sc.AutoLoop     = 0;
        sc.GraphRegion  = 1;
        sc.FreeDLL      = 0;
        sc.DrawZeros    = 0;
        sc.UpdateAlways = 1;

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
        in_ArrowWindow.SetIntLimits(1, 1000000);  // effectively no cap

        return;
    }

    // =========================================================================
    // GUARD: nothing to process
    // =========================================================================
    const int totalBars = sc.ArraySize;
    if (totalBars < 2)
        return;

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

    // =========================================================================
    // PRE-FETCH TRIGGER ARRAYS  (the key efficiency win)
    // GetStudyArrayFromChartUsingID is called ONCE per trigger here, then the
    // bar loop reads directly from these local array references.
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

    // -------------------------------------------------------------------------
    // Startup guard: if triggers are configured but all arrays are empty,
    // the trigger studies haven't finished calculating yet.
    // -------------------------------------------------------------------------
    if (hasConfiguredTriggers && totalConnected == 0)
        return;

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
    // BAR-CLOSE DETECTION
    // =========================================================================
    const int lastBar = totalBars - 1;

    int& lastKnownBars = sc.GetPersistentInt(1);

    const bool isFullRecalc = (sc.UpdateStartIndex == 0);
    const bool isNewBar     = (totalBars > lastKnownBars);

    if (!isFullRecalc && !isNewBar)
        return;  // intrabar tick — nothing to do

    lastKnownBars = totalBars;

    const int windowStart = 0;  // always full pass

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

            // Bounds guard — trigger array may be shorter than current chart
            if (i >= trigData[t].GetArraySize())
                continue;

            ++dbgConn;

            // -----------------------------------------------------------------
            // RISING EDGE DETECTION  (V3 change from V2)
            //
            // Count a trigger only on the first bar it fires (0 -> nonzero).
            // Bars where it stays nonzero are reprints — ignored.
            //
            // This fixes HTF triggers that stay active across multiple chart
            // bars on live data, which inflated the confluence sum in V2.
            // On historical data the behavior is identical (HTF triggers
            // already show on one bar only).
            //
            // isLast case: the last bar just opened and has no data yet.
            // We use i-1 as a proxy (last closed bar), so the rising edge
            // check looks one step further back at i-2.
            // -----------------------------------------------------------------
            bool fired = false;
            if (isLast)
            {
                // Proxy: did bar i-1 just start a new firing?
                if (trigData[t][i - 1] != 0.0f)
                    fired = (i < 2 || trigData[t][i - 2] == 0.0f);
            }
            else
            {
                // Normal: did bar i just start a new firing?
                if (trigData[t][i] != 0.0f)
                    fired = (i == 0 || trigData[t][i - 1] == 0.0f);
            }

            if (fired)
            {
                score += static_cast<float>(trigWeight[t]);
                ++dbgFire;
            }
        }

        sg_Level1.Arrays[0][i] = score;

        // ---------------------------------------------------------------------
        // 2. Rolling confluence sum  (reads persisted scores from Arrays[0])
        // ---------------------------------------------------------------------
        const int sumStart = (i - lookback + 1 > 0) ? (i - lookback + 1) : 0;
        float rollingSum = 0.0f;
        for (int b = sumStart; b <= i; ++b)
            rollingSum += sg_Level1.Arrays[0][b];

        // ---------------------------------------------------------------------
        // 2b. Windowed arrow score  (V3.1)
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
        // Change Level 1/2/3 colors in Study Settings and histogram updates automatically.
        if      (arrowScore >= l3) sg_Score.DataColor[i] = sg_Level3.PrimaryColor;
        else if (arrowScore >= l2) sg_Score.DataColor[i] = sg_Level2.PrimaryColor;
        else if (arrowScore >= l1) sg_Score.DataColor[i] = sg_Level1.PrimaryColor;
        else                       sg_Score.DataColor[i] = sg_Score.PrimaryColor;

    }

    // =========================================================================
    // 7. ALERTS  (V3.2 — post-loop new-signal scan)
    //
    // WHY THE OLD WAY FAILED:
    //   Arrows often appear 2-3 bars BACK from the live bar. HTF trigger
    //   studies backfill their values onto historical chart bars only after
    //   the HTF bar closes, so the rising edge — and therefore the arrow —
    //   lands on a bar that is already historical by the time it is
    //   detectable. The old code called sc.SetAlert(sound, i, ...) with that
    //   stale bar index from inside the bar loop; Sierra Chart suppresses /
    //   ignores alerts raised for old bar indexes during real-time updates,
    //   so nothing ever fired.
    //
    // NEW APPROACH:
    //   After the bar loop, scan the recent bars for any signal NEWER than
    //   the last one we alerted on (tracked in persistent ints, keyed by bar
    //   index). If found, call sc.SetAlert anchored to the CURRENT last bar
    //   so SC treats it as a live alert, with the message stating how many
    //   bars back the arrow printed.
    //
    //   Dedup is by bar index: an arrow that later "moves back" one bar
    //   (live-bar proxy resolving to its final position) has a SMALLER index
    //   than the one already alerted, so it never re-alerts.
    //
    //   On a full recalculate the watermarks are fast-forwarded to the last
    //   bar and no alert is raised — prevents an alert storm on chart load
    //   (SC would suppress those anyway).
    // =========================================================================
    int& lastAlertedSigBar  = sc.GetPersistentInt(2);
    int& lastAlertedConfBar = sc.GetPersistentInt(3);

    if (isFullRecalc || sc.IsFullRecalculation || lastAlertedSigBar == 0)
    {
        // Initialize / reset watermarks; never alert on historical data.
        lastAlertedSigBar  = lastBar;
        lastAlertedConfBar = lastBar;
        return;
    }

    const int ALERT_SCAN_BARS = 10;  // how far back a late-printing arrow is still alert-worthy
    const int scanFloor = (totalBars - ALERT_SCAN_BARS > 0) ? (totalBars - ALERT_SCAN_BARS) : 0;

    // ---- Signal alert: newest un-alerted arrow in the scan window ----------
    if (sigSound > 0)
    {
        const int start = (lastAlertedSigBar + 1 > scanFloor) ? (lastAlertedSigBar + 1) : scanFloor;
        for (int b = lastBar; b >= start; --b)   // newest first
        {
            int level = 0;
            if      (sg_Level3[b] != 0.0f) level = 3;
            else if (sg_Level2[b] != 0.0f) level = 2;
            else if (sg_Level1[b] != 0.0f) level = 1;

            if (level > 0)
            {
                SCString msg;
                msg.Format("Orderflow Signal L%d (%d bar(s) back)", level, lastBar - b);
                sc.SetAlert(sigSound, lastBar, msg);
                lastAlertedSigBar = b;
                break;  // one alert per update cycle
            }
        }
    }
    else
        lastAlertedSigBar = lastBar;

    // ---- Confluence alert: newest un-alerted zone ONSET ---------------------
    if (confSound > 0)
    {
        const int start = (lastAlertedConfBar + 1 > scanFloor) ? (lastAlertedConfBar + 1) : scanFloor;
        for (int b = lastBar; b >= start; --b)
        {
            const bool onset = (sg_Confluence[b] > 0.5f) &&
                               (b == 0 || sg_Confluence[b - 1] < 0.5f);
            if (onset)
            {
                SCString msg;
                msg.Format("Orderflow Confluence Zone (%d bar(s) back)", lastBar - b);
                sc.SetAlert(confSound, lastBar, msg);
                lastAlertedConfBar = b;
                break;
            }
        }
    }
    else
        lastAlertedConfBar = lastBar;
}
