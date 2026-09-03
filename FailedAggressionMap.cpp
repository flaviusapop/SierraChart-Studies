// =============================================================================
// FailedAggressionMap.cpp
// Sierra Chart ACSIL Custom Study — Failed Aggression Map v1.0
//
// PURPOSE:
//   Detect directional aggression at bar/window extremes, confirm that price did
//   not reward it and displaced against it, then visualize the inferred failed
//   inventory as a volume-scaled bubble at its delta-weighted centroid plus a
//   thin horizontal ribbon. Standalone study — not Trapped Traders v2.
//
// EPISTEMIC LIMIT:
//   VAP shows executed bid/ask volume, not trader identity or open positions.
//   "Failed buyers/sellers" is an inference from strong directional aggression
//   followed by poor price reward and adverse displacement. The ribbon is drawn
//   and labeled as INFERRED inventory, not exact trader break-even.
//
// EVIDENCE STATES (kept separate, never conflated):
//   ATTEMPT (0) -> FAILURE_CONFIRMED (1) -> FIRST_RETEST (2)
//             -> RETEST_FAILED (3) or ACCEPTED_THROUGH (4); DISCARDED (5) pruned.
//   A raw footprint cluster is an ATTEMPT, not rejection.
//   A touch is a FIRST_RETEST, not a failed retest.
//
// DETECTION (closed bars only, integer-tick domain):
//   Rolling window of Detection Lookback Bars; per-tick Ask-Bid delta aggregated
//   with int64 math; high side = positive-delta levels within Proximity Ticks of
//   the rolling high (>= Minimum Blocks levels, cluster total >= zone threshold).
//   Low side mirror with negative delta. Centroid = delta-weighted tick mean.
//
// CONFIRMATION / RETEST / ACCEPTANCE (closed bars only, integer ticks):
//   High-side ATTEMPT confirms when a close within Confirmation Bars lands
//   Adverse Displacement Ticks below the cluster bottom (low-side mirror above
//   the cluster top); otherwise discarded at the deadline. First retest must
//   approach from the displaced side and overlap the cluster range. Retest fails
//   only on exit back in the displacement direction by Retest Exit Ticks.
//   Acceptance-through = closed bar beyond the FAR cluster edge + Acceptance
//   Ticks; terminates the ribbon and marks X. Checked before retest-exit.
//
// THRESHOLD MODES: Manual (fixed 95/350 defaults), Auto (median of daily
//   percentile values over 5 previous complete Sierra trading days, cached once
//   per day, no lookahead; visible warmup warning when history is short), Both
//   (independent detectors, Manual precedence on inclusive-tick intersection).
//
// MAPPED CONFLUENCE: optional Study-Subgraph input (disabled by default).
//   Nonzero value on the detection bar adds a gold halo + SG flag only; it NEVER
//   suppresses raw detections in v1.0. Uses SetChartStudySubgraphValues per the
//   task; if the target SC version does not expose that symbol, the one-line
//   fallback is SetStudySubgraphValues(0, 0) (read path is compatible).
//
// ALERTS (optional, off by default): live-only watermark, one alert per
//   transition type per bar, anchored to the current bar. Failure-confirm and
//   failed-retest alert; acceptance-through intentionally has no alert.
//
// RENDERING MODES: 0 = Bubble + Ribbon (default), 1 = Lifecycle Glyphs,
//   2 = Legacy Rectangle, 3 = Bubble + Ribbon + Rectangle.
//
// INPUTS (In:N label = index + 1):
//   In:1  [0]  Detection Lookback Bars        (structural)
//   In:2  [1]  Proximity Ticks                (structural)
//   In:3  [2]  Minimum Blocks                 (structural)
//   In:4  [3]  Manual Block Delta             (structural)
//   In:5  [4]  Manual Cluster Delta           (structural)
//   In:6  [5]  Confirmation Bars              (structural)
//   In:7  [6]  Adverse Displacement Ticks     (structural)
//   In:8  [7]  Retest Exit Ticks              (structural)
//   In:9  [8]  Acceptance Ticks               (structural)
//   In:10 [9]  Threshold Mode                 (structural)
//   In:11 [10] Auto Days                      (structural)
//   In:12 [11] Auto Block Percentile          (structural)
//   In:13 [12] Auto Zone Percentile           (structural)
//   In:14 [13] Mapped Location Source         (structural enable/disable only)
//   In:15 [14] Rendering Mode                 (structural)
//   In:16 [15] Bubble Min Radius (ticks)      (display)
//   In:17 [16] Bubble Max Radius (ticks)      (display)
//   In:18 [17] Max Events Drawn               (display/perf)
//   In:19 [18] Failed Buyers Color            (display)
//   In:20 [19] Failed Sellers Color           (display)
//   In:21 [20] Confluence/Halo Color          (display)
//   In:22 [21] Enable Alerts                  (display)
//   In:23 [22] Alert Sound Number             (display)
//
// SUBGRAPHS (14 total, all downstream-readable, stale-cleared):
//   SG0  Candidate Side            SG1  Failure Confirmed Side
//   SG2  Centroid Price            SG3  Cluster Top
//   SG4  Cluster Bottom            SG5  Normalized Strength 0-10
//   SG6  First Retest Pulse        SG7  Failed Retest Entry
//   SG8  Accepted Through          SG9  Mapped Confluence Flag
//   SG10 Threshold Source          SG11 Event ID
//   SG12 Failure Bubble (POINT)    SG13 Retest/Accept Marker (DIAMOND)
//
// PERSISTENT STATE (one heap struct, GetPersistentPointer slot 1):
//   Fixed-capacity array (MAX 128 events, no STL in persistent state), counters,
//   per-day Auto cache, settings fingerprint words, alert watermarks,
//   prevLastClosed for missed-bar processing. Freed + nulled on
//   LastCallToFunction after collapsing all drawings.
//   Persistent int slots are NOT used; everything lives in the struct.
//
// DRAWINGS: ordinary (non-user-drawn) sc.UseTool objects in one namespace;
//   deleted with sc.DeleteACSChartDrawing(CHART, TOOL_DELETE_CHARTDRAWING, N).
//   Bases: bubble 110000, halo 120000, ribbon 130000, ring 140000,
//   arrow-text 150000, X-text 160000, legacy rect 170000, label 180000,
//   warmup warning 199999. Line number = base + event ID.
//
// AutoLoop: 0 (manual loop; VAP reads + one study-array fetch per call)
// =============================================================================

#include "sierrachart.h"
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <climits>

using std::map;
using std::vector;

SCDLLName("FailedAggressionMap")

// =============================================================================
// Fixed-capacity persistent state (no STL containers in persistent state)
// =============================================================================

static const int FAM_MAX_EVENTS   = 128;
static const int FAM_MAX_DAYS     = 5;
static const int FAM_MIN_DAYBARS  = 5;
static const int FAM_PTR_SLOT     = 1;

// Event states — the evidence ladder, kept distinct.
static const int FAM_ATTEMPT      = 0;
static const int FAM_CONFIRMED    = 1;
static const int FAM_RETEST       = 2;
static const int FAM_FAILED       = 3;
static const int FAM_ACCEPTED     = 4;
static const int FAM_DISCARDED    = 5;

// Threshold sources.
static const int FAM_SRC_MANUAL   = 1;
static const int FAM_SRC_AUTO     = 2;

