// Portable tests for TriggerMatrixV2.cpp (pure core behind TMV2_UNITTEST).
// No Sierra headers, no external packages.
// TDD Task 1: written BEFORE TriggerMatrixV2.cpp exists -> RED (compile fail).
#define TMV2_UNITTEST 1
#include "../TriggerMatrixV2.cpp"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; std::printf("FAIL %d: %s\n", __LINE__, msg); } \
} while (0)

static void WinInit(Tmv2Window& w, double tick = 0.25)
{
    Tmv2_ClearWindow(&w);
    w.tick = tick;
}

static void SetBar(Tmv2Window& w, int k, double o, double h, double l,
                   double c, double av, double bv)
{
    w.O[k] = o; w.H[k] = h; w.L[k] = l; w.C[k] = c;
    w.AV[k] = av; w.BV[k] = bv;
}

static void SetD(Tmv2Window& w, int k, double d, double up, double lo)
{
    w.D[k] = d; w.dOk[k] = 1;
    w.Up[k] = up; w.upOk[k] = 1;
    w.Lo[k] = lo; w.loOk[k] = 1;
}

// Mirror a bull-true window into a bear-true window around pivot P.
// Prices x -> 2P-x; AV<->BV; D sign flips with Up<->Lo swap.
static void MirrorWindow(const Tmv2Window& src, Tmv2Window& dst, double piv = 100.0)
{
    Tmv2_ClearWindow(&dst);
    dst.tick = src.tick;
    const double M = 2.0 * piv;
    for (int k = 0; k < 6; k++)
    {
        dst.O[k] = M - src.O[k];
        dst.H[k] = M - src.L[k];
        dst.L[k] = M - src.H[k];
        dst.C[k] = M - src.C[k];
        dst.AV[k] = src.BV[k];
        dst.BV[k] = src.AV[k];
        dst.D[k] = -src.D[k];   dst.dOk[k] = src.dOk[k];
        dst.Up[k] = -src.Lo[k]; dst.upOk[k] = src.loOk[k];
        dst.Lo[k] = -src.Up[k]; dst.loOk[k] = src.upOk[k];
        dst.VpA[k] = M - src.VpA[k]; dst.vpaOk[k] = src.vpaOk[k];
        dst.VpB[k] = M - src.VpB[k]; dst.vpbOk[k] = src.vpbOk[k];
        dst.VAH[k] = M - src.VAL[k]; dst.vahOk[k] = src.valOk[k];
        dst.VAL[k] = M - src.VAH[k]; dst.valOk[k] = src.vahOk[k];
    }
    for (int k = 0; k < 2; k++)
    {
        dst.Vp6[k] = M - src.Vp6[k]; dst.vp6Ok[k] = src.vp6Ok[k];
        dst.Vp8[k] = M - src.Vp8[k]; dst.vp8Ok[k] = src.vp8Ok[k];
        dst.AskD[k] = src.BidD[k]; dst.askOk[k] = src.bidOk[k];
        dst.BidD[k] = src.AskD[k]; dst.bidOk[k] = src.askOk[k];
    }
    dst.vapMultIs1 = src.vapMultIs1;
    for (int k = 0; k < 5; k++)
    {
        dst.vapTot[k] = src.vapTot[k];
        dst.vapSide[k] = src.vapSide[k];
        dst.vapOk[k] = src.vapOk[k];
    }
}

// Flow fixture: totals of 100 with normalized imbalance n (exact dyadic
// steps of 1/32 where boundary precision matters).
static void SetNorm(Tmv2Window& w, int k, double n, double total = 100.0)
{
    double d = n * total;
    w.AV[k] = (total + d) / 2.0;
    w.BV[k] = (total - d) / 2.0;
}

// ---------- T01 static catalog ledger ----------
static void T01_Ledger()
{
    CHECK(Tmv2_ReadyFamilyCount() == 16, "16 ready families");
    CHECK(Tmv2_OutputCount() == TMV2_N_OUT, "38 outputs");
    const int* ords = Tmv2_ReadyOrdinals();
    int seen[20] = {0};
    for (int i = 0; i < 16; i++)
    {
        CHECK(ords[i] >= 1 && ords[i] <= 19, "ordinal in range");
        seen[ords[i]]++;
        int sg = Tmv2_FamilySgBull(ords[i]);
        CHECK(sg >= 0 && sg <= 30 && sg % 2 == 0, "even bull SG 0..30");
        CHECK(Tmv2_FamilySgBear(ords[i]) == sg + 1, "bear SG = bull+1");
        CHECK(Tmv2_FamilySgBull(ords[i]) <= 59, "SG within 60-study limit");
    }
    for (int o = 1; o <= 19; o++)
    {
        if (o == 3 || o == 12 || o == 13)
        {
            CHECK(seen[o] == 0, "blocked family absent");
            CHECK(Tmv2_IsBlocked(o) == 1, "blocked flag");
            CHECK(Tmv2_FamilySgBull(o) < 0, "blocked has no SG");
        }
        else
        {
            CHECK(seen[o] == 1, "ready family present once");
            CHECK(Tmv2_IsBlocked(o) == 0, "ready not blocked");
        }
    }
    // SG uniqueness across all 16 pairs
    int used[32] = {0};
    for (int i = 0; i < 16; i++)
    {
        int sb = Tmv2_FamilySgBull(ords[i]);
        int sr = Tmv2_FamilySgBear(ords[i]);
        CHECK(sb >= 0 && sr == sb + 1, "pair adjacency");
        if (sb >= 0) { used[sb]++; used[sr]++; }
    }
    for (int s = 0; s < 32; s++) CHECK(used[s] == 1, "SG0..31 unique");
    CHECK(Tmv2_FamilyRole(20) == 2 && Tmv2_FamilySgBull(20) == 32, "LTR SG32 Renko8");
    CHECK(Tmv2_FamilyRole(21) == 2 && Tmv2_FamilySgBull(21) == 34, "TBY SG34 Renko8");
    CHECK(Tmv2_FamilyRole(22) == 2 && Tmv2_FamilySgBull(22) == 36, "RBY SG36 Renko8");
    CHECK(std::string(Tmv2_FamilyCode(20)) == "LTR", "code LTR");
    CHECK(std::string(Tmv2_FamilyCode(21)) == "TBY", "code TBY");
    CHECK(std::string(Tmv2_FamilyCode(22)) == "RBY", "code RBY");
    CHECK(Tmv2_MaxDirectFootprint(20) == 3, "fp LTR");
    CHECK(Tmv2_MaxDirectFootprint(21) == 6, "fp TBY");
    CHECK(Tmv2_MaxDirectFootprint(22) == 3, "fp RBY");
    // Pins
    CHECK(std::string(Tmv2_CatalogVersion()) == "2.0.0-research-2026-09-09", "catalog version pin");
    CHECK(std::string(Tmv2_CatalogSha()) == "ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618", "catalog sha pin");
}

// ---------- T02 role map ----------
static void T02_Roles()
{
    const int rangeFam[11] = {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 19};
    for (int i = 0; i < 11; i++) CHECK(Tmv2_FamilyRole(rangeFam[i]) == 0, "range role");
    CHECK(Tmv2_FamilyRole(14) == 1, "r6 fam14");
    CHECK(Tmv2_FamilyRole(15) == 1, "r6 fam15");
    CHECK(Tmv2_FamilyRole(16) == 2, "r8 fam16");
    CHECK(Tmv2_FamilyRole(17) == 2, "r8 fam17");
    CHECK(Tmv2_FamilyRole(18) == 2, "r8 fam18");
    CHECK(Tmv2_FamilyRole(3) < 0 && Tmv2_FamilyRole(12) < 0 && Tmv2_FamilyRole(13) < 0, "blocked have no role");
    // Footprints (max direct bars)
    CHECK(Tmv2_MaxDirectFootprint(1) == 4, "fp fam1");
    CHECK(Tmv2_MaxDirectFootprint(5) == 1, "fp fam5 single-bar");
    CHECK(Tmv2_MaxDirectFootprint(11) == 5, "fp fam11");
    CHECK(Tmv2_MaxDirectFootprint(17) == 6, "fp fam17");
}

