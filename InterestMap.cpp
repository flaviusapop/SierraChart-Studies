// InterestMap.cpp
// =============================================================================
// Interest Map — Persistent Multi-Evidence Level Book  (Sierra Chart ACSIL)
//
// LEVEL-CENTRIC, not bar-centric: maintains a persistent book of price levels
// (buckets of Bucket Width ticks). Each level accumulates a time-decayed
// SCORE from four evidence streams, then is ranked; the top levels draw as
// horizontal bands extending right until broken.
//
// EVIDENCE (weights configurable):
//   VOLUME      bucket's total volume ranks >= Min Volume Percentile against
//               a rolling baseline of per-bar strongest buckets.
//   ABSORPTION  qualifying bucket whose |delta|/total is below Absorption Max
//               Imbalance — heavy two-way business, someone absorbing.
//   WHALE       Time & Sales aggregates (same-side prints within Whale
//               Aggregation ms, <=2 ticks apart) >= Whale Min Size hitting the
//               bucket. Historical reach limited to retained T&S; whales are
//               kept in a ledger and replayed after recalculations.
//   DEFENSE     price returns to the level and closes back on the defended
//               side -> score rises. Feedback none of the bubble studies have.
//
// SIDE — each level tracks buy vs sell evidence; dominant side colors the
// band (blue = buy interest below price, red = sell interest above) and
// defines defense/break direction.
//
// LIFECYCLE — touch counted when a bar trades into the band (after having
// left it); DEFENDED when the touch bar closes back on the defended side;
// BROKEN when a bar CLOSES through the far edge -> level removed. All
// evidence decays with half-life Decay Half-Life bars, so the map always
// reflects the current market.
//
// DISPLAY — top Max Levels Shown by score (>= Min Score) as rectangles from
// the level's creation bar to the current bar; opacity fixed, label options
// show score / touches / defenses ("S12 T3 D2").
//
// ALERTS — Approach: price within N ticks of a displayed level (once per
// visit). Whale-at-level: a committed whale lands inside a displayed level.
//
// DATA — VAP evidence is fully historical (Lookback bars). Whale evidence
// accumulates live from T&S (+ledger replay across recalcs within the SC
// session).
//
// -----------------------------------------------------------------------------
// BUILD NOTES (confirmed SC build-server patterns)
//   - No std::min/std::max/std::fabs, no <algorithm>.
//   - VAP via sc.VolumeAtPriceForBars->GetVAPElementAtIndex with
//     sc.MaintainVolumeAtPriceData = 1; guard NULL / GetNumberOfBars.
//   - DRAWING_RECTANGLEHIGHLIGHT: Color = border, SecondaryColor = fill.
//   - T&S: r.Type SC_TS_BID/SC_TS_ASK, price * sc.RealTimePriceMultiplier,
//     dedup via r.Sequence; do NOT read the intraday tick file (renko freeze).
//   - Heap state via GetPersistentPointer (delete on LastCallToFunction).
//   - Map time->bar with manual binary search over sc.BaseDateTimeIn.
//   - Intrabar guard: persistent last-processed bar; closed bars only.
//   - All study drawings deleted up front on full recalculation.
// =============================================================================

#include "sierrachart.h"

SCDLLName("InterestMap")

const int MAX_LVL    = 256;     // level book capacity
const int BASE_CAP   = 512;     // baseline ring capacity (bars)
const int WHALE_CAP  = 8192;    // whale ledger capacity
const int MAX_BUCKETS= 300;     // buckets buffered per bar

const int LN_RECT = 95000000;
const int LN_LBL  = 96000000;

const COLORREF COL_BUY  = RGB(60, 120, 235);
const COLORREF COL_SELL = RGB(225, 60, 60);

// ---- Inputs --------------------------------------------------------------------
const int IN_BUCKET    = 0;   // bucket width (ticks)
const int IN_BASELEN   = 1;   // baseline length (bars)
const int IN_MINPCT    = 2;   // min volume percentile
const int IN_ABSORBMAX = 3;   // absorption max imbalance (%)
const int IN_WHALEMIN  = 4;   // whale min aggregated size (0 = off)
const int IN_WHALEAGG  = 5;   // whale aggregation period (ms)
const int IN_W_VOL     = 6;   // weight: volume
const int IN_W_ABS     = 7;   // weight: absorption
const int IN_W_WHALE   = 8;   // weight: whale
const int IN_W_DEF     = 9;   // weight: defense
const int IN_HALFLIFE  = 10;  // decay half-life (bars)
const int IN_MAXSHOW   = 11;  // max levels shown
const int IN_MINSCORE  = 12;  // min score to show
const int IN_LOOKBACK  = 13;  // historical lookback (bars)
const int IN_TRANSP    = 14;  // band transparency
const int IN_LABEL     = 15;  // label mode
const int IN_LABELSIZE = 16;
const int IN_APPROACH  = 17;  // approach alert distance (ticks, 0 = off)
const int IN_WHALERT   = 18;  // whale-at-level alert
const int IN_ALERTSND  = 19;
const int IN_DEBUG     = 20;
const int IN_NAKEDDK   = 21;  // decay mode for never-touched (naked) levels
const int IN_ONBREAK   = 22;  // broken level handling: remove / clip & keep

