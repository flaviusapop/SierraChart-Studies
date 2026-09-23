#include "sierrachart.h"

SCDLLName("AnchoredCVD")

// =====================================================================
// AnchoredCVD
// ---------------------------------------------------------------------
// Cumulative volume delta anchored to meaningful start points, rendered
// as CVD candles, with deterministic divergence and absorption flags.
// Companion to AuctionContextMap: ACM answers WHERE to do business,
// AnchoredCVD answers WHETHER the other side showed up when price got
// there. Rejection/acceptance classification is deliberately excluded —
// it needs zone context and belongs to the future zone-gate study
// (consumes this study's SG7-9 + ACM SG17-19).
//
// DESIGN PRINCIPLE: read, don't compute. All CVD math comes from native
// sc.CumulativeDeltaVolume() with per-index reset (reset at the anchor
// bar IS the anchoring mechanism — verified against SC's own
// scsf_CumulativeDeltaBarsVolume in ACS_Source/Studies8.cpp):
//   function writes LAST into the passed subgraph[i],
//   OPEN into .Arrays[0][i], HIGH into .Arrays[1][i], LOW into .Arrays[2][i].
//
// Anchor slots:
//   A (primary, CVD candles SG1-4): default RTH Session Open
//   B (line SG5):                   default Overnight Session Open
//   C (line SG6):                   default Latest Swing Pivot (auto re-anchor)
// Modes per slot: 0=Off, 1=RTH Session Open, 2=Overnight Session Open,
//   3=Custom Time Daily, 4=Fixed DateTime, 5=Latest Swing Pivot.
//
// Signals (completed bars only, never repaint):
//   Divergence (slot A): consecutive confirmed price swings vs CVD
//     highs/lows, same anchor segment only. OFF by default in v0.3 —
//     standalone swing divergence prints all through trends; it returns
//     zone-conditioned in the gate study.
//   Absorption (slot A): |CVD change over K bars| >= T*K*avgAbsDelta
//     while price drifts <= M ticks. Sellers absorbed -> +1 (bullish),
//     buyers absorbed -> -1 (bearish). Yellow/orange body tint.
//
// v0.3 additions:
//   Size Delta CVD (SG15, gold line): cumulative (avg ask trade size −
//     avg bid trade size) from sc.NumberOfAsk/BidTrades base data,
//     anchored with slot A. Institutional-vs-retail proxy: total CVD up
//     while Size CVD down = rally bought by small lots, sold by big lots.
//     Display scaled by input (raw in Arrays[0] for the zone gate).
//   Delta Efficiency (SG16, machine-readable): signed price ticks per
//     unit of baseline-normalized delta over the efficiency window.
//     ~0 on heavy delta = absorption; outsized on thin delta = liquidity
//     vacuum (magenta body tint) — don't chase, don't fade.
//
// Chart requirements: intraday chart with bid/ask volume. For mode 2,
// chart Session Times must have the evening session enabled.
// Mode 4 note: bars before the fixed anchor show accumulation from bar 0;
// only the segment from the anchor forward is meaningful.
// =====================================================================

// One confirmed price swing record (for divergence tracking).
struct ACVDSwing
{
    int   bar   = -1;   // pivot bar index
    float price = 0;    // price extreme at the pivot
    float cvd   = 0;    // slot-A CVD High (swing high) / Low (swing low) at pivot
    bool  valid = false;
};

// Single heap-allocated persistent record, held via sc.GetPersistentPointer(1).
struct ACVDState
{
    int  anchorIdx[3]       = { 0, 0, 0 };   // current anchor bar per slot
    int  fixedAnchorIdx[3]  = { -1, -1, -1 };// resolved index for mode 4
    ACVDSwing high1, high2;                  // high1 = most recent confirmed swing high
    ACVDSwing low1,  low2;
    int  lastProcessedBar   = -1;            // completed-bar gate
    bool dataWarned         = false;         // bid/ask-volume warning issued once
};

// Compute/refresh one slot over a bar range by calling the native CVD
// function per index. Reset is raised exactly at the slot's anchor bar
// (and at bar 0 so the accumulation always has a defined start).
static void ACVD_ComputeRange(SCStudyInterfaceRef sc, SCSubgraphRef CalcSG,
                              int FromIndex, int ToIndex, int AnchorIndex)
{
    for (int b = FromIndex; b <= ToIndex; b++)
        sc.CumulativeDeltaVolume(sc.BaseDataIn, CalcSG, b, (b == AnchorIndex || b == 0));
}

