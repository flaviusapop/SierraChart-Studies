#include "sierrachart.h"

// ============================================================================
// DOMReader.cpp
//
// Port of the MotiveWave / OrderFlow Labs "DOM Reader" (Pull/Stack,
// History Mode = Delta). Measures liquidity intent from the order book:
// resting limit orders being STACKED (added) or PULLED (removed) across the
// top N price levels on each side, plotted as a fill-to-zero histogram
// oscillator. Blue = book tilts bullish, red = book tilts bearish.
//
// *** FORWARD-ONLY ***
// Market depth in Sierra Chart is real-time only. Historical bars CANNOT be
// reconstructed from depth, so this study populates only from the moment it
// starts running and accumulates depth state going forward. Historical bars
// to the left of the start remain flat/zero by design.
//
// History across settings changes (Option A): the live depth stream cannot be
// replayed, but the per-bar VALUES we compute are cached in persistent memory.
// A settings change triggers a full recalc that clears the subgraph arrays;
// persistent pointers are NOT cleared, so we repaint the cached series instead
// of wiping it. Smoothing is recomputed with the current period; Max Levels /
// Skip Empty changes apply only to bars computed after the change. The cache
// lives for as long as the study/chart is loaded (lost when SC closes).
//
// Author: built for Flavius' ES scalping ACSIL workflow.
// ============================================================================

SCDLLName("DOMReader")

// ---- Persistent-pointer slot for our cross-call state -----------------------
const int PP_STATE = 1;

// Hard capacity for the per-side level snapshot arrays. MaxLevels input is
// clamped to this. Kept generous so raising MaxLevels never overflows.
const int LEVEL_CAP = 256;

// One resting-order level: price expressed as an integer tick key + its size.
struct s_Lvl
{
    int    tick;
    double qty;
};

// All state that must persist between study calls. Allocated on the heap and
// held via sc.SetPersistentPointer so it survives across live depth updates.
struct s_DomState
{
    s_Lvl  prevBid[LEVEL_CAP];
    int    prevBidN;
    s_Lvl  prevAsk[LEVEL_CAP];
    int    prevAskN;

    double bidNetAccum;   // Sigma(size changes) on bid side, current forming bar
    double askNetAccum;   // Sigma(size changes) on ask side, current forming bar
    int    curBarIndex;   // bar index the accumulators belong to (forming bar)
    int    hasPrev;       // 0 until we have captured a first snapshot
    int    depthWarned;   // 0 until the one-time "no depth" log has been posted

    // ---- History cache (Option A: survive settings changes within a session) ----
    double* rawCache;     // raw netPressure per absolute bar index
    double* bidCache;     // bid net per bar index
    double* askCache;     // ask net per bar index
    int     cacheCap;     // allocated length of the cache arrays
    int     cacheCount;   // number of valid leading entries (= max written index + 1)
};

// Linear lookup of a tick in a level array. Returns the qty, or -1.0 if absent.
static double FindQty(const s_Lvl* arr, int n, int tick)
{
    for (int i = 0; i < n; ++i)
        if (arr[i].tick == tick)
            return arr[i].qty;
    return -1.0;
}

// Grow the history cache to hold at least 'needed' entries, preserving content.
static void EnsureCacheCap(s_DomState* st, int needed)
{
    if (needed <= st->cacheCap)
        return;
    int newCap = (st->cacheCap > 0) ? st->cacheCap : 1024;
    while (newCap < needed)
        newCap *= 2;
    double* nr = new double[newCap];
    double* nb = new double[newCap];
    double* na = new double[newCap];
    for (int i = 0; i < newCap; ++i) { nr[i] = 0.0; nb[i] = 0.0; na[i] = 0.0; }
    for (int i = 0; i < st->cacheCount; ++i)
    {
        nr[i] = st->rawCache[i];
        nb[i] = st->bidCache[i];
        na[i] = st->askCache[i];
    }
    delete[] st->rawCache;
    delete[] st->bidCache;
    delete[] st->askCache;
    st->rawCache = nr;
    st->bidCache = nb;
    st->askCache = na;
    st->cacheCap = newCap;
}

