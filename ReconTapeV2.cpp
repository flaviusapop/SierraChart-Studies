// ReconTapeV2.cpp
// =============================================================================
// Recon Tape V2 — Multi-Tier Reconstructed Tape  (Sierra Chart ACSIL)
//
// Three independent aggregation tiers run in parallel on the same chart. Each
// tier groups the bar's Volume-at-Price ladder into buckets of N chart ticks
// (Tick Multiplier; e.g. on ES 0.25 chart: 1 = 0.25, 2 = 0.50, 4 = 1.00),
// scores each bucket against ITS OWN trailing baseline by Reference
// Percentile, and draws qualifying buckets as fixed-size bubbles at the
// bucket's volume-weighted price. Coarser tiers pool split volume back
// together, so what passes is a campaign at an area rather than a single
// print — bigger bubble = wider area = stronger signal.
//
// Per tier: Enabled, Tick Multiplier, Print Metric, Min Percentile, Bubble
// Size (fixed px), Print Metric Filter (absolute floor), Color Bar Min
// Bubbles, Bubble Label, Alert.
//
// PRINT METRIC (per tier)
//   Bid/Ask Volume: bucket ask volume -> buy side, bid volume -> sell side
//                   (both sides of a bucket can print).
//   Delta:          bucket (ask - bid); sign picks the side.
//   Total Volume:   bucket (ask + bid); one bubble, colored by net delta side.
//   Dominant Side:  max(ask, bid); one bubble, colored by the WINNING side —
//                   like Total Volume but keeps the side information.
//
// BASELINE — each tier keeps its own per-bar strongest-bucket magnitude over
// Baseline Length bars; a bucket's percentile is its rank within that history.
//
// BASELINE SCOPE
//   All Bars:      trailing window of the previous Baseline Length bars.
//   Same Session:  the window only contains bars of the SAME session type as
//                  the scored bar (day session vs overnight, per the chart's
//                  Session Times), rolling back across session boundaries —
//                  RTH ranks against recent RTH (today + previous RTH days),
//                  Globex against Globex. Overnight volume never deflates or
//                  inflates RTH percentiles and vice versa.
//
// COLOR BAR — checked highest tier first; if a tier with Min Bubbles > 0
// reaches its count, the whole bar is colored by the FULL bar delta (all
// levels, not just qualifying buckets) and individual bubbles are hidden.
// Divergence diamonds (optional) print on colored bars when the full bar
// delta disagrees with the candle direction (green below = bullish,
// orange above = bearish).
//
// TIMING — Live updates the forming bar in place; On Bar Close keeps the
// forming bar blank and finalizes each bar at close.
//
// Removed vs V1: Z-Score mode, Zones of Interest, Measure Reaction.
//
// -----------------------------------------------------------------------------
// BUILD NOTES (confirmed SC build-server patterns)
//   - No std::min/std::max/std::fabs, no <algorithm>.
//   - DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE: Data[] = price,
//     Arrays[0][] = pixel size, DataColor[] (AUTOCOLOR_NONE set first).
//   - Custom-string inputs read with .GetIndex().
//   - Data via sc.VolumeAtPriceForBars with sc.MaintainVolumeAtPriceData = 1.
//   - Labels via sc.UseTool DRAWING_TEXT, incremental per-bar counts tracked.
//   - Scale SCALE_SAMEASREGION; state in Subgraph extra arrays + persistent int.
// =============================================================================

#include "sierrachart.h"

SCDLLName("ReconTapeV2")

const int NUM_TIERS   = 3;
const int SLOTS       = 8;     // bubble slots per side per tier
const int MAX_BUCKETS = 300;   // aggregated price buckets buffered per bar
const int MIN_SAMPLES = 2;     // min baseline samples to score

// ---- Subgraph layout ---------------------------------------------------------
//   0..47   bubbles: tier*16 + side*8 + slot   (side 0 = buy, 1 = sell)
//   48      color bar
//   49      divergence diamonds
//   50..52  per-tier hidden cache
const int SG_COLORBAR = NUM_TIERS * 2 * SLOTS;   // 48
const int SG_DIVERG   = SG_COLORBAR + 1;         // 49
const int SG_CACHE0   = SG_DIVERG + 1;           // 50

inline int BubSG(int tier, int side, int slot) { return tier * 2 * SLOTS + side * SLOTS + slot; }
inline int CacheSG(int tier)                   { return SG_CACHE0 + tier; }

// Cache extra-array layout (per tier)
const int CA_MAXV     = 0;   // bar's strongest bucket magnitude (baseline)
const int CA_NA       = 1;   // buy-side labels drawn on the bar
const int CA_NB       = 2;   // sell-side labels drawn on the bar
const int CA_ALERTED  = 3;   // bubble count already alerted for the bar
const int CA_BUBCNT   = 4;   // bubbles drawn on the bar
const int CA_BARDELTA = 5;   // FULL bar delta (tier 0 cache only)
const int CA_SESSION  = 6;   // 1 = day session, 0 = overnight (tier 0 only)
const int CA_PREVSAME = 7;   // index of previous same-session bar (tier 0 only)

const int MAX_BASE = 2000;   // hard cap on baseline window size