// ---------- T03 flow primitives ----------
static void T03_Flow()
{
    CHECK(Tmv2_FlowValid(10, 10) == 1, "valid flow");
    CHECK(Tmv2_FlowValid(0, 0) == 1, "zero flow structurally valid (veto handles)");
    CHECK(Tmv2_FlowValid(-1, 10) == 0, "negative AV invalid");
    CHECK(Tmv2_FlowValid(10, -1) == 0, "negative BV invalid");
    double nan = std::acos(2.0);
    double inf = 1.0 / 0.0;
    CHECK(Tmv2_FlowValid(nan, 10) == 0, "NaN invalid");
    CHECK(Tmv2_FlowValid(10, inf) == 0, "Inf invalid");
    // norm denominator guard: total 0 -> delta/max(1,0)=delta
    CHECK(std::fabs(Tmv2_Norm(0, 0) - 0.0) < 1e-12, "norm zero-zero");
    CHECK(std::fabs(Tmv2_Norm(15, 5) - 0.5) < 1e-12, "norm 0.5");
    // 20-contract veto: totals 19/20 boundary on fam1 (needs 4 bars)
    {
        Tmv2Window w; WinInit(w);
        for (int k = 0; k < 4; k++) { SetBar(w, k, 100, 101, 99, 100, 5, 15); } // n=-0.5 each... slope 0
        // slope 0 fails; build proper decay instead via SetNorm below in T04.
        (void)w;
    }
    CHECK(Tmv2_FlowValid(20, 0) == 1, "flow valid at veto edge");
}

// ---------- T04 family 1: Opposing Effort Decay ----------
static void BuildF01Bull(Tmv2Window& w)
{
    WinInit(w);
    // n3=-0.5 n2=-0.375 n1=-0.25 n0=-0.125 (dyadic exact, total 32):
    // slope=(1.5+0.375-0.25-0.375)/10=1.25/10=0.125>=0.05; decay=0.375>=0.20
    SetNorm(w, 3, -0.5, 32.0);
    SetNorm(w, 2, -0.375, 32.0);
    SetNorm(w, 1, -0.25, 32.0);
    SetNorm(w, 0, -0.125, 32.0);
    SetBar(w, 3, 100, 101, 99, 100.0, w.AV[3], w.BV[3]);
    SetBar(w, 2, 100, 101, 99, 100.0, w.AV[2], w.BV[2]);
    SetBar(w, 1, 100, 101, 99, 100.0, w.AV[1], w.BV[1]);
    SetBar(w, 0, 100, 101, 99, 100.0, w.AV[0], w.BV[0]);
    // SetBar overwrote AV/BV ordering? re-assert norms (SetBar takes av/bv):
    SetNorm(w, 3, -0.5, 32.0); SetNorm(w, 2, -0.375, 32.0);
    SetNorm(w, 1, -0.25, 32.0); SetNorm(w, 0, -0.125, 32.0);
}

static void T04_Fam1()
{
    Tmv2Window w; BuildF01Bull(w);
    CHECK(Tmv2_F01Bull(&w) == 1, "fam1 bull true");
    CHECK(Tmv2_F01Bear(&w) == 0, "fam1 bear false on bull fixture");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F01Bear(&m) == 1, "fam1 bear mirror true");
    CHECK(Tmv2_F01Bull(&m) == 0, "fam1 bull false on bear fixture");
    // one-clause-false: slope (flatten series -> slope 0)
    { Tmv2Window v = w; SetNorm(v, 0, -0.5, 32.0); SetNorm(v, 1, -0.5, 32.0); SetNorm(v, 2, -0.5, 32.0); CHECK(Tmv2_F01Bull(&v) == 0, "fam1 flat slope false"); }
    // one-clause-false: decay too small (n0-n3=0.0625)
    { Tmv2Window v = w; SetNorm(v, 0, -0.4375, 32.0); CHECK(Tmv2_F01Bull(&v) == 0, "fam1 small decay false"); }
    // one-clause-false: wrong sign bar
    { Tmv2Window v = w; SetNorm(v, 1, 0.25, 32.0); CHECK(Tmv2_F01Bull(&v) == 0, "fam1 wrong-sign false"); }
    // veto: one bar total 19
    { Tmv2Window v = w; v.AV[2] = 9.0; v.BV[2] = 10.0; CHECK(Tmv2_F01Bull(&v) == 0, "fam1 veto 19 false"); }
    // veto edge: all totals exactly 20 with same shape -> true
    { Tmv2Window v = w; for (int k = 0; k < 4; k++) { double n = (k == 3) ? -0.5 : (k == 2) ? -0.375 : (k == 1) ? -0.25 : -0.125; SetNorm(v, k, n, 20.0); } CHECK(Tmv2_F01Bull(&v) == 1, "fam1 totals==20 true"); }
    // adverse close cap: C0 == C3-2t true; C3-2t-eps false
    { Tmv2Window v = w; v.C[0] = v.C[3] - 2 * v.tick; CHECK(Tmv2_F01Bull(&v) == 1, "fam1 close cap edge true"); }
    { Tmv2Window v = w; v.C[0] = v.C[3] - 2 * v.tick - 0.01; CHECK(Tmv2_F01Bull(&v) == 0, "fam1 close cap breach false"); }
    // all-false zero window
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F01Bull(&z) == 0 && Tmv2_F01Bear(&z) == 0, "fam1 zero false"); }
    // every-true-bar: same window evaluated twice stays true (no transition)
    CHECK(Tmv2_F01Bull(&w) == 1 && Tmv2_F01Bull(&w) == 1, "fam1 every-true-bar");
    // tick<=0 fail-closed
    { Tmv2Window v = w; v.tick = 0.0; CHECK(Tmv2_F01Bull(&v) == 0, "fam1 tick0 false"); }
}

// ---------- T05 family 2: Directional Effort Slope Response ----------
static void BuildF02Bull(Tmv2Window& w)
{
    WinInit(w);
    SetNorm(w, 3, 0.0, 200.0);
    SetNorm(w, 2, 0.1, 200.0);
    SetNorm(w, 1, 0.2, 200.0);
    SetNorm(w, 0, 0.35, 200.0);
    // slope=(-0-0.1+0.2+1.05)/10=0.115; impulse=0.35; steps all aligned
    SetBar(w, 3, 100, 101, 99, 100.0, w.AV[3], w.BV[3]);
    SetBar(w, 2, 100, 101, 99, 100.5, w.AV[2], w.BV[2]);
    SetBar(w, 1, 100, 101, 99, 100.75, w.AV[1], w.BV[1]);
    SetBar(w, 0, 100, 101, 99, 101.0, w.AV[0], w.BV[0]);
    SetNorm(w, 3, 0.0, 200.0); SetNorm(w, 2, 0.1, 200.0);
    SetNorm(w, 1, 0.2, 200.0); SetNorm(w, 0, 0.35, 200.0);
}

static void T05_Fam2()
{
    Tmv2Window w; BuildF02Bull(w);
    CHECK(Tmv2_F02Bull(&w) == 1, "fam2 bull true");
    CHECK(Tmv2_F02Bear(&w) == 0, "fam2 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F02Bear(&m) == 1, "fam2 bear mirror true");
    // branch 2 only: n2>n3, n0>n1, n1<=n2
    { Tmv2Window v = w; SetNorm(v, 1, 0.1, 200.0); SetNorm(v, 0, 0.35, 200.0); CHECK(Tmv2_F02Bull(&v) == 1, "fam2 branch2 true"); }
    // branch 3 only: n1>n2, n0>n1, n2<=n3 (keep impulse/slope passing)
    { Tmv2Window v = w; SetNorm(v, 3, 0.3, 200.0); SetNorm(v, 2, 0.1, 200.0); SetNorm(v, 1, 0.2, 200.0); SetNorm(v, 0, 0.55, 200.0); CHECK(Tmv2_F02Bull(&v) == 1, "fam2 branch3 true"); }
    // no aligned pair -> false
    { Tmv2Window v = w; SetNorm(v, 3, 0.3, 200.0); SetNorm(v, 2, 0.2, 200.0); SetNorm(v, 1, 0.1, 200.0); SetNorm(v, 0, 0.35, 200.0); CHECK(Tmv2_F02Bull(&v) == 0, "fam2 monotone-down false"); }
    // terminal threshold edge: n0==0.10 true
    { Tmv2Window v = w; SetNorm(v, 3, -0.15, 200.0); SetNorm(v, 2, -0.05, 200.0); SetNorm(v, 1, 0.05, 200.0); SetNorm(v, 0, 0.10, 200.0); CHECK(Tmv2_F02Bull(&v) == 1, "fam2 terminal edge true"); }
    // displacement edge
    { Tmv2Window v = w; v.C[0] = v.C[3] + 2 * v.tick; CHECK(Tmv2_F02Bull(&v) == 1, "fam2 displacement edge true"); }
    { Tmv2Window v = w; v.C[0] = v.C[3] + 2 * v.tick - 0.01; CHECK(Tmv2_F02Bull(&v) == 0, "fam2 displacement short false"); }
    // close location: C == H-0.25(H-L) true
    { Tmv2Window v = w; v.C[0] = v.H[0] - 0.25 * (v.H[0] - v.L[0]); CHECK(Tmv2_F02Bull(&v) == 1, "fam2 close-loc edge true"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F02Bull(&z) == 0 && Tmv2_F02Bear(&z) == 0, "fam2 zero false"); }
}

// ---------- T06 family 4: Extreme Effort Failure ----------
static void BuildF04Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 102, 99, 101, 20, 20);   // t=40
    SetBar(w, 1, 100, 102, 100, 101, 20, 20);  // t=40, L1=100
    SetBar(w, 0, 100, 101.5, 99.5, 101, 20, 30); // total 50; 30>=1.5*20; 50>=1.25*40
    // L0=99.5<=100-0.25; C0=101>=100.25; C>=101.5-0.5=101 edge
}

