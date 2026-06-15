// =============================================================================
// LiquidityZones.cpp
// Sierra Chart ACSIL Custom Study — OFL-style Liquidity Zone
//
// CONCEPT:
//   A liquidity zone is an area where the auction temporarily pauses and
//   potentially reverses due to the presence of a passive participant who
//   continuously refills their side of the book (iceberg / algo).
//
//   SELL ZONE  — price moves up, passive seller absorbs buyers at the top
//                → high positive delta accumulates at the top price levels
//   BUY  ZONE  — price moves down, passive buyer absorbs sellers at the bottom
//                → high negative delta accumulates at the bottom price levels
//
// ZONE STATES:
//   DETECTED   delta thresholds met on a closed bar; no push-away yet
//   ACTIVATED  price has moved away ≥ DistanceToActivate points from the zone
//   TOUCHED    price re-entered the zone after activation (retest)
//   CLIPPED    price closed through the far end (zone invalidated / erased)
//
// INPUTS (numbered to match the UI In:N labels):
//   In:1   Numbers Bars Lookback             — bars to scan each update
//   In:2   Number of Blocks Per Zone         — max contiguous price levels
//   In:3   Minimum Single Block Delta        — min abs-delta per level
//   In:4   Zone Delta Threshold              — min total abs-delta for zone
//   In:5   Distance/Heat to Activate (pts)   — price push-away required
//   In:6   (reserved / RTH end time HH*100+MM, default 1615)
//   In:7   Visible Zones (0=None, 1-5000)
//   In:8   Zone Border Width
//   In:9   Show Zone Prices (0=No, 1=Yes)
//   In:10  Zone Price Position (0=Left, 1=Right)
//   In:11  Transparency Level
//   In:12  Drawing Erasure Mode (0=Erase Invalidated, 1=Erase on Touch)
//   In:13  Drawing Overlap Mode (0=Ignore Intersecting, 1=Allow)
//   In:14  User Drawing Mode Enabled (0=No, 1=Yes)
//   In:15  Sell Zone Fill Color
//   In:16  Sell Zone Border Color
//   In:17  Buy Zone Fill Color
//   In:18  Buy Zone Border Color
//   In:19  Clipped Fill Color
//   In:20  Clipped Border Color
//   In:21  Sell Zone Touched Fill Color
//   In:22  Sell Zone Border Touched Color
//   In:23  Buy Zone Touched Fill Color
//   In:24  Buy Zone Border Touched Color
//   In:25  Sell Detection Zone Fill Color
//   In:26  Sell Detection Zone Border Color
//   In:27  Buy Detection Zone Fill Color
//   In:28  Buy Detection Zone Border Color
//   In:29  Border Line Style (0=Solid, 1=Dash, 2=Dot)
//   In:30  Resize Adjacent Intersecting Zones (0=No, 1=Yes)
//   In:31  Signal Offset (ticks) — offset zone edge for touch detection
//   In:32  Draw Activation Zone From (0=Detection bar, 1=Activation bar)
//   In:33  Detection Zone Erasure Mode (0=Erase on Activate, 1=Keep)
//   In:34  Detection Zone Visibility (0=Visible, 1=Hidden)
//   In:35  No New Zones (mins before RTH Close)
//   In:36  (reserved)
//   In:37  (reserved)
//   In:38  Show Delta Info (0=No, 1=Yes)
//   In:39  Transparent Label Background (0=No, 1=Yes)
//
// SUBGRAPHS (all Ignore — data outputs only):
//   SG1  Sell Zone Delta          SG2  Buy Zone Delta
//   SG3  Sell Zone Price          SG4  Buy Zone Price
//   SG7  Activation Sell 1st Touch
//   SG8  Activation Buy  1st Touch
//   SG11 Is Activated Buy Zone    SG12 Is Activated Sell Zone
//   SG13 Is Detected Buy Zone     SG14 Is Detected Sell Zone
//
// AutoLoop: 0  (manual loop for performance)
// =============================================================================

#include "sierrachart.h"
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdio>

using std::vector;

SCDLLName("LiquidityZones")

// =============================================================================
// Zone data structures
// =============================================================================

enum ZoneState_e : uint8_t
{
    ZS_DETECTED   = 0,
    ZS_ACTIVATED  = 1,
    ZS_TOUCHED    = 2,
    ZS_CLIPPED    = 3,
};

struct ZoneRec
{
    int         ID;            // unique sequential identifier
    float       TopPrice;
    float       BottomPrice;
    SCDateTime  DetectedDT;    // DateTime of the bar that triggered detection
    SCDateTime  ActivatedDT;   // DateTime of bar when activation occurred
    int         DetectedBar;
    int         ActivatedBar;
    bool        IsBuyZone;
    ZoneState_e State;
    float       TotalDelta;    // positive for sell zone, negative for buy zone
    bool        Valid;         // false = slot is free
    int         ClipBar;       // bar index where zone was clipped (-1 = not clipped)
};

// Drawing line-number bases (each zone uses its ID + base)
static const int BASE_SELL_ACT  = 10000;   // activated sell zone rects
static const int BASE_BUY_ACT   = 20000;   // activated buy zone rects
static const int BASE_SELL_DET  = 30000;   // detected sell zone rects
static const int BASE_BUY_DET   = 40000;   // detected buy zone rects
static const int BASE_LBL_ACT   = 50000;   // delta-info labels (activated)
static const int BASE_LBL_DET   = 60000;   // delta-info labels (detected)
static const int BASE_PRICE_LBL = 70000;   // price labels

// Persistent pointer slot
static const int PVEC_SLOT  = 1;
static const int PIDCTR_SLOT = 2;   // sc.GetPersistentInt(2) = next zone ID

// =============================================================================
// Helpers
// =============================================================================

static void DeleteACSILDrawing(SCStudyInterfaceRef sc, int lineNum)
{
    // Hide the drawing by collapsing it to a zero-size transparent rect.
    // This avoids needing the delete-by-line-number API (whose signature
    // varies across SC versions) entirely.
    if (lineNum <= 0) return;
    s_UseTool Tool;
    Tool.ChartNumber       = sc.ChartNumber;
    Tool.DrawingType       = DRAWING_RECTANGLEHIGHLIGHT;
    Tool.LineNumber        = lineNum;
    Tool.BeginDateTime     = sc.BaseDateTimeIn[0];   // minimal non-zero range
    Tool.EndDateTime       = sc.BaseDateTimeIn[0];
    Tool.BeginValue        = 0;
    Tool.EndValue          = 0;
    Tool.TransparencyLevel = 100;
    Tool.AddMethod         = UTAM_ADD_OR_ADJUST;
    sc.UseTool(Tool);
}

