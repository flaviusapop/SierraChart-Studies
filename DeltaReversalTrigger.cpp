// DeltaReversalTrigger.cpp
// Sierra Chart ACSIL custom study.
//
// Selectable delta series with a running sigma band, plus the raw acceleration
// of an INDEPENDENTLY selectable series in its own region. This lets you run,
// e.g., the delta line/bands on Rolling Sum while acceleration is computed on
// the Weighted series.
//
//   SG0 Delta Line      - the selected delta series S[t] (Delta Source), raw.
//   SG1 Upper Band       - mu + K*sigma (running Welford stats on S).
//   SG2 Lower Band       - mu - K*sigma.
//   SG3 Raw Acceleration - A = P[t] - 2*P[t-L] + P[t-2L] on the Accel Source
//                          series P[t], magenta (Region 2).
//   SG4 Accel Zero Line  - 0 reference for SG3 (Region 2).
//
// Series source modes (used by both Delta Source and Accel Source):
//   0 Weighted    - exp-weighted mean: sum(d[i]*exp(-lambda*(N-1-i)))/sum(w)
//   1 Rolling Sum - equal-weighted sum of delta over the last N bars
//   2 Cumulative  - running sum of delta, reset at each new trading day
//
// Build notes (defensive fixes confirmed against the SC build server):
//   * No std::min/max/fabs/round and no <algorithm>; scstructures.h defines
//     min/max as C macros - use the bare SC macros.
//   * sc.PersistVars does NOT exist. Cross-bar state (the two series histories,
//     Welford mean/M2/count, and the Cumulative running sums) is kept in HIDDEN
//     subgraph arrays indexed by bar. This is also more correct than
//     sc.GetPersistentFloat here: AutoLoop reprocesses the last bar on every
//     incoming tick, so a plain accumulator would double-count intrabar.
//     Bar-indexed arrays rebuild idempotently from bar (i-1) each call, and
//     are implicitly reset on full recalculation (the pass restarts at i==0).
//   * sc.MasterData[SC_DELTA] does NOT exist on the build server. Delta is
//     Ask volume - Bid volume from base data (0 cleanly when no order flow).
//   * GraphRegion is NOT settable per-subgraph from ACSIL. The study draws in
//     Region 1; move SG3 (Raw Acceleration) and SG4 (Accel Zero Line) to
//     Graph Region 2 via Chart Studies -> this study -> Subgraphs tab.
//   * Auto scale constant is SCALE_AUTO (not SCALE_AUTOMATIC).
//   * Custom-string inputs are read with GetIndex() (not GetCustomInputIndex()).
//   * "Reuse circular-buffer slot addressing": acceleration reads P[i-L] and
//     P[i-2L] directly from the bar-indexed series history - functionally the
//     same lookback addressing, but idempotent under AutoLoop.

#include "sierrachart.h"

SCDLLName("DeltaReversalTrigger")

// ---- Colors ----
#define COL_DELTA RGB(255, 255, 255)
#define COL_BAND  RGB(110, 110, 125)
#define COL_ACCEL RGB(200, 0, 220)   // magenta
#define COL_ZERO  RGB(110, 110, 125)

// ---- Series source modes ----
const int SRC_WEIGHTED    = 0;
const int SRC_ROLLING_SUM = 1;
const int SRC_CUMULATIVE   = 2;

// Delta for a single bar = Ask volume - Bid volume.
inline float BarDelta(SCStudyInterfaceRef sc, int i)
{
    return sc.BaseData[SC_ASKVOL][i] - sc.BaseData[SC_BIDVOL][i];
}

// Compute a delta-derived series value for bar i under the given source mode.
// For Cumulative, prevCumulative is the series value at bar i-1 (the running
// sum so far), which is reset whenever startFreshCumul is true (i==0 or a new
// trading day).
inline float ComputeSeries(SCStudyInterfaceRef sc, int i, int mode,
                           int N, float lambda, bool startFreshCumul,
                           float prevCumulative)
{
    if (mode == SRC_WEIGHTED)
    {
        int windowStart = i - N + 1; if (windowStart < 0) windowStart = 0;
        double sumW = 0.0, sumWD = 0.0;
        for (int j = windowStart; j <= i; ++j)
        {
            double w = exp(-(double)lambda * (double)(i - j));
            sumW  += w;
            sumWD += (double)BarDelta(sc, j) * w;
        }
        return (sumW > 1e-12) ? (float)(sumWD / sumW) : 0.0f;
    }
    else if (mode == SRC_ROLLING_SUM)
    {
        int windowStart = i - N + 1; if (windowStart < 0) windowStart = 0;
        double sum = 0.0;
        for (int j = windowStart; j <= i; ++j)
            sum += (double)BarDelta(sc, j);
        return (float)sum;
    }
    else // SRC_CUMULATIVE - running sum, reset on new trading day
    {
        if (startFreshCumul) return BarDelta(sc, i);
        return prevCumulative + BarDelta(sc, i);
    }
}