/*==========================================================================*/
SCSFExport scsf_DOMReader(SCStudyInterfaceRef sc)
{
    // ---- Subgraphs ----
    SCSubgraphRef sgNet     = sc.Subgraph[0];   // Net Pull/Stack (smoothed, plotted)
    SCSubgraphRef sgBidNet  = sc.Subgraph[1];   // Bid Net (optional, hidden)
    SCSubgraphRef sgAskNet  = sc.Subgraph[2];   // Ask Net (optional, hidden)
    SCSubgraphRef sgRaw     = sc.Subgraph[3];   // Raw Net (hidden storage for SMA)

    // ---- Inputs ----
    SCInputRef inMaxLevels  = sc.Input[0];
    SCInputRef inSmoothPer  = sc.Input[1];
    SCInputRef inSkipEmpty  = sc.Input[2];
    SCInputRef inSmoothOn   = sc.Input[3];

    if (sc.SetDefaults)
    {
        sc.GraphName     = "DOM Reader (Pull/Stack)";
        sc.GraphRegion   = 1;            // own subgraph region, separate from price
        sc.AutoLoop      = 0;            // manual last-bar indexing (we drive the loop)
        sc.UpdateAlways  = 1;            // receive live depth updates, not only trades
        sc.UsesMarketDepthData = 1;      // *** required to read the order book ***
        sc.FreeDLL       = 0;
        sc.ScaleRangeType = SCALE_AUTO;

        // SG0 - main output: fill-to-zero histogram (DRAWSTYLE_BAR draws each
        // column from 0 to the value). Per-bar color is set via DataColor[].
        sgNet.Name       = "Net Pull/Stack";
        sgNet.DrawStyle  = DRAWSTYLE_BAR;
        sgNet.LineWidth  = 3;            // bar thickness (approximates MW fill density)
        sgNet.PrimaryColor   = RGB(40, 130, 230);  // bullish blue (fallback)
        sgNet.SecondaryColor = RGB(220, 60, 60);   // bearish red  (fallback)
        sgNet.DrawZeros  = 1;
        sgNet.AutoColoring = AUTOCOLOR_NONE;        // we color manually per bar

        sgBidNet.Name      = "Bid Net";
        sgBidNet.DrawStyle = DRAWSTYLE_HIDDEN;      // optional, hidden by default
        sgBidNet.PrimaryColor = RGB(40, 130, 230);
        sgBidNet.LineWidth = 1;
        sgBidNet.DrawZeros = 1;

        sgAskNet.Name      = "Ask Net";
        sgAskNet.DrawStyle = DRAWSTYLE_HIDDEN;      // optional, hidden by default
        sgAskNet.PrimaryColor = RGB(220, 60, 60);
        sgAskNet.LineWidth = 1;
        sgAskNet.DrawZeros = 1;

        sgRaw.Name      = "Raw Net (internal)";
        sgRaw.DrawStyle = DRAWSTYLE_IGNORE;         // storage only, never drawn
        sgRaw.DrawZeros = 1;

        inMaxLevels.Name = "Max Levels";
        inMaxLevels.SetInt(16);
        inMaxLevels.SetIntLimits(1, LEVEL_CAP);

        inSmoothPer.Name = "Smoothing Period";
        inSmoothPer.SetInt(5);
        inSmoothPer.SetIntLimits(1, 1000);

        inSkipEmpty.Name = "Skip Empty Levels";
        inSkipEmpty.SetYesNo(1);

        inSmoothOn.Name = "Smoothing Enabled";
        inSmoothOn.SetYesNo(1);

        return;
    }

    // ---- Acquire / manage persistent state -------------------------------
    s_DomState* st = (s_DomState*)sc.GetPersistentPointer(PP_STATE);

    if (sc.LastCallToFunction)
    {
        if (st != NULL)
        {
            delete[] st->rawCache;
            delete[] st->bidCache;
            delete[] st->askCache;
            delete st;
            sc.SetPersistentPointer(PP_STATE, NULL);
        }
        return;
    }

    if (st == NULL)
    {
        st = new s_DomState();
        st->prevBidN    = 0;
        st->prevAskN    = 0;
        st->bidNetAccum = 0.0;
        st->askNetAccum = 0.0;
        st->curBarIndex = -1;
        st->hasPrev     = 0;
        st->depthWarned = 0;
        st->rawCache    = NULL;
        st->bidCache    = NULL;
        st->askCache    = NULL;
        st->cacheCap    = 0;
        st->cacheCount  = 0;
        sc.SetPersistentPointer(PP_STATE, st);
    }

    const int   maxLevels   = inMaxLevels.GetInt();
    const int   smoothPer   = inSmoothPer.GetInt();
    const int   skipEmpty   = inSkipEmpty.GetYesNo();
    const int   smoothOn    = inSmoothOn.GetYesNo();
    const double tickSize   = (sc.TickSize > 0.0) ? sc.TickSize : 1.0;

    // ---- Full recalculation -----------------------------------------------
    // A settings change (or chart reload) clears the subgraph arrays and lands
    // us here. The live depth stream cannot be replayed, so the FORWARD diff
    // state is reset — but the computed history is NOT lost: we repaint it from
    // the persistent cache. Smoothing is recomputed with the current period, so
    // changing Smoothing Period / Enabled re-applies cleanly across all history.
    // (Max Levels / Skip Empty changes affect only bars computed after the
    // change, since re-diffing the past would require the depth stream.)
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        st->prevBidN    = 0;
        st->prevAskN    = 0;
        st->bidNetAccum = 0.0;
        st->askNetAccum = 0.0;
        st->curBarIndex = -1;
        st->hasPrev     = 0;
        // NB: cache and depthWarned are intentionally preserved.

        int n = min(st->cacheCount, sc.ArraySize);
        for (int i = 0; i < n; ++i)
        {
            sgRaw[i]    = st->rawCache[i];
            sgBidNet[i] = st->bidCache[i];
            sgAskNet[i] = st->askCache[i];

            double outVal = st->rawCache[i];
            if (smoothOn && smoothPer > 1)
            {
                int start = i - (smoothPer - 1);
                if (start < 0) start = 0;
                double sum = 0.0; int cnt = 0;
                for (int k = start; k <= i; ++k) { sum += st->rawCache[k]; cnt++; }
                if (cnt > 0) outVal = sum / (double)cnt;
            }
            sgNet[i] = outVal;
            sgNet.DataColor[i] = (outVal >= 0.0) ? RGB(40, 130, 230)
                                                 : RGB(220, 60, 60);
        }
    }

    const int formingIdx = sc.ArraySize - 1;
    if (formingIdx < 0)
        return;

    // ---- Snapshot the current book (top N levels, keyed by price->tick) ----
    s_Lvl curBid[LEVEL_CAP]; int curBidN = 0;
    s_Lvl curAsk[LEVEL_CAP]; int curAskN = 0;

    const int nBidLvls = sc.GetBidMarketDepthNumberOfLevels();
    const int nAskLvls = sc.GetAskMarketDepthNumberOfLevels();

    int wantBid = min(maxLevels, nBidLvls);
    int wantAsk = min(maxLevels, nAskLvls);
    if (wantBid > LEVEL_CAP) wantBid = LEVEL_CAP;
    if (wantAsk > LEVEL_CAP) wantAsk = LEVEL_CAP;

    for (int i = 0; i < wantBid; ++i)
    {
        s_MarketDepthEntry e;
        if (!sc.GetBidMarketDepthEntryAtLevel(e, i))
            continue;
        double q = (double)e.Quantity;
        if (skipEmpty && q <= 0.0)
            continue;
        int tk = (int)round((double)e.Price / tickSize);
        curBid[curBidN].tick = tk;
        curBid[curBidN].qty  = q;
        curBidN++;
    }

    for (int i = 0; i < wantAsk; ++i)
    {
        s_MarketDepthEntry e;
        if (!sc.GetAskMarketDepthEntryAtLevel(e, i))
            continue;
        double q = (double)e.Quantity;
        if (skipEmpty && q <= 0.0)
            continue;
        int tk = (int)round((double)e.Price / tickSize);
        curAsk[curAskN].tick = tk;
        curAsk[curAskN].qty  = q;
        curAskN++;
    }

    // ---- Depth availability guard ----------------------------------------
    // If we truly have no book this update, do nothing (output stays flat).
    // Post a one-time note only when running live (not during the historical
    // recalc pass, where depth is legitimately absent).
    if (curBidN == 0 && curAskN == 0)
    {
        if (!sc.IsFullRecalculation && st->depthWarned == 0)
        {
            sc.AddMessageToLog(
                "DOM Reader: no market depth data available for this symbol/feed. "
                "Output will remain flat. Depth is real-time and forward-only.", 1);
            st->depthWarned = 1;
        }
        return;
    }

    // ---- Bar roll: finalize previous bar, reset accumulators --------------
    // Live values are written to the forming bar on every update below, so the
    // just-closed bar already holds its final value. Here we only advance the
    // accumulation target and zero the accumulators for the new bar.
    if (st->hasPrev && formingIdx > st->curBarIndex)
    {
        st->bidNetAccum = 0.0;
        st->askNetAccum = 0.0;
        st->curBarIndex = formingIdx;
    }

    // ---- First snapshot: establish baseline, contribute nothing -----------
    if (!st->hasPrev)
    {
        for (int i = 0; i < curBidN; ++i) st->prevBid[i] = curBid[i];
        for (int i = 0; i < curAskN; ++i) st->prevAsk[i] = curAsk[i];
        st->prevBidN    = curBidN;
        st->prevAskN    = curAskN;
        st->bidNetAccum = 0.0;
        st->askNetAccum = 0.0;
        st->curBarIndex = formingIdx;
        st->hasPrev     = 1;

        sgRaw[formingIdx]    = 0.0;
        sgBidNet[formingIdx] = 0.0;
        sgAskNet[formingIdx] = 0.0;
        sgNet[formingIdx]    = 0.0;
        sgNet.DataColor[formingIdx] = sgNet.PrimaryColor;

        EnsureCacheCap(st, formingIdx + 1);
        st->rawCache[formingIdx] = 0.0;
        st->bidCache[formingIdx] = 0.0;
        st->askCache[formingIdx] = 0.0;
        if (formingIdx + 1 > st->cacheCount) st->cacheCount = formingIdx + 1;
        return;
    }

    // ======================================================================
    // BOOK-SHIFT-AWARE DIFF  (the key correctness logic)
    // ----------------------------------------------------------------------
    // The tracked "window" is the top N levels per side. As the inside market
    // moves, price slots shift in and out of that window. We must NOT mistake
    // window movement for genuine pull/stack. Rules, applied per side:
    //
    //   * Price present in BOTH prev and cur snapshots:
    //         change = curSize - prevSize   (genuine stack if +, pull if -)
    //
    //   * Price in CUR but NOT in PREV (newly entered the window because the
    //     inside market moved toward it): contributes 0. It was always resting
    //     there, just outside our window — not a fresh stack.
    //
    //   * Price in PREV but NOT in CUR (vanished): distinguish by price range.
    //     The current window spans [minTick .. maxTick] of the cur snapshot.
    //         - If the vanished price is STILL INSIDE that range, the size went
    //           to zero while in view  => GENUINE PULL, contributes (0 - prev).
    //         - If it is OUTSIDE the range, the window simply moved away from it
    //           => contributes 0 (not a pull).
    //
    // Because membership is decided by PRICE RANGE (not slot adjacency), a
    // multi-tick gap in the inside market between updates is handled correctly.
    // ======================================================================

    double bidNet = 0.0;
    double askNet = 0.0;

    // --- Bid side ---
    if (curBidN > 0)
    {
        int bidMinTick = curBid[0].tick;
        int bidMaxTick = curBid[0].tick;
        for (int i = 1; i < curBidN; ++i)
        {
            if (curBid[i].tick < bidMinTick) bidMinTick = curBid[i].tick;
            if (curBid[i].tick > bidMaxTick) bidMaxTick = curBid[i].tick;
        }

        for (int i = 0; i < curBidN; ++i)
        {
            double pq = FindQty(st->prevBid, st->prevBidN, curBid[i].tick);
            if (pq >= 0.0)
                bidNet += (curBid[i].qty - pq);   // genuine change
            // else: entered the window -> contributes 0
        }

        for (int i = 0; i < st->prevBidN; ++i)
        {
            int t = st->prevBid[i].tick;
            if (FindQty(curBid, curBidN, t) < 0.0)
            {
                if (t >= bidMinTick && t <= bidMaxTick)
                    bidNet += (0.0 - st->prevBid[i].qty);  // genuine pull
                // else: window moved away from it -> contributes 0
            }
        }
    }

    // --- Ask side ---
    if (curAskN > 0)
    {
        int askMinTick = curAsk[0].tick;
        int askMaxTick = curAsk[0].tick;
        for (int i = 1; i < curAskN; ++i)
        {
            if (curAsk[i].tick < askMinTick) askMinTick = curAsk[i].tick;
            if (curAsk[i].tick > askMaxTick) askMaxTick = curAsk[i].tick;
        }

        for (int i = 0; i < curAskN; ++i)
        {
            double pq = FindQty(st->prevAsk, st->prevAskN, curAsk[i].tick);
            if (pq >= 0.0)
                askNet += (curAsk[i].qty - pq);
        }

        for (int i = 0; i < st->prevAskN; ++i)
        {
            int t = st->prevAsk[i].tick;
            if (FindQty(curAsk, curAskN, t) < 0.0)
            {
                if (t >= askMinTick && t <= askMaxTick)
                    askNet += (0.0 - st->prevAsk[i].qty);
            }
        }
    }

    // ---- Accumulate into the forming bar ---------------------------------
    st->bidNetAccum += bidNet;
    st->askNetAccum += askNet;

    // History Mode = Delta:
    //   netPressure = bidNet - askNet
    //   bids growing / asks shrinking -> positive -> bullish book
    //   bids shrinking / asks growing -> negative -> bearish book
    const double rawNet = st->bidNetAccum - st->askNetAccum;

    const int bi = st->curBarIndex;
    sgRaw[bi]    = rawNet;
    sgBidNet[bi] = st->bidNetAccum;
    sgAskNet[bi] = st->askNetAccum;

    // Mirror into the persistent history cache so a later settings change can
    // repaint this bar instead of losing it.
    EnsureCacheCap(st, bi + 1);
    st->rawCache[bi] = rawNet;
    st->bidCache[bi] = st->bidNetAccum;
    st->askCache[bi] = st->askNetAccum;
    if (bi + 1 > st->cacheCount) st->cacheCount = bi + 1;

    // ---- Smoothing: SMA of the raw netPressure series --------------------
    double outVal = rawNet;
    if (smoothOn && smoothPer > 1)
    {
        int start = bi - (smoothPer - 1);
        if (start < 0) start = 0;
        double sum = 0.0;
        int    cnt = 0;
        for (int k = start; k <= bi; ++k)
        {
            sum += sgRaw[k];
            cnt++;
        }
        if (cnt > 0)
            outVal = sum / (double)cnt;
    }

    sgNet[bi] = outVal;
    sgNet.DataColor[bi] = (outVal >= 0.0) ? RGB(40, 130, 230)   // bullish blue
                                          : RGB(220, 60, 60);   // bearish red

    // ---- Persist current snapshot as the new "previous" ------------------
    for (int i = 0; i < curBidN; ++i) st->prevBid[i] = curBid[i];
    for (int i = 0; i < curAskN; ++i) st->prevAsk[i] = curAsk[i];
    st->prevBidN = curBidN;
    st->prevAskN = curAskN;
}
