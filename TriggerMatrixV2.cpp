// =============================================================================
// TriggerMatrixV2.cpp
// Sierra Chart ACSIL Custom Study — Trigger Matrix V2 Display v1.0
//
// Display-only study: evaluates the 16 ready primary V2 detector families
// from the pinned research catalog and publishes 32 stable Bull/Bear output
// subgraphs (SG0..SG31) with deterministic stacking. One instance per
// source-chart role (Range / Renko 6t / Renko 8t); outputs of non-selected
// roles remain exactly zero.
//
// Canonical catalog: v2-formula-catalog.json
//   version 2.0.0-research-2026-09-09
//   SHA-256 ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618
// The 32 primary bull/bear formulas are ported mechanically from the
// catalog. Optional *WithContext variants and blocked families (ordinals
// 3, 12, 13) are NOT implemented. Legacy TriggerMatrix.cpp predicates are
// non-authoritative and unused here.
//
// SUBGRAPHS (even=Bull below low, odd=Bear above high):
//   0/1 OED  Opposing Effort Decay (TEXT)              ord 1  Range
//   2/3 DES  Directional Effort Slope Response (ARROW) ord 2  Range
//   4/5 EEF  Extreme Effort Failure (TEXT)             ord 4  Range
//   6/7 VGD  VAP Gradient Divergence (TEXT)            ord 5  Range
//   8/9 WDM  Whole-Distribution Migration (TEXT)       ord 6  Range
//   10/11 DRP Delta Reversal Profile Response (TEXT)   ord 7  Range
//   12/13 PDR POC Delta Reclaim (TEXT)                 ord 8  Range
//   14/15 FVR Frozen VPOC Reclaim (TEXT)               ord 9  Range
//   16/17 ERM Effort Reversal with Value Migration     ord 10 Range
//   18/19 PBM Fixed POC Balance Migration (SQUARE)     ord 11 Range
//   20/21 OFR Order-Flow Reversal (TEXT)               ord 14 Renko 6t
//   22/23 DVR Delta Reversal VPOC Reclaim (TEXT)       ord 15 Renko 6t
//   24/25 VXC Frozen VPOC Cross (TEXT)                 ord 16 Renko 8t
//   26/27 PEV Persistent Effort Volume Response        ord 17 Renko 8t
//   28/29 R8F Renko-8 Effort Failure (TEXT)            ord 18 Renko 8t
//   30/31 FPR Frozen POC Recovery (TEXT)               ord 19 Range
//
// Producer index convention: catalog Spreadsheet notation ID{...}.SGn is
// ONE-based; ACSIL SubgraphIndex is ZERO-based (index = n - 1): SG1->0,
// SG2->1, SG3->2, SG4->3, SG59->58. TMV2_SG_* constants carry the
// zero-based indices and are shared by SetStudySubgraphValues and
// GetStudyArrayFromChartUsingID.
// INPUTS (0-based):
//   In:0  Chart Role (Range=0, Renko 6t=1, Renko 8t=2)
//   In:1  Range Delta Study (catalog SG4 -> index 3)
//   In:2  Range Delta Bands Study (SG1 upper -> 0 / SG3 lower -> 2, 20/0.9 SMA)
//   In:3  Range VPOC A Study (catalog SG1 -> index 0)
//   In:4  Range VPOC B Study (catalog SG1 -> index 0)
//   In:5  Range VA 68 Study (SG1 VVAH -> 0 / SG2 VVAL -> 1)
//   In:6  Renko 6 Delta Study (catalog SG4 -> index 3)
//   In:7  Renko 6 Delta Bands Study (SG1/SG3 -> 0/2)
//   In:8  Renko 6 VPOC Study (catalog SG1 -> index 0)
//   In:9  Renko 6 Ask Diagonal Study (catalog SG59 -> index 58, +300%)
//   In:10 Renko 6 Bid Diagonal Study (catalog SG59 -> index 58, -300%)
//   In:11 Renko 8 VPOC Study (catalog SG1 -> index 0)
//   In:12 Bull Base Offset (ticks, display only)
//   In:13 Bear Base Offset (ticks, display only)
//   In:14 Stack Step (ticks, display only)
//   In:15 VAP Multiplier Is 1 (family-5 precondition)
//   In:16 Source Configuration Revision
//
// PERSISTENT SLOTS:
//   Int 4,5 : 64-bit structural fingerprint (lo, hi). Slots 1-3 unused
//             (no new-bar guard needed under AutoLoop=0 manual loop;
//             no alert watermarks — display only, no alerts).
//
// STATUS (honest): native_sierra_compile=false, sierra_runtime_parity=false
// until user-side Sierra F5 compile and on-chart 32-formula parity pass.
// =============================================================================

#ifndef TMV2_UNITTEST
#include "sierrachart.h"
#endif
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Portable core: pure helpers shared by the ACSIL body and the Linux tests.
// No Sierra headers, no STL containers, no static mutable state.
// Indexing convention: offset 0 = current completed bar, k = k bars prior.
// ---------------------------------------------------------------------------

#define TMV2_VERSION "1.0"
#define TMV2_CATALOG_VERSION "2.0.0-research-2026-09-09"
#define TMV2_CATALOG_SHA "ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618"
#define TMV2_SCHEMA_VERSION 1

#define TMV2_ROLE_RANGE 0
#define TMV2_ROLE_RENKO6 1
#define TMV2_ROLE_RENKO8 2

// Producer subgraph indices (test-visible, portable seam).
// Catalog Spreadsheet notation ID{...}.SGn is ONE-based; ACSIL
// SubgraphIndex is ZERO-based, so index = n - 1:
//   SG1->0, SG2->1, SG3->2, SG4->3, SG59->58.
#define TMV2_SG_DELTA 3
#define TMV2_SG_BAND_UP 0
#define TMV2_SG_BAND_LO 2
#define TMV2_SG_VPOC 0
#define TMV2_SG_VVAH 0
#define TMV2_SG_VVAL 1
#define TMV2_SG_DIAG 58
#define TMV2_FNV_OFFSET_BASIS 14695981039346656037ULL

static int Tmv2_CatalogSgToIndex(int sgOneBased)
{
    return sgOneBased - 1;
}

static unsigned long long Tmv2_FnvOffsetBasis()
{
    return TMV2_FNV_OFFSET_BASIS;
}

struct Tmv2Window
{
    double tick;            // chart tick size; predicates require tick > 0
    double O[6], H[6], L[6], C[6];
    double AV[6], BV[6];    // native ask/bid volume per bar
    double D[6];  int dOk[6];    // role-local bar delta (producer SG4)
    double Up[6]; int upOk[6];   // role-local delta upper band (SG1)
    double Lo[6]; int loOk[6];   // role-local delta lower band (SG3)
    double VpA[6]; int vpaOk[6]; // Range VPOC instance A (SG1)
    double VpB[6]; int vpbOk[6]; // Range VPOC instance B (SG1)
    double VAH[6]; int vahOk[6]; // Range 68% VVAH (SG1)
    double VAL[6]; int valOk[6]; // Range 68% VVAL (SG2)
    double Vp6[2]; int vp6Ok[2]; // Renko 6t VPOC (SG1), [0]=cur [1]=prior
    double Vp8[2]; int vp8Ok[2]; // Renko 8t VPOC (SG1)
    double AskD[2]; int askOk[2];// Renko 6t ask-diagonal SG59 count
    double BidD[2]; int bidOk[2];// Renko 6t bid-diagonal SG59 count
    int vapMultIs1;              // family-5 precondition flag
    double vapTot[5];            // exact-tick VAP total volume rows
    double vapSide[5];           // AVAP rows (bull) or BVAP rows (bear)
    int vapOk[5];                // all five rows present with total > 0
};

static void Tmv2_ClearWindow(Tmv2Window* w)
{
    std::memset(w, 0, sizeof(*w));
}

static const char* Tmv2_CatalogVersion() { return TMV2_CATALOG_VERSION; }
static const char* Tmv2_CatalogSha() { return TMV2_CATALOG_SHA; }
static int Tmv2_ReadyFamilyCount() { return 16; }
static int Tmv2_OutputCount() { return 32; }

static const int* Tmv2_ReadyOrdinals()
{
    static const int k[16] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19};
    return k;
}

static int Tmv2_IsBlocked(int ord)
{
    return (ord == 3 || ord == 12 || ord == 13) ? 1 : 0;
}

// Chart role of a catalog ordinal: 0 Range, 1 Renko 6t, 2 Renko 8t, -1 none.
static int Tmv2_FamilyRole(int ord)
{
    switch (ord)
    {
    case 1: case 2: case 4: case 5: case 6: case 7:
    case 8: case 9: case 10: case 11: case 19:
        return TMV2_ROLE_RANGE;
    case 14: case 15:
        return TMV2_ROLE_RENKO6;
    case 16: case 17: case 18:
        return TMV2_ROLE_RENKO8;
    default:
        return -1;
    }
}

// Even (Bull) output SG for an ordinal, or -1 when absent.
static int Tmv2_FamilySgBull(int ord)
{
    switch (ord)
    {
    case 1: return 0;  case 2: return 2;   case 4: return 4;
    case 5: return 6;  case 6: return 8;   case 7: return 10;
    case 8: return 12; case 9: return 14;  case 10: return 16;
    case 11: return 18; case 14: return 20; case 15: return 22;
    case 16: return 24; case 17: return 26; case 18: return 28;
    case 19: return 30;
    default: return -1;
    }
}

static int Tmv2_FamilySgBear(int ord)
{
    int b = Tmv2_FamilySgBull(ord);
    return (b < 0) ? -1 : b + 1;
}

