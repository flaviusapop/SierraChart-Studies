// =============================================================================
// MTFCloseFilter.cpp  —  Sierra Chart ACSIL Custom Study
// Multi-TimeFrame OTF Close Filter · Status Bar
// =============================================================================
//
// Draws a six-cell status bar at the bottom of the price chart — one cell per
// configured timeframe (1m · 2m · 5m · 30m · 60m · 120m).
//
// ─── OTF CLOSE FILTER RULES ───────────────────────────────────────────────────
// Each bar: find the highest and lowest CLOSE over the previous N bars.
// Window = [bar-1 … bar-N]  (current bar is excluded; wicks are never used).
//
//   Mode 0 — Breakout (strict)
//     UP      : close > highest close of prev-N bars
//     DOWN    : close < lowest  close of prev-N bars
//     Neutral : inside the range
//
//   Mode 1 — Continuation (lenient)
//     Same thresholds as Mode 0; produces identical zones.
//
//   Mode 2 — Stateful (no neutral once a state is established)
//     Breakout flips to UP/DOWN.
//     Close inside the range → carry the prior state forward.
//     Neutral only at the very start, before the first breakout.
//
//   Output values: +1 = Up · -1 = Down · 0 = Neutral
//
// ─── TUNING N ─────────────────────────────────────────────────────────────────
//   N = 1        compare to previous close; most reactive
//   N = 3–5      tolerates brief pullbacks
//   N = 8–12     higher-timeframe smoothness
//   N = 15+      structural; breaks only on genuine trend changes
//
// ─── VISUAL ───────────────────────────────────────────────────────────────────
//   Green  (0, 200, 80)   = UP
//   Red    (200, 50, 50)  = DOWN
//   Gray   (90, 90, 90)   = Neutral
//   Dark background rectangle behind the entire row.
//   A transparent-look is achieved by keeping the background very dark
//   (adjustable via the Bottom-offset and Height inputs).
//
// ─── SETUP ────────────────────────────────────────────────────────────────────
//   1. Open six chart windows with the desired timeframes.
//   2. Check each chart's number (Window menu ▸ Chart Numbers, or the chart
//      tab / title bar).
//   3. Enter those six chart numbers in the study inputs.
//   4. Add only ONE instance of this study per chart.
//      (Drawing line slots 11000–11015 are shared within a chart.)
//
// ─── INPUTS ───────────────────────────────────────────────────────────────────
//   [0]  Mode                0 / 1 / 2
//   [1]  N lookback LTF      applied to 1m · 2m · 5m charts
//   [2]  N lookback HTF      applied to 30m · 60m · 120m charts
//   [3]  Chart # — 1-min
//   [4]  Chart # — 2-min
//   [5]  Chart # — 5-min
//   [6]  Chart # — 30-min
//   [7]  Chart # — 60-min
//   [8]  Chart # — 120-min
//   [9]  Bar height  (% of visible price range, default 5.5)
//   [10] Bottom offset (% of visible price range, default 0.5)
//
// ─── SUBGRAPHS (DRAWSTYLE_IGNORE) ─────────────────────────────────────────────
//   [0]  State 1m    [1] State 2m    [2] State 5m
//   [3]  State 30m   [4] State 60m   [5] State 120m
//   Values: +1.0 = Up · 0.0 = Neutral · -1.0 = Down
//   Use these for alert conditions or cross-study references.
//
// ─── DRAWING LINE NUMBERS RESERVED ───────────────────────────────────────────
//   11000          background rectangle
//   11001–11006    per-TF colored boxes
//   11010–11015    per-TF text labels
// =============================================================================

#include "sierrachart.h"

SCDLLName("MTFCloseFilter")

// ─── Color constants ──────────────────────────────────────────────────────────
static const COLORREF kColorUp      = RGB(  0, 200,  80);
static const COLORREF kColorDown    = RGB(200,  50,  50);
static const COLORREF kColorNeutral = RGB( 90,  90,  90);
static const COLORREF kColorBg      = RGB( 18,  18,  18);
static const COLORREF kColorText    = RGB(255, 255, 255);

// ─── Drawing line-number slots ────────────────────────────────────────────────
static const int kLineBg    = 11000;   // background rectangle
static const int kLineTF    = 11001;   // colored boxes  11001–11006
static const int kLineLabel = 11010;   // text labels    11010–11015

// ─── TF display labels ───────────────────────────────────────────────────────
static const char* const kTFLabel[6] = { "1m", "2m", "5m", "30m", "60m", "120m" };

