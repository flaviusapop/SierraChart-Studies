// =============================================================================
// DOMAlwaysInTrader.cpp
// Sierra Chart ACSIL Custom Study — DOM Always-In Trader
//
// *** KEEP IN SYNC WITH DOMAlwaysInLab.cpp — ANY CHANGE TO ONE REQUIRES THE
// *** SAME CHANGE TO THE OTHER. DOMAlwaysInLab.cpp is the no-orders visual
// *** helper; its state machine is a behaviour-for-behaviour copy of this one.
// *** If the two diverge, the helper validates a configuration this study does
// *** not actually run. Sync list: DOMAlwaysInLab.cpp header + BuildSpec §2.
//
// An ALWAYS-IN autotrader driven by the DOM Pressure v3 study. It holds a
// long or a short essentially all session and reverses when the book turns.
// It is also the MEASUREMENT layer for the strategy: there is no separate
// no-orders lab study, so this one writes a full per-closed-bar CSV whether
// or not trading is armed.
//
// Spec: Trading/Futures_Day_Trading/DOMAlwaysIn_BuildSpec.md
//
// -----------------------------------------------------------------------------
// NON-NEGOTIABLES (each one is a bug that has a specific cause; do not "clean
// them up" without reading the spec):
//
//  1. DEPTH IS FORWARD-ONLY. DOM Pressure v3 builds its series from live market
//     depth. Its cache starts empty, repaints on a settings change and is wiped
//     by a platform restart. THERE IS NO BACKTEST — Chart Replay cannot produce
//     the signal. Everything here must be safe when the signal is absent, and
//     "absent" is the normal state after every restart.
//
//  2. v3 WRITES TO THE FORMING BAR. It runs AutoLoop = 0 / UpdateAlways = 1 and
//     writes sc.ArraySize-1, updating tick by tick. Reading the forming bar
//     makes the sign flip mid-bar and the trader churn. THIS STUDY READS ONLY
//     CLOSED BARS: sc.ArraySize-2, sc.AutoLoop = 0, persistent last-processed
//     index. When the source is on ANOTHER chart, the time-mapped source index
//     is additionally clamped to that chart's own last CLOSED bar.
//
//  3. TRIPLE ZERO = NO DATA, NOT "BALANCED BOOK". SG1, SG2 and SG3 all exactly
//     0.0 on the same bar means v3 never wrote that bar (empty book / no depth
//     updates / warmup / pre-restart history). A genuinely measured bar hitting
//     exact triple zero is effectively impossible. Never trade a no-data bar,
//     never let it reset state, and never read it as "balanced".
//
//  4. SG SEMANTICS. SG1 = smoothed net (SMA of the raw net, user's period).
//     SG2/SG3 = bid net / ask net. SG4 = RAW unsmoothed net. SG5 = normalized
//     net (SG1 divided by a strictly positive rolling denominator).
//     Flip evidence is taken from SG4 RAW, never from SG1: SG1 is a moving
//     average, so consecutive readings are not independent evidence and they
//     lag by the smoothing period.
//
// -----------------------------------------------------------------------------
// v1.2 — ALL MAGNITUDE THRESHOLDS REMOVED
//
// Entry Threshold, Flip Sum Threshold and Entry Source are GONE. Rationale:
// depth is forward-only, so there is no backtest that could ever calibrate a
// magnitude constant — both thresholds were permanent TBD-CALIBRATION guesses
// and a guessed constant in the entry path is worse than no constant, because
// it looks like a decision. The system now runs on SIGN + BAR COUNTS only,
// which is fully specified with no unresolvable parameters.
//
// Entry Source went with them as a CONSEQUENCE, not as an independent choice:
// with no threshold the entry test reduces to sign(signal) != 0, and
// sign(SG5) == sign(SG1) identically because SG5 is SG1 divided by a strictly
// positive denominator. The input would have selected between two answers that
// are always the same.
//
// The entire flip decision is now two integers: Flip Bars N and Min Hold M.
//
// -----------------------------------------------------------------------------
// STATE MACHINE (evaluated once per newly CLOSED bar i):
//
//   noData(i) = (SG1[i]==0 && SG2[i]==0 && SG3[i]==0)
//
//   if noData(i):  hold position, no counter change, blindCount++, warmup=0
//                  if holding and blindCount >= K -> Blind Policy (Flatten|Hold)
//                  -> next bar
//   else:          blindCount = 0; warmupCount++
//
//   s = sign(SG1[i])                                   // +1 / -1 / 0
//
//   if FLAT and warmupCount >= W and no stop lockout and s != 0:
//         S = s                                        // enter — that is all
//   else if IN POSITION:
//         if s ==  S : oppEvidence reset
//         if s ==  0 : transparent — no advance, no reset
//         if s == -S : accumulate opposing evidence; if the Flip Test passes on
//                      SG4 RAW and Min Hold Bars have elapsed -> S = -S
//
//   Flip Test (both modes kept; neither has a magnitude constant):
//         Consecutive : N consecutive opposing SG4 bars. One agreeing RAW bar
//                       resets the count.
//         Rolling Sum : ALL THREE of —
//                         N opposing SG4 bars since the entry / last flip
//                           (not required to be consecutive; cleared only when
//                            the SMOOTHED sign returns to our side)
//                         a full window of N VALID source bars, walking back
//                           from this bar and never past the entry / last flip
//                         sign( sum of SG4 over that window ) == -S
//                       (no-data bars are skipped, never counted as zeros)
//     Rolling Sum survives because that is what motivated it: one small
//     opposing bar cannot reset the evidence. v1.5 restores the other half of
//     that sentence — one bar cannot FLIP either (see v1.5 below).
//
//   Target T = S ; reconcile: if Position != T * Qty -> market reversal order.
//
// Reconciliation runs EVERY closed bar rather than on events, so a missed or
// rejected fill self-heals on the next bar.
//
// -----------------------------------------------------------------------------
// v1.5 — ROLLING SUM FLIP TEST REPAIRED (both studies; parity required)
//
// Symptom: with Flip Test = Rolling Sum and Flip Bars N = 3, arrows printed
// after a SINGLE opposing candle. Three independent defects, all in the Rolling
// Sum branch — Consecutive mode never had any of them:
//
//  1. `counted > 0`. The window was a fixed span of N source bars with no-data
//     bars SKIPPED, so a span containing one valid bar passed the test on that
//     one bar's sign. Depth data is patchy by nature (that is why Blind Bars
//     exists), so N silently collapsed to 1 exactly when the book was thin.
//     Now: walk back until N VALID bars are collected — which is what the
//     header always described — and require counted >= N.
//
//  2. NO ANCHOR. The window was "the last N bars", full stop, so it summed bars
//     from before the position existed. Entries are taken on SMOOTHED SG1 while
//     the flip test reads RAW SG4, and raw turns first: on the first opposing
//     bar of a fresh trade the sum was frequently already opposing, so the
//     trade flipped straight back out on evidence that predated it. Min Hold M
//     did not help — it delays the flip by BARS, it does not require new
//     evidence. Consecutive mode was immune because OppCount is zeroed on every
//     entry and flip. Now the window stops at the entry / last flip bar.
//
//  3. MAGNITUDE, NOT COUNT. The sum let one large opposing bar outweigh N-1
//     agreeing ones, so "Flip Bars N" never meant N bars in this mode. Now the
//     test also requires N opposing bars (OppRun) before the sum is consulted:
//     the sum CONFIRMS accumulated evidence, it can no longer manufacture it.
//
// OppRun is deliberately NOT OppCount. OppCount is reset by a single agreeing
// RAW bar — that is the definition of Consecutive. OppRun is reset only by the
// SMOOTHED sign returning to our side (or by a flip/entry), which keeps Rolling
// Sum's tolerance to one contrary bar while still demanding N bars of evidence.
// Without the smoothed-sign reset it would ratchet up across a long trade until
// it was permanently satisfied and the mode decayed back into defect 3.
//
// The same three fixes are applied to the `Stop Re-Entry = Next flip` lookback,
// which is a copy of this test evaluated against StopDir (anchor = the stop).
//
// NET EFFECT: fewer flips, all of them later. No input changed and no saved
// setting is invalidated. Flip Bars N now means N bars in BOTH modes.
//
// -----------------------------------------------------------------------------
// v1.1 — PRICE DIRECTION FILTER (default OFF; v1.0 behaviour is preserved)
//
// The depth signal produces frequent single-bar flips in rotation zones. P is a
// price-DISPLACEMENT confirmation, deliberately NOT a consecutive-rising-closes
// test: consecutive is fragile (one opposing close resets it) and "rising"
// without a magnitude lets a 1-tick drift count as confirmation.
//
//   dP = Close[i] - Close[i-N]
//   P  = +1 if dP >=  D ticks ; -1 if dP <= -D ticks ; 0 otherwise
//
// P is debounced (must hold its new value for `Price Filter Debounce` bars;
// 1 = no debounce). S is UNCHANGED by the filter — P only composes the target:
//
//   Filter off        : T = S
//   Flat on disagree  : T = S when S == P, else FLAT   (P == 0 is disagreement)
//   Entry only        : T = S always; P gates only ENTRIES FROM FLAT, never
//                       flips (P == 0 is no permission)
//   Hold on disagree  : T = S when S == P, else HOLD (T = previous bar's T,
//                       falling back to the account position when there is
//                       none). P == 0 means hold.
//
// Because the target is reconciled every closed bar, DEFERRED ENTRY is free:
// depth may have flipped N bars ago and price only agree now — the position
// converges on the bar the filter agrees. No event-based re-entry logic exists
// and none should be added.
//
// A NO-DATA bar never updates P's debounce state and never applies the filter.
// No-data handling takes precedence.
//
// *** SAFETY INTERLOCK ***
// "Hold on disagree" removes the only exit mechanism this system has: in an
// always-in strategy the FLIP IS THE STOP, so holding through a disagreeing
// depth flip is an unstopped position with no exit condition. Therefore:
//
//   Price Filter Mode == Hold on disagree  AND  Stop Per Trade == 0
//        -> ALL TRADING IS BLOCKED (same visible-failure path as an unreadable
//           source: red panel + log line).
//
// It does not degrade silently and it does not pick a stop value for you.
// -----------------------------------------------------------------------------
// SETUP
//   1. Put DOM Pressure v3 on a chart and let it run forward — it has no
//      history until it has watched the book.
//   2. Add this study. Set "Pressure Source" to that chart / DOM Pressure v3 /
//      subgraph "Net Pull/Stack" (SG1). SG2..SG5 are derived from that slot by
//      offset (+1..+4) — do NOT point the input at SG5.
//      Chart 0 = this chart; then this study must sit BELOW v3 in the study
//      list, because Sierra calculates studies in list order.
//   3. Leave Enable Trading = No, CSV Export = Yes, and collect a few sessions
//      of rows first. As of v1.2 there is nothing left to calibrate in the
//      entry path — the only tuning knobs are the integers Flip Bars N, Min
//      Hold M, Warmup W, Blind Bars K and (optionally) the price filter's N/D.
//      Validate them ON THE CHART with DOMAlwaysInLab.cpp, then copy them here.
//   4. Only then: Trade Simulation Mode / demo account, Enable Trading = Yes.
//
// *** v1.2 BREAKS SAVED SETTINGS. Three inputs were removed, so every input
// *** index below them shifted. Any chartbook holding v1.1 settings for this
// *** study will apply them to the WRONG inputs. Delete the study instance and
// *** re-add it after the v1.2 rebuild — do not trust a migrated instance.
//
// -----------------------------------------------------------------------------
// v1.4 — REAL PER-TRADE STOP (both studies; parity required)
//
// `Disaster Stop` is renamed `Stop Per Trade` and repositioned: it was a rare
// backstop, it is now a normal per-trade risk control. Same input index, so no
// delete-and-re-add is needed for it.
//
//  1. INTRABAR. v1.3 tested the CLOSE, which is not a stop: a 10-point stop can
//     see far more adverse excursion inside a bar and close back inside it, so
//     it would never trigger. The level is fixed at entry:
//
//         stopLevel = EntryPx - stop * dir
//         long  hit when Low[i]  <= stopLevel
//         short hit when High[i] >= stopLevel
//
//     The LAB tests the bar's range and FILLS AT THE LEVEL. The TRADER places a
//     REAL ATTACHED STOP ORDER with the entry, so it rests at the broker and
//     fills intrabar for real. This reverses v1.0's "no attached stops" rule —
//     that rule existed because "the flip is the stop"; a per-trade stop that
//     only fires on a closed bar is not a stop.
//
//  2. PRECEDENCE. If a bar would satisfy both the stop and the entry/flip logic,
//     THE STOP WINS. Pessimistic, matching the barrier-sweep convention in
//     Testing/CLAUDE.md. Same-bar guard: a position opened at this bar's close
//     did not exist while this bar's High/Low printed, so those extremes cannot
//     stop it.
//
//  3. MIN HOLD DOES NOT APPLY TO THE STOP. Min Hold M gates FLIPS; a stop is not
//     a flip. Neither study's stop path reads minHold or LastFlipBar. A stop
//     delayed by a hold timer is not a stop.
//
//  4. RE-ENTRY (`Stop Re-Entry`, new input, appended so nothing renumbers).
//     After a stop the model is FLAT, so an unconditional "re-enter on the next
//     signal" would re-enter on the very next bar when the sign has not changed
//     — paying the stop distance and reinstating the same trade, making the stop
//     a no-op. So:
//       Signal change (DEFAULT) — the book must print a direction other than the
//           one stopped out (opposite or neutral). This is the v1.2 rule, kept.
//       Next flip — the full Flip Test must fire against the stopped-out
//           direction, using the same Flip Test / Flip Bars N as the in-position
//           machine. Strictly stronger.
//
//  5. ASYNCHRONOUS FILLS (trader only, and the sharpest edge here). A resting
//     stop can take the position flat BETWEEN CALLS with the study deciding
//     nothing. If the next reconcile simply saw target != position with S
//     unchanged it would re-enter at once and the stop would have achieved
//     nothing. The trader therefore remembers what it last COMMANDED and, on
//     entry to every call, treats "account flat while CmdTarget != 0" as a stop
//     fill: lockout set, S zeroed, a CSV row latched. Every exit path also
//     cancels the resting stop, so a stale stop can never fire against a later
//     position.
//
// -----------------------------------------------------------------------------
// v1.3 — DISPLAY-ONLY. One change: a full recalculation now CLEARS the arrow
// and diagnostic subgraphs before replaying, so arrows drawn under previous
// settings cannot linger and mix with the new ones.
//
// The trader is deliberately NOT converted to the Lab's always-full-pass
// scheduling. It reconciles only the freshest closed bar because it cannot
// trade the past, and that is not changing. Consequently, when the source is a
// CROSS-CHART v3 that repaints its cache without recalculating THIS chart, this
// study's historical arrows still describe the data that existed when each
// decision was made. That is correct for an execution layer — its arrows are a
// record of decisions, not a re-derivation — and it is precisely why
// DOMAlwaysInLab.cpp exists. Look at the Lab for "what would the machine do
// with the current data"; look here for "what did the machine actually decide".
//
// No order code, no state machine and no input changed in v1.3. The version
// number is bumped only to keep it equal to the Lab's, which the sync contract
// requires.
// =============================================================================

