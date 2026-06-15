// TapeReader.cpp
// =============================================================================
// Realized-Aggression "Tape Flow" reader with a speed-gated urgency highlight.
//
// SG0  Tape Flow        Histogram of realized aggression (delta flow),
//                       auto-colored per bar by sign (buy vs sell).
// SG1  Urgency Highlight Semi-transparent per-bar background fill, drawn only on
//                       bars whose formation speed crosses the urgency gate.
//                       Tint direction follows that bar's raw delta sign.
// SG2  Speed            Raw bar speed line, hidden by default so the threshold
//                       can be tuned.
//
// Standalone study living in its own subgraph region (not the price graph).
// AutoLoop = 1. Computation is stateless per bar, so full-recalc needs no
// running-state reset.
//
// Build notes (defensive, from confirmed build-server patterns):
//   - No std::min/max/fabs; SC macros only. No <algorithm>.
//   - SCALE_AUTO (not SCALE_AUTOMATIC).
//   - Bid/Ask aggressive volume read from sc.BaseData[SC_ASKVOL]/[SC_BIDVOL].
// =============================================================================

#include "sierrachart.h"

SCDLLName("TapeReader")

// ---- Subgraph indices --------------------------------------------------------
const int SG_FLOW    = 0;   // Tape Flow histogram
const int SG_URGENT  = 1;   // Urgency background highlight
const int SG_SPEED   = 2;   // Raw speed line (hidden by default)

// ---- Input indices -----------------------------------------------------------
const int IN_FLOW_MODE   = 0;   // 0 = SMA of rawDelta, 1 = rolling sum
const int IN_SMOOTH_PER  = 1;   // Bars for flow SMA / rolling sum
const int IN_SPEED_LEN   = 2;   // Lookback for speed mean & std
const int IN_K           = 3;   // Speed std-dev multiplier
const int IN_MIN_DUR     = 4;   // Min bar duration (s) divide-by-zero guard