const COLORREF COL_BROKEN = RGB(150, 150, 150);

// ---- Persistent ints/doubles -----------------------------------------------------
const int PI_LASTBAR  = 0;    // last processed CLOSED bar
const int PI_SEQ      = 1;    // last processed T&S sequence
const int PI_PENDSIDE = 2;    // whale aggregation pending side (-1 none)
const int PD_VOL      = 0;
const int PD_PV       = 1;
const int PD_LASTDT   = 2;
const int PD_LASTPX   = 3;
const int PD_FIRSTDT  = 4;

// ---- Book ----------------------------------------------------------------------
struct IMLevel
{
    bool   valid;
    int    id;          // unique (drawing line numbers)
    int    key;         // bucket key (price ticks / bucketTicks)
    float  volE, absE, whaleE, defE;   // decayed evidence components
    float  buyE, sellE;                // decayed side evidence
    int    touches, defends;
    int    createdBar;
    bool   inBand;      // price currently in band (touch state)
    bool   approachArmed; // approach alert armed (re-arms when price leaves)
    bool   drawn;       // currently displayed
    bool   broken;      // invalidated; kept & clipped when On Break = keep
    int    breakBar;    // bar that broke the level (right edge when clipped)
};

struct IMWhale { double dt; float price; float vol; int side; };

struct IMBook
{
    IMLevel  L[MAX_LVL];
    float    baseRing[BASE_CAP];   // per-bar strongest bucket total
    int      baseN, baseHead;
    IMWhale  W[WHALE_CAP];
    int      wCount, wHead;
    int      nextId;
};

struct IMBucket { int key; float av, bv; };

struct IMCfg
{
    int   bucketTicks;
    int   baseLen;
    float minPct;
    float absorbMax;     // %
    float whaleMin;
    float wVol, wAbs, wWhale, wDef;
    float decayMul;      // per-bar multiplier
    int   maxShow;
    float minScore;
    int   transp;
    int   labelMode;     // 0 off, 1 score, 2 score+stats
    int   labelSize;
    int   approachTicks;
    bool  whaleAlert;
    int   alertSnd;
};

static IMBook* IMGetBook(SCStudyInterfaceRef sc)
{
    IMBook *b = (IMBook*)sc.GetPersistentPointer(1);
    if (b == NULL)
    {
        b = new IMBook;
        for (int i = 0; i < MAX_LVL; ++i) b->L[i].valid = false;
        b->baseN = 0; b->baseHead = 0;
        b->wCount = 0; b->wHead = 0;
        b->nextId = 1;
        sc.SetPersistentPointer(1, b);
    }
    return b;
}

inline float IMScore(const IMLevel &l)
{ return l.volE + l.absE + l.whaleE + l.defE; }

inline float IMBandLo(SCStudyInterfaceRef sc, int key, int bw)
{ return ((float)key * bw) * sc.TickSize - sc.TickSize * 0.5f; }

inline float IMBandHi(SCStudyInterfaceRef sc, int key, int bw)
{ return ((float)key * bw + (bw - 1)) * sc.TickSize + sc.TickSize * 0.5f; }

static void IMDeleteDrawings(SCStudyInterfaceRef sc, const IMLevel &l)
{
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, LN_RECT + l.id);
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, LN_LBL + l.id);
}

// Find existing level for key, else create (reusing free / weakest slot).
static IMLevel* IMFindOrCreate(SCStudyInterfaceRef sc, IMBook *b, int key, int bar)
{
    int freeSlot = -1, weakSlot = -1;
    float weakScore = 1e30f;
    for (int i = 0; i < MAX_LVL; ++i)
    {
        if (b->L[i].valid)
        {
            if (b->L[i].key == key && !b->L[i].broken)
                return &b->L[i];   // broken levels stay frozen; same key gets a NEW level
            const float s = IMScore(b->L[i]);
            if (s < weakScore) { weakScore = s; weakSlot = i; }
        }
        else if (freeSlot < 0)
            freeSlot = i;
    }
    int slot = (freeSlot >= 0) ? freeSlot : weakSlot;
    if (slot < 0)
        return NULL;
    if (freeSlot < 0)
        IMDeleteDrawings(sc, b->L[slot]);   // evicting the weakest

    IMLevel &l = b->L[slot];
    l.valid = true;
    l.id    = b->nextId++;
    l.key   = key;
    l.volE = l.absE = l.whaleE = l.defE = 0.f;
    l.buyE = l.sellE = 0.f;
    l.touches = 0; l.defends = 0;
    l.createdBar = bar;
    l.inBand = false;
    l.approachArmed = true;
    l.drawn = false;
    l.broken = false;
    l.breakBar = -1;
    return &l;
}

