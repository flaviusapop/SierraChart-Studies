// =============================================================================
// OrderflowConfluence.cpp
// Sierra Chart ACSIL Custom Study
//
// PURPOSE:
//   Combines up to 25 bull and 25 bear orderflow triggers via a weighted-point
//   system. Fires two types of signals:
//
//   1. IMMEDIATE SIGNAL (arrows):
//      Current bar's point score >= threshold → arrow on that bar only.
//
//   2. LOOKBACK CONFLUENCE (background shading):
//      Rolling sum of scores over the last N bars >= threshold → background
//      shading that persists until the rolling sum falls below the threshold.
//
// SCORING MODES (user-selectable):
//   Shared     — Bull triggers add points (+), bear triggers subtract (−).
//                One combined score. Long fires at >= +threshold, short at
//                <= −threshold.
//   Independent — Bull and bear pools are completely separate. Both can fire
//                 on the same bar.
//
// TRIGGER ACTIVE CONDITION:
//   A trigger fires (contributes its weight) when its referenced study
//   subgraph has a non-zero value on the current bar.
//
// INPUT LAYOUT  (106 total — within Sierra Chart's 128 limit):
//   [0 .. 49]   Bull triggers: Input[2n]   = Study Subgraph reference
//                               Input[2n+1] = Weight (0=off, 1, 2, or 3 pts)
//   [50 .. 99]  Bear triggers: Input[50+2n]   = Study Subgraph reference
//                               Input[50+2n+1] = Weight (0=off, 1, 2, or 3 pts)
//   [100]  Scoring Mode (Shared / Independent)
//   [101]  Long Entry Threshold  (immediate arrow)
//   [102]  Short Entry Threshold (immediate arrow)
//   [103]  Lookback Window (bars)
//   [104]  Long Confluence Threshold  (lookback sum)
//   [105]  Short Confluence Threshold (lookback sum, entered as positive)
//
// SUBGRAPHS:
//   SG0  Long Signal       — ↑ Arrow, green,   fires on immediate long threshold
//   SG1  Short Signal      — ↓ Arrow, red,     fires on immediate short threshold
//   SG2  Long Confluence   — Background, green, on while lookback sum ≥ threshold
//   SG3  Short Confluence  — Background, red,   on while lookback sum ≥ threshold
//
// AUXILIARY ARRAYS (stored inside sg_LongSignal.Arrays[]):
//   [0]  longScore  per bar  (bull points earned this bar)
//   [1]  shortScore per bar  (bear points earned this bar, stored positive)
//
// SETUP:
//   1. Copy to Sierra Chart ACS_Source folder.
//   2. Build via Analysis >> Build Custom Studies DLL.
//   3. Add the study to your chart.
//   4. In study settings, assign each Bull/Bear trigger to the study+subgraph
//      you want to monitor, and set its weight (0 = disabled).
//   5. Adjust thresholds to match your setup.
// =============================================================================

#include "sierrachart.h"

SCDLLName("OrderflowConfluence")