// Short family code for the structural summary log (e.g. ord 7 -> "DRP").
static const char* Tmv2_FamilyCode(int ord)
{
    switch (ord)
    {
    case 1: return "OED";
    case 2: return "DES";
    case 4: return "EEF";
    case 5: return "VGD";
    case 6: return "WDM";
    case 7: return "DRP";
    case 8: return "PDR";
    case 9: return "FVR";
    case 10: return "ERM";
    case 11: return "PBM";
    case 14: return "OFR";
    case 15: return "DVR";
    case 16: return "VXC";
    case 17: return "PEV";
    case 18: return "R8F";
    case 19: return "FPR";
    default: return "?";
    }
}

// Structural source label for have[11] slots (ids[11] presence flags).
static const char* Tmv2_SrcLabel(int srcIdx)
{
    switch (srcIdx)
    {
    case 0: return "RDelta";
    case 1: return "RBands";
    case 2: return "RVpA";
    case 3: return "RVpB";
    case 4: return "RVA";
    case 5: return "R6Delta";
    case 6: return "R6Bands";
    case 7: return "R6Vp";
    case 8: return "R6AskD";
    case 9: return "R6BidD";
    case 10: return "R8Vp";
    default: return "?";
    }
}

// Structural disabled set: which ready ordinals are disabled by missing
// structural inputs/preconditions for `role`. have[i] is 1 when ids[i] > 0
// (zero ID disables dependent families only). Returns count and fills
// disabled[] in ascending catalog order (deduplicated). Native-only
// families (Range 1,2,4; Renko8 18) are never listed: they need no
// producers. Dynamic per-bar empty/short arrays are NOT structural and are
// excluded here; they fail closed silently in the bar loop with no log.
static int Tmv2_StructDisabled(int role, const int have[11], int vapMultIs1, int disabled[16])
{
    int mark[20];
    for (int o = 0; o < 20; o++) mark[o] = 0;
    if (role == TMV2_ROLE_RANGE)
    {
        int hDelta = (have != 0 && have[0]) ? 1 : 0;
        int hBands = (have != 0 && have[1]) ? 1 : 0;
        int hVpA = (have != 0 && have[2]) ? 1 : 0;
        int hVpB = (have != 0 && have[3]) ? 1 : 0;
        int hVA = (have != 0 && have[4]) ? 1 : 0;
        if (!vapMultIs1) mark[5] = 1;
        if (!hDelta || !hBands) { mark[7] = 1; mark[8] = 1; mark[10] = 1; mark[11] = 1; }
        if (!hVpA) { mark[6] = 1; mark[10] = 1; mark[11] = 1; }
        if (!hVpB) { mark[7] = 1; mark[8] = 1; mark[9] = 1; mark[19] = 1; }
        if (!hVA) { mark[6] = 1; mark[10] = 1; mark[11] = 1; mark[19] = 1; }
    }
    else if (role == TMV2_ROLE_RENKO6)
    {
        int hDelta = (have != 0 && have[5]) ? 1 : 0;
        int hBands = (have != 0 && have[6]) ? 1 : 0;
        int hVp = (have != 0 && have[7]) ? 1 : 0;
        int hAskD = (have != 0 && have[8]) ? 1 : 0;
        int hBidD = (have != 0 && have[9]) ? 1 : 0;
        if (!hDelta || !hBands || !hVp) { mark[14] = 1; mark[15] = 1; }
        if (!hAskD || !hBidD) mark[14] = 1;
    }
    else if (role == TMV2_ROLE_RENKO8)
    {
        int hVp = (have != 0 && have[10]) ? 1 : 0;
        if (!hVp) { mark[16] = 1; mark[17] = 1; }
    }
    else
    {
        const int* ords = Tmv2_ReadyOrdinals();
        for (int i = 0; i < 16; i++) mark[ords[i]] = 1;
    }
    const int* ords = Tmv2_ReadyOrdinals();
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (mark[ords[i]]) disabled[n++] = ords[i];
    return n;
}

// Maximum direct formula footprint in bars (current + priors).
static int Tmv2_MaxDirectFootprint(int ord)
{
    switch (ord)
    {
    case 5: return 1;
    case 6: case 8: return 2;
    case 4: case 9: case 16: case 18: case 19: return 3;
    case 1: case 2: case 7: case 10: case 14: case 15: return 4;
    case 11: return 5;
    case 17: return 6;
    default: return -1;
    }
}

static int Tmv2_IsFinite(double x)
{
    return std::isfinite(x) ? 1 : 0;
}

// Flow primitive validity: AV/BV must be finite and non-negative.
// (Availability itself is enforced by the ACSIL layer via ok flags and
// array-size checks; zeros are structurally valid and left to the veto.)
static int Tmv2_FlowValid(double av, double bv)
{
    if (!Tmv2_IsFinite(av) || !Tmv2_IsFinite(bv)) return 0;
    if (av < 0.0 || bv < 0.0) return 0;
    return 1;
}

static double Tmv2_Total(double av, double bv) { return av + bv; }
static double Tmv2_Norm(double av, double bv)
{
    double t = av + bv;
    double denom = (t > 1.0) ? t : 1.0; // MAX(1,AV+BV), mechanical
    return (av - bv) / denom;
}

static int Tmv2_TickOk(const Tmv2Window* w)
{
    return (w->tick > 0.0 && Tmv2_IsFinite(w->tick)) ? 1 : 0;
}

static int Tmv2_FlowAt(const Tmv2Window* w, int k)
{
    return Tmv2_FlowValid(w->AV[k], w->BV[k]);
}

// Four-bar normalized least-squares slope, mechanical:
// (-3*n3 - n2 + n1 + 3*n0)/10 over offsets 3..0.
static double Tmv2_Slope4(const Tmv2Window* w)
{
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    double n3 = Tmv2_Norm(w->AV[3], w->BV[3]);
    return (-3.0 * n3 - n2 + n1 + 3.0 * n0) / 10.0;
}

// ---- Ordinal 1: Opposing Effort Decay v2 (Range, 4 bars) ----
static int Tmv2_F01Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 4; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    double n3 = Tmv2_Norm(w->AV[3], w->BV[3]);
    if (!(n0 < 0.0 && n1 < 0.0 && n2 < 0.0 && n3 < 0.0)) return 0;
    if (!(Tmv2_Slope4(w) >= 0.05)) return 0;
    if (!((n0 - n3) >= 0.20)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->C[3] - 2.0 * w->tick)) return 0;
    return 1;
}

static int Tmv2_F01Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 4; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    double n3 = Tmv2_Norm(w->AV[3], w->BV[3]);
    if (!(n0 > 0.0 && n1 > 0.0 && n2 > 0.0 && n3 > 0.0)) return 0;
    if (!(Tmv2_Slope4(w) <= -0.05)) return 0;
    if (!((n3 - n0) >= 0.20)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->C[3] + 2.0 * w->tick)) return 0;
    return 1;
}

// ---- Ordinal 2: Directional Effort Slope Response v2 (Range, 4 bars) ----
static int Tmv2_F02Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 4; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    double n3 = Tmv2_Norm(w->AV[3], w->BV[3]);
    if (!(n0 >= 0.10)) return 0;
    if (!(Tmv2_Slope4(w) >= 0.05)) return 0;
    if (!((n0 - n3) >= 0.25)) return 0;
    int steps = ((n2 > n3 && n1 > n2) || (n2 > n3 && n0 > n1) || (n1 > n2 && n0 > n1)) ? 1 : 0;
    if (!steps) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->C[3] + 2.0 * w->tick)) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

static int Tmv2_F02Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 4; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    double n3 = Tmv2_Norm(w->AV[3], w->BV[3]);
    if (!(n0 <= -0.10)) return 0;
    if (!(Tmv2_Slope4(w) <= -0.05)) return 0;
    if (!((n3 - n0) >= 0.25)) return 0;
    int steps = ((n2 < n3 && n1 < n2) || (n2 < n3 && n0 < n1) || (n1 < n2 && n0 < n1)) ? 1 : 0;
    if (!steps) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->C[3] - 2.0 * w->tick)) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

// ---- Ordinal 4: Extreme Effort Failure (Range, 3 bars) ----
static int Tmv2_F04Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->BV[0] >= 1.5 * w->AV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->L[0] <= w->L[1] - w->tick)) return 0;
    if (!(w->C[0] >= w->L[1] + w->tick)) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

static int Tmv2_F04Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->AV[0] >= 1.5 * w->BV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->H[0] >= w->H[1] + w->tick)) return 0;
    if (!(w->C[0] <= w->H[1] - w->tick)) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

// ---- Ordinal 5: VAP Gradient Divergence v2 Direct (Range, single bar) ----
static int Tmv2_F05VapRowsOk(const Tmv2Window* w)
{
    if (!w->vapMultIs1) return 0;
    for (int k = 0; k < 5; k++)
    {
        if (!w->vapOk[k]) return 0;
        if (!(w->vapTot[k] > 0.0)) return 0;
        if (!Tmv2_IsFinite(w->vapTot[k]) || !Tmv2_IsFinite(w->vapSide[k])) return 0;
    }
    return 1;
}

static int Tmv2_F05Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_FlowAt(w, 0)) return 0;
    if (!(w->L[0] > 0.0)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    if (!(t0 >= 20.0)) return 0;
    if (!((w->H[0] - w->L[0]) >= 4.0 * w->tick)) return 0;
    if (!Tmv2_F05VapRowsOk(w)) return 0;
    double sideSum = 0.0;
    for (int k = 0; k < 5; k++) sideSum += w->vapSide[k];
    if (!(sideSum >= 20.0)) return 0;
    if (!((w->AV[0] - w->BV[0]) <= -0.10 * t0)) return 0;
    if (!(w->C[0] > w->O[0])) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    const double* a = w->vapSide; // AVAP rows L .. L+4t
    if (!(a[4] >= 2.0 * a[0] + 10.0)) return 0;
    int grad = ((a[1] > a[0] && a[2] > a[1] && a[3] > a[2]) ||
                (a[1] > a[0] && a[2] > a[1] && a[4] > a[3]) ||
                (a[1] > a[0] && a[3] > a[2] && a[4] > a[3]) ||
                (a[2] > a[1] && a[3] > a[2] && a[4] > a[3])) ? 1 : 0;
    if (!grad) return 0;
    return 1;
}