// Tier default colors (buy / sell), light -> vivid -> dark with tier.
const COLORREF DEF_BUY [NUM_TIERS] = { RGB(130, 175, 255), RGB(60, 120, 235), RGB(15, 55, 190) };
const COLORREF DEF_SELL[NUM_TIERS] = { RGB(255, 145, 145), RGB(225, 60, 60),  RGB(170, 15, 15) };

// ---- Input indices (logical order) --------------------------------------------
// General
const int IN_BASELINE  = 0;   // baseline length (bars)
const int IN_TIMING    = 1;   // bubble timing
const int IN_MAXBUB    = 2;   // max bubbles per side (per tier)
// Labels
const int IN_LABELSIZE = 3;
const int IN_LABELCOL  = 4;
// Alerts
const int IN_ALERTSOUND= 5;
// Divergence
const int IN_DIVERG    = 6;
const int IN_DIVSIZE   = 7;
// Per-tier blocks
const int IN_TIER0     = 8;   // base index of tier 0 block
const int TIER_INPUTS  = 9;   // inputs per tier block
const int TI_ON     = 0;      // enabled
const int TI_MULT   = 1;      // tick multiplier
const int TI_METRIC = 2;      // print metric
const int TI_MINPCT = 3;      // min percentile
const int TI_SIZE   = 4;      // bubble size (px, fixed)
const int TI_FILTER = 5;      // print metric filter (absolute floor)
const int TI_CBMIN  = 6;      // color bar min bubbles
const int TI_LABEL  = 7;      // bubble label
const int TI_ALERT  = 8;      // alert on new bubble

inline int TierIn(int tier, int which) { return IN_TIER0 + tier * TIER_INPUTS + which; }

// Appended after the tier blocks so existing instances keep their settings.
const int IN_SCOPE     = IN_TIER0 + NUM_TIERS * TIER_INPUTS;   // baseline scope
const int IN_SESSRC    = IN_SCOPE + 1;   // session source: chart / custom
const int IN_RTHSTART  = IN_SCOPE + 2;   // custom RTH start time
const int IN_RTHEND    = IN_SCOPE + 3;   // custom RTH end time
const int IN_TBASE0    = IN_SCOPE + 4;   // per-tier baseline length overrides
inline int TierBaseIn(int tier) { return IN_TBASE0 + tier; }

// Label drawing line numbers (unique per bar/tier/side/slot).
const int LN_BASE = 80000000;
inline int LabelLN(int i, int tier, int side, int slot)
{ return LN_BASE + (((i * NUM_TIERS + tier) * 2 + side) * SLOTS) + slot; }

// ---- Small structs -------------------------------------------------------------
struct V2Tier
{
    bool  on;
    int   baseLen;    // effective baseline length (override or global)
    int   mult;
    int   metric;     // 0 Bid/Ask, 1 Delta, 2 Total, 3 Dominant Side
    float minPct;
    float sizePx;
    float filter;
    int   cbMin;
    int   label;      // 0 Off 1 Vol 2 Score 3 Delta 4 Imbal% 5 Vol+Delta
    bool  alert;
};

struct V2Bucket { int key; float av; float bv; float pxv; };   // pxv = sum(price*totVol)

struct V2Cand { float price; float met; float mag; float delta; float total; };

// -----------------------------------------------------------------------------
// Aggregate bar's VAP ladder into buckets of `mult` ticks. Returns bucket count.
static int V2Aggregate(SCStudyInterfaceRef sc, int barIndex, int mult, V2Bucket *B)
{
    int n = 0;
    const int levels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(barIndex);
    for (int k = 0; k < levels; ++k)
    {
        const s_VolumeAtPriceV2 *vap = NULL;
        if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(barIndex, k, &vap))
            break;
        if (vap == NULL)
            continue;

        int key = vap->PriceInTicks;
        if (key >= 0) key /= mult;
        else          key = -((-key + mult - 1) / mult);

        const float av = (float)vap->AskVolume;
        const float bv = (float)vap->BidVolume;
        const float px = (float)vap->PriceInTicks * sc.TickSize * (av + bv);

        int j = 0;
        for (; j < n; ++j)
            if (B[j].key == key)
                break;
        if (j == n)
        {
            if (n >= MAX_BUCKETS)
                continue;
            B[n].key = key; B[n].av = 0.f; B[n].bv = 0.f; B[n].pxv = 0.f;
            ++n;
        }
        B[j].av += av; B[j].bv += bv; B[j].pxv += px;
    }
    return n;
}

// Baseline magnitude of a bucket under a metric.
static float V2Mag(int metric, float av, float bv)
{
    if (metric == 0 || metric == 3) return (av > bv) ? av : bv;
    if (metric == 1) { const float d = av - bv; return (d >= 0.f) ? d : -d; }
    return av + bv;
}

// Percentile of mag within the tier's baseline bars (explicit index list).
// -1 = cannot score.
static float V2Pct(SCStudyInterfaceRef sc, int tier, float mag,
                   const int *baseIdx, int nBase)
{
    if (mag <= 0.f)
        return -1.f;
    int le = 0, tot = 0;
    for (int q = 0; q < nBase; ++q)
    {
        const float mv = sc.Subgraph[CacheSG(tier)].Arrays[CA_MAXV][baseIdx[q]];
        if (mv > 0.f) { ++tot; if (mv <= mag) ++le; }
    }
    if (tot < MIN_SAMPLES)
        return -1.f;
    return 100.f * (float)le / (float)tot;
}