// =============================================================================
SCSFExport scsf_OrderflowConfluence(SCStudyInterfaceRef sc)
{
    // -------------------------------------------------------------------------
    // SUBGRAPHS
    // -------------------------------------------------------------------------
    SCSubgraphRef sg_LongSignal      = sc.Subgraph[0];
    SCSubgraphRef sg_ShortSignal     = sc.Subgraph[1];
    SCSubgraphRef sg_LongConfluence  = sc.Subgraph[2];
    SCSubgraphRef sg_ShortConfluence = sc.Subgraph[3];

    // sg_LongSignal.Arrays[0] — bull (long) score stored per bar
    // sg_LongSignal.Arrays[1] — bear (short) score stored per bar (positive)

    // -------------------------------------------------------------------------
    // INPUTS — signal settings (indices 100-105)
    // -------------------------------------------------------------------------
    SCInputRef in_ScoringMode         = sc.Input[100];
    SCInputRef in_LongEntryThreshold  = sc.Input[101];
    SCInputRef in_ShortEntryThreshold = sc.Input[102];
    SCInputRef in_LookbackBars        = sc.Input[103];
    SCInputRef in_LongLookbackThresh  = sc.Input[104];
    SCInputRef in_ShortLookbackThresh = sc.Input[105];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Orderflow Confluence";
        sc.StudyDescription =
            "Combines up to 25 bull + 25 bear orderflow triggers via weighted "
            "point scores. Fires arrows on immediate threshold and background "
            "shading when a rolling lookback sum meets the confluence threshold. "
            "Inputs [0-49]: Bull triggers (study subgraph + weight). "
            "Inputs [50-99]: Bear triggers. Inputs [100-105]: thresholds.";
        sc.AutoLoop         = 1;
        sc.GraphRegion      = 0;   // overlay on main price chart
        sc.FreeDLL          = 0;
        sc.DrawZeros        = 0;
        sc.UpdateAlways     = 1;   // keep current bar live

        // -- Subgraph: Long Signal (arrow up) --------------------------------
        sg_LongSignal.Name          = "Long Signal";
        sg_LongSignal.DrawStyle     = DRAWSTYLE_ARROWUP;
        sg_LongSignal.LineWidth     = 3;
        sg_LongSignal.PrimaryColor  = RGB(0, 220, 80);
        sg_LongSignal.DrawZeros     = 0;

        // -- Subgraph: Short Signal (arrow down) -----------------------------
        sg_ShortSignal.Name         = "Short Signal";
        sg_ShortSignal.DrawStyle    = DRAWSTYLE_ARROWDOWN;
        sg_ShortSignal.LineWidth    = 3;
        sg_ShortSignal.PrimaryColor = RGB(220, 50, 50);
        sg_ShortSignal.DrawZeros    = 0;

        // -- Subgraph: Long Confluence (background) --------------------------
        sg_LongConfluence.Name         = "Long Confluence (Lookback)";
        sg_LongConfluence.DrawStyle    = DRAWSTYLE_BACKGROUND;
        sg_LongConfluence.PrimaryColor = RGB(0, 180, 80);
        sg_LongConfluence.DrawZeros    = 0;

        // -- Subgraph: Short Confluence (background) -------------------------
        sg_ShortConfluence.Name         = "Short Confluence (Lookback)";
        sg_ShortConfluence.DrawStyle    = DRAWSTYLE_BACKGROUND;
        sg_ShortConfluence.PrimaryColor = RGB(220, 50, 50);
        sg_ShortConfluence.DrawZeros    = 0;

        // -- Bull trigger names (first 10 pre-labeled, rest generic) ---------
        static const char* BullNames[25] = {
            "Bull Trigger 1  — Absorption",
            "Bull Trigger 2  — Stacked Imbalance",
            "Bull Trigger 3  — Delta Divergence",
            "Bull Trigger 4  — Bid Exhaustion",
            "Bull Trigger 5  — Sweep Long",
            "Bull Trigger 6  — Volume Climax",
            "Bull Trigger 7  — POC Reclaim",
            "Bull Trigger 8  — VWAP Reclaim",
            "Bull Trigger 9  — Trapped Shorts",
            "Bull Trigger 10 — Iceberg Bid",
            "Bull Trigger 11",
            "Bull Trigger 12",
            "Bull Trigger 13",
            "Bull Trigger 14",
            "Bull Trigger 15",
            "Bull Trigger 16",
            "Bull Trigger 17",
            "Bull Trigger 18",
            "Bull Trigger 19",
            "Bull Trigger 20",
            "Bull Trigger 21",
            "Bull Trigger 22",
            "Bull Trigger 23",
            "Bull Trigger 24",
            "Bull Trigger 25"
        };

        for (int i = 0; i < 25; ++i)
        {
            const int refIdx = i * 2;       // study subgraph reference
            const int wtIdx  = i * 2 + 1;  // weight

            sc.Input[refIdx].Name = BullNames[i];
            sc.Input[refIdx].SetStudySubgraphValues(0, 0);

            SCString wtName;
            wtName.Format("%s — Weight", BullNames[i]);
            sc.Input[wtIdx].Name = wtName;
            sc.Input[wtIdx].SetCustomInputStrings("0 — Disabled;1 Point;2 Points;3 Points");
            sc.Input[wtIdx].SetCustomInputIndex(0);
        }

        // -- Bear trigger names ----------------------------------------------
        static const char* BearNames[25] = {
            "Bear Trigger 1  — Absorption",
            "Bear Trigger 2  — Stacked Imbalance",
            "Bear Trigger 3  — Delta Divergence",
            "Bear Trigger 4  — Ask Exhaustion",
            "Bear Trigger 5  — Sweep Short",
            "Bear Trigger 6  — Volume Climax",
            "Bear Trigger 7  — POC Rejection",
            "Bear Trigger 8  — VWAP Rejection",
            "Bear Trigger 9  — Trapped Longs",
            "Bear Trigger 10 — Iceberg Ask",
            "Bear Trigger 11",
            "Bear Trigger 12",
            "Bear Trigger 13",
            "Bear Trigger 14",
            "Bear Trigger 15",
            "Bear Trigger 16",
            "Bear Trigger 17",
            "Bear Trigger 18",
            "Bear Trigger 19",
            "Bear Trigger 20",
            "Bear Trigger 21",
            "Bear Trigger 22",
            "Bear Trigger 23",
            "Bear Trigger 24",
            "Bear Trigger 25"
        };

        for (int i = 0; i < 25; ++i)
        {
            const int refIdx = 50 + i * 2;
            const int wtIdx  = 50 + i * 2 + 1;

            sc.Input[refIdx].Name = BearNames[i];
            sc.Input[refIdx].SetStudySubgraphValues(0, 0);

            SCString wtName;
            wtName.Format("%s — Weight", BearNames[i]);
            sc.Input[wtIdx].Name = wtName;
            sc.Input[wtIdx].SetCustomInputStrings("0 — Disabled;1 Point;2 Points;3 Points");
            sc.Input[wtIdx].SetCustomInputIndex(0);
        }

        // -- Signal / threshold settings -------------------------------------
        in_ScoringMode.Name = "Scoring Mode";
        in_ScoringMode.SetCustomInputStrings(
            "Shared — Bull adds / Bear subtracts (one combined score);"
            "Independent — Separate long and short score pools");
        in_ScoringMode.SetCustomInputIndex(0);

        in_LongEntryThreshold.Name = "Long Entry Threshold — Immediate Arrow (pts)";
        in_LongEntryThreshold.SetInt(5);
        in_LongEntryThreshold.SetIntLimits(1, 75);

        in_ShortEntryThreshold.Name = "Short Entry Threshold — Immediate Arrow (pts, enter positive)";
        in_ShortEntryThreshold.SetInt(5);
        in_ShortEntryThreshold.SetIntLimits(1, 75);

        in_LookbackBars.Name = "Lookback Window (bars)";
        in_LookbackBars.SetInt(5);
        in_LookbackBars.SetIntLimits(1, 200);

        in_LongLookbackThresh.Name = "Long Confluence Threshold — Lookback Sum (pts)";
        in_LongLookbackThresh.SetInt(6);
        in_LongLookbackThresh.SetIntLimits(1, 225);

        in_ShortLookbackThresh.Name = "Short Confluence Threshold — Lookback Sum (pts, enter positive)";
        in_ShortLookbackThresh.SetInt(6);
        in_ShortLookbackThresh.SetIntLimits(1, 225);

        return;
    }

    // =========================================================================
    // PER-BAR CALCULATION
    // =========================================================================
    const int idx = sc.Index;

    // -------------------------------------------------------------------------
    // 1. Compute bull score for this bar
    //    A trigger is active when its referenced SG has a non-zero value.
    //    Weight comes from the custom input index (0=off, 1, 2, or 3 pts).
    // -------------------------------------------------------------------------
    float longScore = 0.0f;

    for (int i = 0; i < 25; ++i)
    {
        const int refIdx = i * 2;
        const int wtIdx  = i * 2 + 1;

        const int weight = sc.Input[wtIdx].GetIndex(); // 0,1,2,3
        if (weight == 0)
            continue;

        SCFloatArray refData;
        sc.GetStudyArrayFromChartUsingID(
            sc.ChartNumber,
            sc.Input[refIdx].GetStudyID(),
            sc.Input[refIdx].GetSubgraphIndex(),
            refData
        );

        if (refData.GetArraySize() == 0)
            continue;

        if (refData[idx] != 0.0f)
            longScore += static_cast<float>(weight);
    }

    // -------------------------------------------------------------------------
    // 2. Compute bear score for this bar (stored and used as a positive value;
    //    sign is applied later based on scoring mode)
    // -------------------------------------------------------------------------
    float shortScore = 0.0f;

    for (int i = 0; i < 25; ++i)
    {
        const int refIdx = 50 + i * 2;
        const int wtIdx  = 50 + i * 2 + 1;

        const int weight = sc.Input[wtIdx].GetIndex();
        if (weight == 0)
            continue;

        SCFloatArray refData;
        sc.GetStudyArrayFromChartUsingID(
            sc.ChartNumber,
            sc.Input[refIdx].GetStudyID(),
            sc.Input[refIdx].GetSubgraphIndex(),
            refData
        );

        if (refData.GetArraySize() == 0)
            continue;

        if (refData[idx] != 0.0f)
            shortScore += static_cast<float>(weight);
    }

    // -------------------------------------------------------------------------
    // 3. Persist per-bar scores for rolling lookback
    // -------------------------------------------------------------------------
    sg_LongSignal.Arrays[0][idx] = longScore;
    sg_LongSignal.Arrays[1][idx] = shortScore;

    // -------------------------------------------------------------------------
    // 4. Rolling lookback sum over the last N bars (inclusive of current bar)
    // -------------------------------------------------------------------------
    const int lookback  = in_LookbackBars.GetInt();
    const int startBar  = (idx - lookback + 1 > 0) ? (idx - lookback + 1) : 0;

    float rollingLong  = 0.0f;
    float rollingShort = 0.0f;

    for (int b = startBar; b <= idx; ++b)
    {
        rollingLong  += sg_LongSignal.Arrays[0][b];
        rollingShort += sg_LongSignal.Arrays[1][b];
    }

    // -------------------------------------------------------------------------
    // 5. Apply scoring mode and evaluate thresholds
    // -------------------------------------------------------------------------
    const int scoringMode = in_ScoringMode.GetIndex(); // 0=shared, 1=independent

    const float longThresh    = static_cast<float>(in_LongEntryThreshold.GetInt());
    const float shortThresh   = static_cast<float>(in_ShortEntryThreshold.GetInt());
    const float longLBThresh  = static_cast<float>(in_LongLookbackThresh.GetInt());
    const float shortLBThresh = static_cast<float>(in_ShortLookbackThresh.GetInt());

    bool fireLongImmediate  = false;
    bool fireShortImmediate = false;
    bool fireLongLookback   = false;
    bool fireShortLookback  = false;

    if (scoringMode == 0)
    {
        // -- Shared mode: one combined score per bar -------------------------
        // Bull adds positive points; bear subtracts.
        const float combined        = longScore   - shortScore;
        const float rollingCombined = rollingLong - rollingShort;

        fireLongImmediate  = (combined        >=  longThresh);
        fireShortImmediate = (combined        <= -shortThresh);
        fireLongLookback   = (rollingCombined >=  longLBThresh);
        fireShortLookback  = (rollingCombined <= -shortLBThresh);
    }
    else
    {
        // -- Independent mode: separate pools --------------------------------
        // Each direction evaluated on its own accumulated positive score.
        fireLongImmediate  = (longScore   >= longThresh);
        fireShortImmediate = (shortScore  >= shortThresh);
        fireLongLookback   = (rollingLong  >= longLBThresh);
        fireShortLookback  = (rollingShort >= shortLBThresh);
    }

    // -------------------------------------------------------------------------
    // 6. Write to subgraphs
    //
    //    Arrow subgraphs: value = price level where arrow is drawn.
    //      Long arrow  (ARROWUP)   → placed at sc.Low[idx]  (below bar, pointing up)
    //      Short arrow (ARROWDOWN) → placed at sc.High[idx] (above bar, pointing down)
    //    Set to 0 when not firing; DrawZeros = 0 suppresses drawing.
    //
    //    Background subgraphs: any non-zero value activates the background shade.
    // -------------------------------------------------------------------------
    sg_LongSignal[idx]      = fireLongImmediate  ? sc.Low[idx]  : 0.0f;
    sg_ShortSignal[idx]     = fireShortImmediate ? sc.High[idx] : 0.0f;
    sg_LongConfluence[idx]  = fireLongLookback   ? 1.0f         : 0.0f;
    sg_ShortConfluence[idx] = fireShortLookback  ? 1.0f         : 0.0f;
}