// ─── Mode-2 performance cap ───────────────────────────────────────────────────
// In Stateful mode the full bar history must be scanned to carry the state
// forward.  For very long charts we cap the scan at the most-recent
// kMaxScanBars bars.  At N=5, 10 000 bars ≈ ~28 trading days of 1-min data,
// which is more than enough to establish a valid state.
static const int kMaxScanBars = 10000;

// =============================================================================
// ComputeCurrentState
//   Returns +1, -1, or 0 for the last bar in the supplied close array.
//   Precondition: n > N  (checked by caller before entry).
// =============================================================================
static int ComputeCurrentState(SCFloatArrayRef cl, int n, int N, int mode)
{
    if (mode < 2)
    {
        // ── Modes 0 & 1: simple one-shot comparison at the last bar ──────────
        // Window: [last-N … last-1]  (N bars, current bar excluded)
        const int last = n - 1;
        float hi = cl[last - N];
        float lo = cl[last - N];
        for (int j = last - N + 1; j < last; ++j)
        {
            if (cl[j] > hi) hi = cl[j];
            if (cl[j] < lo) lo = cl[j];
        }
        const float c = cl[last];
        if (c > hi) return  1;
        if (c < lo) return -1;
        return 0;
    }
    else
    {
        // ── Mode 2: stateful carry-forward scan ───────────────────────────────
        // Start from max(N, n - kMaxScanBars) so we always begin at a valid
        // bar (index >= N) while capping the work on very long charts.
        const int startBar = (n > kMaxScanBars) ? (n - kMaxScanBars) : N;
        int state = 0;

        for (int i = startBar; i < n; ++i)
        {
            // Window for bar i: [i-N … i-1]
            float hi = cl[i - N];
            float lo = cl[i - N];
            for (int j = i - N + 1; j < i; ++j)
            {
                if (cl[j] > hi) hi = cl[j];
                if (cl[j] < lo) lo = cl[j];
            }
            const float c = cl[i];
            if      (c > hi) state =  1;
            else if (c < lo) state = -1;
            // inside range → keep previous state (mode 2 — no neutral)
        }
        return state;
    }
}

// =============================================================================
// GetTFState
//   Pulls close data from the specified chart via sc.GetChartBaseData() and
//   returns the current OTF state (+1 / -1 / 0).
// =============================================================================
static int GetTFState(SCStudyInterfaceRef sc, int chartNum, int N, int mode)
{
    if (chartNum <= 0) return 0;

    SCGraphData data;
    sc.GetChartBaseData(chartNum, data);

    SCFloatArrayRef cl = data[SC_LAST];   // SC_LAST == close
    const int n = cl.GetArraySize();
    if (n <= N) return 0;                 // insufficient history

    return ComputeCurrentState(cl, n, N, mode);
}

