// Self-contained primitive tests for TriggerMatrixV2.cpp (TDD Task 2 RED).
// Written BEFORE the internal calculation helpers exist -> compile fail.
// Covers: native-delta bands (20/0.9 population-SD, readiness), per-bar
// VPOC ties (closest-to-midpoint, lower on equidistance), 68% VA expansion
// (greater side, equal includes both), diagonal +/-300% ratio counts
// (exact threshold, zero-denominator skip, missing-row skip, min totals),
// exact-tick VAP side rows, tick conversion, and adapter-level integration
// (role partition, recalc-vs-incremental determinism, correction/recovery,
// warm-up gating on internally built windows).
#define TMV2_UNITTEST 1
#include "../TriggerMatrixV2.cpp"
#include <cmath>
#include <cstdio>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; std::printf("FAIL %d: %s\n", __LINE__, msg); } \
} while (0)

static Tmv2_VapRow MkRow(int tick, double vol, double ask, double bid)
{
    Tmv2_VapRow r;
    r.tick = tick; r.vol = vol; r.ask = ask; r.bid = bid;
    return r;
}

// ---------- S01 canonical math parameters ----------
static void S01_Params()
{
    CHECK(TMV2_BAND_LEN == 20, "band length 20");
    CHECK(std::fabs(TMV2_BAND_MULT - 0.9) < 1e-12, "band multiplier 0.9");
    CHECK(std::fabs(TMV2_VA_PCT - 0.68) < 1e-12, "VA 68%");
    CHECK(std::fabs(TMV2_DIAG_PCT - 300.0) < 1e-12, "diagonal 300%");
    CHECK(std::fabs(TMV2_DIAG_MIN_TOTAL - 20.0) < 1e-12, "diagonal min total 20");
    CHECK(TMV2_MAX_VAP_ROWS >= 256, "row cap sane");
    CHECK(Tmv2_TickSizeOk(0.25) == 1, "tick ok");
    CHECK(Tmv2_TickSizeOk(0.0) == 0, "tick zero bad");
    CHECK(Tmv2_TickSizeOk(-0.25) == 0, "tick negative bad");
    {
        int t = -1;
        CHECK(Tmv2_PriceToTickSafe(100.0, 0.25, &t) == 1 && t == 400, "price to tick");
        CHECK(Tmv2_PriceToTickSafe(100.125, 0.25, &t) == 1 && t == 401, "half-tick rounds up");
        CHECK(Tmv2_PriceToTickSafe(99.875, 0.25, &t) == 1 && t == 400, "price to tick rounds down-half");
    }
}

// ---------- S02 bands: 20/0.9 population SD ----------
static void S02_Bands()
{
    double d[20];
    int ok[20];
    for (int i = 0; i < 20; i++) { d[i] = 1.0 + i; ok[i] = 1; }
    double up = 0.0, lo = 0.0;
    // mean 10.5, popVar 33.25 -> SD=5.766281297594190; 0.9*SD=5.189653167834771
    CHECK(Tmv2_BandAt(d, ok, 20, 19, 0.9, &up, &lo) == 1, "band valid full window");
    CHECK(std::fabs(up - 15.689653167834771) < 1e-9, "band upper exact");
    CHECK(std::fabs(lo - 5.310346832165229) < 1e-9, "band lower exact");
    // sample SD would give 10.5+0.9*sqrt(35)=15.8246 -> distinguishes population
    CHECK(std::fabs(up - 15.8246) > 0.01, "band is population not sample SD");
    // 19-bar window fails closed
    {
        double d19[19];
        int ok19[19];
        for (int i = 0; i < 19; i++) { d19[i] = 1.0 + i; ok19[i] = 1; }
        CHECK(Tmv2_BandAt(d19, ok19, 19, 18, 0.9, &up, &lo) == 0, "19 bars not ready");
    }
    // one invalid bar inside window fails closed
    {
        int okBad[20];
        for (int i = 0; i < 20; i++) okBad[i] = 1;
        okBad[7] = 0;
        CHECK(Tmv2_BandAt(d, okBad, 20, 19, 0.9, &up, &lo) == 0, "hole fails closed");
    }
    // NaN delta fails closed even when flagged ok
    {
        double dNaN[20];
        int okAll[20];
        for (int i = 0; i < 20; i++) { dNaN[i] = 1.0 + i; okAll[i] = 1; }
        dNaN[3] = std::acos(2.0);
        CHECK(Tmv2_BandAt(dNaN, okAll, 20, 19, 0.9, &up, &lo) == 0, "NaN fails closed");
    }
    // constant series -> SD 0, bands equal mean
    {
        double dc[20];
        int okc[20];
        for (int i = 0; i < 20; i++) { dc[i] = 7.0; okc[i] = 1; }
        CHECK(Tmv2_BandAt(dc, okc, 20, 19, 0.9, &up, &lo) == 1, "flat valid");
        CHECK(up == 7.0 && lo == 7.0, "flat bands equal mean");
    }
    // end-index lag: band ending at 19 over dh[0..19] with longer history
    {
        double dh[25];
        int okh[25];
        for (int i = 0; i < 25; i++) { dh[i] = 101.0 + i; okh[i] = 1; }
        double upE = 0.0, loE = 0.0;
        CHECK(Tmv2_BandAt(dh, okh, 25, 19, 0.9, &upE, &loE) == 1, "lagged band valid");
        // window is values 101..120: mean 110.5, popVar 33.25 (same shape as 1..20)
        CHECK(std::fabs(upE - (110.5 + 0.9 * std::sqrt(33.25))) < 1e-9, "lagged upper exact");
        CHECK(std::fabs(loE - (110.5 - 0.9 * std::sqrt(33.25))) < 1e-9, "lagged lower exact");
    }
    // end out of range fails closed
    CHECK(Tmv2_BandAt(d, ok, 20, 19, 0.9, 0, &lo) == 0, "null out fails");
    CHECK(Tmv2_BandAt(d, ok, 20, 20, 0.9, &up, &lo) == 0, "end past n fails");
    CHECK(Tmv2_BandAt(d, ok, 20, 18, 0.9, &up, &lo) == 0, "end short of window fails");
}