#include "sierrachart.h"
#include <cstdio>
#include <string>
#include <cmath>

SCDLLName("DOM Always-In Trader")

// ---- Subgraph code indices (UI shows these +1) ------------------------------
const int SG_BUY = 0, SG_SELL = 1, SG_STATE = 2, SG_TARGET = 3, SG_NODATA = 4;

// ---- Input code indices -----------------------------------------------------
// v1.2: Entry Source, Entry Threshold and Flip Sum Threshold REMOVED. Indices
// below them shifted down; 25 inputs, 0..24, contiguous.
const int IN_ENABLE      = 0,  IN_SENDSVC     = 1,  IN_QTY         = 2,
          IN_SOURCE      = 3,  IN_FLIPTEST    = 4,  IN_FLIPBARS    = 5,
          IN_MINHOLD     = 6,  IN_WARMUP      = 7,  IN_BLINDPOL    = 8,
          IN_BLINDBARS   = 9,  IN_STOPPTS    = 10, IN_SESSSTART   = 11,
          IN_SESSEND     = 12, IN_FLATTEN     = 13, IN_MAXTRADES   = 14,
          IN_MAXLOSS     = 15, IN_CSVEXPORT   = 16, IN_CSVPATH     = 17,
          IN_SHOWPANEL   = 18, IN_RESETSESS   = 19,
          // v1.1 price direction filter
          IN_PFILTER     = 20, IN_PFMODE      = 21, IN_PLOOKBACK   = 22,
          IN_PMINMOVE    = 23, IN_PDEBOUNCE   = 24,
          // v1.4: APPENDED at the end so no existing index moves.
          IN_STOPREENTRY = 25;

// Price Filter Mode ordinals — used by the safety interlock, so they are named.
const int PFM_FLAT = 0, PFM_ENTRY = 1, PFM_HOLD = 2;

// ---- Persistent state keys --------------------------------------------------
const int PS_LASTPROC   = 1;   // last CLOSED bar index put through the machine
const int PS_S          = 2;   // standing direction S (+1/-1/0)
const int PS_BLIND      = 3;   // consecutive no-data bars
const int PS_OPP        = 4;   // consecutive opposing SG4 bars (Consecutive test)
const int PS_TRADEDATE  = 5;
const int PS_TRADESTODAY= 6;
const int PS_STOPPEDDAY = 7;
const int PS_WARMUP     = 8;   // consecutive valid bars
const int PS_LASTFLIP   = 9;   // bar index of the last S change
const int PS_STOPBLOCK = 10;  // 1 = 1 = re-entry locked out after a stop fill
const int PS_STOPARM   = 11;  // 1 = signal has cooled below threshold since
const int PS_CSVHDR     = 12;  // 1 = header written this platform session
const int PS_DIAGSTATE  = 13;  // last logged tradingEnabled value (-1 = never)
const int PS_SRCWARNED  = 14;  // 1 = "source missing" already logged
const int PS_PCUR       = 15;  // v1.1: debounced price direction P (+1/-1/0)
const int PS_PPEND      = 16;  // v1.1: pending P value awaiting debounce
const int PS_PPENDCNT   = 17;  // v1.1: bars the pending value has held
const int PS_ILKWARNED  = 18;  // v1.1: 1 = interlock breach already logged
const int PS_PREVTARGET = 19;  // v1.1: previous bar's target T (Hold mode memory)
const int PS_STOPDIR   = 20;  // v1.2: direction that was stopped out
const int PS_STOPOPP    = 22;  // v1.4: "Next flip" re-entry evidence vs StopDir
const int PS_CMDTARGET  = 23;  // v1.4: the target the study last reconciled to
const int PS_PENDSTOP   = 24;  // v1.4: 1 = an async stop fill awaiting a CSV row
const int PS_PENDFLAT   = 21;  // v1.2: 0 none / 1 max_loss / 2 flatten_time —
                               //       a pre-loop flatten waiting to be logged
const int PS_LASTFLIPSRC= 25;  // v1.5: SOURCE bar index of the last S change —
                               //       the anchor the Rolling Sum window cannot
                               //       reach back past (-1 = none / flat)
const int PS_OPPRUN     = 26;  // v1.5: opposing SG4 bars since that anchor, NOT
                               //       reset by a single agreeing RAW bar
const int PS_STOPSRC    = 27;  // v1.5: same anchor for the post-stop lookback
const int PS_STOPRUN    = 28;  // v1.5: opposing SG4 bars vs StopDir since it

