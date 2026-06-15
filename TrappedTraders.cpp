// =============================================================================
// TrappedTraders.cpp
// Sierra Chart ACSIL Custom Study — Trapped Traders Zones
//
// CONCEPT:
//   Detects price extremes where aggressive participants accumulated large
//   directional delta within a rolling window of closed bars but failed to
//   continue — trapping them on the wrong side of the market.
//
//   Delta is AGGREGATED across the lookback window at each price level,
//   then scanned for qualifying clusters near the window's price extreme.
//
//   SELL ZONE  — positive delta cluster near window HIGH
//                Buyers pushed to the top; price rejected → trapped longs
//                → resistance zone, expect downward reaction
//
//   LONG ZONE  — negative delta cluster near window LOW
//                Sellers pushed to the bottom; price rejected → trapped shorts
//                → support zone, expect upward reaction
//
// ZONE LIFECYCLE:
//   1. DETECTED on bar close when delta thresholds are met.
//   2. CONFIRMED on the NEXT bar close:
//        Sell zone → next bar must NOT open above TopPrice
//                    (if it does, buyers weren't trapped — discard zone)
//        Long zone → next bar must NOT open below BottomPrice
//                    (if it does, sellers weren't trapped — discard zone)
//      Discarded zones are never drawn, even in Show Erased Zones mode.
//   3. ERASED once confirmed, when price closes fully through it:
//        Sell zone → close above TopPrice    (buyers broke through resistance)
//        Long zone → close below BottomPrice (sellers broke through support)
//   Overlapping zones of the same type are merged into one wider zone.
//
// CROSS-CHART DISPLAY:
//   Zones are created as user drawings with AllowCopyToOtherCharts = 1.
//   To display zones on another chart: on the target chart go to
//   Chart Settings → Chart Drawings → "Copy chart drawings from chart #'s"
//   and enter this chart's number.
//
// INPUTS:
//   In:1  Numbers Bars Lookback         bars in rolling aggregation window
//   In:2  Number of Blocks Per Zone     min qualifying price levels in cluster
//   In:3  Minimum Single Block Delta    min aggregated |delta| per price level
//   In:4  Zone Delta Threshold          min total aggregated |delta| for zone
//   In:5  Proximity Tolerance (blocks)  max distance from window extreme
//   In:6  Sell Zone Fill Color
//   In:7  Sell Zone Border Color
//   In:8  Long Zone Fill Color
//   In:9  Long Zone Border Color
//   In:10 Zone Border Width
//   In:11 Transparency Level
//   In:12 Show Delta Info
//   In:13 Show Erased Zones
//
// SUBGRAPHS (DRAWSTYLE_IGNORE — data outputs only):
//   SG1  Sell Zone Delta
//   SG2  Buy Zone Delta
//
// REQUIREMENTS:
//   Numbers Bar / Footprint chart with sc.MaintainVolumeAtPriceData = 1
//   Designed for PnF or Renko charts; tick size 0.50 recommended for ES
//
// AutoLoop: 0 (manual loop for performance)
// =============================================================================

#include "sierrachart.h"
#include <vector>
#include <map>
#include <cmath>
#include <cstdio>
using std::vector;
using std::map;
using std::pair;

SCDLLName("TrappedTraders")

// =============================================================================
// Zone record
// =============================================================================

struct ZoneRec
{
    int     ID;
    float   TopPrice;
    float   BottomPrice;
    int     DetectedBar;   // window-end bar of first detection (drawing left edge)
    int     ErasedBar;     // bar where zone was invalidated; -1 = never invalidated
    bool    IsBuyZone;     // true = long zone (neg delta), false = sell zone (pos delta)
    float   TotalDelta;    // negative for long zone, positive for sell zone
    bool    Valid;         // false = rejected before confirmation OR erased after confirmation
    bool    Pending;       // true = waiting for next-bar price confirmation; not yet drawn
};

// Drawing line-number bases — each zone uses its ID + base
static const int BASE_SELL = 10000;
static const int BASE_BUY  = 20000;
static const int BASE_LBL  = 30000;

// Persistent storage slots
static const int PVEC_SLOT   = 1;   // vector<ZoneRec>*
static const int PIDCTR_SLOT = 2;   // next zone ID counter
// Slot 3: intrabar guard (last seen sc.ArraySize)

// =============================================================================
// Drawing helpers
// =============================================================================

