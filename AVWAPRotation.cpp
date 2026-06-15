// =============================================================================
// AVWAPRotation.cpp  —  Sierra Chart ACSIL Custom Study
//
// Detects price rotation legs and plots Anchored VWAP + dynamic Midline for
// each leg.  Up to 5 historical legs tracked in a ring buffer.
// Optional Potential Reversal Line at half-threshold retrace.
//
// Detection modes:
//   0 = Manual  — fixed point retrace (e.g. 20 pts for ES)
//   1 = ATR     — threshold = ATR(period) × multiplier
//
// Architecture (AutoLoop = 0):
//   Full recalc  (sc.UpdateStartIndex == 0  OR  PS_FORCE_RECALC flag set)
//     Phase 1: sequential detection walk — builds ring buffer, writes midlines
//     Phase 2: AVWAP accumulation walk  — accumulates HLC3×Vol from each
//              leg's anchor forward, writes AVWAP subgraphs
//   Incremental (live tick)
//     Runs detection on the forming bar only.
//     AVWAP cumulative updated on bar-close; live bar uses temp accumulators
//     (avoids double-counting intrabar volume).
//     If a new leg is confirmed, PS_FORCE_RECALC = 1 is set so the very next
//     SC call (next tick, ~50 ms) performs a full recalc restoring all
//     historical AVWAP lines correctly.
//
// Subgraph / slot mapping:
//   Leg slot j (0 = most recent, 4 = oldest):
//     sc.Subgraph[j*2]     — AVWAP solid line
//     sc.Subgraph[j*2 + 1] — Midline dashed
//   sc.Subgraph[10]        — Potential Reversal Line (magenta dash)
//   sc.Subgraph[11]        — ATR scratch (hidden, not in SetDefaults)
//
// Persistent state — ONLY sc.GetPersistentInt / sc.SetPersistentInt used.
// Floats and doubles are bit-cast to/from ints via anonymous unions.
//
// Int slots (direct):
//   [j*4+0]  anchorBarIndex   (j=0..4, slots 0..19)
//   [j*4+1]  direction        +1=UP, -1=DOWN
//   [j*4+2]  extremeBarIndex  bar where runningExtreme last set
//   [j*4+3]  legExists        1=populated, 0=empty
//   [20]     legCount
//   [21]     candidateActive
//   [22]     candidateExtremeBarIndex
//   [23]     barCloseGuard    sc.ArraySize at last bar-close update
//   [24]     forceFullRecalc  1 = treat next call as full recalc
//
// Float-as-int slots (union bit-cast):
//   [30+j*2+0]  anchorPrice[j]     (j=0..4, slots 30..39)
//   [30+j*2+1]  runningExtreme[j]
//   [40]         candidateAnchor
//
// Double-as-two-int slots (lo word at slot, hi word at slot+1):
//   [50+j*4+0/1]  cumPV[j]   (j=0..4, slots 50..69)
//   [50+j*4+2/3]  cumVol[j]
// =============================================================================

#include "sierrachart.h"

SCDLLName("AVWAPRotation")

// ─── Persistent slot constants ────────────────────────────────────────────────
static const int PS_LEGCOUNT       = 20;
static const int PS_CAND_ACTIVE    = 21;
static const int PS_CAND_EXT_BAR   = 22;
static const int PS_BAR_GUARD      = 23;
static const int PS_FORCE_RECALC   = 24;
static const int PS_CAND_ANCHOR    = 40;   // float-as-int


// =============================================================================
// Bit-cast helpers — encode float/double into persistent int slots
// using anonymous unions (well-supported on MSVC and GCC, the two compilers
// Sierra Chart uses for its built-in DLL builder).
// =============================================================================

static void SetPF(SCStudyInterfaceRef sc, int slot, float val)
{
    union { float f; int i; } u;
    u.f = val;
    sc.SetPersistentInt(slot, u.i);
}

static float GetPF(SCStudyInterfaceRef sc, int slot)
{
    union { float f; int i; } u;
    u.i = sc.GetPersistentInt(slot);
    return u.f;
}

