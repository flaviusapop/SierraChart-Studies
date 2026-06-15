// BigTradesTape.cpp
// =============================================================================
// Big Trades Tape  (Sierra Chart ACSIL)
//
// MotiveWave-"Big Trades"-style detector driven by TIME AND SALES. Individual
// prints are aggregated into trades: consecutive prints on the SAME side,
// arriving within the Aggregation Period of each other and within Max Tick
// Gap of the running trade's last price (sweeps split across levels), are
// merged. When an aggregate's total size reaches the session's minimum size
// it is committed as a bubble at the aggregate's volume-weighted price on the
// bar containing its first print.
//
// Blue = buy aggressor (at ask), Red = sell aggressor (at bid).
//
// SESSIONS — trades are classified RTH/Globex by the RTH Start/End time
// inputs (time of day, chart time zone; window may wrap midnight). Session
// Filter shows All / RTH Only / Globex Only. Globex Min Size (0 = use Min
// Aggregated Size) lets the overnight threshold differ from RTH.
//
// VIEW-RELATIVE SIZING — every chart update the study finds the largest
// committed trade among the bars CURRENTLY VISIBLE on screen and rescales all
// visible bubbles between Bubble Size Min..Max relative to it.
//
// DISPLAY CAP — Max Bubbles Displayed keeps only the most recent N bubbles;
// older ones (and their labels) are pruned oldest-bar-first.
//
// PERSISTENCE — committed trades are kept in an in-memory ledger that
// survives full recalculations (settings changes, chart reloads): bubbles are
// redrawn from the ledger, re-mapped to bars by timestamp. The ledger lives
// until the study is removed or SC closes.
//
// HISTORY — Time and Sales is an in-memory rolling buffer; first-run backfill
// reaches only as far as Sierra retains records (Global Settings >>
// Data/Trade Service Settings >> Time and Sales records to retain).
//
//   SG 0..23   Buy bubble slots    SG 24..47  Sell bubble slots
//   SG 48      Hidden cache (per-bar slot counts)
//
// -----------------------------------------------------------------------------
// BUILD NOTES (confirmed SC build-server patterns)
//   - No std::min/std::max/std::fabs, no <algorithm>.
//   - DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE: Data[] = price,
//     Arrays[0][] = pixel size, DataColor[] (AUTOCOLOR_NONE set first).
//     Arrays[1][] holds the aggregate volume (for view-relative sizing).
//   - T&S prices must be multiplied by sc.RealTimePriceMultiplier.
//   - Do NOT read the intraday tick file on renko charts (no records / SC
//     freeze). T&S is the supported per-trade path.
//   - sc.GetContainingIndexForSCDateTime resolved to the CURRENT bar when
//     redrawing after a recalculation — map time to bar with a manual binary
//     search over sc.BaseDateTimeIn. Anchor text drawings by BeginIndex with
//     that bar (DRAWING_TEXT anchored only by BeginDateTime did not render).
//   - SCDateTime has no SetAsDouble; construct from double when needed.
//   - Labels via sc.UseTool DRAWING_TEXT; all study drawings deleted up front
//     on full recalculation (drawings do NOT auto-delete on recalc).
// =============================================================================

#include "sierrachart.h"
#include <stdio.h>

SCDLLName("BigTradesTape")

const int SLOTS    = 24;            // bubble slots per side per bar
const int SG_CACHE = 2 * SLOTS;     // hidden cache subgraph

inline int BubSG(int side, int slot) { return side * SLOTS + slot; }   // side 0=buy 1=sell

// Cache extra-array layout
const int CA_NBUY  = 0;   // committed buy bubbles on the bar
const int CA_NSELL = 1;   // committed sell bubbles on the bar

const COLORREF COL_BUY  = RGB(60, 120, 235);
const COLORREF COL_SELL = RGB(225, 60, 60);

