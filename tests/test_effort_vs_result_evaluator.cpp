// Portable tests for EffortVsResultEvaluator.cpp (pure core behind
// EVR_EVALUATOR_TEST). No Sierra headers, no external packages.
#define EVR_EVALUATOR_TEST 1
#include "../EffortVsResultEvaluator.cpp"
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } \
} while (0)

#define CHECK_DBL(a, b, eps, msg) do { \
    g_checks++; \
    if (std::fabs((double)(a) - (double)(b)) > (double)(eps)) { \
        g_fails++; \
        std::printf("FAIL %s:%d: %s (got %f want %f)\n", __FILE__, __LINE__, msg, (double)(a), (double)(b)); } \
} while (0)

// Build a flat 60s-spaced tape of n bars with flat OHLC, then let each
// test carve the bars it needs.
static void FlatTape(float* h, float* l, float* c, float* o, double* t, int n,
                     float px)
{
    for (int i = 0; i < n; i++)
    {
        h[i] = px + 1.0f;
        l[i] = px - 1.0f;
        c[i] = px;
        if (o)
            o[i] = px;
        t[i] = (double)i * 60.0;
    }
}

static void T1_LongOutcomes()
{
    // 20 bars, 60s spacing: horizon 900s covered (19*60=1140).
    const int n = 20;
    float h[20], l[20], c[20], o[20];
    double t[20];
    EvrOutcome oc;

    // Target: entry 100 risk 2 target 102; bar1 high touches.
    FlatTape(h, l, c, o, t, n, 100.0f);
    h[1] = 103.0f;
    int rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                              h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_TARGET, "long target status");
    CHECK_DBL(oc.outcomeR, 1.0, 1e-9, "long target R");
    CHECK(oc.evaluated == 1, "long target evaluated");
    CHECK(oc.exitBar == 1, "long target exit bar");

    // Stop: bar1 low touches stop 98.
    FlatTape(h, l, c, o, t, n, 100.0f);
    l[1] = 97.0f;
    rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_STOP, "long stop status");
    CHECK_DBL(oc.outcomeR, -1.0, 1e-9, "long stop R");

    // Timeout: never touches; exit at final eligible bar (index 15).
    FlatTape(h, l, c, o, t, n, 100.0f);
    for (int i = 0; i < n; i++)
        c[i] = 100.0f + (float)i * 0.1f; // drift up, never reaching 102
    // c[15] = 101.5 -> R = 1.5/2 = 0.75
    rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_TIMEOUT, "long timeout status");
    CHECK(oc.exitBar == 15, "long timeout exit is final eligible bar");
    CHECK_DBL(oc.outcomeR, (101.5 - 100.0) / 2.0, 1e-6, "long timeout R");

    // Ambiguous default (Mark/Exclude): bar1 hits both.
    FlatTape(h, l, c, o, t, n, 100.0f);
    h[1] = 103.0f;
    l[1] = 97.0f;
    rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_AMBIGUOUS, "long ambiguous status");
    CHECK(oc.evaluated == 0, "ambiguous excluded from aggregates");
    CHECK_DBL(oc.outcomeR, 0.0, 1e-9, "ambiguous R is 0");
    CHECK(oc.mfeR > 0.0, "ambiguous still reports MFE");

    // Stop-first policy resolves the same bar to -1R.
    rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 1, 1.0, &oc);
    CHECK(oc.status == EVRE_ST_STOP && oc.evaluated == 1, "stop-first policy");
    CHECK_DBL(oc.outcomeR, -1.0, 1e-9, "stop-first R");

    // Target-first policy resolves the same bar to +TargetR.
    rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 2, 2.0, &oc);
    CHECK(oc.status == EVRE_ST_TARGET, "target-first policy");
    CHECK_DBL(oc.outcomeR, 2.0, 1e-9, "target-first R uses TargetR");
}

