// =============================================================================
// FlowConviction.cpp — Sierra Chart ACSIL Custom Study   (v2 — reworked)
// =============================================================================
//
// V2 CHANGES (vs v1):
//   1. FC_Normalize rewritten as true EWMA mean/variance.  v1 bug: M2_std
//      accumulated forever while n_std was capped → std-dev inflated all
//      session → signal amplitude shrank toward zero by the afternoon.
//   2. Sustained Aggression is now a ROLLING CUMULATIVE weighted delta over
//      "Aggression Window" bars (was single-bar delta).  Stays positive
//      through a sustained grind instead of mean-reverting every bar.
//   3. Directional Velocity uses the same formula (delta / duration) on the
//      live bar as on historical bars (v1 live bar used call-count ratio).
//   4. Uptick Ratio replaced by DELTA SIGN PERSISTENCE: fraction of last
//      "Persistence Window" bars with same-sign delta, ±100.  Orthogonal to
//      Velocity (sign vs size); Uptick was algebraically delta/totVol ≈
//      a rescaled copy of Aggression on tick charts.
//   5. Composite smoothed with 3-bar EMA; background color has 2-bar
//      hysteresis (needs 2 consecutive bars beyond threshold to flip).
//   6. Divergence color override tightened: fires only when composite is
//      OPPOSITE sign to price direction on 2 consecutive bars (was: merely
//      "not strongly confirming", which painted half the histogram).
//   7. Aggression/Velocity/Slope are z-scored WITHOUT mean subtraction
//      (they are naturally zero-centered; subtracting a rolling mean erased
//      trends — the "not sustained" complaint).  PER keeps mean subtraction.
//   8. Tick buffer removed — no longer needed (live bar uses partial bar
//      volumes directly).  Cluster Window input kept as placeholder so
//      saved settings don't shift, but it is unused.
//
// SUBGRAPH INDEX (0-based):
//   Subgraph[0]  SG1  Sustained Aggression   — white  LINE  3px (rolling cum delta)
//   Subgraph[1]  SG2  Directional Velocity   — cyan   LINE  2px (delta/duration)
//   Subgraph[2]  SG3  Delta Persistence      — yellow LINE  1px (sign one-sidedness)
//   Subgraph[3]  SG4  Composite Background   — histogram (green/red/gray/orange/purple)
//   Subgraph[4]  SG5  Correlation Marker     — white  ARROWUP
//   Subgraph[5]  SG6  Composite Slope        — white  LINE  1px
//   Subgraph[6]  (unused)
//   Subgraph[7]  SG7  Divergence Warning     — orange/cyan POINT
//   Subgraph[8]  SG8  Price Efficiency Ratio — magenta LINE  1px
//
// INPUT INDEX (0-based):
//   Input[0]   (unused — was Cluster Window ms, kept as placeholder)
//   Input[1]   Large Print Size
//   Input[2]   Large Print Weight Multiplier
//   Input[3]   Normalization Mean Window (EWMA length)
//   Input[4]   Normalization Std Window  (EWMA length)
//   Input[5]   Normalization Mode (Rolling/Session)
//   Input[6]   Weight — Sustained Aggression
//   Input[7]   Weight — Directional Velocity
//   Input[8]   Weight — Delta Persistence
//   Input[9]   Auto Correlation Adjust
//   Input[10]  Contested Zone Width
//   Input[11]  Divergence Lookback (bars)
//   Input[12]  Divergence Aggression Threshold
//   Input[13]  PER Absorption Threshold
//   Input[14]  Slope Lookback (bars)
//   Input[15]  Aggression Window (bars)
//   Input[16]  Persistence Window (bars)
//
// BUILD-SERVER RULES APPLIED:
//   - No std::min/max/fabs/round  → bare min() max() fabs() sqrtf() tanhf()
//   - No sc.PersistVars           → GetPersistentPointer
//   - SCDateTime never cast to double; use GetHour/GetMinute/GetSecond
//   - Custom string input read with .GetIndex()
//   - Welford/EWMA updated on closed bars only (intrabar guard)
// =============================================================================

#include "sierrachart.h"
#include <cmath>
#include <cstring>

SCDLLName("FlowConviction")

// =============================================================================
// Tunables
// =============================================================================

static const int FC_CORR_WIN = 50;    // correlation ring-buffer window