// Aggregate one bar's VAP ladder into buckets. Returns count.
static int IMAggregate(SCStudyInterfaceRef sc, int barIndex, int bw, IMBucket *B)
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
        if (key >= 0) key /= bw;
        else          key = -((-key + bw - 1) / bw);
        int j = 0;
        for (; j < n; ++j)
            if (B[j].key == key)
                break;
        if (j == n)
        {
            if (n >= MAX_BUCKETS) continue;
            B[n].key = key; B[n].av = 0.f; B[n].bv = 0.f;
            ++n;
        }
        B[j].av += (float)vap->AskVolume;
        B[j].bv += (float)vap->BidVolume;
    }
    return n;
}

// Percentile of tot within the baseline ring. -1 = not enough samples.
static float IMPct(const IMBook *b, float tot)
{
    if (b->baseN < 10 || tot <= 0.f)
        return -1.f;
    int le = 0;
    for (int q = 0; q < b->baseN; ++q)
        if (b->baseRing[q] <= tot) ++le;
    return 100.f * (float)le / (float)b->baseN;
}

// Add whale evidence for a committed aggregate (no ledger append here).
static void IMAddWhaleEvidence(SCStudyInterfaceRef sc, IMBook *b, const IMCfg &c,
                               float price, float vol, int side, int bar)
{
    if (c.whaleMin <= 0.f || vol < c.whaleMin)
        return;
    int pit = (int)(price / sc.TickSize + 0.5f);
    int key = pit;
    if (key >= 0) key /= c.bucketTicks;
    else          key = -((-key + c.bucketTicks - 1) / c.bucketTicks);

    IMLevel *l = IMFindOrCreate(sc, b, key, bar);
    if (l == NULL)
        return;
    float f = vol / c.whaleMin;
    if (f > 3.f) f = 3.f;
    l->whaleE += c.wWhale * f;
    if (side == 0) l->buyE  += c.wWhale * f;
    else           l->sellE += c.wWhale * f;
}