static int Tmv2_F05Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_FlowAt(w, 0)) return 0;
    if (!((w->H[0] - 4.0 * w->tick) > 0.0)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    if (!(t0 >= 20.0)) return 0;
    if (!((w->H[0] - w->L[0]) >= 4.0 * w->tick)) return 0;
    if (!Tmv2_F05VapRowsOk(w)) return 0;
    double sideSum = 0.0;
    for (int k = 0; k < 5; k++) sideSum += w->vapSide[k];
    if (!(sideSum >= 20.0)) return 0;
    if (!((w->AV[0] - w->BV[0]) >= 0.10 * t0)) return 0;
    if (!(w->C[0] < w->O[0])) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    const double* b = w->vapSide; // BVAP rows H .. H-4t
    if (!(b[4] >= 2.0 * b[0] + 10.0)) return 0;
    int grad = ((b[1] > b[0] && b[2] > b[1] && b[3] > b[2]) ||
                (b[1] > b[0] && b[2] > b[1] && b[4] > b[3]) ||
                (b[1] > b[0] && b[3] > b[2] && b[4] > b[3]) ||
                (b[2] > b[1] && b[3] > b[2] && b[4] > b[3])) ? 1 : 0;
    if (!grad) return 0;
    return 1;
}

// ---- Ordinal 6: Whole-Distribution Migration (Range, 2 bars) ----
static int Tmv2_F06Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpaOk[0] || !w->vpaOk[1]) return 0;
    if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
    if (!(w->VpA[0] > 0.0 && w->VpA[1] > 0.0)) return 0;
    if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
    if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
    if (!(w->VpA[0] >= w->VpA[1] + w->tick)) return 0;
    if (!(w->VAH[0] >= w->VAH[1] + w->tick)) return 0;
    if (!(w->VAL[0] >= w->VAL[1] + w->tick)) return 0;
    if (!(w->C[0] >= w->VAH[0])) return 0;
    if (!(w->C[0] > w->C[1])) return 0;
    return 1;
}

static int Tmv2_F06Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpaOk[0] || !w->vpaOk[1]) return 0;
    if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
    if (!(w->VpA[0] > 0.0 && w->VpA[1] > 0.0)) return 0;
    if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
    if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
    if (!(w->VpA[0] <= w->VpA[1] - w->tick)) return 0;
    if (!(w->VAH[0] <= w->VAH[1] - w->tick)) return 0;
    if (!(w->VAL[0] <= w->VAL[1] - w->tick)) return 0;
    if (!(w->C[0] <= w->VAL[0])) return 0;
    if (!(w->C[0] < w->C[1])) return 0;
    return 1;
}

// ---- Ordinal 7: Delta Reversal Profile Response v2 (Range) ----
// Seed uses the PRIOR-FROZEN band: D[-2] vs band[-3]. Current D vs band[-1].
static int Tmv2_F07Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!w->vpbOk[0] || !w->vpbOk[1]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0)) return 0;
    if (!w->dOk[2] || !w->dOk[1] || !w->dOk[0]) return 0;
    if (!w->loOk[3] || !w->upOk[1]) return 0;
    if (!(w->D[2] <= w->Lo[3])) return 0;
    if (!(w->D[1] < 0.0)) return 0;
    if (!(w->D[0] >= w->Up[1])) return 0;
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    if (!(n0 >= 0.15)) return 0;
    if (!((n0 - n2) >= 0.60)) return 0;
    if (!(w->C[1] > w->O[1])) return 0;
    if (!(w->C[0] > w->O[0])) return 0;
    if (!(w->C[0] > w->C[1])) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->VpB[0] >= w->VpB[1] + w->tick)) return 0;
    if (!(w->C[0] >= w->VpB[0])) return 0;
    return 1;
}

static int Tmv2_F07Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!w->vpbOk[0] || !w->vpbOk[1]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0)) return 0;
    if (!w->dOk[2] || !w->dOk[1] || !w->dOk[0]) return 0;
    if (!w->upOk[3] || !w->loOk[1]) return 0;
    if (!(w->D[2] >= w->Up[3])) return 0;
    if (!(w->D[1] > 0.0)) return 0;
    if (!(w->D[0] <= w->Lo[1])) return 0;
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    if (!(n0 <= -0.15)) return 0;
    if (!((n2 - n0) >= 0.60)) return 0;
    if (!(w->C[1] < w->O[1])) return 0;
    if (!(w->C[0] < w->O[0])) return 0;
    if (!(w->C[0] < w->C[1])) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->VpB[0] <= w->VpB[1] - w->tick)) return 0;
    if (!(w->C[0] <= w->VpB[0])) return 0;
    return 1;
}

// ---- Ordinal 8: POC Delta v2 (Range, 2 bars; current D vs prior band) ----
static int Tmv2_F08Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 2; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!w->vpbOk[0] || !w->vpbOk[1]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0)) return 0;
    if (!w->dOk[0] || !w->upOk[1]) return 0;
    if (!(w->O[0] < w->VpB[1])) return 0;
    if (!(w->C[0] >= w->VpB[1])) return 0;
    if (!(w->VpB[0] >= w->VpB[1])) return 0;
    if (!(w->C[1] < w->O[1])) return 0;
    if (!(w->C[0] > w->O[0])) return 0;
    if (!(w->C[0] > w->C[1])) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->D[0] >= w->Up[1])) return 0;
    return 1;
}

static int Tmv2_F08Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    for (int k = 0; k < 2; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!w->vpbOk[0] || !w->vpbOk[1]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0)) return 0;
    if (!w->dOk[0] || !w->loOk[1]) return 0;
    if (!(w->O[0] > w->VpB[1])) return 0;
    if (!(w->C[0] <= w->VpB[1])) return 0;
    if (!(w->VpB[0] <= w->VpB[1])) return 0;
    if (!(w->C[1] > w->O[1])) return 0;
    if (!(w->C[0] < w->O[0])) return 0;
    if (!(w->C[0] < w->C[1])) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->D[0] <= w->Lo[1])) return 0;
    return 1;
}

// ---- Ordinal 9: Frozen VPOC Reclaim Core v2, PRIMARY only (Range, 3 bars) ----
static int Tmv2_F09Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpbOk[0] || !w->vpbOk[1]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->BV[0] >= 1.5 * w->AV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->L[0] <= w->VpB[1] - w->tick)) return 0;
    if (!(w->C[0] >= w->VpB[1] + w->tick)) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->VpB[0] >= w->VpB[1])) return 0;
    return 1;
}

static int Tmv2_F09Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpbOk[0] || !w->vpbOk[1]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->AV[0] >= 1.5 * w->BV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->H[0] >= w->VpB[1] + w->tick)) return 0;
    if (!(w->C[0] <= w->VpB[1] - w->tick)) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->VpB[0] <= w->VpB[1])) return 0;
    return 1;
}

// ---- Ordinal 10: Effort Reversal with Value Migration v2 (Range) ----
static int Tmv2_F10ProfileGatesBull(const Tmv2Window* w)
{
    if (!w->vpaOk[0] || !w->vpaOk[1]) return 0;
    if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
    if (!(w->VpA[0] > 0.0 && w->VpA[1] > 0.0)) return 0;
    if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
    if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
    return 1;
}

static int Tmv2_F10Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_F10ProfileGatesBull(w)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!w->dOk[2] || !w->dOk[1] || !w->dOk[0]) return 0;
    if (!w->loOk[3]) return 0;
    if (!(w->C[2] < w->O[2])) return 0;
    if (!(w->D[2] < 0.0)) return 0;
    if (!(w->D[2] <= w->Lo[3])) return 0;
    if (!(w->C[1] > w->O[1])) return 0;
    if (!(w->D[1] > 0.0)) return 0;
    if (!(w->C[0] > w->O[0])) return 0;
    if (!(w->D[0] > 0.0)) return 0;
    {
        double lhs = w->D[1] + w->D[0];
        double rhs = 1.25 * std::fabs(w->D[2]);
        if (!(lhs >= rhs)) return 0;
    }
    if (!(w->C[1] <= w->H[2])) return 0;
    if (!(w->C[0] >= w->H[2] + w->tick)) return 0;
    if (!(w->C[0] > w->C[1])) return 0;
    if (!(w->VpA[0] >= w->VpA[1] + w->tick)) return 0;
    {
        double mid0 = (w->VAH[0] + w->VAL[0]) / 2.0;
        double mid1 = (w->VAH[1] + w->VAL[1]) / 2.0;
        if (!(mid0 >= mid1 + w->tick)) return 0;
    }
    return 1;
}

static int Tmv2_F10Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_F10ProfileGatesBull(w)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!w->dOk[2] || !w->dOk[1] || !w->dOk[0]) return 0;
    if (!w->upOk[3]) return 0;
    if (!(w->C[2] > w->O[2])) return 0;
    if (!(w->D[2] > 0.0)) return 0;
    if (!(w->D[2] >= w->Up[3])) return 0;
    if (!(w->C[1] < w->O[1])) return 0;
    if (!(w->D[1] < 0.0)) return 0;
    if (!(w->C[0] < w->O[0])) return 0;
    if (!(w->D[0] < 0.0)) return 0;
    {
        double lhs = w->D[1] + w->D[0];
        double rhs = -1.25 * std::fabs(w->D[2]);
        if (!(lhs <= rhs)) return 0;
    }
    if (!(w->C[1] >= w->L[2])) return 0;
    if (!(w->C[0] <= w->L[2] - w->tick)) return 0;
    if (!(w->C[0] < w->C[1])) return 0;
    if (!(w->VpA[0] <= w->VpA[1] - w->tick)) return 0;
    {
        double mid0 = (w->VAH[0] + w->VAL[0]) / 2.0;
        double mid1 = (w->VAH[1] + w->VAL[1]) / 2.0;
        if (!(mid0 <= mid1 - w->tick)) return 0;
    }
    return 1;
}