// ---- Inputs --------------------------------------------------------------------
const int IN_AGGMS     = 0;   // aggregation period (ms, 0 = raw prints)
const int IN_AGGGAP    = 1;   // max tick gap to keep aggregating (sweeps)
const int IN_MINSIZE   = 2;   // min aggregated size to commit (RTH)
const int IN_MAXBUB    = 3;   // max bubbles per side per bar
const int IN_SIZEMIN   = 4;   // bubble px at the small end of the view
const int IN_SIZEMAX   = 5;   // bubble px for the largest trade in view
const int IN_SCALING   = 6;   // linear / square root
const int IN_LABEL     = 7;   // off / volume
const int IN_LABELSIZE = 8;
const int IN_LABELCOL  = 9;
const int IN_ALERTMIN  = 10;  // alert when an aggregate >= this size (0 = off)
const int IN_ALERTSND  = 11;
const int IN_MAXTOTAL  = 12;  // max bubbles displayed (most recent)
const int IN_DEBUG     = 13;  // diagnostic summary to message log
const int IN_SESSFILT  = 14;  // session filter: all / RTH / Globex
const int IN_RTHSTART  = 15;  // RTH start time (chart time zone)
const int IN_RTHEND    = 16;  // RTH end time
const int IN_GLOBEXMIN = 17;  // Globex min size (0 = use Min Aggregated Size)
const int IN_KEEPDAYS  = 18;  // history file retention (days, 0 = no file)

// ---- Persistent state ------------------------------------------------------------
const int PI_SEQ      = 0;    // last processed T&S sequence number
const int PI_PENDSIDE = 1;    // pending aggregate side: -1 none, 0 buy, 1 sell
const int PI_TOTAL    = 2;    // total bubbles currently displayed
const int PI_OLDBAR   = 3;    // oldest bar that may still hold bubbles
const int PI_RCOUNT   = 4;    // trades stored in the persistent ledger ring
const int PI_RHEAD    = 5;    // ring head (oldest entry)
const int PI_LOADED   = 6;    // 1 = history file loaded this SC session
const int PD_VOL      = 0;    // pending volume
const int PD_PV       = 1;    // pending sum(price*volume)
const int PD_LASTDT   = 2;    // pending last print datetime (as double)
const int PD_LASTPX   = 3;    // pending last print price
const int PD_FIRSTDT  = 4;    // pending first print datetime (as double)

// Persistent in-memory ledger of committed trades. Survives full
// recalculations — bubbles are redrawn from it instead of being lost.
struct BTTrade { double dt; float price; float vol; int side; };
const int RING_CAP = 65536;

// ---- History file (per symbol, in the SC Data Files folder) -----------------------
static void BTFilePath(SCStudyInterfaceRef sc, SCString &path)
{
    // Sanitize the symbol for use in a filename.
    char sym[128];
    const char *s = sc.Symbol.GetChars();
    int n = 0;
    for (int i = 0; s != NULL && s[i] != 0 && n < 120; ++i)
    {
        const char ch = s[i];
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')
                     || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '.';
        sym[n++] = ok ? ch : '_';
    }
    sym[n] = 0;
    path.Format("%s\\BigTradesTape_%s.csv", sc.DataFilesFolder().GetChars(), sym);
}

static void BTFileAppend(SCStudyInterfaceRef sc, double dt, float price,
                         float vol, int side)
{
    SCString path;
    BTFilePath(sc, path);
    FILE *f = fopen(path.GetChars(), "a");
    if (f == NULL)
        return;
    fprintf(f, "%.10f,%.4f,%.0f,%d\n", dt, price, vol, side);
    fclose(f);
}

// Settings bundle threaded through commit/flush.
struct BTCfg
{
    float    minSize;     // RTH minimum aggregate size
    float    globexMin;   // Globex minimum (0 = use minSize)
    int      sessFilt;    // 0 all, 1 RTH only, 2 Globex only
    int      rthStart;    // seconds since midnight
    int      rthEnd;
    int      maxSlots;
    int      labelMode;
    int      labelSize;
    COLORREF labelCol;
    float    alertMin;
    int      alertSnd;
    int      keepDays;    // history file retention (0 = no file)
};

