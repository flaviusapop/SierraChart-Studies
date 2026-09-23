// RankedBigTrades.cpp
// =============================================================================
// Ranked Big Trades  (Sierra Chart ACSIL)
//
// Top-N overlay of the largest aggregated Time and Sales prints. Four
// independent rank pools: Day Ask, Day Bid, Week Ask, Week Bid. N is per
// side. Not a min-size dump of every big trade — that is BigTradesTape.
//
// CLOCK — s_TimeAndSales::DateTime is UTC. Every T&S record is converted to
// chart time zone EXACTLY ONCE at ingest (sc.ConvertDateTimeUTCToChartTimeZone).
// That converted value is the only clock downstream: aggregation gap, ledger,
// CSV, session filter, bar mapping. sc.BaseDateTimeIn, sc.Input[].GetTime()
// and sc.CurrentSystemDateTime are chart tz.
//
// SESSIONS — Overnight (ETH) / RTH / Full day (ETH+RTH). Overnight is
// [ETH Start, RTH Start) wrapping midnight (RTH open exclusive). RTH is
// [RTH Start, RTH End] inclusive. Full day is [ETH Start, Full Day End]
// wrapping, inclusive. Default RTH 09:30–16:00, ETH start 18:00, full-day
// end 17:00. Halt 17:00:01–17:59:59 matches no filter.
//
// RANK — on bar close of this chart, not tick-by-tick. Forming-bar prints
// sit in the ledger but do not enter the drawn list until that bar closes.
// Current leaders on the canvas at their original print time/price — not a
// rank-through-time heatmap. Drop-off removes the bubble; no ghost.
// A print in both its day pool and its week pool draws once, day colors win.
//
// AGG — user Aggregation Period (ms), default 1000, 0 = raw prints. Same-side
// + time window + max tick gap merge copied from BigTradesTape. Ranking
// punishes fragmented sweeps, which is why the default is not 250.
//
// DISPLAY — DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE, GraphRegion 0,
// 40 shared slots (N max 10 per side so 4×10 fits). Four color inputs.
// Labels: Off / Size / Rank / Rank+Size. View-relative px among leaders
// currently on screen.
//
// HISTORY — closed bars are filled from the chart's 1-tick .scid file via
// ReadIntradayFileRecordForBarIndexAndSubIndex (same source as Sierra's
// Large Volume Trade Indicator). Requires Intraday Data Storage Time Unit
// = 1 Tick. T&S is only the live/RAM path for the forming bar.
//
// PERSISTENCE — in-memory ring + RankedBigTrades_<symbol>.csv. Per-day cap
// 1024, session-unfiltered. Session filter is applied at rank time. CSV
// de-dup via PD_LOADHWM.
//
//   SG 0..39   bubble slots (unique display list)
//   SG 40      Cache (IGNORE)
//
// LineNumbers 92000000..92000039 labels, 92000100 status. Never TOOL_DELETE_ALL.
//
// -----------------------------------------------------------------------------
// BUILD NOTES
//   - No std::min/std::max/std::fabs, no <algorithm>.
//   - DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE: Data[] = price,
//     Arrays[0][] = pixel size, Arrays[1][] = contracts, AUTOCOLOR_NONE +
//     DataColor[].
//   - T&S prices * sc.RealTimePriceMultiplier.
//   - T&S DateTime is UTC; convert once at ingest.
//   - Do NOT read the intraday tick file (renko freeze). T&S only.
//   - Bar map: manual binary search. GetContainingIndexForSCDateTime is
//     wrong after recalc (resolves to the current bar).
//   - DRAWING_TEXT: BeginIndex, not BeginDateTime-only.
//   - SCDateTime has no SetAsDouble; construct from double.
//   - Do not pin writes to ArraySize-1.
//   - PI_SEQ is NOT reset on full recalc.
//   - GetTradingDayDate takes SCDateTime, not a bar index.
//   - Persistent ints are a container lookup, not an array.
//   - c_ArrayWrapper<float> has no Zero(); loop writes.
//   - UpdateAlways=1 + unbounded UseTool freezes the UI. Dirty flag required.
//   - int64_t + <cstdint>. Never int64 / DWORD / __int64.
// =============================================================================

#include "sierrachart.h"
#include <stdio.h>
#include <string.h>
#include <cstdint>

SCDLLName("RankedBigTrades")

const int RBT_RING_CAP    = 65536;
const int RBT_PER_DAY_CAP = 1024;
const int RBT_MAX_N       = 10;
const int RBT_BUBBLE_SG   = 40;
const int RBT_SG_CACHE    = 40;
const int RBT_HIST_MAX    = 20;
const int RBT_DISP_CAP    = 400;
const int RBT_WRITE_CAP   = 512;

const int RBT_LINE_BASE   = 92000000;
const int RBT_LINE_STATUS = 92001000;

const double RBT_HWM_EPS  = 0.5 / 86400.0;

// ---- Inputs (contiguous, 0..30) --------------------------------------------
const int IN_AGGMS     = 0;
const int IN_AGGGAP    = 1;
const int IN_MINSIZE   = 2;
const int IN_DAYN      = 3;
const int IN_WEEKN     = 4;
const int IN_SHOWDAY   = 5;
const int IN_SHOWWEEK  = 6;
const int IN_SHOWASK   = 7;
const int IN_SHOWBID   = 8;
const int IN_SESSFILT  = 9;
const int IN_RTHSTART  = 10;
const int IN_RTHEND    = 11;
const int IN_ETHSTART  = 12;
const int IN_FULLEND   = 13;
const int IN_WEEKMODE  = 14;
const int IN_ROLLN     = 15;
const int IN_COLDAYASK = 16;
const int IN_COLDAYBID = 17;
const int IN_COLWKASK  = 18;
const int IN_COLWKBID  = 19;
const int IN_SIZEMIN   = 20;
const int IN_SIZEMAX   = 21;
const int IN_SCALING   = 22;
const int IN_LABEL     = 23;
const int IN_LABELSIZE = 24;
const int IN_LABELCOL  = 25;
const int IN_ALERT     = 26;
const int IN_ALERTSND  = 27;
const int IN_KEEPDAYS  = 28;
const int IN_STATUS    = 29;
const int IN_DEBUG     = 30;
const int IN_HISTDAYS  = 31;

// ---- Persistent ints / doubles ---------------------------------------------
// PI_SEQ stores an unsigned T&S sequence in an int slot; bit round-trip is
// intentional (same as BigTradesTape.cpp).
const int PI_SEQ       = 0;
const int PI_PENDSIDE  = 1;
const int PI_RCOUNT    = 2;
const int PI_RHEAD     = 3;
const int PI_LOADED    = 4;
const int PI_LASTCLOSED= 5;
const int PI_LASTDAY   = 6;
const int PI_LABELDIRTY= 7;
const int PI_HIDDEN    = 8;
const int PI_LASTLBLMODE = 9;
const int PI_LASTLBLSIZE = 10;
const int PI_LASTLBLCOL  = 11;
const int PI_STATUSON    = 12;
const int PI_DISPCNT     = 13;
const int PI_OVERFLOW    = 14;
const int PI_CSVFAIL     = 15;

const int PD_VOL     = 0;
const int PD_PV      = 1;
const int PD_LASTDT  = 2;
const int PD_LASTPX  = 3;
const int PD_FIRSTDT = 4;
const int PD_LOADHWM = 5;

struct RBTTrade
{
    double dt;
    float  price;
    float  vol;
    int    side;
    int    dayDate;
};

struct RBTLeader
{
    double dt;
    float  price;
    float  vol;
    int    side;
    int    rank;
};

struct RBTDisp
{
    double   dt;
    float    price;
    float    vol;
    int      side;
    int      rank;
    int      bar;
    int      sg;
    COLORREF col;
};

struct RBTWrite
{
    int sg;
    int bar;
};