// ---- Ordinal 11: Fixed POC Balance Migration v2 (Range, 5 bars) ----
// Anchor window is the four STRICTLY PRIOR bars [-1:-4]; current excluded.
static int Tmv2_F11Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpaOk[0] || !w->vpaOk[1] || !w->vpaOk[2] || !w->vpaOk[3] || !w->vpaOk[4]) return 0;
    if (!(w->VpA[0] > 0.0)) return 0;
    {
        double mn = w->VpA[1];
        double mx = w->VpA[1];
        for (int k = 2; k <= 4; k++)
        {
            if (!(w->VpA[k] > 0.0)) return 0;
            if (w->VpA[k] < mn) mn = w->VpA[k];
            if (w->VpA[k] > mx) mx = w->VpA[k];
        }
        if (!((mx - mn) <= 2.0 * w->tick)) return 0;
        if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
        if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
        if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
        if (!(w->C[2] < w->O[2])) return 0;
        if (!(w->C[1] > w->O[1])) return 0;
        if (!(w->C[0] > w->O[0])) return 0;
        if (!(w->C[1] <= mx)) return 0;
        if (!(w->C[0] > mx)) return 0;
        if (!(w->VpA[0] > mx)) return 0;
        double mid1 = (w->VAH[1] + w->VAL[1]) / 2.0;
        double mid0 = (w->VAH[0] + w->VAL[0]) / 2.0;
        if (!(mid1 <= mx)) return 0;
        if (!(mid0 > mx)) return 0;
        if (!w->dOk[1] || !w->dOk[0]) return 0;
        if (!w->upOk[2] || !w->upOk[1]) return 0;
        if (!(w->D[1] > 0.0 && w->D[0] > 0.0)) return 0;
        if (!((w->D[1] > w->Up[2]) || (w->D[0] > w->Up[1]))) return 0;
    }
    return 1;
}

static int Tmv2_F11Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpaOk[0] || !w->vpaOk[1] || !w->vpaOk[2] || !w->vpaOk[3] || !w->vpaOk[4]) return 0;
    if (!(w->VpA[0] > 0.0)) return 0;
    {
        double mn = w->VpA[1];
        double mx = w->VpA[1];
        for (int k = 2; k <= 4; k++)
        {
            if (!(w->VpA[k] > 0.0)) return 0;
            if (w->VpA[k] < mn) mn = w->VpA[k];
            if (w->VpA[k] > mx) mx = w->VpA[k];
        }
        if (!((mx - mn) <= 2.0 * w->tick)) return 0;
        if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
        if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
        if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
        if (!(w->C[2] > w->O[2])) return 0;
        if (!(w->C[1] < w->O[1])) return 0;
        if (!(w->C[0] < w->O[0])) return 0;
        if (!(w->C[1] >= mn)) return 0;
        if (!(w->C[0] < mn)) return 0;
        if (!(w->VpA[0] < mn)) return 0;
        double mid1 = (w->VAH[1] + w->VAL[1]) / 2.0;
        double mid0 = (w->VAH[0] + w->VAL[0]) / 2.0;
        if (!(mid1 >= mn)) return 0;
        if (!(mid0 < mn)) return 0;
        if (!w->dOk[1] || !w->dOk[0]) return 0;
        if (!w->loOk[2] || !w->loOk[1]) return 0;
        if (!(w->D[1] < 0.0 && w->D[0] < 0.0)) return 0;
        if (!((w->D[1] < w->Lo[2]) || (w->D[0] < w->Lo[1]))) return 0;
    }
    return 1;
}

// ---- Ordinal 14: OF Reversal Concentration + Profile Response vNext ----
// Renko 6t. SG59 diagonal counts required; no raw-volume approximation.
static int Tmv2_F14SeedBull(const Tmv2Window* w)
{
    if (!w->dOk[1] || !w->loOk[2] || !w->dOk[2] || !w->loOk[3]) return 0;
    int arm1 = (w->D[1] < 0.0 && w->D[1] <= w->Lo[2]) ? 1 : 0;
    int arm2 = (w->D[2] < 0.0 && w->D[2] <= w->Lo[3]) ? 1 : 0;
    return (arm1 || arm2) ? 1 : 0;
}

static int Tmv2_F14SeedBear(const Tmv2Window* w)
{
    if (!w->dOk[1] || !w->upOk[2] || !w->dOk[2] || !w->upOk[3]) return 0;
    int arm1 = (w->D[1] > 0.0 && w->D[1] >= w->Up[2]) ? 1 : 0;
    int arm2 = (w->D[2] > 0.0 && w->D[2] >= w->Up[3]) ? 1 : 0;
    return (arm1 || arm2) ? 1 : 0;
}

static int Tmv2_F14Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp6Ok[0] || !w->vp6Ok[1]) return 0;
    if (!(w->Vp6[0] > 0.0 && w->Vp6[1] > 0.0)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!Tmv2_F14SeedBull(w)) return 0;
    if (!w->dOk[0] || !w->upOk[1]) return 0;
    if (!(w->D[0] > 0.0)) return 0;
    if (!(w->D[0] >= w->Up[1])) return 0;
    if (!w->bidOk[1] || !w->askOk[0] || !w->bidOk[0]) return 0;
    if (!(w->BidD[1] >= 1.0)) return 0;
    if (!(w->AskD[0] >= 3.0)) return 0;
    if (!(w->BidD[0] <= 1.0)) return 0;
    if (!(w->C[1] <= w->Vp6[1])) return 0;
    if (!(w->O[0] <= w->Vp6[1])) return 0;
    if (!(w->C[0] >= w->Vp6[1] + w->tick)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->Vp6[0] >= w->Vp6[1] + w->tick)) return 0;
    return 1;
}

static int Tmv2_F14Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp6Ok[0] || !w->vp6Ok[1]) return 0;
    if (!(w->Vp6[0] > 0.0 && w->Vp6[1] > 0.0)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!Tmv2_F14SeedBear(w)) return 0;
    if (!w->dOk[0] || !w->loOk[1]) return 0;
    if (!(w->D[0] < 0.0)) return 0;
    if (!(w->D[0] <= w->Lo[1])) return 0;
    if (!w->askOk[1] || !w->bidOk[0] || !w->askOk[0]) return 0;
    if (!(w->AskD[1] >= 1.0)) return 0;
    if (!(w->BidD[0] >= 3.0)) return 0;
    if (!(w->AskD[0] <= 1.0)) return 0;
    if (!(w->C[1] >= w->Vp6[1])) return 0;
    if (!(w->O[0] >= w->Vp6[1])) return 0;
    if (!(w->C[0] <= w->Vp6[1] - w->tick)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->Vp6[0] <= w->Vp6[1] - w->tick)) return 0;
    return 1;
}

// ---- Ordinal 15: Delta Reversal VPOC Reclaim v2 (Renko 6t) ----
static int Tmv2_F15Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp6Ok[0] || !w->vp6Ok[1]) return 0;
    if (!(w->Vp6[0] > 0.0 && w->Vp6[1] > 0.0)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!(w->C[1] <= w->O[1])) return 0;
    if (!(w->C[0] > w->O[0])) return 0;
    if (!(w->L[1] <= w->L[2] - w->tick)) return 0;
    if (!(w->L[0] >= w->L[1])) return 0;
    if (!Tmv2_F14SeedBull(w)) return 0;
    if (!w->dOk[0] || !w->upOk[1]) return 0;
    if (!(w->D[0] > 0.0)) return 0;
    if (!(w->D[0] >= w->Up[1])) return 0;
    if (!(w->O[0] <= w->Vp6[1])) return 0;
    if (!(w->C[0] >= w->Vp6[1] + w->tick)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->Vp6[0] >= w->Vp6[1] + w->tick)) return 0;
    return 1;
}

static int Tmv2_F15Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp6Ok[0] || !w->vp6Ok[1]) return 0;
    if (!(w->Vp6[0] > 0.0 && w->Vp6[1] > 0.0)) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!(w->C[1] >= w->O[1])) return 0;
    if (!(w->C[0] < w->O[0])) return 0;
    if (!(w->H[1] >= w->H[2] + w->tick)) return 0;
    if (!(w->H[0] <= w->H[1])) return 0;
    if (!Tmv2_F14SeedBear(w)) return 0;
    if (!w->dOk[0] || !w->loOk[1]) return 0;
    if (!(w->D[0] < 0.0)) return 0;
    if (!(w->D[0] <= w->Lo[1])) return 0;
    if (!(w->O[0] >= w->Vp6[1])) return 0;
    if (!(w->C[0] <= w->Vp6[1] - w->tick)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->Vp6[0] <= w->Vp6[1] - w->tick)) return 0;
    return 1;
}

// ---- Ordinal 16: Frozen VPOC Cross Executed Effort v2, PRIMARY (Renko 8t) ----
static int Tmv2_F16Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp8Ok[0] || !w->vp8Ok[1]) return 0;
    if (!(w->Vp8[0] > 0.0 && w->Vp8[1] > 0.0)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->AV[0] >= 1.5 * w->BV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->O[0] < w->Vp8[1])) return 0;
    if (!(w->C[0] >= w->Vp8[1] + w->tick)) return 0;
    if (!(w->Vp8[0] >= w->Vp8[1] + w->tick)) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

static int Tmv2_F16Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp8Ok[0] || !w->vp8Ok[1]) return 0;
    if (!(w->Vp8[0] > 0.0 && w->Vp8[1] > 0.0)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->BV[0] >= 1.5 * w->AV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->O[0] > w->Vp8[1])) return 0;
    if (!(w->C[0] <= w->Vp8[1] - w->tick)) return 0;
    if (!(w->Vp8[0] <= w->Vp8[1] - w->tick)) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

