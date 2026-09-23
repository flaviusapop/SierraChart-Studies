// =============================================================================
// DOMAlwaysInLab.cpp
// Sierra Chart ACSIL Custom Study — DOM Always-In LAB (no orders)
//
// *** KEEP IN SYNC WITH DOMAlwaysInTrader.cpp — ANY CHANGE TO ONE REQUIRES THE
// *** SAME CHANGE TO THE OTHER. The state machine in this file is a byte-for-
// *** behaviour copy of DOMAlwaysInTrader v1.1. If the two ever diverge this
// *** helper is worthless: it would validate a configuration that the trader
// *** does not actually run. Sync list at the bottom of this header.
//
// WHAT THIS IS
// A VISUAL VALIDATION LAYER. It maps the DOMAlwaysInTrader state machine onto
// the chart so its decisions can be checked BY EYE on a chart where DOM
// Pressure v3 has already accumulated hours of depth history. There is no
// backtest for this strategy (depth is forward-only), so "look at the chart and
// see where it would have entered, reversed, gone flat — and where it WANTED to
// act but was blocked, and why" is the only fast feedback loop that exists.
//
// Spec: Trading/Futures_Day_Trading/DOMAlwaysInLab_BuildSpec.md
// Trader spec: Trading/Futures_Day_Trading/DOMAlwaysIn_BuildSpec.md
//
// -----------------------------------------------------------------------------
// NON-NEGOTIABLES (inherited from the trader — do not "clean up" without
// reading DOMAlwaysIn_BuildSpec.md):
//
//  1. DEPTH IS FORWARD-ONLY. DOM Pressure v3 builds its series from live market
//     depth. Its cache starts empty, repaints on a settings change and is wiped
//     by a platform restart. THERE IS NO BACKTEST — Chart Replay cannot produce
//     the signal. Bars that closed BEFORE v3 started running are triple-zero and
//     read as no-data here. EXPECT A DEAD REGION AT THE LEFT OF THE CHART.
//     THAT IS CORRECT BEHAVIOUR, NOT A BUG.
//
//  2. v3 WRITES TO THE FORMING BAR (AutoLoop = 0 / UpdateAlways = 1, writes
//     sc.ArraySize-1 tick by tick). This study reads only CLOSED bars:
//     sc.ArraySize-2, sc.AutoLoop = 0. When the source is on ANOTHER chart the
//     time-mapped source index is additionally clamped to that chart's own last
//     CLOSED bar.
//
//  3. TRIPLE ZERO = NO DATA, NOT "BALANCED BOOK". SG1, SG2 and SG3 all exactly
//     0.0 on the same bar means v3 never wrote that bar. Never treated as a
//     signal, never allowed to reset state, never read as "balanced".
//
//  4. SG SEMANTICS. SG1 = smoothed net. SG2/SG3 = bid/ask net. SG4 = RAW
//     unsmoothed net. SG5 = normalized (SG1 over a strictly positive rolling
//     denominator). Flip evidence is taken from SG4 RAW, never from SG1 (SG1
//     is a moving average: consecutive readings are not independent evidence
//     and they lag by the smoothing period).
//
// -----------------------------------------------------------------------------
// v1.2 — ALL MAGNITUDE THRESHOLDS REMOVED (mirrors the trader exactly)
//
// Entry Threshold, Flip Sum Threshold and Entry Source are GONE. Depth is
// forward-only, so no backtest could ever calibrate a magnitude constant —
// both thresholds were permanent TBD-CALIBRATION guesses, and a guessed
// constant in the entry path is worse than none because it looks like a
// decision. Entry Source went with them as a CONSEQUENCE, not as an
// independent choice: with no threshold the entry test reduces to
// sign(signal) != 0, and sign(SG5) == sign(SG1) identically (SG5 is SG1 over a
// strictly positive denominator), so the input selected between two answers
// that are always the same.
//
//   Entry from flat : valid bar, warmup satisfied, no stop lockout,
//                     inside session, and sign(SG1[i]) != 0  ->  S = that sign
//   Flip test       : Consecutive — N consecutive opposing SG4 bars; one
//                                   agreeing RAW bar resets the count
//                     Rolling Sum — ALL THREE of: N opposing SG4 bars since the
//                                   entry / last flip (not required to be
//                                   consecutive), a full window of N VALID
//                                   source bars that never reaches back past
//                                   that entry / flip, and sign( sum of SG4
//                                   over that window ) == -S    (no-data bars
//                                   skipped, never counted as zeros) — v1.5
//   Min Hold M still gates the flip.
//
// The entire flip decision is now two integers: Flip Bars N and Min Hold M.
//
// *** v1.2 BREAKS SAVED SETTINGS. Three inputs were removed, so every input
// *** index below them shifted. Delete and re-add any existing instance.
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
// which is a copy of this test evaluated against StopDir (anchor = the stop),
// and to the CSV's `opp_evidence` column so it still reports the window the
// decision was actually made on.
//
// NET EFFECT: fewer flips, all of them later. No input changed and no saved
// setting is invalidated. Flip Bars N now means N bars in BOTH modes.
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
// -----------------------------------------------------------------------------
// v1.3 — ALWAYS FULL PASS (recalculation scheduling only; NO logic change)
//
// SYMPTOM: stale arrows, markers and ribbon on historical bars after changing
// DOM Pressure v3's settings.
//
// CAUSE: the source is normally CROSS-CHART. A v3 settings change repaints v3's
// entire cache on ITS chart — every historical SG1/SG4/SG5 value changes — but
// that recalculation does not propagate to this chart. sc.UpdateStartIndex
// therefore never became 0 here, the persistent state was never reset, and the
// loop resumed at LastProc + 1. Every bar below that kept subgraph values
// computed from the PREVIOUS source data. sc.UpdateAlways = 0 made it worse:
// this study was not reliably called at all when the source chart updated.
//
// FIX: hold no incremental state. sc.UpdateAlways = 1; the persistent state
// block is reset on EVERY call; the loop runs bar 0 .. sc.ArraySize-2 on EVERY
// call. Because every bar in range is rewritten every pass, a stale value is
// structurally impossible rather than merely unlikely. The forming bar
// (sc.ArraySize-1) is explicitly cleared each pass, since the loop never
// reaches it but it WAS written when it was the newest closed bar.
//
// The CSV is throttled to compensate — see the throttle comment at the CSV
// run-scope block. A pass over an unchanged bar set does no file I/O at all.
//
// WHAT DID NOT CHANGE: the state machine, in any respect. Same bars, same code,
// same order — only always starting from bar 0. The loop BOUND is still
// sc.ArraySize - 2: closed bars only. Trader/Lab behavioural parity is intact,
// and the trader is deliberately NOT converted to full-pass (it reconciles the
// freshest closed bar by design).
//
// -----------------------------------------------------------------------------
// HOW THIS DIFFERS FROM THE TRADER (and only how)
//
//  A. ZERO ORDER CODE. No sc.SupportReversals, no BuyEntry/SellEntry, no
//     FlattenAndCancelAllOrders, no Enable Trading / Send Orders inputs.
//     "Position" here is the MODEL's own target, tracked internally.
//
//  B. IT EVALUATES THE WHOLE ARRAY. The trader cannot trade the past, so it
//     only ever reconciles the freshest closed bar. The helper can and must
//     analyse the past: as of v1.3 the loop runs from bar 0 on EVERY call
//     through sc.ArraySize - 2, reading v3's repainted cache.
//
//  C. GUARDS THAT NEEDED AN ACCOUNT BECOME "WOULD-HAVE-BLOCKED" LABELS.
//     Max Trades Per Day, Max Daily Loss, Flatten Time, the per-trade stop and
//     the Hold-on-disagree safety interlock are all still evaluated, against
//     the MODELLED position and MODELLED day P&L, and are reported as blocked
//     reasons instead of being enforced with orders.
//
//  D. THE TRADER'S PER-CALL CHECKS BECOME PER-BAR CHECKS. Flatten Time, the
//     daily reset and the daily-loss latch run once per study call in the
//     trader (there is only one live bar to act on) and once per evaluated bar
//     here (there are thousands). Same rule, applied where each study can
//     apply it.
//
//  E. THE MODELLED POSITION REPLACES THE ACCOUNT POSITION. Everywhere the
//     trader reads s_SCPositionData (posDir, AveragePrice, DailyProfitLoss),
//     this study reads its own model. With Enable Trading = No the trader's
//     account is always flat, which is exactly why the trader's own per-trade
//     stop and hold-fallback cannot be measured from its CSV — the Lab exists
//     to close that hole.
//
//  F. CSV IS REWRITE-ON-PASS AND THROTTLED, NOT APPEND-ONLY. The trader appends
//     because its data is forward-only and unregenerable. The Lab regenerates
//     its whole dataset from v3's cache every pass, so appending would
//     duplicate the entire evaluated range every pass. Each {date} file is
//     TRUNCATED once per writing pass and the header written into the empty
//     file. v1.3 throttle: a pass only writes when the newest closed bar has
//     advanced since the last write, or on a full recalculation — so a pass
//     over an unchanged bar set does NO file I/O at all. CSV Export defaults to
//     NO; the trader owns the logging path.
//
// -----------------------------------------------------------------------------
// SYNC LIST — the things that MUST stay identical to DOMAlwaysInTrader.cpp:
//   * Entry from flat: sign(SG1) != 0, no magnitude test (v1.2).
//   * S derivation: sign(SG1) gates, the SG4 RAW flip test supplies evidence
//     (Consecutive count, or — v1.5 — N opposing bars since the entry/flip PLUS
//     the SIGN of the Rolling Sum over a full window of N valid source bars
//     anchored at that entry/flip; v1.2 removed that test's magnitude constant).
//   * The per-trade stop: level, intrabar trigger, precedence over the state
//     machine on the same bar, and its exemption from Min Hold.
//   * Stop re-arm / `Stop Re-Entry`: Signal change (the book must print
//     something other than the stopped-out direction) or Next flip (the full
//     Flip Test must fire against it).
//   * No-data rule: SG1 == 0.0 && SG2 == 0.0 && SG3 == 0.0 (or unreadable
//     source), and its precedence over everything else.
//   * Warmup gate: CONSECUTIVE valid bars, reset to 0 by any no-data bar.
//   * Blind policy: Flatten | Hold after K consecutive no-data bars.
//   * Min hold: (i - lastFlipBar) >= M before a flip is allowed.
//   * Per-trade stop + the stopArm "fresh sub-threshold cross" re-arm.
//   * Price filter P: dP = Close[i] - Close[i-N], D-tick neutral band, debounce
//     in which 0 propagates, all four modes (off / flat / entry / hold), and
//     "Hold means the model's PREVIOUS target, falling back to the position".
//   * The Hold-on-disagree + Stop Per Trade == 0 safety interlock.
//   * Session gate on S, session-start reset.
//   * Input names, defaults and relative order (minus the two order-routing
//     inputs, which do not exist here — see the BuildSpec input map).
//   * CSV column order and meaning (57 shared columns + run_generator).
// =============================================================================