static void T06_Fam4()
{
    Tmv2Window w; BuildF04Bull(w);
    CHECK(Tmv2_F04Bull(&w) == 1, "fam4 bull true");
    CHECK(Tmv2_F04Bear(&w) == 0, "fam4 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F04Bear(&m) == 1, "fam4 bear mirror true");
    { Tmv2Window v = w; v.BV[0] = 29.0; CHECK(Tmv2_F04Bull(&v) == 0, "fam4 ratio 29 false"); }
    { Tmv2Window v = w; v.AV[1] = 21; v.BV[1] = 20; CHECK(Tmv2_F04Bull(&v) == 0, "fam4 activity false"); } // total 50 < 1.25*41=51.25
    { Tmv2Window v = w; v.L[0] = v.L[1]; CHECK(Tmv2_F04Bull(&v) == 0, "fam4 no probe false"); }
    { Tmv2Window v = w; v.C[0] = v.L[1]; CHECK(Tmv2_F04Bull(&v) == 0, "fam4 no reclaim false"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F04Bull(&z) == 0 && Tmv2_F04Bear(&z) == 0, "fam4 zero false"); }
}

// ---------- T07 family 5: VAP Gradient Divergence (direct) ----------
static void BuildF05Bull(Tmv2Window& w)
{
    WinInit(w); // tick 0.25
    SetBar(w, 0, 100.5, 101.5, 100.0, 101.25, 10, 40); // total 50; -30 <= -5
    w.vapMultIs1 = 1;
    double side[5] = {5, 8, 12, 16, 22}; // sum 63; 22>=2*5+10=20
    for (int k = 0; k < 5; k++) { w.vapTot[k] = 30.0; w.vapSide[k] = side[k]; w.vapOk[k] = 1; }
}

static void T07_Fam5()
{
    Tmv2Window w; BuildF05Bull(w);
    CHECK(Tmv2_F05Bull(&w) == 1, "fam5 bull true");
    CHECK(Tmv2_F05Bear(&w) == 0, "fam5 bear false on bull");
    // manual bear (VAP side semantics differ, no generic mirror)
    { Tmv2Window b; WinInit(b);
      SetBar(b, 0, 199.5, 200.0, 198.5, 198.75, 40, 10); // +30 >= +5; C<O
      b.vapMultIs1 = 1;
      double side[5] = {5, 8, 12, 16, 22};
      for (int k = 0; k < 5; k++) { b.vapTot[k] = 30.0; b.vapSide[k] = side[k]; b.vapOk[k] = 1; }
      CHECK(Tmv2_F05Bear(&b) == 1, "fam5 bear true");
      CHECK(Tmv2_F05Bull(&b) == 0, "fam5 bull false on bear"); }
    CHECK(Tmv2_F05Bull(&w) == 1, "fam5 retrue");
    { Tmv2Window v = w; v.vapMultIs1 = 0; CHECK(Tmv2_F05Bull(&v) == 0, "fam5 multiplier gate"); }
    { Tmv2Window v = w; v.vapOk[2] = 0; CHECK(Tmv2_F05Bull(&v) == 0, "fam5 missing row"); }
    { Tmv2Window v = w; v.vapTot[4] = 0.0; CHECK(Tmv2_F05Bull(&v) == 0, "fam5 zero total row"); }
    // amplitude edge: side4 == 2*side0+10
    { Tmv2Window v = w; v.vapSide[4] = 2 * v.vapSide[0] + 10; CHECK(Tmv2_F05Bull(&v) == 1, "fam5 amplitude edge true"); }
    { Tmv2Window v = w; v.vapSide[4] = 2 * v.vapSide[0] + 9; CHECK(Tmv2_F05Bull(&v) == 0, "fam5 amplitude short false"); }
    // side-sum edge: exactly 20 but amplitude short -> false
    { Tmv2Window v = w; double s[5] = {1, 2, 3, 4, 10}; for (int k = 0; k < 5; k++) v.vapSide[k] = s[k]; CHECK(Tmv2_F05Bull(&v) == 0, "fam5 sum20 amplitude short false"); }
    // strict steps: tie two different steps so no 3-of-4 combo survives
    { Tmv2Window v = w; v.vapSide[0] = 8; v.vapSide[1] = 8; v.vapSide[2] = 16; v.vapSide[4] = 26; CHECK(Tmv2_F05Bull(&v) == 0, "fam5 tied steps false"); }
    // 3-of-4 proof: break only step0 (tie a0/a1), keep amplitude -> still true
    { Tmv2Window v = w; v.vapSide[0] = 8; v.vapSide[1] = 8; v.vapSide[4] = 26; CHECK(Tmv2_F05Bull(&v) == 1, "fam5 three-of-four true"); }
    // imbalance edge: (AV-BV) == -0.10*total
    { Tmv2Window v = w; v.AV[0] = 22.5; v.BV[0] = 27.5; CHECK(Tmv2_F05Bull(&v) == 1, "fam5 imbalance edge true"); } // -5 <= -5
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F05Bull(&z) == 0 && Tmv2_F05Bear(&z) == 0, "fam5 zero false"); }
}

// ---------- T08 family 6: Whole-Distribution Migration ----------
static void BuildF06Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 1, 100, 102, 99, 101, 20, 20);
    SetBar(w, 0, 101, 103, 100, 102.5, 20, 20);
    w.VpA[1] = 100; w.vpaOk[1] = 1; w.VpA[0] = 101; w.vpaOk[0] = 1;
    w.VAH[1] = 101; w.vahOk[1] = 1; w.VAL[1] = 99; w.valOk[1] = 1;
    w.VAH[0] = 102; w.vahOk[0] = 1; w.VAL[0] = 100; w.valOk[0] = 1;
}

static void T08_Fam6()
{
    Tmv2Window w; BuildF06Bull(w);
    CHECK(Tmv2_F06Bull(&w) == 1, "fam6 bull true");
    CHECK(Tmv2_F06Bear(&w) == 0, "fam6 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F06Bear(&m) == 1, "fam6 bear mirror true");
    { Tmv2Window v = w; v.VpA[0] = v.VpA[1]; CHECK(Tmv2_F06Bull(&v) == 0, "fam6 no vpoc migration"); }
    { Tmv2Window v = w; v.vpaOk[0] = 0; CHECK(Tmv2_F06Bull(&v) == 0, "fam6 missing vpoc"); }
    { Tmv2Window v = w; v.VAH[0] = v.VAL[0] - 1; CHECK(Tmv2_F06Bull(&v) == 0, "fam6 unordered VA"); }
    { Tmv2Window v = w; v.C[0] = v.VAH[0] - 0.01; CHECK(Tmv2_F06Bull(&v) == 0, "fam6 close inside VA"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F06Bull(&z) == 0 && Tmv2_F06Bear(&z) == 0, "fam6 zero false"); }
}

// ---------- T09 family 7: Delta Reversal Profile Response ----------
static void BuildF07Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 102, 99, 100, 30, 70);   // n2=-0.4
    SetBar(w, 1, 100, 101, 99.5, 100.5, 40, 60); // C1>O1
    SetBar(w, 0, 100.5, 101.5, 100, 101.25, 80, 20); // n0=0.6; C0>O0
    SetD(w, 2, -50, 45, -35); SetD(w, 1, -10, 50, -45);
    // D2=-50 <= Lo3=-40 (prior-frozen); D1<0; D0=60>=Up1=50
    w.D[0] = 60; w.dOk[0] = 1; w.Up[1] = 50; w.upOk[1] = 1; w.Lo[3] = -40; w.loOk[3] = 1;
    w.Lo[2] = -35; w.loOk[2] = 1; w.Up[0] = 55; w.upOk[0] = 1;
    w.VpB[1] = 100; w.vpbOk[1] = 1; w.VpB[0] = 101; w.vpbOk[0] = 1;
}

