#include "sierrachart.h"

// ============================================================================
// DOMReaderV2.cpp
//
// v2 of the MotiveWave / OrderFlow Labs "DOM Reader" (Pull/Stack,
// History Mode = Delta) port. The v1 forward-only Pull/Stack oscillator is
// preserved byte-for-byte on its existing output (SG1 Net Pull/Stack, plus the
// hidden Bid Net / Ask Net / Raw Net). v2 layers analytics on top.
//
// WHAT v2 ADDS
//   1. Atoms: each side's per-level change is split into positive-magnitude
//      Stack and Pull buckets -> bidStack, bidPull, askStack, askPull (SG5-8).
//      Invariant preserved: bidNet = bidStack - bidPull, askNet = askStack - askPull.
//   2. Conviction butterfly: bullFlow = bidStack + askPull (drawn up, SG9),
//      bearFlow = askStack + bidPull (mirror -bearFlow drawn down, SG11; true
//      magnitude kept internally in SG10). Invariant: bullFlow - bearFlow == rawNet.
//   3. Forward Welford stats (mean/variance), per trading session, with warmup,
//      over bullFlow / bearFlow / net. Updated once per CLOSED bar.
//   4. Two bar-close, edge-triggered, cooldown-gated triggers, each bull+bear:
//        - Collapse/Vacuum (a tall wall on one side evaporates)
//        - Contested-then-Resolve (both sides hot, then net breaks out)
//      Markers SG12-15.
//   5. Directional SMA filter on HLC/3 trade price (full history, native; not
//      depth). Tags each marker aligned (saturated) / counter (muted) /
//      neutral. Display-only by default (GateCounterTriggers = No).
//
// FORWARD-ONLY (depth) vs FULL-HISTORY (price):
//   Depth-derived series (atoms, conviction, triggers) cannot be reconstructed
//   from history and accumulate forward only. A SETTINGS CHANGE repaints them
//   from the Option-A persistent cache (and replays stats+triggers so every
//   threshold is retunable across cached history). The SMA filter is trade
//   data and paints full history natively.
//
// Sierra Chart subgraph/input numbering is 1-based in the UI (first subgraph =
// SG1). Code indices stay 0-based; all human-facing names/comments use SG1..SG16.
//
// v2.1 changes: (1) removed the negated bearFlow "mirror" — Bear Flow is now a
//   single upward, positive-magnitude plottable line like Bull Flow. (2) Added
//   a Flow Calculation Mode (Raw / Moving Average / Cumulative) that transforms
//   ONLY the visible Bull Flow / Bear Flow lines. Raw magnitudes are stored in
//   internal Bull Flow Raw / Bear Flow Raw subgraphs; all stats and triggers
//   consume the raw series, never the display transform.
//
// Author: built for Flavius' ES scalping ACSIL workflow (target: 200vol ES).
// ============================================================================

SCDLLName("DOMReaderV2")

// ---- Persistent-pointer slot for our cross-call state -----------------------
const int PP_STATE = 1;

// Hard capacity for the per-side level snapshot arrays. MaxLevels is clamped.
const int LEVEL_CAP = 256;

// Subgraph code indices (UI shows these +1, i.e. SG1..SG16).
const int SG_NET=0, SG_BIDNET=1, SG_ASKNET=2, SG_RAW=3;
const int SG_BIDSTACK=4, SG_BIDPULL=5, SG_ASKSTACK=6, SG_ASKPULL=7;
const int SG_BULL=8, SG_BEAR=9;             // visible flow lines (display-transformed)
const int SG_BULLRAW=10, SG_BEARRAW=11;     // raw magnitudes (stats/triggers source)
const int SG_CB=12, SG_CBR=13, SG_KB=14, SG_KBR=15, SG_SLOPE=16;

// Input code indices.
const int IN_MAXLVL=0, IN_SMOOTHPER=1, IN_SKIPEMPTY=2, IN_SMOOTHON=3;
const int IN_W=4, IN_K=5, IN_M=6, IN_DECLINE=7, IN_REQOPP=8, IN_J=9,
          IN_MINCON=10, IN_COOLDOWN=11, IN_WARMUP=12, IN_SMALEN=13,
          IN_SLOPEDB=14, IN_GATE=15;
const int IN_FLOWMODE=16, IN_FLOWMATYPE=17, IN_FLOWMALEN=18,
          IN_FLOWCUMCNT=19, IN_FLOWCUMRESET=20;

// One resting-order level: price as integer-tick key + size.
struct s_Lvl { int tick; double qty; };

// Tunable parameters snapshot (read once per call, passed to the close hook).
struct s_Par
{
    int    W;            // Event Lookback
    double k;            // Tall/Hot sigma
    double M;            // Collapse drop sigma
    int    declineSpan;  // Collapse decline span
    int    reqOpp;       // Require opposite holds (bool)
    double J;            // Net breakout sigma
    int    minContested; // Min contested bars
    int    cooldown;     // Cooldown bars
    int    warmup;       // Warmup bars
    int    smaLen;       // SMA length L
    double slopeDeadT;   // Slope deadband (ticks)
    int    gateCounter;  // Gate counter triggers (bool)
    double tickSize;
};

// Persistent state across study calls.
struct s_DomState
{
    // --- forward depth-diff state (v1) ---
    s_Lvl  prevBid[LEVEL_CAP]; int prevBidN;
    s_Lvl  prevAsk[LEVEL_CAP]; int prevAskN;

    // per-bar accumulators (positive-magnitude atoms + derived nets)
    double bidStackAccum, bidPullAccum, askStackAccum, askPullAccum;
    double bidNetAccum, askNetAccum;   // = stack - pull (kept for SG2/SG3)

    int    curBarIndex;
    int    hasPrev;
    int    depthWarned;
    int    infoLogged;

