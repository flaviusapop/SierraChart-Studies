// =============================================================================
// CoilingState.cpp — Sierra Chart ACSIL Custom Study
// =============================================================================
//
// PURPOSE:
//   Detects "coiling state" — when multiple orderflow triggers are
//   simultaneously near their firing threshold, signalling a pre-ignition
//   setup before any single trigger fires.
//
//   All 38 active triggers (40 total, excluding #17/#18 which require NYSE
//   Tick data) are computed natively from sc.AskVolume / sc.BidVolume and
//   internal indicator computation.  No GetStudyArrayFromChartUsingID.
//   Chart-type agnostic: loads on Range Bar, Renko 6t, or Renko 8t unchanged.
//
// OUTPUTS:
//   — 38 DRAWSTYLE_IGNORE proximity subgraphs (downstream studies may read)
//   — CoilScoreLong / CoilScoreShort (indicator panel, trigger count)
//   — CoilAlertLong (dot below bar) / CoilAlertShort (dot above bar)
//
// PERSISTENT SLOTS:
//   Int(1) = lastKnownBars         (new-bar detection / intrabar guard)
//   Int(2) = alertedBarLong        (alert watermark — long)
//   Int(3) = alertedBarShort       (alert watermark — short)
//   Int(4) = fingerprint word 0    (settings change detection)
//   Int(5) = fingerprint word 1
//   Int(6) = fingerprint word 2
//
// BUILD RULES:
//   No std::min / std::max — use SC_MIN / SC_MAX macros or ternary
//   No STL containers in hot path
//   sc.MovingAverage uses 5-arg form: (In, Out, MAType, BarIndex, Length)
//   VAP iteration: GetNextHigherVAPElement with int PriceInTicks = INT_MIN
// =============================================================================

#include "sierrachart.h"
#include <cmath>
#include <climits>

SCDLLName("CoilingState")
// v1.1 — build check 2026-06-18

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------
#define SC_MAX(a,b)   ((a)>(b)?(a):(b))
#define SC_MIN(a,b)   ((a)<(b)?(a):(b))
#define CLAMPF(x,lo,hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#define SAT(x)        CLAMPF((x),0.0f,1.0f)    // saturate to [0,1]
#define BFLT(cond)    ((cond)?1.0f:0.0f)        // bool → float

// ---------------------------------------------------------------------------
// Subgraph indices — INTERNAL (DRAWSTYLE_IGNORE)
// SG_MACD_LINE removed (computed inline); total = 60 SGs (0-59) to fit SC limit.
// ---------------------------------------------------------------------------
#define SG_DELTA           0   // bar delta = AskVol - BidVol
#define SG_BB_UPPER        1   // BB upper band on delta
#define SG_BB_LOWER        2   // BB lower band on delta
#define SG_BB_MIDDLE       3   // BB middle (SMA of delta)
#define SG_VPOC            4   // Volume Point of Control price
#define SG_VAH             5   // Value Area High (68%)
#define SG_VAL             6   // Value Area Low  (68%)
#define SG_EMA50           7   // 50-period EMA on close
#define SG_EMA200          8   // 200-period EMA on close
#define SG_MACD_FAST       9   // 9-period EMA on close (MACD fast)
#define SG_MACD_SLOW       10  // 26-period EMA on close (MACD slow)
#define SG_MACD_SIG        11  // MACD signal = 9-EMA of (fast-slow); line computed inline
#define SG_ADX             12  // Average Directional Index (14,14 Wilder)
#define SG_RSI             13  // RSI 14-period (classic avg gain/loss)
#define SG_STOCH_K         14  // Fast Stochastic %K (10-period)
#define SG_STOCH_D         15  // Stochastic %D = 3-SMA of %K
#define SG_CUM_DELTA       16  // Cumulative delta (running sum)
#define SG_SMOOTH_TR       17  // Wilder-smoothed True Range (ADX intermediate)
#define SG_SMOOTH_PLUS_DM  18  // Wilder-smoothed +DM  (ADX intermediate)
#define SG_SMOOTH_MINUS_DM 19  // Wilder-smoothed -DM  (ADX intermediate)

// ---------------------------------------------------------------------------
// Subgraph indices — PROXIMITY (one per trigger, DRAWSTYLE_IGNORE)
// Triggers 1-16 and 19-40 (17,18 excluded — require NYSE Tick)
// ---------------------------------------------------------------------------
#define SG_PROX_T01  20   // Fading MOMO Bull
#define SG_PROX_T02  21   // Fading MOMO Bear
#define SG_PROX_T03  22   // Delta Rise Bull
#define SG_PROX_T04  23   // Delta Drop Bear
#define SG_PROX_T05  24   // BPOC Bull
#define SG_PROX_T06  25   // BPOC Bear
#define SG_PROX_T07  26   // EXH Bull
#define SG_PROX_T08  27   // EXH Bear
#define SG_PROX_T09  28   // VOL SEQ + Delta DIV Bull
#define SG_PROX_T10  29   // VOL SEQ + Delta DIV Bear
#define SG_PROX_T11  30   // VA Long
#define SG_PROX_T12  31   // VA Short
#define SG_PROX_T13  32   // Delta Slingshot Buy  ★★
#define SG_PROX_T14  33   // Delta Slingshot Sell ★★
#define SG_PROX_T15  34   // POC Delta Bull
#define SG_PROX_T16  35   // POC Delta Bear
// SG 36,37 reserved (T17,T18 excluded)
#define SG_PROX_T19  36   // Delta Trap Bull ★★
#define SG_PROX_T20  37   // Delta Trap Bear ★★
#define SG_PROX_T21  38   // POCL Bull ★★
#define SG_PROX_T22  39   // POCS Bear ★★
#define SG_PROX_T23  40   // BR + Delta DIV Bull
#define SG_PROX_T24  41   // BR + Delta DIV Bear
#define SG_PROX_T25  42   // SVOL Bull
#define SG_PROX_T26  43   // SVOL Bear
#define SG_PROX_T27  44   // OF Long v1  ★★
#define SG_PROX_T28  45   // OF Long v2  ★★
#define SG_PROX_T29  46   // OF Short v1 ★★
#define SG_PROX_T30  47   // OF Short v2 ★★
#define SG_PROX_T31  48   // FA+ Bull
#define SG_PROX_T32  49   // FA- Bear
#define SG_PROX_T33  50   // Long Trigger 8t ★★
#define SG_PROX_T34  51   // Short Trigger 8t ★★
#define SG_PROX_T35  52   // T BUY
#define SG_PROX_T36  53   // T SELL
#define SG_PROX_T37  54   // R BUY
#define SG_PROX_T38  55   // R SELL
#define SG_PROX_T39  56   // POC Wave Bull ★★
#define SG_PROX_T40  57   // POC Wave Bear ★★

// ---------------------------------------------------------------------------
// Subgraph indices — OUTPUT (SG_SCORE_LONG/SHORT removed to stay within 60-SG limit)
// ---------------------------------------------------------------------------
#define SG_ALERT_LONG   58  // dot below sc.Low when score >= MinTriggers
#define SG_ALERT_SHORT  59  // dot above sc.High when score >= MinTriggers

// ---------------------------------------------------------------------------
// Input indices
// ---------------------------------------------------------------------------
#define IN_BB_LEN       0   // BB length (structural)
#define IN_BB_MULT      1   // BB multiplier (structural)
#define IN_COIL_THRESH  2   // coiling threshold 0-1 (structural)
#define IN_MIN_BB_WIDTH 3   // BB width guard (structural)
#define IN_MIN_TRIGGERS 4   // minimum trigger count to fire alert (structural)
#define IN_ACCUM_BARS   5   // accumulation window in bars (structural)
#define IN_ALERT_SOUND  6   // alert sound ID (structural)
#define IN_LONG_COLOR   7   // long alert color (display only)
#define IN_SHORT_COLOR  8   // short alert color (display only)