// =============================================================================
// DrawStatusBar
//   Draws the six-cell status bar at the bottom of the price chart via
//   sc.UseTool().  Uses line-number replacement so every call refreshes the
//   same set of drawings without accumulation.
//
//   heightPct   — bar height as a % of the visible region range (e.g. 5.5)
//   bottomPct   — distance from the region bottom as a % (e.g. 0.5)
// =============================================================================
static void DrawStatusBar(SCStudyInterfaceRef sc,
                          const int   states[6],
                          float       heightPct,
                          float       bottomPct)
{
    const int last = sc.ArraySize - 1;
    if (last < 1) return;

    // ── Y: bottom band derived from the visible region ────────────────────────
    const float regHi  = (float)sc.RegionHighValue;
    const float regLo  = (float)sc.RegionLowValue;
    const float regRng = regHi - regLo;
    if (regRng <= 0.0f) return;

    const float boxH  = regRng * (heightPct * 0.01f);
    const float offY  = regRng * (bottomPct * 0.01f);
    const float bgBtm = regLo + offY;
    const float bgTop = bgBtm + boxH;
    const float midY  = (bgBtm + bgTop) * 0.5f;
    const float padY  = regRng * 0.003f;   // inner padding between cell edge and fill

    // ── X: ~10 bars wide, ending just before the last bar ────────────────────
    // Using doubles for SCDateTime arithmetic (SCDateTime is a double in ACSIL)
    const double t1 = (double)sc.BaseDateTimeIn[last];
    const double t0 = (double)sc.BaseDateTimeIn[last - 1];
    const double bw = t1 - t0;             // one-bar width in datetime units
    const double xL = t1 - bw * 9.6;      // left edge
    const double xR = t1 - bw * 0.2;      // right edge (just inside last bar)
    const double cw = (xR - xL) / 6.0;    // per-cell width

    // ── Background rectangle (dark, spans all six cells) ─────────────────────
    s_UseTool bg;
    bg.ChartNumber           = sc.ChartNumber;
    bg.DrawingType           = DRAWING_RECTANGLE_EX;
    bg.LineNumber            = kLineBg;
    bg.BeginDateTime         = xL;
    bg.EndDateTime           = xR;
    bg.BeginValue            = (double)bgBtm;
    bg.EndValue              = (double)bgTop;
    bg.Color                 = kColorBg;
    bg.Color2                = kColorBg;   // Color2 = fill colour for RECTANGLE_EX
    bg.LineWidth             = 0;
    bg.AddAsUserDrawnDrawing = 0;
    sc.UseTool(bg);

    // ── Six TF cells (coloured box + text label) ──────────────────────────────
    for (int i = 0; i < 6; ++i)
    {
        const double cL = xL + cw * i;
        const double cR = xL + cw * (i + 1) - bw * 0.05;   // small gap between cells

        const COLORREF fill = (states[i] ==  1) ? kColorUp
                            : (states[i] == -1) ? kColorDown
                                                 : kColorNeutral;

        // Coloured box
        s_UseTool box;
        box.ChartNumber           = sc.ChartNumber;
        box.DrawingType           = DRAWING_RECTANGLE_EX;
        box.LineNumber            = kLineTF + i;
        box.BeginDateTime         = cL;
        box.EndDateTime           = cR;
        box.BeginValue            = (double)(bgBtm + padY);
        box.EndValue              = (double)(bgTop  - padY);
        box.Color                 = fill;
        box.Color2                = fill;
        box.LineWidth             = 1;
        box.AddAsUserDrawnDrawing = 0;
        sc.UseTool(box);

        // Text label — centred horizontally in the cell
        s_UseTool lbl;
        lbl.ChartNumber           = sc.ChartNumber;
        lbl.DrawingType           = DRAWING_TEXT;
        lbl.LineNumber            = kLineLabel + i;
        lbl.BeginDateTime         = cL + cw * 0.5;
        lbl.BeginValue            = (double)midY;
        lbl.Color                 = kColorText;
        lbl.FontSize              = 9;
        lbl.Text                  = kTFLabel[i];
        lbl.AddAsUserDrawnDrawing = 0;
        sc.UseTool(lbl);
    }
}