    // --- history cache (Option A) ---
    double *rawCache, *bidCache, *askCache;
    double *bullCache, *bearCache;
    double *bidStackCache, *bidPullCache, *askStackCache, *askPullCache;
    int    cacheCap, cacheCount;

    // --- Welford session stats (closed-bar) ---
    int    wN;
    double muBull, m2Bull, muBear, m2Bear, muNet, m2Net;
    int    sessionDate;   // trading-day date int; -1 = none yet (dates are positive)
    int    lastStatBar;   // highest bar index already folded/evaluated

    // --- trigger latch + cooldown (k = 0..3) ---
    int    condPrev[4];
    int    cooldownUntil[4];
};

// ---- small helpers ---------------------------------------------------------
static double FindQty(const s_Lvl* arr, int n, int tick)
{
    for (int i = 0; i < n; ++i)
        if (arr[i].tick == tick) return arr[i].qty;
    return -1.0;
}

static double* GrowArr(double* old, int oldCount, int newCap)
{
    double* na = new double[newCap];
    for (int i = 0; i < newCap; ++i) na[i] = 0.0;
    for (int i = 0; i < oldCount; ++i) na[i] = old[i];
    delete[] old;
    return na;
}

static void EnsureCacheCap(s_DomState* st, int needed)
{
    if (needed <= st->cacheCap) return;
    int newCap = (st->cacheCap > 0) ? st->cacheCap : 1024;
    while (newCap < needed) newCap *= 2;
    st->rawCache      = GrowArr(st->rawCache,      st->cacheCount, newCap);
    st->bidCache      = GrowArr(st->bidCache,      st->cacheCount, newCap);
    st->askCache      = GrowArr(st->askCache,      st->cacheCount, newCap);
    st->bullCache     = GrowArr(st->bullCache,     st->cacheCount, newCap);
    st->bearCache     = GrowArr(st->bearCache,     st->cacheCount, newCap);
    st->bidStackCache = GrowArr(st->bidStackCache, st->cacheCount, newCap);
    st->bidPullCache  = GrowArr(st->bidPullCache,  st->cacheCount, newCap);
    st->askStackCache = GrowArr(st->askStackCache, st->cacheCount, newCap);
    st->askPullCache  = GrowArr(st->askPullCache,  st->cacheCount, newCap);
    st->cacheCap = newCap;
}

static void WelfordAdd(int& n, double& mean, double& M2, double x)
{
    // n is the shared count; caller increments it ONCE per bar (see below).
    double delta = x - mean;
    mean += delta / (double)n;
    M2   += delta * (x - mean);
}

static double SampleSigma(double M2, int n)
{
    return (n > 1) ? sqrt(M2 / (double)(n - 1)) : 0.0;
}

static void ResetWelford(s_DomState* st)
{
    st->wN = 0;
    st->muBull = 0.0; st->m2Bull = 0.0;
    st->muBear = 0.0; st->m2Bear = 0.0;
    st->muNet  = 0.0; st->m2Net  = 0.0;
}

// SMA of typical price (HLC/3) over [idx-L+1 .. idx]. Trade data -> full history.
static double TypicalSMA(SCStudyInterfaceRef sc, int idx, int L)
{
    if (idx < 0) return 0.0;
    int start = idx - (L - 1);
    if (start < 0) start = 0;
    double sum = 0.0; int cnt = 0;
    for (int i = start; i <= idx; ++i)
    {
        double tp = (sc.BaseDataIn[SC_HIGH][i] + sc.BaseDataIn[SC_LOW][i]
                     + sc.BaseDataIn[SC_LAST][i]) / 3.0;
        sum += tp; cnt++;
    }
    return (cnt > 0) ? sum / (double)cnt : 0.0;
}

// Slope state: +1 Bull / 0 Neutral / -1 Bear, with tick deadband.
static int SlopeState(SCStudyInterfaceRef sc, int idx, int L, double deadbandTicks)
{
    if (idx - L < 0) return 0;
    double s = TypicalSMA(sc, idx, L) - TypicalSMA(sc, idx - L, L);
    double db = deadbandTicks * sc.TickSize;
    if (s >  db) return  1;
    if (s < -db) return -1;
    return 0;
}

// Marker colour: family (0=Collapse,1=Contested) modulated by alignment
// (1=aligned -> saturated, -1=counter -> muted, 0=neutral).
static unsigned int MarkerColor(int family, int alignment)
{
    if (family == 0) // Collapse -> orange family
    {
        if (alignment > 0) return RGB(255, 140, 0);    // saturated
        if (alignment < 0) return RGB(180, 150, 110);  // muted
        return RGB(160, 160, 160);                     // neutral
    }
    // Contested -> purple family
    if (alignment > 0) return RGB(150, 80, 220);
    if (alignment < 0) return RGB(150, 130, 180);
    return RGB(160, 160, 160);
}

// ---- Flow display transform (v2.1) -----------------------------------------
// Computes the VISIBLE flow line value at bar i from the RAW magnitude series.
// This is display-only; it never feeds stats or triggers (those read the raw
// subgraphs / cache). Deterministic from the raw series, so it recomputes
// correctly on full-recalc repaint.
//   mode 0 = Raw Data, 1 = Moving Average, 2 = Cumulative
static void ApplyFlowDisplay(SCStudyInterfaceRef sc, int mode,
                             MovAvgTypeEnum maType, int maLen,
                             int cumCnt, int cumReset,
                             SCSubgraphRef rawSG, SCSubgraphRef visSG, int i)
{
    if (i < 0) return;

    if (mode == 1)            // Moving Average of the raw series
    {
        sc.MovingAverage(rawSG, visSG, maType, i, maLen);
    }
    else if (mode == 2)       // Cumulative rolling window (optional session reset)
    {
        int start = i - (cumCnt - 1);
        if (start < 0) start = 0;
        int dayI = cumReset ? sc.GetTradingDayDate(sc.BaseDateTimeIn[i]) : 0;
        double sum = 0.0;
        for (int j = i; j >= start; --j)
        {
            if (cumReset && sc.GetTradingDayDate(sc.BaseDateTimeIn[j]) != dayI)
                break;          // do not let the window cross the session start
            sum += rawSG[j];
        }
        visSG[i] = sum;
    }
    else                      // Raw Data (current behaviour)
    {
        visSG[i] = rawSG[i];
    }
}