// =============================================================================
SCSFExport scsf_InterestMap(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Interest Map (Level Book)";
        sc.GraphRegion      = 0;
        sc.AutoLoop         = 0;
        sc.ScaleRangeType   = SCALE_SAMEASREGION;
        sc.DrawZeros        = 0;
        sc.ValueFormat      = VALUEFORMAT_INHERITED;
        sc.UpdateAlways     = 1;
        sc.MaintainVolumeAtPriceData = 1;

        sc.Subgraph[0].Name = "Top Buy Level";
        sc.Subgraph[0].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[0].DrawZeros = 0;
        sc.Subgraph[1].Name = "Top Sell Level";
        sc.Subgraph[1].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[1].DrawZeros = 0;

        sc.Input[IN_BUCKET].Name = "Bucket Width (ticks)";
        sc.Input[IN_BUCKET].SetInt(4);
        sc.Input[IN_BUCKET].SetIntLimits(1, 100);

        sc.Input[IN_BASELEN].Name = "Baseline Length (bars)";
        sc.Input[IN_BASELEN].SetInt(200);
        sc.Input[IN_BASELEN].SetIntLimits(10, BASE_CAP);

        sc.Input[IN_MINPCT].Name = "Min Volume Percentile";
        sc.Input[IN_MINPCT].SetFloat(95.0f);
        sc.Input[IN_MINPCT].SetFloatLimits(50.0f, 100.0f);

        sc.Input[IN_ABSORBMAX].Name = "Absorption Max Imbalance (%)";
        sc.Input[IN_ABSORBMAX].SetFloat(25.0f);
        sc.Input[IN_ABSORBMAX].SetFloatLimits(1.0f, 100.0f);

        sc.Input[IN_WHALEMIN].Name = "Whale Min Size (contracts, 0=off)";
        sc.Input[IN_WHALEMIN].SetFloat(200.0f);
        sc.Input[IN_WHALEMIN].SetFloatLimits(0.0f, 100000000.0f);

        sc.Input[IN_WHALEAGG].Name = "Whale Aggregation (ms)";
        sc.Input[IN_WHALEAGG].SetInt(250);
        sc.Input[IN_WHALEAGG].SetIntLimits(0, 60000);

        sc.Input[IN_W_VOL].Name = "Weight: Volume";
        sc.Input[IN_W_VOL].SetFloat(1.0f);
        sc.Input[IN_W_VOL].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_W_ABS].Name = "Weight: Absorption";
        sc.Input[IN_W_ABS].SetFloat(1.0f);
        sc.Input[IN_W_ABS].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_W_WHALE].Name = "Weight: Whale";
        sc.Input[IN_W_WHALE].SetFloat(1.5f);
        sc.Input[IN_W_WHALE].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_W_DEF].Name = "Weight: Defense";
        sc.Input[IN_W_DEF].SetFloat(2.0f);
        sc.Input[IN_W_DEF].SetFloatLimits(0.0f, 100.0f);

        sc.Input[IN_HALFLIFE].Name = "Decay Half-Life (bars)";
        sc.Input[IN_HALFLIFE].SetInt(300);
        sc.Input[IN_HALFLIFE].SetIntLimits(10, 100000);

        sc.Input[IN_MAXSHOW].Name = "Max Levels Shown";
        sc.Input[IN_MAXSHOW].SetInt(8);
        sc.Input[IN_MAXSHOW].SetIntLimits(1, 50);

        sc.Input[IN_MINSCORE].Name = "Min Score To Show";
        sc.Input[IN_MINSCORE].SetFloat(1.0f);
        sc.Input[IN_MINSCORE].SetFloatLimits(0.0f, 1000.0f);

        sc.Input[IN_LOOKBACK].Name = "Historical Lookback (bars)";
        sc.Input[IN_LOOKBACK].SetInt(2000);
        sc.Input[IN_LOOKBACK].SetIntLimits(50, 1000000);

        sc.Input[IN_TRANSP].Name = "Band Transparency (0-100)";
        sc.Input[IN_TRANSP].SetInt(75);
        sc.Input[IN_TRANSP].SetIntLimits(0, 100);

        sc.Input[IN_LABEL].Name = "Level Label";
        sc.Input[IN_LABEL].SetCustomInputStrings("Off;Score;Score+Stats");
        sc.Input[IN_LABEL].SetCustomInputIndex(2);

        sc.Input[IN_LABELSIZE].Name = "Label Font Size";
        sc.Input[IN_LABELSIZE].SetInt(8);
        sc.Input[IN_LABELSIZE].SetIntLimits(4, 72);

        sc.Input[IN_APPROACH].Name = "Approach Alert (ticks, 0=off)";
        sc.Input[IN_APPROACH].SetInt(0);
        sc.Input[IN_APPROACH].SetIntLimits(0, 1000);

        sc.Input[IN_WHALERT].Name = "Whale-At-Level Alert";
        sc.Input[IN_WHALERT].SetCustomInputStrings("Off;On");
        sc.Input[IN_WHALERT].SetCustomInputIndex(1);

        sc.Input[IN_ALERTSND].Name = "Alert Sound Number";
        sc.Input[IN_ALERTSND].SetInt(1);
        sc.Input[IN_ALERTSND].SetIntLimits(0, 100);

        sc.Input[IN_DEBUG].Name = "Debug Log";
        sc.Input[IN_DEBUG].SetCustomInputStrings("Off;On");
        sc.Input[IN_DEBUG].SetCustomInputIndex(0);

        sc.Input[IN_NAKEDDK].Name = "Naked (untested) Level Decay";
        sc.Input[IN_NAKEDDK].SetCustomInputStrings("None;Half Speed;Normal");
        sc.Input[IN_NAKEDDK].SetCustomInputIndex(0);

        sc.Input[IN_ONBREAK].Name = "On Break";
        sc.Input[IN_ONBREAK].SetCustomInputStrings("Remove;Clip To Break Bar (keep)");
        sc.Input[IN_ONBREAK].SetCustomInputIndex(1);

        return;
    }

    if (sc.LastCallToFunction)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_ALL, 0);
        IMBook *b = (IMBook*)sc.GetPersistentPointer(1);
        if (b != NULL)
        {
            delete b;
            sc.SetPersistentPointer(1, NULL);
        }
        return;
    }

    const int last = sc.ArraySize - 1;
    if (last < 1)
        return;
    if (sc.VolumeAtPriceForBars == NULL)
        return;
    if ((int)sc.VolumeAtPriceForBars->GetNumberOfBars() < sc.ArraySize)
        return;

    // ---- Config ---------------------------------------------------------------
    IMCfg c;
    c.bucketTicks = sc.Input[IN_BUCKET].GetInt();
    if (c.bucketTicks < 1) c.bucketTicks = 1;
    c.baseLen   = sc.Input[IN_BASELEN].GetInt();
    if (c.baseLen > BASE_CAP) c.baseLen = BASE_CAP;
    c.minPct    = sc.Input[IN_MINPCT].GetFloat();
    c.absorbMax = sc.Input[IN_ABSORBMAX].GetFloat();
    c.whaleMin  = sc.Input[IN_WHALEMIN].GetFloat();
    c.wVol      = sc.Input[IN_W_VOL].GetFloat();
    c.wAbs      = sc.Input[IN_W_ABS].GetFloat();
    c.wWhale    = sc.Input[IN_W_WHALE].GetFloat();
    c.wDef      = sc.Input[IN_W_DEF].GetFloat();
    const int halfLife = sc.Input[IN_HALFLIFE].GetInt();
    c.decayMul  = (float)pow(0.5, 1.0 / (double)halfLife);
    c.maxShow   = sc.Input[IN_MAXSHOW].GetInt();
    c.minScore  = sc.Input[IN_MINSCORE].GetFloat();
    c.transp    = sc.Input[IN_TRANSP].GetInt();
    c.labelMode = sc.Input[IN_LABEL].GetIndex();
    c.labelSize = sc.Input[IN_LABELSIZE].GetInt();
    c.approachTicks = sc.Input[IN_APPROACH].GetInt();
    c.whaleAlert = (sc.Input[IN_WHALERT].GetIndex() == 1);
    c.alertSnd  = sc.Input[IN_ALERTSND].GetInt();
    const int whaleAggMs = sc.Input[IN_WHALEAGG].GetInt();
    const int lookback   = sc.Input[IN_LOOKBACK].GetInt();
    const bool debugOn   = (sc.Input[IN_DEBUG].GetIndex() == 1);

    IMBook *b = IMGetBook(sc);

    // ---- Full recalculation: rebuild the book from history --------------------
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_ALL, 0);
        for (int i = 0; i < MAX_LVL; ++i) b->L[i].valid = false;
        b->baseN = 0; b->baseHead = 0;
        // Whale ledger and T&S sequence are intentionally KEPT — whales are
        // replayed into the rebuilt book below.
        int sb = last - lookback;
        if (sb < 0) sb = 0;
        sc.SetPersistentInt(PI_LASTBAR, sb);   // bar loop starts at sb+1
    }

    // =========================================================================
    // 1) Process newly CLOSED bars: decay, VAP evidence, retest engine.
    // =========================================================================
    int from = sc.GetPersistentInt(PI_LASTBAR) + 1;
    if (from < 1) from = 1;
    const int lastClosed = last - 1;
    const float tick = sc.TickSize;
    const int bw = c.bucketTicks;

    int dbgBars = 0, dbgEvid = 0, dbgDef = 0, dbgBreak = 0;

    if (from <= lastClosed)
    {
        IMBucket B[MAX_BUCKETS];

        for (int i = from; i <= lastClosed; ++i)
        {
            ++dbgBars;

            // ---- decay all levels once per closed bar -----------------------
            // Naked (never-touched) levels can decay slower or not at all —
            // unresolved interest stays relevant until price comes back.
            const int nakedMode = sc.Input[IN_NAKEDDK].GetIndex();
            for (int q = 0; q < MAX_LVL; ++q)
            {
                IMLevel &l = b->L[q];
                if (!l.valid) continue;
                float dm = c.decayMul;
                if (l.touches == 0)
                {
                    if (nakedMode == 0)      dm = 1.f;                       // none
                    else if (nakedMode == 1) dm = (1.f + c.decayMul) * 0.5f; // half
                }
                l.volE *= dm; l.absE *= dm;
                l.whaleE *= dm; l.defE *= dm;
                l.buyE *= dm; l.sellE *= dm;
                // prune dust
                if (IMScore(l) < c.minScore * 0.05f)
                {
                    IMDeleteDrawings(sc, l);
                    l.valid = false;
                }
            }

            // ---- VAP evidence ------------------------------------------------
            const int nb = IMAggregate(sc, i, bw, B);
            float barMax = 0.f;
            for (int j = 0; j < nb; ++j)
            {
                const float tot = B[j].av + B[j].bv;
                if (tot > barMax) barMax = tot;
            }

            for (int j = 0; j < nb; ++j)
            {
                const float tot = B[j].av + B[j].bv;
                const float pct = IMPct(b, tot);
                if (pct < c.minPct)
                    continue;

                IMLevel *l = IMFindOrCreate(sc, b, B[j].key, i);
                if (l == NULL)
                    continue;
                ++dbgEvid;

                const float denom = 100.f - c.minPct;
                float norm = (denom > 0.f) ? (pct - c.minPct) / denom : 1.f;
                if (norm > 1.f) norm = 1.f;
                l->volE += c.wVol * norm;

                // side: bucket below the close = buy interest, above = sell
                const float mid = ((float)B[j].key * bw + (bw - 1) * 0.5f) * tick;
                if (mid <= sc.Close[i]) l->buyE  += c.wVol * norm;
                else                    l->sellE += c.wVol * norm;

                // absorption bonus
                const float dlt = B[j].av - B[j].bv;
                const float ad  = (dlt >= 0.f) ? dlt : -dlt;
                const float imb = (tot > 0.f) ? (ad / tot * 100.f) : 100.f;
                if (imb <= c.absorbMax)
                {
                    const float af = 1.f - imb / c.absorbMax;
                    l->absE += c.wAbs * af;
                    if (mid <= sc.Close[i]) l->buyE  += c.wAbs * af;
                    else                    l->sellE += c.wAbs * af;
                }
            }

            // push baseline AFTER scoring (bar doesn't rank against itself)
            if (barMax > 0.f)
            {
                if (b->baseN < c.baseLen)
                    b->baseRing[(b->baseHead + b->baseN++) % BASE_CAP] = barMax;
                else
                {
                    b->baseRing[b->baseHead] = barMax;
                    b->baseHead = (b->baseHead + 1) % BASE_CAP;
                }
            }

            // ---- retest engine -------------------------------------------------
            const int onBreak = sc.Input[IN_ONBREAK].GetIndex();
            for (int q = 0; q < MAX_LVL; ++q)
            {
                IMLevel &l = b->L[q];
                if (!l.valid || l.broken || i <= l.createdBar)
                    continue;
                const float lo = IMBandLo(sc, l.key, bw);
                const float hi = IMBandHi(sc, l.key, bw);
                const bool touching = (sc.High[i] >= lo && sc.Low[i] <= hi);
                const bool buySide  = (l.buyE >= l.sellE);

                if (touching && !l.inBand)
                {
                    ++l.touches;
                    l.inBand = true;
                }
                if (!touching)
                    l.inBand = false;

                if (touching)
                {
                    const float cl = sc.Close[i];
                    bool defended = false, broken = false;
                    if (buySide)
                    {
                        if (cl > hi) defended = true;
                        else if (cl < lo) broken = true;
                    }
                    else
                    {
                        if (cl < lo) defended = true;
                        else if (cl > hi) broken = true;
                    }

                    if (defended)
                    {
                        ++l.defends; ++dbgDef;
                        l.defE += c.wDef;
                        if (buySide) l.buyE += c.wDef; else l.sellE += c.wDef;
                        l.inBand = false;   // resolved; re-arm for next touch
                    }
                    else if (broken)
                    {
                        ++dbgBreak;
                        if (onBreak == 1)
                        {
                            // Keep & clip: freeze the zone at the break bar so
                            // its swing validity can be back-tested visually.
                            l.broken   = true;
                            l.breakBar = i;
                            l.inBand   = false;
                        }
                        else
                        {
                            IMDeleteDrawings(sc, l);
                            l.valid = false;
                        }
                    }
                }
            }
        }
        sc.SetPersistentInt(PI_LASTBAR, lastClosed);
    }

    // ---- Whale ledger replay (after a full-recalc rebuild) --------------------
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0 && c.whaleMin > 0.f)
    {
        for (int q = 0; q < b->wCount; ++q)
        {
            const IMWhale &w = b->W[(b->wHead + q) % WHALE_CAP];
            IMAddWhaleEvidence(sc, b, c, w.price, w.vol, w.side, lastClosed);
        }
    }

    // =========================================================================
    // 2) Time & Sales -> whale aggregates -> ledger + evidence + alert.
    // =========================================================================
    if (c.whaleMin > 0.f)
    {
        c_SCTimeAndSalesArray TS;
        sc.GetTimeAndSales(TS);
        const int tsSize = TS.Size();
        unsigned int lastSeq = (unsigned int)sc.GetPersistentInt(PI_SEQ);
        const double gapDays = (double)whaleAggMs / 86400000.0;

        for (int x = 0; x < tsSize; ++x)
        {
            const s_TimeAndSales &r = TS[x];
            if (r.Sequence == 0 || r.Sequence <= lastSeq)
                continue;
            lastSeq = r.Sequence;
            if (r.Type != SC_TS_BID && r.Type != SC_TS_ASK)
                continue;
            if (r.Volume == 0)
                continue;

            const float price = r.Price * sc.RealTimePriceMultiplier;
            const float vol   = (float)r.Volume;
            const int   side  = (r.Type == SC_TS_ASK) ? 0 : 1;
            const double dt   = r.DateTime.GetAsDouble();

            const int pendSide = sc.GetPersistentInt(PI_PENDSIDE);
            bool merged = false;
            if (whaleAggMs > 0 && pendSide == side)
            {
                const double dGap = dt - sc.GetPersistentDouble(PD_LASTDT);
                float pGap = price - (float)sc.GetPersistentDouble(PD_LASTPX);
                if (pGap < 0.f) pGap = -pGap;
                if (dGap >= 0.0 && dGap <= gapDays && pGap <= 2.f * tick + tick * 0.01f)
                {
                    sc.SetPersistentDouble(PD_VOL, sc.GetPersistentDouble(PD_VOL) + vol);
                    sc.SetPersistentDouble(PD_PV,  sc.GetPersistentDouble(PD_PV) + (double)price * vol);
                    sc.SetPersistentDouble(PD_LASTDT, dt);
                    sc.SetPersistentDouble(PD_LASTPX, price);
                    merged = true;
                }
            }

            if (!merged)
            {
                // commit previous pending if it qualifies
                const double pv = sc.GetPersistentDouble(PD_VOL);
                if (sc.GetPersistentInt(PI_PENDSIDE) >= 0 && pv >= (double)c.whaleMin)
                {
                    const float vw = (float)(sc.GetPersistentDouble(PD_PV) / pv);
                    const int   ps = sc.GetPersistentInt(PI_PENDSIDE);
                    const double fd = sc.GetPersistentDouble(PD_FIRSTDT);
                    // ledger
                    int idx;
                    if (b->wCount < WHALE_CAP)
                        { idx = (b->wHead + b->wCount++) % WHALE_CAP; }
                    else
                        { idx = b->wHead; b->wHead = (b->wHead + 1) % WHALE_CAP; }
                    b->W[idx].dt = fd; b->W[idx].price = vw;
                    b->W[idx].vol = (float)pv; b->W[idx].side = ps;
                    // evidence
                    IMAddWhaleEvidence(sc, b, c, vw, (float)pv, ps, last);
                    // whale-at-level alert
                    if (c.whaleAlert && !sc.IsFullRecalculation)
                    {
                        for (int q = 0; q < MAX_LVL; ++q)
                        {
                            const IMLevel &l = b->L[q];
                            if (!l.valid || l.broken || !l.drawn) continue;
                            if (vw >= IMBandLo(sc, l.key, bw) && vw <= IMBandHi(sc, l.key, bw))
                            {
                                SCString msg;
                                msg.Format("InterestMap: WHALE %s %d at level %.2f (S%.0f T%d D%d)",
                                           (ps == 0) ? "BUY" : "SELL", (int)(pv + 0.5),
                                           vw, IMScore(l), l.touches, l.defends);
                                sc.SetAlert(c.alertSnd, msg);
                                break;
                            }
                        }
                    }
                }
                sc.SetPersistentInt(PI_PENDSIDE, side);
                sc.SetPersistentDouble(PD_VOL, vol);
                sc.SetPersistentDouble(PD_PV, (double)price * vol);
                sc.SetPersistentDouble(PD_FIRSTDT, dt);
                sc.SetPersistentDouble(PD_LASTDT, dt);
                sc.SetPersistentDouble(PD_LASTPX, price);
            }
        }
        sc.SetPersistentInt(PI_SEQ, (int)lastSeq);
    }

    // =========================================================================
    // 3) Approach alerts on the forming bar.
    // =========================================================================
    if (c.approachTicks > 0 && !sc.IsFullRecalculation)
    {
        const float cl = sc.Close[last];
        for (int q = 0; q < MAX_LVL; ++q)
        {
            IMLevel &l = b->L[q];
            if (!l.valid || l.broken || !l.drawn) continue;
            const float lo = IMBandLo(sc, l.key, bw);
            const float hi = IMBandHi(sc, l.key, bw);
            float dist;
            if (cl < lo)      dist = lo - cl;
            else if (cl > hi) dist = cl - hi;
            else              dist = 0.f;

            const float arm = (float)c.approachTicks * tick;
            if (dist <= arm && l.approachArmed)
            {
                l.approachArmed = false;
                SCString msg;
                msg.Format("InterestMap: approaching level %.2f-%.2f (S%.0f T%d D%d, %s)",
                           lo, hi, IMScore(l), l.touches, l.defends,
                           (l.buyE >= l.sellE) ? "BUY interest" : "SELL interest");
                sc.SetAlert(c.alertSnd, msg);
            }
            else if (dist > arm * 2.f)
                l.approachArmed = true;   // re-arm after price leaves
        }
    }

    // =========================================================================
    // 4) Draw the top-N levels.
    // =========================================================================
    int order[MAX_LVL];
    int nv = 0;
    for (int q = 0; q < MAX_LVL; ++q)
        if (b->L[q].valid && !b->L[q].broken && IMScore(b->L[q]) >= c.minScore)
            order[nv++] = q;
    // insertion sort by score desc
    for (int a = 1; a < nv; ++a)
    {
        const int v = order[a];
        const float s = IMScore(b->L[v]);
        int p = a;
        while (p > 0 && IMScore(b->L[order[p - 1]]) < s) { order[p] = order[p - 1]; --p; }
        order[p] = v;
    }

    int shown = (nv < c.maxShow) ? nv : c.maxShow;
    float topBuy = 0.f, topSell = 0.f;

    for (int a = 0; a < nv; ++a)
    {
        IMLevel &l = b->L[order[a]];
        if (a < shown)
        {
            const float lo = IMBandLo(sc, l.key, bw);
            const float hi = IMBandHi(sc, l.key, bw);
            const bool buySide = (l.buyE >= l.sellE);
            const COLORREF col = buySide ? COL_BUY : COL_SELL;

            int fromBar = l.createdBar;
            if (fromBar < 0) fromBar = 0;
            if (fromBar > last) fromBar = last;

            s_UseTool T;
            T.Clear();
            T.ChartNumber       = sc.ChartNumber;
            T.DrawingType       = DRAWING_RECTANGLEHIGHLIGHT;
            T.LineNumber        = LN_RECT + l.id;
            T.BeginDateTime     = sc.BaseDateTimeIn[fromBar];
            T.EndDateTime       = sc.BaseDateTimeIn[last];
            T.BeginValue        = lo;
            T.EndValue          = hi;
            T.Color             = col;     // border
            T.SecondaryColor    = col;     // fill
            T.TransparencyLevel = c.transp;
            T.AddMethod         = UTAM_ADD_OR_ADJUST;
            sc.UseTool(T);

            if (c.labelMode != 0)
            {
                SCString t;
                if (c.labelMode == 1)
                    t.Format("S%.0f", IMScore(l));
                else
                    t.Format("S%.0f T%d D%d", IMScore(l), l.touches, l.defends);
                s_UseTool TL;
                TL.Clear();
                TL.ChartNumber   = sc.ChartNumber;
                TL.DrawingType   = DRAWING_TEXT;
                TL.LineNumber    = LN_LBL + l.id;
                TL.BeginIndex    = last;
                TL.BeginValue    = (lo + hi) * 0.5f;
                TL.Color         = RGB(255, 255, 255);
                TL.FontSize      = c.labelSize;
                TL.Text          = t;
                TL.TextAlignment = DT_LEFT | DT_VCENTER;
                TL.AddMethod     = UTAM_ADD_OR_ADJUST;
                sc.UseTool(TL);
            }
            else if (l.drawn)
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, LN_LBL + l.id);

            l.drawn = true;

            if (buySide && topBuy == 0.f)  topBuy  = (lo + hi) * 0.5f;
            if (!buySide && topSell == 0.f) topSell = (lo + hi) * 0.5f;
        }
        else if (l.drawn)
        {
            IMDeleteDrawings(sc, l);
            l.drawn = false;
        }
    }
    // levels below minScore that were drawn earlier (broken handled below)
    for (int q = 0; q < MAX_LVL; ++q)
        if (b->L[q].valid && !b->L[q].broken && b->L[q].drawn
            && IMScore(b->L[q]) < c.minScore)
        {
            IMDeleteDrawings(sc, b->L[q]);
            b->L[q].drawn = false;
        }

    // ---- Broken levels: frozen gray rectangles clipped at the break bar ------
    for (int q = 0; q < MAX_LVL; ++q)
    {
        IMLevel &l = b->L[q];
        if (!l.valid || !l.broken)
            continue;
        // fade out: once decay drops a broken level low enough, remove it
        if (IMScore(l) < c.minScore * 0.25f)
        {
            if (l.drawn) { IMDeleteDrawings(sc, l); l.drawn = false; }
            continue;
        }

        int fromBar = l.createdBar;
        if (fromBar < 0) fromBar = 0;
        int toBar = (l.breakBar >= 0 && l.breakBar <= last) ? l.breakBar : last;
        if (toBar < fromBar) toBar = fromBar;

        s_UseTool T;
        T.Clear();
        T.ChartNumber       = sc.ChartNumber;
        T.DrawingType       = DRAWING_RECTANGLEHIGHLIGHT;
        T.LineNumber        = LN_RECT + l.id;
        T.BeginDateTime     = sc.BaseDateTimeIn[fromBar];
        T.EndDateTime       = sc.BaseDateTimeIn[toBar];
        T.BeginValue        = IMBandLo(sc, l.key, bw);
        T.EndValue          = IMBandHi(sc, l.key, bw);
        T.Color             = COL_BROKEN;   // border
        T.SecondaryColor    = COL_BROKEN;   // fill
        T.TransparencyLevel = c.transp;
        T.AddMethod         = UTAM_ADD_OR_ADJUST;
        sc.UseTool(T);

        if (c.labelMode == 2)
        {
            SCString t;
            t.Format("S%.0f T%d D%d X", IMScore(l), l.touches, l.defends);
            s_UseTool TL;
            TL.Clear();
            TL.ChartNumber   = sc.ChartNumber;
            TL.DrawingType   = DRAWING_TEXT;
            TL.LineNumber    = LN_LBL + l.id;
            TL.BeginIndex    = toBar;
            TL.BeginValue    = (IMBandLo(sc, l.key, bw) + IMBandHi(sc, l.key, bw)) * 0.5f;
            TL.Color         = COL_BROKEN;
            TL.FontSize      = c.labelSize;
            TL.Text          = t;
            TL.TextAlignment = DT_LEFT | DT_VCENTER;
            TL.AddMethod     = UTAM_ADD_OR_ADJUST;
            sc.UseTool(TL);
        }
        l.drawn = true;
    }

    sc.Subgraph[0][last] = topBuy;
    sc.Subgraph[1][last] = topSell;

    if (debugOn && dbgBars > 0)
    {
        SCString m;
        m.Format("InterestMap dbg: bars=%d evid=%d defends=%d breaks=%d "
                 "levels=%d shown=%d whales=%d",
                 dbgBars, dbgEvid, dbgDef, dbgBreak, nv, shown, b->wCount);
        sc.AddMessageToLog(m, 0);
    }
}