// ---------- S03 VPOC: max volume, ties to midpoint then lower ----------
static void S03_Poc()
{
    int poc = -1;
    { // clear max
        Tmv2_VapRow r[4] = { MkRow(0, 5, 3, 2), MkRow(1, 9, 5, 4),
                             MkRow(2, 30, 15, 15), MkRow(3, 7, 4, 3) };
        CHECK(Tmv2_ProfilePoc(r, 4, &poc) == 1 && poc == 2, "poc clear max");
    }
    { // tie equidistant from midpoint -> lower (ticks 0..3, mid 1.5)
        Tmv2_VapRow r[4] = { MkRow(0, 5, 3, 2), MkRow(1, 20, 10, 10),
                             MkRow(2, 20, 10, 10), MkRow(3, 5, 3, 2) };
        CHECK(Tmv2_ProfilePoc(r, 4, &poc) == 1 && poc == 1, "poc equidistant tie lower");
    }
    { // tie asymmetric: closer to midpoint wins (ticks 0..4, mid 2)
        Tmv2_VapRow r[5] = { MkRow(0, 5, 3, 2), MkRow(1, 5, 3, 2), MkRow(2, 20, 10, 10),
                             MkRow(3, 20, 10, 10), MkRow(4, 5, 3, 2) };
        CHECK(Tmv2_ProfilePoc(r, 5, &poc) == 1 && poc == 2, "poc closer-to-mid wins");
    }
    { // tie both equidistant non-centered profile (ticks 0 and 4 only, mid 2)
        Tmv2_VapRow r[2] = { MkRow(0, 20, 10, 10), MkRow(4, 20, 10, 10) };
        CHECK(Tmv2_ProfilePoc(r, 2, &poc) == 1 && poc == 0, "poc sparse tie lower");
    }
    { // single row valid
        Tmv2_VapRow r[1] = { MkRow(7, 12, 6, 6) };
        CHECK(Tmv2_ProfilePoc(r, 1, &poc) == 1 && poc == 0, "poc single row");
    }
    { // empty / all-zero / negative / NaN / overflow / null
        Tmv2_VapRow r[2] = { MkRow(0, 10, 5, 5), MkRow(1, 10, 5, 5) };
        CHECK(Tmv2_ProfilePoc(r, 0, &poc) == 0, "poc empty invalid");
        CHECK(Tmv2_ProfilePoc(0, 0, &poc) == 0, "poc null invalid");
        Tmv2_VapRow z[2] = { MkRow(0, 0, 0, 0), MkRow(1, 0, 0, 0) };
        CHECK(Tmv2_ProfilePoc(z, 2, &poc) == 0, "poc all-zero invalid");
        Tmv2_VapRow neg[2] = { MkRow(0, 10, 5, 5), MkRow(1, -3, 0, 0) };
        CHECK(Tmv2_ProfilePoc(neg, 2, &poc) == 0, "poc negative invalid");
        Tmv2_VapRow nn[2] = { MkRow(0, 10, 5, 5), MkRow(1, std::acos(2.0), 5, 5) };
        CHECK(Tmv2_ProfilePoc(nn, 2, &poc) == 0, "poc NaN invalid");
        CHECK(Tmv2_ProfilePoc(r, TMV2_MAX_VAP_ROWS + 1, &poc) == 0, "poc overflow invalid");
        CHECK(Tmv2_ProfilePoc(r, 2, 0) == 0, "poc null out invalid");
    }
    { // unsorted ticks rejected (fail closed, not silently misread)
        Tmv2_VapRow r[3] = { MkRow(2, 5, 3, 2), MkRow(0, 30, 15, 15), MkRow(1, 5, 3, 2) };
        CHECK(Tmv2_ProfilePoc(r, 3, &poc) == 0, "poc unsorted invalid");
    }
    { // duplicate ticks rejected
        Tmv2_VapRow r[2] = { MkRow(1, 30, 15, 15), MkRow(1, 30, 15, 15) };
        CHECK(Tmv2_ProfilePoc(r, 2, &poc) == 0, "poc duplicate tick invalid");
    }
}