// ---- the bar-close hook ----------------------------------------------------
// Runs §3 (Welford) + §4 (triggers) + §7 (SMA tag) for the just-CLOSED bar t.
// Used live (one call per real close) and during full-recalc replay.
static void ProcessClosedBar(SCStudyInterfaceRef sc, s_DomState* st,
                             const s_Par& P, int t)
{
    if (t < 0) return;
    if (t <= st->lastStatBar) return;   // never double-process a bar

    const double bull = st->bullCache[t];
    const double bear = st->bearCache[t];
    const double net  = st->rawCache[t];

    // --- session boundary ---
    int dayDate = (int)sc.GetTradingDayDate(sc.BaseDateTimeIn[t]);
    if (dayDate != st->sessionDate)
    {
        ResetWelford(st);
        st->sessionDate = dayDate;
    }

    // --- fold into Welford (count incremented ONCE, shared across streams) ---
    st->wN += 1;
    WelfordAdd(st->wN, st->muBull, st->m2Bull, bull);
    {
        int nb = st->wN; double mb = st->muBear, M2b = st->m2Bear;
        WelfordAdd(nb, mb, M2b, bear);
        st->muBear = mb; st->m2Bear = M2b;
    }
    {
        int nn = st->wN; double mn = st->muNet, M2n = st->m2Net;
        WelfordAdd(nn, mn, M2n, net);
        st->muNet = mn; st->m2Net = M2n;
    }

    const double muBull = st->muBull, sBull = SampleSigma(st->m2Bull, st->wN);
    const double muBear = st->muBear, sBear = SampleSigma(st->m2Bear, st->wN);
    const double sNet   = SampleSigma(st->m2Net, st->wN);

    const int warm  = (st->wN >= P.warmup);
    const int slope = SlopeState(sc, t, P.smaLen, P.slopeDeadT);
    if (SG_SLOPE >= 0) sc.Subgraph[SG_SLOPE][t] = (double)slope;

    double off = 0.3 * sNet;
    if (off < 1e-6) off = 1.0;

    // window bounds [lo..t]
    int lo = t - (P.W - 1);
    if (lo < 0) lo = 0;

    // ===== Trigger 1: Collapse / Vacuum =====
    // Bull variant watches BEAR collapsing; Bear variant watches BULL.
    int collapseBull = 0, collapseBear = 0;
    {
        // --- bull: bear wall collapsed ---
        double P_bear = bear; int pj_bear = t;
        for (int j = lo; j <= t; ++j)
            if (st->bearCache[j] > P_bear) { P_bear = st->bearCache[j]; pj_bear = j; }
        int wasTall_b = (P_bear >= muBear + P.k * sBear) && ((t - pj_bear) <= P.declineSpan);
        int fell_b    = ((P_bear - bear) >= P.M * sBear) && (bear <= muBear);
        int oppHold_b = (!P.reqOpp) || (bull >= muBull);
        collapseBull  = (sBear > 0.0) && wasTall_b && fell_b && oppHold_b;

        // --- bear: bull wall collapsed ---
        double P_bull = bull; int pj_bull = t;
        for (int j = lo; j <= t; ++j)
            if (st->bullCache[j] > P_bull) { P_bull = st->bullCache[j]; pj_bull = j; }
        int wasTall_k = (P_bull >= muBull + P.k * sBull) && ((t - pj_bull) <= P.declineSpan);
        int fell_k    = ((P_bull - bull) >= P.M * sBull) && (bull <= muBull);
        int oppHold_k = (!P.reqOpp) || (bear >= muBear);
        collapseBear  = (sBull > 0.0) && wasTall_k && fell_k && oppHold_k;
    }

    // ===== Trigger 2: Contested-then-Resolve =====
    int contestedBull = 0, contestedBear = 0;
    {
        int fight = 0;
        for (int j = lo; j <= t; ++j)
        {
            if (st->bullCache[j] >= muBull + P.k * sBull &&
                st->bearCache[j] >= muBear + P.k * sBear)
                fight++;
        }
        int enoughFight = (fight >= P.minContested) && (sBull > 0.0) && (sBear > 0.0);
        if (enoughFight)
        {
            contestedBull = (net >  P.J * sNet) && (bear < muBear);
            contestedBear = (net < -P.J * sNet) && (bull < muBull);
        }
    }

    // ===== firing discipline: edge-trigger + cooldown + warmup =====
    // k: 0 CollapseBull(SG12), 1 CollapseBear(SG13), 2 ContestedBull(SG14), 3 ContestedBear(SG15)
    int cond[4] = { collapseBull, collapseBear, contestedBull, contestedBear };
    int sgIdx[4]   = { SG_CB, SG_CBR, SG_KB, SG_KBR };
    int dir[4]     = { +1, -1, +1, -1 };   // +1 bull marker (under zero), -1 bear (over)
    int family[4]  = { 0, 0, 1, 1 };       // 0 Collapse, 1 Contested

    for (int kk = 0; kk < 4; ++kk)
    {
        int fired = cond[kk] && !st->condPrev[kk] && warm && (t > st->cooldownUntil[kk]);

        // default: no marker this bar (0 is hidden via DrawZeros = 0)
        sc.Subgraph[sgIdx[kk]][t] = 0.0;

        if (fired)
        {
            int alignment = (slope == 0) ? 0 : ((dir[kk] * slope > 0) ? 1 : -1);
            int suppress  = (P.gateCounter && alignment < 0);
            if (!suppress)
            {
                double val = (dir[kk] > 0) ? -off : off;  // bull under, bear over
                sc.Subgraph[sgIdx[kk]][t] = val;
                sc.Subgraph[sgIdx[kk]].DataColor[t] = MarkerColor(family[kk], alignment);
            }
            st->cooldownUntil[kk] = t + P.cooldown;
        }
        st->condPrev[kk] = cond[kk] ? 1 : 0;
    }

    st->lastStatBar = t;
}