static BTTrade* BTRing(SCStudyInterfaceRef sc)
{
    BTTrade *p = (BTTrade*)sc.GetPersistentPointer(1);
    if (p == NULL)
    {
        p = new BTTrade[RING_CAP];
        sc.SetPersistentPointer(1, p);
        sc.SetPersistentInt(PI_RCOUNT, 0);
        sc.SetPersistentInt(PI_RHEAD, 0);
    }
    return p;
}

static void BTRingAppend(SCStudyInterfaceRef sc, double dt, float price,
                         float vol, int side)
{
    BTTrade *R = BTRing(sc);
    int count = sc.GetPersistentInt(PI_RCOUNT);
    int head  = sc.GetPersistentInt(PI_RHEAD);
    int idx;
    if (count < RING_CAP)
    {
        idx = (head + count) % RING_CAP;
        sc.SetPersistentInt(PI_RCOUNT, count + 1);
    }
    else
    {
        idx = head;   // overwrite oldest
        sc.SetPersistentInt(PI_RHEAD, (head + 1) % RING_CAP);
    }
    R[idx].dt = dt; R[idx].price = price; R[idx].vol = vol; R[idx].side = side;
}

// Load the history file into the ledger (oldest entries beyond cutoff are
// skipped), then rewrite it compacted so it doesn't grow without bound.
static void BTFileLoad(SCStudyInterfaceRef sc, double cutoffDt)
{
    SCString path;
    BTFilePath(sc, path);
    FILE *f = fopen(path.GetChars(), "r");
    if (f == NULL)
        return;
    double dt; float px, vol; int side;
    while (fscanf(f, "%lf,%f,%f,%d", &dt, &px, &vol, &side) == 4)
    {
        if (dt >= cutoffDt && (side == 0 || side == 1) && vol > 0.f)
            BTRingAppend(sc, dt, px, vol, side);
    }
    fclose(f);

    // Rewrite compacted from the ledger.
    FILE *w = fopen(path.GetChars(), "w");
    if (w == NULL)
        return;
    const BTTrade *R = BTRing(sc);
    const int count = sc.GetPersistentInt(PI_RCOUNT);
    const int head  = sc.GetPersistentInt(PI_RHEAD);
    for (int q = 0; q < count; ++q)
    {
        const BTTrade &t = R[(head + q) % RING_CAP];
        fprintf(w, "%.10f,%.4f,%.0f,%d\n", t.dt, t.price, t.vol, t.side);
    }
    fclose(w);
}

// Last bar whose start time is <= dt (manual binary search; see build notes).
static int BTBarIndexForTime(SCStudyInterfaceRef sc, double dt)
{
    const int last = sc.ArraySize - 1;
    if (last < 0)
        return -1;
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
    return lo;
}

// Is the trade's time of day inside the RTH window (window may wrap midnight)?
static bool BTInRTH(double dt, int s, int e)
{
    const SCDateTime d(dt);
    const int tod = d.GetTimeInSeconds();
    if (s <= e) return (tod >= s && tod <= e);
    return (tod >= s || tod <= e);
}

// Effective min size for a trade at dt under the session settings.
// Returns -1 when the trade's session is filtered out entirely.
static float BTEffMinSize(const BTCfg &c, double dt)
{
    const bool rth = BTInRTH(dt, c.rthStart, c.rthEnd);
    if (c.sessFilt == 1 && !rth) return -1.f;
    if (c.sessFilt == 2 && rth)  return -1.f;
    if (!rth && c.globexMin > 0.f) return c.globexMin;
    return c.minSize;
}