// Keep the top maxN candidates ranked by percentile (met) descending.
static void V2TopK(V2Cand *arr, int &n, int maxN, const V2Cand &c)
{
    if (n < maxN)
    {
        int p = n;
        while (p > 0 && arr[p - 1].met < c.met) { arr[p] = arr[p - 1]; --p; }
        arr[p] = c; ++n;
    }
    else if (n > 0 && c.met > arr[n - 1].met)
    {
        int p = n - 1;
        while (p > 0 && arr[p - 1].met < c.met) { arr[p] = arr[p - 1]; --p; }
        arr[p] = c;
    }
}

// Format a bubble label. 1 Vol 2 Score 3 Delta 4 Imbal% 5 Vol+Delta.
static void V2Fmt(SCString &t, int labelMode, const V2Cand &c)
{
    const int d = (int)((c.delta >= 0.f) ? (c.delta + 0.5f) : (c.delta - 0.5f));
    if (labelMode == 1)
        t.Format("%d", (int)(c.mag + 0.5f));
    else if (labelMode == 2)
        t.Format("%.0f", c.met);
    else if (labelMode == 3)
        t.Format("%+d", d);
    else if (labelMode == 4)
    {
        const float im = (c.total > 0.f) ? (c.delta / c.total * 100.f) : 0.f;
        t.Format("%+.0f%%", im);
    }
    else if (labelMode == 5)
        t.Format("%d / %+d", (int)(c.mag + 0.5f), d);
}

// Add/adjust (or delete) a centered text label.
static void V2Label(SCStudyInterfaceRef sc, int lineNumber, bool show,
                    int barIndex, float price, const SCString &text,
                    COLORREF color, int fontSize)
{
    if (!show)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNumber);
        return;
    }
    s_UseTool Tool;
    Tool.Clear();
    Tool.ChartNumber   = sc.ChartNumber;
    Tool.DrawingType   = DRAWING_TEXT;
    Tool.LineNumber    = lineNumber;
    Tool.BeginIndex    = barIndex;
    Tool.BeginValue    = price;
    Tool.Color         = color;
    Tool.FontSize      = fontSize;
    Tool.Text          = text;
    Tool.TextAlignment = DT_CENTER | DT_VCENTER;
    Tool.AddMethod     = UTAM_ADD_OR_ADJUST;
    sc.UseTool(Tool);
}

// Zero all bubble slots + color bar + divergence on bar i.
static void V2ClearBubbles(SCStudyInterfaceRef sc, int i)
{
    for (int t = 0; t < NUM_TIERS; ++t)
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < SLOTS; ++k)
            {
                sc.Subgraph[BubSG(t, s, k)][i]           = 0.f;
                sc.Subgraph[BubSG(t, s, k)].Arrays[0][i] = 0.f;
            }
    sc.Subgraph[SG_COLORBAR][i] = 0.f;
    sc.Subgraph[SG_DIVERG][i]   = 0.f;
}

// Blank a bar entirely: bubbles + labels + counters.
static void V2BlankBar(SCStudyInterfaceRef sc, int i)
{
    V2ClearBubbles(sc, i);
    SCString empty;
    for (int t = 0; t < NUM_TIERS; ++t)
    {
        const int prevA = (int)sc.Subgraph[CacheSG(t)].Arrays[CA_NA][i];
        const int prevB = (int)sc.Subgraph[CacheSG(t)].Arrays[CA_NB][i];
        for (int k = 0; k < prevA; ++k) V2Label(sc, LabelLN(i, t, 0, k), false, i, 0.f, empty, 0, 0);
        for (int k = 0; k < prevB; ++k) V2Label(sc, LabelLN(i, t, 1, k), false, i, 0.f, empty, 0, 0);
        sc.Subgraph[CacheSG(t)].Arrays[CA_NA][i]     = 0.f;
        sc.Subgraph[CacheSG(t)].Arrays[CA_NB][i]     = 0.f;
        sc.Subgraph[CacheSG(t)].Arrays[CA_BUBCNT][i] = 0.f;
    }
}