// Persistent-pointer slots
static const int FC_PS_STATE = 1;

// =============================================================================
// EWMA normalisation state per component
// =============================================================================
struct s_FcWf
{
    float mean;     // EWMA mean
    float var;      // EWMA variance
    int   n;        // sample count (0 = uninitialised)
};

// =============================================================================
// Full persistent study state (heap-allocated, reset on full recalc)
// =============================================================================
struct s_FCState
{
    // ── Core components ───────────────────────────────────────────────────────
    s_FcWf wf[3];               // [0]=Aggression  [1]=Velocity  [2]=Persistence

    // ── Pearson correlation ring buffers ──────────────────────────────────────
    float  ring[3][FC_CORR_WIN];
    int    ring_wr;
    int    ring_n;
    float  corr[3];             // [0]=AB  [1]=AC  [2]=BC
    bool   corr_flag[3];
    float  fw[3];               // effective weights, sum to 1.0

    // ── PER EWMA ──────────────────────────────────────────────────────────────
    s_FcWf wf_per;

    // ── Composite Slope EWMA (SG6) ────────────────────────────────────────────
    s_FcWf wf_slope;
};

// =============================================================================
// Pearson correlation over n samples
// =============================================================================
static float FC_Pearson(const float* a, const float* b, int n)
{
    if (n < 2) return 0.0f;
    float sa = 0.0f, sb = 0.0f;
    for (int k = 0; k < n; k++) { sa += a[k]; sb += b[k]; }
    float ma = sa / (float)n;
    float mb = sb / (float)n;
    float num = 0.0f, da2 = 0.0f, db2 = 0.0f;
    for (int k = 0; k < n; k++)
    {
        float da = a[k] - ma;
        float db = b[k] - mb;
        num += da * db;
        da2 += da * da;
        db2 += db * db;
    }
    float denom = sqrtf(da2 * db2);
    if (denom < 1e-9f) return 0.0f;
    return num / denom;
}

// =============================================================================
// EWMA normalise — update state on closed bars, return tanh(Z)*100
//
//   subtractMean = true  → Z = (raw − mean) / sd      (for always-positive
//                          series like PER that need centering)
//   subtractMean = false → Z = raw / sd               (for naturally
//                          zero-centered series: delta sums, velocity, slope.
//                          Subtracting a rolling mean from these erases
//                          sustained trends — the v1 "not sustained" problem.)
//
//   Variance is always computed around the EWMA mean (correct spread measure
//   either way).  EWMA cannot inflate unboundedly — fixes the v1 M2 bug.
// =============================================================================
static float FC_Normalize(s_FcWf& wf, float raw,
                          int meanWin, int stdWin, float sdFloor,
                          bool updateState, bool subtractMean)
{
    if (updateState)
    {
        if (wf.n == 0)
        {
            wf.mean = raw;
            wf.var  = 0.0f;
            wf.n    = 1;
        }
        else
        {
            float aM = 2.0f / ((float)meanWin + 1.0f);
            float aS = 2.0f / ((float)stdWin  + 1.0f);
            float d  = raw - wf.mean;
            wf.mean += aM * d;
            // EWMA variance around (pre-update) mean
            wf.var   = (1.0f - aS) * wf.var + aS * d * d;
            if (wf.n < 1000000) wf.n++;
        }
    }

    float v = wf.var;
    if (v < 0.0f) v = 0.0f;
    float sd = sqrtf(v);
    if (sd < sdFloor) sd = sdFloor;

    float Z = subtractMean ? (raw - wf.mean) / sd
                           : raw / sd;
    return tanhf(Z) * 100.0f;
}