static void DrawZoneRect(SCStudyInterfaceRef sc,
                         int    lineNum,
                         int    startBar,
                         int    endBar,
                         float  botPrice,
                         float  topPrice,
                         COLORREF fillColor,
                         COLORREF borderColor,
                         int    borderWidth,
                         int    transparency,
                         int    /*lineStyle*/)    // reserved — SC rect border style not settable via UseTool
{
    s_UseTool Tool;
    Tool.Clear();
    Tool.ChartNumber       = sc.ChartNumber;
    Tool.DrawingType       = DRAWING_RECTANGLEHIGHLIGHT;
    Tool.BeginDateTime     = sc.BaseDateTimeIn[startBar];
    Tool.EndDateTime       = sc.BaseDateTimeIn[endBar];
    Tool.BeginValue        = (botPrice < topPrice) ? (double)botPrice : (double)topPrice;
    Tool.EndValue          = (botPrice < topPrice) ? (double)topPrice : (double)botPrice;
    Tool.Color             = borderColor; // outline color
    Tool.SecondaryColor    = fillColor;   // fill color for RECTANGLE_EXT_HIGHLIGHT
    Tool.LineWidth         = (uint16_t)borderWidth;
    Tool.TransparencyLevel = transparency;
    Tool.AddMethod         = UTAM_ADD_OR_ADJUST;
    Tool.LineNumber        = lineNum;
    sc.UseTool(Tool);
}

static void DrawTextLabel(SCStudyInterfaceRef sc,
                          int lineNum, int barIndex, float price,
                          const char* text, bool transparentBG)
{
    s_UseTool Tool;
    Tool.Clear();
    Tool.ChartNumber       = sc.ChartNumber;
    Tool.DrawingType       = DRAWING_TEXT;
    Tool.BeginIndex        = barIndex;
    Tool.BeginValue        = price;
    Tool.Text              = text;
    Tool.Color             = COLOR_WHITE;
    Tool.FontSize          = 8;
    Tool.AddMethod         = UTAM_ADD_OR_ADJUST;
    Tool.LineNumber        = lineNum;
    Tool.TransparencyLevel = transparentBG ? 75 : 0;
    sc.UseTool(Tool);
}

// Returns true if two price ranges [a0,a1] and [b0,b1] overlap
static bool PriceRangesOverlap(float a0, float a1, float b0, float b1)
{
    float aLo = (a0 < a1) ? a0 : a1;
    float aHi = (a0 < a1) ? a1 : a0;
    float bLo = (b0 < b1) ? b0 : b1;
    float bHi = (b0 < b1) ? b1 : b0;
    return aLo < bHi && bLo < aHi;
}

// =============================================================================
// Footprint scanner — finds a zone candidate at the extreme of a bar
//
// isBuyZone=true  → scan from bottom upward, looking for negative delta
// isBuyZone=false → scan from top downward, looking for positive delta
//
// Returns true if thresholds are met; fills outTop/Bottom/TotalDelta.
// =============================================================================
static bool ScanBarForZone(SCStudyInterfaceRef sc,
                            int   barIndex,
                            int   numBlocks,
                            float minBlockDelta,    // abs value
                            float zoneDeltaThresh,  // abs value
                            bool  isBuyZone,
                            float& outTop,
                            float& outBottom,
                            float& outTotalDelta)
{
    // Collect VAP entries sorted ascending by price using the correct SC API.
    // GetNextHigherVAPElement iterates low→high; start with PriceInTicks=INT_MIN
    // to begin from the lowest level.  sc.VolumeAtPriceForBars is used WITHOUT
    // subscript — the bar index is passed as a parameter to each call.
    struct Level { float Price; float Delta; };
    vector<Level> levels;
    levels.reserve(32);

    {
        int PriceInTicks = INT_MIN;
        const s_VolumeAtPriceV2* pVAP = nullptr;
        while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement(
                   (unsigned int)barIndex, PriceInTicks, &pVAP))
        {
            if (!pVAP) continue;
            float price = (float)PriceInTicks * sc.TickSize;
            float delta = (float)((int64_t)pVAP->AskVolume - (int64_t)pVAP->BidVolume);
            levels.push_back({price, delta});
        }
    }

    if (levels.empty()) return false;

    if (isBuyZone)
    {
        // Scan from lowest price upward (bottom of bar).
        // Accumulate levels whose abs-delta meets minBlockDelta.
        // We do NOT break on a failing level — a low-delta tick between two
        // high-delta ticks at the extreme is still part of the absorption zone.
        // scanLimit caps the search to the bar's lowest numBlocks*3 levels so we
        // cannot accidentally match absorption buried in the middle of the bar.
        float totalAbs = 0.0f;
        int   count    = 0;
        float minP     = 1e30f, maxP = -1e30f;
        // Scan window: bottom numBlocks*3 levels from the bar's price floor.
        // No count-based stop — accumulate ALL qualifying blocks in the window;
        // numBlocks is a MINIMUM requirement, not a cap.
        int   maxIdx   = numBlocks * 3;
        if (maxIdx > (int)levels.size()) maxIdx = (int)levels.size();

        for (int i = 0; i < maxIdx; i++)
        {
            float absDelta = -levels[i].Delta;   // negative delta → positive abs
            if (absDelta < minBlockDelta) continue;  // level doesn't meet per-block minimum

            if (levels[i].Price < minP) minP = levels[i].Price;
            if (levels[i].Price > maxP) maxP = levels[i].Price;

            totalAbs += absDelta;
            count++;
        }

        // Require AT LEAST numBlocks qualifying blocks (Input 2) AND total >= threshold (Input 4)
        if (count >= numBlocks && totalAbs >= zoneDeltaThresh)
        {
            // The negative-delta cluster must be at the bar's price extreme (bar Low).
            // If the lowest qualifying level is more than 2 ticks above the bar's Low,
            // this is mid-bar absorption, not a bottom-extreme buy zone.
            if (minP > sc.Low[barIndex] + sc.TickSize * 2.0f)
                return false;

            outBottom     = minP;
            outTop        = maxP;
            outTotalDelta = -totalAbs;  // negative for buy zone
            return true;
        }
    }
    else
    {
        // Scan from highest price downward (top of bar).
        // Accumulate levels whose delta meets minBlockDelta.
        // Same reasoning — do not break on a sparse tick between absorptions.
        // scanLimit caps the search to the bar's highest numBlocks*3 levels.
        float total  = 0.0f;
        int   count  = 0;
        float minP   = 1e30f, maxP = -1e30f;
        // Scan window: top numBlocks*3 levels from the bar's price ceiling.
        // No count-based stop — accumulate ALL qualifying blocks in the window.
        int   minIdx = (int)levels.size() - numBlocks * 3;
        if (minIdx < 0) minIdx = 0;

        for (int i = (int)levels.size() - 1; i >= minIdx; i--)
        {
            float d = levels[i].Delta;
            if (d < minBlockDelta) continue;  // level doesn't meet per-block minimum

            if (levels[i].Price < minP) minP = levels[i].Price;
            if (levels[i].Price > maxP) maxP = levels[i].Price;

            total += d;
            count++;
        }

        // Require AT LEAST numBlocks qualifying blocks (Input 2) AND total >= threshold (Input 4)
        if (count >= numBlocks && total >= zoneDeltaThresh)
        {
            // The positive-delta cluster must be at the bar's price extreme (bar High).
            // If the highest qualifying level is more than 2 ticks below the bar's High,
            // this is mid-bar absorption, not a top-extreme sell zone.
            if (maxP < sc.High[barIndex] - sc.TickSize * 2.0f)
                return false;

            outBottom     = minP;
            outTop        = maxP;
            outTotalDelta = total;       // positive for sell zone
            return true;
        }
    }

    return false;
}

