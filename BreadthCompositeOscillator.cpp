// =============================================================================
// BreadthCompositeOscillator.cpp
// Sierra Chart ACSIL Custom Study
// Spec: BREADTH_OSCILLATOR_PROJECT.md v1.3
//
// v1.3 change: Fixed sign/direction bug in pure ROC approach by blending
// absolute level Z with ROC Z per indicator via LevelROC_Blend input.
// When VOLD/ADD is in negative territory but ROC is positive (decline
// slowing), pure ROC incorrectly read bullish. Blending with absolute level
// restores directional context.
//
// v1.2 change: Removed $TICK. Replaced with ROC derivatives of $VOLD and
// $ADD to capture breadth momentum (acceleration/deceleration).
//
// ARCHITECTURE:
//   Four normalized streams, each through full dual Z-score / RATIO pipeline:
//     VOLD_level_Zblend  — absolute VOLD level
//     VOLD_ROC_Zblend    — VOLD rate of change (current - N bars ago)
//     ADD_level_Zblend   — absolute ADD level
//     ADD_ROC_Zblend     — ADD rate of change
//
//   Per-indicator signal:
//     VOLD_signal = VOLD_level_Zblend * LevelROC_Blend
//                 + VOLD_ROC_Zblend   * (1 - LevelROC_Blend)
//     ADD_signal  = ADD_level_Zblend  * LevelROC_Blend
//                 + ADD_ROC_Zblend    * (1 - LevelROC_Blend)
//
//   Composite = VOLD_signal * VOLD_W + ADD_signal * ADD_W
//
// ROC note: ROC values can go negative even when the indicator is positive
// (rate of advance slowing). This is intentional — ROC captures momentum
// acceleration. The level blend restores directional context.
//
// Suppression: both MinBarsForShortZ and ROC_Lookback must be satisfied
// before the composite exits the suppression (gray) window.
//
// SETUP:
//   1. Copy to Sierra Chart ACS_Source. Build via Analysis >> Build DLL.
//   2. On your 1-min chart add two Symbol studies: $VOLD and $ADD.
//   3. Add this BCO study. Configure:
//        In[0] = VOLD Symbol study, subgraph 4 (Last)
//        In[1] = ADD  Symbol study, subgraph 4 (Last)
//   4. Overlay this chart on your range bar trading chart.
// =============================================================================

#include "sierrachart.h"
#include <cmath>

SCDLLName("BreadthCompositeOscillator")

// ---------------------------------------------------------------------------
// Rolling mean + sample std dev over arr[startIdx..endIdx].
// Skips bars where arr[i] == unusedVal. Returns false if < 2 valid bars.
// ---------------------------------------------------------------------------
static bool RollingStats(SCFloatArrayRef arr, int startIdx, int endIdx,
                          float unusedVal, float& outMean, float& outStdDev)
{
    double sum = 0.0, sumSq = 0.0;
    int count = 0;
    for (int i = startIdx; i <= endIdx; ++i)
    {
        float v = arr[i];
        if (v == unusedVal) continue;
        sum   += v;
        sumSq += static_cast<double>(v) * v;
        ++count;
    }
    if (count < 2) { outMean = 0.0f; outStdDev = 0.0f; return false; }
    outMean = static_cast<float>(sum / count);
    double var = sumSq / count - (sum / count) * (sum / count);
    outStdDev = sqrtf(static_cast<float>(var > 0.0 ? var : 0.0));
    return true;
}

// ---------------------------------------------------------------------------
// Std dev of arr[startIdx..endIdx] — no unused-skip (blend/signal arrays).
// ---------------------------------------------------------------------------
static float ArrayStdDev(SCFloatArrayRef arr, int startIdx, int endIdx, float floorVal)
{
    if (endIdx <= startIdx) return floorVal;
    double sum = 0.0, sumSq = 0.0;
    int count = endIdx - startIdx + 1;
    for (int i = startIdx; i <= endIdx; ++i)
    {
        double v = arr[i];
        sum   += v;
        sumSq += v * v;
    }
    double mean = sum / count;
    double var  = sumSq / count - mean * mean;
    float  sd   = sqrtf(static_cast<float>(var > 0.0 ? var : 0.0));
    return sd < floorVal ? floorVal : sd;
}