// =============================================================================
// Main study function
// =============================================================================
SCSFExport scsf_FlowConviction(SCStudyInterfaceRef sc)
{
    // ── Subgraph references ──────────────────────────────────────────────────
    SCSubgraphRef sg_Aggr  = sc.Subgraph[0];   // SG1 Sustained Aggression
    SCSubgraphRef sg_Vel   = sc.Subgraph[1];   // SG2 Directional Velocity
    SCSubgraphRef sg_Pers  = sc.Subgraph[2];   // SG3 Delta Persistence
    SCSubgraphRef sg_Comp  = sc.Subgraph[3];   // SG4 Composite Background
    SCSubgraphRef sg_Corr  = sc.Subgraph[4];   // SG5 Correlation Marker
    SCSubgraphRef sg_Slope = sc.Subgraph[5];   // SG6 Composite Slope
    // Subgraph[6] unused
    SCSubgraphRef sg_Div   = sc.Subgraph[7];   // SG7 Divergence Warning
    SCSubgraphRef sg_PER   = sc.Subgraph[8];   // SG8 Price Efficiency Ratio

    // ── Input references ─────────────────────────────────────────────────────
    SCInputRef in_Unused0     = sc.Input[0];   // placeholder (was Cluster Window)
    SCInputRef in_LargeSz     = sc.Input[1];
    SCInputRef in_LargeWt     = sc.Input[2];
    SCInputRef in_MeanWin     = sc.Input[3];
    SCInputRef in_StdWin      = sc.Input[4];
    SCInputRef in_NormMode    = sc.Input[5];
    SCInputRef in_WtAggr      = sc.Input[6];
    SCInputRef in_WtVel       = sc.Input[7];
    SCInputRef in_WtPers      = sc.Input[8];
    SCInputRef in_AutoCorr    = sc.Input[9];
    SCInputRef in_Contest     = sc.Input[10];
    SCInputRef in_DivLookback = sc.Input[11];
    SCInputRef in_DivAggrThr  = sc.Input[12];
    SCInputRef in_PERAbsThr   = sc.Input[13];
    SCInputRef in_SlopeLB     = sc.Input[14];
    SCInputRef in_AggrWin     = sc.Input[15];
    SCInputRef in_PersWin     = sc.Input[16];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Flow Conviction";
        sc.StudyDescription =
            "Three-component order-flow conviction v2: Sustained Aggression "
            "(rolling cumulative weighted delta, white), Directional Velocity "
            "(delta/duration, cyan), Delta Persistence (sign one-sidedness, "
            "yellow).  EWMA-normalized ±100.  Smoothed composite histogram "
            "with hysteresis coloring and tightened divergence overrides.  "
            "Target: 100T/200T ES tick charts.";
        sc.AutoLoop          = 0;
        sc.GraphRegion       = 1;
        sc.UpdateAlways      = 1;
        sc.DrawZeros         = 0;
        sc.FreeDLL           = 0;
        sc.MaintainAdditionalChartDataArrays = 1;

        // ── Subgraphs ────────────────────────────────────────────────────────
        sg_Aggr.Name         = "Sustained Aggression";
        sg_Aggr.DrawStyle    = DRAWSTYLE_LINE;
        sg_Aggr.LineWidth    = 3;
        sg_Aggr.PrimaryColor = RGB(255, 255, 255);
        sg_Aggr.DrawZeros    = 1;

        sg_Vel.Name          = "Directional Velocity";
        sg_Vel.DrawStyle     = DRAWSTYLE_LINE;
        sg_Vel.LineWidth     = 2;
        sg_Vel.PrimaryColor  = RGB(0, 220, 220);
        sg_Vel.DrawZeros     = 1;

        sg_Pers.Name         = "Delta Persistence";
        sg_Pers.DrawStyle    = DRAWSTYLE_LINE;
        sg_Pers.LineWidth    = 1;
        sg_Pers.PrimaryColor = RGB(220, 220, 0);
        sg_Pers.DrawZeros    = 1;

        sg_Comp.Name         = "Composite Background";
        sg_Comp.DrawStyle    = DRAWSTYLE_BAR;
        sg_Comp.LineWidth    = 2;
        sg_Comp.PrimaryColor = RGB(0, 130, 60);
        sg_Comp.DrawZeros    = 1;

        sg_Corr.Name         = "Correlation Marker";
        sg_Corr.DrawStyle    = DRAWSTYLE_ARROWUP;
        sg_Corr.LineWidth    = 3;
        sg_Corr.PrimaryColor = RGB(255, 255, 255);
        sg_Corr.DrawZeros    = 0;

        sg_Slope.Name         = "Composite Slope";
        sg_Slope.DrawStyle    = DRAWSTYLE_LINE_SKIPZEROS;
        sg_Slope.LineWidth    = 1;
        sg_Slope.PrimaryColor = RGB(255, 255, 255);
        sg_Slope.DrawZeros    = 0;

        sg_Div.Name           = "Divergence Warning";
        sg_Div.DrawStyle      = DRAWSTYLE_POINT;
        sg_Div.LineWidth      = 5;
        sg_Div.PrimaryColor   = RGB(255, 140, 0);
        sg_Div.DrawZeros      = 0;

        sg_PER.Name           = "Price Efficiency Ratio";
        sg_PER.DrawStyle      = DRAWSTYLE_LINE;
        sg_PER.LineWidth      = 1;
        sg_PER.PrimaryColor   = RGB(200, 0, 220);
        sg_PER.DrawZeros      = 1;

        // ── Inputs ───────────────────────────────────────────────────────────
        in_Unused0.Name = "(unused — kept for settings compatibility)";
        in_Unused0.SetInt(500);

        in_LargeSz.Name = "Large Print Size (contracts)";
        in_LargeSz.SetInt(10);
        in_LargeSz.SetIntLimits(1, 500);

        in_LargeWt.Name = "Large Print Weight Multiplier";
        in_LargeWt.SetFloat(3.0f);
        in_LargeWt.SetFloatLimits(1.0f, 20.0f);

        in_MeanWin.Name = "Normalization Mean Window (bars, EWMA)";
        in_MeanWin.SetInt(50);
        in_MeanWin.SetIntLimits(5, 500);

        in_StdWin.Name = "Normalization Std Window (bars, EWMA)";
        in_StdWin.SetInt(50);
        in_StdWin.SetIntLimits(3, 500);

        in_NormMode.Name = "Normalization Mode";
        in_NormMode.SetCustomInputStrings("Rolling;Session");
        in_NormMode.SetCustomInputIndex(0);

        in_WtAggr.Name = "Weight — Sustained Aggression";
        in_WtAggr.SetFloat(0.5f);
        in_WtAggr.SetFloatLimits(0.0f, 10.0f);

        in_WtVel.Name = "Weight — Directional Velocity";
        in_WtVel.SetFloat(0.25f);
        in_WtVel.SetFloatLimits(0.0f, 10.0f);

        in_WtPers.Name = "Weight — Delta Persistence";
        in_WtPers.SetFloat(0.25f);
        in_WtPers.SetFloatLimits(0.0f, 10.0f);

        in_AutoCorr.Name = "Auto Correlation Adjust";
        in_AutoCorr.SetYesNo(1);

        in_Contest.Name = "Contested Zone Width (±N)";
        in_Contest.SetFloat(15.0f);
        in_Contest.SetFloatLimits(0.0f, 50.0f);

        in_DivLookback.Name = "Divergence Lookback (bars)";
        in_DivLookback.SetInt(10);
        in_DivLookback.SetIntLimits(3, 100);

        in_DivAggrThr.Name = "Divergence Aggression Threshold";
        in_DivAggrThr.SetFloat(10.0f);
        in_DivAggrThr.SetFloatLimits(0.0f, 100.0f);

        in_PERAbsThr.Name = "PER Absorption Threshold (normalized)";
        in_PERAbsThr.SetFloat(30.0f);
        in_PERAbsThr.SetFloatLimits(0.0f, 100.0f);

        in_SlopeLB.Name = "Slope Lookback (bars)";
        in_SlopeLB.SetInt(10);
        in_SlopeLB.SetIntLimits(3, 200);

        in_AggrWin.Name = "Aggression Window (bars, rolling cum delta)";
        in_AggrWin.SetInt(20);
        in_AggrWin.SetIntLimits(2, 200);

        in_PersWin.Name = "Persistence Window (bars)";
        in_PersWin.SetInt(20);
        in_PersWin.SetIntLimits(3, 200);

        return;
    }

    // =========================================================================
    // CLEANUP
    // =========================================================================
    if (sc.LastCallToFunction)
    {
        s_FCState* pS =
            reinterpret_cast<s_FCState*>(sc.GetPersistentPointer(FC_PS_STATE));
        if (pS) { delete pS; sc.SetPersistentPointer(FC_PS_STATE, nullptr); }
        return;
    }

    // =========================================================================
    // GUARD
    // =========================================================================
    const int totalBars = sc.ArraySize;
    if (totalBars < 2) return;

    // =========================================================================
    // READ SETTINGS
    // =========================================================================
    const int   meanWin     = max(in_MeanWin.GetInt(), 5);
    const int   stdWin      = max(in_StdWin.GetInt(),  3);
    const int   normMode    = in_NormMode.GetIndex();
    const int   largeSz     = in_LargeSz.GetInt();
    const float largeWt     = in_LargeWt.GetFloat();
    const bool  autoCorr    = (in_AutoCorr.GetYesNo() == 1);
    const float contested   = in_Contest.GetFloat();
    const int   divLookback = max(3, in_DivLookback.GetInt());
    const float divAggrThr  = in_DivAggrThr.GetFloat();
    const float perAbsThr   = in_PERAbsThr.GetFloat();
    const int   slopeLB     = max(3, in_SlopeLB.GetInt());
    const int   aggrWin     = max(2, in_AggrWin.GetInt());
    const int   persWin     = max(3, in_PersWin.GetInt());

    // Self-normalise user weights
    float wA = max(in_WtAggr.GetFloat(), 0.0f);
    float wV = max(in_WtVel.GetFloat(),  0.0f);
    float wP = max(in_WtPers.GetFloat(), 0.0f);
    float wtTot = wA + wV + wP;
    if (wtTot <= 0.0f) wtTot = 1.0f;
    const float fw0_usr = wA / wtTot;
    const float fw1_usr = wV / wtTot;
    const float fw2_usr = wP / wtTot;

    // =========================================================================
    // PERSISTENT STATE
    // =========================================================================
    const bool isFullRecalc = (sc.UpdateStartIndex == 0);

    s_FCState* pS =
        reinterpret_cast<s_FCState*>(sc.GetPersistentPointer(FC_PS_STATE));
    if (isFullRecalc || pS == nullptr)
    {
        if (pS) delete pS;
        pS = new s_FCState();
        memset(pS, 0, sizeof(s_FCState));
        pS->fw[0] = fw0_usr;  pS->fw[1] = fw1_usr;  pS->fw[2] = fw2_usr;
        sc.SetPersistentPointer(FC_PS_STATE, pS);
    }

    // =========================================================================
    // BAR RANGE
    // =========================================================================
    const int processStart = isFullRecalc ? 0 : sc.UpdateStartIndex;
    const int lastBar      = totalBars - 1;

    // =========================================================================
    // BAR LOOP
    // =========================================================================
    for (int i = processStart; i <= lastBar; i++)
    {
        const bool isLiveBar     = (i == lastBar);
        const bool updateWelford = !isLiveBar;   // intrabar guard

        // ── Session reset ─────────────────────────────────────────────────────
        if (normMode == 1 && i > 0 && updateWelford)
        {
            if (sc.BaseDateTimeIn[i].GetDate() != sc.BaseDateTimeIn[i - 1].GetDate())
            {
                memset(pS->wf,        0, sizeof(pS->wf));
                memset(pS->ring,      0, sizeof(pS->ring));
                pS->ring_wr = 0;  pS->ring_n = 0;
                memset(pS->corr,      0, sizeof(pS->corr));
                memset(pS->corr_flag, 0, sizeof(pS->corr_flag));
                pS->fw[0] = fw0_usr;  pS->fw[1] = fw1_usr;  pS->fw[2] = fw2_usr;
                memset(&pS->wf_per,   0, sizeof(pS->wf_per));
                memset(&pS->wf_slope, 0, sizeof(pS->wf_slope));
            }
        }

        // ── Aggregated bar data ───────────────────────────────────────────────
        float askVol = sc.BaseData[SC_ASKVOL][i];
        float bidVol = sc.BaseData[SC_BIDVOL][i];
        float totVol = askVol + bidVol;

        // ── Skip empty bars ───────────────────────────────────────────────────
        if (totVol < 1.0f)
        {
            sg_Aggr[i]  = (i > 0) ? sg_Aggr[i - 1] : 0.0f;
            sg_Vel[i]   = (i > 0) ? sg_Vel[i - 1]  : 0.0f;
            sg_Pers[i]  = (i > 0) ? sg_Pers[i - 1] : 0.0f;
            sg_Comp[i]  = (i > 0) ? sg_Comp[i - 1] : 0.0f;
            sg_Comp.DataColor[i] = (i > 0) ? sg_Comp.DataColor[i - 1]
                                           : RGB(80, 80, 80);
            sg_Corr[i]  = 0.0f;
            sg_Slope[i] = 0.0f;
            sg_Div[i]   = 0.0f;
            sg_PER[i]   = (i > 0) ? sg_PER[i - 1] : 0.0f;
            continue;
        }
        if (totVol < 1.0f) totVol = 1.0f;

        // =====================================================================
        // COMPONENT 1 — SUSTAINED AGGRESSION
        //   Rolling cumulative weighted delta over last aggrWin bars
        //   (includes the live bar's partial volumes — consistent live/hist).
        //   Large-print weighting: bars with avg trade size >= largeSz get
        //   their delta multiplied by largeWt (totVol/100 ≈ vol per trade on
        //   a 100T chart).
        // =====================================================================
        float raw_aggr = 0.0f;
        {
            int wStart = i - aggrWin + 1;
            if (wStart < 0) wStart = 0;
            for (int k = wStart; k <= i; k++)
            {
                float a  = sc.BaseData[SC_ASKVOL][k];
                float b  = sc.BaseData[SC_BIDVOL][k];
                float tv = a + b;
                float scale = (tv / 100.0f >= (float)largeSz) ? largeWt : 1.0f;
                raw_aggr += (a - b) * scale;
            }
        }

        // =====================================================================
        // COMPONENT 2 — DIRECTIONAL VELOCITY (delta / bar duration, seconds)
        //   Same formula live and historical.  Live bar uses elapsed time.
        // =====================================================================
        float barDur = 1.0f;
        {
            int t0 = sc.BaseDateTimeIn[i].GetHour()   * 3600
                   + sc.BaseDateTimeIn[i].GetMinute() * 60
                   + sc.BaseDateTimeIn[i].GetSecond();

            int t1 = t0 + 1;
            if (!isLiveBar && i + 1 < totalBars)
            {
                t1 = sc.BaseDateTimeIn[i + 1].GetHour()   * 3600
                   + sc.BaseDateTimeIn[i + 1].GetMinute() * 60
                   + sc.BaseDateTimeIn[i + 1].GetSecond();
            }
            else if (isLiveBar)
            {
                SCDateTime nowDT = sc.LatestDateTimeForLastBar;
                t1 = nowDT.GetHour()   * 3600
                   + nowDT.GetMinute() * 60
                   + nowDT.GetSecond();
            }

            int dur = t1 - t0;
            if (dur > 0) barDur = (float)dur;   // midnight wrap → fallback 1s
        }
        if (barDur < 0.1f) barDur = 0.1f;

        float raw_vel = (askVol - bidVol) / barDur;

        // =====================================================================
        // COMPONENT 3 — DELTA SIGN PERSISTENCE  (already bounded, no z-score)
        //   (#pos-delta bars − #neg-delta bars) / window * 100  over persWin.
        //   Measures one-sidedness of the tape regardless of size: a slow
        //   grind of small positive deltas reads just as strong as spikes.
        // =====================================================================
        float n_pers = 0.0f;
        {
            int pStart = i - persWin + 1;
            if (pStart < 0) pStart = 0;
            int pos = 0, neg = 0;
            for (int k = pStart; k <= i; k++)
            {
                float d = sc.BaseData[SC_ASKVOL][k] - sc.BaseData[SC_BIDVOL][k];
                if      (d > 0.0f) pos++;
                else if (d < 0.0f) neg++;
            }
            int cnt = i - pStart + 1;
            if (cnt < 1) cnt = 1;
            n_pers = ((float)(pos - neg) / (float)cnt) * 100.0f;
        }

        // =====================================================================
        // NORMALISE SG1/SG2  (no mean subtraction — zero-centered by nature)
        // =====================================================================
        float n_aggr = FC_Normalize(pS->wf[0], raw_aggr, meanWin, stdWin,
                                    1.0f, updateWelford, false);
        float n_vel  = FC_Normalize(pS->wf[1], raw_vel,  meanWin, stdWin,
                                    0.5f, updateWelford, false);

        // =====================================================================
        // CORRELATION RING BUFFER
        // =====================================================================
        if (updateWelford)
        {
            int ri = pS->ring_wr % FC_CORR_WIN;
            pS->ring[0][ri] = n_aggr;
            pS->ring[1][ri] = n_vel;
            pS->ring[2][ri] = n_pers;
            pS->ring_wr = (pS->ring_wr + 1) % FC_CORR_WIN;
            if (pS->ring_n < FC_CORR_WIN) pS->ring_n++;
        }

        // =====================================================================
        // CORRELATION CHECK + WEIGHT ADJUSTMENT
        // =====================================================================
        if (autoCorr && updateWelford && pS->ring_n >= FC_CORR_WIN)
        {
            float cAB = FC_Pearson(pS->ring[0], pS->ring[1], FC_CORR_WIN);
            float cAC = FC_Pearson(pS->ring[0], pS->ring[2], FC_CORR_WIN);
            float cBC = FC_Pearson(pS->ring[1], pS->ring[2], FC_CORR_WIN);

            pS->corr[0] = cAB;  pS->corr_flag[0] = (fabs(cAB) > 0.7f);
            pS->corr[1] = cAC;  pS->corr_flag[1] = (fabs(cAC) > 0.7f);
            pS->corr[2] = cBC;  pS->corr_flag[2] = (fabs(cBC) > 0.7f);

            float a0 = fw0_usr, a1 = fw1_usr, a2 = fw2_usr;
            if (pS->corr_flag[0]) { float t = a0 * 0.5f; a0 -= t; a1 += t; }
            if (pS->corr_flag[1]) { float t = a0 * 0.5f; a0 -= t; a2 += t; }
            if (pS->corr_flag[2]) { float t = a1 * 0.5f; a1 -= t; a2 += t; }
            float wsum = a0 + a1 + a2;
            if (wsum > 0.0f) { a0 /= wsum; a1 /= wsum; a2 /= wsum; }
            pS->fw[0] = a0;  pS->fw[1] = a1;  pS->fw[2] = a2;
        }

        // =====================================================================
        // COMPOSITE SCORE — weighted sum, then 3-bar EMA smoothing
        // =====================================================================
        float comp_raw = n_aggr * pS->fw[0]
                       + n_vel  * pS->fw[1]
                       + n_pers * pS->fw[2];

        float composite;
        if (i > 0)
            composite = sg_Comp[i - 1] + 0.5f * (comp_raw - sg_Comp[i - 1]);
        else
            composite = comp_raw;

        // =====================================================================
        // WRITE SG1/SG2/SG3
        // =====================================================================
        sg_Aggr[i] = n_aggr;
        sg_Vel[i]  = n_vel;
        sg_Pers[i] = n_pers;

        // =====================================================================
        // SG4 — COMPOSITE HISTOGRAM, HYSTERESIS COLORING
        //
        // Base color needs 2 CONSECUTIVE bars in a zone to flip:
        //   both > +contested → Green
        //   both < −contested → Red
        //   both within ±contested → Gray
        //   otherwise → carry previous bar's color (no flicker)
        //
        // Divergence override (tightened): composite must be OPPOSITE sign
        // to price direction on this bar AND the previous bar:
        //   price up   + composite < 0 (2 bars) → Orange (bearish divergence)
        //   price down + composite > 0 (2 bars) → Purple (bullish divergence)
        // =====================================================================
        sg_Comp[i] = composite;

        {
            float compPrev = (i > 0) ? sg_Comp[i - 1] : composite;

            COLORREF baseColor;
            if (composite > contested && compPrev > contested)
                baseColor = RGB(0, 150, 70);
            else if (composite < -contested && compPrev < -contested)
                baseColor = RGB(200, 45, 45);
            else if (fabs(composite) <= contested && fabs(compPrev) <= contested)
                baseColor = RGB(80, 80, 80);
            else
                baseColor = (i > 0) ? sg_Comp.DataColor[i - 1]
                                    : RGB(80, 80, 80);
            sg_Comp.DataColor[i] = baseColor;

            if (i >= divLookback + 1)
            {
                float pc0 = sc.Close[i]     - sc.Close[i - divLookback];
                float pc1 = sc.Close[i - 1] - sc.Close[i - 1 - divLookback];

                if (pc0 > 0.0f && pc1 > 0.0f
                 && composite < 0.0f && compPrev < 0.0f)
                {
                    sg_Comp.DataColor[i] = RGB(255, 140, 0);   // Orange
                }
                else if (pc0 < 0.0f && pc1 < 0.0f
                      && composite > 0.0f && compPrev > 0.0f)
                {
                    sg_Comp.DataColor[i] = RGB(140, 0, 200);   // Purple
                }
            }
        }

        // =====================================================================
        // SG5 — CORRELATION MARKER
        // =====================================================================
        bool anyCorr = pS->corr_flag[0] || pS->corr_flag[1] || pS->corr_flag[2];
        if (anyCorr)
        {
            sg_Corr[i] = 0.5f;
            if      (pS->corr_flag[0]) sg_Corr.DataColor[i] = RGB(255, 255, 255);
            else if (pS->corr_flag[1]) sg_Corr.DataColor[i] = RGB(0,   220, 220);
            else                       sg_Corr.DataColor[i] = RGB(220, 220, 0);
        }
        else
        {
            sg_Corr[i] = 0.0f;
        }

        // =====================================================================
        // SG6 — COMPOSITE SLOPE (OLS over last slopeLB smoothed composite bars)
        // =====================================================================
        if (i >= slopeLB)
        {
            int    N      = slopeLB;
            float  sum_y  = 0.0f;
            float  sum_xy = 0.0f;
            float  sum_x  = (float)(N * (N - 1)) / 2.0f;
            float  sum_x2 = (float)(N * (N - 1) * (2 * N - 1)) / 6.0f;

            for (int k = 0; k < N; k++)
            {
                float y  = sg_Comp[i - N + 1 + k];
                sum_y  += y;
                sum_xy += (float)k * y;
            }

            float denom_slope = (float)N * sum_x2 - sum_x * sum_x;
            float raw_slope   = 0.0f;
            if (fabs(denom_slope) > 1e-9f)
                raw_slope = ((float)N * sum_xy - sum_x * sum_y) / denom_slope;

            float norm_slope = FC_Normalize(pS->wf_slope, raw_slope,
                                            meanWin, stdWin, 0.1f,
                                            updateWelford, false);
            sg_Slope[i] = norm_slope;
        }
        else
        {
            sg_Slope[i] = 0.0f;
        }

        // =====================================================================
        // SG8 — PRICE EFFICIENCY RATIO
        //   raw_per = totalVolume / max(|close-open|, tiny_floor)
        //   High PER = absorption;  Low PER = efficient directional move.
        //   Always-positive series → keeps mean subtraction.
        // =====================================================================
        float price_move = fabs(sc.Close[i] - sc.Open[i]);
        if (price_move < 0.0001f) price_move = 0.0001f;
        float raw_per  = totVol / price_move;
        float norm_per = FC_Normalize(pS->wf_per, raw_per, meanWin, stdWin,
                                      1.0f, updateWelford, true);
        sg_PER[i] = norm_per;

        // =====================================================================
        // SG7 — DIVERGENCE WARNING (unchanged logic, now on smoothed composite)
        //
        //   BEARISH (distribution at N-bar high):
        //     High[i] == N-bar high  AND  n_aggr < −divAggrThr
        //     AND  composite < 0  AND  norm_per > perAbsThr   → Orange point
        //
        //   BULLISH (absorption at N-bar low):
        //     Low[i] == N-bar low  AND  n_aggr > +divAggrThr
        //     AND  composite > 0  AND  norm_per > perAbsThr   → Cyan point
        // =====================================================================
        sg_Div[i] = 0.0f;

        if (updateWelford && i >= divLookback)
        {
            float highMax = sc.High[i];
            float lowMin  = sc.Low[i];
            for (int k = i - divLookback + 1; k < i; k++)
            {
                if (sc.High[k] > highMax) highMax = sc.High[k];
                if (sc.Low[k]  < lowMin)  lowMin  = sc.Low[k];
            }

            bool atHigh = (sc.High[i] >= highMax);
            bool atLow  = (sc.Low[i]  <= lowMin);

            if (atHigh
             && n_aggr   < -divAggrThr
             && composite < 0.0f
             && norm_per  > perAbsThr)
            {
                sg_Div[i] = composite;
                sg_Div.DataColor[i] = RGB(255, 140, 0);   // orange
            }
            else if (atLow
                  && n_aggr   >  divAggrThr
                  && composite > 0.0f
                  && norm_per  > perAbsThr)
            {
                sg_Div[i] = composite;
                sg_Div.DataColor[i] = RGB(0, 210, 240);   // cyan
            }
        }

    }   // end bar loop
}
