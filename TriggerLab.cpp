// =============================================================================
// TriggerLab.cpp
// Sierra Chart ACSIL Custom Study — Trigger Lab
//
// Measures what ONE trigger is actually worth, over a long period, under
// realistic conditions. Not a strategy engine, not an auto-trader. It answers
// one question: if I had taken every print of trigger X, entering N bars
// late, only during my session, how far did price go my way before it went
// against me?
//
// Reports Maximum Favourable Excursion (MFE) and Maximum Adverse Excursion
// (MAE) per trade as raw ticks, plus percentiles across the whole trade
// history. MFE/MAE are DESCRIPTIVE, not a backtest with a stop baked in:
//
//     p75 of MAE is the stop candidate. Median of MFE is the target
//     candidate. Choose the stop AFTER seeing the data, not before.
//
// An optional stop/target evaluator (OFF by default) exists only to confirm
// a choice once the MFE/MAE distribution suggests one.
//
// Spec: Trading/Futures_Day_Trading/TriggerLab_BuildSpec.md
//
// NON-NEGOTIABLES (see spec for why):
//   - Fire test runs in SOURCE time on the source chart's own bars.
//   - knowableBar = destOfSrc[s+1], never the source bar's own open.
//   - Entry Price = Open with Entry Delay = 0 falls back to Close (reported).
//   - Excursion window is entryBar+1 .. horizonEnd; entry bar excluded.
//   - Incomplete trades (horizon not fully elapsed) excluded from every stat.
//   - Stop/target ambiguity inside one bar resolves as LOSS (pessimistic).
//   - SG3-SG6 are DRAWSTYLE_IGNORE.
//   - No trading calls of any kind.
//   - sc.AutoLoop = 0, single full pass, rebuild only on full recalc or
//     bar-count change.
//
// Persistent slots used by this study:
//   Persistent Pointer 1  — std::vector<Trade>* trade history (all trades,
//                           long + short, rebuilt from scratch every pass)
//   Persistent Int 1      — last known bar count (rebuild trigger)
// =============================================================================

#include "sierrachart.h"
#include <vector>
#include <algorithm>
#include <cstdio>
#include <string>
#include <cstdint>
#include <utility>

SCDLLName("TriggerLab")

// -----------------------------------------------------------------------
// Trade cap. Beyond this, stop appending and show a LIMIT marker on the
// panel rather than silently truncating stats (spec section 7).
// -----------------------------------------------------------------------
static const int TL_MAX_TRADES = 20000;

// Upper bound on any delay value (Entry Delay and Delay Sweep Max both use
// IntLimits(0,20) at the input level; this sizes the per-delay overlap
// watermark array so every possible delay has its own slot).
static const int TL_MAX_DELAY = 20;

// Stop/target outcome codes.
enum TLOutcome { TL_NONE = 0, TL_WIN = 1, TL_LOSS = 2, TL_TIMEOUT = 3 };

// -----------------------------------------------------------------------
// One completed (or pending-completeness) trade record. Kept in full for
// the life of the study so percentiles (p75, median) can be computed over
// the whole history — that requires the full list, not a running average.
// -----------------------------------------------------------------------
struct TLTrade
{
    int        side;              // +1 long, -1 short
    int        sourceChart;
    int        sourceStudyID;
    int        sourceSG;
    SCDateTime signalDT;           // source bar s's open (spanStart)
    SCDateTime knowableDT;         // knowableBar's open
    SCDateTime entryDT;            // entryBar's open
    int        weekday;            // 1..7 (Sunday = 1)
    int        sessionMinute;      // minutes since Session Start
    double     entryPrice;
    int        delayBars;
    SCDateTime horizonEndDT;
    int        horizonBarsActual;
    double     mfeTicks;
    double     maeTicks;
    int        barsToMfe;
    int        barsToMae;
    double     minutesToMfe;   // elapsed minutes, entry bar -> the bar where MFE was set
    double     minutesToMae;   // elapsed minutes, entry bar -> the bar where MAE was set
    double     minutesToStExit; // elapsed minutes, entry bar -> the bar where stop/target
                                 // resolved (WIN/LOSS: touch bar; TIMEOUT: horizonEnd;
                                 // Evaluate Stop/Target=No: 0.00)
    double     horizonPnlTicks;
    int        stOutcome;
    double     stPnlTicks;
    bool       truncatedBySession;
    bool       complete;
    bool       isBaseline;           // Baseline Mode pseudo-entry, not a real trigger fire.
                                      // side is still +1/-1; CSV writes LONG_BASE/SHORT_BASE.
                                      // Excluded from every panel statistic.
    // Window bounds, kept so Barrier Sweep can re-walk this trade's bars
    // without re-deriving the horizon. Set by TLBuildTradeAtEntry at the
    // point the window is established, so the sweep can never disagree with
    // the single-barrier evaluation about which bars the trade spanned.
    // winStart = entryBar + 1 (entry bar excluded, same rule as MFE/MAE).
    int        entryBar;
    int        horizonEndBar;
    bool       hasWindow;

    int        runDelay;             // CSV run_delay column. Real trades: the delay that
                                      // produced this row (== delayBars). Baseline, non-sweep:
                                      // the run's single Entry Delay (unchanged legacy value).
                                      // Baseline, sweep mode: -1 sentinel ("applies to every
                                      // delay cell"). This is the sweep pipeline's grouping key,
                                      // kept separate from delayBars so baseline's delayBars=0
                                      // stays semantically "no delay applied" while run_delay
                                      // still tells the analysis pipeline which cell(s) to join.
};

// -----------------------------------------------------------------------
// Persistent trade vector accessor. Allocated on first call, freed on
// sc.LastCallToFunction. Rebuilt (cleared + refilled) from scratch on every
// recalculation pass — this is a research tool, correctness beats
// incrementality, and a per-tick incremental append of 20k trades is not
// needed since we already gate the whole rebuild on full-recalc/bar-count
// change (see CALCULATION MODEL below).
// -----------------------------------------------------------------------
static std::vector<TLTrade>* TLGetTrades(SCStudyInterfaceRef sc)
{
    std::vector<TLTrade>* p = (std::vector<TLTrade>*)sc.GetPersistentPointer(1);
    if (p == NULL)
    {
        p = new std::vector<TLTrade>();
        sc.SetPersistentPointer(1, p);
    }
    return p;
}

// Percentile over a SORTED copy of values (nearest-rank, simple and stable
// — this is a research panel, not a statistics package).
static double TLPercentile(std::vector<double>& sorted, double pct)
{
    const int n = (int)sorted.size();
    if (n == 0)
        return 0.0;
    int idx = (int)(pct * (n - 1));
    if (idx < 0) idx = 0;
    if (idx > n - 1) idx = n - 1;
    return sorted[idx];
}

static double TLAverage(const std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
        sum += v[i];
    return sum / (double)v.size();
}

// SCDateTime -> "YYYY-MM-DD HH:MM:SS" for the CSV export.
//
// PREVIOUS BUG: this built Y/M/D from GetDate() by splitting it as a
// packed YYYYMMDD integer. GetDate() is NOT that — it is the DATE PART as
// a raw serial day count (days since SC's epoch), so treating it as YYYYMMDD
// produced garbage (year 0004, month 62). Verified directly against the
// installed header this time, not inferred:
//   C:\SierraChart\ACS_Source\scdatetime.h:1121
//     void GetDateTimeYMDHMS(int& Year, int& Month, int& Day,
//                            int& Hour, int& Minute, int& Second) const;
// This is the real combined accessor and decomposes the FULL SCDateTime
// (date + time) directly into calendar fields in one call — no manual
// splitting of any serial value required.
static SCString TLFormatDT(const SCDateTime& dt)
{
    SCDateTime ncdt = dt;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    ncdt.GetDateTimeYMDHMS(year, month, day, hour, minute, second);

    SCString s;
    s.Format("%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return s;
}

// -----------------------------------------------------------------------
// {bars} token — a filesystem-safe description of the MAIN chart's bar
// period, so runs at different bar types/timeframes write to different
// CSV files and can be pooled afterward.
//
// Derived from sc.GetBarPeriodParameters(n_ACSIL::s_BarPeriod&) and the
// enum IntradayBarPeriodTypeEnum, BOTH CONFIRMED against the real installed
// header (not the trimmed sc_acsil_functions_reference.md, which documents
// only the function's existence and signature, not the struct fields or
// enum ordinals):
//   C:\SierraChart\ACS_Source\sierrachart.h   :3461  GetBarPeriodParameters
//   C:\SierraChart\ACS_Source\scstructures.h  :3258  struct s_BarPeriod
//                                                     (namespace n_ACSIL)
//   C:\SierraChart\ACS_Source\scconstants.h   :1230  IntradayBarPeriodTypeEnum
//
// Range charts report their size in TICKS (IntradayChartBarPeriodParameter1);
// converted to points via sc.TickSize to match Sierra's own "2.0 range"
// display convention. Renko/tick/volume/etc. are left as raw integers —
// Sierra's UI does not apply the same points conversion to those, and there
// was no header evidence either way, so the raw stored value is reported
// rather than guessing a scale factor.
// -----------------------------------------------------------------------
static SCString TLBarsToken(SCStudyInterfaceRef sc)
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
            // Unrecognised type — numeric fallback rather than a guessed
            // label, per spec.
            s.Format("bp%d_p%d_%d", (int)bp.IntradayChartBarPeriodType, p1, p2);
            break;
    }
    return s;
}