// Anchor-reset test for modes 1-4 at bar i (mode 5 is event-driven).
static bool ACVD_IsResetBar(SCStudyInterfaceRef sc, int Mode, int i,
                            int CustomTimeSec, int FixedAnchorIdx)
{
    if (i == 0)
        return true;

    const SCDateTime& cur  = sc.BaseDateTimeIn[i];
    const SCDateTime& prev = sc.BaseDateTimeIn[i - 1];

    switch (Mode)
    {
        case 1:  // RTH Session Open: transition into day session or new trading day
        {
            const bool inDay     = sc.IsDateTimeInDaySession(cur) != 0;
            const bool prevInDay = sc.IsDateTimeInDaySession(prev) != 0;
            if (!inDay)
                return false;
            if (!prevInDay)
                return true;
            return sc.GetTradingDayDate(cur) != sc.GetTradingDayDate(prev);
        }
        case 2:  // Overnight Session Open: transition out of day session
        {
            const bool inDay     = sc.IsDateTimeInDaySession(cur) != 0;
            const bool prevInDay = sc.IsDateTimeInDaySession(prev) != 0;
            if (inDay)
                return false;
            if (prevInDay)
                return true;
            return sc.GetTradingDayDate(cur) != sc.GetTradingDayDate(prev);
        }
        case 3:  // Custom Time Daily: first bar at/after the time each calendar day
        {
            const int tsec  = cur.GetTimeInSeconds();
            if (tsec < CustomTimeSec)
                return false;
            const int psec  = prev.GetTimeInSeconds();
            const int date  = cur.GetDate();
            const int pdate = prev.GetDate();
            return (pdate != date) || (psec < CustomTimeSec);
        }
        case 4:  // Fixed DateTime: reset only at the resolved anchor index
            return i == FixedAnchorIdx;
    }
    return false;
}