// ---------- S04 VA 68%: greater side, equal includes both ----------
static void S04_ValueArea()
{
    int hi = -1, lo = -1;
    { // vols [5,10,30,10,5] total 60 target 40.8; up=10 vs down=10 equal -> both
        Tmv2_VapRow r[5] = { MkRow(0, 5, 3, 2), MkRow(1, 10, 5, 5), MkRow(2, 30, 15, 15),
                             MkRow(3, 10, 5, 5), MkRow(4, 5, 3, 2) };
        CHECK(Tmv2_ProfileVa(r, 5, 2, 0.68, &hi, &lo) == 1, "va tie valid");
        CHECK(hi == 3 && lo == 1, "va equal tie includes both");
    }
    { // vols [5,8,30,12,5]: up 12 > down 8 -> up only, 42 >= 40.8 stops
        Tmv2_VapRow r[5] = { MkRow(0, 5, 3, 2), MkRow(1, 8, 4, 4), MkRow(2, 30, 15, 15),
                             MkRow(3, 12, 6, 6), MkRow(4, 5, 3, 2) };
        CHECK(Tmv2_ProfileVa(r, 5, 2, 0.68, &hi, &lo) == 1, "va greater valid");
        CHECK(hi == 3 && lo == 2, "va greater side only");
    }
    { // asymmetric multi-step: vols [4,6,30,7,8,5] total 60 target 40.8
      // poc idx2: up 7 vs down 6 -> up (37); up 8 vs down 6 -> up (45>=40.8)
        Tmv2_VapRow r[6] = { MkRow(0, 4, 2, 2), MkRow(1, 6, 3, 3), MkRow(2, 30, 15, 15),
                             MkRow(3, 7, 4, 3), MkRow(4, 8, 4, 4), MkRow(5, 5, 3, 2) };
        CHECK(Tmv2_ProfileVa(r, 6, 2, 0.68, &hi, &lo) == 1, "va multi valid");
        CHECK(hi == 4 && lo == 2, "va grows one row at a time greater side");
    }
    { // single row: POC alone meets target
        Tmv2_VapRow r[1] = { MkRow(7, 50, 25, 25) };
        CHECK(Tmv2_ProfileVa(r, 1, 0, 0.68, &hi, &lo) == 1, "va single valid");
        CHECK(hi == 7 && lo == 7, "va single is poc");
    }
    { // POC at edge: only one side to grow
        Tmv2_VapRow r[3] = { MkRow(0, 40, 20, 20), MkRow(1, 10, 5, 5), MkRow(2, 10, 5, 5) };
        CHECK(Tmv2_ProfileVa(r, 3, 0, 0.68, &hi, &lo) == 1, "va edge valid");
        CHECK(hi == 1 && lo == 0, "va edge grows available side");
    }
    { // invalid: bad poc, empty, null rows, null outs
        Tmv2_VapRow r[3] = { MkRow(0, 10, 5, 5), MkRow(1, 30, 15, 15), MkRow(2, 10, 5, 5) };
        CHECK(Tmv2_ProfileVa(r, 3, 5, 0.68, &hi, &lo) == 0, "va bad poc invalid");
        CHECK(Tmv2_ProfileVa(r, 3, -1, 0.68, &hi, &lo) == 0, "va neg poc invalid");
        CHECK(Tmv2_ProfileVa(r, 0, 0, 0.68, &hi, &lo) == 0, "va empty invalid");
        CHECK(Tmv2_ProfileVa(0, 0, 0, 0.68, &hi, &lo) == 0, "va null invalid");
        CHECK(Tmv2_ProfileVa(r, 3, 1, 0.68, 0, &lo) == 0, "va null hi invalid");
        CHECK(Tmv2_ProfileVa(r, 3, 1, 0.68, &hi, 0) == 0, "va null lo invalid");
    }
}