// Replace every occurrence of `from` with `to` in `s`. Plain std::string —
// SCString has no confirmed find/replace member anywhere in this repo or
// in sc_acsil_functions_reference.md, so this avoids guessing that API too.
static std::string TLReplaceAll(std::string s, const std::string& from, const std::string& to)
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

// Strips characters illegal in a Windows filename (< > : " / \ | ? *) and
// collapses whitespace to single underscores. Applied to each TOKEN VALUE
// before substitution — never to the whole path — so legitimate path
// separators and the drive-letter colon survive untouched. A token that
// sanitises down to nothing becomes "x" so a malformed token can never
// silently produce an unopenable path (e.g. an empty path segment).
static std::string TLSanitizeFilenamePart(const std::string& in)
{
    std::string out;
    bool lastWasUnderscore = false;
    for (size_t i = 0; i < in.size(); ++i)
    {
        const char ch = in[i];
        if (ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*')
        {
            continue;   // stripped, not replaced
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

// Expands {chart}/{bars}/{delay} in the CSV Path input. {side} is
// deliberately not implemented — both sides share one file, per spec.
//
// {delay} is d<N> normally, e.g. "d2". While Delay Sweep is ON it expands
// to dsweep0-<max> instead (e.g. "dsweep0-3") so a sweep file can never
// collide with, or be mistaken for, a single-delay file — the two are not
// interchangeable data and must not share a filename pattern.
static SCString TLExpandCSVPath(SCStudyInterfaceRef sc, const SCString& rawPath,
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

    const std::string chartTok = TLSanitizeFilenamePart(chartTokRaw.GetChars());
    const std::string barsTok  = TLSanitizeFilenamePart(barsToken.GetChars());
    const std::string delayTok = TLSanitizeFilenamePart(delayTokRaw.GetChars());

    path = TLReplaceAll(path, "{chart}", chartTok);
    path = TLReplaceAll(path, "{bars}",  barsTok);
    path = TLReplaceAll(path, "{delay}", delayTok);

    SCString result;
    result.Format("%s", path.c_str());
    return result;
}

// -----------------------------------------------------------------------
// SESSION GATE  (handles midnight wrap: start > end -> t>=start || t<=end)
// Free function rather than a per-call lambda so both the real-trigger
// loop and Baseline Mode's day/bar sampling call the exact same code.
// -----------------------------------------------------------------------
static bool TLInSession(int timeOfDay, int sessStart, int sessEnd)
{
    if (sessStart <= sessEnd)
        return timeOfDay >= sessStart && timeOfDay <= sessEnd;
    else
        return timeOfDay >= sessStart || timeOfDay <= sessEnd;
}

// -----------------------------------------------------------------------
// Settings shared by every trade built this pass, real or Baseline Mode.
// Grouped so TLBuildTradeAtEntry() takes one bundle instead of a dozen
// loose parameters.
// -----------------------------------------------------------------------
struct TLRunConfig
{
    int    lastClosedBar;
    int    sessStart, sessEnd;
    bool   flattenAtEnd;
    int    horizonMode;      // 0 Bars, 1 Minutes
    int    horizonBars;
    int    horizonMinutes;
    bool   evalStopTgt;
    double targetTicks;
    double stopTicks;
    double tickSize;
};

// -----------------------------------------------------------------------
// Builds ONE trade record given an already-chosen entry bar and side. This
// is the ENTIRE horizon / session-truncation / completeness / MFE-MAE /
// stop-target evaluation, factored out so real trigger signals and
// Baseline Mode's pseudo-random entries run through the IDENTICAL code —
// not a parallel reimplementation. If this logic ever needs to change, it
// changes here once, for both.
//
// `useOpen` is a caller-supplied parameter, not derived from cfg, but the
// RULE for every caller is the same: fill by the identical convention as
// real trades in every configuration. A control group is only valid if it
// differs from the treatment group in exactly one respect — whether the
// entry was chosen by the trigger or at random. Any second difference
// (e.g. a different price-fill rule) contaminates the comparison. This is
// why Baseline Mode's caller passes useOpenEntry (the already-fallback-
// resolved value), not its own recomputation of the Entry Price input —
// even though a baseline entry has no knowable bar and so no lookahead
// risk from raw Open, lookahead is not the constraint that governs a
// control group's price convention; comparability is.
// -----------------------------------------------------------------------
static TLTrade TLBuildTradeAtEntry(SCStudyInterfaceRef sc, const TLRunConfig& cfg,
                                   int side, int entryBar, int delayBars, bool useOpen,
                                   int sourceChart, int sourceStudyID, int sourceSG,
                                   SCDateTime signalDT, SCDateTime knowableDT)
{
    TLTrade t;
    t.side               = side;
    t.sourceChart        = sourceChart;
    t.sourceStudyID      = sourceStudyID;
    t.sourceSG           = sourceSG;
    t.signalDT           = signalDT;
    t.knowableDT         = knowableDT;
    t.entryDT            = sc.BaseDateTimeIn[entryBar];
    t.delayBars          = delayBars;
    t.isBaseline         = false;   // caller flips this for Baseline Mode rows
    t.runDelay           = delayBars;   // sane default; caller overrides for
                                         // baseline rows (legacy value or the
                                         // -1 sweep sentinel — see TLTrade)

    // GetDayOfWeek() is a real, direct SCDateTime accessor. Verified:
    //   C:\SierraChart\ACS_Source\scdatetime.h:1132  int GetDayOfWeek() const;
    //   C:\SierraChart\ACS_Source\scdatetime.h:1834  return DAY_OF_WEEK(GetDate());
    //   C:\SierraChart\ACS_Source\scdatetime.h:55-58  SUNDAY=0, MONDAY=1, ... SATURDAY=6
    t.weekday = t.entryDT.GetDayOfWeek() + 1;   // -> spec's 1..7, Sunday=1

    const int entryTOD = t.entryDT.GetTime();
    {
        int diff = entryTOD - cfg.sessStart;
        if (diff < 0) diff += 24 * 60 * 60;   // midnight-wrap session
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
    bool minutesCutoffReached = true;   // Bars mode has no cutoff to miss
    if (cfg.horizonMode == 0)
    {
        horizonEnd = entryBar + cfg.horizonBars;
    }
    else
    {
        // Minutes mode: walk sc.BaseDateTimeIn forward from the entry bar.
        // SCDateTime stores days as a double, so N minutes = N/1440.0 days.
        const SCDateTime cutoff = sc.BaseDateTimeIn[entryBar] +
            (double)cfg.horizonMinutes / 1440.0;
        horizonEnd = entryBar;
        while (horizonEnd < cfg.lastClosedBar && sc.BaseDateTimeIn[horizonEnd + 1] <= cutoff)
            ++horizonEnd;
        // Cutoff satisfied only if the NEXT bar exists and lies beyond it,
        // or the current bar's own time already passed it. Running out of
        // chart is NOT the same as the horizon having elapsed.
        minutesCutoffReached = (horizonEnd < cfg.lastClosedBar) ||
            (sc.BaseDateTimeIn[horizonEnd] >= cutoff);
    }

    bool truncatedBySession = false;
    if (cfg.flattenAtEnd)
    {
        // Truncate at the last bar still inside the session.
        int sessionCap = horizonEnd;
        while (sessionCap > entryBar &&
               !TLInSession(sc.BaseDateTimeIn[sessionCap].GetTime(), cfg.sessStart, cfg.sessEnd))
            --sessionCap;
        if (sessionCap < horizonEnd)
        {
            horizonEnd = sessionCap;
            truncatedBySession = true;
        }
    }

    // Clamp by the last CLOSED bar — an in-progress bar's High/Low are not
    // final and would fabricate excursion.
    bool completeByData = true;
    const int fullHorizonEnd = horizonEnd;
    if (horizonEnd > cfg.lastClosedBar)
    {
        horizonEnd = cfg.lastClosedBar;
        completeByData = false;
    }
    if (!minutesCutoffReached)
        completeByData = false;

    // Window = entryBar+1 .. horizonEnd inclusive. Entry bar itself is
    // excluded — its post-fill range is unknowable from bar data.
    const int winStart = entryBar + 1;
    const bool hasWindow = (winStart <= horizonEnd);

    // A trade whose horizon has not fully elapsed is excluded from every
    // statistic (still counted, but complete=0).
    const bool complete = completeByData && (fullHorizonEnd == horizonEnd) && hasWindow;

    // ---- MFE / MAE / horizon P&L over the window ----
    double mfeTicks = 0.0, maeTicks = 0.0;
    int barsToMfe = 0, barsToMae = 0;
    // Absolute bar index where MFE/MAE was set. Defaults to entryBar so a
    // trade that never goes favourable/adverse (bestFav/bestAdv stay at 0)
    // measures a zero-length elapsed time -> minutes_to_mfe/mae = 0.00,
    // which is the correct value for that case, not a missing one.
    // Captured HERE, at the point the max is actually observed, and the
    // minutes are derived from these same indexes below — never
    // reconstructed later from barsToMfe/barsToMae, so the two can't drift.
    int mfeBar = entryBar, maeBar = entryBar;
    double bestFav = 0.0, bestAdv = 0.0;   // in price units, clamped >=0
    int stOutcome = TL_NONE;
    double stPnlTicks = 0.0;
    bool stResolved = !cfg.evalStopTgt;
    // Bar where the stop/target evaluation resolved. Defaults to entryBar
    // so Evaluate Stop/Target = No (st_outcome empty) reports 0.00 minutes,
    // same zero-length-case convention as mfeBar/maeBar above. Set to the
    // resolving bar's index at the MOMENT of resolution (WIN/LOSS below),
    // or to horizonEnd for TIMEOUT — never reconstructed afterwards.
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

            // AMBIGUITY RULE: if both stop and target are inside the SAME
            // bar's range, the STOP wins — the pessimistic assumption is
            // the only one that will not flatter the result.
            if (cfg.evalStopTgt && !stResolved)
            {
                const bool hitTarget = (side > 0) ? (hi >= entryPrice + cfg.targetTicks * cfg.tickSize)
                                                   : (lo <= entryPrice - cfg.targetTicks * cfg.tickSize);
                const bool hitStop   = (side > 0) ? (lo <= entryPrice - cfg.stopTicks * cfg.tickSize)
                                                   : (hi >= entryPrice + cfg.stopTicks * cfg.tickSize);
                if (hitStop)
                {
                    stOutcome  = TL_LOSS;
                    stPnlTicks = -cfg.stopTicks;
                    stResolved = true;
                    stExitBar  = k;
                }
                else if (hitTarget)
                {
                    stOutcome  = TL_WIN;
                    stPnlTicks = cfg.targetTicks;
                    stResolved = true;
                    stExitBar  = k;
                }
            }
        }

        if (cfg.evalStopTgt && !stResolved && complete)
        {
            // Neither level touched by horizonEnd -> TIMEOUT, exit at
            // Close[horizonEnd], actual P&L. Only a real outcome once the
            // trade is complete (mirrors the exclusion rule for every
            // other statistic). TIMEOUT means the full window elapsed, so
            // its resolving bar IS horizonEnd.
            const double closeAt = sc.Close[horizonEnd];
            stOutcome  = TL_TIMEOUT;
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

    // Elapsed minutes, entry bar -> the bar where MFE/MAE was set. mfeBar/
    // maeBar default to entryBar (see above), so a trade with zero
    // excursion gets 0.00 minutes here too — correct, not a missing value.
    // operator-(SCDateTime,SCDateTime) (scdatetime.h:2544) yields a
    // duration; GetFloatMinutesSinceBaseDate() (scdatetime.h:1968) reads
    // that duration's raw internal microseconds straight as minutes —
    // NOT GetAsDouble(), which round-trips through SCUNIXTimeToSCDateTime
    // and is for absolute timestamps, not durations.
    const double minutesToMfe = (sc.BaseDateTimeIn[mfeBar] - sc.BaseDateTimeIn[entryBar]).GetFloatMinutesSinceBaseDate();
    const double minutesToMae = (sc.BaseDateTimeIn[maeBar] - sc.BaseDateTimeIn[entryBar]).GetFloatMinutesSinceBaseDate();

    // Same derivation as minutes_to_mfe/mae: resolving bar index captured
    // at the moment of resolution above (stExitBar), diffed against the
    // entry bar via the same operator-/GetFloatMinutesSinceBaseDate()
    // pair — never GetAsDouble(), which is for absolute timestamps, not
    // durations. Evaluate Stop/Target=No leaves stExitBar at its entryBar
    // default -> 0.00, the same zero-length-case convention as the other
    // minutes columns.
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
// BARRIER SWEEP
//
// Re-evaluates ONE trade against many (stop, target) pairs in a single walk
// of its window, and appends one narrow CSV row per pair.
//
// WHY THIS EXISTS: a single export carries exactly one honest barrier point
// (st_outcome / st_pnl_ticks at the configured stop and target). Everything
// else has to be reconstructed downstream from MFE/MAE plus bars_to_mfe /
// bars_to_mae — and that reconstruction was measured against this study's
// own path walk on 2026-07-28 and found to be wrong by -45% on the long side
// and +73% on the short side at 40/40. It gets the win RATE approximately
// right and the expectancy badly wrong, because bars_to_mfe is the bar of
// the EXTREME, not the bar of FIRST TOUCH. A trade that clips the stop
// early, recovers, then runs to target is a LOSS here and a WIN there.
//
// So the grid is computed where the path actually is.
//
// METHOD: first-touch bars, not per-pair re-walks. One pass over the window
// records, for each stop level, the first bar whose adverse excursion
// reached it, and likewise for each target level. Both level lists are
// ascending, so a single advancing index per list is enough — a level lower
// than the current pointer was necessarily touched on an earlier bar. Each
// (stop, target) pair is then resolved from two integers, so the cost is
// O(window + pairs) per trade rather than O(window * pairs).
//
// The resolution rule is IDENTICAL to the single-barrier evaluator above,
// including its pessimism: `firstAdv <= firstFav` gives the stop priority,
// so a bar containing both levels is a LOSS. Any divergence between the two
// would make the sweep incomparable with the main CSV's st_* columns, which
// is the one thing that must not happen — the 40/40 pair is the join that
// proves the sweep agrees with the study.
// -----------------------------------------------------------------------
// Returns the number of rows written, so the panel's row/size readout is the
// real figure and not trades x pairs — trades with no window emit nothing.
static long TLSweepTradeBarriers(SCStudyInterfaceRef sc, const TLRunConfig& cfg,
                                 const TLTrade& t, int tradeId,
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

    // -1 = never touched inside the window.
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

    // TIMEOUT exit price. Mirrors the single-barrier evaluator: the exit is
    // Close[horizonEnd] and the P&L is real, not zero.
    const double closeAt      = sc.Close[t.horizonEndBar];
    const double timeoutTicks = side * (closeAt - entryPrice) / cfg.tickSize;

    const char* sideStr = t.isBaseline ? ((side > 0) ? "LONG_BASE" : "SHORT_BASE")
                                       : ((side > 0) ? "LONG" : "SHORT");

    for (int i = 0; i < nS; ++i)
    {
        for (int j = 0; j < nT; ++j)
        {
            // R:R floor. Applied here rather than when the lists are built
            // so both axes stay independently configurable.
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

// Builds an ascending level list from min/max/step, clamped to a sane count.
static std::vector<int> TLBuildLevels(int lo, int hi, int step)
{
    std::vector<int> v;
    if (step <= 0 || hi < lo)
        return v;
    for (int x = lo; x <= hi && (int)v.size() < 512; x += step)
        v.push_back(x);
    return v;
}

// =============================================================================
SCSFExport scsf_TriggerLab(SCStudyInterfaceRef sc)
{
    // -------------------------------------------------------------------------
    // SUBGRAPHS  (6 — do not add more; SG3-SG6 must stay DRAWSTYLE_IGNORE so
    // they can never wreck the price scale)
    // -------------------------------------------------------------------------
    SCSubgraphRef sg_LongEntry  = sc.Subgraph[0];   // SG1
    SCSubgraphRef sg_ShortEntry = sc.Subgraph[1];   // SG2
    SCSubgraphRef sg_MFE        = sc.Subgraph[2];   // SG3
    SCSubgraphRef sg_MAE        = sc.Subgraph[3];   // SG4
    SCSubgraphRef sg_HorizonPnl = sc.Subgraph[4];   // SG5
    SCSubgraphRef sg_DbgConn    = sc.Subgraph[5];   // SG6

    // -------------------------------------------------------------------------
    // INPUTS  (0-24 per build spec section 9; 25-26 added for Baseline Mode —
    // approved addition, spec update owned by the coordinator, not this file)
    // -------------------------------------------------------------------------
    SCInputRef in_LongEnable      = sc.Input[0];
    SCInputRef in_LongChartNum    = sc.Input[1];
    SCInputRef in_LongStudySub    = sc.Input[2];
    SCInputRef in_ShortEnable     = sc.Input[3];
    SCInputRef in_ShortChartNum   = sc.Input[4];
    SCInputRef in_ShortStudySub   = sc.Input[5];
    SCInputRef in_FireTest        = sc.Input[6];
    SCInputRef in_FireThreshold   = sc.Input[7];
    SCInputRef in_EntryDelay      = sc.Input[8];
    SCInputRef in_EntryPrice      = sc.Input[9];
    SCInputRef in_Overlap         = sc.Input[10];
    SCInputRef in_SessionStart    = sc.Input[11];
    SCInputRef in_SessionEnd      = sc.Input[12];
    SCInputRef in_FlattenAtEnd    = sc.Input[13];
    SCInputRef in_HorizonMode     = sc.Input[14];
    SCInputRef in_HorizonBars     = sc.Input[15];
    SCInputRef in_HorizonMinutes  = sc.Input[16];
    SCInputRef in_MfeHitTarget    = sc.Input[17];
    SCInputRef in_EvalStopTarget  = sc.Input[18];
    SCInputRef in_Target          = sc.Input[19];
    SCInputRef in_Stop            = sc.Input[20];
    SCInputRef in_ShowPanel       = sc.Input[21];
    SCInputRef in_PanelTextSize   = sc.Input[22];
    SCInputRef in_CSVExport       = sc.Input[23];
    SCInputRef in_CSVPath         = sc.Input[24];
    SCInputRef in_BaselineMode    = sc.Input[25];
    SCInputRef in_BaselinePerDay  = sc.Input[26];
    SCInputRef in_DelaySweep      = sc.Input[27];
    SCInputRef in_DelaySweepMax   = sc.Input[28];
    SCInputRef in_BarrierSweep    = sc.Input[29];
    SCInputRef in_BSStopMin       = sc.Input[30];
    SCInputRef in_BSStopMax       = sc.Input[31];
    SCInputRef in_BSStopStep      = sc.Input[32];
    SCInputRef in_BSTargetMin     = sc.Input[33];
    SCInputRef in_BSTargetMax     = sc.Input[34];
    SCInputRef in_BSTargetStep    = sc.Input[35];
    SCInputRef in_BSMinRR         = sc.Input[36];
    SCInputRef in_BSSweepBaseline = sc.Input[37];
    SCInputRef in_BSDelayMin      = sc.Input[38];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        // Freshness canary — if the panel still says "Trigger Lab" without the
        // version, an old DLL is loaded and the barrier sweep inputs are not
        // really there. Bump on every build that changes the CSV schema.
        sc.GraphName        = "Trigger Lab v1.2";
        sc.StudyDescription =
            "Measures one trigger's MFE/MAE under a delayed, session-gated "
            "entry, over the full chart history. p75 of MAE is the stop "
            "candidate; median MFE is the target candidate. Not a strategy "
            "engine — no trading calls of any kind. AutoLoop=0, single full "
            "pass, rebuild only on full recalculation or bar-count change.";
        sc.AutoLoop    = 0;
        sc.GraphRegion = 0;
        sc.FreeDLL     = 0;
        sc.DrawZeros   = 0;

        // Run AFTER the source studies on this chart when Chart Number = 0
        // (same-chart reads). Same-chart sources must sit ABOVE Trigger Lab
        // in the study list.
        sc.CalculationPrecedence = LOW_PREC_LEVEL;

        sg_LongEntry.Name         = "Long Entry";
        sg_LongEntry.DrawStyle    = DRAWSTYLE_ARROW_UP;
        sg_LongEntry.LineWidth    = 5;
        sg_LongEntry.PrimaryColor = RGB(0, 220, 80);
        sg_LongEntry.DrawZeros    = 0;

        sg_ShortEntry.Name         = "Short Entry";
        sg_ShortEntry.DrawStyle    = DRAWSTYLE_ARROW_DOWN;
        sg_ShortEntry.LineWidth    = 5;
        sg_ShortEntry.PrimaryColor = RGB(220, 60, 60);
        sg_ShortEntry.DrawZeros    = 0;

        sg_MFE.Name         = "MFE (ticks)";
        sg_MFE.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_MFE.DrawZeros    = 0;

        sg_MAE.Name         = "MAE (ticks)";
        sg_MAE.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_MAE.DrawZeros    = 0;

        sg_HorizonPnl.Name         = "Horizon P&L (ticks)";
        sg_HorizonPnl.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_HorizonPnl.DrawZeros    = 0;

        sg_DbgConn.Name         = "Debug: Sources Connected";
        sg_DbgConn.DrawStyle    = DRAWSTYLE_IGNORE;
        sg_DbgConn.DrawZeros    = 1;

        in_LongEnable.Name = "Long Enable";
        in_LongEnable.SetYesNo(1);

        in_LongChartNum.Name = "Long Source Chart Number  (0 = this chart)";
        in_LongChartNum.SetInt(0);
        in_LongChartNum.SetIntLimits(0, 500);

        in_LongStudySub.Name = "Long Source Study+Subgraph";
        in_LongStudySub.SetStudySubgraphValues(0, 0);

        in_ShortEnable.Name = "Short Enable";
        in_ShortEnable.SetYesNo(1);

        in_ShortChartNum.Name = "Short Source Chart Number  (0 = this chart)";
        in_ShortChartNum.SetInt(0);
        in_ShortChartNum.SetIntLimits(0, 500);

        in_ShortStudySub.Name = "Short Source Study+Subgraph";
        in_ShortStudySub.SetStudySubgraphValues(0, 0);

        in_FireTest.Name = "Fire Test";
        in_FireTest.SetCustomInputStrings(
            "Non-Zero Rising Edge;Cross Above Threshold;Cross Below Threshold");
        in_FireTest.SetCustomInputIndex(0);

        in_FireThreshold.Name = "Fire Threshold  (used by Cross Above/Below)";
        in_FireThreshold.SetFloat(0.0f);

        in_EntryDelay.Name = "Entry Delay (bars)";
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
        in_CSVPath.SetString("C:\\SierraChart\\Data\\TriggerLab_{chart}_{bars}_{delay}.csv");

        in_BaselineMode.Name = "Baseline Mode  (adds random control-group entries to the CSV)";
        in_BaselineMode.SetYesNo(0);

        in_BaselinePerDay.Name = "Baseline Entries Per Day";
        in_BaselinePerDay.SetInt(5);
        in_BaselinePerDay.SetIntLimits(1, 200);

        in_DelaySweep.Name = "Delay Sweep  (emit a trade at every delay 0..Max; ignores Entry Delay)";
        in_DelaySweep.SetYesNo(0);

        in_DelaySweepMax.Name = "Delay Sweep Max";
        in_DelaySweepMax.SetInt(3);
        in_DelaySweepMax.SetIntLimits(0, 20);

        // ---- Barrier Sweep ----
        // Writes a SECOND, narrow CSV alongside the main one. The main export
        // is untouched, so the existing analysis pipeline keeps working; the
        // sweep file joins to it on (run_chart, run_bars, run_delay, trade_id).
        // Narrow because 30 of the main file's 33 columns do not change when a
        // barrier moves — re-emitting the wide row per pair costs ~5x the disk
        // for no information.
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

        // 1.0 keeps only pairs at or above 1:1. Cuts the grid ~45% and drops
        // the region where tight-stop payoff arithmetic flatters every entry,
        // random ones included.
        in_BSMinRR.Name = "  Barrier Sweep: Min Reward:Risk (target >= stop x this)";
        in_BSMinRR.SetFloat(1.0f);
        in_BSMinRR.SetFloatLimits(0.1f, 100.0f);

        // The baseline is ~70% of the sweep file's rows and is bar-type
        // independent in principle (a random in-session entry has no relation
        // to how the chart draws bars). Sweep it on the FIRST timeframe, then
        // turn this off for the rest and reuse that control group.
        in_BSSweepBaseline.Name = "  Barrier Sweep: Include Baseline Rows";
        in_BSSweepBaseline.SetYesNo(1);

        // Barrier sweep x delay sweep multiplies row count. Delays below this
        // are still emitted to the MAIN csv, just not swept.
        in_BSDelayMin.Name = "  Barrier Sweep: Min Delay To Sweep";
        in_BSDelayMin.SetInt(1);
        in_BSDelayMin.SetIntLimits(0, 20);

        return;
    }

    // =========================================================================
    // LAST CALL — free the persistent trade vector
    // =========================================================================
    if (sc.LastCallToFunction)
    {
        std::vector<TLTrade>* p = (std::vector<TLTrade>*)sc.GetPersistentPointer(1);
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
    const int lastClosedBar = totalBars - 2;   // current forming bar excluded
    if (lastClosedBar < 0)
        return;

    // =========================================================================
    // CALCULATION MODEL  (spec section 8)
    //
    // Rebuild the ENTIRE trade list when either a full recalculation was
    // requested or the bar count changed since the last pass. Otherwise
    // return immediately — this is a research tool, correctness beats
    // incrementality, and a per-tick rebuild of up to 20k trades would stall
    // the chart.
    // =========================================================================
    int& lastKnownBars = sc.GetPersistentInt(1);
    const bool isFullRecalc = (sc.UpdateStartIndex == 0) || sc.IsFullRecalculation;
    const bool barsChanged  = (totalBars != lastKnownBars);

    if (!isFullRecalc && !barsChanged)
        return;

    lastKnownBars = totalBars;

    // -------------------------------------------------------------------------
    // READ SETTINGS
    // -------------------------------------------------------------------------
    const bool  longEnabled   = in_LongEnable.GetYesNo() != 0;
    const bool  shortEnabled  = in_ShortEnable.GetYesNo() != 0;

    const int   fireMode      = in_FireTest.GetIndex();   // 0 rising edge, 1 cross above, 2 cross below
    const float fireThresh    = in_FireThreshold.GetFloat();

    const int   entryDelay    = in_EntryDelay.GetInt();
    const int   entryPriceMode= in_EntryPrice.GetIndex(); // 0 Close, 1 Open
    const int   overlapMode   = in_Overlap.GetIndex();    // 0 skip while in trade, 1 allow all

    const int   sessStart     = in_SessionStart.GetTime();
    const int   sessEnd       = in_SessionEnd.GetTime();
    const bool  flattenAtEnd  = in_FlattenAtEnd.GetYesNo() != 0;

    const int   horizonMode   = in_HorizonMode.GetIndex(); // 0 Bars, 1 Minutes
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

    // Entry Delay = 0 + Entry Price = Open is lookahead (Open[knowableBar]
    // precedes the source bar's own close). Fall back to Close and report
    // it — computed once here since it depends only on config, not on any
    // individual trade.
    const bool entryPriceFallback = (entryPriceMode == 1 && entryDelay == 0);
    const bool useOpenEntry = (entryPriceMode == 1) && !entryPriceFallback;

    const bool baselineMode    = in_BaselineMode.GetYesNo() != 0;
    const int  baselinePerDay  = in_BaselinePerDay.GetInt();

    // Delay Sweep: while ON, every fired signal emits a trade at EVERY
    // delay 0..sweepMax inclusive instead of one trade at Input 8 (Entry
    // Delay), which is ignored while sweeping. dStart/dEnd below collapse
    // to a single iteration (entryDelay..entryDelay) when sweep is OFF, so
    // the non-sweep code path is untouched byte-for-byte.
    const bool sweepMode = in_DelaySweep.GetYesNo() != 0;
    int sweepMax = in_DelaySweepMax.GetInt();
    if (sweepMax < 0) sweepMax = 0;
    if (sweepMax > TL_MAX_DELAY) sweepMax = TL_MAX_DELAY;
    const int dStart = sweepMode ? 0 : entryDelay;
    const int dEnd   = sweepMode ? sweepMax : entryDelay;

    // Bundle passed to TLBuildTradeAtEntry() — shared verbatim by real
    // trigger signals and, further below, Baseline Mode's pseudo-entries.
    TLRunConfig runCfg;
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
    // ZERO SUBGRAPHS ACROSS THE FULL RANGE  (full rebuild every pass)
    // =========================================================================
    for (int i = 0; i <= lastBar; ++i)
    {
        sg_LongEntry[i]            = 0.0f;
        sg_ShortEntry[i]           = 0.0f;
        sg_MFE[i]                  = 0.0f;
        sg_MAE[i]                  = 0.0f;
        sg_HorizonPnl[i]           = 0.0f;
        sg_DbgConn[i]              = 0.0f;
    }

    std::vector<TLTrade>* tradesPtr = TLGetTrades(sc);
    std::vector<TLTrade>& trades = *tradesPtr;
    trades.clear();

    bool limitHit = false;
    int  sourcesConnected = 0;

    // =========================================================================
    // PER-SIDE PROCESSING  (Long slot, then Short slot — identical logic,
    // only the sign and the subgraph differ). Both slots share every input
    // except Enable / Chart Number / Study+Subgraph, per spec: "Do not add
    // per-side delay/horizon inputs."
    // =========================================================================
    for (int sideIdx = 0; sideIdx < 2; ++sideIdx)
    {
        const bool enabled = (sideIdx == 0) ? longEnabled : shortEnabled;
        if (!enabled)
            continue;

        const int side = (sideIdx == 0) ? 1 : -1;

        // Plain if/else rather than a ternary over SCInputRef — SCInputRef
        // is itself a reference typedef in ACSIL, and binding a fresh
        // reference-to-reference via `?:` is an unnecessary risk with no
        // compiler on hand to confirm it collapses cleanly.
        int chartNum;
        int studyID;
        int sgIndex;
        if (sideIdx == 0)
        {
            chartNum = in_LongChartNum.GetInt();
            studyID  = in_LongStudySub.GetStudyID();
            sgIndex  = in_LongStudySub.GetSubgraphIndex();
        }
        else
        {
            chartNum = in_ShortChartNum.GetInt();
            studyID  = in_ShortStudySub.GetStudyID();
            sgIndex  = in_ShortStudySub.GetSubgraphIndex();
        }
        if (chartNum <= 0)
            chartNum = sc.ChartNumber;      // 0 = "this chart"
        if (studyID == 0)
            continue;   // slot not configured

        // ---- DATA LAYER  (copied pattern: OrderflowSignalV5.cpp:640-760) ----
        // Fetch the trigger array DIRECTLY from its source chart, so it is
        // indexed by the SOURCE chart's own bars — nothing collapsed yet.
        // For chartNum == sc.ChartNumber this is the identity-map case; same
        // code path, not a special case.
        SCFloatArray arr;
        sc.GetStudyArrayFromChartUsingID(chartNum, studyID, sgIndex, arr);

        SCDateTimeArray srcDT;
        sc.GetChartDateTimeArray(chartNum, srcDT);
        const int nSrc = srcDT.GetArraySize();
        if (nSrc < 2 || arr.GetArraySize() < 2)
            continue;   // source chart closed/loading — contributes nothing this pass

        ++sourcesConnected;

        // ----- merge walk: destOfSrc[s] = main bar containing source bar s ---
        // Monotonic cursor, never rewinds -> O(nSrc + nDest). -1 when the
        // source bar predates main history. Identical for chartNum ==
        // sc.ChartNumber (identity map).
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

        // -------------------------------------------------------------------
        // FIRE TEST  — evaluated in SOURCE time, on the source chart's own
        // bars, never on main-chart samples. Point sampling would drop
        // events when the source chart is faster than the main chart.
        //
        // Only CLOSED source bars are evaluated: the forming source bar
        // (s == nSrc-1) can still repaint, so the last countable source bar
        // is nSrc-2. Its knowable bar (destOfSrc[s+1]) still resolves fine
        // because s+1 only needs to have OPENED, not closed.
        // -------------------------------------------------------------------
        const int alen = arr.GetArraySize();
        int top = nSrc - 2;
        if (top > alen - 1) top = alen - 1;

        const int overlapWindow = (overlapMode == 0) ? 1 : 0; // 0=skip-while-in-trade

        // ONE WATERMARK PER DELAY, genuinely separate storage (a plain
        // fixed-size array, one slot per possible delay 0..TL_MAX_DELAY).
        // With Skip While In Trade, which signals get accepted depends on
        // entryBar, which depends on delay — a single shared watermark
        // across the sweep would let delay 0's accepted trades suppress
        // delay 3's and vice versa, contaminating the very curve the sweep
        // exists to measure cleanly. Non-sweep mode only ever touches
        // index [entryDelay], so this is exactly the old scalar's role,
        // just addressed by delay instead of being the only delay there is.
        int lastAcceptedHorizonEnd[TL_MAX_DELAY + 1];
        for (int di = 0; di <= TL_MAX_DELAY; ++di)
            lastAcceptedHorizonEnd[di] = -1;   // -1 = no open trade yet, this side only

        for (int s = 1; s <= top; ++s)
        {
            bool fires = false;
            switch (fireMode)
            {
                case 0: // Non-Zero Rising Edge
                    fires = (arr[s] != 0.0f && arr[s - 1] == 0.0f);
                    break;
                case 1: // Cross Above Threshold
                    fires = (arr[s] > fireThresh && arr[s - 1] <= fireThresh);
                    break;
                case 2: // Cross Below Threshold
                    fires = (arr[s] < fireThresh && arr[s - 1] >= fireThresh);
                    break;
            }
            if (!fires)
                continue;

            const int spanStart = destOfSrc[s];
            if (spanStart < 0)
                continue;   // predates main history

            // Where the event becomes KNOWABLE = source bar s's close =
            // the open of source bar s+1. NEVER the source bar's own open —
            // that would be lookahead.
            int knowableBar = (s + 1 < nSrc) ? destOfSrc[s + 1] : -1;
            if (knowableBar < 0)
                continue;   // next source bar predates main history (shouldn't happen for s>=1)
            if (knowableBar > lastBar)
                knowableBar = lastBar;

            // ---- DELAY SWEEP  (dStart==dEnd==entryDelay when Delay Sweep
            // is OFF, so this loop runs exactly once with the same delay
            // as before — non-sweep output is unaffected). Same signal,
            // same knowable bar, one entry bar per delay in the sweep. ----
            for (int d = dStart; d <= dEnd; ++d)
            {
                const int entryBar = knowableBar + d;
                if (entryBar > lastClosedBar)
                    continue;   // signal pending at this delay — no trade yet

                // ---- session gate applied to the ENTRY bar, not the signal ----
                const int entryTOD = sc.BaseDateTimeIn[entryBar].GetTime();
                if (!TLInSession(entryTOD, sessStart, sessEnd))
                    continue;

                // ---- overlap policy — PER-DELAY watermark ----
                if (overlapWindow && entryBar <= lastAcceptedHorizonEnd[d])
                    continue;   // discarded: THIS DELAY already has an open trade

                // ---- build the trade: horizon / session-truncation / completeness /
                // MFE-MAE / stop-target, IDENTICAL code Baseline Mode uses below ----
                TLTrade t = TLBuildTradeAtEntry(sc, runCfg, side, entryBar, d, useOpenEntry,
                                                chartNum, studyID, sgIndex,
                                                sc.BaseDateTimeIn[spanStart], sc.BaseDateTimeIn[knowableBar]);
                t.runDelay = d;   // per-row in sweep mode; equals entryDelay when not sweeping

                // Watermark uses the SAME horizonEnd TLBuildTradeAtEntry() just
                // computed (entryBar + horizonBarsActual) — derived from the
                // trade's own result rather than recomputed, so there is no
                // second horizon calculation to drift out of sync with the
                // first.
                if (overlapWindow)
                    lastAcceptedHorizonEnd[d] = entryBar + t.horizonBarsActual;

                // ---- write subgraphs on the ENTRY bar only, and ONLY for the
                // base-configured delay (Input 8) — real trades only, and the
                // chart/panel stay anchored to one delay regardless of sweep,
                // per spec: everything but the CSV schema is untouched. Extra
                // sweep delays are CSV-only, same principle as Baseline Mode. ----
                if (d == entryDelay)
                {
                    if (side > 0)
                        sg_LongEntry[entryBar] = (float)(sc.Low[entryBar]);
                    else
                        sg_ShortEntry[entryBar] = (float)(sc.High[entryBar]);
                    sg_MFE[entryBar]        = (float)t.mfeTicks;
                    sg_MAE[entryBar]        = (float)t.maeTicks;
                    sg_HorizonPnl[entryBar] = (float)t.horizonPnlTicks;
                }

                // ---- record the trade ----
                if ((int)trades.size() >= TL_MAX_TRADES)
                    limitHit = true;
                else
                    trades.push_back(t);
            }
        }
    }

    // SG6: count of enabled slots that resolved to a live source array.
    sg_DbgConn[lastBar] = (float)sourcesConnected;

    // =========================================================================
    // BASELINE MODE — pseudo-random control-group entries.
    //
    // Same purpose as the trigger, same bars, same TLBuildTradeAtEntry() code
    // path (horizon / session-truncation / completeness / MFE-MAE / stop-
    // target) — NOT a parallel implementation, so a trigger's numbers and the
    // baseline's numbers are directly comparable; whatever the trigger's
    // horizon/stop/target settings measure, the baseline measures the same
    // way, on the same bars.
    //
    // NO DELAY: a random entry has no signal to be late to, so delayBars=0
    // and entryBar IS the picked bar directly (no knowable-bar machinery).
    // Also no Entry-Price-Open lookahead fallback: that guard exists because
    // Open[knowableBar] precedes the signal's own close; a baseline pick has
    // no such precedence relationship to anything, so Open[entryBar] here is
    // just that bar's own already-printed open. See TLBuildTradeAtEntry()'s
    // `useOpen` comment.
    //
    // OVERLAP POLICY DOES NOT APPLY: baseline entries are independent
    // samples; suppressing one because a previous baseline trade is still
    // "open" would bias the sample toward quiet periods. All K per day are
    // generated regardless of overlap with each other or with real trades.
    //
    // DETERMINISM: seeded from sc.ChartNumber + the trading day's date
    // serial (GetDate() — the SAME raw serial value whose YYYYMMDD
    // MISINTERPRETATION was the prior date-decomposition bug; here it is
    // used correctly, as an opaque per-day integer, never decoded). Same
    // chart + same day => same seed => same draws, every recalculation,
    // forever. No seed input is exposed — determinism is not optional.
    // =========================================================================
    if (baselineMode && baselinePerDay > 0)
    {
        for (int sideIdx = 0; sideIdx < 2; ++sideIdx)
        {
            const bool enabled = (sideIdx == 0) ? longEnabled : shortEnabled;
            if (!enabled)
                continue;

            const int side = (sideIdx == 0) ? 1 : -1;
            // Independent draw per side: distinct XOR constant so long and
            // short pick different bars, never the same bars mirrored.
            const uint64_t sideSalt = (side > 0) ? 0x9E3779B97F4A7C15ULL : 0xC2B2AE3D27D4EB4FULL;

            // Baseline must fill by the SAME convention as real trades in
            // every configuration — a control group that differs from the
            // treatment group in a second respect (not just "how the entry
            // bar was chosen") measures neither. Skipping the Entry-Delay-0
            // fallback here was tempting because a random entry has no
            // knowable-bar and so no lookahead risk from using Open — but
            // lookahead is not the constraint that governs a control
            // group's price convention; comparability is. At Delay 0 +
            // Entry Price = Open, real trades take the Close fallback; if
            // baseline used raw Open instead, treatment and control would
            // fill on different sides of the same bar — exactly where it
            // hurts most, since the delay-decay curve (0/1/2/3) is this
            // study's primary output, and the artifact would show up as a
            // spurious jump at delay 0 that looks like signal decay.
            const bool useOpenBaseline = useOpenEntry;

            int dayStart = 0;
            while (dayStart <= lastClosedBar)
            {
                const int dayKey = sc.BaseDateTimeIn[dayStart].GetDate();   // opaque serial day id
                int dayEnd = dayStart;
                while (dayEnd + 1 <= lastClosedBar && sc.BaseDateTimeIn[dayEnd + 1].GetDate() == dayKey)
                    ++dayEnd;

                // In-session, already-CLOSED bars this day. Baseline entries
                // are restricted to closed bars, same as real trades — an
                // entryBar's Close/Open must be a final, already-printed
                // price.
                std::vector<int> pool;
                for (int i = dayStart; i <= dayEnd; ++i)
                    if (TLInSession(sc.BaseDateTimeIn[i].GetTime(), sessStart, sessEnd))
                        pool.push_back(i);

                const int kToPick = (baselinePerDay < (int)pool.size()) ? baselinePerDay : (int)pool.size();

                if (kToPick > 0)
                {
                    // xorshift64* — small, deterministic, inline. Seed
                    // combines chart number, day key and side so distinct
                    // (chart, day, side) triples never collide.
                    uint64_t rngState = ((uint64_t)(uint32_t)sc.ChartNumber << 32) ^
                                        (uint64_t)(uint32_t)dayKey ^ sideSalt;
                    if (rngState == 0)
                        rngState = 0x2545F4914F6CDD1DULL;   // xorshift needs a non-zero state

                    auto nextRand = [&rngState]() -> uint64_t
                    {
                        rngState ^= rngState >> 12;
                        rngState ^= rngState << 25;
                        rngState ^= rngState >> 27;
                        return rngState * 2685821657736338717ULL;
                    };

                    // Partial Fisher-Yates: draws kToPick bars WITHOUT
                    // replacement, deterministically, from the in-session
                    // pool for this day.
                    std::vector<int> pickPool = pool;
                    const int n = (int)pickPool.size();
                    for (int pick = 0; pick < kToPick; ++pick)
                    {
                        const int remaining = n - pick;
                        const int j = pick + (int)(nextRand() % (uint64_t)remaining);
                        std::swap(pickPool[pick], pickPool[j]);

                        const int entryBar = pickPool[pick];

                        TLTrade t = TLBuildTradeAtEntry(sc, runCfg, side, entryBar, /*delayBars=*/0,
                                                        useOpenBaseline,
                                                        sc.ChartNumber, /*sourceStudyID=*/0, /*sourceSG=*/0,
                                                        sc.BaseDateTimeIn[entryBar], sc.BaseDateTimeIn[entryBar]);
                        t.isBaseline = true;
                        // Baseline is emitted ONCE, never duplicated per
                        // delay (a random entry has no signal to be late
                        // to, so one control group is valid for every
                        // delay). In sweep mode, run_delay = -1 is the
                        // explicit sentinel meaning "applies to every
                        // delay cell" — the analysis pipeline broadcasts
                        // it across delay cells separately. Outside sweep
                        // mode this is UNCHANGED from before: run_delay
                        // stays the run's single Entry Delay, not -1.
                        t.runDelay = sweepMode ? -1 : entryDelay;

                        if ((int)trades.size() >= TL_MAX_TRADES)
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
    // STATISTICS  (per side + combined). Only COMPLETE trades count.
    // Baseline Mode rows are excluded from every statistic here — the panel
    // is a live sanity check on the TRIGGER; silently folding the control
    // group into its numbers would change what those numbers mean with
    // nothing on screen saying so. Baseline exists for the CSV only.
    // =========================================================================
    struct TLStats
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

    TLStats longStats, shortStats, combined;
    const double mfeHitTarget = (double)in_MfeHitTarget.GetInt();

    for (size_t i = 0; i < trades.size(); ++i)
    {
        TLTrade& t = trades[i];
        if (t.isBaseline)
            continue;   // excluded from every panel statistic, per spec
        if (t.runDelay != entryDelay)
            continue;   // Delay Sweep: panel stays anchored to the ONE
                         // base-configured delay (Input 8), same principle
                         // as excluding baseline — the panel is a live
                         // sanity check on one configuration, not a blend
                         // across delays. Non-sweep mode: every real
                         // trade's runDelay == entryDelay always, so this
                         // is a no-op there.

        TLStats& s = (t.side > 0) ? longStats : shortStats;

        if (!t.complete)
        {
            s.nIncomplete++;
            combined.nIncomplete++;
            continue;
        }

        s.nComplete++;
        combined.nComplete++;
        s.mfe.push_back(t.mfeTicks);
        s.mae.push_back(t.maeTicks);
        combined.mfe.push_back(t.mfeTicks);
        combined.mae.push_back(t.maeTicks);

        if (t.mfeTicks >= mfeHitTarget) { s.nHitTarget++; combined.nHitTarget++; }

        s.horizonPnlSum += t.horizonPnlTicks;
        combined.horizonPnlSum += t.horizonPnlTicks;

        if (evalStopTgt && t.stOutcome != TL_NONE)
        {
            s.nSTResolved++; combined.nSTResolved++;
            s.expectancySum += t.stPnlTicks;
            combined.expectancySum += t.stPnlTicks;
            s.totalTicks += t.stPnlTicks;
            combined.totalTicks += t.stPnlTicks;

            if (t.stOutcome == TL_WIN)       { s.nWin++;     combined.nWin++; }
            else if (t.stOutcome == TL_LOSS) { s.nLoss++;    combined.nLoss++; }
            else                             { s.nTimeout++; combined.nTimeout++; }

            if (t.stPnlTicks > 0) { s.grossWin  += t.stPnlTicks; combined.grossWin  += t.stPnlTicks; }
            else                  { s.grossLoss += -t.stPnlTicks; combined.grossLoss += -t.stPnlTicks; }
        }
    }

    // =========================================================================
    // CSV EXPORT  (whole file rewritten every rebuild, never appended, so a
    // recalculation cannot duplicate rows. Header row always written.)
    //
    // Computed BEFORE the panel so a write failure can be surfaced there —
    // silent export failure is the worst outcome for a study whose entire
    // purpose is producing a dataset.
    //
    // {chart}/{bars}/{delay} tokens in the CSV Path are expanded here so
    // each bar-type/timeframe/delay configuration writes its OWN file;
    // whole-file rewrite stays (appending would duplicate every row on
    // every recalculation), but distinct configurations no longer clobber
    // each other, so multiple runs can be pooled afterward in Excel/pandas.
    //
    // run_chart / run_bars / run_horizon_mode / run_horizon / run_fire_test /
    // run_entry_price / run_eval_st / run_target / run_stop are constant for
    // the whole file (one run = one configuration) and are written on every
    // row so a pooled, concatenated CSV is self-describing without needing the
    // filename.
    //
    // run_eval_st / run_target / run_stop exist because st_outcome and
    // st_pnl_ticks are meaningless without the barriers that produced them.
    // Before these columns a barrier sweep wrote files that were byte-level
    // indistinguishable in configuration — the barrier size could only be
    // recovered by inspecting the WIN/LOSS values of st_pnl_ticks, which
    // works only while every run uses a symmetric, always-hit barrier.
    // run_target / run_stop carry the configured input values even when
    // Evaluate Stop/Target = No; run_eval_st is what says whether they were
    // applied. Do NOT overload the size columns with a sentinel.
    //
    // run_delay is the ONE exception: outside Delay Sweep it is also a
    // run-level constant (== Entry Delay, written identically to every
    // row — unchanged from before Delay Sweep existed). Under Delay Sweep
    // it becomes PER-ROW (t.runDelay), because the analysis pipeline's
    // cell key is (side, run_bars, run_delay) — making the column per-row
    // is what lets the existing pipeline group the sweep correctly with no
    // changes on that side. Baseline rows carry run_delay = entryDelay
    // outside sweep mode (unchanged) or the -1 sentinel under sweep mode
    // (see the Baseline Mode block above).
    // =========================================================================
    bool csvWriteFailed = false;
    const SCString barsToken = TLBarsToken(sc);

    if (csvExport)
    {
        const SCString expandedPath = TLExpandCSVPath(sc, csvPath, barsToken, entryDelay, sweepMode, sweepMax);

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
                "run_eval_st,run_target,run_stop,"
                "trade_id,side,source_chart,source_study_id,source_sg,"
                "signal_dt,knowable_dt,entry_dt,weekday,session_minute,"
                "entry_price,delay_bars,horizon_end_dt,horizon_bars_actual,"
                "mfe_ticks,mae_ticks,bars_to_mfe,bars_to_mae,horizon_pnl_ticks,"
                "st_outcome,st_pnl_ticks,truncated_by_session,complete,"
                "minutes_to_mfe,minutes_to_mae,minutes_to_st_exit\n");

            const char* outcomeStr[4] = { "", "WIN", "LOSS", "TIMEOUT" };

            for (size_t i = 0; i < trades.size(); ++i)
            {
                TLTrade& t = trades[i];
                const char* sideStr = t.isBaseline
                    ? ((t.side > 0) ? "LONG_BASE" : "SHORT_BASE")
                    : ((t.side > 0) ? "LONG" : "SHORT");
                fprintf(f,
                    "%d,%s,%d,%s,%d,%s,%s,"
                    "%d,%d,%d,"
                    "%d,%s,%d,%d,%d,"
                    "%s,%s,%s,%d,%d,"
                    "%.6f,%d,%s,%d,"
                    "%.2f,%.2f,%d,%d,%.2f,"
                    "%s,%.2f,%d,%d,"
                    "%.2f,%.2f,%.2f\n",
                    sc.ChartNumber, barsToken.GetChars(), t.runDelay, runHorizonMode, runHorizon,
                    runFireTest, runEntryPrice,
                    evalStopTgt ? 1 : 0, (int)targetTicks, (int)stopTicks,
                    (int)i + 1, sideStr,
                    t.sourceChart, t.sourceStudyID, t.sourceSG,
                    TLFormatDT(t.signalDT).GetChars(), TLFormatDT(t.knowableDT).GetChars(),
                    TLFormatDT(t.entryDT).GetChars(), t.weekday, t.sessionMinute,
                    t.entryPrice, t.delayBars, TLFormatDT(t.horizonEndDT).GetChars(), t.horizonBarsActual,
                    t.mfeTicks, t.maeTicks, t.barsToMfe, t.barsToMae, t.horizonPnlTicks,
                    outcomeStr[t.stOutcome], t.stPnlTicks, t.truncatedBySession ? 1 : 0, t.complete ? 1 : 0,
                    t.minutesToMfe, t.minutesToMae, t.minutesToStExit);
            }
            fclose(f);
        }
    }

    // =========================================================================
    // BARRIER SWEEP EXPORT  (second, narrow CSV — see TLSweepTradeBarriers)
    //
    // Written only when the main CSV is also being written: the sweep file is
    // useless on its own, since run_horizon, run_fire_test, entry times and
    // every other run-level fact live in the main export. It joins on
    // (run_chart, run_bars, run_delay, trade_id).
    //
    // trade_id here is the SAME (int)i + 1 the main writer uses, and both
    // loops walk `trades` in the same order, so the join key is exact.
    // =========================================================================
    bool bsWriteFailed = false;
    int  bsPairCount   = 0;
    long bsRowCount    = 0;

    if (csvExport && bsEnabled && !csvWriteFailed)
    {
        const std::vector<int> bsStops   = TLBuildLevels(in_BSStopMin.GetInt(),
                                                          in_BSStopMax.GetInt(),
                                                          in_BSStopStep.GetInt());
        const std::vector<int> bsTargets = TLBuildLevels(in_BSTargetMin.GetInt(),
                                                          in_BSTargetMax.GetInt(),
                                                          in_BSTargetStep.GetInt());

        for (size_t si = 0; si < bsStops.size(); ++si)
            for (size_t ti = 0; ti < bsTargets.size(); ++ti)
                if ((double)bsTargets[ti] >= (double)bsStops[si] * bsMinRR)
                    ++bsPairCount;

        // Same token expansion as the main path, with "_barriers" before the
        // extension so the two files sort together and neither can be mistaken
        // for the other by the collector.
        SCString bsPath = TLExpandCSVPath(sc, csvPath, barsToken, entryDelay, sweepMode, sweepMax);
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
                const TLTrade& t = trades[i];

                // Baseline rows carry run_delay = -1 under sweep mode, which
                // must NOT be filtered out by the delay floor — the sentinel
                // means "applies to every delay cell", not "delay -1".
                if (t.isBaseline)
                {
                    if (!bsBaseline)
                        continue;
                }
                else if (t.runDelay < bsDelayMin)
                {
                    continue;
                }

                bsRowCount += TLSweepTradeBarriers(sc, runCfg, t, (int)i + 1,
                                                   bsStops, bsTargets, bsMinRR,
                                                   sc.ChartNumber, barsToken, bf);
            }
            fclose(bf);
        }
    }

    // =========================================================================
    // PANEL  (reuses the fixed-corner text-drawing style from
    // OFSignalAutoTrader.cpp / AggregatorAutoTrader.cpp: s_UseTool with
    // DRAWING_TEXT, UseRelativeVerticalValues=1, AddMethod=UTAM_ADD_OR_ADJUST)
    // =========================================================================
    if (in_ShowPanel.GetYesNo())
    {
        auto fmtRow = [&](const char* label, TLStats& s) -> SCString
        {
            std::vector<double> mfeSorted = s.mfe;
            std::vector<double> maeSorted = s.mae;
            std::sort(mfeSorted.begin(), mfeSorted.end());
            std::sort(maeSorted.begin(), maeSorted.end());

            const double mfeAvg = TLAverage(s.mfe);
            const double maeAvg = TLAverage(s.mae);
            const double mfeMed = TLPercentile(mfeSorted, 0.50);
            const double maeMed = TLPercentile(maeSorted, 0.50);
            const double mfeP75 = TLPercentile(mfeSorted, 0.75);
            const double maeP75 = TLPercentile(maeSorted, 0.75);
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
        SCString limitNote = limitHit ? "  *** TRADE LIMIT (20000) REACHED ***" : "";
        SCString csvNote = csvWriteFailed ? "  *** CSV: WRITE FAILED ***" : "";

        // Baseline rows are excluded from every stat above (real trigger
        // sanity check only) — this line exists purely so it's visible on
        // screen that extra control-group rows are going to the CSV.
        SCString baselineNote;
        if (baselineMode)
            baselineNote.Format("\nBaseline: ON (%d/day)", baselinePerDay);

        // Row count is the whole risk with the barrier sweep — a mis-set grid
        // writes hundreds of MB without complaining. Put it on screen.
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

        SCString txt;
        txt.Format("Trigger Lab | Sources connected: %d%s%s%s\n%s\n%s\n%s%s",
                    sourcesConnected, fallbackNote.GetChars(), limitNote.GetChars(), csvNote.GetChars(),
                    fmtRow("Long", longStats).GetChars(),
                    fmtRow("Short", shortStats).GetChars(),
                    fmtRow("Combined", combined).GetChars(),
                    baselineNote.GetChars());

        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.LineNumber  = 84920001;
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