static void T09_Fam7()
{
    Tmv2Window w; BuildF07Bull(w);
    CHECK(Tmv2_F07Bull(&w) == 1, "fam7 bull true");
    CHECK(Tmv2_F07Bear(&w) == 0, "fam7 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F07Bear(&m) == 1, "fam7 bear mirror true");
    // lag fidelity: seed passes only via Lo[-3]; make Lo[-3] fail while
    // Lo[-2] would pass -> must stay false (no contemporaneous substitution)
    { Tmv2Window v = w; v.Lo[3] = -60; CHECK(Tmv2_F07Bull(&v) == 0, "fam7 frozen-lag enforced"); }
    // reversal amplitude edge: n0-n2 == 0.60
    { Tmv2Window v = w; SetNorm(v, 0, 0.2, 100.0); CHECK(Tmv2_F07Bull(&v) == 1, "fam7 reversal edge true"); } // 0.2-(-0.4)=0.6
    { Tmv2Window v = w; v.VpB[0] = v.VpB[1]; CHECK(Tmv2_F07Bull(&v) == 0, "fam7 no vpoc migration"); }
    { Tmv2Window v = w; v.dOk[2] = 0; CHECK(Tmv2_F07Bull(&v) == 0, "fam7 missing delta"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F07Bull(&z) == 0 && Tmv2_F07Bear(&z) == 0, "fam7 zero false"); }
}

// ---------- T10 family 8: POC Delta ----------
static void BuildF08Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 1, 100, 101, 99, 99, 30, 30);     // C1<O1
    SetBar(w, 0, 99.5, 100.75, 99.75, 100.5, 40, 20); // O<100, C>=100
    w.VpB[1] = 100; w.vpbOk[1] = 1; w.VpB[0] = 100.5; w.vpbOk[0] = 1;
    w.D[0] = 50; w.dOk[0] = 1; w.Up[1] = 40; w.upOk[1] = 1;
}

static void T10_Fam8()
{
    Tmv2Window w; BuildF08Bull(w);
    CHECK(Tmv2_F08Bull(&w) == 1, "fam8 bull true");
    CHECK(Tmv2_F08Bear(&w) == 0, "fam8 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F08Bear(&m) == 1, "fam8 bear mirror true");
    // frozen-prior reclaim: C must reach VpB[-1]; O must start below
    { Tmv2Window v = w; v.C[0] = v.VpB[1] - 0.01; CHECK(Tmv2_F08Bull(&v) == 0, "fam8 no reclaim"); }
    { Tmv2Window v = w; v.O[0] = v.VpB[1]; CHECK(Tmv2_F08Bull(&v) == 0, "fam8 open not below"); }
    // prior-frozen band: D0 vs Up[-1]; Up[-2] must not substitute
    { Tmv2Window v = w; v.Up[1] = 60; v.Up[2] = 40; v.upOk[2] = 1; CHECK(Tmv2_F08Bull(&v) == 0, "fam8 band lag enforced"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F08Bull(&z) == 0 && Tmv2_F08Bear(&z) == 0, "fam8 zero false"); }
}

// ---------- T11 family 9: Frozen VPOC Reclaim Core (primary only) ----------
static void BuildF09Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 102, 99, 100, 10, 10);
    SetBar(w, 1, 100, 102, 99, 100, 10, 10);
    SetBar(w, 0, 100, 101.5, 99.5, 101, 20, 30);
    w.VpB[1] = 100; w.vpbOk[1] = 1; w.VpB[0] = 100.5; w.vpbOk[0] = 1;
}

static void T11_Fam9()
{
    Tmv2Window w; BuildF09Bull(w);
    CHECK(Tmv2_F09Bull(&w) == 1, "fam9 bull true");
    CHECK(Tmv2_F09Bear(&w) == 0, "fam9 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F09Bear(&m) == 1, "fam9 bear mirror true");
    { Tmv2Window v = w; v.L[0] = v.VpB[1]; CHECK(Tmv2_F09Bull(&v) == 0, "fam9 no probe"); }
    { Tmv2Window v = w; v.C[0] = v.VpB[1]; CHECK(Tmv2_F09Bull(&v) == 0, "fam9 no reclaim"); }
    { Tmv2Window v = w; v.VpB[0] = v.VpB[1] - 0.25; CHECK(Tmv2_F09Bull(&v) == 0, "fam9 adverse vpoc"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F09Bull(&z) == 0 && Tmv2_F09Bear(&z) == 0, "fam9 zero false"); }
    // no context inputs exist in the portable seam (excluded by design)
}

// ---------- T12 family 10: Effort Reversal with Value Migration ----------
static void BuildF10Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 101, 99, 99, 40, 60);    // C2<O2
    SetBar(w, 1, 99.5, 101, 99, 100.5, 35, 25); // C1>O1, D1=10
    SetBar(w, 0, 100.5, 102, 100, 101.5, 40, 20); // C0>O0, D0=20
    SetD(w, 2, -20, 25, -15); SetD(w, 1, 15, 20, -10);
    w.D[0] = 20; w.dOk[0] = 1; w.Lo[3] = -15; w.loOk[3] = 1; // D2<0, -20<=-15
    w.VpA[1] = 100; w.vpaOk[1] = 1; w.VpA[0] = 101; w.vpaOk[0] = 1;
    w.VAH[1] = 101; w.vahOk[1] = 1; w.VAL[1] = 99; w.valOk[1] = 1;
    w.VAH[0] = 102; w.vahOk[0] = 1; w.VAL[0] = 100; w.valOk[0] = 1;
    // recovery (15+20)=35 >= 1.25*20=25; C1=100.5<=H2=101; C0=101.5>=101.25
}

static void T12_Fam10()
{
    Tmv2Window w; BuildF10Bull(w);
    CHECK(Tmv2_F10Bull(&w) == 1, "fam10 bull true");
    CHECK(Tmv2_F10Bear(&w) == 0, "fam10 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F10Bear(&m) == 1, "fam10 bear mirror true");
    // recovery ratio edge: (D1+D0) == 1.25*|D2|
    { Tmv2Window v = w; v.D[0] = 10; CHECK(Tmv2_F10Bull(&v) == 1, "fam10 recovery edge true"); } // 25>=25
    { Tmv2Window v = w; v.D[0] = 9; CHECK(Tmv2_F10Bull(&v) == 0, "fam10 recovery short false"); } // 24<25
    // seed-extreme cross: C0 == H2+tick edge
    { Tmv2Window v = w; v.C[0] = v.H[2] + v.tick; CHECK(Tmv2_F10Bull(&v) == 1, "fam10 cross edge true"); }
    { Tmv2Window v = w; v.C[0] = v.H[2]; CHECK(Tmv2_F10Bull(&v) == 0, "fam10 no cross false"); }
    { Tmv2Window v = w; v.C[1] = v.H[2] + 1; CHECK(Tmv2_F10Bull(&v) == 0, "fam10 C1 above extreme false"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F10Bull(&z) == 0 && Tmv2_F10Bear(&z) == 0, "fam10 zero false"); }
}

// ---------- T13 family 11: Fixed POC Balance Migration ----------
static void BuildF11Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 101, 99, 99.5, 20, 20);  // C2<O2
    SetBar(w, 1, 99.5, 100.5, 99, 100, 30, 20); // C1>O1, D1=10
    SetBar(w, 0, 100, 101.5, 99.5, 101, 35, 20); // C0>O0, D0=15
    w.VpA[4] = 100; w.vpaOk[4] = 1; w.VpA[3] = 100.25; w.vpaOk[3] = 1;
    w.VpA[2] = 100; w.vpaOk[2] = 1; w.VpA[1] = 100.25; w.vpaOk[1] = 1;
    w.VpA[0] = 100.5; w.vpaOk[0] = 1; // maxA=100.25, spread 0.25
    w.VAH[1] = 100.5; w.vahOk[1] = 1; w.VAL[1] = 99.5; w.valOk[1] = 1; // mid1=100
    w.VAH[0] = 101.5; w.vahOk[0] = 1; w.VAL[0] = 100; w.valOk[0] = 1;  // mid0=100.75
    SetD(w, 1, 10, 20, -20); w.Up[2] = 5; w.upOk[2] = 1; // D1=10 > Up2=5
    w.D[0] = 15; w.dOk[0] = 1; w.Up[1] = 20; w.upOk[1] = 1; w.Lo[1] = -20; w.loOk[1] = 1;
}

