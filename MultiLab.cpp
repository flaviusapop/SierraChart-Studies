// =============================================================================
// MultiLab.cpp
// Sierra Chart ACSIL Custom Study — Multi Lab
//
// Multi-trigger evolution of TriggerLab.cpp ("Trigger Lab v1.2"). TriggerLab
// measures ONE trigger (one long SG + one short SG) per run — measuring 8
// triggers costs 8 chart configurations, 8 CSV files, and cross-trigger
// confluence has to be reconstructed after the fact by fuzzy-matching
// timestamps across separate files.
//
// MultiLab measures 6 triggers (12 source SGs: 6 long + 6 short) in a SINGLE
// run, writing one shared CSV. It is TriggerLab's ENTIRE measurement
// apparatus — fire test, delay sweep, horizon modes, session gating,
// MFE/MAE, the single-barrier evaluator, the Barrier Sweep second export —
// unchanged, wrapped in an outer loop over 6 trigger slots, plus two
// genuinely new things:
//
//   1. source_name  — a human-readable label per trigger, written on every
//      row, so registry.md's whole reason for existing (decoding opaque
//      source_sg integers) becomes unnecessary for MultiLab data.
//   2. fired_mask / fired_count — because the study holds all 6 triggers'
//      entry bars in memory at once, it can answer "how many OTHER triggers
//      were active when this one fired" directly, instead of matching
//      timestamps across 5 separate TriggerLab files within a fuzzy window.
//
// Spec: MultiLab_BuildSpec.md (same folder). Read it first — this file does
// not re-derive the reasoning behind any design choice, only implements it.
//
// Reference implementation, READ ONLY, never modified by this file or its
// build: C:\SierraChart\ACS_Source\TriggerLab.cpp
//
// NON-NEGOTIABLES (inherited from TriggerLab, see its header for why; not
// re-derived here):
//   - Fire test runs in SOURCE time on the source chart's own bars.
//   - knowableBar = destOfSrc[s+1], never the source bar's own open.
//   - Entry Price = Open with Entry Delay = 0 falls back to Close (reported).
//   - Excursion window is entryBar+1 .. horizonEnd; entry bar excluded.
//   - Incomplete trades (horizon not fully elapsed) excluded from every stat.
//   - Stop/target ambiguity inside one bar resolves as LOSS (pessimistic).
//   - No trading calls of any kind.
//   - sc.AutoLoop = 0, single full pass, rebuild only on full recalc or
//     bar-count change.
//
// NEW NON-NEGOTIABLES (MultiLab-specific, see BuildSpec section 3/4/6):
//   - Baseline is emitted ONCE per side, shared across all 6 triggers — NOT
//     once per trigger. Getting this wrong 6x's the baseline row count
//     silently (see BuildSpec section 3).
//   - trade_id is the 1-based position in the ONE global trade vector at
//     CSV-write time — unique across the whole file by construction, never
//     restarted per trigger (see BuildSpec section 6).
//   - fired_mask bit i = Trigger (i+1) in input order. A trigger's own bit
//     is always set in its own rows (see BuildSpec section 4.3).
//
// Persistent slots used by this study:
//   Persistent Pointer 1  — std::vector<MLTrade>* trade history (all
//                           triggers, both sides, baseline; rebuilt from
//                           scratch every pass)
//   Persistent Int 1      — last known bar count (rebuild trigger)
//
// =============================================================================
// CHANGELOG
// =============================================================================
//   2026-08-02  v1.4  Same-chart trigger/filter sources now read via
//                      sc.GetStudyArrayUsingID + sc.BaseDateTimeIn (identity
//                      mapping) instead of sc.GetStudyArrayFromChartUsingID +
//                      sc.GetChartDateTimeArray, matching the external/
//                      internal split already used by RenkoFlipAutoTrader.cpp
//                      and OTFMultiTrader.cpp. Cross-chart behaviour is
//                      unchanged. Added a new debug subgraph, "Debug: Source
//                      Array Size (per slot, see Arrays[0-11])" (SG18), whose
//                      12 aux arrays show every trigger/side slot's resolved
//                      source array size at once, plus a once-per-full-recalc
//                      sc.AddMessageToLog warning naming the trigger, side,
//                      chart, study ID and subgraph whenever a CONFIGURED slot
//                      resolves to an unusable array. Investigated against
//                      Testing/ta-v2-multilab-sourcing-diagnosis.md (2026-07-
//                      30): that diagnosis found the reported "same-chart
//                      TriggerAggregatorV2 source -> zero trades, no error"
//                      symptom was NOT an ACSIL same-chart read defect (this
//                      file's own filter-read code, and TriggerAggregatorV2.cpp
//                      reading V5/V5Sell off its own chart, already prove
//                      sc.GetStudyArrayFromChartUsingID works for chartNum ==
//                      sc.ChartNumber) — the real cause is one of: TA V2's own
//                      upstream sources not connected (it then returns without
//                      writing any subgraph, but Sierra still allocates the
//                      array, so it LOOKS connected while reading all zeros),
//                      a stale Study ID left over from reordering/re-adding a
//                      study, or a misconfigured source subgraph (e.g. "Debug:
//                      Sources Connected", which is a hardcoded constant and
//                      can never satisfy a rising-edge fire test). This
//                      release's same-chart branch is a real fix for a latent
//                      correctness gap (see the branch's own comment) but does
//                      NOT by itself resolve that symptom; the new SG18 +
//                      AddMessageToLog diagnostics are what make each of those
//                      root causes distinguishable on the next occurrence.
// =============================================================================

#include "sierrachart.h"
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>
#include <cstdint>
#include <utility>
#include <limits>

SCDLLName("MultiLab")

// -----------------------------------------------------------------------
// Trade cap. TriggerLab's TL_MAX_TRADES (20000) scaled 6x: six source
// triggers sharing one run can plausibly produce ~6x the row count of one
// TriggerLab run on the same chart history, so the same fixed cap would be
// reached ~6x sooner. Scaling it keeps TriggerLab's actual intent (a LIMIT
// marker on runaway configs, not a ceiling routinely hit by well-configured
// runs) rather than making MultiLab hit it 6x more often. See BuildSpec
// section 7.
// -----------------------------------------------------------------------
static const int ML_MAX_TRADES = 120000;

// Number of trigger slots. Not an input — changing this is a structural
// change (subgraph/input layout both depend on it), not a runtime setting.
static const int ML_NUM_TRIGGERS = 6;

// Upper bound on any delay value (Entry Delay and Delay Sweep Max both use
// IntLimits(0,20) at the input level; sizes the per-delay cell arrays).
static const int ML_MAX_DELAY = 20;

// Stop/target outcome codes. Same meaning as TriggerLab's TLOutcome.
enum MLOutcome { ML_NONE = 0, ML_WIN = 1, ML_LOSS = 2, ML_TIMEOUT = 3 };

// -----------------------------------------------------------------------
// One completed (or pending-completeness) trade record. Same fields as
// TriggerLab's TLTrade, plus:
//   sourceName    — CSV source_name column (BuildSpec section 1).
//   triggerIndex  — 0..5 for a real trigger's trade, -1 for a baseline row.
//                   Used only internally (mask fill, subgraph write); not
//                   itself a CSV column (source_study_id/source_sg/
//                   source_name already identify the trigger).
//   firedMask, firedCount — BuildSpec section 4. Computed in a second pass
//                   AFTER every trigger's entry bars are known for the
//                   whole run, so they cannot be filled at the point the
//                   trade is built (unlike every other field here).
// -----------------------------------------------------------------------
struct MLTrade
{
    int         side;              // +1 long, -1 short
    int         sourceChart;
    int         sourceStudyID;
    int         sourceSG;
    std::string sourceName;
    int         triggerIndex;      // 0..5 real trigger, -1 baseline
    SCDateTime  signalDT;
    SCDateTime  knowableDT;
    SCDateTime  entryDT;
    int         weekday;
    int         sessionMinute;
    double      entryPrice;
    int         delayBars;
    SCDateTime  horizonEndDT;
    int         horizonBarsActual;
    double      mfeTicks;
    double      maeTicks;
    int         barsToMfe;
    int         barsToMae;
    double      minutesToMfe;
    double      minutesToMae;
    double      minutesToStExit;
    double      horizonPnlTicks;
    int         stOutcome;
    double      stPnlTicks;
    bool        truncatedBySession;
    bool        complete;
    bool        isBaseline;
    int         entryBar;
    int         horizonEndBar;
    bool        hasWindow;
    int         runDelay;
    int         firedMask;         // filled in pass B, see MLFillFiredMasks()
    int         firedCount;
};

// -----------------------------------------------------------------------
// Persistent trade vector accessor — identical pattern to TriggerLab's
// TLGetTrades(), same persistent slot number (1), same lifecycle (allocated
// on first call, freed on sc.LastCallToFunction, cleared + rebuilt every
// full-recalc/bar-count-change pass).
// -----------------------------------------------------------------------
static std::vector<MLTrade>* MLGetTrades(SCStudyInterfaceRef sc)
{
    std::vector<MLTrade>* p = (std::vector<MLTrade>*)sc.GetPersistentPointer(1);
    if (p == NULL)
    {
        p = new std::vector<MLTrade>();
        sc.SetPersistentPointer(1, p);
    }
    return p;
}

// Percentile over a SORTED copy of values (nearest-rank). Identical to
// TriggerLab's TLPercentile.
static double MLPercentile(std::vector<double>& sorted, double pct)
{
    const int n = (int)sorted.size();
    if (n == 0)
        return 0.0;
    int idx = (int)(pct * (n - 1));
    if (idx < 0) idx = 0;
    if (idx > n - 1) idx = n - 1;
    return sorted[idx];
}

static double MLAverage(const std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
        sum += v[i];
    return sum / (double)v.size();
}

// SCDateTime -> "YYYY-MM-DD HH:MM:SS" for the CSV export. Identical to
// TriggerLab's TLFormatDT, including the same verified-against-header
// accessor (GetDateTimeYMDHMS at scdatetime.h:1121) — NOT a manual split of
// GetDate(), which is a raw serial day count, not YYYYMMDD.
static SCString MLFormatDT(const SCDateTime& dt)
{
    SCDateTime ncdt = dt;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    ncdt.GetDateTimeYMDHMS(year, month, day, hour, minute, second);

    SCString s;
    s.Format("%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return s;
}

// -----------------------------------------------------------------------
// {bars} token. Identical logic and header citations to TriggerLab's
// TLBarsToken — copied verbatim, not re-derived, since the bar-period enum
// mapping was already confirmed against the installed headers for that
// study and nothing about it changes for a multi-source study.
// -----------------------------------------------------------------------
static SCString MLBarsToken(SCStudyInterfaceRef sc)
{
    n_ACSIL::s_BarPeriod bp;
    sc.GetBarPeriodParameters(bp);

    const int p1 = bp.IntradayChartBarPeriodParameter1;
    const int p2 = bp.IntradayChartBarPeriodParameter2;
    SCString s;

    switch (bp.IntradayChartBarPeriodType)
    {
        case IBPT_DAYS_MINS_SECS:
            if (p1 > 0 && (p1 % 60) == 0)
                s.Format("min%d", p1 / 60);
            else if (p1 > 0)
                s.Format("sec%d", p1);
            else
                s.Format("bp%d_p%d_%d", (int)bp.IntradayChartBarPeriodType, p1, p2);
            break;

        case IBPT_NUM_TRADES_PER_BAR:
            s.Format("tick%d", p1);
            break;

        case IBPT_VOLUME_PER_BAR:
            s.Format("vol%d", p1);
            break;

        case IBPT_DELTA_VOLUME_PER_BAR:
            s.Format("deltavol%d", p1);
            break;

        case IBPT_RANGE_IN_TICKS_STANDARD:
        case IBPT_RANGE_IN_TICKS_NEWBAR_ON_RANGEMET:
        case IBPT_RANGE_IN_TICKS_TRUE:
        case IBPT_RANGE_IN_TICKS_FILL_GAPS:
        case IBPT_RANGE_IN_TICKS_OPEN_EQUAL_CLOSE:
        case IBPT_RANGE_IN_TICKS_NEW_BAR_ON_RANGE_MET_OPEN_EQUALS_PRIOR_CLOSE:
        {
            const double ts = (sc.TickSize > 0) ? sc.TickSize : 1.0;
            s.Format("range%.1f", p1 * ts);
            break;
        }

        case IBPT_REVERSAL_IN_TICKS:
            s.Format("reversal%d", p1);
            break;

        case IBPT_RENKO_IN_TICKS:
        case IBPT_FLEX_RENKO_IN_TICKS:
        case IBPT_FLEX_RENKO_IN_TICKS_INVERSE_SETTINGS:
        case IBPT_ALIGNED_RENKO:
            s.Format("renko%d", p1);
            break;

        case IBPT_PRICE_CHANGES_PER_BAR:
            s.Format("pricechg%d", p1);
            break;

        case IBPT_MONTHS_PER_BAR:
            s.Format("month%d", p1);
            break;

        case IBPT_POINT_AND_FIGURE:
            s.Format("pnf%d_%d", p1, p2);
            break;

        case IBPT_ACSIL_CUSTOM:
            s.Format("custom%d_%d", p1, p2);
            break;

        default:
            s.Format("bp%d_p%d_%d", (int)bp.IntradayChartBarPeriodType, p1, p2);
            break;
    }
    return s;
}

// Replace every occurrence of `from` with `to` in `s`. Identical to
// TriggerLab's TLReplaceAll.
static std::string MLReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty())
        return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
    return s;
}