// ---------------------------------------------------------------------------
// Helper: compute VPOC, VAH, VAL for a single bar using VAP footprint data.
// Levels from GetNextHigherVAPElement are ascending in price (p=0 = lowest).
// ---------------------------------------------------------------------------
static void ComputeVPOCandVA(SCStudyInterfaceRef sc, int barIndex,
                              float& vpocOut, float& vahOut, float& valOut)
{
    vpocOut = vahOut = valOut = sc.Close[barIndex];
    if (sc.VolumeAtPriceForBars == nullptr) return;

    // --- First pass: accumulate into fixed stack arrays ---
    const int MAX_LEVELS = 512;
    float  lvlPrice[MAX_LEVELS];
    float  lvlVol[MAX_LEVELS];
    int    count     = 0;
    float  totalVol  = 0.0f;
    int    vpocIdx   = 0;
    float  maxVol    = 0.0f;

    int    PriceInTicks = INT_MIN;
    const  s_VolumeAtPriceV2* pVAP = nullptr;

    while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement(
               (unsigned int)barIndex, PriceInTicks, &pVAP))
    {
        if (!pVAP) continue;
        if (count >= MAX_LEVELS) break;

        float price = (float)pVAP->PriceInTicks * sc.TickSize;
        float vol   = (float)pVAP->Volume;

        lvlPrice[count] = price;
        lvlVol[count]   = vol;
        totalVol += vol;

        if (vol > maxVol) {
            maxVol   = vol;
            vpocIdx  = count;
            vpocOut  = price;
        }
        count++;
    }

    if (count == 0 || totalVol <= 0.0f) return;

    // --- Second pass: expand from VPOC outward to cover 68% of total volume ---
    float targetVol = totalVol * 0.68f;
    float accVol    = maxVol;
    int   upIdx     = vpocIdx;
    int   downIdx   = vpocIdx;
    vahOut = vpocOut;
    valOut = vpocOut;

    while (accVol < targetVol)
    {
        float upAddable   = (upIdx   + 1 < count) ? lvlVol[upIdx   + 1] : 0.0f;
        float downAddable = (downIdx - 1 >= 0)     ? lvlVol[downIdx - 1] : 0.0f;

        if (upAddable == 0.0f && downAddable == 0.0f) break;

        if (upAddable >= downAddable) {
            upIdx++;
            accVol += upAddable;
            vahOut  = lvlPrice[upIdx];
        } else {
            downIdx--;
            accVol += downAddable;
            valOut  = lvlPrice[downIdx];
        }
    }
}

// ---------------------------------------------------------------------------
// Direction arrays for the 38 proximity subgraphs.
// 1 = Long trigger, -1 = Short trigger.
// Indexed as proxSGIndex - SG_PROX_T01 (i.e. relative index 0..37)
// Triggers in order: T01,T02,...,T16,T19,T20,T21,T22,T23,...,T40
// ---------------------------------------------------------------------------
static const int TRIGGER_DIR[38] = {
     1,-1, // T01,T02
     1,-1, // T03,T04
     1,-1, // T05,T06
     1,-1, // T07,T08
     1,-1, // T09,T10
     1,-1, // T11,T12
     1,-1, // T13,T14
     1,-1, // T15,T16
     1,-1, // T19,T20
     1,-1, // T21,T22
     1,-1, // T23,T24
     1,-1, // T25,T26
     1,-1, // T27,T28
     1,-1, // T29,T30
     1,-1, // T31,T32
     1,-1, // T33,T34
     1,-1, // T35,T36
     1,-1, // T37,T38
     1,-1  // T39,T40
};

// Mapping from relative trigger index (0-37) to absolute SG index
static const int PROX_SG[38] = {
    SG_PROX_T01, SG_PROX_T02, SG_PROX_T03, SG_PROX_T04,
    SG_PROX_T05, SG_PROX_T06, SG_PROX_T07, SG_PROX_T08,
    SG_PROX_T09, SG_PROX_T10, SG_PROX_T11, SG_PROX_T12,
    SG_PROX_T13, SG_PROX_T14, SG_PROX_T15, SG_PROX_T16,
    SG_PROX_T19, SG_PROX_T20, SG_PROX_T21, SG_PROX_T22,
    SG_PROX_T23, SG_PROX_T24, SG_PROX_T25, SG_PROX_T26,
    SG_PROX_T27, SG_PROX_T28, SG_PROX_T29, SG_PROX_T30,
    SG_PROX_T31, SG_PROX_T32, SG_PROX_T33, SG_PROX_T34,
    SG_PROX_T35, SG_PROX_T36, SG_PROX_T37, SG_PROX_T38,
    SG_PROX_T39, SG_PROX_T40
};