static void T13_Fam11()
{
    Tmv2Window w; BuildF11Bull(w);
    CHECK(Tmv2_F11Bull(&w) == 1, "fam11 bull true");
    CHECK(Tmv2_F11Bear(&w) == 0, "fam11 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F11Bear(&m) == 1, "fam11 bear mirror true");
    // balance spread edge: max-min == 2t true (keep cross clauses passing)
    { Tmv2Window v = w; v.VpA[3] = 100.5; v.VpA[1] = 100.5; v.VpA[0] = 100.75; CHECK(Tmv2_F11Bull(&v) == 1, "fam11 spread edge true"); } // max 100.5 min 100
    { Tmv2Window v = w; v.VpA[3] = 100.75; CHECK(Tmv2_F11Bull(&v) == 0, "fam11 spread wide false"); }
    // current bar excluded from anchor: anchor uses [-1:-4] only
    { Tmv2Window v = w; v.VpA[0] = 100.25; CHECK(Tmv2_F11Bull(&v) == 0, "fam11 vpoc must exceed anchor"); }
    // delta OR: D1 fails but D0 passes
    { Tmv2Window v = w; v.Up[2] = 15; v.Up[1] = 5; CHECK(Tmv2_F11Bull(&v) == 1, "fam11 delta second-arm true"); } // D1=10!>15; D0=15>5
    { Tmv2Window v = w; v.Up[2] = 15; v.Up[1] = 20; CHECK(Tmv2_F11Bull(&v) == 0, "fam11 delta both fail false"); }
    { Tmv2Window v = w; v.vpaOk[4] = 0; CHECK(Tmv2_F11Bull(&v) == 0, "fam11 missing anchor bar"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F11Bull(&z) == 0 && Tmv2_F11Bear(&z) == 0, "fam11 zero false"); }
}

// ---------- T14 family 14: OF Reversal Concentration + Profile (vNext) ----------
static void BuildF14Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 101, 99, 99.5, 30, 50);
    SetBar(w, 1, 100, 101, 99, 99.5, 30, 50);
    SetBar(w, 0, 99.5, 101, 99, 100.5, 60, 20);
    SetD(w, 2, -20, 25, -15); SetD(w, 1, -30, 20, -20);
    w.D[0] = 40; w.dOk[0] = 1; w.Up[1] = 30; w.upOk[1] = 1;
    w.Lo[2] = -20; w.loOk[2] = 1; w.Lo[3] = -15; w.loOk[3] = 1;
    // seed: D1=-30<=Lo2=-20 true (first OR arm)
    w.Vp6[1] = 100; w.vp6Ok[1] = 1; w.Vp6[0] = 101; w.vp6Ok[0] = 1;
    w.BidD[1] = 2; w.bidOk[1] = 1; w.AskD[0] = 3; w.askOk[0] = 1;
    w.BidD[0] = 0; w.bidOk[0] = 1;
}

static void T14_Fam14()
{
    Tmv2Window w; BuildF14Bull(w);
    CHECK(Tmv2_F14Bull(&w) == 1, "fam14 bull true");
    CHECK(Tmv2_F14Bear(&w) == 0, "fam14 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F14Bear(&m) == 1, "fam14 bear mirror true");
    // second seed arm: D1 fails, D2 passes
    { Tmv2Window v = w; v.D[1] = 5; v.D[2] = -20; v.Lo[3] = -15; CHECK(Tmv2_F14Bull(&v) == 1, "fam14 seed second arm"); }
    { Tmv2Window v = w; v.D[1] = 5; v.D[2] = -10; CHECK(Tmv2_F14Bull(&v) == 0, "fam14 no seed false"); }
    // both diagonal IDs required; no approximation
    { Tmv2Window v = w; v.askOk[0] = 0; CHECK(Tmv2_F14Bull(&v) == 0, "fam14 missing ask false"); }
    { Tmv2Window v = w; v.bidOk[1] = 0; CHECK(Tmv2_F14Bull(&v) == 0, "fam14 missing bid false"); }
    { Tmv2Window v = w; v.AskD[0] = 2; CHECK(Tmv2_F14Bull(&v) == 0, "fam14 ask count short"); }
    { Tmv2Window v = w; v.BidD[0] = 2; CHECK(Tmv2_F14Bull(&v) == 0, "fam14 opposite veto"); }
    { Tmv2Window v = w; v.BidD[1] = 0; CHECK(Tmv2_F14Bull(&v) == 0, "fam14 prior count short"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F14Bull(&z) == 0 && Tmv2_F14Bear(&z) == 0, "fam14 zero false"); }
}

// ---------- T15 family 15: Delta Reversal VPOC Reclaim ----------
static void BuildF15Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 101, 100, 100.5, 30, 30);   // L2=100
    SetBar(w, 1, 100, 100.5, 99.5, 99.5, 30, 50); // C1<=O1; L1=99.5<=99.75
    SetBar(w, 0, 99.5, 100.75, 99.75, 100.5, 60, 20); // C0>O0; L0=99.75>=L1
    SetD(w, 2, -20, 25, -15); SetD(w, 1, -30, 20, -20);
    w.D[0] = 40; w.dOk[0] = 1; w.Up[1] = 30; w.upOk[1] = 1;
    w.Lo[2] = -20; w.loOk[2] = 1; w.Lo[3] = -15; w.loOk[3] = 1;
    w.Vp6[1] = 100; w.vp6Ok[1] = 1; w.Vp6[0] = 101; w.vp6Ok[0] = 1;
}

static void T15_Fam15()
{
    Tmv2Window w; BuildF15Bull(w);
    CHECK(Tmv2_F15Bull(&w) == 1, "fam15 bull true");
    CHECK(Tmv2_F15Bear(&w) == 0, "fam15 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F15Bear(&m) == 1, "fam15 bear mirror true");
    // failed-extension: L must not make a new low (L0>=L1), probe L1<=L2-t
    { Tmv2Window v = w; v.L[0] = v.L[1] - 0.25; CHECK(Tmv2_F15Bull(&v) == 0, "fam15 new low false"); }
    { Tmv2Window v = w; v.L[1] = v.L[2]; CHECK(Tmv2_F15Bull(&v) == 0, "fam15 no probe false"); }
    { Tmv2Window v = w; v.D[1] = 5; v.D[2] = -20; CHECK(Tmv2_F15Bull(&v) == 1, "fam15 seed second arm"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F15Bull(&z) == 0 && Tmv2_F15Bear(&z) == 0, "fam15 zero false"); }
}

// ---------- T16 family 16: Frozen VPOC Cross Executed Effort (primary) ----------
static void BuildF16Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 101, 99, 100, 10, 10);
    SetBar(w, 1, 100, 101, 99, 100, 10, 10);
    SetBar(w, 0, 99.5, 100.75, 99.75, 100.5, 40, 20); // 40>=1.5*20; 60>=25
    w.Vp8[1] = 100; w.vp8Ok[1] = 1; w.Vp8[0] = 101; w.vp8Ok[0] = 1;
}

static void T16_Fam16()
{
    Tmv2Window w; BuildF16Bull(w);
    CHECK(Tmv2_F16Bull(&w) == 1, "fam16 bull true");
    CHECK(Tmv2_F16Bear(&w) == 0, "fam16 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F16Bear(&m) == 1, "fam16 bear mirror true");
    { Tmv2Window v = w; v.O[0] = v.Vp8[1]; CHECK(Tmv2_F16Bull(&v) == 0, "fam16 open not below"); }
    { Tmv2Window v = w; v.C[0] = v.Vp8[1]; CHECK(Tmv2_F16Bull(&v) == 0, "fam16 no cross"); }
    { Tmv2Window v = w; v.AV[0] = 29; v.BV[0] = 20; CHECK(Tmv2_F16Bull(&v) == 0, "fam16 ratio short"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F16Bull(&z) == 0 && Tmv2_F16Bear(&z) == 0, "fam16 zero false"); }
}

// ---------- T17 family 17: Persistent Effort Volume Response (primary) ----------
static void BuildF17Bull(Tmv2Window& w)
{
    WinInit(w);
    for (int k = 1; k <= 5; k++) SetBar(w, k, 100, 101, 99, 100, 22, 18); // t=40
    SetBar(w, 0, 100, 101, 99.75, 100.75, 64, 32); // total 96 >= 1.25*40=50
    // n1=n2=0.1, n0=1/3: pairs (0,1) true; sum=0.5333>=0.40
    // C0=100.75>=C2+0.5=100.5; close 100.75>=101-0.3125=100.6875
    w.Vp8[1] = 100; w.vp8Ok[1] = 1; w.Vp8[0] = 101; w.vp8Ok[0] = 1;
}

static void T17_Fam17()
{
    Tmv2Window w; BuildF17Bull(w);
    CHECK(Tmv2_F17Bull(&w) == 1, "fam17 bull true");
    CHECK(Tmv2_F17Bear(&w) == 0, "fam17 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F17Bear(&m) == 1, "fam17 bear mirror true");
    // 2-of-3 variants: only pair (0,2)
    { Tmv2Window v = w; SetNorm(v, 1, 0.0, 40.0); CHECK(Tmv2_F17Bull(&v) == 1, "fam17 pair02 true"); }
    // only pair (1,2): n0 below 0.10 but sum still passes via n1+n2
    { Tmv2Window v = w; SetNorm(v, 0, 0.05, 96.0); SetNorm(v, 1, 0.2, 40.0); SetNorm(v, 2, 0.2, 40.0); CHECK(Tmv2_F17Bull(&v) == 1, "fam17 pair12 true"); }
    // none of the pairs (n0 low and n2 low)
    { Tmv2Window v = w; SetNorm(v, 0, 0.05, 96.0); SetNorm(v, 2, 0.0, 40.0); CHECK(Tmv2_F17Bull(&v) == 0, "fam17 no pair false"); }
    // sum edge: exactly 0.30+... use dyadic: n0=n1=n2=0.125 -> sum 0.375 < 0.40
    { Tmv2Window v = w; SetNorm(v, 0, 0.125, 32.0); SetNorm(v, 1, 0.125, 32.0); SetNorm(v, 2, 0.125, 32.0); CHECK(Tmv2_F17Bull(&v) == 0, "fam17 sum short false"); }
    // five-bar baseline: one zero bar fails closed
    { Tmv2Window v = w; v.AV[4] = 0; v.BV[4] = 0; CHECK(Tmv2_F17Bull(&v) == 0, "fam17 zero baseline false"); }
    // activity edge: total0 == 1.25*avg is inclusive-true
    { Tmv2Window v = w; v.AV[0] = 30; v.BV[0] = 20; CHECK(Tmv2_F17Bull(&v) == 1, "fam17 activity edge true"); } // 50>=50; n0=0.2 sum=0.4
    { Tmv2Window v = w; v.AV[0] = 29; v.BV[0] = 20; CHECK(Tmv2_F17Bull(&v) == 0, "fam17 activity short false"); } // 49<50
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F17Bull(&z) == 0 && Tmv2_F17Bear(&z) == 0, "fam17 zero false"); }
}