/*==========================================================================*/
SCSFExport scsf_DOMReaderV2(SCStudyInterfaceRef sc)
{
    SCSubgraphRef sgNet      = sc.Subgraph[SG_NET];
    SCSubgraphRef sgBidNet   = sc.Subgraph[SG_BIDNET];
    SCSubgraphRef sgAskNet   = sc.Subgraph[SG_ASKNET];
    SCSubgraphRef sgRaw      = sc.Subgraph[SG_RAW];
    SCSubgraphRef sgBidStack = sc.Subgraph[SG_BIDSTACK];
    SCSubgraphRef sgBidPull  = sc.Subgraph[SG_BIDPULL];
    SCSubgraphRef sgAskStack = sc.Subgraph[SG_ASKSTACK];
    SCSubgraphRef sgAskPull  = sc.Subgraph[SG_ASKPULL];
    SCSubgraphRef sgBull     = sc.Subgraph[SG_BULL];
    SCSubgraphRef sgBear     = sc.Subgraph[SG_BEAR];
    SCSubgraphRef sgBullRaw  = sc.Subgraph[SG_BULLRAW];
    SCSubgraphRef sgBearRaw  = sc.Subgraph[SG_BEARRAW];

    SCInputRef inMaxLevels = sc.Input[IN_MAXLVL];
    SCInputRef inSmoothPer = sc.Input[IN_SMOOTHPER];
    SCInputRef inSkipEmpty = sc.Input[IN_SKIPEMPTY];
    SCInputRef inSmoothOn  = sc.Input[IN_SMOOTHON];
    SCInputRef inW         = sc.Input[IN_W];
    SCInputRef inK         = sc.Input[IN_K];
    SCInputRef inM         = sc.Input[IN_M];
    SCInputRef inDecline   = sc.Input[IN_DECLINE];
    SCInputRef inReqOpp    = sc.Input[IN_REQOPP];
    SCInputRef inJ         = sc.Input[IN_J];
    SCInputRef inMinCon    = sc.Input[IN_MINCON];
    SCInputRef inCooldown  = sc.Input[IN_COOLDOWN];
    SCInputRef inWarmup    = sc.Input[IN_WARMUP];
    SCInputRef inSMALen    = sc.Input[IN_SMALEN];
    SCInputRef inSlopeDB   = sc.Input[IN_SLOPEDB];
    SCInputRef inGate      = sc.Input[IN_GATE];
    SCInputRef inFlowMode  = sc.Input[IN_FLOWMODE];
    SCInputRef inFlowMAType= sc.Input[IN_FLOWMATYPE];
    SCInputRef inFlowMALen = sc.Input[IN_FLOWMALEN];
    SCInputRef inFlowCumCnt= sc.Input[IN_FLOWCUMCNT];
    SCInputRef inFlowCumRst= sc.Input[IN_FLOWCUMRESET];

    if (sc.SetDefaults)
    {
        sc.GraphName     = "DOM Reader v2 (Conviction Butterfly)";
        sc.GraphRegion   = 1;
        sc.AutoLoop      = 0;
        sc.UpdateAlways  = 1;
        sc.UsesMarketDepthData = 1;
        sc.FreeDLL       = 0;
        sc.ScaleRangeType = SCALE_AUTO;

        // SG1 - headline fill-to-zero histogram (DRAWSTYLE_BAR draws each column
        // from 0 to value). Per-bar colour via DataColor[]. UNCHANGED from v1.
        sgNet.Name = "Net Pull/Stack";
        sgNet.DrawStyle = DRAWSTYLE_BAR;
        sgNet.LineWidth = 3;
        sgNet.PrimaryColor   = RGB(40, 130, 230);
        sgNet.SecondaryColor = RGB(220, 60, 60);
        sgNet.DrawZeros = 1;
        sgNet.AutoColoring = AUTOCOLOR_NONE;

        sgBidNet.Name = "Bid Net";          // SG2
        sgBidNet.DrawStyle = DRAWSTYLE_HIDDEN;
        sgBidNet.PrimaryColor = RGB(40, 130, 230); sgBidNet.DrawZeros = 1;

        sgAskNet.Name = "Ask Net";          // SG3
        sgAskNet.DrawStyle = DRAWSTYLE_HIDDEN;
        sgAskNet.PrimaryColor = RGB(220, 60, 60); sgAskNet.DrawZeros = 1;

        sgRaw.Name = "Raw Net (internal)";  // SG4
        sgRaw.DrawStyle = DRAWSTYLE_IGNORE; sgRaw.DrawZeros = 1;

        sgBidStack.Name = "Bid Stack";      // SG5
        sgBidStack.DrawStyle = DRAWSTYLE_HIDDEN;
        sgBidStack.PrimaryColor = RGB(60, 170, 110); sgBidStack.DrawZeros = 1;

        sgBidPull.Name = "Bid Pull";        // SG6
        sgBidPull.DrawStyle = DRAWSTYLE_HIDDEN;
        sgBidPull.PrimaryColor = RGB(200, 120, 60); sgBidPull.DrawZeros = 1;

        sgAskStack.Name = "Ask Stack";      // SG7
        sgAskStack.DrawStyle = DRAWSTYLE_HIDDEN;
        sgAskStack.PrimaryColor = RGB(200, 80, 80); sgAskStack.DrawZeros = 1;

        sgAskPull.Name = "Ask Pull";        // SG8
        sgAskPull.DrawStyle = DRAWSTYLE_HIDDEN;
        sgAskPull.PrimaryColor = RGB(80, 160, 200); sgAskPull.DrawZeros = 1;

        sgBull.Name = "Bull Flow";          // SG9 - visible line (draws up), display-transformed
        sgBull.DrawStyle = DRAWSTYLE_HIDDEN;
        sgBull.PrimaryColor = RGB(40, 130, 230); sgBull.LineWidth = 2;
        sgBull.DrawZeros = 1;

        sgBear.Name = "Bear Flow";          // SG10 - visible line (draws up, positive mag), display-transformed
        sgBear.DrawStyle = DRAWSTYLE_HIDDEN;
        sgBear.PrimaryColor = RGB(220, 60, 60); sgBear.LineWidth = 2;
        sgBear.DrawZeros = 1;

        sgBullRaw.Name = "Bull Flow Raw";   // SG11 - internal raw magnitude (stats/triggers source)
        sgBullRaw.DrawStyle = DRAWSTYLE_IGNORE; sgBullRaw.DrawZeros = 1;

        sgBearRaw.Name = "Bear Flow Raw";   // SG12 - internal raw magnitude (stats/triggers source)
        sgBearRaw.DrawStyle = DRAWSTYLE_IGNORE; sgBearRaw.DrawZeros = 1;

        // Markers SG12-15: arrows, only drawn on fired bars (DrawZeros = 0).
        sc.Subgraph[SG_CB].Name  = "Collapse - Bull";   // SG13
        sc.Subgraph[SG_CB].DrawStyle = DRAWSTYLE_ARROW_UP;
        sc.Subgraph[SG_CB].PrimaryColor = RGB(255, 140, 0);
        sc.Subgraph[SG_CB].LineWidth = 2; sc.Subgraph[SG_CB].DrawZeros = 0;

        sc.Subgraph[SG_CBR].Name = "Collapse - Bear";   // SG14
        sc.Subgraph[SG_CBR].DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sc.Subgraph[SG_CBR].PrimaryColor = RGB(255, 140, 0);
        sc.Subgraph[SG_CBR].LineWidth = 2; sc.Subgraph[SG_CBR].DrawZeros = 0;

        sc.Subgraph[SG_KB].Name  = "Contested - Bull";  // SG15
        sc.Subgraph[SG_KB].DrawStyle = DRAWSTYLE_ARROW_UP;
        sc.Subgraph[SG_KB].PrimaryColor = RGB(150, 80, 220);
        sc.Subgraph[SG_KB].LineWidth = 2; sc.Subgraph[SG_KB].DrawZeros = 0;

        sc.Subgraph[SG_KBR].Name = "Contested - Bear";  // SG16
        sc.Subgraph[SG_KBR].DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sc.Subgraph[SG_KBR].PrimaryColor = RGB(150, 80, 220);
        sc.Subgraph[SG_KBR].LineWidth = 2; sc.Subgraph[SG_KBR].DrawZeros = 0;

        sc.Subgraph[SG_SLOPE].Name = "SMA Slope State (internal)";  // SG17
        sc.Subgraph[SG_SLOPE].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[SG_SLOPE].DrawZeros = 1;

        // ---- existing inputs (unchanged) ----
        inMaxLevels.Name = "Max Levels";
        inMaxLevels.SetInt(16); inMaxLevels.SetIntLimits(1, LEVEL_CAP);
        inSmoothPer.Name = "Smoothing Period";
        inSmoothPer.SetInt(5); inSmoothPer.SetIntLimits(1, 1000);
        inSkipEmpty.Name = "Skip Empty Levels"; inSkipEmpty.SetYesNo(1);
        inSmoothOn.Name = "Smoothing Enabled";  inSmoothOn.SetYesNo(1);

        // ---- v2 inputs ----
        inW.Name = "Event Lookback (W)"; inW.SetInt(6); inW.SetIntLimits(2, 50);
        inK.Name = "Tall/Hot Sigma (k)"; inK.SetFloat(2.0f);
        inM.Name = "Collapse Drop Sigma (M)"; inM.SetFloat(2.0f);
        inDecline.Name = "Collapse Decline Span"; inDecline.SetInt(3); inDecline.SetIntLimits(1, 20);
        inReqOpp.Name = "Require Opposite Holds"; inReqOpp.SetYesNo(1);
        inJ.Name = "Net Breakout Sigma (J)"; inJ.SetFloat(1.5f);
        inMinCon.Name = "Min Contested Bars (m)"; inMinCon.SetInt(2); inMinCon.SetIntLimits(1, 20);
        inCooldown.Name = "Cooldown Bars"; inCooldown.SetInt(5); inCooldown.SetIntLimits(0, 100);
        inWarmup.Name = "Warmup Bars"; inWarmup.SetInt(30); inWarmup.SetIntLimits(1, 500);
        inSMALen.Name = "SMA Length (L)"; inSMALen.SetInt(10); inSMALen.SetIntLimits(2, 200);
        inSlopeDB.Name = "Slope Deadband (ticks)"; inSlopeDB.SetFloat(2.0f);
        inGate.Name = "Gate Counter Triggers"; inGate.SetYesNo(0);

        // ---- v2.1 flow display inputs (display-only; never feed stats/triggers) ----
        inFlowMode.Name = "Flow Calculation Mode";
        inFlowMode.SetCustomInputStrings("Raw Data;Moving Average;Cumulative");
        inFlowMode.SetCustomInputIndex(0);   // default Raw Data
        inFlowMAType.Name = "Flow MA Type";
        inFlowMAType.SetMovAvgType(MOVAVGTYPE_WEIGHTED);
        inFlowMALen.Name = "Flow MA Length";
        inFlowMALen.SetInt(50); inFlowMALen.SetIntLimits(1, 1000);
        inFlowCumCnt.Name = "Flow Cumulative Bar Count";
        inFlowCumCnt.SetInt(10); inFlowCumCnt.SetIntLimits(1, 1000);
        inFlowCumRst.Name = "Flow Cumulative Reset At Session";
        inFlowCumRst.SetYesNo(0);

        return;
    }

    // ---- persistent state ----
    s_DomState* st = (s_DomState*)sc.GetPersistentPointer(PP_STATE);

    if (sc.LastCallToFunction)
    {
        if (st != NULL)
        {
            delete[] st->rawCache;      delete[] st->bidCache;      delete[] st->askCache;
            delete[] st->bullCache;     delete[] st->bearCache;
            delete[] st->bidStackCache; delete[] st->bidPullCache;
            delete[] st->askStackCache; delete[] st->askPullCache;
            delete st;
            sc.SetPersistentPointer(PP_STATE, NULL);
        }
        return;
    }

    if (st == NULL)
    {
        st = new s_DomState();
        st->prevBidN = 0; st->prevAskN = 0;
        st->bidStackAccum = st->bidPullAccum = st->askStackAccum = st->askPullAccum = 0.0;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = -1; st->hasPrev = 0; st->depthWarned = 0; st->infoLogged = 0;
        st->rawCache = st->bidCache = st->askCache = NULL;
        st->bullCache = st->bearCache = NULL;
        st->bidStackCache = st->bidPullCache = st->askStackCache = st->askPullCache = NULL;
        st->cacheCap = 0; st->cacheCount = 0;
        ResetWelford(st);
        st->sessionDate = -1; st->lastStatBar = -1;   // -1 = no session yet (dates are positive)
        for (int i = 0; i < 4; ++i) { st->condPrev[i] = 0; st->cooldownUntil[i] = -1; }
        sc.SetPersistentPointer(PP_STATE, st);
    }

    // ---- read params ----
    const int    maxLevels = inMaxLevels.GetInt();
    const int    smoothPer = inSmoothPer.GetInt();
    const int    skipEmpty = inSkipEmpty.GetYesNo();
    const int    smoothOn  = inSmoothOn.GetYesNo();
    const double tickSize  = (sc.TickSize > 0.0) ? sc.TickSize : 1.0;

    s_Par P;
    P.W = inW.GetInt(); P.k = inK.GetFloat(); P.M = inM.GetFloat();
    P.declineSpan = inDecline.GetInt(); P.reqOpp = inReqOpp.GetYesNo();
    P.J = inJ.GetFloat(); P.minContested = inMinCon.GetInt();
    P.cooldown = inCooldown.GetInt(); P.warmup = inWarmup.GetInt();
    P.smaLen = inSMALen.GetInt(); P.slopeDeadT = inSlopeDB.GetFloat();
    P.gateCounter = inGate.GetYesNo(); P.tickSize = tickSize;

    // flow display params (v2.1) — display transform only
    const int flowMode  = inFlowMode.GetIndex();           // 0 Raw, 1 MA, 2 Cumulative
    const MovAvgTypeEnum flowMAType = (MovAvgTypeEnum)inFlowMAType.GetMovAvgType();
    const int flowMALen = inFlowMALen.GetInt();
    const int flowCumCnt= inFlowCumCnt.GetInt();
    const int flowCumRst= inFlowCumRst.GetYesNo();

    if (!st->infoLogged)
    {
        sc.AddMessageToLog(
            "DOM Reader v2: depth-derived series (atoms, conviction, triggers) are "
            "FORWARD-ONLY and repaint from cache on settings change. The SMA "
            "directional filter is trade data and paints full history.", 0);
        st->infoLogged = 1;
    }

    // ---- full recalculation: repaint depth series + replay stats/triggers ----
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        // reset forward diff state (depth cannot be replayed)
        st->prevBidN = 0; st->prevAskN = 0;
        st->bidStackAccum = st->bidPullAccum = st->askStackAccum = st->askPullAccum = 0.0;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = -1; st->hasPrev = 0;
        // cache + depthWarned preserved.

        // reset stats + triggers for deterministic replay
        ResetWelford(st);
        st->sessionDate = -1; st->lastStatBar = -1;
        for (int i = 0; i < 4; ++i) { st->condPrev[i] = 0; st->cooldownUntil[i] = -1; }

        int n = min(st->cacheCount, sc.ArraySize);

        // 1) repaint depth-derived display arrays for all cached bars
        for (int i = 0; i < n; ++i)
        {
            sgRaw[i]      = st->rawCache[i];
            sgBidNet[i]   = st->bidCache[i];
            sgAskNet[i]   = st->askCache[i];
            sgBidStack[i] = st->bidStackCache[i];
            sgBidPull[i]  = st->bidPullCache[i];
            sgAskStack[i] = st->askStackCache[i];
            sgAskPull[i]  = st->askPullCache[i];
            sgBullRaw[i]  = st->bullCache[i];      // raw magnitude (stats/triggers source)
            sgBearRaw[i]  = st->bearCache[i];

            double outVal = st->rawCache[i];
            if (smoothOn && smoothPer > 1)
            {
                int start = i - (smoothPer - 1); if (start < 0) start = 0;
                double sum = 0.0; int cnt = 0;
                for (int kk = start; kk <= i; ++kk) { sum += st->rawCache[kk]; cnt++; }
                if (cnt > 0) outVal = sum / (double)cnt;
            }
            sgNet[i] = outVal;
            sgNet.DataColor[i] = (outVal >= 0.0) ? RGB(40, 130, 230) : RGB(220, 60, 60);
        }

        // 2) flow display transform across all cached bars (raw is now filled,
        //    so MA / Cumulative recompute deterministically -> history repaints)
        for (int i = 0; i < n; ++i)
        {
            ApplyFlowDisplay(sc, flowMode, flowMAType, flowMALen, flowCumCnt, flowCumRst,
                             sgBullRaw, sgBull, i);
            ApplyFlowDisplay(sc, flowMode, flowMAType, flowMALen, flowCumCnt, flowCumRst,
                             sgBearRaw, sgBear, i);
        }

        // 3) SMA filter is native price data -> paint full history (all bars)
        for (int i = 0; i < sc.ArraySize; ++i)
            sc.Subgraph[SG_SLOPE][i] = (double)SlopeState(sc, i, P.smaLen, P.slopeDeadT);

        // 4) replay stats + triggers on fully-closed cached bars (exclude the
        //    still-forming last cached bar; it re-processes when it truly closes)
        for (int i = 0; i <= n - 2; ++i)
            ProcessClosedBar(sc, st, P, i);
    }

    const int formingIdx = sc.ArraySize - 1;
    if (formingIdx < 0) return;

    // ---- snapshot current book (top N levels, keyed by price->tick) ----
    s_Lvl curBid[LEVEL_CAP]; int curBidN = 0;
    s_Lvl curAsk[LEVEL_CAP]; int curAskN = 0;

    int wantBid = min(maxLevels, sc.GetBidMarketDepthNumberOfLevels());
    int wantAsk = min(maxLevels, sc.GetAskMarketDepthNumberOfLevels());
    if (wantBid > LEVEL_CAP) wantBid = LEVEL_CAP;
    if (wantAsk > LEVEL_CAP) wantAsk = LEVEL_CAP;

    for (int i = 0; i < wantBid; ++i)
    {
        s_MarketDepthEntry e;
        if (!sc.GetBidMarketDepthEntryAtLevel(e, i)) continue;
        double q = (double)e.Quantity;
        if (skipEmpty && q <= 0.0) continue;
        curBid[curBidN].tick = (int)round((double)e.Price / tickSize);
        curBid[curBidN].qty  = q; curBidN++;
    }
    for (int i = 0; i < wantAsk; ++i)
    {
        s_MarketDepthEntry e;
        if (!sc.GetAskMarketDepthEntryAtLevel(e, i)) continue;
        double q = (double)e.Quantity;
        if (skipEmpty && q <= 0.0) continue;
        curAsk[curAskN].tick = (int)round((double)e.Price / tickSize);
        curAsk[curAskN].qty  = q; curAskN++;
    }

    // ---- depth availability guard ----
    if (curBidN == 0 && curAskN == 0)
    {
        if (!sc.IsFullRecalculation && st->depthWarned == 0)
        {
            sc.AddMessageToLog(
                "DOM Reader v2: no market depth data available for this symbol/feed. "
                "Output will remain flat. Depth is real-time and forward-only.", 1);
            st->depthWarned = 1;
        }
        return;
    }

    // ---- bar roll: run the CLOSE HOOK on the just-closed bar, then reset ----
    if (st->hasPrev && formingIdx > st->curBarIndex)
    {
        // The closed bar (st->curBarIndex) already holds finalized live values
        // in the subgraphs and cache. Run §3/§4/§7 before zeroing accumulators.
        ProcessClosedBar(sc, st, P, st->curBarIndex);

        st->bidStackAccum = st->bidPullAccum = st->askStackAccum = st->askPullAccum = 0.0;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = formingIdx;
    }

    // ---- first snapshot: establish baseline, contribute nothing ----
    if (!st->hasPrev)
    {
        for (int i = 0; i < curBidN; ++i) st->prevBid[i] = curBid[i];
        for (int i = 0; i < curAskN; ++i) st->prevAsk[i] = curAsk[i];
        st->prevBidN = curBidN; st->prevAskN = curAskN;
        st->bidStackAccum = st->bidPullAccum = st->askStackAccum = st->askPullAccum = 0.0;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = formingIdx; st->hasPrev = 1;

        EnsureCacheCap(st, formingIdx + 1);
        sgRaw[formingIdx] = sgBidNet[formingIdx] = sgAskNet[formingIdx] = 0.0;
        sgBidStack[formingIdx] = sgBidPull[formingIdx] = 0.0;
        sgAskStack[formingIdx] = sgAskPull[formingIdx] = 0.0;
        sgBull[formingIdx] = sgBear[formingIdx] = 0.0;
        sgBullRaw[formingIdx] = sgBearRaw[formingIdx] = 0.0;
        sgNet[formingIdx] = 0.0; sgNet.DataColor[formingIdx] = sgNet.PrimaryColor;
        sc.Subgraph[SG_SLOPE][formingIdx] = (double)SlopeState(sc, formingIdx, P.smaLen, P.slopeDeadT);

        st->rawCache[formingIdx] = st->bidCache[formingIdx] = st->askCache[formingIdx] = 0.0;
        st->bullCache[formingIdx] = st->bearCache[formingIdx] = 0.0;
        st->bidStackCache[formingIdx] = st->bidPullCache[formingIdx] = 0.0;
        st->askStackCache[formingIdx] = st->askPullCache[formingIdx] = 0.0;
        if (formingIdx + 1 > st->cacheCount) st->cacheCount = formingIdx + 1;
        return;
    }

    // ======================================================================
    // BOOK-SHIFT-AWARE DIFF, split into positive-magnitude atoms.
    // Membership rules (unchanged from v1):
    //   * price in BOTH  -> d = cur - prev   (d>0 stack, d<0 pull)
    //   * price in CUR only (entered window because inside market moved) -> 0
    //   * price in PREV only (vanished):
    //        - still inside current [minTick..maxTick] -> genuine pull (d=-prev)
    //        - outside -> window moved away -> 0
    // Range membership (not slot adjacency) handles multi-tick inside gaps.
    // ======================================================================
    double bidStackD = 0.0, bidPullD = 0.0, askStackD = 0.0, askPullD = 0.0;

    if (curBidN > 0)
    {
        int loT = curBid[0].tick, hiT = curBid[0].tick;
        for (int i = 1; i < curBidN; ++i)
        {
            if (curBid[i].tick < loT) loT = curBid[i].tick;
            if (curBid[i].tick > hiT) hiT = curBid[i].tick;
        }
        for (int i = 0; i < curBidN; ++i)
        {
            double pq = FindQty(st->prevBid, st->prevBidN, curBid[i].tick);
            if (pq >= 0.0)
            {
                double d = curBid[i].qty - pq;
                if (d > 0.0) bidStackD += d; else if (d < 0.0) bidPullD += -d;
            }
        }
        for (int i = 0; i < st->prevBidN; ++i)
        {
            int t = st->prevBid[i].tick;
            if (FindQty(curBid, curBidN, t) < 0.0)
                if (t >= loT && t <= hiT) bidPullD += st->prevBid[i].qty;  // d = -prev
        }
    }

    if (curAskN > 0)
    {
        int loT = curAsk[0].tick, hiT = curAsk[0].tick;
        for (int i = 1; i < curAskN; ++i)
        {
            if (curAsk[i].tick < loT) loT = curAsk[i].tick;
            if (curAsk[i].tick > hiT) hiT = curAsk[i].tick;
        }
        for (int i = 0; i < curAskN; ++i)
        {
            double pq = FindQty(st->prevAsk, st->prevAskN, curAsk[i].tick);
            if (pq >= 0.0)
            {
                double d = curAsk[i].qty - pq;
                if (d > 0.0) askStackD += d; else if (d < 0.0) askPullD += -d;
            }
        }
        for (int i = 0; i < st->prevAskN; ++i)
        {
            int t = st->prevAsk[i].tick;
            if (FindQty(curAsk, curAskN, t) < 0.0)
                if (t >= loT && t <= hiT) askPullD += st->prevAsk[i].qty;
        }
    }

    // ---- accumulate atoms into the forming bar ----
    st->bidStackAccum += bidStackD; st->bidPullAccum += bidPullD;
    st->askStackAccum += askStackD; st->askPullAccum += askPullD;

    // derived signed nets (invariant: net = stack - pull)
    st->bidNetAccum = st->bidStackAccum - st->bidPullAccum;
    st->askNetAccum = st->askStackAccum - st->askPullAccum;

    const double rawNet  = st->bidNetAccum - st->askNetAccum;
    const double bullFlow = st->bidStackAccum + st->askPullAccum;  // demand + offers vacating
    const double bearFlow = st->askStackAccum + st->bidPullAccum;  // supply + bids vacating
    // invariant: bullFlow - bearFlow == rawNet (see consistency check, header §10)

    const int bi = st->curBarIndex;

    sgRaw[bi]      = rawNet;
    sgBidNet[bi]   = st->bidNetAccum;
    sgAskNet[bi]   = st->askNetAccum;
    sgBidStack[bi] = st->bidStackAccum;
    sgBidPull[bi]  = st->bidPullAccum;
    sgAskStack[bi] = st->askStackAccum;
    sgAskPull[bi]  = st->askPullAccum;
    sgBullRaw[bi]  = bullFlow;   // raw magnitude (stats/triggers source)
    sgBearRaw[bi]  = bearFlow;
    ApplyFlowDisplay(sc, flowMode, flowMAType, flowMALen, flowCumCnt, flowCumRst,
                     sgBullRaw, sgBull, bi);
    ApplyFlowDisplay(sc, flowMode, flowMAType, flowMALen, flowCumCnt, flowCumRst,
                     sgBearRaw, sgBear, bi);
    sc.Subgraph[SG_SLOPE][bi] = (double)SlopeState(sc, bi, P.smaLen, P.slopeDeadT);

    // mirror into persistent cache
    EnsureCacheCap(st, bi + 1);
    st->rawCache[bi]      = rawNet;
    st->bidCache[bi]      = st->bidNetAccum;
    st->askCache[bi]      = st->askNetAccum;
    st->bullCache[bi]     = bullFlow;
    st->bearCache[bi]     = bearFlow;
    st->bidStackCache[bi] = st->bidStackAccum;
    st->bidPullCache[bi]  = st->bidPullAccum;
    st->askStackCache[bi] = st->askStackAccum;
    st->askPullCache[bi]  = st->askPullAccum;
    if (bi + 1 > st->cacheCount) st->cacheCount = bi + 1;

    // ---- SG1 smoothing (SMA of raw netPressure) — UNCHANGED from v1 ----
    double outVal = rawNet;
    if (smoothOn && smoothPer > 1)
    {
        int start = bi - (smoothPer - 1); if (start < 0) start = 0;
        double sum = 0.0; int cnt = 0;
        for (int k = start; k <= bi; ++k) { sum += sgRaw[k]; cnt++; }
        if (cnt > 0) outVal = sum / (double)cnt;
    }
    sgNet[bi] = outVal;
    sgNet.DataColor[bi] = (outVal >= 0.0) ? RGB(40, 130, 230) : RGB(220, 60, 60);

    // ---- consistency check (header §10): bull - bear must equal raw ----
    if (fabs((bullFlow - bearFlow) - rawNet) > 1e-6 && st->depthWarned < 2)
    {
        sc.AddMessageToLog("DOM Reader v2: atom invariant broken (bull-bear != rawNet).", 1);
        st->depthWarned = 2;  // log once (distinct from no-depth sentinel)
    }

    // ---- persist current snapshot as the new "previous" ----
    for (int i = 0; i < curBidN; ++i) st->prevBid[i] = curBid[i];
    for (int i = 0; i < curAskN; ++i) st->prevAsk[i] = curAsk[i];
    st->prevBidN = curBidN; st->prevAskN = curAskN;
}