// =============================================================================
// Main Study Function
// =============================================================================
SCSFExport scsf_CoilingState(SCStudyInterfaceRef sc)
{
    // =========================================================================
    // SetDefaults
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName                = "CoilingState";
        sc.StudyDescription         = "Detects pre-ignition coiling — multiple triggers near threshold simultaneously.";
        sc.AutoLoop                 = 0;   // manual loop
        sc.GraphRegion              = 0;
        sc.MaintainVolumeAtPriceData = 1;  // required for VPOC/VA computation

        // --- Inputs ---
        sc.Input[IN_BB_LEN].Name = "BB Length";
        sc.Input[IN_BB_LEN].SetInt(20);
        sc.Input[IN_BB_LEN].SetIntLimits(2, 200);

        sc.Input[IN_BB_MULT].Name = "BB Multiplier";
        sc.Input[IN_BB_MULT].SetFloat(0.9f);
        sc.Input[IN_BB_MULT].SetFloatLimits(0.1f, 5.0f);

        sc.Input[IN_COIL_THRESH].Name = "Coil Threshold (0-1)";
        sc.Input[IN_COIL_THRESH].SetFloat(0.80f);
        sc.Input[IN_COIL_THRESH].SetFloatLimits(0.0f, 1.0f);

        sc.Input[IN_MIN_BB_WIDTH].Name = "Min BB Width Guard";
        sc.Input[IN_MIN_BB_WIDTH].SetFloat(5.0f);
        sc.Input[IN_MIN_BB_WIDTH].SetFloatLimits(0.0f, 1000.0f);

        sc.Input[IN_MIN_TRIGGERS].Name = "Min Triggers to Alert";
        sc.Input[IN_MIN_TRIGGERS].SetInt(3);
        sc.Input[IN_MIN_TRIGGERS].SetIntLimits(1, 38);

        sc.Input[IN_ACCUM_BARS].Name = "Accumulation Bars";
        sc.Input[IN_ACCUM_BARS].SetInt(1);
        sc.Input[IN_ACCUM_BARS].SetIntLimits(1, 20);

        sc.Input[IN_ALERT_SOUND].Name = "Alert Sound ID";
        sc.Input[IN_ALERT_SOUND].SetInt(2);
        sc.Input[IN_ALERT_SOUND].SetIntLimits(0, 100);

        sc.Input[IN_LONG_COLOR].Name = "Long Alert Color";
        sc.Input[IN_LONG_COLOR].SetColor(0, 200, 0);    // green

        sc.Input[IN_SHORT_COLOR].Name = "Short Alert Color";
        sc.Input[IN_SHORT_COLOR].SetColor(200, 0, 0);   // red

        // --- Internal subgraphs (IGNORE) ---
        auto SetIgnore = [](SCSubgraphRef& sg, const char* name) {
            sg.Name      = name;
            sg.DrawStyle = DRAWSTYLE_IGNORE;
            sg.PrimaryColor = RGB(128,128,128);
        };

        SetIgnore(sc.Subgraph[SG_DELTA],          "Delta");
        SetIgnore(sc.Subgraph[SG_BB_UPPER],        "BB_Upper");
        SetIgnore(sc.Subgraph[SG_BB_LOWER],        "BB_Lower");
        SetIgnore(sc.Subgraph[SG_BB_MIDDLE],       "BB_Middle");
        SetIgnore(sc.Subgraph[SG_VPOC],            "VPOC");
        SetIgnore(sc.Subgraph[SG_VAH],             "VAH");
        SetIgnore(sc.Subgraph[SG_VAL],             "VAL");
        SetIgnore(sc.Subgraph[SG_EMA50],           "EMA50");
        SetIgnore(sc.Subgraph[SG_EMA200],          "EMA200");
        SetIgnore(sc.Subgraph[SG_MACD_FAST],       "MACD_Fast");
        SetIgnore(sc.Subgraph[SG_MACD_SLOW],       "MACD_Slow");
        SetIgnore(sc.Subgraph[SG_MACD_SIG],        "MACD_Sig");
        SetIgnore(sc.Subgraph[SG_ADX],             "ADX");
        SetIgnore(sc.Subgraph[SG_RSI],             "RSI");
        SetIgnore(sc.Subgraph[SG_STOCH_K],         "Stoch_K");
        SetIgnore(sc.Subgraph[SG_STOCH_D],         "Stoch_D");
        SetIgnore(sc.Subgraph[SG_CUM_DELTA],       "CumDelta");
        SetIgnore(sc.Subgraph[SG_SMOOTH_TR],       "Smooth_TR");
        SetIgnore(sc.Subgraph[SG_SMOOTH_PLUS_DM],  "Smooth_PlusDM");
        SetIgnore(sc.Subgraph[SG_SMOOTH_MINUS_DM], "Smooth_MinusDM");

        // --- Proximity subgraphs (IGNORE, for downstream study reads) ---
        SetIgnore(sc.Subgraph[SG_PROX_T01], "Prox_T01_FadingMOMO_L");
        SetIgnore(sc.Subgraph[SG_PROX_T02], "Prox_T02_FadingMOMO_S");
        SetIgnore(sc.Subgraph[SG_PROX_T03], "Prox_T03_DeltaRise_L");
        SetIgnore(sc.Subgraph[SG_PROX_T04], "Prox_T04_DeltaDrop_S");
        SetIgnore(sc.Subgraph[SG_PROX_T05], "Prox_T05_BPOC_L");
        SetIgnore(sc.Subgraph[SG_PROX_T06], "Prox_T06_BPOC_S");
        SetIgnore(sc.Subgraph[SG_PROX_T07], "Prox_T07_EXH_L");
        SetIgnore(sc.Subgraph[SG_PROX_T08], "Prox_T08_EXH_S");
        SetIgnore(sc.Subgraph[SG_PROX_T09], "Prox_T09_VolSeqDiv_L");
        SetIgnore(sc.Subgraph[SG_PROX_T10], "Prox_T10_VolSeqDiv_S");
        SetIgnore(sc.Subgraph[SG_PROX_T11], "Prox_T11_VALong");
        SetIgnore(sc.Subgraph[SG_PROX_T12], "Prox_T12_VAShort");
        SetIgnore(sc.Subgraph[SG_PROX_T13], "Prox_T13_Slingshot_L");
        SetIgnore(sc.Subgraph[SG_PROX_T14], "Prox_T14_Slingshot_S");
        SetIgnore(sc.Subgraph[SG_PROX_T15], "Prox_T15_POCDelta_L");
        SetIgnore(sc.Subgraph[SG_PROX_T16], "Prox_T16_POCDelta_S");
        SetIgnore(sc.Subgraph[SG_PROX_T19], "Prox_T19_DeltaTrap_L");
        SetIgnore(sc.Subgraph[SG_PROX_T20], "Prox_T20_DeltaTrap_S");
        SetIgnore(sc.Subgraph[SG_PROX_T21], "Prox_T21_POCL");
        SetIgnore(sc.Subgraph[SG_PROX_T22], "Prox_T22_POCS");
        SetIgnore(sc.Subgraph[SG_PROX_T23], "Prox_T23_BRDiv_L");
        SetIgnore(sc.Subgraph[SG_PROX_T24], "Prox_T24_BRDiv_S");
        SetIgnore(sc.Subgraph[SG_PROX_T25], "Prox_T25_SVOL_L");
        SetIgnore(sc.Subgraph[SG_PROX_T26], "Prox_T26_SVOL_S");
        SetIgnore(sc.Subgraph[SG_PROX_T27], "Prox_T27_OFLong_v1");
        SetIgnore(sc.Subgraph[SG_PROX_T28], "Prox_T28_OFLong_v2");
        SetIgnore(sc.Subgraph[SG_PROX_T29], "Prox_T29_OFShort_v1");
        SetIgnore(sc.Subgraph[SG_PROX_T30], "Prox_T30_OFShort_v2");
        SetIgnore(sc.Subgraph[SG_PROX_T31], "Prox_T31_FA_Plus");
        SetIgnore(sc.Subgraph[SG_PROX_T32], "Prox_T32_FA_Minus");
        SetIgnore(sc.Subgraph[SG_PROX_T33], "Prox_T33_LongTrig8t");
        SetIgnore(sc.Subgraph[SG_PROX_T34], "Prox_T34_ShortTrig8t");
        SetIgnore(sc.Subgraph[SG_PROX_T35], "Prox_T35_TBUY");
        SetIgnore(sc.Subgraph[SG_PROX_T36], "Prox_T36_TSELL");
        SetIgnore(sc.Subgraph[SG_PROX_T37], "Prox_T37_RBUY");
        SetIgnore(sc.Subgraph[SG_PROX_T38], "Prox_T38_RSELL");
        SetIgnore(sc.Subgraph[SG_PROX_T39], "Prox_T39_POCWave_L");
        SetIgnore(sc.Subgraph[SG_PROX_T40], "Prox_T40_POCWave_S");

        // --- Output subgraphs ---
        sc.DrawZeros = 0;

        // NOTE: SG_SCORE_LONG/SHORT removed to stay within 60-SG limit.
        // CoilAlertLong  (SG58) — green dot at bar low when longCount  >= MinTriggers
        // CoilAlertShort (SG59) — red dot at bar high when shortCount >= MinTriggers
        sc.Subgraph[SG_ALERT_LONG].Name        = "CoilAlertLong";
        sc.Subgraph[SG_ALERT_LONG].DrawStyle   = DRAWSTYLE_POINT;
        sc.Subgraph[SG_ALERT_LONG].PrimaryColor = RGB(0,200,0);
        sc.Subgraph[SG_ALERT_LONG].LineWidth    = 3;

        sc.Subgraph[SG_ALERT_SHORT].Name        = "CoilAlertShort";
        sc.Subgraph[SG_ALERT_SHORT].DrawStyle   = DRAWSTYLE_POINT;
        sc.Subgraph[SG_ALERT_SHORT].PrimaryColor = RGB(200,0,0);
        sc.Subgraph[SG_ALERT_SHORT].LineWidth    = 3;

        return;
    }

    // =========================================================================
    // Study body
    // =========================================================================

    // --- Read inputs ---
    const int   bbLen      = sc.Input[IN_BB_LEN].GetInt();
    const float bbMult     = sc.Input[IN_BB_MULT].GetFloat();
    const float coilThresh = sc.Input[IN_COIL_THRESH].GetFloat();
    const float minBBWidth = sc.Input[IN_MIN_BB_WIDTH].GetFloat();
    const int   minTrig    = sc.Input[IN_MIN_TRIGGERS].GetInt();
    const int   accumBars  = sc.Input[IN_ACCUM_BARS].GetInt();
    const int   soundID    = sc.Input[IN_ALERT_SOUND].GetInt();

    // --- Persistent slots ---
    int& lastKnownBars  = sc.GetPersistentInt(1);
    int& alertedLong    = sc.GetPersistentInt(2);
    int& alertedShort   = sc.GetPersistentInt(3);
    int& storedFp0      = sc.GetPersistentInt(4);
    int& storedFp1      = sc.GetPersistentInt(5);
    int& storedFp2      = sc.GetPersistentInt(6);

    // --- Intrabar guard ---
    const bool isFullRecalc = (sc.UpdateStartIndex == 0);
    const bool isNewBar     = (sc.ArraySize > lastKnownBars);
    if (!isFullRecalc && !isNewBar) return;
    lastKnownBars = sc.ArraySize;

    // --- Settings fingerprint (structural inputs only) ---
    int fp0 = sc.Input[IN_BB_LEN].GetInt()
              ^ ((int)(sc.Input[IN_BB_MULT].GetFloat()     * 1000));
    int fp1 = (int)(sc.Input[IN_COIL_THRESH].GetFloat()    * 1000)
              ^ (int)(sc.Input[IN_MIN_BB_WIDTH].GetFloat() *  100);
    int fp2 = sc.Input[IN_MIN_TRIGGERS].GetInt()
              ^ (sc.Input[IN_ACCUM_BARS].GetInt()  << 8)
              ^ (sc.Input[IN_ALERT_SOUND].GetInt() << 16);

    bool settingsChanged = (storedFp0 != fp0 || storedFp1 != fp1 || storedFp2 != fp2);
    if (settingsChanged) {
        storedFp0 = fp0; storedFp1 = fp1; storedFp2 = fp2;
        // Force full recalc by resetting start index
        sc.UpdateStartIndex = 0;
    }

    const int startIdx = sc.UpdateStartIndex;
    const int lastBar  = sc.ArraySize - 1;

    // EMA multipliers (constant, computed once)
    const float kEMA50   = 2.0f / 51.0f;
    const float kEMA200  = 2.0f / 201.0f;
    const float kMACDFst = 2.0f / 10.0f;   // 9-period EMA
    const float kMACDSlw = 2.0f / 27.0f;   // 26-period EMA
    const float kMACDSig = 2.0f / 10.0f;   // 9-period signal EMA
    const float adxLen   = 14.0f;
    const int   rsiLen   = 14;
    const int   stochK   = 10;   // stochastic lookback
    const int   stochD   = 3;    // stochastic %D smoothing

    // =========================================================================
    // Main per-bar computation loop
    // =========================================================================
    for (int i = startIdx; i < sc.ArraySize; i++)
    {
        // ---------------------------------------------------------------
        // 1. Bar delta
        // ---------------------------------------------------------------
        sc.Subgraph[SG_DELTA][i] = sc.AskVolume[i] - sc.BidVolume[i];

        // ---------------------------------------------------------------
        // 2. Bollinger Bands on delta (Welford's single-pass mean+variance)
        // ---------------------------------------------------------------
        {
            int   bbStart  = (i >= bbLen) ? (i - bbLen + 1) : 0;
            int   n        = 0;
            float mean     = 0.0f, M2 = 0.0f;

            for (int k = bbStart; k <= i; k++) {
                n++;
                float x  = sc.Subgraph[SG_DELTA][k];
                float dw = x - mean;
                mean += dw / (float)n;
                M2   += dw * (x - mean);
            }

            float sd = (n > 1) ? sqrtf(M2 / (float)n) : 0.0f;
            sc.Subgraph[SG_BB_MIDDLE][i] = mean;
            sc.Subgraph[SG_BB_UPPER][i]  = mean + bbMult * sd;
            sc.Subgraph[SG_BB_LOWER][i]  = mean - bbMult * sd;
        }

        // ---------------------------------------------------------------
        // 3. Cumulative delta (running sum)
        // ---------------------------------------------------------------
        sc.Subgraph[SG_CUM_DELTA][i] = (i > 0)
            ? sc.Subgraph[SG_CUM_DELTA][i-1] + sc.Subgraph[SG_DELTA][i]
            : sc.Subgraph[SG_DELTA][i];

        // ---------------------------------------------------------------
        // 4. EMA50 and EMA200 on close
        // ---------------------------------------------------------------
        {
            float prev50  = (i > 0) ? sc.Subgraph[SG_EMA50][i-1]  : sc.Close[i];
            float prev200 = (i > 0) ? sc.Subgraph[SG_EMA200][i-1] : sc.Close[i];
            sc.Subgraph[SG_EMA50][i]  = sc.Close[i] * kEMA50  + prev50  * (1.0f - kEMA50);
            sc.Subgraph[SG_EMA200][i] = sc.Close[i] * kEMA200 + prev200 * (1.0f - kEMA200);
        }

        // ---------------------------------------------------------------
        // 5. MACD (9-EMA fast, 26-EMA slow, 9-EMA signal)
        // ---------------------------------------------------------------
        {
            float prevFst = (i > 0) ? sc.Subgraph[SG_MACD_FAST][i-1] : sc.Close[i];
            float prevSlw = (i > 0) ? sc.Subgraph[SG_MACD_SLOW][i-1] : sc.Close[i];
            sc.Subgraph[SG_MACD_FAST][i] = sc.Close[i] * kMACDFst + prevFst * (1.0f - kMACDFst);
            sc.Subgraph[SG_MACD_SLOW][i] = sc.Close[i] * kMACDSlw + prevSlw * (1.0f - kMACDSlw);
            float macdLineNow = sc.Subgraph[SG_MACD_FAST][i] - sc.Subgraph[SG_MACD_SLOW][i];
            float prevSig = (i > 0) ? sc.Subgraph[SG_MACD_SIG][i-1] : macdLineNow;
            sc.Subgraph[SG_MACD_SIG][i] = macdLineNow * kMACDSig + prevSig * (1.0f - kMACDSig);
        }

        // ---------------------------------------------------------------
        // 6. ADX (Wilder's 14,14)
        // ---------------------------------------------------------------
        if (i >= 1) {
            float prevH = sc.High[i-1], prevL = sc.Low[i-1], prevC = sc.Close[i-1];
            float tr = SC_MAX(sc.High[i] - sc.Low[i],
                      SC_MAX(fabs(sc.High[i] - prevC),
                             fabs(sc.Low[i]  - prevC)));

            float upMove   = sc.High[i] - prevH;
            float downMove = prevL      - sc.Low[i];
            float plusDM   = (upMove > downMove && upMove   > 0.0f) ? upMove   : 0.0f;
            float minusDM  = (downMove > upMove && downMove > 0.0f) ? downMove : 0.0f;

            if (i < (int)adxLen) {
                // Initial period: accumulate simple sums
                sc.Subgraph[SG_SMOOTH_TR][i]       = sc.Subgraph[SG_SMOOTH_TR][i-1]       + tr;
                sc.Subgraph[SG_SMOOTH_PLUS_DM][i]  = sc.Subgraph[SG_SMOOTH_PLUS_DM][i-1]  + plusDM;
                sc.Subgraph[SG_SMOOTH_MINUS_DM][i] = sc.Subgraph[SG_SMOOTH_MINUS_DM][i-1] + minusDM;
                sc.Subgraph[SG_ADX][i] = 0.0f;
            } else {
                float smTR   = sc.Subgraph[SG_SMOOTH_TR][i-1]
                               - sc.Subgraph[SG_SMOOTH_TR][i-1]       / adxLen + tr;
                float smPDM  = sc.Subgraph[SG_SMOOTH_PLUS_DM][i-1]
                               - sc.Subgraph[SG_SMOOTH_PLUS_DM][i-1]  / adxLen + plusDM;
                float smMDM  = sc.Subgraph[SG_SMOOTH_MINUS_DM][i-1]
                               - sc.Subgraph[SG_SMOOTH_MINUS_DM][i-1] / adxLen + minusDM;

                sc.Subgraph[SG_SMOOTH_TR][i]       = smTR;
                sc.Subgraph[SG_SMOOTH_PLUS_DM][i]  = smPDM;
                sc.Subgraph[SG_SMOOTH_MINUS_DM][i] = smMDM;

                float plusDI  = (smTR > 0.0f) ? 100.0f * smPDM / smTR : 0.0f;
                float minusDI = (smTR > 0.0f) ? 100.0f * smMDM / smTR : 0.0f;
                float diSum   = plusDI + minusDI;
                float dx      = (diSum > 0.0f) ? 100.0f * fabs(plusDI - minusDI) / diSum : 0.0f;

                if (i == (int)adxLen) {
                    sc.Subgraph[SG_ADX][i] = dx;
                } else {
                    sc.Subgraph[SG_ADX][i] = (sc.Subgraph[SG_ADX][i-1] * (adxLen - 1.0f) + dx)
                                             / adxLen;
                }
            }
        } else {
            sc.Subgraph[SG_SMOOTH_TR][i]       = 0.0f;
            sc.Subgraph[SG_SMOOTH_PLUS_DM][i]  = 0.0f;
            sc.Subgraph[SG_SMOOTH_MINUS_DM][i] = 0.0f;
            sc.Subgraph[SG_ADX][i]             = 0.0f;
        }

        // ---------------------------------------------------------------
        // 7. RSI (classic average gains/losses, 14-period)
        // ---------------------------------------------------------------
        {
            int   rsiStart = (i >= rsiLen) ? (i - rsiLen + 1) : 1;
            float gains    = 0.0f, losses = 0.0f;
            int   rsiN     = 0;
            for (int k = rsiStart; k <= i; k++) {
                float chg = sc.Close[k] - sc.Close[k-1];
                if (chg > 0.0f) gains  += chg;
                else            losses -= chg;  // losses positive
                rsiN++;
            }
            if (rsiN > 0) {
                float avgG = gains  / (float)rsiN;
                float avgL = losses / (float)rsiN;
                sc.Subgraph[SG_RSI][i] = (avgL > 0.0f)
                    ? 100.0f - 100.0f / (1.0f + avgG / avgL)
                    : (avgG > 0.0f ? 100.0f : 50.0f);
            } else {
                sc.Subgraph[SG_RSI][i] = 50.0f;
            }
        }

        // ---------------------------------------------------------------
        // 8. Stochastic %K (10-period) and %D (3-bar SMA of %K)
        // ---------------------------------------------------------------
        {
            int kStart = (i >= stochK) ? (i - stochK + 1) : 0;
            float hh = sc.High[kStart], ll = sc.Low[kStart];
            for (int k = kStart+1; k <= i; k++) {
                if (sc.High[k] > hh) hh = sc.High[k];
                if (sc.Low[k]  < ll) ll = sc.Low[k];
            }
            float range = hh - ll;
            sc.Subgraph[SG_STOCH_K][i] = (range > 0.0f)
                ? 100.0f * (sc.Close[i] - ll) / range
                : 50.0f;

            // %D = 3-bar SMA of %K
            float dSum = sc.Subgraph[SG_STOCH_K][i];
            int   dN   = 1;
            if (i >= 1) { dSum += sc.Subgraph[SG_STOCH_K][i-1]; dN++; }
            if (i >= 2) { dSum += sc.Subgraph[SG_STOCH_K][i-2]; dN++; }
            sc.Subgraph[SG_STOCH_D][i] = dSum / (float)dN;
        }

        // ---------------------------------------------------------------
        // 9. VPOC and Value Area (68%)
        // ---------------------------------------------------------------
        {
            float vpoc = 0.0f, vah = 0.0f, val = 0.0f;
            ComputeVPOCandVA(sc, i, vpoc, vah, val);
            sc.Subgraph[SG_VPOC][i] = vpoc;
            sc.Subgraph[SG_VAH][i]  = vah;
            sc.Subgraph[SG_VAL][i]  = val;
        }

        // ---------------------------------------------------------------
        // 10. Proximity computation for all 38 triggers
        //
        // Convention:
        //   BFLT(cond)    = 1.0f if cond true, 0.0f otherwise
        //   SAT(x)        = clamp to [0,1]
        //   proximity     = SAT(sum of weighted sub-conditions / total)
        //
        // For triggers that use delta/BB ratio as a continuous component,
        // the ratio approaches 1.0 as the trigger is about to fire.
        // ---------------------------------------------------------------

        // Safe look-back references (0-clamp to avoid out-of-bounds)
        float delta   = sc.Subgraph[SG_DELTA][i];
        float bbUpper = sc.Subgraph[SG_BB_UPPER][i];
        float bbLower = sc.Subgraph[SG_BB_LOWER][i];
        float bbWidth = bbUpper - bbLower;
        float vpoc    = sc.Subgraph[SG_VPOC][i];
        float vah     = sc.Subgraph[SG_VAH][i];
        float val_    = sc.Subgraph[SG_VAL][i];

        // Helper: safe previous bar access
        float delta1    = (i>0)?sc.Subgraph[SG_DELTA][i-1]:0.f;
        float delta2    = (i>1)?sc.Subgraph[SG_DELTA][i-2]:0.f;
        float delta3    = (i>2)?sc.Subgraph[SG_DELTA][i-3]:0.f;
        float bbU1      = (i>0)?sc.Subgraph[SG_BB_UPPER][i-1]:bbUpper;
        float bbL1      = (i>0)?sc.Subgraph[SG_BB_LOWER][i-1]:bbLower;
        float bbU2      = (i>1)?sc.Subgraph[SG_BB_UPPER][i-2]:bbUpper;
        float bbL2      = (i>1)?sc.Subgraph[SG_BB_LOWER][i-2]:bbLower;
        float vpoc1     = (i>0)?sc.Subgraph[SG_VPOC][i-1]:vpoc;
        float vpoc2     = (i>1)?sc.Subgraph[SG_VPOC][i-2]:vpoc;
        float vah1      = (i>0)?sc.Subgraph[SG_VAH][i-1]:vah;
        float vah2      = (i>1)?sc.Subgraph[SG_VAH][i-2]:vah;
        float val1_     = (i>0)?sc.Subgraph[SG_VAL][i-1]:val_;
        float val2_     = (i>1)?sc.Subgraph[SG_VAL][i-2]:val_;
        float cumD1     = (i>0)?sc.Subgraph[SG_CUM_DELTA][i-1]:0.f;

        float hi  = sc.High[i],    lo  = sc.Low[i];
        float cl  = sc.Close[i],   op  = sc.Open[i];
        float hi1 = (i>0)?sc.High[i-1]:hi;
        float lo1 = (i>0)?sc.Low[i-1]:lo;
        float cl1 = (i>0)?sc.Close[i-1]:cl;
        float op1 = (i>0)?sc.Open[i-1]:op;
        float hi2 = (i>1)?sc.High[i-2]:hi;
        float lo2 = (i>1)?sc.Low[i-2]:lo;
        float cl2 = (i>1)?sc.Close[i-2]:cl;
        float op2 = (i>1)?sc.Open[i-2]:op;
        float cl3 = (i>2)?sc.Close[i-3]:cl;
        float op3 = (i>2)?sc.Open[i-3]:op;

        float adx    = sc.Subgraph[SG_ADX][i];
        float rsi    = sc.Subgraph[SG_RSI][i];
        float stochK_= sc.Subgraph[SG_STOCH_K][i];
        float stochD_= sc.Subgraph[SG_STOCH_D][i];
        float stochK1= (i>0)?sc.Subgraph[SG_STOCH_K][i-1]:stochK_;
        float stochD1= (i>0)?sc.Subgraph[SG_STOCH_D][i-1]:stochD_;
        float macdL  = sc.Subgraph[SG_MACD_FAST][i] - sc.Subgraph[SG_MACD_SLOW][i];
        float macdS  = sc.Subgraph[SG_MACD_SIG][i];
        float ema50  = sc.Subgraph[SG_EMA50][i];
        float ema200 = sc.Subgraph[SG_EMA200][i];
        float cumD   = sc.Subgraph[SG_CUM_DELTA][i];

        float barRange = hi - lo;

        // BB near-zero guard — zero all proxies if BB is degenerate
        if (bbWidth < minBBWidth) {
            for (int t = 0; t < 38; t++)
                sc.Subgraph[PROX_SG[t]][i] = 0.0f;
            sc.Subgraph[SG_ALERT_LONG][i]  = 0.0f;
            sc.Subgraph[SG_ALERT_SHORT][i] = 0.0f;
            continue;
        }

        // Normalized position of delta within the BB band, relative to midline.
        // ratioLong  = 0 at BB_Middle, 1 at BB_Upper  (band-sign agnostic)
        // ratioShort = 0 at BB_Middle, 1 at BB_Lower
        // Using bbMiddle+halfWidth avoids the old bbUpper>0 guard that zeroed
        // all long ratios whenever the mean delta was negative (selling market).
        float bbMid      = sc.Subgraph[SG_BB_MIDDLE][i];
        float bbHW       = bbWidth * 0.5f;   // > 0 guaranteed by minBBWidth guard above
        float ratioLong  = (delta > bbMid)  ? SAT((delta  - bbMid) / bbHW) : 0.0f;
        float ratioShort = (delta < bbMid)  ? SAT((bbMid  - delta) / bbHW) : 0.0f;

        float bbMid1     = (i>0) ? sc.Subgraph[SG_BB_MIDDLE][i-1] : bbMid;
        float bbW1       = bbU1 - bbL1;
        float bbHW1      = (bbW1 > 0.0f) ? bbW1 * 0.5f : bbHW;
        float ratioL1    = (delta1 > bbMid1) ? SAT((delta1 - bbMid1) / bbHW1) : 0.0f;
        float ratioS1    = (delta1 < bbMid1) ? SAT((bbMid1 - delta1) / bbHW1) : 0.0f;

        float bbMid2     = (i>1) ? sc.Subgraph[SG_BB_MIDDLE][i-2] : bbMid;
        float bbW2       = bbU2 - bbL2;
        float bbHW2      = (bbW2 > 0.0f) ? bbW2 * 0.5f : bbHW;
        float ratioL2    = (delta2 > bbMid2) ? SAT((delta2 - bbMid2) / bbHW2) : 0.0f;
        float ratioS2    = (delta2 < bbMid2) ? SAT((bbMid2 - delta2) / bbHW2) : 0.0f;

        // --- T01: Fading MOMO Bull (delta rising 3 bars but still negative) ---
        sc.Subgraph[SG_PROX_T01][i] = SAT(
            (BFLT(delta2 < delta1) + BFLT(delta1 < delta) + BFLT(delta < 0.0f)) / 3.0f);

        // --- T02: Fading MOMO Bear ---
        sc.Subgraph[SG_PROX_T02][i] = SAT(
            (BFLT(delta2 > delta1) + BFLT(delta1 > delta) + BFLT(delta > 0.0f)) / 3.0f);

        // --- T03: Delta Rise Bull (4 bars rising, positive) ---
        sc.Subgraph[SG_PROX_T03][i] = SAT(
            (BFLT(delta3 < delta2) + BFLT(delta2 < delta1) + BFLT(delta1 < delta)
             + BFLT(delta > 0.0f)) / 4.0f);

        // --- T04: Delta Drop Bear ---
        sc.Subgraph[SG_PROX_T04][i] = SAT(
            (BFLT(delta3 > delta2) + BFLT(delta2 > delta1) + BFLT(delta1 > delta)
             + BFLT(delta < 0.0f)) / 4.0f);

        // --- T05: BPOC Bull (bullish bar, VPOC in lower 30% of range) ---
        {
            float vpocZone = lo + barRange * 0.30f;
            sc.Subgraph[SG_PROX_T05][i] = SAT(
                (BFLT(cl > op) + BFLT(vpoc >= lo) + BFLT(vpoc <= vpocZone)) / 3.0f);
        }

        // --- T06: BPOC Bear (bearish bar, VPOC in upper 30% of range) ---
        {
            float vpocZone = hi - barRange * 0.30f;
            sc.Subgraph[SG_PROX_T06][i] = SAT(
                (BFLT(cl < op) + BFLT(vpoc <= hi) + BFLT(vpoc >= vpocZone)) / 3.0f);
        }

        // --- T07: EXH Bull (3 consecutive bear bars then bull bar) ---
        sc.Subgraph[SG_PROX_T07][i] = SAT(
            (BFLT(cl3 < op3) + BFLT(cl2 < op2) + BFLT(cl1 < op1) + BFLT(cl > op)) / 4.0f);

        // --- T08: EXH Bear ---
        sc.Subgraph[SG_PROX_T08][i] = SAT(
            (BFLT(cl3 > op3) + BFLT(cl2 > op2) + BFLT(cl1 > op1) + BFLT(cl < op)) / 4.0f);

        // --- T09: VOL SEQ + Delta DIV Bull (partial: bull close + neg delta) ---
        sc.Subgraph[SG_PROX_T09][i] = SAT(
            (BFLT(delta < 0.0f) + BFLT(cl > op)) / 2.0f);

        // --- T10: VOL SEQ + Delta DIV Bear ---
        sc.Subgraph[SG_PROX_T10][i] = SAT(
            (BFLT(delta > 0.0f) + BFLT(cl < op)) / 2.0f);

        // --- T11: VA Long (VA expanding, bullish close above prior close) ---
        sc.Subgraph[SG_PROX_T11][i] = SAT(
            (BFLT(vah > vah1) + BFLT(val_ < val1_) + BFLT(cl > op) + BFLT(cl > cl1)) / 4.0f);

        // --- T12: VA Short ---
        sc.Subgraph[SG_PROX_T12][i] = SAT(
            (BFLT(vah > vah1) + BFLT(val_ < val1_) + BFLT(cl < op) + BFLT(cl < cl1)) / 4.0f);

        // --- T13: Delta Slingshot Buy ★★ ---
        // Setup: delta[-2] hit bear extreme; [-1] bearish then reversed
        // Continuous: ratio of delta to bbUpper (approaches 1 as slingshot fires)
        {
            float c1 = BFLT(delta2 <= bbL2);   // hit bear extreme 2 bars ago
            float c2 = BFLT(delta1 < 0.0f);    // still negative last bar
            float c3 = BFLT(cl1 > op1);        // bullish reversal bar
            // c4 = continuous ratio toward bbUpper (firing condition)
            float c4 = ratioLong;
            sc.Subgraph[SG_PROX_T13][i] = SAT((c1 + c2 + c3 + c4) / 4.0f);
        }

        // --- T14: Delta Slingshot Sell ★★ ---
        {
            float c1 = BFLT(delta2 >= bbU2);
            float c2 = BFLT(delta1 > 0.0f);
            float c3 = BFLT(cl1 < op1);
            float c4 = ratioShort;
            sc.Subgraph[SG_PROX_T14][i] = SAT((c1 + c2 + c3 + c4) / 4.0f);
        }

        // --- T15: POC Delta Bull (open below VPOC, close above after bear prior) ---
        {
            float c1 = BFLT(vpoc > op);        // VPOC above open
            float c2 = BFLT(cl > op);
            float c3 = BFLT(cl > cl1);
            float c4 = BFLT(cl1 < op1);        // prior bar bearish
            float c5 = ratioLong;               // delta approaching bull extreme
            sc.Subgraph[SG_PROX_T15][i] = SAT((c1 + c2 + c3 + c4 + c5) / 5.0f);
        }

        // --- T16: POC Delta Bear ---
        {
            float c1 = BFLT(vpoc < op);
            float c2 = BFLT(cl < op);
            float c3 = BFLT(cl < cl1);
            float c4 = BFLT(cl1 > op1);
            float c5 = ratioShort;
            sc.Subgraph[SG_PROX_T16][i] = SAT((c1 + c2 + c3 + c4 + c5) / 5.0f);
        }

        // --- T19: Delta Trap Bull ★★ ---
        // Bear bar with extreme neg delta, then 2 bull bars recover more delta,
        // Value Area shifting up
        {
            float c1 = BFLT(cl2 < op2);                         // bear bar 2 ago
            float c2 = BFLT(delta2 <= bbL2);                    // extreme neg delta
            float c3 = BFLT(cl1 > op1);                         // first recovery
            float c4 = BFLT(delta1 > 0.0f);
            float c5 = BFLT(cl  > op);                          // second recovery
            float c6 = BFLT(delta > 0.0f);
            float c7 = BFLT((delta1 + delta) > fabs(delta2));   // recovered more than dumped
            float c8 = BFLT(vah > vah1);                        // VA shifting up
            float c9 = BFLT(val_ > val1_);
            sc.Subgraph[SG_PROX_T19][i] = SAT(
                (c1+c2+c3+c4+c5+c6+c7+c8+c9) / 9.0f);
        }

        // --- T20: Delta Trap Bear ★★ ---
        {
            float c1 = BFLT(cl2 > op2);
            float c2 = BFLT(delta2 >= bbU2);
            float c3 = BFLT(cl1 < op1);
            float c4 = BFLT(delta1 < 0.0f);
            float c5 = BFLT(cl  < op);
            float c6 = BFLT(delta < 0.0f);
            float c7 = BFLT((delta1 + delta) < -fabs(delta2));
            float c8 = BFLT(vah < vah1);
            float c9 = BFLT(val_ < val1_);
            sc.Subgraph[SG_PROX_T20][i] = SAT(
                (c1+c2+c3+c4+c5+c6+c7+c8+c9) / 9.0f);
        }

        // --- T21: POCL Bull ★★ (stable VPOC, bear-bull-bull, extreme delta, VA expanding up) ---
        {
            float vpocStable = BFLT(fabs(vpoc1 - vpoc2) < sc.TickSize * 0.5f);
            float c1 = BFLT(cl2 < op2);
            float c2 = BFLT(cl1 > op1);
            float c3 = BFLT(cl  > op);
            float c4 = vpocStable;
            float c5 = BFLT(delta1 > 0.0f);
            float c6 = BFLT(delta  > 0.0f);
            float c7 = BFLT(ratioL1 >= coilThresh || ratioLong >= coilThresh); // extreme delta
            float c8 = BFLT(vah > vah1);
            float c9 = BFLT(val_ > val1_);
            sc.Subgraph[SG_PROX_T21][i] = SAT(
                (c1+c2+c3+c4+c5+c6+c7+c8+c9) / 9.0f);
        }

        // --- T22: POCS Bear ★★ ---
        {
            float vpocStable = BFLT(fabs(vpoc1 - vpoc2) < sc.TickSize * 0.5f);
            float c1 = BFLT(cl2 > op2);
            float c2 = BFLT(cl1 < op1);
            float c3 = BFLT(cl  < op);
            float c4 = vpocStable;
            float c5 = BFLT(delta1 < 0.0f);
            float c6 = BFLT(delta  < 0.0f);
            float c7 = BFLT(ratioS1 >= coilThresh || ratioShort >= coilThresh);
            float c8 = BFLT(vah < vah1);
            float c9 = BFLT(val_ < val1_);
            sc.Subgraph[SG_PROX_T22][i] = SAT(
                (c1+c2+c3+c4+c5+c6+c7+c8+c9) / 9.0f);
        }

        // --- T23: BR + Delta DIV Bull (partial: neg delta + bull close) ---
        sc.Subgraph[SG_PROX_T23][i] = SAT(
            (BFLT(delta < 0.0f) + BFLT(cl > op)) / 2.0f);

        // --- T24: BR + Delta DIV Bear ---
        sc.Subgraph[SG_PROX_T24][i] = SAT(
            (BFLT(delta > 0.0f) + BFLT(cl < op)) / 2.0f);

        // --- T25: SVOL Bull (partial: bull close, VPOC in bottom 20%) ---
        {
            float zone = lo + barRange * 0.20f;
            sc.Subgraph[SG_PROX_T25][i] = SAT(
                (BFLT(cl > op) + BFLT(vpoc >= lo) + BFLT(vpoc <= zone)) / 3.0f);
        }

        // --- T26: SVOL Bear ---
        {
            float zone = hi - barRange * 0.20f;
            sc.Subgraph[SG_PROX_T26][i] = SAT(
                (BFLT(cl < op) + BFLT(vpoc <= hi) + BFLT(vpoc >= zone)) / 3.0f);
        }

        // --- T27: OF Long v1 ★★ (Renko 6t — same delta/BB/VPOC logic as T13) ---
        // delta hit bear extreme in last 1 or 2 bars; now approaching bull extreme; close > prior VPOC
        {
            float springLoaded = BFLT(delta1 <= bbL1 || delta2 <= bbL2);
            float c2 = ratioLong;                    // continuous: approaching bbUpper
            float c3 = BFLT(cl > vpoc1);             // close above prior VPOC
            sc.Subgraph[SG_PROX_T27][i] = SAT(
                (springLoaded + c2 + c3) / 3.0f);
        }

        // --- T28: OF Long v2 ★★ (same as T27, vol-at-price threshold omitted) ---
        sc.Subgraph[SG_PROX_T28][i] = sc.Subgraph[SG_PROX_T27][i];

        // --- T29: OF Short v1 ★★ ---
        {
            float springLoaded = BFLT(delta1 >= bbU1 || delta2 >= bbU2);
            float c2 = ratioShort;
            float c3 = BFLT(cl < vpoc1);
            sc.Subgraph[SG_PROX_T29][i] = SAT(
                (springLoaded + c2 + c3) / 3.0f);
        }

        // --- T30: OF Short v2 ★★ (same as T29) ---
        sc.Subgraph[SG_PROX_T30][i] = sc.Subgraph[SG_PROX_T29][i];

        // --- T31: FA+ Bull (Failed Auction Long) ---
        // Bull bar after bearish, declining prior highs, close > prior VPOC, delta reversal
        {
            float c1 = BFLT(cl > op);
            float c2 = BFLT(cl1 <= op1);
            float c3 = BFLT(hi2 > hi1);           // declining highs
            float c4 = BFLT(cl > vpoc1);
            float c5 = BFLT(delta1 <= bbL1 || delta2 <= bbL2);  // spring loaded
            float c6 = ratioLong;                  // continuous toward bbUpper
            sc.Subgraph[SG_PROX_T31][i] = SAT((c1+c2+c3+c4+c5+c6) / 6.0f);
        }

        // --- T32: FA- Bear ---
        {
            float c1 = BFLT(cl < op);
            float c2 = BFLT(cl1 >= op1);
            float c3 = BFLT(lo2 < lo1);           // rising lows
            float c4 = BFLT(cl < vpoc1);
            float c5 = BFLT(delta1 >= bbU1 || delta2 >= bbU2);
            float c6 = ratioShort;
            sc.Subgraph[SG_PROX_T32][i] = SAT((c1+c2+c3+c4+c5+c6) / 6.0f);
        }

        // --- T33: Long Trigger 8t ★★ ---
        // MACD bullish, SMI structure (proxy: RSI < 50), EMA50 > EMA200,
        // ADX > 20, close > prior VPOC, SMI gate (proxy: RSI < 40)
        {
            float c1 = BFLT(macdL > macdS);        // MACD bullish cross
            float c2 = BFLT(rsi < 50.0f);          // RSI below mid (SMI proxy)
            float c3 = BFLT(ema50 > ema200);        // uptrend
            float c4 = BFLT(adx > 20.0f);          // trending
            float c5 = BFLT(cl > vpoc1);            // above prior VPOC
            float c6 = BFLT(rsi < 40.0f);          // oversold gate (SMI ±60 proxy)
            sc.Subgraph[SG_PROX_T33][i] = SAT((c1+c2+c3+c4+c5+c6) / 6.0f);
        }

        // --- T34: Short Trigger 8t ★★ ---
        {
            float c1 = BFLT(macdL < macdS);
            float c2 = BFLT(rsi > 50.0f);
            float c3 = BFLT(ema50 < ema200);
            float c4 = BFLT(adx > 20.0f);
            float c5 = BFLT(cl < vpoc1);
            float c6 = BFLT(rsi > 60.0f);
            sc.Subgraph[SG_PROX_T34][i] = SAT((c1+c2+c3+c4+c5+c6) / 6.0f);
        }

        // --- T35: T BUY ---
        // ADX > 25, stoch < 20, stoch crossing up from below, cumDelta rising, RSI gate
        {
            float c1 = BFLT(adx > 25.0f);
            float c2 = BFLT(stochK_ < 20.0f);
            float c3 = BFLT(stochK_ > stochD_ && stochK1 <= stochD1); // crossover
            float c4 = BFLT(cumD > cumD1);
            float c5 = BFLT(rsi < 40.0f);
            sc.Subgraph[SG_PROX_T35][i] = SAT((c1+c2+c3+c4+c5) / 5.0f);
        }

        // --- T36: T SELL ---
        {
            float c1 = BFLT(adx > 25.0f);
            float c2 = BFLT(stochK_ > 80.0f);
            float c3 = BFLT(stochK_ < stochD_ && stochK1 >= stochD1);
            float c4 = BFLT(cumD < cumD1);
            float c5 = BFLT(rsi > 60.0f);
            sc.Subgraph[SG_PROX_T36][i] = SAT((c1+c2+c3+c4+c5) / 5.0f);
        }

        // --- T37: R BUY ---
        // ADX < 20 (ranging), RSI oversold, bid vol dominance, RSI gate
        {
            float c1 = BFLT(adx < 20.0f);
            float c2 = BFLT(rsi < 30.0f);
            float c3 = BFLT(sc.BidVolume[i] > sc.AskVolume[i] * 2.5f);
            float c4 = BFLT(rsi < 40.0f);
            sc.Subgraph[SG_PROX_T37][i] = SAT((c1+c2+c3+c4) / 4.0f);
        }

        // --- T38: R SELL ---
        {
            float c1 = BFLT(adx < 20.0f);
            float c2 = BFLT(rsi > 70.0f);
            float c3 = BFLT(sc.AskVolume[i] > sc.BidVolume[i] * 2.5f);
            float c4 = BFLT(rsi > 60.0f);
            sc.Subgraph[SG_PROX_T38][i] = SAT((c1+c2+c3+c4) / 4.0f);
        }

        // --- T39: POC Wave Bull ★★ ---
        // Bear-bull-bull, VPOC dips then recovers above 2-bar-prior level
        {
            float c1 = BFLT(cl2 < op2);
            float c2 = BFLT(cl1 > op1);
            float c3 = BFLT(cl  > op);
            float c4 = BFLT(vpoc1 < vpoc2);      // VPOC dipped
            float c5 = BFLT(vpoc  > vpoc2);      // VPOC recovered above 2-bar prior
            sc.Subgraph[SG_PROX_T39][i] = SAT((c1+c2+c3+c4+c5) / 5.0f);
        }

        // --- T40: POC Wave Bear ★★ ---
        {
            float c1 = BFLT(cl2 > op2);
            float c2 = BFLT(cl1 < op1);
            float c3 = BFLT(cl  < op);
            float c4 = BFLT(vpoc1 > vpoc2);
            float c5 = BFLT(vpoc  < vpoc2);
            sc.Subgraph[SG_PROX_T40][i] = SAT((c1+c2+c3+c4+c5) / 5.0f);
        }

        // ---------------------------------------------------------------
        // 11. Count triggers in coiling zone with accumulation window
        //     A trigger counts if proximity >= coilThresh on ANY bar
        //     within the last accumBars bars.
        // ---------------------------------------------------------------
        int longCount  = 0;
        int shortCount = 0;
        int accumStart = (i - accumBars + 1 > 0) ? (i - accumBars + 1) : 0;

        for (int t = 0; t < 38; t++) {
            int sgIdx = PROX_SG[t];
            bool triggered = false;
            for (int k = i; k >= accumStart && !triggered; k--) {
                if (sc.Subgraph[sgIdx][k] >= coilThresh) triggered = true;
            }
            if (triggered) {
                if (TRIGGER_DIR[t] == 1) longCount++;
                else                     shortCount++;
            }
        }

        sc.Subgraph[SG_ALERT_LONG][i]  = (longCount  >= minTrig) ? lo : 0.0f;
        sc.Subgraph[SG_ALERT_SHORT][i] = (shortCount >= minTrig) ? hi : 0.0f;

    }  // end main per-bar loop

    // =========================================================================
    // 12. Rising-edge alert — post-loop watermark (CLAUDE.md standard pattern)
    //     Alert fires once at the ONSET of a coiling condition, not sustained.
    //     Anchored to lastBar (current bar), not the historical coiling bar.
    // =========================================================================
    const int scanBars = SC_MAX(10, accumBars + 2);

    // Long alert
    for (int i = lastBar; i >= SC_MAX(0, lastBar - scanBars); i--) {
        if (sc.Subgraph[SG_ALERT_LONG][i] != 0.0f && i > alertedLong) {
            // Rising edge: previous bar was NOT coiling
            bool prevCoiling = (i > 0 && sc.Subgraph[SG_ALERT_LONG][i-1] != 0.0f);
            if (!prevCoiling) {
                sc.SetAlert(soundID, lastBar, "CoilState: Long onset");
                alertedLong = i;
                break;
            }
        }
    }

    // Short alert
    for (int i = lastBar; i >= SC_MAX(0, lastBar - scanBars); i--) {
        if (sc.Subgraph[SG_ALERT_SHORT][i] != 0.0f && i > alertedShort) {
            bool prevCoiling = (i > 0 && sc.Subgraph[SG_ALERT_SHORT][i-1] != 0.0f);
            if (!prevCoiling) {
                sc.SetAlert(soundID, lastBar, "CoilState: Short onset");
                alertedShort = i;
                break;
            }
        }
    }
}
