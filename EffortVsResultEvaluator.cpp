// =============================================================================
// EffortVsResultEvaluator.cpp
// Sierra Chart ACSIL Custom Study — Effort vs Result Evaluator v1.0
//
// PURPOSE:
//   Standalone companion event-study evaluator for EffortVsResult.cpp. It is
//   NOT a trading system and never sends orders. It reads the canonical
//   source study arrays on the same chart and measures, for every eligible
//   confirmation event, the forward outcome of a frozen entry/stop/target
//   over a time-based holding horizon.
//
// SOURCE SG MAP (EffortVsResult study, read-only, never recomputed here):
//   SG:2  Normalized Effort
//   SG:5  Raw Signed Failure
//   SG:6  Smoothed Signed Failure
//   SG:7  Buyer Failure Pulse
//   SG:8  Seller Failure Pulse
//   SG:9  Buyer Failure Arrow
//   SG:10 Seller Failure Arrow
//
// EVENT MODEL:
//   Signals come only from source arrays at confirmation bar j. Candidate
//   bar is exactly j-1. Buyer failure = short (side -1); seller failure =
//   long (side +1). Both sides nonzero on one bar => conflict, not evaluated.
//   The forming bar is never a signal or outcome source.
//
// INPUTS (0-based):
//   In:0  Effort vs Result Source (Study/Subgraph; Study ID used, SG7 default)
//   In:1  Event Source (Pulse; Arrow; Both Deduplicated)      default 0
//   In:2  Entry Mode (Next Bar Open; Confirmation Close)      default 0
//   In:3  Stop Buffer Ticks            int    default 2, 0..100
//   In:4  Target R Multiple            float  default 1.0, 0.1..20
//   In:5  Maximum Hold Minutes         int    default 15, 1..1440
//   In:6  Ambiguous Bar Policy (Mark/Exclude; Stop First; Target First)
//   In:7  Overlapping Event Policy (Include+Flag; Exclude)
//   In:8  Enable Session Filter        yes/no default 0
//   In:9  Session Start (chart TZ)     time   default 09:30:00
//   In:10 Session End (chart TZ)       time   default 16:00:00
//   In:11 Evaluation Start Date YYYYMMDD (0=all) int default 0
//   In:12 Evaluation End Date YYYYMMDD (0=all)   int default 0
//   In:13 Export CSV on Full Recalculation       yes/no default 0
//   In:14 CSV File Prefix              string default "EVR_Evaluator"
//
// SUBGRAPHS (0-based, all DRAWSTYLE_IGNORE by default):
//   SG:0  Event Side (-1 buyer/short, +1 seller/long)
//   SG:1  Evaluated Flag (1 = included in aggregates)
//   SG:2  Outcome R
//   SG:3  MFE R
//   SG:4  MAE R
//   SG:5  Entry Price
//   SG:6  Stop Price
//   SG:7  Target Price
//   SG:8  Holding Seconds
//   SG:9  Status Code
//   SG:10 Signal Strength (pulse |value| preferred, else 0)
//   SG:11 Candidate Normalized Effort
//   SG:12 Candidate Raw Signed Failure
//   SG:13 Candidate Smoothed Signed Failure
//   SG:14 Overlap Flag
//   SG:15 Cumulative Included Events
//   SG:16 Cumulative Average R
//   SG:17 Cumulative Win Rate Percent
//
// STATUS CODES (stable):
//    1 target
//   -1 stop
//    2 timeout
//    3 ambiguous (both touches one bar, Mark/Exclude policy)
//   -2 invalid risk (entry at/beyond stop, or non-positive risk/tick)
//   -3 excluded overlap (Exclude While Prior Active policy)
//   -4 insufficient future data (right-edge censoring, never a timeout)
//   -5 conflicting sides (both channels nonzero on one bar)
//    0 no event at this bar
//
// PERSISTENT SLOTS:
//   Int 4-6 : settings fingerprint words (structural inputs only; CSV
//             enable/prefix excluded)
//   No heap state, no persistent pointer.
//
// AutoLoop: 0 (manual loop; full recalc covers history deterministically)
// =============================================================================

#ifndef EVR_EVALUATOR_TEST
#include "sierrachart.h"
#endif
#include <cmath>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Portable core: pure helpers shared by the ACSIL body and the Linux tests.
// No Sierra headers, no STL, no static mutable state.
// ---------------------------------------------------------------------------

#define EVRE_ST_NONE 0
#define EVRE_ST_TARGET 1
#define EVRE_ST_STOP -1
#define EVRE_ST_TIMEOUT 2
#define EVRE_ST_AMBIGUOUS 3
#define EVRE_ST_INVALID_RISK -2
#define EVRE_ST_EXCLUDED_OVERLAP -3
#define EVRE_ST_INSUFFICIENT -4
#define EVRE_ST_CONFLICT -5

#define EVRE_KIND_NONE 0
#define EVRE_KIND_PULSE 1
#define EVRE_KIND_ARROW 2
#define EVRE_KIND_BOTH 3

struct EvrOutcome
{
    int status;       // one of EVRE_ST_*
    double outcomeR;  // R multiple (0 for excluded statuses)
    double mfeR;      // >= 0
    double maeR;      // <= 0
    int exitBar;      // outcome/exit bar index, -1 when none
    double holdingSec;// exit start time minus traversal-start time
    int evaluated;    // 1 when included in performance aggregates
};