// Strips characters illegal in a Windows filename, collapses whitespace to
// underscores. Identical to TriggerLab's TLSanitizeFilenamePart.
static std::string MLSanitizeFilenamePart(const std::string& in)
{
    std::string out;
    bool lastWasUnderscore = false;
    for (size_t i = 0; i < in.size(); ++i)
    {
        const char ch = in[i];
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
        {
            continue;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
        {
            if (!lastWasUnderscore)
            {
                out += '_';
                lastWasUnderscore = true;
            }
            continue;
        }
        out += ch;
        lastWasUnderscore = false;
    }
    if (out.empty())
        out = "x";
    return out;
}

// Expands {chart}/{bars}/{delay} in the CSV Path input. Identical to
// TriggerLab's TLExpandCSVPath — {side} still not implemented, and now also
// not per-trigger: all 6 triggers share one CSV, which is the entire point
// of this study, so there is no {trigger} token either.
static SCString MLExpandCSVPath(SCStudyInterfaceRef sc, const SCString& rawPath,
                                 const SCString& barsToken, int entryDelay,
                                 bool sweepMode, int sweepMax)
{
    std::string path = rawPath.GetChars();

    SCString chartTokRaw; chartTokRaw.Format("%d", sc.ChartNumber);
    SCString delayTokRaw;
    if (sweepMode)
        delayTokRaw.Format("dsweep0-%d", sweepMax);
    else
        delayTokRaw.Format("d%d", entryDelay);

    const std::string chartTok = MLSanitizeFilenamePart(chartTokRaw.GetChars());
    const std::string barsTok  = MLSanitizeFilenamePart(barsToken.GetChars());
    const std::string delayTok = MLSanitizeFilenamePart(delayTokRaw.GetChars());

    path = MLReplaceAll(path, "{chart}", chartTok);
    path = MLReplaceAll(path, "{bars}",  barsTok);
    path = MLReplaceAll(path, "{delay}", delayTok);

    SCString result;
    result.Format("%s", path.c_str());
    return result;
}

// -----------------------------------------------------------------------
// SESSION GATE (handles midnight wrap). Identical to TriggerLab's
// TLInSession.
// -----------------------------------------------------------------------
static bool MLInSession(int timeOfDay, int sessStart, int sessEnd)
{
    if (sessStart <= sessEnd)
        return timeOfDay >= sessStart && timeOfDay <= sessEnd;
    else
        return timeOfDay >= sessStart || timeOfDay <= sessEnd;
}

// -----------------------------------------------------------------------
// Directional-filter sampling (MultiLab_Filters_BuildSpec.md section 3).
// Returns the value of a host-chart filter study as of the last bar
// GUARANTEED closed before `mainBar` executes its entry — never `mainBar`
// itself, because Entry Price mode can use `mainBar`'s Open, which occurs
// before that bar (and therefore any same-bar study value derived from its
// Close) exists. `arr`/`srcDT` are fetched ONCE by the caller, outside the
// per-trade loop (BuildSpec section 3) — this function only indexes them.
//
//   - studyID == 0            -> unconfigured slot, returns false.
//   - chartNum == sc.ChartNumber (the confirmed/expected wiring) -> same
//     index space as the main chart, so the answer is simply arr[mainBar-1].
//   - chartNum != sc.ChartNumber (future-proofing only) -> binary search
//     `srcDT` (ascending) for the last source bar whose timestamp is
//     strictly before sc.BaseDateTimeIn[mainBar], i.e. the last one
//     unambiguously closed before mainBar opens. Same alignment discipline
//     TriggerLab/MultiLab already use for trigger sources, generalized.
// -----------------------------------------------------------------------
static bool MLSampleFilterValue(SCStudyInterfaceRef sc, int chartNum, int studyID,
                                 int mainBar, SCFloatArray& arr, SCDateTimeArray& srcDT,
                                 bool arraysLoaded, double& outValue)
{
    if (studyID == 0 || !arraysLoaded)
        return false;

    if (chartNum == sc.ChartNumber)
    {
        const int sampleBar = mainBar - 1;
        if (sampleBar < 0 || sampleBar >= arr.GetArraySize())
            return false;
        outValue = (double)arr[sampleBar];
        return true;
    }

    const int nSrc = srcDT.GetArraySize();
    if (nSrc < 1 || arr.GetArraySize() < 1)
        return false;

    const SCDateTime cutoff = sc.BaseDateTimeIn[mainBar];
    int lo = 0, hi = nSrc - 1, s = -1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (srcDT[mid] < cutoff) { s = mid; lo = mid + 1; }
        else                     { hi = mid - 1; }
    }
    if (s < 0 || s >= arr.GetArraySize())
        return false;
    outValue = (double)arr[s];
    return true;
}

// Formats a tick-distance value, or an empty string when `valid` is false —
// the empty-field convention BuildSpec section 4 requires (never 0, never
// NaN, for "not configured" / "not knowable").
static SCString MLFormatTicksOrEmpty(bool valid, double ticks)
{
    SCString s;
    if (valid)
        s.Format("%.2f", ticks);
    return s;
}

// -----------------------------------------------------------------------
// Settings shared by every trade built this pass, real or Baseline Mode.
// Identical to TriggerLab's TLRunConfig — study-wide, not per-trigger (see
// BuildSpec section 1, "Study-wide settings are shared across all 6
// triggers").
// -----------------------------------------------------------------------
struct MLRunConfig
{
    int    lastClosedBar;
    int    sessStart, sessEnd;
    bool   flattenAtEnd;
    int    horizonMode;
    int    horizonBars;
    int    horizonMinutes;
    bool   evalStopTgt;
    double targetTicks;
    double stopTicks;
    double tickSize;
};

// -----------------------------------------------------------------------
// Builds ONE trade record given an already-chosen entry bar and side.
// Byte-for-byte the same horizon / session-truncation / completeness /
// MFE-MAE / stop-target logic as TriggerLab's TLBuildTradeAtEntry — the
// entire reason this function exists is so real trigger signals (any of
// the 6) and Baseline Mode's pseudo-random entries run through IDENTICAL
// code, exactly as TriggerLab's single trigger and its baseline do today.
// Two new parameters vs. TriggerLab: triggerIndex (which of the 6 slots
// produced this trade; -1 for baseline) and sourceName (the CSV label).
// Neither participates in any measurement — they are pass-through fields
// for the CSV writer and the fired_mask pass.
// -----------------------------------------------------------------------
static MLTrade MLBuildTradeAtEntry(SCStudyInterfaceRef sc, const MLRunConfig& cfg,
                                   int side, int entryBar, int delayBars, bool useOpen,
                                   int sourceChart, int sourceStudyID, int sourceSG,
                                   const std::string& sourceName, int triggerIndex,
                                   SCDateTime signalDT, SCDateTime knowableDT)
{
    MLTrade t;
    t.side               = side;
    t.sourceChart        = sourceChart;
    t.sourceStudyID      = sourceStudyID;
    t.sourceSG           = sourceSG;
    t.sourceName         = sourceName;
    t.triggerIndex       = triggerIndex;
    t.signalDT           = signalDT;
    t.knowableDT         = knowableDT;
    t.entryDT            = sc.BaseDateTimeIn[entryBar];
    t.delayBars          = delayBars;
    t.isBaseline         = false;
    t.runDelay           = delayBars;
    t.firedMask          = 0;   // filled in pass B (MLFillFiredMasks)
    t.firedCount         = 0;

    t.weekday = t.entryDT.GetDayOfWeek() + 1;   // Sunday=1

    const int entryTOD = t.entryDT.GetTime();
    {
        int diff = entryTOD - cfg.sessStart;
        if (diff < 0) diff += 24 * 60 * 60;
        t.sessionMinute = diff / 60;
    }

    double entryPrice;
    if (useOpen)
        entryPrice = sc.Open[entryBar];
    else
        entryPrice = sc.Close[entryBar];
    t.entryPrice = entryPrice;

    // ---- horizon end ----
    int horizonEnd;
    bool minutesCutoffReached = true;
    if (cfg.horizonMode == 0)
    {
        horizonEnd = entryBar + cfg.horizonBars;
    }
    else
    {
        const SCDateTime cutoff = sc.BaseDateTimeIn[entryBar] +
            (double)cfg.horizonMinutes / 1440.0;
        horizonEnd = entryBar;
        while (horizonEnd < cfg.lastClosedBar && sc.BaseDateTimeIn[horizonEnd + 1] <= cutoff)
            ++horizonEnd;
        minutesCutoffReached = (horizonEnd < cfg.lastClosedBar) ||
            (sc.BaseDateTimeIn[horizonEnd] >= cutoff);
    }

    bool truncatedBySession = false;
    if (cfg.flattenAtEnd)
    {
        int sessionCap = horizonEnd;
        while (sessionCap > entryBar &&
               !MLInSession(sc.BaseDateTimeIn[sessionCap].GetTime(), cfg.sessStart, cfg.sessEnd))
            --sessionCap;
        if (sessionCap < horizonEnd)
        {
            horizonEnd = sessionCap;
            truncatedBySession = true;
        }
    }

    bool completeByData = true;
    const int fullHorizonEnd = horizonEnd;
    if (horizonEnd > cfg.lastClosedBar)
    {
        horizonEnd = cfg.lastClosedBar;
        completeByData = false;
    }
    if (!minutesCutoffReached)
        completeByData = false;

    const int winStart = entryBar + 1;
    const bool hasWindow = (winStart <= horizonEnd);

    const bool complete = completeByData && (fullHorizonEnd == horizonEnd) && hasWindow;

    double mfeTicks = 0.0, maeTicks = 0.0;
    int barsToMfe = 0, barsToMae = 0;
    int mfeBar = entryBar, maeBar = entryBar;
    double bestFav = 0.0, bestAdv = 0.0;
    int stOutcome = ML_NONE;
    double stPnlTicks = 0.0;
    bool stResolved = !cfg.evalStopTgt;
    int stExitBar = entryBar;

    if (hasWindow)
    {
        for (int k = winStart; k <= horizonEnd; ++k)
        {
            const double hi = sc.High[k];
            const double lo = sc.Low[k];

            double fav, adv;
            if (side > 0)
            {
                fav = hi - entryPrice;
                adv = entryPrice - lo;
            }
            else
            {
                fav = entryPrice - lo;
                adv = hi - entryPrice;
            }
            if (fav > bestFav) { bestFav = fav; barsToMfe = k - entryBar; mfeBar = k; }
            if (adv > bestAdv) { bestAdv = adv; barsToMae = k - entryBar; maeBar = k; }

            // AMBIGUITY RULE: same-bar stop wins (pessimistic). Identical
            // to TriggerLab.
            if (cfg.evalStopTgt && !stResolved)
            {
                const bool hitTarget = (side > 0) ? (hi >= entryPrice + cfg.targetTicks * cfg.tickSize)
                                                   : (lo <= entryPrice - cfg.targetTicks * cfg.tickSize);
                const bool hitStop   = (side > 0) ? (lo <= entryPrice - cfg.stopTicks * cfg.tickSize)
                                                   : (hi >= entryPrice + cfg.stopTicks * cfg.tickSize);
                if (hitStop)
                {
                    stOutcome  = ML_LOSS;
                    stPnlTicks = -cfg.stopTicks;
                    stResolved = true;
                    stExitBar  = k;
                }
                else if (hitTarget)
                {
                    stOutcome  = ML_WIN;
                    stPnlTicks = cfg.targetTicks;
                    stResolved = true;
                    stExitBar  = k;
                }
            }
        }

        if (cfg.evalStopTgt && !stResolved && complete)
        {
            const double closeAt = sc.Close[horizonEnd];
            stOutcome  = ML_TIMEOUT;
            stPnlTicks = side * (closeAt - entryPrice) / cfg.tickSize;
            stExitBar  = horizonEnd;
        }

        mfeTicks = (bestFav > 0.0) ? bestFav / cfg.tickSize : 0.0;
        maeTicks = (bestAdv > 0.0) ? bestAdv / cfg.tickSize : 0.0;
    }

    double horizonPnlTicks = 0.0;
    if (hasWindow)
    {
        const double closeAt = sc.Close[horizonEnd];
        horizonPnlTicks = side * (closeAt - entryPrice) / cfg.tickSize;
    }

    const double minutesToMfe = (sc.BaseDateTimeIn[mfeBar] - sc.BaseDateTimeIn[entryBar]).GetFloatMinutesSinceBaseDate();
    const double minutesToMae = (sc.BaseDateTimeIn[maeBar] - sc.BaseDateTimeIn[entryBar]).GetFloatMinutesSinceBaseDate();
    const double minutesToStExit = (sc.BaseDateTimeIn[stExitBar] - sc.BaseDateTimeIn[entryBar]).GetFloatMinutesSinceBaseDate();

    t.entryBar           = entryBar;
    t.horizonEndBar      = horizonEnd;
    t.hasWindow          = hasWindow;

    t.horizonEndDT       = sc.BaseDateTimeIn[horizonEnd];
    t.horizonBarsActual  = horizonEnd - entryBar;
    t.mfeTicks           = mfeTicks;
    t.maeTicks           = maeTicks;
    t.barsToMfe          = barsToMfe;
    t.barsToMae          = barsToMae;
    t.minutesToMfe       = minutesToMfe;
    t.minutesToMae       = minutesToMae;
    t.minutesToStExit    = minutesToStExit;
    t.horizonPnlTicks    = horizonPnlTicks;
    t.stOutcome          = stOutcome;
    t.stPnlTicks         = stPnlTicks;
    t.truncatedBySession = truncatedBySession;
    t.complete           = complete;

    return t;
}

// -----------------------------------------------------------------------
// BARRIER SWEEP — identical method and pessimism rule to TriggerLab's
// TLSweepTradeBarriers. Schema UNCHANGED (BuildSpec section 2/9): no
// fired_mask column here, by design — join on trade_id to the main file if
// that is ever needed.
// -----------------------------------------------------------------------
static long MLSweepTradeBarriers(SCStudyInterfaceRef sc, const MLRunConfig& cfg,
                                 const MLTrade& t, int tradeId,
                                 const std::vector<int>& stops,
                                 const std::vector<int>& targets,
                                 double minRR, int runChart, const SCString& barsToken,
                                 FILE* f)
{
    if (f == NULL || !t.hasWindow)
        return 0;

    const int nS = (int)stops.size();
    const int nT = (int)targets.size();
    if (nS == 0 || nT == 0)
        return 0;

    long rowsWritten = 0;

    std::vector<int> firstAdvBar(nS, -1);
    std::vector<int> firstFavBar(nT, -1);

    int nextS = 0, nextT = 0;
    const double entryPrice = t.entryPrice;
    const int    side       = t.side;

    for (int k = t.entryBar + 1; k <= t.horizonEndBar && (nextS < nS || nextT < nT); ++k)
    {
        const double hi = sc.High[k];
        const double lo = sc.Low[k];

        const double favTicks = ((side > 0) ? (hi - entryPrice) : (entryPrice - lo)) / cfg.tickSize;
        const double advTicks = ((side > 0) ? (entryPrice - lo) : (hi - entryPrice)) / cfg.tickSize;

        while (nextS < nS && advTicks >= (double)stops[nextS])
            firstAdvBar[nextS++] = k;
        while (nextT < nT && favTicks >= (double)targets[nextT])
            firstFavBar[nextT++] = k;
    }

    const double closeAt      = sc.Close[t.horizonEndBar];
    const double timeoutTicks = side * (closeAt - entryPrice) / cfg.tickSize;

    const char* sideStr = t.isBaseline ? ((side > 0) ? "LONG_BASE" : "SHORT_BASE")
                                       : ((side > 0) ? "LONG" : "SHORT");

    for (int i = 0; i < nS; ++i)
    {
        for (int j = 0; j < nT; ++j)
        {
            if ((double)targets[j] < (double)stops[i] * minRR)
                continue;

            const int ka = firstAdvBar[i];
            const int kt = firstFavBar[j];

            const char* outcome;
            double pnlTicks;
            int    exitBar;

            if (ka < 0 && kt < 0)
            {
                outcome  = "TIMEOUT";
                pnlTicks = timeoutTicks;
                exitBar  = t.horizonEndBar;
            }
            else if (ka >= 0 && (kt < 0 || ka <= kt))
            {
                outcome  = "LOSS";
                pnlTicks = -(double)stops[i];
                exitBar  = ka;
            }
            else
            {
                outcome  = "WIN";
                pnlTicks = (double)targets[j];
                exitBar  = kt;
            }

            const double minutesToExit =
                (sc.BaseDateTimeIn[exitBar] - sc.BaseDateTimeIn[t.entryBar]).GetFloatMinutesSinceBaseDate();

            fprintf(f, "%d,%s,%d,%d,%s,%d,%d,%d,%s,%.2f,%.2f\n",
                    runChart, barsToken.GetChars(), t.runDelay, tradeId, sideStr,
                    t.complete ? 1 : 0, stops[i], targets[j],
                    outcome, pnlTicks, minutesToExit);
            ++rowsWritten;
        }
    }

    return rowsWritten;
}