SCSFExport scsf_TapeReader(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName       = "Tape Reader (Flow + Urgency)";
        sc.GraphRegion     = 1;          // its own region, below the price graph
        sc.AutoLoop        = 1;          // per-bar evaluation
        sc.ScaleRangeType  = SCALE_AUTO;
        sc.DrawZeros       = 0;          // a zero value draws nothing

        // SG0 -- Tape Flow histogram ------------------------------------------
        sc.Subgraph[SG_FLOW].Name         = "Tape Flow";
        sc.Subgraph[SG_FLOW].DrawStyle    = DRAWSTYLE_BAR;
        sc.Subgraph[SG_FLOW].LineWidth    = 3;
        sc.Subgraph[SG_FLOW].PrimaryColor = RGB(40, 130, 230);   // net buying
        sc.Subgraph[SG_FLOW].DrawZeros    = 0;

        // SG1 -- Urgency Highlight (background fill) ---------------------------
        sc.Subgraph[SG_URGENT].Name         = "Urgency Highlight";
        sc.Subgraph[SG_URGENT].DrawStyle    = DRAWSTYLE_BACKGROUND;
        sc.Subgraph[SG_URGENT].PrimaryColor = RGB(40, 130, 230);
        sc.Subgraph[SG_URGENT].DrawZeros    = 0;  // only urgent bars draw

        // SG2 -- Speed line (hidden by default) -------------------------------
        sc.Subgraph[SG_SPEED].Name         = "Speed";
        sc.Subgraph[SG_SPEED].DrawStyle    = DRAWSTYLE_IGNORE;   // turn on to tune
        sc.Subgraph[SG_SPEED].LineWidth    = 1;
        sc.Subgraph[SG_SPEED].PrimaryColor = RGB(180, 180, 180);
        sc.Subgraph[SG_SPEED].DrawZeros    = 0;

        // ---- Inputs ----------------------------------------------------------
        sc.Input[IN_FLOW_MODE].Name = "Flow Mode (0=SMA rate, 1=Rolling Sum)";
        sc.Input[IN_FLOW_MODE].SetInt(0);
        sc.Input[IN_FLOW_MODE].SetIntLimits(0, 1);

        sc.Input[IN_SMOOTH_PER].Name = "Smoothing Period";
        sc.Input[IN_SMOOTH_PER].SetInt(5);
        sc.Input[IN_SMOOTH_PER].SetIntLimits(1, 10000);

        sc.Input[IN_SPEED_LEN].Name = "Speed Length";
        sc.Input[IN_SPEED_LEN].SetInt(50);
        sc.Input[IN_SPEED_LEN].SetIntLimits(2, 10000);

        sc.Input[IN_K].Name = "Speed StdDev Mult (K)";
        sc.Input[IN_K].SetFloat(2.0f);

        sc.Input[IN_MIN_DUR].Name = "Min Bar Duration (s)";
        sc.Input[IN_MIN_DUR].SetFloat(0.05f);

        return;
    }

    // ---- Read inputs ---------------------------------------------------------
    const int   flowMode  = sc.Input[IN_FLOW_MODE].GetInt();
    const int   smoothPer = max(1, sc.Input[IN_SMOOTH_PER].GetInt());
    const int   speedLen  = max(2, sc.Input[IN_SPEED_LEN].GetInt());
    const float K         = sc.Input[IN_K].GetFloat();
    double      minDur    = sc.Input[IN_MIN_DUR].GetFloat();
    if (minDur < 1e-6) minDur = 1e-6;   // hard floor against zero/negative input

    const int i = sc.Index;

    // -------------------------------------------------------------------------
    // 1) Per-bar realized aggression delta.
    //    SC_ASKVOL = volume traded at the ask  -> aggressive BUYING.
    //    SC_BIDVOL = volume traded at the bid  -> aggressive SELLING.
    // -------------------------------------------------------------------------
    const double askVol   = sc.BaseData[SC_ASKVOL][i];
    const double bidVol   = sc.BaseData[SC_BIDVOL][i];
    const double rawDelta = askVol - bidVol;     // >0 net buying, <0 net selling
    const bool   noFlowData = (askVol + bidVol) <= 0.0;

    // -------------------------------------------------------------------------
    // 2) Flow value (SG0): SMA of rawDelta, or rolling sum, over Smoothing Period.
    //    Both computed with a trailing window so there is no running state to
    //    reset on full recalculation.
    // -------------------------------------------------------------------------
    double flow = 0.0;
    if (i >= smoothPer - 1)
    {
        double sum = 0.0;
        for (int j = i - smoothPer + 1; j <= i; ++j)
        {
            const double a = sc.BaseData[SC_ASKVOL][j];
            const double b = sc.BaseData[SC_BIDVOL][j];
            if ((a + b) > 0.0)          // bars with no order-flow contribute 0
                sum += (a - b);
        }
        flow = (flowMode == 1) ? sum : (sum / smoothPer);
    }
    if (noFlowData && i < smoothPer - 1)
        flow = 0.0;

    sc.Subgraph[SG_FLOW][i] = (float)flow;
    sc.Subgraph[SG_FLOW].DataColor[i] =
        (flow >= 0.0) ? RGB(40, 130, 230)   // net buying  -> blue
                      : RGB(220, 60, 60);    // net selling -> red

    // -------------------------------------------------------------------------
    // 3) Bar speed = bar volume / formation time (seconds).
    //    SCDateTime stores days as a double with sub-second precision, so
    //    (t[i] - t[i-1]) * 86400 yields elapsed seconds. First bar has no prior
    //    timestamp, so its speed is undefined and it is skipped.
    // -------------------------------------------------------------------------
    double speed = 0.0;
    if (i >= 1)
    {
        double durSec = (sc.BaseDateTimeIn[i].GetAsDouble()
                       - sc.BaseDateTimeIn[i - 1].GetAsDouble()) * 86400.0;
        if (durSec < minDur) durSec = minDur;            // divide-by-zero guard
        speed = sc.BaseData[SC_VOLUME][i] / durSec;
    }
    sc.Subgraph[SG_SPEED][i] = (float)speed;

    // -------------------------------------------------------------------------
    // 4) Urgency gate (SG1). Over the trailing Speed Length bars compute the
    //    mean and standard deviation of speed; threshold = mean + K*std.
    //    One-sided: only unusually FAST bars qualify. No highlight until at
    //    least Speed Length valid speed samples exist.
    // -------------------------------------------------------------------------
    float urgentVal = 0.0f;        // 0 -> DrawZeros=0 suppresses the fill

    // Need speedLen samples, and a valid speed requires i>=1, so first usable
    // window starts at index speedLen.
    if (i >= speedLen)
    {
        double sum = 0.0, sumSq = 0.0;
        for (int j = i - speedLen + 1; j <= i; ++j)
        {
            double dur = (sc.BaseDateTimeIn[j].GetAsDouble()
                        - sc.BaseDateTimeIn[j - 1].GetAsDouble()) * 86400.0;
            if (dur < minDur) dur = minDur;
            const double s = sc.BaseData[SC_VOLUME][j] / dur;
            sum   += s;
            sumSq += s * s;
        }
        const double n    = (double)speedLen;
        const double mean = sum / n;
        double var = (sumSq / n) - (mean * mean);
        if (var < 0.0) var = 0.0;                 // guard FP rounding
        const double sd  = sqrt(var);
        const double threshold = mean + (double)K * sd;

        if (speed > threshold)
        {
            urgentVal = 1.0f;     // any non-zero value triggers the background
            // Directional tint by THIS bar's raw delta sign.
            sc.Subgraph[SG_URGENT].DataColor[i] =
                (rawDelta >= 0.0) ? RGB(60, 200, 255)   // buy  -> cyan/blue tint
                                  : RGB(255, 140, 40);   // sell -> orange/red tint
        }
    }

    sc.Subgraph[SG_URGENT][i] = urgentVal;   // 0 on non-urgent bars -> no fill
}
