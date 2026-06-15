// ReconTape.cpp
// =============================================================================
// Reconstructed Tape  (Sierra Chart ACSIL)
//
// Surfaces significant aggressive prints as semi-transparent variable-size
// circles at the exact price where they happened, and optionally collapses
// contiguous runs of significant levels into "Zones of Interest" rectangles.
// Red = buyers lifting the offer (ask aggressor / +delta), Blue = sellers
// hitting the bid (bid aggressor / -delta).
//
//   SG 0..MAX-1        Ask / Buy bubble slots  (red)
//   SG MAX..2*MAX-1    Bid / Sell bubble slots (blue)
//   SG 2*MAX           Hidden cache: baseline prefix sums + draw counts
//
// -----------------------------------------------------------------------------
// SIGNIFICANCE FILTER (per price level, vs a trailing baseline)
//   Each level's magnitude is scored against a baseline of recent per-bar max
//   prints over Baseline Length bars.
//     Mode 0  Z-Score:    z = (mag - mean)/stdev;  show if z >= Min Z-Score.
//     Mode 1  Percentile: rank of mag in the baseline; show if >= Min Percentile.
//   Bubble pixel size scales Point Size Small..Large with the score.
//
// METRIC (Print Metric input)
//   Bid/Ask Volume: each level's ask volume -> red, bid volume -> blue.
//   Delta:          each level's (ask - bid); +delta -> red, -delta -> blue.
//   Total Volume:   each level's (ask + bid); one bubble per level, sized by
//                   total volume and SHADED by delta imbalance (delta / total):
//                   big pale = absorption, big vivid = initiative.
//
// ZONES OF INTEREST (optional)
//   When enabled, the price ladder of each bar is walked tick by tick. A run of
//   Min Zone Ticks or more CONSECUTIVE levels that all pass the filter becomes a
//   single filled rectangle spanning that price range on that bar (the run ends
//   at the first level that fails or a price gap). Isolated passers (runs below
//   the minimum) still draw as bubbles. Zone color mirrors the metric: side
//   modes -> red/blue by the run's net side; Total Volume -> shaded by the run's
//   aggregate imbalance.
//
// LABEL (Bubble Label input): Off / Volume / Score / Delta / Imbalance % /
//   Volume+Delta. Applies to both bubbles and zones (zones show run aggregates).
//
// TIMING (Bubble Timing input): Live (default) updates the forming bar in place;
//   On Bar Close keeps the forming bar blank and finalizes each bar at close.
//
// -----------------------------------------------------------------------------
// BUILD NOTES (defensive — confirmed SC build-server patterns)
//   - No std::min/std::max/std::fabs, no <algorithm>: min/max are C macros.
//   - DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE: Data[] = price, Arrays[0][] =
//     pixel size, DataColor[] = color (AUTOCOLOR_NONE set first).
//   - Custom-string inputs read with .GetIndex().
//   - Data via sc.VolumeAtPriceForBars (s_VolumeAtPriceV2) with
//     sc.MaintainVolumeAtPriceData = 1.
//   - Zones use DRAWING_RECTANGLEHIGHLIGHT (non-EXT, respects the bar);
//     Color = border, SecondaryColor = fill.
//   - Labels/zones via sc.UseTool; ACSIL tool drawings auto-delete on
//     recalculation, so only incremental per-bar counts are tracked.
//   - Scale SCALE_SAMEASREGION; state in Subgraph extra arrays + 1 persistent int.
// =============================================================================

#include "sierrachart.h"

SCDLLName("ReconTape")

const int MAX_SLOTS  = 24;     // simultaneous bubbles per side per bar
const int MAX_ZONES  = 24;     // simultaneous zones per bar
const int MAX_LEVELS = 300;    // price levels buffered per bar

const int SG_COLORBAR = 2 * MAX_SLOTS;       // price-bar coloring
const int SG_DIVERG   = 2 * MAX_SLOTS + 1;   // divergence diamonds
const int CACHE_SG    = 2 * MAX_SLOTS + 2;   // hidden cache

// Buy/sell colors for bubbles (buy = blue, sell = red).
const COLORREF COL_BUY  = RGB(60, 120, 235);   // blue
const COLORREF COL_SELL = RGB(225, 60, 60);    // red
// Zones keep the original convention (buy = red, sell = blue).
const COLORREF ZCOL_BUY  = RGB(225, 60, 60);   // red
const COLORREF ZCOL_SELL = RGB(60, 120, 235);  // blue

inline int AskSG(int slot) { return slot; }
inline int BidSG(int slot) { return MAX_SLOTS + slot; }

// Cache (CACHE_SG) extra-array layout
const int CA_MAXV  = 0;   // bar's strongest print magnitude (baseline source)
const int CA_PS    = 1;   // prefix sum of CA_MAXV
const int CA_PS2   = 2;   // prefix sum of CA_MAXV^2
const int CA_PCNT  = 3;   // prefix count of print bars
const int CA_NASK  = 4;   // ask labels drawn on the bar
const int CA_NBID  = 5;   // bid labels drawn on the bar
const int CA_NZONE = 6;   // zones drawn on the bar
const int CA_ALERTED  = 7; // signal count already alerted for the bar
const int CA_BUBCNT   = 8; // bubbles drawn on the bar (independent of labels)
const int CA_BUBDELTA = 9; // net delta of the drawn bubbles on the bar