// Drawing line-number bases (line number = base + event ID).
static const int FAM_BASE_BUBBLE  = 110000;
static const int FAM_BASE_HALO    = 120000;
static const int FAM_BASE_RIBBON  = 130000;
static const int FAM_BASE_RING    = 140000;
static const int FAM_BASE_ARROW   = 150000;
static const int FAM_BASE_X       = 160000;
static const int FAM_BASE_RECT    = 170000;
static const int FAM_BASE_LABEL   = 180000;
static const int FAM_LINE_WARN    = 199999;

// Fixed Auto colors (distinct from the Manual input colors).
static const COLORREF FAM_AUTO_BUY  = RGB(255, 110, 40);
static const COLORREF FAM_AUTO_SELL = RGB(110, 150, 255);
static const COLORREF FAM_GRAY_X    = RGB(160, 160, 160);

struct FAMEvent
{
    int   Used;          // 0 = free slot, 1 = live record
    int   ID;            // stable sequential ID (deterministic under full recalc)
    int   Side;          // +1 = failed buyers (high side), -1 = failed sellers (low)
    int   Source;        // 1 = Manual, 2 = Auto
    int   State;         // FAM_* above
    int   DetectBar;     // window-end closed bar of detection
    int   ConfirmBar;    // closed bar of failure confirmation (-1 if none)
    int   RetestBar;     // closed bar of first retest (-1 if none)
    int   FailBar;       // closed bar of failed retest (-1 if none)
    int   AcceptBar;     // closed bar of acceptance-through (-1 if none)
    int   DeadlineBar;   // DetectBar + Confirmation Bars
    int   TopTick;       // cluster top, integer ticks
    int   BotTick;       // cluster bottom, integer ticks
    int   CentroidTick;  // delta-weighted centroid, integer ticks
    int64 TotalDelta;    // signed cluster delta sum (int64, overflow-safe)
    int64 TickWeight;    // sum(tick * |delta|) for centroid recompute on merge
    float Strength10;    // normalized 0.5..10
    int   MappedFlag;    // 1 if mapped-location confluence on detection bar
    int   RibbonEndBar;  // right edge of ribbon (last closed bar, or AcceptBar)
};

struct FAMState
{
    FAMEvent Events[FAM_MAX_EVENTS];
    int   Count;               // live records (Used == 1)
    int   NextID;              // next event ID
    int   MaxIDUsed;           // highest ID issued (bounds drawing purge)
    int   PrevLastClosed;      // last processed closed bar (missed-bar resume)
    int   Fp0, Fp1, Fp2, Fp3;  // settings fingerprint words
    int   MappedFp;            // fingerprint of mapped-source StudyID/Subgraph
    int   CachedDay;           // trading day the Auto cache was built for
    int   AutoReady;           // 1 when Auto thresholds are usable
    int   AutoBlock;           // cached Auto per-block threshold
    int   AutoZone;            // cached Auto cluster threshold
    int   WarnVisible;         // 1 when the warmup warning drawing is up
    int   LastAlertConfirmBar; // watermark: newest closed bar already alerted
    int   LastAlertFailBar;    // watermark: newest closed bar already alerted
};

// =============================================================================
// File-scope helpers (no static locals inside the study function)
// =============================================================================

static void FAMDeleteDrawing(SCStudyInterfaceRef sc, int lineNum)
{
    if (lineNum <= 0)
        return;
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNum);
}

static void FAMDeleteEventDrawings(SCStudyInterfaceRef sc, int eventID)
{
    if (eventID <= 0)
        return;
    FAMDeleteDrawing(sc, FAM_BASE_BUBBLE + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_HALO   + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_RIBBON + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_RING   + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_ARROW  + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_X      + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_RECT   + eventID);
    FAMDeleteDrawing(sc, FAM_BASE_LABEL  + eventID);
}

// Inclusive integer-tick range intersection (touching ranges intersect).
static bool FAMRangesIntersect(int aBot, int aTop, int bBot, int bTop)
{
    const int lo = aBot > bBot ? aBot : bBot;
    const int hi = aTop < bTop ? aTop : bTop;
    return lo <= hi;
}

static int FAMPriceToTick(float price, float tickSize)
{
    // Round-half-away on both sides of zero (exact for non-negative
    // futures prices; also correct if price ever goes negative).
    if (price >= 0.0f)
        return (int)floorf(price / tickSize + 0.5f);
    return (int)ceilf(price / tickSize - 0.5f);
}