// ---------- S05 diagonals: signed ratio at exactly +/-300% ----------
static void S05_Diagonals()
{
    int ask = -1, bid = -1;
    { // ask 40 vs bid 10 -> +400% counts
        Tmv2_VapRow r[2] = { MkRow(0, 30, 5, 10), MkRow(1, 50, 40, 10) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag ask valid");
        CHECK(ask == 1 && bid == 0, "diag ask +400 counts");
    }
    { // exactly +300% counts (meets-or-exceeds)
        Tmv2_VapRow r[2] = { MkRow(0, 30, 5, 10), MkRow(1, 50, 30, 20) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag edge valid");
        CHECK(ask == 1 && bid == 0, "diag exactly 300 counts");
    }
    { // 299% does not count
        Tmv2_VapRow r[2] = { MkRow(0, 30, 5, 10), MkRow(1, 50, 29.9, 20) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag short valid");
        CHECK(ask == 0 && bid == 0, "diag 299 no count");
    }
    { // bid dominant: bid 40 vs askUp 10 -> -400% counts
        Tmv2_VapRow r[2] = { MkRow(0, 50, 5, 40), MkRow(1, 30, 10, 20) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag bid valid");
        CHECK(ask == 0 && bid == 1, "diag bid -400 counts");
    }
    { // exactly -300% counts
        Tmv2_VapRow r[2] = { MkRow(0, 50, 5, 30), MkRow(1, 30, 10, 20) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag bid edge valid");
        CHECK(ask == 0 && bid == 1, "diag exactly -300 counts");
    }
    { // zero denominator skipped (zero compares disabled)
        Tmv2_VapRow r[2] = { MkRow(0, 30, 5, 0), MkRow(1, 50, 40, 10) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag zero valid profile");
        CHECK(ask == 0 && bid == 0, "diag zero denominator no count");
    }
    { // min total: row under 20 skips the pair
        Tmv2_VapRow r[2] = { MkRow(0, 19, 5, 10), MkRow(1, 50, 40, 10) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag small valid");
        CHECK(ask == 0 && bid == 0, "diag min-total gate");
    }
    { // equality takes ask side: ratio 100 < 300 -> no count either side
        Tmv2_VapRow r[2] = { MkRow(0, 30, 5, 10), MkRow(1, 30, 10, 20) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "diag equal valid");
        CHECK(ask == 0 && bid == 0, "diag equality no false bid");
    }
    { // single-row profile: valid zero counts (complete tiny profile)
        Tmv2_VapRow r[1] = { MkRow(3, 40, 20, 20) };
        CHECK(Tmv2_DiagCounts(r, 1, 300.0, 20.0, &ask, &bid) == 1, "diag single valid");
        CHECK(ask == 0 && bid == 0, "diag single zero counts");
    }
    { // missing whole profile must not become valid zero
        CHECK(Tmv2_DiagCounts(0, 0, 300.0, 20.0, &ask, &bid) == 0, "diag missing invalid");
    }
    { // malformed row invalidates (negative ask)
        Tmv2_VapRow r[2] = { MkRow(0, 30, 5, 10), MkRow(1, 50, -1, 10) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 0, "diag malformed invalid");
    }
    { // multi-pair count accumulates
        Tmv2_VapRow r[3] = { MkRow(0, 30, 5, 10), MkRow(1, 50, 40, 10),
                             MkRow(2, 60, 50, 5) };
        // pair(0,1): 40/10=400 ask; pair(1,2): bid 10 vs ask 50 -> ask 500
        CHECK(Tmv2_DiagCounts(r, 3, 300.0, 20.0, &ask, &bid) == 1, "diag multi valid");
        CHECK(ask == 2 && bid == 0, "diag multi ask 2");
    }
}

// ---------- S06 exact-tick VAP side rows (family 5) ----------
static void S06_SideRows()
{
    Tmv2_VapRow r[5];
    for (int k = 0; k < 5; k++) r[k] = MkRow(400 + k, 30.0, 5.0 + k, 25.0 - k);
    double tot = 0.0, side = 0.0;
    CHECK(Tmv2_VapSideRow(r, 5, 402, &tot, &side, 1) == 1, "side exact hit");
    CHECK(tot == 30.0 && side == 7.0, "side bull ask value");
    CHECK(Tmv2_VapSideRow(r, 5, 402, &tot, &side, 0) == 1, "side bear hit");
    CHECK(side == 23.0, "side bear bid value");
    CHECK(Tmv2_VapSideRow(r, 5, 399, &tot, &side, 1) == 0, "side missing tick invalid");
    CHECK(Tmv2_VapSideRow(r, 5, 405, &tot, &side, 1) == 0, "side past end invalid");
    { // zero total row invalid (never a valid VAP row)
        Tmv2_VapRow z[2] = { MkRow(400, 0, 0, 0), MkRow(401, 30, 5, 25) };
        CHECK(Tmv2_VapSideRow(z, 2, 400, &tot, &side, 1) == 0, "side zero total invalid");
    }
    CHECK(Tmv2_VapSideRow(0, 0, 400, &tot, &side, 1) == 0, "side null invalid");
    CHECK(Tmv2_VapSideRow(r, 5, 402, 0, &side, 1) == 0, "side null tot invalid");
}

// ---------- S07 production-seam integration ----------
// These tests drive the SAME seams the ACSIL body calls: Tmv2_FillNative
// (native OHLC/flow/delta/bands), Tmv2_FillProfileOffset (POC/VA/diagonals
// per lag offset), Tmv2_FillVapSides (family 5), Tmv2_PlanUpdate (first/
// lastClosed planning). Scenarios start from a REAL firing trigger,
// clear it via missing data/correction, restore it, and compare repeated
// builds. The forming-bar zero write itself is an sc.Subgraph assignment
// in the ACSIL body (unrunnable without Sierra); the planner boundary that
// drives it (closed-to-forming reclassification, tiny charts) is tested in
// S12. No coverage is claimed beyond what these seams execute.
static void HistFill(double* O, double* H, double* L, double* C,
                     double* AV, double* BV, double* TV, int n,
                     double o, double h, double l, double c,
                     double av, double bv)
{
    for (int i = 0; i < n; i++)
    {
        O[i] = o; H[i] = h; L[i] = l; C[i] = c;
        AV[i] = av; BV[i] = bv; TV[i] = av + bv;
    }
}

// Family-6 bull firing setup through the seams (Range): VpA 100.0 -> 100.5,
// VA 99.75..100.25 -> 100.25..100.75, close at high.
static void BuildFam6Seam(double* O, double* H, double* L, double* C,
                          double* AV, double* BV, double* TV,
                          Tmv2_VapRow* r0, Tmv2_VapRow* r1)
{
    HistFill(O, H, L, C, AV, BV, TV, 31, 100.0, 101.0, 99.0, 100.0, 20.0, 20.0);
    O[30] = 100.5; H[30] = 101.5; L[30] = 100.0; C[30] = 101.0;
    O[29] = 100.0; H[29] = 101.0; L[29] = 99.0; C[29] = 100.0;
    r0[0] = MkRow(400, 5, 3, 2); r0[1] = MkRow(401, 10, 5, 5);
    r0[2] = MkRow(402, 30, 15, 15); r0[3] = MkRow(403, 10, 5, 5);
    r0[4] = MkRow(404, 5, 3, 2);
    r1[0] = MkRow(398, 5, 3, 2); r1[1] = MkRow(399, 10, 5, 5);
    r1[2] = MkRow(400, 30, 15, 15); r1[3] = MkRow(401, 10, 5, 5);
    r1[4] = MkRow(402, 5, 3, 2);
}

// Family-14 bull firing setup through the seams (Renko6): flat -30 deltas
// with a +40 current reversal, ask-heavy current profile (3 ask levels),
// bid-marked prior profile, rising VPOC 100.0 -> 100.5.
static void BuildFam14Seam(double* O, double* H, double* L, double* C,
                           double* AV, double* BV, double* TV,
                           Tmv2_VapRow* r0, Tmv2_VapRow* r1)
{
    HistFill(O, H, L, C, AV, BV, TV, 31, 100.0, 101.0, 99.0, 100.0, 20.0, 50.0);
    AV[30] = 70.0; BV[30] = 30.0; TV[30] = 100.0;
    O[30] = 100.0; H[30] = 100.75; L[30] = 99.75; C[30] = 100.5;
    C[29] = 99.75;
    r0[0] = MkRow(400, 50, 5, 10); r0[1] = MkRow(401, 50, 40, 10);
    r0[2] = MkRow(402, 60, 45, 15); r0[3] = MkRow(403, 50, 50, 12);
    r0[4] = MkRow(404, 50, 10, 10);
    r1[0] = MkRow(398, 50, 5, 40); r1[1] = MkRow(399, 50, 10, 20);
    r1[2] = MkRow(400, 60, 60, 10); r1[3] = MkRow(401, 50, 10, 10);
}

static void S07_Integration()
{
    double O[31], H[31], L[31], C[31], AV[31], BV[31], TV[31];
    // Family 6 through the seams: fires, clears on missing lagged profile,
    // restores deterministically.
    {
        Tmv2_VapRow r0[5], r1[5];
        BuildFam6Seam(O, H, L, C, AV, BV, TV, r0, r1);
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        CHECK(Tmv2_FillProfileOffset(&w, r0, 5, 20.0, 20.0, 0, 0.25) == 1, "seam poc0");
        CHECK(Tmv2_FillProfileOffset(&w, r1, 5, 20.0, 20.0, 1, 0.25) == 1, "seam poc1");
        CHECK(w.VpA[0] == w.VpB[0] && w.vpaOk[0] && w.vpbOk[0], "seam vpoc A==B single producer");
        CHECK(w.VAH[0] >= w.VAL[0], "seam va ordered");
        int fired[TMV2_N_OUT];
        CHECK(Tmv2_EvalRoleAt(0, &w, 30, fired) >= 1 && fired[8] == 1, "seam fam6 fires SG8");
        int f1[TMV2_N_OUT], f2[TMV2_N_OUT];
        CHECK(Tmv2_EvalRole(1, &w, f1) == 0, "seam role1 silent on range");
        CHECK(Tmv2_EvalRole(2, &w, f2) == 0, "seam role2 silent on range");
        for (int s = 20; s < 24; s++) CHECK(f1[s] == 0, "seam r6 SGs silent");
        // Missing lagged profile clears the dependent trigger only.
        Tmv2Window v = w;
        CHECK(Tmv2_FillProfileOffset(&v, r1, 0, 20.0, 20.0, 1, 0.25) == 0, "seam missing lag invalid");
        int cleared[TMV2_N_OUT];
        Tmv2_EvalRoleAt(0, &v, 30, cleared);
        CHECK(cleared[8] == 0, "seam missing lag clears SG8");
        // Restore returns the identical trigger (deterministic recovery).
        Tmv2Window r;
        Tmv2_ClearWindow(&r);
        Tmv2_FillNative(&r, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        Tmv2_FillProfileOffset(&r, r0, 5, 20.0, 20.0, 0, 0.25);
        Tmv2_FillProfileOffset(&r, r1, 5, 20.0, 20.0, 1, 0.25);
        int again[TMV2_N_OUT];
        Tmv2_EvalRoleAt(0, &r, 30, again);
        for (int s = 0; s < TMV2_N_OUT; s++) CHECK(again[s] == fired[s], "seam recovery identical");
    }
    // Family 14 through the seams: diagonal-dependent trigger fires,
    // clears on missing current profile AND on price correction, restores.
    {
        Tmv2_VapRow r0[5], r1[4];
        BuildFam14Seam(O, H, L, C, AV, BV, TV, r0, r1);
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        CHECK(Tmv2_FillProfileOffset(&w, r0, 5, 70.0, 30.0, 0, 0.25) == 1, "seam14 poc0");
        CHECK(Tmv2_FillProfileOffset(&w, r1, 4, 20.0, 50.0, 1, 0.25) == 1, "seam14 poc1");
        CHECK(w.AskD[0] == 3.0 && w.askOk[0], "seam14 ask count 3");
        CHECK(w.BidD[1] == 1.0 && w.bidOk[1], "seam14 prior bid count 1");
        int fired[TMV2_N_OUT];
        Tmv2_EvalRoleAt(1, &w, 30, fired);
        CHECK(fired[20] == 1, "seam14 fires SG20");
        for (int s = 0; s < 20; s++) CHECK(fired[s] == 0, "seam14 range SGs silent");
        // Missing current profile clears.
        { Tmv2Window v = w;
          CHECK(Tmv2_FillProfileOffset(&v, r0, 0, 70.0, 30.0, 0, 0.25) == 0, "seam14 missing invalid");
          int c[TMV2_N_OUT]; Tmv2_EvalRoleAt(1, &v, 30, c);
          CHECK(c[20] == 0, "seam14 missing clears SG20"); }
        // Price correction breaking C1<=Vp6_1 clears without touching data.
        { Tmv2Window v = w; v.C[1] = 100.5;
          int c[TMV2_N_OUT]; Tmv2_EvalRoleAt(1, &v, 30, c);
          CHECK(c[20] == 0, "seam14 correction clears SG20"); }
        // Rebuild is bit-identical (full vs incremental determinism).
        { Tmv2Window r; Tmv2_ClearWindow(&r);
          Tmv2_FillNative(&r, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
          Tmv2_FillProfileOffset(&r, r0, 5, 70.0, 30.0, 0, 0.25);
          Tmv2_FillProfileOffset(&r, r1, 4, 20.0, 50.0, 1, 0.25);
          int a[TMV2_N_OUT]; Tmv2_EvalRoleAt(1, &r, 30, a);
          for (int s = 0; s < TMV2_N_OUT; s++) CHECK(a[s] == fired[s], "seam14 rebuild identical"); }
    }
    // Warm-up gating on seam windows: ord9 (SG14/15) needs bar >= 2.
    {
        Tmv2_VapRow r0[5], r1[5];
        BuildFam6Seam(O, H, L, C, AV, BV, TV, r0, r1);
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        Tmv2_FillProfileOffset(&w, r0, 5, 20.0, 20.0, 0, 0.25);
        Tmv2_FillProfileOffset(&w, r1, 5, 20.0, 20.0, 1, 0.25);
        int early[TMV2_N_OUT];
        CHECK(Tmv2_EvalRoleAt(0, &w, 1, early) >= 0, "seam warmup evals");
        CHECK(early[14] == 0 && early[15] == 0, "seam warmup clears ord9 at bar 1");
        int bar0[TMV2_N_OUT];
        CHECK(Tmv2_EvalRoleAt(0, &w, 0, bar0) == 0, "seam bar 0 all gated");
    }
}

// ---------- S08 sc.Round-compatible half-away-from-zero ----------
static void S08_Round()
{
    CHECK(Tmv2_RoundHalfAway(300.0) == 300.0, "round exact");
    CHECK(Tmv2_RoundHalfAway(299.5) == 300.0, "round half up");
    CHECK(Tmv2_RoundHalfAway(299.49) == 299.0, "round below half down");
    CHECK(Tmv2_RoundHalfAway(-299.5) == -300.0, "round negative half away");
    CHECK(Tmv2_RoundHalfAway(-299.49) == -299.0, "round negative below half");
    CHECK(Tmv2_RoundHalfAway(2.5) == 3.0, "round 2.5 up");
    CHECK(Tmv2_RoundHalfAway(-2.5) == -3.0, "round -2.5 away");
    CHECK(Tmv2_RoundHalfAway(0.5) == 1.0, "round 0.5 up");
    CHECK(Tmv2_RoundHalfAway(-0.5) == -1.0, "round -0.5 away");
    CHECK(Tmv2_RoundHalfAway(0.0) == 0.0, "round zero");
}

// ---------- S09 diagonals: rounded ratios, sparse rows ----------
// Sierra rounds the signed ratio (half away from zero) BEFORE the
// threshold compare: 599/200*100 = 299.5 -> 300 -> counts.
static void S09_DiagRound()
{
    int ask = -1, bid = -1;
    { // integer volumes: +299.5 rounds to +300 -> ask counts
        Tmv2_VapRow r[2] = { MkRow(0, 250, 5, 200), MkRow(1, 650, 599, 50) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "round-up valid");
        CHECK(ask == 1 && bid == 0, "round-up 299.5 counts");
    }
    { // below half stays out: 299.4 -> 299
        Tmv2_VapRow r[2] = { MkRow(0, 250, 5, 200), MkRow(1, 650, 598, 50) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "below-half valid");
        CHECK(ask == 0 && bid == 0, "below-half 299.4 no count");
    }
    { // bid side: -299.5 rounds away to -300 -> counts
        Tmv2_VapRow r[2] = { MkRow(0, 650, 5, 599), MkRow(1, 250, 200, 50) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "bid round valid");
        CHECK(ask == 0 && bid == 1, "bid round -299.5 counts");
    }
    { // bid below half: -299.4 -> -299 -> out
        Tmv2_VapRow r[2] = { MkRow(0, 650, 5, 598), MkRow(1, 250, 200, 50) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "bid below-half valid");
        CHECK(ask == 0 && bid == 0, "bid below-half no count");
    }
    { // sparse stored rows pair across the tick gap (consecutive indices)
        Tmv2_VapRow r[3] = { MkRow(0, 50, 5, 10), MkRow(5, 60, 40, 10),
                             MkRow(9, 50, 10, 40) };
        // (0,5): 40/10=400 ask; (5,9): bidLo=10 vs askUp=10 -> 100, no count
        CHECK(Tmv2_DiagCounts(r, 3, 300.0, 20.0, &ask, &bid) == 1, "sparse valid");
        CHECK(ask == 1 && bid == 0, "sparse gap pairs count");
    }
    { // sparse bid pair across gap
        Tmv2_VapRow r[2] = { MkRow(2, 60, 5, 40), MkRow(7, 60, 10, 20) };
        CHECK(Tmv2_DiagCounts(r, 2, 300.0, 20.0, &ask, &bid) == 1, "sparse bid valid");
        CHECK(ask == 0 && bid == 1, "sparse bid gap counts");
    }
}

// ---------- S10 VAP-vs-chart volume reconciliation ----------
static void S10_Completeness()
{
    Tmv2_VapRow r[3] = { MkRow(0, 20, 10, 10), MkRow(1, 30, 15, 15),
                         MkRow(2, 10, 5, 5) }; // sum 60
    CHECK(Tmv2_ProfileVolumeOk(r, 3, 30.0, 30.0) == 1, "volume exact match");
    CHECK(Tmv2_ProfileVolumeOk(r, 3, 20.0, 20.0) == 1, "volume excess allowed");
    CHECK(Tmv2_ProfileVolumeOk(r, 3, 40.0, 30.0) == 0, "volume shortfall partial");
    CHECK(Tmv2_ProfileVolumeOk(r, 3, 0.0, 0.0) == 0, "volume zero chart no signal");
    CHECK(Tmv2_ProfileVolumeOk(r, 3, std::acos(2.0), 30.0) == 0, "volume NaN flow invalid");
    CHECK(Tmv2_ProfileVolumeOk(r, 3, -5.0, 30.0) == 0, "volume negative flow invalid");
    // Partial profile fails the full offset fill even with good shape.
    {
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        CHECK(Tmv2_FillProfileOffset(&w, r, 3, 40.0, 30.0, 0, 0.25) == 0, "offset partial invalid");
        CHECK(w.vpaOk[0] == 0 && w.VpA[0] == 0.0, "offset partial cleared");
    }
    {
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        CHECK(Tmv2_FillProfileOffset(&w, r, 3, 30.0, 30.0, 0, 0.25) == 1, "offset complete valid");
        CHECK(w.vpaOk[0] == 1 && w.VpA[0] == 0.25, "offset poc tick1 price");
    }
}

// ---------- S11 guarded price-to-tick ----------
static void S11_SafeTick()
{
    int t = -99;
    CHECK(Tmv2_PriceToTickSafe(100.0, 0.25, &t) == 1 && t == 400, "safe basic");
    CHECK(Tmv2_PriceToTickSafe(100.125, 0.25, &t) == 1 && t == 401, "safe half up");
    CHECK(Tmv2_PriceToTickSafe(100.0, 0.0, &t) == 0, "safe zero tick");
    CHECK(Tmv2_PriceToTickSafe(std::acos(2.0), 0.25, &t) == 0, "safe NaN price");
    CHECK(Tmv2_PriceToTickSafe(1.0 / 0.0, 0.25, &t) == 0, "safe Inf price");
    CHECK(Tmv2_PriceToTickSafe(100.0, 0.25, 0) == 0, "safe null out");
    CHECK(Tmv2_PriceToTickSafe(1e18, 0.01, &t) == 0, "safe overflow rejected");
    CHECK(Tmv2_PriceToTickSafe(-1e18, 0.01, &t) == 0, "safe negative overflow rejected");
    // Boundary: largest exactly-representable tick with anchor margin.
    CHECK(Tmv2_PriceToTickSafe(2147483640.0 * 0.25, 0.25, &t) == 1 && t == 2147483640, "safe max tick");
    CHECK(Tmv2_PriceToTickSafe(2147483647.0 * 0.25, 0.25, &t) == 0, "safe over-margin rejected");
}

// ---------- S12 update planner (production-called) ----------
static void S12_Planner()
{
    int f = -1, lc = -1;
    CHECK(Tmv2_PlanUpdate(0, 0, 0, 0, -1, &f, &lc) == 0, "plan tiny0 no work");
    CHECK(Tmv2_PlanUpdate(1, 0, 0, 0, -1, &f, &lc) == 0, "plan tiny1 no work");
    CHECK(Tmv2_PlanUpdate(2, 0, 1, 0, -1, &f, &lc) == 1 && f == 0 && lc == 0, "plan two bars");
    CHECK(Tmv2_PlanUpdate(100, 90, 0, 0, -1, &f, &lc) == 1 && f == 58 && lc == 98, "plan overlap 32");
    CHECK(Tmv2_PlanUpdate(100, 5, 0, 0, -1, &f, &lc) == 1 && f == 0 && lc == 98, "plan clamp zero");
    CHECK(Tmv2_PlanUpdate(100, 90, 1, 0, -1, &f, &lc) == 1 && f == 0 && lc == 98, "plan full recalc");
    CHECK(Tmv2_PlanUpdate(100, 90, 0, 1, -1, &f, &lc) == 1 && f == 0 && lc == 98, "plan fingerprint");
    CHECK(Tmv2_PlanUpdate(100, 98, 0, 0, 40, &f, &lc) == 1 && f == 40 && lc == 98, "plan retry extends");
    CHECK(Tmv2_PlanUpdate(100, 98, 0, 0, 200, &f, &lc) == 1 && f == 66 && lc == 98, "plan stale retry ignored");
    CHECK(Tmv2_PlanUpdate(100, 98, 0, 0, -1, &f, &lc) == 1 && f == 66 && lc == 98, "plan no retry");
    // Closed-to-forming reclassification drives the forming-zero write:
    // ArraySize 50 -> lastClosed 48; ArraySize 51 -> lastClosed 49.
    { int f0 = -1, lc0 = -1, fN = -1, lcN = -1;
      Tmv2_PlanUpdate(50, 48, 0, 0, -1, &f0, &lc0);
      Tmv2_PlanUpdate(51, 49, 0, 0, -1, &fN, &lcN);
      CHECK(lc0 == 48 && lcN == 49, "plan new bar edge advances"); }
    CHECK(Tmv2_PlanUpdate(100, 90, 0, 0, -1, 0, &lc) == 0, "plan null first");
    CHECK(Tmv2_PlanUpdate(100, 90, 0, 0, -1, &f, 0) == 0, "plan null last");
}

// ---------- S13 native seam details ----------
static void S13_NativeSeam()
{
    double O[31], H[31], L[31], C[31], AV[31], BV[31], TV[31];
    // Lagged band exactness through FillNative: ramp deltas 1..31.
    {
        for (int i = 0; i < 31; i++)
        {
            O[i] = 100; H[i] = 101; L[i] = 99; C[i] = 100;
            AV[i] = 50.0 + (i + 1) / 2.0; BV[i] = 50.0 - (i + 1) / 2.0;
            TV[i] = 100.0;
        }
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        // k=3 -> end 27 -> window idx 8..27 -> deltas 9..28, mean 18.5
        CHECK(w.upOk[3] && w.loOk[3], "native lagged band valid");
        CHECK(std::fabs(w.Up[3] - (18.5 + 0.9 * std::sqrt(33.25))) < 1e-9, "native lagged upper");
        CHECK(std::fabs(w.Lo[3] - (18.5 - 0.9 * std::sqrt(33.25))) < 1e-9, "native lagged lower");
        CHECK(w.D[0] == 31.0 && w.dOk[0] == 1, "native delta current");
        CHECK(w.D[5] == 26.0 && w.dOk[5] == 1, "native delta lag5");
    }
    // Invalid-flow hole poisons only covering windows: hole at idx27 kills
    // ends 27..30 (k=0..3), spares ends 25,26 (k=5,4).
    {
        HistFill(O, H, L, C, AV, BV, TV, 31, 100.0, 101.0, 99.0, 100.0, 30.0, 30.0);
        AV[27] = -1.0;
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        CHECK(!w.upOk[0] && !w.upOk[1] && !w.upOk[2] && !w.upOk[3], "native hole kills k0-3");
        CHECK(w.upOk[4] && w.upOk[5], "native hole spares k4-5");
    }
    // Missing classified volume (chart total positive, AV+BV zero) is not
    // a valid zero delta: dOk drops and covering bands invalidate.
    {
        HistFill(O, H, L, C, AV, BV, TV, 31, 100.0, 101.0, 99.0, 100.0, 30.0, 30.0);
        AV[27] = 0.0; BV[27] = 0.0; TV[27] = 50.0;
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        CHECK(w.dOk[3] == 0, "native missing classified dOk 0");
        CHECK(!w.upOk[3] && !w.upOk[0], "native missing kills covering bands");
        CHECK(w.upOk[4] && w.upOk[5], "native missing spares distant bands");
    }
    // Genuinely empty bar (all zero incl. chart total) stays a valid zero.
    {
        HistFill(O, H, L, C, AV, BV, TV, 31, 100.0, 101.0, 99.0, 100.0, 30.0, 30.0);
        AV[27] = 0.0; BV[27] = 0.0; TV[27] = 0.0;
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, TV, 1, 31, 30, 0.25);
        CHECK(w.dOk[3] == 1 && w.D[3] == 0.0, "native empty stays valid zero");
        CHECK(w.upOk[3] == 1, "native empty keeps bands");
    }
    // Unknown chart totals (hasVol=0) fall back to flow validity.
    {
        HistFill(O, H, L, C, AV, BV, TV, 31, 100.0, 101.0, 99.0, 100.0, 0.0, 0.0);
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, O, H, L, C, AV, BV, 0, 0, 31, 30, 0.25);
        CHECK(w.dOk[0] == 1, "native novol fallback valid");
    }
    // Short history: bands fail closed, available bars still fill.
    {
        double o2[3] = {100, 100, 100}, h2[3] = {101, 101, 101};
        double l2[3] = {99, 99, 99}, c2[3] = {100, 100, 100};
        double av2[3] = {30, 30, 30}, bv2[3] = {30, 30, 30}, tv2[3] = {60, 60, 60};
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        Tmv2_FillNative(&w, o2, h2, l2, c2, av2, bv2, tv2, 1, 3, 2, 0.25);
        CHECK(w.dOk[0] == 1 && w.dOk[2] == 1, "native short bars fill");
        CHECK(!w.upOk[0], "native short bands closed");
    }
    // Profile offsets: k out of range, tick-size gate, VpB/Vp6 depth.
    {
        Tmv2_VapRow r[3] = { MkRow(400, 20, 10, 10), MkRow(401, 30, 15, 15),
                             MkRow(402, 20, 10, 10) };
        Tmv2Window w;
        Tmv2_ClearWindow(&w);
        CHECK(Tmv2_FillProfileOffset(&w, r, 3, 25.0, 25.0, 5, 0.25) == 0, "offset k5 rejected");
        CHECK(Tmv2_FillProfileOffset(&w, r, 3, 25.0, 25.0, -1, 0.25) == 0, "offset kneg rejected");
        CHECK(Tmv2_FillProfileOffset(&w, r, 3, 25.0, 25.0, 0, 0.0) == 0, "offset bad tick rejected");
        CHECK(Tmv2_FillProfileOffset(&w, r, 3, 25.0, 25.0, 1, 0.25) == 1, "offset k1 fills");
        CHECK(w.VpA[1] == 100.25 && w.vpaOk[1], "offset k1 poc");
        CHECK(w.VpA[0] == 0.0 && !w.vpaOk[0], "offset k1 spares k0");
        CHECK(w.VpB[1] == 100.25 && w.VAH[1] > 0.0 && w.Vp6[1] == 100.25, "offset k1 depth");
        CHECK(w.AskD[1] == 0.0 && w.askOk[1], "offset k1 diag zero valid");
    }
    // Family-5 sides through the seam: gate, values, partial failure.
    {
        Tmv2_VapRow r[5];
        for (int k = 0; k < 5; k++) r[k] = MkRow(400 + k, 30.0, 5.0 + k, 25.0 - k);
        Tmv2Window b, s;
        Tmv2_ClearWindow(&b); Tmv2_ClearWindow(&s);
        CHECK(Tmv2_FillVapSides(&b, &s, r, 5, 40.0, 40.0, 0, 400, 404) == 0, "sides gate off");
        CHECK(b.vapOk[0] == 0, "sides gate leaves cleared");
        Tmv2_ClearWindow(&b); Tmv2_ClearWindow(&s);
        CHECK(Tmv2_FillVapSides(&b, &s, r, 5, 40.0, 40.0, 1, 400, 404) == 1, "sides gate on");
        CHECK(b.vapSide[0] == 5.0 && b.vapSide[4] == 9.0, "sides bull asks");
        CHECK(s.vapSide[0] == 21.0 && s.vapSide[4] == 25.0, "sides bear bids");
        CHECK(b.vapTot[2] == 30.0, "sides totals");
        Tmv2_ClearWindow(&b); Tmv2_ClearWindow(&s);
        CHECK(Tmv2_FillVapSides(&b, &s, r, 5, 100.0, 100.0, 1, 400, 404) == 0, "sides partial");
    }
}

int main()
{
    S01_Params();
    S02_Bands();
    S03_Poc();
    S04_ValueArea();
    S05_Diagonals();
    S06_SideRows();
    S07_Integration();
    S08_Round();
    S09_DiagRound();
    S10_Completeness();
    S11_SafeTick();
    S12_Planner();
    S13_NativeSeam();
    if (g_fails == 0)
        std::printf("TMV2 SELFCONTAINED GREEN: %d checks\n", g_checks);
    else
        std::printf("TMV2 SELFCONTAINED RED: %d fails / %d checks\n", g_fails, g_checks);
    return g_fails ? 1 : 0;
}