// ---- Input indices -----------------------------------------------------------
const int IN_BASELINE  = 0;
const int IN_MODE      = 1;
const int IN_MINZ      = 2;
const int IN_FULLZ     = 3;
const int IN_MINPCT    = 4;
const int IN_SIZESMALL = 5;
const int IN_SIZELARGE = 6;
const int IN_LABEL     = 7;
const int IN_LABELSIZE = 8;
const int IN_LABELCOL  = 9;
const int IN_METRIC    = 10;
const int IN_TIMING    = 11;
const int IN_MAXBUB    = 12;
const int IN_ZONES     = 13;
const int IN_ZONEMIN   = 14;
const int IN_ZONETRANSP= 15;
const int IN_ALERT     = 16;   // alert on new bubble/zone
const int IN_ALERTSOUND= 17;   // alert sound number
const int IN_MEASURE   = 18;   // measure reaction
const int IN_REACTWIN  = 19;   // reaction window (bars)
const int IN_SCALPTGT  = 20;   // scalp target (ticks)
const int IN_COLORBARMIN = 21; // color bar when >= N bubbles (0 = off)
const int IN_METRICFILT  = 22; // absolute metric floor for a bubble (0 = off)
const int IN_DIVERG      = 23; // show divergence diamonds on colored bars
const int IN_DIVSIZE     = 24; // diamond size

const int MIN_SAMPLES = 2;   // min baseline length / samples to score

const int LN_BASE      = 80000000;    // bubble labels
const int LN_ZONE_RECT = 200000000;   // zone rectangles
const int LN_ZONE_LBL  = 300000000;   // zone labels

inline int AskLabelLN(int i, int s) { return LN_BASE + (i * MAX_SLOTS + s) * 2; }
inline int BidLabelLN(int i, int s) { return LN_BASE + (i * MAX_SLOTS + s) * 2 + 1; }
inline int ZoneRectLN(int i, int z) { return LN_ZONE_RECT + i * MAX_ZONES + z; }
inline int ZoneLblLN (int i, int z) { return LN_ZONE_LBL  + i * MAX_ZONES + z; }

// Candidate kept while selecting the top bubbles for a bar.
//   mag=scored magnitude, met=raw z/percentile, delta=ask-bid, total=ask+bid.
struct ReconCand { float price; float sig; float mag; float met; float delta; float total; };

// One buffered price level for a bar (sorted by pit for zone runs).
//   sigA/sigB >= 0 means that side passed the filter (-1 = no candidate).
struct ReconLvl { int pit; float av; float bv;
                  float sigA; float magA; float metA;
                  float sigB; float magB; float metB; };

// -----------------------------------------------------------------------------
static float ReconSignificance(SCStudyInterfaceRef sc, float mag, int mode,
                               float mean, float sd, float minZ, float fullZ,
                               float minPct, int lo, int hi, float &outMetric)
{
    outMetric = 0.f;
    if (mag <= 0.f)
        return -1.f;

    if (mode == 0)
    {
        float z;
        if (sd > 0.f) z = (mag - mean) / sd;
        else          z = (mag > mean) ? fullZ : 0.f;
        outMetric = z;
        if (z < minZ) return -1.f;
        const float denom = fullZ - minZ;
        float sig = (denom > 0.f) ? (z - minZ) / denom : 1.f;
        if (sig < 0.f) sig = 0.f;
        if (sig > 1.f) sig = 1.f;
        return sig;
    }
    else
    {
        int le = 0, tot = 0;
        for (int b = lo; b <= hi; ++b)
        {
            const float mv = sc.Subgraph[CACHE_SG].Arrays[CA_MAXV][b];
            if (mv > 0.f) { ++tot; if (mv <= mag) ++le; }
        }
        if (tot < MIN_SAMPLES) return -1.f;
        const float pct = 100.f * (float)le / (float)tot;
        outMetric = pct;
        if (pct < minPct) return -1.f;
        const float denom = 100.f - minPct;
        float sig = (denom > 0.f) ? (pct - minPct) / denom : 1.f;
        if (sig < 0.f) sig = 0.f;
        if (sig > 1.f) sig = 1.f;
        return sig;
    }
}

static void TopKInsert(ReconCand *arr, int &n, int maxN, const ReconCand &c)
{
    if (n < maxN)
    {
        int p = n;
        while (p > 0 && arr[p - 1].sig < c.sig) { arr[p] = arr[p - 1]; --p; }
        arr[p] = c; ++n;
    }
    else if (n > 0 && c.sig > arr[n - 1].sig)
    {
        int p = n - 1;
        while (p > 0 && arr[p - 1].sig < c.sig) { arr[p] = arr[p - 1]; --p; }
        arr[p] = c;
    }
}

// Two-tone intensity color from a signed imbalance ratio in [-1, +1].
//   ratio >= 0 (net buying) -> cPos, ratio < 0 (net selling) -> cNeg;
//   pale when balanced, vivid when lopsided.
static COLORREF ReconShade(float ratio, COLORREF cPos, COLORREF cNeg)
{
    if (ratio > 1.f)  ratio = 1.f;
    if (ratio < -1.f) ratio = -1.f;
    const float a = (ratio >= 0.f) ? ratio : -ratio;
    const COLORREF full = (ratio >= 0.f) ? cPos : cNeg;
    const int fr = GetRValue(full), fg = GetGValue(full), fb = GetBValue(full);
    const int pr = (int)(fr + (255 - fr) * 0.62f);
    const int pg = (int)(fg + (255 - fg) * 0.62f);
    const int pb = (int)(fb + (255 - fb) * 0.62f);
    return RGB((int)(pr + (fr - pr) * a),
               (int)(pg + (fg - pg) * a),
               (int)(pb + (fb - pb) * a));
}