// Double stored as two consecutive ints: lo word at slot, hi word at slot+1.
static void SetPD(SCStudyInterfaceRef sc, int slot, double val)
{
    union { double d; int i[2]; } u;
    u.d = val;
    sc.SetPersistentInt(slot,     u.i[0]);
    sc.SetPersistentInt(slot + 1, u.i[1]);
}

static double GetPD(SCStudyInterfaceRef sc, int slot)
{
    union { double d; int i[2]; } u;
    u.i[0] = sc.GetPersistentInt(slot);
    u.i[1] = sc.GetPersistentInt(slot + 1);
    return u.d;
}


// =============================================================================
// LegState — local struct; never persisted as a struct, only via the
// individual helpers above.
// =============================================================================
struct LegState
{
    int    anchorBarIndex;    // bar where AVWAP accumulation begins
    int    extremeBarIndex;   // bar where runningExtreme currently lives
    float  anchorPrice;       // price at the anchor (swing extreme that started leg)
    float  runningExtreme;    // highest High (UP) or lowest Low (DOWN) so far
    int    direction;         // +1 = UP leg, -1 = DOWN leg
    bool   exists;            // true = slot is populated
    double cumPV;             // Σ (HLC3 × Volume) from anchorBarIndex forward
    double cumVol;            // Σ Volume from anchorBarIndex forward
};


// =============================================================================
// LoadState / SaveState — move all leg state between PersistVars and local struct
// =============================================================================
static void LoadState(SCStudyInterfaceRef sc,
                      LegState legs[5], int& legCount,
                      bool& candActive, float& candAnchor, int& candExtBar)
{
    legCount   = sc.GetPersistentInt(PS_LEGCOUNT);
    candActive = (sc.GetPersistentInt(PS_CAND_ACTIVE) != 0);
    candExtBar = sc.GetPersistentInt(PS_CAND_EXT_BAR);
    candAnchor = GetPF(sc, PS_CAND_ANCHOR);

    for (int j = 0; j < 5; ++j)
    {
        legs[j].anchorBarIndex  = sc.GetPersistentInt(j*4 + 0);
        legs[j].direction       = sc.GetPersistentInt(j*4 + 1);
        legs[j].extremeBarIndex = sc.GetPersistentInt(j*4 + 2);
        legs[j].exists          = (sc.GetPersistentInt(j*4 + 3) != 0);
        legs[j].anchorPrice     = GetPF(sc, 30 + j*2 + 0);
        legs[j].runningExtreme  = GetPF(sc, 30 + j*2 + 1);
        legs[j].cumPV           = GetPD(sc, 50 + j*4 + 0);
        legs[j].cumVol          = GetPD(sc, 50 + j*4 + 2);
    }
}

static void SaveState(SCStudyInterfaceRef sc,
                      const LegState legs[5], int legCount,
                      bool candActive, float candAnchor, int candExtBar)
{
    sc.SetPersistentInt(PS_LEGCOUNT,     legCount);
    sc.SetPersistentInt(PS_CAND_ACTIVE,  candActive ? 1 : 0);
    sc.SetPersistentInt(PS_CAND_EXT_BAR, candExtBar);
    SetPF(sc, PS_CAND_ANCHOR, candAnchor);

    for (int j = 0; j < 5; ++j)
    {
        sc.SetPersistentInt(j*4 + 0, legs[j].anchorBarIndex);
        sc.SetPersistentInt(j*4 + 1, legs[j].direction);
        sc.SetPersistentInt(j*4 + 2, legs[j].extremeBarIndex);
        sc.SetPersistentInt(j*4 + 3, legs[j].exists ? 1 : 0);
        SetPF(sc, 30 + j*2 + 0, legs[j].anchorPrice);
        SetPF(sc, 30 + j*2 + 1, legs[j].runningExtreme);
        SetPD(sc, 50 + j*4 + 0, legs[j].cumPV);
        SetPD(sc, 50 + j*4 + 2, legs[j].cumVol);
    }
}