// Collapses a drawing to a zero-size transparent rect — the correct way to
// "delete" a UseTool drawing without version-dependent delete APIs.
static void DeleteACSILDrawing(SCStudyInterfaceRef sc, int lineNum)
{
    if (lineNum <= 0) return;
    s_UseTool T;
    T.ChartNumber       = sc.ChartNumber;
    T.DrawingType       = DRAWING_RECTANGLEHIGHLIGHT;
    T.LineNumber        = lineNum;
    T.BeginDateTime     = sc.BaseDateTimeIn[0];
    T.EndDateTime       = sc.BaseDateTimeIn[0];
    T.BeginValue        = 0;
    T.EndValue          = 0;
    T.TransparencyLevel = 100;
    T.AddMethod         = UTAM_ADD_OR_ADJUST;
    sc.UseTool(T);
}

// Draws or refreshes a highlight rectangle between two bars.
// NOTE: DRAWING_RECTANGLEHIGHLIGHT (not EXT) is used so EndDateTime is
// respected — the EXT variant always extends to the chart's right edge.
// AddAsUserDrawnDrawing + AllowCopyToOtherCharts let the zone appear on other
// charts via Chart Settings → Chart Drawings → "Copy chart drawings from chart #'s".
static void DrawZoneRect(SCStudyInterfaceRef sc,
                         int lineNum, int startBar, int endBar,
                         float botPrice, float topPrice,
                         COLORREF fill, COLORREF border,
                         int width, int transparency)
{
    s_UseTool T;
    T.Clear();
    T.ChartNumber            = sc.ChartNumber;
    T.DrawingType            = DRAWING_RECTANGLEHIGHLIGHT;
    T.BeginDateTime          = sc.BaseDateTimeIn[startBar];
    T.EndDateTime            = sc.BaseDateTimeIn[endBar];
    T.BeginValue             = (double)min(botPrice, topPrice);
    T.EndValue               = (double)max(botPrice, topPrice);
    T.Color                  = border;        // outline / border color
    T.SecondaryColor         = fill;          // interior fill color
    T.LineWidth              = (uint16_t)width;
    T.TransparencyLevel      = transparency;
    T.AddMethod              = UTAM_ADD_OR_ADJUST;
    T.LineNumber             = lineNum;
    T.AddAsUserDrawnDrawing  = 1;
    T.AllowCopyToOtherCharts = 1;
    sc.UseTool(T);
}

// Returns true if two price ranges [a0,a1] and [b0,b1] overlap.
static bool PriceRangesOverlap(float a0, float a1, float b0, float b1)
{
    return min(a0, a1) < max(b0, b1) &&
           min(b0, b1) < max(a0, a1);
}

// =============================================================================
// Main study function
// =============================================================================