// =============================================================================
// MAIN STUDY FUNCTION
// =============================================================================
SCSFExport scsf_MTFCloseFilterStatusBar(SCStudyInterfaceRef sc)
{
    // ── Subgraph references ───────────────────────────────────────────────────
    SCSubgraphRef sg_1m   = sc.Subgraph[0];
    SCSubgraphRef sg_2m   = sc.Subgraph[1];
    SCSubgraphRef sg_5m   = sc.Subgraph[2];
    SCSubgraphRef sg_30m  = sc.Subgraph[3];
    SCSubgraphRef sg_60m  = sc.Subgraph[4];
    SCSubgraphRef sg_120m = sc.Subgraph[5];

    // ── Input references ──────────────────────────────────────────────────────
    SCInputRef in_Mode      = sc.Input[0];
    SCInputRef in_N_Lo      = sc.Input[1];
    SCInputRef in_N_Hi      = sc.Input[2];
    SCInputRef in_C1m       = sc.Input[3];
    SCInputRef in_C2m       = sc.Input[4];
    SCInputRef in_C5m       = sc.Input[5];
    SCInputRef in_C30m      = sc.Input[6];
    SCInputRef in_C60m      = sc.Input[7];
    SCInputRef in_C120m     = sc.Input[8];
    SCInputRef in_HeightPct = sc.Input[9];
    SCInputRef in_BotPct    = sc.Input[10];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "MTF OTF Close Filter";
        sc.StudyDescription =
            "Multi-TimeFrame OTF Close Filter Status Bar. "
            "Shows UP / DOWN / Neutral state for six configured timeframe charts. "
            "Mode 0 = Breakout, Mode 1 = Continuation (same zones), "
            "Mode 2 = Stateful (no neutral once a state is established). "
            "Requires one open chart window per timeframe.";

        sc.AutoLoop     = 0;    // manual — cross-chart state runs outside autoloop
        sc.GraphRegion  = 0;    // overlay on the price chart
        sc.UpdateAlways = 1;    // redraw every tick so bar stays current
        sc.FreeDLL      = 0;
        sc.DrawZeros    = 0;

        // ── Subgraphs — hidden, exposed for alerts / cross-study use ─────────
        sg_1m.Name   = "State 1m";   sg_1m.DrawStyle   = DRAWSTYLE_IGNORE;
        sg_2m.Name   = "State 2m";   sg_2m.DrawStyle   = DRAWSTYLE_IGNORE;
        sg_5m.Name   = "State 5m";   sg_5m.DrawStyle   = DRAWSTYLE_IGNORE;
        sg_30m.Name  = "State 30m";  sg_30m.DrawStyle  = DRAWSTYLE_IGNORE;
        sg_60m.Name  = "State 60m";  sg_60m.DrawStyle  = DRAWSTYLE_IGNORE;
        sg_120m.Name = "State 120m"; sg_120m.DrawStyle = DRAWSTYLE_IGNORE;

        // ── Mode ─────────────────────────────────────────────────────────────
        in_Mode.Name = "Mode  (0 Breakout  ·  1 Continuation  ·  2 Stateful)";
        in_Mode.SetInt(2);
        in_Mode.SetIntLimits(0, 2);

        // ── N lookback ───────────────────────────────────────────────────────
        in_N_Lo.Name = "N lookback  —  1m / 2m / 5m";
        in_N_Lo.SetInt(3);
        in_N_Lo.SetIntLimits(1, 500);

        in_N_Hi.Name = "N lookback  —  30m / 60m / 120m";
        in_N_Hi.SetInt(5);
        in_N_Hi.SetIntLimits(1, 500);

        // ── Chart numbers ────────────────────────────────────────────────────
        in_C1m.Name   = "Chart #  —  1-min";   in_C1m.SetChartNumber(2);
        in_C2m.Name   = "Chart #  —  2-min";   in_C2m.SetChartNumber(3);
        in_C5m.Name   = "Chart #  —  5-min";   in_C5m.SetChartNumber(4);
        in_C30m.Name  = "Chart #  —  30-min";  in_C30m.SetChartNumber(5);
        in_C60m.Name  = "Chart #  —  60-min";  in_C60m.SetChartNumber(6);
        in_C120m.Name = "Chart #  —  120-min"; in_C120m.SetChartNumber(7);

        // ── Visual sizing ────────────────────────────────────────────────────
        in_HeightPct.Name = "Bar height  (% of visible range,  default 5.5)";
        in_HeightPct.SetFloat(5.5f);
        in_HeightPct.SetFloatLimits(1.0f, 25.0f);

        in_BotPct.Name = "Bottom offset  (% of visible range,  default 0.5)";
        in_BotPct.SetFloat(0.5f);
        in_BotPct.SetFloatLimits(0.0f, 15.0f);

        return;
    }

    // =========================================================================
    // COMPUTE STATES FOR ALL SIX TIMEFRAMES
    // =========================================================================
    const int mode = in_Mode.GetInt();
    const int N_Lo = in_N_Lo.GetInt();
    const int N_Hi = in_N_Hi.GetInt();

    int states[6];
    states[0] = GetTFState(sc, in_C1m.GetChartNumber(),   N_Lo, mode);
    states[1] = GetTFState(sc, in_C2m.GetChartNumber(),   N_Lo, mode);
    states[2] = GetTFState(sc, in_C5m.GetChartNumber(),   N_Lo, mode);
    states[3] = GetTFState(sc, in_C30m.GetChartNumber(),  N_Hi, mode);
    states[4] = GetTFState(sc, in_C60m.GetChartNumber(),  N_Hi, mode);
    states[5] = GetTFState(sc, in_C120m.GetChartNumber(), N_Hi, mode);

    // ── Publish states at the last bar (for alerts / cross-study references) ──
    const int last = sc.ArraySize - 1;
    if (last >= 0)
    {
        sg_1m[last]   = (float)states[0];
        sg_2m[last]   = (float)states[1];
        sg_5m[last]   = (float)states[2];
        sg_30m[last]  = (float)states[3];
        sg_60m[last]  = (float)states[4];
        sg_120m[last] = (float)states[5];
    }

    // =========================================================================
    // DRAW STATUS BAR
    // =========================================================================
    DrawStatusBar(sc, states, in_HeightPct.GetFloat(), in_BotPct.GetFloat());
}
