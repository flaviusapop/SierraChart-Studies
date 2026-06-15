// DeltaVelocityProfile.cpp
// Sierra Chart ACSIL custom study.
//
// Exponential-decay-weighted delta momentum with three stacked layers:
//   SG0 Weighted Delta  - EXP-weighted delta over a rolling window (who is in
//                          control right now, recency-biased), normalized.
//   SG1 Delta Velocity  - first derivative of SG0 (is control accelerating or
//                          decelerating), RAW (un-normalized) histogram in
//                          delta units with 4-state fade coloring.
//   SG2 Delta Accel.     - second derivative of SG0 (earliest warning), plotted
//                          only when its magnitude clears a rolling-std-dev
//                          noise threshold.
// All outputs normalized to a fixed -100/+100 scale. Works across all bar
// types (time, range, volume, tick, Renko).
//
// Build notes (defensive fixes carried over from prior SC builds):
//   * No std::min / std::max / std::fabs / std::round, and no <algorithm>.
//     scstructures.h defines min/max as C macros; use the bare SC macros.
//   * sc.PersistVars does NOT exist. This study avoids persistent arrays:
//     with AutoLoop=1 it recomputes the window directly from base data each
//     bar, and stores per-bar raw SG0/SG1/SG2 in hidden subgraphs (SG3/4/5)
//     for the slope lookbacks and rolling std dev.
//   * sc.MasterData[SC_DELTA] does NOT exist on the build server. Delta is
//     computed as Ask volume - Bid volume from base data, which is robust and
//     gracefully yields 0 when order-flow data is unavailable.
//   * sc.SASF_NO_VALUE does NOT exist. Values that should not plot are left at
//     0 with DrawZeros = 0, which suppresses drawing on that bar (used for the
//     acceleration noise filter and for warmup bars).
//   * Custom-string inputs (none here) would be read with GetIndex().

#include "sierrachart.h"

SCDLLName("DeltaVelocityProfile")

// ---- Persistent int slots ----
const int PS_FADE_COUNTER = 2; // consecutive bars velocity magnitude shrinking

// ---- Persistent float slots ----
const int PF_PREV_VEL_MAG = 0; // previous bar's abs(velocity) for fade detection

// ---- Velocity color states ----
// (RGB() in ACSIL takes arguments in R,G,B order.)
#define COL_BRIGHT_GREEN RGB(0, 220, 80)
#define COL_PALE_GREEN   RGB(0, 120, 60)
#define COL_BRIGHT_RED   RGB(220, 50, 50)
#define COL_PALE_RED     RGB(120, 50, 50)

// Delta for a single bar = Ask volume - Bid volume.
inline float BarDelta(SCStudyInterfaceRef sc, int i)
{
    return sc.BaseData[SC_ASKVOL][i] - sc.BaseData[SC_BIDVOL][i];
}