SCSFExport scsf_TrappedTraders(SCStudyInterfaceRef sc)
{
    // ── Input references ──────────────────────────────────────────────────────
    SCInputRef In_BarsLookback    = sc.Input[0];   // In:1
    SCInputRef In_BlocksPerZone   = sc.Input[1];   // In:2
    SCInputRef In_MinBlockDelta   = sc.Input[2];   // In:3
    SCInputRef In_ZoneDeltaThresh = sc.Input[3];   // In:4
    SCInputRef In_ProximityTol    = sc.Input[4];   // In:5
    SCInputRef In_SellFillColor   = sc.Input[5];   // In:6
    SCInputRef In_SellBordColor   = sc.Input[6];   // In:7
    SCInputRef In_BuyFillColor    = sc.Input[7];   // In:8
    SCInputRef In_BuyBordColor    = sc.Input[8];   // In:9
    SCInputRef In_BorderWidth     = sc.Input[9];   // In:10
    SCInputRef In_Transparency    = sc.Input[10];  // In:11
    SCInputRef In_ShowDelta       = sc.Input[11];  // In:12
    SCInputRef In_DisplayMode     = sc.Input[12];  // In:13

    // ── Subgraph references ───────────────────────────────────────────────────
    SCSubgraphRef SG_SellDelta = sc.Subgraph[0];   // SG1 — sell zone total delta
    SCSubgraphRef SG_BuyDelta  = sc.Subgraph[1];   // SG2 — buy zone total delta

    // =========================================================================
    // SetDefaults
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Trapped Traders";
        sc.StudyDescription = "Zones where aggressive participants pushed price "
                              "to an extreme within a rolling bar window but "
                              "failed to continue — trapping them on the wrong side.";
        sc.AutoLoop              = 0;
        sc.GraphRegion           = 0;
        sc.ScaleRangeType        = SCALE_SAMEASREGION;
        sc.DrawZeros             = 0;
        sc.MaintainVolumeAtPriceData = 1;   // REQUIRED: footprint/VAP data

        // Subgraphs — data outputs only, not drawn on chart
        SG_SellDelta.Name       = "Sell Zone Delta";
        SG_SellDelta.DrawStyle  = DRAWSTYLE_IGNORE;
        SG_SellDelta.DrawZeros  = 0;

        SG_BuyDelta.Name        = "Buy Zone Delta";
        SG_BuyDelta.DrawStyle   = DRAWSTYLE_IGNORE;
        SG_BuyDelta.DrawZeros   = 0;

        // Inputs
        In_BarsLookback.Name    = "Numbers Bars Lookback";
        In_BarsLookback.SetInt(2);

        In_BlocksPerZone.Name   = "Number of Blocks Per Zone";
        In_BlocksPerZone.SetInt(3);

        In_MinBlockDelta.Name   = "Minimum Single Block Delta";
        In_MinBlockDelta.SetFloat(95.0f);

        In_ZoneDeltaThresh.Name = "Zone Delta Threshold";
        In_ZoneDeltaThresh.SetFloat(350.0f);

        In_ProximityTol.Name    = "Proximity Tolerance (blocks from extreme)";
        In_ProximityTol.SetInt(3);

        In_SellFillColor.Name   = "Sell Zone Fill Color";
        In_SellFillColor.SetColor(RGB(180, 40, 50));

        In_SellBordColor.Name   = "Sell Zone Border Color";
        In_SellBordColor.SetColor(RGB(220, 80, 90));

        In_BuyFillColor.Name    = "Long Zone Fill Color";
        In_BuyFillColor.SetColor(RGB(30, 80, 160));

        In_BuyBordColor.Name    = "Long Zone Border Color";
        In_BuyBordColor.SetColor(RGB(60, 130, 220));

        In_BorderWidth.Name     = "Zone Border Width";
        In_BorderWidth.SetInt(1);

        In_Transparency.Name    = "Transparency Level";
        In_Transparency.SetInt(50);

        In_ShowDelta.Name       = "Show Delta Info (0=No, 1=Yes)";
        In_ShowDelta.SetYesNo(1);

        In_DisplayMode.Name     = "Show Erased Zones";
        In_DisplayMode.SetDescription("No = Clipped Zones (default): erased zones disappear. "
                                      "Yes = Show Erased Zones: invalidated zones remain visible "
                                      "from their origin to the bar they were erased, drawn at "
                                      "higher transparency to distinguish from active zones.");
        In_DisplayMode.SetYesNo(1);

        return;
    }

    // =========================================================================
    // Cleanup on study removal
    // =========================================================================
    if (sc.LastCallToFunction)
    {
        vector<ZoneRec>* pZ =
            reinterpret_cast<vector<ZoneRec>*>(sc.GetPersistentPointer(PVEC_SLOT));
        if (pZ)
        {
            for (auto& z : *pZ)
            {
                int base = z.IsBuyZone ? BASE_BUY : BASE_SELL;
                DeleteACSILDrawing(sc, base     + z.ID);
                DeleteACSILDrawing(sc, BASE_LBL + z.ID);
            }
            delete pZ;
            sc.SetPersistentPointer(PVEC_SLOT, nullptr);
        }
        return;
    }

    // =========================================================================
    // Guard: VAP data must be available
    // =========================================================================
    if (sc.VolumeAtPriceForBars == nullptr) return;

    // =========================================================================
    // Read inputs into locals
    // =========================================================================
    const int   lookback     = max(1, In_BarsLookback.GetInt());
    const int   blocksMin    = max(1, In_BlocksPerZone.GetInt());
    // These are absolute thresholds — always positive regardless of what the user enters.
    // Sign direction is handled internally: positive delta for sell zones,
    // negative delta for long zones.
    const float minDelta     = fabs(In_MinBlockDelta.GetFloat());
    const float totalThresh  = fabs(In_ZoneDeltaThresh.GetFloat());
    const int   proxTol      = max(1, In_ProximityTol.GetInt());
    const COLORREF sellFill  = In_SellFillColor.GetColor();
    const COLORREF sellBord  = In_SellBordColor.GetColor();
    const COLORREF buyFill   = In_BuyFillColor.GetColor();
    const COLORREF buyBord   = In_BuyBordColor.GetColor();
    const int   borderWidth  = In_BorderWidth.GetInt();
    const int   transparency = In_Transparency.GetInt();
    const int   showDelta    = In_ShowDelta.GetYesNo();
    const int   showErased   = In_DisplayMode.GetYesNo();

    // =========================================================================
    // Settings-change detection
    // =========================================================================
    // SC does not always pass UpdateStartIndex = 0 when the user changes inputs
    // (behaviour varies by input type and SC version).  Without this guard the
    // intrabar-tick check fires (same ArraySize, no new bar) and the function
    // returns early, leaving old zones and stale drawings on screen until the
    // user restarts SC.
    //
    // Fix: pack structural inputs into three fingerprint words stored in
    // persistent slots 4-6.  Any change forces isFullRecalc = true, bypassing
    // the intrabar guard and triggering a full zone rebuild.
    //
    // showErased is included because toggling it affects which drawings should
    // exist (erased-zone rects must be deleted or created immediately).
    // Display-only inputs (colours, transparency, borderWidth, showDelta) are
    // intentionally excluded — PASS 4 always redraws with the current values,
    // so those changes take effect naturally on the next bar close.
    const int fp1 = (lookback & 0xFFFF) | ((blocksMin & 0xFFFF) << 16);
    const int fp2 = ((int)roundf(minDelta) & 0xFFFF) | ((proxTol & 0xFFFF) << 16);
    const int fp3 = ((int)roundf(totalThresh) & 0xFFFF) | ((showErased & 0xFF) << 16);

    const bool settingsChanged = (sc.GetPersistentInt(4) != fp1) ||
                                  (sc.GetPersistentInt(5) != fp2) ||
                                  (sc.GetPersistentInt(6) != fp3);

    // =========================================================================
    // Persistent zone vector
    // =========================================================================
    vector<ZoneRec>* pZones =
        reinterpret_cast<vector<ZoneRec>*>(sc.GetPersistentPointer(PVEC_SLOT));

    bool isFullRecalc = (sc.UpdateStartIndex == 0) || settingsChanged;

    if (isFullRecalc || pZones == nullptr)
    {
        // Save new fingerprint so the next call doesn't re-trigger full recalc
        sc.SetPersistentInt(4, fp1);
        sc.SetPersistentInt(5, fp2);
        sc.SetPersistentInt(6, fp3);

        // Erase ALL existing drawings (active and erased) then rebuild from scratch.
        if (pZones)
        {
            for (auto& z : *pZones)
            {
                int base = z.IsBuyZone ? BASE_BUY : BASE_SELL;
                DeleteACSILDrawing(sc, base     + z.ID);
                DeleteACSILDrawing(sc, BASE_LBL + z.ID);
            }
            delete pZones;
        }
        pZones = new vector<ZoneRec>();
        sc.SetPersistentPointer(PVEC_SLOT, pZones);
        sc.SetPersistentInt(PIDCTR_SLOT, 1);
    }

    // =========================================================================
    // Skip intrabar ticks — only process on bar close or full recalc
    // =========================================================================
    // Persistent slot 3 holds the last sc.ArraySize we processed.
    // A change means a new bar has closed.
    if (!isFullRecalc)
    {
        int prev = sc.GetPersistentInt(3);
        if (sc.ArraySize == prev) return;   // intrabar tick — nothing to do
        sc.SetPersistentInt(3, sc.ArraySize);
    }

    const int lastBar = sc.ArraySize - 2;   // last CLOSED bar; current forming bar excluded
    if (lastBar < lookback - 1) return;     // not enough closed bars yet

    // =========================================================================
    // PASS 1 — Scan windows for new zone candidates
    // =========================================================================
    // Incremental : only scan the latest window (last N closed bars)
    // Full recalc : slide window across ALL history, one bar at a time,
    //               replicating what would have happened if the study ran live.
    //
    // Chain-merge guard: a zone can never grow wider than proxTol * tickSize.
    // Without this cap, consecutive overlapping windows in a trend each produce
    // a zone 1 tick higher than the last, which keeps merging and growing the
    // zone by one tick per bar → 100-pt zones after 200 bars in a trend.
    const float maxZoneWidth = (float)proxTol * sc.TickSize;
    const int   winEndStart  = isFullRecalc ? (lookback - 1) : lastBar;

    for (int winEnd = winEndStart; winEnd <= lastBar; winEnd++)
    {
        int winStart = winEnd - lookback + 1;
        if (winStart < 0) winStart = 0;

        // ── Build aggregated delta map for this window ────────────────────────
        // Key   = price in ticks (integer)
        // Value = cumulative delta (ask - bid) across all bars in window
        map<int, float> dMap;
        float wHigh = -1e30f;
        float wLow  =  1e30f;

        for (int bi = winStart; bi <= winEnd; bi++)
        {
            if (sc.High[bi] > wHigh) wHigh = sc.High[bi];
            if (sc.Low[bi]  < wLow)  wLow  = sc.Low[bi];

            int PIT = INT_MIN;
            const s_VolumeAtPriceV2* pVAP = nullptr;
            while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement(
                       (unsigned int)bi, PIT, &pVAP))
            {
                if (!pVAP) continue;
                float d = (float)((int64_t)pVAP->AskVolume - (int64_t)pVAP->BidVolume);
                dMap[PIT] += d;
            }
        }

        if (dMap.empty()) continue;

        // Convert to sorted vector ascending by price tick (map is already sorted)
        vector<pair<int, float>> lv(dMap.begin(), dMap.end());

        // Tick keys for the window's price extremes
        const int highTick = (int)round(wHigh / sc.TickSize);
        const int lowTick  = (int)round(wLow  / sc.TickSize);

        // ── Sell zone scan (positive delta near window high) ──────────────────
        // Scan from the highest price level downward within proxTol ticks.
        // Collect levels with delta >= minDelta.
        {
            const int scanFloor = highTick - proxTol;
            float total = 0.0f;
            int   count = 0;
            float minP  =  1e30f;
            float maxP  = -1e30f;

            for (int i = (int)lv.size() - 1; i >= 0; i--)
            {
                if (lv[i].first < scanFloor) break;    // below proximity window
                if (lv[i].second < minDelta) continue; // level below per-block threshold

                float price = (float)lv[i].first * sc.TickSize;
                if (price < minP) minP = price;
                if (price > maxP) maxP = price;
                total += lv[i].second;
                count++;
            }

            // Both minimum block count AND total delta threshold must be met
            if (count >= blocksMin && total >= totalThresh)
            {
                float zBot = minP, zTop = maxP;

                // Check for overlap with existing sell zones — merge if found.
                // Guard: only merge if the result stays within maxZoneWidth.
                // Without this cap, consecutive windows in a trend chain-merge
                // into 100-pt zones.
                bool merged = false;
                for (auto& ez : *pZones)
                {
                    if (!ez.Valid || ez.IsBuyZone) continue;
                    if (PriceRangesOverlap(zBot, zTop, ez.BottomPrice, ez.TopPrice))
                    {
                        float newBot = min(ez.BottomPrice, zBot);
                        float newTop = max(ez.TopPrice,    zTop);
                        if ((newTop - newBot) > maxZoneWidth) break; // too wide — treat as separate zone
                        ez.BottomPrice = newBot;
                        ez.TopPrice    = newTop;
                        if (winEnd < ez.DetectedBar) ez.DetectedBar = winEnd;
                        merged = true;
                        break;
                    }
                }

                if (!merged)
                {
                    ZoneRec z;
                    z.ID          = sc.GetPersistentInt(PIDCTR_SLOT);
                    sc.SetPersistentInt(PIDCTR_SLOT, z.ID + 1);
                    z.TopPrice    = zTop;
                    z.BottomPrice = zBot;
                    z.DetectedBar = winEnd;
                    z.ErasedBar   = -1;
                    z.IsBuyZone   = false;
                    z.TotalDelta  = total;    // positive for sell zone
                    z.Valid       = true;
                    z.Pending     = true;     // held for next-bar price confirmation
                    pZones->push_back(z);
                }
            }
        }

        // ── Long zone scan (negative delta near window low) ───────────────────
        // Scan from the lowest price level upward within proxTol ticks.
        // Collect levels with abs(delta) >= minDelta where delta < 0.
        {
            const int scanCeil = lowTick + proxTol;
            float totalAbs = 0.0f;
            int   count    = 0;
            float minP     =  1e30f;
            float maxP     = -1e30f;

            for (int i = 0; i < (int)lv.size(); i++)
            {
                if (lv[i].first > scanCeil) break;     // above proximity window
                float absDelta = -lv[i].second;         // flip: neg delta → pos magnitude
                if (absDelta < minDelta) continue;      // level below per-block threshold

                float price = (float)lv[i].first * sc.TickSize;
                if (price < minP) minP = price;
                if (price > maxP) maxP = price;
                totalAbs += absDelta;
                count++;
            }

            if (count >= blocksMin && totalAbs >= totalThresh)
            {
                float zBot = minP, zTop = maxP;

                bool merged = false;
                for (auto& ez : *pZones)
                {
                    if (!ez.Valid || !ez.IsBuyZone) continue;
                    if (PriceRangesOverlap(zBot, zTop, ez.BottomPrice, ez.TopPrice))
                    {
                        float newBot = min(ez.BottomPrice, zBot);
                        float newTop = max(ez.TopPrice,    zTop);
                        if ((newTop - newBot) > maxZoneWidth) break; // too wide — treat as separate zone
                        ez.BottomPrice = newBot;
                        ez.TopPrice    = newTop;
                        if (winEnd < ez.DetectedBar) ez.DetectedBar = winEnd;
                        merged = true;
                        break;
                    }
                }

                if (!merged)
                {
                    ZoneRec z;
                    z.ID          = sc.GetPersistentInt(PIDCTR_SLOT);
                    sc.SetPersistentInt(PIDCTR_SLOT, z.ID + 1);
                    z.TopPrice    = zTop;
                    z.BottomPrice = zBot;
                    z.DetectedBar = winEnd;
                    z.ErasedBar   = -1;
                    z.IsBuyZone   = true;
                    z.TotalDelta  = -totalAbs;    // negative for long zone
                    z.Valid       = true;
                    z.Pending     = true;         // held for next-bar price confirmation
                    pZones->push_back(z);
                }
            }
        }
    }

    // =========================================================================
    // PASS 1.5 — Price-confirmation filter for pending zones
    // =========================================================================
    // A freshly detected zone is held in Pending state for one bar close.
    // On the bar immediately after detection we check whether price opened
    // through the zone — which would mean participants were NOT trapped:
    //
    //   Sell zone (pos delta near HIGH):
    //     Next bar opens ABOVE TopPrice → buyers drove price higher, no resistance
    //     → discard zone (never draw it)
    //
    //   Long zone (neg delta near LOW):
    //     Next bar opens BELOW BottomPrice → sellers drove price lower, no support
    //     → discard zone (never draw it)
    //
    // Discarded zones have Valid=false and ErasedBar=-1 (sentinel: never confirmed).
    // They are never drawn, even in Show Erased Zones mode — they were false signals.
    // If the confirmation bar has not closed yet, the zone remains Pending until
    // the next bar close.
    for (auto& z : *pZones)
    {
        if (!z.Valid || !z.Pending) continue;

        const int confirmBar = z.DetectedBar + 1;
        if (confirmBar > lastBar) continue;   // confirmation bar not closed yet — stay pending

        const float openNext = sc.Open[confirmBar];
        const bool  rejected = z.IsBuyZone
                               ? (openNext < z.BottomPrice)   // sellers drove through support
                               : (openNext > z.TopPrice);      // buyers broke out above resistance

        if (rejected)
        {
            z.Valid   = false;
            z.Pending = false;
            // ErasedBar stays -1: sentinel meaning "rejected before confirmation — never drawn"
        }
        else
        {
            z.Pending = false;   // confirmed — zone will be drawn from PASS 4 onward
        }
    }

    // =========================================================================
    // PASS 2 — Invalidate zones that price has closed through
    // =========================================================================
    // Sell zone: close > TopPrice    → buyers broke through resistance → erase
    // Long zone: close < BottomPrice → sellers broke through support   → erase
    const int curBar = sc.ArraySize - 1;   // includes the currently forming bar

    for (auto& z : *pZones)
    {
        if (!z.Valid || z.Pending) continue;  // skip erased/rejected and still-pending zones

        // Start checking from the bar after detection (or from UpdateStartIndex
        // on incremental updates, but never before DetectedBar + 1)
        int checkStart = z.DetectedBar + 1;
        if (!isFullRecalc && sc.UpdateStartIndex > checkStart)
            checkStart = sc.UpdateStartIndex;

        for (int bi = checkStart; bi <= curBar; bi++)
        {
            float cl = sc.Close[bi];
            bool clipped = z.IsBuyZone
                           ? (cl < z.BottomPrice)   // sellers broke long zone support
                           : (cl > z.TopPrice);      // buyers broke sell zone resistance

            if (clipped)
            {
                z.Valid     = false;
                z.ErasedBar = bi;

                if (!showErased)
                {
                    // Clipped Zones mode: delete drawing immediately
                    int base = z.IsBuyZone ? BASE_BUY : BASE_SELL;
                    DeleteACSILDrawing(sc, base     + z.ID);
                    DeleteACSILDrawing(sc, BASE_LBL + z.ID);
                }
                // Show Erased Zones mode: PASS 4 will redraw it frozen at ErasedBar
                break;
            }
        }
    }

    // =========================================================================
    // PASS 3 — Subgraph output (data arrays for downstream studies / alerts)
    // =========================================================================
    SG_SellDelta[curBar] = 0.0f;
    SG_BuyDelta[curBar]  = 0.0f;

    for (auto& z : *pZones)
    {
        if (!z.Valid || z.Pending) continue;
        if (!z.IsBuyZone)
            SG_SellDelta[z.DetectedBar] = z.TotalDelta;
        else
            SG_BuyDelta[z.DetectedBar]  = z.TotalDelta;
    }

    // =========================================================================
    // PASS 4 — Draw zone rectangles
    // =========================================================================
    // Active zones:  extend to curBar so they always reach the chart's right edge.
    //                PASS 4 refreshes on every bar close (no EXT variant needed).
    // Erased zones:  only drawn when Show Erased Zones is on.
    //                EndDateTime is frozen at ErasedBar (zone stopped there).
    //                Transparency is raised by 30 points to visually distinguish
    //                erased zones from active ones.
    for (auto& z : *pZones)
    {
        // Skip zones not yet confirmed by the next bar's open
        if (z.Pending) continue;
        // Skip zones rejected before confirmation (ErasedBar=-1, Valid=false)
        // These are false signals — never draw them even in Show Erased Zones mode
        if (!z.Valid && z.ErasedBar < 0) continue;
        // Skip erased zones when in Clipped Zones mode
        if (!z.Valid && !showErased) continue;

        int      base   = z.IsBuyZone ? BASE_BUY  : BASE_SELL;
        COLORREF fill   = z.IsBuyZone ? buyFill   : sellFill;
        COLORREF border = z.IsBuyZone ? buyBord   : sellBord;

        int endBar;
        int drawTransp;
        if (z.Valid)
        {
            // Active zone — tracks right edge of chart
            endBar     = curBar;
            drawTransp = transparency;
        }
        else
        {
            // Erased zone — frozen at the bar where price closed through it
            endBar     = z.ErasedBar;
            drawTransp = min(transparency + 30, 95);   // dimmer, but still visible
        }

        DrawZoneRect(sc, base + z.ID,
                     z.DetectedBar, endBar,
                     z.BottomPrice, z.TopPrice,
                     fill, border, borderWidth, drawTransp);

        // Delta label — only show for active zones (erased zones are historical noise)
        if (showDelta && z.Valid)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f", fabs(z.TotalDelta));

            s_UseTool T;
            T.Clear();
            T.ChartNumber            = sc.ChartNumber;
            T.DrawingType            = DRAWING_TEXT;
            T.BeginIndex             = z.DetectedBar;
            T.BeginValue             = z.IsBuyZone
                                       ? (double)z.BottomPrice
                                       : (double)z.TopPrice;
            T.Text                   = buf;
            T.Color                  = COLOR_WHITE;
            T.FontSize               = 8;
            T.AddMethod              = UTAM_ADD_OR_ADJUST;
            T.LineNumber             = BASE_LBL + z.ID;
            T.TransparencyLevel      = 75;
            T.AddAsUserDrawnDrawing  = 1;
            T.AllowCopyToOtherCharts = 1;
            sc.UseTool(T);
        }
        else if (!z.Valid)
        {
            // Erased zone — ensure any stale delta label is removed
            DeleteACSILDrawing(sc, BASE_LBL + z.ID);
        }
    }

    // Update the intrabar guard for the next call
    sc.SetPersistentInt(3, sc.ArraySize);

}   // end scsf_TrappedTraders