// =============================================================================
// SCDateTime -> "YYYY-MM-DD HH:MM:SS".
// GetDateTimeYMDHMS is the real combined accessor; GetDate() is a serial day
// count, NOT a packed YYYYMMDD (that mistake produced year 0004 in TriggerLab).
//   C:\SierraChart\ACS_Source\scdatetime.h:1121
// =============================================================================
static SCString DAIFormatDT(const SCDateTime& dt)
{
    SCDateTime ncdt = dt;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    ncdt.GetDateTimeYMDHMS(year, month, day, hour, minute, second);

    SCString s;
    s.Format("%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return s;
}

// =============================================================================
// {bars} token — filesystem-safe description of THIS chart's bar period, so
// runs on different bar types write to different files and can be pooled.
// Ported unchanged from TriggerLab.cpp (fields confirmed against the installed
// headers: sierrachart.h:3461, scstructures.h:3258, scconstants.h:1230).
// =============================================================================
static SCString DAIBarsToken(SCStudyInterfaceRef sc)
{
    n_ACSIL::s_BarPeriod bp;
    sc.GetBarPeriodParameters(bp);

    const int p1 = bp.IntradayChartBarPeriodParameter1;
    const int p2 = bp.IntradayChartBarPeriodParameter2;
    SCString s;

    switch (bp.IntradayChartBarPeriodType)
    {
        case IBPT_DAYS_MINS_SECS:
            if (p1 > 0 && (p1 % 60) == 0)      s.Format("min%d", p1 / 60);
            else if (p1 > 0)                   s.Format("sec%d", p1);
            else s.Format("bp%d_p%d_%d", (int)bp.IntradayChartBarPeriodType, p1, p2);
            break;
        case IBPT_NUM_TRADES_PER_BAR:    s.Format("tick%d", p1); break;
        case IBPT_VOLUME_PER_BAR:        s.Format("vol%d", p1); break;
        case IBPT_DELTA_VOLUME_PER_BAR:  s.Format("deltavol%d", p1); break;
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
        case IBPT_REVERSAL_IN_TICKS:     s.Format("reversal%d", p1); break;
        case IBPT_RENKO_IN_TICKS:
        case IBPT_FLEX_RENKO_IN_TICKS:
        case IBPT_FLEX_RENKO_IN_TICKS_INVERSE_SETTINGS:
        case IBPT_ALIGNED_RENKO:         s.Format("renko%d", p1); break;
        case IBPT_PRICE_CHANGES_PER_BAR: s.Format("pricechg%d", p1); break;
        case IBPT_MONTHS_PER_BAR:        s.Format("month%d", p1); break;
        case IBPT_POINT_AND_FIGURE:      s.Format("pnf%d_%d", p1, p2); break;
        case IBPT_ACSIL_CUSTOM:          s.Format("custom%d_%d", p1, p2); break;
        default:
            s.Format("bp%d_p%d_%d", (int)bp.IntradayChartBarPeriodType, p1, p2);
            break;
    }
    return s;
}

static std::string DAIReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
    return s;
}

// Strips characters illegal in a Windows filename. Applied to each TOKEN VALUE
// before substitution, never to the whole path, so separators and the drive
// colon survive. An empty result becomes "x" so a malformed token cannot
// silently produce an unopenable path.
static std::string DAISanitize(const std::string& in)
{
    std::string out;
    bool lastUnd = false;
    for (size_t i = 0; i < in.size(); ++i)
    {
        const char ch = in[i];
        if (ch=='<'||ch=='>'||ch==':'||ch=='"'||ch=='/'||ch=='\\'||ch=='|'||ch=='?'||ch=='*')
            continue;
        if (ch==' '||ch=='\t'||ch=='\n'||ch=='\r')
        {
            if (!lastUnd) { out += '_'; lastUnd = true; }
            continue;
        }
        out += ch; lastUnd = false;
    }
    if (out.empty()) out = "x";
    return out;
}

// Expands {chart}, {bars}, {date} in the CSV Path input.
static SCString DAIExpandPath(SCStudyInterfaceRef sc, const SCString& rawPath,
                              const SCString& barsToken, const SCDateTime& dt)
{
    std::string path = rawPath.GetChars();

    SCString chartRaw; chartRaw.Format("%d", sc.ChartNumber);

    SCDateTime ncdt = dt;
    int y=0, mo=0, d=0, h=0, mi=0, se=0;
    ncdt.GetDateTimeYMDHMS(y, mo, d, h, mi, se);
    SCString dateRaw; dateRaw.Format("%04d-%02d-%02d", y, mo, d);

    path = DAIReplaceAll(path, "{chart}", DAISanitize(chartRaw.GetChars()));
    path = DAIReplaceAll(path, "{bars}",  DAISanitize(barsToken.GetChars()));
    path = DAIReplaceAll(path, "{date}",  DAISanitize(dateRaw.GetChars()));

    SCString result; result.Format("%s", path.c_str());
    return result;
}

// Action codes, shared by the CSV writer and the panel.
static const char* const DAI_ACTION_STR[5] =
    { "none", "enter", "reverse", "flatten", "blocked" };

// Session gate. Handles the midnight wrap (start > end -> t>=start || t<=end).
static bool DAIInSession(int t, int start, int end)
{
    if (start <= end) return t >= start && t <= end;
    return t >= start || t <= end;
}