// Format a label for the selected mode. 1 Vol 2 Score 3 Delta 4 Imbal% 5 Vol+Delta.
static void ReconFmt(SCString &t, int labelMode, int mode, const ReconCand &c)
{
    const int d = (int)((c.delta >= 0.f) ? (c.delta + 0.5f) : (c.delta - 0.5f));
    if (labelMode == 1)
        t.Format("%d", (int)(c.mag + 0.5f));
    else if (labelMode == 2)
    {
        if (mode == 0) t.Format("%.1f", c.met);
        else           t.Format("%.0f", c.met);
    }
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
static void ReconLabel(SCStudyInterfaceRef sc, int lineNumber, bool show,
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

// Add/adjust (or delete) a zone rectangle spanning just bar i.
static void ReconZone(SCStudyInterfaceRef sc, int lineNumber, bool show, int i,
                      int last, float loPrice, float hiPrice, COLORREF color, int transp)
{
    if (!show)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNumber);
        return;
    }
    SCDateTime bt = sc.BaseDateTimeIn[i];
    SCDateTime et;
    if (i < last)      et = sc.BaseDateTimeIn[i + 1];
    else if (i > 0)    et = bt + (sc.BaseDateTimeIn[i] - sc.BaseDateTimeIn[i - 1]);
    else               et = bt;

    s_UseTool Tool;
    Tool.Clear();
    Tool.ChartNumber      = sc.ChartNumber;
    Tool.DrawingType      = DRAWING_RECTANGLEHIGHLIGHT;
    Tool.LineNumber       = lineNumber;
    Tool.BeginDateTime    = bt;
    Tool.EndDateTime      = et;
    Tool.BeginValue       = loPrice;
    Tool.EndValue         = hiPrice;
    Tool.Color            = color;     // border
    Tool.SecondaryColor   = color;     // fill
    Tool.TransparencyLevel = transp;
    Tool.AddMethod        = UTAM_ADD_OR_ADJUST;
    sc.UseTool(Tool);
}

static void ReconClearBubbles(SCStudyInterfaceRef sc, int i)
{
    for (int k = 0; k < MAX_SLOTS; ++k)
    {
        sc.Subgraph[AskSG(k)][i]           = 0.f;
        sc.Subgraph[AskSG(k)].Arrays[0][i] = 0.f;
        sc.Subgraph[BidSG(k)][i]           = 0.f;
        sc.Subgraph[BidSG(k)].Arrays[0][i] = 0.f;
    }
    sc.Subgraph[SG_COLORBAR][i] = 0.f;   // 0 = leave bar default
    sc.Subgraph[SG_DIVERG][i]   = 0.f;   // 0 = no diamond
}

// Blank a bar entirely: clear bubbles, labels, and zones.
static void ReconBlankBar(SCStudyInterfaceRef sc, int i)
{
    ReconClearBubbles(sc, i);
    const int prevA = (int)sc.Subgraph[CACHE_SG].Arrays[CA_NASK][i];
    const int prevB = (int)sc.Subgraph[CACHE_SG].Arrays[CA_NBID][i];
    const int prevZ = (int)sc.Subgraph[CACHE_SG].Arrays[CA_NZONE][i];
    SCString empty;
    for (int k = 0; k < prevA; ++k) ReconLabel(sc, AskLabelLN(i, k), false, i, 0.f, empty, 0, 0);
    for (int k = 0; k < prevB; ++k) ReconLabel(sc, BidLabelLN(i, k), false, i, 0.f, empty, 0, 0);
    for (int z = 0; z < prevZ; ++z)
    {
        ReconZone(sc, ZoneRectLN(i, z), false, i, 0, 0.f, 0.f, 0, 0);
        ReconLabel(sc, ZoneLblLN(i, z), false, i, 0.f, empty, 0, 0);
    }
    sc.Subgraph[CACHE_SG].Arrays[CA_NASK][i]   = 0.f;
    sc.Subgraph[CACHE_SG].Arrays[CA_NBID][i]   = 0.f;
    sc.Subgraph[CACHE_SG].Arrays[CA_NZONE][i]  = 0.f;
    sc.Subgraph[CACHE_SG].Arrays[CA_BUBCNT][i] = 0.f;
}