// Score and render one bar across all tiers.
static void V2RenderBar(SCStudyInterfaceRef sc, int i, int scope,
                        const V2Tier *T, int maxSlots,
                        int labelSize, COLORREF labelCol, bool divOn)
{
    V2ClearBubbles(sc, i);

    int prevA[NUM_TIERS], prevB[NUM_TIERS];
    for (int t = 0; t < NUM_TIERS; ++t)
    {
        prevA[t] = (int)sc.Subgraph[CacheSG(t)].Arrays[CA_NA][i];
        prevB[t] = (int)sc.Subgraph[CacheSG(t)].Arrays[CA_NB][i];
    }

    V2Cand buy [NUM_TIERS][SLOTS];
    V2Cand sell[NUM_TIERS][SLOTS];
    int nA[NUM_TIERS] = {0, 0, 0};
    int nB[NUM_TIERS] = {0, 0, 0};

    if (i >= 1)
    {
        // Build ONE baseline bar-index list per bar at the longest enabled
        // tier length, ordered most-recent-first; each tier then uses only
        // the first (most recent) baseLen entries of it.
        int maxLen = 0;
        for (int t = 0; t < NUM_TIERS; ++t)
            if (T[t].on && T[t].baseLen > maxLen) maxLen = T[t].baseLen;
        if (maxLen > MAX_BASE) maxLen = MAX_BASE;

        int baseIdx[MAX_BASE];
        int nBase = 0;
        if (scope == 1)   // Same Session: walk the same-session chain backwards
        {
            int j = (int)sc.Subgraph[CacheSG(0)].Arrays[CA_PREVSAME][i];
            while (j >= 0 && nBase < maxLen)
            {
                baseIdx[nBase++] = j;
                j = (int)sc.Subgraph[CacheSG(0)].Arrays[CA_PREVSAME][j];
            }
        }
        else              // All Bars: trailing window, newest first
        {
            for (int b = i - 1; b >= 0 && nBase < maxLen; --b)
                baseIdx[nBase++] = b;
        }

        const float tick = sc.TickSize;

        V2Bucket B[MAX_BUCKETS];

        for (int t = 0; t < NUM_TIERS; ++t)
        {
            if (!T[t].on)
                continue;

            const int metric = T[t].metric;
            const int nBT = (T[t].baseLen < nBase) ? T[t].baseLen : nBase;
            const int nb = V2Aggregate(sc, i, T[t].mult, B);

            for (int j = 0; j < nb; ++j)
            {
                const float av  = B[j].av;
                const float bv  = B[j].bv;
                const float tot = av + bv;
                const float dlt = av - bv;
                const float price = (tot > 0.f)
                    ? B[j].pxv / tot
                    : ((float)B[j].key * T[t].mult + (T[t].mult - 1) * 0.5f) * tick;

                if (metric == 0)        // Bid/Ask Volume — both sides may print
                {
                    if (av > 0.f && (T[t].filter <= 0.f || av >= T[t].filter))
                    {
                        const float pct = V2Pct(sc, t, av, baseIdx, nBT);
                        if (pct >= T[t].minPct)
                        { V2Cand c = { price, pct, av, dlt, tot }; V2TopK(buy[t], nA[t], maxSlots, c); }
                    }
                    if (bv > 0.f && (T[t].filter <= 0.f || bv >= T[t].filter))
                    {
                        const float pct = V2Pct(sc, t, bv, baseIdx, nBT);
                        if (pct >= T[t].minPct)
                        { V2Cand c = { price, pct, bv, dlt, tot }; V2TopK(sell[t], nB[t], maxSlots, c); }
                    }
                }
                else if (metric == 1)   // Delta — sign picks the side
                {
                    const float mag = (dlt >= 0.f) ? dlt : -dlt;
                    if (mag > 0.f && (T[t].filter <= 0.f || mag >= T[t].filter))
                    {
                        const float pct = V2Pct(sc, t, mag, baseIdx, nBT);
                        if (pct >= T[t].minPct)
                        {
                            V2Cand c = { price, pct, mag, dlt, tot };
                            if (dlt >= 0.f) V2TopK(buy[t],  nA[t], maxSlots, c);
                            else            V2TopK(sell[t], nB[t], maxSlots, c);
                        }
                    }
                }
                else if (metric == 2)   // Total Volume — one bubble, net-delta side
                {
                    if (tot > 0.f && (T[t].filter <= 0.f || tot >= T[t].filter))
                    {
                        const float pct = V2Pct(sc, t, tot, baseIdx, nBT);
                        if (pct >= T[t].minPct)
                        {
                            V2Cand c = { price, pct, tot, dlt, tot };
                            if (dlt >= 0.f) V2TopK(buy[t],  nA[t], maxSlots, c);
                            else            V2TopK(sell[t], nB[t], maxSlots, c);
                        }
                    }
                }
                else                    // Dominant Side — winner only
                {
                    const float mag = (av >= bv) ? av : bv;
                    if (mag > 0.f && (T[t].filter <= 0.f || mag >= T[t].filter))
                    {
                        const float pct = V2Pct(sc, t, mag, baseIdx, nBT);
                        if (pct >= T[t].minPct)
                        {
                            V2Cand c = { price, pct, mag, dlt, tot };
                            if (av >= bv) V2TopK(buy[t],  nA[t], maxSlots, c);
                            else          V2TopK(sell[t], nB[t], maxSlots, c);
                        }
                    }
                }
            }
        }
    }

    // ---- Color bar: highest tier with a satisfied Min Bubbles wins ------------
    bool barColored = false;
    for (int t = NUM_TIERS - 1; t >= 0 && !barColored; --t)
        if (T[t].on && T[t].cbMin > 0 && (nA[t] + nB[t]) >= T[t].cbMin)
            barColored = true;

    const float barDelta = sc.Subgraph[CacheSG(0)].Arrays[CA_BARDELTA][i];

    if (barColored)
    {
        sc.Subgraph[SG_COLORBAR][i]           = sc.Close[i];
        sc.Subgraph[SG_COLORBAR].DataColor[i] = (barDelta >= 0.f) ? DEF_BUY[1] : DEF_SELL[1];

        if (divOn)
        {
            const float op = sc.Open[i];
            const float cl = sc.Close[i];
            if (barDelta > 0.f && cl < op)        // buy delta into a DOWN bar
            {
                sc.Subgraph[SG_DIVERG][i]           = sc.Low[i] - 2.f * sc.TickSize;
                sc.Subgraph[SG_DIVERG].DataColor[i] = RGB(0, 200, 0);
            }
            else if (barDelta < 0.f && cl > op)   // sell delta into an UP bar
            {
                sc.Subgraph[SG_DIVERG][i]           = sc.High[i] + 2.f * sc.TickSize;
                sc.Subgraph[SG_DIVERG].DataColor[i] = RGB(255, 140, 0);
            }
        }
    }
    else
    {
        for (int t = 0; t < NUM_TIERS; ++t)
        {
            for (int k = 0; k < nA[t]; ++k)
            {
                const int sg = BubSG(t, 0, k);
                sc.Subgraph[sg][i]           = buy[t][k].price;
                sc.Subgraph[sg].Arrays[0][i] = T[t].sizePx;
                sc.Subgraph[sg].DataColor[i] = sc.Subgraph[sg].PrimaryColor;
            }
            for (int k = 0; k < nB[t]; ++k)
            {
                const int sg = BubSG(t, 1, k);
                sc.Subgraph[sg][i]           = sell[t][k].price;
                sc.Subgraph[sg].Arrays[0][i] = T[t].sizePx;
                sc.Subgraph[sg].DataColor[i] = sc.Subgraph[sg].PrimaryColor;
            }

            if (T[t].label != 0)
            {
                for (int k = 0; k < nA[t]; ++k)
                {
                    SCString s; V2Fmt(s, T[t].label, buy[t][k]);
                    V2Label(sc, LabelLN(i, t, 0, k), true, i, buy[t][k].price, s, labelCol, labelSize);
                }
                for (int k = 0; k < nB[t]; ++k)
                {
                    SCString s; V2Fmt(s, T[t].label, sell[t][k]);
                    V2Label(sc, LabelLN(i, t, 1, k), true, i, sell[t][k].price, s, labelCol, labelSize);
                }
            }
        }
    }

    // ---- Delete stale labels, store counters ----------------------------------
    SCString empty;
    for (int t = 0; t < NUM_TIERS; ++t)
    {
        const int keepA = (T[t].label != 0 && !barColored) ? nA[t] : 0;
        const int keepB = (T[t].label != 0 && !barColored) ? nB[t] : 0;
        for (int k = keepA; k < prevA[t]; ++k) V2Label(sc, LabelLN(i, t, 0, k), false, i, 0.f, empty, 0, 0);
        for (int k = keepB; k < prevB[t]; ++k) V2Label(sc, LabelLN(i, t, 1, k), false, i, 0.f, empty, 0, 0);

        sc.Subgraph[CacheSG(t)].Arrays[CA_NA][i]     = (float)keepA;
        sc.Subgraph[CacheSG(t)].Arrays[CA_NB][i]     = (float)keepB;
        sc.Subgraph[CacheSG(t)].Arrays[CA_BUBCNT][i] = (float)(nA[t] + nB[t]);
    }
}