// =============================================================================
// RotateIn — confirm a new leg and shift the ring buffer
//
// Slots shift: 4←3←2←1←0, new leg installed at slot 0.
// Ring buffer convention: slot 0 = active leg (SG1), slot 4 = oldest (SG9).
// =============================================================================
static void RotateIn(LegState legs[5], int& legCount,
                     int anchorBar, int extremeBar,
                     float anchorPrice, float startExtreme, int direction)
{
    for (int j = 4; j > 0; --j)
        legs[j] = legs[j - 1];

    legs[0].anchorBarIndex  = anchorBar;
    legs[0].extremeBarIndex = extremeBar;
    legs[0].anchorPrice     = anchorPrice;
    legs[0].runningExtreme  = startExtreme;
    legs[0].direction       = direction;
    legs[0].exists          = true;
    legs[0].cumPV           = 0.0;
    legs[0].cumVol          = 0.0;

    if (legCount < 5) ++legCount;
}


// =============================================================================
// MAIN STUDY FUNCTION
// =============================================================================
SCSFExport scsf_AVWAPRotation(SCStudyInterfaceRef sc)
{
    // ── Input refs ────────────────────────────────────────────────────────────
    SCInputRef in_Mode     = sc.Input[0];
    SCInputRef in_ManPts   = sc.Input[1];
    SCInputRef in_ATRPer   = sc.Input[2];
    SCInputRef in_ATRMult  = sc.Input[3];
    SCInputRef in_LegsDisp = sc.Input[4];
    SCInputRef in_ShowCand = sc.Input[5];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "AVWAP Rotation";
        sc.StudyDescription =
            "Detects price rotation legs and plots Anchored VWAP + Midline for "
            "each. Up to 5 legs in a ring buffer. Optional Potential Reversal "
            "Line at half-threshold retrace. Manual or ATR threshold.";

        sc.AutoLoop     = 0;
        sc.GraphRegion  = 0;
        sc.FreeDLL      = 0;
        sc.DrawZeros    = 0;
        sc.UpdateAlways = 1;

        // ── Leg 1 — White ────────────────────────────────────────────────
        sc.Subgraph[0].Name         = "Leg 1 AVWAP";
        sc.Subgraph[0].DrawStyle    = DRAWSTYLE_LINE;
        sc.Subgraph[0].LineWidth    = 2;
        sc.Subgraph[0].PrimaryColor = RGB(255, 255, 255);
        sc.Subgraph[0].DrawZeros    = 0;

        sc.Subgraph[1].Name         = "Leg 1 Midline";
        sc.Subgraph[1].DrawStyle    = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[1].LineWidth    = 1;
        sc.Subgraph[1].PrimaryColor = RGB(200, 200, 200);
        sc.Subgraph[1].DrawZeros    = 0;

        // ── Leg 2 — Cyan ─────────────────────────────────────────────────
        sc.Subgraph[2].Name         = "Leg 2 AVWAP";
        sc.Subgraph[2].DrawStyle    = DRAWSTYLE_LINE;
        sc.Subgraph[2].LineWidth    = 2;
        sc.Subgraph[2].PrimaryColor = RGB(0, 255, 255);
        sc.Subgraph[2].DrawZeros    = 0;

        sc.Subgraph[3].Name         = "Leg 2 Midline";
        sc.Subgraph[3].DrawStyle    = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[3].LineWidth    = 1;
        sc.Subgraph[3].PrimaryColor = RGB(0, 200, 200);
        sc.Subgraph[3].DrawZeros    = 0;

        // ── Leg 3 — Yellow ───────────────────────────────────────────────
        sc.Subgraph[4].Name         = "Leg 3 AVWAP";
        sc.Subgraph[4].DrawStyle    = DRAWSTYLE_LINE;
        sc.Subgraph[4].LineWidth    = 2;
        sc.Subgraph[4].PrimaryColor = RGB(255, 255, 0);
        sc.Subgraph[4].DrawZeros    = 0;

        sc.Subgraph[5].Name         = "Leg 3 Midline";
        sc.Subgraph[5].DrawStyle    = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[5].LineWidth    = 1;
        sc.Subgraph[5].PrimaryColor = RGB(200, 200, 0);
        sc.Subgraph[5].DrawZeros    = 0;

        // ── Leg 4 — Orange ───────────────────────────────────────────────
        sc.Subgraph[6].Name         = "Leg 4 AVWAP";
        sc.Subgraph[6].DrawStyle    = DRAWSTYLE_LINE;
        sc.Subgraph[6].LineWidth    = 2;
        sc.Subgraph[6].PrimaryColor = RGB(255, 165, 0);
        sc.Subgraph[6].DrawZeros    = 0;

        sc.Subgraph[7].Name         = "Leg 4 Midline";
        sc.Subgraph[7].DrawStyle    = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[7].LineWidth    = 1;
        sc.Subgraph[7].PrimaryColor = RGB(200, 130, 0);
        sc.Subgraph[7].DrawZeros    = 0;

        // ── Leg 5 — Gray ─────────────────────────────────────────────────
        sc.Subgraph[8].Name         = "Leg 5 AVWAP";
        sc.Subgraph[8].DrawStyle    = DRAWSTYLE_LINE;
        sc.Subgraph[8].LineWidth    = 1;
        sc.Subgraph[8].PrimaryColor = RGB(150, 150, 150);
        sc.Subgraph[8].DrawZeros    = 0;

        sc.Subgraph[9].Name         = "Leg 5 Midline";
        sc.Subgraph[9].DrawStyle    = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[9].LineWidth    = 1;
        sc.Subgraph[9].PrimaryColor = RGB(120, 120, 120);
        sc.Subgraph[9].DrawZeros    = 0;

        // ── Potential Reversal Line — Magenta ─────────────────────────────
        sc.Subgraph[10].Name         = "Potential Reversal Line";
        sc.Subgraph[10].DrawStyle    = DRAWSTYLE_DASH;
        sc.Subgraph[10].LineWidth    = 1;
        sc.Subgraph[10].PrimaryColor = RGB(255, 0, 255);
        sc.Subgraph[10].DrawZeros    = 0;
        // sc.Subgraph[11] = ATR scratch (hidden, allocated but not declared)

        // ── Inputs ────────────────────────────────────────────────────────
        in_Mode.Name = "Detection Mode";
        in_Mode.SetCustomInputStrings("Manual (Fixed Points);ATR x Multiplier");
        in_Mode.SetCustomInputIndex(0);

        in_ManPts.Name = "Manual Threshold (points)";
        in_ManPts.SetFloat(20.0f);
        in_ManPts.SetFloatLimits(0.25f, 10000.0f);

        in_ATRPer.Name = "ATR Period";
        in_ATRPer.SetInt(20);
        in_ATRPer.SetIntLimits(1, 500);

        in_ATRMult.Name = "ATR Multiplier";
        in_ATRMult.SetFloat(2.0f);
        in_ATRMult.SetFloatLimits(0.1f, 50.0f);

        in_LegsDisp.Name = "Legs to Display (1-5)";
        in_LegsDisp.SetInt(5);
        in_LegsDisp.SetIntLimits(1, 5);

        in_ShowCand.Name = "Show Potential Reversal Line";
        in_ShowCand.SetYesNo(1);

        return;
    }

    const int totalBars = sc.ArraySize;
    if (totalBars < 2) return;

    // ── Read inputs ───────────────────────────────────────────────────────────
    // GetIndex() is correct for SetCustomInputStrings enumerations.
    const int   mode         = in_Mode.GetIndex();
    const float manThreshold = in_ManPts.GetFloat();
    const int   atrPeriod    = in_ATRPer.GetInt();
    const float atrMult      = in_ATRMult.GetFloat();
    const int   legsDisp     = in_LegsDisp.GetInt();
    const bool  showCand     = (in_ShowCand.GetYesNo() != 0);

    // ── Full recalc if SC says so OR we set the force flag ourselves ──────────
    // PS_FORCE_RECALC is set when a new leg forms during an incremental call.
    // SC will call us again within ~50 ms; that call will hit this branch and
    // rebuild everything correctly from bar 0.
    // A partial recalc (0 < UpdateStartIndex < totalBars-1) means SC blanked
    // bars from UpdateStartIndex onward and expects us to refill them.  We
    // can't resume from an arbitrary bar (detection state is sequential), so
    // any such call is treated the same as a full recalc from bar 0.
    const bool doFullRecalc = (sc.UpdateStartIndex == 0)
                           || (sc.UpdateStartIndex < totalBars - 1)
                           || (sc.GetPersistentInt(PS_FORCE_RECALC) != 0);

    // =========================================================================
    // FULL RECALCULATION
    // =========================================================================
    if (doFullRecalc)
    {
        sc.SetPersistentInt(PS_FORCE_RECALC, 0);   // clear the flag

        LegState legs[5] = {};
        int   legCount   = 0;
        bool  candActive = false;
        float candAnchor = 0.0f;
        int   candExtBar = 0;

        // ── Seed leg 0 from bar 0 ─────────────────────────────────────────
        {
            int   dir = (sc.Close[0] >= sc.Open[0]) ? 1 : -1;
            float anc = (dir ==  1) ? sc.Low[0]  : sc.High[0];
            float ext = (dir ==  1) ? sc.High[0] : sc.Low[0];
            legs[0].anchorBarIndex  = 0;
            legs[0].extremeBarIndex = 0;
            legs[0].anchorPrice     = anc;
            legs[0].runningExtreme  = ext;
            legs[0].direction       = dir;
            legs[0].exists          = true;
            legs[0].cumPV           = 0.0;
            legs[0].cumVol          = 0.0;
            legCount = 1;
        }
        // Write midline for bar 0
        sc.Subgraph[1][0] =
            (legs[0].anchorPrice + legs[0].runningExtreme) * 0.5f;

        // ─────────────────────────────────────────────────────────────────
        // PHASE 1 — Detection walk + Midline output
        //
        // Steps per bar:
        //   1. Compute threshold (Manual or ATR, called sequentially for ATR)
        //   2. Gap-open edge case
        //   3. Update running extreme of active leg
        //   4. Check full threshold → RotateIn if met
        //   5. Check half threshold → set candidate if met
        //   6. Write midlines for all current legs at bar i
        //
        // Midlines written here so the active leg's midline captures the
        // live runningExtreme at each bar (not the final value from Phase 2).
        // ─────────────────────────────────────────────────────────────────
        for (int i = 1; i < totalBars; ++i)
        {
            // ── 1. Threshold ──────────────────────────────────────────────
            float threshold = manThreshold;
            if (mode == 1)
            {
                // ATR called sequentially → computes incrementally, O(1)/bar
                sc.ATR(sc.BaseDataIn, sc.Subgraph[11], i, atrPeriod);
                float atrVal = sc.Subgraph[11][i];
                if (atrVal > 0.0f) threshold = atrVal * atrMult;
            }
            const float halfThresh = threshold * 0.5f;

            LegState& act      = legs[0];
            bool      rotated  = false;

            // ── 2. Gap-open edge case ─────────────────────────────────────
            // Opening beyond the full threshold from the running extreme
            // immediately triggers a new leg at the gap bar.
            {
                float gap = (act.direction == 1)
                          ? act.runningExtreme - sc.Open[i]
                          : sc.Open[i] - act.runningExtreme;

                if (gap >= threshold)
                {
                    int   nd  = -act.direction;
                    float na  = act.runningExtreme;
                    int   nab = act.extremeBarIndex;
                    float ne  = (nd == 1) ? sc.High[i] : sc.Low[i];
                    RotateIn(legs, legCount, nab, i, na, ne, nd);
                    candActive = false;
                    rotated    = true;
                }
            }

            if (!rotated)
            {
                // ── 3. Update running extreme ──────────────────────────────
                if (act.direction == 1)
                {
                    if (sc.High[i] > act.runningExtreme)
                    {
                        act.runningExtreme  = sc.High[i];
                        act.extremeBarIndex = i;
                        candActive = false;   // new high invalidates candidate
                    }
                }
                else
                {
                    if (sc.Low[i] < act.runningExtreme)
                    {
                        act.runningExtreme  = sc.Low[i];
                        act.extremeBarIndex = i;
                        candActive = false;   // new low invalidates candidate
                    }
                }

                // ── 4. Full threshold → new leg ────────────────────────────
                {
                    float retrace = (act.direction == 1)
                                  ? act.runningExtreme - sc.Low[i]
                                  : sc.High[i] - act.runningExtreme;

                    if (retrace >= threshold)
                    {
                        // Anchor of new leg = running extreme of old leg.
                        // candidateAnchor == runningExtreme, so candidate
                        // upgrades silently at the same price level.
                        int   nd  = -act.direction;
                        float na  = act.runningExtreme;
                        int   nab = act.extremeBarIndex;
                        float ne  = (nd == 1) ? sc.High[i] : sc.Low[i];
                        RotateIn(legs, legCount, nab, i, na, ne, nd);
                        candActive = false;
                        rotated    = true;
                    }
                }

                // ── 5. Half threshold → candidate reversal ─────────────────
                if (!rotated && showCand && !candActive)
                {
                    float hr = (legs[0].direction == 1)
                             ? legs[0].runningExtreme - sc.Low[i]
                             : sc.High[i] - legs[0].runningExtreme;

                    if (hr >= halfThresh)
                    {
                        candActive = true;
                        candAnchor = legs[0].runningExtreme;
                        candExtBar = legs[0].extremeBarIndex;
                    }
                }
            }

            // ── 6. Write midlines at bar i ────────────────────────────────
            // Active leg (j=0): dynamic midline using current runningExtreme.
            // Historical legs (j>0): frozen — runningExtreme unchanged after rotate.
            {
                int sc_ = (legsDisp < legCount) ? legsDisp : legCount;
                for (int j = 0; j < sc_; ++j)
                {
                    if (!legs[j].exists) continue;
                    sc.Subgraph[j*2 + 1][i] =
                        (legs[j].anchorPrice + legs[j].runningExtreme) * 0.5f;
                }
                for (int j = sc_; j < 5; ++j)
                {
                    sc.Subgraph[j*2][i]     = 0.0f;
                    sc.Subgraph[j*2 + 1][i] = 0.0f;
                }
                sc.Subgraph[10][i] = (showCand && candActive) ? candAnchor : 0.0f;
            }
        } // end Phase 1

        // ─────────────────────────────────────────────────────────────────
        // PHASE 2 — AVWAP accumulation
        //
        // Walk bar 0..N-2 (exclude the forming bar, totalBars-1).
        // The forming bar's contribution is handled every tick via temp
        // accumulators in the incremental path — keeping it out of the saved
        // cumulatives prevents double-counting when the bar closes and the
        // bar-close guard adds it for the first time.
        //
        // Pre-anchor bars get 0 (hidden by DrawZeros=0).
        // ─────────────────────────────────────────────────────────────────
        {
            // Ensure clean accumulators (Phase 1 left them at 0 via struct init)
            for (int j = 0; j < 5; ++j) { legs[j].cumPV = 0.0; legs[j].cumVol = 0.0; }

            int showCnt = (legsDisp < legCount) ? legsDisp : legCount;

            // Stop at totalBars-1 (exclusive) → last bar processed = totalBars-2
            for (int i = 0; i < totalBars - 1; ++i)
            {
                const float vol  = sc.Volume[i];
                const float hlc3 = sc.HLCAvg[i];   // (H+L+C)/3

                for (int j = 0; j < showCnt; ++j)
                {
                    if (!legs[j].exists)
                    {
                        sc.Subgraph[j*2][i]     = 0.0f;
                        sc.Subgraph[j*2 + 1][i] = 0.0f;
                        continue;
                    }
                    // Before this leg's anchor: blank
                    if (i < legs[j].anchorBarIndex)
                    {
                        sc.Subgraph[j*2][i]     = 0.0f;
                        sc.Subgraph[j*2 + 1][i] = 0.0f;
                        continue;
                    }
                    // Historical legs end where the next (newer) leg begins.
                    // Blanking here also overrides whatever Phase 1 wrote for
                    // those bars, giving clean non-overlapping display.
                    if (j > 0 && i >= legs[j - 1].anchorBarIndex)
                    {
                        sc.Subgraph[j*2][i]     = 0.0f;
                        sc.Subgraph[j*2 + 1][i] = 0.0f;
                        continue;
                    }
                    if (vol > 0.0f)
                    {
                        legs[j].cumPV  += (double)hlc3 * (double)vol;
                        legs[j].cumVol += (double)vol;
                    }
                    sc.Subgraph[j*2][i] = (legs[j].cumVol > 0.0)
                                        ? (float)(legs[j].cumPV / legs[j].cumVol)
                                        : 0.0f;
                }
                for (int j = showCnt; j < 5; ++j)
                    sc.Subgraph[j*2][i] = 0.0f;
            }
        }

        // ── Save final state for incremental path ─────────────────────────
        SaveState(sc, legs, legCount, candActive, candAnchor, candExtBar);
        sc.SetPersistentInt(PS_BAR_GUARD, totalBars);
        return;
    }


    // =========================================================================
    // INCREMENTAL UPDATE — live tick
    // =========================================================================
    LegState legs[5] = {};
    int   legCount   = 0;
    bool  candActive = false;
    float candAnchor = 0.0f;
    int   candExtBar = 0;

    LoadState(sc, legs, legCount, candActive, candAnchor, candExtBar);

    if (legCount == 0)
    {
        // State not yet initialised — request full recalc on next call.
        sc.SetPersistentInt(PS_FORCE_RECALC, 1);
        return;
    }

    const int i = totalBars - 1;   // forming bar index

    // ── Threshold for forming bar ─────────────────────────────────────────────
    float threshold = manThreshold;
    if (mode == 1)
    {
        sc.ATR(sc.BaseDataIn, sc.Subgraph[11], i, atrPeriod);
        float atrVal = sc.Subgraph[11][i];
        if (atrVal > 0.0f) threshold = atrVal * atrMult;
    }
    const float halfThresh = threshold * 0.5f;

    // ── Bar-close guard: update cumulative when a new bar has started ─────────
    // sc.ArraySize grows by 1 each time a bar closes and a new one opens.
    // At that moment i-1 is the freshly closed bar; add its final contribution.
    {
        int prevSize = sc.GetPersistentInt(PS_BAR_GUARD);
        if (totalBars != prevSize && i >= 1)
        {
            const int   prev = i - 1;
            const float pv   = sc.Volume[prev];
            const float ph3  = sc.HLCAvg[prev];
            // Only the active leg (slot 0) accumulates new bars.
            // Historical legs' cumulatives are frozen at their leg-end values.
            if (legs[0].exists && prev >= legs[0].anchorBarIndex && pv > 0.0f)
            {
                legs[0].cumPV  += (double)ph3 * (double)pv;
                legs[0].cumVol += (double)pv;
            }
            sc.SetPersistentInt(PS_BAR_GUARD, totalBars);
        }
    }

    // ── Detection on forming bar ──────────────────────────────────────────────
    LegState& act    = legs[0];
    bool      rotated = false;

    // Gap-open check
    {
        float gap = (act.direction == 1)
                  ? act.runningExtreme - sc.Open[i]
                  : sc.Open[i] - act.runningExtreme;

        if (gap >= threshold)
        {
            int   nd  = -act.direction;
            float na  = act.runningExtreme;
            int   nab = act.extremeBarIndex;
            float ne  = (nd == 1) ? sc.High[i] : sc.Low[i];
            RotateIn(legs, legCount, nab, i, na, ne, nd);
            candActive = false;
            rotated    = true;

            // New leg: save state and flag a full recalc for next call.
            // Historical AVWAP for the new leg will appear correctly then.
            SaveState(sc, legs, legCount, false, 0.0f, 0);
            sc.SetPersistentInt(PS_FORCE_RECALC, 1);
            return;
        }
    }

    if (!rotated)
    {
        // Update running extreme
        if (act.direction == 1)
        {
            if (sc.High[i] > act.runningExtreme)
            {
                act.runningExtreme  = sc.High[i];
                act.extremeBarIndex = i;
                candActive = false;
            }
        }
        else
        {
            if (sc.Low[i] < act.runningExtreme)
            {
                act.runningExtreme  = sc.Low[i];
                act.extremeBarIndex = i;
                candActive = false;
            }
        }

        // Full threshold check
        {
            float retrace = (act.direction == 1)
                          ? act.runningExtreme - sc.Low[i]
                          : sc.High[i] - act.runningExtreme;

            if (retrace >= threshold)
            {
                int   nd  = -act.direction;
                float na  = act.runningExtreme;
                int   nab = act.extremeBarIndex;
                float ne  = (nd == 1) ? sc.High[i] : sc.Low[i];
                RotateIn(legs, legCount, nab, i, na, ne, nd);
                candActive = false;
                rotated    = true;

                SaveState(sc, legs, legCount, false, 0.0f, 0);
                sc.SetPersistentInt(PS_FORCE_RECALC, 1);
                return;
            }
        }

        // Half-threshold candidate check
        if (showCand && !candActive)
        {
            float hr = (act.direction == 1)
                     ? act.runningExtreme - sc.Low[i]
                     : sc.High[i] - act.runningExtreme;

            if (hr >= halfThresh)
            {
                candActive = true;
                candAnchor = act.runningExtreme;
                candExtBar = act.extremeBarIndex;
            }
        }
    }

    // ── Write subgraphs for forming bar ───────────────────────────────────────
    {
        const float vol  = sc.Volume[i];
        const float hlc3 = sc.HLCAvg[i];
        int showCnt = (legsDisp < legCount) ? legsDisp : legCount;

        for (int j = 0; j < showCnt; ++j)
        {
            if (!legs[j].exists || i < legs[j].anchorBarIndex)
            {
                sc.Subgraph[j*2][i]     = 0.0f;
                sc.Subgraph[j*2 + 1][i] = 0.0f;
                continue;
            }
            // Historical legs: their period ended at the next leg's anchor.
            // The forming bar is always after that point, so blank here.
            if (j > 0 && i >= legs[j - 1].anchorBarIndex)
            {
                sc.Subgraph[j*2][i]     = 0.0f;
                sc.Subgraph[j*2 + 1][i] = 0.0f;
                continue;
            }
            // Active leg (j == 0): live AVWAP via temp accumulators.
            // Do NOT write back to legs[0].cumPV/cumVol — the bar-close guard
            // does that once per closed bar, preventing double-counting.
            double tPV  = legs[0].cumPV;
            double tVol = legs[0].cumVol;
            if (vol > 0.0f)
            {
                tPV  += (double)hlc3 * (double)vol;
                tVol += (double)vol;
            }
            sc.Subgraph[j*2][i]     = (tVol > 0.0) ? (float)(tPV / tVol) : 0.0f;
            sc.Subgraph[j*2 + 1][i] =
                (legs[j].anchorPrice + legs[j].runningExtreme) * 0.5f;
        }

        for (int j = showCnt; j < 5; ++j)
        {
            sc.Subgraph[j*2][i]     = 0.0f;
            sc.Subgraph[j*2 + 1][i] = 0.0f;
        }

        sc.Subgraph[10][i] = (showCand && candActive) ? candAnchor : 0.0f;
    }

    // ── Save updated detection state ──────────────────────────────────────────
    // cumPV/cumVol reflect the last bar-close baseline — intentionally not
    // updated with the forming bar's temp contribution.
    SaveState(sc, legs, legCount, candActive, candAnchor, candExtBar);

} // end scsf_AVWAPRotation