// =============================================================================
// Main study function
// =============================================================================
SCSFExport scsf_LiquidityZones(SCStudyInterfaceRef sc)
{
    // ── Subgraph references ─────────────────────────────────────────────────
    SCSubgraphRef SG_SellDelta       = sc.Subgraph[0];   // SG1
    SCSubgraphRef SG_BuyDelta        = sc.Subgraph[1];   // SG2
    SCSubgraphRef SG_SellPrice       = sc.Subgraph[2];   // SG3
    SCSubgraphRef SG_BuyPrice        = sc.Subgraph[3];   // SG4
    // SG5, SG6 unused (skip to match OFL numbering)
    SCSubgraphRef SG_ActSell1T       = sc.Subgraph[6];   // SG7
    SCSubgraphRef SG_ActBuy1T        = sc.Subgraph[7];   // SG8
    // SG9, SG10 unused
    SCSubgraphRef SG_IsActBuy        = sc.Subgraph[10];  // SG11
    SCSubgraphRef SG_IsActSell       = sc.Subgraph[11];  // SG12
    SCSubgraphRef SG_IsDetBuy        = sc.Subgraph[12];  // SG13
    SCSubgraphRef SG_IsDetSell       = sc.Subgraph[13];  // SG14

    // ── Input references ────────────────────────────────────────────────────
    SCInputRef In_BarsLookback       = sc.Input[0];   // In:1
    SCInputRef In_BlocksPerZone      = sc.Input[1];   // In:2
    SCInputRef In_MinBlockDelta      = sc.Input[2];   // In:3
    SCInputRef In_ZoneDeltaThresh    = sc.Input[3];   // In:4
    SCInputRef In_DistanceToActivate = sc.Input[4];   // In:5
    SCInputRef In_RTHCloseTime       = sc.Input[5];   // In:6  (HHMM, e.g. 1615)
    SCInputRef In_VisibleZones       = sc.Input[6];   // In:7
    SCInputRef In_BorderWidth        = sc.Input[7];   // In:8
    SCInputRef In_ShowPrices         = sc.Input[8];   // In:9
    SCInputRef In_PricePosition      = sc.Input[9];   // In:10
    SCInputRef In_Transparency       = sc.Input[10];  // In:11
    SCInputRef In_ErasureMode        = sc.Input[11];  // In:12
    SCInputRef In_OverlapMode        = sc.Input[12];  // In:13
    SCInputRef In_UserDrawMode       = sc.Input[13];  // In:14
    SCInputRef In_SellColor          = sc.Input[14];  // In:15
    SCInputRef In_SellBorderColor    = sc.Input[15];  // In:16
    SCInputRef In_BuyColor           = sc.Input[16];  // In:17
    SCInputRef In_BuyBorderColor     = sc.Input[17];  // In:18
    SCInputRef In_ClipColor          = sc.Input[18];  // In:19
    SCInputRef In_ClipBorderColor    = sc.Input[19];  // In:20
    SCInputRef In_SellTouchColor     = sc.Input[20];  // In:21
    SCInputRef In_SellTouchBorder    = sc.Input[21];  // In:22
    SCInputRef In_BuyTouchColor      = sc.Input[22];  // In:23
    SCInputRef In_BuyTouchBorder     = sc.Input[23];  // In:24
    SCInputRef In_SellDetColor       = sc.Input[24];  // In:25
    SCInputRef In_SellDetBorder      = sc.Input[25];  // In:26
    SCInputRef In_BuyDetColor        = sc.Input[26];  // In:27
    SCInputRef In_BuyDetBorder       = sc.Input[27];  // In:28
    SCInputRef In_BorderStyle        = sc.Input[28];  // In:29
    SCInputRef In_ResizeAdjacent     = sc.Input[29];  // In:30
    SCInputRef In_SignalOffset       = sc.Input[30];  // In:31
    SCInputRef In_DrawFrom           = sc.Input[31];  // In:32
    SCInputRef In_DetErasureMode     = sc.Input[32];  // In:33
    SCInputRef In_DetVisibility      = sc.Input[33];  // In:34
    SCInputRef In_NoZoneMins         = sc.Input[34];  // In:35
    // In:36, In:37 reserved
    SCInputRef In_ShowDeltaInfo      = sc.Input[37];  // In:38
    SCInputRef In_TransparentLabels  = sc.Input[38];  // In:39

    // ========================================================================
    // SetDefaults
    // ========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName             = "Liquidity Zones";
        sc.StudyDescription      = "OFL-style liquidity zone detection. Finds areas of "
                                   "passive participant absorption on footprint charts.";
        sc.AutoLoop              = 0;
        sc.GraphRegion           = 0;
        sc.ScaleRangeType        = SCALE_SAMEASREGION;
        sc.DrawZeros             = 0;
        sc.MaintainVolumeAtPriceData = 1;   // REQUIRED for footprint/VAP data

        // ── Subgraph defaults ──
        auto SetIgnore = [](SCSubgraphRef& sg, const char* name)
        {
            sg.Name        = name;
            sg.DrawStyle   = DRAWSTYLE_IGNORE;
            sg.PrimaryColor = COLOR_WHITE;
            sg.DrawZeros   = 0;
        };

        SetIgnore(SG_SellDelta,  "Sell Zone Delta");
        SetIgnore(SG_BuyDelta,   "Buy Zone Delta");
        SetIgnore(SG_SellPrice,  "Sell Zone Price");
        SetIgnore(SG_BuyPrice,   "Buy Zone Price");
        SetIgnore(SG_ActSell1T,  "Activation Sell Zone 1st Touch");
        SetIgnore(SG_ActBuy1T,   "Activation Buy Zone 1st Touch");
        SetIgnore(SG_IsActBuy,   "Is Activated Buy Zone");
        SetIgnore(SG_IsActSell,  "Is Activated Sell Zone");
        SetIgnore(SG_IsDetBuy,   "Is Detected Buy Zone");
        SetIgnore(SG_IsDetSell,  "Is Detected Sell Zone");

        // ── Inputs ──
        In_BarsLookback.Name       = "Numbers Bars Lookback";
        In_BarsLookback.SetInt(3);

        In_BlocksPerZone.Name      = "Number of Blocks Per Zone";
        In_BlocksPerZone.SetInt(4);

        In_MinBlockDelta.Name      = "Minimum Single Block Delta";
        In_MinBlockDelta.SetFloat(100.0f);

        In_ZoneDeltaThresh.Name    = "Zone Delta Threshold";
        In_ZoneDeltaThresh.SetFloat(350.0f);

        In_DistanceToActivate.Name = "Distance/Heat from Zone to Activate (points)";
        In_DistanceToActivate.SetFloat(5.0f);

        In_RTHCloseTime.Name       = "RTH Close Time (HHMM)";
        In_RTHCloseTime.SetInt(1615);

        In_VisibleZones.Name       = "Visible Zones (0==None, 1-5000)";
        In_VisibleZones.SetInt(100);

        In_BorderWidth.Name        = "Zone Border Width";
        In_BorderWidth.SetInt(1);

        In_ShowPrices.Name         = "Show Zone Prices (0=None, 1=Show)";
        In_ShowPrices.SetYesNo(0);

        In_PricePosition.Name      = "Zone Price Position (0=Left, 1=Right)";
        In_PricePosition.SetInt(0);

        In_Transparency.Name       = "Transparency Level";
        In_Transparency.SetInt(50);

        In_ErasureMode.Name        = "Drawing Erasure Mode";
        In_ErasureMode.SetCustomInputStrings("Erase Invalidated;Clip to Break Bar");
        In_ErasureMode.SetCustomInputIndex(0);

        In_OverlapMode.Name        = "Drawing Overlap Mode (0=Ignore Intersecting, 1=Allow)";
        In_OverlapMode.SetInt(0);

        In_UserDrawMode.Name       = "User Drawing Mode Enabled";
        In_UserDrawMode.SetYesNo(1);

        In_SellColor.Name          = "Sell Zone Color";
        In_SellColor.SetColor(RGB(243, 54, 73));

        In_SellBorderColor.Name    = "Sell Zone Border Color";
        In_SellBorderColor.SetColor(RGB(92, 46, 46));

        In_BuyColor.Name           = "Buy Zone Color";
        In_BuyColor.SetColor(RGB(44, 56, 86));

        In_BuyBorderColor.Name     = "Buy Zone Border Color";
        In_BuyBorderColor.SetColor(RGB(0, 102, 204));

        In_ClipColor.Name          = "Clipped Color";
        In_ClipColor.SetColor(RGB(192, 192, 192));

        In_ClipBorderColor.Name    = "Clipped Border Color";
        In_ClipBorderColor.SetColor(RGB(192, 192, 192));

        In_SellTouchColor.Name     = "Sell Zone Color Touched";
        In_SellTouchColor.SetColor(RGB(37, 13, 48));

        In_SellTouchBorder.Name    = "Sell Zone Border Color Touched";
        In_SellTouchBorder.SetColor(RGB(208, 87, 87));

        In_BuyTouchColor.Name      = "Buy Zone Color Touched";
        In_BuyTouchColor.SetColor(RGB(37, 40, 48));

        In_BuyTouchBorder.Name     = "Buy Zone Border Color Touched";
        In_BuyTouchBorder.SetColor(RGB(62, 158, 255));

        In_SellDetColor.Name       = "Sell Detection Zone Color";
        In_SellDetColor.SetColor(RGB(54, 69, 105));

        In_SellDetBorder.Name      = "Sell Detection Zone Border Color";
        In_SellDetBorder.SetColor(RGB(37, 40, 48));

        In_BuyDetColor.Name        = "Buy Detection Zone Color";
        In_BuyDetColor.SetColor(RGB(64, 68, 87));

        In_BuyDetBorder.Name       = "Buy Detection Zone Border Color";
        In_BuyDetBorder.SetColor(RGB(37, 40, 48));

        In_BorderStyle.Name        = "Border Line Style (0=Solid, 1=Dash, 2=Dot)";
        In_BorderStyle.SetInt(0);

        In_ResizeAdjacent.Name     = "Resize Adjacent Intersecting Zones";
        In_ResizeAdjacent.SetYesNo(1);

        In_SignalOffset.Name       = "Signal Offset (ticks)";
        In_SignalOffset.SetInt(1);

        In_DrawFrom.Name           = "Draw Activation Zone From (0=Detection, 1=Activation)";
        In_DrawFrom.SetInt(1);   // default: draw from activation bar

        In_DetErasureMode.Name     = "Detection Zone Erasure Mode (0=Erase on Activate, 1=Keep)";
        In_DetErasureMode.SetInt(0);

        In_DetVisibility.Name      = "Detection Zone Visibility (0=Visible, 1=Hidden)";
        In_DetVisibility.SetInt(1);   // hidden by default

        In_NoZoneMins.Name         = "No New Zones (mins before RTH Close)";
        In_NoZoneMins.SetInt(15);

        In_ShowDeltaInfo.Name      = "Show Delta Info";
        In_ShowDeltaInfo.SetYesNo(1);

        In_TransparentLabels.Name  = "Transparent Label Background";
        In_TransparentLabels.SetYesNo(1);

        return;
    }

    // ========================================================================
    // Cleanup on study removal
    // ========================================================================
    if (sc.LastCallToFunction)
    {
        vector<ZoneRec>* pZones =
            reinterpret_cast<vector<ZoneRec>*>(sc.GetPersistentPointer(PVEC_SLOT));
        if (pZones)
        {
            for (auto& z : *pZones)
            {
                if (!z.Valid) continue;
                bool isSell = !z.IsBuyZone;
                int actBase = isSell ? BASE_SELL_ACT : BASE_BUY_ACT;
                int detBase = isSell ? BASE_SELL_DET : BASE_BUY_DET;
                DeleteACSILDrawing(sc, actBase + z.ID);
                DeleteACSILDrawing(sc, detBase + z.ID);
                DeleteACSILDrawing(sc, BASE_LBL_ACT + z.ID);
                DeleteACSILDrawing(sc, BASE_LBL_DET + z.ID);
                DeleteACSILDrawing(sc, BASE_PRICE_LBL + z.ID);
            }
            delete pZones;
            sc.SetPersistentPointer(PVEC_SLOT, nullptr);
        }
        return;
    }

    // ========================================================================
    // Read inputs into locals for convenience
    // ========================================================================
    const int   barsLookback    = In_BarsLookback.GetInt();
    const int   blocksPerZone   = In_BlocksPerZone.GetInt();
    const float minBlockDelta   = In_MinBlockDelta.GetFloat();
    const float zoneDeltaThresh = In_ZoneDeltaThresh.GetFloat();
    const float distancePoints  = In_DistanceToActivate.GetFloat();
    const int   rthCloseHHMM    = In_RTHCloseTime.GetInt();       // e.g. 1615
    const int   visibleZones    = In_VisibleZones.GetInt();
    const int   borderWidth     = In_BorderWidth.GetInt();
    const int   showPrices      = In_ShowPrices.GetYesNo();
    const int   transparency    = In_Transparency.GetInt();
    const int   erasureMode     = In_ErasureMode.GetIndex();
    const int   overlapMode     = In_OverlapMode.GetInt();
    const COLORREF sellColor    = In_SellColor.GetColor();
    const COLORREF sellBorder   = In_SellBorderColor.GetColor();
    const COLORREF buyColor     = In_BuyColor.GetColor();
    const COLORREF buyBorder    = In_BuyBorderColor.GetColor();
    const COLORREF clipColor    = In_ClipColor.GetColor();
    const COLORREF clipBorder   = In_ClipBorderColor.GetColor();
    const COLORREF sellTouchC   = In_SellTouchColor.GetColor();
    const COLORREF sellTouchB   = In_SellTouchBorder.GetColor();
    const COLORREF buyTouchC    = In_BuyTouchColor.GetColor();
    const COLORREF buyTouchB    = In_BuyTouchBorder.GetColor();
    const COLORREF sellDetC     = In_SellDetColor.GetColor();
    const COLORREF sellDetB     = In_SellDetBorder.GetColor();
    const COLORREF buyDetC      = In_BuyDetColor.GetColor();
    const COLORREF buyDetB      = In_BuyDetBorder.GetColor();
    const int   borderStyle     = In_BorderStyle.GetInt();
    const int   signalOffsetTks = In_SignalOffset.GetInt();
    const int   drawFromMode    = In_DrawFrom.GetInt();           // 0=detection, 1=activation
    const int   detErasureMode  = In_DetErasureMode.GetInt();
    const int   detVisibility   = In_DetVisibility.GetInt();
    const int   noZoneMins      = In_NoZoneMins.GetInt();
    const int   showDeltaInfo   = In_ShowDeltaInfo.GetYesNo();
    const int   transLabels     = In_TransparentLabels.GetYesNo();

    // Derived
    const float offsetPrice     = signalOffsetTks * sc.TickSize;

    // Line style mapping
    int scLineStyle = 0;   // 0=solid; passed to DrawZoneRect but not used for rect border
    if (borderStyle == 1) scLineStyle = 1;
    else if (borderStyle == 2) scLineStyle = 2;

    // ========================================================================
    // Persistent zone vector
    // ========================================================================
    vector<ZoneRec>* pZones =
        reinterpret_cast<vector<ZoneRec>*>(sc.GetPersistentPointer(PVEC_SLOT));

    bool isFullRecalc = (sc.UpdateStartIndex == 0);

    if (isFullRecalc || pZones == nullptr)
    {
        // Delete all existing drawings if we had zones
        if (pZones)
        {
            for (auto& z : *pZones)
            {
                if (!z.Valid) continue;
                bool isSell = !z.IsBuyZone;
                DeleteACSILDrawing(sc, (isSell ? BASE_SELL_ACT : BASE_BUY_ACT) + z.ID);
                DeleteACSILDrawing(sc, (isSell ? BASE_SELL_DET : BASE_BUY_DET) + z.ID);
                DeleteACSILDrawing(sc, BASE_LBL_ACT + z.ID);
                DeleteACSILDrawing(sc, BASE_LBL_DET + z.ID);
                DeleteACSILDrawing(sc, BASE_PRICE_LBL + z.ID);
            }
            delete pZones;
        }
        pZones = new vector<ZoneRec>();
        sc.SetPersistentPointer(PVEC_SLOT, pZones);
        sc.SetPersistentInt(PIDCTR_SLOT, 1);
    }

    // ========================================================================
    // Skip intrabar ticks — only process on bar close / full recalc
    // ========================================================================
    // On full recalc, sc.UpdateStartIndex == 0 and we process all bars.
    // On incremental, sc.UpdateStartIndex points to the last closed bar.
    // The forming bar is sc.ArraySize - 1; we never scan it.
    if (!isFullRecalc)
    {
        // Only run when a new bar has closed.
        // Persistent slot 3 stores the array size from the last processed call.
        int prevSize = sc.GetPersistentInt(3);
        int curSize  = sc.ArraySize;
        if (curSize == prevSize) return;   // intrabar tick — nothing to do
        sc.SetPersistentInt(3, curSize);
    }

    const int lastClosedBar = sc.ArraySize - 2;  // current forming bar excluded
    if (lastClosedBar < 0) return;

    // ========================================================================
    // RTH close guard — no new zones within noZoneMins of RTH close
    // ========================================================================
    // Convert HHMM to total minutes: 1615 → 16*60+15 = 975
    int rthH   = rthCloseHHMM / 100;
    int rthM   = rthCloseHHMM % 100;
    int rthMin = rthH * 60 + rthM;

    auto IsNearRTHClose = [&](int barIdx) -> bool
    {
        if (noZoneMins <= 0) return false;
        SCDateTime dt = sc.BaseDateTimeIn[barIdx];
        int h  = dt.GetHour();
        int m  = dt.GetMinute();
        int bm = h * 60 + m;
        return (bm >= (rthMin - noZoneMins) && bm < rthMin);
    };

    // ========================================================================
    // PASS 1 — Scan bars for new zone candidates
    // ========================================================================
    // IMPORTANT: scan NEWEST→OLDEST so that the visibleZones cap fills with
    // the most-recent zones first.  Scanning oldest→newest caused the cap to
    // fill with off-screen historical zones, leaving the visible chart empty.
    //
    // Full recalc:   scan lastClosedBar → 0
    // Incremental:   scan lastClosedBar → lastClosedBar - barsLookback + 1
    int scanFloor = isFullRecalc ? 0 : (lastClosedBar - barsLookback + 1);
    if (scanFloor < 0) scanFloor = 0;

    // Guard: VAP data might not be ready on this chart type.
    // This study requires a Numbers Bar / Footprint chart with
    // sc.MaintainVolumeAtPriceData = 1.
    if (sc.VolumeAtPriceForBars != nullptr)
    {
    // ── Diagnostic log (full recalc only) ────────────────────────────────
    // Check the most-recent closed bar and report VAP level count and the
    // maximum absolute delta seen.  Read these in SC's Message Log window.
    if (isFullRecalc && lastClosedBar >= 0)
    {
        int   vapCount    = 0;
        float maxAbsDelta = 0.0f;
        int   PIT2        = INT_MIN;
        const s_VolumeAtPriceV2* pV2 = nullptr;
        while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement(
                   (unsigned int)lastClosedBar, PIT2, &pV2))
        {
            if (!pV2) continue;
            vapCount++;
            float ad = (float)((int64_t)pV2->AskVolume - (int64_t)pV2->BidVolume);
            if (ad < 0) ad = -ad;
            if (ad > maxAbsDelta) maxAbsDelta = ad;
        }
        char dbuf[256];
        snprintf(dbuf, sizeof(dbuf),
                 "LiqZones DIAG: bar %d vapLevels=%d maxAbsDelta=%.0f "
                 "| minBlockDelta=%.0f zoneDeltaThresh=%.0f",
                 lastClosedBar, vapCount, maxAbsDelta,
                 minBlockDelta, zoneDeltaThresh);
        sc.AddMessageToLog(dbuf, 0);
    }

    // Track which bars already have a zone to avoid duplicates
    auto BarAlreadyHasZone = [&](int barIdx, bool isBuyZone) -> bool
    {
        for (auto& z : *pZones)
        {
            if (!z.Valid) continue;
            if (z.DetectedBar == barIdx && z.IsBuyZone == isBuyZone) return true;
        }
        return false;
    };

    for (int bi = lastClosedBar; bi >= scanFloor; bi--)
    {
        if (IsNearRTHClose(bi)) continue;

        // Limit total active zones to visibleZones
        int activeCount = 0;
        for (auto& z : *pZones) if (z.Valid) activeCount++;
        if (visibleZones > 0 && activeCount >= visibleZones) break;

        // --- Try sell zone ---
        if (!BarAlreadyHasZone(bi, false))
        {
            float zTop = 0, zBot = 0, zDelta = 0;
            bool found = ScanBarForZone(sc, bi, blocksPerZone,
                                        minBlockDelta, zoneDeltaThresh,
                                        false, zTop, zBot, zDelta);
            if (found)
            {
                // Next-bar price direction confirmation.
                // Sell zone: passive sellers absorbed buyers at the bar top.
                // If the very next bar opens ABOVE the zone top, price continued
                // upward — the "absorption" was not rejection, it was continuation.
                // Skip this zone entirely.
                int nextBar = bi + 1;
                if (nextBar < sc.ArraySize && sc.Open[nextBar] > zTop)
                    found = false;
            }
            if (found)
            {
                // Check overlap with existing zones (Ignore Intersecting mode)
                bool blocked = false;
                if (overlapMode == 0)
                {
                    for (auto& ez : *pZones)
                    {
                        if (!ez.Valid) continue;
                        if (PriceRangesOverlap(zBot, zTop, ez.BottomPrice, ez.TopPrice))
                        { blocked = true; break; }
                    }
                }

                if (!blocked)
                {
                    ZoneRec z;
                    z.ID           = sc.GetPersistentInt(PIDCTR_SLOT);
                    sc.SetPersistentInt(PIDCTR_SLOT, z.ID + 1);
                    z.TopPrice     = zTop;
                    z.BottomPrice  = zBot;
                    z.DetectedDT   = sc.BaseDateTimeIn[bi];
                    z.ActivatedDT  = z.DetectedDT;
                    z.DetectedBar  = bi;
                    z.ActivatedBar = bi;
                    z.IsBuyZone    = false;
                    z.State        = ZS_DETECTED;
                    z.TotalDelta   = zDelta;
                    z.Valid        = true;
                    z.ClipBar      = -1;
                    pZones->push_back(z);
                }
            }
        }

        // --- Try buy zone ---
        if (!BarAlreadyHasZone(bi, true))
        {
            float zTop = 0, zBot = 0, zDelta = 0;
            bool found = ScanBarForZone(sc, bi, blocksPerZone,
                                        minBlockDelta, zoneDeltaThresh,
                                        true, zTop, zBot, zDelta);
            if (found)
            {
                // Next-bar price direction confirmation.
                // Buy zone: passive buyers absorbed sellers at the bar bottom.
                // If the very next bar opens BELOW the zone bottom, price continued
                // downward — the "absorption" was not rejection, it was continuation.
                // Skip this zone entirely.
                int nextBar = bi + 1;
                if (nextBar < sc.ArraySize && sc.Open[nextBar] < zBot)
                    found = false;
            }
            if (found)
            {
                bool blocked = false;
                if (overlapMode == 0)
                {
                    for (auto& ez : *pZones)
                    {
                        if (!ez.Valid) continue;
                        if (PriceRangesOverlap(zBot, zTop, ez.BottomPrice, ez.TopPrice))
                        { blocked = true; break; }
                    }
                }

                if (!blocked)
                {
                    ZoneRec z;
                    z.ID           = sc.GetPersistentInt(PIDCTR_SLOT);
                    sc.SetPersistentInt(PIDCTR_SLOT, z.ID + 1);
                    z.TopPrice     = zTop;
                    z.BottomPrice  = zBot;
                    z.DetectedDT   = sc.BaseDateTimeIn[bi];
                    z.ActivatedDT  = z.DetectedDT;
                    z.DetectedBar  = bi;
                    z.ActivatedBar = bi;
                    z.IsBuyZone    = true;
                    z.State        = ZS_DETECTED;
                    z.TotalDelta   = zDelta;
                    z.Valid        = true;
                    z.ClipBar      = -1;
                    pZones->push_back(z);
                }
            }
        }
    }
    // ── Post-scan diagnostic (full recalc only) ──────────────────────────
    if (isFullRecalc)
    {
        int cnt = 0;
        for (auto& z2 : *pZones) if (z2.Valid) cnt++;
        char buf2[128];
        snprintf(buf2, sizeof(buf2),
                 "LiqZones DIAG: PASS1 done — %d zones detected (scanned bars %d→%d)",
                 cnt, lastClosedBar, scanFloor);
        sc.AddMessageToLog(buf2, 0);
    }
    }   // end if (sc.VolumeAtPriceForBars != nullptr)

    // ========================================================================
    // PASS 2 — Update zone states based on current price action
    // ========================================================================
    // We check bars from each zone's detection bar forward to the last bar.
    // This is done on the forming bar too (lastClosedBar + 1 = sc.ArraySize-1)
    // so the chart shows real-time touches.
    const int currentBar   = sc.ArraySize - 1;
    const float currentClose = sc.Close[currentBar];
    const float currentHigh  = sc.High[currentBar];
    const float currentLow   = sc.Low[currentBar];

    for (auto& z : *pZones)
    {
        if (!z.Valid) continue;
        if (z.State == ZS_CLIPPED) continue;  // nothing more to do

        // On full recalc scan from the detection bar forward.
        // On incremental: newly detected zones need a full scan from detection;
        // already-activated/touched zones only need to check new bars.
        int checkStart = z.DetectedBar + 1;
        if (!isFullRecalc && z.State != ZS_DETECTED)
            checkStart = sc.UpdateStartIndex;

        for (int bi = checkStart; bi <= currentBar; bi++)
        {
            float hi = sc.High[bi];
            float lo = sc.Low[bi];
            float cl = sc.Close[bi];

            // ── DETECTED → ACTIVATED or INVALIDATED ─────────────────────────
            if (z.State == ZS_DETECTED)
            {
                if (!z.IsBuyZone)
                {
                    // Sell zone (positive delta at bar top — passive seller absorbed buyers).
                    // Confirmed when price DROPS below zone bottom by ≥ distancePoints.
                    // Invalidated immediately if price closes ABOVE the zone top
                    // (buyers broke through — absorption failed).
                    if (cl <= z.BottomPrice - distancePoints)
                    {
                        z.State        = ZS_ACTIVATED;
                        z.ActivatedDT  = sc.BaseDateTimeIn[bi];
                        z.ActivatedBar = bi;
                        if (detErasureMode == 0)
                            DeleteACSILDrawing(sc, BASE_SELL_DET + z.ID);
                    }
                    else if (cl > z.TopPrice)
                    {
                        // Price closed above the sell zone — zone is invalid, erase it
                        z.Valid = false;
                        DeleteACSILDrawing(sc, BASE_SELL_DET + z.ID);
                    }
                }
                else
                {
                    // Buy zone (negative delta at bar bottom — passive buyer absorbed sellers).
                    // Confirmed when price RISES above zone top by ≥ distancePoints.
                    // Invalidated immediately if price closes BELOW the zone bottom
                    // (sellers broke through — absorption failed).
                    if (cl >= z.TopPrice + distancePoints)
                    {
                        z.State        = ZS_ACTIVATED;
                        z.ActivatedDT  = sc.BaseDateTimeIn[bi];
                        z.ActivatedBar = bi;
                        if (detErasureMode == 0)
                            DeleteACSILDrawing(sc, BASE_BUY_DET + z.ID);
                    }
                    else if (cl < z.BottomPrice)
                    {
                        // Price closed below the buy zone — zone is invalid, erase it
                        z.Valid = false;
                        DeleteACSILDrawing(sc, BASE_BUY_DET + z.ID);
                    }
                }
                continue;  // don't check touch/clip on same iteration
            }

            // ── ACTIVATED → TOUCHED ─────────────────────────────────────────
            if (z.State == ZS_ACTIVATED)
            {
                float zoneTop = z.TopPrice + offsetPrice;
                float zoneBot = z.BottomPrice - offsetPrice;

                // Price re-enters zone
                bool entered = (hi >= zoneBot && lo <= zoneTop);
                if (entered)
                {
                    z.State = ZS_TOUCHED;
                    // Signal subgraphs on first touch bar
                    if (!z.IsBuyZone)
                        SG_ActSell1T[bi] = z.TopPrice;
                    else
                        SG_ActBuy1T[bi]  = z.BottomPrice;
                }
                // fall through: also check clip on same bar
            }

            // ── TOUCHED → check clip ─────────────────────────────────────────
            if (z.State == ZS_TOUCHED || z.State == ZS_ACTIVATED)
            {
                bool clipped = false;

                if (!z.IsBuyZone)
                {
                    // Sell zone clipped if close ABOVE the zone top
                    if (cl > z.TopPrice + offsetPrice) clipped = true;
                }
                else
                {
                    // Buy zone clipped if close BELOW the zone bottom
                    if (cl < z.BottomPrice - offsetPrice) clipped = true;
                }

                if (clipped)
                {
                    if (erasureMode == 0)
                    {
                        // Erase Invalidated: zone disappears — delete drawing and free slot
                        z.Valid = false;
                        int actBase = !z.IsBuyZone ? BASE_SELL_ACT : BASE_BUY_ACT;
                        DeleteACSILDrawing(sc, actBase + z.ID);
                        DeleteACSILDrawing(sc, BASE_LBL_ACT + z.ID);
                        DeleteACSILDrawing(sc, BASE_PRICE_LBL + z.ID);
                    }
                    else
                    {
                        // Clip to Break Bar: rectangle right edge snaps to the break
                        // bar itself (the candle that invalidated the zone).
                        z.State   = ZS_CLIPPED;
                        z.ClipBar = bi;
                    }
                }
            }
        }
    }

    // ========================================================================
    // PASS 3 — Write subgraph arrays for current bar
    // ========================================================================
    // Zero out current bar first
    SG_SellDelta[currentBar]  = 0;
    SG_BuyDelta[currentBar]   = 0;
    SG_SellPrice[currentBar]  = 0;
    SG_BuyPrice[currentBar]   = 0;
    SG_IsActBuy[currentBar]   = 0;
    SG_IsActSell[currentBar]  = 0;
    SG_IsDetBuy[currentBar]   = 0;
    SG_IsDetSell[currentBar]  = 0;

    for (auto& z : *pZones)
    {
        if (!z.Valid) continue;
        bool activated = (z.State == ZS_ACTIVATED || z.State == ZS_TOUCHED);

        if (!z.IsBuyZone)
        {
            SG_SellDelta[z.DetectedBar] = z.TotalDelta;
            SG_SellPrice[z.DetectedBar] = z.TopPrice;
            if (activated) SG_IsActSell[currentBar] = 1;
            else           SG_IsDetSell[currentBar]  = 1;
        }
        else
        {
            SG_BuyDelta[z.DetectedBar]  = z.TotalDelta;
            SG_BuyPrice[z.DetectedBar]  = z.BottomPrice;
            if (activated) SG_IsActBuy[currentBar] = 1;
            else           SG_IsDetBuy[currentBar]  = 1;
        }
    }

    // ========================================================================
    // PASS 4 — Draw / update zone rectangles
    // ========================================================================
    int drawnCount = 0;

    for (auto& z : *pZones)
    {
        if (!z.Valid) continue;
        if (visibleZones > 0 && drawnCount >= visibleZones) break;

        bool isSell = !z.IsBuyZone;

        // ── Decide which bar to use as the left edge ──────────────────────
        int leftBar = (drawFromMode == 1 && z.State != ZS_DETECTED)
                    ? z.ActivatedBar
                    : z.DetectedBar;

        // ── Detection-phase drawing ───────────────────────────────────────
        if (z.State == ZS_DETECTED)
        {
            int detBase  = isSell ? BASE_SELL_DET : BASE_BUY_DET;
            COLORREF dFill   = isSell ? sellDetC : buyDetC;
            COLORREF dBorder = isSell ? sellDetB : buyDetB;
            int detTrans = (detVisibility == 1) ? 99 : transparency;

            DrawZoneRect(sc, detBase + z.ID, leftBar, sc.ArraySize - 1,
                         z.BottomPrice, z.TopPrice,
                         dFill, dBorder, borderWidth, detTrans, scLineStyle);

            if (showDeltaInfo)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.0f", std::abs(z.TotalDelta));
                DrawTextLabel(sc, BASE_LBL_DET + z.ID, leftBar,
                              isSell ? z.TopPrice : z.BottomPrice,
                              buf, transLabels != 0);
            }
            drawnCount++;
            continue;
        }

        // ── Activated / Touched drawing ───────────────────────────────────
        if (z.State == ZS_ACTIVATED || z.State == ZS_TOUCHED)
        {
            int actBase = isSell ? BASE_SELL_ACT : BASE_BUY_ACT;
            COLORREF fill, border;

            if (z.State == ZS_TOUCHED)
            {
                fill   = isSell ? sellTouchC : buyTouchC;
                border = isSell ? sellTouchB : buyTouchB;
            }
            else
            {
                fill   = isSell ? sellColor : buyColor;
                border = isSell ? sellBorder : buyBorder;
            }

            DrawZoneRect(sc, actBase + z.ID, leftBar, sc.ArraySize - 1,
                         z.BottomPrice, z.TopPrice,
                         fill, border, borderWidth, transparency, scLineStyle);

            if (showDeltaInfo)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.0f", std::abs(z.TotalDelta));
                DrawTextLabel(sc, BASE_LBL_ACT + z.ID, leftBar,
                              isSell ? z.TopPrice : z.BottomPrice,
                              buf, transLabels != 0);
            }

            if (showPrices)
            {
                char pbuf[48];
                snprintf(pbuf, sizeof(pbuf), "%.2f - %.2f",
                         z.BottomPrice, z.TopPrice);
                int priceBar = (In_PricePosition.GetInt() == 0)
                             ? leftBar
                             : currentBar;
                DrawTextLabel(sc, BASE_PRICE_LBL + z.ID, priceBar,
                              (z.BottomPrice + z.TopPrice) * 0.5f,
                              pbuf, transLabels != 0);
            }

            drawnCount++;
            continue;
        }

        // ── Clipped drawing (mode 1 — Clip to Break Bar) ─────────────────
        // Mode 0 (Erase Invalidated) sets z.Valid=false so we never reach here.
        // Mode 1 keeps the zone but freezes its right edge at z.ClipBar.
        if (z.State == ZS_CLIPPED && z.ClipBar >= 0)
        {
            int actBase     = isSell ? BASE_SELL_ACT : BASE_BUY_ACT;
            COLORREF fill   = isSell ? sellColor : buyColor;
            COLORREF border = isSell ? sellBorder : buyBorder;
            DrawZoneRect(sc, actBase + z.ID, leftBar, z.ClipBar,
                         z.BottomPrice, z.TopPrice,
                         fill, border, borderWidth, transparency, scLineStyle);
            drawnCount++;
        }

    }   // end for (auto& z : *pZones) -- PASS 4 drawing loop

    // Always update the intrabar guard so the next incremental call correctly
    // detects whether a new bar has closed (including after a full recalc).
    sc.SetPersistentInt(3, sc.ArraySize);
}       // end scsf_LiquidityZones