#include "sierrachart.h"
#include <cstdio>
#include <string>
#include <cmath>

SCDLLName("DOM Always-In Lab")

// ---- Subgraph code indices (UI shows these +1) ------------------------------
// Contiguous 0..19. Do not reorder — chartbook settings are index-based.
const int SG_LONG_ENTRY  = 0,  SG_SHORT_ENTRY = 1,  SG_LONG_REV   = 2,
          SG_SHORT_REV   = 3,  SG_EXIT_FLAT   = 4,  SG_RIBBON     = 5,
          SG_EQUITY      = 6,
          SG_BLK_NODATA  = 7,  SG_BLK_WARMUP  = 8,  SG_BLK_MINHOLD= 9,
          SG_BLK_VETOENT = 10, SG_BLK_VETOFLT = 11, SG_BLK_FHOLD  = 12,
          SG_BLK_STOPLK   = 13, SG_BLK_SESSION = 14,
          SG_D_S         = 15, SG_D_TARGET    = 16, SG_D_NODATA   = 17,
          SG_D_P         = 18, SG_D_DP        = 19;

const int LAB_NUM_SG = 20;

// ---- Input code indices -----------------------------------------------------
// Indices 0..25 mirror DOMAlwaysInTrader's inputs 2..27 in the SAME ORDER, with
// the two order-routing inputs (Enable Trading, Send Orders To Trade Service)
// removed because this study cannot trade. Lab-only inputs are APPENDED at the
// end so the shared block stays contiguous and comparable.
// v1.2: Entry Source, Entry Threshold and Flip Sum Threshold REMOVED here too.
// 29 inputs, 0..28, contiguous. Indices 0..22 mirror the trader's inputs 2..24.
const int IN_QTY         = 0,  IN_SOURCE      = 1,  IN_FLIPTEST    = 2,
          IN_FLIPBARS    = 3,  IN_MINHOLD     = 4,  IN_WARMUP      = 5,
          IN_BLINDPOL    = 6,  IN_BLINDBARS   = 7,  IN_STOPPTS    = 8,
          IN_SESSSTART   = 9,  IN_SESSEND     = 10, IN_FLATTEN     = 11,
          IN_MAXTRADES   = 12, IN_MAXLOSS     = 13, IN_CSVEXPORT   = 14,
          IN_CSVPATH     = 15, IN_SHOWPANEL   = 16, IN_RESETSESS   = 17,
          IN_PFILTER     = 18, IN_PFMODE      = 19, IN_PLOOKBACK   = 20,
          IN_PMINMOVE    = 21, IN_PDEBOUNCE   = 22,
          // ---- Lab-only, appended ----
          IN_DISPLAY     = 23, IN_SHOWBLOCKED = 24, IN_CUTBARS     = 25,
          IN_ARROWOFF    = 26, IN_MARKEROFF   = 27, IN_PANELSIZE   = 28,
          // v1.4: APPENDED at the end so no existing index moves. It belongs
          // to the shared block logically (the trader has the same input), but
          // slotting it there would have renumbered every Lab-only input and
          // forced another delete-and-re-add. Index stability wins.
          IN_STOPREENTRY = 29;

// Price Filter Mode ordinals — used by the safety interlock, so they are named.
const int PFM_FLAT = 0, PFM_ENTRY = 1, PFM_HOLD = 2;

// Display Mode ordinals. ACSIL gives a STUDY one chart region, not a region per
// subgraph, so the three-pane layout is three instances of this study (or one
// instance plus Sierra's built-in "Study Subgraph Reference"). See BuildSpec §4.
const int DM_SIGNALS = 0, DM_RIBBON = 1, DM_EQUITY = 2, DM_ALL = 3;

// ---- Persistent state keys --------------------------------------------------
// 1..19 deliberately carry the SAME meaning as DOMAlwaysInTrader's keys so the
// two files read the same when compared side by side. 20+ are Lab-only.
// PS key 1 (PS_LASTPROC in the trader) is RETIRED here as of v1.3: this study
// holds no incremental progress marker, because every call is a full pass.
const int PS_S          = 2;   // standing direction S (+1/-1/0)
const int PS_BLIND      = 3;   // consecutive no-data bars
const int PS_OPP        = 4;   // consecutive opposing SG4 bars (Consecutive test)
const int PS_TRADEDATE  = 5;
const int PS_TRADESTODAY= 6;   // modelled position changes today
const int PS_STOPPEDDAY = 7;
const int PS_WARMUP     = 8;   // consecutive valid bars
const int PS_LASTFLIP   = 9;   // bar index of the last S change
const int PS_STOPBLOCK = 10;  // 1 = 1 = re-entry locked out after a stop fill
const int PS_STOPARM   = 11;  // 1 = signal has cooled below threshold since
const int PS_CSVHDR     = 12;  // 1 = header written this platform session
const int PS_SRCWARNED  = 14;  // 1 = "source missing" already logged
const int PS_PCUR       = 15;  // debounced price direction P (+1/-1/0)
const int PS_PPEND      = 16;  // pending P value awaiting debounce
const int PS_PPENDCNT   = 17;  // bars the pending value has held
const int PS_ILKWARNED  = 18;  // 1 = interlock breach already logged
const int PS_PREVTARGET = 19;  // previous bar's target T (Hold mode memory)
const int PS_STOPDIR   = 20;  // v1.2: direction that was stopped out

// ---- Lab-only accumulators (30+, so the mirrored block above can grow) -------
const int PS_MODELPOS   = 30;  // modelled position direction (+1/0/-1)
const int PS_HOLDSTART  = 31;  // bar index the current non-flat hold began
const int PS_POSCHANGES = 32;  // modelled position changes over the whole range
const int PS_HOLDSUM    = 33;  // sum of completed hold lengths, in bars
const int PS_HOLDCNT    = 34;  // number of completed holds
const int PS_CUTCNT     = 35;  // completed holds shorter than Cut Threshold Bars
const int PS_BARSFLAT   = 36;
const int PS_BARSLONG   = 37;
const int PS_BARSSHORT  = 38;
const int PS_NODATABARS = 39;
const int PS_EVALBARS   = 40;
const int PS_WINS       = 41;
const int PS_LOSSES     = 42;
const int PS_LASTS      = 43;  // panel: S on the last evaluated bar
const int PS_LASTTARGET = 44;  // panel: T on the last evaluated bar
const int PS_LASTAGREE  = 45;  // panel: filter agreement on the last bar
const int PS_LASTACTION = 46;  // panel: action code on the last bar
const int PS_LASTREASON = 47;  // panel: reason code on the last bar
const int PS_CSVLASTBAR = 48;  // v1.3: newest closed bar the CSV was written for
const int PS_STOPOPP    = 49;  // v1.4: "Next flip" re-entry evidence vs StopDir
const int PS_LASTFLIPSRC= 50;  // v1.5: SOURCE bar index of the last S change —
                               //       the anchor the Rolling Sum window cannot
                               //       reach back past (-1 = none / flat)
const int PS_OPPRUN     = 51;  // v1.5: opposing SG4 bars since that anchor, NOT
                               //       reset by a single agreeing RAW bar
const int PS_STOPSRC    = 52;  // v1.5: same anchor for the post-stop lookback
const int PS_STOPRUN    = 53;  // v1.5: opposing SG4 bars vs StopDir since it
const int PS_BLK_BASE   = 60;  // 60..67 — one counter per blocked category

// Persistent doubles
const int PD_EQUITY     = 1;   // cumulative REALIZED points over the range
const int PD_ENTRYPX    = 2;   // modelled average entry price of the open hold
const int PD_MAXADVERSE = 3;   // largest adverse excursion seen, in points (<=0)
const int PD_DAYREAL    = 4;   // realized points on the current trading day
const int PD_LASTDP     = 5;   // panel: dP in ticks on the last evaluated bar