struct RBTState
{
    RBTTrade  ring[RBT_RING_CAP];
    RBTLeader dayAsk[RBT_MAX_N];
    RBTLeader dayBid[RBT_MAX_N];
    RBTLeader weekAsk[RBT_MAX_N];
    RBTLeader weekBid[RBT_MAX_N];
    int       nDayAsk;
    int       nDayBid;
    int       nWeekAsk;
    int       nWeekBid;
    int       histDate[RBT_HIST_MAX];
    int       nHist;
    RBTLeader histAsk[RBT_HIST_MAX][RBT_MAX_N];
    RBTLeader histBid[RBT_HIST_MAX][RBT_MAX_N];
    int       nHistAsk[RBT_HIST_MAX];
    int       nHistBid[RBT_HIST_MAX];
    RBTDisp   disp[RBT_DISP_CAP];
    int       nDisp;
    RBTWrite  prevW[RBT_WRITE_CAP];
    int       nPrevW;
    int       prevVFirst;
    int       prevVLast;
    int       labelOn[RBT_DISP_CAP];
    int       dbgOnce;
    int       tickBarDone;  // last closed bar ingested from the .scid file; -1 none
    int       tickReads;
};

struct RBTCfg
{
    int      aggMs;
    int      aggGap;
    float    minSize;
    int      dayN;
    int      weekN;
    int      showDay;
    int      showWeek;
    int      showAsk;
    int      showBid;
    int      sessFilt;
    int      rthStart;
    int      rthEnd;
    int      ethStart;
    int      fullEnd;
    int      weekMode;
    int      rollN;
    COLORREF colDayAsk;
    COLORREF colDayBid;
    COLORREF colWkAsk;
    COLORREF colWkBid;
    int      sizeMin;
    int      sizeMax;
    int      scaling;
    int      labelMode;
    int      labelSize;
    COLORREF labelCol;
    int      alertOn;
    int      alertSnd;
    int      keepDays;
    int      histDays;
    int      showStatus;
    int      debugOn;
};

static int  RBTDayDate(SCStudyInterfaceRef sc, double dt);
static void RBTRingInsert(SCStudyInterfaceRef sc, RBTState *st, const RBTTrade &t);

static RBTState* RBTGet(SCStudyInterfaceRef sc)
{
    RBTState *p = (RBTState*)sc.GetPersistentPointer(1);
    if (p == NULL)
    {
        p = new RBTState;
        memset(p, 0, sizeof(RBTState));
        p->prevVFirst   = -1;
        p->prevVLast    = -1;
        p->tickBarDone  = -1;
        p->tickReads    = 0;
        sc.SetPersistentPointer(1, p);
        sc.SetPersistentInt(PI_RCOUNT, 0);
        sc.SetPersistentInt(PI_RHEAD, 0);
        sc.SetPersistentInt(PI_PENDSIDE, -1);
        sc.SetPersistentInt(PI_LASTCLOSED, -2);
        sc.SetPersistentInt(PI_LASTDAY, 0);
        // Fresh ring: reload CSV and reprocess T&S. Persistent ints survive a
        // DLL rebuild after LastCallToFunction deleted the pointer; leaving
        // PI_LOADED/PI_SEQ set would leave an empty ring forever.
        sc.SetPersistentInt(PI_LOADED, 0);
        sc.SetPersistentInt(PI_SEQ, 0);
        sc.SetPersistentDouble(PD_LOADHWM, 0.0);
    }
    return p;
}

static void RBTSanitizeSymbol(SCStudyInterfaceRef sc, char *sym, int cap)
{
    const char *s = sc.Symbol.GetChars();
    int n = 0;
    for (int i = 0; s != NULL && s[i] != 0 && n < cap - 1; ++i)
    {
        const char ch = s[i];
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')
                     || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '.';
        sym[n++] = ok ? ch : '_';
    }
    sym[n] = 0;
}

static void RBTFilePath(SCStudyInterfaceRef sc, SCString &path)
{
    char sym[128];
    RBTSanitizeSymbol(sc, sym, 128);
    path.Format("%s\\RankedBigTrades_%s.csv", sc.DataFilesFolder().GetChars(), sym);
}

static void RBTFileRewrite(SCStudyInterfaceRef sc, RBTState *st)
{
    SCString path;
    RBTFilePath(sc, path);
    FILE *w = fopen(path.GetChars(), "w");
    if (w == NULL)
        return;
    const int count = sc.GetPersistentInt(PI_RCOUNT);
    const int head  = sc.GetPersistentInt(PI_RHEAD);
    for (int q = 0; q < count; ++q)
    {
        const RBTTrade &t = st->ring[(head + q) % RBT_RING_CAP];
        fprintf(w, "%.10f,%.4f,%.0f,%d,%d\n", t.dt, t.price, t.vol, t.side, t.dayDate);
    }
    fclose(w);
}

// Same-symbol BigTradesTape CSV. That study keeps history across SC restarts;
// T&S does not. Used only when our own file is missing so a closed session
// still has something to rank.
static int RBTImportBigTradesTape(SCStudyInterfaceRef sc, RBTState *st, double &maxDtOut)
{
    char sym[128];
    RBTSanitizeSymbol(sc, sym, 128);
    SCString path;
    path.Format("%s\\BigTradesTape_%s.csv", sc.DataFilesFolder().GetChars(), sym);
    FILE *f = fopen(path.GetChars(), "r");
    if (f == NULL)
        return 0;
    int n = 0;
    double dt; float px, vol; int side;
    while (fscanf(f, "%lf,%f,%f,%d", &dt, &px, &vol, &side) == 4)
    {
        if ((side == 0 || side == 1) && vol > 0.f && px > 0.f)
        {
            RBTTrade t;
            t.dt = dt; t.price = px; t.vol = vol; t.side = side;
            t.dayDate = RBTDayDate(sc, dt);
            RBTRingInsert(sc, st, t);
            if (dt > maxDtOut)
                maxDtOut = dt;
            ++n;
        }
    }
    fclose(f);
    return n;
}

static void RBTFileAppend(SCStudyInterfaceRef sc, const RBTTrade &t)
{
    SCString path;
    RBTFilePath(sc, path);
    FILE *f = fopen(path.GetChars(), "a");
    if (f == NULL)
    {
        if (sc.GetPersistentInt(PI_CSVFAIL) == 0)
        {
            sc.SetPersistentInt(PI_CSVFAIL, 1);
            sc.AddMessageToLog("RankedBigTrades: CSV fopen failed (append)", 0);
        }
        return;
    }
    fprintf(f, "%.10f,%.4f,%.0f,%d,%d\n", t.dt, t.price, t.vol, t.side, t.dayDate);
    fclose(f);
}

static void RBTRingInsert(SCStudyInterfaceRef sc, RBTState *st, const RBTTrade &t)
{
    int count = sc.GetPersistentInt(PI_RCOUNT);
    int head  = sc.GetPersistentInt(PI_RHEAD);

    int dayN = 0;
    int minIdx = -1;
    float minVol = 0.f;
    double minDt = 0.0;
    for (int q = 0; q < count; ++q)
    {
        const int idx = (head + q) % RBT_RING_CAP;
        if (st->ring[idx].dayDate != t.dayDate)
            continue;
        ++dayN;
        if (minIdx < 0
            || st->ring[idx].vol < minVol
            || (st->ring[idx].vol == minVol && st->ring[idx].dt > minDt))
        {
            minIdx = idx;
            minVol = st->ring[idx].vol;
            minDt  = st->ring[idx].dt;
        }
    }

    if (dayN >= RBT_PER_DAY_CAP)
    {
        if (t.vol < minVol || (t.vol == minVol && t.dt >= minDt))
            return;
        st->ring[minIdx] = t;
        return;
    }

    int idx;
    if (count < RBT_RING_CAP)
    {
        idx = (head + count) % RBT_RING_CAP;
        sc.SetPersistentInt(PI_RCOUNT, count + 1);
    }
    else
    {
        idx = head;
        sc.SetPersistentInt(PI_RHEAD, (head + 1) % RBT_RING_CAP);
        if (sc.GetPersistentInt(PI_OVERFLOW) == 0)
        {
            sc.SetPersistentInt(PI_OVERFLOW, 1);
            sc.AddMessageToLog("RankedBigTrades: ring full, evicting oldest", 0);
        }
    }
    st->ring[idx] = t;
}

