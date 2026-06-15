// AbsorptionGradient.cpp
// Sierra Chart ACSIL custom study.
// Effort vs. result analysis across a rolling window: detects exhaustion
// (rising absorption) and trapped-trader conditions. All outputs normalized
// to a common -100/+100 scale.
//
// Build notes (defensive fixes carried over from prior SC builds):
//   * No std::min / std::max / std::fabs / std::round, and no <algorithm>.
//     scstructures.h defines min/max as C macros; use the bare SC macros.
//   * sc.PersistVars does NOT exist. This study avoids persistent float arrays
//     entirely: with AutoLoop=1 it recomputes the window directly from base
//     data each bar, and stores per-bar raw SG0 in a hidden subgraph (SG4)
//     for the slope lookback.
//   * sc.SASF_NO_VALUE does NOT exist. Markers/values that should not plot are
//     left at 0 with DrawZeros = 0, which suppresses drawing on that bar.
//   * Delta is computed as Ask volume - Bid volume from base data, which is
//     robust and gracefully yields 0 when order-flow data is unavailable.

#include "sierrachart.h"

SCDLLName("AbsorptionGradient")

// ---- Price change modes ----
const int PCM_HIGH_LOW = 0;
const int PCM_CLOSE_TO_CLOSE = 1;
const int PCM_AUTO = 2;

// ---- Persistent int slots ----
const int PS_RESOLVED_MODE = 2; // resolved auto mode (0 = HL, 1 = CtC)
const int PS_MODE_RESOLVED = 3; // 0 = not yet resolved, 1 = resolved

// Net delta for a bar = Ask volume - Bid volume. Returns 0 if no order flow.
static float AG_Delta(SCStudyInterfaceRef sc, int i)
{
    return sc.BaseData[SC_ASKVOL][i] - sc.BaseData[SC_BIDVOL][i];
}

// Price change for a bar, expressed in ticks, floored at MinTicks.
static float AG_PriceChangeTicks(SCStudyInterfaceRef sc, int i, int mode,
                                 float tickSize, int minTicks)
{
    float pc;
    if (mode == PCM_HIGH_LOW)
        pc = sc.High[i] - sc.Low[i];
    else // CLOSE_TO_CLOSE
        pc = (i >= 1) ? fabs(sc.Close[i] - sc.Close[i - 1]) : 0.0f;

    if (tickSize <= 0.0f)
        tickSize = 0.25f;

    float ticks = pc / tickSize;
    float floorTicks = (float)minTicks;
    if (ticks < floorTicks)
        ticks = floorTicks;
    return ticks;
}