// ---- Ordinal 17: Persistent Effort Volume Response v2, PRIMARY (Renko 8t) ----
static int Tmv2_F17Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp8Ok[0] || !w->vp8Ok[1]) return 0;
    if (!(w->Vp8[0] > 0.0 && w->Vp8[1] > 0.0)) return 0;
    for (int k = 0; k < 6; k++) if (!Tmv2_FlowAt(w, k)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double base = 0.0;
    for (int k = 1; k <= 5; k++)
    {
        double tk = Tmv2_Total(w->AV[k], w->BV[k]);
        if (!(tk > 0.0)) return 0;
        base += tk;
    }
    if (!(t0 >= 20.0)) return 0;
    if (!(t0 >= 1.25 * (base / 5.0))) return 0;
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    int persist = (((n0 >= 0.10) && (n1 >= 0.10)) ||
                   ((n0 >= 0.10) && (n2 >= 0.10)) ||
                   ((n1 >= 0.10) && (n2 >= 0.10))) ? 1 : 0;
    if (!persist) return 0;
    if (!((n0 + n1 + n2) >= 0.40)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->C[2] + 2.0 * w->tick)) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->Vp8[0] >= w->Vp8[1] + w->tick)) return 0;
    return 1;
}

static int Tmv2_F17Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vp8Ok[0] || !w->vp8Ok[1]) return 0;
    if (!(w->Vp8[0] > 0.0 && w->Vp8[1] > 0.0)) return 0;
    for (int k = 0; k < 6; k++) if (!Tmv2_FlowAt(w, k)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double base = 0.0;
    for (int k = 1; k <= 5; k++)
    {
        double tk = Tmv2_Total(w->AV[k], w->BV[k]);
        if (!(tk > 0.0)) return 0;
        base += tk;
    }
    if (!(t0 >= 20.0)) return 0;
    if (!(t0 >= 1.25 * (base / 5.0))) return 0;
    double n0 = Tmv2_Norm(w->AV[0], w->BV[0]);
    double n1 = Tmv2_Norm(w->AV[1], w->BV[1]);
    double n2 = Tmv2_Norm(w->AV[2], w->BV[2]);
    int persist = (((n0 <= -0.10) && (n1 <= -0.10)) ||
                   ((n0 <= -0.10) && (n2 <= -0.10)) ||
                   ((n1 <= -0.10) && (n2 <= -0.10))) ? 1 : 0;
    if (!persist) return 0;
    if (!((n0 + n1 + n2) <= -0.40)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->C[2] - 2.0 * w->tick)) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    if (!(w->Vp8[0] <= w->Vp8[1] - w->tick)) return 0;
    return 1;
}

// ---- Ordinal 18: Extreme Effort Failure Core v2, PRIMARY (Renko 8t) ----
static int Tmv2_F18Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->BV[0] >= 1.5 * w->AV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->L[0] <= w->L[1] - w->tick)) return 0;
    if (!(w->C[0] >= w->L[1] + w->tick)) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

static int Tmv2_F18Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!Tmv2_FlowAt(w, 0) || !Tmv2_FlowAt(w, 1) || !Tmv2_FlowAt(w, 2)) return 0;
    double t0 = Tmv2_Total(w->AV[0], w->BV[0]);
    double t1 = Tmv2_Total(w->AV[1], w->BV[1]);
    double t2 = Tmv2_Total(w->AV[2], w->BV[2]);
    if (!(t0 >= 20.0)) return 0;
    if (!(t1 > 0.0 && t2 > 0.0)) return 0;
    if (!(w->AV[0] >= 1.5 * w->BV[0])) return 0;
    if (!(t0 >= 1.25 * ((t1 + t2) / 2.0))) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->H[0] >= w->H[1] + w->tick)) return 0;
    if (!(w->C[0] <= w->H[1] - w->tick)) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    return 1;
}

// ---- Ordinal 19: Frozen POC Recovery + Distribution Confirmation v2 ----
// Anchor is frozen at [-2]; interim [-1] must depart, current must recover.
static int Tmv2_F19Bull(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpbOk[0] || !w->vpbOk[1] || !w->vpbOk[2]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0 && w->VpB[2] > 0.0)) return 0;
    if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
    if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
    if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!(w->C[2] < w->O[2])) return 0;
    if (!(w->C[1] > w->O[1])) return 0;
    if (!(w->C[0] > w->O[0])) return 0;
    if (!(w->VpB[1] <= w->VpB[2] - w->tick)) return 0;
    if (!(w->VpB[0] >= w->VpB[2] + w->tick)) return 0;
    if (!(w->C[0] >= w->VpB[2] + w->tick)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] >= w->H[0] - 0.25 * (w->H[0] - w->L[0]))) return 0;
    {
        double mid0 = (w->VAH[0] + w->VAL[0]) / 2.0;
        double mid1 = (w->VAH[1] + w->VAL[1]) / 2.0;
        if (!(mid0 >= mid1 + w->tick)) return 0;
        if (!(mid0 > w->VpB[2])) return 0;
    }
    return 1;
}