static void RBTFileLoad(SCStudyInterfaceRef sc, RBTState *st, double cutoffDt, double &maxDtOut)
{
    maxDtOut = 0.0;
    SCString path;
    RBTFilePath(sc, path);
    FILE *f = fopen(path.GetChars(), "r");
    if (f == NULL)
        return;
    double dt; float px, vol; int side, dayDate;
    while (fscanf(f, "%lf,%f,%f,%d,%d", &dt, &px, &vol, &side, &dayDate) == 5)
    {
        if (dt >= cutoffDt && (side == 0 || side == 1) && vol > 0.f && px > 0.f)
        {
            RBTTrade t;
            t.dt = dt; t.price = px; t.vol = vol; t.side = side; t.dayDate = dayDate;
            RBTRingInsert(sc, st, t);
            if (dt > maxDtOut)
                maxDtOut = dt;
        }
    }
    fclose(f);
    RBTFileRewrite(sc, st);
}

static bool RBTBarContainsPrice(SCStudyInterfaceRef sc, int bar, float price)
{
    if (bar < 0 || bar >= sc.ArraySize)
        return false;
    float lo = sc.Low[bar];
    float hi = sc.High[bar];
    if (lo > hi)
    {
        const float t = lo;
        lo = hi;
        hi = t;
    }
    float slack = (float)sc.TickSize;
    if (slack <= 0.f)
        slack = 0.25f;
    return (price + slack >= lo && price - slack <= hi);
}

// Daily (and longer) bars: one candle per trading date. Clock search would
// park Sunday 18:00–midnight ETH prints on the previous calendar bar.
static int RBTIsDayOrLonger(SCStudyInterfaceRef sc)
{
    if (sc.ArraySize < 2)
        return 0;
    const int last = sc.ArraySize - 1;
    double span = sc.BaseDateTimeIn[last].GetAsDouble()
                - sc.BaseDateTimeIn[last - 1].GetAsDouble();
    if (span < 0.0)
        span = -span;
    return (span >= 0.6) ? 1 : 0;
}

static int RBTBarIndexForTradingDay(SCStudyInterfaceRef sc, int dayDate)
{
    for (int i = sc.ArraySize - 1; i >= 0; --i)
    {
        if (sc.GetTradingDayDate(sc.BaseDateTimeIn[i]) == dayDate)
            return i;
    }
    return -1;
}

static int RBTBarIndexForTime(SCStudyInterfaceRef sc, double dt, float price)
{
    const int last = sc.ArraySize - 1;
    if (last < 0)
        return -1;

    if (RBTIsDayOrLonger(sc))
    {
        const int byDay = RBTBarIndexForTradingDay(sc, RBTDayDate(sc, dt));
        if (byDay >= 0)
            return byDay;
    }

    if (dt < sc.BaseDateTimeIn[0].GetAsDouble())
        return -1;
    if (dt >= sc.BaseDateTimeIn[last].GetAsDouble())
        return last;
    int lo = 0, hi = last;
    while (lo < hi)
    {
        const int mid = lo + (hi - lo + 1) / 2;
        if (sc.BaseDateTimeIn[mid].GetAsDouble() <= dt)
            lo = mid;
        else
            hi = mid - 1;
    }

    if (price > 0.f && RBTBarContainsPrice(sc, lo, price))
        return lo;

    // Renko/range: several bricks can share a timestamp or a sweep's VWAP
    // sits on a later brick than dtFirst. Prefer a nearby bar that contains
    // the print price, without walking past the print clock.
    if (price > 0.f)
    {
        const double latest = dt + (2.0 / 86400.0);
        int j = lo;
        int steps = 0;
        while (j <= last && steps < 24)
        {
            if (RBTBarContainsPrice(sc, j, price))
                return j;
            if (j + 1 > last)
                break;
            const double nxt = sc.BaseDateTimeIn[j + 1].GetAsDouble();
            const double cur = sc.BaseDateTimeIn[j].GetAsDouble();
            if (nxt > latest && nxt > cur + 1e-12)
                break;
            ++j;
            ++steps;
        }
        for (int d = 1; d <= 8; ++d)
        {
            if (lo - d < 0)
                break;
            if (RBTBarContainsPrice(sc, lo - d, price))
                return lo - d;
        }
    }
    return lo;
}

static bool RBTInWindow(int tod, int s, int e, bool endExclusive)
{
    if (endExclusive)
    {
        if (s == e)
            return false;
        if (s < e)
            return (tod >= s && tod < e);
        return (tod >= s || tod < e);
    }
    if (s <= e)
        return (tod >= s && tod <= e);
    return (tod >= s || tod <= e);
}

static bool RBTInSession(double dt, const RBTCfg &c)
{
    const SCDateTime d(dt);
    const int tod = d.GetTimeInSeconds();
    if (c.sessFilt == 0)
        return RBTInWindow(tod, c.ethStart, c.rthStart, true);
    if (c.sessFilt == 1)
        return RBTInWindow(tod, c.rthStart, c.rthEnd, false);
    return RBTInWindow(tod, c.ethStart, c.fullEnd, false);
}

static int RBTWeekMondaySerial(int tradingDayDate)
{
    if (tradingDayDate < 1000 || tradingDayDate > 300000)
        return -1;
    const int dow = (tradingDayDate + 6) % 7;
    return tradingDayDate - ((dow + 6) % 7);
}

static int RBTDayDate(SCStudyInterfaceRef sc, double dt)
{
    return sc.GetTradingDayDate(SCDateTime(dt));
}

static void RBTDeleteOurDrawings(SCStudyInterfaceRef sc)
{
    for (int i = 0; i < RBT_DISP_CAP; ++i)
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING,
                                 RBT_LINE_BASE + i);
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING,
                             RBT_LINE_STATUS);
}

static void RBTLabel(SCStudyInterfaceRef sc, int lineNumber, bool show,
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

static void RBTStatus(SCStudyInterfaceRef sc, bool show, const SCString &text, COLORREF color)
{
    if (!show)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, RBT_LINE_STATUS);
        sc.SetPersistentInt(PI_STATUSON, 0);
        return;
    }
    s_UseTool Tool;
    Tool.Clear();
    Tool.ChartNumber = sc.ChartNumber;
    Tool.DrawingType = DRAWING_TEXT;
    Tool.LineNumber  = RBT_LINE_STATUS;
    Tool.AddMethod   = UTAM_ADD_OR_ADJUST;
    Tool.UseRelativeVerticalValues = 1;
    Tool.BeginDateTime = 2;
    Tool.BeginValue = 95;
    Tool.Color = color;
    Tool.FontSize = 10;
    Tool.Text = text;
    sc.UseTool(Tool);
    sc.SetPersistentInt(PI_STATUSON, 1);
}

static int RBTCmpLeader(const RBTLeader &a, const RBTLeader &b)
{
    if (a.vol > b.vol) return -1;
    if (a.vol < b.vol) return 1;
    if (a.dt < b.dt) return -1;
    if (a.dt > b.dt) return 1;
    return 0;
}

static void RBTOffer(RBTLeader *a, int *n, int cap, const RBTLeader &cand)
{
    if (*n < cap)
    {
        int i = *n;
        a[i] = cand;
        *n = *n + 1;
        while (i > 0 && RBTCmpLeader(a[i], a[i - 1]) < 0)
        {
            const RBTLeader tmp = a[i];
            a[i] = a[i - 1];
            a[i - 1] = tmp;
            --i;
        }
        return;
    }
    if (RBTCmpLeader(cand, a[cap - 1]) >= 0)
        return;
    a[cap - 1] = cand;
    int i = cap - 1;
    while (i > 0 && RBTCmpLeader(a[i], a[i - 1]) < 0)
    {
        const RBTLeader tmp = a[i];
        a[i] = a[i - 1];
        a[i - 1] = tmp;
        --i;
    }
}

static void RBTAssignRanks(RBTLeader *a, int n)
{
    for (int i = 0; i < n; ++i)
        a[i].rank = i + 1;
}

static bool RBTSamePrint(double dtA, int sideA, double dtB, int sideB)
{
    if (sideA != sideB)
        return false;
    double d = dtA - dtB;
    if (d < 0.0) d = -d;
    return d <= RBT_HWM_EPS;
}

