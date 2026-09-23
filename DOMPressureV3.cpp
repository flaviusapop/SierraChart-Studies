#include "sierrachart.h"

// ============================================================================
// DOMPressureV3.cpp
//
// v3 = slim rebuild of DOM Reader v2, keeping only what is actually used:
// the book-shift-aware depth diff and its three outputs (Net Pull/Stack
// histogram, Bid Net, Ask Net). Everything else from v2 (stack/pull atoms,
// conviction butterfly, Welford stats, Collapse/Contested triggers, SMA slope
// filter, flow display modes) is removed.
//
// WHAT v3 ADDS over the v1 core:
//   SG5 Normalized Net = smoothed net / rolling mean of |smoothed net| over a
//   lookback. Adaptive "how unusual is this stacking" value, intended as the
//   source for the companion study DOMPressureBarColor (DRAWSTYLE_COLOR_BAR
//   in the price region). ~1.0 = average magnitude; 2-3 = strong event.
//
// FORWARD-ONLY: depth cannot be reconstructed from history. The Option-A
// persistent cache is preserved from v2 so a settings change / full recalc
// repaints the accumulated depth history instead of wiping it.
//
// Subgraphs (UI is 1-based):
//   SG1 Net Pull/Stack  - fill-to-zero histogram, smoothed, colored by sign
//   SG2 Bid Net         - hidden by default
//   SG3 Ask Net         - hidden by default
//   SG4 Raw Net         - internal (unsmoothed), IGNORE
//   SG5 Normalized Net  - hidden; source for the bar-color companion study
//
// Author: built for Flavius' NQ intraday ACSIL workflow.
// ============================================================================

SCDLLName("DOMPressureV3")

// ---- Persistent-pointer slot for our cross-call state -----------------------
const int PP_STATE = 1;

// Hard capacity for the per-side level snapshot arrays. MaxLevels is clamped.
const int LEVEL_CAP = 256;

// Subgraph code indices (UI shows these +1, i.e. SG1..SG5).
const int SG_NET = 0, SG_BIDNET = 1, SG_ASKNET = 2, SG_RAW = 3, SG_NORM = 4;

// Input code indices.
const int IN_MAXLVL = 0, IN_SMOOTHPER = 1, IN_SKIPEMPTY = 2, IN_SMOOTHON = 3,
          IN_NORMLOOK = 4;

// One resting-order level: price as integer-tick key + size.
struct s_Lvl { int tick; double qty; };

// Persistent state across study calls.
struct s_DomState
{
    // --- forward depth-diff state ---
    s_Lvl  prevBid[LEVEL_CAP]; int prevBidN;
    s_Lvl  prevAsk[LEVEL_CAP]; int prevAskN;

    // per-bar accumulators (signed nets = stack - pull, per side)
    double bidNetAccum, askNetAccum;

    int    curBarIndex;
    int    hasPrev;
    int    depthWarned;
    int    infoLogged;

    // --- history cache (Option A): survives settings-change full recalcs ---
    double *rawCache, *bidCache, *askCache;
    int    cacheCap, cacheCount;
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
    st->rawCache = GrowArr(st->rawCache, st->cacheCount, newCap);
    st->bidCache = GrowArr(st->bidCache, st->cacheCount, newCap);
    st->askCache = GrowArr(st->askCache, st->cacheCount, newCap);
    st->cacheCap = newCap;
}

// SMA of the raw net over [i-per+1 .. i], reading from the raw cache.
static double SmoothedNet(const s_DomState* st, int i, int smoothOn, int per)
{
    if (!smoothOn || per <= 1) return st->rawCache[i];
    int start = i - (per - 1); if (start < 0) start = 0;
    double sum = 0.0; int cnt = 0;
    for (int k = start; k <= i; ++k) { sum += st->rawCache[k]; cnt++; }
    return (cnt > 0) ? sum / (double)cnt : st->rawCache[i];
}

// Normalized net at bar i: sgNet[i] / rolling mean of |sgNet| over lookback.
// Adaptive to regime: ~1.0 = typical magnitude, 2-3 = strong stacking event.
static double NormalizedNet(SCSubgraphRef sgNet, int i, int lookback)
{
    int start = i - (lookback - 1); if (start < 0) start = 0;
    double sumAbs = 0.0; int cnt = 0;
    for (int k = start; k <= i; ++k) { sumAbs += fabs(sgNet[k]); cnt++; }
    if (cnt == 0) return 0.0;
    double denom = sumAbs / (double)cnt;
    if (denom < 1e-9) return 0.0;
    return sgNet[i] / denom;
}