static int Tmv2_F19Bear(const Tmv2Window* w)
{
    if (!Tmv2_TickOk(w)) return 0;
    if (!w->vpbOk[0] || !w->vpbOk[1] || !w->vpbOk[2]) return 0;
    if (!(w->VpB[0] > 0.0 && w->VpB[1] > 0.0 && w->VpB[2] > 0.0)) return 0;
    if (!w->vahOk[0] || !w->valOk[0] || !w->vahOk[1] || !w->valOk[1]) return 0;
    if (!(w->VAH[0] > 0.0 && w->VAL[0] > 0.0 && w->VAH[0] >= w->VAL[0])) return 0;
    if (!(w->VAH[1] > 0.0 && w->VAL[1] > 0.0 && w->VAH[1] >= w->VAL[1])) return 0;
    for (int k = 0; k < 3; k++)
    {
        if (!Tmv2_FlowAt(w, k)) return 0;
        if (Tmv2_Total(w->AV[k], w->BV[k]) < 20.0) return 0;
    }
    if (!(w->C[2] > w->O[2])) return 0;
    if (!(w->C[1] < w->O[1])) return 0;
    if (!(w->C[0] < w->O[0])) return 0;
    if (!(w->VpB[1] >= w->VpB[2] + w->tick)) return 0;
    if (!(w->VpB[0] <= w->VpB[2] - w->tick)) return 0;
    if (!(w->C[0] <= w->VpB[2] - w->tick)) return 0;
    if (!(w->H[0] > w->L[0])) return 0;
    if (!(w->C[0] <= w->L[0] + 0.25 * (w->H[0] - w->L[0]))) return 0;
    {
        double mid0 = (w->VAH[0] + w->VAL[0]) / 2.0;
        double mid1 = (w->VAH[1] + w->VAL[1]) / 2.0;
        if (!(mid0 <= mid1 - w->tick)) return 0;
        if (!(mid0 < w->VpB[2])) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Role dispatch: evaluate only families belonging to `role` into fired[32]
// indexed by output SG (even=Bull, odd=Bear). Non-role outputs stay zero.
// Returns count of fired outputs.
// ---------------------------------------------------------------------------
static int Tmv2_EvalRole(int role, const Tmv2Window* w, int fired[32])
{
    for (int s = 0; s < 32; s++) fired[s] = 0;
    int n = 0;
#define TMV2_TRY(ord, bullFn, bearFn) do { \
        if (Tmv2_FamilyRole(ord) == role) { \
            int sb = Tmv2_FamilySgBull(ord); \
            if (bullFn(w)) { fired[sb] = 1; n++; } \
            if (bearFn(w)) { fired[sb + 1] = 1; n++; } \
        } \
    } while (0)
    TMV2_TRY(1, Tmv2_F01Bull, Tmv2_F01Bear);
    TMV2_TRY(2, Tmv2_F02Bull, Tmv2_F02Bear);
    TMV2_TRY(4, Tmv2_F04Bull, Tmv2_F04Bear);
    TMV2_TRY(5, Tmv2_F05Bull, Tmv2_F05Bear);
    TMV2_TRY(6, Tmv2_F06Bull, Tmv2_F06Bear);
    TMV2_TRY(7, Tmv2_F07Bull, Tmv2_F07Bear);
    TMV2_TRY(8, Tmv2_F08Bull, Tmv2_F08Bear);
    TMV2_TRY(9, Tmv2_F09Bull, Tmv2_F09Bear);
    TMV2_TRY(10, Tmv2_F10Bull, Tmv2_F10Bear);
    TMV2_TRY(11, Tmv2_F11Bull, Tmv2_F11Bear);
    TMV2_TRY(14, Tmv2_F14Bull, Tmv2_F14Bear);
    TMV2_TRY(15, Tmv2_F15Bull, Tmv2_F15Bear);
    TMV2_TRY(16, Tmv2_F16Bull, Tmv2_F16Bear);
    TMV2_TRY(17, Tmv2_F17Bull, Tmv2_F17Bear);
    TMV2_TRY(18, Tmv2_F18Bull, Tmv2_F18Bear);
    TMV2_TRY(19, Tmv2_F19Bull, Tmv2_F19Bear);
#undef TMV2_TRY
    return n;
}

// ---------------------------------------------------------------------------
// Fail-closed history warm-ups: minimum zero-based current bar index per
// ordinal. Band families need up to 23 completed bars for a 20-period band
// at offset -3, so zero/unpublished band values can never read as valid.
// Returns -1 for blocked/unknown ordinals.
// ---------------------------------------------------------------------------
static int Tmv2_MinBarIndex(int ord)
{
    switch (ord)
    {
    case 1: return 3;
    case 2: return 3;
    case 4: return 2;
    case 5: return 0;
    case 6: return 1;
    case 7: return 22;
    case 8: return 20;
    case 9: return 2;
    case 10: return 22;
    case 11: return 21;
    case 14: return 22;
    case 15: return 22;
    case 16: return 2;
    case 17: return 5;
    case 18: return 2;
    case 19: return 2;
    default: return -1;
    }
}

// Clear fired outputs whose family warm-up is not satisfied at barIndex.
static void Tmv2_ApplyWarmup(int role, int barIndex, int fired[32])
{
    const int* ords = Tmv2_ReadyOrdinals();
    for (int i = 0; i < 16; i++)
    {
        int ord = ords[i];
        if (Tmv2_FamilyRole(ord) != role) continue;
        if (barIndex < Tmv2_MinBarIndex(ord))
        {
            int sb = Tmv2_FamilySgBull(ord);
            if (sb >= 0) { fired[sb] = 0; fired[sb + 1] = 0; }
        }
    }
}

// Role dispatch with warm-up gating. Returns count of surviving outputs.
static int Tmv2_EvalRoleAt(int role, const Tmv2Window* w, int barIndex, int fired[32])
{
    int n = Tmv2_EvalRole(role, w, fired);
    Tmv2_ApplyWarmup(role, barIndex, fired);
    n = 0;
    for (int s = 0; s < 32; s++) n += fired[s] ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------------------
// Deterministic stacking: dense side-specific lanes in SG/catalog order.
// DRAWSTYLE_IGNORE outputs keep truth (fired stays 1) but take no lane
// (lane stays -1). Idle outputs report lane -1.
// ---------------------------------------------------------------------------
static void Tmv2_StackLanes(const int fired[32], const int hidden[32],
                            int bullLane[32], int bearLane[32])
{
    for (int s = 0; s < 32; s++) { bullLane[s] = -1; bearLane[s] = -1; }
    int bl = 0;
    for (int s = 0; s < 32; s += 2)
        if (fired[s] && !hidden[s]) bullLane[s] = bl++;
    int rl = 0;
    for (int s = 1; s < 32; s += 2)
        if (fired[s] && !hidden[s]) bearLane[s] = rl++;
}

static double Tmv2_StackY(int isBull, int lane, double low, double high,
                          int baseTicks, int stepTicks, double tick)
{
    if (isBull) return low - (baseTicks + lane * stepTicks) * tick;
    return high + (baseTicks + lane * stepTicks) * tick;
}

// ---------------------------------------------------------------------------
// 64-bit FNV-1a structural fingerprint over schema, catalog pin, role,
// source IDs, VAP flag, config revision, display geometry, and tick bits.
// ---------------------------------------------------------------------------
static void Tmv2_FnvMix(unsigned long long* h, unsigned long long v)
{
    for (int b = 0; b < 8; b++)
    {
        *h ^= (unsigned long long)((v >> (b * 8)) & 0xFFu);
        *h *= 1099511628211ULL;
    }
}

static unsigned long long Tmv2_Fingerprint(int role, const int ids[11],
                                           int vapFlag, int revCfg,
                                           int bullBase, int bearBase,
                                           int step, unsigned long long tickBits)
{
    unsigned long long h = TMV2_FNV_OFFSET_BASIS;
    Tmv2_FnvMix(&h, (unsigned long long)(TMV2_SCHEMA_VERSION * 31 + 7));
    const char* pin = TMV2_CATALOG_VERSION TMV2_CATALOG_SHA;
    for (const char* p = pin; *p; p++)
    {
        h ^= (unsigned long long)(unsigned char)(*p);
        h *= 1099511628211ULL;
    }
    Tmv2_FnvMix(&h, (unsigned long long)(role + 0x9e37));
    for (int i = 0; i < 11; i++)
        Tmv2_FnvMix(&h, (unsigned long long)(ids[i] + 0x10001 * (i + 1)));
    Tmv2_FnvMix(&h, (unsigned long long)vapFlag);
    Tmv2_FnvMix(&h, (unsigned long long)revCfg);
    Tmv2_FnvMix(&h, (unsigned long long)bullBase);
    Tmv2_FnvMix(&h, (unsigned long long)bearBase);
    Tmv2_FnvMix(&h, (unsigned long long)step);
    Tmv2_FnvMix(&h, tickBits);
    if (h == 0) h = 0x9e3779b97f4a7c15ULL;
    return h;
}

static void Tmv2_FingerprintSplit(unsigned long long fp, int* lo, int* hi)
{
    unsigned int uLo = (unsigned int)(fp & 0xFFFFFFFFULL);
    unsigned int uHi = (unsigned int)((fp >> 32) & 0xFFFFFFFFULL);
    std::memcpy(lo, &uLo, 4);
    std::memcpy(hi, &uHi, 4);
}

static unsigned long long Tmv2_FingerprintJoin(int lo, int hi)
{
    unsigned int uLo, uHi;
    std::memcpy(&uLo, &lo, 4);
    std::memcpy(&uHi, &hi, 4);
    return ((unsigned long long)uHi << 32) | uLo;
}

// ============================================================================
// ACSIL study body (excluded from portable unit tests).
// ============================================================================
#ifndef TMV2_UNITTEST

SCDLLName("TriggerMatrixV2")

// Producer subgraph indices are defined once in the portable seam above
// (zero-based ACSIL SubgraphIndex = catalog one-based SGn minus 1).
// Exact-tick VAP lookup, no nearest-row substitution.
static int Tmv2_AcsilVapRow(SCStudyInterfaceRef sc, int barIndex, int targetTick,
                            float* tot, float* ask, float* bid)
{
    if (sc.VolumeAtPriceForBars == NULL) return 0;
    const int levels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(barIndex);
    for (int k = 0; k < levels; k++)
    {
        const s_VolumeAtPriceV2* e = NULL;
        if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(barIndex, k, &e)) break;
        if (e == NULL) continue;
        if (e->PriceInTicks == targetTick)
        {
            *tot = e->Volume;
            *ask = e->AskVolume;
            *bid = e->BidVolume;
            return 1;
        }
    }
    return 0;
}

// Fill one side of the family-5 VAP rows: bullSide=1 -> AVAP at L+k*t,
// bullSide=0 -> BVAP at H-k*t. All five rows must exist with total > 0.
static void Tmv2_AcsilFillVap(SCStudyInterfaceRef sc, int i, int bullSide,
                              int vapMultIs1, Tmv2Window* w)
{
    for (int k = 0; k < 5; k++) w->vapOk[k] = 0;
    if (!vapMultIs1) return;
    if (!(sc.TickSize > 0.0f)) return;
    if (sc.VolumeAtPriceForBars == NULL) return;
    const float tick = sc.TickSize;
    const float extreme = bullSide ? sc.Low[i] : sc.High[i];
    for (int k = 0; k < 5; k++)
    {
        float price = bullSide ? (extreme + k * tick) : (extreme - k * tick);
        int targetTick = (int)floor(price / tick + 0.5f);
        float tot = 0.0f, ask = 0.0f, bid = 0.0f;
        if (!Tmv2_AcsilVapRow(sc, i, targetTick, &tot, &ask, &bid)) return;
        if (!(tot > 0.0f)) return;
        w->vapTot[k] = (double)tot;
        w->vapSide[k] = bullSide ? (double)ask : (double)bid;
        w->vapOk[k] = 1;
    }
}

SCSFExport scsf_TriggerMatrixV2(SCStudyInterfaceRef sc)
{
    SCInputRef In_Role      = sc.Input[0];
    SCInputRef In_RDelta    = sc.Input[1];
    SCInputRef In_RBands    = sc.Input[2];
    SCInputRef In_RVpA      = sc.Input[3];
    SCInputRef In_RVpB      = sc.Input[4];
    SCInputRef In_RVA       = sc.Input[5];
    SCInputRef In_R6Delta   = sc.Input[6];
    SCInputRef In_R6Bands   = sc.Input[7];
    SCInputRef In_R6Vp      = sc.Input[8];
    SCInputRef In_R6AskD    = sc.Input[9];
    SCInputRef In_R6BidD    = sc.Input[10];
    SCInputRef In_R8Vp      = sc.Input[11];
    SCInputRef In_BullBase  = sc.Input[12];
    SCInputRef In_BearBase  = sc.Input[13];
    SCInputRef In_Step      = sc.Input[14];
    SCInputRef In_VapMult1  = sc.Input[15];
    SCInputRef In_SrcRev    = sc.Input[16];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Trigger Matrix V2 Display v1.0";
        sc.StudyDescription =
            "Display-only primary V2 detectors (16 families, 32 Bull/Bear outputs). "
            "Catalog 2.0.0-research-2026-09-09 "
            "sha ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618. "
            "One instance per chart role. No alerts, no trading.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.MaintainVolumeAtPriceData = 1;

        const char* bullName[16] = {
            "OED+ | Bull | Opposing Effort Decay v2",
            "DES+ | Bull | Directional Effort Slope Response v2",
            "EEF+ | Bull | Extreme Effort Failure",
            "VGD+ | Bull | VAP Gradient Divergence v2 Direct",
            "WDM+ | Bull | Whole-Distribution Migration with Directional Edge Close",
            "DRP+ | Bull | Delta Reversal Profile Response v2",
            "PDR+ | Bull | POC Delta v2",
            "FVR+ | Bull | Frozen VPOC Reclaim Core v2",
            "ERM+ | Bull | Effort Reversal with Value Migration v2",
            "PBM+ | Bull | Fixed POC Balance Migration v2",
            "OFR+ | Bull | Order-Flow Reversal Concentration vNext",
            "DVR+ | Bull | Delta Reversal VPOC Reclaim v2",
            "VXC+ | Bull | Frozen VPOC Cross Executed Effort v2",
            "PEV+ | Bull | Persistent Effort Volume Response v2",
            "R8F+ | Bull | Extreme Effort Failure Core v2",
            "FPR+ | Bull | Frozen POC Recovery + Distribution Confirmation v2"};
        const char* bearName[16] = {
            "OED- | Bear | Opposing Effort Decay v2",
            "DES- | Bear | Directional Effort Slope Response v2",
            "EEF- | Bear | Extreme Effort Failure",
            "VGD- | Bear | VAP Gradient Divergence v2 Direct",
            "WDM- | Bear | Whole-Distribution Migration with Directional Edge Close",
            "DRP- | Bear | Delta Reversal Profile Response v2",
            "PDR- | Bear | POC Delta v2",
            "FVR- | Bear | Frozen VPOC Reclaim Core v2",
            "ERM- | Bear | Effort Reversal with Value Migration v2",
            "PBM- | Bear | Fixed POC Balance Migration v2",
            "OFR- | Bear | Order-Flow Reversal Concentration vNext",
            "DVR- | Bear | Delta Reversal VPOC Reclaim v2",
            "VXC- | Bear | Frozen VPOC Cross Executed Effort v2",
            "PEV- | Bear | Persistent Effort Volume Response v2",
            "R8F- | Bear | Extreme Effort Failure Core v2",
            "FPR- | Bear | Frozen POC Recovery + Distribution Confirmation v2"};
        const char* bullText[16] = {
            "OED+", "DES+", "EEF+", "VGD+", "WDM+", "DRP+", "PDR+", "FVR+",
            "ERM+", "PBM+", "OFR+", "DVR+", "VXC+", "PEV+", "R8F+", "FPR+"};
        const char* bearText[16] = {
            "OED-", "DES-", "EEF-", "VGD-", "WDM-", "DRP-", "PDR-", "FVR-",
            "ERM-", "PBM-", "OFR-", "DVR-", "VXC-", "PEV-", "R8F-", "FPR-"};

        for (int f = 0; f < 16; f++)
        {
            const int sgB = f * 2;
            const int sgR = f * 2 + 1;
            sc.Subgraph[sgB].Name = bullName[f];
            sc.Subgraph[sgR].Name = bearName[f];
            if (f == 1) // DES: arrows
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_ARROW_UP;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_ARROW_DOWN;
            }
            else if (f == 9) // PBM: squares
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_SQUARE;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_SQUARE;
            }
            else
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_TEXT;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_TEXT;
                sc.Subgraph[sgB].TextDrawStyleText = bullText[f];
                sc.Subgraph[sgR].TextDrawStyleText = bearText[f];
                sc.Subgraph[sgB].LineWidth = 8;
                sc.Subgraph[sgR].LineWidth = 8;
            }
            if (f == 1 || f == 9)
            {
                sc.Subgraph[sgB].LineWidth = 4;
                sc.Subgraph[sgR].LineWidth = 4;
            }
            sc.Subgraph[sgB].PrimaryColor = RGB(0, 200, 255);
            sc.Subgraph[sgR].PrimaryColor = RGB(255, 159, 28);
            sc.Subgraph[sgB].DrawZeros = 0;
            sc.Subgraph[sgR].DrawZeros = 0;
        }

        In_Role.Name = "Chart Role";
        In_Role.SetCustomInputStrings("Range;Renko 6t;Renko 8t");
        In_Role.SetCustomInputIndex(0);

        In_RDelta.Name = "Range Delta Study (fixed SG4)";
        In_RDelta.SetStudySubgraphValues(0, TMV2_SG_DELTA);
        In_RBands.Name = "Range Delta Bands Study (fixed SG1/SG3, 20/0.9 SMA)";
        In_RBands.SetStudySubgraphValues(0, TMV2_SG_BAND_UP);
        In_RVpA.Name = "Range VPOC A Study (fixed SG1)";
        In_RVpA.SetStudySubgraphValues(0, TMV2_SG_VPOC);
        In_RVpB.Name = "Range VPOC B Study (fixed SG1)";
        In_RVpB.SetStudySubgraphValues(0, TMV2_SG_VPOC);
        In_RVA.Name = "Range VA 68 Study (fixed SG1 VVAH / SG2 VVAL)";
        In_RVA.SetStudySubgraphValues(0, TMV2_SG_VVAH);
        In_R6Delta.Name = "Renko 6 Delta Study (fixed SG4)";
        In_R6Delta.SetStudySubgraphValues(0, TMV2_SG_DELTA);
        In_R6Bands.Name = "Renko 6 Delta Bands Study (fixed SG1/SG3)";
        In_R6Bands.SetStudySubgraphValues(0, TMV2_SG_BAND_UP);
        In_R6Vp.Name = "Renko 6 VPOC Study (fixed SG1)";
        In_R6Vp.SetStudySubgraphValues(0, TMV2_SG_VPOC);
        In_R6AskD.Name = "Renko 6 Ask Diagonal Study (fixed SG59, +300%)";
        In_R6AskD.SetStudySubgraphValues(0, TMV2_SG_DIAG);
        In_R6BidD.Name = "Renko 6 Bid Diagonal Study (fixed SG59, -300%)";
        In_R6BidD.SetStudySubgraphValues(0, TMV2_SG_DIAG);
        In_R8Vp.Name = "Renko 8 VPOC Study (fixed SG1)";
        In_R8Vp.SetStudySubgraphValues(0, TMV2_SG_VPOC);

        In_BullBase.Name = "Bull Base Offset (ticks, display only)";
        In_BullBase.SetInt(2);
        In_BullBase.SetIntLimits(0, 50);
        In_BearBase.Name = "Bear Base Offset (ticks, display only)";
        In_BearBase.SetInt(2);
        In_BearBase.SetIntLimits(0, 50);
        In_Step.Name = "Stack Step (ticks, display only)";
        In_Step.SetInt(3);
        In_Step.SetIntLimits(1, 50);
        In_VapMult1.Name = "VAP Multiplier Is 1 (family-5 precondition)";
        In_VapMult1.SetYesNo(0);
        In_SrcRev.Name = "Source Configuration Revision";
        In_SrcRev.SetInt(1);
        In_SrcRev.SetIntLimits(1, 9999);
        return;
    }

    // ---- Runtime ----
    const int role = In_Role.GetIndex();
    const int bullBase = In_BullBase.GetInt();
    const int bearBase = In_BearBase.GetInt();
    const int stepTicks = In_Step.GetInt();
    const int vapMultIs1 = In_VapMult1.GetYesNo();
    const int srcRev = In_SrcRev.GetInt();
    const float tickF = sc.TickSize;

    int ids[11];
    ids[0] = (int)In_RDelta.GetStudyID();
    ids[1] = (int)In_RBands.GetStudyID();
    ids[2] = (int)In_RVpA.GetStudyID();
    ids[3] = (int)In_RVpB.GetStudyID();
    ids[4] = (int)In_RVA.GetStudyID();
    ids[5] = (int)In_R6Delta.GetStudyID();
    ids[6] = (int)In_R6Bands.GetStudyID();
    ids[7] = (int)In_R6Vp.GetStudyID();
    ids[8] = (int)In_R6AskD.GetStudyID();
    ids[9] = (int)In_R6BidD.GetStudyID();
    ids[10] = (int)In_R8Vp.GetStudyID();

    // Fetch role-relevant producer arrays once per update.
    SCFloatArray aDelta, aUp, aLo, aVpA, aVpB, aVAH, aVAL;
    SCFloatArray aVp6, aAskD, aBidD, aVp8;
    int idDelta = 0, idBands = 0, idVpA = 0, idVpB = 0, idVA = 0;
    int idVp6 = 0, idAskD = 0, idBidD = 0, idVp8 = 0;
    if (role == TMV2_ROLE_RANGE)
    {
        idDelta = ids[0]; idBands = ids[1]; idVpA = ids[2]; idVpB = ids[3]; idVA = ids[4];
        if (idDelta > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idDelta, TMV2_SG_DELTA, aDelta);
        if (idBands > 0)
        {
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idBands, TMV2_SG_BAND_UP, aUp);
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idBands, TMV2_SG_BAND_LO, aLo);
        }
        if (idVpA > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idVpA, TMV2_SG_VPOC, aVpA);
        if (idVpB > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idVpB, TMV2_SG_VPOC, aVpB);
        if (idVA > 0)
        {
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idVA, TMV2_SG_VVAH, aVAH);
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idVA, TMV2_SG_VVAL, aVAL);
        }
    }
    else if (role == TMV2_ROLE_RENKO6)
    {
        idDelta = ids[5]; idBands = ids[6]; idVp6 = ids[7]; idAskD = ids[8]; idBidD = ids[9];
        if (idDelta > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idDelta, TMV2_SG_DELTA, aDelta);
        if (idBands > 0)
        {
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idBands, TMV2_SG_BAND_UP, aUp);
            sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idBands, TMV2_SG_BAND_LO, aLo);
        }
        if (idVp6 > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idVp6, TMV2_SG_VPOC, aVp6);
        if (idAskD > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idAskD, TMV2_SG_DIAG, aAskD);
        if (idBidD > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idBidD, TMV2_SG_DIAG, aBidD);
    }
    else if (role == TMV2_ROLE_RENKO8)
    {
        idVp8 = ids[10];
        if (idVp8 > 0) sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, idVp8, TMV2_SG_VPOC, aVp8);
    }

    // Structural fingerprint (two persistent ints, slots 4-5).
    unsigned int tickBits32 = 0;
    {
        float tf = tickF;
        std::memcpy(&tickBits32, &tf, 4);
    }
    unsigned long long fp = Tmv2_Fingerprint(role, ids, vapMultIs1, srcRev,
                                            bullBase, bearBase, stepTicks,
                                            (unsigned long long)tickBits32);
    int fpLo = 0, fpHi = 0;
    Tmv2_FingerprintSplit(fp, &fpLo, &fpHi);
    int& storedLo = sc.GetPersistentInt(4);
    int& storedHi = sc.GetPersistentInt(5);
    const int fingerprintChanged = (storedLo != fpLo || storedHi != fpHi) ? 1 : 0;

    // One Message Log summary per new structural fingerprint: names the
    // selected role, each missing structural input/precondition, and each
    // disabled ordinal/code. Emitted BEFORE the ArraySize<2 early return so
    // tiny charts still report. Dynamic per-bar empty/short producer arrays
    // are NOT logged here; the bar loop fails closed silently (no log spam).
    if (fingerprintChanged)
    {
        int have[11];
        for (int h = 0; h < 11; h++) have[h] = (ids[h] > 0) ? 1 : 0;
        int disabled[16];
        const int nDis = Tmv2_StructDisabled(role, have, vapMultIs1, disabled);
        const char* roleName = (role == 0) ? "Range" : (role == 1) ? "Renko 6t" : (role == 2) ? "Renko 8t" : "Unknown";
        char missBuf[384];
        missBuf[0] = 0;
        int missLen = 0;
        int missCount = 0;
        char item[64];
        // Role-relevant missing structural inputs only; off-role IDs ignored.
        if (role == TMV2_ROLE_RANGE)
        {
            const int rel[5] = {0, 1, 2, 3, 4};
            if (!vapMultIs1)
            {
                std::snprintf(item, sizeof(item), "VAP Multiplier Is 1=No");
                std::snprintf(missBuf + missLen, sizeof(missBuf) - (unsigned)missLen,
                              "%s%s", missLen > 0 ? ", " : "", item);
                missLen = (int)std::strlen(missBuf);
                missCount++;
            }
            for (int r = 0; r < 5; r++)
            {
                int si = rel[r];
                if (have[si]) continue;
                std::snprintf(item, sizeof(item), "%s(id=%d)", Tmv2_SrcLabel(si), ids[si]);
                std::snprintf(missBuf + missLen, sizeof(missBuf) - (unsigned)missLen,
                              "%s%s", missLen > 0 ? ", " : "", item);
                missLen = (int)std::strlen(missBuf);
                missCount++;
            }
        }
        else if (role == TMV2_ROLE_RENKO6)
        {
            const int rel[5] = {5, 6, 7, 8, 9};
            for (int r = 0; r < 5; r++)
            {
                int si = rel[r];
                if (have[si]) continue;
                std::snprintf(item, sizeof(item), "%s(id=%d)", Tmv2_SrcLabel(si), ids[si]);
                std::snprintf(missBuf + missLen, sizeof(missBuf) - (unsigned)missLen,
                              "%s%s", missLen > 0 ? ", " : "", item);
                missLen = (int)std::strlen(missBuf);
                missCount++;
            }
        }
        else if (role == TMV2_ROLE_RENKO8)
        {
            if (!have[10])
            {
                std::snprintf(item, sizeof(item), "%s(id=%d)", Tmv2_SrcLabel(10), ids[10]);
                std::snprintf(missBuf + missLen, sizeof(missBuf) - (unsigned)missLen,
                              "%s%s", missLen > 0 ? ", " : "", item);
                missLen = (int)std::strlen(missBuf);
                missCount++;
            }
        }
        else
        {
            std::snprintf(missBuf, sizeof(missBuf), "unknown role=%d", role);
            missCount = 1;
        }
        if (missCount == 0) std::snprintf(missBuf, sizeof(missBuf), "none");
        char disBuf[384];
        disBuf[0] = 0;
        int disLen = 0;
        for (int d = 0; d < nDis; d++)
        {
            std::snprintf(item, sizeof(item), "ord%d/%s", disabled[d], Tmv2_FamilyCode(disabled[d]));
            std::snprintf(disBuf + disLen, sizeof(disBuf) - (unsigned)disLen,
                          "%s%s", disLen > 0 ? ", " : "", item);
            disLen = (int)std::strlen(disBuf);
        }
        if (nDis == 0) std::snprintf(disBuf, sizeof(disBuf), "none");
        char logMsg[1024];
        std::snprintf(logMsg, sizeof(logMsg),
                      "TriggerMatrixV2: structural rebuild role=%s srcRev=%d vapMult1=%d "
                      "| missing: %s | disabled: %s "
                      "| dynamic per-bar gaps fail closed silently.",
                      roleName, srcRev, vapMultIs1, missBuf, disBuf);
        sc.AddMessageToLog(logMsg, 0);
        storedLo = fpLo;
        storedHi = fpHi;
    }

    const int lastClosed = sc.ArraySize - 2;
    if (lastClosed < 0)
    {
        if (sc.ArraySize > 0)
            for (int s = 0; s < 32; s++) sc.Subgraph[s][sc.ArraySize - 1] = 0.0f;
        return;
    }

    const int structuralRebuild = (sc.IsFullRecalculation || sc.UpdateStartIndex == 0 || fingerprintChanged) ? 1 : 0;
    int first = structuralRebuild ? 0 : sc.UpdateStartIndex;
    if (first < 0) first = 0;
    if (first > lastClosed) first = lastClosed;

    int hidden[32];
    for (int s = 0; s < 32; s++)
        hidden[s] = (sc.Subgraph[s].DrawStyle == DRAWSTYLE_IGNORE) ? 1 : 0;

    const int baseOK = (sc.BaseData[SC_ASKVOL].GetArraySize() >= sc.ArraySize &&
                        sc.BaseData[SC_BIDVOL].GetArraySize() >= sc.ArraySize) ? 1 : 0;

    for (int i = first; i <= lastClosed; i++)
    {
        for (int s = 0; s < 32; s++) sc.Subgraph[s][i] = 0.0f;
        if (!baseOK) continue;

        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        w.tick = (double)tickF;
        for (int k = 0; k < 6; k++)
        {
            int b = i - k;
            if (b < 0) break;
            w.O[k] = (double)sc.Open[b];
            w.H[k] = (double)sc.High[b];
            w.L[k] = (double)sc.Low[b];
            w.C[k] = (double)sc.Close[b];
            w.AV[k] = (double)sc.BaseData[SC_ASKVOL][b];
            w.BV[k] = (double)sc.BaseData[SC_BIDVOL][b];
        }
        // Role-local delta + bands (offsets as each formula requires).
        if (idDelta > 0 && aDelta.GetArraySize() > i)
            for (int k = 0; k < 6 && i - k >= 0; k++)
            {
                w.D[k] = (double)aDelta[i - k];
                w.dOk[k] = 1;
            }
        if (idBands > 0)
        {
            if (aUp.GetArraySize() > i)
                for (int k = 0; k < 6 && i - k >= 0; k++)
                {
                    w.Up[k] = (double)aUp[i - k];
                    w.upOk[k] = 1;
                }
            if (aLo.GetArraySize() > i)
                for (int k = 0; k < 6 && i - k >= 0; k++)
                {
                    w.Lo[k] = (double)aLo[i - k];
                    w.loOk[k] = 1;
                }
        }
        if (idVpA > 0 && aVpA.GetArraySize() > i)
            for (int k = 0; k < 6 && i - k >= 0; k++)
            {
                w.VpA[k] = (double)aVpA[i - k];
                w.vpaOk[k] = 1;
            }
        if (idVpB > 0 && aVpB.GetArraySize() > i)
            for (int k = 0; k < 3 && i - k >= 0; k++)
            {
                w.VpB[k] = (double)aVpB[i - k];
                w.vpbOk[k] = 1;
            }
        if (idVA > 0)
        {
            if (aVAH.GetArraySize() > i)
                for (int k = 0; k < 2 && i - k >= 0; k++)
                {
                    w.VAH[k] = (double)aVAH[i - k];
                    w.vahOk[k] = 1;
                }
            if (aVAL.GetArraySize() > i)
                for (int k = 0; k < 2 && i - k >= 0; k++)
                {
                    w.VAL[k] = (double)aVAL[i - k];
                    w.valOk[k] = 1;
                }
        }
        if (idVp6 > 0 && aVp6.GetArraySize() > i)
            for (int k = 0; k < 2 && i - k >= 0; k++)
            {
                w.Vp6[k] = (double)aVp6[i - k];
                w.vp6Ok[k] = 1;
            }
        if (idAskD > 0 && aAskD.GetArraySize() > i)
            for (int k = 0; k < 2 && i - k >= 0; k++)
            {
                w.AskD[k] = (double)aAskD[i - k];
                w.askOk[k] = 1;
            }
        if (idBidD > 0 && aBidD.GetArraySize() > i)
            for (int k = 0; k < 2 && i - k >= 0; k++)
            {
                w.BidD[k] = (double)aBidD[i - k];
                w.bidOk[k] = 1;
            }
        if (idVp8 > 0 && aVp8.GetArraySize() > i)
            for (int k = 0; k < 2 && i - k >= 0; k++)
            {
                w.Vp8[k] = (double)aVp8[i - k];
                w.vp8Ok[k] = 1;
            }

        // Family 5 needs side-specific VAP rows: evaluate Bull side with
        // AVAP rows and Bear side with BVAP rows, keeping each side's SGs.
        Tmv2Window wBull = w;
        Tmv2Window wBear = w;
        wBull.vapMultIs1 = vapMultIs1;
        wBear.vapMultIs1 = vapMultIs1;
        if (role == TMV2_ROLE_RANGE)
        {
            Tmv2_AcsilFillVap(sc, i, 1, vapMultIs1, &wBull);
            Tmv2_AcsilFillVap(sc, i, 0, vapMultIs1, &wBear);
        }
        int firedB[32], firedR[32];
        Tmv2_EvalRoleAt(role, &wBull, i, firedB);
        Tmv2_EvalRoleAt(role, &wBear, i, firedR);
        int fired[32];
        for (int s = 0; s < 32; s++)
            fired[s] = (s % 2 == 0) ? firedB[s] : firedR[s];

        int bullLane[32], bearLane[32];
        Tmv2_StackLanes(fired, hidden, bullLane, bearLane);
        const float lo = sc.Low[i];
        const float hi = sc.High[i];
        for (int s = 0; s < 32; s++)
        {
            if (!fired[s]) continue;
            const int isBull = (s % 2 == 0) ? 1 : 0;
            int lane = isBull ? bullLane[s] : bearLane[s];
            if (lane < 0) lane = 0; // hidden: base-position truth, no lane
            double y = Tmv2_StackY(isBull, lane, (double)lo, (double)hi,
                                   isBull ? bullBase : bearBase,
                                   stepTicks, (double)tickF);
            sc.Subgraph[s][i] = (float)y;
        }
    }

    for (int s = 0; s < 32; s++) sc.Subgraph[s][sc.ArraySize - 1] = 0.0f;
}

#endif // TMV2_UNITTEST