// ---------- T18 family 18: Extreme Effort Failure Core (primary) ----------
static void BuildF18Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 102, 99, 101, 20, 20);
    SetBar(w, 1, 100, 102, 100, 101, 20, 20);
    SetBar(w, 0, 100, 101.5, 99.5, 101, 20, 30);
}

static void T18_Fam18()
{
    Tmv2Window w; BuildF18Bull(w);
    CHECK(Tmv2_F18Bull(&w) == 1, "fam18 bull true");
    CHECK(Tmv2_F18Bear(&w) == 0, "fam18 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F18Bear(&m) == 1, "fam18 bear mirror true");
    { Tmv2Window v = w; v.C[0] = v.L[1]; CHECK(Tmv2_F18Bull(&v) == 0, "fam18 no reclaim false"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F18Bull(&z) == 0 && Tmv2_F18Bear(&z) == 0, "fam18 zero false"); }
}

// ---------- T19 family 19: Frozen POC Recovery + Distribution ----------
static void BuildF19Bull(Tmv2Window& w)
{
    WinInit(w);
    SetBar(w, 2, 100, 101, 99, 99, 25, 25);     // C2<O2
    SetBar(w, 1, 99, 101, 98.5, 100, 25, 25);   // C1>O1
    SetBar(w, 0, 100, 101.25, 100, 101, 30, 30); // C0>O0; C0>=100.25
    w.VpB[2] = 100; w.vpbOk[2] = 1;
    w.VpB[1] = 99.5; w.vpbOk[1] = 1;  // 99.5<=99.75 interim departure
    w.VpB[0] = 100.5; w.vpbOk[0] = 1; // >=100.25 recovery
    w.VAH[1] = 100; w.vahOk[1] = 1; w.VAL[1] = 99; w.valOk[1] = 1; // mid1=99.5
    w.VAH[0] = 101; w.vahOk[0] = 1; w.VAL[0] = 100; w.valOk[0] = 1; // mid0=100.5>=99.75, >100
}

static void T19_Fam19()
{
    Tmv2Window w; BuildF19Bull(w);
    CHECK(Tmv2_F19Bull(&w) == 1, "fam19 bull true");
    CHECK(Tmv2_F19Bear(&w) == 0, "fam19 bear false on bull");
    Tmv2Window m; MirrorWindow(w, m);
    CHECK(Tmv2_F19Bear(&m) == 1, "fam19 bear mirror true");
    // anchor freeze: recovery measured against VpB[-2], not VpB[-1]
    { Tmv2Window v = w; v.VpB[0] = 99.75; CHECK(Tmv2_F19Bull(&v) == 0, "fam19 anchor enforced"); }
    { Tmv2Window v = w; v.VpB[1] = 100.5; CHECK(Tmv2_F19Bull(&v) == 0, "fam19 interim must depart"); } // 100.5<=99.75 false
    { Tmv2Window v = w; v.vpbOk[2] = 0; CHECK(Tmv2_F19Bull(&v) == 0, "fam19 missing anchor"); }
    { Tmv2Window z; WinInit(z); CHECK(Tmv2_F19Bull(&z) == 0 && Tmv2_F19Bear(&z) == 0, "fam19 zero false"); }
}

// ---------- T20 deterministic stacking ----------
static void T20_Stack()
{
    int fired[TMV2_N_OUT] = {0};
    int hidden[TMV2_N_OUT] = {0};
    int bullLane[TMV2_N_OUT], bearLane[TMV2_N_OUT];
    // single bull at SG0 -> lane 0; single bear at SG1 -> lane 0
    fired[0] = 1;
    Tmv2_StackLanes(fired, hidden, bullLane, bearLane);
    CHECK(bullLane[0] == 0, "single bull lane 0");
    CHECK(bearLane[1] == -1, "bear idle -1");
    for (int s = 0; s < TMV2_N_PRIMARY; s++) fired[s] = 1;
    Tmv2_StackLanes(fired, hidden, bullLane, bearLane);
    int expect = 0;
    for (int s = 0; s < TMV2_N_PRIMARY; s += 2) CHECK(bullLane[s] == expect++, "bull dense lanes");
    expect = 0;
    for (int s = 1; s < TMV2_N_PRIMARY; s += 2) CHECK(bearLane[s] == expect++, "bear dense lanes");
    CHECK(expect == 16, "16 primary lanes per side");
    // hidden outputs consume no lane but keep truth
    hidden[0] = 1; hidden[2] = 1;
    Tmv2_StackLanes(fired, hidden, bullLane, bearLane);
    CHECK(bullLane[0] == -1 && bullLane[2] == -1, "hidden no lane");
    CHECK(bullLane[4] == 0, "visible compacts");
    CHECK(fired[0] == 1 && fired[2] == 1, "hidden truth retained");
    // geometry: bull below low, bear above high, base+lane*step
    {
        double tick = 0.25, low = 100.0, high = 101.0;
        double yBull0 = low - (2 + 0 * 3) * tick;
        double yBull15 = low - (2 + 15 * 3) * tick;
        double yBear0 = high + (2 + 0 * 3) * tick;
        CHECK(yBull0 < low && yBull15 < yBull0, "bull stacks downward");
        CHECK(yBear0 > high, "bear above high");
        CHECK(Tmv2_StackY(true, 0, low, high, 2, 3, tick) == yBull0, "stackY bull base");
        CHECK(Tmv2_StackY(false, 0, low, high, 2, 3, tick) == yBear0, "stackY bear base");
    }
}

// ---------- T21 fingerprint (v2.1: no source IDs, no revCfg) ----------
// Signature: (role, vapFlag, vapMultActual, bullBase, bearBase, step, tickBits).
static void T21_Fingerprint()
{
    unsigned long long a = Tmv2_Fingerprint(0, 1, 1, 2, 2, 3, 0x3E80000000000000ULL);
    unsigned long long b = Tmv2_Fingerprint(0, 1, 1, 2, 2, 3, 0x3E80000000000000ULL);
    CHECK(a == b && a != 0, "fingerprint deterministic nonzero");
    CHECK(Tmv2_Fingerprint(1, 1, 1, 2, 2, 3, 0x3E80000000000000ULL) != a, "fingerprint senses role");
    CHECK(Tmv2_Fingerprint(0, 0, 1, 2, 2, 3, 0x3E80000000000000ULL) != a, "fingerprint senses vap flag");
    CHECK(Tmv2_Fingerprint(0, 1, 2, 2, 2, 3, 0x3E80000000000000ULL) != a, "fingerprint senses multiplier");
    CHECK(Tmv2_Fingerprint(0, 1, 1, 5, 2, 3, 0x3E80000000000000ULL) != a, "fingerprint senses display offset");
    CHECK(Tmv2_Fingerprint(0, 1, 1, 2, 2, 3, 0x3E80000000000001ULL) != a, "fingerprint senses tickbits");
    int lo, hi;
    Tmv2_FingerprintSplit(a, &lo, &hi);
    CHECK(Tmv2_FingerprintJoin(lo, hi) == a, "fingerprint split/join roundtrip");
}

// ---------- T22 role dispatch (Tmv2_EvalRole) ----------
static int CountFired(const int fired[TMV2_N_OUT])
{
    int n = 0;
    for (int s = 0; s < TMV2_N_OUT; s++) n += fired[s] ? 1 : 0;
    return n;
}

static void T22_EvalRole()
{
    int fired[TMV2_N_OUT];
    { Tmv2Window w; BuildF01Bull(w);
      CHECK(Tmv2_EvalRole(0, &w, fired) == 1 && fired[0] == 1, "role0 fires SG0");
      CHECK(CountFired(fired) == 1, "role0 exactly one");
      CHECK(Tmv2_EvalRole(1, &w, fired) == 0 && CountFired(fired) == 0, "role1 silent on range");
      CHECK(Tmv2_EvalRole(2, &w, fired) == 0 && CountFired(fired) == 0, "role2 silent on range"); }
    { Tmv2Window w; BuildF14Bull(w);
      CHECK(Tmv2_EvalRole(1, &w, fired) == 1 && fired[20] == 1, "role1 fires SG20");
      CHECK(Tmv2_EvalRole(0, &w, fired) == 0, "role0 silent on r6"); }
    { Tmv2Window w; BuildF16Bull(w);
      CHECK(Tmv2_EvalRole(2, &w, fired) == 1 && fired[24] == 1, "role2 fires SG24");
      CHECK(Tmv2_EvalRole(1, &w, fired) == 0, "role1 silent on r8"); }
    { Tmv2Window w; BuildF05Bull(w);
      CHECK(Tmv2_EvalRole(99, &w, fired) == 0, "unknown role silent"); }
}

// ---------- T23 producer subgraph index translation ----------
// Catalog Spreadsheet notation ID{...}.SGn is one-based; ACSIL
// SubgraphIndex is zero-based (index = n - 1).
static void T23_SgMapping()
{
    CHECK(TMV2_SG_DELTA == 3, "SG4 delta -> index 3");
    CHECK(TMV2_SG_BAND_UP == 0, "SG1 upper -> index 0");
    CHECK(TMV2_SG_BAND_LO == 2, "SG3 lower -> index 2");
    CHECK(TMV2_SG_VPOC == 0, "SG1 vpoc -> index 0");
    CHECK(TMV2_SG_VVAH == 0, "SG1 vvah -> index 0");
    CHECK(TMV2_SG_VVAL == 1, "SG2 vval -> index 1");
    CHECK(TMV2_SG_DIAG == 58, "SG59 diag -> index 58");
    CHECK(Tmv2_CatalogSgToIndex(1) == 0, "catalog SG1 -> 0");
    CHECK(Tmv2_CatalogSgToIndex(2) == 1, "catalog SG2 -> 1");
    CHECK(Tmv2_CatalogSgToIndex(3) == 2, "catalog SG3 -> 2");
    CHECK(Tmv2_CatalogSgToIndex(4) == 3, "catalog SG4 -> 3");
    CHECK(Tmv2_CatalogSgToIndex(59) == 58, "catalog SG59 -> 58");
}

// ---------- T24 fail-closed history warm-ups ----------
static void T24_Warmup()
{
    CHECK(Tmv2_MinBarIndex(1) == 3, "warmup ord1");
    CHECK(Tmv2_MinBarIndex(2) == 3, "warmup ord2");
    CHECK(Tmv2_MinBarIndex(4) == 2, "warmup ord4");
    CHECK(Tmv2_MinBarIndex(5) == 0, "warmup ord5");
    CHECK(Tmv2_MinBarIndex(6) == 1, "warmup ord6");
    CHECK(Tmv2_MinBarIndex(7) == 22, "warmup ord7");
    CHECK(Tmv2_MinBarIndex(8) == 20, "warmup ord8");
    CHECK(Tmv2_MinBarIndex(9) == 2, "warmup ord9");
    CHECK(Tmv2_MinBarIndex(10) == 22, "warmup ord10");
    CHECK(Tmv2_MinBarIndex(11) == 21, "warmup ord11");
    CHECK(Tmv2_MinBarIndex(14) == 22, "warmup ord14");
    CHECK(Tmv2_MinBarIndex(15) == 22, "warmup ord15");
    CHECK(Tmv2_MinBarIndex(16) == 2, "warmup ord16");
    CHECK(Tmv2_MinBarIndex(17) == 5, "warmup ord17");
    CHECK(Tmv2_MinBarIndex(18) == 2, "warmup ord18");
    CHECK(Tmv2_MinBarIndex(19) == 2, "warmup ord19");
    CHECK(Tmv2_MinBarIndex(20) == 200, "warmup LTR ema200");
    CHECK(Tmv2_MinBarIndex(21) == 40, "warmup TBY");
    CHECK(Tmv2_MinBarIndex(22) == 40, "warmup RBY");
    CHECK(Tmv2_MinBarIndex(3) < 0, "warmup blocked ord3 none");
    CHECK(Tmv2_MinBarIndex(12) < 0, "warmup blocked ord12 none");
    CHECK(Tmv2_MinBarIndex(13) < 0, "warmup blocked ord13 none");
    // Gating: true predicate below threshold must clear its pair.
    { Tmv2Window w; BuildF01Bull(w);
      int fired[TMV2_N_OUT];
      CHECK(Tmv2_EvalRole(0, &w, fired) == 1 && fired[0] == 1, "warmup setup fires");
      int gated[TMV2_N_OUT];
      CHECK(Tmv2_EvalRoleAt(0, &w, 2, gated) == 0 && gated[0] == 0 && gated[1] == 0, "warmup ord1 cleared at bar 2");
      CHECK(Tmv2_EvalRoleAt(0, &w, 3, gated) == 1 && gated[0] == 1, "warmup ord1 kept at bar 3"); }
    { Tmv2Window w; BuildF07Bull(w);
      int gated[TMV2_N_OUT];
      CHECK(Tmv2_EvalRoleAt(0, &w, 21, gated) == 0 && gated[10] == 0, "warmup ord7 cleared at bar 21");
      CHECK(Tmv2_EvalRoleAt(0, &w, 22, gated) == 1 && gated[10] == 1, "warmup ord7 kept at bar 22"); }
    { Tmv2Window w; BuildF08Bull(w);
      int gated[TMV2_N_OUT];
      CHECK(Tmv2_EvalRoleAt(0, &w, 19, gated) == 0 && gated[12] == 0, "warmup ord8 cleared at bar 19");
      CHECK(Tmv2_EvalRoleAt(0, &w, 20, gated) == 1 && gated[12] == 1, "warmup ord8 kept at bar 20"); }
}

// ---------- T25 portable finite ----------
static void T25_Finite()
{
    double nan = std::acos(2.0);
    double inf = std::numeric_limits<double>::infinity();
    CHECK(Tmv2_IsFinite(0.0) == 1, "finite zero");
    CHECK(Tmv2_IsFinite(1.5) == 1, "finite value");
    CHECK(Tmv2_IsFinite(-123.456) == 1, "finite negative");
    CHECK(Tmv2_IsFinite(nan) == 0, "NaN not finite");
    CHECK(Tmv2_IsFinite(inf) == 0, "Inf not finite");
    CHECK(Tmv2_IsFinite(-inf) == 0, "-Inf not finite");
}

// ---------- T26 FNV-1a offset basis ----------
static void T26_FnvBasis()
{
    CHECK(Tmv2_FnvOffsetBasis() == 14695981039346656037ULL, "FNV-1a 64-bit offset basis");
}

// ---------- T27 self-contained structural gate ----------
// v2.1: no external source studies, no retired-slot labels. Family codes
// stay pinned for the log. Tmv2_SelfDisabled reports only the family-5
// VAP gate (Range + actual multiplier != 1 -> ord5).
static void T27_StructDisabled()
{
    int disabled[16];
    int n = 0;
    // Family-code pins (each disabled ordinal must name its code in the log).
    CHECK(std::string(Tmv2_FamilyCode(1)) == "OED", "code ord1 OED");
    CHECK(std::string(Tmv2_FamilyCode(2)) == "DES", "code ord2 DES");
    CHECK(std::string(Tmv2_FamilyCode(4)) == "EEF", "code ord4 EEF");
    CHECK(std::string(Tmv2_FamilyCode(5)) == "VGD", "code ord5 VGD");
    CHECK(std::string(Tmv2_FamilyCode(6)) == "WDM", "code ord6 WDM");
    CHECK(std::string(Tmv2_FamilyCode(7)) == "DRP", "code ord7 DRP");
    CHECK(std::string(Tmv2_FamilyCode(8)) == "PDR", "code ord8 PDR");
    CHECK(std::string(Tmv2_FamilyCode(9)) == "FVR", "code ord9 FVR");
    CHECK(std::string(Tmv2_FamilyCode(10)) == "ERM", "code ord10 ERM");
    CHECK(std::string(Tmv2_FamilyCode(11)) == "PBM", "code ord11 PBM");
    CHECK(std::string(Tmv2_FamilyCode(14)) == "OFR", "code ord14 OFR");
    CHECK(std::string(Tmv2_FamilyCode(15)) == "DVR", "code ord15 DVR");
    CHECK(std::string(Tmv2_FamilyCode(16)) == "VXC", "code ord16 VXC");
    CHECK(std::string(Tmv2_FamilyCode(17)) == "PEV", "code ord17 PEV");
    CHECK(std::string(Tmv2_FamilyCode(18)) == "R8F", "code ord18 R8F");
    CHECK(std::string(Tmv2_FamilyCode(19)) == "FPR", "code ord19 FPR");
    // Self-contained gate: only the family-5 VAP gate disables anything.
    { n = Tmv2_SelfDisabled(0, 1, disabled);
      CHECK(n == 0, "range gate-on none disabled"); }
    { n = Tmv2_SelfDisabled(0, 0, disabled);
      CHECK(n == 1 && disabled[0] == 5, "range gate-off disables ord5 only"); }
    { n = Tmv2_SelfDisabled(1, 0, disabled);
      CHECK(n == 0, "r6 gate-off none disabled"); }
    { n = Tmv2_SelfDisabled(1, 1, disabled);
      CHECK(n == 0, "r6 gate-on none disabled"); }
    { n = Tmv2_SelfDisabled(2, 0, disabled);
      CHECK(n == 0, "r8 gate-off none disabled"); }
    { n = Tmv2_SelfDisabled(99, 0, disabled);
      CHECK(n == 0, "unknown role none disabled"); }
}


static void FillLtrBullCtx(Tmv2Window& w)
{
    w.Macd[0] = -0.5; w.macdOk[0] = 1;
    w.Ema50[0] = 101.0; w.Ema200[0] = 100.0; w.emaOk[0] = 1;
    w.Adx[0] = 21.0; w.adxOk[0] = 1;
    w.Smi[0] = -61.0; w.smiOk[0] = 1;
}

static void FillTbyBullCtx(Tmv2Window& w)
{
    w.Adx[0] = 26.0; w.adxOk[0] = 1;
    w.StochK[0] = 19.0; w.StochD[0] = 18.0; w.stochOk[0] = 1;
    w.StochK[1] = 15.0; w.StochD[1] = 16.0; w.stochOk[1] = 1;
    w.Smi[0] = -61.0; w.smiOk[0] = 1;
}

static void FillRbyBullCtx(Tmv2Window& w)
{
    w.Adx[0] = 19.0; w.adxOk[0] = 1;
    w.Rsi[0] = 29.0; w.rsiOk[0] = 1;
    w.BbLo[0] = 101.0; w.BbUp[0] = 110.0; w.bbOk[0] = 1;
    w.C[0] = 100.5; // still must satisfy core close-in-outer-25%
    w.Smi[0] = -61.0; w.smiOk[0] = 1;
}

static void T28_HtmlContext()
{
    {
        Tmv2Window w; BuildF16Bull(w);
        CHECK(Tmv2_F16Bull(&w) == 1, "ltr core bull");
        CHECK(Tmv2_F16BullCtx(&w) == 0, "ltr ctx fail closed without inds");
        int fired[TMV2_N_OUT];
        CHECK(Tmv2_EvalRole(2, &w, fired) == 1 && fired[24] == 1 && fired[32] == 0, "ltr core only SG24");
        FillLtrBullCtx(w);
        CHECK(Tmv2_F16BullCtx(&w) == 1, "ltr ctx true");
        CHECK(Tmv2_EvalRole(2, &w, fired) == 2 && fired[24] == 1 && fired[32] == 1, "ltr fires SG24+32");
        { Tmv2Window v = w; v.macdOk[0] = 0; CHECK(Tmv2_F16BullCtx(&v) == 0, "ltr missing macd"); }
        { Tmv2Window v = w; v.Macd[0] = 0.1; CHECK(Tmv2_F16BullCtx(&v) == 0, "ltr macd not below zero"); }
        { Tmv2Window v = w; v.Adx[0] = 20.0; CHECK(Tmv2_F16BullCtx(&v) == 0, "ltr adx not >20"); }
        Tmv2Window m; MirrorWindow(w, m);
        m.Macd[0] = 0.5; m.macdOk[0] = 1;
        m.Ema50[0] = 99.0; m.Ema200[0] = 100.0; m.emaOk[0] = 1;
        m.Adx[0] = 21.0; m.adxOk[0] = 1;
        m.Smi[0] = 61.0; m.smiOk[0] = 1;
        CHECK(Tmv2_F16BearCtx(&m) == 1, "ltr bear ctx mirror");
        int gated[TMV2_N_OUT];
        CHECK(Tmv2_EvalRoleAt(2, &w, 199, gated) == 1 && gated[32] == 0 && gated[24] == 1, "ltr warmup clears SG32 keeps VXC");
        CHECK(Tmv2_EvalRoleAt(2, &w, 200, gated) == 2 && gated[32] == 1, "ltr warmup kept at 200");
    }
    {
        Tmv2Window w; BuildF17Bull(w);
        CHECK(Tmv2_F17BullCtx(&w) == 0, "tby ctx fail closed");
        FillTbyBullCtx(w);
        CHECK(Tmv2_F17BullCtx(&w) == 1, "tby ctx true");
        int fired[TMV2_N_OUT];
        CHECK(Tmv2_EvalRole(2, &w, fired) == 2 && fired[26] == 1 && fired[34] == 1, "tby fires SG26+34");
        { Tmv2Window v = w; v.StochK[0] = 20.0; CHECK(Tmv2_F17BullCtx(&v) == 0, "tby k not <20"); }
        { Tmv2Window v = w; v.StochK[1] = 17.0; v.StochD[1] = 16.0; CHECK(Tmv2_F17BullCtx(&v) == 0, "tby no prior k<=d"); }
    }
    {
        Tmv2Window w; BuildF18Bull(w);
        CHECK(Tmv2_F18BullCtx(&w) == 0, "rby ctx fail closed");
        FillRbyBullCtx(w);
        // core F18Bull uses C>=H-0.25*(H-L); FillRby may have moved C. Re-apply after knowing H/L.
        // If C was lowered below core, ctx must still include core. Rebuild then overlay C<=BbLo.
        BuildF18Bull(w);
        w.Adx[0] = 19.0; w.adxOk[0] = 1;
        w.Rsi[0] = 29.0; w.rsiOk[0] = 1;
        w.Smi[0] = -61.0; w.smiOk[0] = 1;
        w.BbUp[0] = w.H[0] + 10.0;
        w.BbLo[0] = w.C[0]; // C <= BbLo
        w.bbOk[0] = 1;
        CHECK(Tmv2_F18Bull(&w) == 1, "rby core still true");
        CHECK(Tmv2_F18BullCtx(&w) == 1, "rby ctx true");
        int fired[TMV2_N_OUT];
        CHECK(Tmv2_EvalRole(2, &w, fired) == 2 && fired[28] == 1 && fired[36] == 1, "rby fires SG28+36");
        { Tmv2Window v = w; v.Adx[0] = 20.0; CHECK(Tmv2_F18BullCtx(&v) == 0, "rby adx not <20"); }
        { Tmv2Window v = w; v.C[0] = v.BbLo[0] + 0.25; CHECK(Tmv2_F18BullCtx(&v) == 0, "rby close above lower band"); }
    }
}

int main()
{
    T01_Ledger();
    T02_Roles();
    T03_Flow();
    T04_Fam1();
    T05_Fam2();
    T06_Fam4();
    T07_Fam5();
    T08_Fam6();
    T09_Fam7();
    T10_Fam8();
    T11_Fam9();
    T12_Fam10();
    T13_Fam11();
    T14_Fam14();
    T15_Fam15();
    T16_Fam16();
    T17_Fam17();
    T18_Fam18();
    T19_Fam19();
    T20_Stack();
    T21_Fingerprint();
    T22_EvalRole();
    T23_SgMapping();
    T24_Warmup();
    T25_Finite();
    T26_FnvBasis();
    T27_StructDisabled();
    T28_HtmlContext();
    if (g_fails == 0)
        std::printf("TMV2 ALL GREEN: %d checks\n", g_checks);
    else
        std::printf("TMV2 RED: %d fails / %d checks\n", g_fails, g_checks);
    return g_fails ? 1 : 0;
}