// -----------------------------------------------------------------------------
// Text label anchored to a bar index (the index comes from BTBarIndexForTime,
// so it is recomputed correctly on every redraw).
static void BTLabel(SCStudyInterfaceRef sc, int lineNumber, bool show,
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

// Commit an aggregated trade as a bubble on the bar containing dtFirst.
// Keeps the largest maxSlots trades per side per bar (replaces the smallest).
static void BTCommit(SCStudyInterfaceRef sc, double dtFirst, float vwap,
                     float vol, int side, const BTCfg &c, bool record)
{
    int bi;
    if (record)
    {
        // LIVE commit: the trade belongs to the forming bar. Anchor the ledger
        // entry to that bar's own start time (chart time zone by definition) —
        // raw T&S timestamps are in a different time zone and would re-map to
        // the current bar on every redraw.
        bi = sc.ArraySize - 1;
        if (bi < 0)
            return;
        dtFirst = sc.BaseDateTimeIn[bi].GetAsDouble();
        BTRingAppend(sc, dtFirst, vwap, vol, side);
        if (c.keepDays > 0)
            BTFileAppend(sc, dtFirst, vwap, vol, side);
    }
    else
    {
        bi = BTBarIndexForTime(sc, dtFirst);
        if (bi < 0)
            return;
    }

    const int caN = (side == 0) ? CA_NBUY : CA_NSELL;
    int n = (int)sc.Subgraph[SG_CACHE].Arrays[caN][bi];

    bool isNew = false;
    int slot = -1;
    if (n < c.maxSlots)
    {
        slot = n;
        isNew = true;
        sc.Subgraph[SG_CACHE].Arrays[caN][bi] = (float)(n + 1);
    }
    else
    {
        // Replace the smallest committed trade if this one is bigger.
        int   minK = 0;
        float minV = sc.Subgraph[BubSG(side, 0)].Arrays[1][bi];
        for (int k = 1; k < n; ++k)
        {
            const float v = sc.Subgraph[BubSG(side, k)].Arrays[1][bi];
            if (v < minV) { minV = v; minK = k; }
        }
        if (vol > minV)
            slot = minK;
    }
    if (slot < 0)
        return;

    if (isNew)
        sc.SetPersistentInt(PI_TOTAL, sc.GetPersistentInt(PI_TOTAL) + 1);

    const int sg = BubSG(side, slot);
    sc.Subgraph[sg][bi]           = vwap;
    sc.Subgraph[sg].Arrays[1][bi] = vol;
    sc.Subgraph[sg].DataColor[bi] = sc.Subgraph[sg].PrimaryColor;
    // Arrays[0] (pixel size) is filled by the view-relative sizing pass.

    if (c.labelMode == 1)
    {
        SCString t;
        t.Format("%d", (int)(vol + 0.5f));
        BTLabel(sc, (90000000 + (bi * 2 + side) * SLOTS + slot), true,
                bi, vwap, t, c.labelCol, c.labelSize);
    }

    if (c.alertMin > 0.f && vol >= c.alertMin && !sc.IsFullRecalculation && record)
    {
        SCString msg;
        msg.Format("BigTradesTape: %s %d @ %.2f",
                   (side == 0) ? "BUY" : "SELL", (int)(vol + 0.5f), vwap);
        sc.SetAlert(c.alertSnd, msg);
    }
}

// Flush the pending aggregate (if any): commit when it meets its session's
// minimum size and passes the session filter, then clear it.
static void BTFlush(SCStudyInterfaceRef sc, const BTCfg &c)
{
    if (sc.GetPersistentInt(PI_PENDSIDE) < 0)
        return;
    const double dVol = sc.GetPersistentDouble(PD_VOL);
    const double dtF  = sc.GetPersistentDouble(PD_FIRSTDT);
    const float  eff  = BTEffMinSize(c, dtF);
    if (eff > 0.f && dVol >= (double)eff && dVol > 0.0)
    {
        const float vwap = (float)(sc.GetPersistentDouble(PD_PV) / dVol);
        BTCommit(sc, dtF, vwap, (float)dVol,
                 sc.GetPersistentInt(PI_PENDSIDE), c, true);
    }
    sc.SetPersistentInt(PI_PENDSIDE, -1);
    sc.SetPersistentDouble(PD_VOL, 0.0);
    sc.SetPersistentDouble(PD_PV, 0.0);
}

// Prune oldest bubbles until the displayed total is within the cap.
static void BTPrune(SCStudyInterfaceRef sc, int cap, int last)
{
    int total = sc.GetPersistentInt(PI_TOTAL);
    int ob    = sc.GetPersistentInt(PI_OLDBAR);
    if (ob < 0) ob = 0;

    while (total > cap && ob <= last)
    {
        for (int s = 0; s < 2; ++s)
        {
            const int caN = (s == 0) ? CA_NBUY : CA_NSELL;
            const int n = (int)sc.Subgraph[SG_CACHE].Arrays[caN][ob];
            for (int k = 0; k < n; ++k)
            {
                const int sg = BubSG(s, k);
                sc.Subgraph[sg][ob]           = 0.f;
                sc.Subgraph[sg].Arrays[0][ob] = 0.f;
                sc.Subgraph[sg].Arrays[1][ob] = 0.f;
                sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING,
                                         90000000 + (ob * 2 + s) * SLOTS + k);
            }
            sc.Subgraph[SG_CACHE].Arrays[caN][ob] = 0.f;
            total -= n;
        }
        ++ob;
    }

    sc.SetPersistentInt(PI_TOTAL, total);
    sc.SetPersistentInt(PI_OLDBAR, ob);
}