SCSFExport scsf_DeltaReversalTrigger(SCStudyInterfaceRef sc)
{
    // --- Visible subgraphs ---
    SCSubgraphRef DeltaLine = sc.Subgraph[0];
    SCSubgraphRef UpperBand = sc.Subgraph[1];
    SCSubgraphRef LowerBand = sc.Subgraph[2];
    SCSubgraphRef Accel     = sc.Subgraph[3];
    SCSubgraphRef AccelZero = sc.Subgraph[4];

    // --- Hidden state/history subgraphs (indexed by bar) ---
    SCSubgraphRef SHist  = sc.Subgraph[5]; // delta-source series history (also cumulative store)
    SCSubgraphRef WMean  = sc.Subgraph[6]; // Welford running mean on S
    SCSubgraphRef WM2    = sc.Subgraph[7]; // Welford running M2
    SCSubgraphRef WCount = sc.Subgraph[8]; // Welford running count
    SCSubgraphRef PHist  = sc.Subgraph[9]; // accel-source series history (also cumulative store)

    // --- Inputs ---
    SCInputRef WindowLength    = sc.Input[0];
    SCInputRef Lambda          = sc.Input[1];
    SCInputRef SlopeLookback   = sc.Input[2];
    SCInputRef ExtensionK      = sc.Input[3];
    SCInputRef SigmaMode       = sc.Input[4];
    SCInputRef SigmaMinSamples = sc.Input[5];
    SCInputRef DeltaSource     = sc.Input[6];
    SCInputRef AccelSource     = sc.Input[7];

    if (sc.SetDefaults)
    {
        sc.GraphName        = "Delta Reversal Trigger";
        sc.StudyDescription = "Selectable delta series with a running sigma band "
                              "and raw acceleration of an independently "
                              "selectable series in its own region.";
        sc.AutoLoop         = 1;
        sc.GraphRegion      = 1;
        sc.MaintainVolumeAtPriceData = 1;
        sc.ScaleRangeType   = SCALE_AUTO;

        DeltaLine.Name         = "Delta Line";
        DeltaLine.DrawStyle    = DRAWSTYLE_LINE;
        DeltaLine.PrimaryColor = COL_DELTA;
        DeltaLine.LineWidth    = 2;
        DeltaLine.DrawZeros    = 1; // delta can legitimately sit at 0

        UpperBand.Name         = "Upper Band (mu + K*sigma)";
        UpperBand.DrawStyle    = DRAWSTYLE_LINE;
        UpperBand.PrimaryColor = COL_BAND;
        UpperBand.LineWidth    = 1;
        UpperBand.DrawZeros    = 0; // hidden during warmup

        LowerBand.Name         = "Lower Band (mu - K*sigma)";
        LowerBand.DrawStyle    = DRAWSTYLE_LINE;
        LowerBand.PrimaryColor = COL_BAND;
        LowerBand.LineWidth    = 1;
        LowerBand.DrawZeros    = 0;

        // SG3 + SG4 belong in Region 2 - move them there via the Subgraphs tab.
        Accel.Name         = "Raw Acceleration";
        Accel.DrawStyle    = DRAWSTYLE_LINE;
        Accel.PrimaryColor = COL_ACCEL;
        Accel.LineWidth    = 1;
        Accel.DrawZeros    = 0; // line skips lone zero vertices; hides warmup

        AccelZero.Name         = "Accel Zero Line";
        AccelZero.DrawStyle    = DRAWSTYLE_LINE;
        AccelZero.LineStyle    = LINESTYLE_DASH;
        AccelZero.PrimaryColor = COL_ZERO;
        AccelZero.LineWidth    = 1;
        AccelZero.DrawZeros    = 1; // static 0 reference

        // Hidden state arrays
        SHist.Name   = "S history (hidden)";     SHist.DrawStyle  = DRAWSTYLE_IGNORE; SHist.DrawZeros  = 0;
        WMean.Name   = "Welford mean (hidden)";  WMean.DrawStyle  = DRAWSTYLE_IGNORE; WMean.DrawZeros  = 0;
        WM2.Name     = "Welford M2 (hidden)";    WM2.DrawStyle    = DRAWSTYLE_IGNORE; WM2.DrawZeros    = 0;
        WCount.Name  = "Welford count (hidden)"; WCount.DrawStyle = DRAWSTYLE_IGNORE; WCount.DrawZeros = 0;
        PHist.Name   = "Accel series (hidden)";  PHist.DrawStyle  = DRAWSTYLE_IGNORE; PHist.DrawZeros  = 0;

        // --- Inputs ---
        WindowLength.Name = "Window Length";
        WindowLength.SetInt(12);
        WindowLength.SetIntLimits(2, 500);

        Lambda.Name = "Lambda (Decay)";
        Lambda.SetFloat(0.30f);
        Lambda.SetFloatLimits(0.05f, 0.95f);

        SlopeLookback.Name = "Slope Lookback (L)";
        SlopeLookback.SetInt(4);
        SlopeLookback.SetIntLimits(1, 100);

        ExtensionK.Name = "Extension K (sigma)";
        ExtensionK.SetFloat(2.0f);
        ExtensionK.SetFloatLimits(0.1f, 10.0f);

        SigmaMode.Name = "Sigma Mode";
        SigmaMode.SetCustomInputStrings("Continuous (running);Reset Each Trading Day");
        SigmaMode.SetCustomInputIndex(1);

        SigmaMinSamples.Name = "Sigma Min Samples";
        SigmaMinSamples.SetInt(50);
        SigmaMinSamples.SetIntLimits(2, 100000);

        DeltaSource.Name = "Delta Source";
        DeltaSource.SetCustomInputStrings("Weighted;Rolling Sum;Cumulative");
        DeltaSource.SetCustomInputIndex(1); // Rolling Sum

        AccelSource.Name = "Accel Source";
        AccelSource.SetCustomInputStrings("Weighted;Rolling Sum;Cumulative");
        AccelSource.SetCustomInputIndex(0); // Weighted

        return;
    }

    // --- Resolved / clamped inputs ---
    int   N = WindowLength.GetInt();   if (N < 2) N = 2;
    float lambda = max(0.05f, min(0.95f, Lambda.GetFloat()));
    int   L = SlopeLookback.GetInt();  if (L < 1) L = 1;
    float K = ExtensionK.GetFloat();
    int   sigmaMode   = SigmaMode.GetIndex();      // 0 = continuous, 1 = reset daily
    int   minSamples  = SigmaMinSamples.GetInt();  if (minSamples < 2) minSamples = 2;
    int   deltaSource = DeltaSource.GetIndex();    // series for SG0/bands
    int   accelSource = AccelSource.GetIndex();    // series for SG3 acceleration

    const int i = sc.Index;
    int warmupIndex = max(N, 2 * L + 1);

    // --- New-trading-day detection (session-aware, not calendar midnight) ---
    bool newDay = false;
    if (i > 0)
    {
        int tdayCur  = sc.GetTradingDayDate(sc.BaseDateTimeIn[i]);
        int tdayPrev = sc.GetTradingDayDate(sc.BaseDateTimeIn[i - 1]);
        newDay = (tdayCur != tdayPrev);
    }
    // Cumulative series always reset on a new trading day (regardless of Sigma Mode).
    bool freshCumul = (i == 0) || newDay;

    // ----------------------------------------------------------------
    // 1) DELTA-SOURCE SERIES S[t] (SG0 + bands)
    // ----------------------------------------------------------------
    float prevSCumul = (i == 0) ? 0.0f : SHist[i - 1];
    float S = ComputeSeries(sc, i, deltaSource, N, lambda, freshCumul, prevSCumul);
    SHist[i]     = S;
    DeltaLine[i] = S;

    // ----------------------------------------------------------------
    // 2) ACCEL-SOURCE SERIES P[t] (independent of S)
    // ----------------------------------------------------------------
    float prevPCumul = (i == 0) ? 0.0f : PHist[i - 1];
    float P = ComputeSeries(sc, i, accelSource, N, lambda, freshCumul, prevPCumul);
    PHist[i] = P;

    // ----------------------------------------------------------------
    // 3) WELFORD RUNNING STATS on S (committed per bar -> idempotent).
    //    Optional daily reset when Sigma Mode == 1.
    // ----------------------------------------------------------------
    bool startFresh = (i == 0) || (sigmaMode == 1 && newDay);
    double prevCount, prevMean, prevM2;
    if (startFresh) { prevCount = 0.0; prevMean = 0.0; prevM2 = 0.0; }
    else
    {
        prevCount = (double)WCount[i - 1];
        prevMean  = (double)WMean[i - 1];
        prevM2    = (double)WM2[i - 1];
    }

    double count = prevCount + 1.0;
    double d     = (double)S - prevMean;
    double mean  = prevMean + d / count;
    double M2    = prevM2 + d * ((double)S - mean);

    WCount[i] = (float)count;
    WMean[i]  = (float)mean;
    WM2[i]    = (float)M2;

    double sigma = (count >= 1.0) ? sqrt(M2 / count) : 0.0;
    if (sigma < 1e-6) sigma = 0.0;

    // ----------------------------------------------------------------
    // 4) BANDS - only after warmup and enough samples.
    // ----------------------------------------------------------------
    if (i >= warmupIndex && count >= (double)minSamples && sigma > 0.0)
    {
        UpperBand[i] = (float)(mean + (double)K * sigma);
        LowerBand[i] = (float)(mean - (double)K * sigma);
    }

    // ----------------------------------------------------------------
    // 5) RAW ACCELERATION of P (second difference over L) + zero reference.
    // ----------------------------------------------------------------
    if (i >= warmupIndex)
    {
        Accel[i]     = PHist[i] - 2.0f * PHist[i - L] + PHist[i - 2 * L];
        AccelZero[i] = 0.0f;
    }
}