// =============================================================================
// Action codes — same set and same strings as DOMAlwaysInTrader.
// =============================================================================
static const char* const LAB_ACTION_STR[5] =
    { "none", "enter", "reverse", "flatten", "blocked" };

// =============================================================================
// Reason codes. The first 19 are DOMAlwaysInTrader's reason strings verbatim.
// R_MAX_LOSS / R_FLATTEN_TIME / R_INTERLOCK are Lab-only: in the trader those
// three outcomes happen OUTSIDE the closed-bar loop (they are per-call account
// actions), so they never reach its CSV. Here they are per-bar decisions and
// must be nameable. R_INIT matches the trader's panel placeholder.
// =============================================================================
enum LabReason
{
    R_HOLD = 0, R_NO_DATA, R_BLIND_FLATTEN, R_BLIND_HOLD, R_WARMUP,
    R_STOP_LOCKOUT, R_NO_SIGN, R_ENTRY_SIGN, R_AGREE,
    R_TRANSPARENT, R_OPPOSING, R_FLIP, R_FLIP_BLOCKED_MINHOLD,
    R_OUT_OF_SESSION, R_STOP_HIT, R_MAX_TRADES,
    R_FILTER_VETO_ENTRY, R_FILTER_VETO_FLAT, R_FILTER_HOLD,
    R_MAX_LOSS, R_FLATTEN_TIME, R_INTERLOCK, R_STOPPED_DAY, R_INIT,
    R_COUNT
};

static const char* const LAB_REASON_STR[R_COUNT] =
{
    "hold", "no_data", "blind_flatten", "blind_hold", "warmup",
    "stop_lockout", "no_sign", "entry_sign", "agree",
    "transparent", "opposing", "flip", "flip_blocked_minhold",
    "out_of_session", "stop_hit", "max_trades",
    "filter_veto_entry", "filter_veto_flat", "filter_hold",
    "max_loss", "flatten_time", "interlock_blocked", "stopped_for_day", "init"
};

// =============================================================================
// Blocked-marker categories — the "why nothing happened" layer. Exactly the
// eight the chart needs to answer that question at a glance.
// =============================================================================
enum LabBlocked
{
    BM_NODATA = 0, BM_WARMUP, BM_MINHOLD, BM_VETO_ENTRY, BM_VETO_FLAT,
    BM_FILTER_HOLD, BM_STOP_LOCK, BM_SESSION,
    BM_COUNT
};

static const char* const LAB_BLOCKED_STR[BM_COUNT] =
{
    "no-data", "warmup", "min-hold", "filter_veto_entry", "filter_veto_flat",
    "filter_hold", "stop_lockout", "outside_session"
};

// Reason -> blocked marker. -1 = not a blocked state (or not one of the eight).
static int LabBlockedForReason(int reasonCode)
{
    switch (reasonCode)
    {
        case R_NO_DATA:               return BM_NODATA;
        case R_BLIND_HOLD:            return BM_NODATA;
        case R_BLIND_FLATTEN:         return BM_NODATA;
        case R_WARMUP:                return BM_WARMUP;
        case R_FLIP_BLOCKED_MINHOLD:  return BM_MINHOLD;
        case R_FILTER_VETO_ENTRY:     return BM_VETO_ENTRY;
        case R_FILTER_VETO_FLAT:      return BM_VETO_FLAT;
        case R_FILTER_HOLD:           return BM_FILTER_HOLD;
        case R_STOP_LOCKOUT:         return BM_STOP_LOCK;
        case R_OUT_OF_SESSION:        return BM_SESSION;
        default:                      return -1;
    }
}

// =============================================================================
// SCDateTime -> "YYYY-MM-DD HH:MM:SS".
// GetDateTimeYMDHMS is the real combined accessor; GetDate() is a serial day
// count, NOT a packed YYYYMMDD (that mistake produced year 0004 in TriggerLab).
//   C:\SierraChart\ACS_Source\scdatetime.h:1121
// =============================================================================
static SCString LabFormatDT(const SCDateTime& dt)
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
// Ported unchanged from DOMAlwaysInTrader.cpp / TriggerLab.cpp.
// =============================================================================
static SCString LabBarsToken(SCStudyInterfaceRef sc)
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

static std::string LabReplaceAll(std::string s, const std::string& from, const std::string& to)
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
static std::string LabSanitize(const std::string& in)
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
static SCString LabExpandPath(SCStudyInterfaceRef sc, const SCString& rawPath,
                              const SCString& barsToken, const SCDateTime& dt)
{
    std::string path = rawPath.GetChars();

    SCString chartRaw; chartRaw.Format("%d", sc.ChartNumber);

    SCDateTime ncdt = dt;
    int y=0, mo=0, d=0, h=0, mi=0, se=0;
    ncdt.GetDateTimeYMDHMS(y, mo, d, h, mi, se);
    SCString dateRaw; dateRaw.Format("%04d-%02d-%02d", y, mo, d);

    path = LabReplaceAll(path, "{chart}", LabSanitize(chartRaw.GetChars()));
    path = LabReplaceAll(path, "{bars}",  LabSanitize(barsToken.GetChars()));
    path = LabReplaceAll(path, "{date}",  LabSanitize(dateRaw.GetChars()));

    SCString result; result.Format("%s", path.c_str());
    return result;
}

// Session gate. Handles the midnight wrap (start > end -> t>=start || t<=end).
// Identical to DOMAlwaysInTrader::DAIInSession.
static bool LabInSession(int t, int start, int end)
{
    if (start <= end) return t >= start && t <= end;
    return t >= start || t <= end;
}