// Builds an ascending level list from min/max/step. Identical to
// TriggerLab's TLBuildLevels.
static std::vector<int> MLBuildLevels(int lo, int hi, int step)
{
    std::vector<int> v;
    if (step <= 0 || hi < lo)
        return v;
    for (int x = lo; x <= hi && (int)v.size() < 512; x += step)
        v.push_back(x);
    return v;
}

// -----------------------------------------------------------------------
// One (trigger, side, delay) cell: the ascending entry-bar list used both
// to build the actual trade records AND, in pass B, as the thing every
// OTHER trigger's rows are checked against for fired_mask (BuildSpec
// section 4.4). entryBars is ascending by construction (signals are
// scanned in ascending source-bar order, destOfSrc is a monotonic cursor),
// so pass B can binary-search it directly with no separate sort step.
// -----------------------------------------------------------------------
struct MLCell
{
    std::vector<int>     entryBars;
    std::vector<MLTrade> tradesInCell;
};

// Returns true if `cell`'s entryBars contains any value in
// [target - lookback, target] — never forward (BuildSpec section 4.1).
static bool MLCellHasEntryInWindow(const std::vector<int>& entryBars, int target, int lookback)
{
    if (entryBars.empty())
        return false;
    const int lo = target - lookback;
    // First entry >= lo.
    std::vector<int>::const_iterator it =
        std::lower_bound(entryBars.begin(), entryBars.end(), lo);
    return (it != entryBars.end()) && (*it <= target);
}

// -----------------------------------------------------------------------
// SetDefaults helpers for the per-trigger subgraph/input pairs. Take their
// handles BY REFERENCE PARAMETER (legal — a function parameter binds a
// fresh reference each call) rather than by array (illegal for a reference
// typedef, see the NOTE ON REFERENCE-TYPE HANDLES comment in scsf_MultiLab).
// Called once per trigger, explicitly, from SetDefaults.
// -----------------------------------------------------------------------
static void MLSetupTriggerSubgraphs(SCSubgraphRef longSG, SCSubgraphRef shortSG,
                                    int triggerNum, int r, int g, int b)
{
    SCString nm;

    nm.Format("Trigger %d Long Entry", triggerNum);
    longSG.Name         = nm;
    longSG.DrawStyle    = DRAWSTYLE_ARROW_UP;
    longSG.LineWidth    = 4;
    longSG.PrimaryColor = RGB(r, g, b);
    longSG.DrawZeros    = 0;

    nm.Format("Trigger %d Short Entry", triggerNum);
    shortSG.Name         = nm;
    shortSG.DrawStyle    = DRAWSTYLE_ARROW_DOWN;
    shortSG.LineWidth    = 4;
    shortSG.PrimaryColor = RGB(r, g, b);
    shortSG.DrawZeros    = 0;
}

static void MLSetupTriggerInputs(SCInputRef enableIn, SCInputRef nameIn,
                                 SCInputRef longChartIn, SCInputRef longStudyIn,
                                 SCInputRef shortChartIn, SCInputRef shortStudyIn,
                                 int triggerNum)
{
    SCString label;

    label.Format("Trigger %d: Enable", triggerNum);
    enableIn.Name = label;
    enableIn.SetYesNo(1);

    label.Format("Trigger %d: Name", triggerNum);
    nameIn.Name = label;
    {
        SCString def; def.Format("Trigger %d", triggerNum);
        nameIn.SetString(def);
    }

    label.Format("Trigger %d: Long Source Chart Number  (0 = this chart)", triggerNum);
    longChartIn.Name = label;
    longChartIn.SetInt(0);
    longChartIn.SetIntLimits(0, 500);

    label.Format("Trigger %d: Long Source Study+Subgraph", triggerNum);
    longStudyIn.Name = label;
    longStudyIn.SetStudySubgraphValues(0, 0);

    label.Format("Trigger %d: Short Source Chart Number  (0 = this chart)", triggerNum);
    shortChartIn.Name = label;
    shortChartIn.SetInt(0);
    shortChartIn.SetIntLimits(0, 500);

    label.Format("Trigger %d: Short Source Study+Subgraph", triggerNum);
    shortStudyIn.Name = label;
    shortStudyIn.SetStudySubgraphValues(0, 0);
}

// Plain struct (no reference members) holding one trigger's resolved
// runtime config — safe to array (unlike the SCInputRef handles that feed
// it). Read once per pass via MLReadTriggerCfg(), then indexed by trigger
// number throughout Pass A / Pass B.
struct MLTriggerCfg
{
    bool        enabled;
    std::string name;
    int         longChart, longStudy, longSG;
    int         shortChart, shortStudy, shortSG;
};

static MLTriggerCfg MLReadTriggerCfg(SCStudyInterfaceRef sc,
                                     SCInputRef enableIn, SCInputRef nameIn,
                                     SCInputRef longChartIn, SCInputRef longStudyIn,
                                     SCInputRef shortChartIn, SCInputRef shortStudyIn)
{
    MLTriggerCfg c;
    c.enabled = enableIn.GetYesNo() != 0;
    c.name    = nameIn.GetString();

    c.longChart = longChartIn.GetInt();
    c.longStudy = longStudyIn.GetStudyID();
    c.longSG    = longStudyIn.GetSubgraphIndex();
    if (c.longChart <= 0)
        c.longChart = sc.ChartNumber;

    c.shortChart = shortChartIn.GetInt();
    c.shortStudy = shortStudyIn.GetStudyID();
    c.shortSG    = shortStudyIn.GetSubgraphIndex();
    if (c.shortChart <= 0)
        c.shortChart = sc.ChartNumber;

    return c;
}