// =============================================================================
SCSFExport scsf_ReconTapeV2(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Recon Tape V2 (Multi-Tier Bubbles)";
        sc.GraphRegion      = 0;
        sc.AutoLoop         = 0;
        sc.ScaleRangeType   = SCALE_SAMEASREGION;
        sc.DrawZeros        = 0;
        sc.ValueFormat      = VALUEFORMAT_INHERITED;
        sc.UpdateAlways     = 1;
        sc.MaintainVolumeAtPriceData = 1;

        for (int t = 0; t < NUM_TIERS; ++t)
            for (int s = 0; s < 2; ++s)
                for (int k = 0; k < SLOTS; ++k)
                {
                    const int sg = BubSG(t, s, k);
                    SCString nm;
                    nm.Format("T%d %s %d", t + 1, (s == 0) ? "Buy" : "Sell", k + 1);
                    sc.Subgraph[sg].Name         = nm;
                    sc.Subgraph[sg].DrawStyle    = DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE;
                    sc.Subgraph[sg].PrimaryColor = (s == 0) ? DEF_BUY[t] : DEF_SELL[t];
                    sc.Subgraph[sg].DrawZeros    = 0;
                    sc.Subgraph[sg].AutoColoring = AUTOCOLOR_NONE;
                }

        sc.Subgraph[SG_COLORBAR].Name         = "Bar Color";
        sc.Subgraph[SG_COLORBAR].DrawStyle    = DRAWSTYLE_COLOR_BAR;
        sc.Subgraph[SG_COLORBAR].PrimaryColor = RGB(180, 180, 180);
        sc.Subgraph[SG_COLORBAR].DrawZeros    = 0;
        sc.Subgraph[SG_COLORBAR].AutoColoring = AUTOCOLOR_NONE;

        sc.Subgraph[SG_DIVERG].Name         = "Divergence";
        sc.Subgraph[SG_DIVERG].DrawStyle    = DRAWSTYLE_DIAMOND;
        sc.Subgraph[SG_DIVERG].PrimaryColor = RGB(255, 140, 0);
        sc.Subgraph[SG_DIVERG].LineWidth    = 6;
        sc.Subgraph[SG_DIVERG].DrawZeros    = 0;
        sc.Subgraph[SG_DIVERG].AutoColoring = AUTOCOLOR_NONE;

        for (int t = 0; t < NUM_TIERS; ++t)
        {
            SCString nm; nm.Format("Cache T%d (internal)", t + 1);
            sc.Subgraph[CacheSG(t)].Name      = nm;
            sc.Subgraph[CacheSG(t)].DrawStyle = DRAWSTYLE_IGNORE;
            sc.Subgraph[CacheSG(t)].DrawZeros = 0;
        }

        // ---- General ---------------------------------------------------------
        sc.Input[IN_BASELINE].Name = "Baseline Length (bars)";
        sc.Input[IN_BASELINE].SetInt(50);
        sc.Input[IN_BASELINE].SetIntLimits(MIN_SAMPLES, MAX_BASE);

        sc.Input[IN_SCOPE].Name = "Baseline Scope";
        sc.Input[IN_SCOPE].SetCustomInputStrings(
            "All Bars (continuous);Same Session (RTH/Globex, rolling)");
        sc.Input[IN_SCOPE].SetCustomInputIndex(1);

        sc.Input[IN_SESSRC].Name = "Session Source (for Same Session scope)";
        sc.Input[IN_SESSRC].SetCustomInputStrings(
            "Chart Session Times;Custom Times Below");
        sc.Input[IN_SESSRC].SetCustomInputIndex(0);

        sc.Input[IN_RTHSTART].Name = "Custom RTH Start Time";
        sc.Input[IN_RTHSTART].SetTime(HMS_TIME(9, 30, 0));

        sc.Input[IN_RTHEND].Name = "Custom RTH End Time";
        sc.Input[IN_RTHEND].SetTime(HMS_TIME(16, 14, 59));

        const int defTBase[NUM_TIERS] = { 0, 0, 300 };
        for (int t = 0; t < NUM_TIERS; ++t)
        {
            SCString nm;
            nm.Format("Tier %d: Baseline Length (0=global)", t + 1);
            sc.Input[TierBaseIn(t)].Name = nm;
            sc.Input[TierBaseIn(t)].SetInt(defTBase[t]);
            sc.Input[TierBaseIn(t)].SetIntLimits(0, MAX_BASE);
        }

        sc.Input[IN_TIMING].Name = "Bubble Timing";
        sc.Input[IN_TIMING].SetCustomInputStrings("On Bar Close;Live");
        sc.Input[IN_TIMING].SetCustomInputIndex(1);

        sc.Input[IN_MAXBUB].Name = "Max Bubbles Per Side (per tier)";
        sc.Input[IN_MAXBUB].SetInt(SLOTS);
        sc.Input[IN_MAXBUB].SetIntLimits(1, SLOTS);

        // ---- Labels ------------------------------------------------------------
        sc.Input[IN_LABELSIZE].Name = "Label Font Size";
        sc.Input[IN_LABELSIZE].SetInt(8);
        sc.Input[IN_LABELSIZE].SetIntLimits(4, 72);

        sc.Input[IN_LABELCOL].Name = "Label Color";
        sc.Input[IN_LABELCOL].SetColor(RGB(255, 255, 255));

        // ---- Alerts ------------------------------------------------------------
        sc.Input[IN_ALERTSOUND].Name = "Alert Sound Number";
        sc.Input[IN_ALERTSOUND].SetInt(1);
        sc.Input[IN_ALERTSOUND].SetIntLimits(0, 100);

        // ---- Divergence --------------------------------------------------------
        sc.Input[IN_DIVERG].Name = "Divergence Diamonds (on colored bars)";
        sc.Input[IN_DIVERG].SetCustomInputStrings("Off;On");
        sc.Input[IN_DIVERG].SetCustomInputIndex(1);

        sc.Input[IN_DIVSIZE].Name = "Diamond Size";
        sc.Input[IN_DIVSIZE].SetInt(6);
        sc.Input[IN_DIVSIZE].SetIntLimits(1, 50);

        // ---- Tier blocks -------------------------------------------------------
        const int   defMult [NUM_TIERS] = { 1, 2, 4 };
        const int   defSize [NUM_TIERS] = { 8, 12, 16 };
        const int   defAlert[NUM_TIERS] = { 0, 0, 1 };
        for (int t = 0; t < NUM_TIERS; ++t)
        {
            SCString nm;

            nm.Format("Tier %d: Enabled", t + 1);
            sc.Input[TierIn(t, TI_ON)].Name = nm;
            sc.Input[TierIn(t, TI_ON)].SetCustomInputStrings("Off;On");
            sc.Input[TierIn(t, TI_ON)].SetCustomInputIndex(1);

            nm.Format("Tier %d: Tick Multiplier", t + 1);
            sc.Input[TierIn(t, TI_MULT)].Name = nm;
            sc.Input[TierIn(t, TI_MULT)].SetInt(defMult[t]);
            sc.Input[TierIn(t, TI_MULT)].SetIntLimits(1, 100);

            nm.Format("Tier %d: Print Metric", t + 1);
            sc.Input[TierIn(t, TI_METRIC)].Name = nm;
            sc.Input[TierIn(t, TI_METRIC)].SetCustomInputStrings(
                "Bid/Ask Volume;Delta;Total Volume;Dominant Side");
            sc.Input[TierIn(t, TI_METRIC)].SetCustomInputIndex(2);

            nm.Format("Tier %d: Min Percentile", t + 1);
            sc.Input[TierIn(t, TI_MINPCT)].Name = nm;
            sc.Input[TierIn(t, TI_MINPCT)].SetFloat(95.0f);
            sc.Input[TierIn(t, TI_MINPCT)].SetFloatLimits(0.0f, 100.0f);

            nm.Format("Tier %d: Bubble Size (px)", t + 1);
            sc.Input[TierIn(t, TI_SIZE)].Name = nm;
            sc.Input[TierIn(t, TI_SIZE)].SetInt(defSize[t]);
            sc.Input[TierIn(t, TI_SIZE)].SetIntLimits(1, 200);

            nm.Format("Tier %d: Print Metric Filter (0=off)", t + 1);
            sc.Input[TierIn(t, TI_FILTER)].Name = nm;
            sc.Input[TierIn(t, TI_FILTER)].SetFloat(0.0f);
            sc.Input[TierIn(t, TI_FILTER)].SetFloatLimits(0.0f, 100000000.0f);

            nm.Format("Tier %d: Color Bar Min Bubbles (0=off)", t + 1);
            sc.Input[TierIn(t, TI_CBMIN)].Name = nm;
            sc.Input[TierIn(t, TI_CBMIN)].SetInt(0);
            sc.Input[TierIn(t, TI_CBMIN)].SetIntLimits(0, 1000);

            nm.Format("Tier %d: Bubble Label", t + 1);
            sc.Input[TierIn(t, TI_LABEL)].Name = nm;
            sc.Input[TierIn(t, TI_LABEL)].SetCustomInputStrings(
                "Off;Volume;Score;Delta;Imbalance %;Volume+Delta");
            sc.Input[TierIn(t, TI_LABEL)].SetCustomInputIndex(0);

            nm.Format("Tier %d: Alert On New Bubble", t + 1);
            sc.Input[TierIn(t, TI_ALERT)].Name = nm;
            sc.Input[TierIn(t, TI_ALERT)].SetCustomInputStrings("Off;On");
            sc.Input[TierIn(t, TI_ALERT)].SetCustomInputIndex(defAlert[t]);
        }

        return;
    }

    // Remove every drawing this study made when it is removed from the chart.
    if (sc.LastCallToFunction)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_ALL, 0);
        return;
    }

    const int last = sc.ArraySize - 1;
    if (last < 0)
        return;

    // On a full recalculation (input change, chart reload) the per-bar label
    // counters are wiped, so stale text drawings from the previous settings
    // would never be matched for deletion and remain orphaned on the chart.
    // Delete ALL of this study's drawings up front; rendering re-adds what the
    // current settings require.
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_ALL, 0);

    if (sc.VolumeAtPriceForBars == NULL)
        return;
    if ((int)sc.VolumeAtPriceForBars->GetNumberOfBars() < sc.ArraySize)
        return;

    // Re-force styles at runtime so instances added before a rebuild pick them up.
    if (sc.Subgraph[SG_COLORBAR].DrawStyle != DRAWSTYLE_COLOR_BAR)
        sc.Subgraph[SG_COLORBAR].DrawStyle = DRAWSTYLE_COLOR_BAR;
    if (sc.Subgraph[SG_DIVERG].DrawStyle != DRAWSTYLE_DIAMOND)
        sc.Subgraph[SG_DIVERG].DrawStyle = DRAWSTYLE_DIAMOND;
    sc.Subgraph[SG_DIVERG].AutoColoring = AUTOCOLOR_NONE;
    sc.Subgraph[SG_DIVERG].LineWidth    = sc.Input[IN_DIVSIZE].GetInt();

    // ---- Read inputs -----------------------------------------------------------
    int baseLen = sc.Input[IN_BASELINE].GetInt();
    if (baseLen < MIN_SAMPLES) baseLen = MIN_SAMPLES;
    if (baseLen > MAX_BASE)    baseLen = MAX_BASE;
    const int      scope     = sc.Input[IN_SCOPE].GetIndex();
    const int      sessSrc   = sc.Input[IN_SESSRC].GetIndex();
    const int      rthStart  = sc.Input[IN_RTHSTART].GetTime();
    const int      rthEnd    = sc.Input[IN_RTHEND].GetTime();
    const int      timing    = sc.Input[IN_TIMING].GetIndex();
    int maxSlots = sc.Input[IN_MAXBUB].GetInt();
    if (maxSlots < 1) maxSlots = 1;
    if (maxSlots > SLOTS) maxSlots = SLOTS;
    const int      labelSize = sc.Input[IN_LABELSIZE].GetInt();
    const COLORREF labelCol  = sc.Input[IN_LABELCOL].GetColor();
    const bool     divOn     = (sc.Input[IN_DIVERG].GetIndex() == 1);

    V2Tier T[NUM_TIERS];
    for (int t = 0; t < NUM_TIERS; ++t)
    {
        T[t].on     = (sc.Input[TierIn(t, TI_ON)].GetIndex() == 1);
        T[t].baseLen = sc.Input[TierBaseIn(t)].GetInt();
        if (T[t].baseLen <= 0)         T[t].baseLen = baseLen;   // 0 = use global
        if (T[t].baseLen < MIN_SAMPLES) T[t].baseLen = MIN_SAMPLES;
        if (T[t].baseLen > MAX_BASE)    T[t].baseLen = MAX_BASE;
        T[t].mult   = sc.Input[TierIn(t, TI_MULT)].GetInt();
        if (T[t].mult < 1) T[t].mult = 1;
        T[t].metric = sc.Input[TierIn(t, TI_METRIC)].GetIndex();
        T[t].minPct = sc.Input[TierIn(t, TI_MINPCT)].GetFloat();
        T[t].sizePx = (float)sc.Input[TierIn(t, TI_SIZE)].GetInt();
        T[t].filter = sc.Input[TierIn(t, TI_FILTER)].GetFloat();
        T[t].cbMin  = sc.Input[TierIn(t, TI_CBMIN)].GetInt();
        T[t].label  = sc.Input[TierIn(t, TI_LABEL)].GetIndex();
        T[t].alert  = (sc.Input[TierIn(t, TI_ALERT)].GetIndex() == 1);
    }

    int start = sc.UpdateStartIndex;
    if (start < 0) start = 0;

    // PASS 1 — per-tier baseline (strongest bucket magnitude), full bar delta,
    // and session tagging (same-session back-link chain).
    {
        V2Bucket B[MAX_BUCKETS];
        for (int b = start; b <= last; ++b)
        {
            // Session flag + back-link to the previous same-session bar.
            // Sessions are contiguous, so the scan is one step except right
            // after a session boundary.
            int flag;
            if (sessSrc == 1)   // custom RTH window (handles overnight windows too)
            {
                const int tod = sc.BaseDateTimeIn[b].GetTimeInSeconds();
                if (rthStart <= rthEnd)
                    flag = (tod >= rthStart && tod <= rthEnd) ? 1 : 0;
                else
                    flag = (tod >= rthStart || tod <= rthEnd) ? 1 : 0;
            }
            else                // chart Session Times (day session)
                flag = sc.IsDateTimeInDaySession(sc.BaseDateTimeIn[b]) ? 1 : 0;
            sc.Subgraph[CacheSG(0)].Arrays[CA_SESSION][b] = (float)flag;
            int p = b - 1;
            while (p >= 0 && (int)sc.Subgraph[CacheSG(0)].Arrays[CA_SESSION][p] != flag)
                --p;
            sc.Subgraph[CacheSG(0)].Arrays[CA_PREVSAME][b] = (float)p;

            float barDelta = 0.f;
            for (int t = 0; t < NUM_TIERS; ++t)
            {
                const int nb = V2Aggregate(sc, b, T[t].mult, B);
                float mx = 0.f;
                for (int j = 0; j < nb; ++j)
                {
                    const float m = V2Mag(T[t].metric, B[j].av, B[j].bv);
                    if (m > mx) mx = m;
                    if (t == 0) barDelta += B[j].av - B[j].bv;
                }
                sc.Subgraph[CacheSG(t)].Arrays[CA_MAXV][b] = mx;
            }
            sc.Subgraph[CacheSG(0)].Arrays[CA_BARDELTA][b] = barDelta;
        }
    }

    // PASS 2 — render.
    if (timing == 1)   // Live
    {
        for (int i = start; i <= last; ++i)
            V2RenderBar(sc, i, scope, T, maxSlots, labelSize, labelCol, divOn);
    }
    else               // On Bar Close
    {
        int prevFinal = sc.GetPersistentInt(0);
        if (sc.IsFullRecalculation || prevFinal > last)
            prevFinal = -1;

        V2BlankBar(sc, last);

        int f0 = prevFinal + 1;
        if (f0 < 1) f0 = 1;
        for (int i = f0; i <= last - 1; ++i)
            V2RenderBar(sc, i, scope, T, maxSlots, labelSize, labelCol, divOn);

        if (last - 1 > prevFinal)
            sc.SetPersistentInt(0, last - 1);
    }

    // ---- Per-tier alerts (deduped per bar, suppressed during recalculation) ---
    if (!sc.IsFullRecalculation)
    {
        const int active = (timing == 1) ? last : (last - 1);
        if (active >= 0)
        {
            for (int t = 0; t < NUM_TIERS; ++t)
            {
                if (!T[t].on || !T[t].alert)
                    continue;
                const int sigNow  = (int)sc.Subgraph[CacheSG(t)].Arrays[CA_BUBCNT][active];
                const int alerted = (int)sc.Subgraph[CacheSG(t)].Arrays[CA_ALERTED][active];
                if (sigNow > alerted)
                {
                    SCString msg;
                    msg.Format("ReconTapeV2: Tier %d bubble printed (%d on bar)", t + 1, sigNow);
                    sc.SetAlert(sc.Input[IN_ALERTSOUND].GetInt(), msg);
                    sc.Subgraph[CacheSG(t)].Arrays[CA_ALERTED][active] = (float)sigNow;
                }
            }
        }
    }
}
