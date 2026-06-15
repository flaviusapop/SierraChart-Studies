// =============================================================================
// OrderflowSignalV2.cpp
// Sierra Chart ACSIL Custom Study  —  v1 (AutoLoop = 0 edition)
//
// Identical feature set to OrderflowSignal.cpp with one key architectural
// change: sc.AutoLoop = 0.
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
//
// AUXILIARY ARRAYS (sg_Level1.Arrays[]):
//   [0]  Score per bar
//   [1]  Signal alert-fired flag per bar
//   [2]  Confluence alert-fired flag per bar
// =============================================================================

#include "sierrachart.h"

SCDLLName("OrderflowSignalV2")

// =============================================================================
SCSFExport scsf_OrderflowSignalV2(SCStudyInterfaceRef sc)
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

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Orderflow Signal V2";
        sc.StudyDescription =
            "AutoLoop=0 edition. Trigger arrays fetched once per cycle; "
            "bar loop covers only the rolling confluence window. "
            "Feature-identical to Orderflow Signal.";
        sc.AutoLoop     = 0;   // <<< KEY CHANGE — we manage the bar loop
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
            sc.Input[i * 2 + 1].SetCustomInputStrings("0 — Disabled;1 Point;2 Points;3 Points");
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

        in_TriggerPosition.Name = "Trigger Position";
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

        in_OTFArrowSlot2.Name = "Arrow OTF Slot 2  (optional — AND with Slot 1)";
        in_OTFArrowSlot2.SetStudySubgraphValues(0, 0);

        in_EnableOTFConf.Name = "Enable OTF Filter for Confluence";
        in_EnableOTFConf.SetCustomInputStrings("No;Yes");
        in_EnableOTFConf.SetCustomInputIndex(0);

        in_OTFConfSlot1.Name = "Confluence OTF Slot 1";
        in_OTFConfSlot1.SetStudySubgraphValues(0, 0);

        in_OTFConfSlot2.Name = "Confluence OTF Slot 2  (optional — AND with Slot 1)";
        in_OTFConfSlot2.SetStudySubgraphValues(0, 0);

        in_SubpanelMode.Name = "Sub-panel Mode  (arrows at level 1/2/3 instead of price)";
        in_SubpanelMode.SetCustomInputStrings(
            "No  (price chart — arrows at Low / High + offset);"
            "Yes  (sub-panel — arrows at Y=1 / 2 / 3)");
        in_SubpanelMode.SetCustomInputIndex(0);

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
    const int   position = in_TriggerPosition.GetIndex();
    const int   sigSound  = in_AlertSignal.GetInt();
    const int   confSound = in_AlertConfluence.GetInt();
    const bool  subpanel  = (in_SubpanelMode.GetIndex() == 1);

    // =========================================================================
    // PRE-FETCH TRIGGER ARRAYS  (the key efficiency win)
    // GetStudyArrayFromChartUsingID is called ONCE per trigger here, then the
    // bar loop reads directly from these local array references — no repeated
    // API calls per bar.
    // =========================================================================
    SCFloatArray trigData[50];
    int          trigWeight[50]     = {};
    bool         trigActive[50]     = {};
    bool         hasConfiguredTriggers = false;
    int          totalConnected        = 0;

    for (int t = 0; t < 50; ++t)
    {
        const int weight   = sc.Input[t * 2 + 1].GetIndex();
        const int studyID  = sc.Input[t * 2].GetStudyID();

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
    // Return early — sc.UpdateAlways = 1 will call us again within 200 ms.
    // Once trigger studies are ready, totalConnected > 0 and we proceed.
    // If history is missing after SC starts, one manual Recalculate is still
    // needed (same as V1) because SC may not re-issue UpdateStartIndex = 0
    // on a plain UpdateAlways tick.
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
    // DETERMINE WINDOW TO PROCESS
    //
    // windowStart ensures the rolling sum window (lookback bars) is always
    // fully rescored on every call, at minimal cost.
    //
    // sc.UpdateStartIndex is set by Sierra Chart:
    //   Full recalculate → 0  (process everything)
    //   Live tick        → last bar  (process last lookback+1 bars only)
    // =========================================================================
    const int lastBar = totalBars - 1;

    // ---- Bar-close detection ------------------------------------------------
    // sc.GetPersistentInt(1) stores the ArraySize from the previous call.
    //
    // We only process on:
    //   (a) A new bar forming   — ArraySize grew since last call
    //   (b) Full recalculate    — sc.UpdateStartIndex == 0
    //       (first add, Studies menu Recalculate, startup guard reset)
    //
    // Intrabar ticks are skipped entirely: the last bar-close pass already
    // scored every bar correctly, and intrabar partial signals would just
    // disappear before the bar closes anyway.
    // -------------------------------------------------------------------------
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

            const bool fired = (trigData[t][i] != 0.0f) ||
                               (isLast && trigData[t][i - 1] != 0.0f);
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
            if      (score >= l3) fireLevel = 3;
            else if (score >= l2) fireLevel = 2;
            else if (score >= l1) fireLevel = 1;
        }

        const bool fireConfluence = otfConfPasses && (rollingSum >= ct);

        // ---------------------------------------------------------------------
        // 5. Arrow Y placement
        //    Price chart mode : Low - offset  /  High + offset
        //    Sub-panel mode   : fire level (1 / 2 / 3) — stays on-scale
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

        // Score bar tiers follow the Level subgraph primary colors.
        // Change Level 1/2/3 colors in Study Settings and the histogram updates automatically.
        if      (score >= l3) sg_Score.DataColor[i] = sg_Level3.PrimaryColor;
        else if (score >= l2) sg_Score.DataColor[i] = sg_Level2.PrimaryColor;
        else if (score >= l1) sg_Score.DataColor[i] = sg_Level1.PrimaryColor;
        else                  sg_Score.DataColor[i] = sg_Score.PrimaryColor;

        // ---------------------------------------------------------------------
        // 7. Alerts
        // Fires for any bar in the window where a signal newly appears.
        // alertFiredFlag (Arrays[1]) suppresses repeats — once set for a bar
        // it won't fire again until the signal resets to 0.
        // This means a HTF trigger that fires 3 bars back still alerts.
        // ---------------------------------------------------------------------

        // Signal alert: fires once per bar on first arrow, resets when gone
        float& sigFlag = sg_Level1.Arrays[1][i];
        if (sigSound > 0)
        {
            if (fireLevel > 0 && sigFlag < 0.5f)
            {
                sc.SetAlert(sigSound, i, "Orderflow Signal");
                sigFlag = 1.0f;
            }
            else if (fireLevel == 0)
                sigFlag = 0.0f;
        }
        else
        {
            sigFlag = 0.0f;
        }

        // Confluence alert: fires on zone onset only (transition 0 → 1)
        float& confFlag      = sg_Level1.Arrays[2][i];
        const bool prevConf  = (i > 0) && (sg_Confluence[i - 1] > 0.5f);
        const bool confOnset = fireConfluence && !prevConf;

        if (confSound > 0)
        {
            if (confOnset && confFlag < 0.5f)
            {
                sc.SetAlert(confSound, i, "Orderflow Confluence Zone");
                confFlag = 1.0f;
            }
            else if (!fireConfluence)
                confFlag = 0.0f;
        }
        else
        {
            confFlag = 0.0f;
        }
    }
}