// =============================================================================
SCSFExport scsf_MultiLab(SCStudyInterfaceRef sc)
{
    // -------------------------------------------------------------------------
    // SUBGRAPHS  (18 of 60 used — see BuildSpec section 5 for the full budget
    // table and the "last write wins on the shared debug SGs" tradeoff).
    // SG13-18 must stay DRAWSTYLE_IGNORE so they can never wreck the price
    // scale, same rule as TriggerLab's SG3-SG6.
    // -------------------------------------------------------------------------
    // NOTE ON REFERENCE-TYPE HANDLES: SCSubgraphRef and SCInputRef are
    // reference typedefs in ACSIL (confirmed directly in TriggerLab.cpp's
    // own comment on its SCInputRef ternary-avoidance: "SCInputRef is
    // itself a reference typedef in ACSIL"). Arrays of references are not
    // legal C++, so — unlike the plain-int/bool/std::string config values
    // below, which ARE stored in arrays — every SCSubgraphRef and every
    // SCInputRef in this file is declared individually, exactly as
    // TriggerLab itself does (it has zero loops over either). Passing one
    // of these handles BY REFERENCE into a small helper function (see
    // MLSetupTriggerSubgraphs / MLSetupTriggerInputs below) is fine — a
    // function parameter binds a fresh reference per call; only an ARRAY of
    // the reference type is illegal.
    SCSubgraphRef sg_T1LongEntry  = sc.Subgraph[0];    // SG1
    SCSubgraphRef sg_T1ShortEntry = sc.Subgraph[1];    // SG2
    SCSubgraphRef sg_T2LongEntry  = sc.Subgraph[2];    // SG3
    SCSubgraphRef sg_T2ShortEntry = sc.Subgraph[3];    // SG4
    SCSubgraphRef sg_T3LongEntry  = sc.Subgraph[4];    // SG5
    SCSubgraphRef sg_T3ShortEntry = sc.Subgraph[5];    // SG6
    SCSubgraphRef sg_T4LongEntry  = sc.Subgraph[6];    // SG7
    SCSubgraphRef sg_T4ShortEntry = sc.Subgraph[7];    // SG8
    SCSubgraphRef sg_T5LongEntry  = sc.Subgraph[8];    // SG9
    SCSubgraphRef sg_T5ShortEntry = sc.Subgraph[9];    // SG10
    SCSubgraphRef sg_T6LongEntry  = sc.Subgraph[10];   // SG11
    SCSubgraphRef sg_T6ShortEntry = sc.Subgraph[11];   // SG12
    SCSubgraphRef sg_MFE          = sc.Subgraph[12];   // SG13
    SCSubgraphRef sg_MAE          = sc.Subgraph[13];   // SG14
    SCSubgraphRef sg_HorizonPnl   = sc.Subgraph[14];   // SG15
    SCSubgraphRef sg_FiredCount   = sc.Subgraph[15];   // SG16
    SCSubgraphRef sg_DbgConn      = sc.Subgraph[16];   // SG17
    // Per-slot source array size (v1.4 diagnostic — see BuildSpec section 5
    // and the "SAME-CHART vs CROSS-CHART READS" section). sg_DbgArrSize[i]
    // itself carries the SAME thing sg_DbgConn already reports (mirrored for
    // convenience); the 12 aux arrays are the actual point: Arrays[0..11][i]
    // hold, per trigger/side slot, the GetArraySize() this pass resolved for
    // that slot's source array — 0 for an unconfigured slot OR a configured
    // slot whose array came back unusable (< 2 elements). A slot that is
    // "connected" (sg_DbgConn counts it) but whose source study never
    // actually wrote anything (e.g. TriggerAggregatorV2 bailing out before
    // any subgraph assignment, Testing/ta-v2-multilab-sourcing-diagnosis.md
    // Fact B) still shows a real array size here — size alone does not prove
    // the content is non-zero, only that the read itself succeeded. Combine
    // with the AddMessageToLog warning below and the Values window on the
    // source study for full confirmation.
    SCSubgraphRef sg_DbgArrSize   = sc.Subgraph[17];   // SG18

    // -------------------------------------------------------------------------
    // INPUTS  (70 total, indices 0-69 — see BuildSpec section 8 and section 11
    // item 1: this input count is UNVERIFIED against the real ACSIL Input[]
    // array bound; flagged for a human to check before compiling.)
    //
    // Per-trigger block: indices 0-35 (6 inputs x 6 triggers). One Enable flag
    // per TRIGGER, not per side (BuildSpec section 1) — to run one side only,
    // leave that side's Study+Subgraph unconfigured (Study ID 0), same
    // existing TriggerLab convention for "slot not configured".
    // -------------------------------------------------------------------------
    SCInputRef in_T1Enable          = sc.Input[0];
    SCInputRef in_T1Name            = sc.Input[1];
    SCInputRef in_T1LongChartNum    = sc.Input[2];
    SCInputRef in_T1LongStudySub    = sc.Input[3];
    SCInputRef in_T1ShortChartNum   = sc.Input[4];
    SCInputRef in_T1ShortStudySub   = sc.Input[5];

    SCInputRef in_T2Enable          = sc.Input[6];
    SCInputRef in_T2Name            = sc.Input[7];
    SCInputRef in_T2LongChartNum    = sc.Input[8];
    SCInputRef in_T2LongStudySub    = sc.Input[9];
    SCInputRef in_T2ShortChartNum   = sc.Input[10];
    SCInputRef in_T2ShortStudySub   = sc.Input[11];

    SCInputRef in_T3Enable          = sc.Input[12];
    SCInputRef in_T3Name            = sc.Input[13];
    SCInputRef in_T3LongChartNum    = sc.Input[14];
    SCInputRef in_T3LongStudySub    = sc.Input[15];
    SCInputRef in_T3ShortChartNum   = sc.Input[16];
    SCInputRef in_T3ShortStudySub   = sc.Input[17];

    SCInputRef in_T4Enable          = sc.Input[18];
    SCInputRef in_T4Name            = sc.Input[19];
    SCInputRef in_T4LongChartNum    = sc.Input[20];
    SCInputRef in_T4LongStudySub    = sc.Input[21];
    SCInputRef in_T4ShortChartNum   = sc.Input[22];
    SCInputRef in_T4ShortStudySub   = sc.Input[23];

    SCInputRef in_T5Enable          = sc.Input[24];
    SCInputRef in_T5Name            = sc.Input[25];
    SCInputRef in_T5LongChartNum    = sc.Input[26];
    SCInputRef in_T5LongStudySub    = sc.Input[27];
    SCInputRef in_T5ShortChartNum   = sc.Input[28];
    SCInputRef in_T5ShortStudySub   = sc.Input[29];

    SCInputRef in_T6Enable          = sc.Input[30];
    SCInputRef in_T6Name            = sc.Input[31];
    SCInputRef in_T6LongChartNum    = sc.Input[32];
    SCInputRef in_T6LongStudySub    = sc.Input[33];
    SCInputRef in_T6ShortChartNum   = sc.Input[34];
    SCInputRef in_T6ShortStudySub   = sc.Input[35];

    // Study-wide block: indices 36-69. Shared across all 6 triggers on
    // purpose (BuildSpec section 1) — one Fire Test, one Entry Delay, one
    // session window, etc. for the whole run.
    SCInputRef in_FireTest        = sc.Input[36];
    SCInputRef in_FireThreshold   = sc.Input[37];
    SCInputRef in_EntryDelay      = sc.Input[38];
    SCInputRef in_EntryPrice      = sc.Input[39];
    SCInputRef in_Overlap         = sc.Input[40];
    SCInputRef in_SessionStart    = sc.Input[41];
    SCInputRef in_SessionEnd      = sc.Input[42];
    SCInputRef in_FlattenAtEnd    = sc.Input[43];
    SCInputRef in_HorizonMode     = sc.Input[44];
    SCInputRef in_HorizonBars     = sc.Input[45];
    SCInputRef in_HorizonMinutes  = sc.Input[46];
    SCInputRef in_MfeHitTarget    = sc.Input[47];
    SCInputRef in_EvalStopTarget  = sc.Input[48];
    SCInputRef in_Target          = sc.Input[49];
    SCInputRef in_Stop            = sc.Input[50];
    SCInputRef in_ShowPanel       = sc.Input[51];
    SCInputRef in_PanelTextSize   = sc.Input[52];
    SCInputRef in_CSVExport       = sc.Input[53];
    SCInputRef in_CSVPath         = sc.Input[54];
    SCInputRef in_BaselineMode    = sc.Input[55];
    SCInputRef in_BaselinePerDay  = sc.Input[56];
    SCInputRef in_DelaySweep      = sc.Input[57];
    SCInputRef in_DelaySweepMax   = sc.Input[58];
    SCInputRef in_BarrierSweep    = sc.Input[59];
    SCInputRef in_BSStopMin       = sc.Input[60];
    SCInputRef in_BSStopMax       = sc.Input[61];
    SCInputRef in_BSStopStep      = sc.Input[62];
    SCInputRef in_BSTargetMin     = sc.Input[63];
    SCInputRef in_BSTargetMax     = sc.Input[64];
    SCInputRef in_BSTargetStep    = sc.Input[65];
    SCInputRef in_BSMinRR         = sc.Input[66];
    SCInputRef in_BSSweepBaseline = sc.Input[67];
    SCInputRef in_BSDelayMin      = sc.Input[68];
    SCInputRef in_FiredMaskLookback = sc.Input[69];

    // ---- Directional-filter columns (MultiLab_Filters_BuildSpec.md) ----
    // All read studies live on the host chart per the confirmed wiring;
    // chart-number inputs default to 0 = "this chart", resolved the same
    // way MLReadTriggerCfg resolves trigger source charts. Study ID = 0
    // means the slot is unconfigured (BuildSpec section 2/4).
    SCInputRef in_FilterEmaChartNum     = sc.Input[70];
    SCInputRef in_FilterEmaStudySub     = sc.Input[71];
    SCInputRef in_FilterVwap24hChartNum = sc.Input[72];
    SCInputRef in_FilterVwap24hStudySub = sc.Input[73];
    SCInputRef in_FilterVwapRthChartNum = sc.Input[74];
    SCInputRef in_FilterVwapRthStudySub = sc.Input[75];
    SCInputRef in_OpeningRangeSeconds   = sc.Input[76];
    SCInputRef in_FilterOrHighChartNum  = sc.Input[77];
    SCInputRef in_FilterOrHighStudySub  = sc.Input[78];
    SCInputRef in_FilterOrLowChartNum   = sc.Input[79];
    SCInputRef in_FilterOrLowStudySub   = sc.Input[80];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        // Freshness canary — bump on every build that changes the CSV schema.
        sc.GraphName        = "Multi Lab v1.4";
        sc.StudyDescription =
            "Measures 6 triggers' MFE/MAE under a delayed, session-gated entry, "
            "over the full chart history, in ONE run with ONE shared baseline. "
            "fired_mask/fired_count record which other triggers were active at "
            "each entry bar. Not a strategy engine - no trading calls of any "
            "kind. AutoLoop=0, single full pass, rebuild only on full "
            "recalculation or bar-count change.";
        sc.AutoLoop    = 0;
        sc.GraphRegion = 0;
        sc.FreeDLL     = 0;
        sc.DrawZeros   = 0;

        // VERY_LOW, not LOW. Precedence OUTRANKS chart study order: a study at
        // LOW_PREC_LEVEL (1) always calculates before one at VERY_LOW (2), no
        // matter where it sits in the study list.
        //
        // MultiLab was LOW and read every source fine EXCEPT TriggerAggregator
        // V2, which is VERY_LOW - so MultiLab ran first, read the aggregator's
        // subgraphs before they were filled, saw all zeros, and produced an
        // export containing baseline rows only. No error, no warning: an
        // unfilled source array is indistinguishable from a trigger that never
        // fired. Diagnosed 2026-07-29 on chart 17 renko4.
        //
        // At VERY_LOW the tie with the aggregator is broken by study order, so
        // MULTILAB MUST BE PLACED BELOW ITS SOURCE STUDIES in the chart's study
        // list. That is now a real requirement, not a preference.
        sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL;

        // One distinct color per trigger so the chart can tell them apart at
        // a glance. Long/short of the same trigger share a color; direction
        // is already distinguished by arrow-up vs arrow-down.
        MLSetupTriggerSubgraphs(sg_T1LongEntry, sg_T1ShortEntry, 1,   0, 220,  80);
        MLSetupTriggerSubgraphs(sg_T2LongEntry, sg_T2ShortEntry, 2,   0, 160, 220);
        MLSetupTriggerSubgraphs(sg_T3LongEntry, sg_T3ShortEntry, 3, 220, 160,   0);
        MLSetupTriggerSubgraphs(sg_T4LongEntry, sg_T4ShortEntry, 4, 180,  60, 220);
        MLSetupTriggerSubgraphs(sg_T5LongEntry, sg_T5ShortEntry, 5,  80, 220, 160);
        MLSetupTriggerSubgraphs(sg_T6LongEntry, sg_T6ShortEntry, 6, 220,  80, 140);

        sg_MFE.Name         = "MFE (ticks) [debug, shared across triggers]";
        sg_MFE.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_MFE.DrawZeros    = 0;

        sg_MAE.Name         = "MAE (ticks) [debug, shared across triggers]";
        sg_MAE.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_MAE.DrawZeros    = 0;

        sg_HorizonPnl.Name         = "Horizon P&L (ticks) [debug, shared across triggers]";
        sg_HorizonPnl.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_HorizonPnl.DrawZeros    = 0;

        sg_FiredCount.Name         = "Fired Count (debug, shared across triggers)";
        sg_FiredCount.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_FiredCount.DrawZeros    = 0;

        sg_DbgConn.Name         = "Debug: Sources Connected (of up to 12)";
        sg_DbgConn.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_DbgConn.DrawZeros    = 1;

        // v1.4 — per-slot resolved array size, one value per trigger/side in
        // Arrays[0..11] (slot = k*2 + sideIdx, k=0..5, sideIdx 0=Long,
        // 1=Short — same order the trigger inputs are laid out in). Read via
        // Chart > Study Values (Values window), or plot an individual Arrays
        // index with Draw Style = Line if you need it on the chart. 0 means
        // "unconfigured or unreadable this pass" for that slot.
        sg_DbgArrSize.Name         = "Debug: Source Array Size (per slot, see Arrays[0-11])";
        sg_DbgArrSize.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_DbgArrSize.DrawZeros    = 1;

        MLSetupTriggerInputs(in_T1Enable, in_T1Name, in_T1LongChartNum, in_T1LongStudySub,
                            in_T1ShortChartNum, in_T1ShortStudySub, 1);
        MLSetupTriggerInputs(in_T2Enable, in_T2Name, in_T2LongChartNum, in_T2LongStudySub,
                            in_T2ShortChartNum, in_T2ShortStudySub, 2);
        MLSetupTriggerInputs(in_T3Enable, in_T3Name, in_T3LongChartNum, in_T3LongStudySub,
                            in_T3ShortChartNum, in_T3ShortStudySub, 3);
        MLSetupTriggerInputs(in_T4Enable, in_T4Name, in_T4LongChartNum, in_T4LongStudySub,
                            in_T4ShortChartNum, in_T4ShortStudySub, 4);
        MLSetupTriggerInputs(in_T5Enable, in_T5Name, in_T5LongChartNum, in_T5LongStudySub,
                            in_T5ShortChartNum, in_T5ShortStudySub, 5);
        MLSetupTriggerInputs(in_T6Enable, in_T6Name, in_T6LongChartNum, in_T6LongStudySub,
                            in_T6ShortChartNum, in_T6ShortStudySub, 6);

        in_FireTest.Name = "Fire Test  (applies to all 6 triggers)";
        in_FireTest.SetCustomInputStrings(
            "Non-Zero Rising Edge;Cross Above Threshold;Cross Below Threshold");
        in_FireTest.SetCustomInputIndex(0);

        in_FireThreshold.Name = "Fire Threshold  (used by Cross Above/Below)";
        in_FireThreshold.SetFloat(0.0f);

        in_EntryDelay.Name = "Entry Delay (bars)  (applies to all 6 triggers)";
        in_EntryDelay.SetInt(2);
        in_EntryDelay.SetIntLimits(0, 20);

        in_EntryPrice.Name = "Entry Price";
        in_EntryPrice.SetCustomInputStrings("Close of Entry Bar;Open of Entry Bar");
        in_EntryPrice.SetCustomInputIndex(0);

        in_Overlap.Name = "Overlapping Signals";
        in_Overlap.SetCustomInputStrings("Skip While In Trade;Allow All");
        in_Overlap.SetCustomInputIndex(0);

        in_SessionStart.Name = "Session Start Time";
        in_SessionStart.SetTime(HMS_TIME(9, 30, 0));

        in_SessionEnd.Name = "Session End Time";
        in_SessionEnd.SetTime(HMS_TIME(16, 0, 0));

        in_FlattenAtEnd.Name = "Flatten At Session End";
        in_FlattenAtEnd.SetYesNo(1);

        in_HorizonMode.Name = "Horizon Mode";
        in_HorizonMode.SetCustomInputStrings("Bars;Minutes");
        in_HorizonMode.SetCustomInputIndex(0);

        in_HorizonBars.Name = "Horizon Bars";
        in_HorizonBars.SetInt(20);
        in_HorizonBars.SetIntLimits(1, 2000);

        in_HorizonMinutes.Name = "Horizon Minutes";
        in_HorizonMinutes.SetInt(30);
        in_HorizonMinutes.SetIntLimits(1, 2000);

        in_MfeHitTarget.Name = "MFE Hit Target (ticks)";
        in_MfeHitTarget.SetInt(20);
        in_MfeHitTarget.SetIntLimits(1, 10000);

        in_EvalStopTarget.Name = "Evaluate Stop/Target";
        in_EvalStopTarget.SetYesNo(0);

        in_Target.Name = "Target (ticks)";
        in_Target.SetInt(20);
        in_Target.SetIntLimits(1, 10000);

        in_Stop.Name = "Stop (ticks)";
        in_Stop.SetInt(10);
        in_Stop.SetIntLimits(1, 10000);

        in_ShowPanel.Name = "Show Panel";
        in_ShowPanel.SetYesNo(1);

        in_PanelTextSize.Name = "Panel Text Size";
        in_PanelTextSize.SetInt(10);
        in_PanelTextSize.SetIntLimits(6, 32);

        in_CSVExport.Name = "CSV Export";
        in_CSVExport.SetYesNo(0);

        in_CSVPath.Name = "CSV Path  ({chart},{bars},{delay} tokens expanded)";
        in_CSVPath.SetString("C:\\SierraChart\\Data\\MultiLab_{chart}_{bars}_{delay}.csv");

        in_BaselineMode.Name = "Baseline Mode  (adds random control-group entries to the CSV, ONCE per side, shared by all 6 triggers)";
        in_BaselineMode.SetYesNo(0);

        in_BaselinePerDay.Name = "Baseline Entries Per Day";
        in_BaselinePerDay.SetInt(5);
        in_BaselinePerDay.SetIntLimits(1, 200);

        in_DelaySweep.Name = "Delay Sweep  (emit a trade at every delay 0..Max; ignores Entry Delay)";
        in_DelaySweep.SetYesNo(0);

        in_DelaySweepMax.Name = "Delay Sweep Max";
        in_DelaySweepMax.SetInt(3);
        in_DelaySweepMax.SetIntLimits(0, 20);

        in_BarrierSweep.Name = "Barrier Sweep  (second CSV: every stop x target pair, real path)";
        in_BarrierSweep.SetYesNo(0);

        in_BSStopMin.Name = "  Barrier Sweep: Stop Min (ticks)";
        in_BSStopMin.SetInt(10);
        in_BSStopMin.SetIntLimits(1, 1000);

        in_BSStopMax.Name = "  Barrier Sweep: Stop Max (ticks)";
        in_BSStopMax.SetInt(40);
        in_BSStopMax.SetIntLimits(1, 1000);

        in_BSStopStep.Name = "  Barrier Sweep: Stop Step (ticks)";
        in_BSStopStep.SetInt(2);
        in_BSStopStep.SetIntLimits(1, 1000);

        in_BSTargetMin.Name = "  Barrier Sweep: Target Min (ticks)";
        in_BSTargetMin.SetInt(10);
        in_BSTargetMin.SetIntLimits(1, 5000);

        in_BSTargetMax.Name = "  Barrier Sweep: Target Max (ticks)";
        in_BSTargetMax.SetInt(120);
        in_BSTargetMax.SetIntLimits(1, 5000);

        in_BSTargetStep.Name = "  Barrier Sweep: Target Step (ticks)";
        in_BSTargetStep.SetInt(4);
        in_BSTargetStep.SetIntLimits(1, 1000);

        in_BSMinRR.Name = "  Barrier Sweep: Min Reward:Risk (target >= stop x this)";
        in_BSMinRR.SetFloat(1.0f);
        in_BSMinRR.SetFloatLimits(0.1f, 100.0f);

        in_BSSweepBaseline.Name = "  Barrier Sweep: Include Baseline Rows";
        in_BSSweepBaseline.SetYesNo(1);

        in_BSDelayMin.Name = "  Barrier Sweep: Min Delay To Sweep";
        in_BSDelayMin.SetInt(1);
        in_BSDelayMin.SetIntLimits(0, 20);

        // ---- fired_mask ----
        // Lookback window, NEVER forward (BuildSpec section 4.1). 0 = same
        // entry bar only, exact alignment. Configurable because a trigger
        // that fired 1-2 bars earlier is arguably still confirming.
        in_FiredMaskLookback.Name = "Fired Mask Lookback (bars)  (0 = same entry bar only, never forward)";
        in_FiredMaskLookback.SetInt(0);
        in_FiredMaskLookback.SetIntLimits(0, 50);

        // ---- Directional-filter columns (BuildSpec section 2) ----
        // Each filter is a chart-number/study+subgraph pair, exactly mirroring
        // the trigger source inputs above: SetStudySubgraphValues renders the
        // selectable study+subgraph dropdown instead of making the user type a
        // raw study ID, and a Study ID of 0 (no dropdown selection) still means
        // "not configured" for MLReadTriggerCfg-style callers below.
        in_FilterEmaChartNum.Name = "Filter EMA: Source Chart Number  (0 = this chart)";
        in_FilterEmaChartNum.SetInt(0);
        in_FilterEmaChartNum.SetIntLimits(0, 500);

        in_FilterEmaStudySub.Name = "Filter EMA: Source Study+Subgraph";
        in_FilterEmaStudySub.SetStudySubgraphValues(0, 0);

        in_FilterVwap24hChartNum.Name = "Filter VWAP 24h: Source Chart Number  (0 = this chart)";
        in_FilterVwap24hChartNum.SetInt(0);
        in_FilterVwap24hChartNum.SetIntLimits(0, 500);

        in_FilterVwap24hStudySub.Name = "Filter VWAP 24h: Source Study+Subgraph";
        in_FilterVwap24hStudySub.SetStudySubgraphValues(0, 0);

        in_FilterVwapRthChartNum.Name = "Filter VWAP RTH: Source Chart Number  (0 = this chart)";
        in_FilterVwapRthChartNum.SetInt(0);
        in_FilterVwapRthChartNum.SetIntLimits(0, 500);

        in_FilterVwapRthStudySub.Name = "Filter VWAP RTH: Source Study+Subgraph";
        in_FilterVwapRthStudySub.SetStudySubgraphValues(0, 0);

        // Internal opening range — NOT read from a study (BuildSpec section
        // 3). Default 30 matches the "first 30 seconds" spec; swept later
        // without a rebuild by changing this input.
        in_OpeningRangeSeconds.Name = "Opening Range Seconds";
        in_OpeningRangeSeconds.SetInt(30);
        in_OpeningRangeSeconds.SetIntLimits(1, 3600);

        // Study-sourced opening range — deliberately redundant with the
        // internally-computed one above, purely to validate it (BuildSpec
        // section 6).
        in_FilterOrHighChartNum.Name = "Filter OR High: Source Chart Number  (0 = this chart)";
        in_FilterOrHighChartNum.SetInt(0);
        in_FilterOrHighChartNum.SetIntLimits(0, 500);

        in_FilterOrHighStudySub.Name = "Filter OR High: Source Study+Subgraph";
        in_FilterOrHighStudySub.SetStudySubgraphValues(0, 0);

        in_FilterOrLowChartNum.Name = "Filter OR Low: Source Chart Number  (0 = this chart)";
        in_FilterOrLowChartNum.SetInt(0);
        in_FilterOrLowChartNum.SetIntLimits(0, 500);

        in_FilterOrLowStudySub.Name = "Filter OR Low: Source Study+Subgraph";
        in_FilterOrLowStudySub.SetStudySubgraphValues(0, 0);

        return;
    }

    // =========================================================================
    // LAST CALL — free the persistent trade vector
    // =========================================================================
    if (sc.LastCallToFunction)
    {
        std::vector<MLTrade>* p = (std::vector<MLTrade>*)sc.GetPersistentPointer(1);
        if (p != NULL)
        {
            delete p;
            sc.SetPersistentPointer(1, NULL);
        }
        return;
    }

    // =========================================================================
    // GUARD
    // =========================================================================
    const int totalBars = sc.ArraySize;
    if (totalBars < 2)
        return;

    const int lastBar       = totalBars - 1;
    const int lastClosedBar = totalBars - 2;
    if (lastClosedBar < 0)
        return;

    // =========================================================================
    // CALCULATION MODEL — identical gating to TriggerLab: rebuild the ENTIRE
    // trade list on full recalc or bar-count change, otherwise return
    // immediately.
    // =========================================================================
    int& lastKnownBars = sc.GetPersistentInt(1);
    const bool isFullRecalc = (sc.UpdateStartIndex == 0) || sc.IsFullRecalculation;
    const bool barsChanged  = (totalBars != lastKnownBars);

    if (!isFullRecalc && !barsChanged)
        return;

    lastKnownBars = totalBars;

    // -------------------------------------------------------------------------
    // READ PER-TRIGGER SETTINGS
    // -------------------------------------------------------------------------
    // Plain-struct array (see MLTriggerCfg above) — the SCInputRef handles
    // themselves cannot be arrayed (reference typedef), but their RESOLVED
    // VALUES can, and that is all Pass A/B ever need to index by trigger
    // number.
    const MLTriggerCfg trigCfg[ML_NUM_TRIGGERS] =
    {
        MLReadTriggerCfg(sc, in_T1Enable, in_T1Name, in_T1LongChartNum, in_T1LongStudySub,
                         in_T1ShortChartNum, in_T1ShortStudySub),
        MLReadTriggerCfg(sc, in_T2Enable, in_T2Name, in_T2LongChartNum, in_T2LongStudySub,
                         in_T2ShortChartNum, in_T2ShortStudySub),
        MLReadTriggerCfg(sc, in_T3Enable, in_T3Name, in_T3LongChartNum, in_T3LongStudySub,
                         in_T3ShortChartNum, in_T3ShortStudySub),
        MLReadTriggerCfg(sc, in_T4Enable, in_T4Name, in_T4LongChartNum, in_T4LongStudySub,
                         in_T4ShortChartNum, in_T4ShortStudySub),
        MLReadTriggerCfg(sc, in_T5Enable, in_T5Name, in_T5LongChartNum, in_T5LongStudySub,
                         in_T5ShortChartNum, in_T5ShortStudySub),
        MLReadTriggerCfg(sc, in_T6Enable, in_T6Name, in_T6LongChartNum, in_T6LongStudySub,
                         in_T6ShortChartNum, in_T6ShortStudySub),
    };

    // -------------------------------------------------------------------------
    // READ STUDY-WIDE SETTINGS  (shared by all 6 triggers)
    // -------------------------------------------------------------------------
    const int   fireMode      = in_FireTest.GetIndex();
    const float fireThresh    = in_FireThreshold.GetFloat();

    const int   entryDelay    = in_EntryDelay.GetInt();
    const int   entryPriceMode= in_EntryPrice.GetIndex();
    const int   overlapMode   = in_Overlap.GetIndex();

    const int   sessStart     = in_SessionStart.GetTime();
    const int   sessEnd       = in_SessionEnd.GetTime();
    const bool  flattenAtEnd  = in_FlattenAtEnd.GetYesNo() != 0;

    const int   horizonMode   = in_HorizonMode.GetIndex();
    const int   horizonBars   = in_HorizonBars.GetInt();
    const int   horizonMinutes= in_HorizonMinutes.GetInt();

    const bool  evalStopTgt   = in_EvalStopTarget.GetYesNo() != 0;
    const double targetTicks  = (double)in_Target.GetInt();
    const double stopTicks    = (double)in_Stop.GetInt();

    const bool  csvExport     = in_CSVExport.GetYesNo() != 0;
    SCString    csvPath       = in_CSVPath.GetString();

    const bool  bsEnabled     = in_BarrierSweep.GetYesNo() != 0;
    const bool  bsBaseline    = in_BSSweepBaseline.GetYesNo() != 0;
    const int   bsDelayMin    = in_BSDelayMin.GetInt();
    const double bsMinRR      = (double)in_BSMinRR.GetFloat();

    const double tickSize = (sc.TickSize > 0) ? sc.TickSize : 0.01;

    const bool entryPriceFallback = (entryPriceMode == 1 && entryDelay == 0);
    const bool useOpenEntry = (entryPriceMode == 1) && !entryPriceFallback;

    const bool baselineMode    = in_BaselineMode.GetYesNo() != 0;
    const int  baselinePerDay  = in_BaselinePerDay.GetInt();

    const bool sweepMode = in_DelaySweep.GetYesNo() != 0;
    int sweepMax = in_DelaySweepMax.GetInt();
    if (sweepMax < 0) sweepMax = 0;
    if (sweepMax > ML_MAX_DELAY) sweepMax = ML_MAX_DELAY;
    const int dStart = sweepMode ? 0 : entryDelay;
    const int dEnd   = sweepMode ? sweepMax : entryDelay;

    const int firedMaskLookback = in_FiredMaskLookback.GetInt();

    // ---- Directional-filter settings (MultiLab_Filters_BuildSpec.md) ----
    // Each pair is read via GetStudyID()/GetSubgraphIndex(), same as the
    // trigger sources in MLReadTriggerCfg above. GetStudyID() returns 0 when
    // the dropdown has no selection, so the "0 = not configured" contract
    // below is unchanged.
    int filterEmaChart = in_FilterEmaChartNum.GetInt();
    if (filterEmaChart <= 0) filterEmaChart = sc.ChartNumber;
    const int filterEmaStudyID = in_FilterEmaStudySub.GetStudyID();
    const int filterEmaSG      = in_FilterEmaStudySub.GetSubgraphIndex();

    int filterVwap24hChart = in_FilterVwap24hChartNum.GetInt();
    if (filterVwap24hChart <= 0) filterVwap24hChart = sc.ChartNumber;
    const int filterVwap24hStudyID = in_FilterVwap24hStudySub.GetStudyID();
    const int filterVwap24hSG      = in_FilterVwap24hStudySub.GetSubgraphIndex();

    int filterVwapRthChart = in_FilterVwapRthChartNum.GetInt();
    if (filterVwapRthChart <= 0) filterVwapRthChart = sc.ChartNumber;
    const int filterVwapRthStudyID = in_FilterVwapRthStudySub.GetStudyID();
    const int filterVwapRthSG      = in_FilterVwapRthStudySub.GetSubgraphIndex();

    const int openingRangeSeconds = in_OpeningRangeSeconds.GetInt();

    int filterOrHighChart = in_FilterOrHighChartNum.GetInt();
    if (filterOrHighChart <= 0) filterOrHighChart = sc.ChartNumber;
    const int filterOrHighStudyID = in_FilterOrHighStudySub.GetStudyID();
    const int filterOrHighSG      = in_FilterOrHighStudySub.GetSubgraphIndex();

    int filterOrLowChart = in_FilterOrLowChartNum.GetInt();
    if (filterOrLowChart <= 0) filterOrLowChart = sc.ChartNumber;
    const int filterOrLowStudyID = in_FilterOrLowStudySub.GetStudyID();
    const int filterOrLowSG      = in_FilterOrLowStudySub.GetSubgraphIndex();

    MLRunConfig runCfg;
    runCfg.lastClosedBar = lastClosedBar;
    runCfg.sessStart     = sessStart;
    runCfg.sessEnd       = sessEnd;
    runCfg.flattenAtEnd  = flattenAtEnd;
    runCfg.horizonMode   = horizonMode;
    runCfg.horizonBars   = horizonBars;
    runCfg.horizonMinutes= horizonMinutes;
    runCfg.evalStopTgt   = evalStopTgt;
    runCfg.targetTicks   = targetTicks;
    runCfg.stopTicks     = stopTicks;
    runCfg.tickSize      = tickSize;

    // =========================================================================
    // ZERO SUBGRAPHS ACROSS THE FULL RANGE
    // =========================================================================
    for (int i = 0; i <= lastBar; ++i)
    {
        sg_T1LongEntry[i]  = 0.0f;  sg_T1ShortEntry[i] = 0.0f;
        sg_T2LongEntry[i]  = 0.0f;  sg_T2ShortEntry[i] = 0.0f;
        sg_T3LongEntry[i]  = 0.0f;  sg_T3ShortEntry[i] = 0.0f;
        sg_T4LongEntry[i]  = 0.0f;  sg_T4ShortEntry[i] = 0.0f;
        sg_T5LongEntry[i]  = 0.0f;  sg_T5ShortEntry[i] = 0.0f;
        sg_T6LongEntry[i]  = 0.0f;  sg_T6ShortEntry[i] = 0.0f;
        sg_MFE[i]        = 0.0f;
        sg_MAE[i]        = 0.0f;
        sg_HorizonPnl[i] = 0.0f;
        sg_FiredCount[i] = 0.0f;
        sg_DbgConn[i]    = 0.0f;
        sg_DbgArrSize[i] = 0.0f;
        for (int slot = 0; slot < 12; ++slot)
            sg_DbgArrSize.Arrays[slot][i] = 0.0f;
    }

    std::vector<MLTrade>* tradesPtr = MLGetTrades(sc);
    std::vector<MLTrade>& trades = *tradesPtr;
    trades.clear();

    bool limitHit = false;
    int  sourcesConnected = 0;
    // Running total across every (trigger, side, delay) cell during Pass A.
    // `trades` itself stays EMPTY until Pass B (rows are only pushed there,
    // after fired_mask is computed), so checking trades.size() during Pass A
    // would never see anything but 0 and would not actually enforce the cap
    // across the whole run — this counter is what makes ML_MAX_TRADES a real
    // ceiling instead of a no-op.
    int accumulatedTrades = 0;

    // =========================================================================
    // PASS A — PER TRIGGER, PER SIDE: fire test + entry building, exactly
    // TriggerLab's per-side loop, now nested inside a per-trigger loop. Real
    // trades are accumulated into cell[k][sideIdx][d] instead of being
    // pushed straight into the global `trades` vector, because fired_mask
    // (pass B, below) needs every trigger's entry bars to already be known
    // before any trade's mask can be computed (BuildSpec section 4.4).
    // =========================================================================
    // Deliberately NOT `static`: a plain local so each chart instance running
    // this study gets its own working set. A static here would be shared
    // across every chart the DLL is attached to, silently mixing one chart's
    // entry bars into another's fired_mask computation the moment two charts
    // ran Multi Lab at once. The persistent trade vector avoids this same
    // trap correctly via sc.GetPersistentPointer (per-instance); this array
    // is pure scratch space rebuilt fresh every full-recalc pass, so a plain
    // local costs nothing extra and carries no cross-instance risk.
    MLCell cell[ML_NUM_TRIGGERS][2][ML_MAX_DELAY + 1];

    for (int k = 0; k < ML_NUM_TRIGGERS; ++k)
    {
        if (!trigCfg[k].enabled)
            continue;

        for (int sideIdx = 0; sideIdx < 2; ++sideIdx)
        {
            const int side = (sideIdx == 0) ? 1 : -1;

            int chartNum, studyID, sgIndex;
            if (sideIdx == 0)
            {
                chartNum = trigCfg[k].longChart;
                studyID  = trigCfg[k].longStudy;
                sgIndex  = trigCfg[k].longSG;
            }
            else
            {
                chartNum = trigCfg[k].shortChart;
                studyID  = trigCfg[k].shortStudy;
                sgIndex  = trigCfg[k].shortSG;
            }
            if (studyID == 0)
                continue;   // slot not configured

            const int dbgSlot = k * 2 + sideIdx;   // 0=T1 Long .. 11=T6 Short

            // ---- DATA LAYER ----
            // Same-chart vs cross-chart branch (v1.4). GetStudyArrayFromChart-
            // UsingID is the cross-chart snapshot API; for a source on THIS
            // chart, the direct sc.GetStudyArrayUsingID (sierrachart.h:2693) is
            // used instead, matching the external/internal split already used
            // by RenkoFlipAutoTrader.cpp:365-368 and OTFMultiTrader.cpp:318-327.
            // When the source is this chart, its bars ARE this chart's bars, so
            // sc.BaseDateTimeIn is used directly instead of a separate
            // sc.GetChartDateTimeArray fetch — same idea already applied to the
            // filter reads below (MLSampleFilterValue's "chartNum ==
            // sc.ChartNumber" branch, mainBar-1 direct index). The merge-walk
            // two blocks down is untouched for both branches; fed an identity
            // srcDT it degenerates to destOfSrc[s] == s, so cross-chart
            // behaviour stays byte-for-byte identical to before this change.
            const bool srcIsThisChart = (chartNum == sc.ChartNumber);
            SCFloatArray arr;
            SCDateTimeArray srcDT;
            if (srcIsThisChart)
            {
                sc.GetStudyArrayUsingID(studyID, sgIndex, arr);
                srcDT = sc.BaseDateTimeIn;
            }
            else
            {
                sc.GetStudyArrayFromChartUsingID(chartNum, studyID, sgIndex, arr);
                sc.GetChartDateTimeArray(chartNum, srcDT);
            }
            const int nSrc    = srcDT.GetArraySize();
            const int arrSize = arr.GetArraySize();

            // v1.4 diagnostic — record the resolved array size for this slot
            // regardless of whether it passes the usability check below (so
            // all 12 slots are visible together in Arrays[0..11]), and warn
            // once per full-recalc pass when a CONFIGURED slot resolves to
            // something too small to use. This function only reaches here on
            // isFullRecalc || barsChanged (see the guard near the top), so
            // this cannot spam on every tick. See
            // Testing/ta-v2-multilab-sourcing-diagnosis.md D1/D2: an allocated
            // but never-written source array was previously indistinguishable
            // from a genuinely quiet trigger — this is the fix for that gap,
            // not a claim that array size alone proves the content is real.
            for (int i = 0; i <= lastBar; ++i)
                sg_DbgArrSize.Arrays[dbgSlot][i] = (float)arrSize;

            if (nSrc < 2 || arrSize < 2)
            {
                SCString warnMsg;
                warnMsg.Format(
                    "MultiLab: Trigger %d \"%s\" %s source unreadable — chart %d, study ID %d, subgraph %d (array size %d)",
                    k + 1, trigCfg[k].name.c_str(), (sideIdx == 0 ? "Long" : "Short"),
                    chartNum, studyID, sgIndex, arrSize);
                sc.AddMessageToLog(warnMsg, 0);
                continue;   // source chart closed/loading — contributes nothing this pass
            }

            ++sourcesConnected;

            std::vector<int> destOfSrc(nSrc);
            {
                int d = 0;
                const SCDateTime firstMain = sc.BaseDateTimeIn[0];
                for (int s = 0; s < nSrc; ++s)
                {
                    while (d + 1 < totalBars && sc.BaseDateTimeIn[d + 1] <= srcDT[s])
                        ++d;
                    destOfSrc[s] = (srcDT[s] < firstMain) ? -1 : d;
                }
            }

            const int alen = arr.GetArraySize();
            int top = nSrc - 2;
            if (top > alen - 1) top = alen - 1;

            const int overlapWindow = (overlapMode == 0) ? 1 : 0;

            // Per-delay watermark, identical role to TriggerLab's
            // lastAcceptedHorizonEnd[TL_MAX_DELAY+1] — one array PER (trigger,
            // side), since six triggers' overlap policies must not interact
            // with each other (BuildSpec section 2).
            int lastAcceptedHorizonEnd[ML_MAX_DELAY + 1];
            for (int di = 0; di <= ML_MAX_DELAY; ++di)
                lastAcceptedHorizonEnd[di] = -1;

            for (int s = 1; s <= top; ++s)
            {
                bool fires = false;
                switch (fireMode)
                {
                    case 0: fires = (arr[s] != 0.0f && arr[s - 1] == 0.0f); break;
                    case 1: fires = (arr[s] > fireThresh && arr[s - 1] <= fireThresh); break;
                    case 2: fires = (arr[s] < fireThresh && arr[s - 1] >= fireThresh); break;
                }
                if (!fires)
                    continue;

                const int spanStart = destOfSrc[s];
                if (spanStart < 0)
                    continue;

                int knowableBar = (s + 1 < nSrc) ? destOfSrc[s + 1] : -1;
                if (knowableBar < 0)
                    continue;
                if (knowableBar > lastBar)
                    knowableBar = lastBar;

                for (int d = dStart; d <= dEnd; ++d)
                {
                    const int entryBar = knowableBar + d;
                    if (entryBar > lastClosedBar)
                        continue;

                    const int entryTOD = sc.BaseDateTimeIn[entryBar].GetTime();
                    if (!MLInSession(entryTOD, sessStart, sessEnd))
                        continue;

                    if (overlapWindow && entryBar <= lastAcceptedHorizonEnd[d])
                        continue;

                    MLTrade t = MLBuildTradeAtEntry(sc, runCfg, side, entryBar, d, useOpenEntry,
                                                    chartNum, studyID, sgIndex,
                                                    trigCfg[k].name, k,
                                                    sc.BaseDateTimeIn[spanStart], sc.BaseDateTimeIn[knowableBar]);
                    t.runDelay = d;

                    if (overlapWindow)
                        lastAcceptedHorizonEnd[d] = entryBar + t.horizonBarsActual;

                    if (accumulatedTrades >= ML_MAX_TRADES)
                    {
                        limitHit = true;
                    }
                    else
                    {
                        cell[k][sideIdx][d].entryBars.push_back(entryBar);
                        cell[k][sideIdx][d].tradesInCell.push_back(t);
                        ++accumulatedTrades;
                    }
                }
            }
        }
    }

    sg_DbgConn[lastBar] = (float)sourcesConnected;

    // =========================================================================
    // PASS B — FIRE MASK FILL, then flush into the global `trades` vector.
    //
    // Order: trigger 0 long, trigger 0 short, trigger 1 long, ... trigger 5
    // short, each in ascending-delay-then-ascending-entry-bar order. This is
    // deterministic pass-to-pass (fixed nested loop, no dependency on how
    // many signals fired), which is all trade_id's uniqueness scheme actually
    // needs (BuildSpec section 6) — it does not reproduce TriggerLab's exact
    // interleaving of signal order and delay order, which is a harmless,
    // deliberate simplification (row order is not semantically meaningful in
    // either study).
    //
    // fired_mask: for i = 0..5 (every trigger, including this trade's own),
    // set bit i if trigger i has a SAME-SIDE entry bar within
    // [entryBar-lookback, entryBar] at the SAME delay this trade was built
    // at. A trade's own trigger always finds itself this way (its own
    // entryBar is trivially inside that window), so fired_count >= 1 always
    // and no special-cased "own bit" is needed (BuildSpec section 4.3).
    // =========================================================================
    for (int k = 0; k < ML_NUM_TRIGGERS; ++k)
    {
        for (int sideIdx = 0; sideIdx < 2; ++sideIdx)
        {
            for (int d = 0; d <= ML_MAX_DELAY; ++d)
            {
                std::vector<MLTrade>& cellTrades = cell[k][sideIdx][d].tradesInCell;
                for (size_t ti = 0; ti < cellTrades.size(); ++ti)
                {
                    MLTrade& t = cellTrades[ti];

                    int mask = 0;
                    for (int j = 0; j < ML_NUM_TRIGGERS; ++j)
                    {
                        if (MLCellHasEntryInWindow(cell[j][sideIdx][d].entryBars, t.entryBar, firedMaskLookback))
                            mask |= (1 << j);
                    }
                    t.firedMask  = mask;
                    // popcount — small fixed width, plain loop is clearer
                    // here than relying on a compiler intrinsic that may not
                    // be available in every ACSIL build environment.
                    int count = 0;
                    for (int b = 0; b < ML_NUM_TRIGGERS; ++b)
                        if (mask & (1 << b))
                            ++count;
                    t.firedCount = count;

                    // ---- write subgraphs on the entry bar, real trades
                    // only, ONLY for the base-configured delay (Input 32) —
                    // same principle as TriggerLab: the chart/panel stay
                    // anchored to one delay regardless of sweep. ----
                    if (d == entryDelay)
                    {
                        // Plain if/else per case, not a ternary over the
                        // SCSubgraphRef pair — same reasoning as TriggerLab's
                        // own SCInputRef comment: these are reference
                        // typedefs, and a ternary between two of them is an
                        // unverified risk with no compiler on hand to check
                        // it collapses cleanly. Switch on trigger index to
                        // pick the right named entry subgraph — cannot be an
                        // array lookup, see the NOTE ON REFERENCE-TYPE
                        // HANDLES comment above.
                        switch (k)
                        {
                            case 0:
                                if (t.side > 0) sg_T1LongEntry[t.entryBar]  = (float)(sc.Low[t.entryBar]);
                                else             sg_T1ShortEntry[t.entryBar] = (float)(sc.High[t.entryBar]);
                                break;
                            case 1:
                                if (t.side > 0) sg_T2LongEntry[t.entryBar]  = (float)(sc.Low[t.entryBar]);
                                else             sg_T2ShortEntry[t.entryBar] = (float)(sc.High[t.entryBar]);
                                break;
                            case 2:
                                if (t.side > 0) sg_T3LongEntry[t.entryBar]  = (float)(sc.Low[t.entryBar]);
                                else             sg_T3ShortEntry[t.entryBar] = (float)(sc.High[t.entryBar]);
                                break;
                            case 3:
                                if (t.side > 0) sg_T4LongEntry[t.entryBar]  = (float)(sc.Low[t.entryBar]);
                                else             sg_T4ShortEntry[t.entryBar] = (float)(sc.High[t.entryBar]);
                                break;
                            case 4:
                                if (t.side > 0) sg_T5LongEntry[t.entryBar]  = (float)(sc.Low[t.entryBar]);
                                else             sg_T5ShortEntry[t.entryBar] = (float)(sc.High[t.entryBar]);
                                break;
                            case 5:
                                if (t.side > 0) sg_T6LongEntry[t.entryBar]  = (float)(sc.Low[t.entryBar]);
                                else             sg_T6ShortEntry[t.entryBar] = (float)(sc.High[t.entryBar]);
                                break;
                        }
                        sg_MFE[t.entryBar]        = (float)t.mfeTicks;
                        sg_MAE[t.entryBar]        = (float)t.maeTicks;
                        sg_HorizonPnl[t.entryBar] = (float)t.horizonPnlTicks;
                        sg_FiredCount[t.entryBar] = (float)t.firedCount;
                    }

                    trades.push_back(t);
                }
            }
        }
    }

    // =========================================================================
    // BASELINE MODE — pseudo-random control-group entries. EMITTED ONCE PER
    // SIDE, SHARED ACROSS ALL 6 TRIGGERS (BuildSpec section 3). This block
    // is deliberately OUTSIDE the per-trigger loop above — it does not read
    // any per-trigger input, and must not be moved inside that loop or it
    // would silently produce 5 baselines instead of 1.
    //
    // Same TLBuildTradeAtEntry-equivalent code path as real trades (not a
    // parallel implementation), same determinism (seeded from chart number +
    // day serial + side, xorshift64*), same "no delay, no overlap policy"
    // rules as TriggerLab's baseline. See TriggerLab.cpp's own Baseline Mode
    // comment block for the full reasoning — unchanged here.
    // =========================================================================
    if (baselineMode && baselinePerDay > 0)
    {
        for (int sideIdx = 0; sideIdx < 2; ++sideIdx)
        {
            const int side = (sideIdx == 0) ? 1 : -1;
            const uint64_t sideSalt = (side > 0) ? 0x9E3779B97F4A7C15ULL : 0xC2B2AE3D27D4EB4FULL;
            const bool useOpenBaseline = useOpenEntry;

            int dayStart = 0;
            while (dayStart <= lastClosedBar)
            {
                const int dayKey = sc.BaseDateTimeIn[dayStart].GetDate();
                int dayEnd = dayStart;
                while (dayEnd + 1 <= lastClosedBar && sc.BaseDateTimeIn[dayEnd + 1].GetDate() == dayKey)
                    ++dayEnd;

                std::vector<int> pool;
                for (int i = dayStart; i <= dayEnd; ++i)
                    if (MLInSession(sc.BaseDateTimeIn[i].GetTime(), sessStart, sessEnd))
                        pool.push_back(i);

                const int kToPick = (baselinePerDay < (int)pool.size()) ? baselinePerDay : (int)pool.size();

                if (kToPick > 0)
                {
                    uint64_t rngState = ((uint64_t)(uint32_t)sc.ChartNumber << 32) ^
                                        (uint64_t)(uint32_t)dayKey ^ sideSalt;
                    if (rngState == 0)
                        rngState = 0x2545F4914F6CDD1DULL;

                    auto nextRand = [&rngState]() -> uint64_t
                    {
                        rngState ^= rngState >> 12;
                        rngState ^= rngState << 25;
                        rngState ^= rngState >> 27;
                        return rngState * 2685821657736338717ULL;
                    };

                    std::vector<int> pickPool = pool;
                    const int n = (int)pickPool.size();
                    for (int pick = 0; pick < kToPick; ++pick)
                    {
                        const int remaining = n - pick;
                        const int j = pick + (int)(nextRand() % (uint64_t)remaining);
                        std::swap(pickPool[pick], pickPool[j]);

                        const int entryBar = pickPool[pick];

                        MLTrade t = MLBuildTradeAtEntry(sc, runCfg, side, entryBar, /*delayBars=*/0,
                                                        useOpenBaseline,
                                                        sc.ChartNumber, /*sourceStudyID=*/0, /*sourceSG=*/0,
                                                        "BASELINE", /*triggerIndex=*/-1,
                                                        sc.BaseDateTimeIn[entryBar], sc.BaseDateTimeIn[entryBar]);
                        t.isBaseline = true;
                        t.runDelay = sweepMode ? -1 : entryDelay;

                        // Baseline fired_mask: anchored to the study's single
                        // configured Entry Delay input, regardless of Delay
                        // Sweep, because baseline itself is delay-invariant
                        // by construction (one draw, broadcast to every
                        // delay cell downstream). See BuildSpec section 4.2,
                        // "Baseline rows are the one exception". fired_count
                        // can legitimately be 0 here — baseline is not owned
                        // by any trigger, so no bit is guaranteed set.
                        // Loop variable deliberately named trigJ, not j — the
                        // enclosing scope already has a `j` (the Fisher-Yates
                        // pivot index above); reusing the name would shadow
                        // it. Legal C++, but needless confusion in a block
                        // that already reads `pickPool[j]` a few lines up.
                        int mask = 0;
                        for (int trigJ = 0; trigJ < ML_NUM_TRIGGERS; ++trigJ)
                        {
                            if (MLCellHasEntryInWindow(cell[trigJ][sideIdx][entryDelay].entryBars,
                                                        entryBar, firedMaskLookback))
                                mask |= (1 << trigJ);
                        }
                        t.firedMask = mask;
                        int count = 0;
                        for (int b = 0; b < ML_NUM_TRIGGERS; ++b)
                            if (mask & (1 << b))
                                ++count;
                        t.firedCount = count;

                        if ((int)trades.size() >= ML_MAX_TRADES)
                            limitHit = true;
                        else
                            trades.push_back(t);
                    }
                }

                dayStart = dayEnd + 1;
            }
        }
    }

    // =========================================================================
    // STATISTICS  (per trigger + overall combined). Only COMPLETE, non-
    // baseline trades at the base-configured delay count — same exclusion
    // rules as TriggerLab's panel stats, generalized from "per side" to "per
    // trigger" since the panel's job here is a live sanity check on each of
    // the 6 triggers, not a blend across them.
    // =========================================================================
    struct MLStats
    {
        int    nComplete = 0;
        int    nIncomplete = 0;
        std::vector<double> mfe, mae;
        int    nHitTarget = 0;
        double horizonPnlSum = 0.0;
        int    nWin = 0, nLoss = 0, nTimeout = 0;
        double expectancySum = 0.0;
        double totalTicks = 0.0;
        double grossWin = 0.0, grossLoss = 0.0;
        int    nSTResolved = 0;
    };

    MLStats perTrigger[ML_NUM_TRIGGERS];
    MLStats overall;
    const double mfeHitTarget = (double)in_MfeHitTarget.GetInt();

    for (size_t i = 0; i < trades.size(); ++i)
    {
        MLTrade& t = trades[i];
        if (t.isBaseline)
            continue;
        if (t.runDelay != entryDelay)
            continue;   // panel stays anchored to the ONE base-configured delay

        MLStats& s = perTrigger[t.triggerIndex];

        if (!t.complete)
        {
            s.nIncomplete++;
            overall.nIncomplete++;
            continue;
        }

        s.nComplete++;
        overall.nComplete++;
        s.mfe.push_back(t.mfeTicks);
        s.mae.push_back(t.maeTicks);
        overall.mfe.push_back(t.mfeTicks);
        overall.mae.push_back(t.maeTicks);

        if (t.mfeTicks >= mfeHitTarget) { s.nHitTarget++; overall.nHitTarget++; }

        s.horizonPnlSum += t.horizonPnlTicks;
        overall.horizonPnlSum += t.horizonPnlTicks;

        if (evalStopTgt && t.stOutcome != ML_NONE)
        {
            s.nSTResolved++; overall.nSTResolved++;
            s.expectancySum += t.stPnlTicks;
            overall.expectancySum += t.stPnlTicks;
            s.totalTicks += t.stPnlTicks;
            overall.totalTicks += t.stPnlTicks;

            if (t.stOutcome == ML_WIN)       { s.nWin++;     overall.nWin++; }
            else if (t.stOutcome == ML_LOSS) { s.nLoss++;    overall.nLoss++; }
            else                             { s.nTimeout++; overall.nTimeout++; }

            if (t.stPnlTicks > 0) { s.grossWin  += t.stPnlTicks; overall.grossWin  += t.stPnlTicks; }
            else                  { s.grossLoss += -t.stPnlTicks; overall.grossLoss += -t.stPnlTicks; }
        }
    }

    // =========================================================================
    // OPENING RANGE (first `openingRangeSeconds` of each RTH session) —
    // computed ONCE per full-recalc pass, over the WHOLE chart history,
    // INTERNALLY rather than from a study (MultiLab_Filters_BuildSpec.md
    // section 3) — it must behave identically on every bar type (range,
    // renko, minute), which a fixed-time-per-bar study reading cannot
    // guarantee. Reuses `sessStart`, the exact anchor `sessionMinute`
    // already relies on, so there is one definition of "session start" in
    // this file, not two.
    //
    // orHigh[i]/orLow[i]/orReady[i] describe the session bar i belongs to.
    // orReady[i] is true only once that session's window has fully closed
    // AND captured at least one bar inside it; orHigh[i]/orLow[i] are then
    // fixed for the rest of that session. Bar i is NEVER part of its own
    // orHigh[i]/orLow[i] (see the loop below — a bar only contributes while
    // still inside the window, i.e. while orReady for it is false), so
    // reading orHigh[entryBar]/orLow[entryBar] directly at the entry bar is
    // not lookahead: the value was already fixed by an earlier bar's close.
    //
    // Known limitation: the "diff dropped vs. the previous bar" session
    // boundary detection assumes a session start that does not itself wrap
    // across midnight in a way that produces a false mid-session decrease.
    // True for this project's RTH sessions (9:30 anchor); would need
    // revisiting for a session spanning midnight.
    // =========================================================================
    std::vector<double> orHigh(totalBars, 0.0);
    std::vector<double> orLow(totalBars, 0.0);
    std::vector<bool>   orReady(totalBars, false);
    {
        // NOTE: cannot use std::numeric_limits<double>::max() as an
        // "unset" sentinel here — scstructures.h #defines max(a,b) as a
        // 2-argument function-like macro, which mangles the 0-argument
        // std::numeric_limits<double>::max() call at preprocessing and
        // fails the build (and the parenthesised (std::numeric_limits<
        // double>::max)() workaround is one "tidy the parens" edit away
        // from breaking again). Track "has this session captured a bar
        // inside the OR window yet?" directly with a bool instead.
        int    prevDiff  = -1;
        bool   curSeeded = false;
        double curHigh   = 0.0;
        double curLow    = 0.0;

        for (int i = 0; i <= lastBar; ++i)
        {
            const int tod = sc.BaseDateTimeIn[i].GetTime();
            int diff = tod - sessStart;
            if (diff < 0) diff += 24 * 60 * 60;

            if (prevDiff < 0 || diff < prevDiff)
            {
                // New session: reset the running window accumulator.
                curSeeded = false;
                curHigh   = 0.0;
                curLow    = 0.0;
            }
            prevDiff = diff;

            if (diff < openingRangeSeconds)
            {
                // Bar i is itself inside the window — accumulate, but this
                // bar (and every bar before the window closes) stays NOT
                // ready: an entry here gets empty fields, not a partial range.
                if (!curSeeded)
                {
                    curHigh   = sc.High[i];
                    curLow    = sc.Low[i];
                    curSeeded = true;
                }
                else
                {
                    if (sc.High[i] > curHigh) curHigh = sc.High[i];
                    if (sc.Low[i]  < curLow)  curLow  = sc.Low[i];
                }
                orReady[i] = false;
            }
            else if (curSeeded)
            {
                // Window closed with at least one bar captured inside it.
                orHigh[i]  = curHigh;
                orLow[i]   = curLow;
                orReady[i] = true;
            }
            // else: window closed but never captured a bar this session
            // (chart history starts mid-session) — orReady[i] stays false,
            // correctly "could not be established".
        }
    }

    // -------------------------------------------------------------------------
    // Directional-filter study arrays — fetched ONCE here, outside the
    // per-trade loop below, then indexed per trade by MLSampleFilterValue
    // (BuildSpec section 3). A Study ID of 0 means the slot is unconfigured
    // and its array is simply never fetched.
    // -------------------------------------------------------------------------
    SCFloatArray arrFilterEma, arrFilterVwap24h, arrFilterVwapRth;
    SCFloatArray arrFilterOrHigh, arrFilterOrLow;
    SCDateTimeArray dtFilterEma, dtFilterVwap24h, dtFilterVwapRth;
    SCDateTimeArray dtFilterOrHigh, dtFilterOrLow;
    bool filterEmaLoaded = false, filterVwap24hLoaded = false, filterVwapRthLoaded = false;
    bool filterOrHighLoaded = false, filterOrLowLoaded = false;

    if (filterEmaStudyID != 0)
    {
        // v1.4: same-chart vs cross-chart split, matching the trigger data
        // layer above — sc.GetStudyArrayUsingID for this chart's own studies,
        // sc.GetStudyArrayFromChartUsingID kept only for a genuinely foreign
        // chart. The DateTime fetch below was already skipped for same-chart
        // (mainBar-1 direct index in MLSampleFilterValue); only the array
        // fetch itself needed the same split.
        if (filterEmaChart == sc.ChartNumber)
            sc.GetStudyArrayUsingID(filterEmaStudyID, filterEmaSG, arrFilterEma);
        else
            sc.GetStudyArrayFromChartUsingID(filterEmaChart, filterEmaStudyID, filterEmaSG, arrFilterEma);
        if (filterEmaChart != sc.ChartNumber)
            sc.GetChartDateTimeArray(filterEmaChart, dtFilterEma);
        filterEmaLoaded = (arrFilterEma.GetArraySize() > 0);
    }
    if (filterVwap24hStudyID != 0)
    {
        // v1.4: same-chart vs cross-chart split — see the EMA slot above.
        if (filterVwap24hChart == sc.ChartNumber)
            sc.GetStudyArrayUsingID(filterVwap24hStudyID, filterVwap24hSG, arrFilterVwap24h);
        else
            sc.GetStudyArrayFromChartUsingID(filterVwap24hChart, filterVwap24hStudyID, filterVwap24hSG, arrFilterVwap24h);
        if (filterVwap24hChart != sc.ChartNumber)
            sc.GetChartDateTimeArray(filterVwap24hChart, dtFilterVwap24h);
        filterVwap24hLoaded = (arrFilterVwap24h.GetArraySize() > 0);
    }
    if (filterVwapRthStudyID != 0)
    {
        // v1.4: same-chart vs cross-chart split — see the EMA slot above.
        if (filterVwapRthChart == sc.ChartNumber)
            sc.GetStudyArrayUsingID(filterVwapRthStudyID, filterVwapRthSG, arrFilterVwapRth);
        else
            sc.GetStudyArrayFromChartUsingID(filterVwapRthChart, filterVwapRthStudyID, filterVwapRthSG, arrFilterVwapRth);
        if (filterVwapRthChart != sc.ChartNumber)
            sc.GetChartDateTimeArray(filterVwapRthChart, dtFilterVwapRth);
        filterVwapRthLoaded = (arrFilterVwapRth.GetArraySize() > 0);
    }
    if (filterOrHighStudyID != 0)
    {
        // v1.4: same-chart vs cross-chart split — see the EMA slot above.
        if (filterOrHighChart == sc.ChartNumber)
            sc.GetStudyArrayUsingID(filterOrHighStudyID, filterOrHighSG, arrFilterOrHigh);
        else
            sc.GetStudyArrayFromChartUsingID(filterOrHighChart, filterOrHighStudyID, filterOrHighSG, arrFilterOrHigh);
        if (filterOrHighChart != sc.ChartNumber)
            sc.GetChartDateTimeArray(filterOrHighChart, dtFilterOrHigh);
        filterOrHighLoaded = (arrFilterOrHigh.GetArraySize() > 0);
    }
    if (filterOrLowStudyID != 0)
    {
        // v1.4: same-chart vs cross-chart split — see the EMA slot above.
        if (filterOrLowChart == sc.ChartNumber)
            sc.GetStudyArrayUsingID(filterOrLowStudyID, filterOrLowSG, arrFilterOrLow);
        else
            sc.GetStudyArrayFromChartUsingID(filterOrLowChart, filterOrLowStudyID, filterOrLowSG, arrFilterOrLow);
        if (filterOrLowChart != sc.ChartNumber)
            sc.GetChartDateTimeArray(filterOrLowChart, dtFilterOrLow);
        filterOrLowLoaded = (arrFilterOrLow.GetArraySize() > 0);
    }

    // =========================================================================
    // CSV EXPORT — whole file rewritten every rebuild, header always written.
    // 48 columns: TriggerLab's 36 plus run_fired_mask_lookback, source_name,
    // fired_mask, fired_count (BuildSpec section 9), plus 8 directional-filter
    // distance columns appended at the end (MultiLab_Filters_BuildSpec.md
    // section 5). Every other run_* column and every other field is
    // unchanged in meaning from TriggerLab.
    // =========================================================================
    bool csvWriteFailed = false;
    const SCString barsToken = MLBarsToken(sc);

    if (csvExport)
    {
        const SCString expandedPath = MLExpandCSVPath(sc, csvPath, barsToken, entryDelay, sweepMode, sweepMax);

        FILE* f = fopen(expandedPath.GetChars(), "w");
        if (f == NULL)
        {
            csvWriteFailed = true;
        }
        else
        {
            const char* runHorizonMode = (horizonMode == 0) ? "bars" : "minutes";
            const int   runHorizon     = (horizonMode == 0) ? horizonBars : horizonMinutes;
            const char* runFireTest    = (fireMode == 0) ? "rising_edge"
                                        : (fireMode == 1) ? "cross_above" : "cross_below";
            const char* runEntryPrice  = entryPriceFallback ? "close_fallback"
                                        : (useOpenEntry ? "open" : "close");

            fprintf(f,
                "run_chart,run_bars,run_delay,run_horizon_mode,run_horizon,run_fire_test,run_entry_price,"
                "run_eval_st,run_target,run_stop,run_fired_mask_lookback,"
                "trade_id,side,source_chart,source_study_id,source_sg,source_name,"
                "signal_dt,knowable_dt,entry_dt,weekday,session_minute,"
                "entry_price,delay_bars,horizon_end_dt,horizon_bars_actual,"
                "mfe_ticks,mae_ticks,bars_to_mfe,bars_to_mae,horizon_pnl_ticks,"
                "st_outcome,st_pnl_ticks,truncated_by_session,complete,"
                "minutes_to_mfe,minutes_to_mae,minutes_to_st_exit,"
                "fired_mask,fired_count,"
                "dist_ema9_ticks,dist_vwap24h_ticks,dist_vwaprth_ticks,vwaprth_minus_vwap24h_ticks,"
                "dist_or30_high_ticks,dist_or30_low_ticks,"
                "dist_orstudy_high_ticks,dist_orstudy_low_ticks\n");

            const char* outcomeStr[4] = { "", "WIN", "LOSS", "TIMEOUT" };

            for (size_t i = 0; i < trades.size(); ++i)
            {
                MLTrade& t = trades[i];
                const char* sideStr = t.isBaseline
                    ? ((t.side > 0) ? "LONG_BASE" : "SHORT_BASE")
                    : ((t.side > 0) ? "LONG" : "SHORT");

                // source_name may contain a comma if mistyped by the user;
                // that would corrupt the CSV column count silently. Strip
                // commas defensively rather than trust free-text input.
                std::string safeName = t.sourceName;
                for (size_t ci = 0; ci < safeName.size(); ++ci)
                    if (safeName[ci] == ',') safeName[ci] = ' ';

                // ---- Directional-filter columns (BuildSpec section 3) ----
                double emaVal = 0.0, vwap24hVal = 0.0, vwapRthVal = 0.0;
                double orStudyHighVal = 0.0, orStudyLowVal = 0.0;
                const bool emaOk = MLSampleFilterValue(sc, filterEmaChart, filterEmaStudyID,
                                                       t.entryBar, arrFilterEma, dtFilterEma,
                                                       filterEmaLoaded, emaVal);
                const bool vwap24hOk = MLSampleFilterValue(sc, filterVwap24hChart, filterVwap24hStudyID,
                                                           t.entryBar, arrFilterVwap24h, dtFilterVwap24h,
                                                           filterVwap24hLoaded, vwap24hVal);
                const bool vwapRthOk = MLSampleFilterValue(sc, filterVwapRthChart, filterVwapRthStudyID,
                                                           t.entryBar, arrFilterVwapRth, dtFilterVwapRth,
                                                           filterVwapRthLoaded, vwapRthVal);
                const bool orStudyHighOk = MLSampleFilterValue(sc, filterOrHighChart, filterOrHighStudyID,
                                                               t.entryBar, arrFilterOrHigh, dtFilterOrHigh,
                                                               filterOrHighLoaded, orStudyHighVal);
                const bool orStudyLowOk = MLSampleFilterValue(sc, filterOrLowChart, filterOrLowStudyID,
                                                              t.entryBar, arrFilterOrLow, dtFilterOrLow,
                                                              filterOrLowLoaded, orStudyLowVal);

                const SCString distEma       = MLFormatTicksOrEmpty(emaOk, (t.entryPrice - emaVal) / tickSize);
                const SCString distVwap24h   = MLFormatTicksOrEmpty(vwap24hOk, (t.entryPrice - vwap24hVal) / tickSize);
                const SCString distVwapRth   = MLFormatTicksOrEmpty(vwapRthOk, (t.entryPrice - vwapRthVal) / tickSize);
                const SCString rthMinus24h   = MLFormatTicksOrEmpty(vwap24hOk && vwapRthOk,
                                                                    (vwapRthVal - vwap24hVal) / tickSize);

                const bool orInternalOk = (t.entryBar >= 0 && t.entryBar <= lastBar) && orReady[t.entryBar];
                const SCString distOr30High = MLFormatTicksOrEmpty(orInternalOk,
                                                                    (t.entryPrice - orHigh[t.entryBar]) / tickSize);
                const SCString distOr30Low  = MLFormatTicksOrEmpty(orInternalOk,
                                                                    (t.entryPrice - orLow[t.entryBar]) / tickSize);

                const SCString distOrStudyHigh = MLFormatTicksOrEmpty(orStudyHighOk,
                                                                       (t.entryPrice - orStudyHighVal) / tickSize);
                const SCString distOrStudyLow  = MLFormatTicksOrEmpty(orStudyLowOk,
                                                                       (t.entryPrice - orStudyLowVal) / tickSize);

                fprintf(f,
                    "%d,%s,%d,%s,%d,%s,%s,"
                    "%d,%d,%d,%d,"
                    "%d,%s,%d,%d,%d,%s,"
                    "%s,%s,%s,%d,%d,"
                    "%.6f,%d,%s,%d,"
                    "%.2f,%.2f,%d,%d,%.2f,"
                    "%s,%.2f,%d,%d,"
                    "%.2f,%.2f,%.2f,"
                    "%d,%d,"
                    "%s,%s,%s,%s,"
                    "%s,%s,"
                    "%s,%s\n",
                    sc.ChartNumber, barsToken.GetChars(), t.runDelay, runHorizonMode, runHorizon,
                    runFireTest, runEntryPrice,
                    evalStopTgt ? 1 : 0, (int)targetTicks, (int)stopTicks, firedMaskLookback,
                    (int)i + 1, sideStr,
                    t.sourceChart, t.sourceStudyID, t.sourceSG, safeName.c_str(),
                    MLFormatDT(t.signalDT).GetChars(), MLFormatDT(t.knowableDT).GetChars(),
                    MLFormatDT(t.entryDT).GetChars(), t.weekday, t.sessionMinute,
                    t.entryPrice, t.delayBars, MLFormatDT(t.horizonEndDT).GetChars(), t.horizonBarsActual,
                    t.mfeTicks, t.maeTicks, t.barsToMfe, t.barsToMae, t.horizonPnlTicks,
                    outcomeStr[t.stOutcome], t.stPnlTicks, t.truncatedBySession ? 1 : 0, t.complete ? 1 : 0,
                    t.minutesToMfe, t.minutesToMae, t.minutesToStExit,
                    t.firedMask, t.firedCount,
                    distEma.GetChars(), distVwap24h.GetChars(), distVwapRth.GetChars(), rthMinus24h.GetChars(),
                    distOr30High.GetChars(), distOr30Low.GetChars(),
                    distOrStudyHigh.GetChars(), distOrStudyLow.GetChars());
            }
            fclose(f);
        }
    }

    // =========================================================================
    // BARRIER SWEEP EXPORT — second, narrow CSV. Schema UNCHANGED from
    // TriggerLab (BuildSpec section 2/9). trade_id here is the SAME
    // (int)i + 1 the main writer uses, and both loops walk `trades` in the
    // same order, so the join key is exact — identical guarantee to
    // TriggerLab, just over a vector that now holds 6 triggers' rows.
    // =========================================================================
    bool bsWriteFailed = false;
    int  bsPairCount   = 0;
    long bsRowCount    = 0;

    if (csvExport && bsEnabled && !csvWriteFailed)
    {
        const std::vector<int> bsStops   = MLBuildLevels(in_BSStopMin.GetInt(),
                                                          in_BSStopMax.GetInt(),
                                                          in_BSStopStep.GetInt());
        const std::vector<int> bsTargets = MLBuildLevels(in_BSTargetMin.GetInt(),
                                                          in_BSTargetMax.GetInt(),
                                                          in_BSTargetStep.GetInt());

        for (size_t si = 0; si < bsStops.size(); ++si)
            for (size_t ti = 0; ti < bsTargets.size(); ++ti)
                if ((double)bsTargets[ti] >= (double)bsStops[si] * bsMinRR)
                    ++bsPairCount;

        SCString bsPath = MLExpandCSVPath(sc, csvPath, barsToken, entryDelay, sweepMode, sweepMax);
        {
            std::string p = bsPath.GetChars();
            const size_t dot = p.find_last_of('.');
            if (dot == std::string::npos)
                p += "_barriers.csv";
            else
                p = p.substr(0, dot) + "_barriers" + p.substr(dot);
            bsPath = p.c_str();
        }

        FILE* bf = fopen(bsPath.GetChars(), "w");
        if (bf == NULL || bsPairCount == 0)
        {
            bsWriteFailed = true;
            if (bf != NULL) fclose(bf);
        }
        else
        {
            fprintf(bf,
                "run_chart,run_bars,run_delay,trade_id,side,complete,"
                "stop_ticks,target_ticks,outcome,pnl_ticks,minutes_to_exit\n");

            for (size_t i = 0; i < trades.size(); ++i)
            {
                const MLTrade& t = trades[i];

                if (t.isBaseline)
                {
                    if (!bsBaseline)
                        continue;
                }
                else if (t.runDelay < bsDelayMin)
                {
                    continue;
                }

                bsRowCount += MLSweepTradeBarriers(sc, runCfg, t, (int)i + 1,
                                                   bsStops, bsTargets, bsMinRR,
                                                   sc.ChartNumber, barsToken, bf);
            }
            fclose(bf);
        }
    }

    // =========================================================================
    // PANEL — one row per trigger (long+short pooled, like TriggerLab's
    // "Combined" row generalized from 1 trigger to 6), plus one overall row
    // pooling all 6. Baseline excluded from every stat, same as TriggerLab —
    // this is a live sanity check on the TRIGGERS, not the control group.
    // =========================================================================
    if (in_ShowPanel.GetYesNo())
    {
        auto fmtRow = [&](const char* label, MLStats& s) -> SCString
        {
            std::vector<double> mfeSorted = s.mfe;
            std::vector<double> maeSorted = s.mae;
            std::sort(mfeSorted.begin(), mfeSorted.end());
            std::sort(maeSorted.begin(), maeSorted.end());

            const double mfeAvg = MLAverage(s.mfe);
            const double maeAvg = MLAverage(s.mae);
            const double mfeMed = MLPercentile(mfeSorted, 0.50);
            const double maeMed = MLPercentile(maeSorted, 0.50);
            const double mfeP75 = MLPercentile(mfeSorted, 0.75);
            const double maeP75 = MLPercentile(maeSorted, 0.75);
            const double hitPct = (s.nComplete > 0) ? (100.0 * s.nHitTarget / s.nComplete) : 0.0;
            const double hznPnlAvg = (s.nComplete > 0) ? (s.horizonPnlSum / s.nComplete) : 0.0;

            SCString row;
            row.Format("%s: trades %d | incomplete %d | MFE %.1f/%.1f/%.1f | MAE %.1f/%.1f/%.1f | "
                        ">=%.0ft %.1f%% | HznP&L avg %.1f tot %.1f",
                        label, s.nComplete, s.nIncomplete,
                        mfeAvg, mfeMed, mfeP75, maeAvg, maeMed, maeP75,
                        mfeHitTarget, hitPct, hznPnlAvg,
                        s.horizonPnlSum);

            if (evalStopTgt && s.nSTResolved > 0)
            {
                const double winPct = 100.0 * s.nWin / s.nSTResolved;
                const double expectancy = s.expectancySum / s.nSTResolved;
                const double pf = (s.grossLoss > 0.0) ? (s.grossWin / s.grossLoss) : 0.0;
                SCString stRow;
                stRow.Format(" | Win%% %.1f | Exp %.2f t | Total %.1f t | PF %.2f",
                             winPct, expectancy, s.totalTicks, pf);
                row += stRow;
            }
            return row;
        };

        SCString fallbackNote = entryPriceFallback
            ? "  [Entry Price=Open + Delay=0 -> FELL BACK TO CLOSE (lookahead guard)]"
            : "";
        SCString limitNote = limitHit ? "  *** TRADE LIMIT (120000) REACHED ***" : "";
        SCString csvNote = csvWriteFailed ? "  *** CSV: WRITE FAILED ***" : "";

        SCString baselineNote;
        if (baselineMode)
            baselineNote.Format("\nBaseline: ON (%d/day, ONE shared set for all 6 triggers)", baselinePerDay);

        if (bsEnabled)
        {
            SCString bsNote;
            if (bsWriteFailed)
                bsNote = "\nBarrier Sweep: *** WRITE FAILED / EMPTY GRID ***";
            else if (!csvExport)
                bsNote = "\nBarrier Sweep: IDLE (needs CSV Export = Yes)";
            else
                bsNote.Format("\nBarrier Sweep: %d pairs, %ld rows (~%.1f MB), delay >= %d, baseline %s",
                              bsPairCount, bsRowCount, (double)bsRowCount * 48.0 / 1048576.0,
                              bsDelayMin, bsBaseline ? "IN" : "OUT");
            baselineNote += bsNote;
        }

        SCString triggerRows[ML_NUM_TRIGGERS];
        for (int k = 0; k < ML_NUM_TRIGGERS; ++k)
        {
            SCString label;
            label.Format("%s", trigCfg[k].name.c_str());
            triggerRows[k] = fmtRow(label.GetChars(), perTrigger[k]);
        }

        SCString allTriggerRows;
        for (int k = 0; k < ML_NUM_TRIGGERS; ++k)
        {
            allTriggerRows += triggerRows[k];
            if (k < ML_NUM_TRIGGERS - 1)
                allTriggerRows += "\n";
        }

        SCString txt;
        txt.Format("Multi Lab | Sources connected: %d of 12%s%s%s\n%s\n%s\n%s",
                    sourcesConnected, fallbackNote.GetChars(), limitNote.GetChars(), csvNote.GetChars(),
                    allTriggerRows.GetChars(),
                    fmtRow("Overall (all 6 triggers)", overall).GetChars(),
                    baselineNote.GetChars());

        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.LineNumber  = 84930001;   // distinct from TriggerLab's 84920001
        Tool.AddAsUserDrawnDrawing = 0;
        Tool.AddMethod   = UTAM_ADD_OR_ADJUST;
        Tool.UseRelativeVerticalValues = 1;
        Tool.BeginDateTime = 3;
        Tool.BeginValue    = 90;
        Tool.Color    = RGB(0, 220, 120);
        Tool.FontSize = in_PanelTextSize.GetInt();
        Tool.FontBold = 1;
        Tool.Text     = txt;
        sc.UseTool(Tool);
    }
}