static void T2_ShortOutcomes()
{
    const int n = 20;
    float h[20], l[20], c[20], o[20];
    double t[20];
    EvrOutcome oc;

    // Short mirror: entry 100, stop 102, target 98, risk 2.
    FlatTape(h, l, c, o, t, n, 100.0f);
    l[2] = 97.0f;
    int rc = EvrEvaluateEvent(-1, 100.0, 102.0, 98.0, 2.0,
                              h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_TARGET, "short target status");
    CHECK_DBL(oc.outcomeR, 1.0, 1e-9, "short target R");
    CHECK(oc.exitBar == 2, "short target exit bar");

    FlatTape(h, l, c, o, t, n, 100.0f);
    h[2] = 103.0f;
    rc = EvrEvaluateEvent(-1, 100.0, 102.0, 98.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(oc.status == EVRE_ST_STOP, "short stop status");
    CHECK_DBL(oc.outcomeR, -1.0, 1e-9, "short stop R");

    // Short timeout downward drift: c falls -> positive R for shorts.
    FlatTape(h, l, c, o, t, n, 100.0f);
    for (int i = 0; i < n; i++)
        c[i] = 100.0f - (float)i * 0.05f;
    rc = EvrEvaluateEvent(-1, 100.0, 102.0, 98.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(oc.status == EVRE_ST_TIMEOUT, "short timeout status");
    CHECK_DBL(oc.outcomeR, (100.0 - c[15]) / 2.0, 1e-6, "short timeout R sign");

    // Short ambiguous bar: low<=target and high>=stop same bar.
    FlatTape(h, l, c, o, t, n, 100.0f);
    h[1] = 103.0f;
    l[1] = 97.0f;
    rc = EvrEvaluateEvent(-1, 100.0, 102.0, 98.0, 2.0,
                          h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(oc.status == EVRE_ST_AMBIGUOUS && oc.evaluated == 0,
          "short ambiguous excluded");
}

static void T3_MfeMae()
{
    const int n = 20;
    float h[20], l[20], c[20], o[20];
    double t[20];
    EvrOutcome oc;

    // Long: entry 100 risk 2. Bar1 H=101 (fav 1), L=99.5 (adv -0.5);
    // bar2 H=101.6 (fav 1.6) then timeout at bar15 close 100.
    FlatTape(h, l, c, o, t, n, 100.0f);
    for (int i = 0; i < n; i++) { h[i] = 100.1f; l[i] = 99.9f; c[i] = 100.0f; }
    h[1] = 101.0f;
    l[1] = 99.5f;
    h[2] = 101.6f;
    l[2] = 99.8f;
    EvrEvaluateEvent(1, 100.0, 98.0, 110.0, 2.0,
                     h, l, c, t, n, 0, 900.0, 0, 5.0, &oc);
    CHECK(oc.status == EVRE_ST_TIMEOUT, "mfe timeout");
    CHECK_DBL(oc.mfeR, 1.6 / 2.0, 1e-6, "long MFE_R normalized");
    CHECK(oc.mfeR >= 0.0, "MFE nonnegative");
    CHECK_DBL(oc.maeR, -0.5 / 2.0, 1e-6, "long MAE_R nonpositive");
    CHECK(oc.maeR <= 0.0, "MAE nonpositive");

    // Short mirror: entry 100 risk 2, bar1 L=99 (fav 1), H=100.5 (adv -0.5).
    FlatTape(h, l, c, o, t, n, 100.0f);
    for (int i = 0; i < n; i++) { h[i] = 100.1f; l[i] = 99.9f; c[i] = 100.0f; }
    l[1] = 99.0f;
    h[1] = 100.5f;
    EvrEvaluateEvent(-1, 100.0, 102.0, 90.0, 2.0,
                     h, l, c, t, n, 0, 900.0, 0, 5.0, &oc);
    CHECK_DBL(oc.mfeR, 1.0 / 2.0, 1e-6, "short MFE_R normalized");
    CHECK_DBL(oc.maeR, -0.5 / 2.0, 1e-6, "short MAE_R normalized");
}

static void T4_ConfirmCloseTraversal()
{
    // Confirmation-close entry must not reuse the confirmation bar's H/L.
    // Long: candidate low 90, stop 89 (buf 1 tick=1). Confirmation close
    // entry 100. Confirmation bar had low 50 (would stop us if reused) but
    // traversal starts on the NEXT bar, so outcome is a timeout.
    const int n = 20;
    float h[20], l[20], c[20], o[20];
    double t[20];
    FlatTape(h, l, c, o, t, n, 100.0f);
    l[0] = 50.0f; // confirmation bar extreme (must be ignored)
    h[0] = 100.5f;
    c[0] = 100.0f;
    EvrOutcome oc;
    // firstBar=1: scan starts after the confirmation bar.
    int rc = EvrEvaluateEvent(1, 100.0, 89.0, 111.0, 11.0,
                              h, l, c, t, n, 1, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_TIMEOUT,
          "close-entry traversal starts on following bar");

    // Same setup but scanning FROM bar 0 would stop out: proves the test
    // is sensitive to the traversal start.
    rc = EvrEvaluateEvent(1, 100.0, 89.0, 111.0, 11.0,
                          h, l, c, t, n, 0, 960.0, 0, 1.0, &oc);
    CHECK(oc.status == EVRE_ST_STOP, "scan-from-confirmation-bar stops out");

    // Stop/target construction sanity for both entry prices.
    double s, tg, rk;
    CHECK(EvrComputeTrade(1, 90.0, 90.0, 100.0, 1.0, 1, 1.0, &s, &tg, &rk) == 1,
          "compute trade long");
    CHECK_DBL(s, 89.0, 1e-9, "long stop below candidate low");
    CHECK(EvrComputeTrade(-1, 110.0, 110.0, 100.0, 1.0, 2, 1.0, &s, &tg, &rk) == 1,
          "compute trade short");
    CHECK_DBL(s, 112.0, 1e-9, "short stop above candidate high");
    // Entry at stop => invalid risk.
    CHECK(EvrComputeTrade(1, 100.0, 100.0, 100.0, 1.0, 0, 1.0, &s, &tg, &rk) == 0,
          "entry-at-stop invalid risk");
    CHECK(EvrComputeTrade(1, 90.0, 90.0, 100.0, 0.0, 1, 1.0, &s, &tg, &rk) == 0,
          "non-positive tick invalid");
}

static void T5_RightEdge()
{
    // Only 3 bars: horizon 900s can never elapse -> insufficient, not timeout.
    const int n = 3;
    float h[3], l[3], c[3], o[3];
    double t[3];
    FlatTape(h, l, c, o, t, n, 100.0f);
    EvrOutcome oc;
    int rc = EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0,
                              h, l, c, t, n, 0, 900.0, 0, 1.0, &oc);
    CHECK(rc == 1 && oc.status == EVRE_ST_INSUFFICIENT, "right-edge excluded");
    CHECK(oc.evaluated == 0, "insufficient not aggregated");
    CHECK_DBL(oc.outcomeR, 0.0, 1e-9, "insufficient R is 0");
    CHECK(oc.exitBar == -1, "insufficient has no exit bar");
}

static void T6_EventSelection()
{
    int side = 0, kind = 0, conflict = 0;
    float strength = 0.0f;
    int has;

    // Pulse default: buyer pulse only -> short.
    has = EvrSelectEvent(-1.5f, 0.0f, 0.0f, 0.0f, 0,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 1 && side == -1 && kind == EVRE_KIND_PULSE, "pulse buyer short");
    CHECK_DBL(strength, 1.5, 1e-6, "pulse strength is |value|");

    // Pulse mode ignores arrows.
    has = EvrSelectEvent(0.0f, 0.0f, 0.0f, 123.0f, 0,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 0, "pulse mode ignores arrow-only");

    // Arrow mode: seller arrow -> long, strength 0 (price is not strength).
    has = EvrSelectEvent(0.0f, 0.0f, 0.0f, 456.0f, 1,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 1 && side == 1 && kind == EVRE_KIND_ARROW, "arrow seller long");
    CHECK_DBL(strength, 0.0, 1e-9, "arrow strength is 0");

    // Arrow mode ignores pulses.
    has = EvrSelectEvent(0.0f, 2.5f, 0.0f, 0.0f, 1,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 0, "arrow mode ignores pulse-only");

    // Both: pulse+arrow same side dedup to one event, kind both, pulse wins.
    has = EvrSelectEvent(0.0f, 2.5f, 0.0f, 789.0f, 2,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 1 && side == 1 && kind == EVRE_KIND_BOTH, "both dedup one event");
    CHECK_DBL(strength, 2.5, 1e-6, "both prefers pulse strength");

    // Both: arrow-only side still yields an arrow event with 0 strength.
    has = EvrSelectEvent(-1.25f, 0.0f, 111.0f, 0.0f, 2,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 1 && side == -1 && kind == EVRE_KIND_BOTH, "both buyer dedup");
    CHECK_DBL(strength, 1.25, 1e-6, "both buyer pulse strength");
}

static void T7_Conflict()
{
    int side = 0, kind = 0, conflict = 0;
    float strength = 0.0f;
    // Same-bar both sides in pulse mode.
    int has = EvrSelectEvent(-1.0f, 1.0f, 0.0f, 0.0f, 0,
                             &side, &kind, &strength, &conflict);
    CHECK(has == 0 && conflict == 1, "pulse conflict flagged");
    // Same-bar both sides in arrow mode.
    has = EvrSelectEvent(0.0f, 0.0f, 10.0f, 20.0f, 1,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 0 && conflict == 1, "arrow conflict flagged");
    // Both mode cross-channel conflict (pulse buy + arrow sell).
    has = EvrSelectEvent(-1.0f, 0.0f, 0.0f, 20.0f, 2,
                         &side, &kind, &strength, &conflict);
    CHECK(has == 0 && conflict == 1, "both-mode cross conflict flagged");
}

static void T8_Session()
{
    const int S = 9 * 3600 + 30 * 60;
    const int E = 16 * 3600;
    CHECK(EvrSessionEligible(10 * 3600, 1, S, E) == 1, "session inside");
    CHECK(EvrSessionEligible(8 * 3600, 1, S, E) == 0, "session outside");
    CHECK(EvrSessionEligible(8 * 3600, 0, S, E) == 1, "session disabled");
    CHECK(EvrSessionEligible(S, 1, S, E) == 1, "session start inclusive");
    // Crossing midnight: 22:00..06:00.
    const int CS = 22 * 3600, CE = 6 * 3600;
    CHECK(EvrSessionEligible(23 * 3600, 1, CS, CE) == 1, "midnight-cross late");
    CHECK(EvrSessionEligible(2 * 3600, 1, CS, CE) == 1, "midnight-cross early");
    CHECK(EvrSessionEligible(12 * 3600, 1, CS, CE) == 0, "midnight-cross midday out");
    CHECK(EvrSessionEligible(22 * 3600, 1, CS, CE) == 1, "midnight-cross start");
}

static void T9_Dates()
{
    CHECK(EvrValidYYYYMMDD(0) == 1, "date 0 unbounded valid");
    CHECK(EvrValidYYYYMMDD(20240115) == 1, "date valid");
    CHECK(EvrValidYYYYMMDD(20241301) == 0, "date bad month");
    CHECK(EvrValidYYYYMMDD(20240230) == 0, "date bad day");
    CHECK(EvrValidYYYYMMDD(20240229) == 1, "date leap day 2024");
    CHECK(EvrValidYYYYMMDD(20230229) == 0, "date non-leap Feb29");
    CHECK(EvrValidYYYYMMDD(-5) == 0, "date negative");
    CHECK(EvrDateEligible(20240115, 20240101, 20240131) == 1, "date inside");
    CHECK(EvrDateEligible(20240201, 20240101, 20240131) == 0, "date after end");
    CHECK(EvrDateEligible(20231231, 20240101, 20240131) == 0, "date before start");
    CHECK(EvrDateEligible(19990101, 0, 0) == 1, "date unbounded");
    CHECK(EvrDateEligible(20240115, 0, 20240131) == 1, "date open start");
}

static void T10_Overlap()
{
    int flag = 0, excl = 0;
    // Include+Flag: entry at bar 10, prior exit 12 -> flagged, kept.
    EvrOverlapCheck(10, 12, 1, 0, &flag, &excl);
    CHECK(flag == 1 && excl == 0, "include+flag marks overlap");
    // Include+Flag: no overlap.
    EvrOverlapCheck(13, 12, 1, 0, &flag, &excl);
    CHECK(flag == 0 && excl == 0, "include+flag clean");
    // Exclude policy: overlapping event excluded.
    EvrOverlapCheck(10, 12, 1, 1, &flag, &excl);
    CHECK(excl == 1 && flag == 1, "exclude policy drops overlapped");
    // Exclude policy: non-overlapping kept.
    EvrOverlapCheck(13, 12, 1, 1, &flag, &excl);
    CHECK(excl == 0 && flag == 0, "exclude policy keeps clean");
    // No prior: never flagged.
    EvrOverlapCheck(0, -1, 0, 1, &flag, &excl);
    CHECK(flag == 0 && excl == 0, "no prior no overlap");
    // Boundary: entry begins exactly at prior exit bar -> still overlap.
    EvrOverlapCheck(12, 12, 1, 0, &flag, &excl);
    CHECK(flag == 1, "boundary entry==exit is overlap");
}

static void T11_Sanitize()
{
    char dst[64];
    EvrSanitize(dst, sizeof(dst), "ES H5!@#");
    CHECK(std::strcmp(dst, "ES_H5___") == 0, "sanitize replaces outsiders");
    EvrSanitize(dst, sizeof(dst), "EVR_Evaluator-1.2");
    CHECK(std::strcmp(dst, "EVR_Evaluator-1.2") == 0, "sanitize keeps legal set");
    EvrSanitize(dst, sizeof(dst), "a/b\\c:d");
    CHECK(std::strcmp(dst, "a_b_c_d") == 0, "sanitize kills path separators");
    EvrSanitize(dst, sizeof(dst), "");
    CHECK(dst[0] == '\0', "sanitize empty stays empty");
    char name[256];
    EvrBuildCsvName(name, sizeof(name), "", "ESM5", 3, 7);
    CHECK(std::strcmp(name, "EVR_Evaluator_ESM5_Chart3_Study7.csv") == 0,
          "empty prefix falls back");
    EvrBuildCsvName(name, sizeof(name), "Pre/fix", "S:Y", 3, 7);
    CHECK(std::strcmp(name, "Pre_fix_S_Y_Chart3_Study7.csv") == 0,
          "csv name sanitizes both parts");
}

static void T12_Determinism()
{
    const int n = 20;
    float h[20], l[20], c[20], o[20];
    double t[20];
    FlatTape(h, l, c, o, t, n, 100.0f);
    h[3] = 103.0f;
    l[5] = 97.0f;
    EvrOutcome a, b;
    EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0, h, l, c, t, n, 0, 900.0, 0, 1.0, &a);
    EvrEvaluateEvent(1, 100.0, 98.0, 102.0, 2.0, h, l, c, t, n, 0, 900.0, 0, 1.0, &b);
    CHECK(a.status == b.status && a.exitBar == b.exitBar, "determinism status/exit");
    CHECK_DBL(a.outcomeR, b.outcomeR, 0.0, "determinism outcomeR");
    CHECK_DBL(a.mfeR, b.mfeR, 0.0, "determinism mfeR");
    CHECK_DBL(a.maeR, b.maeR, 0.0, "determinism maeR");
    CHECK_DBL(a.holdingSec, b.holdingSec, 0.0, "determinism holding");
}

static void T13_DirectionalStopValidity()
{
    // Directional stop validity: a long stop must sit below the entry, a
    // short stop above it. Non-positive risk distance is already rejected;
    // wrong-side stops with nonzero distance must also be rejected.
    double s, tg, rk;
    // Long: candidate low 90, 1-tick buffer => stop 89, but entry 80 puts
    // the stop ABOVE the entry (invalid for a long).
    CHECK(EvrComputeTrade(1, 90.0, 90.0, 80.0, 1.0, 1, 1.0, &s, &tg, &rk) == 0,
          "long stop above entry invalid");
    // Short: candidate high 110, 1-tick buffer => stop 111, but entry 120
    // puts the stop BELOW the entry (invalid for a short).
    CHECK(EvrComputeTrade(-1, 110.0, 110.0, 120.0, 1.0, 1, 1.0, &s, &tg, &rk) == 0,
          "short stop below entry invalid");
    // Correct-side stops stay valid.
    CHECK(EvrComputeTrade(1, 90.0, 90.0, 100.0, 1.0, 1, 1.0, &s, &tg, &rk) == 1,
          "long stop below entry valid");
    CHECK(EvrComputeTrade(-1, 110.0, 110.0, 100.0, 1.0, 2, 1.0, &s, &tg, &rk) == 1,
          "short stop above entry valid");
}

static void T14_SerialDaySeconds()
{
    // Sierra SCDateTime GetAsDouble() is serial DAYS; ACSIL horizon/holding
    // math is in seconds and must scale day-differences by 86400.
    // Static source check: the ACSIL body must contain the conversion.
    FILE* f = std::fopen("EffortVsResultEvaluator.cpp", "r");
    if (f == nullptr)
        f = std::fopen("../EffortVsResultEvaluator.cpp", "r");
    if (f == nullptr)
        f = std::fopen("tests/../EffortVsResultEvaluator.cpp", "r");
    CHECK(f != nullptr, "evaluator source readable");
    char buf[65536];
    std::size_t n = 0;
    if (f != nullptr)
        n = std::fread(buf, 1, sizeof(buf) - 1, f);
    if (f != nullptr)
        std::fclose(f);
    buf[(n < sizeof(buf) - 1) ? n : (sizeof(buf) - 1)] = '\0';
    CHECK(std::strstr(buf, "86400.0") != nullptr,
          "serial-day to seconds conversion present (86400.0)");
    // Portable helper contract: day-differences scale to seconds.
    CHECK_DBL(EvrDaysToSeconds(900.0 / 86400.0), 900.0, 1e-9,
              "days helper scales to seconds");
    CHECK_DBL(EvrSecondsBetween(2.0, 1.0), 86400.0, 1e-9,
              "seconds-between helper spans one day");
    CHECK_DBL(900.0 / 86400.0 * 86400.0, 900.0, 1e-9,
              "day-difference scales back to seconds");
}

int main()
{
    T1_LongOutcomes();
    T2_ShortOutcomes();
    T3_MfeMae();
    T4_ConfirmCloseTraversal();
    T5_RightEdge();
    T6_EventSelection();
    T7_Conflict();
    T8_Session();
    T9_Dates();
    T10_Overlap();
    T11_Sanitize();
    T12_Determinism();
    T13_DirectionalStopValidity();
    T14_SerialDaySeconds();
    std::printf("checks=%d fails=%d\n", g_checks, g_fails);
    if (g_fails == 0)
    {
        std::printf("ALL EVR EVALUATOR TESTS PASSED\n");
        return 0;
    }
    std::printf("EVR EVALUATOR TESTS FAILED\n");
    return 1;
}