static bool RBTInLeaderList(const RBTLeader *a, int n, double dt, int side)
{
    for (int i = 0; i < n; ++i)
    {
        if (RBTSamePrint(a[i].dt, a[i].side, dt, side))
            return true;
    }
    return false;
}

static void RBTCollectDates(RBTState *st, int count, int head, int *dates, int *nd)
{
    *nd = 0;
    for (int q = 0; q < count; ++q)
    {
        const int d = st->ring[(head + q) % RBT_RING_CAP].dayDate;
        int found = 0;
        for (int i = 0; i < *nd; ++i)
        {
            if (dates[i] == d) { found = 1; break; }
        }
        if (!found && *nd < 64)
            dates[(*nd)++] = d;
    }
    for (int i = 0; i < *nd; ++i)
    {
        for (int j = i + 1; j < *nd; ++j)
        {
            if (dates[j] > dates[i])
            {
                const int tmp = dates[i];
                dates[i] = dates[j];
                dates[j] = tmp;
            }
        }
    }
}

static bool RBTInWeekWindow(int dayDate, int today, const RBTCfg &c,
                            const int *dates, int nd)
{
    if (c.weekMode == 0)
    {
        const int monday = RBTWeekMondaySerial(today);
        if (monday < 0)
            return false;
        return (dayDate >= monday && dayDate <= today);
    }
    int take = c.rollN;
    if (take > nd) take = nd;
    for (int i = 0; i < take; ++i)
    {
        if (dates[i] == dayDate)
            return true;
    }
    return false;
}

static void RBTZeroSlot(SCStudyInterfaceRef sc, int sg, int bar)
{
    if (bar < 0 || bar >= sc.ArraySize)
        return;
    sc.Subgraph[sg][bar]           = 0.f;
    sc.Subgraph[sg].Arrays[0][bar] = 0.f;
    sc.Subgraph[sg].Arrays[1][bar] = 0.f;
}

static void RBTFlush(SCStudyInterfaceRef sc, RBTState *st, const RBTCfg &c)
{
    if (sc.GetPersistentInt(PI_PENDSIDE) < 0)
        return;
    const double dVol = sc.GetPersistentDouble(PD_VOL);
    const double dPV  = sc.GetPersistentDouble(PD_PV);
    const double dtF  = sc.GetPersistentDouble(PD_FIRSTDT);
    const int    side = sc.GetPersistentInt(PI_PENDSIDE);
    sc.SetPersistentInt(PI_PENDSIDE, -1);
    sc.SetPersistentDouble(PD_VOL, 0.0);
    sc.SetPersistentDouble(PD_PV, 0.0);

    if (dVol <= 0.0)
        return;
    if (c.minSize > 0.f && dVol < (double)c.minSize)
        return;

    RBTTrade t;
    t.dt      = dtF;
    t.price   = (float)(dPV / dVol);
    t.vol     = (float)dVol;
    t.side    = side;
    t.dayDate = RBTDayDate(sc, dtF);
    if (t.price <= 0.f)
        return;

    if (dtF > sc.GetPersistentDouble(PD_LOADHWM) + RBT_HWM_EPS)
    {
        RBTRingInsert(sc, st, t);
        if (c.keepDays > 0)
            RBTFileAppend(sc, t);
        if (dtF > sc.GetPersistentDouble(PD_LOADHWM))
            sc.SetPersistentDouble(PD_LOADHWM, dtF);
    }
}

static void RBTIngestPrint(SCStudyInterfaceRef sc, RBTState *st, const RBTCfg &c,
                           double dt, float price, float vol, int side,
                           double gapDays, float tick)
{
    if (vol <= 0.f || price <= 0.f || (side != 0 && side != 1))
        return;

    const int pendSide = sc.GetPersistentInt(PI_PENDSIDE);
    bool merged = false;
    if (c.aggMs > 0 && pendSide == side)
    {
        const double dGap = dt - sc.GetPersistentDouble(PD_LASTDT);
        float pGap = price - (float)sc.GetPersistentDouble(PD_LASTPX);
        if (pGap < 0.f) pGap = -pGap;
        if (dGap >= 0.0 && dGap <= gapDays && pGap <= (float)c.aggGap * tick + tick * 0.01f)
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
        RBTFlush(sc, st, c);
        sc.SetPersistentInt(PI_PENDSIDE, side);
        sc.SetPersistentDouble(PD_VOL, vol);
        sc.SetPersistentDouble(PD_PV, (double)price * vol);
        sc.SetPersistentDouble(PD_FIRSTDT, dt);
        sc.SetPersistentDouble(PD_LASTDT, dt);
        sc.SetPersistentDouble(PD_LASTPX, price);
    }
}

static int RBTSideFromTick(const s_IntradayRecord &r)
{
    if (r.AskVolume > 0 && r.BidVolume == 0)
        return 0;
    if (r.BidVolume > 0 && r.AskVolume == 0)
        return 1;
    const float px  = r.Close;
    const float ask = r.High;
    const float bid = r.Low;
    if (ask > 0.f && px + 0.00001f >= ask)
        return 0;
    if (bid > 0.f && px - 0.00001f <= bid)
        return 1;
    return -1;
}

// Historical 1-tick records from the chart's .scid file. Same data Large
// Volume Trade Indicator uses. Closed bars only, and only bars not yet
// ingested — walking the whole file every tick freezes the UI.
static void RBTIngestClosedBarsFromFile(SCStudyInterfaceRef sc, RBTState *st,
                                        const RBTCfg &c, int lastClosed,
                                        double gapDays, float tick)
{
    if (lastClosed < 0)
        return;

    int start = st->tickBarDone + 1;
    if (start < 0)
        start = 0;

    if (c.keepDays > 0)
    {
        const double cutoff = sc.BaseDateTimeIn[sc.ArraySize - 1].GetAsDouble()
                            - (double)c.keepDays;
        while (start <= lastClosed
               && sc.BaseDateTimeIn[start].GetAsDouble() < cutoff)
            ++start;
    }
    if (start > lastClosed)
        return;

    s_IntradayRecord rec;
    int reads = 0;
    for (int bar = start; bar <= lastClosed; ++bar)
    {
        int sub = 0;
        int first = 1;
        int ok = 1;
        while (ok)
        {
            const IntradayFileLockActionEnum lock = first
                ? IFLA_LOCK_READ_HOLD
                : IFLA_NO_CHANGE;
            first = 0;
            ok = sc.ReadIntradayFileRecordForBarIndexAndSubIndex(bar, sub, rec, lock);
            if (!ok)
                break;
            ++sub;
            ++reads;
            if (rec.IsBidAskUpdateOnly())
                continue;
            if (rec.TotalVolume == 0)
                continue;
            if (rec.NumTrades != 1 && !rec.IsSingleTradeWithBidAsk())
                continue;
            const int side = RBTSideFromTick(rec);
            if (side < 0)
                continue;
            const double dt = sc.ConvertDateTimeUTCToChartTimeZone(rec.DateTime).GetAsDouble();
            RBTIngestPrint(sc, st, c, dt, rec.Close,
                           (float)rec.TotalVolume, side, gapDays, tick);
        }
        sc.ReadIntradayFileRecordForBarIndexAndSubIndex(-1, -1, rec, IFLA_RELEASE_AFTER_READ);
    }

    RBTFlush(sc, st, c);
    st->tickBarDone = lastClosed;
    st->tickReads += reads;
}

static int RBTHistDayIndex(const RBTState *st, int dayDate)
{
    for (int i = 0; i < st->nHist; ++i)
    {
        if (st->histDate[i] == dayDate)
            return i;
    }
    return -1;
}