// =============================================================================
SCSFExport scsf_BreadthCompositeOscillator(SCStudyInterfaceRef sc)
{
    // -------------------------------------------------------------------------
    // SUBGRAPHS  (8 total)
    // -------------------------------------------------------------------------
    SCSubgraphRef sg_Composite    = sc.Subgraph[0]; // SG1 blended composite
    SCSubgraphRef sg_VoldSignal   = sc.Subgraph[1]; // SG2 VOLD_signal (level+ROC blend)
    SCSubgraphRef sg_AddSignal    = sc.Subgraph[2]; // SG3 ADD_signal  (level+ROC blend)
    SCSubgraphRef sg_ZeroLine     = sc.Subgraph[3]; // SG4 neutral reference
    SCSubgraphRef sg_UpperThresh  = sc.Subgraph[4]; // SG5 bull threshold
    SCSubgraphRef sg_LowerThresh  = sc.Subgraph[5]; // SG6 bear threshold
    SCSubgraphRef sg_UpperExtreme = sc.Subgraph[6]; // SG7 bull extreme
    SCSubgraphRef sg_LowerExtreme = sc.Subgraph[7]; // SG8 bear extreme

    // sg_Composite.Arrays[] — per-bar scratch:
    //   [0] raw VOLD level        (sc.Unused_1 outside RTH)
    //   [1] raw ADD  level        (sc.Unused_1 outside RTH)
    //   [2] VOLD_ROC              (sc.Unused_1 when not computable)
    //   [3] ADD_ROC               (sc.Unused_1 when not computable)
    //   [4] VOLD_signal           (0.0 outside RTH) — used for dynamic consistency
    //   [5] ADD_signal            (0.0 outside RTH)
    //
    // sg_ZeroLine.Arrays[] — persistent state:
    //   [0] sessionOpenBar  [1] prevBarDate  [2] barsAboveThresh
    //   [3] barsBelowThresh [4] colorState

    // -------------------------------------------------------------------------
    // INPUTS
    // -------------------------------------------------------------------------
    SCInputRef in_VoldData      = sc.Input[0];
    SCInputRef in_AddData       = sc.Input[1];
    SCInputRef in_LongZSess     = sc.Input[2];
    SCInputRef in_SessStart     = sc.Input[3];
    SCInputRef in_SessEnd       = sc.Input[4];
    SCInputRef in_ChartIntv     = sc.Input[5];
    SCInputRef in_WeightMode    = sc.Input[6];
    SCInputRef in_MinBarsShZ    = sc.Input[7];
    SCInputRef in_StdDevFloor   = sc.Input[8];
    SCInputRef in_ConsistLB     = sc.Input[9];
    SCInputRef in_DispMode      = sc.Input[10];
    SCInputRef in_ROCLookback   = sc.Input[11];
    SCInputRef in_LevelROCBlend = sc.Input[12]; // NEW v1.3

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Breadth Composite Oscillator";
        sc.StudyDescription = "v1.3 — VOLD+ADD via level+ROC dual Z-blend. "
                              "In[0]=VOLD Symbol study (sg4), In[1]=ADD Symbol study (sg4).";
        sc.AutoLoop         = 1;
        sc.GraphRegion      = 1;
        sc.FreeDLL          = 0;
        sc.DrawZeros        = 0;

        sg_Composite.Name               = "Composite";
        sg_Composite.DrawStyle          = DRAWSTYLE_LINE;
        sg_Composite.LineWidth          = 2;
        sg_Composite.PrimaryColor       = COLOR_GRAY;
        sg_Composite.SecondaryColor     = RGB(0, 200, 80);
        sg_Composite.SecondaryColorUsed = 1;

        sg_VoldSignal.Name         = "VOLD Signal";
        sg_VoldSignal.DrawStyle    = DRAWSTYLE_LINE;
        sg_VoldSignal.LineWidth    = 1;
        sg_VoldSignal.PrimaryColor = RGB(210, 140, 70);

        sg_AddSignal.Name         = "ADD Signal";
        sg_AddSignal.DrawStyle    = DRAWSTYLE_LINE;
        sg_AddSignal.LineWidth    = 1;
        sg_AddSignal.PrimaryColor = RGB(150, 100, 200);

        sg_ZeroLine.Name         = "Zero Line";
        sg_ZeroLine.DrawStyle    = DRAWSTYLE_LINE;
        sg_ZeroLine.LineStyle    = LINESTYLE_DASH;
        sg_ZeroLine.LineWidth    = 1;
        sg_ZeroLine.PrimaryColor = COLOR_GRAY;

        sg_UpperThresh.Name         = "Upper Threshold (+1 / 1.5)";
        sg_UpperThresh.DrawStyle    = DRAWSTYLE_LINE;
        sg_UpperThresh.LineStyle    = LINESTYLE_DOT;
        sg_UpperThresh.LineWidth    = 1;
        sg_UpperThresh.PrimaryColor = RGB(0, 180, 0);

        sg_LowerThresh.Name         = "Lower Threshold (-1 / 0.5)";
        sg_LowerThresh.DrawStyle    = DRAWSTYLE_LINE;
        sg_LowerThresh.LineStyle    = LINESTYLE_DOT;
        sg_LowerThresh.LineWidth    = 1;
        sg_LowerThresh.PrimaryColor = RGB(180, 0, 0);

        sg_UpperExtreme.Name         = "Upper Extreme (+2 / 2.0)";
        sg_UpperExtreme.DrawStyle    = DRAWSTYLE_LINE;
        sg_UpperExtreme.LineStyle    = LINESTYLE_DASH;
        sg_UpperExtreme.LineWidth    = 1;
        sg_UpperExtreme.PrimaryColor = RGB(120, 240, 120);

        sg_LowerExtreme.Name         = "Lower Extreme (-2 / 0.0)";
        sg_LowerExtreme.DrawStyle    = DRAWSTYLE_LINE;
        sg_LowerExtreme.LineStyle    = LINESTYLE_DASH;
        sg_LowerExtreme.LineWidth    = 1;
        sg_LowerExtreme.PrimaryColor = RGB(240, 120, 120);

        in_VoldData.Name = "VOLD Data Source (Symbol study, subgraph 4 = Last)";
        in_VoldData.SetStudySubgraphValues(0, 4);

        in_AddData.Name = "ADD Data Source (Symbol study, subgraph 4 = Last)";
        in_AddData.SetStudySubgraphValues(0, 4);

        in_LongZSess.Name = "Long Z Sessions";
        in_LongZSess.SetInt(5);
        in_LongZSess.SetIntLimits(1, 20);

        in_SessStart.Name = "Session Start Time";
        in_SessStart.SetTime(HMS_TIME(9, 30, 0));

        in_SessEnd.Name = "Session End Time";
        in_SessEnd.SetTime(HMS_TIME(16, 0, 0));

        in_ChartIntv.Name = "Chart Interval Minutes";
        in_ChartIntv.SetInt(1);
        in_ChartIntv.SetIntLimits(1, 60);

        in_WeightMode.Name = "Weight Mode";
        in_WeightMode.SetCustomInputStrings("FIXED;DYNAMIC");
        in_WeightMode.SetCustomInputIndex(1);

        in_MinBarsShZ.Name = "Min Bars for Short Z (suppression)";
        in_MinBarsShZ.SetInt(10);
        in_MinBarsShZ.SetIntLimits(1, 60);

        in_StdDevFloor.Name = "Std Dev Floor";
        in_StdDevFloor.SetFloat(0.5f);

        in_ConsistLB.Name = "Consistency Lookback (dynamic weights)";
        in_ConsistLB.SetInt(20);
        in_ConsistLB.SetIntLimits(2, 100);

        in_DispMode.Name = "Display Mode";
        in_DispMode.SetCustomInputStrings("ZSCORE;RATIO");
        in_DispMode.SetCustomInputIndex(0);

        in_ROCLookback.Name = "ROC Lookback (bars)";
        in_ROCLookback.SetInt(10);
        in_ROCLookback.SetIntLimits(1, 60);

        in_LevelROCBlend.Name = "Level vs ROC Blend (0.0=pure ROC, 1.0=pure Level, 0.5=equal)";
        in_LevelROCBlend.SetFloat(0.5f);
        in_LevelROCBlend.SetFloatLimits(0.0f, 1.0f);

        return;
    }

    // =========================================================================
    // PARAMETERS
    // =========================================================================
    const int   chartIntv       = in_ChartIntv.GetInt();
    const int   barsPerSession  = 390 / (chartIntv > 0 ? chartIntv : 1);
    const int   longZBars       = in_LongZSess.GetInt() * barsPerSession;
    const int   minBarsShZ      = in_MinBarsShZ.GetInt();
    const float sdFloor         = in_StdDevFloor.GetFloat();
    const int   consistLB       = in_ConsistLB.GetInt();
    const bool  isDynamic       = (in_WeightMode.GetIndex() == 1);
    const bool  isRatio         = (in_DispMode.GetIndex() == 1);
    const int   rocLookback     = in_ROCLookback.GetInt();
    const float levelROCBlend   = in_LevelROCBlend.GetFloat(); // weight for level stream
    const float rocWeight       = 1.0f - levelROCBlend;        // weight for ROC  stream

    const int sessStartSec = in_SessStart.GetTime();
    const int sessEndSec   = in_SessEnd.GetTime();

    // =========================================================================
    // PERSISTENT STATE via sg_ZeroLine.Arrays[0..4]
    // =========================================================================
    int sessionOpenBar, prevBarDate, barsAboveThresh, barsBelowThresh, colorState;

    if (sc.Index == 0)
    {
        sessionOpenBar  = 0;
        prevBarDate     = 0;
        barsAboveThresh = 0;
        barsBelowThresh = 0;
        colorState      = 0;
    }
    else
    {
        const int prev  = sc.Index - 1;
        sessionOpenBar  = static_cast<int>(sg_ZeroLine.Arrays[0][prev]);
        prevBarDate     = static_cast<int>(sg_ZeroLine.Arrays[1][prev]);
        barsAboveThresh = static_cast<int>(sg_ZeroLine.Arrays[2][prev]);
        barsBelowThresh = static_cast<int>(sg_ZeroLine.Arrays[3][prev]);
        colorState      = static_cast<int>(sg_ZeroLine.Arrays[4][prev]);
    }

    auto SaveState = [&]()
    {
        sg_ZeroLine.Arrays[0][sc.Index] = static_cast<float>(sessionOpenBar);
        sg_ZeroLine.Arrays[1][sc.Index] = static_cast<float>(prevBarDate);
        sg_ZeroLine.Arrays[2][sc.Index] = static_cast<float>(barsAboveThresh);
        sg_ZeroLine.Arrays[3][sc.Index] = static_cast<float>(barsBelowThresh);
        sg_ZeroLine.Arrays[4][sc.Index] = static_cast<float>(colorState);
    };

    // =========================================================================
    // TIME CHECKS
    // =========================================================================
    SCDateTime barDT      = sc.BaseDateTimeIn[sc.Index];
    int        barDate    = barDT.GetDate();
    int        barTimeSec = barDT.GetTimeInSeconds();

    const bool inRTH = (barTimeSec >= sessStartSec && barTimeSec <= sessEndSec);

    const float refNeutral    = isRatio ?  1.0f :  0.0f;
    const float refBullThresh = isRatio ?  1.5f :  1.0f;
    const float refBearThresh = isRatio ?  0.5f : -1.0f;
    const float refBullEx     = isRatio ?  2.0f :  2.0f;
    const float refBearEx     = isRatio ?  0.0f : -2.0f;
    const float neutral       = isRatio ?  1.0f :  0.0f; // neutral signal value

    // -------------------------------------------------------------------------
    // Outside RTH
    // -------------------------------------------------------------------------
    if (!inRTH)
    {
        sg_Composite[sc.Index]    = sc.Unused_1;
        sg_VoldSignal[sc.Index]   = sc.Unused_1;
        sg_AddSignal[sc.Index]    = sc.Unused_1;
        sg_ZeroLine[sc.Index]     = sc.Unused_1;
        sg_UpperThresh[sc.Index]  = sc.Unused_1;
        sg_LowerThresh[sc.Index]  = sc.Unused_1;
        sg_UpperExtreme[sc.Index] = sc.Unused_1;
        sg_LowerExtreme[sc.Index] = sc.Unused_1;
        sg_Composite.Arrays[0][sc.Index] = sc.Unused_1;
        sg_Composite.Arrays[1][sc.Index] = sc.Unused_1;
        sg_Composite.Arrays[2][sc.Index] = sc.Unused_1;
        sg_Composite.Arrays[3][sc.Index] = sc.Unused_1;
        sg_Composite.Arrays[4][sc.Index] = 0.0f;
        sg_Composite.Arrays[5][sc.Index] = 0.0f;
        SaveState();
        return;
    }

    // =========================================================================
    // SESSION OPEN DETECTION
    // =========================================================================
    if (barDate != prevBarDate)
    {
        sessionOpenBar  = sc.Index;
        prevBarDate     = barDate;
        barsAboveThresh = 0;
        barsBelowThresh = 0;
        colorState      = 0;
    }

    const int barsSinceOpen = sc.Index - sessionOpenBar;

    // =========================================================================
    // FETCH EXTERNAL DATA  (Symbol studies always on this chart)
    // =========================================================================
    const bool inputsReady = (in_VoldData.GetStudyID() > 0 &&
                               in_AddData.GetStudyID()  > 0);

    if (!inputsReady)
    {
        sg_Composite[sc.Index]    = refNeutral;
        sg_VoldSignal[sc.Index]   = refNeutral;
        sg_AddSignal[sc.Index]    = refNeutral;
        sg_ZeroLine[sc.Index]     = refNeutral;
        sg_UpperThresh[sc.Index]  = refBullThresh;
        sg_LowerThresh[sc.Index]  = refBearThresh;
        sg_UpperExtreme[sc.Index] = refBullEx;
        sg_LowerExtreme[sc.Index] = refBearEx;
        for (int k = 0; k < 6; ++k) sg_Composite.Arrays[k][sc.Index] = 0.0f;
        SaveState();
        return;
    }

    SCFloatArray voldArr, addArr;
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, in_VoldData.GetStudyID(),
                                      in_VoldData.GetSubgraphIndex(), voldArr);
    sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, in_AddData.GetStudyID(),
                                      in_AddData.GetSubgraphIndex(), addArr);

    const float voldVal = voldArr[sc.Index];
    const float addVal  = addArr[sc.Index];

    // Store raw levels for level-Z normalization and ROC lookback
    sg_Composite.Arrays[0][sc.Index] = voldVal;
    sg_Composite.Arrays[1][sc.Index] = addVal;

    // =========================================================================
    // ROC COMPUTATION
    // Not computable if the lookback bar is outside this RTH session.
    // =========================================================================
    bool rocReady = false;
    float voldROC = 0.0f, addROC = 0.0f;

    if (sc.Index >= rocLookback)
    {
        const float voldPrev = sg_Composite.Arrays[0][sc.Index - rocLookback];
        const float addPrev  = sg_Composite.Arrays[1][sc.Index - rocLookback];

        if (voldPrev != sc.Unused_1 && addPrev != sc.Unused_1)
        {
            voldROC  = voldVal - voldPrev;
            addROC   = addVal  - addPrev;
            rocReady = true;
        }
    }

    sg_Composite.Arrays[2][sc.Index] = rocReady ? voldROC : sc.Unused_1;
    sg_Composite.Arrays[3][sc.Index] = rocReady ? addROC  : sc.Unused_1;

    // Suppression flags
    const bool levelSuppressed = (barsSinceOpen < minBarsShZ);
    const bool rocSuppressed   = levelSuppressed || !rocReady;
    // Combined: suppress color until both level and ROC are reliable
    const bool inSuppression   = levelSuppressed || !rocReady;

    // =========================================================================
    // NORMALIZATION
    // Dual Z-score / Ratio pipeline. suppress=true forces Long Z only (no Short Z).
    // =========================================================================
    auto Normalize = [&](SCFloatArrayRef rawArr, float curVal, bool suppress) -> float
    {
        const int longStart = (sc.Index >= longZBars) ? (sc.Index - longZBars + 1) : 0;

        float longMean = 0.0f, longSd = 0.0f;
        bool  longValid = RollingStats(rawArr, longStart, sc.Index,
                                        sc.Unused_1, longMean, longSd);
        longSd = (longSd < sdFloor) ? sdFloor : longSd;

        float longNorm;
        if (!longValid)
            longNorm = isRatio ? 1.0f : 0.0f;
        else if (isRatio)
            longNorm = curVal / (fabsf(longMean) < sdFloor ? sdFloor : fabsf(longMean));
        else
            longNorm = (curVal - longMean) / longSd;

        if (suppress) return longNorm;

        float shortMean = 0.0f, shortSd = 0.0f;
        bool  shortValid = RollingStats(rawArr, sessionOpenBar, sc.Index,
                                         sc.Unused_1, shortMean, shortSd);
        shortSd = (shortSd < sdFloor) ? sdFloor : shortSd;

        float shortNorm;
        if (!shortValid)
            shortNorm = isRatio ? 1.0f : 0.0f;
        else if (isRatio)
            shortNorm = curVal / (fabsf(shortMean) < sdFloor ? sdFloor : fabsf(shortMean));
        else
            shortNorm = (curVal - shortMean) / shortSd;

        return shortNorm * 0.6f + longNorm * 0.4f;
    };

    // =========================================================================
    // FOUR NORMALIZED STREAMS
    //   Level streams use raw arrays [0],[1] and level suppression
    //   ROC   streams use ROC arrays [2],[3] and ROC   suppression
    // =========================================================================
    const float voldLevelZ = Normalize(sg_Composite.Arrays[0], voldVal, levelSuppressed);
    const float addLevelZ  = Normalize(sg_Composite.Arrays[1], addVal,  levelSuppressed);

    float voldROCZ = neutral;
    float addROCZ  = neutral;
    if (rocReady)
    {
        voldROCZ = Normalize(sg_Composite.Arrays[2], voldROC, rocSuppressed);
        addROCZ  = Normalize(sg_Composite.Arrays[3], addROC,  rocSuppressed);
    }

    // =========================================================================
    // PER-INDICATOR SIGNAL  (level + ROC blend)
    // When ROC is not yet available, fall back to level only.
    // =========================================================================
    float voldSignal, addSignal;
    if (rocReady)
    {
        voldSignal = voldLevelZ * levelROCBlend + voldROCZ * rocWeight;
        addSignal  = addLevelZ  * levelROCBlend + addROCZ  * rocWeight;
    }
    else
    {
        voldSignal = voldLevelZ;
        addSignal  = addLevelZ;
    }

    // Store final signals for dynamic consistency computation
    sg_Composite.Arrays[4][sc.Index] = voldSignal;
    sg_Composite.Arrays[5][sc.Index] = addSignal;

    // =========================================================================
    // WEIGHTS
    // DYNAMIC: consistency on final per-indicator signal (post level+ROC blend)
    // =========================================================================
    float voldW, addW;
    if (!isDynamic)
    {
        voldW = 0.5f;
        addW  = 0.5f;
    }
    else
    {
        const int cbStart = (sc.Index >= consistLB) ? (sc.Index - consistLB + 1) : 0;
        float voldSd = ArrayStdDev(sg_Composite.Arrays[4], cbStart, sc.Index, sdFloor);
        float addSd  = ArrayStdDev(sg_Composite.Arrays[5], cbStart, sc.Index, sdFloor);

        float voldC = 1.0f / voldSd;
        float addC  = 1.0f / addSd;
        float total = voldC + addC;

        if (total > 0.0f) {
            voldW = voldC / total;
            addW  = addC  / total;
        } else {
            voldW = addW = 0.5f;
        }
    }

    // =========================================================================
    // COMPOSITE
    // =========================================================================
    const float composite = voldSignal * voldW + addSignal * addW;

    // =========================================================================
    // COLOR LOGIC — 2-bar hold (R5/R6)
    // =========================================================================
    COLORREF compositeColor;
    if (inSuppression)
    {
        compositeColor  = COLOR_GRAY;
        barsAboveThresh = 0;
        barsBelowThresh = 0;
        colorState      = 0;
    }
    else
    {
        if (composite > refBullThresh) ++barsAboveThresh; else barsAboveThresh = 0;
        if (composite < refBearThresh) ++barsBelowThresh; else barsBelowThresh = 0;

        if      (barsAboveThresh >= 2) colorState =  1;
        else if (barsBelowThresh >= 2) colorState = -1;
        else if (barsAboveThresh == 0 && barsBelowThresh == 0) colorState = 0;

        if      (colorState ==  1) compositeColor = RGB(0, 200, 80);
        else if (colorState == -1) compositeColor = RGB(220, 50, 50);
        else                       compositeColor = COLOR_GRAY;
    }

    // =========================================================================
    // WRITE OUTPUTS
    // =========================================================================
    sg_Composite[sc.Index]           = composite;
    sg_Composite.DataColor[sc.Index] = compositeColor;
    sg_VoldSignal[sc.Index]          = voldSignal;
    sg_AddSignal[sc.Index]           = addSignal;
    sg_ZeroLine[sc.Index]            = refNeutral;
    sg_UpperThresh[sc.Index]         = refBullThresh;
    sg_LowerThresh[sc.Index]         = refBearThresh;
    sg_UpperExtreme[sc.Index]        = refBullEx;
    sg_LowerExtreme[sc.Index]        = refBearEx;

    SaveState();
}
