#include "sierrachart.h"
SCDLLName("TriggerMatrixV2")

// =============================================================================
// TriggerMatrixV2.cpp
// Sierra Chart ACSIL Custom Study — Trigger Matrix V2 Self-Contained v2.4
//
// Self-contained study: computes ALL role-relevant data internally from the
// current chart native OHLC, bid/ask volume (SC_ASKVOL/SC_BIDVOL) and Volume
// at Price (sc.VolumeAtPriceForBars). No GetStudyArrayFromChartUsingID, no
// external-study selection. Evaluates the 16 ready primary V2 detector
// families from the pinned research catalog and publishes 32 stable
// primary Bull/Bear output subgraphs (SG0..SG31) plus HTML v2 withContext
// Long Trigger / T BUY / R BUY (SG32..SG37). One instance per source-chart
// role (Range / Renko 6t / Renko 8t); outputs of non-selected roles remain
// exactly zero.
//
// Canonical catalog: v2-formula-catalog.json
//   version 2.0.0-research-2026-09-09
//   SHA-256 ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618
// Display names match TriggerMatrix V1 / HTML sourceName. Predicates are
// HTML v2 formulas, not V1. withContext arms for ordinals 16/17/18 publish
// as Long Trigger / T BUY / R BUY on SG32..37 (cores stay byte-identical).
// Family-9 NYSE TICK overlay and blocked families (ordinals 3, 12, 13) are
// NOT implemented. Legacy TriggerMatrix.cpp predicates are unused here.
//
// INTERNAL PRODUCERS (v2.0, fixed canonical math, no inputs):
//   Bar delta  = native AskVol - BidVol per bar, valid only when classified
//                volume is consistent with chart total volume (SC_VOLUME):
//                positive chart volume with zero AV+BV reads as missing
//                data, never as a valid zero delta.
//   Bands      = 20-period SMA +/- 0.9 * population SD over bar deltas;
//                a band value is valid only with a complete 20-bar window
//                of valid deltas (fail closed until ready).
//   Per-bar VPOC from the bar's own VAP rows: max total-volume row; ties
//     resolve to the row closest to the profile price midpoint
//     ((minTick+maxTick)/2), equidistant ties take the lower row. This is
//     the documented Sierra Volume-POC rule; Range VPOC A and Range VPOC B
//     are the SAME documented no-input per-bar producer, so one internal
//     calculation serves both (VpA == VpB by construction).
//   Per-bar 68% value area from the same VAP rows: start at POC, expand one
//     stored row at a time taking the greater-volume side, including BOTH
//     rows on equal volumes (documented Sierra Volume-VA rule; expansion
//     steps over consecutive stored rows, so sparse profiles pair across
//     tick gaps — same consecutive-index iteration the installed Sierra
//     diagonal-ratio reference uses). VVAH/VVAL are the included-edge row
//     prices.
//   Renko diagonal counts: per consecutive stored VAP-row pair (lower-row
//     bid vs next-higher-row ask, gaps included per the installed Sierra
//     reference) the SIGNED ROUNDED diagonal ratio (sc.Round half away
//     from zero, so 299.5 -> 300 counts): ask-dominant ratio =
//     round(askUp/bidLo*100), count ask when > 0 and >= +300%,
//     bid-dominant = round(bidLo/askUp*-100), count bid when < 0 and
//     <= -300% (meets-or-exceeds on the rounded value).
//     Zero denominators are skipped (Sierra zero-compares disabled, the
//     catalog ord14 pin), both rows need total volume >= 20 (catalog ord14
//     pin), missing rows skip the pair (never a valid zero).
//   Family-5 VAP rows: exact-tick VAP(L+k*t)/VAP(H-k*t) lookups, gated on
//     actual sc.VolumeAtPriceMultiplier == 1.
//   Completeness: every profile use reconciles stored-row volume against
//     the bar's classified AV+BV — shortfalls mean partially published VAP
//     (late data) and yield no POC/VA/count signals. No price-range
//     coverage assumption: Renko OHLC endpoints are synthetic, not traded
//     ticks.
//
// SUBGRAPHS (even=Bull below low, odd=Bear above high). Names = V1 / HTML
// sourceName. Formulas = HTML v2 (not TriggerMatrix.cpp).
//   0/1  Fading MOMO Below/Above (TEXT)                ord 1  Range
//   2/3  Delta Rise / Delta Drop (ARROW)               ord 2  Range
//   4/5  EXH+ / EXH- (TEXT)                            ord 4  Range
//   6/7  VOL SEQ Bull/Bear (SQUARE)                    ord 5  Range
//   8/9  VA Long / VA Short (TEXT)                     ord 6  Range
//   10/11 Slingshot Buy/Sell (TEXT)                    ord 7  Range
//   12/13 POC Delta Bull/Bear (TRIANGLE)               ord 8  Range
//   14/15 MPOC+ / MPOC- (TEXT)                         ord 9  Range
//   16/17 Delta Trap Bull/Bear (TEXT)                  ord 10 Range
//   18/19 POCL Long / POCS Short (SQUARE)              ord 11 Range
//   20/21 OF Long / OF Short (TRIANGLE)                ord 14 Renko 6t
//   22/23 FA+ / FA- (TRIANGLE)                         ord 15 Renko 6t
//   24/25 Long/Short Trigger Core (TEXT)               ord 16 Renko 8t
//   26/27 T BUY/T SELL Core (TEXT)                     ord 17 Renko 8t
//   28/29 R BUY/R SELL Core (TEXT)                     ord 18 Renko 8t
//   30/31 POC Wave Bull/Bear (TEXT)                    ord 19 Range
//   32/33 Long Trigger / Short Trigger (HTML withContext) ord 20 Renko 8t
//   34/35 T BUY / T SELL (HTML withContext)            ord 21 Renko 8t
//   36/37 R BUY / R SELL (HTML withContext)            ord 22 Renko 8t
//   38 Long Level 1 / 39 Long Level 2 (ARROW, V1 BLOCK Z)
//   40 Short Level 1 / 41 Short Level 2
//   50..56 hidden indicator workspace (DRAWSTYLE_IGNORE)
//
// Producer index convention (catalog pin only; no external studies):
// catalog Spreadsheet notation ID{...}.SGn is ONE-based; ACSIL
// SubgraphIndex is ZERO-based (index = n - 1): SG1->0, SG2->1, SG3->2,
// SG4->3, SG59->58. TMV2_SG_* constants keep those pins for tests.
// INPUTS (v2.1, V1-style: no study-ID pickers. REMOVE AND RE-ADD the
//   study — Sierra saved settings are slot-indexed and the 11 retired
//   source-ID slots plus opt-in/revision are gone):
//   In:0  Chart Role (Range=0, Renko 6t=1, Renko 8t=2)
//   In:1  Bull Base Offset (ticks, display only)
//   In:2  Bear Base Offset (ticks, display only)
//   In:3  Stack Step (ticks, display only)
//   Family 5 (VGD) enables when sc.VolumeAtPriceMultiplier == 1.
//
// PERSISTENT SLOTS:
//   Int 1   : late-VAP retry mark (-1 = none).
//   Int 4,5 : 64-bit structural fingerprint (lo, hi).
//
// STATUS (honest): native_sierra_compile=false, sierra_runtime_parity=false
// until user-side Sierra F5 compile and on-chart parity pass.
// Cross-compile against real Sierra headers is build verification only.
// =============================================================================

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// Portable core: pure helpers shared by the ACSIL body and the Linux tests.
// No Sierra headers, no STL containers, no static mutable state.
// Indexing convention: offset 0 = current completed bar, k = k bars prior.
// ---------------------------------------------------------------------------

#define TMV2_VERSION "2.4-self-contained"
#define TMV2_CATALOG_VERSION "2.0.0-research-2026-09-09"
#define TMV2_CATALOG_SHA "ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618"
#define TMV2_SCHEMA_VERSION 6

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
#define TMV2_N_PRIMARY 32
#define TMV2_N_OUT 38
#define TMV2_SG_LONG_LV1 38
#define TMV2_SG_LONG_LV2 39
#define TMV2_SG_SHORT_LV1 40
#define TMV2_SG_SHORT_LV2 41
#define TMV2_ORD_LTR 20
#define TMV2_ORD_TBY 21
#define TMV2_ORD_RBY 22

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
    // HTML v2 withContext indicators (Renko 8t). Offset 0 = current, 1 = prior.
    double Macd[2]; int macdOk[2];   // MACD line (SG1); zero-line compare is vs 0
    double Ema50[2]; double Ema200[2]; int emaOk[2];
    double Adx[2]; int adxOk[2];
    double Smi[2]; int smiOk[2];
    double StochK[2]; double StochD[2]; int stochOk[2];
    double Rsi[2]; int rsiOk[2];
    double BbUp[2]; double BbLo[2]; int bbOk[2];
};