// Days per month helper (proleptic Gregorian).
static int EvrDaysInMonth(int year, int month)
{
    static const int kDays[12] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    if (month != 2)
        return kDays[month - 1];
    const int leap =
        (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
}

// 0 means unbounded and is always valid. Nonzero must be a real calendar
// date in YYYYMMDD form. Returns 1 when valid, 0 when invalid.
static int EvrValidYYYYMMDD(int v)
{
    if (v == 0)
        return 1;
    if (v < 19000101 || v > 21001231)
        return 0;
    const int day = v % 100;
    const int month = (v / 100) % 100;
    const int year = v / 10000;
    if (month < 1 || month > 12)
        return 0;
    const int dim = EvrDaysInMonth(year, month);
    if (day < 1 || day > dim)
        return 0;
    // Recompose to reject values with junk in unused digit ranges.
    if (year * 10000 + month * 100 + day != v)
        return 0;
    return 1;
}

// Date bounds: 0 on either bound means unbounded on that side.
static int EvrDateEligible(int barDate, int startDate, int endDate)
{
    if (startDate != 0 && barDate < startDate)
        return 0;
    if (endDate != 0 && barDate > endDate)
        return 0;
    return 1;
}

// Session filter on seconds-since-midnight in the chart time zone.
// Supports windows crossing midnight. start == end means full-day (always
// eligible) so a zeroed input pair can never silently filter everything.
static int EvrSessionEligible(int todSec, int enabled, int startSec, int endSec)
{
    if (!enabled)
        return 1;
    int t = todSec % 86400;
    if (t < 0)
        t += 86400;
    int s = startSec % 86400;
    if (s < 0)
        s += 86400;
    int e = endSec % 86400;
    if (e < 0)
        e += 86400;
    if (s == e)
        return 1;
    if (s < e)
        return (t >= s && t < e) ? 1 : 0;
    return (t >= s || t < e) ? 1 : 0;
}

// Event selection from one confirmation bar's source channels.
// Buyer failure => side -1 (short). Seller failure => side +1 (long).
// Signal presence is a nonzero check written without float equality.
// Strength is the pulse absolute value when a pulse is present; an
// arrow-only event reports 0 (arrow prices are not a strength).
// Returns 1 when exactly one side has an event, 0 otherwise (none or
// conflict). Conflict is reported via outConflict.
static int EvrSelectEvent(float pulseBuy, float pulseSell,
                          float arrowBuy, float arrowSell,
                          int eventSource,
                          int* outSide, int* outKind,
                          float* outStrength, int* outConflict)
{
    const int hasPB = (pulseBuy < 0.0f || pulseBuy > 0.0f) ? 1 : 0;
    const int hasPS = (pulseSell < 0.0f || pulseSell > 0.0f) ? 1 : 0;
    const int hasAB = (arrowBuy < 0.0f || arrowBuy > 0.0f) ? 1 : 0;
    const int hasAS = (arrowSell < 0.0f || arrowSell > 0.0f) ? 1 : 0;

    int buyEv = 0;
    int sellEv = 0;
    int kindB = EVRE_KIND_NONE;
    int kindS = EVRE_KIND_NONE;

    if (eventSource == 1)
    {
        buyEv = hasAB;
        sellEv = hasAS;
        kindB = EVRE_KIND_ARROW;
        kindS = EVRE_KIND_ARROW;
    }
    else if (eventSource == 2)
    {
        buyEv = (hasPB || hasAB) ? 1 : 0;
        sellEv = (hasPS || hasAS) ? 1 : 0;
        if (hasPB && hasAB)
            kindB = EVRE_KIND_BOTH;
        else if (hasPB)
            kindB = EVRE_KIND_PULSE;
        else
            kindB = EVRE_KIND_ARROW;
        if (hasPS && hasAS)
            kindS = EVRE_KIND_BOTH;
        else if (hasPS)
            kindS = EVRE_KIND_PULSE;
        else
            kindS = EVRE_KIND_ARROW;
    }
    else
    {
        buyEv = hasPB;
        sellEv = hasPS;
        kindB = EVRE_KIND_PULSE;
        kindS = EVRE_KIND_PULSE;
    }

    if (buyEv && sellEv)
    {
        if (outSide)
            *outSide = 0;
        if (outKind)
            *outKind = EVRE_KIND_NONE;
        if (outStrength)
            *outStrength = 0.0f;
        if (outConflict)
            *outConflict = 1;
        return 0;
    }
    if (outConflict)
        *outConflict = 0;
    if (buyEv)
    {
        if (outSide)
            *outSide = -1;
        if (outKind)
            *outKind = kindB;
        if (outStrength)
            *outStrength = hasPB ? (float)fabs((double)pulseBuy) : 0.0f;
        return 1;
    }
    if (sellEv)
    {
        if (outSide)
            *outSide = 1;
        if (outKind)
            *outKind = kindS;
        if (outStrength)
            *outStrength = hasPS ? (float)fabs((double)pulseSell) : 0.0f;
        return 1;
    }
    if (outSide)
        *outSide = 0;
    if (outKind)
        *outKind = EVRE_KIND_NONE;
    if (outStrength)
        *outStrength = 0.0f;
    return 0;
}

// Stop/target construction. Entry price is supplied by the caller (open of
// j+1 for Next Bar Open, close of j for Confirmation Close).
// Returns 1 on success, 0 when risk is invalid (non-positive tick size,
// bad side, non-positive risk distance, or a stop on the wrong side of the
// entry for the requested direction: long stops must sit below the entry,
// short stops above it).
static int EvrComputeTrade(int side, double candHigh, double candLow,
                           double entryPrice, double tickSize, int bufTicks,
                           double targetR,
                           double* outStop, double* outTarget, double* outRisk)
{
    if (side != 1 && side != -1)
        return 0;
    if (!(tickSize > 0.0))
        return 0;
    if (!(targetR > 0.0))
        return 0;
    if (bufTicks < 0)
        bufTicks = 0;
    if (bufTicks > 100)
        bufTicks = 100;
    const double buf = (double)bufTicks * tickSize;
    double stop = (side < 0) ? (candHigh + buf) : (candLow - buf);
    double risk = 0.0;
    if (side > 0)
    {
        if (!(stop < entryPrice))
            return 0;
        risk = entryPrice - stop;
    }
    else
    {
        if (!(stop > entryPrice))
            return 0;
        risk = stop - entryPrice;
    }
    if (!(risk > 0.0))
        return 0;
    double target = (side > 0) ? (entryPrice + targetR * risk)
                               : (entryPrice - targetR * risk);
    if (outStop)
        *outStop = stop;
    if (outTarget)
        *outTarget = target;
    if (outRisk)
        *outRisk = risk;
    return 1;
}

// Forward outcome scan over CLOSED bars only, starting at firstBar (j+1)
// regardless of entry mode. Only bars whose start timestamp is within
// horizonSec of the traversal-start bar are scanned. Right-edge censoring
// (chart data ends before the horizon fully elapses) reports
// EVRE_ST_INSUFFICIENT, never a timeout.
// Price touches use >= / <= only; no float equality on prices.
// Returns 1 when an outcome was determined, 0 on bad arguments.
static int EvrEvaluateEvent(int side, double entry, double stop, double target,
                            double risk,
                            const float* highs, const float* lows,
                            const float* closes, const double* startTimes,
                            int nBars, int firstBar, double horizonSec,
                            int ambPolicy, double targetR,
                            struct EvrOutcome* out)
{
    if (out == nullptr)
        return 0;
    out->status = EVRE_ST_NONE;
    out->outcomeR = 0.0;
    out->mfeR = 0.0;
    out->maeR = 0.0;
    out->exitBar = -1;
    out->holdingSec = 0.0;
    out->evaluated = 0;
    if (side != 1 && side != -1)
        return 0;
    if (highs == nullptr || lows == nullptr || closes == nullptr ||
        startTimes == nullptr)
        return 0;
    if (!(risk > 0.0))
        return 0;
    if (nBars <= 0 || firstBar < 0 || firstBar >= nBars)
    {
        out->status = EVRE_ST_INSUFFICIENT;
        return 1;
    }
    if (!(horizonSec > 0.0))
        return 0;

    const double t0 = startTimes[firstBar];
    // Right-edge censoring: the chart must extend far enough that the full
    // horizon has elapsed. Otherwise the event is excluded, not a timeout.
    const double lastStart = startTimes[nBars - 1];
    if (!(lastStart + 1e-9 >= t0 + horizonSec))
    {
        out->status = EVRE_ST_INSUFFICIENT;
        return 1;
    }

    int lastEligible = firstBar;
    for (int k = firstBar; k < nBars; k++)
    {
        if (startTimes[k] - t0 <= horizonSec + 1e-9)
            lastEligible = k;
        else
            break;
    }

    double bestFav = 0.0;
    double worstAdv = 0.0;
    for (int k = firstBar; k <= lastEligible; k++)
    {
        const double h = (double)highs[k];
        const double l = (double)lows[k];
        double fav;
        double adv;
        int hitT;
        int hitS;
        if (side > 0)
        {
            const double upFav = h - entry;
            const double dnAdv = l - entry;
            fav = (upFav > 0.0) ? upFav : 0.0;
            adv = (dnAdv < 0.0) ? dnAdv : 0.0;
            hitT = (h >= target) ? 1 : 0;
            hitS = (l <= stop) ? 1 : 0;
        }
        else
        {
            const double dnFav = entry - l;
            const double upAdv = entry - h;
            fav = (dnFav > 0.0) ? dnFav : 0.0;
            adv = (upAdv < 0.0) ? upAdv : 0.0;
            hitT = (l <= target) ? 1 : 0;
            hitS = (h >= stop) ? 1 : 0;
        }
        if (fav > bestFav)
            bestFav = fav;
        if (adv < worstAdv)
            worstAdv = adv;

        if (hitT && hitS)
        {
            out->mfeR = bestFav / risk;
            out->maeR = worstAdv / risk;
            out->exitBar = k;
            out->holdingSec = startTimes[k] - t0;
            if (ambPolicy == 1)
            {
                out->status = EVRE_ST_STOP;
                out->outcomeR = -1.0;
                out->evaluated = 1;
            }
            else if (ambPolicy == 2)
            {
                out->status = EVRE_ST_TARGET;
                out->outcomeR = targetR;
                out->evaluated = 1;
            }
            else
            {
                out->status = EVRE_ST_AMBIGUOUS;
                out->outcomeR = 0.0;
                out->evaluated = 0;
            }
            return 1;
        }
        if (hitS)
        {
            out->status = EVRE_ST_STOP;
            out->outcomeR = -1.0;
            out->mfeR = bestFav / risk;
            out->maeR = worstAdv / risk;
            out->exitBar = k;
            out->holdingSec = startTimes[k] - t0;
            out->evaluated = 1;
            return 1;
        }
        if (hitT)
        {
            out->status = EVRE_ST_TARGET;
            out->outcomeR = targetR;
            out->mfeR = bestFav / risk;
            out->maeR = worstAdv / risk;
            out->exitBar = k;
            out->holdingSec = startTimes[k] - t0;
            out->evaluated = 1;
            return 1;
        }
    }

    // Timeout: exit at the close of the final eligible bar.
    const double exitClose = (double)closes[lastEligible];
    double r;
    if (side > 0)
        r = (exitClose - entry) / risk;
    else
        r = (entry - exitClose) / risk;
    out->status = EVRE_ST_TIMEOUT;
    out->outcomeR = r;
    out->mfeR = bestFav / risk;
    out->maeR = worstAdv / risk;
    out->exitBar = lastEligible;
    out->holdingSec = startTimes[lastEligible] - t0;
    out->evaluated = 1;
    return 1;
}

// Overlap bookkeeping across evaluated (included) events in chronological
// order. priorExitBar is the exit bar of the previous INCLUDED event;
// hasPrior is 0 when no included event precedes the current one.
// policy 0 (Include+Flag): never excludes, flags late entries.
// policy 1 (Exclude While Prior Active): excludes the later event.
// Returns 1 on success.
static int EvrOverlapCheck(int curFirstBar, int priorExitBar, int hasPrior,
                           int policy, int* outFlag, int* outExcluded)
{
    const int overlaps = (hasPrior && curFirstBar <= priorExitBar) ? 1 : 0;
    if (policy == 1)
    {
        if (outExcluded)
            *outExcluded = overlaps;
        if (outFlag)
            *outFlag = overlaps;
        return 1;
    }
    if (outExcluded)
        *outExcluded = 0;
    if (outFlag)
        *outFlag = overlaps;
    return 1;
}

// Sierra SCDateTime GetAsDouble() is serial DAYS; ACSIL horizon/holding
// math is in seconds. Centralize the scale so both the study body and the
// portable tests share one contract. The portable outcome core
// (EvrEvaluateEvent) already works purely in seconds.
static double EvrDaysToSeconds(double days)
{
    return days * 86400.0;
}

// Elapsed seconds between two serial-day timestamps.
static double EvrSecondsBetween(double laterDays, double earlierDays)
{
    return (laterDays - earlierDays) * 86400.0;
}

// Filename sanitizer: keep [A-Za-z0-9_.-], replace everything else with
// '_'. Never writes more than dstSize bytes including the terminator.
static int EvrSanitize(char* dst, int dstSize, const char* src)
{
    if (dst == nullptr || dstSize <= 0)
        return 0;
    dst[0] = '\0';
    if (src == nullptr)
        return 0;
    int n = 0;
    for (int i = 0; src[i] != '\0' && n < dstSize - 1; i++)
    {
        const char ch = src[i];
        const int ok = (ch >= '0' && ch <= '9') ||
                       (ch >= 'A' && ch <= 'Z') ||
                       (ch >= 'a' && ch <= 'z') ||
                       ch == '_' || ch == '.' || ch == '-';
        dst[n++] = ok ? ch : '_';
    }
    dst[n] = '\0';
    return n;
}

// Deterministic CSV file name. Empty prefix falls back to EVR_Evaluator;
// empty symbol falls back to NOSYM. Returns 1 on success.
static int EvrBuildCsvName(char* dst, int dstSize, const char* prefix,
                           const char* symbol, int chartNum, int studyInstance)
{
    if (dst == nullptr || dstSize <= 0)
        return 0;
    char cleanPrefix[128];
    char cleanSymbol[128];
    EvrSanitize(cleanPrefix, (int)sizeof(cleanPrefix), prefix);
    EvrSanitize(cleanSymbol, (int)sizeof(cleanSymbol), symbol);
    const char* usePrefix = (cleanPrefix[0] != '\0') ? cleanPrefix : "EVR_Evaluator";
    const char* useSymbol = (cleanSymbol[0] != '\0') ? cleanSymbol : "NOSYM";
    std::snprintf(dst, (size_t)dstSize, "%s_%s_Chart%d_Study%d.csv",
                  usePrefix, useSymbol, chartNum, studyInstance);
    dst[dstSize - 1] = '\0';
    return 1;
}

// ---------------------------------------------------------------------------
// ACSIL study body (excluded from the portable test build).
// ---------------------------------------------------------------------------
#ifndef EVR_EVALUATOR_TEST

SCDLLName("EffortVsResultEvaluator")

// Persistent slot assignments (see header block)
static const int EVRE_INT_FP0 = 4;
static const int EVRE_INT_FP1 = 5;
static const int EVRE_INT_FP2 = 6;

// Canonical source subgraph indices (EffortVsResult study)
static const int EVRE_SRC_EFFORT = 2;
static const int EVRE_SRC_RAWFAIL = 5;
static const int EVRE_SRC_SMOOTHFAIL = 6;
static const int EVRE_SRC_BUYPULSE = 7;
static const int EVRE_SRC_SELLPULSE = 8;
static const int EVRE_SRC_BUYARROW = 9;
static const int EVRE_SRC_SELLARROW = 10;

// CSV schema version written into every export
static const int EVRE_CSV_SCHEMA = 1;

SCSFExport scsf_EffortVsResultEvaluator(SCStudyInterfaceRef sc)
{
    SCInputRef In_Source   = sc.Input[0];
    SCInputRef In_EvSrc    = sc.Input[1];
    SCInputRef In_Entry    = sc.Input[2];
    SCInputRef In_BufTicks = sc.Input[3];
    SCInputRef In_TargetR  = sc.Input[4];
    SCInputRef In_HoldMin  = sc.Input[5];
    SCInputRef In_AmbP     = sc.Input[6];
    SCInputRef In_OvlP     = sc.Input[7];
    SCInputRef In_SessEn   = sc.Input[8];
    SCInputRef In_SessSt   = sc.Input[9];
    SCInputRef In_SessEn2  = sc.Input[10];
    SCInputRef In_StartDt  = sc.Input[11];
    SCInputRef In_EndDt    = sc.Input[12];
    SCInputRef In_CsvEn    = sc.Input[13];
    SCInputRef In_CsvPre   = sc.Input[14];

    SCSubgraphRef SG_Side     = sc.Subgraph[0];
    SCSubgraphRef SG_EvalFlag = sc.Subgraph[1];
    SCSubgraphRef SG_OutcomeR = sc.Subgraph[2];
    SCSubgraphRef SG_MfeR     = sc.Subgraph[3];
    SCSubgraphRef SG_MaeR     = sc.Subgraph[4];
    SCSubgraphRef SG_Entry    = sc.Subgraph[5];
    SCSubgraphRef SG_Stop     = sc.Subgraph[6];
    SCSubgraphRef SG_Target   = sc.Subgraph[7];
    SCSubgraphRef SG_HoldSec  = sc.Subgraph[8];
    SCSubgraphRef SG_Status   = sc.Subgraph[9];
    SCSubgraphRef SG_Strength = sc.Subgraph[10];
    SCSubgraphRef SG_CandEff  = sc.Subgraph[11];
    SCSubgraphRef SG_CandRaw  = sc.Subgraph[12];
    SCSubgraphRef SG_CandSm   = sc.Subgraph[13];
    SCSubgraphRef SG_OvlFlag  = sc.Subgraph[14];
    SCSubgraphRef SG_CumN     = sc.Subgraph[15];
    SCSubgraphRef SG_CumAvg   = sc.Subgraph[16];
    SCSubgraphRef SG_CumWin   = sc.Subgraph[17];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Effort vs Result Evaluator v1.0";
        sc.StudyDescription =
            "Event-study evaluator for the Effort vs Result study. Freezes "
            "each failure confirmation into entry/stop/target/horizon and "
            "measures the forward outcome on closed bars only. Status codes: "
            "1 target, -1 stop, 2 timeout, 3 ambiguous, -2 invalid risk, "
            "-3 excluded overlap, -4 insufficient future data, "
            "-5 conflicting sides, 0 no event. Never sends orders.";
        sc.AutoLoop = 0;
        sc.GraphRegion = 1;
        sc.DrawZeros = 0;

        SG_Side.Name = "Event Side";
        SG_EvalFlag.Name = "Evaluated Flag";
        SG_OutcomeR.Name = "Outcome R";
        SG_MfeR.Name = "MFE R";
        SG_MaeR.Name = "MAE R";
        SG_Entry.Name = "Entry Price";
        SG_Stop.Name = "Stop Price";
        SG_Target.Name = "Target Price";
        SG_HoldSec.Name = "Holding Seconds";
        SG_Status.Name = "Status Code";
        SG_Strength.Name = "Signal Strength";
        SG_CandEff.Name = "Candidate Normalized Effort";
        SG_CandRaw.Name = "Candidate Raw Signed Failure";
        SG_CandSm.Name = "Candidate Smoothed Signed Failure";
        SG_OvlFlag.Name = "Overlap Flag";
        SG_CumN.Name = "Cumulative Included Events";
        SG_CumAvg.Name = "Cumulative Average R";
        SG_CumWin.Name = "Cumulative Win Rate Percent";
        SCSubgraphRef allSG[18] = {
            SG_Side, SG_EvalFlag, SG_OutcomeR, SG_MfeR, SG_MaeR,
            SG_Entry, SG_Stop, SG_Target, SG_HoldSec, SG_Status,
            SG_Strength, SG_CandEff, SG_CandRaw, SG_CandSm, SG_OvlFlag,
            SG_CumN, SG_CumAvg, SG_CumWin};
        for (int k = 0; k < 18; k++)
        {
            allSG[k].DrawStyle = DRAWSTYLE_IGNORE;
            allSG[k].LineWidth = 1;
            allSG[k].PrimaryColor = RGB(200, 200, 200);
            allSG[k].DrawZeros = 0;
        }

        In_Source.Name = "Effort vs Result Source (Study/Subgraph)";
        In_Source.SetStudySubgraphValues(0, 7);

        In_EvSrc.Name = "Event Source (Pulse; Arrow; Both Deduplicated)";
        In_EvSrc.SetCustomInputStrings("Pulse;Arrow;Both Deduplicated");
        In_EvSrc.SetCustomInputIndex(0);

        In_Entry.Name = "Entry Mode (Next Bar Open; Confirmation Close)";
        In_Entry.SetCustomInputStrings("Next Bar Open;Confirmation Close");
        In_Entry.SetCustomInputIndex(0);

        In_BufTicks.Name = "Stop Buffer Ticks";
        In_BufTicks.SetInt(2);
        In_BufTicks.SetIntLimits(0, 100);

        In_TargetR.Name = "Target R Multiple";
        In_TargetR.SetFloat(1.0f);
        In_TargetR.SetFloatLimits(0.1f, 20.0f);

        In_HoldMin.Name = "Maximum Hold Minutes";
        In_HoldMin.SetInt(15);
        In_HoldMin.SetIntLimits(1, 1440);

        In_AmbP.Name = "Ambiguous Bar Policy (Mark/Exclude; Stop First; Target First)";
        In_AmbP.SetCustomInputStrings("Mark/Exclude;Stop First;Target First");
        In_AmbP.SetCustomInputIndex(0);

        In_OvlP.Name = "Overlapping Event Policy (Include+Flag; Exclude)";
        In_OvlP.SetCustomInputStrings("Include+Flag;Exclude");
        In_OvlP.SetCustomInputIndex(0);

        In_SessEn.Name = "Enable Session Filter";
        In_SessEn.SetYesNo(0);

        In_SessSt.Name = "Session Start (chart time zone)";
        In_SessSt.SetTime(9 * 3600 + 30 * 60);

        In_SessEn2.Name = "Session End (chart time zone)";
        In_SessEn2.SetTime(16 * 3600);

        In_StartDt.Name = "Evaluation Start Date YYYYMMDD (0=all)";
        In_StartDt.SetInt(0);

        In_EndDt.Name = "Evaluation End Date YYYYMMDD (0=all)";
        In_EndDt.SetInt(0);

        In_CsvEn.Name = "Export CSV on Full Recalculation";
        In_CsvEn.SetYesNo(0);

        In_CsvPre.Name = "CSV File Prefix";
        In_CsvPre.SetString("EVR_Evaluator");

        return;
	}

    // Every declared input is read here (gate requirement).
    const int srcStudyID = (int)In_Source.GetStudyID();
    const int eventSource = (int)In_EvSrc.GetIndex();
    const int entryMode = (int)In_Entry.GetIndex();
    int bufTicks = In_BufTicks.GetInt();
    float targetRf = In_TargetR.GetFloat();
    int holdMin = In_HoldMin.GetInt();
    const int ambPolicy = (int)In_AmbP.GetIndex();
    const int ovlPolicy = (int)In_OvlP.GetIndex();
    const int sessEnabled = In_SessEn.GetYesNo();
    const int sessStart = In_SessSt.GetTime();
    const int sessEnd = In_SessEn2.GetTime();
    const int startDate = In_StartDt.GetInt();
    const int endDate = In_EndDt.GetInt();
    const int csvEnabled = In_CsvEn.GetYesNo();
    const char* csvPrefixRaw = In_CsvPre.GetString();

    if (bufTicks < 0)
        bufTicks = 0;
    if (bufTicks > 100)
        bufTicks = 100;
    double targetR = (double)targetRf;
    if (!(targetR >= 0.1) || !(targetR <= 20.0))
        targetR = 1.0;
    if (holdMin < 1)
        holdMin = 1;
    if (holdMin > 1440)
        holdMin = 1440;
    int evSrc = eventSource;
    if (evSrc < 0 || evSrc > 2)
        evSrc = 0;
    int entMode = entryMode;
    if (entMode < 0 || entMode > 1)
        entMode = 0;
    int ambP = ambPolicy;
    if (ambP < 0 || ambP > 2)
        ambP = 0;
    int ovlP = ovlPolicy;
    if (ovlP < 0 || ovlP > 1)
        ovlP = 0;

    float tickSize = sc.TickSize;
    const double horizonSec = (double)holdMin * 60.0;

    // Structural fingerprint: every input except CSV enable/prefix.
    // Time inputs pack as seconds; dates pack low 16 bits with mixing;
    // targetR packs at 0.001 quantum; StudyID folds in fully.
    unsigned int ufp0 = ((unsigned int)(evSrc & 0xFF)) |
        (((unsigned int)(entMode & 0xFF)) << 8) |
        (((unsigned int)(bufTicks & 0xFF)) << 16) |
        (((unsigned int)(ambP & 0xFF)) << 24);
    unsigned int ufp1 = ((unsigned int)(holdMin & 0xFFFF)) |
        (((unsigned int)(ovlP & 0xFF)) << 16) |
        (((unsigned int)(sessEnabled & 0x1)) << 24);
    ufp1 = ufp1 * 31u + (unsigned int)(sessStart & 0xFFFFFF);
    ufp1 = ufp1 * 31u + (unsigned int)(sessEnd & 0xFFFFFF);
    unsigned int ufp2 = (unsigned int)((int)(targetR * 1000.0));
    ufp2 = ufp2 * 31u + (unsigned int)(startDate ^ (startDate >> 11));
    ufp2 = ufp2 * 31u + (unsigned int)(endDate ^ (endDate >> 11));
    ufp2 = ufp2 * 31u + (unsigned int)srcStudyID;
    const int fp0 = (int)ufp0;
    const int fp1 = (int)ufp1;
    const int fp2 = (int)ufp2;

    const bool settingsChanged =
        (sc.GetPersistentInt(EVRE_INT_FP0) != fp0) ||
        (sc.GetPersistentInt(EVRE_INT_FP1) != fp1) ||
        (sc.GetPersistentInt(EVRE_INT_FP2) != fp2);

    bool isFullRecalc =
        (sc.UpdateStartIndex == 0) ||
        (sc.IsFullRecalculation != 0) ||
        settingsChanged;

    // Full-recalculation gate: the evaluator runs only on full recalculation
    // or a structural-settings rebuild. Incremental new-bar refresh is
    // intentionally disabled so history stays deterministic and the CSV
    // export below (full-recalc only) always matches the subgraphs.
    if (!isFullRecalc)
        return;

    const int lastClosed = sc.ArraySize - 2;
    if (lastClosed < 0 || sc.ArraySize < 3)
    {
        return;
    }

    // Fetch each source array once per call (AutoLoop = 0).
    SCFloatArray srcEffort, srcRaw, srcSmooth;
    SCFloatArray srcBuyP, srcSellP, srcBuyA, srcSellA;
    if (srcStudyID > 0)
    {
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_EFFORT, srcEffort);
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_RAWFAIL, srcRaw);
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_SMOOTHFAIL, srcSmooth);
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_BUYPULSE, srcBuyP);
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_SELLPULSE, srcSellP);
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_BUYARROW, srcBuyA);
        sc.GetStudyArrayFromChartUsingID(sc.ChartNumber, srcStudyID,
                                         EVRE_SRC_SELLARROW, srcSellA);
    }

    // Configuration validation: one message per invalid configuration.
    // These checks run on the evaluation pass below; messages are logged
    // once per call, never once per bar.
    bool configLogged = false;
    char cfgMsg[256];
    cfgMsg[0] = '\0';
    bool configOK = true;
    int configFailKind = 0; // 1 source, 2 date, 3 tick
    const int srcSize = (srcStudyID > 0) ? srcBuyP.GetArraySize() : 0;
    if (!(srcStudyID > 0))
    {
        configOK = false;
        configFailKind = 1;
        std::snprintf(cfgMsg, sizeof(cfgMsg),
            "Effort vs Result Evaluator: Effort vs Result Source Study ID "
            "is not configured. Point Input[0] at the Effort vs Result study.");
    }
    else if (srcSize < sc.ArraySize ||
             srcEffort.GetArraySize() < sc.ArraySize ||
             srcRaw.GetArraySize() < sc.ArraySize ||
             srcSmooth.GetArraySize() < sc.ArraySize ||
             srcSellP.GetArraySize() < sc.ArraySize ||
             srcBuyA.GetArraySize() < sc.ArraySize ||
             srcSellA.GetArraySize() < sc.ArraySize)
    {
        configOK = false;
        configFailKind = 1;
        std::snprintf(cfgMsg, sizeof(cfgMsg),
            "Effort vs Result Evaluator: source study ID %d arrays are not "
            "available on this chart (same-chart source required).",
            srcStudyID);
    }
    if (configOK && (!EvrValidYYYYMMDD(startDate) ||
                     !EvrValidYYYYMMDD(endDate)))
    {
        configOK = false;
        configFailKind = 2;
        std::snprintf(cfgMsg, sizeof(cfgMsg),
            "Effort vs Result Evaluator: invalid Evaluation Date bound "
            "(%d..%d). Use 0 for unbounded or a real YYYYMMDD date.",
            startDate, endDate);
    }
    if (configOK && startDate != 0 && endDate != 0 && startDate > endDate)
    {
        configOK = false;
        configFailKind = 2;
        std::snprintf(cfgMsg, sizeof(cfgMsg),
            "Effort vs Result Evaluator: Evaluation Start Date %d is after "
            "Evaluation End Date %d. No evaluation performed.",
            startDate, endDate);
    }
    if (configOK && !(tickSize > 0.0f))
    {
        configOK = false;
        configFailKind = 3;
        std::snprintf(cfgMsg, sizeof(cfgMsg),
            "Effort vs Result Evaluator: TickSize is not positive on this "
            "chart. Cannot build stops/targets.");
    }
    (void)configFailKind;

    // Deterministic clear of every output on full recalc/settings rebuild.
    if (isFullRecalc)
    {
        for (int i = 0; i < sc.ArraySize; i++)
        {
            SG_Side[i] = 0.0f;
            SG_EvalFlag[i] = 0.0f;
            SG_OutcomeR[i] = 0.0f;
            SG_MfeR[i] = 0.0f;
            SG_MaeR[i] = 0.0f;
            SG_Entry[i] = 0.0f;
            SG_Stop[i] = 0.0f;
            SG_Target[i] = 0.0f;
            SG_HoldSec[i] = 0.0f;
            SG_Status[i] = 0.0f;
            SG_Strength[i] = 0.0f;
            SG_CandEff[i] = 0.0f;
            SG_CandRaw[i] = 0.0f;
            SG_CandSm[i] = 0.0f;
            SG_OvlFlag[i] = 0.0f;
            SG_CumN[i] = 0.0f;
            SG_CumAvg[i] = 0.0f;
            SG_CumWin[i] = 0.0f;
        }
        sc.SetPersistentInt(EVRE_INT_FP0, fp0);
        sc.SetPersistentInt(EVRE_INT_FP1, fp1);
        sc.SetPersistentInt(EVRE_INT_FP2, fp2);
    }

    if (!configOK)
    {
        if (!configLogged)
        {
            sc.AddMessageToLog(cfgMsg, 1);
            configLogged = true;
        }
        return;
    }

    // Bar period parameters for the CSV contract (numeric type + params).
    n_ACSIL::s_BarPeriod barPeriod;
    sc.GetBarPeriodParameters(barPeriod);
    const int csvBarType = (int)barPeriod.IntradayChartBarPeriodType;
    const int csvBarP1 = barPeriod.IntradayChartBarPeriodParameter1;
    const int csvBarP2 = barPeriod.IntradayChartBarPeriodParameter2;
    const int csvBarP3 = barPeriod.IntradayChartBarPeriodParameter3;
    const int csvBarP4 = barPeriod.IntradayChartBarPeriodParameter4;

    // Main event scan over confirmation bars j (need j >= 1 and a closed
    // j+1 for entry). Outcome traversal always begins at j+1, for both
    // entry modes, so the confirmation bar's high/low is never reused
    // after a Confirmation Close entry.
    double cumSumR = 0.0;
    int cumN = 0;
    int cumWins = 0;
    int hasPriorIncluded = 0;
    int priorExitBar = -1;

    // No row staging buffer: the CSV export below is a read-only
    // serialization pass over the evaluator SGs written here and streams
    // rows directly to the temp file, so this evaluation pass only writes
    // subgraphs (no function-local static storage).

    for (int j = 1; j <= lastClosed - 1; j++)
    {
        const float pB = srcBuyP[j];
        const float pS = srcSellP[j];
        const float aB = srcBuyA[j];
        const float aS = srcSellA[j];

        int side = 0;
        int kind = EVRE_KIND_NONE;
        float strength = 0.0f;
        int conflict = 0;
        const int hasEvent = EvrSelectEvent(pB, pS, aB, aS, evSrc,
                                            &side, &kind, &strength,
                                            &conflict);

        // Per-bar defaults (also written for non-events so incremental
        // refresh never leaves stale values behind).
        float outSide = 0.0f;
        float outEval = 0.0f;
        float outR = 0.0f;
        float outMfe = 0.0f;
        float outMae = 0.0f;
        float outEntry = 0.0f;
        float outStop = 0.0f;
        float outTarget = 0.0f;
        float outHold = 0.0f;
        float outStatus = 0.0f;
        float outStrength = 0.0f;
        float outCandEff = 0.0f;
        float outCandRaw = 0.0f;
        float outCandSm = 0.0f;
        float outOvl = 0.0f;

        const int confDate = sc.GetTradingDayDate(sc.BaseDateTimeIn[j]);
        const int confTOD = sc.BaseDateTimeIn[j].GetTimeInSeconds();

        // Eligibility first: date bounds + session filter on confirmation
        // time. Bars outside the configured evaluation population stay
        // omitted/zero, including conflicts (BuildSpec population filter).
        const int dateOK = EvrDateEligible(confDate, startDate, endDate);
        const int sessOK = EvrSessionEligible(confTOD, sessEnabled,
                                              sessStart, sessEnd);
        const int inPopulation = (dateOK && sessOK) ? 1 : 0;

        if (!hasEvent && !conflict)
        {
            SG_Side[j] = outSide;
            SG_EvalFlag[j] = outEval;
            SG_OutcomeR[j] = outR;
            SG_MfeR[j] = outMfe;
            SG_MaeR[j] = outMae;
            SG_Entry[j] = outEntry;
            SG_Stop[j] = outStop;
            SG_Target[j] = outTarget;
            SG_HoldSec[j] = outHold;
            SG_Status[j] = outStatus;
            SG_Strength[j] = outStrength;
            SG_CandEff[j] = outCandEff;
            SG_CandRaw[j] = outCandRaw;
            SG_CandSm[j] = outCandSm;
            SG_OvlFlag[j] = outOvl;
            SG_CumN[j] = (float)cumN;
            SG_CumAvg[j] = (cumN > 0) ? (float)(cumSumR / (double)cumN) : 0.0f;
            SG_CumWin[j] = (cumN > 0)
                ? (float)(100.0 * (double)cumWins / (double)cumN) : 0.0f;
            continue;
        }

        if (!inPopulation)
        {
            SG_CumN[j] = (float)cumN;
            SG_CumAvg[j] = (cumN > 0) ? (float)(cumSumR / (double)cumN) : 0.0f;
            SG_CumWin[j] = (cumN > 0)
                ? (float)(100.0 * (double)cumWins / (double)cumN) : 0.0f;
            continue;
        }

        if (conflict)
        {
            outStatus = (float)EVRE_ST_CONFLICT;
            SG_Side[j] = outSide;
            SG_EvalFlag[j] = outEval;
            SG_OutcomeR[j] = outR;
            SG_MfeR[j] = outMfe;
            SG_MaeR[j] = outMae;
            SG_Entry[j] = outEntry;
            SG_Stop[j] = outStop;
            SG_Target[j] = outTarget;
            SG_HoldSec[j] = outHold;
            SG_Status[j] = outStatus;
            SG_Strength[j] = outStrength;
            SG_CandEff[j] = srcEffort[j - 1];
            SG_CandRaw[j] = srcRaw[j - 1];
            SG_CandSm[j] = srcSmooth[j - 1];
            SG_OvlFlag[j] = outOvl;
            SG_CumN[j] = (float)cumN;
            SG_CumAvg[j] = (cumN > 0) ? (float)(cumSumR / (double)cumN) : 0.0f;
            SG_CumWin[j] = (cumN > 0)
                ? (float)(100.0 * (double)cumWins / (double)cumN) : 0.0f;
            continue;
        }

        const int firstBar = j + 1;
        const double entryPrice = (entMode == 1) ? (double)sc.Close[j]
                                                 : (double)sc.Open[firstBar];
        const double candHigh = (double)sc.High[j - 1];
        const double candLow = (double)sc.Low[j - 1];

        double stopP = 0.0;
        double targetP = 0.0;
        double riskP = 0.0;
        const int tradeOK = EvrComputeTrade(side, candHigh, candLow,
                                            entryPrice, (double)tickSize,
                                            bufTicks, targetR,
                                            &stopP, &targetP, &riskP);
        outSide = (float)side;
        outStrength = strength;
        outCandEff = srcEffort[j - 1];
        outCandRaw = srcRaw[j - 1];
        outCandSm = srcSmooth[j - 1];
        outEntry = (float)entryPrice;

        int status = EVRE_ST_NONE;
        double outcomeR = 0.0;
        double mfeR = 0.0;
        double maeR = 0.0;
        int exitBar = -1;
        double holdSec = 0.0;
        int evaluated = 0;

        if (!tradeOK)
        {
            status = EVRE_ST_INVALID_RISK;
        }
        else
        {
            outStop = (float)stopP;
            outTarget = (float)targetP;
            // Overlap gate comes before the forward scan so an excluded
            // event costs no scan and never touches aggregates.
            int ovlFlag = 0;
            int ovlExcluded = 0;
            EvrOverlapCheck(firstBar, priorExitBar, hasPriorIncluded, ovlP,
                            &ovlFlag, &ovlExcluded);
            outOvl = (float)ovlFlag;
            if (ovlExcluded)
            {
                status = EVRE_ST_EXCLUDED_OVERLAP;
            }
            else
            {
                // Forward scan over closed bars within the horizon.
                // SCDateTime GetAsDouble() is serial DAYS; horizon/holding
                // math is in seconds via EvrSecondsBetween.
                const double t0 =
                    sc.BaseDateTimeIn[firstBar].GetAsDouble();
                const double lastStart =
                    sc.BaseDateTimeIn[lastClosed].GetAsDouble();
                if (!(EvrSecondsBetween(lastStart, t0) + 1e-6 >= horizonSec))
                {
                    status = EVRE_ST_INSUFFICIENT;
                }
                else
                {
                    int lastEligible = firstBar;
                    for (int k = firstBar; k <= lastClosed; k++)
                    {
                        if (EvrSecondsBetween(
                                sc.BaseDateTimeIn[k].GetAsDouble(), t0) <=
                            horizonSec + 1e-6)
                            lastEligible = k;
                        else
                            break;
                    }
                    double bestFav = 0.0;
                    double worstAdv = 0.0;
                    int done = 0;
                    for (int k = firstBar; k <= lastEligible && !done; k++)
                    {
                        const double h = (double)sc.High[k];
                        const double l = (double)sc.Low[k];
                        double fav;
                        double adv;
                        int hitT;
                        int hitS;
                        if (side > 0)
                        {
                            const double upFav = h - entryPrice;
                            const double dnAdv = l - entryPrice;
                            fav = (upFav > 0.0) ? upFav : 0.0;
                            adv = (dnAdv < 0.0) ? dnAdv : 0.0;
                            hitT = (h >= targetP) ? 1 : 0;
                            hitS = (l <= stopP) ? 1 : 0;
                        }
                        else
                        {
                            const double dnFav = entryPrice - l;
                            const double upAdv = entryPrice - h;
                            fav = (dnFav > 0.0) ? dnFav : 0.0;
                            adv = (upAdv < 0.0) ? upAdv : 0.0;
                            hitT = (l <= targetP) ? 1 : 0;
                            hitS = (h >= stopP) ? 1 : 0;
                        }
                        if (fav > bestFav)
                            bestFav = fav;
                        if (adv < worstAdv)
                            worstAdv = adv;
                        if (hitT && hitS)
                        {
                            mfeR = bestFav / riskP;
                            maeR = worstAdv / riskP;
                            exitBar = k;
                            holdSec = EvrSecondsBetween(
                                sc.BaseDateTimeIn[k].GetAsDouble(), t0);
                            if (ambP == 1)
                            {
                                status = EVRE_ST_STOP;
                                outcomeR = -1.0;
                                evaluated = 1;
                            }
                            else if (ambP == 2)
                            {
                                status = EVRE_ST_TARGET;
                                outcomeR = targetR;
                                evaluated = 1;
                            }
                            else
                            {
                                status = EVRE_ST_AMBIGUOUS;
                                outcomeR = 0.0;
                                evaluated = 0;
                            }
                            done = 1;
                        }
                        else if (hitS)
                        {
                            status = EVRE_ST_STOP;
                            outcomeR = -1.0;
                            mfeR = bestFav / riskP;
                            maeR = worstAdv / riskP;
                            exitBar = k;
                            holdSec = EvrSecondsBetween(
                                sc.BaseDateTimeIn[k].GetAsDouble(), t0);
                            evaluated = 1;
                            done = 1;
                        }
                        else if (hitT)
                        {
                            status = EVRE_ST_TARGET;
                            outcomeR = targetR;
                            mfeR = bestFav / riskP;
                            maeR = worstAdv / riskP;
                            exitBar = k;
                            holdSec = EvrSecondsBetween(
                                sc.BaseDateTimeIn[k].GetAsDouble(), t0);
                            evaluated = 1;
                            done = 1;
                        }
                    }
                    if (!done)
                    {
                        const double exitClose =
                            (double)sc.Close[lastEligible];
                        if (side > 0)
                            outcomeR = (exitClose - entryPrice) / riskP;
                        else
                            outcomeR = (entryPrice - exitClose) / riskP;
                        status = EVRE_ST_TIMEOUT;
                        mfeR = bestFav / riskP;
                        maeR = worstAdv / riskP;
                        exitBar = lastEligible;
                        holdSec = EvrSecondsBetween(
                            sc.BaseDateTimeIn[lastEligible].GetAsDouble(), t0);
                        evaluated = 1;
                    }
                }
            }
        }

        outStatus = (float)status;
        outR = (float)outcomeR;
        outMfe = (float)mfeR;
        outMae = (float)maeR;
        outHold = (float)holdSec;
        outEval = evaluated ? 1.0f : 0.0f;

        SG_Side[j] = outSide;
        SG_EvalFlag[j] = outEval;
        SG_OutcomeR[j] = outR;
        SG_MfeR[j] = outMfe;
        SG_MaeR[j] = outMae;
        SG_Entry[j] = outEntry;
        SG_Stop[j] = outStop;
        SG_Target[j] = outTarget;
        SG_HoldSec[j] = outHold;
        SG_Status[j] = outStatus;
        SG_Strength[j] = outStrength;
        SG_CandEff[j] = outCandEff;
        SG_CandRaw[j] = outCandRaw;
        SG_CandSm[j] = outCandSm;
        SG_OvlFlag[j] = outOvl;

        if (evaluated)
        {
            cumSumR += outcomeR;
            cumN++;
            if (outcomeR > 0.0)
                cumWins++;
            hasPriorIncluded = 1;
            priorExitBar = exitBar;
        }
        SG_CumN[j] = (float)cumN;
        SG_CumAvg[j] = (cumN > 0) ? (float)(cumSumR / (double)cumN) : 0.0f;
        SG_CumWin[j] = (cumN > 0)
            ? (float)(100.0 * (double)cumWins / (double)cumN) : 0.0f;
    }

    // Carry cumulative state onto lastClosed: the event loop stops at
    // lastClosed - 1 (j+1 must be closed), so without this the newest
    // closed bar would still read the cleared zeros on SG15/16/17.
    if (lastClosed >= 0)
    {
        SG_CumN[lastClosed] = (float)cumN;
        SG_CumAvg[lastClosed] =
            (cumN > 0) ? (float)(cumSumR / (double)cumN) : 0.0f;
        SG_CumWin[lastClosed] = (cumN > 0)
            ? (float)(100.0 * (double)cumWins / (double)cumN) : 0.0f;
    }

    // Forming bar: explicit zeros, never a signal source.
    const int forming = sc.ArraySize - 1;
    if (forming >= 0 && forming > lastClosed)
    {
        SG_Side[forming] = 0.0f;
        SG_EvalFlag[forming] = 0.0f;
        SG_OutcomeR[forming] = 0.0f;
        SG_MfeR[forming] = 0.0f;
        SG_MaeR[forming] = 0.0f;
        SG_Entry[forming] = 0.0f;
        SG_Stop[forming] = 0.0f;
        SG_Target[forming] = 0.0f;
        SG_HoldSec[forming] = 0.0f;
        SG_Status[forming] = 0.0f;
        SG_Strength[forming] = 0.0f;
        SG_CandEff[forming] = 0.0f;
        SG_CandRaw[forming] = 0.0f;
        SG_CandSm[forming] = 0.0f;
        SG_OvlFlag[forming] = 0.0f;
        SG_CumN[forming] = (float)cumN;
        SG_CumAvg[forming] = (cumN > 0) ? (float)(cumSumR / (double)cumN) : 0.0f;
        SG_CumWin[forming] = (cumN > 0)
            ? (float)(100.0 * (double)cumWins / (double)cumN) : 0.0f;
    }

    // CSV export: full recalculation/settings rebuild only, never intrabar.
    // Read-only serialization pass over the already-computed evaluator SGs
    // streams rows directly to the temp file (no staging buffer, no
    // function-local static, no duplicated trade/outcome engine). Repeat
    // exports replace the destination atomically via temp + backup/restore;
    // any write error preserves the prior completed CSV.
    if (isFullRecalc && csvEnabled)
    {
        char cleanPrefix[128];
        char cleanSymbol[128];
        const char* symChars = sc.Symbol.GetChars();
        EvrSanitize(cleanPrefix, (int)sizeof(cleanPrefix), csvPrefixRaw);
        EvrSanitize(cleanSymbol, (int)sizeof(cleanSymbol), symChars);
        if (cleanPrefix[0] == '\0')
        {
            EvrSanitize(cleanPrefix, (int)sizeof(cleanPrefix),
                        "EVR_Evaluator");
        }
        if (cleanSymbol[0] == '\0')
        {
            EvrSanitize(cleanSymbol, (int)sizeof(cleanSymbol), "NOSYM");
        }
        char fileName[256];
        EvrBuildCsvName(fileName, (int)sizeof(fileName), cleanPrefix,
                        cleanSymbol, sc.ChartNumber, sc.StudyGraphInstanceID);

        SCString folder = sc.DataFilesFolder();
        SCString destPath;
        destPath.Format("%s\\%s", folder.GetChars(), fileName);
        SCString tmpPath;
        tmpPath.Format("%s\\%s.tmp", folder.GetChars(), fileName);
        SCString bakPath;
        bakPath.Format("%s\\%s.bak", folder.GetChars(), fileName);

        FILE* f = fopen(tmpPath.GetChars(), "w");
        if (f == nullptr)
        {
            SCString err;
            err.Format("Effort vs Result Evaluator: cannot open temp CSV "
                       "for write: %s", tmpPath.GetChars());
            sc.AddMessageToLog(err, 1);
        }
        else
        {
            int writeErr = 0;
            if (fprintf(f, "schema_version,symbol,chart_number,study_instance,"
                           "bar_period_type,bar_param1,bar_param2,bar_param3,"
                           "bar_param4,confirmation_datetime,entry_datetime,"
                           "event_source,side,signal_strength,candidate_effort,"
                           "candidate_raw_failure,candidate_smoothed_failure,"
                           "entry_price,stop_price,target_price,risk_ticks,"
                           "status_code,outcome_r,mfe_r,mae_r,holding_seconds,"
                           "overlap_flag\n") < 0)
                writeErr = 1;
            // Sanitized symbol only: never emit raw chart-symbol text.
            const char* symOut = cleanSymbol;
            // Read-only serialization over the already-computed evaluator
            // SGs. Source arrays are re-selected only to recover the event
            // kind string; every price/status/outcome/MFE/MAE/hold/overlap
            // and candidate value below is a canonical SG output. risk_ticks
            // derives only from SG stop/entry and TickSize. Date/session
            // eligibility applies before exporting, including conflicts.
            // Status -5 emits event_source "conflict".
            for (int j = 1; j <= lastClosed - 1 && !writeErr; j++)
            {
                const int cDate =
                    sc.GetTradingDayDate(sc.BaseDateTimeIn[j]);
                const int cTOD =
                    sc.BaseDateTimeIn[j].GetTimeInSeconds();
                if (!EvrDateEligible(cDate, startDate, endDate) ||
                    !EvrSessionEligible(cTOD, sessEnabled, sessStart, sessEnd))
                    continue;
                const int sgStatus = (int)SG_Status[j];
                if (sgStatus == EVRE_ST_NONE)
                    continue;
                const float pB = srcBuyP[j];
                const float pS = srcSellP[j];
                const float aB = srcBuyA[j];
                const float aS = srcSellA[j];
                int selSide = 0;
                int selKind = EVRE_KIND_NONE;
                float selStrength = 0.0f;
                int selConflict = 0;
                EvrSelectEvent(pB, pS, aB, aS, evSrc,
                               &selSide, &selKind, &selStrength,
                               &selConflict);
                (void)selSide;
                (void)selStrength;
                (void)selConflict;
                const char* kindStr = "pulse";
                if (sgStatus == EVRE_ST_CONFLICT)
                    kindStr = "conflict";
                else if (selKind == EVRE_KIND_ARROW)
                    kindStr = "arrow";
                else if (selKind == EVRE_KIND_BOTH)
                    kindStr = "both";
                const double confDT = sc.BaseDateTimeIn[j].GetAsDouble();
                const double entryDT =
                    sc.BaseDateTimeIn[j + 1].GetAsDouble();
                const int sgSide = (int)SG_Side[j];
                const double sgStrength = (double)SG_Strength[j];
                const double sgCEff = (double)SG_CandEff[j];
                const double sgCRaw = (double)SG_CandRaw[j];
                const double sgCSm = (double)SG_CandSm[j];
                const double sgEntry = (double)SG_Entry[j];
                const double sgStop = (double)SG_Stop[j];
                const double sgTarget = (double)SG_Target[j];
                double riskTicks = 0.0;
                if ((double)tickSize > 0.0 && SG_Stop[j] != 0.0f)
                    riskTicks = fabs(sgStop - sgEntry) / (double)tickSize;
                const int sgOvl = (int)SG_OvlFlag[j];
                if (fprintf(f, "%d,%s,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%s,%d,"
                               "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.4f,%d,"
                               "%.6f,%.6f,%.6f,%.1f,%d\n",
                            EVRE_CSV_SCHEMA, symOut, sc.ChartNumber,
                            sc.StudyGraphInstanceID,
                            csvBarType, csvBarP1, csvBarP2, csvBarP3, csvBarP4,
                            confDT, entryDT, kindStr, sgSide,
                            sgStrength, sgCEff, sgCRaw, sgCSm,
                            sgEntry, sgStop, sgTarget, riskTicks,
                            sgStatus, (double)SG_OutcomeR[j],
                            (double)SG_MfeR[j], (double)SG_MaeR[j],
                            (double)SG_HoldSec[j], sgOvl) < 0)
                    writeErr = 1;
            }
            if (fflush(f) != 0)
                writeErr = 1;
            if (ferror(f))
                writeErr = 1;
            if (fclose(f) != 0)
                writeErr = 1;
            if (writeErr)
            {
                SCString err;
                err.Format("Effort vs Result Evaluator: failed writing temp "
                           "CSV: %s (prior completed CSV preserved)",
                           tmpPath.GetChars());
                sc.AddMessageToLog(err, 1);
                remove(tmpPath.GetChars());
            }
            else
            {
                // Stale backup first so a repeat export can always restore.
                remove(bakPath.GetChars());
                int hasDest = 0;
                FILE* probe = fopen(destPath.GetChars(), "r");
                if (probe != nullptr)
                {
                    fclose(probe);
                    hasDest = 1;
                }
                int staged = 0;
                if (hasDest)
                {
                    if (rename(destPath.GetChars(), bakPath.GetChars()) != 0)
                    {
                        SCString err;
                        err.Format("Effort vs Result Evaluator: failed staging "
                                   "CSV backup: %s (prior completed CSV "
                                   "preserved)", destPath.GetChars());
                        sc.AddMessageToLog(err, 1);
                        remove(tmpPath.GetChars());
                    }
                    else
                    {
                        staged = 1;
                    }
                }
                else
                {
                    staged = 1;
                }
                if (staged)
                {
                    if (rename(tmpPath.GetChars(), destPath.GetChars()) != 0)
                    {
                        SCString err;
                        err.Format("Effort vs Result Evaluator: failed "
                                   "replacing CSV destination: %s (prior "
                                   "completed CSV preserved)",
                                   destPath.GetChars());
                        sc.AddMessageToLog(err, 1);
                        remove(tmpPath.GetChars());
                        if (hasDest)
                            rename(bakPath.GetChars(), destPath.GetChars());
                    }
                    else
                    {
                        remove(bakPath.GetChars());
                    }
                }
            }
        }
    }
}

#endif // EVR_EVALUATOR_TEST