SCSFExport scsf_AbsorptionGradient(SCStudyInterfaceRef sc)
{
    SCSubgraphRef SG_Absorption = sc.Subgraph[0];
    SCSubgraphRef SG_Efficiency = sc.Subgraph[1];
    SCSubgraphRef SG_Slope      = sc.Subgraph[2];
    SCSubgraphRef SG_Trapped    = sc.Subgraph[3];
    SCSubgraphRef SG_RawAbs      = sc.Subgraph[4]; // hidden: raw SG0 history

    SCInputRef In_WindowLen   = sc.Input[0];
    SCInputRef In_PriceMode   = sc.Input[1];
    SCInputRef In_MinTicks    = sc.Input[2];
    SCInputRef In_TickSize    = sc.Input[3];
    SCInputRef In_SlopeLB     = sc.Input[4];
    SCInputRef In_TrapThresh  = sc.Input[5];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Absorption Gradient";
        sc.StudyDescription =
            "Effort/result analysis across a rolling window. Detects exhaustion "
            "(rising absorption) and trapped-trader conditions. Outputs normalized "
            "to a -100/+100 scale. Works across time, range, volume, tick and Renko bars.";
        sc.AutoLoop = 1;
        sc.GraphRegion = 1; // own subgraph panel

        sc.ScaleRangeType = SCALE_USERDEFINED;
        sc.ScaleRangeTop = 100.0f;
        sc.ScaleRangeBottom = -100.0f;

        // SG0 - Absorption Gradient
        SG_Absorption.Name = "Absorption Gradient";
        SG_Absorption.DrawStyle = DRAWSTYLE_LINE;
        SG_Absorption.PrimaryColor = RGB(255, 255, 0); // yellow
        SG_Absorption.LineWidth = 2;
        SG_Absorption.DrawZeros = 0;

        // SG1 - Efficiency Ratio
        SG_Efficiency.Name = "Efficiency Ratio";
        SG_Efficiency.DrawStyle = DRAWSTYLE_LINE;
        SG_Efficiency.PrimaryColor = RGB(70, 130, 180); // steel blue
        SG_Efficiency.LineWidth = 1;
        SG_Efficiency.DrawZeros = 0;

        // SG2 - Gradient Slope (histogram, colored per bar by sign)
        SG_Slope.Name = "Gradient Slope";
        SG_Slope.DrawStyle = DRAWSTYLE_BAR;
        SG_Slope.PrimaryColor = RGB(0, 200, 80);    // green = positive slope
        SG_Slope.SecondaryColor = RGB(220, 50, 50); // red   = negative slope
        SG_Slope.SecondaryColorUsed = 1;
        SG_Slope.LineWidth = 3;
        SG_Slope.DrawZeros = 0;

        // SG3 - Trapped Bars (fixed markers at +90 / -90)
        SG_Trapped.Name = "Trapped Bars";
        SG_Trapped.DrawStyle = DRAWSTYLE_POINT;
        SG_Trapped.PrimaryColor = RGB(0, 200, 220); // cyan (trapped shorts, +90)
        SG_Trapped.SecondaryColor = RGB(255, 140, 0); // orange (trapped longs, -90)
        SG_Trapped.SecondaryColorUsed = 1;
        SG_Trapped.LineWidth = 4; // point size
        SG_Trapped.DrawZeros = 0;

        // SG4 - hidden raw absorption history (for slope lookback)
        SG_RawAbs.Name = "SG0 Raw (internal)";
        SG_RawAbs.DrawStyle = DRAWSTYLE_IGNORE;
        SG_RawAbs.DrawZeros = 0;

        In_WindowLen.Name = "Window Length";
        In_WindowLen.SetInt(15);
        In_WindowLen.SetIntLimits(2, 5000);

        In_PriceMode.Name = "Price Change Mode (0=HighLow, 1=CloseToClose, 2=Auto)";
        In_PriceMode.SetInt(PCM_CLOSE_TO_CLOSE);
        In_PriceMode.SetIntLimits(0, 2);

        In_MinTicks.Name = "Min Price Change Ticks";
        In_MinTicks.SetInt(1);
        In_MinTicks.SetIntLimits(1, 1000);

        In_TickSize.Name = "Tick Size";
        In_TickSize.SetFloat(0.25f);

        In_SlopeLB.Name = "Gradient Slope Lookback";
        In_SlopeLB.SetInt(4);
        In_SlopeLB.SetIntLimits(1, 1000);

        In_TrapThresh.Name = "Trapped Delta Threshold";
        In_TrapThresh.SetFloat(0.30f);

        return;
    }

    // -------- Read inputs --------
    int windowLen  = In_WindowLen.GetInt();
    int inMode     = In_PriceMode.GetInt();
    int minTicks   = In_MinTicks.GetInt();
    float tickSize = In_TickSize.GetFloat();
    int slopeLB    = In_SlopeLB.GetInt();
    float trapThresh = In_TrapThresh.GetFloat();

    if (windowLen < 2) windowLen = 2;
    if (slopeLB < 1) slopeLB = 1;

    int minBars = max(windowLen, slopeLB + 1);

    // -------- Resolve price-change mode --------
    int& resolvedMode = sc.GetPersistentInt(PS_RESOLVED_MODE);
    int& modeResolved = sc.GetPersistentInt(PS_MODE_RESOLVED);

    if (sc.Index == 0)
    {
        // Reset resolution state at the start of a (re)calculation.
        modeResolved = 0;
        resolvedMode = PCM_CLOSE_TO_CLOSE;
    }

    int effMode;
    if (inMode != PCM_AUTO)
    {
        effMode = inMode; // 0 or 1
    }
    else
    {
        if (!modeResolved && sc.Index >= 10)
        {
            // Auto-detect: if the High-Low range is constant across the last 5
            // bars, the chart is likely a Range bar type -> use Close-to-Close.
            bool constantRange = true;
            float r0 = sc.High[sc.Index] - sc.Low[sc.Index];
            for (int k = 1; k < 5; ++k)
            {
                float rk = sc.High[sc.Index - k] - sc.Low[sc.Index - k];
                if (fabs(rk - r0) > 1e-6f)
                {
                    constantRange = false;
                    break;
                }
            }
            resolvedMode = constantRange ? PCM_CLOSE_TO_CLOSE : PCM_HIGH_LOW;
            modeResolved = 1;
        }
        // Before resolution, default to Close-to-Close.
        effMode = modeResolved ? resolvedMode : PCM_CLOSE_TO_CLOSE;
    }

    // -------- Warmup: leave outputs at 0 (hidden via DrawZeros) --------
    if (sc.Index < minBars || (inMode == PCM_AUTO && sc.Index < 10))
    {
        SG_Absorption[sc.Index] = 0.0f;
        SG_Efficiency[sc.Index] = 0.0f;
        SG_Slope[sc.Index] = 0.0f;
        SG_Trapped[sc.Index] = 0.0f;
        SG_RawAbs[sc.Index] = 0.0f;
        return;
    }

    // -------- Window aggregates (recomputed directly from base data) --------
    int start = sc.Index - windowLen + 1;
    if (start < 0) start = 0;
    int n = sc.Index - start + 1;

    float sumAbsRatio = 0.0f;
    float sumEffRatio = 0.0f;
    float sumAbsDelta = 0.0f;
    float minAbs = 1e30f, maxAbs = -1e30f;
    float minEff = 1e30f, maxEff = -1e30f;

    for (int j = start; j <= sc.Index; ++j)
    {
        float d = AG_Delta(sc, j);
        float pct = AG_PriceChangeTicks(sc, j, effMode, tickSize, minTicks);

        float absRatio = fabs(d) / pct;                 // absorption: effort / result
        float effRatio = pct / max(fabs(d), 1.0f);      // efficiency: result / effort

        sumAbsRatio += absRatio;
        sumEffRatio += effRatio;
        sumAbsDelta += fabs(d);

        if (absRatio < minAbs) minAbs = absRatio;
        if (absRatio > maxAbs) maxAbs = absRatio;
        if (effRatio < minEff) minEff = effRatio;
        if (effRatio > maxEff) maxEff = effRatio;
    }

    float meanAbs = sumAbsRatio / n;
    float meanEff = sumEffRatio / n;
    float windowAvgAbsDelta = sumAbsDelta / n;

    // Store raw absorption mean for slope lookback (hidden subgraph).
    SG_RawAbs[sc.Index] = meanAbs;

    // -------- Normalize SG0 (absorption) to [0, 100] --------
    float sg0;
    if (maxAbs - minAbs < 1e-6f)
        sg0 = 50.0f;
    else
        sg0 = ((meanAbs - minAbs) / (maxAbs - minAbs)) * 100.0f;
    if (sg0 < 0.0f) sg0 = 0.0f;
    if (sg0 > 100.0f) sg0 = 100.0f;
    SG_Absorption[sc.Index] = sg0;

    // -------- Normalize SG1 (efficiency) to [0, 100] --------
    float sg1;
    if (maxEff - minEff < 1e-6f)
        sg1 = 50.0f;
    else
        sg1 = ((meanEff - minEff) / (maxEff - minEff)) * 100.0f;
    if (sg1 < 0.0f) sg1 = 0.0f;
    if (sg1 > 100.0f) sg1 = 100.0f;
    SG_Efficiency[sc.Index] = sg1;

    // -------- SG2 (gradient slope) normalized to [-100, 100] --------
    // Requires the raw-absorption value from `slopeLB` bars ago to be valid,
    // i.e. that bar must itself be past warmup.
    if (sc.Index - slopeLB >= minBars)
    {
        float rawSlope = SG_RawAbs[sc.Index] - SG_RawAbs[sc.Index - slopeLB];

        // Rolling min/max of slope across the window (only valid bars).
        float minSl = 1e30f, maxSl = -1e30f;
        for (int j = start; j <= sc.Index; ++j)
        {
            if (j - slopeLB >= minBars)
            {
                float s = SG_RawAbs[j] - SG_RawAbs[j - slopeLB];
                if (s < minSl) minSl = s;
                if (s > maxSl) maxSl = s;
            }
        }

        float sg2;
        if (maxSl <= -1e29f || maxSl - minSl < 1e-6f)
            sg2 = 0.0f; // flat -> midpoint of [-100, 100]
        else
            sg2 = ((rawSlope - minSl) / (maxSl - minSl)) * 200.0f - 100.0f;
        if (sg2 < -100.0f) sg2 = -100.0f;
        if (sg2 > 100.0f) sg2 = 100.0f;

        SG_Slope[sc.Index] = sg2;
        SG_Slope.DataColor[sc.Index] =
            (rawSlope >= 0.0f) ? SG_Slope.PrimaryColor : SG_Slope.SecondaryColor;
    }
    else
    {
        SG_Slope[sc.Index] = 0.0f;
    }

    // -------- SG3 (trapped bars) --------
    int trappedType = 0; // +1 = trapped shorts (bullish, +90), -1 = trapped longs (bearish, -90)
    if (sc.Index >= 1)
    {
        float d = AG_Delta(sc, sc.Index);
        float thr = trapThresh * windowAvgAbsDelta;
        bool closeUp = sc.Close[sc.Index] > sc.Close[sc.Index - 1];
        bool closeDn = sc.Close[sc.Index] < sc.Close[sc.Index - 1];

        if (closeDn && d > 0.0f && d > thr)
            trappedType = -1;      // price down but net buying -> trapped longs
        else if (closeUp && d < 0.0f && fabs(d) > thr)
            trappedType = +1;      // price up but net selling -> trapped shorts
    }

    if (trappedType == +1)
    {
        SG_Trapped[sc.Index] = 90.0f;
        SG_Trapped.DataColor[sc.Index] = SG_Trapped.PrimaryColor;   // cyan
    }
    else if (trappedType == -1)
    {
        SG_Trapped[sc.Index] = -90.0f;
        SG_Trapped.DataColor[sc.Index] = SG_Trapped.SecondaryColor; // orange
    }
    else
    {
        SG_Trapped[sc.Index] = 0.0f; // hidden by DrawZeros = 0
    }
}