// =====================================================================
SCSFExport scsf_AnchoredCVD(SCStudyInterfaceRef sc)
{
    // ── Output subgraphs — FIXED ORDER, never renumber ──────────────────
    // SG1-4 follow SC's GDT_CANDLESTICK convention (SC_OPEN/HIGH/LOW/LAST
    // = Subgraph[0..3]), wiring copied from scsf_CumulativeDeltaBarsVolume.
    SCSubgraphRef SG_AOpen   = sc.Subgraph[0];   // SG1  CVD A Open
    SCSubgraphRef SG_AHigh   = sc.Subgraph[1];   // SG2  CVD A High
    SCSubgraphRef SG_ALow    = sc.Subgraph[2];   // SG3  CVD A Low
    SCSubgraphRef SG_ALast   = sc.Subgraph[3];   // SG4  CVD A Last
    SCSubgraphRef SG_BLast   = sc.Subgraph[4];   // SG5  CVD B Last (line)
    SCSubgraphRef SG_CLast   = sc.Subgraph[5];   // SG6  CVD C Last (line)
    SCSubgraphRef SG_BullDiv = sc.Subgraph[6];   // SG7  Bullish Divergence
    SCSubgraphRef SG_BearDiv = sc.Subgraph[7];   // SG8  Bearish Divergence
    SCSubgraphRef SG_Absorb  = sc.Subgraph[8];   // SG9  Absorption Flag (-1/0/+1)
    SCSubgraphRef SG_BarDelta = sc.Subgraph[9];  // SG10 Bar Delta (diagnostic)
    SCSubgraphRef SG_AvgAbs  = sc.Subgraph[10];  // SG11 Avg Abs Delta baseline
    // Hidden calc SGs — sc.CumulativeDeltaVolume reserves .Arrays[0-2] on
    // these; display SGs above never collide with the reserved arrays.
    SCSubgraphRef SG_CalcA   = sc.Subgraph[11];  // SG12 calc slot A
    SCSubgraphRef SG_CalcB   = sc.Subgraph[12];  // SG13 calc slot B
    SCSubgraphRef SG_CalcC   = sc.Subgraph[13];  // SG14 calc slot C
    // v0.3 additions
    SCSubgraphRef SG_SizeCVD = sc.Subgraph[14];  // SG15 Size Delta CVD (scaled;
                                                 //      raw value in Arrays[0])
    SCSubgraphRef SG_Effic   = sc.Subgraph[15];  // SG16 Delta Efficiency
                                                 //      (machine-readable; |eff|
                                                 //      in Arrays[0], SMA|eff|
                                                 //      in Arrays[1])

    // ── Inputs ──────────────────────────────────────────────────────────
    SCInputRef In_ModeA      = sc.Input[0];
    SCInputRef In_ModeB      = sc.Input[1];
    SCInputRef In_ModeC      = sc.Input[2];
    SCInputRef In_CustomTime = sc.Input[3];
    SCInputRef In_FixedDT    = sc.Input[4];
    SCInputRef In_PivotLen   = sc.Input[5];
    SCInputRef In_AbsorbK    = sc.Input[6];
    SCInputRef In_AbsorbT    = sc.Input[7];
    SCInputRef In_AbsorbM    = sc.Input[8];
    SCInputRef In_BaselineW  = sc.Input[9];
    SCInputRef In_ShowDiv    = sc.Input[10];
    SCInputRef In_ShowAbsorb = sc.Input[11];
    // v0.3 additions
    SCInputRef In_SizeScale  = sc.Input[12];
    SCInputRef In_EffWindow  = sc.Input[13];
    SCInputRef In_VacuumMult = sc.Input[14];
    SCInputRef In_ShowVacuum = sc.Input[15];

    // =================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "AnchoredCVD v0.3";
        sc.StudyDescription =
            "Anchored cumulative volume delta (native sc.CumulativeDeltaVolume "
            "with per-anchor reset) rendered as CVD candles, plus divergence "
            "and absorption flags. Anchor slot A = candles, B/C = lines. "
            "Modes: RTH open / ON open / custom time / fixed datetime / latest "
            "swing pivot. Intraday charts with bid/ask volume only. "
            "Rejection/acceptance classification reserved for the zone-gate "
            "study (combine SG7-9 with AuctionContextMap SG17-19).";

        sc.AutoLoop      = 0;    // manual loop
        sc.UpdateAlways  = 1;
        sc.GraphRegion   = 1;    // own subgraph region
        sc.ValueFormat   = 0;
        // Must stay GDT_CUSTOM: candlestick Graph Draw Types render ONLY
        // SG1-4 and suppress every other subgraph (confirmed by SC
        // engineering, Support Board thread 25327). CVD candles are instead
        // drawn per-subgraph: body via CANDLESTICK_BODY_OPEN/_CLOSE pair
        // (Open SG colors = fill, Close SG colors = outline; primary = up
        // bar, secondary = down bar), wick via BAR_TOP/_BOTTOM pair.
        sc.GraphDrawType = GDT_CUSTOM;
        sc.MaintainAdditionalChartDataArrays = 1;  // per SC's own CVD example

        SG_AOpen.Name = "CVD A Open";
        SG_AOpen.DrawStyle = DRAWSTYLE_CANDLESTICK_BODY_OPEN;
        SG_AOpen.PrimaryColor = RGB(0, 180, 90);      // up-body fill
        SG_AOpen.SecondaryColor = RGB(200, 60, 60);   // down-body fill
        SG_AOpen.SecondaryColorUsed = true;
        SG_AOpen.DrawZeros = true;

        SG_AHigh.Name = "CVD A High";
        SG_AHigh.DrawStyle = DRAWSTYLE_BAR_TOP;       // wick: High->Low bar
        SG_AHigh.PrimaryColor = RGB(150, 150, 150);
        SG_AHigh.LineWidth = 1;
        SG_AHigh.DrawZeros = true;

        SG_ALow.Name = "CVD A Low";
        SG_ALow.DrawStyle = DRAWSTYLE_BAR_BOTTOM;
        SG_ALow.PrimaryColor = RGB(150, 150, 150);
        SG_ALow.LineWidth = 1;
        SG_ALow.DrawZeros = true;

        SG_ALast.Name = "CVD A Last";
        SG_ALast.DrawStyle = DRAWSTYLE_CANDLESTICK_BODY_CLOSE;
        SG_ALast.PrimaryColor = RGB(0, 220, 120);     // up-body outline
        SG_ALast.SecondaryColor = RGB(255, 90, 90);   // down-body outline
        SG_ALast.SecondaryColorUsed = true;
        SG_ALast.DrawZeros = true;

        SG_BLast.Name = "CVD B Last";
        SG_BLast.DrawStyle = DRAWSTYLE_LINE;
        SG_BLast.PrimaryColor = RGB(130, 130, 160);   // dim
        SG_BLast.LineWidth = 1;
        SG_BLast.DrawZeros = true;

        SG_CLast.Name = "CVD C Last";
        SG_CLast.DrawStyle = DRAWSTYLE_LINE;
        SG_CLast.PrimaryColor = RGB(0, 190, 255);
        SG_CLast.LineWidth = 1;
        SG_CLast.DrawZeros = true;

        SG_BullDiv.Name = "Bullish Divergence";
        SG_BullDiv.DrawStyle = DRAWSTYLE_ARROW_UP;
        SG_BullDiv.PrimaryColor = RGB(0, 230, 120);
        SG_BullDiv.LineWidth = 3;
        SG_BullDiv.DrawZeros = 0;

        SG_BearDiv.Name = "Bearish Divergence";
        SG_BearDiv.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        SG_BearDiv.PrimaryColor = RGB(255, 80, 80);
        SG_BearDiv.LineWidth = 3;
        SG_BearDiv.DrawZeros = 0;

        SG_Absorb.Name = "Absorption Flag";
        SG_Absorb.DrawStyle = DRAWSTYLE_IGNORE;   // machine-readable
        SG_Absorb.DrawZeros = 1;

        SG_BarDelta.Name = "Bar Delta";
        SG_BarDelta.DrawStyle = DRAWSTYLE_HIDDEN; // diagnostic
        SG_BarDelta.PrimaryColor = RGB(160, 160, 160);
        SG_BarDelta.DrawZeros = 1;

        SG_AvgAbs.Name = "Avg Abs Delta";
        SG_AvgAbs.DrawStyle = DRAWSTYLE_IGNORE;   // diagnostic
        SG_AvgAbs.DrawZeros = 1;

        SG_CalcA.Name = "Calc A";
        SG_CalcA.DrawStyle = DRAWSTYLE_IGNORE;
        SG_CalcA.DrawZeros = 0;

        SG_CalcB.Name = "Calc B";
        SG_CalcB.DrawStyle = DRAWSTYLE_IGNORE;
        SG_CalcB.DrawZeros = 0;

        SG_CalcC.Name = "Calc C";
        SG_CalcC.DrawStyle = DRAWSTYLE_IGNORE;
        SG_CalcC.DrawZeros = 0;

        // v0.3: "whose delta is it" — cumulative (avg ask trade size − avg
        // bid trade size), anchored with slot A. Total CVD rising while this
        // falls = rally bought by small lots, sold by large lots.
        SG_SizeCVD.Name = "Size Delta CVD";
        SG_SizeCVD.DrawStyle = DRAWSTYLE_LINE;
        SG_SizeCVD.PrimaryColor = RGB(255, 215, 120);   // gold
        SG_SizeCVD.LineWidth = 2;
        SG_SizeCVD.DrawZeros = true;

        // v0.3: signed price ticks per unit of baseline-normalized delta over
        // the efficiency window. ~0 with heavy delta = absorption; extreme
        // with thin delta = liquidity vacuum. Machine-readable (zone gate).
        SG_Effic.Name = "Delta Efficiency";
        SG_Effic.DrawStyle = DRAWSTYLE_IGNORE;
        SG_Effic.DrawZeros = 1;

        In_ModeA.Name = "Anchor A Mode (candles)";
        In_ModeA.SetCustomInputStrings(
            "Off;RTH Session Open;Overnight Session Open;Custom Time Daily;Fixed DateTime;Latest Swing Pivot");
        In_ModeA.SetCustomInputIndex(1);

        In_ModeB.Name = "Anchor B Mode (line)";
        In_ModeB.SetCustomInputStrings(
            "Off;RTH Session Open;Overnight Session Open;Custom Time Daily;Fixed DateTime;Latest Swing Pivot");
        In_ModeB.SetCustomInputIndex(2);

        In_ModeC.Name = "Anchor C Mode (line)";
        In_ModeC.SetCustomInputStrings(
            "Off;RTH Session Open;Overnight Session Open;Custom Time Daily;Fixed DateTime;Latest Swing Pivot");
        In_ModeC.SetCustomInputIndex(5);

        In_CustomTime.Name = "Custom Time (mode 3)";
        In_CustomTime.SetTime(HMS_TIME(9, 30, 0));

        In_FixedDT.Name = "Fixed DateTime (mode 4)";
        In_FixedDT.SetDateTime(0.0);

        In_PivotLen.Name = "Swing Pivot Length (mode 5 + divergence)";
        In_PivotLen.SetInt(5);
        In_PivotLen.SetIntLimits(2, 50);

        In_AbsorbK.Name = "Absorption Window K (bars)";
        In_AbsorbK.SetInt(8);
        In_AbsorbK.SetIntLimits(2, 100);

        In_AbsorbT.Name = "Absorption Threshold T (x avg abs delta)";
        In_AbsorbT.SetFloat(2.0f);

        In_AbsorbM.Name = "Absorption Max Price Drift (ticks)";
        In_AbsorbM.SetInt(12);

        In_BaselineW.Name = "Baseline Length W (bars)";
        In_BaselineW.SetInt(100);
        In_BaselineW.SetIntLimits(10, 1000);

        // v0.3: arrows default OFF — naive swing divergence prints all
        // through trends (passive absorption = continuous divergence) and
        // only earns its keep once conditioned on ACM zones in the gate
        // study. Re-enable here for testing at levels.
        In_ShowDiv.Name = "Show Divergence Arrows";
        In_ShowDiv.SetYesNo(0);

        In_ShowAbsorb.Name = "Show Absorption Highlight";
        In_ShowAbsorb.SetYesNo(1);

        In_SizeScale.Name = "Size CVD Scale Multiplier";
        In_SizeScale.SetFloat(10.0f);

        In_EffWindow.Name = "Efficiency Window (bars)";
        In_EffWindow.SetInt(8);
        In_EffWindow.SetIntLimits(2, 100);

        In_VacuumMult.Name = "Vacuum Threshold (x avg |efficiency|)";
        In_VacuumMult.SetFloat(3.0f);

        In_ShowVacuum.Name = "Show Vacuum Highlight";
        In_ShowVacuum.SetYesNo(1);

        return;
    }

    // =================================================================
    // Persistent state teardown.
    // =================================================================
    if (sc.LastCallToFunction)
    {
        void*& ptrRef = sc.GetPersistentPointer(1);
        ACVDState* st = (ACVDState*)ptrRef;
        if (st != nullptr)
        {
            delete st;
            ptrRef = nullptr;
        }
        return;
    }

    // =================================================================
    // Persistent state — allocate on first call, reset on full recalc.
    // =================================================================
    void*& ptrRef = sc.GetPersistentPointer(1);
    ACVDState* state = (ACVDState*)ptrRef;
    if (state == nullptr)
    {
        state = new ACVDState();
        ptrRef = state;
    }

    const bool fullRecalc = (sc.IsFullRecalculation != 0 || sc.UpdateStartIndex == 0);
    if (fullRecalc)
    {
        for (int s = 0; s < 3; s++)
        {
            state->anchorIdx[s]      = 0;
            state->fixedAnchorIdx[s] = -1;
        }
        state->high1 = ACVDSwing();
        state->high2 = ACVDSwing();
        state->low1  = ACVDSwing();
        state->low2  = ACVDSwing();
        state->lastProcessedBar = -1;
    }

    if (sc.ArraySize < 2)
        return;

    // ── Bid/ask volume availability check (once per load) ───────────────
    if (!state->dataWarned && fullRecalc)
    {
        const int scanFrom = max(0, sc.ArraySize - 200);
        bool anyData = false;
        for (int b = scanFrom; b < sc.ArraySize && !anyData; b++)
            if (sc.AskVolume[b] != 0.0f || sc.BidVolume[b] != 0.0f)
                anyData = true;
        if (!anyData)
            sc.AddMessageToLog(
                "AnchoredCVD: no bid/ask volume found in recent bars - CVD will be flat. "
                "This study requires an intraday chart with bid/ask volume data.", 1);
        state->dataWarned = true;
    }

    const int mode[3] = { (int)In_ModeA.GetIndex(), (int)In_ModeB.GetIndex(), (int)In_ModeC.GetIndex() };
    // SCSubgraphRef is a reference typedef — arrays of references are illegal,
    // so slot calc subgraphs are addressed through pointers.
    s_SCSubgraph_260* calcSG[3] = { &SG_CalcA, &SG_CalcB, &SG_CalcC };

    // Resolve mode-4 fixed anchors once.
    for (int s = 0; s < 3; s++)
    {
        if (mode[s] == 4 && state->fixedAnchorIdx[s] < 0)
        {
            SCDateTime dt = In_FixedDT.GetDateTime();
            state->fixedAnchorIdx[s] =
                (dt > 0.0) ? sc.GetContainingIndexForSCDateTime(sc.ChartNumber, dt) : 0;
        }
    }

    const int   customSec = In_CustomTime.GetTime();
    const int   pivotLen  = In_PivotLen.GetInt();
    const int   absK      = In_AbsorbK.GetInt();
    const float absT      = In_AbsorbT.GetFloat();
    const int   absM      = In_AbsorbM.GetInt();
    const int   baseW     = In_BaselineW.GetInt();
    const int   effW      = In_EffWindow.GetInt();
    const float vacMult   = In_VacuumMult.GetFloat();
    const float halfTick  = 0.5f * sc.TickSize;

    // =================================================================
    // PASS 1 — per-bar accumulation for all slots + diagnostics.
    // =================================================================
    for (int i = sc.UpdateStartIndex; i < sc.ArraySize; i++)
    {
        bool resetA = (i == 0);   // slot A reset also anchors the Size CVD

        for (int s = 0; s < 3; s++)
        {
            if (mode[s] == 0)
                continue;

            bool reset;
            if (mode[s] == 5)
            {
                // Event-driven anchor: reset only at the current pivot anchor.
                reset = (i == state->anchorIdx[s]);
            }
            else
            {
                reset = ACVD_IsResetBar(sc, mode[s], i, customSec, state->fixedAnchorIdx[s]);
                if (reset)
                    state->anchorIdx[s] = i;
            }

            if (s == 0 && reset)
                resetA = true;

            sc.CumulativeDeltaVolume(sc.BaseDataIn, *calcSG[s], i, (reset || i == 0));
        }

        // Signal SGs default to 0 for every (re)computed bar; pass 2 sets
        // them on completed bars only, so flags never repaint.
        SG_BullDiv[i] = 0;
        SG_BearDiv[i] = 0;
        SG_Absorb[i]  = 0;
        SG_Effic[i]   = 0;

        // Display copies (mapping verified from scsf_CumulativeDeltaBarsVolume:
        // SubgraphOut[i]=Last, Arrays[0]=Open, Arrays[1]=High, Arrays[2]=Low).
        if (mode[0] != 0)
        {
            SG_AOpen[i] = SG_CalcA.Arrays[0][i];
            SG_AHigh[i] = SG_CalcA.Arrays[1][i];
            SG_ALow[i]  = SG_CalcA.Arrays[2][i];
            SG_ALast[i] = SG_CalcA[i];
        }
        if (mode[1] != 0)
            SG_BLast[i] = SG_CalcB[i];
        if (mode[2] != 0)
            SG_CLast[i] = SG_CalcC[i];

        // Diagnostics: raw bar delta + rolling average of |delta| (native SMA).
        const float barDelta = sc.AskVolume[i] - sc.BidVolume[i];
        SG_BarDelta[i] = barDelta;
        SG_AvgAbs.Arrays[0][i] = (barDelta < 0.0f) ? -barDelta : barDelta;
        sc.SimpleMovAvg(SG_AvgAbs.Arrays[0], SG_AvgAbs, i, baseW);

        // Size Delta CVD: cumulative (avg ask trade size − avg bid trade
        // size), reset with slot A's anchor so both accumulations cover the
        // same segment. Raw value kept in Arrays[0]; display scaled to sit
        // readably against the contract-scale CVD candles.
        {
            const float nAsk = sc.NumberOfAskTrades[i];
            const float nBid = sc.NumberOfBidTrades[i];
            const float avgAskSize = (nAsk > 0.0f) ? sc.AskVolume[i] / nAsk : 0.0f;
            const float avgBidSize = (nBid > 0.0f) ? sc.BidVolume[i] / nBid : 0.0f;
            const float prevRaw = (resetA || i == 0) ? 0.0f : SG_SizeCVD.Arrays[0][i - 1];
            SG_SizeCVD.Arrays[0][i] = prevRaw + (avgAskSize - avgBidSize);
            SG_SizeCVD[i] = SG_SizeCVD.Arrays[0][i] * In_SizeScale.GetFloat();
        }
    }

    // =================================================================
    // PASS 2 — completed-bar logic (pivots, re-anchoring, divergence,
    // absorption). Bars [lastProcessedBar+1 .. ArraySize-2]; never the
    // forming bar, so flags never repaint.
    // =================================================================
    const int lastCompleted = sc.ArraySize - 2;
    int c = state->lastProcessedBar + 1;
    if (c < 2 * pivotLen)
        c = 2 * pivotLen;

    for (; c <= lastCompleted; c++)
    {
        // ── Pivot confirmation at p = c - pivotLen ──────────────────────
        // Right side strict, left side non-strict (so equal double tops /
        // bottoms still confirm and can feed the divergence equality case).
        const int p = c - pivotLen;

        bool isSwingHigh = true;
        bool isSwingLow  = true;
        for (int k = p - pivotLen; k <= p + pivotLen && (isSwingHigh || isSwingLow); k++)
        {
            if (k == p)
                continue;
            if (k < p)
            {
                if (sc.High[k] >  sc.High[p]) isSwingHigh = false;
                if (sc.Low[k]  <  sc.Low[p])  isSwingLow  = false;
            }
            else
            {
                if (sc.High[k] >= sc.High[p]) isSwingHigh = false;
                if (sc.Low[k]  <= sc.Low[p])  isSwingLow  = false;
            }
        }

        bool newPivot = false;

        if (isSwingHigh && state->high1.bar != p)
        {
            state->high2 = state->high1;
            state->high1.bar   = p;
            state->high1.price = sc.High[p];
            state->high1.cvd   = SG_CalcA.Arrays[1][p];   // CVD High at pivot
            state->high1.valid = true;
            newPivot = true;

            // Bearish divergence: HH in price (equality allowed), LH in CVD,
            // both swings inside slot A's current anchor segment.
            if (In_ShowDiv.GetYesNo() && mode[0] != 0 &&
                state->high2.valid &&
                state->high1.bar >= state->anchorIdx[0] &&
                state->high2.bar >= state->anchorIdx[0] &&
                state->high1.price >= state->high2.price - halfTick &&
                state->high1.cvd   <  state->high2.cvd)
            {
                SG_BearDiv[c] = SG_CalcA.Arrays[1][p];
            }
        }

        if (isSwingLow && state->low1.bar != p)
        {
            state->low2 = state->low1;
            state->low1.bar   = p;
            state->low1.price = sc.Low[p];
            state->low1.cvd   = SG_CalcA.Arrays[2][p];    // CVD Low at pivot
            state->low1.valid = true;
            newPivot = true;

            // Bullish divergence: LL in price (equality allowed), HL in CVD.
            if (In_ShowDiv.GetYesNo() && mode[0] != 0 &&
                state->low2.valid &&
                state->low1.bar >= state->anchorIdx[0] &&
                state->low2.bar >= state->anchorIdx[0] &&
                state->low1.price <= state->low2.price + halfTick &&
                state->low1.cvd   >  state->low2.cvd)
            {
                SG_BullDiv[c] = SG_CalcA.Arrays[2][p];
            }
        }

        // ── Mode-5 re-anchor on any newly confirmed pivot ───────────────
        if (newPivot)
        {
            // Most recent confirmed pivot of either direction.
            int latestPivotBar = -1;
            if (state->high1.valid) latestPivotBar = state->high1.bar;
            if (state->low1.valid && state->low1.bar > latestPivotBar)
                latestPivotBar = state->low1.bar;

            for (int s = 0; s < 3; s++)
            {
                if (mode[s] == 5 && latestPivotBar >= 0 &&
                    latestPivotBar != state->anchorIdx[s])
                {
                    // Recompute this slot from the new anchor to chart end.
                    // Full-recalc cost is O(bars-remaining) per historical
                    // pivot — fine for intraday charts limited to days of
                    // data; keep Days to Load reasonable on this chart.
                    state->anchorIdx[s] = latestPivotBar;
                    ACVD_ComputeRange(sc, *calcSG[s], latestPivotBar,
                                      sc.ArraySize - 1, latestPivotBar);
                    // Refresh display copies over the recomputed segment.
                    for (int b = latestPivotBar; b < sc.ArraySize; b++)
                    {
                        if (s == 0)
                        {
                            SG_AOpen[b] = SG_CalcA.Arrays[0][b];
                            SG_AHigh[b] = SG_CalcA.Arrays[1][b];
                            SG_ALow[b]  = SG_CalcA.Arrays[2][b];
                            SG_ALast[b] = SG_CalcA[b];
                        }
                        else if (s == 1)
                            SG_BLast[b] = SG_CalcB[b];
                        else
                            SG_CLast[b] = SG_CalcC[b];
                    }
                }
            }
        }

        // ── Absorption flag (slot A) ────────────────────────────────────
        SG_Absorb[c] = 0;
        if (mode[0] != 0 &&
            c - absK >= state->anchorIdx[0])   // whole window inside segment
        {
            const float avgAbs = SG_AvgAbs[c];
            if (avgAbs > 0.0f)
            {
                const float deltaSum   = SG_CalcA[c] - SG_CalcA[c - absK];
                const float threshold  = absT * (float)absK * avgAbs;
                const float driftTicks =
                    (sc.Close[c] > sc.Close[c - absK]
                         ? sc.Close[c] - sc.Close[c - absK]
                         : sc.Close[c - absK] - sc.Close[c]) / sc.TickSize;

                if (driftTicks <= (float)absM)
                {
                    if (deltaSum <= -threshold)
                        SG_Absorb[c] = 1;    // heavy selling absorbed -> bullish
                    else if (deltaSum >= threshold)
                        SG_Absorb[c] = -1;   // heavy buying absorbed -> bearish
                }

                // Optional visual: tint the CVD candle at the absorption bar.
                if (SG_Absorb[c] != 0 && In_ShowAbsorb.GetYesNo())
                {
                    const COLORREF hl = (SG_Absorb[c] > 0) ? RGB(255, 235, 60)
                                                           : RGB(255, 140, 0);
                    SG_AOpen.DataColor[c] = hl;
                    SG_AHigh.DataColor[c] = hl;
                    SG_ALow.DataColor[c]  = hl;
                    SG_ALast.DataColor[c] = hl;
                }
            }
        }

        // ── Delta efficiency (slot A): signed price ticks per unit of
        // baseline-normalized delta over the efficiency window ──────────
        SG_Effic[c] = 0;
        if (mode[0] != 0 && c - effW >= state->anchorIdx[0])
        {
            const float avgAbs = SG_AvgAbs[c];
            if (avgAbs > 0.0f)
            {
                float deltaNorm = SG_CalcA[c] - SG_CalcA[c - effW];
                deltaNorm = ((deltaNorm < 0.0f) ? -deltaNorm : deltaNorm)
                            / ((float)effW * avgAbs);
                const bool thinDelta = (deltaNorm < 1.0f);   // below average pace
                if (deltaNorm < 0.10f)
                    deltaNorm = 0.10f;                       // avoid blowups

                const float priceTicks =
                    (sc.Close[c] - sc.Close[c - effW]) / sc.TickSize;
                const float eff = priceTicks / deltaNorm;
                SG_Effic[c] = eff;

                // Rolling avg |efficiency| baseline (native SMA on Arrays).
                SG_Effic.Arrays[0][c] = (eff < 0.0f) ? -eff : eff;
                sc.SimpleMovAvg(SG_Effic.Arrays[0], SG_Effic.Arrays[1], c, baseW);
                const float avgEff = SG_Effic.Arrays[1][c];

                // Vacuum: outsized travel on thin delta — pulled liquidity /
                // short covering. Tint bodies magenta (distinct from the
                // yellow/orange absorption tints). Don't chase, don't fade.
                if (In_ShowVacuum.GetYesNo() && avgEff > 0.0f && thinDelta &&
                    SG_Effic.Arrays[0][c] >= vacMult * avgEff &&
                    SG_Absorb[c] == 0)
                {
                    const COLORREF vc = RGB(200, 80, 255);
                    SG_AOpen.DataColor[c] = vc;
                    SG_AHigh.DataColor[c] = vc;
                    SG_ALow.DataColor[c]  = vc;
                    SG_ALast.DataColor[c] = vc;
                }
            }
        }

        state->lastProcessedBar = c;
    }
}