// Score every level on bar i; draw zones (runs) and bubbles (singles) + labels.
static void ReconRenderBar(SCStudyInterfaceRef sc, int i, int last, int baseLen,
                           int mode, int metric, float minZ, float fullZ, float minPct,
                           float sizeSmall, float sizeLarge,
                           int labelMode, int labelSize, COLORREF labelCol,
                           int maxSlots, bool zonesOn, int minZoneTicks, int zoneTransp,
                           int colorBarMin, float metricFilter, bool divOn)
{
    ReconClearBubbles(sc, i);
    const int prevA = (int)sc.Subgraph[CACHE_SG].Arrays[CA_NASK][i];
    const int prevB = (int)sc.Subgraph[CACHE_SG].Arrays[CA_NBID][i];
    const int prevZ = (int)sc.Subgraph[CACHE_SG].Arrays[CA_NZONE][i];

    int nA = 0, nB = 0, nZ = 0;
    bool barColored = false;
    ReconCand asks[MAX_SLOTS];
    ReconCand bids[MAX_SLOTS];

    if (i >= 1)
    {
        int lo = i - baseLen;
        if (lo < 0) lo = 0;
        const int hi = i - 1;

        const float hiPS  = sc.Subgraph[CACHE_SG].Arrays[CA_PS][hi];
        const float hiPS2 = sc.Subgraph[CACHE_SG].Arrays[CA_PS2][hi];
        const float hiCN  = sc.Subgraph[CACHE_SG].Arrays[CA_PCNT][hi];
        const float loPS  = (lo > 0) ? sc.Subgraph[CACHE_SG].Arrays[CA_PS][lo - 1]   : 0.f;
        const float loPS2 = (lo > 0) ? sc.Subgraph[CACHE_SG].Arrays[CA_PS2][lo - 1]  : 0.f;
        const float loCN  = (lo > 0) ? sc.Subgraph[CACHE_SG].Arrays[CA_PCNT][lo - 1] : 0.f;

        const float sum  = hiPS  - loPS;
        const float sum2 = hiPS2 - loPS2;
        const float cnt  = hiCN  - loCN;

        if (cnt >= (float)MIN_SAMPLES)
        {
            const float mean = sum / cnt;
            float var = sum2 / cnt - mean * mean;
            if (var < 0.f) var = 0.f;
            const float sd = sqrt(var);
            const float tick = sc.TickSize;

            // ---- Build per-level buffer (significance per side) --------------
            ReconLvl lvl[MAX_LEVELS];
            int n = 0;
            const int levels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(i);
            for (int k = 0; k < levels && n < MAX_LEVELS; ++k)
            {
                const s_VolumeAtPriceV2 *vap = NULL;
                if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(i, k, &vap))
                    break;
                if (vap == NULL)
                    continue;

                const float av = (float)vap->AskVolume;
                const float bv = (float)vap->BidVolume;
                ReconLvl L;
                L.pit = vap->PriceInTicks;
                L.av = av; L.bv = bv;
                L.sigA = -1.f; L.sigB = -1.f; L.magA = 0.f; L.magB = 0.f; L.metA = 0.f; L.metB = 0.f;

                if (metric == 0)        // Bid/Ask Volume
                {
                    L.sigA = ReconSignificance(sc, av, mode, mean, sd, minZ, fullZ, minPct, lo, hi, L.metA); L.magA = av;
                    L.sigB = ReconSignificance(sc, bv, mode, mean, sd, minZ, fullZ, minPct, lo, hi, L.metB); L.magB = bv;
                }
                else if (metric == 1)   // Delta
                {
                    const float d = av - bv;
                    if (d > 0.f)      { L.sigA = ReconSignificance(sc, d,  mode, mean, sd, minZ, fullZ, minPct, lo, hi, L.metA); L.magA = d; }
                    else if (d < 0.f) { L.sigB = ReconSignificance(sc, -d, mode, mean, sd, minZ, fullZ, minPct, lo, hi, L.metB); L.magB = -d; }
                }
                else                    // Total Volume
                {
                    L.sigA = ReconSignificance(sc, av + bv, mode, mean, sd, minZ, fullZ, minPct, lo, hi, L.metA); L.magA = av + bv;
                }

                // Absolute metric floor: drop a side whose magnitude is below it.
                if (metricFilter > 0.f)
                {
                    if (L.magA < metricFilter) L.sigA = -1.f;
                    if (L.magB < metricFilter) L.sigB = -1.f;
                }

                // Insertion-sort by pit ascending.
                int p = n;
                while (p > 0 && lvl[p - 1].pit > L.pit) { lvl[p] = lvl[p - 1]; --p; }
                lvl[p] = L;
                ++n;
            }

            bool zoned[MAX_LEVELS];
            for (int k = 0; k < n; ++k) zoned[k] = false;

            // ---- Zones: contiguous runs of passing levels -------------------
            if (zonesOn)
            {
                int j = 0;
                while (j < n)
                {
                    const bool pass = (lvl[j].sigA >= 0.f || lvl[j].sigB >= 0.f);
                    if (!pass) { ++j; continue; }

                    int e = j;
                    while (e + 1 < n
                           && (lvl[e + 1].sigA >= 0.f || lvl[e + 1].sigB >= 0.f)
                           && lvl[e + 1].pit == lvl[e].pit + 1)
                        ++e;

                    const int runLen = e - j + 1;
                    if (runLen >= minZoneTicks && nZ < MAX_ZONES)
                    {
                        float sumAv = 0.f, sumBv = 0.f, maxMet = 0.f;
                        for (int m = j; m <= e; ++m)
                        {
                            sumAv += lvl[m].av; sumBv += lvl[m].bv;
                            zoned[m] = true;
                            const float mt = (lvl[m].sigA >= 0.f) ? lvl[m].metA : lvl[m].metB;
                            if (mt > maxMet) maxMet = mt;
                        }
                        const float sumD = sumAv - sumBv;
                        const float sumT = sumAv + sumBv;
                        const float loP = lvl[j].pit * tick - tick * 0.5f;
                        const float hiP = lvl[e].pit * tick + tick * 0.5f;

                        COLORREF col;
                        if (metric == 2) col = ReconShade((sumT > 0.f) ? sumD / sumT : 0.f, ZCOL_BUY, ZCOL_SELL);
                        else             col = (sumD >= 0.f) ? ZCOL_BUY : ZCOL_SELL;

                        ReconZone(sc, ZoneRectLN(i, nZ), true, i, last, loP, hiP, col, zoneTransp);

                        if (labelMode != 0)
                        {
                            const float mid = (lvl[j].pit + lvl[e].pit) * 0.5f * tick;
                            ReconCand zc = { mid, 0.f, sumT, maxMet, sumD, sumT };
                            SCString t; ReconFmt(t, labelMode, mode, zc);
                            ReconLabel(sc, ZoneLblLN(i, nZ), true, i, mid, t, labelCol, labelSize);
                        }
                        ++nZ;
                    }
                    j = e + 1;
                }
            }

            // ---- Bubbles: passing levels NOT absorbed into a zone -----------
            for (int k = 0; k < n; ++k)
            {
                if (zoned[k]) continue;
                const float price = lvl[k].pit * tick;
                const float dlt = lvl[k].av - lvl[k].bv;
                const float tot = lvl[k].av + lvl[k].bv;
                if (lvl[k].sigA >= 0.f)
                { ReconCand c = { price, lvl[k].sigA, lvl[k].magA, lvl[k].metA, dlt, tot }; TopKInsert(asks, nA, maxSlots, c); }
                if (lvl[k].sigB >= 0.f)
                { ReconCand c = { price, lvl[k].sigB, lvl[k].magB, lvl[k].metB, dlt, tot }; TopKInsert(bids, nB, maxSlots, c); }
            }

            // Net delta of the qualifying bubbles (for the color-bar feature).
            float sumBubD = 0.f;
            for (int k = 0; k < nA; ++k) sumBubD += asks[k].delta;
            for (int k = 0; k < nB; ++k) sumBubD += bids[k].delta;
            sc.Subgraph[CACHE_SG].Arrays[CA_BUBDELTA][i] = sumBubD;

            barColored = (colorBarMin > 0 && (nA + nB) >= colorBarMin);

            if (barColored)
            {
                // The whole bar carries the signal; hide the individual bubbles.
                sc.Subgraph[SG_COLORBAR][i]           = sc.Close[i];   // in-range nonzero
                sc.Subgraph[SG_COLORBAR].DataColor[i] = (sumBubD >= 0.f) ? COL_BUY : COL_SELL;

                // Divergence: delta winner disagrees with the candle direction.
                if (divOn)
                {
                    const float op = sc.Open[i];
                    const float cl = sc.Close[i];
                    if (sumBubD > 0.f && cl < op)        // buying delta into a DOWN candle -> bullish
                    {
                        sc.Subgraph[SG_DIVERG][i]           = sc.Low[i] - 2.f * sc.TickSize;
                        sc.Subgraph[SG_DIVERG].DataColor[i] = RGB(0, 200, 0);     // green
                    }
                    else if (sumBubD < 0.f && cl > op)   // selling delta into an UP candle -> bearish
                    {
                        sc.Subgraph[SG_DIVERG][i]           = sc.High[i] + 2.f * sc.TickSize;
                        sc.Subgraph[SG_DIVERG].DataColor[i] = RGB(255, 140, 0);   // orange
                    }
                }
            }
            else
            {
                // Draw bubbles.
                for (int k = 0; k < nA; ++k)
                {
                    sc.Subgraph[AskSG(k)][i]           = asks[k].price;
                    sc.Subgraph[AskSG(k)].Arrays[0][i] = sizeSmall + asks[k].sig * (sizeLarge - sizeSmall);
                    sc.Subgraph[AskSG(k)].DataColor[i] =
                        (metric == 2)
                            ? ((asks[k].delta >= 0.f) ? COL_BUY : COL_SELL)   // solid by net side
                            : sc.Subgraph[AskSG(k)].PrimaryColor;
                }
                for (int k = 0; k < nB; ++k)
                {
                    sc.Subgraph[BidSG(k)][i]           = bids[k].price;
                    sc.Subgraph[BidSG(k)].Arrays[0][i] = sizeSmall + bids[k].sig * (sizeLarge - sizeSmall);
                    sc.Subgraph[BidSG(k)].DataColor[i] = sc.Subgraph[BidSG(k)].PrimaryColor;
                }

                // Bubble labels.
                if (labelMode != 0)
                {
                    for (int k = 0; k < nA; ++k)
                    {
                        SCString t; ReconFmt(t, labelMode, mode, asks[k]);
                        ReconLabel(sc, AskLabelLN(i, k), true, i, asks[k].price, t, labelCol, labelSize);
                    }
                    for (int k = 0; k < nB; ++k)
                    {
                        SCString t; ReconFmt(t, labelMode, mode, bids[k]);
                        ReconLabel(sc, BidLabelLN(i, k), true, i, bids[k].price, t, labelCol, labelSize);
                    }
                }
            }
        }
    }

    // ---- Delete drawings no longer used --------------------------------------
    const int keepA = (labelMode != 0 && !barColored) ? nA : 0;
    const int keepB = (labelMode != 0 && !barColored) ? nB : 0;
    SCString empty;
    for (int k = keepA; k < prevA; ++k) ReconLabel(sc, AskLabelLN(i, k), false, i, 0.f, empty, 0, 0);
    for (int k = keepB; k < prevB; ++k) ReconLabel(sc, BidLabelLN(i, k), false, i, 0.f, empty, 0, 0);
    for (int z = nZ; z < prevZ; ++z)
    {
        ReconZone(sc, ZoneRectLN(i, z), false, i, 0, 0.f, 0.f, 0, 0);
        ReconLabel(sc, ZoneLblLN(i, z), false, i, 0.f, empty, 0, 0);
    }

    sc.Subgraph[CACHE_SG].Arrays[CA_NASK][i]   = (float)keepA;
    sc.Subgraph[CACHE_SG].Arrays[CA_NBID][i]   = (float)keepB;
    sc.Subgraph[CACHE_SG].Arrays[CA_NZONE][i]  = (float)nZ;
    sc.Subgraph[CACHE_SG].Arrays[CA_BUBCNT][i] = (float)(nA + nB);
}