/*===========================================================================*/
SCSFExport scsf_DOMAlwaysInLab(SCStudyInterfaceRef sc)
{
    SCSubgraphRef sgLongEntry  = sc.Subgraph[SG_LONG_ENTRY];
    SCSubgraphRef sgShortEntry = sc.Subgraph[SG_SHORT_ENTRY];
    SCSubgraphRef sgLongRev    = sc.Subgraph[SG_LONG_REV];
    SCSubgraphRef sgShortRev   = sc.Subgraph[SG_SHORT_REV];
    SCSubgraphRef sgExitFlat   = sc.Subgraph[SG_EXIT_FLAT];
    SCSubgraphRef sgRibbon     = sc.Subgraph[SG_RIBBON];
    SCSubgraphRef sgEquity     = sc.Subgraph[SG_EQUITY];
    SCSubgraphRef sgDS         = sc.Subgraph[SG_D_S];
    SCSubgraphRef sgDTarget    = sc.Subgraph[SG_D_TARGET];
    SCSubgraphRef sgDNoData    = sc.Subgraph[SG_D_NODATA];
    SCSubgraphRef sgDP         = sc.Subgraph[SG_D_P];
    SCSubgraphRef sgDdP        = sc.Subgraph[SG_D_DP];

    SCInputRef in_Qty        = sc.Input[IN_QTY];
    SCInputRef in_Source     = sc.Input[IN_SOURCE];
    SCInputRef in_FlipTest   = sc.Input[IN_FLIPTEST];
    SCInputRef in_FlipBars   = sc.Input[IN_FLIPBARS];
    SCInputRef in_MinHold    = sc.Input[IN_MINHOLD];
    SCInputRef in_Warmup     = sc.Input[IN_WARMUP];
    SCInputRef in_BlindPol   = sc.Input[IN_BLINDPOL];
    SCInputRef in_BlindBars  = sc.Input[IN_BLINDBARS];
    SCInputRef in_StopPts   = sc.Input[IN_STOPPTS];
    SCInputRef in_SessStart  = sc.Input[IN_SESSSTART];
    SCInputRef in_SessEnd    = sc.Input[IN_SESSEND];
    SCInputRef in_Flatten    = sc.Input[IN_FLATTEN];
    SCInputRef in_MaxTrades  = sc.Input[IN_MAXTRADES];
    SCInputRef in_MaxLoss    = sc.Input[IN_MAXLOSS];
    SCInputRef in_CSVExport  = sc.Input[IN_CSVEXPORT];
    SCInputRef in_CSVPath    = sc.Input[IN_CSVPATH];
    SCInputRef in_ShowPanel  = sc.Input[IN_SHOWPANEL];
    SCInputRef in_ResetSess  = sc.Input[IN_RESETSESS];
    SCInputRef in_PFilter    = sc.Input[IN_PFILTER];
    SCInputRef in_PFMode     = sc.Input[IN_PFMODE];
    SCInputRef in_PLookback  = sc.Input[IN_PLOOKBACK];
    SCInputRef in_PMinMove   = sc.Input[IN_PMINMOVE];
    SCInputRef in_PDebounce  = sc.Input[IN_PDEBOUNCE];
    SCInputRef in_Display    = sc.Input[IN_DISPLAY];
    SCInputRef in_ShowBlk    = sc.Input[IN_SHOWBLOCKED];
    SCInputRef in_CutBars    = sc.Input[IN_CUTBARS];
    SCInputRef in_ArrowOff   = sc.Input[IN_ARROWOFF];
    SCInputRef in_MarkerOff  = sc.Input[IN_MARKEROFF];
    SCInputRef in_PanelSize  = sc.Input[IN_PANELSIZE];
    SCInputRef in_StopReEnt  = sc.Input[IN_STOPREENTRY];

    // ===================== DEFAULTS =========================================
    if (sc.SetDefaults)
    {
        sc.GraphName    = "DOM Always-In Lab";
        sc.GraphRegion  = 0;             // price region: arrows sit on the bars
        sc.AutoLoop     = 0;             // manual loop over CLOSED bars only
        // v1.3: UpdateAlways = 1. With UpdateAlways = 0 this study was not
        // reliably called when its CROSS-CHART source updated, so a v3 repaint
        // could sit unreflected on the chart indefinitely. It still reads only
        // CLOSED bars (loop bound sc.ArraySize - 2) — being called every tick
        // does NOT mean reading the forming bar, and that rule is unchanged.
        sc.UpdateAlways = 1;
        sc.ValueFormat  = 2;
        sc.FreeDLL      = 0;
        // NO trading configuration of any kind. This study sends nothing.

        const int arrowW = 3;

        sgLongEntry.Name = "Long Entry";
        sgLongEntry.DrawStyle = DRAWSTYLE_ARROW_UP;
        sgLongEntry.PrimaryColor = RGB(0, 220, 120);
        sgLongEntry.LineWidth = arrowW; sgLongEntry.DrawZeros = false;

        sgShortEntry.Name = "Short Entry";
        sgShortEntry.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sgShortEntry.PrimaryColor = RGB(255, 70, 70);
        sgShortEntry.LineWidth = arrowW; sgShortEntry.DrawZeros = false;

        sgLongRev.Name = "Long Reversal";
        sgLongRev.DrawStyle = DRAWSTYLE_ARROW_UP;
        sgLongRev.PrimaryColor = RGB(0, 150, 255);   // blue = came from a short
        sgLongRev.LineWidth = arrowW; sgLongRev.DrawZeros = false;

        sgShortRev.Name = "Short Reversal";
        sgShortRev.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        sgShortRev.PrimaryColor = RGB(255, 150, 0);  // orange = came from a long
        sgShortRev.LineWidth = arrowW; sgShortRev.DrawZeros = false;

        sgExitFlat.Name = "Exit To Flat";
        sgExitFlat.DrawStyle = DRAWSTYLE_DIAMOND;
        sgExitFlat.PrimaryColor = RGB(240, 240, 90);
        sgExitFlat.LineWidth = 4; sgExitFlat.DrawZeros = false;

        // The highest-value visual: always-in continuity vs flat gaps at a
        // glance. DRAWSTYLE_BAR draws each column from 0 to the value, so the
        // ribbon reads +1 up / -1 down / nothing when flat. Per-bar colour via
        // DataColor[] (the DOMPressureV3 pattern), so AutoColoring stays off.
        sgRibbon.Name = "Position Ribbon";
        sgRibbon.DrawStyle = DRAWSTYLE_BAR;
        sgRibbon.PrimaryColor   = RGB(0, 220, 120);
        sgRibbon.SecondaryColor = RGB(255, 70, 70);
        sgRibbon.LineWidth = 6;
        sgRibbon.DrawZeros = 1;
        sgRibbon.AutoColoring = AUTOCOLOR_NONE;

        sgEquity.Name = "Equity (pts, HYPOTHETICAL)";
        sgEquity.DrawStyle = DRAWSTYLE_LINE;
        sgEquity.PrimaryColor = RGB(220, 220, 220);
        sgEquity.LineWidth = 2; sgEquity.DrawZeros = 1;

        // ---- blocked-state markers: the "why nothing happened" layer --------
        sc.Subgraph[SG_BLK_NODATA].Name = "Blk: no-data";
        sc.Subgraph[SG_BLK_NODATA].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[SG_BLK_NODATA].PrimaryColor = RGB(110, 110, 110);
        sc.Subgraph[SG_BLK_NODATA].LineWidth = 2;
        sc.Subgraph[SG_BLK_NODATA].DrawZeros = false;

        sc.Subgraph[SG_BLK_WARMUP].Name = "Blk: warmup";
        sc.Subgraph[SG_BLK_WARMUP].DrawStyle = DRAWSTYLE_POINT;
        sc.Subgraph[SG_BLK_WARMUP].PrimaryColor = RGB(80, 140, 220);
        sc.Subgraph[SG_BLK_WARMUP].LineWidth = 3;
        sc.Subgraph[SG_BLK_WARMUP].DrawZeros = false;

        sc.Subgraph[SG_BLK_MINHOLD].Name = "Blk: min-hold";
        sc.Subgraph[SG_BLK_MINHOLD].DrawStyle = DRAWSTYLE_SQUARE;
        sc.Subgraph[SG_BLK_MINHOLD].PrimaryColor = RGB(180, 100, 220);
        sc.Subgraph[SG_BLK_MINHOLD].LineWidth = 4;
        sc.Subgraph[SG_BLK_MINHOLD].DrawZeros = false;

        sc.Subgraph[SG_BLK_VETOENT].Name = "Blk: filter veto entry";
        sc.Subgraph[SG_BLK_VETOENT].DrawStyle = DRAWSTYLE_DIAMOND;
        sc.Subgraph[SG_BLK_VETOENT].PrimaryColor = RGB(255, 140, 0);
        sc.Subgraph[SG_BLK_VETOENT].LineWidth = 4;
        sc.Subgraph[SG_BLK_VETOENT].DrawZeros = false;

        sc.Subgraph[SG_BLK_VETOFLT].Name = "Blk: filter veto flat";
        sc.Subgraph[SG_BLK_VETOFLT].DrawStyle = DRAWSTYLE_DIAMOND;
        sc.Subgraph[SG_BLK_VETOFLT].PrimaryColor = RGB(230, 200, 60);
        sc.Subgraph[SG_BLK_VETOFLT].LineWidth = 4;
        sc.Subgraph[SG_BLK_VETOFLT].DrawZeros = false;

        sc.Subgraph[SG_BLK_FHOLD].Name = "Blk: filter hold";
        sc.Subgraph[SG_BLK_FHOLD].DrawStyle = DRAWSTYLE_PLUS;
        sc.Subgraph[SG_BLK_FHOLD].PrimaryColor = RGB(0, 200, 200);
        sc.Subgraph[SG_BLK_FHOLD].LineWidth = 3;
        sc.Subgraph[SG_BLK_FHOLD].DrawZeros = false;

        sc.Subgraph[SG_BLK_STOPLK].Name = "Blk: stop lockout";
        sc.Subgraph[SG_BLK_STOPLK].DrawStyle = DRAWSTYLE_STAR;
        sc.Subgraph[SG_BLK_STOPLK].PrimaryColor = RGB(255, 40, 40);
        sc.Subgraph[SG_BLK_STOPLK].LineWidth = 4;
        sc.Subgraph[SG_BLK_STOPLK].DrawZeros = false;

        sc.Subgraph[SG_BLK_SESSION].Name = "Blk: outside session";
        sc.Subgraph[SG_BLK_SESSION].DrawStyle = DRAWSTYLE_X;
        sc.Subgraph[SG_BLK_SESSION].PrimaryColor = RGB(150, 150, 150);
        sc.Subgraph[SG_BLK_SESSION].LineWidth = 2;
        sc.Subgraph[SG_BLK_SESSION].DrawZeros = false;

        // ---- diagnostics: IGNORE so they can never wreck the price scale ----
        sgDS.Name      = "S state (diag)";      sgDS.DrawStyle      = DRAWSTYLE_IGNORE;
        sgDTarget.Name = "Target (diag)";       sgDTarget.DrawStyle = DRAWSTYLE_IGNORE;
        sgDNoData.Name = "No-Data (diag)";      sgDNoData.DrawStyle = DRAWSTYLE_IGNORE;
        sgDP.Name      = "P debounced (diag)";  sgDP.DrawStyle      = DRAWSTYLE_IGNORE;
        sgDdP.Name     = "dP ticks (diag)";     sgDdP.DrawStyle     = DRAWSTYLE_IGNORE;

        // ================= INPUTS ===========================================
        // Same names, same defaults, same relative order as DOMAlwaysInTrader,
        // minus the two order-routing inputs. A configuration validated here
        // transfers to the trader unchanged.
        in_Qty.Name = "Order Quantity (modelled; scales CSV position only)";
        in_Qty.SetInt(1); in_Qty.SetIntLimits(1, 20);

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

        in_MaxTrades.Name = "Max Trades Per Day (0 = off; modelled as blocked)";
        in_MaxTrades.SetInt(0); in_MaxTrades.SetIntLimits(0, 1000);

        in_MaxLoss.Name = "Max Daily Loss (points; 0 = off; modelled as blocked)";
        in_MaxLoss.SetFloat(0.0f);

        in_CSVExport.Name = "CSV Export";
        in_CSVExport.SetYesNo(0);                     // the TRADER owns logging
        in_CSVPath.Name = "CSV Path  ({chart},{bars},{date} tokens expanded)";
        in_CSVPath.SetString("C:\\SierraChart\\Data\\DOMAlwaysInLab_{chart}_{bars}_{date}.csv");

        in_ShowPanel.Name = "Show status panel";
        in_ShowPanel.SetYesNo(1);

        in_ResetSess.Name = "Reset State At Session Start";
        in_ResetSess.SetYesNo(1);

        in_PFilter.Name = "Price Filter";
        in_PFilter.SetYesNo(0);

        in_PFMode.Name = "Price Filter Mode";
        in_PFMode.SetCustomInputStrings("Flat on disagree;Entry only;Hold on disagree");
        in_PFMode.SetCustomInputIndex(PFM_FLAT);

        in_PLookback.Name = "Price Lookback N (bars)";
        in_PLookback.SetInt(5); in_PLookback.SetIntLimits(1, 500);

        in_PMinMove.Name = "Price Min Move D (ticks; TBD-CALIBRATION)";
        in_PMinMove.SetFloat(4.0f);

        in_PDebounce.Name = "Price Filter Debounce (bars; 1 = none)";
        in_PDebounce.SetInt(1); in_PDebounce.SetIntLimits(1, 100);

        // ---- Lab-only ----
        in_Display.Name = "Display Mode";
        in_Display.SetCustomInputStrings(
            "Signals (price region);Ribbon;Equity;All");
        in_Display.SetCustomInputIndex(DM_SIGNALS);

        in_ShowBlk.Name = "Show Blocked Markers";
        in_ShowBlk.SetYesNo(1);

        in_CutBars.Name = "Cut Threshold Bars (holds shorter than this = a cut)";
        in_CutBars.SetInt(3); in_CutBars.SetIntLimits(1, 500);

        in_ArrowOff.Name = "Arrow Offset (ticks from high/low)";
        in_ArrowOff.SetInt(4); in_ArrowOff.SetIntLimits(0, 200);

        in_MarkerOff.Name = "Blocked Marker Offset (ticks below low)";
        in_MarkerOff.SetInt(10); in_MarkerOff.SetIntLimits(0, 400);

        in_PanelSize.Name = "Panel Text Size";
        in_PanelSize.SetInt(11); in_PanelSize.SetIntLimits(6, 30);

        // v1.4 — shared with the trader; see the stop re-entry arming block.
        in_StopReEnt.Name = "Stop Re-Entry (after Stop Per Trade fills)";
        in_StopReEnt.SetCustomInputStrings("Signal change;Next flip");
        in_StopReEnt.SetCustomInputIndex(0);          // Signal change

        return;
    }

    if (sc.LastCallToFunction)
        return;

    // ===================== persistent state ==================================
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
    int& SrcWarned   = sc.GetPersistentInt(PS_SRCWARNED);
    int& PCur        = sc.GetPersistentInt(PS_PCUR);
    int& PPend       = sc.GetPersistentInt(PS_PPEND);
    int& PPendCnt    = sc.GetPersistentInt(PS_PPENDCNT);
    int& IlkWarned   = sc.GetPersistentInt(PS_ILKWARNED);
    int& PrevTarget  = sc.GetPersistentInt(PS_PREVTARGET);
    int& StopDir    = sc.GetPersistentInt(PS_STOPDIR);
    int& StopOpp    = sc.GetPersistentInt(PS_STOPOPP);
    int& LastFlipSrc = sc.GetPersistentInt(PS_LASTFLIPSRC);
    int& OppRun      = sc.GetPersistentInt(PS_OPPRUN);
    int& StopSrc     = sc.GetPersistentInt(PS_STOPSRC);
    int& StopRun     = sc.GetPersistentInt(PS_STOPRUN);

    int& ModelPos    = sc.GetPersistentInt(PS_MODELPOS);
    int& HoldStart   = sc.GetPersistentInt(PS_HOLDSTART);
    int& PosChanges  = sc.GetPersistentInt(PS_POSCHANGES);
    int& HoldSum     = sc.GetPersistentInt(PS_HOLDSUM);
    int& HoldCnt     = sc.GetPersistentInt(PS_HOLDCNT);
    int& CutCnt      = sc.GetPersistentInt(PS_CUTCNT);
    int& BarsFlat    = sc.GetPersistentInt(PS_BARSFLAT);
    int& BarsLong    = sc.GetPersistentInt(PS_BARSLONG);
    int& BarsShort   = sc.GetPersistentInt(PS_BARSSHORT);
    int& NoDataBars  = sc.GetPersistentInt(PS_NODATABARS);
    int& EvalBars    = sc.GetPersistentInt(PS_EVALBARS);
    int& WinCnt      = sc.GetPersistentInt(PS_WINS);
    int& LossCnt     = sc.GetPersistentInt(PS_LOSSES);
    int& LastS       = sc.GetPersistentInt(PS_LASTS);
    int& LastTarget  = sc.GetPersistentInt(PS_LASTTARGET);
    int& LastAgree   = sc.GetPersistentInt(PS_LASTAGREE);
    int& LastAction  = sc.GetPersistentInt(PS_LASTACTION);
    int& LastReason  = sc.GetPersistentInt(PS_LASTREASON);

    double& Equity     = sc.GetPersistentDouble(PD_EQUITY);
    double& EntryPx    = sc.GetPersistentDouble(PD_ENTRYPX);
    double& MaxAdverse = sc.GetPersistentDouble(PD_MAXADVERSE);
    double& DayReal    = sc.GetPersistentDouble(PD_DAYREAL);
    double& LastDP     = sc.GetPersistentDouble(PD_LASTDP);

    // v1.3: the CSV throttle's memory. It must survive the reset block below —
    // that block now runs on EVERY call, and a reset throttle would rewrite the
    // file every tick, which is the exact I/O this throttle exists to prevent.
    int& CsvLastBar = sc.GetPersistentInt(PS_CSVLASTBAR);

    // =========================================================================
    // v1.3: EVERY CALL IS A FULL PASS. There is no incremental mode.
    //
    // THE BUG THIS FIXES: the source is normally CROSS-CHART. When DOM Pressure
    // v3's settings change it repaints its entire cache on ITS chart — every
    // historical SG1/SG4/SG5 value changes — but that recalculation does not
    // propagate to this chart. sc.UpdateStartIndex therefore never becomes 0
    // here, the persistent state was never reset, and the loop resumed at
    // LastProc + 1. Every bar below that kept the subgraph values written from
    // the PREVIOUS source data: stale arrows, stale markers, stale ribbon.
    //
    // THE FIX: hold no incremental state at all. This study sends no orders and
    // replaying a few hundred to a few thousand bars per call is negligible, so
    // incremental state bought nothing and was the entire source of this bug
    // class. Every bar in range is rewritten on every pass, which makes a stale
    // value structurally impossible rather than merely unlikely.
    //
    // NOTHING ABOUT THE STATE MACHINE CHANGES. This is scheduling only: the
    // same bars go through the same code in the same order, just always from
    // bar 0. Trader/Lab behavioural parity is unaffected.
    // =========================================================================
    {
        S = 0; BlindCount = 0; OppCount = 0; WarmupCnt = 0; LastFlipBar = -1;
        StopBlock = 0; StopArm = 1;
        TradeDate = 0; TradesToday = 0; StoppedDay = 0;
        SrcWarned = 0;
        PCur = 0; PPend = 0; PPendCnt = 0; IlkWarned = 0; PrevTarget = 0;
        StopDir = 0; StopOpp = 0;
        LastFlipSrc = -1; OppRun = 0; StopSrc = -1; StopRun = 0;

        ModelPos = 0; HoldStart = -1;
        PosChanges = 0; HoldSum = 0; HoldCnt = 0; CutCnt = 0;
        BarsFlat = 0; BarsLong = 0; BarsShort = 0;
        NoDataBars = 0; EvalBars = 0; WinCnt = 0; LossCnt = 0;
        LastS = 0; LastTarget = 0; LastAgree = 0;
        LastAction = 0; LastReason = R_INIT;
        Equity = 0.0; EntryPx = 0.0; MaxAdverse = 0.0; DayReal = 0.0;
        LastDP = 0.0;
        CsvHdrDone = 0;

        for (int b = 0; b < BM_COUNT; ++b)
            sc.GetPersistentInt(PS_BLK_BASE + b) = 0;
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

    const int    dispMode   = in_Display.GetIndex();
    const int    cutBars    = in_CutBars.GetInt();
    const double arrowOff   = (double)in_ArrowOff.GetInt()  * tickSize;
    const double markerOff  = (double)in_MarkerOff.GetInt() * tickSize;

    const bool showSignals = (dispMode == DM_SIGNALS || dispMode == DM_ALL);
    const bool showRibbon  = (dispMode == DM_RIBBON  || dispMode == DM_ALL);
    const bool showEquity  = (dispMode == DM_EQUITY  || dispMode == DM_ALL);
    const bool showBlocked = showSignals && (in_ShowBlk.GetYesNo() != 0);

    // ACSIL gives a STUDY one chart region, never a region per subgraph, so
    // "own region" is expressed as a Display Mode: put one instance in the
    // price region for signals and further instances in their own regions for
    // the ribbon and the equity curve. Draw styles are set per call because the
    // mode is a runtime input.
    sgLongEntry.DrawStyle  = showSignals ? DRAWSTYLE_ARROW_UP   : DRAWSTYLE_IGNORE;
    sgShortEntry.DrawStyle = showSignals ? DRAWSTYLE_ARROW_DOWN : DRAWSTYLE_IGNORE;
    sgLongRev.DrawStyle    = showSignals ? DRAWSTYLE_ARROW_UP   : DRAWSTYLE_IGNORE;
    sgShortRev.DrawStyle   = showSignals ? DRAWSTYLE_ARROW_DOWN : DRAWSTYLE_IGNORE;
    sgExitFlat.DrawStyle   = showSignals ? DRAWSTYLE_DIAMOND    : DRAWSTYLE_IGNORE;
    sgRibbon.DrawStyle     = showRibbon  ? DRAWSTYLE_BAR        : DRAWSTYLE_IGNORE;
    sgEquity.DrawStyle     = showEquity  ? DRAWSTYLE_LINE       : DRAWSTYLE_IGNORE;

    {
        const int blkStyle[BM_COUNT] =
        {
            DRAWSTYLE_DASH, DRAWSTYLE_POINT, DRAWSTYLE_SQUARE, DRAWSTYLE_DIAMOND,
            DRAWSTYLE_DIAMOND, DRAWSTYLE_PLUS, DRAWSTYLE_STAR, DRAWSTYLE_X
        };
        for (int b = 0; b < BM_COUNT; ++b)
            sc.Subgraph[SG_BLK_NODATA + b].DrawStyle =
                showBlocked ? (uint16_t)blkStyle[b] : (uint16_t)DRAWSTYLE_IGNORE;
    }

    const int lastBar = sc.ArraySize - 2;   // last CLOSED bar — never -1
    if (lastBar < 1)
        return;

    // v1.3: clear the FORMING bar on every pass. The loop stops at lastBar, so
    // sc.ArraySize - 1 is never written by it — but that index WAS written when
    // the same bar was the newest closed bar one bar ago, and array indices do
    // not shift. Without this clear, the forming bar can display a marker left
    // over from a previous pass. Every other index in 0..lastBar is rewritten
    // unconditionally inside the loop, so this is the only gap.
    {
        const int f = sc.ArraySize - 1;
        for (int g = 0; g < LAB_NUM_SG; ++g)
            sc.Subgraph[g][f] = 0.0f;
        sc.Subgraph[SG_RIBBON].DataColor[f] = RGB(90, 90, 90);
    }

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
            "DOM Always-In Lab: pressure source array is EMPTY. Point the "
            "'Pressure Source' input at DOM Pressure v3 SG1 'Net Pull/Stack' on a "
            "chart that is open in this chartbook. Every bar will read as no-data "
            "until it returns data.", 1);
        SrcWarned = 1;
    }
    if (sourceOK)
        SrcWarned = 0;

    const bool crossChart = (ref1.ChartNumber != sc.ChartNumber);

    // ===================== SAFETY INTERLOCK ==================================
    // Identical rule to the trader: "Hold on disagree" removes the only exit
    // mechanism an always-in system has (the flip IS the stop), so that mode
    // with Stop Per Trade = 0 has no exit condition at all. The trader blocks
    // ALL trading; the Lab therefore models a permanently flat position and
    // labels every would-be action "interlock_blocked", so the chart shows the
    // same nothing the trader would have done.
    const bool interlockBreach = pFilterOn && (pFMode == PFM_HOLD) && !(stopPts > 0.0);

    if (interlockBreach && !IlkWarned)
    {
        sc.AddMessageToLog(
            "DOM Always-In Lab: MODEL BLOCKED — Price Filter Mode is 'Hold on "
            "disagree' with Stop Per Trade = 0. In the trader this configuration "
            "blocks ALL trading, so the Lab models a permanently flat position. "
            "Set a Stop Per Trade (points) or choose a different Price Filter Mode.", 1);
        IlkWarned = 1;
    }
    if (!interlockBreach)
        IlkWarned = 0;

    // ===================== CSV run-scope state ===============================
    // v1.3 CSV THROTTLE. Every pass now recomputes every bar, so writing the
    // CSV on every pass would rewrite the whole file on every tick. The rule:
    //
    //   write ONLY when the newest closed bar has advanced since the last
    //   write, or when Sierra reports a full recalculation (a settings change,
    //   which changes what the rows say).
    //
    // A pass that recomputes an identical row set for an unchanged bar set does
    // NO FILE I/O AT ALL — no open, no truncate, no header, no close. Because a
    // write is always a complete rewrite of the evaluated range, a throttled
    // pass can never leave the file half-updated: it is either the previous
    // complete snapshot or the new one.
    const SCString barsToken = LabBarsToken(sc);
    bool csvWriteFailed = false;

    const bool csvDue = csvExport &&
                        ((lastBar != CsvLastBar) || (sc.IsFullRecalculation != 0));

    FILE*    csvF = NULL;
    SCString csvOpenPath;

    // ===================== closed-bar loop ===================================
    // THE KEY DIFFERENCE FROM THE TRADER: this loop analyses the whole array.
    // The trader can only reconcile the freshest closed bar because it cannot
    // trade the past; the Lab can and must read v3's repainted cache back to
    // the beginning of the chart.
    //
    // v1.3: bar 0 through lastBar, unconditionally, on every call. No
    // sc.UpdateStartIndex clamp and no last-processed marker — see the reset
    // block above for why. The loop BOUND is unchanged: sc.ArraySize - 2,
    // closed bars only.
    for (int i = 0; i <= lastBar; ++i)
    {
        // ---- trading-day rollover ------------------------------------------
        // The trader does this once per CALL against the newest bar; the Lab
        // does it per BAR because it walks history. Same rule, same reset set.
        const int barDay = sc.GetTradingDayDate(sc.BaseDateTimeIn[i]);
        if (barDay != TradeDate)
        {
            TradeDate   = barDay;
            TradesToday = 0;
            StoppedDay  = 0;
            DayReal     = 0.0;
            if (in_ResetSess.GetYesNo())
            {
                // Never carry a standing direction across the session boundary —
                // the book that produced it is 17 hours stale.
                S = 0; OppCount = 0; BlindCount = 0; WarmupCnt = 0;
                LastFlipBar = -1; StopBlock = 0; StopArm = 1; StopDir = 0;
                StopOpp = 0;
                PCur = 0; PPend = 0; PPendCnt = 0; PrevTarget = 0;
            }
        }

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
        // effectively impossible. Missing source counts as no-data too. Bars
        // that closed before v3 was running are ALL no-data: that is the dead
        // region at the left of the chart, and it is correct.
        const bool noData = (!haveBar) || (v1 == 0.0 && v2 == 0.0 && v3 == 0.0);

        // v1.2: the entry signal IS SG1 — no Entry Source input any more
        // (sign(SG5) == sign(SG1) identically) and no magnitude to compare.
        // Kept as a name because the CSV column `entry_signal` is kept, so
        // v1.1 and v1.2 exports pool without a column shift.
        const double entrySignal = v1;

        // ---- PRICE DIRECTION P (this chart's own closes) ----
        // Net displacement over N bars, NOT consecutive rising closes.
        // dP / pRaw are computed on every bar for REPORTING; the debounce and
        // the filter itself are updated only on valid bars.
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
        int reasonCode = R_HOLD;
        const double decisionPx = sc.Close[i];

        if (noData)
        {
            // Hold position, do NOT touch S or oppEvidence, do NOT advance the
            // flip test. Warmup restarts: warmup means CONSECUTIVE valid bars.
            BlindCount++;
            WarmupCnt = 0;
            reasonCode = R_NO_DATA;

            if (S != 0 && BlindCount >= blindBars)
            {
                if (blindPolIx == 0)     // Flatten
                {
                    S = 0;
                    action = 3;
                    reasonCode = R_BLIND_FLATTEN;
                }
                else                     // Hold
                {
                    reasonCode = R_BLIND_HOLD;
                }
            }
        }
        else
        {
            BlindCount = 0;
            if (WarmupCnt < 1000000) WarmupCnt++;

            // ---- P debounce (valid bars only; a no-data bar never advances it)
            // 0 is allowed to propagate: P == 0 is a real state (inside the
            // neutral band) and Flat-on-disagree treats it as disagreement.
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
                    reasonCode = R_WARMUP;
                }
                else if (StopBlock && !StopArm)
                {
                    reasonCode = R_STOP_LOCKOUT;
                    action = 4;
                }
                else if (s != 0)
                {
                    const int wantDir = s;

                    // Entry-only mode: P gates entries FROM FLAT and nothing
                    // else. P == 0 is no permission. S is left at 0, so the
                    // next bar re-tests — that IS the deferred entry.
                    if (pFilterOn && pFMode == PFM_ENTRY && PCur != wantDir)
                    {
                        action = 4;
                        reasonCode = R_FILTER_VETO_ENTRY;
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
                        reasonCode = R_ENTRY_SIGN;
                    }
                }
                else
                {
                    reasonCode = R_NO_SIGN;
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
                    reasonCode = R_AGREE;
                }
                else if (s == 0)
                {
                    // Transparent bar: no advance, no reset.
                    reasonCode = R_TRANSPARENT;
                }
                else
                {
                    // Opposing. SG1 says the smoothed regime disagrees; the FLIP
                    // TEST itself is measured on SG4 RAW.
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
                        reasonCode = R_FLIP;
                    }
                    else if (flipPass)
                    {
                        reasonCode = R_FLIP_BLOCKED_MINHOLD;
                    }
                    else
                    {
                        reasonCode = R_OPPOSING;
                    }
                }
            }
        }

        // ---- session gate on ENTRIES (exits are never gated) ----
        const int barT = sc.BaseDateTimeIn[i].GetTime();
        const bool inSession = LabInSession(barT, sessStart, sessEnd);
        if (S != 0 && !inSession)
        {
            S = 0;
            if (action == 1 || action == 2) { action = 4; reasonCode = R_OUT_OF_SESSION; }
        }

        int target = S;

        // The trader reads the ACCOUNT here. The Lab reads its own modelled
        // position — which is what the account would hold if the trader were
        // armed and filling. This is the study's one deliberate substitution
        // and it is what makes the per-trade stop and the Hold fallback
        // measurable at all (with Enable Trading = No the trader's account is
        // always flat, so neither ever fires in its CSV).
        const int posDir = ModelPos;

        // ---- FILTER COMPOSITION: S is untouched, only T changes -------------
        const bool filterAgrees = (S != 0) && (PCur == S);
        if (pFilterOn && !noData && S != 0)
        {
            if (pFMode == PFM_FLAT && !filterAgrees)
            {
                target = 0;
                if (action == 0 || action == 1 || action == 2)
                {
                    action = (posDir != 0) ? 3 : 0;
                    reasonCode = R_FILTER_VETO_FLAT;
                }
            }
            else if (pFMode == PFM_HOLD && !filterAgrees)
            {
                // Hold = keep the MODEL's previous target, falling back to the
                // position only when the model has none.
                target = (PrevTarget != 0) ? PrevTarget : posDir;
                if (action == 0 || action == 1 || action == 2)
                {
                    action = 0;
                    reasonCode = R_FILTER_HOLD;
                }
            }
        }

        // ---- PER-TRADE STOP (v1.4: intrabar) --------------------------------
        // v1.3 and earlier tested the CLOSE: (Close - EntryPx) * dir <= -stop.
        // That is wrong for a stop. A 10-point stop can see far more adverse
        // excursion inside a bar and close back inside it, so the stop would
        // never trigger and the modelled risk per trade was unbounded.
        //
        // v1.4 tests the bar's RANGE against a fixed level and FILLS AT THE
        // LEVEL, which is what a resting stop order does:
        //
        //     stopLevel = EntryPx - stop * dir
        //     long  hit when Low[i]  <= stopLevel
        //     short hit when High[i] >= stopLevel
        //
        // PRECEDENCE: the stop wins over anything the state machine wanted on
        // the same bar — it is evaluated after the target is composed and
        // overrides it. Pessimistic, consistent with the barrier-sweep
        // convention in Testing/CLAUDE.md (same-bar ambiguity resolves against
        // the trade).
        //
        // MIN HOLD DOES NOT APPLY. Min Hold M gates FLIPS, and a stop is not a
        // flip. Nothing in this block consults minHold or LastFlipBar, and
        // nothing should: a stop delayed by a hold timer is not a stop.
        //
        // SAME-BAR GUARD: a position opened at THIS bar's close did not exist
        // while this bar's High/Low were printing, so those extremes cannot
        // stop it. posDir is the position held INTO bar i, so a position opened
        // at bar i is already excluded (posDir == 0); HoldStart != i is belt
        // and braces for any future change that opens intrabar.
        bool   stopHit   = false;
        double stopLevel = 0.0;
        if (posDir != 0 && stopPts > 0.0 && HoldStart != i)
        {
            stopLevel = EntryPx - stopPts * (double)posDir;
            const bool touched = (posDir > 0) ? (sc.Low[i]  <= stopLevel)
                                              : (sc.High[i] >= stopLevel);
            if (touched)
            {
                S = 0; target = 0;
                StopBlock = 1; StopArm = 0;
                StopDir = posDir;      // the direction that must cool off
                StopOpp = 0;           // v1.4: fresh evidence for "Next flip"
                StopSrc = si; StopRun = 0;   // v1.5: anchor the lookback here
                LastFlipSrc = -1; OppRun = 0;
                action = 3; reasonCode = R_STOP_HIT; stopHit = true;
            }
        }

        // v1.4: the price every P&L number on this bar is measured at. On a
        // stop bar that is the STOP LEVEL, not the close — equity, day P&L,
        // MaxAdverse, win/loss and the CSV fill_price all use it, so they
        // describe the fill the resting stop would have got.
        const double markPx = stopHit ? stopLevel : (double)sc.Close[i];

        // ---- Flatten Time -----------------------------------------------------
        // Trader: checked on every call so it fires even in dead tape, then
        // latches StoppedDay. Lab: same test, per bar.
        {
            const bool pastFlatten = (sessStart <= flattenT)
                ? (barT >= flattenT)
                : (barT >= flattenT && barT < sessStart);
            if (pastFlatten && !StoppedDay)
            {
                StoppedDay = 1;
                S = 0;
                if (target != 0 || posDir != 0)
                {
                    target = 0;
                    action = 3;
                    reasonCode = R_FLATTEN_TIME;
                }
                else
                {
                    target = 0;
                }
            }
        }

        // ---- Max Daily Loss (would-have-blocked) -----------------------------
        // Trader flattens and latches the day off the ACCOUNT's DailyProfitLoss.
        // Lab uses the modelled day P&L: realized so far today plus the open
        // mark-to-market at this close.
        {
            const double openPtsNow = (posDir != 0) ? (markPx - EntryPx) * posDir : 0.0;
            const double dayPtsNow  = DayReal + openPtsNow;
            if (!StoppedDay && maxLoss > 0.0 && dayPtsNow <= -maxLoss)
            {
                StoppedDay = 1;
                S = 0; target = 0;
                action = 3; reasonCode = R_MAX_LOSS;
            }
        }

        // ---- stopped for the day: suppress any target -------------------------
        // v1.2 DEFECT FIX (was DOMAlwaysInLab.cpp:1213-1217): this only
        // rewrote `action` when it had been 1 or 2, so an `action == 0` bar
        // that happened to be holding a position closed it SILENTLY — no
        // diamond, no action=3 in the CSV. The action correction now lives in
        // one place, below, driven by the transition itself.
        if (StoppedDay && target != 0)
        {
            target = 0;
            // Record WHY the target was overridden. The bar that latched the
            // stop already carries max_loss / flatten_time; every bar after it
            // carries this. The ACTION is still decided by the invariant below.
            reasonCode = R_STOPPED_DAY;
        }

        // ---- Max Trades Per Day (would-have-blocked) --------------------------
        // Mirrors the trader's cap semantics exactly: the cap blocks NEW
        // exposure, it never traps existing exposure — at the cap an open
        // position is flattened, not held. v1.2 FIX: the "blocked while flat"
        // branch left `target` non-zero, so the model took on exposure the
        // trader would never have taken. A blocked entry means flat.
        if (maxTrades > 0 && TradesToday >= maxTrades && target != posDir && target != 0)
        {
            target = 0;
            if (posDir == 0) action = 4;
            reasonCode = R_MAX_TRADES;
        }

        // ---- safety interlock: the trader would have sent nothing at all ------
        // v1.2 DEFECT FIX (was DOMAlwaysInLab.cpp:1231-1236): this explicitly
        // downgraded action 3 -> 4, turning a real exit into a "blocked"
        // marker. The reason still records WHY; the action must record WHAT
        // HAPPENED. Downgrade removed.
        if (interlockBreach)
        {
            target = 0;
            reasonCode = R_INTERLOCK;
        }

        // ---- INVARIANT: the action must describe what the position DID --------
        // Any late guard may force `target` for its own reason. The reason code
        // keeps that reason, but the ACTION is not the guard's to decide: a
        // modelled position going to flat is an EXIT and must draw the exit
        // marker and log action == 3, whatever forced it. Enforcing it once,
        // here, from the transition itself is what makes the whole guard chain
        // safe — including guards added later. Do not re-introduce per-guard
        // action rewriting above.
        if (target != posDir)
        {
            if (target == 0)           action = 3;   // exit to flat
            else if (posDir == 0)      action = 1;   // entry from flat
            else                       action = 2;   // reversal
        }

        // ======================= MODEL POSITION UPDATE =========================
        // Close-to-close, no slippage, no commission — EXCEPT a stop fill, which
        // is modelled at the stop level (v1.4). Entries are always at the close
        // of the decision bar, exactly like the trader's decision_price. This is
        // a SHAPE, not a P&L claim.
        double fillPx = 0.0;
        const double openPtsPre = (posDir != 0) ? (markPx - EntryPx) * posDir : 0.0;
        if (posDir != 0 && openPtsPre < MaxAdverse)
            MaxAdverse = openPtsPre;

        if (target != ModelPos)
        {
            // realize the outgoing hold — at markPx, so a stopped trade
            // realizes exactly -stopPts and never the (possibly better or
            // worse) close of the bar the stop was hit on.
            if (ModelPos != 0)
            {
                const double pts = (markPx - EntryPx) * ModelPos;
                Equity  += pts;
                DayReal += pts;
                if (pts > 0.0)      WinCnt++;
                else if (pts < 0.0) LossCnt++;

                if (HoldStart >= 0)
                {
                    const int held = i - HoldStart;
                    HoldSum += held;
                    HoldCnt++;
                    if (held < cutBars) CutCnt++;
                }
            }

            ModelPos = target;
            PosChanges++;
            TradesToday++;
            // v1.4: a stop exit fills at the stop level, everything else at the
            // decision bar's close.
            fillPx = (stopHit && target == 0) ? stopLevel : decisionPx;

            if (ModelPos != 0) { EntryPx = sc.Close[i]; HoldStart = i; }
            else               { EntryPx = 0.0;         HoldStart = -1; }
        }

        // ======================= DRAWING =======================================
        sgLongEntry[i]  = 0.0f;  sgShortEntry[i] = 0.0f;
        sgLongRev[i]    = 0.0f;  sgShortRev[i]   = 0.0f;
        sgExitFlat[i]   = 0.0f;

        // v1.2: markers are driven by the TRANSITION, not by the action code.
        // The action code can legitimately be 3 on a bar where nothing moved
        // (blind_flatten while already flat, exactly as the trader logs it), and
        // that must not paint an exit diamond. Conversely, no guard can suppress
        // a marker for a transition that really happened. This is the drawing
        // half of the invariant enforced above.
        if (target != posDir)
        {
            if (posDir == 0)          // entry from flat
            {
                if (target > 0)      sgLongEntry[i]  = (float)(sc.Low[i]  - arrowOff);
                else                 sgShortEntry[i] = (float)(sc.High[i] + arrowOff);
            }
            else if (target == 0)     // exit to flat — ALWAYS drawn
            {
                sgExitFlat[i] = (float)(sc.High[i] + arrowOff);
            }
            else                      // reversal
            {
                if (target > 0)      sgLongRev[i]  = (float)(sc.Low[i]  - arrowOff);
                else                 sgShortRev[i] = (float)(sc.High[i] + arrowOff);
            }
        }

        // blocked-state markers — the "why nothing happened" layer
        for (int b = 0; b < BM_COUNT; ++b)
            sc.Subgraph[SG_BLK_NODATA + b][i] = 0.0f;

        const int blkCat = LabBlockedForReason(reasonCode);
        if (blkCat >= 0)
        {
            sc.Subgraph[SG_BLK_NODATA + blkCat][i] = (float)(sc.Low[i] - markerOff);
            sc.GetPersistentInt(PS_BLK_BASE + blkCat) += 1;
        }

        // position ribbon — always-in continuity vs flat gaps at a glance
        sgRibbon[i] = (float)target;
        sgRibbon.DataColor[i] = (target > 0) ? RGB(0, 200, 110)
                              : (target < 0) ? RGB(230, 60, 60)
                                             : RGB(90, 90, 90);

        // hypothetical equity: realized + open mark-to-market at this close
        const double openPtsPost = (ModelPos != 0) ? (sc.Close[i] - EntryPx) * ModelPos : 0.0;
        sgEquity[i] = (float)(Equity + openPtsPost);

        // diagnostics
        sgDS[i]      = (float)S;
        sgDTarget[i] = (float)target;
        sgDNoData[i] = noData ? 1.0f : 0.0f;
        sgDP[i]      = (float)PCur;
        sgDdP[i]     = (float)dPTicks;

        // ---- running statistics ------------------------------------------------
        EvalBars++;
        if (noData) NoDataBars++;
        if (target > 0)      BarsLong++;
        else if (target < 0) BarsShort++;
        else                 BarsFlat++;

        PrevTarget = target;
        LastS      = S;
        LastTarget = target;
        LastAgree  = filterAgrees ? 1 : 0;
        LastAction = action;
        LastReason = reasonCode;
        LastDP     = dPTicks;

        // ================= CSV ROW =============================================
        // Same 57 columns as DOMAlwaysInTrader, in the same order, plus a final
        // run_generator column so pooled files can tell lab rows from trader
        // rows. Positions 1..57 are unchanged, so a positional reader still
        // works. Grouping is by run_* columns per Testing/CLAUDE.md.
        if (csvDue)
        {
            SCString rawPath;
            rawPath.Format("%s", in_CSVPath.GetString());
            const SCString path = LabExpandPath(sc, rawPath, barsToken, sc.BaseDateTimeIn[i]);

            if (csvF == NULL || csvOpenPath != path)
            {
                if (csvF != NULL) { fclose(csvF); csvF = NULL; }
                // TRUNCATE on open, always. A pass regenerates the ENTIRE
                // evaluated range, so appending would add a duplicate copy of
                // the whole dataset every pass. Bar times are monotonic, so a
                // given {date} path is opened exactly ONCE per pass and can
                // never be truncated twice — which is why v1.2's
                // "first-open-of-the-pass truncates, the rest append" rule was
                // wrong for a multi-day array: day 2's file appended to what a
                // previous pass had already written, and grew without bound
                // once v1.3 made passes frequent. Only a pass that cleared the
                // v1.3 throttle ever gets here. (The trader is append-only
                // because its data is forward-only and unregenerable; the
                // Lab's is neither.)
                csvF = fopen(path.GetChars(), "w");
                csvOpenPath = path;
                if (csvF == NULL)
                    csvWriteFailed = true;
            }

            if (csvF != NULL)
            {
                if (ftell(csvF) == 0)
                {
                    fprintf(csvF,
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
                            "trades_today,run_generator\n");
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

                fprintf(csvF,
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
                    "%d,%s\n",
                    sc.ChartNumber, barsToken.GetChars(), qty, entrySrcStr, entryThr,
                    flipTestStr, flipBars, flipSum, minHold, warmupReq,
                    blindPolStr, blindBars, stopPts, sessStart,
                    sessEnd, flattenT, maxTrades, maxLoss,
                    ref1.ChartNumber, (int)ref1.StudyID, (int)ref1.SubgraphIndex,
                    0, 0,
                    pFilterOn ? 1 : 0, pfModeStr, pLookback, pMinMove, pDebounce,
                    LabFormatDT(sc.BaseDateTimeIn[i]).GetChars(), i, si,
                    v1, v2, v3, v4, v5,
                    noData ? 1 : 0, entrySignal, dPTicks, pRaw, PCur, filterAgrees ? 1 : 0,
                    S, target, (double)(ModelPos * qty),
                    LAB_ACTION_STR[action], LAB_REASON_STR[reasonCode], 0,
                    decisionPx, fillPx, EntryPx,
                    DayReal + openPtsPost, openPtsPost, BlindCount, oppEvidence, WarmupCnt,
                    TradesToday, "lab");
            }
        }

    }

    if (csvF != NULL)
        fclose(csvF);

    // v1.3: remember what the CSV was written for, so the next pass over an
    // unchanged bar set performs no file I/O. Only advanced on a write that
    // actually opened a file — a failed open must be retried, not swallowed.
    if (csvDue && !csvWriteFailed)
        CsvLastBar = lastBar;

    // ===================== status panel ======================================
    if (in_ShowPanel.GetYesNo())
    {
        const char* srcTxt = sourceOK ? "OK" : "*** SOURCE MISSING ***";

        const double evalD    = (EvalBars > 0) ? (double)EvalBars : 1.0;
        const double avgHold  = (HoldCnt > 0) ? ((double)HoldSum / (double)HoldCnt) : 0.0;
        const double cutPct   = (HoldCnt > 0) ? (100.0 * (double)CutCnt / (double)HoldCnt) : 0.0;
        const double pctFlat  = 100.0 * (double)BarsFlat  / evalD;
        const double pctLong  = 100.0 * (double)BarsLong  / evalD;
        const double pctShort = 100.0 * (double)BarsShort / evalD;
        const double pctND    = 100.0 * (double)NoDataBars / evalD;

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
            filtTxt.Format("Filter: %s  P: %+d  dP: %+.1f tk (N=%d D=%.1f)  agree: %s",
                modeShort, PCur, LastDP, pLookback, pMinMove,
                LastAgree ? "YES" : "no");
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

        SCString blkTxt = "Blocked:";
        for (int b = 0; b < BM_COUNT; ++b)
            blkTxt.AppendFormat("  %s %d", LAB_BLOCKED_STR[b],
                                sc.GetPersistentInt(PS_BLK_BASE + b));

        SCString txt;
        txt.Format(
            "DOM Always-In LAB (no orders) | Src: %s | Evaluated bars: %d\n"
            "Position changes: %d   Avg hold: %.1f bars   Holds < %d bars: %d of %d (%.0f%%)\n"
            "Bars  flat %d (%.0f%%)   long %d (%.0f%%)   short %d (%.0f%%)   no-data %d (%.0f%%)\n"
            "Gross %+.2f pts (HYPOTHETICAL, close-to-close, no slippage/commission)"
            "   W/L %d/%d   Max adverse %.2f pts\n"
            "S: %+d  Target: %+d  ModelPos: %+d  Blind: %d  Warmup: %d/%d\n"
            "%s\n"
            "%s\n"
            "Last: %s (%s)\n"
            "%s",
            srcTxt, EvalBars,
            PosChanges, avgHold, cutBars, CutCnt, HoldCnt, cutPct,
            BarsFlat, pctFlat, BarsLong, pctLong, BarsShort, pctShort, NoDataBars, pctND,
            Equity, WinCnt, LossCnt, MaxAdverse,
            LastS, LastTarget, ModelPos, BlindCount, WarmupCnt, warmupReq,
            filtTxt.GetChars(),
            stopTxt.GetChars(),
            LAB_ACTION_STR[(LastAction >= 0 && LastAction < 5) ? LastAction : 0],
            LAB_REASON_STR[(LastReason >= 0 && LastReason < R_COUNT) ? LastReason : 0],
            blkTxt.GetChars());

        if (interlockBreach)
            txt.AppendFormat("\n*** MODEL BLOCKED: Hold-on-disagree needs a Stop Per Trade > 0 "
                             "(the trader would send NOTHING in this configuration) ***");

        if (csvWriteFailed)
            txt.AppendFormat("\n*** CSV: WRITE FAILED ***");

        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_TEXT;
        Tool.LineNumber  = 84211078;   // distinct from the trader's 84211077
        Tool.AddAsUserDrawnDrawing = 0;
        Tool.AddMethod = UTAM_ADD_OR_ADJUST;
        Tool.UseRelativeVerticalValues = 1;
        Tool.BeginDateTime = 3;
        Tool.BeginValue = 90;
        Tool.Color = (!sourceOK || interlockBreach) ? RGB(255, 80, 80) : RGB(200, 200, 200);
        Tool.FontSize = in_PanelSize.GetInt();
        Tool.FontBold = 1;
        Tool.Text = txt;
        sc.UseTool(Tool);
    }
}