/*==========================================================================*/
SCSFExport scsf_DOMPressureV3(SCStudyInterfaceRef sc)
{
    SCSubgraphRef sgNet    = sc.Subgraph[SG_NET];
    SCSubgraphRef sgBidNet = sc.Subgraph[SG_BIDNET];
    SCSubgraphRef sgAskNet = sc.Subgraph[SG_ASKNET];
    SCSubgraphRef sgRaw    = sc.Subgraph[SG_RAW];
    SCSubgraphRef sgNorm   = sc.Subgraph[SG_NORM];

    SCInputRef inMaxLevels = sc.Input[IN_MAXLVL];
    SCInputRef inSmoothPer = sc.Input[IN_SMOOTHPER];
    SCInputRef inSkipEmpty = sc.Input[IN_SKIPEMPTY];
    SCInputRef inSmoothOn  = sc.Input[IN_SMOOTHON];
    SCInputRef inNormLook  = sc.Input[IN_NORMLOOK];

    if (sc.SetDefaults)
    {
        sc.GraphName     = "DOM Pressure v3";
        sc.GraphRegion   = 1;
        sc.AutoLoop      = 0;
        sc.UpdateAlways  = 1;
        sc.UsesMarketDepthData = 1;
        sc.FreeDLL       = 0;
        sc.ScaleRangeType = SCALE_AUTO;

        // SG1 - headline fill-to-zero histogram (DRAWSTYLE_BAR draws each
        // column from 0 to value). Per-bar colour via DataColor[].
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

        sgNorm.Name = "Normalized Net";     // SG5 - bar-color companion source
        sgNorm.DrawStyle = DRAWSTYLE_HIDDEN;
        sgNorm.PrimaryColor = RGB(200, 200, 200); sgNorm.DrawZeros = 1;

        inMaxLevels.Name = "Max Levels";
        inMaxLevels.SetInt(16); inMaxLevels.SetIntLimits(1, LEVEL_CAP);
        inSmoothPer.Name = "Smoothing Period";
        inSmoothPer.SetInt(5); inSmoothPer.SetIntLimits(1, 1000);
        inSkipEmpty.Name = "Skip Empty Levels"; inSkipEmpty.SetYesNo(1);
        inSmoothOn.Name = "Smoothing Enabled";  inSmoothOn.SetYesNo(1);
        inNormLook.Name = "Normalization Lookback (bars)";
        inNormLook.SetInt(50); inNormLook.SetIntLimits(2, 1000);

        return;
    }

    // ---- persistent state ----
    s_DomState* st = (s_DomState*)sc.GetPersistentPointer(PP_STATE);

    if (sc.LastCallToFunction)
    {
        if (st != NULL)
        {
            delete[] st->rawCache; delete[] st->bidCache; delete[] st->askCache;
            delete st;
            sc.SetPersistentPointer(PP_STATE, NULL);
        }
        return;
    }

    if (st == NULL)
    {
        st = new s_DomState();
        st->prevBidN = 0; st->prevAskN = 0;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = -1; st->hasPrev = 0; st->depthWarned = 0; st->infoLogged = 0;
        st->rawCache = st->bidCache = st->askCache = NULL;
        st->cacheCap = 0; st->cacheCount = 0;
        sc.SetPersistentPointer(PP_STATE, st);
    }

    // ---- read params ----
    const int    maxLevels = inMaxLevels.GetInt();
    const int    smoothPer = inSmoothPer.GetInt();
    const int    skipEmpty = inSkipEmpty.GetYesNo();
    const int    smoothOn  = inSmoothOn.GetYesNo();
    const int    normLook  = inNormLook.GetInt();
    const double tickSize  = (sc.TickSize > 0.0) ? sc.TickSize : 1.0;

    if (!st->infoLogged)
    {
        sc.AddMessageToLog(
            "DOM Pressure v3: depth-derived series are FORWARD-ONLY and repaint "
            "from cache on settings change.", 0);
        st->infoLogged = 1;
    }

    // ---- full recalculation: repaint depth series from the cache ----
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        // reset forward diff state (depth cannot be replayed)
        st->prevBidN = 0; st->prevAskN = 0;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = -1; st->hasPrev = 0;
        // cache + depthWarned preserved.

        int n = min(st->cacheCount, sc.ArraySize);
        for (int i = 0; i < n; ++i)
        {
            sgRaw[i]    = st->rawCache[i];
            sgBidNet[i] = st->bidCache[i];
            sgAskNet[i] = st->askCache[i];

            double outVal = SmoothedNet(st, i, smoothOn, smoothPer);
            sgNet[i] = outVal;
            sgNet.DataColor[i] = (outVal >= 0.0) ? RGB(40, 130, 230) : RGB(220, 60, 60);
        }
        for (int i = 0; i < n; ++i)
            sgNorm[i] = NormalizedNet(sgNet, i, normLook);
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
                "DOM Pressure v3: no market depth data available for this symbol/feed. "
                "Output will remain flat. Depth is real-time and forward-only.", 1);
            st->depthWarned = 1;
        }
        return;
    }

    // ---- bar roll: reset accumulators for the new forming bar ----
    if (st->hasPrev && formingIdx > st->curBarIndex)
    {
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = formingIdx;
    }

    // ---- first snapshot: establish baseline, contribute nothing ----
    if (!st->hasPrev)
    {
        for (int i = 0; i < curBidN; ++i) st->prevBid[i] = curBid[i];
        for (int i = 0; i < curAskN; ++i) st->prevAsk[i] = curAsk[i];
        st->prevBidN = curBidN; st->prevAskN = curAskN;
        st->bidNetAccum = st->askNetAccum = 0.0;
        st->curBarIndex = formingIdx; st->hasPrev = 1;

        EnsureCacheCap(st, formingIdx + 1);
        sgRaw[formingIdx] = sgBidNet[formingIdx] = sgAskNet[formingIdx] = 0.0;
        sgNorm[formingIdx] = 0.0;
        sgNet[formingIdx] = 0.0; sgNet.DataColor[formingIdx] = sgNet.PrimaryColor;

        st->rawCache[formingIdx] = st->bidCache[formingIdx] = st->askCache[formingIdx] = 0.0;
        if (formingIdx + 1 > st->cacheCount) st->cacheCount = formingIdx + 1;
        return;
    }

    // ======================================================================
    // BOOK-SHIFT-AWARE DIFF (unchanged from v1/v2), collapsed to signed nets.
    // Membership rules:
    //   * price in BOTH  -> d = cur - prev   (d>0 stack, d<0 pull)
    //   * price in CUR only (entered window because inside market moved) -> 0
    //   * price in PREV only (vanished):
    //        - still inside current [minTick..maxTick] -> genuine pull (d=-prev)
    //        - outside -> window moved away -> 0
    // Range membership (not slot adjacency) handles multi-tick inside gaps.
    // ======================================================================
    double bidNetD = 0.0, askNetD = 0.0;

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
            if (pq >= 0.0) bidNetD += curBid[i].qty - pq;
        }
        for (int i = 0; i < st->prevBidN; ++i)
        {
            int t = st->prevBid[i].tick;
            if (FindQty(curBid, curBidN, t) < 0.0)
                if (t >= loT && t <= hiT) bidNetD -= st->prevBid[i].qty;
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
            if (pq >= 0.0) askNetD += curAsk[i].qty - pq;
        }
        for (int i = 0; i < st->prevAskN; ++i)
        {
            int t = st->prevAsk[i].tick;
            if (FindQty(curAsk, curAskN, t) < 0.0)
                if (t >= loT && t <= hiT) askNetD -= st->prevAsk[i].qty;
        }
    }

    // ---- accumulate into the forming bar ----
    st->bidNetAccum += bidNetD;
    st->askNetAccum += askNetD;

    const double rawNet = st->bidNetAccum - st->askNetAccum;
    const int bi = st->curBarIndex;

    sgRaw[bi]    = rawNet;
    sgBidNet[bi] = st->bidNetAccum;
    sgAskNet[bi] = st->askNetAccum;

    // mirror into persistent cache
    EnsureCacheCap(st, bi + 1);
    st->rawCache[bi] = rawNet;
    st->bidCache[bi] = st->bidNetAccum;
    st->askCache[bi] = st->askNetAccum;
    if (bi + 1 > st->cacheCount) st->cacheCount = bi + 1;

    // ---- SG1 smoothing (SMA of raw net) ----
    double outVal = SmoothedNet(st, bi, smoothOn, smoothPer);
    sgNet[bi] = outVal;
    sgNet.DataColor[bi] = (outVal >= 0.0) ? RGB(40, 130, 230) : RGB(220, 60, 60);

    // ---- SG5 normalized net (bar-color companion source) ----
    sgNorm[bi] = NormalizedNet(sgNet, bi, normLook);

    // ---- persist current snapshot as the new "previous" ----
    for (int i = 0; i < curBidN; ++i) st->prevBid[i] = curBid[i];
    for (int i = 0; i < curAskN; ++i) st->prevAsk[i] = curAsk[i];
    st->prevBidN = curBidN; st->prevAskN = curAskN;
}