static void RBTRank(SCStudyInterfaceRef sc, RBTState *st, const RBTCfg &c,
                    int lastClosed, int today)
{
    const int count = sc.GetPersistentInt(PI_RCOUNT);
    const int head  = sc.GetPersistentInt(PI_RHEAD);

    int dates[64];
    int nd = 0;
    RBTCollectDates(st, count, head, dates, &nd);

    st->nDayAsk = st->nDayBid = st->nWeekAsk = st->nWeekBid = 0;
    st->nHist = 0;
    for (int i = 0; i < RBT_HIST_MAX; ++i)
        st->nHistAsk[i] = st->nHistBid[i] = 0;

    int want = c.histDays;
    if (want < 1) want = 1;
    if (want > RBT_HIST_MAX) want = RBT_HIST_MAX;
    for (int i = 0; i < nd && st->nHist < want; ++i)
    {
        if (dates[i] <= today)
            st->histDate[st->nHist++] = dates[i];
    }

    for (int q = 0; q < count; ++q)
    {
        const RBTTrade &t = st->ring[(head + q) % RBT_RING_CAP];
        if (t.vol <= 0.f)
            continue;
        if (c.minSize > 0.f && t.vol < c.minSize)
            continue;
        if (!RBTInSession(t.dt, c))
            continue;

        const int bi = RBTBarIndexForTime(sc, t.dt, t.price);
        if (bi >= 0 && bi > lastClosed)
            continue;

        RBTLeader cand;
        cand.dt = t.dt; cand.price = t.price; cand.vol = t.vol;
        cand.side = t.side; cand.rank = 0;

        const int hi = RBTHistDayIndex(st, t.dayDate);
        if (hi >= 0)
        {
            if (t.side == 0)
                RBTOffer(st->histAsk[hi], &st->nHistAsk[hi], c.dayN, cand);
            else
                RBTOffer(st->histBid[hi], &st->nHistBid[hi], c.dayN, cand);
        }
        if (t.dayDate == today)
        {
            if (t.side == 0)
                RBTOffer(st->dayAsk, &st->nDayAsk, c.dayN, cand);
            else
                RBTOffer(st->dayBid, &st->nDayBid, c.dayN, cand);
        }
        if (RBTInWeekWindow(t.dayDate, today, c, dates, nd))
        {
            if (t.side == 0)
                RBTOffer(st->weekAsk, &st->nWeekAsk, c.weekN, cand);
            else
                RBTOffer(st->weekBid, &st->nWeekBid, c.weekN, cand);
        }
    }

    for (int d = 0; d < st->nHist; ++d)
    {
        RBTAssignRanks(st->histAsk[d], st->nHistAsk[d]);
        RBTAssignRanks(st->histBid[d], st->nHistBid[d]);
    }
    RBTAssignRanks(st->dayAsk,  st->nDayAsk);
    RBTAssignRanks(st->dayBid,  st->nDayBid);
    RBTAssignRanks(st->weekAsk, st->nWeekAsk);
    RBTAssignRanks(st->weekBid, st->nWeekBid);
}

static void RBTPushDisp(RBTState *st, const RBTLeader &L, COLORREF col, int bar, int sg)
{
    if (st->nDisp >= RBT_DISP_CAP)
        return;
    if (sg < 0 || sg >= RBT_BUBBLE_SG)
        return;
    RBTDisp &d = st->disp[st->nDisp];
    d.dt = L.dt; d.price = L.price; d.vol = L.vol;
    d.side = L.side; d.rank = L.rank; d.bar = bar; d.sg = sg; d.col = col;
    st->nDisp += 1;
}

static void RBTBuildDisplay(SCStudyInterfaceRef sc, RBTState *st, const RBTCfg &c)
{
    st->nDisp = 0;
    const int dayN = c.dayN;
    if (c.showDay && c.showAsk)
    {
        for (int d = 0; d < st->nHist; ++d)
        {
            for (int i = 0; i < st->nHistAsk[d]; ++i)
            {
                const RBTLeader &L = st->histAsk[d][i];
                RBTPushDisp(st, L, c.colDayAsk,
                            RBTBarIndexForTime(sc, L.dt, L.price), i);
            }
        }
    }
    if (c.showDay && c.showBid)
    {
        const int sg0 = dayN;
        for (int d = 0; d < st->nHist; ++d)
        {
            for (int i = 0; i < st->nHistBid[d]; ++i)
            {
                const RBTLeader &L = st->histBid[d][i];
                RBTPushDisp(st, L, c.colDayBid,
                            RBTBarIndexForTime(sc, L.dt, L.price), sg0 + i);
            }
        }
    }
    int weekSg = dayN * 2;
    if (weekSg < 0) weekSg = 0;
    if (c.showWeek && c.showAsk)
    {
        for (int i = 0; i < st->nWeekAsk; ++i)
        {
            if (c.showDay && c.showAsk
                && RBTInLeaderList(st->dayAsk, st->nDayAsk, st->weekAsk[i].dt, st->weekAsk[i].side))
                continue;
            if (weekSg >= RBT_BUBBLE_SG)
                break;
            RBTPushDisp(st, st->weekAsk[i], c.colWkAsk,
                        RBTBarIndexForTime(sc, st->weekAsk[i].dt, st->weekAsk[i].price),
                        weekSg);
            ++weekSg;
        }
    }
    if (c.showWeek && c.showBid)
    {
        for (int i = 0; i < st->nWeekBid; ++i)
        {
            if (c.showDay && c.showBid
                && RBTInLeaderList(st->dayBid, st->nDayBid, st->weekBid[i].dt, st->weekBid[i].side))
                continue;
            if (weekSg >= RBT_BUBBLE_SG)
                break;
            RBTPushDisp(st, st->weekBid[i], c.colWkBid,
                        RBTBarIndexForTime(sc, st->weekBid[i].dt, st->weekBid[i].price),
                        weekSg);
            ++weekSg;
        }
    }
}

static void RBTRewriteSlots(SCStudyInterfaceRef sc, RBTState *st)
{
    for (int i = 0; i < st->nPrevW; ++i)
        RBTZeroSlot(sc, st->prevW[i].sg, st->prevW[i].bar);

    st->nPrevW = 0;
    for (int i = 0; i < st->nDisp; ++i)
    {
        const int bar = st->disp[i].bar;
        const int sg  = st->disp[i].sg;
        if (bar < 0 || sg < 0 || sg >= RBT_BUBBLE_SG)
            continue;
        sc.Subgraph[sg][bar]           = st->disp[i].price;
        sc.Subgraph[sg].Arrays[1][bar] = st->disp[i].vol;
        sc.Subgraph[sg].DataColor[bar] = st->disp[i].col;
        if (st->nPrevW < RBT_WRITE_CAP)
        {
            st->prevW[st->nPrevW].sg  = sg;
            st->prevW[st->nPrevW].bar = bar;
            st->nPrevW += 1;
        }
    }

    sc.SetPersistentInt(PI_DISPCNT, st->nDisp);
}