/*===========================================================================*/
SCSFExport scsf_DOMAlwaysInTrader(SCStudyInterfaceRef sc)
{
    SCSubgraphRef sgBuy    = sc.Subgraph[SG_BUY];
    SCSubgraphRef sgSell   = sc.Subgraph[SG_SELL];
    SCSubgraphRef sgState  = sc.Subgraph[SG_STATE];
    SCSubgraphRef sgTarget = sc.Subgraph[SG_TARGET];
    SCSubgraphRef sgNoData = sc.Subgraph[SG_NODATA];

    SCInputRef in_Enable    = sc.Input[IN_ENABLE];
    SCInputRef in_SendSvc   = sc.Input[IN_SENDSVC];
    SCInputRef in_Qty       = sc.Input[IN_QTY];
    SCInputRef in_Source    = sc.Input[IN_SOURCE];
    SCInputRef in_FlipTest  = sc.Input[IN_FLIPTEST];
    SCInputRef in_FlipBars  = sc.Input[IN_FLIPBARS];
    SCInputRef in_MinHold   = sc.Input[IN_MINHOLD];
    SCInputRef in_Warmup    = sc.Input[IN_WARMUP];
    SCInputRef in_BlindPol  = sc.Input[IN_BLINDPOL];
    SCInputRef in_BlindBars = sc.Input[IN_BLINDBARS];
    SCInputRef in_StopPts  = sc.Input[IN_STOPPTS];
    SCInputRef in_SessStart = sc.Input[IN_SESSSTART];
    SCInputRef in_SessEnd   = sc.Input[IN_SESSEND];
    SCInputRef in_Flatten   = sc.Input[IN_FLATTEN];
    SCInputRef in_MaxTrades = sc.Input[IN_MAXTRADES];
    SCInputRef in_MaxLoss   = sc.Input[IN_MAXLOSS];
    SCInputRef in_CSVExport = sc.Input[IN_CSVEXPORT];
    SCInputRef in_CSVPath   = sc.Input[IN_CSVPATH];
    SCInputRef in_ShowPanel = sc.Input[IN_SHOWPANEL];
    SCInputRef in_ResetSess = sc.Input[IN_RESETSESS];
    SCInputRef in_PFilter   = sc.Input[IN_PFILTER];
    SCInputRef in_PFMode    = sc.Input[IN_PFMODE];
    SCInputRef in_PLookback = sc.Input[IN_PLOOKBACK];
    SCInputRef in_PMinMove  = sc.Input[IN_PMINMOVE];
    SCInputRef in_PDebounce = sc.Input[IN_PDEBOUNCE];
    SCInputRef in_StopReEnt = sc.Input[IN_STOPREENTRY];

    // ===================== DEFAULTS =========================================
    if (sc.SetDefaults)
    {
        sc.GraphName    = "DOM Always-In Trader";
        sc.GraphRegion  = 0;             // price region: arrows sit on the bars
        sc.AutoLoop     = 0;             // manual loop over CLOSED bars only
        sc.UpdateAlways = 0;             // nothing here needs intrabar calls
        sc.ValueFormat  = 2;
        sc.FreeDLL      = 0;

        // ---- auto trading configuration ----
        sc.AllowMultipleEntriesInSameDirection = 0;
        sc.MaximumPositionAllowed              = 10;   // tightened to Qty below
        sc.SupportReversals                    = 1;    // one order crosses zero
        sc.AllowOnlyOneTradePerBar             = 1;
        // v1.4: attached stops are now REQUIRED. v1.0 shipped with none because
        // "the flip is the stop" in an always-in system. The Stop Per Trade
        // input turns that rare backstop into a normal per-trade risk control,
        // and a per-trade stop that only fires on a closed bar is not a stop —
        // a 10-point stop can see far more adverse excursion inside a bar. The
        // stop must REST AT THE BROKER so it fills intrabar.
        sc.SupportAttachedOrdersForTrading     = 1;
        sc.CancelAllOrdersOnEntriesAndReversals= 1;
        sc.CancelAllWorkingOrdersOnExit        = 1;
        sc.MaintainTradeStatisticsAndTradesData= 1;
        sc.SendOrdersToTradeService            = 0;    // set per call from input

        sgBuy.Name = "Buy Entry";
        sgBuy.DrawStyle = DRAWSTYLE_ARROW_UP;
        sgBuy.PrimaryColor = RGB(0, 220, 120);
        sgBuy.LineWidth = 3; sgBuy.DrawZeros = false;

        sgSell.Name = "Sell Entry";
        sgSell.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sgSell.PrimaryColor = RGB(255, 70, 70);
        sgSell.LineWidth = 3; sgSell.DrawZeros = false;

        sgState.Name  = "S state (diag)";   sgState.DrawStyle  = DRAWSTYLE_IGNORE;
        sgTarget.Name = "Target (diag)";    sgTarget.DrawStyle = DRAWSTYLE_IGNORE;
        sgNoData.Name = "No-Data (diag)";   sgNoData.DrawStyle = DRAWSTYLE_IGNORE;

        in_Enable.Name = "Enable Trading";
        in_Enable.SetYesNo(0);                       // SAFE DEFAULT — compute only

        in_SendSvc.Name = "Send Orders To Trade Service (No = local simulation)";
        in_SendSvc.SetYesNo(0);                      // SAFE DEFAULT

        in_Qty.Name = "Order Quantity";
        in_Qty.SetInt(1); in_Qty.SetIntLimits(1, 20);

        // Point this at DOM Pressure v3 SG1 "Net Pull/Stack". SG2..SG5 are read
        // from SubgraphIndex+1 .. +4. Chart 0 = this chart.
        in_Source.Name = "Pressure Source (chart / study / SG1 Net Pull-Stack)";
        in_Source.SetChartStudySubgraphValues(0, 0, 0);

        // v1.2: Entry Source / Entry Threshold / Flip Sum Threshold are GONE.
        // Entry is sign(SG1) != 0; the Rolling Sum test is sign(sum) == -S.
        in_FlipTest.Name = "Flip Test (on SG4 RAW)";
        in_FlipTest.SetCustomInputStrings("Consecutive;Rolling Sum");
        in_FlipTest.SetCustomInputIndex(1);           // Rolling Sum

        in_FlipBars.Name = "Flip Bars N";
        in_FlipBars.SetInt(3); in_FlipBars.SetIntLimits(1, 200);

        in_MinHold.Name = "Min Hold Bars M (since last flip)";
        in_MinHold.SetInt(3); in_MinHold.SetIntLimits(0, 500);

        in_Warmup.Name = "Warmup Bars W (consecutive valid bars before entry)";
        in_Warmup.SetInt(50); in_Warmup.SetIntLimits(0, 5000);

        in_BlindPol.Name = "Blind Policy (after K no-data bars while holding)";
        in_BlindPol.SetCustomInputStrings("Flatten;Hold");
        in_BlindPol.SetCustomInputIndex(0);           // Flatten

        in_BlindBars.Name = "Blind Bars K";
        in_BlindBars.SetInt(3); in_BlindBars.SetIntLimits(1, 500);

        in_StopPts.Name = "Stop Per Trade (points; 0 = off)";
        in_StopPts.SetFloat(0.0f);

        in_SessStart.Name = "Session Start (chart timezone)";
        in_SessStart.SetTime(HMS_TIME(9, 30, 0));
        in_SessEnd.Name = "Session End — last entry (chart timezone)";
        in_SessEnd.SetTime(HMS_TIME(15, 45, 0));
        in_Flatten.Name = "Flatten Time (never hold overnight)";
        in_Flatten.SetTime(HMS_TIME(15, 55, 0));

        in_MaxTrades.Name = "Max Trades Per Day (0 = off)";
        in_MaxTrades.SetInt(0); in_MaxTrades.SetIntLimits(0, 1000);

        in_MaxLoss.Name = "Max Daily Loss (points; 0 = off)";
        in_MaxLoss.SetFloat(0.0f);

        in_CSVExport.Name = "CSV Export";
        in_CSVExport.SetYesNo(1);                     // this study IS the lab
        in_CSVPath.Name = "CSV Path  ({chart},{bars},{date} tokens expanded)";
        in_CSVPath.SetString("C:\\SierraChart\\Data\\DOMAlwaysIn_{chart}_{bars}_{date}.csv");

        in_ShowPanel.Name = "Show status panel";
        in_ShowPanel.SetYesNo(1);

        in_ResetSess.Name = "Reset State At Session Start";
        in_ResetSess.SetYesNo(1);

        // ---- v1.1 price direction filter (default OFF = v1.0 behaviour) ----
        in_PFilter.Name = "Price Filter";
        in_PFilter.SetYesNo(0);

        in_PFMode.Name = "Price Filter Mode";
        in_PFMode.SetCustomInputStrings("Flat on disagree;Entry only;Hold on disagree");
        in_PFMode.SetCustomInputIndex(PFM_FLAT);

        // Kept INDEPENDENT of Flip Bars on purpose: they measure different
        // things (price displacement vs depth evidence), and coupling them
        // hides which one is doing the work.
        in_PLookback.Name = "Price Lookback N (bars)";
        in_PLookback.SetInt(5); in_PLookback.SetIntLimits(1, 500);

        in_PMinMove.Name = "Price Min Move D (ticks; TBD-CALIBRATION)";
        in_PMinMove.SetFloat(4.0f);

        in_PDebounce.Name = "Price Filter Debounce (bars; 1 = none)";
        in_PDebounce.SetInt(1); in_PDebounce.SetIntLimits(1, 100);

        // v1.4 — shared with the Lab; see the stop re-entry arming block.
        in_StopReEnt.Name = "Stop Re-Entry (after Stop Per Trade fills)";
        in_StopReEnt.SetCustomInputStrings("Signal change;Next flip");
        in_StopReEnt.SetCustomInputIndex(0);          // Signal change

        return;
    }

    if (sc.LastCallToFunction)
        return;

    // ===================== persistent state ==================================
    int& LastProc    = sc.GetPersistentInt(PS_LASTPROC);
    int& S           = sc.GetPersistentInt(PS_S);
    int& BlindCount  = sc.GetPersistentInt(PS_BLIND);
    int& OppCount    = sc.GetPersistentInt(PS_OPP);
    int& TradeDate   = sc.GetPersistentInt(PS_TRADEDATE);
    int& TradesToday = sc.GetPersistentInt(PS_TRADESTODAY);
    int& StoppedDay  = sc.GetPersistentInt(PS_STOPPEDDAY);
    int& WarmupCnt   = sc.GetPersistentInt(PS_WARMUP);
    int& LastFlipBar = sc.GetPersistentInt(PS_LASTFLIP);
    int& StopBlock  = sc.GetPersistentInt(PS_STOPBLOCK);
    int& StopArm    = sc.GetPersistentInt(PS_STOPARM);
    int& CsvHdrDone  = sc.GetPersistentInt(PS_CSVHDR);
    int& DiagState   = sc.GetPersistentInt(PS_DIAGSTATE);
    int& SrcWarned   = sc.GetPersistentInt(PS_SRCWARNED);
    int& PCur        = sc.GetPersistentInt(PS_PCUR);
    int& PPend       = sc.GetPersistentInt(PS_PPEND);
    int& PPendCnt    = sc.GetPersistentInt(PS_PPENDCNT);
    int& IlkWarned   = sc.GetPersistentInt(PS_ILKWARNED);
    int& PrevTarget  = sc.GetPersistentInt(PS_PREVTARGET);
    int& StopDir    = sc.GetPersistentInt(PS_STOPDIR);
    int& PendFlat    = sc.GetPersistentInt(PS_PENDFLAT);
    int& StopOpp     = sc.GetPersistentInt(PS_STOPOPP);
    int& CmdTarget   = sc.GetPersistentInt(PS_CMDTARGET);
    int& PendStop    = sc.GetPersistentInt(PS_PENDSTOP);
    int& LastFlipSrc = sc.GetPersistentInt(PS_LASTFLIPSRC);
    int& OppRun      = sc.GetPersistentInt(PS_OPPRUN);
    int& StopSrc     = sc.GetPersistentInt(PS_STOPSRC);
    int& StopRun     = sc.GetPersistentInt(PS_STOPRUN);

    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        // A full recalculation replays the state machine over whatever history
        // v3's cache repainted, so S / warmup are primed — but it sends NO
        // orders and writes NO CSV rows (both are gated on realTime below), so
        // a recalculation can neither trade the past nor duplicate rows.
        // v1.3 DISPLAY-ONLY FIX: clear the arrow and diagnostic subgraphs
        // before replaying. The replay redraws arrows for the decisions the
        // machine makes under the CURRENT settings; without this clear, arrows
        // drawn under the PREVIOUS settings stayed on the chart and the user
        // saw a mixture of two parameterisations that never both existed.
        //
        // This is the ONLY thing v1.3 changes in this study. It does NOT
        // convert the trader to full-pass and it does NOT touch the order path:
        // this study still reconciles only the freshest closed bar, by design.
        // Note it therefore still cannot repaint history when a CROSS-CHART v3
        // repaints its cache without recalculating this chart — and that is
        // deliberate. Its arrows are the record of what it DECIDED in real
        // time; back-filling them from repainted history would fabricate a
        // decision history that never happened, which is exactly what
        // DOMAlwaysInLab.cpp exists to provide instead.
        for (int k = 0; k < sc.ArraySize; ++k)
        {
            sgBuy[k] = 0.0f; sgSell[k] = 0.0f;
            sgState[k] = 0.0f; sgTarget[k] = 0.0f; sgNoData[k] = 0.0f;
        }

        LastProc = -1;
        S = 0; BlindCount = 0; OppCount = 0; WarmupCnt = 0; LastFlipBar = -1;
        StopBlock = 0; StopArm = 1;
        // Daily guards reset on every full recalculation: a same-day restart is
        // a recalculation but not a date change, so the date-based reset below
        // would never fire and a latched StoppedDay would persist forever.
        TradeDate = 0; TradesToday = 0; StoppedDay = 0;
        DiagState = -1; SrcWarned = 0;
        PCur = 0; PPend = 0; PPendCnt = 0; IlkWarned = 0; PrevTarget = 0;
        StopDir = 0; PendFlat = 0;
        StopOpp = 0; CmdTarget = 0; PendStop = 0;
        LastFlipSrc = -1; OppRun = 0; StopSrc = -1; StopRun = 0;
    }

    // ===================== per-call configuration ============================
    const int    qty        = in_Qty.GetInt();
    const int    flipTestIx = in_FlipTest.GetIndex();      // 0 = consec, 1 = sum
    const int    flipBars   = in_FlipBars.GetInt();
    const int    minHold    = in_MinHold.GetInt();
    const int    warmupReq  = in_Warmup.GetInt();
    const int    blindPolIx = in_BlindPol.GetIndex();      // 0 = flatten, 1 = hold
    const int    blindBars  = in_BlindBars.GetInt();
    const double stopPts   = in_StopPts.GetFloat();
    const int    reEntryIx  = in_StopReEnt.GetIndex();     // 0 = signal change, 1 = next flip
    const int    sessStart  = in_SessStart.GetTime();
    const int    sessEnd    = in_SessEnd.GetTime();
    const int    flattenT   = in_Flatten.GetTime();
    const int    maxTrades  = in_MaxTrades.GetInt();
    const double maxLoss    = in_MaxLoss.GetFloat();
    const bool   csvExport  = in_CSVExport.GetYesNo() != 0;
    const bool   pFilterOn  = in_PFilter.GetYesNo() != 0;
    const int    pFMode     = in_PFMode.GetIndex();
    const int    pLookback  = in_PLookback.GetInt();
    const double pMinMove   = in_PMinMove.GetFloat();     // TICKS
    const int    pDebounce  = in_PDebounce.GetInt();
    const double tickSize   = (sc.TickSize > 0.0) ? sc.TickSize : 1.0;
    const double pMinMovePx = pMinMove * tickSize;        // points

    sc.SendOrdersToTradeService = in_SendSvc.GetYesNo();
    sc.MaximumPositionAllowed   = qty;

    const int lastBar = sc.ArraySize - 2;   // last CLOSED bar — never -1
    if (lastBar < 1)
        return;

    // ===================== source arrays (SG1..SG5) ==========================
    // The input names the SG1 slot; SG2..SG5 come from the SAME study by
    // copying the reference struct and bumping the subgraph index.
    s_ChartStudySubgraphValues ref1 = in_Source.GetChartStudySubgraphValues();
    if (ref1.ChartNumber <= 0)
        ref1.ChartNumber = sc.ChartNumber;              // 0 = this chart

    s_ChartStudySubgraphValues ref2 = ref1; ref2.SubgraphIndex = ref1.SubgraphIndex + 1;
    s_ChartStudySubgraphValues ref3 = ref1; ref3.SubgraphIndex = ref1.SubgraphIndex + 2;
    s_ChartStudySubgraphValues ref4 = ref1; ref4.SubgraphIndex = ref1.SubgraphIndex + 3;
    s_ChartStudySubgraphValues ref5 = ref1; ref5.SubgraphIndex = ref1.SubgraphIndex + 4;

    SCFloatArray Src1, Src2, Src3, Src4, Src5;
    sc.GetStudyArrayFromChartUsingID(ref1, Src1);
    sc.GetStudyArrayFromChartUsingID(ref2, Src2);
    sc.GetStudyArrayFromChartUsingID(ref3, Src3);
    sc.GetStudyArrayFromChartUsingID(ref4, Src4);
    sc.GetStudyArrayFromChartUsingID(ref5, Src5);

    const int srcSize = Src1.GetArraySize();
    const bool sourceOK = (srcSize > 0 && Src2.GetArraySize() > 0 &&
                           Src3.GetArraySize() > 0 && Src4.GetArraySize() > 0 &&
                           Src5.GetArraySize() > 0);

    if (!sourceOK && !SrcWarned)
    {
        sc.AddMessageToLog(
            "DOM Always-In Trader: pressure source array is EMPTY. Point the "
            "'Pressure Source' input at DOM Pressure v3 SG1 'Net Pull/Stack' on a "
            "chart that is open in this chartbook. Trading is BLOCKED until it "
            "returns data.", 1);
        SrcWarned = 1;
    }
    if (sourceOK)
        SrcWarned = 0;

    const bool crossChart = (ref1.ChartNumber != sc.ChartNumber);

    // ===================== daily counters ====================================
    const int today = sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.ArraySize - 1]);
    if (today != TradeDate)
    {
        TradeDate = today;
        TradesToday = 0;
        StoppedDay  = 0;
        if (in_ResetSess.GetYesNo())
        {
            // Never carry a standing direction across the session boundary —
            // the book that produced it is 17 hours stale.
            S = 0; OppCount = 0; BlindCount = 0; WarmupCnt = 0;
            LastFlipBar = -1; StopBlock = 0; StopArm = 1; StopDir = 0;
            StopOpp = 0;
            LastFlipSrc = -1; OppRun = 0; StopSrc = -1; StopRun = 0;
            PCur = 0; PPend = 0; PPendCnt = 0; PrevTarget = 0;
        }
    }

    // ===================== SAFETY INTERLOCK ==================================
    // "Hold on disagree" holds a position through a disagreeing depth flip. In
    // an always-in system the FLIP IS THE STOP, so that configuration has NO
    // exit condition at all unless a stop backstop exists. This is the
    // conclusion recorded in RenkoFlip_FilteredAlwaysIn_Design.md ("HOLD
    // requires a stop backstop as insurance, not strategy").
    //
    // Block ALL trading rather than degrading silently, and never auto-pick a
    // stop value on the user's behalf.
    const bool interlockBreach = pFilterOn && (pFMode == PFM_HOLD) && !(stopPts > 0.0);

    if (interlockBreach && !IlkWarned)
    {
        sc.AddMessageToLog(
            "DOM Always-In Trader: TRADING BLOCKED — Price Filter Mode is 'Hold on "
            "disagree' with Stop Per Trade = 0. Holding through a disagreeing depth "
            "flip leaves an unstopped position with no exit condition. Set a "
            "Stop Per Trade (points) or choose a different Price Filter Mode.", 1);
        IlkWarned = 1;
    }
    if (!interlockBreach)
        IlkWarned = 0;

    // ===================== trading permission ================================
    const bool realTime = !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0;
    const bool tradingEnabled = in_Enable.GetYesNo() != 0 && realTime && sourceOK
                                && !interlockBreach;

    if ((int)tradingEnabled != DiagState)
    {
        SCString msg;
        msg.Format("DOM Always-In Trader DIAG: tradingEnabled=%d (Enable=%d IsFullRecalc=%d "
                   "DownloadingHist=%d SourceOK=%d InterlockBreach=%d)",
                   (int)tradingEnabled, in_Enable.GetYesNo(), (int)sc.IsFullRecalculation,
                   sc.DownloadingHistoricalData, (int)sourceOK, (int)interlockBreach);
        sc.AddMessageToLog(msg, 0);
        DiagState = (int)tradingEnabled;
    }

    s_SCPositionData Position;
    sc.GetTradePosition(Position);

    // =========================================================================
    // v1.4 — ASYNCHRONOUS STOP-FILL DETECTION. Read this before touching it.
    //
    // The per-trade stop now RESTS AT THE BROKER as an attached order, so the
    // position can go flat BETWEEN CALLS without this study deciding anything.
    // If the next closed-bar reconcile then simply sees target != position with
    // S unchanged, it re-enters immediately and the stop has achieved nothing —
    // it would have paid the stop distance and reinstated the same trade.
    //
    // So: remember what the study last COMMANDED (CmdTarget, set only where an
    // order is actually sent or a flatten is actually issued). If the account is
    // flat while CmdTarget is non-zero, the position went away without us. Treat
    // that as a stop fill and apply the same lockout an in-code stop applies.
    //
    // Checked HERE, on entry to the call, not inside the bar loop: the loop can
    // legitimately process zero bars for many calls, and the lockout must be in
    // force before the next reconcile runs, not after it.
    //
    // A manual flatten by the user, or a broker-side liquidation, also lands
    // here and is also treated as a stop. That is the safe reading: something
    // closed the position that this study did not ask for, so do not re-enter
    // until the signal has changed. It is stated in the spec, not silent.
    if (CmdTarget != 0 && Position.PositionQuantity == 0)
    {
        StopBlock = 1; StopArm = 0;
        StopDir   = CmdTarget;    // the direction that must cool off
        StopOpp   = 0;            // fresh evidence for "Next flip"
        // v1.5: no source index exists out here — the lookback anchors itself on
        // the first bar the loop processes after the lockout goes on.
        StopSrc   = -1; StopRun = 0;
        S         = 0;
        LastFlipSrc = -1; OppRun = 0;
        CmdTarget = 0;
        PendStop  = 1;            // the next processed bar carries it to the CSV

        // Remove anything the broker may still be holding for the position that
        // just closed — a working stop from a partially-filled or replaced order
        // must never survive to fire against a LATER position.
        if (tradingEnabled && Position.WorkingOrdersExist)
            sc.CancelAllOrders();

        sc.AddMessageToLog("DOM Always-In Trader: position went flat without a command "
                           "from this study (resting stop filled, or a manual/broker "
                           "flatten). Treated as a STOP FILL — re-entry locked per "
                           "Stop Re-Entry.", 1);
    }
    else if (Position.PositionQuantity == 0)
    {
        CmdTarget = 0;            // stay in sync while genuinely flat
    }

    const double perPoint  = (sc.TickSize > 0) ? (sc.CurrencyValuePerTick / sc.TickSize) : 0.0;
    const double dayPnLPts = (perPoint > 0) ? (Position.DailyProfitLoss / perPoint) : 0.0;

    // Daily loss guard — checked on every call, not only on bar close.
    if (tradingEnabled && !StoppedDay && maxLoss > 0.0 && dayPnLPts <= -maxLoss)
    {
        if (Position.PositionQuantity != 0 || Position.WorkingOrdersExist)
            sc.FlattenAndCancelAllOrders();
        StoppedDay = 1;
        S = 0;
        // v1.2 DEFECT FIX: this flatten really happens but it happens OUTSIDE
        // the closed-bar loop, so no CSV row ever said so — the position went
        // to zero with no action=3 anywhere in the log. Latch it and let the
        // next bar the loop processes carry it. Latched (not written here)
        // because a call can flatten with zero newly closed bars to write to.
        PendFlat = 1;
        CmdTarget = 0;      // v1.4: the flatten above cancelled the resting stop
        sc.AddMessageToLog("DOM Always-In Trader: max daily loss hit — flattened and stopped for the day.", 1);
    }

    // Flatten time — checked on every call so it fires even in dead tape.
    {
        const int nowT = sc.BaseDateTimeIn[sc.ArraySize - 1].GetTime();
        const bool pastFlatten = (sessStart <= flattenT)
            ? (nowT >= flattenT)
            : (nowT >= flattenT && nowT < sessStart);
        if (pastFlatten && !StoppedDay)
        {
            if (tradingEnabled && (Position.PositionQuantity != 0 || Position.WorkingOrdersExist))
            {
                sc.FlattenAndCancelAllOrders();
                // v1.2 DEFECT FIX: same suppression as the daily-loss path —
                // the flatten happened outside the loop and no row recorded it.
                PendFlat = 2;
                CmdTarget = 0;      // v1.4: the flatten above cancelled the resting stop
                sc.AddMessageToLog("DOM Always-In Trader: flatten time — position closed, no overnight hold.", 0);
            }
            if (tradingEnabled)
                StoppedDay = 1;
            S = 0;
        }
    }

    // ===================== CSV row buffer ====================================
    // One row per closed bar processed in REAL TIME. Appended, because the
    // series is forward-only: there is no history to rewrite, and a rewrite
    // after a restart would destroy every row collected before it.
    const SCString barsToken = DAIBarsToken(sc);
    bool csvWriteFailed = false;
    SCString lastAction = "none";
    SCString lastReason = "init";
    int      lastTarget = S;
    double   lastdPTicks = 0.0;      // v1.1 panel
    bool     lastVetoing = false;

    // ===================== closed-bar loop ===================================
    for (int i = LastProc + 1; i <= lastBar; ++i)
    {
        // ---- map to the source chart's own CLOSED bar ----
        int si = i;
        if (crossChart)
        {
            si = sc.GetContainingIndexForSCDateTime(ref1.ChartNumber, sc.BaseDateTimeIn[i]);
            // The containing bar may still be FORMING on the source chart, and
            // v3 rewrites the forming bar tick by tick. Clamp to that chart's
            // last closed bar.
            if (si > srcSize - 2) si = srcSize - 2;
        }

        double v1 = 0.0, v2 = 0.0, v3 = 0.0, v4 = 0.0, v5 = 0.0;
        bool haveBar = sourceOK && si >= 0 && si < srcSize;
        if (haveBar)
        {
            v1 = Src1[si]; v2 = Src2[si]; v3 = Src3[si];
            v4 = Src4[si]; v5 = Src5[si];
        }

        // ---- NO-DATA TEST: exact triple zero on SG1/SG2/SG3 ----
        // Not "balanced book" — a measured bar producing exact triple zero is
        // effectively impossible. Missing source counts as no-data too.
        const bool noData = (!haveBar) || (v1 == 0.0 && v2 == 0.0 && v3 == 0.0);

        // v1.2: the entry signal IS SG1. There is no Entry Source input any
        // more (sign(SG5) == sign(SG1) identically), and no threshold to
        // compare a magnitude against. The name is kept because the CSV column
        // `entry_signal` is kept, so v1.1 and v1.2 exports stay poolable.
        const double entrySignal = v1;

        // ---- v1.1 PRICE DIRECTION P (this chart's own closes) ----
        // Net displacement over N bars, NOT consecutive rising closes:
        // consecutive is fragile (one opposing close resets it) and "rising"
        // without a magnitude lets a 1-tick drift count as confirmation.
        // dP / pRaw are computed on every bar for REPORTING; the debounce and
        // the filter itself are updated only on valid bars (see below).
        double dPPx = 0.0;
        if (i - pLookback >= 0)
            dPPx = (double)sc.Close[i] - (double)sc.Close[i - pLookback];
        const double dPTicks = (tickSize > 0.0) ? (dPPx / tickSize) : 0.0;

        int pRaw = 0;
        if (i - pLookback >= 0 && pMinMovePx > 0.0)
        {
            if (dPPx >=  pMinMovePx) pRaw =  1;
            else if (dPPx <= -pMinMovePx) pRaw = -1;
        }

        int action = 0;                  // 0 none 1 enter 2 reverse 3 flatten 4 blocked
        SCString reason = "hold";
        double decisionPx = sc.Close[i];

        if (noData)
        {
            // Hold position, do NOT touch S or oppEvidence, do NOT advance the
            // flip test. Warmup restarts: warmup means CONSECUTIVE valid bars.
            BlindCount++;
            WarmupCnt = 0;
            reason = "no_data";

            if (S != 0 && BlindCount >= blindBars)
            {
                if (blindPolIx == 0)     // Flatten
                {
                    S = 0;
                    action = 3;
                    reason = "blind_flatten";
                }
                else                     // Hold
                {
                    reason = "blind_hold";
                }
            }
        }
        else
        {
            BlindCount = 0;
            if (WarmupCnt < 1000000) WarmupCnt++;

            // ---- P debounce (valid bars only; a no-data bar never advances it)
            // Same debounce pattern as RenkoFlipAutoTrader's filter, with ONE
            // deliberate difference: 0 is allowed to propagate. RenkoFlip's
            // version only debounces non-zero values, which means neutral never
            // becomes the current state. Here P == 0 is a real state (inside the
            // neutral band) and must be able to reach the state machine, because
            // Flat-on-disagree treats it as disagreement.
            if (pRaw != PCur)
            {
                if (pRaw == PPend) PPendCnt++;
                else { PPend = pRaw; PPendCnt = 1; }
                if (PPendCnt >= pDebounce)
                {
                    PCur = pRaw;
                    PPend = 0; PPendCnt = 0;
                }
            }
            else
            {
                PPend = 0; PPendCnt = 0;
            }

            const int s = (v1 > 0.0) ? 1 : ((v1 < 0.0) ? -1 : 0);

            // ---- v1.4 STOP RE-ENTRY ARMING ----------------------------------
            // After a stop the model is FLAT. An unconditional "re-enter on the
            // next signal" would therefore re-enter on the very NEXT bar if the
            // sign has not changed, which makes the stop a no-op: it would pay
            // the stop distance and immediately reinstate the same position.
            // So re-entry is gated, and `Stop Re-Entry` chooses how hard:
            //
            //   Signal change (default) - the book must print a direction other
            //       than the one stopped out (opposite or NEUTRAL). This is the
            //       v1.2 rule, preserved exactly.
            //   Next flip - the full flip test must fire against the stopped-out
            //       direction, using the same Flip Test / Flip Bars N the
            //       in-position machine uses. Strictly stronger: a sign that
            //       merely wobbles is not enough, the SG4 RAW evidence has to
            //       turn. Evaluated against StopDir because there is no S to
            //       flip from while flat.
            //
            // Only evaluated while the lockout is actually in force. When
            // StopBlock is 0, StopArm is never read (see the entry branch), and
            // a stop always sets StopArm = 0 fresh, so this is behaviourally
            // identical to v1.2/v1.3 for the default mode.
            if (StopBlock && !StopArm)
            {
                if (reEntryIx == 0)
                {
                    if (StopDir == 0 || s != StopDir)
                        StopArm = 1;
                }
                else
                {
                    // v1.5: anchor the lookback on the first bar processed after
                    // the lockout went on. Evidence that predates the stop is
                    // evidence for the trade that was just stopped out, not for
                    // the next one.
                    if (StopSrc < 0) { StopSrc = si; StopRun = 0; }

                    const int rawSignLk = (v4 > 0.0) ? 1 : ((v4 < 0.0) ? -1 : 0);
                    if (rawSignLk == -StopDir)     StopOpp++;
                    else if (rawSignLk == StopDir) StopOpp = 0;
                    // rawSignLk == 0: leave the counter alone (transparent)

                    // v1.5: StopRun is StopOpp's tolerant twin — it counts
                    // opposing bars since the anchor and is NOT reset by one
                    // agreeing bar. Rolling Sum uses it; Consecutive uses
                    // StopOpp. See the flip test below for the full rationale.
                    if (rawSignLk == -StopDir) StopRun++;

                    bool lkPass = false;
                    if (StopDir == 0)
                    {
                        lkPass = true;               // nothing to flip against
                    }
                    else if (flipTestIx == 0)
                    {
                        lkPass = (StopOpp >= flipBars);
                    }
                    else
                    {
                        double lkSum = 0.0;
                        int lkCounted = 0;
                        for (int k = si; k >= StopSrc && lkCounted < flipBars; --k)
                        {
                            if (Src1[k] == 0.0 && Src2[k] == 0.0 && Src3[k] == 0.0) continue;
                            lkSum += Src4[k];
                            lkCounted++;
                        }
                        lkPass = (lkCounted >= flipBars) && (StopRun >= flipBars)
                                 && ((double)(-StopDir) * lkSum > 0.0);
                    }

                    if (lkPass)
                        StopArm = 1;
                }
            }

            if (S == 0)
            {
                // ---- FLAT: SIGN entry (v1.2 — no magnitude test) ----
                if (WarmupCnt < warmupReq)
                {
                    reason = "warmup";
                }
                else if (StopBlock && !StopArm)
                {
                    reason = "stop_lockout";
                    action = 4;
                }
                else if (s != 0)
                {
                    const int wantDir = s;

                    // Entry-only mode: P gates entries FROM FLAT and nothing
                    // else. P == 0 is no permission. S is left at 0, so the
                    // next bar re-tests — that IS the deferred entry, and the
                    // target-state reconcile makes it self-healing. Do not add
                    // event-based re-entry logic on top of it.
                    if (pFilterOn && pFMode == PFM_ENTRY && PCur != wantDir)
                    {
                        action = 4;
                        reason = "filter_veto_entry";
                    }
                    else
                    {
                        S = wantDir;
                        OppCount = 0;
                        LastFlipBar = i;
                        LastFlipSrc = si;   // v1.5: Rolling Sum window anchor
                        OppRun = 0;
                        StopBlock = 0;
                        StopDir = 0;
                        StopOpp = 0;
                        StopSrc = -1; StopRun = 0;
                        action = 1;
                        reason = "entry_sign";
                    }
                }
                else
                {
                    reason = "no_sign";
                }
            }
            else
            {
                // ---- IN POSITION: agreement / transparent / opposing ----
                if (s == S)
                {
                    OppCount = 0;
                    // v1.5: the smoothed regime came back to our side, so the
                    // opposing evidence collected while it was against us is
                    // stale. Without this reset OppRun ratchets up over a long
                    // trade until it is permanently satisfied, and the flip
                    // decays back to a bare sum test on one bar.
                    OppRun = 0;
                    reason = "agree";
                }
                else if (s == 0)
                {
                    // Transparent bar: no advance, no reset. SG1 == 0 exactly on
                    // a data-bearing bar is a genuine zero crossing of the SMA,
                    // and it is evidence for nobody.
                    reason = "transparent";
                }
                else
                {
                    // Opposing. SG1 says the smoothed regime disagrees; the FLIP
                    // TEST itself is measured on SG4 RAW, because SG1 is a moving
                    // average — consecutive SG1 readings are not independent
                    // evidence and they lag by the smoothing period.
                    const int rawSign = (v4 > 0.0) ? 1 : ((v4 < 0.0) ? -1 : 0);
                    if (rawSign == -S)      OppCount++;
                    else if (rawSign == S)  OppCount = 0;
                    // rawSign == 0: leave the counter alone (transparent)

                    // v1.5: OppRun is OppCount's TOLERANT twin. OppCount is
                    // reset by a single agreeing RAW bar (that is what makes it
                    // "Consecutive"); OppRun is not, and is cleared only when
                    // the SMOOTHED sign returns to our side (the agree branch)
                    // or on a flip/entry. That is exactly what motivated
                    // Rolling Sum in the first place — one small contrary bar
                    // must not wipe out real accumulated evidence.
                    if (rawSign == -S) OppRun++;

                    bool flipPass = false;
                    if (flipTestIx == 0)
                    {
                        flipPass = (OppCount >= flipBars);
                    }
                    else
                    {
                        // Rolling sum of SG4 RAW over the last N VALID closed
                        // source bars, no-data bars skipped (they are not
                        // zeros). SIGN test, no magnitude constant — strictly
                        // greater than zero, because sum == 0 is not a sign.
                        //
                        // v1.5 DEFECT FIX (three defects, all of which printed
                        // an arrow after ONE opposing candle):
                        //
                        //  1. The window was a fixed N-bar span with no-data
                        //     bars skipped, so a span holding one valid bar
                        //     passed on that ONE bar's sign — N degenerated to
                        //     1 exactly when depth was patchy. It now walks
                        //     back until it has collected N VALID bars, which
                        //     is what the header always claimed, and requires
                        //     counted >= N rather than counted > 0.
                        //
                        //  2. The window was not anchored to the position, so
                        //     it could sum bars from BEFORE the entry. Entry is
                        //     taken on SMOOTHED SG1 while this test reads RAW
                        //     SG4, and raw turns first — so on the first
                        //     opposing bar of a fresh trade the sum was often
                        //     already negative and the trade flipped straight
                        //     back out. Consecutive mode never had this bug
                        //     because OppCount is zeroed on entry. The window
                        //     now stops at LastFlipSrc.
                        //
                        //  3. The sum is a MAGNITUDE, so one large opposing bar
                        //     outweighed N-1 agreeing ones and "Flip Bars N"
                        //     never meant N bars. It now also requires N
                        //     opposing bars (OppRun) — the sum confirms the
                        //     evidence, it can no longer manufacture it.
                        const int anchor = (LastFlipSrc >= 0) ? LastFlipSrc : 0;
                        double sum = 0.0;
                        int counted = 0;
                        for (int k = si; k >= anchor && counted < flipBars; --k)
                        {
                            const bool kNo = (Src1[k] == 0.0 && Src2[k] == 0.0 && Src3[k] == 0.0);
                            if (kNo) continue;
                            sum += Src4[k];
                            counted++;
                        }
                        flipPass = (counted >= flipBars) && (OppRun >= flipBars)
                                   && ((double)(-S) * sum > 0.0);
                    }

                    const bool heldLongEnough =
                        (LastFlipBar < 0) || ((i - LastFlipBar) >= minHold);

                    if (flipPass && heldLongEnough)
                    {
                        S = -S;
                        OppCount = 0;
                        OppRun = 0;
                        LastFlipBar = i;
                        LastFlipSrc = si;   // v1.5: new anchor for the new side
                        action = 2;
                        reason = "flip";
                    }
                    else if (flipPass)
                    {
                        reason = "flip_blocked_minhold";
                    }
                    else
                    {
                        reason = "opposing";
                    }
                }
            }
        }

        // ---- session gate on ENTRIES (exits are never gated) ----
        const int barT = sc.BaseDateTimeIn[i].GetTime();
        const bool inSession = DAIInSession(barT, sessStart, sessEnd);
        if (S != 0 && !inSession)
        {
            S = 0;
            if (action == 1 || action == 2) { action = 4; reason = "out_of_session"; }
        }

        int  target      = S;
        bool stopHit = false;

        int posDir = 0;
        if (Position.PositionQuantity > 0) posDir = 1;
        else if (Position.PositionQuantity < 0) posDir = -1;

        // ---- v1.1 FILTER COMPOSITION: S is untouched, only T changes --------
        // Applied only on valid bars — a no-data bar's handling (hold / blind
        // policy) takes precedence and is not second-guessed by price.
        // Entry-only mode did its work inside the state machine above.
        const bool filterAgrees = (S != 0) && (PCur == S);
        if (pFilterOn && !noData && S != 0)
        {
            if (pFMode == PFM_FLAT && !filterAgrees)
            {
                target = 0;
                if (action == 0 || action == 1 || action == 2)
                {
                    action = (posDir != 0) ? 3 : 0;
                    reason = "filter_veto_flat";
                }
            }
            else if (pFMode == PFM_HOLD && !filterAgrees)
            {
                // Hold = keep the MODEL's previous target, falling back to the
                // account only when the model has none. Reading the account
                // first looks purer but is wrong twice: (a) with Enable Trading
                // = No there is never a position, so "hold" would log as flat
                // and the measurement layer would not describe the strategy;
                // (b) if an entry order was missed, the account says flat, and
                // "hold flat" would strand the trade forever instead of
                // retrying. The reconcile still compares T against the REAL
                // position every bar, so self-healing is unaffected.
                target = (PrevTarget != 0) ? PrevTarget : posDir;
                if (action == 0 || action == 1 || action == 2)
                {
                    action = 0;
                    reason = "filter_hold";
                }
            }
        }

        // ---- per-trade stop: CLOSED-BAR BACKSTOP ONLY (v1.4) ----------------
        // The real stop is the ATTACHED STOP ORDER placed with the entry (see
        // the reconcile below). It rests at the broker and fills intrabar, and
        // a fill it produces is detected at the top of the next call by the
        // asynchronous-flat check, not here.
        //
        // This block remains as a backstop for the case where the resting order
        // is absent or was rejected: if a closed bar shows the position beyond
        // the stop distance and the position is still open, flatten in code. It
        // can only ever fire LATE relative to the resting order, never early,
        // so it cannot pre-empt it.
        //
        // MIN HOLD DOES NOT APPLY, here or at the broker. Min Hold M gates
        // FLIPS; a stop is not a flip. Nothing in this block reads minHold or
        // LastFlipBar, and nothing should.
        if (posDir != 0 && stopPts > 0.0 &&
            (sc.Close[i] - Position.AveragePrice) * posDir <= -stopPts)
        {
            S = 0; target = 0;
            StopBlock = 1; StopArm = 0;
            StopDir = posDir;          // the direction that must cool off
            StopOpp = 0;               // v1.4: fresh evidence for "Next flip"
            StopSrc = si; StopRun = 0; // v1.5: anchor the lookback at the stop
            LastFlipSrc = -1; OppRun = 0;
            action = 3; reason = "stop_hit"; stopHit = true;
            if (tradingEnabled)
            {
                sc.FlattenAndCancelAllOrders();
                CmdTarget = 0;
                sc.AddMessageToLog("DOM Always-In Trader: STOP HIT on a closed bar "
                                   "(the resting attached stop did not fire) - flattened, "
                                   "re-entry locked per Stop Re-Entry.", 1);
            }
        }

        // ================= RECONCILE (orders) ================================
        // Orders only on the freshest closed bar, in real time. Reconciling on
        // state (not on events) means a missed or rejected fill self-heals on
        // the next closed bar.
        // reconciled = 1 means THIS bar was the one allowed to send orders. When
        // several bars close between study calls only the freshest reconciles,
        // so an "enter" on an older bar is the machine's decision, not a fill.
        double fillPx = 0.0;
        int    reconciled = 0;
        if (tradingEnabled && i == lastBar && !StoppedDay && !stopHit)
        {
            reconciled = 1;
            const double targetQty = (double)target * qty;
            const double posQty    = Position.PositionQuantity;

            if (targetQty != posQty)
            {
                const bool tradesLeft = !(maxTrades > 0 && TradesToday >= maxTrades);

                if (targetQty == 0.0)
                {
                    if (posQty != 0.0)
                    {
                        // FlattenAndCancelAllOrders also cancels the resting
                        // attached stop, so it can never fire against a later
                        // position. Same on every other exit path below.
                        sc.FlattenAndCancelAllOrders();
                        CmdTarget = 0;
                        action = 3;
                        fillPx = decisionPx;
                    }
                }
                else if (!tradesLeft)
                {
                    // Always-in has no stop, so refusing a reversal on a trade
                    // cap would leave an unstopped position on the wrong side.
                    // The cap blocks NEW exposure; it never traps existing
                    // exposure. Flatten instead.
                    if (posQty != 0.0)
                    {
                        sc.FlattenAndCancelAllOrders();
                        CmdTarget = 0;
                        action = 3;
                        fillPx = decisionPx;
                    }
                    else
                    {
                        action = 4;
                    }
                    reason = "max_trades";
                }
                else
                {
                    // Flat or opposite: ONE entry order establishes the target —
                    // sc.SupportReversals auto-sizes it across zero.
                    s_SCNewOrder Order;
                    Order.OrderQuantity = (targetQty > 0.0) ? targetQty : -targetQty;
                    Order.OrderType     = SCT_ORDERTYPE_MARKET;

                    // v1.4: attach the per-trade stop to the entry. Stop1Offset
                    // is in PRICE units from the fill, so Sierra prices it off
                    // the ACTUAL fill rather than off our decision price, and it
                    // rests at the broker where it can fill intrabar.
                    // sc.CancelAllOrdersOnEntriesAndReversals = 1 (set in the
                    // defaults) removes the previous position's stop as this
                    // order goes in, so a reversal can never leave an orphan
                    // stop resting against the new position.
                    if (stopPts > 0.0)
                    {
                        Order.Stop1Offset            = stopPts;
                        Order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;
                    }

                    const int result = (targetQty > 0.0) ? sc.BuyEntry(Order) : sc.SellEntry(Order);
                    if (result > 0)
                    {
                        TradesToday++;
                        CmdTarget = target;
                        fillPx = decisionPx;
                        action = (posQty == 0.0) ? 1 : 2;
                        if (targetQty > 0.0) sgBuy[i]  = sc.Low[i]  - sc.TickSize * 4;
                        else                 sgSell[i] = sc.High[i] + sc.TickSize * 4;
                    }
                    else
                    {
                        action = 4;
                        reason = "order_rejected";
                        SCString msg;
                        msg.Format("DOM Always-In Trader: order rejected, error %d", result);
                        sc.AddMessageToLog(msg, 1);
                    }
                }
            }
        }
        else if (!tradingEnabled && (action == 1 || action == 2 || action == 3))
        {
            // Signal happened, orders are off. Keep the arrow so the chart still
            // shows what the machine decided, but say so in the CSV.
            if (target > 0)      sgBuy[i]  = sc.Low[i]  - sc.TickSize * 4;
            else if (target < 0) sgSell[i] = sc.High[i] + sc.TickSize * 4;
        }

        // ---- v1.2 DEFECT FIX: surface a pre-loop flatten -----------------
        // Max Daily Loss and Flatten Time flatten the account OUTSIDE this
        // loop, so before v1.2 the position went to zero and no CSV row ever
        // carried action=3 for it. The latch is consumed by the first bar
        // processed after the flatten, whichever call that happens on.
        if (PendStop != 0)
        {
            // v1.4: an ASYNCHRONOUS stop fill - the resting attached stop filled
            // between study calls. Detected before the loop; surfaced here so
            // the CSV carries the exit, exactly like the pre-loop flatten paths.
            action = 3;
            reason = "stop_hit";
            PendStop = 0;
        }
        else if (PendFlat != 0)
        {
            action = 3;
            reason = (PendFlat == 1) ? "max_loss" : "flatten_time";
            PendFlat = 0;
        }

        // ---- diagnostics subgraphs ----
        sgState[i]  = (float)S;
        sgTarget[i] = (float)target;
        sgNoData[i] = noData ? 1.0f : 0.0f;

        PrevTarget  = target;
        lastAction  = DAI_ACTION_STR[action];
        lastReason  = reason;
        lastTarget  = target;
        lastdPTicks = dPTicks;
        lastVetoing = pFilterOn && !noData && (target != S);

        // ================= CSV ROW ===========================================
        // Real-time bars only: a full recalculation replays history through the
        // machine to prime state, and logging it would duplicate rows that were
        // already written when those bars actually closed.
        if (csvExport && realTime)
        {
            SCString rawPath;
            rawPath.Format("%s", in_CSVPath.GetString());
            const SCString path = DAIExpandPath(sc, rawPath, barsToken, sc.BaseDateTimeIn[i]);
            FILE* f = fopen(path.GetChars(), "a");
            if (f == NULL)
            {
                csvWriteFailed = true;
            }
            else
            {
                // Header only when the file is EMPTY. Append mode is deliberate:
                // the series is forward-only, so a rewrite after a restart would
                // destroy every row collected before it. Opening for append puts
                // the position at end-of-file, so ftell == 0 means a new file.
                if (ftell(f) == 0)
                {
                    fprintf(f,
                            "run_chart,run_bars,run_qty,run_entry_source,run_entry_thresh,"
                            "run_flip_test,run_flip_bars,run_flip_sum,run_min_hold,run_warmup,"
                            "run_blind_policy,run_blind_bars,run_disaster,run_sess_start,"
                            "run_sess_end,run_flatten,run_max_trades,run_max_loss,"
                            "run_src_chart,run_src_study,run_src_sg,run_enabled,run_send_service,"
                            "run_price_filter,run_price_mode,run_price_lookback,"
                            "run_price_minmove,run_price_debounce,"
                            "datetime,bar_index,src_index,"
                            "sg1_smoothed,sg2_bidnet,sg3_asknet,sg4_raw,sg5_norm,"
                            "no_data,entry_signal,dP_ticks,P,P_debounced,filter_agrees,"
                            "s_state,target,position,"
                            "action,reason,reconciled,decision_price,fill_price,avg_price,"
                            "day_pnl_pts,open_pnl_pts,blind_count,opp_evidence,warmup_count,"
                            "trades_today\n");
                    CsvHdrDone = 1;
                }

                // v1.2 FROZEN CONFIG COLUMNS. run_entry_source, run_entry_thresh
                // and run_flip_sum describe inputs that no longer exist. They
                // are KEPT so v1.1 and v1.2 exports pool without a column shift
                // and so collect_runs.ps1's required-column list still passes.
                // run_entry_thresh == 0.0 is the unambiguous v1.2 marker: under
                // v1.1 an entry could only fire with entryThr > 0.0, so a v1.1
                // row can never carry 0 here.
                const char* entrySrcStr = "sg1";
                const double entryThr   = 0.0;
                const double flipSum    = 0.0;
                const char* flipTestStr = (flipTestIx == 0) ? "consecutive" : "rolling_sum";
                const char* blindPolStr = (blindPolIx == 0) ? "flatten" : "hold";
                const char* pfModeStr   = (pFMode == PFM_FLAT)  ? "flat_on_disagree"
                                        : (pFMode == PFM_ENTRY) ? "entry_only"
                                                                : "hold_on_disagree";

                // opp_evidence: the consecutive counter under Consecutive, the
                // signed rolling sum under Rolling Sum — always the number the
                // active test actually compared.
                // v1.5: the window must match the one the flip test uses —
                // anchored at the entry / last flip, N VALID bars — or the CSV
                // reports a number no decision was ever made on.
                double oppEvidence = (double)OppCount;
                if (flipTestIx == 1 && haveBar)
                {
                    const int anchorCsv = (LastFlipSrc >= 0) ? LastFlipSrc : 0;
                    double sum = 0.0;
                    int cnt = 0;
                    for (int k = si; k >= anchorCsv && cnt < flipBars; --k)
                    {
                        if (Src1[k] == 0.0 && Src2[k] == 0.0 && Src3[k] == 0.0) continue;
                        sum += Src4[k];
                        cnt++;
                    }
                    oppEvidence = sum;
                }

                const double openPnLPts = (perPoint > 0)
                    ? (Position.OpenProfitLoss / perPoint) : 0.0;

                fprintf(f,
                    "%d,%s,%d,%s,%.4f,"
                    "%s,%d,%.4f,%d,%d,"
                    "%s,%d,%.4f,%d,"
                    "%d,%d,%d,%.4f,"
                    "%d,%d,%d,%d,%d,"
                    "%d,%s,%d,%.4f,%d,"
                    "%s,%d,%d,"
                    "%.4f,%.4f,%.4f,%.4f,%.4f,"
                    "%d,%.4f,%.4f,%d,%d,%d,"
                    "%d,%d,%.0f,"
                    "%s,%s,%d,%.6f,%.6f,%.6f,"
                    "%.4f,%.4f,%d,%.4f,%d,"
                    "%d\n",
                    sc.ChartNumber, barsToken.GetChars(), qty, entrySrcStr, entryThr,
                    flipTestStr, flipBars, flipSum, minHold, warmupReq,
                    blindPolStr, blindBars, stopPts, sessStart,
                    sessEnd, flattenT, maxTrades, maxLoss,
                    ref1.ChartNumber, (int)ref1.StudyID, (int)ref1.SubgraphIndex,
                    in_Enable.GetYesNo(), in_SendSvc.GetYesNo(),
                    pFilterOn ? 1 : 0, pfModeStr, pLookback, pMinMove, pDebounce,
                    DAIFormatDT(sc.BaseDateTimeIn[i]).GetChars(), i, si,
                    v1, v2, v3, v4, v5,
                    noData ? 1 : 0, entrySignal, dPTicks, pRaw, PCur, filterAgrees ? 1 : 0,
                    S, target, Position.PositionQuantity,
                    DAI_ACTION_STR[action], reason.GetChars(), reconciled, decisionPx, fillPx,
                    (double)Position.AveragePrice,
                    dayPnLPts, openPnLPts, BlindCount, oppEvidence, WarmupCnt,
                    TradesToday);

                fclose(f);
            }
        }
    }
    LastProc = lastBar;

    // ===================== status panel ======================================
    if (in_ShowPanel.GetYesNo())
    {
        const char* srcTxt = sourceOK ? "OK" : "*** SOURCE MISSING ***";
        SCString warmTxt;
        if (WarmupCnt >= warmupReq) warmTxt = "ready";
        else warmTxt.Format("%d/%d", WarmupCnt, warmupReq);

        // v1.1 filter line
        SCString filtTxt;
        if (!pFilterOn)
        {
            filtTxt = "Filter: OFF";
        }
        else
        {
            const char* modeShort = (pFMode == PFM_FLAT)  ? "FLAT"
                                  : (pFMode == PFM_ENTRY) ? "ENTRY"
                                                          : "HOLD";
            filtTxt.Format("Filter: %s  P: %+d  dP: %+.1f tk (N=%d D=%.1f)  %s",
                modeShort, PCur, lastdPTicks, pLookback, pMinMove,
                lastVetoing ? "*** VETOING ***" : "passing");
        }

        // v1.4 stop line — what the per-trade stop is doing right now.
        SCString stopTxt;
        if (!(stopPts > 0.0))
        {
            stopTxt = "Stop Per Trade: OFF";
        }
        else
        {
            stopTxt.Format("Stop Per Trade: %.2f pts  |  %s%s  |  re-entry: %s",
                stopPts,
                (StopBlock && !StopArm) ? "*** LOCKED OUT ***" : "armed",
                (StopBlock && !StopArm && StopDir != 0)
                    ? ((StopDir > 0) ? " (was long)" : " (was short)") : "",
                (reEntryIx == 0) ? "signal change" : "next flip");
        }

        SCString txt;
        txt.Format("DOM Always-In: %s%s%s | Src: %s | Warmup: %s\n"
                   "S: %+d  Target: %+d  Pos: %.0f  Blind: %d\n"
                   "%s\n"
                   "%s\n"
                   "Trades today: %d   Day P&L: %+.2f pts   Last: %s (%s)",
            in_Enable.GetYesNo() ? "ARMED" : "OFF",
            in_SendSvc.GetYesNo() ? " [LIVE SERVICE]" : " [local sim]",
            StoppedDay ? " (stopped for day)" : "",
            srcTxt, warmTxt.GetChars(),
            S, lastTarget, Position.PositionQuantity, BlindCount,
            filtTxt.GetChars(),
            stopTxt.GetChars(),
            TradesToday, dayPnLPts,
            lastAction.GetChars(), lastReason.GetChars());

        if (interlockBreach)
            txt.AppendFormat("\n*** TRADING BLOCKED: Hold-on-disagree needs a Stop Per Trade > 0 ***");

        if (csvWriteFailed)
            txt.AppendFormat("\n*** CSV: WRITE FAILED ***");

        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.LineNumber  = 84211077;
        Tool.AddAsUserDrawnDrawing = 0;
        Tool.AddMethod = UTAM_ADD_OR_ADJUST;
        Tool.UseRelativeVerticalValues = 1;
        Tool.BeginDateTime = 3;
        Tool.BeginValue = 90;
        Tool.Color = (!sourceOK || interlockBreach) ? RGB(255, 80, 80)
                   : (in_Enable.GetYesNo() ? RGB(0, 220, 120) : RGB(180, 180, 180));
        Tool.FontSize = 11;
        Tool.FontBold = 1;
        Tool.Text = txt;
        sc.UseTool(Tool);
    }
}