// =============================================================================
SCSFExport scsf_BigTradesTape(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Big Trades Tape (T&S Aggregated)";
        sc.GraphRegion      = 0;
        sc.AutoLoop         = 0;
        sc.ScaleRangeType   = SCALE_SAMEASREGION;
        sc.DrawZeros        = 0;
        sc.ValueFormat      = VALUEFORMAT_INHERITED;
        sc.UpdateAlways     = 1;   // needed for pending flush + view sizing

        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < SLOTS; ++k)
            {
                const int sg = BubSG(s, k);
                SCString nm;
                nm.Format("%s %d", (s == 0) ? "Buy" : "Sell", k + 1);
                sc.Subgraph[sg].Name         = nm;
                sc.Subgraph[sg].DrawStyle    = DRAWSTYLE_TRANSPARENT_CIRCLE_VARIABLE_SIZE;
                sc.Subgraph[sg].PrimaryColor = (s == 0) ? COL_BUY : COL_SELL;
                sc.Subgraph[sg].DrawZeros    = 0;
                sc.Subgraph[sg].AutoColoring = AUTOCOLOR_NONE;
            }

        sc.Subgraph[SG_CACHE].Name      = "Cache (internal)";
        sc.Subgraph[SG_CACHE].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[SG_CACHE].DrawZeros = 0;

        sc.Input[IN_AGGMS].Name = "Aggregation Period (ms, 0=raw prints)";
        sc.Input[IN_AGGMS].SetInt(250);
        sc.Input[IN_AGGMS].SetIntLimits(0, 60000);

        sc.Input[IN_AGGGAP].Name = "Aggregation Max Tick Gap";
        sc.Input[IN_AGGGAP].SetInt(2);
        sc.Input[IN_AGGGAP].SetIntLimits(0, 100);

        sc.Input[IN_MINSIZE].Name = "Min Aggregated Size (RTH)";
        sc.Input[IN_MINSIZE].SetFloat(100.0f);
        sc.Input[IN_MINSIZE].SetFloatLimits(1.0f, 100000000.0f);

        sc.Input[IN_MAXBUB].Name = "Max Bubbles Per Side Per Bar";
        sc.Input[IN_MAXBUB].SetInt(10);
        sc.Input[IN_MAXBUB].SetIntLimits(1, SLOTS);

        sc.Input[IN_SIZEMIN].Name = "Bubble Size Min (px)";
        sc.Input[IN_SIZEMIN].SetInt(4);
        sc.Input[IN_SIZEMIN].SetIntLimits(1, 200);

        sc.Input[IN_SIZEMAX].Name = "Bubble Size Max (px, largest in view)";
        sc.Input[IN_SIZEMAX].SetInt(22);
        sc.Input[IN_SIZEMAX].SetIntLimits(1, 200);

        sc.Input[IN_SCALING].Name = "Size Scaling";
        sc.Input[IN_SCALING].SetCustomInputStrings("Linear;Square Root");
        sc.Input[IN_SCALING].SetCustomInputIndex(1);

        sc.Input[IN_LABEL].Name = "Bubble Label";
        sc.Input[IN_LABEL].SetCustomInputStrings("Off;Volume");
        sc.Input[IN_LABEL].SetCustomInputIndex(1);

        sc.Input[IN_LABELSIZE].Name = "Label Font Size";
        sc.Input[IN_LABELSIZE].SetInt(8);
        sc.Input[IN_LABELSIZE].SetIntLimits(4, 72);

        sc.Input[IN_LABELCOL].Name = "Label Color";
        sc.Input[IN_LABELCOL].SetColor(RGB(255, 255, 255));

        sc.Input[IN_ALERTMIN].Name = "Alert Min Size (0=off)";
        sc.Input[IN_ALERTMIN].SetFloat(0.0f);
        sc.Input[IN_ALERTMIN].SetFloatLimits(0.0f, 100000000.0f);

        sc.Input[IN_ALERTSND].Name = "Alert Sound Number";
        sc.Input[IN_ALERTSND].SetInt(1);
        sc.Input[IN_ALERTSND].SetIntLimits(0, 100);

        sc.Input[IN_MAXTOTAL].Name = "Max Bubbles Displayed (most recent)";
        sc.Input[IN_MAXTOTAL].SetInt(1000);
        sc.Input[IN_MAXTOTAL].SetIntLimits(10, 1000000);

        sc.Input[IN_DEBUG].Name = "Debug Log (diagnostics to Message Log)";
        sc.Input[IN_DEBUG].SetCustomInputStrings("Off;On");
        sc.Input[IN_DEBUG].SetCustomInputIndex(0);

        sc.Input[IN_SESSFILT].Name = "Session Filter";
        sc.Input[IN_SESSFILT].SetCustomInputStrings("All;RTH Only;Globex Only");
        sc.Input[IN_SESSFILT].SetCustomInputIndex(0);

        sc.Input[IN_RTHSTART].Name = "RTH Start Time";
        sc.Input[IN_RTHSTART].SetTime(HMS_TIME(9, 30, 0));

        sc.Input[IN_RTHEND].Name = "RTH End Time";
        sc.Input[IN_RTHEND].SetTime(HMS_TIME(16, 14, 59));

        sc.Input[IN_GLOBEXMIN].Name = "Globex Min Size (0=use RTH min)";
        sc.Input[IN_GLOBEXMIN].SetFloat(0.0f);
        sc.Input[IN_GLOBEXMIN].SetFloatLimits(0.0f, 100000000.0f);

        sc.Input[IN_KEEPDAYS].Name = "History File Keep (days, 0=off)";
        sc.Input[IN_KEEPDAYS].SetInt(5);
        sc.Input[IN_KEEPDAYS].SetIntLimits(0, 365);

        return;
    }

    if (sc.LastCallToFunction)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_ALL, 0);
        BTTrade *p = (BTTrade*)sc.GetPersistentPointer(1);
        if (p != NULL)
        {
            delete[] p;
            sc.SetPersistentPointer(1, NULL);
        }
        return;
    }

    const int last = sc.ArraySize - 1;
    if (last < 0)
        return;

    BTCfg cfg;
    const int aggMs   = sc.Input[IN_AGGMS].GetInt();
    const int aggGap  = sc.Input[IN_AGGGAP].GetInt();
    cfg.minSize   = sc.Input[IN_MINSIZE].GetFloat();
    cfg.globexMin = sc.Input[IN_GLOBEXMIN].GetFloat();
    cfg.sessFilt  = sc.Input[IN_SESSFILT].GetIndex();
    cfg.rthStart  = sc.Input[IN_RTHSTART].GetTime();
    cfg.rthEnd    = sc.Input[IN_RTHEND].GetTime();
    cfg.maxSlots  = sc.Input[IN_MAXBUB].GetInt();
    if (cfg.maxSlots < 1) cfg.maxSlots = 1;
    if (cfg.maxSlots > SLOTS) cfg.maxSlots = SLOTS;
    cfg.labelMode = sc.Input[IN_LABEL].GetIndex();
    cfg.labelSize = sc.Input[IN_LABELSIZE].GetInt();
    cfg.labelCol  = sc.Input[IN_LABELCOL].GetColor();
    cfg.alertMin  = sc.Input[IN_ALERTMIN].GetFloat();
    cfg.alertSnd  = sc.Input[IN_ALERTSND].GetInt();
    cfg.keepDays  = sc.Input[IN_KEEPDAYS].GetInt();

    // Load the history file once per SC session (before the redraw below).
    if (cfg.keepDays > 0 && sc.GetPersistentInt(PI_LOADED) == 0)
    {
        sc.SetPersistentInt(PI_LOADED, 1);
        const double cutoff = sc.BaseDateTimeIn[last].GetAsDouble() - (double)cfg.keepDays;
        BTFileLoad(sc, cutoff);
    }

    const float    sizeMin   = (float)sc.Input[IN_SIZEMIN].GetInt();
    const float    sizeMax   = (float)sc.Input[IN_SIZEMAX].GetInt();
    const int      scaling   = sc.Input[IN_SCALING].GetIndex();
    const int      maxTotal  = sc.Input[IN_MAXTOTAL].GetInt();
    const bool     debugOn   = (sc.Input[IN_DEBUG].GetIndex() == 1);

    // ---- Full recalculation: wipe drawings, REDRAW from the persistent ledger -
    // PI_SEQ is intentionally NOT reset: already-processed T&S records stay
    // consumed; the ledger is the source of truth for committed bubbles, so
    // they survive settings changes and chart reloads until SC closes.
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_ALL, 0);
        sc.SetPersistentInt(PI_TOTAL, 0);
        sc.SetPersistentInt(PI_OLDBAR, 0);
        // Subgraph arrays (committed bubbles + counts) were cleared by SC.

        const BTTrade *R = BTRing(sc);
        const int count = sc.GetPersistentInt(PI_RCOUNT);
        const int head  = sc.GetPersistentInt(PI_RHEAD);
        for (int q = 0; q < count; ++q)
        {
            const BTTrade &t = R[(head + q) % RING_CAP];
            const float eff = BTEffMinSize(cfg, t.dt);
            if (eff <= 0.f || t.vol < eff)
                continue;   // respect session filter / raised threshold
            BTCommit(sc, t.dt, t.price, t.vol, t.side, cfg, false);
        }
    }

    // =========================================================================
    // 1) Process new Time & Sales records into aggregates.
    // =========================================================================
    c_SCTimeAndSalesArray TS;
    sc.GetTimeAndSales(TS);
    const int tsSize = TS.Size();

    unsigned int lastSeq = (unsigned int)sc.GetPersistentInt(PI_SEQ);
    const double gapDays = (double)aggMs / 86400000.0;
    const float  tick    = (sc.TickSize > 0.f) ? sc.TickSize : 0.25f;

    // Diagnostics (logged below when Debug Log is On)
    int  dbgRead = 0, dbgTrades = 0, dbgSkipType = 0;
    float dbgMaxVol = 0.f;
    bool  dbgHaveSample = false;
    float dbgPx = 0.f, dbgVol = 0.f;
    int   dbgType = -1;

    for (int x = 0; x < tsSize; ++x)
    {
        const s_TimeAndSales &r = TS[x];
        if (r.Sequence == 0 || r.Sequence <= lastSeq)
            continue;
        lastSeq = r.Sequence;
        ++dbgRead;

        if (r.Type != SC_TS_BID && r.Type != SC_TS_ASK)
            { ++dbgSkipType; continue; }
        if (r.Volume == 0)
            continue;

        const float price = r.Price * sc.RealTimePriceMultiplier;
        const float vol   = (float)r.Volume;
        const int   side  = (r.Type == SC_TS_ASK) ? 0 : 1;   // 0 buy, 1 sell
        const double dt   = r.DateTime.GetAsDouble();

        ++dbgTrades;
        if (vol > dbgMaxVol) dbgMaxVol = vol;
        if (!dbgHaveSample)
        {
            dbgHaveSample = true;
            dbgPx = price; dbgVol = vol; dbgType = (int)r.Type;
        }

        const int pendSide = sc.GetPersistentInt(PI_PENDSIDE);
        bool merged = false;

        if (aggMs > 0 && pendSide == side)
        {
            const double dGap = dt - sc.GetPersistentDouble(PD_LASTDT);
            float pGap = price - (float)sc.GetPersistentDouble(PD_LASTPX);
            if (pGap < 0.f) pGap = -pGap;
            if (dGap >= 0.0 && dGap <= gapDays && pGap <= (float)aggGap * tick + tick * 0.01f)
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
            // Close out the previous aggregate, then start a new one.
            BTFlush(sc, cfg);
            sc.SetPersistentInt(PI_PENDSIDE, side);
            sc.SetPersistentDouble(PD_VOL, vol);
            sc.SetPersistentDouble(PD_PV, (double)price * vol);
            sc.SetPersistentDouble(PD_FIRSTDT, dt);
            sc.SetPersistentDouble(PD_LASTDT, dt);
            sc.SetPersistentDouble(PD_LASTPX, price);
        }
    }
    sc.SetPersistentInt(PI_SEQ, (int)lastSeq);

    // Flush a pending aggregate that has gone stale (no follow-up prints).
    if (sc.GetPersistentInt(PI_PENDSIDE) >= 0)
    {
        const double idleDays = sc.CurrentSystemDateTime.GetAsDouble()
                              - sc.GetPersistentDouble(PD_LASTDT);
        if (idleDays > gapDays * 4.0 + (1.0 / 86400.0))   // gap*4 + 1s tolerance
            BTFlush(sc, cfg);
    }

    // Diagnostics — ALWAYS logged when Debug Log is On (even with zero reads).
    if (debugOn)
    {
        SCString m;
        m.Format("BigTradesTape dbg: tsArray=%d newRecs=%d trades=%d skipType=%d "
                 "maxTrade=%.0f displayed=%d ledger=%d | sample px=%.2f vol=%.0f type=%d",
                 tsSize, dbgRead, dbgTrades, dbgSkipType, dbgMaxVol,
                 sc.GetPersistentInt(PI_TOTAL), sc.GetPersistentInt(PI_RCOUNT),
                 dbgPx, dbgVol, dbgType);
        sc.AddMessageToLog(m, 0);
    }

    // =========================================================================
    // 2) Display cap: prune oldest bubbles beyond Max Bubbles Displayed.
    // =========================================================================
    BTPrune(sc, maxTotal, last);

    // =========================================================================
    // 3) View-relative sizing: largest aggregate among VISIBLE bars sets the
    //    scale; every visible bubble is sized against it.
    // =========================================================================
    int vFirst = sc.IndexOfFirstVisibleBar;
    int vLast  = sc.IndexOfLastVisibleBar;
    if (vFirst < 0) vFirst = 0;
    if (vLast > last || vLast < 0) vLast = last;
    if (vFirst > vLast) { vFirst = 0; vLast = last; }

    float maxVis = 0.f;
    for (int i = vFirst; i <= vLast; ++i)
        for (int s = 0; s < 2; ++s)
        {
            const int n = (int)sc.Subgraph[SG_CACHE].Arrays[(s == 0) ? CA_NBUY : CA_NSELL][i];
            for (int k = 0; k < n; ++k)
            {
                const float v = sc.Subgraph[BubSG(s, k)].Arrays[1][i];
                if (v > maxVis) maxVis = v;
            }
        }

    if (maxVis > 0.f)
    {
        for (int i = vFirst; i <= vLast; ++i)
            for (int s = 0; s < 2; ++s)
            {
                const int n = (int)sc.Subgraph[SG_CACHE].Arrays[(s == 0) ? CA_NBUY : CA_NSELL][i];
                for (int k = 0; k < n; ++k)
                {
                    const int sg = BubSG(s, k);
                    const float v = sc.Subgraph[sg].Arrays[1][i];
                    if (v <= 0.f)
                        continue;
                    float f = v / maxVis;
                    if (scaling == 1)
                        f = sqrt(f);
                    sc.Subgraph[sg].Arrays[0][i] = sizeMin + f * (sizeMax - sizeMin);
                }
            }
    }
}