// Nearest-rank percentile over a sorted ascending array.
static int64 FAMPercentile(const vector<int64>& sorted, int pct)
{
    const int n = (int)sorted.size();
    if (n <= 0)
        return 0;
    int p = pct < 1 ? 1 : (pct > 100 ? 100 : pct);
    int idx = (p * n + 99) / 100 - 1;
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

static void FAMDrawEllipse(SCStudyInterfaceRef sc, int lineNum,
    SCDateTime dt0, SCDateTime dt1, double v0, double v1,
    COLORREF color, int width, int transparency)
{
    s_UseTool T;
    T.Clear();
    T.ChartNumber       = sc.ChartNumber;
    T.DrawingType       = DRAWING_ELLIPSE;
    T.LineNumber        = lineNum;
    T.BeginDateTime     = dt0;
    T.EndDateTime       = dt1;
    T.BeginValue        = v0;
    T.EndValue          = v1;
    T.Color             = color;
    T.LineWidth         = (uint16_t)width;
    T.TransparencyLevel = (uint8_t)transparency;
    T.AddMethod         = UTAM_ADD_OR_ADJUST;
    sc.UseTool(T);
}

static void FAMDrawRect(SCStudyInterfaceRef sc, int lineNum,
    SCDateTime dt0, SCDateTime dt1, double v0, double v1,
    COLORREF color, COLORREF fill, int width, int transparency)
{
    s_UseTool T;
    T.Clear();
    T.ChartNumber       = sc.ChartNumber;
    T.DrawingType       = DRAWING_RECTANGLEHIGHLIGHT;
    T.LineNumber        = lineNum;
    T.BeginDateTime     = dt0;
    T.EndDateTime       = dt1;
    T.BeginValue        = v0 < v1 ? v0 : v1;
    T.EndValue          = v0 < v1 ? v1 : v0;
    T.Color             = color;
    T.SecondaryColor    = fill;
    T.LineWidth         = (uint16_t)width;
    T.TransparencyLevel = (uint8_t)transparency;
    T.AddMethod         = UTAM_ADD_OR_ADJUST;
    sc.UseTool(T);
}

// Text is anchored by bar index + price (BeginDateTime-only text does not
// reliably render; precedent: BigTradesTape BTLabel).
static void FAMDrawText(SCStudyInterfaceRef sc, int lineNum, int barIndex,
    double price, const char* text, COLORREF color, int fontSize)
{
    s_UseTool T;
    T.Clear();
    T.ChartNumber   = sc.ChartNumber;
    T.DrawingType   = DRAWING_TEXT;
    T.LineNumber    = lineNum;
    T.BeginIndex    = barIndex;
    T.BeginValue    = price;
    T.Color         = color;
    T.FontSize      = (uint8_t)fontSize;
    T.Text          = text;
    T.AddMethod     = UTAM_ADD_OR_ADJUST;
    sc.UseTool(T);
}

// =============================================================================
// Main study function
// =============================================================================

SCSFExport scsf_FailedAggressionMap(SCStudyInterfaceRef sc)
{
    // ── Input references ──────────────────────────────────────────────────────
    SCInputRef In_Lookback   = sc.Input[0];    // In:1
    SCInputRef In_ProxTicks  = sc.Input[1];    // In:2
    SCInputRef In_MinBlocks  = sc.Input[2];    // In:3
    SCInputRef In_ManBlock   = sc.Input[3];    // In:4
    SCInputRef In_ManZone    = sc.Input[4];    // In:5
    SCInputRef In_ConfBars   = sc.Input[5];    // In:6
    SCInputRef In_AdvTicks   = sc.Input[6];    // In:7
    SCInputRef In_ExitTicks  = sc.Input[7];    // In:8
    SCInputRef In_AccTicks   = sc.Input[8];    // In:9
    SCInputRef In_ThreshMode = sc.Input[9];    // In:10
    SCInputRef In_AutoDays   = sc.Input[10];   // In:11
    SCInputRef In_AutoBlkPct = sc.Input[11];   // In:12
    SCInputRef In_AutoZnPct  = sc.Input[12];   // In:13
    SCInputRef In_MappedSrc  = sc.Input[13];   // In:14
    SCInputRef In_RenderMode = sc.Input[14];   // In:15
    SCInputRef In_BubMin     = sc.Input[15];   // In:16
    SCInputRef In_BubMax     = sc.Input[16];   // In:17
    SCInputRef In_MaxDrawn   = sc.Input[17];   // In:18
    SCInputRef In_BuyColor   = sc.Input[18];   // In:19
    SCInputRef In_SellColor  = sc.Input[19];   // In:20
    SCInputRef In_HaloColor  = sc.Input[20];   // In:21
    SCInputRef In_AlertsOn   = sc.Input[21];   // In:22
    SCInputRef In_AlertSnd   = sc.Input[22];   // In:23

    // ── Subgraph references ───────────────────────────────────────────────────
    SCSubgraphRef SG_CandSide  = sc.Subgraph[0];
    SCSubgraphRef SG_ConfSide  = sc.Subgraph[1];
    SCSubgraphRef SG_Centroid  = sc.Subgraph[2];
    SCSubgraphRef SG_Top       = sc.Subgraph[3];
    SCSubgraphRef SG_Bot       = sc.Subgraph[4];
    SCSubgraphRef SG_Strength  = sc.Subgraph[5];
    SCSubgraphRef SG_Retest    = sc.Subgraph[6];
    SCSubgraphRef SG_FailEntry = sc.Subgraph[7];
    SCSubgraphRef SG_Accept    = sc.Subgraph[8];
    SCSubgraphRef SG_Mapped    = sc.Subgraph[9];
    SCSubgraphRef SG_Source    = sc.Subgraph[10];
    SCSubgraphRef SG_EventID   = sc.Subgraph[11];
    SCSubgraphRef SG_Bubble    = sc.Subgraph[12];
    SCSubgraphRef SG_Marker    = sc.Subgraph[13];

    // =========================================================================
    // SetDefaults
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName           = "Failed Aggression Map v1.0";
        sc.StudyDescription    = "Failed directional aggression at bar extremes: "
                                 "volume-scaled bubble at the delta-weighted "
                                 "centroid plus an inferred-inventory ribbon, "
                                 "with confirm / retest / accept lifecycle.";
        sc.AutoLoop            = 0;
        sc.GraphRegion         = 0;
        sc.ScaleRangeType      = SCALE_SAMEASREGION;
        sc.DrawZeros           = 0;
        sc.MaintainVolumeAtPriceData = 1;

        SG_CandSide.Name = "Candidate Side";             SG_CandSide.DrawStyle = DRAWSTYLE_IGNORE;   SG_CandSide.DrawZeros = 0;
        SG_ConfSide.Name = "Failure Confirmed Side";     SG_ConfSide.DrawStyle = DRAWSTYLE_IGNORE;   SG_ConfSide.DrawZeros = 0;
        SG_Centroid.Name = "Centroid Price";             SG_Centroid.DrawStyle = DRAWSTYLE_IGNORE;   SG_Centroid.DrawZeros = 0;
        SG_Top.Name = "Cluster Top";                     SG_Top.DrawStyle = DRAWSTYLE_IGNORE;         SG_Top.DrawZeros = 0;
        SG_Bot.Name = "Cluster Bottom";                  SG_Bot.DrawStyle = DRAWSTYLE_IGNORE;         SG_Bot.DrawZeros = 0;
        SG_Strength.Name = "Normalized Strength 0-10";   SG_Strength.DrawStyle = DRAWSTYLE_IGNORE;   SG_Strength.DrawZeros = 0;
        SG_Retest.Name = "First Retest Pulse";           SG_Retest.DrawStyle = DRAWSTYLE_IGNORE;     SG_Retest.DrawZeros = 0;
        SG_FailEntry.Name = "Failed Retest Entry";       SG_FailEntry.DrawStyle = DRAWSTYLE_IGNORE;  SG_FailEntry.DrawZeros = 0;
        SG_Accept.Name = "Accepted Through";             SG_Accept.DrawStyle = DRAWSTYLE_IGNORE;     SG_Accept.DrawZeros = 0;
        SG_Mapped.Name = "Mapped Confluence Flag";       SG_Mapped.DrawStyle = DRAWSTYLE_IGNORE;     SG_Mapped.DrawZeros = 0;
        SG_Source.Name = "Threshold Source";             SG_Source.DrawStyle = DRAWSTYLE_IGNORE;     SG_Source.DrawZeros = 0;
        SG_EventID.Name = "Event ID";                    SG_EventID.DrawStyle = DRAWSTYLE_IGNORE;    SG_EventID.DrawZeros = 0;
        SG_Bubble.Name = "Failure Bubble";               SG_Bubble.DrawStyle = DRAWSTYLE_POINT;
        SG_Bubble.PrimaryColor = RGB(255, 255, 255);     SG_Bubble.DrawZeros = 0; SG_Bubble.LineWidth = 3;
        SG_Marker.Name = "Retest/Accept Marker";         SG_Marker.DrawStyle = DRAWSTYLE_DIAMOND;
        SG_Marker.PrimaryColor = RGB(255, 200, 40);      SG_Marker.DrawZeros = 0; SG_Marker.LineWidth = 3;

        In_Lookback.Name = "Detection Lookback Bars";    In_Lookback.SetInt(2);  In_Lookback.SetIntLimits(1, 20);
        In_ProxTicks.Name = "Proximity Ticks";           In_ProxTicks.SetInt(3); In_ProxTicks.SetIntLimits(1, 50);
        In_MinBlocks.Name = "Minimum Blocks";            In_MinBlocks.SetInt(3); In_MinBlocks.SetIntLimits(1, 50);
        In_ManBlock.Name = "Manual Block Delta";         In_ManBlock.SetFloat(95.0f);  In_ManBlock.SetFloatLimits(1.0f, 1000000.0f);
        In_ManZone.Name = "Manual Cluster Delta";        In_ManZone.SetFloat(350.0f);  In_ManZone.SetFloatLimits(1.0f, 10000000.0f);
        In_ConfBars.Name = "Confirmation Bars";          In_ConfBars.SetInt(3);  In_ConfBars.SetIntLimits(1, 20);
        In_AdvTicks.Name = "Adverse Displacement Ticks"; In_AdvTicks.SetInt(4);  In_AdvTicks.SetIntLimits(1, 100);
        In_ExitTicks.Name = "Retest Exit Ticks";         In_ExitTicks.SetInt(2); In_ExitTicks.SetIntLimits(1, 100);
        In_AccTicks.Name = "Acceptance Ticks";           In_AccTicks.SetInt(1);  In_AccTicks.SetIntLimits(0, 100);

        In_ThreshMode.Name = "Threshold Mode";
        In_ThreshMode.SetCustomInputStrings("Manual;Auto;Both");
        In_ThreshMode.SetCustomInputIndex(0);

        In_AutoDays.Name = "Auto Days";                  In_AutoDays.SetInt(5);  In_AutoDays.SetIntLimits(1, 5);
        In_AutoBlkPct.Name = "Auto Block Percentile";    In_AutoBlkPct.SetInt(85); In_AutoBlkPct.SetIntLimits(50, 99);
        In_AutoZnPct.Name = "Auto Zone Percentile";      In_AutoZnPct.SetInt(80);  In_AutoZnPct.SetIntLimits(50, 99);

        In_MappedSrc.Name = "Mapped Location Source (disabled = StudyID 0)";
        In_MappedSrc.SetChartStudySubgraphValues(0, 0);

        In_RenderMode.Name = "Rendering Mode";
        In_RenderMode.SetCustomInputStrings("Bubble + Ribbon;Lifecycle Glyphs;Legacy Rectangle;Bubble + Ribbon + Rectangle");
        In_RenderMode.SetCustomInputIndex(0);

        In_BubMin.Name = "Bubble Min Radius (ticks)";    In_BubMin.SetInt(1);    In_BubMin.SetIntLimits(1, 20);
        In_BubMax.Name = "Bubble Max Radius (ticks)";    In_BubMax.SetInt(4);    In_BubMax.SetIntLimits(1, 20);
        In_MaxDrawn.Name = "Max Events Drawn";           In_MaxDrawn.SetInt(50); In_MaxDrawn.SetIntLimits(1, 128);
        In_BuyColor.Name = "Failed Buyers Color";        In_BuyColor.SetColor(RGB(255, 0, 200));
        In_SellColor.Name = "Failed Sellers Color";      In_SellColor.SetColor(RGB(0, 220, 255));
        In_HaloColor.Name = "Confluence/Halo Color";     In_HaloColor.SetColor(RGB(255, 200, 40));
        In_AlertsOn.Name = "Enable Alerts (0=No, 1=Yes)"; In_AlertsOn.SetYesNo(0);
        In_AlertSnd.Name = "Alert Sound Number";         In_AlertSnd.SetInt(0);  In_AlertSnd.SetIntLimits(0, 1000);

        return;
	}

    // =========================================================================
    // Cleanup on study removal
    // =========================================================================
    if (sc.LastCallToFunction)
    {
        FAMState* pOld = reinterpret_cast<FAMState*>(sc.GetPersistentPointer(FAM_PTR_SLOT));
        if (pOld != nullptr)
        {
            for (int k = 0; k < FAM_MAX_EVENTS; k++)
            {
                if (pOld->Events[k].Used)
                    FAMDeleteEventDrawings(sc, pOld->Events[k].ID);
            }
            FAMDeleteDrawing(sc, FAM_LINE_WARN);
            delete pOld;
            sc.SetPersistentPointer(FAM_PTR_SLOT, nullptr);
        }
        return;
    }

    // =========================================================================
    // Guards: VAP data and tick size
    // =========================================================================
    if (sc.VolumeAtPriceForBars == nullptr)
        return;
    if (sc.TickSize <= 0.0f)
        return;
    if (sc.ArraySize < 2)
        return;

    // =========================================================================
    // Read inputs into locals (every declared input is read here)
    // =========================================================================
    const float tickSize  = sc.TickSize;
    int   lookback  = In_Lookback.GetInt();   if (lookback < 1)  lookback = 1;  if (lookback > 20) lookback = 20;
    int   proxTicks = In_ProxTicks.GetInt();  if (proxTicks < 1) proxTicks = 1;
    int   minBlocks = In_MinBlocks.GetInt();  if (minBlocks < 1) minBlocks = 1;
    int64 manBlock  = (int64)(In_ManBlock.GetFloat() < 1.0f ? 1.0f : In_ManBlock.GetFloat());
    int64 manZone   = (int64)(In_ManZone.GetFloat() < 1.0f ? 1.0f : In_ManZone.GetFloat());
    int   confBars  = In_ConfBars.GetInt();   if (confBars < 1)  confBars = 1;
    int   advTicks  = In_AdvTicks.GetInt();   if (advTicks < 1)  advTicks = 1;
    int   exitTicks = In_ExitTicks.GetInt();  if (exitTicks < 1) exitTicks = 1;
    int   accTicks  = In_AccTicks.GetInt();   if (accTicks < 0)  accTicks = 0;
    const int threshMode = In_ThreshMode.GetIndex();   // 0 Manual, 1 Auto, 2 Both
    int   autoDays  = In_AutoDays.GetInt();   if (autoDays < 1) autoDays = 1; if (autoDays > FAM_MAX_DAYS) autoDays = FAM_MAX_DAYS;
    int   autoBlkPct = In_AutoBlkPct.GetInt(); if (autoBlkPct < 50) autoBlkPct = 50; if (autoBlkPct > 99) autoBlkPct = 99;
    int   autoZnPct  = In_AutoZnPct.GetInt();  if (autoZnPct < 50) autoZnPct = 50;  if (autoZnPct > 99) autoZnPct = 99;
    const int mappedStudyID = In_MappedSrc.GetStudyID();
    const int mappedSG      = In_MappedSrc.GetSubgraphIndex();
    const int renderMode = In_RenderMode.GetIndex();   // 0..3
    int   bubMin    = In_BubMin.GetInt();     if (bubMin < 1) bubMin = 1;
    int   bubMax    = In_BubMax.GetInt();     if (bubMax < bubMin) bubMax = bubMin;
    int   maxDrawn  = In_MaxDrawn.GetInt();   if (maxDrawn < 1) maxDrawn = 1; if (maxDrawn > FAM_MAX_EVENTS) maxDrawn = FAM_MAX_EVENTS;
    const COLORREF buyColor  = In_BuyColor.GetColor();
    const COLORREF sellColor = In_SellColor.GetColor();
    const COLORREF haloColor = In_HaloColor.GetColor();
    const int alertsOn = In_AlertsOn.GetYesNo();
    const int alertSnd = In_AlertSnd.GetInt();

    // Mapped-location array: fetched ONCE per call (never per bar).
    SCFloatArray mappedArr;
    const bool useMapped = (mappedStudyID > 0);
    if (useMapped)
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, mappedStudyID, mappedSG, mappedArr);

    // =========================================================================
    // Settings fingerprint (structural inputs only; display inputs excluded)
    // =========================================================================
    const int fp0 = (lookback & 0xFF) | ((proxTicks & 0xFF) << 8) |
                    ((minBlocks & 0xFF) << 16) | ((confBars & 0xFF) << 24);
    // Fold the full 64-bit threshold magnitudes into the word so edits
    // differing by multiples of 65536 still rebuild (M-3 fix).
    const int manBlockFold = (int)(manBlock ^ (manBlock >> 16) ^ (manBlock >> 32));
    const int manZoneFold  = (int)(manZone ^ (manZone >> 16) ^ (manZone >> 32));
    const int fp1 = manBlockFold * 31 ^ manZoneFold;
    const int fp2 = (advTicks & 0xFF) | ((exitTicks & 0xFF) << 8) |
                    ((accTicks & 0xFF) << 16) | ((threshMode & 0xFF) << 24);
    const int fp3 = (autoDays & 0xFF) | ((autoBlkPct & 0xFF) << 8) |
                    ((autoZnPct & 0xFF) << 16) | ((renderMode & 0xFF) << 24);
    const int mappedFp = useMapped ? ((mappedStudyID & 0xFFFF) | ((mappedSG & 0xFFFF) << 16)) : 0;

    FAMState* pState = reinterpret_cast<FAMState*>(sc.GetPersistentPointer(FAM_PTR_SLOT));

    bool isFullRecalc = (sc.UpdateStartIndex == 0);
    if (pState == nullptr)
        isFullRecalc = true;
    else if (pState->Fp0 != fp0 || pState->Fp1 != fp1 ||
             pState->Fp2 != fp2 || pState->Fp3 != fp3 ||
             pState->MappedFp != mappedFp)
        isFullRecalc = true;

    // History rewrite (backfill correcting bars at or below what we already
    // processed) must rebuild, not patch: the incremental path zeroes SGs
    // from UpdateStartIndex forward but would never recompute pulses on
    // bars at or below PrevLastClosed (H-2 fix). Cost is one full recalc.
    if (!isFullRecalc && pState != nullptr && pState->PrevLastClosed >= 0 &&
        sc.UpdateStartIndex <= pState->PrevLastClosed)
        isFullRecalc = true;

    if (isFullRecalc)
    {
        // Purge every drawing that could exist from the previous run, then
        // reset state. Old IDs ran 1..MaxIDUsed so the purge range is exact.
        if (pState != nullptr)
        {
            for (int oldID = 1; oldID <= pState->MaxIDUsed; oldID++)
                FAMDeleteEventDrawings(sc, oldID);
            FAMDeleteDrawing(sc, FAM_LINE_WARN);
            delete pState;
        }
        pState = new FAMState();
        for (int k = 0; k < FAM_MAX_EVENTS; k++)
            pState->Events[k].Used = 0;
        pState->Count = 0;
        pState->NextID = 1;
        pState->MaxIDUsed = 0;
        pState->PrevLastClosed = -1;
        pState->Fp0 = fp0; pState->Fp1 = fp1; pState->Fp2 = fp2; pState->Fp3 = fp3;
        pState->MappedFp = mappedFp;
        pState->CachedDay = 0;
        pState->AutoReady = 0;
        pState->AutoBlock = 0;
        pState->AutoZone = 0;
        pState->WarnVisible = 0;
        pState->LastAlertConfirmBar = -1;
        pState->LastAlertFailBar = -1;
        sc.SetPersistentPointer(FAM_PTR_SLOT, pState);

        // Clear ALL subgraph history (no stale ghost values after rebuild).
        for (int sgi = 0; sgi <= 13; sgi++)
        {
            for (int bi = 0; bi < sc.ArraySize; bi++)
                sc.Subgraph[sgi][bi] = 0.0f;
        }
    }
    else
    {
        // Incremental: clear subgraphs only over the refreshed bar range.
        const int clrFrom = sc.UpdateStartIndex < 0 ? 0 : sc.UpdateStartIndex;
        for (int sgi = 0; sgi <= 13; sgi++)
        {
            for (int bi = clrFrom; bi < sc.ArraySize; bi++)
                sc.Subgraph[sgi][bi] = 0.0f;
        }
    }

    // =========================================================================
    // Closed-bar scope: forming bar (ArraySize - 1) is never logic input
    // =========================================================================
    const int lastClosed = sc.ArraySize - 2;
    if (lastClosed < 0)
        return;

    // Intrabar guard with missed-bar resume: process every closed bar in
    // (PrevLastClosed, lastClosed]. First run processes the full history.
    if (!isFullRecalc && lastClosed <= pState->PrevLastClosed)
        return;   // intrabar tick — nothing new to decide

    const int procStart = isFullRecalc ? 0 : (pState->PrevLastClosed + 1 < 0 ? 0 : pState->PrevLastClosed + 1);

    // The mode needs Auto thresholds at all (drives warmup warning logic).
    const bool needAuto = (threshMode == 1 || threshMode == 2);

    // =========================================================================
    // Sequential closed-bar engine: advance existing events, then detect.
    // Identical order live and on full recalc, so replay reproduces live.
    // =========================================================================
    for (int w = procStart; w <= lastClosed; w++)
    {
        if (w < 0)
            continue;

        // ── Per-day Auto threshold cache (no lookahead: only days before D) ──
        if (needAuto)
        {
            const int dayW = sc.GetTradingDayDate(sc.BaseDateTimeIn[w]);
            if (pState->CachedDay != dayW)
            {
                pState->CachedDay = dayW;
                pState->AutoReady = 0;

                // Collect up to autoDays distinct COMPLETE days strictly before
                // dayW by scanning backward from w - 1.
                int dayStarts[FAM_MAX_DAYS];
                int dayEnds[FAM_MAX_DAYS];
                int nDays = 0;
                int i = w - 1;
                while (i >= 0 && nDays < autoDays)
                {
                    const int d = sc.GetTradingDayDate(sc.BaseDateTimeIn[i]);
                    if (d >= dayW) { i--; continue; }   // same (incomplete) day: skip
                    int e = i;
                    while (i >= 0 && sc.GetTradingDayDate(sc.BaseDateTimeIn[i]) == d)
                        i--;
                    dayStarts[nDays] = i + 1;
                    dayEnds[nDays] = e;
                    nDays++;
                }

                if (nDays >= autoDays)
                {
                    int64 dayBlk[FAM_MAX_DAYS];
                    int64 dayZn[FAM_MAX_DAYS];
                    int nBlk = 0, nZn = 0;
                    for (int dd = 0; dd < nDays; dd++)
                    {
                        vector<int64> lvl;
                        vector<int64> tot;
                        lvl.reserve(2048);
                        tot.reserve(256);
                        for (int we = dayStarts[dd] + lookback - 1; we <= dayEnds[dd]; we++)
                        {
                            if (we < lookback - 1)
                                continue;
                            // Only full-length windows calibrate: live
                            // detection never emits truncated day-open
                            // windows, so the sampler must not either (M-5).
                            if (we - dayStarts[dd] + 1 < lookback)
                                continue;
                            int ws = we - lookback + 1;
                            map<int, int64> dMap;
                            float wHigh = -1e30f, wLow = 1e30f;
                            for (int bi = ws; bi <= we; bi++)
                            {
                                if (sc.High[bi] > wHigh) wHigh = sc.High[bi];
                                if (sc.Low[bi] < wLow) wLow = sc.Low[bi];
                                int pit = INT_MIN;
                                const s_VolumeAtPriceV2* pVAP = nullptr;
                                while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement((unsigned int)bi, pit, &pVAP))
                                {
                                    if (pVAP == nullptr)
                                        continue;
                                    dMap[pit] += (int64)pVAP->AskVolume - (int64)pVAP->BidVolume;
                                }
                            }
                            if (dMap.empty())
                                continue;
                            const int hiT = FAMPriceToTick(wHigh, tickSize);
                            const int loT = FAMPriceToTick(wLow, tickSize);
                            int64 sideTotHi = 0, sideTotLo = 0;
                            for (map<int, int64>::reverse_iterator rit = dMap.rbegin(); rit != dMap.rend(); ++rit)
                            {
                                if (rit->first < hiT - proxTicks)
                                    break;
                                if (rit->second > 0)
                                {
                                    if ((int)lvl.size() < 20000)
                                        lvl.push_back(rit->second);
                                    sideTotHi += rit->second;
                                }
                            }
                            for (map<int, int64>::iterator it = dMap.begin(); it != dMap.end(); ++it)
                            {
                                if (it->first > loT + proxTicks)
                                    break;
                                if (it->second < 0)
                                {
                                    if ((int)lvl.size() < 20000)
                                        lvl.push_back(-it->second);
                                    sideTotLo += -it->second;
                                }
                            }
                            if (sideTotHi > 0 && (int)tot.size() < 4000)
                                tot.push_back(sideTotHi);
                            if (sideTotLo > 0 && (int)tot.size() < 4000)
                                tot.push_back(sideTotLo);
                        }
                        // A usable day needs a minimum sample of windows.
                        if ((int)tot.size() < FAM_MIN_DAYBARS || lvl.empty())
                            continue;
                        std::sort(lvl.begin(), lvl.end());
                        std::sort(tot.begin(), tot.end());
                        dayBlk[nBlk++] = FAMPercentile(lvl, autoBlkPct);
                        dayZn[nZn++] = FAMPercentile(tot, autoZnPct);
                    }
                    if (nBlk >= autoDays && nZn >= autoDays)
                    {
                        vector<int64> sb(dayBlk, dayBlk + nBlk), sz(dayZn, dayZn + nZn);
                        std::sort(sb.begin(), sb.end());
                        std::sort(sz.begin(), sz.end());
                        const int64 mb = sb[nBlk / 2];
                        const int64 mz = sz[nZn / 2];
                        pState->AutoBlock = (int)(mb < 1 ? 1 : mb);
                        pState->AutoZone = (int)(mz < 1 ? 1 : mz);
                        pState->AutoReady = 1;
                    }
                }
            }
        }

        const int closeTickW = FAMPriceToTick(sc.Close[w], tickSize);
        const int highTickW  = FAMPriceToTick(sc.High[w], tickSize);
        const int lowTickW   = FAMPriceToTick(sc.Low[w], tickSize);

        // ── PASS A: advance existing events with closed bar w ─────────────────
        for (int k = 0; k < FAM_MAX_EVENTS; k++)
        {
            FAMEvent* ev = &pState->Events[k];
            if (!ev->Used)
                continue;
            if (ev->State == FAM_DISCARDED || ev->State == FAM_ACCEPTED)
                continue;
            if (ev->DetectBar >= w)
                continue;   // never evaluate the detection bar itself

            const bool isHigh = (ev->Side > 0);

            // Acceptance-through is checked first (wins ties), valid from
            // FAILURE_CONFIRMED onward.
            if (ev->State == FAM_CONFIRMED || ev->State == FAM_RETEST || ev->State == FAM_FAILED)
            {
                const bool accepted = isHigh
                    ? (closeTickW >= ev->TopTick + accTicks)
                    : (closeTickW <= ev->BotTick - accTicks);
                if (accepted)
                {
                    ev->State = FAM_ACCEPTED;
                    ev->AcceptBar = w;
                    ev->RibbonEndBar = w;
                    SG_Accept[w] = (float)ev->Side;
                    SG_Marker[w] = sc.Close[w];
                    continue;
                }
            }

            if (ev->State == FAM_ATTEMPT)
            {
                const bool displaced = isHigh
                    ? (closeTickW <= ev->BotTick - advTicks)
                    : (closeTickW >= ev->TopTick + advTicks);
                if (displaced && w <= ev->DeadlineBar)
                {
                    ev->State = FAM_CONFIRMED;
                    ev->ConfirmBar = w;
                    ev->RibbonEndBar = w;
                    SG_ConfSide[w] = (float)ev->Side;
                    SG_Bubble[w] = (float)ev->CentroidTick * tickSize;
                }
                else if (w >= ev->DeadlineBar)
                {
                    ev->State = FAM_DISCARDED;   // missed the deadline: prune later
                }
            }
            else if (ev->State == FAM_CONFIRMED)
            {
                const bool retest = isHigh
                    ? (lowTickW < ev->BotTick && highTickW >= ev->BotTick)
                    : (highTickW > ev->TopTick && lowTickW <= ev->TopTick);
                if (retest)
                {
                    ev->State = FAM_RETEST;
                    ev->RetestBar = w;
                    SG_Retest[w] = (float)ev->Side;
                }
            }
            else if (ev->State == FAM_RETEST)
            {
                const bool exited = isHigh
                    ? (closeTickW <= ev->BotTick - exitTicks)
                    : (closeTickW >= ev->TopTick + exitTicks);
                if (exited)
                {
                    ev->State = FAM_FAILED;
                    ev->FailBar = w;
                    // Entry WITH the displacement: failed buyers -> short (-1),
                    // failed sellers -> long (+1).
                    SG_FailEntry[w] = isHigh ? -1.0f : 1.0f;
                    SG_Marker[w] = sc.Close[w];
                }
            }
            else if (ev->State == FAM_FAILED)
            {
                ev->RibbonEndBar = w;   // ribbon survives until acceptance
            }

            if (ev->State == FAM_CONFIRMED || ev->State == FAM_RETEST)
                ev->RibbonEndBar = w;
        }

        // ── PASS B: detect new candidates at window end w ─────────────────────
        if (w < lookback - 1)
            continue;

        // Threshold pairs to run: Manual always (modes 0/2), Auto when ready.
        int64 thrBlock[2]; int64 thrZone[2]; int thrSrc[2]; int nThr = 0;
        if (threshMode == 0 || threshMode == 2)
        {
            thrBlock[nThr] = manBlock; thrZone[nThr] = manZone;
            thrSrc[nThr] = FAM_SRC_MANUAL; nThr++;
        }
        if ((threshMode == 1 || threshMode == 2) && pState->AutoReady)
        {
            thrBlock[nThr] = (int64)pState->AutoBlock; thrZone[nThr] = (int64)pState->AutoZone;
            thrSrc[nThr] = FAM_SRC_AUTO; nThr++;
        }

        for (int t = 0; t < nThr; t++)
        {
            const int64 bTh = thrBlock[t] < 1 ? 1 : thrBlock[t];
            const int64 zTh = thrZone[t] < 1 ? 1 : thrZone[t];

            map<int, int64> dMap;
            float wHigh = -1e30f, wLow = 1e30f;
            for (int bi = w - lookback + 1; bi <= w; bi++)
            {
                if (sc.High[bi] > wHigh) wHigh = sc.High[bi];
                if (sc.Low[bi] < wLow) wLow = sc.Low[bi];
                int pit = INT_MIN;
                const s_VolumeAtPriceV2* pVAP = nullptr;
                while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement((unsigned int)bi, pit, &pVAP))
                {
                    if (pVAP == nullptr)
                        continue;
                    dMap[pit] += (int64)pVAP->AskVolume - (int64)pVAP->BidVolume;
                }
            }
            if (dMap.empty())
                continue;

            const int hiT = FAMPriceToTick(wHigh, tickSize);
            const int loT = FAMPriceToTick(wLow, tickSize);

            // Scan both sides: s = +1 high-side buyers, s = -1 low-side sellers.
            for (int s = 0; s < 2; s++)
            {
                const bool isHigh = (s == 0);
                int topT = 0, botT = 0, cnt = 0;
                int64 total = 0, tickW = 0;
                bool first = true;

                if (isHigh)
                {
                    for (map<int, int64>::reverse_iterator rit = dMap.rbegin(); rit != dMap.rend(); ++rit)
                    {
                        if (rit->first < hiT - proxTicks)
                            break;
                        if (rit->second < bTh)
                            continue;
                        if (first) { topT = rit->first; botT = rit->first; first = false; }
                        else
                        {
                            if (rit->first > topT) topT = rit->first;
                            if (rit->first < botT) botT = rit->first;
                        }
                        total += rit->second;
                        tickW += (int64)rit->first * rit->second;
                        cnt++;
                    }
                }
                else
                {
                    for (map<int, int64>::iterator it = dMap.begin(); it != dMap.end(); ++it)
                    {
                        if (it->first > loT + proxTicks)
                            break;
                        const int64 mag = -it->second;
                        if (mag < bTh)
                            continue;
                        if (first) { topT = it->first; botT = it->first; first = false; }
                        else
                        {
                            if (it->first > topT) topT = it->first;
                            if (it->first < botT) botT = it->first;
                        }
                        total += mag;
                        tickW += (int64)it->first * mag;
                        cnt++;
                    }
                }

                if (cnt < minBlocks || total < zTh || first)
                    continue;

                const int side = isHigh ? 1 : -1;
                // Exact integer rounding (total is a positive magnitude sum
                // here, tickW is non-negative for non-negative prices).
                const int centroid = (int)((tickW + total / 2) / total);

                // Both-mode Manual precedence: suppress an Auto candidate whose
                // inclusive tick range intersects any live Manual event.
                if (thrSrc[t] == FAM_SRC_AUTO)
                {
                    bool suppressed = false;
                    for (int k = 0; k < FAM_MAX_EVENTS; k++)
                    {
                        const FAMEvent* me = &pState->Events[k];
                        if (!me->Used || me->Source != FAM_SRC_MANUAL)
                            continue;
                        if (me->State == FAM_DISCARDED || me->State == FAM_ACCEPTED)
                            continue;
                        if (FAMRangesIntersect(botT, topT, me->BotTick, me->TopTick))
                        {
                            suppressed = true;
                            break;
                        }
                    }
                    if (suppressed)
                        continue;
                }

                // Same-mode overlap merges only within same side AND source.
                FAMEvent* target = nullptr;
                for (int k = 0; k < FAM_MAX_EVENTS; k++)
                {
                    FAMEvent* me = &pState->Events[k];
                    if (!me->Used || me->Source != thrSrc[t] || me->Side != side)
                        continue;
                    if (me->State == FAM_DISCARDED || me->State == FAM_ACCEPTED)
                        continue;
                    if (FAMRangesIntersect(botT, topT, me->BotTick, me->TopTick))
                    {
                        target = me;
                        break;
                    }
                }

                float strength = (float)((double)total * 10.0 / ((double)zTh * 3.0));
                if (strength < 0.5f) strength = 0.5f;
                if (strength > 10.0f) strength = 10.0f;

                if (target != nullptr)
                {
                    // Merge: widen, ADD totals, recompute centroid with exact
                    // integer math, recompute strength from the MERGED total
                    // (M-2), and publish the full pulse triple so bar w stays
                    // downstream-consistent (M-1).
                    if (topT > target->TopTick) target->TopTick = topT;
                    if (botT < target->BotTick) target->BotTick = botT;
                    target->TotalDelta += (isHigh ? total : -total);
                    target->TickWeight += tickW;
                    const int64 magT = target->TotalDelta >= 0 ? target->TotalDelta : -target->TotalDelta;
                    if (magT > 0)
                        target->CentroidTick = (int)((target->TickWeight + magT / 2) / magT);
                    float mStr = (float)((double)magT * 10.0 / ((double)zTh * 3.0));
                    if (mStr < 0.5f) mStr = 0.5f;
                    if (mStr > 10.0f) mStr = 10.0f;
                    target->Strength10 = mStr;
                    if (useMapped && mappedArr.GetArraySize() > w && mappedArr[w] != 0.0f)
                        target->MappedFlag = 1;
                    SG_CandSide[w] = (float)side;
                    SG_Centroid[w] = (float)target->CentroidTick * tickSize;
                    SG_Top[w] = (float)target->TopTick * tickSize;
                    SG_Bot[w] = (float)target->BotTick * tickSize;
                    SG_Strength[w] = mStr;
                    SG_Mapped[w] = (float)target->MappedFlag;
                    SG_Source[w] = (float)thrSrc[t];
                    SG_EventID[w] = (float)target->ID;
                    continue;
                }

                // Make room: prune discarded / accepted-oldest records first.
                int slot = -1;
                for (int k = 0; k < FAM_MAX_EVENTS; k++)
                {
                    if (!pState->Events[k].Used) { slot = k; break; }
                }
                if (slot < 0)
                {
                    for (int k = 0; k < FAM_MAX_EVENTS; k++)
                    {
                        if (pState->Events[k].State == FAM_DISCARDED) { slot = k; break; }
                    }
                }
                if (slot < 0)
                {
                    int oldest = -1;
                    for (int k = 0; k < FAM_MAX_EVENTS; k++)
                    {
                        if (pState->Events[k].State == FAM_ACCEPTED &&
                            (oldest < 0 || pState->Events[k].AcceptBar < pState->Events[oldest].AcceptBar))
                            oldest = k;
                    }
                    slot = oldest;
                }
                if (slot < 0)
                    continue;   // at capacity with live events: skip, bounded growth
                if (pState->Events[slot].Used)
                {
                    FAMDeleteEventDrawings(sc, pState->Events[slot].ID);
                    pState->Count--;
                }

                FAMEvent* ev = &pState->Events[slot];
                ev->Used = 1;
                ev->ID = pState->NextID++;
                if (ev->ID > pState->MaxIDUsed)
                    pState->MaxIDUsed = ev->ID;
                ev->Side = side;
                ev->Source = thrSrc[t];
                ev->State = FAM_ATTEMPT;
                ev->DetectBar = w;
                ev->ConfirmBar = -1;
                ev->RetestBar = -1;
                ev->FailBar = -1;
                ev->AcceptBar = -1;
                ev->DeadlineBar = w + confBars;
                ev->TopTick = topT;
                ev->BotTick = botT;
                ev->CentroidTick = centroid;
                ev->TotalDelta = (isHigh ? total : -total);
                ev->TickWeight = tickW;
                ev->Strength10 = strength;
                ev->MappedFlag = 0;
                ev->RibbonEndBar = w;
                pState->Count++;

                if (useMapped && mappedArr.GetArraySize() > w && mappedArr[w] != 0.0f)
                    ev->MappedFlag = 1;

                SG_CandSide[w] = (float)side;
                SG_Centroid[w] = (float)centroid * tickSize;
                SG_Top[w] = (float)topT * tickSize;
                SG_Bot[w] = (float)botT * tickSize;
                SG_Strength[w] = strength;
                SG_Mapped[w] = (float)ev->MappedFlag;
                SG_Source[w] = (float)thrSrc[t];
                SG_EventID[w] = (float)ev->ID;
            }
        }
    }

    pState->PrevLastClosed = lastClosed;

    // =========================================================================
    // Warmup warning (single persistent drawing, toggled — never spammed)
    // =========================================================================
    const bool wantWarn = needAuto && !pState->AutoReady;
    if (wantWarn && !pState->WarnVisible)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "FAM Auto: warming up (need %d complete days)", autoDays);
        FAMDrawText(sc, FAM_LINE_WARN, lastClosed, sc.High[lastClosed], buf, RGB(255, 200, 40), 10);
        pState->WarnVisible = 1;
    }
    else if (!wantWarn && pState->WarnVisible)
    {
        FAMDeleteDrawing(sc, FAM_LINE_WARN);
        pState->WarnVisible = 0;
    }

    // =========================================================================
    // Draw phase: newest maxDrawn live records keep drawings; rest collapsed
    // =========================================================================
    const bool wantBubble  = (renderMode == 0 || renderMode == 3);
    const bool wantGlyphs  = (renderMode == 1);
    const bool wantRect    = (renderMode == 2 || renderMode == 3);

    // Records are appended in ID order, so the tail holds the newest events.
    int liveIdx[FAM_MAX_EVENTS];
    int nLive = 0;
    for (int k = 0; k < FAM_MAX_EVENTS; k++)
    {
        if (pState->Events[k].Used && pState->Events[k].State != FAM_DISCARDED)
            liveIdx[nLive++] = k;
    }
    const int firstDrawn = nLive > maxDrawn ? nLive - maxDrawn : 0;
    for (int li = 0; li < firstDrawn; li++)
        FAMDeleteEventDrawings(sc, pState->Events[liveIdx[li]].ID);

    for (int li = firstDrawn; li < nLive; li++)
    {
        FAMEvent* ev = &pState->Events[liveIdx[li]];
        const bool isHigh = (ev->Side > 0);
        COLORREF sideColor = isHigh ? buyColor : sellColor;
        if (ev->Source == FAM_SRC_AUTO)
            sideColor = isHigh ? FAM_AUTO_BUY : FAM_AUTO_SELL;

        int radius = bubMin + (int)((ev->Strength10 / 10.0f) * (float)(bubMax - bubMin) + 0.5f);
        if (radius < 1) radius = 1;

        const double centP = (double)ev->CentroidTick * tickSize;
        const int endBar = ev->RibbonEndBar > lastClosed ? lastClosed : ev->RibbonEndBar;
        const int eb = ev->DetectBar > lastClosed ? lastClosed : ev->DetectBar;
        const int eb1 = eb + 1 > lastClosed ? lastClosed : eb + 1;

        // Attempt dots (glyph mode): small hollow-looking gray mark at detection.
        if (wantGlyphs && ev->State == FAM_ATTEMPT)
        {
            FAMDrawEllipse(sc, FAM_BASE_BUBBLE + ev->ID,
                sc.BaseDateTimeIn[eb], sc.BaseDateTimeIn[eb1],
                centP - tickSize, centP + tickSize, RGB(150, 150, 150), 1, 85);
            continue;
        }
        if (ev->State == FAM_ATTEMPT)
            continue;   // unconfirmed attempts are never drawn outside glyph mode

        if (wantBubble || wantGlyphs)
        {
            FAMDrawEllipse(sc, FAM_BASE_BUBBLE + ev->ID,
                sc.BaseDateTimeIn[eb], sc.BaseDateTimeIn[eb1],
                centP - radius * tickSize, centP + radius * tickSize,
                sideColor, 2, 35);

            if (ev->MappedFlag)
            {
                FAMDrawEllipse(sc, FAM_BASE_HALO + ev->ID,
                    sc.BaseDateTimeIn[eb], sc.BaseDateTimeIn[eb1],
                    centP - (radius + 2) * tickSize, centP + (radius + 2) * tickSize,
                    haloColor, 2, 70);
            }
            else
            {
                FAMDeleteDrawing(sc, FAM_BASE_HALO + ev->ID);
            }

            // Thin ribbon centered on the inferred centroid.
            const int rb = endBar < eb ? eb : endBar;
            FAMDrawRect(sc, FAM_BASE_RIBBON + ev->ID,
                sc.BaseDateTimeIn[eb], sc.BaseDateTimeIn[rb],
                centP - 0.5 * tickSize, centP + 0.5 * tickSize,
                sideColor, sideColor, 1, 45);

            char lbl[64];
            snprintf(lbl, sizeof(lbl), "FAM inferred %s",
                ev->Source == FAM_SRC_AUTO ? "auto" : "manual");
            FAMDrawText(sc, FAM_BASE_LABEL + ev->ID, rb,
                centP + (radius + 1) * tickSize, lbl, sideColor, 8);
        }
        else
        {
            FAMDeleteDrawing(sc, FAM_BASE_BUBBLE + ev->ID);
            FAMDeleteDrawing(sc, FAM_BASE_HALO + ev->ID);
            FAMDeleteDrawing(sc, FAM_BASE_RIBBON + ev->ID);
            FAMDeleteDrawing(sc, FAM_BASE_LABEL + ev->ID);
        }

        if (ev->State == FAM_RETEST || ev->State == FAM_FAILED || ev->State == FAM_ACCEPTED)
        {
            if (ev->RetestBar >= 0)
            {
                FAMDrawEllipse(sc, FAM_BASE_RING + ev->ID,
                    sc.BaseDateTimeIn[eb], sc.BaseDateTimeIn[eb1],
                    centP - (radius + 2) * tickSize, centP + (radius + 2) * tickSize,
                    haloColor, 3, 60);
            }
        }
        else
        {
            FAMDeleteDrawing(sc, FAM_BASE_RING + ev->ID);
        }

        if (ev->State == FAM_FAILED || ev->State == FAM_ACCEPTED)
        {
            if (ev->FailBar >= 0)
            {
                // Anchored at the fail-bar close so the drawn text agrees
                // with SG13 (M-4 fix).
                const double ap = (double)sc.Close[ev->FailBar];
                FAMDrawText(sc, FAM_BASE_ARROW + ev->ID, ev->FailBar, ap,
                    isHigh ? "FAIL SHORT" : "FAIL LONG", sideColor, 9);
            }
        }
        else
        {
            FAMDeleteDrawing(sc, FAM_BASE_ARROW + ev->ID);
        }

        if (ev->State == FAM_ACCEPTED)
        {
            const double xp = isHigh
                ? (double)ev->TopTick * tickSize
                : (double)ev->BotTick * tickSize;
            const int xb = ev->AcceptBar >= 0 ? ev->AcceptBar : endBar;
            FAMDrawText(sc, FAM_BASE_X + ev->ID, xb, xp, "X", FAM_GRAY_X, 12);
        }
        else
        {
            FAMDeleteDrawing(sc, FAM_BASE_X + ev->ID);
        }

        if (wantRect)
        {
            const int rb = endBar < eb ? eb : endBar;
            FAMDrawRect(sc, FAM_BASE_RECT + ev->ID,
                sc.BaseDateTimeIn[eb], sc.BaseDateTimeIn[rb],
                (double)ev->BotTick * tickSize, (double)ev->TopTick * tickSize,
                sideColor, sideColor, 1, 60);
        }
        else
        {
            FAMDeleteDrawing(sc, FAM_BASE_RECT + ev->ID);
        }
    }

    // Prune discarded records now that their drawings are gone.
    for (int k = 0; k < FAM_MAX_EVENTS; k++)
    {
        if (pState->Events[k].Used && pState->Events[k].State == FAM_DISCARDED)
        {
            FAMDeleteEventDrawings(sc, pState->Events[k].ID);
            pState->Events[k].Used = 0;
            pState->Count--;
        }
    }

    // =========================================================================
    // Live-only alerts with watermark (never from historical bars)
    // =========================================================================
    if (alertsOn && !isFullRecalc)
    {
        for (int k = 0; k < FAM_MAX_EVENTS; k++)
        {
            const FAMEvent* ev = &pState->Events[k];
            if (!ev->Used)
                continue;
            if (ev->ConfirmBar == lastClosed && pState->LastAlertConfirmBar < lastClosed)
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "FAM failure confirmed %s #%d",
                    ev->Side > 0 ? "buyers" : "sellers", ev->ID);
                sc.SetAlert(alertSnd, sc.ArraySize - 1, msg);
                pState->LastAlertConfirmBar = lastClosed;
                break;
            }
        }
        for (int k = 0; k < FAM_MAX_EVENTS; k++)
        {
            const FAMEvent* ev = &pState->Events[k];
            if (!ev->Used)
                continue;
            if (ev->FailBar == lastClosed && pState->LastAlertFailBar < lastClosed)
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "FAM retest failed #%d", ev->ID);
                sc.SetAlert(alertSnd, sc.ArraySize - 1, msg);
                pState->LastAlertFailBar = lastClosed;
                break;
            }
        }
    }

}   // end scsf_FailedAggressionMap