// =============================================================================
SCSFExport scsf_RankedBigTrades(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName      = "Ranked Big Trades";
        sc.GraphRegion    = 0;
        sc.AutoLoop       = 0;
        sc.ScaleRangeType = SCALE_SAMEASREGION;
        sc.DrawZeros      = 0;
        sc.ValueFormat    = VALUEFORMAT_INHERITED;
        sc.UpdateAlways = 1;
        // Required for ReadIntradayFileRecordForBarIndexAndSubIndex (historical
        // 1-tick .scid records — same source as Large Volume Trade Indicator).
        sc.MaintainAdditionalChartDataArrays = 1;

        for (int k = 0; k < RBT_BUBBLE_SG; ++k)
        {
            SCString nm;
            nm.Format("Slot %d", k + 1);
            sc.Subgraph[k].Name         = nm;
            sc.Subgraph[k].DrawStyle    = DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE;
            sc.Subgraph[k].PrimaryColor = RGB(50, 140, 255);
            sc.Subgraph[k].DrawZeros    = 0;
            sc.Subgraph[k].AutoColoring = AUTOCOLOR_NONE;
        }
        sc.Subgraph[RBT_SG_CACHE].Name      = "Cache (internal)";
        sc.Subgraph[RBT_SG_CACHE].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[RBT_SG_CACHE].DrawZeros = 0;

        sc.Input[IN_AGGMS].Name = "Aggregation Period (ms, 0=raw prints)";
        sc.Input[IN_AGGMS].SetInt(1000);
        sc.Input[IN_AGGMS].SetIntLimits(0, 60000);

        sc.Input[IN_AGGGAP].Name = "Aggregation Max Tick Gap";
        sc.Input[IN_AGGGAP].SetInt(2);
        sc.Input[IN_AGGGAP].SetIntLimits(0, 100);

        sc.Input[IN_MINSIZE].Name = "Min Size Floor (0=all eligible)";
        sc.Input[IN_MINSIZE].SetFloat(50.0f);
        sc.Input[IN_MINSIZE].SetFloatLimits(0.0f, 100000000.0f);

        sc.Input[IN_DAYN].Name = "Day Top N Per Side";
        sc.Input[IN_DAYN].SetInt(10);
        sc.Input[IN_DAYN].SetIntLimits(1, RBT_MAX_N);

        sc.Input[IN_WEEKN].Name = "Week Top N Per Side";
        sc.Input[IN_WEEKN].SetInt(10);
        sc.Input[IN_WEEKN].SetIntLimits(1, RBT_MAX_N);

        sc.Input[IN_SHOWDAY].Name = "Show Day";
        sc.Input[IN_SHOWDAY].SetYesNo(1);

        sc.Input[IN_SHOWWEEK].Name = "Show Week";
        sc.Input[IN_SHOWWEEK].SetYesNo(1);

        sc.Input[IN_SHOWASK].Name = "Show Ask";
        sc.Input[IN_SHOWASK].SetYesNo(1);

        sc.Input[IN_SHOWBID].Name = "Show Bid";
        sc.Input[IN_SHOWBID].SetYesNo(1);

        sc.Input[IN_SESSFILT].Name = "Session Filter";
        sc.Input[IN_SESSFILT].SetCustomInputStrings("Overnight (ETH);RTH;Full day (ETH+RTH)");
        sc.Input[IN_SESSFILT].SetCustomInputIndex(2);

        sc.Input[IN_RTHSTART].Name = "RTH Start Time";
        sc.Input[IN_RTHSTART].SetTime(SCDateTime(9, 30, 0, 0).GetTime());

        sc.Input[IN_RTHEND].Name = "RTH End Time";
        sc.Input[IN_RTHEND].SetTime(SCDateTime(16, 0, 0, 0).GetTime());

        sc.Input[IN_ETHSTART].Name = "ETH Start Time (Globex open)";
        sc.Input[IN_ETHSTART].SetTime(SCDateTime(18, 0, 0, 0).GetTime());

        sc.Input[IN_FULLEND].Name = "Full Day End Time";
        sc.Input[IN_FULLEND].SetTime(SCDateTime(17, 0, 0, 0).GetTime());

        sc.Input[IN_WEEKMODE].Name = "Week Mode";
        sc.Input[IN_WEEKMODE].SetCustomInputStrings("Current week;Rolling N trading days");
        sc.Input[IN_WEEKMODE].SetCustomInputIndex(1);

        sc.Input[IN_ROLLN].Name = "Rolling Trading Days";
        sc.Input[IN_ROLLN].SetInt(5);
        sc.Input[IN_ROLLN].SetIntLimits(1, 20);

        sc.Input[IN_COLDAYASK].Name = "Day Ask Color";
        sc.Input[IN_COLDAYASK].SetColor(RGB(50, 140, 255));

        sc.Input[IN_COLDAYBID].Name = "Day Bid Color";
        sc.Input[IN_COLDAYBID].SetColor(RGB(235, 55, 55));

        sc.Input[IN_COLWKASK].Name = "Week Ask Color";
        sc.Input[IN_COLWKASK].SetColor(RGB(30, 140, 130));

        sc.Input[IN_COLWKBID].Name = "Week Bid Color";
        sc.Input[IN_COLWKBID].SetColor(RGB(200, 140, 40));

        sc.Input[IN_SIZEMIN].Name = "Bubble Size Min (px)";
        sc.Input[IN_SIZEMIN].SetInt(6);
        sc.Input[IN_SIZEMIN].SetIntLimits(1, 200);

        sc.Input[IN_SIZEMAX].Name = "Bubble Size Max (px, largest in view)";
        sc.Input[IN_SIZEMAX].SetInt(28);
        sc.Input[IN_SIZEMAX].SetIntLimits(1, 200);

        sc.Input[IN_SCALING].Name = "Size Scaling";
        sc.Input[IN_SCALING].SetCustomInputStrings("Linear;Square Root");
        sc.Input[IN_SCALING].SetCustomInputIndex(1);

        sc.Input[IN_LABEL].Name = "Bubble Label";
        sc.Input[IN_LABEL].SetCustomInputStrings("Off;Size;Rank;Rank+Size");
        sc.Input[IN_LABEL].SetCustomInputIndex(3);

        sc.Input[IN_LABELSIZE].Name = "Label Font Size";
        sc.Input[IN_LABELSIZE].SetInt(8);
        sc.Input[IN_LABELSIZE].SetIntLimits(4, 72);

        sc.Input[IN_LABELCOL].Name = "Label Color";
        sc.Input[IN_LABELCOL].SetColor(RGB(255, 255, 255));

        sc.Input[IN_ALERT].Name = "Alert On New Day Leader";
        sc.Input[IN_ALERT].SetYesNo(0);

        sc.Input[IN_ALERTSND].Name = "Alert Sound Number";
        sc.Input[IN_ALERTSND].SetInt(1);
        sc.Input[IN_ALERTSND].SetIntLimits(0, 100);

        sc.Input[IN_KEEPDAYS].Name = "History File Keep (days, 0=off)";
        sc.Input[IN_KEEPDAYS].SetInt(10);
        sc.Input[IN_KEEPDAYS].SetIntLimits(0, 365);

        sc.Input[IN_HISTDAYS].Name = "Day History (trading days, 1=today only)";
        sc.Input[IN_HISTDAYS].SetInt(5);
        sc.Input[IN_HISTDAYS].SetIntLimits(1, RBT_HIST_MAX);

        sc.Input[IN_STATUS].Name = "Show Status Line";
        sc.Input[IN_STATUS].SetYesNo(1);

        sc.Input[IN_DEBUG].Name = "Debug Log";
        sc.Input[IN_DEBUG].SetCustomInputStrings("Off;On");
        sc.Input[IN_DEBUG].SetCustomInputIndex(0);

        return;
    }

    if (sc.LastCallToFunction)
    {
        RBTDeleteOurDrawings(sc);
        RBTState *p = (RBTState*)sc.GetPersistentPointer(1);
        if (p != NULL)
        {
            delete p;
            sc.SetPersistentPointer(1, NULL);
        }
        return;
    }

    if (sc.ArraySize < 1)
        return;

    RBTState *st = RBTGet(sc);

    RBTCfg cfg;
    cfg.aggMs     = sc.Input[IN_AGGMS].GetInt();
    cfg.aggGap    = sc.Input[IN_AGGGAP].GetInt();
    cfg.minSize   = sc.Input[IN_MINSIZE].GetFloat();
    cfg.dayN      = sc.Input[IN_DAYN].GetInt();
    cfg.weekN     = sc.Input[IN_WEEKN].GetInt();
    if (cfg.dayN < 1) cfg.dayN = 1;
    if (cfg.dayN > RBT_MAX_N) cfg.dayN = RBT_MAX_N;
    if (cfg.weekN < 1) cfg.weekN = 1;
    if (cfg.weekN > RBT_MAX_N) cfg.weekN = RBT_MAX_N;
    cfg.showDay   = (sc.Input[IN_SHOWDAY].GetYesNo() != 0) ? 1 : 0;
    cfg.showWeek  = (sc.Input[IN_SHOWWEEK].GetYesNo() != 0) ? 1 : 0;
    cfg.showAsk   = (sc.Input[IN_SHOWASK].GetYesNo() != 0) ? 1 : 0;
    cfg.showBid   = (sc.Input[IN_SHOWBID].GetYesNo() != 0) ? 1 : 0;
    cfg.sessFilt  = sc.Input[IN_SESSFILT].GetIndex();
    cfg.rthStart  = sc.Input[IN_RTHSTART].GetTime();
    cfg.rthEnd    = sc.Input[IN_RTHEND].GetTime();
    cfg.ethStart  = sc.Input[IN_ETHSTART].GetTime();
    cfg.fullEnd   = sc.Input[IN_FULLEND].GetTime();
    cfg.weekMode  = sc.Input[IN_WEEKMODE].GetIndex();
    cfg.rollN     = sc.Input[IN_ROLLN].GetInt();
    if (cfg.rollN < 1) cfg.rollN = 1;
    if (cfg.rollN > 20) cfg.rollN = 20;
    cfg.colDayAsk = sc.Input[IN_COLDAYASK].GetColor();
    cfg.colDayBid = sc.Input[IN_COLDAYBID].GetColor();
    cfg.colWkAsk  = sc.Input[IN_COLWKASK].GetColor();
    cfg.colWkBid  = sc.Input[IN_COLWKBID].GetColor();
    cfg.sizeMin   = sc.Input[IN_SIZEMIN].GetInt();
    cfg.sizeMax   = sc.Input[IN_SIZEMAX].GetInt();
    if (cfg.sizeMin > cfg.sizeMax)
    {
        const int tmp = cfg.sizeMin;
        cfg.sizeMin = cfg.sizeMax;
        cfg.sizeMax = tmp;
    }
    cfg.scaling   = sc.Input[IN_SCALING].GetIndex();
    cfg.labelMode = sc.Input[IN_LABEL].GetIndex();
    cfg.labelSize = sc.Input[IN_LABELSIZE].GetInt();
    cfg.labelCol  = sc.Input[IN_LABELCOL].GetColor();
    cfg.alertOn   = (sc.Input[IN_ALERT].GetYesNo() != 0) ? 1 : 0;
    cfg.alertSnd  = sc.Input[IN_ALERTSND].GetInt();
    cfg.keepDays  = sc.Input[IN_KEEPDAYS].GetInt();
    cfg.histDays  = sc.Input[IN_HISTDAYS].GetInt();
    if (cfg.histDays < 1) cfg.histDays = 1;
    if (cfg.histDays > RBT_HIST_MAX) cfg.histDays = RBT_HIST_MAX;
    cfg.showStatus= (sc.Input[IN_STATUS].GetYesNo() != 0) ? 1 : 0;
    cfg.debugOn   = (sc.Input[IN_DEBUG].GetIndex() == 1) ? 1 : 0;

    const int last = sc.ArraySize - 1;

    if (sc.GetPersistentInt(PI_LOADED) == 0)
    {
        sc.SetPersistentInt(PI_LOADED, 1);
        double loadedMaxDt = 0.0;
        if (cfg.keepDays > 0)
        {
            const double cutoff = sc.BaseDateTimeIn[last].GetAsDouble() - (double)cfg.keepDays;
            RBTFileLoad(sc, st, cutoff, loadedMaxDt);
        }
        sc.SetPersistentDouble(PD_LOADHWM, loadedMaxDt);
    }

    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        RBTDeleteOurDrawings(sc);
        st->nDisp = 0;
        st->nPrevW = 0;
        for (int i = 0; i < RBT_DISP_CAP; ++i)
            st->labelOn[i] = 0;
        st->prevVFirst = -1;
        st->prevVLast  = -1;
        sc.SetPersistentInt(PI_DISPCNT, 0);
        sc.SetPersistentInt(PI_LABELDIRTY, 1);
        sc.SetPersistentInt(PI_STATUSON, 0);
    }

    const double gapDays = (double)cfg.aggMs / 86400000.0;
    const float  tick    = (sc.TickSize > 0.f) ? sc.TickSize : 0.25f;
    const int    lastClosedPre = sc.ArraySize - 2;

    // ---- 1a) Historical 1-tick .scid (same source as Large Volume Trade) ----
    RBTIngestClosedBarsFromFile(sc, st, cfg, lastClosedPre, gapDays, tick);

    // Fallback if the .scid is not 1-tick (ticks=0): seed from BigTradesTape CSV.
    if (st->tickReads == 0 && sc.GetPersistentInt(PI_RCOUNT) == 0)
    {
        double maxDt = sc.GetPersistentDouble(PD_LOADHWM);
        const int nImp = RBTImportBigTradesTape(sc, st, maxDt);
        if (nImp > 0)
        {
            RBTFileRewrite(sc, st);
            sc.SetPersistentDouble(PD_LOADHWM, maxDt);
            SCString m;
            m.Format("RankedBigTrades: no 1-tick file records; seeded %d from BigTradesTape CSV", nImp);
            sc.AddMessageToLog(m, 0);
        }
        else if (st->dbgOnce == 0)
        {
            sc.AddMessageToLog(
                "RankedBigTrades: no historical ticks. Set Global Settings >> "
                "Data/Trade Service Settings >> Intraday Data Storage Time Unit = 1 Tick "
                "(same requirement as Large Volume Trade Indicator).", 0);
        }
    }

    // ---- 1b) Live T&S (forming bar / RAM buffer) ---------------------------
    c_SCTimeAndSalesArray TS;
    sc.GetTimeAndSales(TS);
    const int tsSize = TS.Size();

    unsigned int lastSeq = (unsigned int)sc.GetPersistentInt(PI_SEQ);
    int dbgRead = 0;

    int startIdx = 0;
    {
        int x = tsSize - 1;
        while (x >= 0 && TS[x].Sequence > lastSeq)
            --x;
        startIdx = x + 1;
    }

    for (int x = startIdx; x < tsSize; ++x)
    {
        const s_TimeAndSales &r = TS[x];
        if (r.Sequence == 0 || r.Sequence <= lastSeq)
            continue;
        lastSeq = r.Sequence;
        ++dbgRead;

        if (r.Type != SC_TS_BID && r.Type != SC_TS_ASK)
            continue;
        if (r.Volume == 0)
            continue;

        double mult = sc.RealTimePriceMultiplier;
        if (mult == 0.0)
            mult = 1.0;
        const float price = (float)(r.Price * mult);
        const float vol   = (float)r.Volume;
        const int   side  = (r.Type == SC_TS_ASK) ? 0 : 1;
        const double dt   = sc.ConvertDateTimeUTCToChartTimeZone(r.DateTime).GetAsDouble();
        RBTIngestPrint(sc, st, cfg, dt, price, vol, side, gapDays, tick);
    }
    sc.SetPersistentInt(PI_SEQ, (int)lastSeq);

    if (sc.GetPersistentInt(PI_PENDSIDE) >= 0)
    {
        const double idleDays = sc.CurrentSystemDateTime.GetAsDouble()
                              - sc.GetPersistentDouble(PD_LASTDT);
        if (sc.IsFullRecalculation || idleDays > gapDays * 4.0 + (1.0 / 86400.0))
            RBTFlush(sc, st, cfg);
    }

    // ---- 2) Rank on bar close / recalc / day rollover ------------------------
    const int lastClosed = sc.ArraySize - 2;
    const int today = RBTDayDate(sc, sc.BaseDateTimeIn[last].GetAsDouble());
    const bool rankNow = sc.IsFullRecalculation
                      || lastClosed > sc.GetPersistentInt(PI_LASTCLOSED)
                      || today != sc.GetPersistentInt(PI_LASTDAY);

    if (lastClosed >= 0 && rankNow)
    {
        RBTLeader oldAsk[RBT_MAX_N];
        RBTLeader oldBid[RBT_MAX_N];
        const int nOldAsk = st->nDayAsk;
        const int nOldBid = st->nDayBid;
        for (int i = 0; i < nOldAsk; ++i) oldAsk[i] = st->dayAsk[i];
        for (int i = 0; i < nOldBid; ++i) oldBid[i] = st->dayBid[i];

        RBTRank(sc, st, cfg, lastClosed, today);

        if (cfg.alertOn && !sc.IsFullRecalculation)
        {
            for (int i = 0; i < st->nDayAsk; ++i)
            {
                if (!RBTInLeaderList(oldAsk, nOldAsk, st->dayAsk[i].dt, st->dayAsk[i].side))
                {
                    SCString msg;
                    msg.Format("RankedBigTrades: DAY ASK #%d %d @ %.2f",
                               st->dayAsk[i].rank,
                               (int)(st->dayAsk[i].vol + 0.5f),
                               st->dayAsk[i].price);
                    sc.SetAlert(cfg.alertSnd, msg);
                }
            }
            for (int i = 0; i < st->nDayBid; ++i)
            {
                if (!RBTInLeaderList(oldBid, nOldBid, st->dayBid[i].dt, st->dayBid[i].side))
                {
                    SCString msg;
                    msg.Format("RankedBigTrades: DAY BID #%d %d @ %.2f",
                               st->dayBid[i].rank,
                               (int)(st->dayBid[i].vol + 0.5f),
                               st->dayBid[i].price);
                    sc.SetAlert(cfg.alertSnd, msg);
                }
            }
        }

        RBTBuildDisplay(sc, st, cfg);
        RBTRewriteSlots(sc, st);
        sc.SetPersistentInt(PI_LABELDIRTY, 1);
        sc.SetPersistentInt(PI_LASTCLOSED, lastClosed);
        sc.SetPersistentInt(PI_LASTDAY, today);

        if (cfg.debugOn)
        {
            SCString m;
            m.Format("RankedBigTrades dbg: tsArray=%d newRecs=%d ledger=%d "
                     "dA=%d dB=%d wA=%d wB=%d today=%d filt=%d agg=%d",
                     tsSize, dbgRead, sc.GetPersistentInt(PI_RCOUNT),
                     st->nDayAsk, st->nDayBid, st->nWeekAsk, st->nWeekBid,
                     today, cfg.sessFilt, cfg.aggMs);
            sc.AddMessageToLog(m, 0);
        }
    }

    const bool hidden = (sc.HideStudy != 0);
    const bool wasHidden = (sc.GetPersistentInt(PI_HIDDEN) != 0);

    if (!hidden && cfg.showStatus)
    {
        const char *sess = (cfg.sessFilt == 0) ? "ETH"
                         : (cfg.sessFilt == 1) ? "RTH" : "FULL";
        SCString line;
        line.Format("RBT ts=%d ticks=%d led=%d  dA=%d/%d dB=%d/%d  hist=%d  %s  floor=%.0f",
                    tsSize, st->tickReads, sc.GetPersistentInt(PI_RCOUNT),
                    st->nDayAsk, cfg.dayN, st->nDayBid, cfg.dayN,
                    cfg.histDays, sess, cfg.minSize);
        RBTStatus(sc, true, line, RGB(200, 200, 200));
    }
    else if (hidden || !cfg.showStatus)
    {
        if (sc.GetPersistentInt(PI_STATUSON) != 0 && hidden)
            ; // hide path below deletes drawings
        else if (!cfg.showStatus && sc.GetPersistentInt(PI_STATUSON) != 0)
            RBTStatus(sc, false, "", 0);
    }

    if (st->dbgOnce == 0)
    {
        st->dbgOnce = 1;
        SCString m;
        m.Format("RankedBigTrades: symbol=%s  ts=%d  ticks=%d  ledger=%d  floor=%.0f",
                 sc.Symbol.GetChars(), tsSize, st->tickReads,
                 sc.GetPersistentInt(PI_RCOUNT), cfg.minSize);
        sc.AddMessageToLog(m, 0);
    }

    // ---- 3) View-relative sizing --------------------------------------------
    int vFirst = sc.IndexOfFirstVisibleBar;
    int vLast  = sc.IndexOfLastVisibleBar;
    if (vFirst < 0) vFirst = 0;
    if (vLast > last || vLast < 0) vLast = last;
    if (vFirst > vLast) { vFirst = 0; vLast = last; }

    if (!hidden)
    {
        float maxVis = 0.f;
        for (int i = 0; i < st->nDisp; ++i)
        {
            const int bar = st->disp[i].bar;
            if (bar < vFirst || bar > vLast)
                continue;
            if (st->disp[i].vol > maxVis)
                maxVis = st->disp[i].vol;
        }
        if (maxVis > 0.f)
        {
            for (int i = 0; i < st->nDisp; ++i)
            {
                const int bar = st->disp[i].bar;
                if (bar < 0)
                    continue;
                float f = st->disp[i].vol / maxVis;
                if (cfg.scaling == 1)
                    f = sqrt(f);
                sc.Subgraph[i].Arrays[0][bar] =
                    (float)cfg.sizeMin + f * (float)(cfg.sizeMax - cfg.sizeMin);
            }
        }
    }

    // ---- 4) Labels -----------------------------------------------------------
    {
        const int lastMode = sc.GetPersistentInt(PI_LASTLBLMODE);
        const int lastSize = sc.GetPersistentInt(PI_LASTLBLSIZE);
        const int lastCol  = sc.GetPersistentInt(PI_LASTLBLCOL);
        if (lastMode != cfg.labelMode || lastSize != cfg.labelSize
            || lastCol != (int)cfg.labelCol)
        {
            sc.SetPersistentInt(PI_LABELDIRTY, 1);
            sc.SetPersistentInt(PI_LASTLBLMODE, cfg.labelMode);
            sc.SetPersistentInt(PI_LASTLBLSIZE, cfg.labelSize);
            sc.SetPersistentInt(PI_LASTLBLCOL, (int)cfg.labelCol);
        }
    }

    if (hidden)
    {
        if (!wasHidden)
        {
            for (int i = 0; i < RBT_DISP_CAP; ++i)
            {
                RBTLabel(sc, RBT_LINE_BASE + i, false, 0, 0.f, "", 0, 0);
                st->labelOn[i] = 0;
            }
            RBTStatus(sc, false, "", 0);
            sc.SetPersistentInt(PI_HIDDEN, 1);
        }
    }
    else
    {
        if (wasHidden)
        {
            sc.SetPersistentInt(PI_HIDDEN, 0);
            sc.SetPersistentInt(PI_LABELDIRTY, 1);
        }

        const bool dirty = (sc.GetPersistentInt(PI_LABELDIRTY) != 0);
        const bool rangeChanged = (vFirst != st->prevVFirst || vLast != st->prevVLast);

        if (cfg.labelMode == 0)
        {
            if (dirty || rangeChanged)
            {
                for (int i = 0; i < RBT_DISP_CAP; ++i)
                {
                    if (st->labelOn[i])
                    {
                        RBTLabel(sc, RBT_LINE_BASE + i, false, 0, 0.f, "", 0, 0);
                        st->labelOn[i] = 0;
                    }
                }
                sc.SetPersistentInt(PI_LABELDIRTY, 0);
                st->prevVFirst = vFirst;
                st->prevVLast  = vLast;
            }
        }
        else if (dirty || rangeChanged)
        {
            for (int i = 0; i < st->nDisp; ++i)
            {
                const int bar = st->disp[i].bar;
                const bool vis = (bar >= vFirst && bar <= vLast && bar >= 0);
                if (!vis)
                {
                    if (st->labelOn[i])
                    {
                        RBTLabel(sc, RBT_LINE_BASE + i, false, 0, 0.f, "", 0, 0);
                        st->labelOn[i] = 0;
                    }
                    continue;
                }
                SCString t;
                const int sz = (int)(st->disp[i].vol + 0.5f);
                if (cfg.labelMode == 1)
                    t.Format("%d", sz);
                else if (cfg.labelMode == 2)
                    t.Format("#%d", st->disp[i].rank);
                else
                    t.Format("#%d %d", st->disp[i].rank, sz);
                RBTLabel(sc, RBT_LINE_BASE + i, true, bar, st->disp[i].price, t,
                         cfg.labelCol, cfg.labelSize);
                st->labelOn[i] = 1;
            }
            for (int i = st->nDisp; i < RBT_DISP_CAP; ++i)
            {
                if (st->labelOn[i])
                {
                    RBTLabel(sc, RBT_LINE_BASE + i, false, 0, 0.f, "", 0, 0);
                    st->labelOn[i] = 0;
                }
            }
            sc.SetPersistentInt(PI_LABELDIRTY, 0);
            st->prevVFirst = vFirst;
            st->prevVLast  = vLast;
        }
    }
}