SCSFExport scsf_DeltaVelocityProfile(SCStudyInterfaceRef sc)
{
    // --- Visible subgraph references ---
    SCSubgraphRef WeightedDelta = sc.Subgraph[0];
    SCSubgraphRef Velocity      = sc.Subgraph[1];
    SCSubgraphRef Acceleration  = sc.Subgraph[2];

    // --- Hidden history subgraphs (per-bar raw values, indexed by bar) ---
    SCSubgraphRef RawWD    = sc.Subgraph[3]; // raw weighted delta history
    SCSubgraphRef RawVel   = sc.Subgraph[4]; // raw velocity history
    SCSubgraphRef RawAccel = sc.Subgraph[5]; // raw acceleration history

    // --- Input references ---
    SCInputRef WindowLength      = sc.Input[0];
    SCInputRef Lambda            = sc.Input[1];
    SCInputRef SlopeLookback     = sc.Input[2];
    SCInputRef AccelStdDevMult   = sc.Input[3];
    SCInputRef AccelStdDevWindow = sc.Input[4];
    SCInputRef TickSize          = sc.Input[5];

    if (sc.SetDefaults)
    {
        sc.GraphName        = "Delta Velocity Profile";
        sc.StudyDescription = "EXP-weighted delta momentum with velocity and "
                              "acceleration layers. Fade coloring detects dying "
                              "momentum before price confirms.";
        sc.AutoLoop         = 1;
        sc.GraphRegion      = 1;
        sc.MaintainVolumeAtPriceData = 1;

        // Auto scale: SG1 velocity is plotted raw (un-normalized), so the
        // panel must fit its true range. SG0 and SG2 remain normalized to
        // +/-100 and will appear small relative to large velocity spikes.
        sc.ScaleRangeType   = SCALE_AUTO;

        // --- Visible subgraph defaults ---
        WeightedDelta.Name        = "Weighted Delta";
        WeightedDelta.DrawStyle   = DRAWSTYLE_LINE;
        WeightedDelta.PrimaryColor = RGB(255, 255, 255);
        WeightedDelta.LineWidth   = 2;
        WeightedDelta.DrawZeros   = 1; // legitimately can sit on zero

        Velocity.Name             = "Delta Velocity";
        Velocity.DrawStyle        = DRAWSTYLE_BAR;
        Velocity.PrimaryColor     = COL_BRIGHT_GREEN;
        Velocity.SecondaryColor   = COL_BRIGHT_RED;
        Velocity.SecondaryColorUsed = 1;
        Velocity.LineWidth        = 3;
        Velocity.DrawZeros        = 1;

        Acceleration.Name         = "Delta Acceleration";
        Acceleration.DrawStyle    = DRAWSTYLE_LINE;
        Acceleration.PrimaryColor = RGB(200, 0, 220); // magenta
        Acceleration.LineWidth    = 1;
        Acceleration.DrawZeros    = 0; // gaps when below noise threshold

        // --- Hidden history subgraphs ---
        RawWD.Name        = "Raw Weighted Delta (hidden)";
        RawWD.DrawStyle    = DRAWSTYLE_IGNORE;
        RawWD.DrawZeros    = 0;

        RawVel.Name       = "Raw Velocity (hidden)";
        RawVel.DrawStyle   = DRAWSTYLE_IGNORE;
        RawVel.DrawZeros   = 0;

        RawAccel.Name     = "Raw Acceleration (hidden)";
        RawAccel.DrawStyle = DRAWSTYLE_IGNORE;
        RawAccel.DrawZeros = 0;

        // --- Input defaults ---
        WindowLength.Name = "Window Length";
        WindowLength.SetInt(12);
        WindowLength.SetIntLimits(2, 500);

        Lambda.Name = "Lambda (Decay)";
        Lambda.SetFloat(0.30f);
        Lambda.SetFloatLimits(0.05f, 0.95f);

        SlopeLookback.Name = "Slope Lookback (bars)";
        SlopeLookback.SetInt(3);
        SlopeLookback.SetIntLimits(1, 100);

        AccelStdDevMult.Name = "Accel Noise Filter (Std Dev Multiplier)";
        AccelStdDevMult.SetFloat(1.0f);
        AccelStdDevMult.SetFloatLimits(0.0f, 10.0f);

        AccelStdDevWindow.Name = "Accel Std Dev Window (bars)";
        AccelStdDevWindow.SetInt(10);
        AccelStdDevWindow.SetIntLimits(2, 500);

        TickSize.Name = "Tick Size";
        TickSize.SetFloat(0.25f);

        return;
    }

    // --- Resolved / clamped inputs ---
    int   N      = WindowLength.GetInt();
    if (N < 2) N = 2;
    float lambda = max(0.05f, min(0.95f, Lambda.GetFloat())); // clamp degenerate weighting
    int   L      = SlopeLookback.GetInt();
    if (L < 1) L = 1;
    float accelMult = AccelStdDevMult.GetFloat();
    int   sdWin  = AccelStdDevWindow.GetInt();
    if (sdWin < 2) sdWin = 2;

    // --- Persistent state ---
    int&   fadeCounter      = sc.GetPersistentInt(PS_FADE_COUNTER);
    float& prevVelMagnitude = sc.GetPersistentFloat(PF_PREV_VEL_MAG);

    // --- Reset on full recalculation ---
    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        fadeCounter      = 0;
        prevVelMagnitude = 0.0f;
    }

    const int i = sc.Index;

    // ----------------------------------------------------------------
    // STEP 3: weighted delta for the current bar (computed every bar so
    // the slope lookbacks always have history available).
    // weight[age] = exp(-lambda * age), age = 0 for newest bar.
    // ----------------------------------------------------------------
    int windowStart = i - N + 1;
    if (windowStart < 0) windowStart = 0;

    double sumW  = 0.0;
    double sumWD = 0.0;
    for (int j = windowStart; j <= i; ++j)
    {
        float age    = (float)(i - j);
        double weight = exp(-(double)lambda * age);
        sumW  += weight;
        sumWD += (double)BarDelta(sc, j) * weight;
    }
    float weightedDelta = (sumW > 1e-12) ? (float)(sumWD / sumW) : 0.0f;
    RawWD[i] = weightedDelta;

    // ----------------------------------------------------------------
    // STEP 4: velocity = SG0[now] - SG0[L bars ago]
    // STEP 5: acceleration = velocity[now] - velocity[L bars ago]
    // ----------------------------------------------------------------
    float rawVelocity     = 0.0f;
    float rawAcceleration = 0.0f;

    if (i >= L)
    {
        rawVelocity = RawWD[i] - RawWD[i - L];
        RawVel[i]   = rawVelocity;
    }

    bool haveAccel = false;
    if (i >= 2 * L)
    {
        rawAcceleration = RawVel[i] - RawVel[i - L];
        RawAccel[i]     = rawAcceleration;
        haveAccel       = true;
    }

    // --- Warmup: not enough bars for the full cascade ---
    int minBars = L * 2 + 1;
    if (i < minBars)
    {
        // Leave visible outputs at 0; hidden via DrawZeros where relevant.
        WeightedDelta[i] = 0.0f;
        Velocity[i]      = 0.0f;
        Acceleration[i]  = 0.0f;
        return;
    }

    // ----------------------------------------------------------------
    // STEP 6: fade detection on raw velocity magnitude.
    // ----------------------------------------------------------------
    float currentVelMagnitude = fabs(rawVelocity);
    if (currentVelMagnitude < prevVelMagnitude)
        fadeCounter++;        // magnitude shrinking this bar
    else
        fadeCounter = 0;      // magnitude grew or held - reset
    bool isFading = (fadeCounter >= 2);
    prevVelMagnitude = currentVelMagnitude;

    // ----------------------------------------------------------------
    // STEP 7: acceleration noise filter - rolling std dev over sdWin bars.
    // ----------------------------------------------------------------
    bool plotAccel = false;
    if (haveAccel)
    {
        int sdStart = i - sdWin + 1;
        if (sdStart < 2 * L) sdStart = 2 * L; // only bars that have valid accel
        int sdCount = i - sdStart + 1;

        if (sdCount >= 1)
        {
            double mean = 0.0;
            for (int j = sdStart; j <= i; ++j)
                mean += RawAccel[j];
            mean /= (double)sdCount;

            double var = 0.0;
            for (int j = sdStart; j <= i; ++j)
            {
                double d = (double)RawAccel[j] - mean;
                var += d * d;
            }
            var /= (double)sdCount;
            double stdDev = sqrt(var);

            double threshold = (double)accelMult * stdDev;
            // Guard: on a flat tape (near-zero std dev) always plot.
            if (stdDev < 1e-6 || fabs(rawAcceleration) > threshold)
                plotAccel = true;
        }
    }

    // ----------------------------------------------------------------
    // STEP 8: normalization via rolling min/max across the window (N bars).
    // normalized = ((raw - min)/(max - min)) * 200 - 100, clamped [-100,100].
    // ----------------------------------------------------------------
    // SG0 - weighted delta
    {
        float lo = RawWD[windowStart];
        float hi = RawWD[windowStart];
        for (int j = windowStart; j <= i; ++j)
        {
            float v = RawWD[j];
            lo = min(lo, v);
            hi = max(hi, v);
        }
        float norm;
        if (hi - lo < 1e-6f) norm = 0.0f;
        else                 norm = ((weightedDelta - lo) / (hi - lo)) * 200.0f - 100.0f;
        norm = max(-100.0f, min(100.0f, norm));
        WeightedDelta[i] = norm;
    }

    // SG1 - velocity plotted RAW (no normalization), in delta units.
    // Panel uses SCALE_AUTO so the true range is visible.
    Velocity[i] = rawVelocity;

    // SG1 color assignment (4-state fade scheme), keyed off the true sign.
    if (rawVelocity >= 0.0f)
        Velocity.DataColor[i] = isFading ? COL_PALE_GREEN : COL_BRIGHT_GREEN;
    else
        Velocity.DataColor[i] = isFading ? COL_PALE_RED : COL_BRIGHT_RED;

    // SG2 - acceleration (only when it clears the noise filter)
    if (plotAccel)
    {
        int aStart = windowStart;
        if (aStart < 2 * L) aStart = 2 * L;
        float lo = RawAccel[aStart];
        float hi = RawAccel[aStart];
        for (int j = aStart; j <= i; ++j)
        {
            float v = RawAccel[j];
            lo = min(lo, v);
            hi = max(hi, v);
        }
        float norm;
        if (hi - lo < 1e-6f) norm = 0.0f;
        else                 norm = ((rawAcceleration - lo) / (hi - lo)) * 200.0f - 100.0f;
        norm = max(-100.0f, min(100.0f, norm));
        Acceleration[i] = norm;
    }
    else
    {
        Acceleration[i] = 0.0f; // hidden by DrawZeros = 0 -> gap on this bar
    }
}