static void Tmv2_ClearWindow(Tmv2Window* w)
{
    std::memset(w, 0, sizeof(*w));
}

static const char* Tmv2_CatalogVersion() { return TMV2_CATALOG_VERSION; }
static const char* Tmv2_CatalogSha() { return TMV2_CATALOG_SHA; }
static int Tmv2_ReadyFamilyCount() { return 16; }
static int Tmv2_OutputCount() { return TMV2_N_OUT; }

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
    case 20: case 21: case 22:
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
    case 20: return 32; case 21: return 34; case 22: return 36;
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
    case 20: return "LTR";
    case 21: return "TBY";
    case 22: return "RBY";
    default: return "?";
    }
}

// Self-contained structural disabled set: no external source studies.
// The only structural gate is family-5 (VGD) requiring actual chart
// VolumeAtPriceMultiplier == 1; when off, ordinal 5 is disabled under
// Range. Returns count and fills disabled[] in ascending catalog order.
static int Tmv2_SelfDisabled(int role, int vapGate, int disabled[16])
{
    if (disabled == 0) return 0;
    int n = 0;
    if (role == TMV2_ROLE_RANGE && !vapGate) disabled[n++] = 5;
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
    case 20: return 3;
    case 21: return 6;
    case 22: return 3;
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

// ---------------------------------------------------------------------------
// Self-contained internal producers (v2.0): delta bands, per-bar volume
// profile VPOC / 68% value area, diagonal ratio counts, exact-tick VAP rows.
// Pure helpers over plain arrays; the ACSIL body copies native chart data
// (OHLC, SC_ASKVOL/SC_BIDVOL, VolumeAtPrice rows) into these shapes once
// per evaluated bar. No Sierra headers, no heap, no static mutable state.
// Fixed canonical math (no inputs): bands 20-period SMA +/- 0.9 population
// SD; VPOC max-volume with Sierra midpoint/lower tie rule; VA 68% with
// Sierra greater-side/equal-includes-both expansion; diagonals +/-300%
// signed ratio with zero-denominator skip and 20-contract both-row minimum.
// ---------------------------------------------------------------------------

#define TMV2_BAND_LEN 20
#define TMV2_BAND_MULT 0.9
#define TMV2_VA_PCT 0.68
#define TMV2_DIAG_PCT 300.0
#define TMV2_DIAG_MIN_TOTAL 20.0
#define TMV2_MAX_VAP_ROWS 2048

struct Tmv2_VapRow
{
    int tick;      // PriceInTicks
    double vol;    // total volume at the row
    double ask;    // ask volume at the row
    double bid;    // bid volume at the row
};

static int Tmv2_TickSizeOk(double ts)
{
    return (ts > 0.0 && Tmv2_IsFinite(ts)) ? 1 : 0;
}

// Guarded price-to-tick: rejects bad tick size, non-finite prices, and
// quotients outside int range with margin for the family-5 anchor steps
// (targetTick +/- 4 must not overflow).
static int Tmv2_PriceToTickSafe(double price, double tickSize, int* outTick)
{
    if (outTick == 0) return 0;
    if (!Tmv2_TickSizeOk(tickSize) || !Tmv2_IsFinite(price)) return 0;
    double q = price / tickSize;
    if (!Tmv2_IsFinite(q)) return 0;
    double r = floor(q + 0.5);
    const double kLim = 2147483640.0; // INT_MAX - 7, margin for +/-4 anchors
    if (r > kLim || r < -kLim) return 0;
    *outTick = (int)r;
    return 1;
}

// sc.Round-compatible rounding (half away from zero): positive fractions
// >= 0.5 round up, negative fractions <= -0.5 round down (more negative).
// Verified against sierrachart.h sc.Round (truncation + half-away adjust).
static double Tmv2_RoundHalfAway(double x)
{
    if (!Tmv2_IsFinite(x)) return x;
    return (x >= 0.0) ? floor(x + 0.5) : ceil(x - 0.5);
}

// 20-period SMA +/- mult * population SD over d[end-19..end]. All 20 ok
// flags must be set and every sample finite; otherwise fail closed (0).
// Pure: production passes a small stack history copied from native deltas.
static int Tmv2_BandAt(const double* d, const int* ok, int n, int end,
                       double mult, double* up, double* lo)
{
    if (d == 0 || ok == 0 || up == 0 || lo == 0) return 0;
    if (n < TMV2_BAND_LEN || end < TMV2_BAND_LEN - 1 || end >= n) return 0;
    if (!(mult >= 0.0) || !Tmv2_IsFinite(mult)) return 0;
    double sum = 0.0;
    for (int t = end - TMV2_BAND_LEN + 1; t <= end; t++)
    {
        if (!ok[t]) return 0;
        if (!Tmv2_IsFinite(d[t])) return 0;
        sum += d[t];
    }
    double mean = sum / (double)TMV2_BAND_LEN;
    double var = 0.0;
    for (int t = end - TMV2_BAND_LEN + 1; t <= end; t++)
    {
        double dev = d[t] - mean;
        var += dev * dev;
    }
    var /= (double)TMV2_BAND_LEN; // population (catalog 20/0.9 SMA)
    double sd = sqrt(var);
    if (!Tmv2_IsFinite(sd)) return 0;
    *up = mean + mult * sd;
    *lo = mean - mult * sd;
    return 1;
}

// Profile shape gate: 1..cap rows, strictly ascending ticks, finite and
// non-negative volumes. Anything else is malformed -> invalid profile.
static int Tmv2_VapRowsValid(const Tmv2_VapRow* rows, int n)
{
    if (rows == 0 || n <= 0 || n > TMV2_MAX_VAP_ROWS) return 0;
    for (int i = 0; i < n; i++)
    {
        if (!Tmv2_IsFinite(rows[i].vol) ||
            !Tmv2_IsFinite(rows[i].ask) ||
            !Tmv2_IsFinite(rows[i].bid)) return 0;
        if (rows[i].vol < 0.0 || rows[i].ask < 0.0 || rows[i].bid < 0.0) return 0;
        if (i > 0 && rows[i].tick <= rows[i - 1].tick) return 0;
    }
    return 1;
}

// Per-bar VPOC: max total-volume row. Ties resolve to the row closest to
// the profile price midpoint ((minTick+maxTick)/2); equidistant ties take
// the lower row. Documented Sierra Volume-POC rule. All-zero profiles have
// no POC (invalid, never a valid zero).
static int Tmv2_ProfilePoc(const Tmv2_VapRow* rows, int n, int* pocIdx)
{
    if (pocIdx == 0 || !Tmv2_VapRowsValid(rows, n)) return 0;
    int best = 0;
    double mid = ((double)rows[0].tick + (double)rows[n - 1].tick) / 2.0;
    for (int i = 1; i < n; i++)
    {
        if (rows[i].vol > rows[best].vol) { best = i; continue; }
        if (rows[i].vol < rows[best].vol) continue;
        double di = fabs((double)rows[i].tick - mid);
        double db = fabs((double)rows[best].tick - mid);
        if (di < db || (di == db && rows[i].tick < rows[best].tick)) best = i;
    }
    if (!(rows[best].vol > 0.0)) return 0;
    *pocIdx = best;
    return 1;
}

// Per-bar value area: start at POC, expand one row at a time taking the
// greater-volume side; equal volumes include BOTH rows then continue one
// row out on each side. Stop once included volume reaches pct of profile
// total. Documented Sierra Volume-VA rule. pct is 0.68 (catalog VA68 pin).
static int Tmv2_ProfileVa(const Tmv2_VapRow* rows, int n, int pocIdx,
                          double pct, int* hiTick, int* loTick)
{
    if (hiTick == 0 || loTick == 0) return 0;
    if (!Tmv2_VapRowsValid(rows, n)) return 0;
    if (pocIdx < 0 || pocIdx >= n) return 0;
    if (!(pct > 0.0) || !(pct <= 1.0) || !Tmv2_IsFinite(pct)) return 0;
    if (!(rows[pocIdx].vol > 0.0)) return 0;
    double total = 0.0;
    for (int i = 0; i < n; i++) total += rows[i].vol;
    if (!(total > 0.0) || !Tmv2_IsFinite(total)) return 0;
    double target = total * pct;
    double incl = rows[pocIdx].vol;
    int hi = pocIdx, lo = pocIdx;
    int up = pocIdx + 1, dn = pocIdx - 1;
    while (incl < target)
    {
        int hasUp = (up < n) ? 1 : 0;
        int hasDn = (dn >= 0) ? 1 : 0;
        if (!hasUp && !hasDn) break;
        if (hasUp && hasDn)
        {
            if (rows[up].vol > rows[dn].vol) { incl += rows[up].vol; hi = up; up++; }
            else if (rows[dn].vol > rows[up].vol) { incl += rows[dn].vol; lo = dn; dn--; }
            else { incl += rows[up].vol + rows[dn].vol; hi = up; lo = dn; up++; dn--; }
        }
        else if (hasUp) { incl += rows[up].vol; hi = up; up++; }
        else { incl += rows[dn].vol; lo = dn; dn--; }
    }
    *hiTick = rows[hi].tick;
    *loTick = rows[lo].tick;
    return 1;
}

// Diagonal qualifying-level counts over consecutive stored-row pairs:
// lower-row bid vs next-higher-row ask. This matches the installed Sierra
// reference (studies8 diagonal-ratio branch iterates PriceIndex and
// PriceIndex+1 over stored VAP elements, gaps included). Ask-dominant
// (askUp >= bidLo): ratio = sc.Round(askUp/bidLo*100), counts when > 0 and
// >= +pctThr. Bid-dominant: ratio = sc.Round(bidLo/askUp*-100), counts when
// < 0 and <= -pctThr (Sierra compares the ROUNDED value, so 299.5 -> 300
// counts). Zero denominators are skipped (Sierra zero-compares disabled =
// catalog ord14 pin, hard-coded); both rows need total volume >= minTotal
// (catalog ord14 20-contract pin); other pairs skip silently.
// Missing/whole-invalid profile -> invalid (never valid 0).
static int Tmv2_DiagCounts(const Tmv2_VapRow* rows, int n, double pctThr,
                           double minTotal, int* askCount, int* bidCount)
{
    if (askCount == 0 || bidCount == 0) return 0;
    if (!Tmv2_VapRowsValid(rows, n)) return 0;
    if (!(pctThr > 0.0) || !Tmv2_IsFinite(pctThr)) return 0;
    if (!(minTotal >= 0.0) || !Tmv2_IsFinite(minTotal)) return 0;
    int ask = 0, bid = 0;
    for (int i = 0; i + 1 < n; i++)
    {
        double bidLo = rows[i].bid;
        double askUp = rows[i + 1].ask;
        if (!(rows[i].vol >= minTotal)) continue;
        if (!(rows[i + 1].vol >= minTotal)) continue;
        if (!(bidLo > 0.0) || !(askUp > 0.0)) continue;
        if (askUp >= bidLo)
        {
            double ratio = Tmv2_RoundHalfAway(askUp / bidLo * 100.0);
            if (Tmv2_IsFinite(ratio) && ratio > 0.0 && ratio >= pctThr) ask++;
        }
        else
        {
            double ratio = Tmv2_RoundHalfAway(bidLo / askUp * -100.0);
            if (Tmv2_IsFinite(ratio) && ratio < 0.0 && ratio <= -pctThr) bid++;
        }
    }
    *askCount = ask;
    *bidCount = bid;
    return 1;
}

// Exact-tick VAP row lookup for family 5: row at targetTick must exist with
// positive total; no nearest-row substitution. bullSide=1 reads ask,
// bullSide=0 reads bid.
static int Tmv2_VapSideRow(const Tmv2_VapRow* rows, int n, int targetTick,
                           double* tot, double* side, int bullSide)
{
    if (tot == 0 || side == 0) return 0;
    if (!Tmv2_VapRowsValid(rows, n)) return 0;
    for (int i = 0; i < n; i++)
    {
        if (rows[i].tick == targetTick)
        {
            if (!(rows[i].vol > 0.0)) return 0;
            *tot = rows[i].vol;
            *side = bullSide ? rows[i].ask : rows[i].bid;
            if (!Tmv2_IsFinite(*side) || *side < 0.0) return 0;
            return 1;
        }
    }
    return 0;
}

// VAP-vs-chart volume reconciliation: the stored rows must cover the bar's
// classified bid/ask volume. A shortfall means partially published VAP
// (late data) and the profile must not yield POC/VA/count signals. Excess
// stored volume (unclassified trades) is allowed. A zero chart total with
// stored volume (or vice versa) is inconsistent -> invalid. Precondition:
// rows are shape-valid (Tmv2_VapRowsValid); only the totals reconcile here.
// Note: no price-range coverage check — Renko OHLC endpoints are synthetic
// and must not be assumed to be traded ticks.
static int Tmv2_ProfileVolumeOk(const Tmv2_VapRow* rows, int n,
                                double av, double bv)
{
    if (rows == 0 || n <= 0) return 0;
    if (!Tmv2_FlowValid(av, bv)) return 0;
    double chartTotal = av + bv;
    if (!(chartTotal > 0.0)) return 0;
    double vapSum = 0.0;
    for (int i = 0; i < n; i++) vapSum += rows[i].vol;
    if (!Tmv2_IsFinite(vapSum)) return 0;
    if (vapSum < chartTotal - 0.001) return 0; // partial publication
    return 1;
}

// Native delta availability: flow must be valid AND classified volume must
// be present when the chart reports traded total volume. A positive chart
// total (SC_VOLUME) with zero AV+BV means missing bid/ask history, which
// must not read as a valid zero delta. Unknown totals (no volume array)
// fall back to flow validity.
static int Tmv2_DeltaOk(double av, double bv, double totalVol, int hasVol)
{
    if (!Tmv2_FlowValid(av, bv)) return 0;
    if (hasVol)
    {
        if (!Tmv2_IsFinite(totalVol) || totalVol < 0.0) return 1;
        if (totalVol > 0.0 && (av + bv) <= 0.0) return 0;
    }
    return 1;
}

// Depth of native history consumed per evaluated bar (covers the 20-bar
// band window plus the 5-bar formula footprint with margin).
#define TMV2_NATIVE_DEPTH 31
// Late-VAP retry bound: unresolved profiles older than this many bars
// behind the edge are forgotten (genuine history corrections arrive via
// rewound UpdateStartIndex and are honored fully).
#define TMV2_RETRY_LOOKBACK 128

// Update planner (production-called): derives [first, lastClosed] from the
// chart size, Sierra's UpdateStartIndex, rebuild flags, and the stored
// late-VAP retry mark. Returns 1 when at least the edge bar needs work.
// Tiny charts (ArraySize < 2) report no work; the caller still zeroes the
// forming bar.
static int Tmv2_PlanUpdate(int arraySize, int updateStartIndex,
                           int isFullRecalc, int fingerprintChanged,
                           int storedRetry, int* first, int* lastClosed)
{
    if (first == 0 || lastClosed == 0) return 0;
    int lc = arraySize - 2;
    *lastClosed = lc;
    *first = 0;
    if (lc < 0) return 0;
    int f = (isFullRecalc || updateStartIndex <= 0 || fingerprintChanged)
        ? 0 : updateStartIndex - 32;
    if (f < 0) f = 0;
    if (storedRetry >= 0 && storedRetry <= lc && storedRetry < f) f = storedRetry;
    if (f > lc) f = lc;
    *first = f;
    return 1;
}

// Native window fill (production-called): OHLC/flow/delta plus internal
// 20/0.9 bands from plain histories. Arrays hold n entries with the
// current bar at index end; TV/hasVol carry chart total volume (SC_VOLUME)
// or NULL/0 when unavailable. Caller clears the window first; profile
// fields are left for Tmv2_FillProfileOffset.
static void Tmv2_FillNative(Tmv2Window* w,
                            const double* O, const double* H,
                            const double* L, const double* C,
                            const double* AV, const double* BV,
                            const double* TV, int hasVol,
                            int n, int end, double tick)
{
    Tmv2_ClearWindow(w);
    w->tick = tick;
    if (O == 0 || H == 0 || L == 0 || C == 0 || AV == 0 || BV == 0) return;
    if (n <= 0 || end < 0 || end >= n) return;
    for (int k = 0; k < 6; k++)
    {
        int j = end - k;
        if (j < 0) break;
        w->O[k] = O[j]; w->H[k] = H[j]; w->L[k] = L[j]; w->C[k] = C[j];
        w->AV[k] = AV[j]; w->BV[k] = BV[j];
        w->D[k] = AV[j] - BV[j];
        w->dOk[k] = Tmv2_DeltaOk(AV[j], BV[j], (hasVol && TV != 0) ? TV[j] : 0.0, hasVol);
    }
    double dh[25];
    int okh[25];
    for (int t = 0; t < 25; t++)
    {
        int j = end - 24 + t;
        if (j < 0 || j >= n)
        {
            okh[t] = 0; dh[t] = 0.0;
        }
        else if (!Tmv2_DeltaOk(AV[j], BV[j], (hasVol && TV != 0) ? TV[j] : 0.0, hasVol))
        {
            okh[t] = 0; dh[t] = 0.0;
        }
        else
        {
            okh[t] = 1; dh[t] = AV[j] - BV[j];
        }
    }
    for (int k = 0; k < 6; k++)
    {
        if (Tmv2_BandAt(dh, okh, 25, 24 - k, TMV2_BAND_MULT, &w->Up[k], &w->Lo[k]))
        {
            w->upOk[k] = 1;
            w->loOk[k] = 1;
        }
    }
}

// Profile offset fill (production-called): reconciles, then computes POC,
// 68% VA and diagonal counts for lag offset k (0..4) into the window.
// Range VPOC A and B share the one calculation. Clears the offset on any
// failure. Returns 1 when the offset profile resolved (retry tracking).
static int Tmv2_FillProfileOffset(Tmv2Window* w, const Tmv2_VapRow* rows,
                                 int n, double av, double bv,
                                 int k, double tickSize)
{
    if (w == 0 || k < 0 || k > 4) return 0;
    w->VpA[k] = 0.0; w->vpaOk[k] = 0;
    if (k < 3) { w->VpB[k] = 0.0; w->vpbOk[k] = 0; }
    if (k < 2)
    {
        w->VAH[k] = 0.0; w->vahOk[k] = 0;
        w->VAL[k] = 0.0; w->valOk[k] = 0;
        w->Vp6[k] = 0.0; w->vp6Ok[k] = 0;
        w->Vp8[k] = 0.0; w->vp8Ok[k] = 0;
        w->AskD[k] = 0.0; w->askOk[k] = 0;
        w->BidD[k] = 0.0; w->bidOk[k] = 0;
    }
    if (!Tmv2_TickSizeOk(tickSize)) return 0;
    if (!Tmv2_VapRowsValid(rows, n)) return 0;
    if (!Tmv2_ProfileVolumeOk(rows, n, av, bv)) return 0;
    int pi = -1;
    if (!Tmv2_ProfilePoc(rows, n, &pi)) return 0;
    // Row price = tick * tick size (== sc.TicksToPriceValue semantics).
    double pocP = (double)rows[pi].tick * tickSize;
    w->VpA[k] = pocP; w->vpaOk[k] = 1;
    if (k < 3) { w->VpB[k] = pocP; w->vpbOk[k] = 1; }
    if (k < 2)
    {
        int hiT = 0, loT = 0;
        if (Tmv2_ProfileVa(rows, n, pi, TMV2_VA_PCT, &hiT, &loT))
        {
            w->VAH[k] = (double)hiT * tickSize; w->vahOk[k] = 1;
            w->VAL[k] = (double)loT * tickSize; w->valOk[k] = 1;
        }
        w->Vp6[k] = pocP; w->vp6Ok[k] = 1;
        w->Vp8[k] = pocP; w->vp8Ok[k] = 1;
        int aq = 0, bq = 0;
        if (Tmv2_DiagCounts(rows, n, TMV2_DIAG_PCT, TMV2_DIAG_MIN_TOTAL, &aq, &bq))
        {
            w->AskD[k] = (double)aq; w->askOk[k] = 1;
            w->BidD[k] = (double)bq; w->bidOk[k] = 1;
        }
    }
    return 1;
}

// Family-5 side fill (production-called): exact-tick AVAP rows at
// lowTick+k (bull) and BVAP rows at highTick-k (bear), all-or-nothing per
// side after volume reconciliation. vapGate=0 leaves both sides cleared.
// Returns 1 when both sides resolved.
static int Tmv2_FillVapSides(Tmv2Window* wBull, Tmv2Window* wBear,
                             const Tmv2_VapRow* rows, int n,
                             double av, double bv, int vapGate,
                             int lowTick, int highTick)
{
    if (wBull == 0 || wBear == 0) return 0;
    if (!vapGate) return 0;
    if (!Tmv2_VapRowsValid(rows, n)) return 0;
    if (!Tmv2_ProfileVolumeOk(rows, n, av, bv)) return 0;
    int okB = 1, okR = 1;
    double totB[5], sdB[5], totR[5], sdR[5];
    for (int k = 0; k < 5; k++)
    {
        double tot = 0.0, sd = 0.0;
        if (!Tmv2_VapSideRow(rows, n, lowTick + k, &tot, &sd, 1)) okB = 0;
        else { totB[k] = tot; sdB[k] = sd; }
        if (!Tmv2_VapSideRow(rows, n, highTick - k, &tot, &sd, 0)) okR = 0;
        else { totR[k] = tot; sdR[k] = sd; }
    }
    if (okB) for (int k = 0; k < 5; k++)
    {
        wBull->vapTot[k] = totB[k]; wBull->vapSide[k] = sdB[k]; wBull->vapOk[k] = 1;
    }
    if (okR) for (int k = 0; k < 5; k++)
    {
        wBear->vapTot[k] = totR[k]; wBear->vapSide[k] = sdR[k]; wBear->vapOk[k] = 1;
    }
    return (okB && okR) ? 1 : 0;
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

// HTML v2 withContext arms. Primary cores stay SG24..29; these publish
// V1-named Long Trigger / T BUY / R BUY on SG32..37.
static int Tmv2_IndPairOk(const int* ok)
{
    return (ok[0] && ok[1]) ? 1 : 0;
}

static int Tmv2_F16BullCtx(const Tmv2Window* w)
{
    if (!Tmv2_F16Bull(w)) return 0;
    if (!w->macdOk[0] || !w->emaOk[0] || !w->adxOk[0] || !w->smiOk[0]) return 0;
    if (!(w->Macd[0] < 0.0)) return 0;
    if (!(w->Ema50[0] > w->Ema200[0])) return 0;
    if (!(w->Adx[0] > 20.0)) return 0;
    if (!(w->Smi[0] < -60.0)) return 0;
    return 1;
}

static int Tmv2_F16BearCtx(const Tmv2Window* w)
{
    if (!Tmv2_F16Bear(w)) return 0;
    if (!w->macdOk[0] || !w->emaOk[0] || !w->adxOk[0] || !w->smiOk[0]) return 0;
    if (!(w->Macd[0] > 0.0)) return 0;
    if (!(w->Ema50[0] < w->Ema200[0])) return 0;
    if (!(w->Adx[0] > 20.0)) return 0;
    if (!(w->Smi[0] > 60.0)) return 0;
    return 1;
}

static int Tmv2_F17BullCtx(const Tmv2Window* w)
{
    if (!Tmv2_F17Bull(w)) return 0;
    if (!w->adxOk[0] || !w->smiOk[0] || !Tmv2_IndPairOk(w->stochOk)) return 0;
    if (!(w->Adx[0] > 25.0)) return 0;
    if (!(w->StochK[0] < 20.0)) return 0;
    if (!(w->StochK[1] <= w->StochD[1])) return 0;
    if (!(w->StochK[0] > w->StochD[0])) return 0;
    if (!(w->Smi[0] < -60.0)) return 0;
    return 1;
}

static int Tmv2_F17BearCtx(const Tmv2Window* w)
{
    if (!Tmv2_F17Bear(w)) return 0;
    if (!w->adxOk[0] || !w->smiOk[0] || !Tmv2_IndPairOk(w->stochOk)) return 0;
    if (!(w->Adx[0] > 25.0)) return 0;
    if (!(w->StochK[0] > 80.0)) return 0;
    if (!(w->StochK[1] >= w->StochD[1])) return 0;
    if (!(w->StochK[0] < w->StochD[0])) return 0;
    if (!(w->Smi[0] > 60.0)) return 0;
    return 1;
}

static int Tmv2_F18BullCtx(const Tmv2Window* w)
{
    if (!Tmv2_F18Bull(w)) return 0;
    if (!w->adxOk[0] || !w->rsiOk[0] || !w->smiOk[0] || !w->bbOk[0]) return 0;
    if (!(w->Adx[0] < 20.0)) return 0;
    if (!(w->Rsi[0] < 30.0)) return 0;
    if (!(w->C[0] <= w->BbLo[0])) return 0;
    if (!(w->Smi[0] < -60.0)) return 0;
    return 1;
}

static int Tmv2_F18BearCtx(const Tmv2Window* w)
{
    if (!Tmv2_F18Bear(w)) return 0;
    if (!w->adxOk[0] || !w->rsiOk[0] || !w->smiOk[0] || !w->bbOk[0]) return 0;
    if (!(w->Adx[0] < 20.0)) return 0;
    if (!(w->Rsi[0] > 70.0)) return 0;
    if (!(w->C[0] >= w->BbUp[0])) return 0;
    if (!(w->Smi[0] > 60.0)) return 0;
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
// Role dispatch: evaluate only families belonging to `role` into fired[TMV2_N_OUT]
// indexed by output SG (even=Bull, odd=Bear). Non-role outputs stay zero.
// Returns count of fired outputs.
// ---------------------------------------------------------------------------
static int Tmv2_EvalRole(int role, const Tmv2Window* w, int fired[TMV2_N_OUT])
{
    for (int s = 0; s < TMV2_N_OUT; s++) fired[s] = 0;
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
    TMV2_TRY(20, Tmv2_F16BullCtx, Tmv2_F16BearCtx);
    TMV2_TRY(21, Tmv2_F17BullCtx, Tmv2_F17BearCtx);
    TMV2_TRY(22, Tmv2_F18BullCtx, Tmv2_F18BearCtx);
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
    case 20: return 200;
    case 21: return 40;
    case 22: return 40;
    default: return -1;
    }
}

// Clear fired outputs whose family warm-up is not satisfied at barIndex.
static void Tmv2_ApplyWarmup(int role, int barIndex, int fired[TMV2_N_OUT])
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
    const int ctx[3] = {20, 21, 22};
    for (int i = 0; i < 3; i++)
    {
        int ord = ctx[i];
        if (Tmv2_FamilyRole(ord) != role) continue;
        if (barIndex < Tmv2_MinBarIndex(ord))
        {
            int sb = Tmv2_FamilySgBull(ord);
            if (sb >= 0) { fired[sb] = 0; fired[sb + 1] = 0; }
        }
    }
}

static int Tmv2_EvalRoleAt(int role, const Tmv2Window* w, int barIndex, int fired[TMV2_N_OUT])
{
    int n = Tmv2_EvalRole(role, w, fired);
    Tmv2_ApplyWarmup(role, barIndex, fired);
    n = 0;
    for (int s = 0; s < TMV2_N_OUT; s++) n += fired[s] ? 1 : 0;
    return n;
}

static void Tmv2_StackLanes(const int fired[TMV2_N_OUT], const int hidden[TMV2_N_OUT],
                            int bullLane[TMV2_N_OUT], int bearLane[TMV2_N_OUT])
{
    for (int s = 0; s < TMV2_N_OUT; s++) { bullLane[s] = -1; bearLane[s] = -1; }
    int bl = 0;
    for (int s = 0; s < TMV2_N_OUT; s += 2)
        if (fired[s] && !hidden[s]) bullLane[s] = bl++;
    int rl = 0;
    for (int s = 1; s < TMV2_N_OUT; s += 2)
        if (fired[s] && !hidden[s]) bearLane[s] = rl++;
}

static double Tmv2_StackY(int isBull, int lane, double low, double high,
                          int baseTicks, int stepTicks, double tick)
{
    if (isBull) return low - (baseTicks + lane * stepTicks) * tick;
    return high + (baseTicks + lane * stepTicks) * tick;
}

// V1 BLOCK Z: count published detectors this bar. Skip Core SGs 24-29 so
// Long Trigger / T BUY / R BUY withContext is not double-counted.
static int Tmv2_LevelCountBar(const int fired[TMV2_N_OUT], int isBull)
{
    int n = 0;
    for (int s = isBull ? 0 : 1; s < TMV2_N_OUT; s += 2)
    {
        if (s >= 24 && s <= 29) continue;
        if (fired[s]) n++;
    }
    return n;
}

// V1 window 0 skipped the loop (never fired). Treat 0 as current bar.
static int Tmv2_LevelWindowSum(const int* perBar, int i, int window)
{
    int w = window;
    if (w < 1) w = 1;
    int sum = 0;
    for (int b = i; b >= i - w + 1 && b >= 0; b--)
        sum += perBar[b];
    return sum;
}

// Highest threshold wins (V1: lv2 else lv1).
static int Tmv2_LevelTier(int count, int lv1, int lv2)
{
    if (count >= lv2) return 2;
    if (count >= lv1) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// 64-bit FNV-1a structural fingerprint over schema, catalog pin, role,
// VAP gate (actual multiplier == 1), ACTUAL VolumeAtPriceMultiplier,
// display geometry, and tick bits. A multiplier change rebuilds history.
// No source-study IDs.
// ---------------------------------------------------------------------------
static void Tmv2_FnvMix(unsigned long long* h, unsigned long long v)
{
    for (int b = 0; b < 8; b++)
    {
        *h ^= (unsigned long long)((v >> (b * 8)) & 0xFFu);
        *h *= 1099511628211ULL;
    }
}

static unsigned long long Tmv2_Fingerprint(int role,
                                           int vapFlag, unsigned int vapMult,
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
    Tmv2_FnvMix(&h, (unsigned long long)vapFlag);
    Tmv2_FnvMix(&h, (unsigned long long)vapMult);
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

// Producer subgraph indices are defined once in the portable seam above
// (zero-based ACSIL SubgraphIndex = catalog one-based SGn minus 1).
// Copy one bar's VAP rows into the caller buffer in container order
// (ascending PriceInTicks). Returns 1 with *nOut rows on success; 0 leaves
// *nOut 0 (missing container, empty bar, or more rows than cap -> the bar's
// profile outputs stay invalid, never a valid zero).
static int Tmv2_AcsilCopyVap(SCStudyInterfaceRef sc, int barIndex,
                             Tmv2_VapRow* rows, int cap, int* nOut)
{
    *nOut = 0;
    if (rows == 0 || cap <= 0 || nOut == 0) return 0;
    if (barIndex < 0) return 0;
    if (sc.VolumeAtPriceForBars == NULL) return 0;
    unsigned int levels =
        sc.VolumeAtPriceForBars->GetSizeAtBarIndex((unsigned int)barIndex);
    if (levels == 0 || levels > (unsigned int)cap) return 0;
    for (unsigned int k = 0; k < levels; k++)
    {
        const s_VolumeAtPriceV2* e = NULL;
        if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex((unsigned int)barIndex,
                                                           (int)k, &e)) return 0;
        if (e == NULL) return 0;
        rows[k].tick = e->PriceInTicks;
        rows[k].vol = (double)e->Volume;
        rows[k].ask = (double)e->AskVolume;
        rows[k].bid = (double)e->BidVolume;
    }
    *nOut = (int)levels;
    return 1;
}

static void Tmv2_EmaPass(SCFloatArrayRef in, SCFloatArrayRef out, int i, int period)
{
    float alpha = 2.0f / (period + 1.0f);
    out[i] = (i == 0) ? in[i] : alpha * in[i] + (1.0f - alpha) * out[i - 1];
}

static void Tmv2_LoadCtxFromSg(Tmv2Window* ww, int bar, int off,
    SCFloatArrayRef macd, SCFloatArrayRef ema50, SCFloatArrayRef ema200,
    SCFloatArrayRef adx, SCFloatArrayRef smi,
    SCFloatArrayRef stochK, SCFloatArrayRef stochD,
    SCFloatArrayRef rsi, SCFloatArrayRef bbUp, SCFloatArrayRef bbLo)
{
    if (bar < 0) return;
    ww->Macd[off] = (double)macd[bar];
    ww->macdOk[off] = Tmv2_IsFinite(ww->Macd[off]) ? 1 : 0;
    ww->Ema50[off] = (double)ema50[bar];
    ww->Ema200[off] = (double)ema200[bar];
    ww->emaOk[off] = (Tmv2_IsFinite(ww->Ema50[off]) && Tmv2_IsFinite(ww->Ema200[off])) ? 1 : 0;
    ww->Adx[off] = (double)adx[bar];
    ww->adxOk[off] = Tmv2_IsFinite(ww->Adx[off]) ? 1 : 0;
    ww->Smi[off] = (double)smi[bar];
    ww->smiOk[off] = Tmv2_IsFinite(ww->Smi[off]) ? 1 : 0;
    ww->StochK[off] = (double)stochK[bar];
    ww->StochD[off] = (double)stochD[bar];
    ww->stochOk[off] = (Tmv2_IsFinite(ww->StochK[off]) && Tmv2_IsFinite(ww->StochD[off])) ? 1 : 0;
    ww->Rsi[off] = (double)rsi[bar];
    ww->rsiOk[off] = Tmv2_IsFinite(ww->Rsi[off]) ? 1 : 0;
    ww->BbUp[off] = (double)bbUp[bar];
    ww->BbLo[off] = (double)bbLo[bar];
    ww->bbOk[off] = (Tmv2_IsFinite(ww->BbUp[off]) && Tmv2_IsFinite(ww->BbLo[off])) ? 1 : 0;
}

SCSFExport scsf_TriggerMatrixV2(SCStudyInterfaceRef sc)
{
    SCInputRef In_Role     = sc.Input[0];
    SCInputRef In_BullBase = sc.Input[1];
    SCInputRef In_BearBase = sc.Input[2];
    SCInputRef In_Step     = sc.Input[3];
    SCInputRef In_LvWindow = sc.Input[4];
    SCInputRef In_Lv1Min   = sc.Input[5];
    SCInputRef In_Lv2Min   = sc.Input[6];
    SCInputRef In_LvOff    = sc.Input[7];
    SCInputRef In_HideTrig = sc.Input[8];
    SCSubgraphRef IndEma50  = sc.Subgraph[50];
    SCSubgraphRef IndMacdF  = sc.Subgraph[51];
    SCSubgraphRef IndAdx    = sc.Subgraph[52];
    SCSubgraphRef IndSmi    = sc.Subgraph[53];
    SCSubgraphRef IndStochK = sc.Subgraph[54];
    SCSubgraphRef IndRsi    = sc.Subgraph[55];
    SCSubgraphRef IndBbMid  = sc.Subgraph[56];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Trigger Matrix V2 Self-Contained v2.4";
        sc.StudyDescription =
            "V1 names, HTML v2 formulas (catalog 2.0.0-research-2026-09-09). "
            "16 primary families SG0-31 plus Long Trigger / T BUY / R BUY withContext SG32-37. "
            "Long/Short Level 1/2 confluence arrows SG38-41 (V1 BLOCK Z). "
            "Internal OHLC/AV/BV/VAP. No external studies. One instance per chart role. "
            "No alerts, no trading.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 0;
        sc.MaintainVolumeAtPriceData = 1;

        const char* bullName[16] = {
            "Fading MOMO Below",
            "Delta Rise",
            "EXH+",
            "VOL SEQ Bull",
            "VA Long",
            "Slingshot Buy",
            "POC Delta Bull",
            "MPOC+",
            "Delta Trap Bull",
            "POCL Long",
            "OF Long",
            "FA+",
            "Long Trigger Core",
            "T BUY Core",
            "R BUY Core",
            "POC Wave Bull"};
        const char* bearName[16] = {
            "Fading MOMO Above",
            "Delta Drop",
            "EXH-",
            "VOL SEQ Bear",
            "VA Short",
            "Slingshot Sell",
            "POC Delta Bear",
            "MPOC-",
            "Delta Trap Bear",
            "POCS Short",
            "OF Short",
            "FA-",
            "Short Trigger Core",
            "T SELL Core",
            "R SELL Core",
            "POC Wave Bear"};
        const char* bullText[16] = {
            "MOMO-", "RISE", "EXH+", "VOL+", "VA+", "BUY", "POCD+", "MPOC+",
            "TRAP+", "POCL", "OFL", "FA+", "LTRC", "TBUYC", "RBUYC", "WAVE+"};
        const char* bearText[16] = {
            "MOMO+", "DROP", "EXH-", "VOL-", "VA-", "SELL", "POCD-", "MPOC-",
            "TRAP-", "POCS", "OFS", "FA-", "STRC", "TSELLC", "RSELLC", "WAVE-"};

        for (int f = 0; f < 16; f++)
        {
            const int sgB = f * 2;
            const int sgR = f * 2 + 1;
            sc.Subgraph[sgB].Name = bullName[f];
            sc.Subgraph[sgR].Name = bearName[f];
            sc.Subgraph[sgB].DrawZeros = 0;
            sc.Subgraph[sgR].DrawZeros = 0;
            sc.Subgraph[sgB].PrimaryColor = RGB(0, 200, 255);
            sc.Subgraph[sgR].PrimaryColor = RGB(255, 159, 28);
            if (f == 1) // Delta Rise / Drop
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_ARROW_UP;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_ARROW_DOWN;
                sc.Subgraph[sgB].LineWidth = 4;
                sc.Subgraph[sgR].LineWidth = 4;
            }
            else if (f == 3 || f == 9) // VOL SEQ, POCL
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_SQUARE;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_SQUARE;
                sc.Subgraph[sgB].LineWidth = 4;
                sc.Subgraph[sgR].LineWidth = 4;
            }
            else if (f == 6 || f == 10 || f == 11) // POC Delta, OF, FA
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_TRIANGLEUP;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_TRIANGLEDOWN;
                sc.Subgraph[sgB].LineWidth = 8;
                sc.Subgraph[sgR].LineWidth = 8;
                if (f == 11)
                {
                    sc.Subgraph[sgB].PrimaryColor = RGB(0, 220, 0);
                    sc.Subgraph[sgR].PrimaryColor = RGB(220, 0, 0);
                }
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
        }

        // V1 names + HTML withContext formulas (core + indicators).
        const char* ctxBullName[3] = {"Long Trigger", "T BUY", "R BUY"};
        const char* ctxBearName[3] = {"Short Trigger", "T SELL", "R SELL"};
        const char* ctxBullText[3] = {"Long Trigger", "T BUY", "R BUY"};
        const char* ctxBearText[3] = {"Short Trigger", "T SELL", "R SELL"};
        for (int c = 0; c < 3; c++)
        {
            const int sgB = 32 + c * 2;
            const int sgR = sgB + 1;
            sc.Subgraph[sgB].Name = ctxBullName[c];
            sc.Subgraph[sgR].Name = ctxBearName[c];
            if (c == 0)
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_SQUARE;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_SQUARE;
                sc.Subgraph[sgB].LineWidth = 4;
                sc.Subgraph[sgR].LineWidth = 4;
            }
            else
            {
                sc.Subgraph[sgB].DrawStyle = DRAWSTYLE_TEXT;
                sc.Subgraph[sgR].DrawStyle = DRAWSTYLE_TEXT;
                sc.Subgraph[sgB].TextDrawStyleText = ctxBullText[c];
                sc.Subgraph[sgR].TextDrawStyleText = ctxBearText[c];
                sc.Subgraph[sgB].LineWidth = 8;
                sc.Subgraph[sgR].LineWidth = 8;
            }
            sc.Subgraph[sgB].PrimaryColor = RGB(0, 100, 255);
            sc.Subgraph[sgR].PrimaryColor = RGB(255, 0, 100);
            sc.Subgraph[sgB].DrawZeros = 0;
            sc.Subgraph[sgR].DrawZeros = 0;
        }

        // V1 Level 1/2 confluence arrows (not stacked with detectors).
        sc.Subgraph[TMV2_SG_LONG_LV1].Name = "Long Level 1";
        sc.Subgraph[TMV2_SG_LONG_LV1].DrawStyle = DRAWSTYLE_ARROW_UP;
        sc.Subgraph[TMV2_SG_LONG_LV1].PrimaryColor = RGB(220, 220, 220);
        sc.Subgraph[TMV2_SG_LONG_LV1].LineWidth = 6;
        sc.Subgraph[TMV2_SG_LONG_LV1].DrawZeros = 0;
        sc.Subgraph[TMV2_SG_LONG_LV2].Name = "Long Level 2";
        sc.Subgraph[TMV2_SG_LONG_LV2].DrawStyle = DRAWSTYLE_ARROW_UP;
        sc.Subgraph[TMV2_SG_LONG_LV2].PrimaryColor = RGB(255, 165, 0);
        sc.Subgraph[TMV2_SG_LONG_LV2].LineWidth = 6;
        sc.Subgraph[TMV2_SG_LONG_LV2].DrawZeros = 0;
        sc.Subgraph[TMV2_SG_SHORT_LV1].Name = "Short Level 1";
        sc.Subgraph[TMV2_SG_SHORT_LV1].DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sc.Subgraph[TMV2_SG_SHORT_LV1].PrimaryColor = RGB(220, 220, 220);
        sc.Subgraph[TMV2_SG_SHORT_LV1].LineWidth = 6;
        sc.Subgraph[TMV2_SG_SHORT_LV1].DrawZeros = 0;
        sc.Subgraph[TMV2_SG_SHORT_LV2].Name = "Short Level 2";
        sc.Subgraph[TMV2_SG_SHORT_LV2].DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sc.Subgraph[TMV2_SG_SHORT_LV2].PrimaryColor = RGB(255, 165, 0);
        sc.Subgraph[TMV2_SG_SHORT_LV2].LineWidth = 6;
        sc.Subgraph[TMV2_SG_SHORT_LV2].DrawZeros = 0;

        // Hidden indicator workspace (Renko 8t withContext). Not outputs.
        sc.Subgraph[50].Name = "IND EMA50/200";
        sc.Subgraph[51].Name = "IND MACD";
        sc.Subgraph[52].Name = "IND ADX";
        sc.Subgraph[53].Name = "IND SMI";
        sc.Subgraph[54].Name = "IND Stoch";
        sc.Subgraph[55].Name = "IND RSI";
        sc.Subgraph[56].Name = "IND Close BB";
        for (int s = 50; s <= 56; s++)
        {
            sc.Subgraph[s].DrawStyle = DRAWSTYLE_IGNORE;
            sc.Subgraph[s].DrawZeros = 0;
        }

        In_Role.Name = "Chart Role";
        In_Role.SetCustomInputStrings("Range;Renko 6t;Renko 8t");
        In_Role.SetCustomInputIndex(0);

        In_BullBase.Name = "Bull Base Offset (ticks, display only)";
        In_BullBase.SetInt(2);
        In_BullBase.SetIntLimits(0, 50);
        In_BearBase.Name = "Bear Base Offset (ticks, display only)";
        In_BearBase.SetInt(2);
        In_BearBase.SetIntLimits(0, 50);
        In_Step.Name = "Stack Step (ticks, display only)";
        In_Step.SetInt(3);
        In_Step.SetIntLimits(1, 50);
        In_LvWindow.Name = "Level Signal Window (bars)";
        In_LvWindow.SetInt(1);
        In_LvWindow.SetIntLimits(0, 50);
        In_Lv1Min.Name = "Level 1 Min Triggers";
        In_Lv1Min.SetInt(3);
        In_Lv1Min.SetIntLimits(1, 20);
        In_Lv2Min.Name = "Level 2 Min Triggers";
        In_Lv2Min.SetInt(6);
        In_Lv2Min.SetIntLimits(1, 20);
        In_LvOff.Name = "Level Signal Offset (ticks)";
        In_LvOff.SetInt(3);
        In_LvOff.SetIntLimits(0, 50);
        In_HideTrig.Name = "Hide Trigger Arrows";
        In_HideTrig.SetYesNo(0);
        return;
    }

    // ---- Runtime ----
    const int role = In_Role.GetIndex();
    const int bullBase = In_BullBase.GetInt();
    const int bearBase = In_BearBase.GetInt();
    const int stepTicks = In_Step.GetInt();
    const int lvWindow = In_LvWindow.GetInt();
    const int lv1Min = In_Lv1Min.GetInt();
    const int lv2Min = In_Lv2Min.GetInt();
    const int lvOff = In_LvOff.GetInt();
    const int hideTrig = In_HideTrig.GetYesNo() ? 1 : 0;
    const float tickF = sc.TickSize;

    // Family-5 VAP gate: actual chart multiplier must be 1. No user claim.
    const unsigned int vapMultActual = sc.VolumeAtPriceMultiplier;
    const int vapGate = (vapMultActual == 1) ? 1 : 0;

    // Structural fingerprint (two persistent ints, slots 4-5).
    unsigned int tickBits32 = 0;
    {
        float tf = tickF;
        std::memcpy(&tickBits32, &tf, 4);
    }
    unsigned long long fp = Tmv2_Fingerprint(role, vapGate, vapMultActual,
                                            bullBase, bearBase, stepTicks,
                                            (unsigned long long)tickBits32);
    Tmv2_FnvMix(&fp, (unsigned long long)lvWindow);
    Tmv2_FnvMix(&fp, (unsigned long long)lv1Min);
    Tmv2_FnvMix(&fp, (unsigned long long)lv2Min);
    Tmv2_FnvMix(&fp, (unsigned long long)lvOff);
    Tmv2_FnvMix(&fp, (unsigned long long)hideTrig);
    if (fp == 0) fp = 0x9e3779b97f4a7c15ULL;
    int fpLo = 0, fpHi = 0;
    Tmv2_FingerprintSplit(fp, &fpLo, &fpHi);
    int& storedLo = sc.GetPersistentInt(4);
    int& storedHi = sc.GetPersistentInt(5);
    const int fingerprintChanged = (storedLo != fpLo || storedHi != fpHi) ? 1 : 0;

    // One Message Log summary per new structural fingerprint: selected role,
    // actual VAP multiplier, family-5 gate, and the disabled ordinal/code
    // (ord5/VGD when multiplier != 1 on Range). Emitted BEFORE the
    // ArraySize<2 early return so tiny charts still report. Dynamic
    // per-bar gaps (missing VAP, incomplete band windows) fail closed
    // silently in the bar loop with no log.
    if (fingerprintChanged)
    {
        int disabled[16];
        const int nDis = Tmv2_SelfDisabled(role, vapGate, disabled);
        const char* roleName = (role == 0) ? "Range" : (role == 1) ? "Renko 6t" : (role == 2) ? "Renko 8t" : "Unknown";
        char item[64];
        char disBuf[128];
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
                      "TriggerMatrixV2 self-contained v2.1: role=%s vapMult=%u "
                      "| disabled: %s "
                      "| delta/bands/VPOC/VA68/diagonals internal; no external studies; "
                      "dynamic per-bar gaps fail closed silently.",
                      roleName, vapMultActual, disBuf);
        sc.AddMessageToLog(logMsg, 0);
        storedLo = fpLo;
        storedHi = fpHi;
    }

    // Late-VAP retry mark (persistent slot 1): earliest bar whose profile
    // was unresolved on a bar carrying volume. -1/negative = none. Reset on
    // structural change (the rebuild below covers everything).
    int& retryStored = sc.GetPersistentInt(1);
    if (fingerprintChanged) retryStored = -1;

    int first = 0, lastClosed = -1;
    const int hasWork = Tmv2_PlanUpdate(sc.ArraySize, sc.UpdateStartIndex,
                                       sc.IsFullRecalculation ? 1 : 0,
                                       fingerprintChanged, retryStored,
                                       &first, &lastClosed);
    if (!hasWork)
    {
        if (sc.ArraySize > 0)
        {
            for (int s = 0; s < TMV2_N_OUT; s++) sc.Subgraph[s][sc.ArraySize - 1] = 0.0f;
            sc.Subgraph[TMV2_SG_LONG_LV1][sc.ArraySize - 1] = 0.0f;
            sc.Subgraph[TMV2_SG_LONG_LV2][sc.ArraySize - 1] = 0.0f;
            sc.Subgraph[TMV2_SG_SHORT_LV1][sc.ArraySize - 1] = 0.0f;
            sc.Subgraph[TMV2_SG_SHORT_LV2][sc.ArraySize - 1] = 0.0f;
        }
        return;
    }

    int hidden[TMV2_N_OUT];
    for (int s = 0; s < TMV2_N_OUT; s++)
        hidden[s] = (sc.Subgraph[s].DrawStyle == DRAWSTYLE_IGNORE) ? 1 : 0;

    const int baseOK = (sc.BaseData[SC_ASKVOL].GetArraySize() >= sc.ArraySize &&
                        sc.BaseData[SC_BIDVOL].GetArraySize() >= sc.ArraySize) ? 1 : 0;
    const int volOK = (sc.BaseData[SC_VOLUME].GetArraySize() >= sc.ArraySize) ? 1 : 0;

    // One VAP row buffer per update (heap, not persistent): every profile
    // primitive for a bar is derived from a single container copy.
    Tmv2_VapRow* vapRows = new (std::nothrow) Tmv2_VapRow[TMV2_MAX_VAP_ROWS];

    // Earliest bar in this pass whose profile stayed unresolved on a bar
    // carrying volume (late-VAP retry tracking).
    int minMissing = -1;

    for (int i = first; i <= lastClosed; i++)
    {
        for (int s = 0; s < TMV2_N_OUT; s++) sc.Subgraph[s][i] = 0.0f;
        sc.Subgraph[TMV2_SG_LONG_LV1][i] = 0.0f;
        sc.Subgraph[TMV2_SG_LONG_LV2][i] = 0.0f;
        sc.Subgraph[TMV2_SG_SHORT_LV1][i] = 0.0f;
        sc.Subgraph[TMV2_SG_SHORT_LV2][i] = 0.0f;
        if (!baseOK) continue;

        // Compact 31-deep native history ending at i (short prefixes on
        // tiny charts stay valid; Tmv2_FillNative fails bands closed).
        double hO[TMV2_NATIVE_DEPTH], hH[TMV2_NATIVE_DEPTH];
        double hL[TMV2_NATIVE_DEPTH], hC[TMV2_NATIVE_DEPTH];
        double hAV[TMV2_NATIVE_DEPTH], hBV[TMV2_NATIVE_DEPTH];
        double hTV[TMV2_NATIVE_DEPTH];
        int m = 0;
        for (int t = 0; t < TMV2_NATIVE_DEPTH; t++)
        {
            int b = i - (TMV2_NATIVE_DEPTH - 1) + t;
            if (b < 0) continue;
            hO[m] = (double)sc.Open[b];
            hH[m] = (double)sc.High[b];
            hL[m] = (double)sc.Low[b];
            hC[m] = (double)sc.Close[b];
            hAV[m] = (double)sc.BaseData[SC_ASKVOL][b];
            hBV[m] = (double)sc.BaseData[SC_BIDVOL][b];
            hTV[m] = volOK ? (double)sc.BaseData[SC_VOLUME][b] : 0.0;
            m++;
        }

        Tmv2Window w;
        Tmv2_FillNative(&w, hO, hH, hL, hC, hAV, hBV, volOK ? hTV : 0, volOK,
                        m, m - 1, (double)tickF);
        // Internal per-bar profiles for current + 4 priors (family 11 reads
        // VpA back to offset -4) through the production seam.
        if (vapRows != 0)
        {
            for (int k = 0; k < 5; k++)
            {
                int b = i - k;
                if (b < 0) break;
                int nb = 0;
                if (!Tmv2_AcsilCopyVap(sc, b, vapRows, TMV2_MAX_VAP_ROWS, &nb)) continue;
                Tmv2_FillProfileOffset(&w, vapRows, nb, hAV[m - 1 - k], hBV[m - 1 - k],
                                       k, (double)tickF);
            }
        }
        if (!w.vpaOk[0])
        {
            double barVol = w.AV[0] + w.BV[0];
            if (barVol > 0.0 || (volOK && (double)sc.BaseData[SC_VOLUME][i] > 0.0))
            {
                if (minMissing < 0 || i < minMissing) minMissing = i;
            }
        }

        if (role == TMV2_ROLE_RENKO8)
        {
            // Catalog pins: MACD 9/26/9, EMA 50/200, ADX 14/14, SMI 5/3/3,
            // Slow Stoch 10/3/3 SMA, RSI 14 SMA, Close BB 20/2.
            sc.ExponentialMovAvg(sc.Close, IndEma50, i, 50);
            sc.ExponentialMovAvg(sc.Close, IndEma50.Arrays[0], i, 200);
            sc.MACD(sc.Close, IndMacdF, i, 9, 26, 9, MOVAVGTYPE_EXPONENTIAL);
            sc.ADX(sc.BaseDataIn, IndAdx, i, 14, 14);
            {
                float smiHi = sc.GetHighest(sc.High, i, 5);
                float smiLo = sc.GetLowest(sc.Low, i, 5);
                IndSmi.Arrays[0][i] = sc.Close[i] - (smiHi + smiLo) * 0.5f;
                IndSmi.Arrays[1][i] = smiHi - smiLo;
                Tmv2_EmaPass(IndSmi.Arrays[0], IndSmi.Arrays[2], i, 3);
                Tmv2_EmaPass(IndSmi.Arrays[1], IndSmi.Arrays[3], i, 3);
                Tmv2_EmaPass(IndSmi.Arrays[2], IndSmi.Arrays[4], i, 3);
                Tmv2_EmaPass(IndSmi.Arrays[3], IndSmi.Arrays[5], i, 3);
                IndSmi[i] = (IndSmi.Arrays[5][i] != 0.0f)
                    ? (100.0f * IndSmi.Arrays[4][i] / (IndSmi.Arrays[5][i] * 0.5f))
                    : 0.0f;
            }
            {
                float hh = sc.GetHighest(sc.High, i, 10);
                float ll = sc.GetLowest(sc.Low, i, 10);
                float range = hh - ll;
                IndStochK.Arrays[0][i] = (range > 0.0f)
                    ? (100.0f * (sc.Close[i] - ll) / range) : 50.0f;
                sc.SimpleMovAvg(IndStochK.Arrays[0], IndStochK, i, 3);
                sc.SimpleMovAvg(IndStochK, IndStochK.Arrays[1], i, 3);
            }
            sc.RSI(sc.Close, IndRsi, i, MOVAVGTYPE_SIMPLE, 14);
            sc.BollingerBands(sc.Close, IndBbMid, i, 20, 2.0f, MOVAVGTYPE_SIMPLE);
            Tmv2_LoadCtxFromSg(&w, i, 0,
                IndMacdF, IndEma50, IndEma50.Arrays[0], IndAdx, IndSmi,
                IndStochK, IndStochK.Arrays[1], IndRsi,
                IndBbMid.Arrays[0], IndBbMid.Arrays[1]);
            Tmv2_LoadCtxFromSg(&w, i - 1, 1,
                IndMacdF, IndEma50, IndEma50.Arrays[0], IndAdx, IndSmi,
                IndStochK, IndStochK.Arrays[1], IndRsi,
                IndBbMid.Arrays[0], IndBbMid.Arrays[1]);
        }

        // Family 5 needs side-specific exact-tick VAP rows: evaluate Bull
        // side with AVAP rows and Bear side with BVAP rows, keeping each
        // side's SGs. Rows come from a fresh single copy of bar i.
        Tmv2Window wBull = w;
        Tmv2Window wBear = w;
        wBull.vapMultIs1 = vapGate;
        wBear.vapMultIs1 = vapGate;
        if (role == TMV2_ROLE_RANGE && vapGate && vapRows != 0 &&
            Tmv2_TickSizeOk((double)tickF))
        {
            int nb = 0;
            if (Tmv2_AcsilCopyVap(sc, i, vapRows, TMV2_MAX_VAP_ROWS, &nb))
            {
                int lowT = 0, highT = 0;
                if (Tmv2_PriceToTickSafe((double)sc.Low[i], (double)tickF, &lowT) &&
                    Tmv2_PriceToTickSafe((double)sc.High[i], (double)tickF, &highT))
                {
                    Tmv2_FillVapSides(&wBull, &wBear, vapRows, nb,
                                      w.AV[0], w.BV[0], vapGate, lowT, highT);
                }
            }
        }
        int firedB[TMV2_N_OUT], firedR[TMV2_N_OUT];
        Tmv2_EvalRoleAt(role, &wBull, i, firedB);
        Tmv2_EvalRoleAt(role, &wBear, i, firedR);
        int fired[TMV2_N_OUT];
        for (int s = 0; s < TMV2_N_OUT; s++)
            fired[s] = (s % 2 == 0) ? firedB[s] : firedR[s];

        int bullLane[TMV2_N_OUT], bearLane[TMV2_N_OUT];
        Tmv2_StackLanes(fired, hidden, bullLane, bearLane);
        const float lo = sc.Low[i];
        const float hi = sc.High[i];
        if (!hideTrig)
        {
            for (int s = 0; s < TMV2_N_OUT; s++)
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

        // V1 BLOCK Z: persist per-bar counts then window-sum. Hide does not
        // zero Arrays[0], so lookback still works.
        const int bullAtI = Tmv2_LevelCountBar(fired, 1);
        const int bearAtI = Tmv2_LevelCountBar(fired, 0);
        sc.Subgraph[TMV2_SG_LONG_LV1].Arrays[0][i] = (float)bullAtI;
        sc.Subgraph[TMV2_SG_SHORT_LV1].Arrays[0][i] = (float)bearAtI;
        int wUse = lvWindow;
        if (wUse < 1) wUse = 1;
        int bullCount = 0;
        int bearCount = 0;
        for (int b = i; b >= i - wUse + 1 && b >= 0; b--)
        {
            bullCount += (int)sc.Subgraph[TMV2_SG_LONG_LV1].Arrays[0][b];
            bearCount += (int)sc.Subgraph[TMV2_SG_SHORT_LV1].Arrays[0][b];
        }
        const int bullTier = Tmv2_LevelTier(bullCount, lv1Min, lv2Min);
        const int bearTier = Tmv2_LevelTier(bearCount, lv1Min, lv2Min);
        const float lvYBull = lo - (float)lvOff * tickF;
        const float lvYBear = hi + (float)lvOff * tickF;
        if (bullTier == 2) sc.Subgraph[TMV2_SG_LONG_LV2][i] = lvYBull;
        else if (bullTier == 1) sc.Subgraph[TMV2_SG_LONG_LV1][i] = lvYBull;
        if (bearTier == 2) sc.Subgraph[TMV2_SG_SHORT_LV2][i] = lvYBear;
        else if (bearTier == 1) sc.Subgraph[TMV2_SG_SHORT_LV1][i] = lvYBear;
    }

    // Bounded late-VAP retry: remember the earliest unresolved volume bar
    // within the lookback so the next update extends its range there even
    // if Sierra does not rewind UpdateStartIndex. Older marks are dropped.
    if (minMissing >= 0 && minMissing >= lastClosed - TMV2_RETRY_LOOKBACK)
        retryStored = minMissing;
    else
        retryStored = -1;

    delete[] vapRows;
    vapRows = 0;

    for (int s = 0; s < TMV2_N_OUT; s++) sc.Subgraph[s][sc.ArraySize - 1] = 0.0f;
    sc.Subgraph[TMV2_SG_LONG_LV1][sc.ArraySize - 1] = 0.0f;
    sc.Subgraph[TMV2_SG_LONG_LV2][sc.ArraySize - 1] = 0.0f;
    sc.Subgraph[TMV2_SG_SHORT_LV1][sc.ArraySize - 1] = 0.0f;
    sc.Subgraph[TMV2_SG_SHORT_LV2][sc.ArraySize - 1] = 0.0f;
}

#endif // TMV2_UNITTEST