SCSFExport scsf_ReconTape(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Recon Tape (Reconstructed Tape Bubbles)";
        sc.GraphRegion      = 0;
        sc.AutoLoop         = 0;
        sc.ScaleRangeType   = SCALE_SAMEASREGION;
        sc.DrawZeros        = 0;
        sc.ValueFormat      = VALUEFORMAT_INHERITED;
        sc.UpdateAlways     = 1;
        sc.MaintainVolumeAtPriceData = 1;

        for (int k = 0; k < MAX_SLOTS; ++k)
        {
            SCString an; an.Format("Ask/Buy %d", k + 1);
            sc.Subgraph[AskSG(k)].Name         = an;
            sc.Subgraph[AskSG(k)].DrawStyle    = DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE;
            sc.Subgraph[AskSG(k)].PrimaryColor = COL_BUY;     // buy = blue
            sc.Subgraph[AskSG(k)].DrawZeros    = 0;
            sc.Subgraph[AskSG(k)].AutoColoring = AUTOCOLOR_NONE;

            SCString bn; bn.Format("Bid/Sell %d", k + 1);
            sc.Subgraph[BidSG(k)].Name         = bn;
            sc.Subgraph[BidSG(k)].DrawStyle    = DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE;
            sc.Subgraph[BidSG(k)].PrimaryColor = COL_SELL;    // sell = red
            sc.Subgraph[BidSG(k)].DrawZeros    = 0;
            sc.Subgraph[BidSG(k)].AutoColoring = AUTOCOLOR_NONE;
        }

        sc.Subgraph[SG_COLORBAR].Name         = "Bar Color";
        sc.Subgraph[SG_COLORBAR].DrawStyle    = DRAWSTYLE_COLOR_BAR;
        sc.Subgraph[SG_COLORBAR].PrimaryColor = RGB(180, 180, 180);
        sc.Subgraph[SG_COLORBAR].DrawZeros    = 0;
        sc.Subgraph[SG_COLORBAR].AutoColoring = AUTOCOLOR_NONE;

        sc.Subgraph[SG_DIVERG].Name         = "Divergence";
        sc.Subgraph[SG_DIVERG].DrawStyle    = DRAWSTYLE_DIAMOND;
        sc.Subgraph[SG_DIVERG].PrimaryColor = RGB(255, 140, 0);
        sc.Subgraph[SG_DIVERG].LineWidth    = 4;
        sc.Subgraph[SG_DIVERG].DrawZeros    = 0;
        sc.Subgraph[SG_DIVERG].AutoColoring = AUTOCOLOR_NONE;

        sc.Subgraph[CACHE_SG].Name      = "Cache (internal)";
        sc.Subgraph[CACHE_SG].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[CACHE_SG].DrawZeros = 0;

        sc.Input[IN_BASELINE].Name = "Baseline Length (bars)";
        sc.Input[IN_BASELINE].SetInt(50);
        sc.Input[IN_BASELINE].SetIntLimits(MIN_SAMPLES, 100000);

        sc.Input[IN_MODE].Name = "Scoring Mode";
        sc.Input[IN_MODE].SetCustomInputStrings("Baseline Z-Score;Reference Percentile");
        sc.Input[IN_MODE].SetCustomInputIndex(0);

        sc.Input[IN_MINZ].Name = "Min Z-Score (mode: Z-Score)";
        sc.Input[IN_MINZ].SetFloat(2.0f);
        sc.Input[IN_MINZ].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_FULLZ].Name = "Z-Score For Max Size (mode: Z-Score)";
        sc.Input[IN_FULLZ].SetFloat(6.0f);
        sc.Input[IN_FULLZ].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_MINPCT].Name = "Min Percentile (mode: Percentile)";
        sc.Input[IN_MINPCT].SetFloat(90.0f);
        sc.Input[IN_MINPCT].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_SIZESMALL].Name = "Point Size Small (px, at threshold)";
        sc.Input[IN_SIZESMALL].SetInt(2);
        sc.Input[IN_SIZESMALL].SetIntLimits(1, 200);

        sc.Input[IN_SIZELARGE].Name = "Point Size Large (px, full strength)";
        sc.Input[IN_SIZELARGE].SetInt(16);
        sc.Input[IN_SIZELARGE].SetIntLimits(1, 200);

        sc.Input[IN_LABEL].Name = "Bubble Label";
        sc.Input[IN_LABEL].SetCustomInputStrings("Off;Volume;Score;Delta;Imbalance %;Volume+Delta");
        sc.Input[IN_LABEL].SetCustomInputIndex(1);

        sc.Input[IN_LABELSIZE].Name = "Label Font Size";
        sc.Input[IN_LABELSIZE].SetInt(8);
        sc.Input[IN_LABELSIZE].SetIntLimits(4, 72);

        sc.Input[IN_LABELCOL].Name = "Label Color";
        sc.Input[IN_LABELCOL].SetColor(RGB(255, 255, 255));

        sc.Input[IN_METRIC].Name = "Print Metric";
        sc.Input[IN_METRIC].SetCustomInputStrings("Bid/Ask Volume;Delta;Total Volume");
        sc.Input[IN_METRIC].SetCustomInputIndex(0);

        sc.Input[IN_TIMING].Name = "Bubble Timing";
        sc.Input[IN_TIMING].SetCustomInputStrings("On Bar Close;Live");
        sc.Input[IN_TIMING].SetCustomInputIndex(1);

        sc.Input[IN_MAXBUB].Name = "Max Bubbles Per Side";
        sc.Input[IN_MAXBUB].SetInt(MAX_SLOTS);
        sc.Input[IN_MAXBUB].SetIntLimits(1, MAX_SLOTS);

        sc.Input[IN_ZONES].Name = "Zones of Interest";
        sc.Input[IN_ZONES].SetCustomInputStrings("Off;On");
        sc.Input[IN_ZONES].SetCustomInputIndex(0);

        sc.Input[IN_ZONEMIN].Name = "Min Zone Ticks (consecutive)";
        sc.Input[IN_ZONEMIN].SetInt(2);
        sc.Input[IN_ZONEMIN].SetIntLimits(2, 1000);

        sc.Input[IN_ZONETRANSP].Name = "Zone Transparency (0-100)";
        sc.Input[IN_ZONETRANSP].SetInt(70);
        sc.Input[IN_ZONETRANSP].SetIntLimits(0, 100);

        sc.Input[IN_ALERT].Name = "Alert On New Bubble";
        sc.Input[IN_ALERT].SetCustomInputStrings("Off;On");
        sc.Input[IN_ALERT].SetCustomInputIndex(0);

        sc.Input[IN_ALERTSOUND].Name = "Alert Sound Number";
        sc.Input[IN_ALERTSOUND].SetInt(1);
        sc.Input[IN_ALERTSOUND].SetIntLimits(0, 100);

        sc.Input[IN_MEASURE].Name = "Measure Reaction (logs summary)";
        sc.Input[IN_MEASURE].SetCustomInputStrings("Off;On");
        sc.Input[IN_MEASURE].SetCustomInputIndex(0);

        sc.Input[IN_REACTWIN].Name = "Reaction Window (bars)";
        sc.Input[IN_REACTWIN].SetInt(3);
        sc.Input[IN_REACTWIN].SetIntLimits(1, 1000);

        sc.Input[IN_SCALPTGT].Name = "Scalp Target (ticks)";
        sc.Input[IN_SCALPTGT].SetInt(4);
        sc.Input[IN_SCALPTGT].SetIntLimits(1, 1000);

        sc.Input[IN_COLORBARMIN].Name = "Color Bar Min Bubbles (0=disabled)";
        sc.Input[IN_COLORBARMIN].SetInt(0);
        sc.Input[IN_COLORBARMIN].SetIntLimits(0, 1000);

        sc.Input[IN_METRICFILT].Name = "Print Metric Filter (0=disabled)";
        sc.Input[IN_METRICFILT].SetFloat(0.0f);
        sc.Input[IN_METRICFILT].SetFloatLimits(0.0f, 100000000.0f);

        sc.Input[IN_DIVERG].Name = "Divergence Diamonds (on colored bars)";
        sc.Input[IN_DIVERG].SetCustomInputStrings("Off;On");
        sc.Input[IN_DIVERG].SetCustomInputIndex(1);

        sc.Input[IN_DIVSIZE].Name = "Diamond Size";
        sc.Input[IN_DIVSIZE].SetInt(4);
        sc.Input[IN_DIVSIZE].SetIntLimits(1, 50);

        return;
    }

    const int last = sc.ArraySize - 1;
    if (last < 0)
        return;

    // SetDefaults only runs when the study is first added, so force the
    // color-bar subgraph's draw style here too — this makes the feature work
    // on study instances added before this build without re-adding the study.
    if (sc.Subgraph[SG_COLORBAR].DrawStyle != DRAWSTYLE_COLOR_BAR)
        sc.Subgraph[SG_COLORBAR].DrawStyle = DRAWSTYLE_COLOR_BAR;

    if (sc.VolumeAtPriceForBars == NULL)
        return;
    if ((int)sc.VolumeAtPriceForBars->GetNumberOfBars() < sc.ArraySize)
        return;

    int baseLen = sc.Input[IN_BASELINE].GetInt();
    if (baseLen < MIN_SAMPLES) baseLen = MIN_SAMPLES;
    const int    mode      = sc.Input[IN_MODE].GetIndex();
    const float  minZ      = sc.Input[IN_MINZ].GetFloat();
    const float  fullZ     = sc.Input[IN_FULLZ].GetFloat();
    const float  minPct    = sc.Input[IN_MINPCT].GetFloat();
    const float  sizeSmall = (float)sc.Input[IN_SIZESMALL].GetInt();
    const float  sizeLarge = (float)sc.Input[IN_SIZELARGE].GetInt();
    const int    labelMode = sc.Input[IN_LABEL].GetIndex();
    const int    labelSize = sc.Input[IN_LABELSIZE].GetInt();
    const COLORREF labelCol= sc.Input[IN_LABELCOL].GetColor();
    const int    metric    = sc.Input[IN_METRIC].GetIndex();
    const int    timing    = sc.Input[IN_TIMING].GetIndex();
    int maxSlots = sc.Input[IN_MAXBUB].GetInt();
    if (maxSlots < 1) maxSlots = 1;
    if (maxSlots > MAX_SLOTS) maxSlots = MAX_SLOTS;
    const bool   zonesOn   = (sc.Input[IN_ZONES].GetIndex() == 1);
    int minZoneTicks = sc.Input[IN_ZONEMIN].GetInt();
    if (minZoneTicks < 2) minZoneTicks = 2;
    const int    zoneTransp  = sc.Input[IN_ZONETRANSP].GetInt();
    const int    colorBarMin = sc.Input[IN_COLORBARMIN].GetInt();
    const float  metricFilter= sc.Input[IN_METRICFILT].GetFloat();
    const bool   divOn       = (sc.Input[IN_DIVERG].GetIndex() == 1);

    // Force the divergence subgraph settings at runtime so they apply to a
    // study instance added before this build (SetDefaults does not re-run).
    if (sc.Subgraph[SG_DIVERG].DrawStyle != DRAWSTYLE_DIAMOND)
        sc.Subgraph[SG_DIVERG].DrawStyle = DRAWSTYLE_DIAMOND;
    sc.Subgraph[SG_DIVERG].AutoColoring = AUTOCOLOR_NONE;
    sc.Subgraph[SG_DIVERG].LineWidth    = sc.Input[IN_DIVSIZE].GetInt();

    int start = sc.UpdateStartIndex;
    if (start < 0) start = 0;

    // PASS 1 — per-bar strongest-print magnitude + baseline prefix sums.
    for (int b = start; b <= last; ++b)
    {
        float barMax = 0.f;
        const int levels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(b);
        for (int k = 0; k < levels; ++k)
        {
            const s_VolumeAtPriceV2 *vap = NULL;
            if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(b, k, &vap))
                break;
            if (vap == NULL)
                continue;
            const float av = (float)vap->AskVolume;
            const float bv = (float)vap->BidVolume;
            if (metric == 0)
            {
                if (av > barMax) barMax = av;
                if (bv > barMax) barMax = bv;
            }
            else if (metric == 1)
            {
                const float ad = (av > bv) ? (av - bv) : (bv - av);
                if (ad > barMax) barMax = ad;
            }
            else
            {
                const float tv = av + bv;
                if (tv > barMax) barMax = tv;
            }
        }

        sc.Subgraph[CACHE_SG].Arrays[CA_MAXV][b] = barMax;
        const float prevPS  = (b > 0) ? sc.Subgraph[CACHE_SG].Arrays[CA_PS][b - 1]   : 0.f;
        const float prevPS2 = (b > 0) ? sc.Subgraph[CACHE_SG].Arrays[CA_PS2][b - 1]  : 0.f;
        const float prevCN  = (b > 0) ? sc.Subgraph[CACHE_SG].Arrays[CA_PCNT][b - 1] : 0.f;
        const float had     = (barMax > 0.f) ? 1.f : 0.f;
        sc.Subgraph[CACHE_SG].Arrays[CA_PS][b]   = prevPS  + barMax;
        sc.Subgraph[CACHE_SG].Arrays[CA_PS2][b]  = prevPS2 + barMax * barMax;
        sc.Subgraph[CACHE_SG].Arrays[CA_PCNT][b] = prevCN  + had;
    }

    // PASS 2 — render.
    if (timing == 1)   // Live
    {
        for (int i = start; i <= last; ++i)
            ReconRenderBar(sc, i, last, baseLen, mode, metric, minZ, fullZ, minPct,
                           sizeSmall, sizeLarge, labelMode, labelSize, labelCol,
                           maxSlots, zonesOn, minZoneTicks, zoneTransp, colorBarMin, metricFilter, divOn);
    }
    else               // On Bar Close
    {
        int prevFinal = sc.GetPersistentInt(0);
        if (sc.IsFullRecalculation || prevFinal > last)
            prevFinal = -1;

        ReconBlankBar(sc, last);

        int f0 = prevFinal + 1;
        if (f0 < 1) f0 = 1;
        for (int i = f0; i <= last - 1; ++i)
            ReconRenderBar(sc, i, last, baseLen, mode, metric, minZ, fullZ, minPct,
                           sizeSmall, sizeLarge, labelMode, labelSize, labelCol,
                           maxSlots, zonesOn, minZoneTicks, zoneTransp, colorBarMin, metricFilter, divOn);

        if (last - 1 > prevFinal)
            sc.SetPersistentInt(0, last - 1);
    }

    // =========================================================================
    // Real-time alert on a newly printed bubble/zone (deduped per bar by count,
    // and suppressed during historical recalculation).
    // =========================================================================
    if (sc.Input[IN_ALERT].GetIndex() == 1 && !sc.IsFullRecalculation)
    {
        const int active = (timing == 1) ? last : (last - 1);
        if (active >= 0)
        {
            const int sigNow = (int)(sc.Subgraph[CACHE_SG].Arrays[CA_BUBCNT][active]
                                   + sc.Subgraph[CACHE_SG].Arrays[CA_NZONE][active]);
            const int alerted = (int)sc.Subgraph[CACHE_SG].Arrays[CA_ALERTED][active];
            if (sigNow > alerted)
            {
                SCString msg;
                msg.Format("ReconTape: significant node printed (%d on bar)", sigNow);
                sc.SetAlert(sc.Input[IN_ALERTSOUND].GetInt(), msg);
                sc.Subgraph[CACHE_SG].Arrays[CA_ALERTED][active] = (float)sigNow;
            }
        }
    }

    // =========================================================================
    // Reaction measurement — for every bar that printed a bubble/zone, how far
    // price travels over the next Reaction Window bars. Logged once per full
    // recalculation so the scalp edge can be evaluated.
    // =========================================================================
    if (sc.Input[IN_MEASURE].GetIndex() == 1 && start == 0 && last >= 2)
    {
        int win = sc.Input[IN_REACTWIN].GetInt();
        if (win < 1) win = 1;
        const float tgt  = (float)sc.Input[IN_SCALPTGT].GetInt();
        const float tick = sc.TickSize;

        int cnt = 0, hitE = 0, hitU = 0, hitD = 0;
        double sumU = 0.0, sumD = 0.0;

        for (int b = 1; b + win <= last; ++b)
        {
            const int sig = (int)(sc.Subgraph[CACHE_SG].Arrays[CA_BUBCNT][b]
                                + sc.Subgraph[CACHE_SG].Arrays[CA_NZONE][b]);
            if (sig < 1)
                continue;

            const float ref = sc.Close[b];
            float hh = sc.High[b + 1], ll = sc.Low[b + 1];
            for (int j = b + 1; j <= b + win; ++j)
            {
                if (sc.High[j] > hh) hh = sc.High[j];
                if (sc.Low[j]  < ll) ll = sc.Low[j];
            }
            const float up = (tick > 0.f) ? (hh - ref) / tick : 0.f;
            const float dn = (tick > 0.f) ? (ref - ll) / tick : 0.f;
            ++cnt; sumU += up; sumD += dn;
            if (up >= tgt) ++hitU;
            if (dn >= tgt) ++hitD;
            if (up >= tgt || dn >= tgt) ++hitE;
        }

        SCString m;
        if (cnt > 0)
            m.Format("ReconTape reaction [win=%d, target=%.0f ticks, n=%d]: "
                     "avg up=%.1f, avg down=%.1f ticks; moved >= target: "
                     "either=%.0f%%, up=%.0f%%, down=%.0f%%",
                     win, tgt, cnt, sumU / cnt, sumD / cnt,
                     100.0 * hitE / cnt, 100.0 * hitU / cnt, 100.0 * hitD / cnt);
        else
            m.Format("ReconTape reaction: no completed signal bars to measure");
        sc.AddMessageToLog(m, 0);
    }
}
