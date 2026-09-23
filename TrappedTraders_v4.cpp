// TrappedTraders_v4.cpp — Sierra Chart ACSIL custom study.
// Full description, input map and v3/v4 change lists are below the SCDLLName line.
//
// [v3-C8] SCDLLName MUST STAY IN THE FIRST FEW LINES OF THIS FILE.
// "Build Custom Studies DLL" scans only the top of each selected source file
// for it and refuses the whole build with "None of the selected source files
// contains the SCDLLName() line at the top of the file" if it is further down.
// This is a single-file delivery whose embedded decision core is ~2300 lines,
// so the old placement after the core (v2 delivery: line 2451) put it far past
// that window and the file could not be built at all — the studies that did
// compile all carry it inside the first ~140 lines. Do not move it back down,
// and do not let the header comment grow above it.
#include "sierrachart.h"

SCDLLName("TrappedTraders_v4")

// =============================================================================
// Single-file Sierra delivery: decision core is embedded below.
// Only the standard SierraChart ACS_Source headers are required.
// Portable verification is not a native Sierra DLL build.
// =============================================================================
// TrappedTraders_v4.cpp  —  v4 (v3 + cross-chart zone merge)
//
// WHAT CHANGED IN v4 (2026-09-22)
//   M1  Cross-chart zone merge.  One instance per timeframe in the same
//       chartbook; the instance set to Zone Display = Merged (In:37) reads
//       up to six others (In:40..45) and draws ONE zone per group of
//       overlapping same-direction zones.  See CROSS-CHART MERGE below.
//       Everything else is v3 unchanged.  v4 has its own DLL name, study
//       function and drawing line-number range (310000+), so v3 and v4 can
//       run on the same chart.
//   M2  Several v4 instances on ONE chart.  Drawing line numbers are now a
//       per-instance block keyed by study ID (Sierra's line numbers are
//       chart-wide, so two instances used to overwrite and delete each other's
//       zones).  A Merged instance can take another instance on its own chart
//       as a Merge Source; labels then carry the mode, e.g. "5m-M+5m-A+15m".
//   M3  Stacked zones inside ONE study fixed.  A candidate merged into the
//       first zone it overlapped and widened it, but the widened zone was
//       never merged with other zones it now overlapped, so zones stacked on
//       top of each other.  Overlapping same-source, same-direction zones are
//       now consolidated after every merge (older zone survives).  Absorbed
//       records export as MERGED in the CSV state column.
//   M4  Lock Zones (In:46, default Yes): every zone rectangle, label and
//       status text is drawn with LockDrawing = 1, so it cannot be dragged or
//       edited, while staying a user drawing that Copy chart drawings copies.
//   M5  Erased zones (Show Erased Zones = Yes) get their own colour, light
//       grey by default (In:47..48), and expire after Keep Erased Zones
//       trading days (In:49, default 3, 0 = forever), counting the day they
//       were erased: 1 = erase day only.  Applies to merged zones too.
//
// v3 (Adaptive Thresholds + Decision Map, hardened)
// Sierra Chart ACSIL Custom Study — Trapped Traders Zones
//
// WHAT CHANGED IN v3 (behaviour of a correctly configured chart is unchanged;
// every item below is either a fix for a wrong result or a fix for work that
// grew with chart length, or — C8 — a reason the file could not be built at
// all).  Detail at each site, tagged [v3-C*] / [v3-P*].
//
//   CORRECTNESS
//   C1  Decision drawings owned by this instance are now DELETED on a decision
//       rebuild instead of having their IDs dropped.  A decision rebuild is not
//       always a Sierra full recalculation (toggling In:32 changes only the
//       decision fingerprint), so dropping the IDs orphaned chart objects that
//       nothing could ever remove.
//   C2  TtdProcessBar's terminal-episode repair could spin forever if the
//       active-slot retire ever failed to find its entry; it now only rescans
//       the slot when the active count actually shrank.
//   C3  PASS 1.5 (next-bar-open confirmation) now goes through the same
//       TtdLegacyNextBarStep helper as PASS 1, so the same-bar close-through
//       fix applies at BOTH confirmation sites instead of only one.
//   C4  Zone Border Width and Transparency Level are clamped and now carry
//       input limits.  Border width reached Sierra as (uint16_t)width, so a
//       zero or negative value became an enormous line width.
//   C5  The display watermarks (P_DMAXN / P_DDETL) are latched even when the
//       decision layer has halted.  While halted they never advanced, so the
//       visual-refresh test fired on every intrabar tick and re-ran the whole
//       renderer forever.
//   C6  A baseline day rejected for thin samples is now recorded in FailDayIdx,
//       so the "not ready" status names the day instead of reporting -1.
//   C7  TtdCsvField never emits an unterminated quoted field: it drops trailing
//       characters rather than the closing quote.
//   C8  SCDLLName moved to the top of the file. Sierra's "Build Custom Studies
//       DLL" scans only the top of a source file for it, so this single-file
//       delivery — whose SCDLLName sat behind ~2300 lines of embedded decision
//       core — was rejected outright with "None of the selected source files
//       contains the SCDLLName() line at the top of the file". The v2 delivery
//       file has the same defect and has therefore never been built; the v2
//       DLL in service was compiled from TrappedTraders_v2.cpp, which carries
//       SCDLLName at line 138.
//
//   PERFORMANCE (all four were super-linear in chart length)
//   P1  PASS 3D published SG7..SG19 with TtdSnapshotAt per availability bar,
//       which is O(episodes x transitions) each — so O(bars x episodes x
//       transitions) across a rebuild.  Replaced by a single forward sweep of
//       the ledger (TtdSweep*), which produces identical values in
//       O(bars + transitions + selected work).
//   P2  PASS 1's per-window loops walked EVERY zone record, including rejected
//       and (with CSV export on) retained ones that are pruned only after the
//       loop — O(zones-ever) per bar.  They now walk live / shadow-pending
//       index lists.
//   P3  PASS 2 re-scanned every bar from DetectedBar+1 on a full recalculation,
//       repeating work PASS 1's inline lifecycle had already done.  Each zone
//       now carries CheckedTo and PASS 2 starts after it.
//   P4  With the Decision Map OFF (the default) the inactive branch rewrote 13
//       subgraphs across the WHOLE array on every bar close.  That clear is now
//       latched and only the newly appended tail is written.
//
// v3 uses its own DLL name, study function and drawing line-number range, so it
// can be loaded alongside v1/v2 on the same chart for comparison.
//
// CONCEPT:
//   Detects price extremes where aggressive participants accumulated large
//   directional delta within a rolling window of closed bars but failed to
//   continue — trapping them on the wrong side of the market.
//
//   Delta is AGGREGATED across the lookback window at each price level,
//   then scanned for qualifying clusters near the window's price extreme.
//
//   SELL ZONE  — positive delta cluster near window HIGH
//                Buyers pushed to the top; price rejected -> trapped longs
//                -> resistance zone, expect downward reaction
//
//   LONG ZONE  — negative delta cluster near window LOW
//                Sellers pushed to the bottom; price rejected -> trapped shorts
//                -> support zone, expect upward reaction
//
// ZONE LIFECYCLE (unchanged from v1):
//   1. DETECTED on bar close when delta thresholds are met.
//   2. CONFIRMED on the NEXT bar close:
//        Sell zone -> next bar must NOT open above TopPrice
//        Long zone -> next bar must NOT open below BottomPrice
//      Discarded zones are never drawn, even in Show Erased Zones mode.
//   3. ERASED once confirmed, when a CLOSED bar closes fully through it:
//        Sell zone -> close above TopPrice
//        Long zone -> close below BottomPrice
//   Overlapping zones of the same source AND same direction merge.
//
// v2 — THRESHOLD MODE (In:14):
//   0 = Manual  : v1 behaviour, uses In:3 / In:4.
//   1 = Auto    : both thresholds calculated from the previous N completed
//                 trading days of THIS chart. Only auto-coloured zones drawn.
//   2 = Both    : manual and auto detection run independently on the same
//                 closed-bar windows. Manual has deterministic precedence —
//                 an auto candidate intersecting a live manual zone is
//                 suppressed, so the two sources can never draw overlapping
//                 rectangles.
//
// AUTO BASELINE (see TrappedTraders_v2_BuildSpec.md §3):
//   Per completed trading day, every rolling window contributes
//     - block-delta samples : |delta| of every directionally valid level
//                             inside the proximity band
//     - zone-total samples  : the summed directionally valid magnitude of a
//                             band holding >= Number of Blocks Per Zone levels
//   No threshold is applied when collecting samples.
//   Each day yields one block percentile (In:16) and one zone percentile
//   (In:17).  The thresholds active on day k are the MEDIAN of those daily
//   values over days k-N .. k-1 — median of per-day percentiles, never a
//   pooled sample set, so every day carries equal weight.
//   Percentile rule: linear interpolation between closest ranks (R-7 /
//   Excel PERCENTILE.INC).  Median = P50 under the same rule.
//   Thresholds are constant for a whole trading day and are cached per day.
//   Without N complete preceding days no thresholds are produced — the study
//   warns on the chart instead of fabricating a fallback.
//
// CROSS-CHART DISPLAY:
//   Zones are created as user drawings with AllowCopyToOtherCharts = 1.
//   Target chart: Chart Settings -> Chart Drawings ->
//   "Copy chart drawings from chart #'s" -> this chart's number.
//
// INPUTS:
//   In:1  Numbers Bars Lookback         bars in rolling aggregation window
//   In:2  Number of Blocks Per Zone     min qualifying price levels in cluster
//   In:3  Minimum Single Block Delta    min aggregated |delta| per price level
//   In:4  Zone Delta Threshold          min total aggregated |delta| for zone
//   In:5  Proximity Tolerance (blocks)  max distance from window extreme
//   In:6  Sell Zone Fill Color
//   In:7  Sell Zone Border Color
//   In:8  Long Zone Fill Color
//   In:9  Long Zone Border Color
//   In:10 Zone Border Width
//   In:11 Transparency Level
//   In:12 Show Delta Info
//   In:13 Show Erased Zones
//   In:14 Threshold Mode (0=Manual, 1=Auto, 2=Both)      [v2]
//   In:15 Auto Lookback Trading Days                     [v2]
//   In:16 Auto Block Delta Percentile                    [v2]
//   In:17 Auto Zone Delta Percentile                     [v2]
//   In:18 Auto Sell Zone Fill Color                      [v2]
//   In:19 Auto Sell Zone Border Color                    [v2]
//   In:20 Auto Long Zone Fill Color                      [v2]
//   In:21 Auto Long Zone Border Color                    [v2]
//   In:22 Show Active Auto Thresholds                    [v2]
//   In:23 Auto Session Filter (Both / RTH only / ETH only) [v2]
//   In:24 RTH Start Time                                 [v2]
//   In:25 RTH End Time                                   [v2]
//   In:26 Zone Confirmation (Displacement / Next bar open) [v2]
//   In:27 Confirmation Displacement (ticks)              [v2]
//   In:28 Confirmation Deadline (bars, 0 = none)         [v2]
//   In:29 Export Zones To CSV                            [v2]
//   In:30 CSV Export Folder                              [v2]
//   In:31 CSV Export Filename                            [v2]
//   In:32..36 Decision Map                               [v2.1]
//   In:37 Zone Display (own / hidden / merged)           [v4-M1]
//   In:38 Merge Bounds (Union / Overlap)                 [v4-M1]
//   In:39 Merge Gap Tolerance (ticks)                    [v4-M1]
//   In:40..45 Merge Source 1..6 (chart + study picker)   [v4-M1]
//   In:46 Lock Zones (block moving/editing)              [v4-M4]
//   In:47 Erased Zone Fill Color (light grey)            [v4-M5]
//   In:48 Erased Zone Border Color                       [v4-M5]
//   In:49 Keep Erased Zones (sessions, 0 = forever)      [v4-M5]
//
// CROSS-CHART MERGE [v4-M1]:
//   Run one instance per timeframe in the same chartbook (same symbol).  Set
//   the source instances to Zone Display = Hidden and the display chart to
//   Merged, picking the sources in In:40..45.  Same-direction zones from all
//   of them that overlap in price (within In:39 ticks) and in time are drawn
//   as ONE rectangle, labelled with the contributing timeframes.  Manual and
//   auto zones merge together.  A merged zone shrinks to its live members as
//   they are erased and ends only when every member is erased.  Turn OFF
//   "Copy chart drawings from chart #'s" between these charts, or the
//   sources' old per-chart rectangles are copied back in.  Detail and rules
//   at the "Cross-chart zone merge" section.
//
// DUAL-MODE CONFIRMATION TRACKING:
//   In:26 selects which confirmation rule DRIVES the zone state machine, but
//   BOTH rules are evaluated for EVERY zone on every bar and their verdicts are
//   recorded on the record (NB_* / DP_*).  The shadow verdicts never touch zone
//   geometry, merging, validity or drawing — they exist so the two rules can be
//   compared over the SAME zone population, which is only meaningful if neither
//   rule is allowed to change the population the other one observes.
//
//   Turning In:29 on additionally RETAINS rejected and erased zone records
//   instead of pruning them, so the CSV describes every zone ever detected
//   rather than the survivors of whichever rule In:26 selected.  Retained
//   records are inert — Slot = -1 and every detection site tests Valid first —
//   but they do make the O(zones) loops longer, which is why the retention is
//   tied to the export input rather than being permanent.
//
// SUBGRAPHS (DRAWSTYLE_IGNORE — data outputs only):
//   SG1  Sell Zone Delta                 (unchanged)
//   SG2  Buy Zone Delta                  (unchanged)
//   SG3  Active Auto Block Threshold     [v2] per bar, per trading day
//   SG4  Active Auto Zone Threshold      [v2] per bar, per trading day
//   SG5  Auto Baseline Ready (1/0)       [v2] per bar
//   SG6  Zone Source Pulse (1=man,2=auto)[v2] at a confirmed zone DetectedBar
//
// REQUIREMENTS:
//   Numbers Bar / Footprint chart with sc.MaintainVolumeAtPriceData = 1
//
// AutoLoop: 0 (manual loop for performance)
// =============================================================================

// sierrachart.h is included at the very top of this file, above SCDLLName.
// === BEGIN INLINED TRAPPED TRADERS DECISION CORE ===
// =============================================================================
// TrappedTraders_v2_DecisionCore.h — Decision Map pure core (Phase 1)
//
// Portable, dependency-free episode engine used by BOTH the Sierra study
// (TrappedTraders_v2.cpp includes this header) and the Linux tests
// (tests/test_trapped_traders_decision*.cpp include this same file).
// No Sierra headers, no STL, no file I/O, no static mutable state.
//
// BuildSpec: TrappedTraders_v2_Decision_BuildSpec.md §§3-5.
//   Direction: +1 = seller failure / buy zone; -1 = buyer failure / sell zone.
//   A candidate discovered on bar d can never confirm/discard using bar d;
//   evaluation starts on d+1 (spec §4).
//
// Capacity (spec §3): TTD_MAX_EPISODES active/retained episode slots and
// TTD_MAX_TRANSITIONS transition records. Overridable at compile time
// (e.g. -DTTD_MAX_EPISODES=8 for deterministic capacity tests) after
// measured sizeof. Never silently evict: episode exhaustion stops admission
// (capacityLimited); transition exhaustion halts the layer (halted).
// =============================================================================
#ifndef TRAPPED_TRADERS_V2_DECISION_CORE_H
#define TRAPPED_TRADERS_V2_DECISION_CORE_H

#include <string.h> // memcpy for exact float-bit hashing
#include <math.h>   // floor for price-to-tick conversion
#include <stdio.h>  // snprintf for the Task 10 detail text

#ifndef TTD_MAX_EPISODES
#define TTD_MAX_EPISODES 4096
#endif
#ifndef TTD_MAX_TRANSITIONS
#define TTD_MAX_TRANSITIONS 32768
#endif

// Episode states (spec §4).
#define TTD_PENDING 0
#define TTD_FAILURE_CONFIRMED 1
#define TTD_RETURN_UNRESOLVED 2
#define TTD_RETURN_REJECTED 3
#define TTD_CROSSED_THROUGH 4
#define TTD_DISCARDED 5

// Candidate sources.
#define TTD_SRC_MANUAL 1
#define TTD_SRC_AUTO 2

// Transition kinds (append-only ledger; snapshots, never overwritten).
#define TTD_TR_FAILURE_CONFIRMED 1
#define TTD_TR_FIRST_RETURN 2
#define TTD_TR_RETURN_STARTED 3
#define TTD_TR_RETURN_REJECTED 4
#define TTD_TR_CROSSED 5
#define TTD_TR_DISCARDED 6

// API results.
#define TTD_OK 0
#define TTD_OK_ASSOCIATED 1
#define TTD_OK_SUPPRESSED 2
#define TTD_ERR_CAPACITY 3
#define TTD_ERR_HALTED 4
#define TTD_ERR_ARG 5

struct TtdCandidate
{
    int dir;            // +1 buy zone / -1 sell zone
    int d;              // discovery (candidate) bar index
    int bottomTick;     // frozen inclusive geometry, chart ticks
    int topTick;
    double origDelta;   // original block evidence from SAME candidate
    int origBlocks;
    int source;         // TTD_SRC_MANUAL | TTD_SRC_AUTO
    int displTicks;     // frozen In[26] displacement distance
    int deadlineBars;   // 0 = none
    float threshBlock;  // baseline block threshold ACTUALLY used (frozen)
    float threshZone;   // baseline zone threshold ACTUALLY used (frozen)
};

struct TtdBar
{
    int j;              // closed bar index being processed
    int closeTick;
    int highTick;
    int lowTick;
    double barStartSec; // bar-start timestamp (seconds); provenance for spans
    double barEndSec;   // bar-end timestamp (seconds)
    int spanValid;      // 1 when timestamps are usable, else missing
    const int* vapPrices;   // per-price VAP ticks within this bar (may be null)
    const double* vapAsk;   // in-anchor AskVolume per price (double, fractional)
    const double* vapBid;
    int vapN;
    int vapMissing;     // 1 when VAP unavailable for this bar
};

// Live per-return measurements. A TtdTransition carries an immutable copy
// taken at emit time; later bars never rewrite earlier transition evidence.
struct TtdReturnMeas
{
    int ordinal;        // return ordinal (0 = no return yet)
    int startBar;       // first touching bar
    double askSum;      // in-zone volume, return bars (AskVolume, double)
    double bidSum;      // in-zone volume, return bars (BidVolume, double)
    int barCount;       // bars accumulated from first touch through latest
    int maxHighTick;    // penetration tracking (sell side)
    int minLowTick;     // penetration tracking (buy side)
    int penetrationTicks;
    int minLaterClose;  // later-close move-away tracking (sell side)
    int maxLaterClose;  // later-close move-away tracking (buy side)
    int hasLaterClose;  // 0 until a bar after the touch bar is observed
    int moveAwayTicks;
    double spanSec;     // return_span_bar_seconds (bar-based, NOT dwell time)
    int spanValid;      // 0 = missing (negative/invalid, never clamped)
    int incomplete;     // persistent per-return VAP-incomplete flag
    int lastAccumBar;   // idempotency: last bar already accumulated
    double retStartSec; // return-start bar start (span provenance)
    int retStartValid;
};

struct TtdEpisode
{
    int id;             // stable integer ID (1-based)
    int dir;
    int source;
    int d;              // original candidate bar
    int bottomTick;     // frozen anchor (never widened by re-observation)
    int topTick;
    double origDelta;   // original evidence (never overwritten)
    int origBlocks;
    int displTicks;     // frozen displacement
    int deadlineBars;
    float threshBlock;  // baseline thresholds actually used (frozen, never
    float threshZone;   // overwritten by re-observation)
    int state;
    int confirmBar;     // -1 while pending
    int armedBar;       // earliest bar allowed to start the next return
    int returnOrdinal;
    int returnStartBar; // start bar of the currently open return (-1 if none)
    int openReturn;     // 1 while a return is unresolved
    int obsCount;       // association observation count
    int lastSeenBar;
    int lastLifecycleBar; // idempotency: last bar lifecycle-processed
    TtdReturnMeas meas;
};

struct TtdTransition
{
    int seq;            // 1-based ledger sequence
    int episodeId;
    int kind;
    int calcBar;        // bar j whose close decided this transition
    int availIndex;     // j+1: first knowable index (spec §6)
    int stateAfter;
    int returnOrdinal;
    int dir;
    int bottomTick;
    int topTick;
    double askSum;      // immutable snapshot
    double bidSum;
    int barCount;
    int penetrationTicks;
    int moveAwayTicks;
    double spanSec;
    int spanValid;
    int incomplete;
    int touchObserved;  // 1 when the deciding bar also touched the anchor
};

struct TtdEngine
{
    TtdEpisode episodes[TTD_MAX_EPISODES];
    int numEpisodes;
    int activeIdx[TTD_MAX_EPISODES]; // indices into episodes[] (nonterminal)
    int numActive;
    TtdTransition transitions[TTD_MAX_TRANSITIONS];
    int numTransitions;
    int nextId;
    int capacityLimited;    // episode storage exhausted: admission stopped
    int halted;             // transition storage exhausted: layer halted
    int suppressedAutoCount; // diagnostic count (Both-mode manual precedence)
    // Deterministic work counters (spec §7, Task 12): new-bars-only episode
    // evaluations and VAP price-level entries scanned during return
    // accumulation. Re-delivered/duplicate bars add zero — the counters are
    // the portable proof that unchanged updates do no semantic/VAP work.
    // Portable cost evidence only, never a Sierra UI measurement.
    long workBars;
    long workVap;
};

static void TtdReset(TtdEngine* eng)
{
    if (eng == nullptr)
        return;
    eng->numEpisodes = 0;
    eng->numActive = 0;
    eng->numTransitions = 0;
    eng->nextId = 1;
    eng->capacityLimited = 0;
    eng->halted = 0;
    eng->suppressedAutoCount = 0;
    eng->workBars = 0;
    eng->workVap = 0;
}

static const TtdEpisode* TtdFindEpisode(const TtdEngine* eng, int id)
{
    if (eng == nullptr)
        return nullptr;
    for (int i = 0; i < eng->numEpisodes; i++)
    {
        if (eng->episodes[i].id == id)
            return &eng->episodes[i];
    }
    return nullptr;
}

static int TtdIsTerminalState(int state)
{
    return (state == TTD_CROSSED_THROUGH || state == TTD_DISCARDED) ? 1 : 0;
}

static int TtdRangesOverlap(int loA, int hiA, int loB, int hiB)
{
    return (loA <= hiB && loB <= hiA) ? 1 : 0;
}

static int TtdIsValidVolume(double v); // defined with the measurement code

static int TtdValidateCandidate(const TtdCandidate* c)
{
    if (c == nullptr)
        return 0;
    if (c->dir != 1 && c->dir != -1)
        return 0;
    if (c->d < 0)
        return 0;
    if (c->bottomTick > c->topTick)
        return 0;
    if (c->source != TTD_SRC_MANUAL && c->source != TTD_SRC_AUTO)
        return 0;
    if (c->displTicks < 0 || c->deadlineBars < 0)
        return 0;
    if (!TtdIsValidVolume((double)c->threshBlock) ||
        !TtdIsValidVolume((double)c->threshZone))
        return 0;
    return 1;
}

// Remove an active-slot entry by episode array index (swap-remove keeps the
// active scan compact; terminal archive records stay in episodes[]).
static void TtdRetireActive(TtdEngine* eng, int epIdx)
{
    for (int i = 0; i < eng->numActive; i++)
    {
        if (eng->activeIdx[i] == epIdx)
        {
            eng->activeIdx[i] = eng->activeIdx[eng->numActive - 1];
            eng->numActive--;
            return;
        }
    }
}

// Feed one detector candidate for discovery bar c->d. Call AFTER lifecycle
// processing for the same bar (spec §3: lifecycle first, then candidates).
static int TtdAddCandidate(TtdEngine* eng, const TtdCandidate* c)
{
    if (eng == nullptr || !TtdValidateCandidate(c))
        return TTD_ERR_ARG;
    if (eng->halted)
        return TTD_ERR_HALTED;

    // Both-mode manual precedence: a NEW auto candidate overlapping a current
    // nonterminal manual anchor of the same direction is suppressed (diagnostic
    // count, not a rejection outcome).
    if (c->source == TTD_SRC_AUTO)
    {
        for (int i = 0; i < eng->numActive; i++)
        {
            const TtdEpisode* a = &eng->episodes[eng->activeIdx[i]];
            if (a->source == TTD_SRC_MANUAL && a->dir == c->dir &&
                TtdRangesOverlap(c->bottomTick, c->topTick, a->bottomTick, a->topTick))
            {
                eng->suppressedAutoCount++;
                return TTD_OK_SUPPRESSED;
            }
        }
    }

    // Association: same-source, same-direction, inclusively overlapping the
    // earliest active frozen anchor. Observation count/last-seen only — never
    // widen boundaries, never touch original evidence, never re-confirm.
    int bestIdx = -1;
    for (int i = 0; i < eng->numActive; i++)
    {
        const TtdEpisode* a = &eng->episodes[eng->activeIdx[i]];
        if (a->source != c->source || a->dir != c->dir)
            continue;
        if (!TtdRangesOverlap(c->bottomTick, c->topTick, a->bottomTick, a->topTick))
            continue;
        if (bestIdx < 0 || a->id < eng->episodes[eng->activeIdx[bestIdx]].id)
            bestIdx = i;
    }
    if (bestIdx >= 0)
    {
        TtdEpisode* e = &eng->episodes[eng->activeIdx[bestIdx]];
        e->obsCount++;
        e->lastSeenBar = c->d;
        return TTD_OK_ASSOCIATED;
    }

    // Admit a new frozen episode. Never silently evict on exhaustion.
    if (eng->numEpisodes >= TTD_MAX_EPISODES)
    {
        eng->capacityLimited = 1;
        return TTD_ERR_CAPACITY;
    }
    TtdEpisode* e = &eng->episodes[eng->numEpisodes];
    e->id = eng->nextId++;
    e->dir = c->dir;
    e->source = c->source;
    e->d = c->d;
    e->bottomTick = c->bottomTick;
    e->topTick = c->topTick;
    e->origDelta = c->origDelta;
    e->origBlocks = c->origBlocks;
    e->displTicks = c->displTicks;
    e->deadlineBars = c->deadlineBars;
    e->threshBlock = c->threshBlock;
    e->threshZone = c->threshZone;
    e->state = TTD_PENDING;
    e->confirmBar = -1;
    e->armedBar = -1;
    e->returnOrdinal = 0;
    e->returnStartBar = -1;
    e->openReturn = 0;
    e->obsCount = 1;
    e->lastSeenBar = c->d;
    e->lastLifecycleBar = c->d; // evaluation starts on d+1, never on d
    e->meas.ordinal = 0;
    e->meas.startBar = -1;
    e->meas.askSum = 0.0;
    e->meas.bidSum = 0.0;
    e->meas.barCount = 0;
    e->meas.maxHighTick = 0;
    e->meas.minLowTick = 0;
    e->meas.penetrationTicks = 0;
    e->meas.minLaterClose = 0;
    e->meas.maxLaterClose = 0;
    e->meas.hasLaterClose = 0;
    e->meas.moveAwayTicks = 0;
    e->meas.spanSec = 0.0;
    e->meas.spanValid = 0;
    e->meas.incomplete = 0;
    e->meas.lastAccumBar = -1;
    e->meas.retStartSec = 0.0;
    e->meas.retStartValid = 0;
    eng->activeIdx[eng->numActive++] = eng->numEpisodes;
    eng->numEpisodes++;
    return TTD_OK;
}

// Append one immutable transition snapshot. On exhaustion the layer halts
// visibly instead of losing history: no state change may be applied without
// its record.
static int TtdEmit(TtdEngine* eng, int episodeId, int kind, int calcBar,
                   int stateAfter, const TtdEpisode* e, int touchObserved)
{
    if (eng->numTransitions >= TTD_MAX_TRANSITIONS)
    {
        eng->halted = 1;
        return TTD_ERR_HALTED;
    }
    TtdTransition* t = &eng->transitions[eng->numTransitions];
    t->seq = eng->numTransitions + 1;
    t->episodeId = episodeId;
    t->kind = kind;
    t->calcBar = calcBar;
    t->availIndex = calcBar + 1; // first knowable index (spec §6)
    t->stateAfter = stateAfter;
    t->returnOrdinal = e->returnOrdinal;
    t->dir = e->dir;
    t->bottomTick = e->bottomTick;
    t->topTick = e->topTick;
    t->askSum = e->meas.askSum;
    t->bidSum = e->meas.bidSum;
    t->barCount = e->meas.barCount;
    t->penetrationTicks = e->meas.penetrationTicks;
    t->moveAwayTicks = e->meas.moveAwayTicks;
    t->spanSec = e->meas.spanSec;
    t->spanValid = e->meas.spanValid;
    t->incomplete = e->meas.incomplete;
    t->touchObserved = touchObserved;
    eng->numTransitions++;
    return TTD_OK;
}

// Pending displacement evaluation for one episode on closed bar b->j (j > d).
// Precedence mirrors the legacy detector: adverse close discards first, then
// displacement confirms, then the optional deadline discards (spec §4).
// Returns 1 when the episode turned terminal (caller retires it).
static int TtdProcessPending(TtdEngine* eng, TtdEpisode* e, const TtdBar* b)
{
    const int B = e->bottomTick;
    const int T = e->topTick;
    const int D = e->displTicks;

    const int adverse = (e->dir == -1) ? (b->closeTick > T) : (b->closeTick < B);
    if (adverse)
    {
        e->state = TTD_DISCARDED;
        if (TtdEmit(eng, e->id, TTD_TR_DISCARDED, b->j, TTD_DISCARDED, e, 0) != TTD_OK)
        {
            e->state = TTD_PENDING; // record lost: do not silently discard
            return 0;
        }
        return 1;
    }

    const int displaced = (e->dir == -1) ? (b->closeTick <= B - D)
                                         : (b->closeTick >= T + D);
    if (displaced)
    {
        e->state = TTD_FAILURE_CONFIRMED;
        e->confirmBar = b->j;
        e->armedBar = b->j + 1; // no retest on the confirmation bar
        if (TtdEmit(eng, e->id, TTD_TR_FAILURE_CONFIRMED, b->j,
                    TTD_FAILURE_CONFIRMED, e, 0) != TTD_OK)
        {
            e->state = TTD_PENDING; // record lost: do not silently confirm
            e->confirmBar = -1;
            e->armedBar = -1;
            return 0;
        }
        return 0;
    }

    if (e->deadlineBars > 0 && b->j >= e->d + e->deadlineBars)
    {
        e->state = TTD_DISCARDED;
        if (TtdEmit(eng, e->id, TTD_TR_DISCARDED, b->j, TTD_DISCARDED, e, 0) != TTD_OK)
        {
            e->state = TTD_PENDING;
            return 0;
        }
        return 1;
    }
    return 0;
}
// Bar-range helpers (chart ticks).
static int TtdTouchesAnchor(const TtdEpisode* e, int highTick, int lowTick)
{
    return (highTick >= e->bottomTick && lowTick <= e->topTick) ? 1 : 0;
}

static int TtdIsCrossedThrough(const TtdEpisode* e, int closeTick)
{
    // Strict close-through only: equality is NOT a cross (spec §4).
    return (e->dir == -1) ? (closeTick > e->topTick)
                          : (closeTick < e->bottomTick);
}

static int TtdIsRejectClose(const TtdEpisode* e, int closeTick)
{
    return (e->dir == -1) ? (closeTick <= e->bottomTick - e->displTicks)
                          : (closeTick >= e->topTick + e->displTicks);
}

// Portable finite/nonnegative check (no <cmath> dependency so the header
// stays includable from the ACSIL study without extra includes).
static int TtdIsValidVolume(double v)
{
    if (!(v == v))
        return 0; // NaN
    if (!(v < 1.0e308 && v > -1.0e308))
        return 0; // +/- infinity
    if (v < 0.0)
        return 0; // negative
    return 1;
}
// Every bar from first touch through resolution counts, overlapping or not;
// the VAP price filter ([B,T]) naturally yields zero off-anchor. Missing VAP
// while price overlaps sets the persistent incomplete flag (spec §5).
// Penetration uses bar-range extremes from the touch bar on; move-away uses
// LATER closed-bar closes only — never the touch bar's own extremes.
static void TtdAccumulateReturnBar(TtdEngine* eng, TtdEpisode* e, const TtdBar* b,
                                   int isTouchBar)
{
    TtdReturnMeas* m = &e->meas;
    if (b->j <= m->lastAccumBar)
        return; // duplicate delivery: never count the same bar twice
    m->lastAccumBar = b->j;
    m->barCount++;

    const int overlap = TtdTouchesAnchor(e, b->highTick, b->lowTick);
    if (b->vapMissing && overlap)
        m->incomplete = 1;

    // Fresh in-anchor volume for THIS return bar only: filter the bar's VAP
    // to the frozen inclusive [B,T], once per bar. Never sums previous
    // detection windows. Installed VAP volumes are double (fractional kept).
    if (!b->vapMissing && b->vapN > 0 && b->vapPrices != nullptr &&
        b->vapAsk != nullptr && b->vapBid != nullptr)
    {
        for (int i = 0; i < b->vapN; i++)
        {
            if (eng != nullptr)
                eng->workVap++; // every entry read, in-anchor or skipped
            const int p = b->vapPrices[i];
            if (p < e->bottomTick || p > e->topTick)
                continue;
            const double a = b->vapAsk[i];
            const double dv = b->vapBid[i];
            if (!TtdIsValidVolume(a) || !TtdIsValidVolume(dv))
            {
                m->incomplete = 1; // invalid element: flagged, not summed
                continue;
            }
            m->askSum += a;
            m->bidSum += dv;
        }
    }

    if (e->dir == -1)
    {
        if (m->barCount == 1)
            m->maxHighTick = b->highTick;
        else if (b->highTick > m->maxHighTick)
            m->maxHighTick = b->highTick;
        const int pen = m->maxHighTick - e->bottomTick;
        m->penetrationTicks = (pen > 0) ? pen : 0;
    }
    else
    {
        if (m->barCount == 1)
            m->minLowTick = b->lowTick;
        else if (b->lowTick < m->minLowTick)
            m->minLowTick = b->lowTick;
        const int pen = e->topTick - m->minLowTick;
        m->penetrationTicks = (pen > 0) ? pen : 0;
    }

    if (!isTouchBar)
    {
        if (e->dir == -1)
        {
            if (!m->hasLaterClose || b->closeTick < m->minLaterClose)
                m->minLaterClose = b->closeTick;
            m->hasLaterClose = 1;
            const int mv = e->bottomTick - m->minLaterClose;
            m->moveAwayTicks = (mv > 0) ? mv : 0;
        }
        else
        {
            if (!m->hasLaterClose || b->closeTick > m->maxLaterClose)
                m->maxLaterClose = b->closeTick;
            m->hasLaterClose = 1;
            const int mv = m->maxLaterClose - e->topTick;
            m->moveAwayTicks = (mv > 0) ? mv : 0;
        }
    }

    // Interaction span: return-start bar start to latest observed return bar
    // end. Includes intervening time; NOT dwell time. Invalid spans are
    // missing, never clamped into evidence.
    if (m->retStartValid && b->spanValid && b->barEndSec >= m->retStartSec)
    {
        m->spanSec = b->barEndSec - m->retStartSec;
        m->spanValid = 1;
    }
    else
    {
        m->spanSec = 0.0;
        m->spanValid = 0;
    }
    // Fresh VAP bid/ask accumulation across [B,T] is implemented above.
}

// Start a new return on touching bar b->j. Returns the emit result.
static int TtdStartReturn(TtdEngine* eng, TtdEpisode* e, const TtdBar* b)
{
    e->returnOrdinal++;
    e->returnStartBar = b->j;
    e->openReturn = 1;
    e->state = TTD_RETURN_UNRESOLVED;
    TtdReturnMeas* m = &e->meas;
    m->ordinal = e->returnOrdinal;
    m->startBar = b->j;
    m->askSum = 0.0;
    m->bidSum = 0.0;
    m->barCount = 0;
    m->penetrationTicks = 0;
    m->minLaterClose = 0;
    m->maxLaterClose = 0;
    m->hasLaterClose = 0;
    m->moveAwayTicks = 0;
    m->spanSec = 0.0;
    m->spanValid = 0;
    m->incomplete = 0;
    m->lastAccumBar = -1;
    m->retStartSec = b->barStartSec;
    m->retStartValid = b->spanValid;
    TtdAccumulateReturnBar(eng, e, b, 1);
    const int kind = (e->returnOrdinal == 1) ? TTD_TR_FIRST_RETURN
                                            : TTD_TR_RETURN_STARTED;
    return TtdEmit(eng, e->id, kind, b->j, TTD_RETURN_UNRESOLVED, e, 1);
}

// Confirmed-state lifecycle for one episode on closed bar b->j:
// crossing (terminal, precedence) > new touch > open-return continuation.
// Returns 1 when the episode turned terminal (caller retires it).
static int TtdProcessConfirmed(TtdEngine* eng, TtdEpisode* e, const TtdBar* b)
{
    const int overlap = TtdTouchesAnchor(e, b->highTick, b->lowTick);
    const TtdEpisode backup = *e; // restore on halt: no change without record

    // Crossing is terminal even on the first touching bar or the same bar
    // another condition might otherwise qualify (spec §4).
    if (TtdIsCrossedThrough(e, b->closeTick))
    {
        if (e->openReturn)
            TtdAccumulateReturnBar(eng, e, b, 0); // final bar counts toward effort
        e->state = TTD_CROSSED_THROUGH;
        e->openReturn = 0;
        if (TtdEmit(eng, e->id, TTD_TR_CROSSED, b->j, TTD_CROSSED_THROUGH, e,
                    overlap) != TTD_OK)
        {
            *e = backup; // record lost: halt with state untouched
            return 0;
        }
        return 1;
    }

    // A later eligible touching bar starts the next return (ordinal 1 first).
    // After a rejection the next return arms no earlier than the following
    // closed bar; prolonged overlap while unresolved never multi-counts.
    if (!e->openReturn &&
        (e->state == TTD_FAILURE_CONFIRMED || e->state == TTD_RETURN_REJECTED) &&
        b->j >= e->armedBar && overlap)
    {
        if (TtdStartReturn(eng, e, b) != TTD_OK)
        {
            *e = backup; // record lost: halt with state untouched
            return 0;
        }
        return 0;
    }

    // Open-return continuation on a LATER bar: reject closes resolve.
    if (e->openReturn && b->j > e->returnStartBar)
    {
        TtdAccumulateReturnBar(eng, e, b, 0);
        if (TtdIsRejectClose(e, b->closeTick))
        {
            e->state = TTD_RETURN_REJECTED;
            e->openReturn = 0;
            e->armedBar = b->j + 1;
            if (TtdEmit(eng, e->id, TTD_TR_RETURN_REJECTED, b->j,
                        TTD_RETURN_REJECTED, e, overlap) != TTD_OK)
            {
                *e = backup; // record lost: halt with state untouched
                return 0;
            }
        }
    }
    return 0;
}

static int TtdProcessBar(TtdEngine* eng, const TtdBar* b)
{
    if (eng == nullptr || b == nullptr)
        return TTD_ERR_ARG;
    if (eng->halted)
        return TTD_ERR_HALTED;

    for (int i = 0; i < eng->numActive; i++)
    {
        TtdEpisode* e = &eng->episodes[eng->activeIdx[i]];
        if (b->j <= e->lastLifecycleBar)
            continue;  // same-bar re-delivery or out-of-order: never re-decide
        if (TtdIsTerminalState(e->state))
        {
            // Invariant: terminal episodes retire immediately; this only
            // repairs the scan if one ever lingers.
            // [v3-C2] Rescan slot i only when the retire actually removed an
            // entry. TtdRetireActive is a no-op when the episode index is not
            // in activeIdx, and the old unconditional `i--` then re-examined
            // the same still-terminal episode forever.
            const int before = eng->numActive;
            TtdRetireActive(eng, eng->activeIdx[i]);
            if (eng->numActive < before)
                i--;
            continue;
        }
        if (e->state == TTD_PENDING)
        {
            const int wasTerminal = TtdProcessPending(eng, e, b);
            if (eng->halted)
                return TTD_ERR_HALTED;
            e->lastLifecycleBar = b->j;
            eng->workBars++; // one new-bar evaluation (redelivery never reaches here)
            if (wasTerminal)
            {
                TtdRetireActive(eng, eng->activeIdx[i]);
                i--;
            }
            continue;
        }
        // Confirmed episodes: crossing > new return > open-return continuation.
        const int crossedTerminal = TtdProcessConfirmed(eng, e, b);
        if (eng->halted)
            return TTD_ERR_HALTED;
        e->lastLifecycleBar = b->j;
        eng->workBars++; // one new-bar evaluation (redelivery never reaches here)
        if (crossedTerminal)
        {
            TtdRetireActive(eng, eng->activeIdx[i]);
            i--;
        }
    }
    return TTD_OK;
}

// =============================================================================
// Task 5: adapter policy — activation, semantic fingerprint, tick conversion
// =============================================================================

// Decision Map requires the Displacement confirmation mode: a next-open
// survival must never be reinterpreted as buying/selling failure (spec §1).
// enableDecisionMap = In[31]; displacementMode = (In[26] == Displacement).
static int TtdDecisionActive(int enableDecisionMap, int displacementMode)
{
    return (enableDecisionMap && displacementMode) ? 1 : 0;
}

// 0 = active; 1 = switched off; 2 = on but incompatible prerequisite.
static int TtdDecisionInactiveReason(int enableDecisionMap, int displacementMode)
{
    if (!enableDecisionMap)
        return 1;
    if (!displacementMode)
        return 2;
    return 0;
}

// FNV-1a byte hash primitives for the decision semantic fingerprint. The
// adapter feeds STRUCTURAL inputs only (detector geometry/thresholds, modes,
// displacement/deadline, tick size); display-only, export-control and
// snapshot-request inputs are never fed, so cosmetic changes cannot reset
// the semantic ledger. Floats hash by exact bit pattern.
static unsigned int TtdFnv1aBytes(const unsigned char* data, int len,
                                  unsigned int h)
{
    for (int i = 0; i < len; i++)
    {
        h ^= (unsigned int)data[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned int TtdFnv1aInt(unsigned int h, int v)
{
    return TtdFnv1aBytes((const unsigned char*)&v, (int)sizeof(v), h);
}

static unsigned int TtdFnv1aFloat(unsigned int h, float f)
{
    unsigned int bits = 0;
    memcpy(&bits, &f, sizeof(bits));
    return TtdFnv1aBytes((const unsigned char*)&bits, (int)sizeof(bits), h);
}

// Chart price to integer chart ticks (adapter geometry; spec §1). Returns 0
// for degenerate tick sizes — the adapter additionally bails on TickSize<=0.
static int TtdPriceToTicks(double price, double tickSize)
{
    if (!(tickSize > 0.0))
        return 0;
    return (int)floor(price / tickSize + 0.5);
}

// =============================================================================
// Task 6: availability counts, as-of selection, snapshots, status
// =============================================================================

// Per-availability-bar event counts, split by side and kind (spec §6: counts
// are nonnegative and split by side so simultaneous opposite events never
// cancel; RETURN_STARTED later ordinals are ledger-only, not first pulses).
struct TtdAvailCounts
{
    int confBuy;
    int confSell;
    int ret1Buy;
    int ret1Sell;
    int rejBuy;
    int rejSell;
    int crossBuy;
    int crossSell;
};

static void TtdCountAtAvail(const TtdEngine* eng, int availIndex,
                            TtdAvailCounts* out)
{
    if (out == nullptr)
        return;
    out->confBuy = 0;
    out->confSell = 0;
    out->ret1Buy = 0;
    out->ret1Sell = 0;
    out->rejBuy = 0;
    out->rejSell = 0;
    out->crossBuy = 0;
    out->crossSell = 0;
    if (eng == nullptr)
        return;
    for (int i = 0; i < eng->numTransitions; i++)
    {
        const TtdTransition* t = &eng->transitions[i];
        if (t->availIndex != availIndex)
            continue;
        const int isBuy = (t->dir == 1) ? 1 : 0;
        switch (t->kind)
        {
        case TTD_TR_FAILURE_CONFIRMED:
            if (isBuy) out->confBuy++; else out->confSell++;
            break;
        case TTD_TR_FIRST_RETURN:
            if (isBuy) out->ret1Buy++; else out->ret1Sell++;
            break;
        case TTD_TR_RETURN_REJECTED:
            if (isBuy) out->rejBuy++; else out->rejSell++;
            break;
        case TTD_TR_CROSSED:
            if (isBuy) out->crossBuy++; else out->crossSell++;
            break;
        default:
            break; // RETURN_STARTED/DISCARDED are ledger-only here
        }
    }
}

// Latest known episode state using only information available at availBar
// (transitions with availIndex <= availBar). Returns 1 when knowable.
static int TtdEpisodeStateAt(const TtdEngine* eng, int episodeId, int availBar,
                             int* stateOut, int* ordinalOut)
{
    if (eng == nullptr || stateOut == nullptr || ordinalOut == nullptr)
        return 0;
    int found = 0;
    int state = TTD_PENDING;
    int ordinal = 0;
    for (int i = 0; i < eng->numTransitions; i++)
    {
        const TtdTransition* t = &eng->transitions[i];
        if (t->episodeId != episodeId || t->availIndex > availBar)
            continue;
        state = t->stateAfter;
        ordinal = t->returnOrdinal;
        found = 1;
    }
    *stateOut = state;
    *ordinalOut = ordinal;
    return found;
}

// Rank eligible episodes as of availBar: intersecting lastCloseTick first,
// then distance to the frozen interval, newer confirmation, stable ID
// (spec §7). Eligible = confirmed with confirm availability <= availBar and
// no terminal transition at or before availBar. Returns the selected count
// (<= maxNearby, <= cap); outIds holds episode IDs.
static int TtdSelectAt(const TtdEngine* eng, int availBar, int lastCloseTick,
                       int maxNearby, int* outIds, int cap)
{
    if (eng == nullptr || outIds == nullptr || cap <= 0 || maxNearby <= 0)
        return 0;
    int cand[TTD_MAX_EPISODES];
    int n = 0;
    for (int i = 0; i < eng->numEpisodes; i++)
    {
        const TtdEpisode* e = &eng->episodes[i];
        if (e->confirmBar < 0 || e->confirmBar + 1 > availBar)
            continue; // pending or not yet knowable at availBar
        int terminal = 0;
        for (int k = 0; k < eng->numTransitions; k++)
        {
            const TtdTransition* t = &eng->transitions[k];
            if (t->episodeId != e->id || t->availIndex > availBar)
                continue;
            if (t->kind == TTD_TR_CROSSED || t->kind == TTD_TR_DISCARDED)
                terminal = 1;
        }
        if (terminal)
            continue;
        cand[n++] = i;
    }
    // Deterministic insertion sort by (nonIntersect, distance, -confirmBar, id).
    for (int i = 1; i < n; i++)
    {
        const int move = cand[i];
        int j = i - 1;
        while (j >= 0)
        {
            const TtdEpisode* a = &eng->episodes[cand[j]];
            const TtdEpisode* b = &eng->episodes[move];
            int da = 0;
            int db = 0;
            int na = 0;
            int nb = 0;
            if (lastCloseTick < a->bottomTick) { da = a->bottomTick - lastCloseTick; na = 1; }
            else if (lastCloseTick > a->topTick) { da = lastCloseTick - a->topTick; na = 1; }
            if (lastCloseTick < b->bottomTick) { db = b->bottomTick - lastCloseTick; nb = 1; }
            else if (lastCloseTick > b->topTick) { db = lastCloseTick - b->topTick; nb = 1; }
            int swap = 0;
            if (nb != na) swap = (nb < na) ? 1 : 0;
            else if (db != da) swap = (db < da) ? 1 : 0;
            else if (b->confirmBar != a->confirmBar)
                swap = (b->confirmBar > a->confirmBar) ? 1 : 0;
            else swap = (b->id < a->id) ? 1 : 0;
            if (!swap)
                break;
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = move;
    }
    int out = n;
    if (out > maxNearby)
        out = maxNearby;
    if (out > cap)
        out = cap;
    for (int i = 0; i < out; i++)
        outIds[i] = eng->episodes[cand[i]].id;
    return out;
}

// Per-availability-bar selected-state snapshot (spec §6 SG15..SG19): counts
// plus the first selected episode's frozen state. Computed from the ledger
// with an availBar cutoff, so later bars can never rewrite it. selId = 0
// means no selected episode. Zero-filled (padding-safe for memcmp).
struct TtdBarSnapshot
{
    int availBar;
    TtdAvailCounts counts;
    int selId;
    int selState;
    int selBottom;
    int selTop;
    int status;
};

static void TtdSnapshotAt(const TtdEngine* eng, int availBar, int lastCloseTick,
                          int maxNearby, int statusBits, TtdBarSnapshot* out)
{
    if (out == nullptr)
        return;
    memset(out, 0, sizeof(*out));
    out->availBar = availBar;
    out->status = statusBits;
    if (eng == nullptr)
        return;
    TtdCountAtAvail(eng, availBar, &out->counts);
    int ids[64];
    const int n = TtdSelectAt(eng, availBar, lastCloseTick, maxNearby, ids, 64);
    if (n <= 0)
        return;
    out->selId = ids[0];
    const TtdEpisode* e = TtdFindEpisode(eng, ids[0]);
    if (e == nullptr)
    {
        out->selId = 0;
        return;
    }
    out->selBottom = e->bottomTick;
    out->selTop = e->topTick;
    int ordinal = 0;
    if (!TtdEpisodeStateAt(eng, ids[0], availBar, &out->selState, &ordinal))
        out->selState = TTD_PENDING;
}

// Exact SG19 decision data status bitmask (spec §6).
static int TtdDataStatus(int inactive, int incompleteVap, int capacityLimited,
                         int exportFailed, int internalFault)
{
    int s = 0;
    if (inactive) s |= 1;
    if (incompleteVap) s |= 2;
    if (capacityLimited) s |= 4;
    if (exportFailed) s |= 8;
    if (internalFault) s |= 16;
    return s;
}

// =============================================================================
// [v3-P1] Linear availability sweep — same answers as TtdSnapshotAt, one pass
// =============================================================================
// TtdSnapshotAt costs O(episodes x transitions) for ONE availability bar
// (TtdCountAtAvail rescans the ledger, TtdSelectAt rescans the ledger once per
// episode, and its insertion sort is quadratic in the eligible set). PASS 3D
// calls it for every availability bar, so publishing SG7..SG19 across a rebuilt
// chart was O(bars x episodes x transitions) — minutes to effectively never on
// a chart with real history. Nothing about the answers required that cost:
//
//   * The ledger is emitted in nondecreasing availIndex order. Bars are
//     processed in increasing j and every transition emitted while processing
//     bar j carries availIndex = j+1, so a single cursor walking transitions
//     as `avail` advances sees exactly the prefix TtdEpisodeStateAt would
//     have selected with an availIndex <= avail cutoff.
//   * TtdSelectAt's eligibility ("confirmed by availBar, no terminal
//     transition at or before availBar") is precisely "a FAILURE_CONFIRMED
//     transition has been consumed and a CROSSED/DISCARDED one has not", so
//     the eligible set is maintained by the same cursor — no per-bar rescan
//     and no precomputed terminal table.
//   * Only the RANKING HEAD is published (TtdSnapshotAt reads ids[0] and
//     nothing else), so the sort disappears: a single minimum under the same
//     comparator is enough.
//
// TtdSnapshotAt / TtdSelectAt are kept unchanged — they still back the draw
// plan (called once per callback) and the portable tests — and this sweep is
// pinned against them by construction: same eligibility, same comparator, same
// count split. The caller owns every array (no allocation, no STL, no static
// mutable state), exactly like the rest of this header.
#define TTD_SWEEP_OK 0

struct TtdSweep
{
    int  cap;        // capacity of the caller arrays; must be >= numEpisodes
    int  cursor;     // next unconsumed transition index
    int  nActive;    // eligible episodes as of the last advanced availability
    int* state;      // [cap] latest stateAfter per episode slot
    int* ordinal;    // [cap] latest returnOrdinal per episode slot
    int* active;     // [cap] eligible episode slots
};

// Episode id -> episodes[] slot. Ids are handed out by nextId++ as episodes are
// appended, so id == slot+1 holds for every engine this header can build; the
// linear fallback keeps the sweep correct if that ever stops being true.
static int TtdEpisodeSlot(const TtdEngine* eng, int episodeId)
{
    if (eng == nullptr)
        return -1;
    const int guess = episodeId - 1;
    if (guess >= 0 && guess < eng->numEpisodes &&
        eng->episodes[guess].id == episodeId)
        return guess;
    for (int i = 0; i < eng->numEpisodes; i++)
    {
        if (eng->episodes[i].id == episodeId)
            return i;
    }
    return -1;
}

// Reset a sweep to availability 0 (nothing consumed, nothing eligible).
static int TtdSweepInit(const TtdEngine* eng, TtdSweep* s)
{
    if (eng == nullptr || s == nullptr || s->state == nullptr ||
        s->ordinal == nullptr || s->active == nullptr)
        return TTD_ERR_ARG;
    if (s->cap < eng->numEpisodes)
        return TTD_ERR_CAPACITY;
    s->cursor = 0;
    s->nActive = 0;
    for (int i = 0; i < eng->numEpisodes; i++)
    {
        s->state[i] = TTD_PENDING;
        s->ordinal[i] = 0;
    }
    return TTD_SWEEP_OK;
}

static void TtdSweepDropActive(TtdSweep* s, int slot)
{
    for (int i = 0; i < s->nActive; i++)
    {
        if (s->active[i] == slot)
        {
            s->active[i] = s->active[s->nActive - 1];
            s->nActive--;
            return;
        }
    }
}

// Consume every transition first knowable at `avail`. MUST be called with
// avail = 1, 2, 3, ... consecutively: a skipped availability would lose that
// bar's counts. Fills `counts` with the same split TtdCountAtAvail produces for
// this availability (nonnegative, per side, RETURN_STARTED ledger-only).
static void TtdSweepAdvance(const TtdEngine* eng, TtdSweep* s, int avail,
                            TtdAvailCounts* counts)
{
    if (counts != nullptr)
    {
        counts->confBuy = 0;  counts->confSell = 0;
        counts->ret1Buy = 0;  counts->ret1Sell = 0;
        counts->rejBuy = 0;   counts->rejSell = 0;
        counts->crossBuy = 0; counts->crossSell = 0;
    }
    if (eng == nullptr || s == nullptr)
        return;
    while (s->cursor < eng->numTransitions &&
           eng->transitions[s->cursor].availIndex <= avail)
    {
        const TtdTransition* t = &eng->transitions[s->cursor];
        s->cursor++;
        const int slot = TtdEpisodeSlot(eng, t->episodeId);
        if (slot < 0 || slot >= s->cap)
            continue;
        s->state[slot] = t->stateAfter;
        s->ordinal[slot] = t->returnOrdinal;

        if (counts != nullptr && t->availIndex == avail)
        {
            const int isBuy = (t->dir == 1) ? 1 : 0;
            switch (t->kind)
            {
            case TTD_TR_FAILURE_CONFIRMED:
                if (isBuy) counts->confBuy++; else counts->confSell++;
                break;
            case TTD_TR_FIRST_RETURN:
                if (isBuy) counts->ret1Buy++; else counts->ret1Sell++;
                break;
            case TTD_TR_RETURN_REJECTED:
                if (isBuy) counts->rejBuy++; else counts->rejSell++;
                break;
            case TTD_TR_CROSSED:
                if (isBuy) counts->crossBuy++; else counts->crossSell++;
                break;
            default:
                break;
            }
        }

        if (t->kind == TTD_TR_FAILURE_CONFIRMED)
        {
            int present = 0;
            for (int i = 0; i < s->nActive && !present; i++)
                if (s->active[i] == slot) present = 1;
            if (!present && s->nActive < s->cap)
                s->active[s->nActive++] = slot;
        }
        else if (t->kind == TTD_TR_CROSSED || t->kind == TTD_TR_DISCARDED)
        {
            TtdSweepDropActive(s, slot);
        }
    }
}

// Ranking head of the eligible set: intersecting lastCloseTick first, then
// distance to the frozen interval, newer confirmation, stable ID — the exact
// comparator TtdSelectAt sorts with. Returns an episodes[] slot, or -1.
static int TtdSweepSelect(const TtdEngine* eng, const TtdSweep* s,
                          int lastCloseTick)
{
    if (eng == nullptr || s == nullptr)
        return -1;
    int best = -1;
    int bd = 0;
    int bn = 0;
    for (int i = 0; i < s->nActive; i++)
    {
        const int slot = s->active[i];
        if (slot < 0 || slot >= eng->numEpisodes)
            continue;
        const TtdEpisode* e = &eng->episodes[slot];
        int d = 0;
        int n = 0;
        if (lastCloseTick < e->bottomTick) { d = e->bottomTick - lastCloseTick; n = 1; }
        else if (lastCloseTick > e->topTick) { d = lastCloseTick - e->topTick; n = 1; }
        if (best < 0)
        {
            best = slot; bd = d; bn = n;
            continue;
        }
        const TtdEpisode* a = &eng->episodes[best];
        int better;
        if (n != bn)                          better = (n < bn) ? 1 : 0;
        else if (d != bd)                     better = (d < bd) ? 1 : 0;
        else if (e->confirmBar != a->confirmBar)
                                              better = (e->confirmBar > a->confirmBar) ? 1 : 0;
        else                                  better = (e->id < a->id) ? 1 : 0;
        if (better)
        {
            best = slot; bd = d; bn = n;
        }
    }
    return best;
}

// =============================================================================
// Task 7: update policy — rebuild vs incremental continue
// =============================================================================

// Production rebuild decision (the adapter calls this exact function):
// rebuild on first use (no engine), explicit full recalculation, rewind or
// array shrink below the processed frontier, a structural fingerprint
// change, or a historical correction inside already-processed closed
// history (Sierra re-requests from updateStartIndex > 0; when that index
// lies at or below the processed frontier, cached engine state and day
// caches are stale). New bars and cosmetic-only rescans continue
// incrementally — the per-episode idempotency makes re-delivery of
// processed bars a no-op, so display-only changes can never reset the
// semantic ledger. Forming-only ticks (updateStartIndex above the frontier)
// and normal appends stay no-op/incremental.
static int TtdNeedsRebuildEx(int hasEngine, int fullRecalc,
                              int updateStartIndex, int lastBar,
                              int decMark, int fpChanged)
{
    if (!hasEngine)
        return 1;
    if (fullRecalc)
        return 1;
    if (fpChanged)
        return 1;
    if (lastBar < decMark)
        return 1;
    if (updateStartIndex > 0 && decMark >= 0 &&
        updateStartIndex <= decMark)
        return 1;
    return 0;
}

static int TtdNeedsRebuild(int hasEngine, int fullRecalc, int lastBar,
                           int decMark, int fpChanged)
{
    return TtdNeedsRebuildEx(hasEngine, fullRecalc, 0, lastBar, decMark,
                             fpChanged);
}

// =============================================================================
// Review-fix-3: halt-visible frontier + one-log latch (BuildSpec §3)
// =============================================================================
// When transition storage exhausts, the core sets eng->halted but the ledger
// keeps every record written so far. The adapter must still advance its
// per-bar frontier past the halting bar (and every later halted bar) so the
// PASS 3D availability loop emits SG19 with bit 4 at avail=j+1 and the halt
// status persists on the right edge while halted. Frozen per-bar snapshots
// behind the frontier are never rewritten (the incremental window touches
// only newly available bars).
//
// Latch policy: status itself is per-availability — recomputed each pass
// from the live engine flags via TtdDataStatus (capacityLimited||halted) —
// so no per-generation status latch exists; persistence comes from the
// advancing frontier plus the halted flag surviving until a rebuild resets
// the generation. The Message Log line uses a per-generation latch
// (adapter persistent P_DHALTLOG): exactly one line when the halt first
// occurs. TTD_HAS_HALT_SCHED marks this seam for the halt regression test.
#define TTD_HAS_HALT_SCHED 1

struct TtdAvailWindow
{
    int firstAvail; // PASS 3D window start (inclusive)
    int lastAvail;  // PASS 3D window end (inclusive)
};

// PASS 3D availability window from the decision frontier: incremental calls
// cover newly processed bars as availabilities markBefore+2..markAfter+1; a
// rebuild restarts at 1. The adapter calls this exact function; the halt
// test drives it with the same arguments.
static TtdAvailWindow TtdAvailWindowFor(int markBefore, int markAfter,
                                        int rebuilt)
{
    TtdAvailWindow w;
    w.firstAvail = (rebuilt != 0) ? 1 : (markBefore + 2);
    w.lastAvail = markAfter + 1;
    return w;
}

// Frontier advance for one closed bar: the frontier advances past every new
// decision bar INCLUDING the halting bar and later halted bars
// (lifecycle/candidates stay stopped while halted — only the frontier
// moves, so PASS 3D can publish the halt status). doBar = this winEnd is a
// new bar for the decision layer (adapter decDoBar). haltedNow is an
// explicit argument so the halted case is pinned by tests; it does not
// block the advance.
static int TtdHaltAdvanceMark(int doBar, int haltedNow)
{
    (void)haltedNow;
    return (doBar != 0) ? 1 : 0;
}

// One-per-generation halt log latch: due exactly once, when the engine is
// halted and the generation has not logged it yet. The adapter passes its
// P_DHALTLOG latch (reset on rebuild) and sets it when this returns 1.
static int TtdHaltLogDue(int haltedNow, int haltLogged)
{
    if (haltedNow == 0 || haltLogged != 0)
        return 0;
    return 1;
}

// Visual-only refresh decision (the adapter calls this exact function on
// the no-new-bar fast path): display/export-only controls (MaxNearby,
// detail panel, one-shot snapshot, journal with pending rows) must refresh
// drawings/panel/files without resetting semantic history, while ordinary
// unchanged ticks do zero semantic/VAP/file work. Returns 1 when the
// adapter must fall through to the render/export paths with an empty
// semantic window; 0 means the callback can return immediately.
// Snapshot term is increase-only (BuildSpec §2, same policy as
// TtdSnapshotDue): a decrease never reschedules a write.
// Attempt honesty: lastSnapReq/lastJournalSeq carry the SERVED-or-ATTEMPTED
// high-water (adapter passes max(P_DSNAP,P_DSATT) / max(P_DJSEQ,P_DJATT)),
// so already-attempted failed work does not reschedule file attempts on
// unchanged ticks; genuinely new rows or a new request increase still do.
// The served watermarks themselves stay honest (advanced only on success).
static int TtdVisualRefreshDue(int lastMaxNearby, int curMaxNearby,
                               int lastDetail, int curDetail,
                               int lastSnapReq, int curSnapReq,
                               int journalEnabled, int lastJournalSeq,
                               int numTransitions)
{
    if (curMaxNearby != lastMaxNearby)
        return 1;
    if (curDetail != lastDetail)
        return 1;
    if (curSnapReq > 0 && curSnapReq > lastSnapReq)
        return 1;
    if (journalEnabled && numTransitions > lastJournalSeq)
        return 1;
    return 0;
}

// Forward: one-shot snapshot request policy (defined with the snapshot
// writer below; needed here by TtdSnapshotAttemptDue).
static int TtdSnapshotDue(int lastReq, int curReq);

// Export attempt gating (the adapter calls these exact functions before
// every optional file operation; BuildSpec §6/§7). Served watermarks
// (P_DJSEQ/P_DSNAP) stay honest — advanced only on success — while the
// attempted high-waters (adapter P_DJATT/P_DSATT) gate repeat FILE
// attempts, so an invalid export folder cannot cause unbounded work on
// unchanged forming ticks. A bounded retry is allowed on a new closed
// bar, on genuinely new transitions, or on an explicit request increase.
// Returns 1 when the adapter must attempt the file operation now; 0 means
// skip with zero file I/O. Pure policy: no file access, no ledger change.
static int TtdJournalAttemptDue(int journalEnabled, int lastServedSeq,
                                int lastAttemptSeq, int numTransitions,
                                int isNewBar)
{
    if (!journalEnabled)
        return 0;
    if (numTransitions <= lastServedSeq)
        return 0; // nothing pending
    if (numTransitions > lastAttemptSeq)
        return 1; // new rows never attempted
    if (isNewBar)
        return 1; // one bounded retry of the same pending batch
    return 0;
}

static int TtdSnapshotAttemptDue(int lastServedReq, int lastAttemptReq,
                                 int curReq, int isNewBar)
{
    if (!TtdSnapshotDue(lastServedReq, curReq))
        return 0; // served, unchanged, at-or-below-served, or non-positive
    if (curReq > lastAttemptReq)
        return 1; // explicit increase never attempted
    if (isNewBar && curReq >= lastAttemptReq)
        return 1; // one bounded retry of the same failed request value
    return 0;     // stale decrease below the attempted high-water: never
}

// Decision-OFF cleanup scheduling (the adapter calls this exact function
// on the no-new-bar fast path when the decision layer is not rendering;
// BuildSpec §7: OFF restores the ordinary renderer). Returns 1 only on the
// transition out of decision rendering (wasRendering latched in P_DMODE,
// owned drawings still on the chart), so the callback falls through once
// with an empty semantic window: PASS 3D flags SG19 inactive, PASS 3E
// removes owned drawings, PASS 4 restores legacy rendering. Steady OFF
// returns 0 (true no-op). Pure policy: no drawing or ledger access.
static int TtdDecisionOffCleanupDue(int wasRendering, int nowRendering)
{
    return (wasRendering != 0 && nowRendering == 0) ? 1 : 0;
}

// =============================================================================
// Task 8: legacy correctness helpers (tested here, called by the adapter)
// =============================================================================

// Full legacy next-bar-open confirmation step WITH the same-bar
// close-through fix. Mirrors the legacy strict float comparisons exactly:
//   0 = still pending (confirmation bar not reached)
//   1 = confirmed and alive
//   2 = rejected by open (never drawn)
//   3 = confirmed by open but erased by same-bar close (the bypass fix:
//       the old confirm-then-`continue` shape never consulted the close)
static int TtdLegacyNextBarStep(int isBuyZone, int winEnd, int detectedBar,
                                float openNext, float close,
                                float topP, float botP)
{
    if (winEnd != detectedBar + 1)
        return 0;
    if (isBuyZone ? (openNext < botP) : (openNext > topP))
        return 2;
    if (isBuyZone ? (close < botP) : (close > topP))
        return 3;
    return 1;
}

// First-fit drawing-slot pool over a raw free list (1 = used). The adapter
// owns the vector; this is the tested allocation core: rejected/erased zones
// free their slot immediately so rejection-heavy batches recycle promptly,
// and later bars retry allocation instead of losing the drawing forever.
static int TtdSlotAlloc(char* used, int n)
{
    if (used == nullptr || n <= 0)
        return -1;
    for (int i = 0; i < n; i++)
    {
        if (!used[i])
        {
            used[i] = 1;
            return i;
        }
    }
    return -1;
}

static void TtdSlotFree(char* used, int n, int slot)
{
    if (used == nullptr || n <= 0)
        return;
    if (slot < 0 || slot >= n)
        return;
    used[slot] = 0;
}

// Bounded dirty-cell set for sparse legacy SG rewrites. Detection-bar cells
// whose zone content changed are re-resolved individually; overflow falls
// back to the marked span [lo,hi] — never a whole-history clear on an
// ordinary callback. The span covers every mark, including duplicates and
// post-overflow marks, so the fallback and the earliest-changed index stay
// accurate. Never grows: no heap.
#define TTD_DIRTY_MAX 256

struct TtdDirtySet
{
    int idx[TTD_DIRTY_MAX];
    int count;
    int overflow;
    int lo;   // min marked bar, -1 when empty
    int hi;   // max marked bar, -1 when empty
};

static void TtdDirtyClear(TtdDirtySet* s)
{
    if (s == nullptr)
        return;
    s->count = 0;
    s->overflow = 0;
    s->lo = -1;
    s->hi = -1;
}

static void TtdDirtyMark(TtdDirtySet* s, int bar)
{
    if (s == nullptr || bar < 0)
        return;
    if (s->lo < 0 || bar < s->lo)
        s->lo = bar;
    if (s->hi < 0 || bar > s->hi)
        s->hi = bar;
    for (int i = 0; i < s->count; i++)
    {
        if (s->idx[i] == bar)
            return;
    }
    if (s->count >= TTD_DIRTY_MAX)
    {
        s->overflow = 1;
        return;
    }
    s->idx[s->count++] = bar;
}

static int TtdDirtyMin(const TtdDirtySet* s)
{
    if (s == nullptr || s->count <= 0)
        return -1;
    int m = s->idx[0];
    for (int i = 1; i < s->count; i++)
    {
        if (s->idx[i] < m)
            m = s->idx[i];
    }
    return m;
}

// =============================================================================
// Task 9: compact drawing intents, selection-bounded plans, owned IDs
// =============================================================================

// Drawing kinds owned per selected episode (band + label + up to 3 markers).
#define TTD_DRAW_BAND 1
#define TTD_DRAW_LABEL 2
#define TTD_DRAW_CONFIRM_POINT 3
#define TTD_DRAW_FIRST_DIAMOND 4
#define TTD_DRAW_CROSS_X 5

#define TTD_DRAW_PLAN_MAX 32
#define TTD_DRAW_OWNER_MAX 160

// Desired drawings for one episode as of availBar, derived from the ledger
// with an availIndex cutoff so later bars can never rewrite it. Pending
// episodes are never filled failure zones (visible=0). Bands start at
// confirmation availability and terminate at crossing availability; markers
// stay at their actual availability. Crossed bands with showErased=0 are
// muted (visible=0); the ledger itself is untouched by visibility.
struct TtdDrawIntent
{
    int episodeId;
    int visible;
    int bandStartAvail;
    int bandEndAvail;
    int bottomTick;
    int topTick;
    int dir;
    int state;
    int returnOrdinal; // latest return ordinal as of availBar (0 = none yet)
    int confirmAvail;
    int hasConfirmPoint;
    int firstAvail;
    int hasFirstDiamond;
    int crossAvail;
    int hasCrossX;
};

static int TtdDrawIntentAt(const TtdEngine* eng, int episodeId, int availBar,
                           int showErased, TtdDrawIntent* out)
{
    if (out == nullptr)
        return 0;
    out->episodeId = episodeId;
    out->visible = 0;
    out->bandStartAvail = 0;
    out->bandEndAvail = 0;
    out->bottomTick = 0;
    out->topTick = 0;
    out->dir = 0;
    out->state = TTD_PENDING;
    out->returnOrdinal = 0;
    out->confirmAvail = -1;
    out->hasConfirmPoint = 0;
    out->firstAvail = -1;
    out->hasFirstDiamond = 0;
    out->crossAvail = -1;
    out->hasCrossX = 0;
    if (eng == nullptr)
        return 0;
    const TtdEpisode* e = TtdFindEpisode(eng, episodeId);
    if (e == nullptr)
        return 0;
    out->bottomTick = e->bottomTick;
    out->topTick = e->topTick;
    out->dir = e->dir;

    int confirmAvail = -1;
    int firstAvail = -1;
    int crossAvail = -1;
    int discarded = 0;
    int state = TTD_PENDING;
    int ordinal = 0;
    int seen = 0;
    for (int i = 0; i < eng->numTransitions; i++)
    {
        const TtdTransition* t = &eng->transitions[i];
        if (t->episodeId != episodeId || t->availIndex > availBar)
            continue;
        seen = 1;
        state = t->stateAfter;
        ordinal = t->returnOrdinal;
        if (t->kind == TTD_TR_FAILURE_CONFIRMED && confirmAvail < 0)
            confirmAvail = t->availIndex;
        if (t->kind == TTD_TR_FIRST_RETURN && firstAvail < 0)
            firstAvail = t->availIndex;
        if (t->kind == TTD_TR_CROSSED && crossAvail < 0)
            crossAvail = t->availIndex;
        if (t->kind == TTD_TR_DISCARDED)
            discarded = 1;
    }
    if (confirmAvail < 0)
        return 1; // pending (or discarded pre-confirm): never a filled band
    out->state = seen ? state : TTD_PENDING;
    out->returnOrdinal = ordinal;
    out->confirmAvail = confirmAvail;
    out->hasConfirmPoint = 1;
    out->bandStartAvail = confirmAvail;
    if (firstAvail >= 0)
    {
        out->firstAvail = firstAvail;
        out->hasFirstDiamond = 1;
    }
    if (crossAvail >= 0)
    {
        out->crossAvail = crossAvail;
        out->hasCrossX = 1;
        out->bandEndAvail = crossAvail;
        out->visible = showErased ? 1 : 0; // muted when erased hidden
        return 1;
    }
    if (discarded)
        return 1; // confirmed then discarded is not expected; stay unfilled
    out->bandEndAvail = availBar;
    out->visible = 1;
    return 1;
}

// Selection-bounded draw plan: intents for exactly the TtdSelectAt ranking
// (head kept when capped). Read-only over the engine; display caps and
// visibility toggles never touch the canonical ledger.
struct TtdDrawPlan
{
    int n;
    TtdDrawIntent intents[TTD_DRAW_PLAN_MAX];
};

static int TtdDrawPlanAt(const TtdEngine* eng, int availBar, int lastCloseTick,
                         int maxNearby, int showErased, TtdDrawPlan* out)
{
    if (out == nullptr)
        return 0;
    out->n = 0;
    if (eng == nullptr)
        return 0;
    if (maxNearby <= 0)
        return 0;
    int cap = maxNearby;
    if (cap > TTD_DRAW_PLAN_MAX)
        cap = TTD_DRAW_PLAN_MAX;
    // Eligible = confirmed with confirm availability <= availBar and no
    // terminal transition at or before availBar; plus, when erased zones are
    // shown, crossed-terminal episodes frozen at their crossing availability
    // (legacy erased-zone display semantics). Pending/discarded episodes are
    // never filled failure zones. Same deterministic ranking as TtdSelectAt:
    // intersecting lastCloseTick first, then distance, newer confirmation,
    // then stable ID.
    int cand[TTD_MAX_EPISODES];
    int n = 0;
    for (int i = 0; i < eng->numEpisodes; i++)
    {
        const TtdEpisode* e = &eng->episodes[i];
        if (e->confirmBar < 0 || e->confirmBar + 1 > availBar)
            continue;
        int crossed = 0;
        int terminal = 0;
        for (int k = 0; k < eng->numTransitions; k++)
        {
            const TtdTransition* t = &eng->transitions[k];
            if (t->episodeId != e->id || t->availIndex > availBar)
                continue;
            if (t->kind == TTD_TR_CROSSED)
                crossed = 1;
            if (t->kind == TTD_TR_CROSSED || t->kind == TTD_TR_DISCARDED)
                terminal = 1;
        }
        if (terminal && !(showErased && crossed))
            continue;
        cand[n++] = i;
    }
    for (int i = 1; i < n; i++)
    {
        const int move = cand[i];
        int j = i - 1;
        while (j >= 0)
        {
            const TtdEpisode* a = &eng->episodes[cand[j]];
            const TtdEpisode* b = &eng->episodes[move];
            int da = 0;
            int db = 0;
            int na = 0;
            int nb = 0;
            if (lastCloseTick < a->bottomTick) { da = a->bottomTick - lastCloseTick; na = 1; }
            else if (lastCloseTick > a->topTick) { da = lastCloseTick - a->topTick; na = 1; }
            if (lastCloseTick < b->bottomTick) { db = b->bottomTick - lastCloseTick; nb = 1; }
            else if (lastCloseTick > b->topTick) { db = lastCloseTick - b->topTick; nb = 1; }
            int swap = 0;
            if (nb != na) swap = (nb < na) ? 1 : 0;
            else if (db != da) swap = (db < da) ? 1 : 0;
            else if (b->confirmBar != a->confirmBar)
                swap = (b->confirmBar > a->confirmBar) ? 1 : 0;
            else swap = (b->id < a->id) ? 1 : 0;
            if (!swap)
                break;
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = move;
    }
    int outN = n;
    if (outN > cap)
        outN = cap;
    for (int i = 0; i < outN; i++)
    {
        const TtdEpisode* e = &eng->episodes[cand[i]];
        TtdDrawIntentAt(eng, e->id, availBar, showErased, &out->intents[i]);
    }
    out->n = outN;
    return outN;
}

// Instance-owned automatic drawing IDs. Each study instance keeps its own
// slot array; Sierra allocates a LineNumber once (UseTool > 0) and the
// adapter retains it, adjusting/deleting exactly that object. Slots never
// store TOOL_DELETE_ALL (-1); deletes name one owned ID.
struct TtdOwnedDraw
{
    int episodeId;
    int kind;
    int lineId;     // Sierra-allocated LineNumber (> 0); 0 = none
    int lastStart;  // last drawn anchor (band start / marker avail)
    int lastEnd;    // last drawn band end (markers: mirror of lastStart)
    int drawn;      // 1 once successfully drawn
};

static int TtdOwnedFind(const TtdOwnedDraw* slots, int n, int episodeId,
                        int kind)
{
    if (slots == nullptr || n <= 0)
        return -1;
    for (int i = 0; i < n; i++)
    {
        if (slots[i].episodeId == episodeId && slots[i].kind == kind)
            return i;
    }
    return -1;
}

// Claim a slot for a Sierra-allocated ID. A non-positive allocatedId is the
// checked-API failure path (UseTool <= 0): nothing is stored so the adapter
// retries later instead of owning a dead ID.
static int TtdOwnedClaim(TtdOwnedDraw* slots, int cap, int* count,
                         int episodeId, int kind, int allocatedId)
{
    if (slots == nullptr || count == nullptr || cap <= 0)
        return TTD_ERR_ARG;
    if (*count < 0 || *count > cap)
        return TTD_ERR_ARG;
    if (episodeId <= 0 || allocatedId <= 0)
        return TTD_ERR_ARG;
    if (kind < TTD_DRAW_BAND || kind > TTD_DRAW_CROSS_X)
        return TTD_ERR_ARG;
    if (TtdOwnedFind(slots, *count, episodeId, kind) >= 0)
        return TTD_ERR_ARG;
    if (*count >= cap)
        return TTD_ERR_CAPACITY;
    TtdOwnedDraw* s = &slots[*count];
    s->episodeId = episodeId;
    s->kind = kind;
    s->lineId = allocatedId;
    s->lastStart = 0;
    s->lastEnd = 0;
    s->drawn = 0;
    (*count)++;
    return TTD_OK;
}

// Release a slot, returning the exact owned ID for a single-object delete.
static int TtdOwnedRelease(TtdOwnedDraw* slots, int* count, int idx,
                           int* lineIdOut)
{
    if (slots == nullptr || count == nullptr || lineIdOut == nullptr)
        return TTD_ERR_ARG;
    if (idx < 0 || idx >= *count)
        return TTD_ERR_ARG;
    *lineIdOut = slots[idx].lineId;
    slots[idx] = slots[*count - 1];
    (*count)--;
    return TTD_OK;
}

// Dirty check: undrawn objects, moved anchors, and visibly extending band
// endpoints redraw; settled bands/markers stay clean so archived/offscreen
// objects are not redrawn each bar. An invisible want means the owned object
// must go away (dirty = delete it).
static int TtdOwnedNeedsDraw(const TtdOwnedDraw* slot,
                             const TtdDrawIntent* want, int curAvailBar)
{
    if (slot == nullptr || want == nullptr)
        return 0;
    (void)curAvailBar;
    if (!want->visible)
        return 1;
    if (!slot->drawn)
        return 1;
    switch (slot->kind)
    {
    case TTD_DRAW_BAND:
    case TTD_DRAW_LABEL:
        if (slot->lastStart != want->bandStartAvail)
            return 1;
        if (slot->lastEnd != want->bandEndAvail)
            return 1;
        return 0;
    case TTD_DRAW_CONFIRM_POINT:
        if (!want->hasConfirmPoint)
            return 1;
        return (slot->lastStart != want->confirmAvail) ? 1 : 0;
    case TTD_DRAW_FIRST_DIAMOND:
        if (!want->hasFirstDiamond)
            return 1;
        return (slot->lastStart != want->firstAvail) ? 1 : 0;
    case TTD_DRAW_CROSS_X:
        if (!want->hasCrossX)
            return 1;
        return (slot->lastStart != want->crossAvail) ? 1 : 0;
    default:
        return 1;
    }
}

// =============================================================================
// Task 10: focus detail panel — original evidence vs fresh return measurements
// =============================================================================

// As-of detail record for one episode: frozen original evidence (delta/blocks,
// geometry) kept separate from the latest return's fresh in-zone measurements.
// Derived from the latest transition at or before availBar, so later bars can
// never rewrite it. hasReturn=0 (ordinal 0) means "No return yet" — never a
// zero return-effort claim. No scores, acceptance, entry or inventory fields.
struct TtdDetail
{
    int found;          // 1 when a transition for episodeId <= availBar exists
    int episodeId;
    int dir;
    int state;          // latest stateAfter as of availBar
    int bottomTick;
    int topTick;
    double origDelta;   // original block evidence (frozen at creation)
    int origBlocks;
    int returnOrdinal;  // latest ordinal as of availBar (0 = none yet)
    int hasReturn;      // 1 once ordinal >= 1
    double retAsk;      // fresh in-zone return-bar AskVolume (NOT origDelta)
    double retBid;      // fresh in-zone return-bar BidVolume
    int retBars;        // return-bar count through the latest snapshot
    int penetrationTicks;
    int moveAwayTicks;  // later-close move-away only
    double spanSec;     // return_span_bar_seconds (bar-based, NOT dwell time)
    int spanValid;      // 0 = missing (never clamped into evidence)
    int incomplete;     // persistent per-return VAP-incomplete flag
};

static int TtdDetailAt(const TtdEngine* eng, int episodeId, int availBar,
                       TtdDetail* out)
{
    if (out == nullptr)
        return 0;
    memset(out, 0, sizeof(*out));
    if (eng == nullptr || episodeId <= 0)
        return 0;
    const TtdEpisode* e = TtdFindEpisode(eng, episodeId);
    if (e == nullptr)
        return 0;
    const TtdTransition* latest = nullptr;
    for (int i = 0; i < eng->numTransitions; i++)
    {
        const TtdTransition* t = &eng->transitions[i];
        if (t->episodeId != episodeId || t->availIndex > availBar)
            continue;
        latest = t; // ledger is append-only in availIndex order per episode
    }
    if (latest == nullptr)
        return 0; // pending or not yet knowable: no panel content
    out->found = 1;
    out->episodeId = e->id;
    out->dir = e->dir;
    out->state = latest->stateAfter;
    out->bottomTick = e->bottomTick;
    out->topTick = e->topTick;
    out->origDelta = e->origDelta;
    out->origBlocks = e->origBlocks;
    out->returnOrdinal = latest->returnOrdinal;
    out->hasReturn = (latest->returnOrdinal >= 1) ? 1 : 0;
    out->retAsk = latest->askSum;
    out->retBid = latest->bidSum;
    out->retBars = latest->barCount;
    out->penetrationTicks = latest->penetrationTicks;
    out->moveAwayTicks = latest->moveAwayTicks;
    out->spanSec = latest->spanSec;
    out->spanValid = latest->spanValid;
    out->incomplete = latest->incomplete;
    return 1;
}

// Short state word for the panel header (interpretation only, no verdict).
static const char* TtdDetailStateWord(int state, int ordinal)
{
    switch (state)
    {
    case TTD_CROSSED_THROUGH:
        return "CROSSED";
    case TTD_RETURN_REJECTED:
        return "REJECTED";
    case TTD_RETURN_UNRESOLVED:
        return (ordinal > 1) ? "RETURN N" : "FIRST RETURN";
    case TTD_FAILURE_CONFIRMED:
        return "CONFIRMED";
    case TTD_DISCARDED:
        return "DISCARDED";
    default:
        return "PENDING";
    }
}

// Format the panel into buf (always NUL-terminated). showDelta gates ONLY the
// original-delta line (Show Delta Info); the detail toggle itself is handled
// adapter-side by not drawing at all. Read-only over the engine.
static void TtdDetailText(const TtdDetail* d, int showDelta, char* buf, int n)
{
    if (buf == nullptr || n <= 0)
        return;
    buf[0] = '\0';
    if (d == nullptr || !d->found)
    {
        snprintf(buf, (size_t)n, "No decision selected");
        buf[n - 1] = '\0';
        return;
    }
    const char* side = (d->dir == 1) ? "BUYING FAILED" : "SELLING FAILED";
    int w = snprintf(buf, (size_t)n, "TT #%d %s %s", d->episodeId, side,
                     TtdDetailStateWord(d->state, d->returnOrdinal));
    if (showDelta)
    {
        w += snprintf(buf + (w < n ? w : n - 1),
                      (size_t)(w < n ? n - w : 1),
                      "\nOrig delta %.2f (%d blocks)", d->origDelta,
                      d->origBlocks);
    }
    if (!d->hasReturn)
    {
        w += snprintf(buf + (w < n ? w : n - 1),
                      (size_t)(w < n ? n - w : 1), "\nNo return yet");
    }
    else
    {
        w += snprintf(buf + (w < n ? w : n - 1),
                      (size_t)(w < n ? n - w : 1),
                      "\nReturn %d: ask %.2f bid %.2f net %.2f (%d bars)",
                      d->returnOrdinal, d->retAsk, d->retBid,
                      d->retAsk - d->retBid, d->retBars);
        w += snprintf(buf + (w < n ? w : n - 1),
                      (size_t)(w < n ? n - w : 1),
                      "\nPenetration %d ticks; move-away %d ticks (later closes)",
                      d->penetrationTicks, d->moveAwayTicks);
        if (d->spanValid)
        {
            w += snprintf(buf + (w < n ? w : n - 1),
                          (size_t)(w < n ? n - w : 1),
                          "\nSpan return_span_bar_seconds=%.1f (bar-based, not dwell)",
                          d->spanSec);
        }
        else
        {
            w += snprintf(buf + (w < n ? w : n - 1),
                          (size_t)(w < n ? n - w : 1),
                          "\nSpan return_span_bar_seconds missing (bar-based, not dwell)");
        }
        if (d->incomplete)
        {
            w += snprintf(buf + (w < n ? w : n - 1),
                          (size_t)(w < n ? n - w : 1),
                          "\nVolume incomplete: effort unknown");
        }
    }
    buf[n - 1] = '\0';
    (void)w;
}

// =============================================================================
// Task 11: canonical checked journal + explicit one-shot snapshot (spec §6)
//
// Pure string generation + an injectable file-operation seam. The Sierra
// adapter implements TtdFileOps with FILE*/MoveFileExA and calls the SAME
// TtdJournalSync/TtdSnapshotWrite the portable tests exercise — no parallel
// reimplementation. No FILE*, no STL, no static mutable state in this header.
// =============================================================================

#define TTD_JOURNAL_SCHEMA 1
#define TTD_SNAPSHOT_SCHEMA 1
#define TTD_ERR_IO 6

// Export context: run/instance identity + chart scale + bar-timestamp source.
// tsForBar fills ISO bar timestamps for a transition's calcBar/availIndex:
// startBuf = BaseDateTimeIn[calcBar], endBuf = bar-end time, availBuf =
// BaseDateTimeIn[availIndex] — an availability bound, NOT callback/exec time.
// Out-of-range bars yield empty strings (never sentinel dates). May be null,
// in which case all three timestamp fields are blank (provenance column still
// records the convention).
struct TtdExportCtx
{
    int runId;          // decision run generation (P_DGEN)
    int fpA;            // semantic fingerprint words (settings identity)
    int fpB;
    int chartNumber;
    int instanceId;     // sc.StudyGraphInstanceID
    const char* symbol; // may be null -> blank field
    double tickSize;    // chart TickSize
    void* tsArg;
    void (*tsForBar)(void* arg, int calcBar, int availIndex,
                     char* startBuf, int sn, char* endBuf, int en,
                     char* avBuf, int an);
};

// Categorical kind spelled as a word (never the volatile integer code).
static const char* TtdTransitionKindWord(int kind)
{
    switch (kind)
    {
    case TTD_TR_FAILURE_CONFIRMED:
        return "FAILURE_CONFIRMED";
    case TTD_TR_FIRST_RETURN:
        return "FIRST_RETURN";
    case TTD_TR_RETURN_STARTED:
        return "RETURN_STARTED";
    case TTD_TR_RETURN_REJECTED:
        return "RETURN_REJECTED";
    case TTD_TR_CROSSED:
        return "CROSSED";
    case TTD_TR_DISCARDED:
        return "DISCARDED";
    default:
        return "UNKNOWN";
    }
}

// CSV field copy with minimal RFC-4180 quoting (used for the symbol only;
// numeric/timestamp fields never need it). Always NUL-terminates.
static void TtdCsvField(const char* s, char* buf, int n)
{
    if (buf == nullptr || n <= 0)
        return;
    buf[0] = '\0';
    if (s == nullptr)
        s = "";
    int needQ = 0;
    for (const char* p = s; *p; p++)
    {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
        {
            needQ = 1;
            break;
        }
    }
    // [v3-C7] Reserve room for the closing quote and the terminator up front,
    // so a long value loses trailing characters instead of losing the closing
    // quote — an unterminated quoted field corrupts the rest of the CSV, not
    // just its own cell.
    const int limit = needQ ? (n - 2) : (n - 1);   // last writable index + 1
    int w = 0;
    if (needQ && limit > 0)
        buf[w++] = '"';
    for (const char* p = s; *p && w < limit; p++)
    {
        if (*p == '"' && needQ)
        {
            if (w + 2 > limit)
                break;              // no room for the escaped pair
            buf[w++] = '"';
        }
        buf[w++] = *p;
    }
    if (needQ && w < n - 1)
        buf[w++] = '"';
    buf[(w < n) ? w : n - 1] = '\0';
}

// Journal CSV header. Returns 1 on success, 0 on truncation.
static int TtdJournalHeaderText(char* buf, int n)
{
    if (buf == nullptr || n <= 0)
        return 0;
    const int w = snprintf(
        buf, (size_t)n,
        "schema,run_id,fp_a,fp_b,chart,instance,symbol,ticksize,"
        "event_id,trans_seq,kind,calc_bar,calc_start,calc_end,"
        "avail_index,avail_start,state_after,return_ordinal,dir,"
        "bottom_tick,top_tick,bottom_price,top_price,"
        "orig_delta,orig_blocks,thresh_block,thresh_zone,"
        "ask_sum,bid_sum,bar_count,penetration_ticks,moveaway_ticks,"
        "span_sec,span_valid,incomplete,touch_observed,"
        "timestamp_provenance\n");
    return (w > 0 && w < n) ? 1 : 0;
}

// One journal row for eng->transitions[transIdx]. External keys are
// (run_id,event_id,trans_seq); geometry/evidence are the frozen originals;
// ask/bid/counts/penetration/move-away/span are the immutable transition
// snapshot. Returns TTD_OK, TTD_ERR_ARG, or TTD_ERR_IO (truncation — a
// truncated row must never be written as if complete).
static int TtdJournalRowText(const TtdEngine* eng, int transIdx,
                             const TtdExportCtx* ctx, char* buf, int n)
{
    if (eng == nullptr || ctx == nullptr || buf == nullptr || n <= 0)
        return TTD_ERR_ARG;
    if (transIdx < 0 || transIdx >= eng->numTransitions)
        return TTD_ERR_ARG;
    const TtdTransition* t = &eng->transitions[transIdx];
    const TtdEpisode* e = TtdFindEpisode(eng, t->episodeId);

    char startB[64];
    char endB[64];
    char availB[64];
    startB[0] = '\0';
    endB[0] = '\0';
    availB[0] = '\0';
    if (ctx->tsForBar != nullptr)
        ctx->tsForBar(ctx->tsArg, t->calcBar, t->availIndex, startB,
                      (int)sizeof(startB), endB, (int)sizeof(endB), availB,
                      (int)sizeof(availB));

    char symB[160];
    TtdCsvField(ctx->symbol, symB, (int)sizeof(symB));

    const double tick = (ctx->tickSize > 0.0) ? ctx->tickSize : 0.0;
    const double botP = (double)t->bottomTick * tick;
    const double topP = (double)t->topTick * tick;
    const double origDelta = (e != nullptr) ? e->origDelta : 0.0;
    const int origBlocks = (e != nullptr) ? e->origBlocks : 0;
    const double thrB = (e != nullptr) ? (double)e->threshBlock : 0.0;
    const double thrZ = (e != nullptr) ? (double)e->threshZone : 0.0;
    const char* dirW = (t->dir == 1) ? "BUY" : "SELL";

    const int w = snprintf(
        buf, (size_t)n,
        "%d,%d,%d,%d,%d,%d,%s,%.5f,"
        "%d,%d,%s,%d,%s,%s,"
        "%d,%s,%d,%d,%s,"
        "%d,%d,%.5f,%.5f,"
        "%.2f,%d,%.5f,%.5f,"
        "%.4f,%.4f,%d,%d,%d,"
        "%.3f,%d,%d,%d,"
        "chart-bar-time;avail-is-bound-not-callback\n",
        TTD_JOURNAL_SCHEMA, ctx->runId, ctx->fpA, ctx->fpB, ctx->chartNumber,
        ctx->instanceId, symB, tick,
        t->episodeId, t->seq, TtdTransitionKindWord(t->kind), t->calcBar,
        startB, endB,
        t->availIndex, availB, t->stateAfter, t->returnOrdinal, dirW,
        t->bottomTick, t->topTick, botP, topP,
        origDelta, origBlocks, thrB, thrZ,
        t->askSum, t->bidSum, t->barCount, t->penetrationTicks,
        t->moveAwayTicks,
        t->spanSec, t->spanValid, t->incomplete, t->touchObserved);
    if (w < 0 || w >= n)
    {
        if (n > 0)
            buf[0] = '\0'; // never hand out a truncated row
        return TTD_ERR_IO;
    }
    return TTD_OK;
}

// Injectable file-operation seam. Production passes FILE*-backed callbacks
// (append "ab", truncate "wb", fwrite byte counts, fclose status,
// Windows MoveFileExA / POSIX rename, remove, existence probe); tests pass
// an in-memory fake with failure injection. The sync logic below is shared.
struct TtdFileOps
{
    void* ctx;
    void* (*openAppend)(void* ctx, const char* path);
    void* (*openWrite)(void* ctx, const char* path);
    long (*writeAll)(void* ctx, void* h, const char* data, long len);
    int (*closeOk)(void* ctx, void* h);
    int (*replaceOk)(void* ctx, const char* tmp, const char* dst);
    int (*removePath)(void* ctx, const char* path);
    int (*exists)(void* ctx, const char* path);
};

// Mirrors persistent export watermarks (adapter-owned ints, never heap here).
struct TtdExportState
{
    int lastJournalSeq;  // highest transition seq journaled (0 = none)
    int lastSnapshotReq; // last served snapshot request value
    int exportFailed;    // sticky until a successful checked op or rebuild
};

// Append rows ONLY for transition batches not yet journaled (seq watermark).
// An unchanged callback performs zero file operations — never a per-call
// full rewrite, never a duplicate append. A new file starts with the header.
// Every open/write/close is checked: any failure returns TTD_ERR_IO, leaves
// the watermark unadvanced (retry resumes the missing rows — no claim of a
// complete audit), and sets the sticky exportFailed flag. Disabled journaling
// is a no-op success. Returns TTD_OK, TTD_ERR_ARG, or TTD_ERR_IO.
static int TtdJournalSync(const TtdEngine* eng, TtdExportState* st,
                          const TtdExportCtx* ctx, const TtdFileOps* ops,
                          int journalEnabled, const char* journalPath)
{
    if (eng == nullptr || st == nullptr || ctx == nullptr || ops == nullptr)
        return TTD_ERR_ARG;
    if (!journalEnabled)
        return TTD_OK;
    if (journalPath == nullptr || journalPath[0] == '\0')
        return TTD_ERR_ARG;
    if (ops->openAppend == nullptr || ops->writeAll == nullptr ||
        ops->closeOk == nullptr || ops->exists == nullptr)
        return TTD_ERR_ARG;

    int firstNew = -1;
    for (int i = 0; i < eng->numTransitions; i++)
    {
        if (eng->transitions[i].seq > st->lastJournalSeq)
        {
            firstNew = i;
            break;
        }
    }
    if (firstNew < 0)
        return TTD_OK; // unchanged: zero file operations

    const int isNewFile = !ops->exists(ops->ctx, journalPath);
    void* h = ops->openAppend(ops->ctx, journalPath);
    if (h == nullptr)
    {
        st->exportFailed = 1;
        return TTD_ERR_IO;
    }

    int rc = TTD_OK;
    char line[4096];
    if (isNewFile)
    {
        if (!TtdJournalHeaderText(line, (int)sizeof(line)))
            rc = TTD_ERR_IO;
        else if (ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
                 (long)strlen(line))
            rc = TTD_ERR_IO;
    }
    int hiSeq = st->lastJournalSeq;
    for (int i = firstNew; rc == TTD_OK && i < eng->numTransitions; i++)
    {
        if (TtdJournalRowText(eng, i, ctx, line, (int)sizeof(line)) != TTD_OK)
        {
            rc = TTD_ERR_IO;
            break;
        }
        if (ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
            (long)strlen(line))
        {
            rc = TTD_ERR_IO; // short write: watermark stays behind
            break;
        }
        hiSeq = eng->transitions[i].seq;
    }
    if (!ops->closeOk(ops->ctx, h))
        rc = TTD_ERR_IO; // failed close: buffered bytes may never have landed
    if (rc == TTD_OK)
    {
        st->lastJournalSeq = hiSeq;
        st->exportFailed = 0; // a successful checked op clears the sticky flag
    }
    else
    {
        st->exportFailed = 1;
    }
    return rc;
}

// A request is served on INCREASE only (BuildSpec §2): an unchanged value
// never repeatedly rewrites; zero/negative never writes; a decrease never
// re-requests an old integer (the adapter additionally folds its attempted
// high-water P_DSATT into the scheduling comparison, so an intervening
// decrease after a failed attempt stays quiet). A positive value on
// first use (lastReq == 0) is allowed one snapshot. Excluded from the
// semantic fingerprint: serving a snapshot never resets the ledger.
static int TtdSnapshotDue(int lastReq, int curReq)
{
    if (curReq <= 0)
        return 0;
    return (curReq > lastReq) ? 1 : 0;
}

// One EPISODE record: frozen originals plus latest lifecycle position. Never
// borrows later merged geometry: bottom/top are the creation-time anchor.
static int TtdSnapshotEpisodeText(const TtdEngine* eng, int epIdx,
                                  const TtdExportCtx* ctx, char* buf, int n)
{
    if (eng == nullptr || ctx == nullptr || buf == nullptr || n <= 0)
        return TTD_ERR_ARG;
    if (epIdx < 0 || epIdx >= eng->numEpisodes)
        return TTD_ERR_ARG;
    const TtdEpisode* e = &eng->episodes[epIdx];
    (void)ctx;
    const int w = snprintf(
        buf, (size_t)n,
        "EPISODE,%d,%d,%d,%d,%d,%d,%.2f,%d,%d,%d,%.5f,%.5f,%d,%d,%d,%d,%d,%d\n",
        e->id, e->dir, e->source, e->d, e->bottomTick, e->topTick,
        e->origDelta, e->origBlocks, e->displTicks, e->deadlineBars,
        (double)e->threshBlock, (double)e->threshZone, e->state,
        e->confirmBar, e->returnOrdinal, e->openReturn, e->obsCount,
        e->lastSeenBar);
    if (w < 0 || w >= n)
    {
        if (n > 0)
            buf[0] = '\0';
        return TTD_ERR_IO;
    }
    return TTD_OK;
}

// Serialize the canonical episodes/transitions with settings and completion
// status into tmpPath, then atomically replace finalPath. Always writes —
// even with zero episodes — so an old nonempty export can never linger as
// stale evidence. The only good file is never truncated in place: all bytes
// land in tmp first and finalPath is touched only by replaceOk. Any short
// write, failed close, or failed replace returns TTD_ERR_IO, removes the tmp
// copy (best effort — never presented as success), leaves finalPath
// untouched, and latches exportFailed. Success clears the sticky flag.
// Caller gates on TtdSnapshotDue (one-shot). Returns TTD_OK / TTD_ERR_ARG /
// TTD_ERR_IO.
static int TtdSnapshotWrite(const TtdEngine* eng, TtdExportState* st,
                            const TtdExportCtx* ctx, const TtdFileOps* ops,
                            const char* tmpPath, const char* finalPath)
{
    if (eng == nullptr || st == nullptr || ctx == nullptr || ops == nullptr)
        return TTD_ERR_ARG;
    if (tmpPath == nullptr || tmpPath[0] == '\0' || finalPath == nullptr ||
        finalPath[0] == '\0')
        return TTD_ERR_ARG;
    if (ops->openWrite == nullptr || ops->writeAll == nullptr ||
        ops->closeOk == nullptr || ops->replaceOk == nullptr ||
        ops->removePath == nullptr)
        return TTD_ERR_ARG;

    void* h = ops->openWrite(ops->ctx, tmpPath);
    if (h == nullptr)
    {
        st->exportFailed = 1;
        return TTD_ERR_IO;
    }

    char line[4096];
    char symB[160];
    TtdCsvField(ctx->symbol, symB, (int)sizeof(symB));
    const double tick = (ctx->tickSize > 0.0) ? ctx->tickSize : 0.0;
    const int complete = eng->halted ? 0 : 1;
    int rc = TTD_OK;

    int w = snprintf(
        line, sizeof(line),
        "snapshot,schema,%d,run,%d,fp_a,%d,fp_b,%d,chart,%d,instance,%d,"
        "symbol,%s,ticksize,%.5f,episodes,%d,transitions,%d,"
        "capacity_limited,%d,halted,%d,complete,%d\n",
        TTD_SNAPSHOT_SCHEMA, ctx->runId, ctx->fpA, ctx->fpB, ctx->chartNumber,
        ctx->instanceId, symB, tick, eng->numEpisodes, eng->numTransitions,
        eng->capacityLimited ? 1 : 0, eng->halted ? 1 : 0, complete);
    if (w < 0 || w >= (int)sizeof(line))
        rc = TTD_ERR_IO;
    if (rc == TTD_OK &&
        ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
            (long)strlen(line))
        rc = TTD_ERR_IO;

    if (rc == TTD_OK)
    {
        const char* eph =
            "episode_header,"
            "id,dir,source,d,bottom_tick,top_tick,orig_delta,orig_blocks,"
            "displ_ticks,deadline_bars,thresh_block,thresh_zone,state,"
            "confirm_bar,return_ordinal,open_return,obs_count,last_seen\n";
        if (ops->writeAll(ops->ctx, h, eph, (long)strlen(eph)) !=
            (long)strlen(eph))
            rc = TTD_ERR_IO;
    }
    for (int i = 0; rc == TTD_OK && i < eng->numEpisodes; i++)
    {
        if (TtdSnapshotEpisodeText(eng, i, ctx, line, (int)sizeof(line)) !=
            TTD_OK)
        {
            rc = TTD_ERR_IO;
            break;
        }
        if (ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
            (long)strlen(line))
        {
            rc = TTD_ERR_IO;
            break;
        }
    }
    if (rc == TTD_OK)
    {
        const char* trh = "transitions\n";
        if (ops->writeAll(ops->ctx, h, trh, (long)strlen(trh)) !=
            (long)strlen(trh))
            rc = TTD_ERR_IO;
        else if (TtdJournalHeaderText(line, (int)sizeof(line)) != 1)
            rc = TTD_ERR_IO;
        else if (ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
                 (long)strlen(line))
            rc = TTD_ERR_IO;
    }
    for (int i = 0; rc == TTD_OK && i < eng->numTransitions; i++)
    {
        if (TtdJournalRowText(eng, i, ctx, line, (int)sizeof(line)) != TTD_OK)
        {
            rc = TTD_ERR_IO;
            break;
        }
        if (ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
            (long)strlen(line))
        {
            rc = TTD_ERR_IO;
            break;
        }
    }
    if (rc == TTD_OK)
    {
        w = snprintf(line, sizeof(line), "snapshot_end,episodes,%d,transitions,%d\n",
                     eng->numEpisodes, eng->numTransitions);
        if (w < 0 || w >= (int)sizeof(line))
            rc = TTD_ERR_IO;
        else if (ops->writeAll(ops->ctx, h, line, (long)strlen(line)) !=
                 (long)strlen(line))
            rc = TTD_ERR_IO;
    }

    if (!ops->closeOk(ops->ctx, h))
        rc = TTD_ERR_IO;
    if (rc != TTD_OK)
    {
        ops->removePath(ops->ctx, tmpPath); // never present a partial copy
        st->exportFailed = 1;
        return TTD_ERR_IO;
    }
    if (!ops->replaceOk(ops->ctx, tmpPath, finalPath))
    {
        ops->removePath(ops->ctx, tmpPath);
        st->exportFailed = 1;
        return TTD_ERR_IO;
    }
    st->exportFailed = 0;
    return TTD_OK;
}

// Decision filenames: the legacy filename's stem plus `_decision_`, kind,
// chart number, study-instance ID and run generation. Never the legacy path
// itself (the legacy schema stays separate). Truncation returns TTD_ERR_IO.
static int TtdDecisionFileName(const char* legacyName, const char* kind,
                               int chart, int inst, int runGen,
                               char* buf, int n)
{
    if (legacyName == nullptr || legacyName[0] == '\0' || kind == nullptr ||
        kind[0] == '\0' || buf == nullptr || n <= 0)
        return TTD_ERR_ARG;
    char stem[192];
    size_t li = 0;
    while (legacyName[li] != '\0' && li + 1 < sizeof(stem))
    {
        stem[li] = legacyName[li];
        li++;
    }
    stem[li] = '\0';
    // Strip a trailing extension (".csv") or path separators are kept as-is:
    // only the final dot-suffix is removed.
    int dot = -1;
    for (int i = 0; stem[i] != '\0'; i++)
    {
        if (stem[i] == '.')
            dot = i;
        if (stem[i] == '\\' || stem[i] == '/')
            dot = -1;
    }
    if (dot > 0)
        stem[dot] = '\0';
    if (stem[0] == '\0')
        return TTD_ERR_ARG;
    const int w = snprintf(buf, (size_t)n, "%s_decision_%s_c%d_i%d_run%d.csv",
                           stem, kind, chart, inst, runGen);
    if (w < 0 || w >= n)
    {
        if (n > 0)
            buf[0] = '\0';
        return TTD_ERR_IO;
    }
    return TTD_OK;
}

// Legacy CSV keeps its configured stem and gains a chart/instance suffix so
// two charts (or two instances on one chart) no longer collide on one path.
static int TtdLegacyCsvName(const char* legacyName, int chart, int inst,
                            char* buf, int n)
{
    if (legacyName == nullptr || legacyName[0] == '\0' || buf == nullptr ||
        n <= 0)
        return TTD_ERR_ARG;
    char stem[192];
    size_t li = 0;
    while (legacyName[li] != '\0' && li + 1 < sizeof(stem))
    {
        stem[li] = legacyName[li];
        li++;
    }
    stem[li] = '\0';
    int dot = -1;
    for (int i = 0; stem[i] != '\0'; i++)
    {
        if (stem[i] == '.')
            dot = i;
        if (stem[i] == '\\' || stem[i] == '/')
            dot = -1;
    }
    if (dot > 0)
        stem[dot] = '\0';
    if (stem[0] == '\0')
        return TTD_ERR_ARG;
    const int w = snprintf(buf, (size_t)n, "%s_c%d_i%d.csv", stem, chart,
                           inst);
    if (w < 0 || w >= n)
    {
        if (n > 0)
            buf[0] = '\0';
        return TTD_ERR_IO;
    }
    return TTD_OK;
}

#endif // TRAPPED_TRADERS_V2_DECISION_CORE_H
// === END INLINED TRAPPED TRADERS DECISION CORE ===
//                                            portable episode engine; included
//                                            but not yet called — no behavior
//                                            change while Decision Map is off)
#include <new>      // std::nothrow for the owned decision engine
#include <vector>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <climits>

using std::vector;

// SCDLLName is declared at the top of this file — see the [v3-C8] note there
// for why it cannot live here.

// =============================================================================
// Constants
// =============================================================================

// Threshold modes (In:14)
static const int TMODE_MANUAL = 0;
static const int TMODE_AUTO   = 1;
static const int TMODE_BOTH   = 2;

// Zone source tags
static const int ZSRC_MANUAL = 1;
static const int ZSRC_AUTO   = 2;

// Zone confirmation mode (In:26)
static const int CONF_NEXTBAR  = 0;   // v1: next bar must not OPEN through the zone
static const int CONF_DISPLACE = 1;   // price must CLOSE K ticks beyond the zone

// Shadow confirmation verdicts.  Recorded for BOTH rules on every zone
// regardless of In:26, so a downstream study of the CSV can compare the two
// rules over one identical zone population.  The displacement rule's "discard"
// is recorded as REJECTED as well: the CSV column is a clean three-value
// categorical, and "the zone never earned its place" is the same fact in both
// rules even though the mechanism differs.
static const int CONFOUT_PENDING   = 0;
static const int CONFOUT_CONFIRMED = 1;
static const int CONFOUT_REJECTED  = 2;

// Auto session filter (In:23)
static const int SESS_BOTH = 0;   // whole trading day, no filter
static const int SESS_RTH  = 1;   // regular session only
static const int SESS_ETH  = 2;   // everything outside the regular session

// True when time-of-day t (seconds) falls in [startT, endT).  Handles a range
// that wraps midnight, which the overnight session always does.
static bool IsTimeInRange(int t, int startT, int endT)
{
    if (startT == endT) return true;                 // degenerate: whole day
    if (startT <  endT) return (t >= startT && t < endT);
    return (t >= startT || t < endT);                // wraps midnight
}

// Drawing line-number namespaces.
//
// v1 uses 10000/20000/30000, v2 uses 110000..170002 and v3 uses 210000..270002.
// v4 uses per-instance blocks from 1,000,000 up (see [v4-M2] below), so v1..v4
// can run on the SAME chart for side-by-side comparison
// without any of them deleting or adjusting another's rectangles.  Each study
// also sweeps only its own range on first initialisation.
//
// Line number = base + Slot, where Slot is a RECYCLED index
// (0..MAX_ZONE_SLOTS-1) from a per-source free list — v1 used base + monotonic
// zone ID, which crosses into the next namespace after 10000 zones (reachable
// on a long full recalculation).
//
// [v4-M2] The values below are OFFSETS inside this instance's own block; every
// use goes through StudyLine(sc, ...).  Sierra's ACS drawing line numbers are
// chart-wide, not per study, so with one fixed range two v4 instances on the
// SAME chart (e.g. one Manual, one Auto) drew into and deleted each other's
// rectangles.  Each instance now owns a 100000-wide block keyed by its study
// ID, which the chartbook saves, so the block is stable across sessions.
// Blocks start at 1,000,000, clear of v1..v3 (all below 300000).
static const int BASE_SELL      = 0;       // manual sell rectangles
static const int BASE_BUY       = 10000;   // manual long rectangles
static const int BASE_LBL       = 20000;   // manual delta labels
static const int BASE_AUTO_SELL = 30000;   // auto sell rectangles
static const int BASE_AUTO_BUY  = 40000;   // auto long rectangles
static const int BASE_AUTO_LBL  = 50000;   // auto delta labels
static const int LINE_STATUS    = 60001;   // auto status label
static const int LINE_WARNING   = 60002;   // auto-unavailable warning
static const int LINE_MERGE_ST  = 60003;   // [v4-M1] merge-source problem line
static const int BASE_MERGE     = 70000;   // [v4-M1] merged rectangles
static const int BASE_MERGE_LBL = 80000;   // [v4-M1] merged timeframe/delta labels

static const int LINE_ORIGIN    = 1000000; // [v4-M2] first instance block
static const int LINE_STRIDE    = 100000;  // [v4-M2] block size per study ID

static int StudyLine(SCStudyInterfaceRef sc, int offset)
{
    return LINE_ORIGIN + sc.StudyGraphInstanceID * LINE_STRIDE + offset;
}

static const int MAX_ZONE_SLOTS = 4000;    // per source; namespaces are 10000 apart

// [v4-M1] Zone Display (In:37) and Merge Bounds (In:38).  Order MUST match the
// dropdown strings — the input returns the string INDEX.
static const int ZDISP_OWN      = 0;   // draw this chart's zones (pre-merge behaviour)
static const int ZDISP_HIDDEN   = 1;   // draw nothing; zones still published for merging
static const int ZDISP_MERGED   = 2;   // draw this chart + sources as merged zones
static const int MBOUND_UNION   = 0;   // outer edges of the member zones
static const int MBOUND_OVERLAP = 1;   // band every member shares
static const int MERGE_MAX_SOURCES = 6;

// Persistent storage slots
static const int P_ZONES        = 1;   // vector<ZoneRec>*
static const int P_IDCTR        = 2;   // next zone ID
static const int P_LASTBAR      = 3;   // last processed lastBar index
static const int P_FP1          = 4;   // settings fingerprint hash A
static const int P_FP2          = 5;   // settings fingerprint hash B
static const int P_RESERVED     = 6;   // was v1 fingerprint word 3
static const int P_DAYS         = 7;   // vector<DaySeg>*
static const int P_SLOTS_MAN    = 8;   // vector<char>* manual slot free list
static const int P_SLOTS_AUTO   = 9;   // vector<char>* auto slot free list
static const int P_LOGGED_HIST  = 10;  // insufficient-history already logged
static const int P_STATUS_DRAWN = 11;  // status label currently on chart
static const int P_WARN_DRAWN   = 12;  // warning label currently on chart
static const int P_LOGGED_SLOTS = 13;  // slot exhaustion already logged
static const int P_HIDDEN       = 14;  // study was hidden on the previous call
static const int P_MAXSLOT      = 15;  // highest slot index ever allocated + 1
static const int P_LOGGED_CSV   = 16;  // CSV open failure already logged
// Decision Map slots (v2.1, Phase 2). Int and pointer key spaces are treated
// as shared — legacy avoids pointer keys 1,7,8,9 for ints — so fresh keys
// 17..23 collide with nothing above.
static const int P_DECISION     = 17;  // TtdEngine* (owned; null when off/failed)
static const int P_DFPA         = 18;  // decision semantic fingerprint A
static const int P_DFPB         = 19;  // decision semantic fingerprint B
static const int P_DLASTBAR     = 20;  // last decision-processed closed bar
static const int P_DGEN         = 21;  // decision run generation (rebuilds)
static const int P_DEXPFAIL     = 22;  // sticky decision export I/O failure
static const int P_DLOGGED      = 23;  // incompatible-mode message logged
// Decision Map drawing ownership (v2.1, Phase 3 Task 9). The owner array is a
// single owned POD allocation (no STL persistent members): Sierra allocates
// each non-user drawing ID once (UseTool > 0) and the adapter retains it,
// adjusting/deleting exactly that object. P_DMODE latches whether decision
// rendering owned the screen on the previous call so legacy drawings can be
// hidden without destroying canonical legacy records (OFF restores them).
static const int P_DDRAW        = 24;  // TtdDrawOwnerState* (owned; null when off)
static const int P_DMODE        = 25;  // 1 when decision renderer was active
// Decision Map export watermarks (v2.1, Phase 3 Task 11). Plain ints, no
// cleanup needed: the journal/snapshot files are opened, checked and closed
// within a single callback — no persistent handles are ever held.
static const int P_DJSEQ        = 26;  // highest journaled transition seq
static const int P_DSNAP        = 27;  // last served snapshot request value
// Decision Map visual-refresh watermarks (review-fix-1). Display-only
// controls (MaxNearby, detail panel) are outside both fingerprints, so the
// no-new-bar fast path compares them to decide between a true no-op and a
// visual-only refresh (drawings/panel/files, no semantic/VAP work).
static const int P_DMAXN        = 28;  // last rendered Maximum Nearby value
static const int P_DDETL        = 29;  // last rendered detail-panel toggle
// Decision Map export attempt watermarks (review-fix-2, BuildSpec §6/§7).
// Served watermarks above stay honest (advanced only on success); these
// hold the attempted high-waters so a failed optional export is retried
// at most once per new closed bar / new transitions / request increase
// instead of on every unchanged forming tick. Reset with the generation.
static const int P_DJATT        = 30;  // highest journaled-attempted trans seq
static const int P_DSATT        = 31;  // highest snapshot request attempted
// Decision Map halt-visibility latch (review-fix-3, BuildSpec §3). Plain
// int, no cleanup needed: 1 once the transition-storage halt line was
// logged for the current run generation (exactly one Message Log line per
// generation). Reset with the generation on rebuild.
static const int P_DHALTLOG     = 32;  // halt line already logged (0/1)
// [v3-P4] Decision-inactive subgraph clear latch. With the Decision Map OFF —
// the default — the inactive branch of PASS 3D rewrote 13 subgraphs across the
// WHOLE array on every bar close, which is pure waste that grows with chart
// length. These two remember how much of the array is already carrying the
// inactive pattern, so an ordinary bar close writes only the appended tail.
// P_DCLRS stores (status + 1) so 0 means "never cleared".
static const int P_DCLRN        = 33;  // bars already cleared while inactive
static const int P_DCLRS        = 34;  // (inactive status value + 1), 0 = none
// [v3-P1] Sweep-vs-reference disagreement already logged for this generation.
static const int P_DVFYLOG      = 35;  // sweep cross-check line logged (0/1)
// [v4-M1] Cross-chart merge.  P_MMAGIC / P_MGEN / P_MTICK are PUBLISHED: other
// instances read them with Get*FromChartStudy, so their keys and meaning are
// part of the merge contract between instances of this DLL.
static const int P_MMAGIC       = 40;  // MergeMagic() once zones are published; 0 = not readable
static const int P_MGEN         = 41;  // bumped on every processing call (sources' change counter)
static const int P_MTICK        = 42;  // double: sc.TickSize the published ticks are in
static const int P_MDRAWN       = 43;  // merged rectangles currently drawn (slot high-water)
static const int P_MSIG         = 44;  // source signature the merged drawing was built from
static const int P_MSTDRAWN     = 45;  // merge-source problem line is on the chart
static const int P_MTMODE       = 46;  // [v4-M2] published Threshold Mode + 1 (0 = unknown)

// Per-day sample sufficiency (see BuildSpec §3.4).  A day short of these is
// not usable as a baseline day — the study reports "not ready" rather than
// producing a threshold from a handful of observations.
// Raised from 10/5 now that sampling is per-level/per-bar rather than
// proximity-band: a normal NQ session yields thousands of block samples and
// hundreds of zone samples, so these gates no longer reject ordinary days and
// still catch a day whose VAP data is genuinely missing or truncated.
static const int MIN_BLOCK_SAMPLES_PER_DAY = 100;
static const int MIN_ZONE_SAMPLES_PER_DAY  = 20;

// =============================================================================
// Records
// =============================================================================

struct ZoneRec
{
    int     ID;            // monotonic; diagnostics / determinism only
    int     Slot;          // line-number slot within its source namespace (-1 = none)
    int     TopTick;       // inclusive integer tick bounds — ALL geometry math
    int     BottomTick;
    int     DetectedBar;   // window-end bar of first detection (drawing left edge)
    int     ErasedBar;     // bar where zone was invalidated; -1 = never invalidated
    bool    IsBuyZone;     // true = long zone (neg delta), false = sell zone (pos delta)
    float   TotalDelta;    // negative for long zone, positive for sell zone
    bool    Valid;         // false = rejected before confirmation OR erased after
    bool    Pending;       // true = waiting for next-bar price confirmation
    bool    Drawn;         // a drawing currently exists for this zone
    int     Source;        // ZSRC_MANUAL | ZSRC_AUTO

    // -- Shadow confirmation verdicts (research outputs, never inputs) --------
    // Both rules are scored on every zone whatever In:26 says.  ONLY the rule
    // In:26 selects is allowed to move Pending/Valid; these five fields are
    // written by the shadow tracker in PASS 1 and read only by the CSV export.
    // If either of these ever feeds back into geometry, merging or validity the
    // comparison is worthless — the two rules would no longer be observing the
    // same zone population.
    int     NB_Outcome;    // CONFOUT_* under the next-bar-open rule
    int     NB_Bar;        // bar that decided NB_Outcome; -1 while pending
    int     DP_Outcome;    // CONFOUT_* under the displacement rule
    int     DP_Bar;        // bar that decided DP_Outcome; -1 while pending

    // Qualifying price levels behind the detection, carried from Candidate.
    // A merge keeps the LARGER count, mirroring how fix B6 keeps the larger
    // TotalDelta magnitude: both describe the strongest single-window
    // observation supporting the zone, not an accumulation across windows.
    int     BlockCount;

    // [v3-P3] Highest closed bar PASS 1's inline lifecycle has already resolved
    // this zone against. PASS 2 repeats the same close-through test, and on a
    // full recalculation it restarted at DetectedBar+1 for every zone — an
    // O(zones x bars) rescan of work PASS 1 had just done, bar for bar. PASS 2
    // now starts after CheckedTo, so on a full recalculation it does nothing
    // and on an incremental call it covers only genuinely unchecked bars.
    // Initialised to DetectedBar (nothing checked yet).
    int     CheckedTo;

    // [v4-M3] ID of the zone that absorbed this one when their ranges grew into
    // each other; -1 = never absorbed.  An absorbed record is dead (Valid =
    // false, ErasedBar = -1) and exports as MERGED rather than REJECTED.
    int     AbsorbedInto;

    // [v4-M1] Bar indices mean nothing on another chart, so every processing
    // call stamps each zone with times before PASS 4.  A merging instance on a
    // different timeframe reads these, never DetectedBar / ErasedBar.
    //   StartDT = start of DetectedBar (the left edge this chart draws)
    //   EndDT   = END of ErasedBar, 0 while the zone is live
    SCDateTime StartDT;
    SCDateTime EndDT;
};

// [v4-M1] Identifies a readable zone vector of THIS DLL build.  The struct size
// is folded in so an instance never reads a vector laid out by a different
// build, and a v1/v2 study picked as a source (key never set) reads as 0.
static int MergeMagic()
{
    return (int)(0x54543400u + (unsigned)sizeof(ZoneRec));
}

// One contiguous run of bars belonging to one Sierra Chart trading day.
struct DaySeg
{
    int   DayDate;         // sc.GetTradingDayDate() value (monotonic)
    int   FirstBar;
    int   LastBar;
    bool  Partial;         // segment 0 — chart data may start mid-day

    bool  StatsComputed;
    bool  StatsValid;
    float BlockPct;        // this day's In:16 percentile of its block samples
    float ZonePct;         // this day's In:17 percentile of its zone-total samples

    int   SampleBlockN;    // block-delta samples collected for this day
    int   SampleZoneN;     // zone-total samples collected for this day

    bool  ThreshComputed;
    bool  ThreshReady;
    float AutoMinBlock;    // thresholds ACTIVE during this day
    float AutoZoneTotal;
    int   BaselineDays;    // usable complete preceding days found

    // Why the baseline was refused, so the chart can say so instead of just
    // "NEED 5 COMPLETE DAYS".  Set on the TARGET day by EnsureDayThresholds.
    int   FailReason;      // BFAIL_* below
    int   FailDayIdx;      // which baseline day failed (reasons 2 and 3)
    int   SkippedDays;     // thin segments stepped over (weekends, holidays)
    int   ScannedDays;     // segments examined walking backwards
    int   OldestUsedDay;   // oldest segment index that fed the baseline
};

// [v4-M5] Index of the day segment holding bar bi, or -1.  Binary search: the
// segments are contiguous and ascending by bar.
static int DaySegForBar(const vector<DaySeg>& days, int bi)
{
    int lo = 0;
    int hi = (int)days.size() - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (bi < days[mid].FirstBar)     hi = mid - 1;
        else if (bi > days[mid].LastBar) lo = mid + 1;
        else                             return mid;
    }
    return -1;
}

// [v4-M5] Keep Erased Zones: an erased zone stays on the chart for
// keepSessions trading days COUNTING the day it was erased in, then goes.
//   1 -> rest of the erase day only, gone the next trading day
//   3 -> erased Monday: shown Mon, Tue, Wed; gone Thursday
// Trading days are Sierra's day segments, so weekends and holidays (no bars)
// never count.  keepSessions <= 0 keeps erased zones forever (pre-v4 behaviour).
static bool ErasedBarExpired(const vector<DaySeg>& days, int erasedBar, int keepSessions)
{
    if (keepSessions <= 0 || erasedBar < 0 || days.empty()) return false;
    const int e = DaySegForBar(days, erasedBar);
    if (e < 0) return false;
    return ((int)days.size() - 1 - e) >= keepSessions;
}

// Baseline failure reasons
static const int BFAIL_NONE       = 0;
static const int BFAIL_TOO_FEW    = 1;   // fewer than N preceding day segments
static const int BFAIL_PARTIAL    = 2;   // a baseline day is the partial first segment
static const int BFAIL_INCOMPLETE = 3;   // a baseline day is not complete
static const int BFAIL_SAMPLES    = 4;   // a baseline day had too few VAP samples
static const int BFAIL_DEGENERATE = 5;   // median percentile came out <= 0

// Reusable proximity-band scratch.  Replaces v1's per-window std::map plus
// map->vector copy (Guidelines R4).  Only the two proximity bands are ever
// examined, so the whole window aggregation is O(proxTol) memory.
struct BandScratch
{
    int           HighTick;
    int           LowTick;
    vector<float> HiDelta;     // index i -> tick (HighTick - proxTol + i)
    vector<char>  HiPresent;
    vector<float> LoDelta;     // index i -> tick (LowTick + i)
    vector<char>  LoPresent;
    bool          Valid;

    // Full-window per-price-level aggregation, filled only when the caller asks
    // for it (baseline sampling).  Detection never needs it — it only ever looks
    // at the two proximity bands — so the live path stays as cheap as before.
    bool          FullValid;
    int           FullBaseTick;   // AllDelta[0] is this tick
    int           FullSpan;       // number of valid entries in AllDelta
    vector<float> AllDelta;
    vector<char>  AllPresent;
    vector<float> TmpMag;         // reused sort buffer for the zone-total sample
};

// A window wider than this many ticks is not aggregated in full — a guard
// against a pathological bar range allocating an enormous array.  Normal NQ
// windows are tens of ticks wide.
static const int MAX_WINDOW_TICKS = 4000;

// A detection candidate produced by one side of one window.
struct Candidate
{
    bool  Found;
    int   BottomTick;
    int   TopTick;
    float TotalMagnitude;      // always positive
    int   Count;               // qualifying price levels behind the candidate
};

// =============================================================================
// Small utilities
// =============================================================================

static uint32_t FnvAdd(uint32_t h, const void* data, size_t len)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; i++)
    {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t FnvAddInt(uint32_t h, int v)   { return FnvAdd(h, &v, sizeof(v)); }
static uint32_t FnvAddFloat(uint32_t h, float v)
{
    // Hash the exact bit pattern so sub-0.5 float edits are detected (fix B7).
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return FnvAdd(h, &bits, sizeof(bits));
}

// Percentile of an ASCENDING-sorted vector.
// Rule: linear interpolation between closest ranks (R-7 / Excel PERCENTILE.INC).
//   r = (P/100) * (n-1);  value = v[floor(r)] + frac * (v[ceil(r)] - v[floor(r)])
// Deterministic for a given sample multiset; documented in the BuildSpec.
static float PercentileSorted(const vector<float>& v, float P)
{
    const int n = (int)v.size();
    if (n <= 0) return 0.0f;
    if (n == 1) return v[0];

    if (P < 0.0f)   P = 0.0f;
    if (P > 100.0f) P = 100.0f;

    double r = ((double)P / 100.0) * (double)(n - 1);
    if (r < 0.0)          r = 0.0;
    if (r > (double)(n-1)) r = (double)(n - 1);

    int lo = (int)floor(r);
    int hi = (int)ceil(r);
    if (lo < 0)      lo = 0;
    if (hi > n - 1)  hi = n - 1;

    const double f = r - (double)lo;
    return (float)((double)v[lo] + f * ((double)v[hi] - (double)v[lo]));
}

// Median = P50 under the same rule (even n -> mean of the two central values).
static float MedianOf(vector<float>& v)
{
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return PercentileSorted(v, 50.0f);
}

// Same-source merge test — INCLUSIVE.
//
// Zone bounds are INCLUSIVE integer ticks, so [a,b] and [c,d] overlap iff
// a <= d && c <= b.  v1 used a strict float comparison
// (min < max && min < max), and translating that literally to ticks meant two
// zones covering the IDENTICAL single tick level did not count as overlapping,
// and neither did two zones sharing exactly one boundary tick.  With
// proxTol = 3 a candidate is only 3-4 ticks tall and consecutive windows
// produce near-identical ranges, so most merges were refused and the study
// stacked duplicate rectangles at the same prices instead of merging them.
// Guidelines R1 called for <= here; preserving v1's strict test preserved the
// bug.  Deliberate divergence from v1, documented in the BuildSpec.
static bool TicksOverlapInclusive(int aBot, int aTop, int bBot, int bTop)
{
    return (aBot <= bTop) && (bBot <= aTop);
}

// Cross-source suppression test — INCLUSIVE, per scope: an auto candidate whose
// inclusive tick range intersects a live manual zone is suppressed, so the two
// sources can never share even a single tick level.
static bool TicksIntersectInclusive(int aBot, int aTop, int bBot, int bTop)
{
    return (aBot <= bTop) && (bBot <= aTop);
}

// =============================================================================
// CSV export helpers
// =============================================================================
//
// The consumer is a pandas script, so every field is written in a form pandas
// parses without a converter: ISO timestamps, fixed-width decimals, no thousands
// separators, and categoricals spelled as words rather than as the internal
// integer codes (which are free to change and would silently re-map an old CSV).

// ISO "YYYY-MM-DD HH:MM:SS" for a bar, or "YYYY-MM-DD" when dateOnly.
// An out-of-range bar index yields an EMPTY field rather than a sentinel date:
// a sentinel would parse as a real timestamp and quietly pollute an aggregate.
static void CsvBarDateTime(SCStudyInterfaceRef sc, int bar, bool dateOnly,
                           char* out, size_t outSize)
{
    if (outSize == 0) return;
    out[0] = '\0';
    if (bar < 0 || bar >= sc.ArraySize) return;

    const SCDateTime dt = sc.BaseDateTimeIn[bar];
    if (dateOnly)
        snprintf(out, outSize, "%04d-%02d-%02d",
                 dt.GetYear(), dt.GetMonth(), dt.GetDay());
    else
        snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d",
                 dt.GetYear(), dt.GetMonth(), dt.GetDay(),
                 dt.GetHour(), dt.GetMinute(), dt.GetSecond());
}

static const char* CsvOutcomeText(int outcome)
{
    if (outcome == CONFOUT_CONFIRMED) return "CONFIRMED";
    if (outcome == CONFOUT_REJECTED)  return "REJECTED";
    return "PENDING";
}

// Wraps a field in double quotes and doubles any embedded quote (RFC 4180).
// Applied to every free-text field — the symbol is the realistic carrier of a
// comma, but a bar-period description is user-influenced too.
static void CsvQuoted(const char* s, char* out, size_t outSize)
{
    if (outSize < 3) { if (outSize) out[0] = '\0'; return; }

    size_t w = 0;
    out[w++] = '"';
    if (s)
    {
        for (const char* p = s; *p && w + 2 < outSize; p++)
        {
            if (*p == '"')
            {
                if (w + 3 >= outSize) break;    // no room for the escaped pair
                out[w++] = '"';
            }
            out[w++] = *p;
        }
    }
    out[w++] = '"';
    out[w]   = '\0';
}

// Human-readable bar period, built from the ONLY ACSIL call that reports it
// (sc.GetBarPeriodParameters).  Deliberately covers just the period types this
// study is realistically applied to and falls back to the raw enum value plus
// its parameter for anything else — an unrecognised chart must still produce a
// parseable row rather than an empty column that hides which chart it came from.
static SCString CsvBarPeriodText(SCStudyInterfaceRef sc)
{
    n_ACSIL::s_BarPeriod bp;
    sc.GetBarPeriodParameters(bp);

    SCString out;

    if (bp.ChartDataType == DAILY_DATA)
    {
        out.Format("DAILY_%d", bp.HistoricalChartDaysPerBar);
        return out;
    }

    switch (bp.IntradayChartBarPeriodType)
    {
        case IBPT_DAYS_MINS_SECS:
            out.Format("TIME_%dS", sc.SecondsPerBar);
            break;
        case IBPT_VOLUME_PER_BAR:
            out.Format("VOLUME_%d", bp.IntradayChartBarPeriodParameter1);
            break;
        case IBPT_NUM_TRADES_PER_BAR:
            out.Format("TRADES_%d", bp.IntradayChartBarPeriodParameter1);
            break;
        case IBPT_REVERSAL_IN_TICKS:
            out.Format("REVERSAL_%dT", bp.IntradayChartBarPeriodParameter1);
            break;
        case IBPT_DELTA_VOLUME_PER_BAR:
            out.Format("DELTAVOL_%d", bp.IntradayChartBarPeriodParameter1);
            break;
        default:
            out.Format("TYPE%d_%d", (int)bp.IntradayChartBarPeriodType,
                       bp.IntradayChartBarPeriodParameter1);
            break;
    }
    return out;
}

// =============================================================================
// Decision Map checked file seam (v2.1, Phase 3 Task 11)
// =============================================================================
// Production TtdFileOps — the SAME seam the portable export tests drive with
// an in-memory fake (short write / close / replace injection). Journal opens
// in append mode ("ab") and receives rows for new transition batches only:
// never a per-call full retained rewrite, never a duplicate append.
// Snapshots land in a tmp file ("wb") and reach their final path only via an
// atomic replace — Windows MoveFileExA with REPLACE_EXISTING (rename()
// refuses to overwrite on Windows), POSIX rename elsewhere. The only good
// file is therefore never truncated in place, and a failed snapshot leaves a
// stale partial tmp copy that is removed, never presented as success.
static void* TtdProdOpenAppend(void* ctx, const char* path)
{
    (void)ctx;
    if (path == nullptr || path[0] == '\0')
        return nullptr;
    return (void*)fopen(path, "ab");
}

static void* TtdProdOpenWrite(void* ctx, const char* path)
{
    (void)ctx;
    if (path == nullptr || path[0] == '\0')
        return nullptr;
    return (void*)fopen(path, "wb");
}

static long TtdProdWriteAll(void* ctx, void* h, const char* data, long len)
{
    (void)ctx;
    if (h == nullptr || data == nullptr || len < 0)
        return -1;
    const size_t w = fwrite(data, 1, (size_t)len, (FILE*)h);
    return (long)w;
}

static int TtdProdCloseOk(void* ctx, void* h)
{
    (void)ctx;
    if (h == nullptr)
        return 0;
    return (fclose((FILE*)h) == 0) ? 1 : 0;
}

static int TtdProdReplaceOk(void* ctx, const char* tmp, const char* dst)
{
    (void)ctx;
    if (tmp == nullptr || dst == nullptr)
        return 0;
#ifdef _WIN32
    return MoveFileExA(tmp, dst,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 1
               : 0;
#else
    return (rename(tmp, dst) == 0) ? 1 : 0;
#endif
}

static int TtdProdRemovePath(void* ctx, const char* path)
{
    (void)ctx;
    if (path == nullptr)
        return 0;
    remove(path);
    return 1;
}

static int TtdProdExists(void* ctx, const char* path)
{
    (void)ctx;
    if (path == nullptr || path[0] == '\0')
        return 0;
    FILE* f = fopen(path, "rb");
    if (f == nullptr)
        return 0;
    fclose(f);
    return 1;
}

static TtdFileOps TtdProdFileOps()
{
    TtdFileOps o;
    o.ctx = nullptr;
    o.openAppend = TtdProdOpenAppend;
    o.openWrite = TtdProdOpenWrite;
    o.writeAll = TtdProdWriteAll;
    o.closeOk = TtdProdCloseOk;
    o.replaceOk = TtdProdReplaceOk;
    o.removePath = TtdProdRemovePath;
    o.exists = TtdProdExists;
    return o;
}

// ISO "YYYY-MM-DD HH:MM:SS" for an SCDateTime value (bar-end provenance).
static void TtdIsoDateTime(const SCDateTime& dt, char* out, size_t outSize)
{
    if (outSize == 0)
        return;
    snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d",
             dt.GetYear(), dt.GetMonth(), dt.GetDay(), dt.GetHour(),
             dt.GetMinute(), dt.GetSecond());
}

// TtdExportCtx timestamp source: calc-bar start/end plus the availability
// bound. Out-of-range bars yield empty fields (never sentinel dates); the
// availability start is a deterministic market-time bound, NOT actual
// callback or execution time (spec §6).
static void TtdProdTimestamps(void* arg, int calcBar, int availIndex,
                              char* sBuf, int sn, char* eBuf, int en,
                              char* avBuf, int an)
{
    if (sBuf != nullptr && sn > 0)
        sBuf[0] = '\0';
    if (eBuf != nullptr && en > 0)
        eBuf[0] = '\0';
    if (avBuf != nullptr && an > 0)
        avBuf[0] = '\0';
    if (arg == nullptr)
        return;
    s_sc* psc = (s_sc*)arg;
    if (calcBar >= 0 && calcBar < psc->ArraySize)
    {
        CsvBarDateTime(*psc, calcBar, false, sBuf, (size_t)sn);
        const SCDateTime endDt = psc->GetEndingDateTimeForBarIndex(calcBar);
        TtdIsoDateTime(endDt, eBuf, (size_t)en);
    }
    if (availIndex >= 0 && availIndex < psc->ArraySize)
        CsvBarDateTime(*psc, availIndex, false, avBuf, (size_t)an);
}

// =============================================================================
// Drawing helpers
// =============================================================================

// Fix B3: v1 "deleted" a drawing by re-adding a zero-size transparent rectangle
// WITHOUT AddAsUserDrawnDrawing=1.  User-drawn drawings live in a separate
// line-number namespace, so that call created a new invisible non-user drawing
// and left the real rectangle in place — junk accumulated in the chartbook and
// stale zones survived settings changes until Sierra Chart was restarted.
// Both delete APIs are issued; the one that does not apply is a cheap no-op.
static void DeleteDrawing(SCStudyInterfaceRef sc, int lineNum)
{
    if (lineNum <= 0) return;
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNum);
    sc.DeleteUserDrawnACSDrawing(sc.ChartNumber, lineNum);
}

static int ZoneRectBase(SCStudyInterfaceRef sc, const ZoneRec& z)
{
    if (z.Source == ZSRC_AUTO)
        return StudyLine(sc, z.IsBuyZone ? BASE_AUTO_BUY : BASE_AUTO_SELL);
    return StudyLine(sc, z.IsBuyZone ? BASE_BUY : BASE_SELL);
}

static int ZoneLabelBase(SCStudyInterfaceRef sc, const ZoneRec& z)
{
    return StudyLine(sc, (z.Source == ZSRC_AUTO) ? BASE_AUTO_LBL : BASE_LBL);
}

static void DeleteZoneDrawings(SCStudyInterfaceRef sc, ZoneRec& z)
{
    if (z.Slot < 0) return;
    DeleteDrawing(sc, ZoneRectBase(sc, z)  + z.Slot);
    DeleteDrawing(sc, ZoneLabelBase(sc, z) + z.Slot);
    z.Drawn = false;
}

// Deletes every line number this study can ever own.  Used on full recalc and
// on study removal so nothing can survive a rebuild (verification item 13).
// Deletes every line number this study can own, without consulting the zone
// list.  Used for study removal, hiding, and first initialisation, where the
// zone list is either about to be destroyed or does not describe what is
// actually on the chart (drawings are user-drawn, so they are SAVED IN THE
// CHARTBOOK and come back on reload with no matching record).
//
// upToSlot bounds the sweep: pass the session's high-water mark for the common
// case, or MAX_ZONE_SLOTS when records from an earlier session may exist.
static void DeleteAllStudyDrawings(SCStudyInterfaceRef sc, int upToSlot)
{
    if (upToSlot < 0) upToSlot = 0;
    if (upToSlot > MAX_ZONE_SLOTS) upToSlot = MAX_ZONE_SLOTS;

    for (int s = 0; s < upToSlot; s++)
    {
        DeleteDrawing(sc, StudyLine(sc, BASE_SELL)      + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_BUY)       + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_LBL)       + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_AUTO_SELL) + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_AUTO_BUY)  + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_AUTO_LBL)  + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_MERGE)     + s);   // [v4-M1]
        DeleteDrawing(sc, StudyLine(sc, BASE_MERGE_LBL) + s);
    }
    DeleteDrawing(sc, StudyLine(sc, LINE_STATUS));
    DeleteDrawing(sc, StudyLine(sc, LINE_WARNING));
    DeleteDrawing(sc, StudyLine(sc, LINE_MERGE_ST));
}

// [v4-M1] Removes every merged drawing this instance has on the chart, bounded
// by the slot high-water P_MDRAWN rather than the whole namespace.
static void DeleteMergedDrawings(SCStudyInterfaceRef sc)
{
    const int drawn = min(sc.GetPersistentInt(P_MDRAWN), MAX_ZONE_SLOTS);
    for (int s = 0; s < drawn; s++)
    {
        DeleteDrawing(sc, StudyLine(sc, BASE_MERGE)     + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_MERGE_LBL) + s);
    }
    if (sc.GetPersistentInt(P_MSTDRAWN) != 0)
        DeleteDrawing(sc, StudyLine(sc, LINE_MERGE_ST));
    sc.SetPersistentInt(P_MDRAWN, 0);
    sc.SetPersistentInt(P_MSTDRAWN, 0);
    sc.SetPersistentInt(P_MSIG, 0);
}

// Draws or refreshes a highlight rectangle between two bars.
// DRAWING_RECTANGLEHIGHLIGHT (not EXT) is retained from v1 so EndDateTime is
// respected and the right edge lands exactly where v1 put it — required for the
// "Manual mode matches v1 side by side" verification.
// [v4-M1] Time-anchored form, so merged zones from other timeframes can be
// placed by time; DrawZoneRect below is the bar-index form and is unchanged.
static void DrawZoneRectDT(SCStudyInterfaceRef sc,
                           int lineNum, SCDateTime beginDT, SCDateTime endDT,
                           float botPrice, float topPrice,
                           COLORREF fill, COLORREF border,
                           int width, int transparency, int lockDrawing)
{
    s_UseTool T;
    T.Clear();
    T.ChartNumber            = sc.ChartNumber;
    T.DrawingType            = DRAWING_RECTANGLEHIGHLIGHT;
    T.BeginDateTime          = beginDT;
    T.EndDateTime            = endDT;
    T.BeginValue             = (double)min(botPrice, topPrice);
    T.EndValue               = (double)max(botPrice, topPrice);
    T.Color                  = border;        // outline / border color
    T.SecondaryColor         = fill;          // interior fill color
    T.LineWidth              = (uint16_t)width;
    T.TransparencyLevel      = transparency;
    T.AddMethod              = UTAM_ADD_OR_ADJUST;
    T.LineNumber             = lineNum;
    T.AddAsUserDrawnDrawing  = 1;
    T.AllowCopyToOtherCharts = 1;
    // [v4-M4] User-drawn so "Copy chart drawings" still copies it, locked so
    // it cannot be dragged or edited on the chart.
    T.LockDrawing            = lockDrawing ? 1 : 0;
    sc.UseTool(T);
}

static void DrawZoneRect(SCStudyInterfaceRef sc,
                         int lineNum, int startBar, int endBar,
                         float botPrice, float topPrice,
                         COLORREF fill, COLORREF border,
                         int width, int transparency, int lockDrawing)
{
    DrawZoneRectDT(sc, lineNum,
                   sc.BaseDateTimeIn[startBar], sc.BaseDateTimeIn[endBar],
                   botPrice, topPrice, fill, border, width, transparency,
                   lockDrawing);
}

// Fixed-position status / warning text.
static void DrawStatusText(SCStudyInterfaceRef sc, int lineNum,
                           int horizPos, int vertPos,
                           const SCString& text, COLORREF color, int lockDrawing)
{
    s_UseTool T;
    T.Clear();
    T.ChartNumber                = sc.ChartNumber;
    T.DrawingType                = DRAWING_TEXT;
    T.Region                     = sc.GraphRegion;
    T.AddMethod                  = UTAM_ADD_OR_ADJUST;
    T.LineNumber                 = lineNum;
    T.BeginDateTime              = horizPos;
    T.BeginValue                 = (double)vertPos;
    T.UseRelativeVerticalValues  = 1;
    T.Color                      = color;
    T.FontSize                   = 9;
    T.FontBold                   = 1;
    T.TransparentLabelBackground = 1;
    T.Text                       = text;
    T.AddAsUserDrawnDrawing      = 1;
    T.AllowCopyToOtherCharts     = 0;
    T.LockDrawing                = lockDrawing ? 1 : 0;   // [v4-M4]
    sc.UseTool(T);
}

// =============================================================================
// [v4-M1] Cross-chart zone merge
// =============================================================================
//
// One instance per timeframe, all in one chartbook, all on one symbol.  Every
// instance publishes its zone vector (P_ZONES) together with P_MMAGIC, P_MTICK
// and P_MGEN.  The instance whose Zone Display (In:37) is Merged reads up to six
// other instances with GetPersistentPointerFromChartStudy and draws ONE
// rectangle per group of same-direction zones that overlap in price (widened by
// Merge Gap Tolerance, In:39) AND whose lifetimes overlap in time.
//
//   - Manual and auto zones merge with each other: a zone is a zone.
//   - Sell and long zones never merge: they predict opposite reactions.
//   - A merged zone is LIVE while ANY member is live.  Its bounds come from the
//     live members only, so it shrinks as members are erased, and it dies only
//     once EVERY member has been erased.
//   - A dead merged zone is drawn (dimmed, bounds from all members) only with
//     Show Erased Zones on here.  A source keeps erased records only with its
//     own Show Erased Zones (or CSV export) on, so the history is only as
//     complete as the sources' settings allow.
//
// Reading another instance's vector in place is safe because Sierra runs the
// study functions of every chart on one thread: a source is never mid-update
// while this function runs.  A source that is removed, hidden or rebuilt nulls
// or replaces its pointer (or clears P_MMAGIC), and the pointer is re-read on
// every call and never cached.

static const int MSRC_UNUSED   = 0;   // input left at chart 0 / study 0
static const int MSRC_OK       = 1;
static const int MSRC_SELF     = 2;   // points at this instance — ignored
static const int MSRC_DUP      = 3;   // same chart/study as an earlier source
static const int MSRC_NOTREADY = 4;   // not a v4 study, hidden, or not calculated yet
static const int MSRC_SYMBOL   = 5;   // chart is on a different symbol

static const double MERGE_LIVE_END = 1.0e300;   // EndDT of a live member

struct MergeSource
{
    int                    Chart;
    int                    Study;
    int                    Status;   // MSRC_*
    int                    Gen;      // source's P_MGEN
    int                    TMode;    // source's Threshold Mode + 1 (0 = unknown)
    double                 Tick;     // tick size the source's ticks are in
    const vector<ZoneRec>* Zones;
    char                   Label[16];
};

struct MergeMember
{
    int    BotTick;      // in THIS chart's ticks
    int    TopTick;
    bool   IsBuy;
    bool   Live;
    double StartDT;
    double EndDT;        // MERGE_LIVE_END while live
    float  AbsDelta;
    int    SrcIdx;       // 0 = this chart, 1..6 = Merge Source n
};

struct MergeAcc
{
    int      N;
    int      MinBot, MaxTop;   // union bounds
    int      MaxBot, MinTop;   // overlap bounds
    float    MaxDelta;
    unsigned SrcMask;
};

struct MergeCluster
{
    bool     IsBuy;
    bool     Live;
    int      BotTick;
    int      TopTick;
    double   StartDT;     // earliest member start
    double   EndDT;       // latest member end; used only when !Live
    float    MaxDelta;    // largest single-member |delta| (fix B6 rule: never summed)
    unsigned SrcMask;     // bit k = source k contributed
};

struct MergeDrawCfg
{
    COLORREF SellFill, SellBord, BuyFill, BuyBord;
    int      Width;
    int      Transparency;
    int      ShowDelta;
    int      ShowErased;
    int      BoundsMode;   // MBOUND_*
    int      GapTicks;
    int      Lock;         // [v4-M4] Lock Zones
    COLORREF ErFill, ErBord;   // [v4-M5] erased zones, both directions
    int      KeepErased;   // [v4-M5] sessions; 0 = forever
    const vector<DaySeg>* Days;   // [v4-M5] this chart's day segments
};

// Short timeframe label: "30s", "5m", "1h", "1D" for time bars, "#<chart>"
// for everything else (number, volume, range bars...).
static void MergeChartLabel(SCStudyInterfaceRef sc, int chart, char* out, size_t n)
{
    n_ACSIL::s_BarPeriod bp;
    sc.GetBarPeriodParametersForChart(chart, bp);
    const int s = bp.IntradayChartBarPeriodParameter1;
    if (bp.ChartDataType == INTRADAY_DATA &&
        bp.IntradayChartBarPeriodType == IBPT_DAYS_MINS_SECS && s > 0)
    {
        if      (s % 86400 == 0) snprintf(out, n, "%dD", s / 86400);
        else if (s % 3600  == 0) snprintf(out, n, "%dh", s / 3600);
        else if (s % 60    == 0) snprintf(out, n, "%dm", s / 60);
        else                     snprintf(out, n, "%ds", s);
    }
    else
        snprintf(out, n, "#%d", chart);
}

// src[0] = this instance; src[1..MERGE_MAX_SOURCES] = Merge Source inputs.
static void MergeResolveSources(SCStudyInterfaceRef sc,
                                const vector<ZoneRec>* own,
                                const int* charts, const int* studies,
                                MergeSource* src)
{
    src[0].Chart  = sc.ChartNumber;
    src[0].Study  = sc.StudyGraphInstanceID;
    src[0].Status = MSRC_OK;
    src[0].Gen    = sc.GetPersistentInt(P_MGEN);
    src[0].TMode  = sc.GetPersistentInt(P_MTMODE);
    src[0].Tick   = sc.TickSize;
    src[0].Zones  = own;
    MergeChartLabel(sc, sc.ChartNumber, src[0].Label, sizeof(src[0].Label));

    for (int k = 1; k <= MERGE_MAX_SOURCES; k++)
    {
        MergeSource& m = src[k];
        m.Chart    = charts[k - 1];
        m.Study    = studies[k - 1];
        m.Status   = MSRC_UNUSED;
        m.Gen      = 0;
        m.TMode    = 0;
        m.Tick     = 0.0;
        m.Zones    = nullptr;
        m.Label[0] = '\0';
        if (m.Chart <= 0 || m.Study <= 0) continue;

        MergeChartLabel(sc, m.Chart, m.Label, sizeof(m.Label));

        if (m.Chart == sc.ChartNumber && m.Study == sc.StudyGraphInstanceID)
        {
            m.Status = MSRC_SELF;
            continue;
        }

        bool dup = false;
        for (int j = 1; j < k; j++)
            if (src[j].Chart == m.Chart && src[j].Study == m.Study) dup = true;
        if (dup) { m.Status = MSRC_DUP; continue; }

        const SCString srcSym = sc.GetChartSymbol(m.Chart);
        if (strcmp(srcSym.GetChars(), sc.Symbol.GetChars()) != 0)
        {
            m.Status = MSRC_SYMBOL;
            continue;
        }

        const int magic = sc.GetPersistentIntFromChartStudy(m.Chart, m.Study, P_MMAGIC);
        const vector<ZoneRec>* pz = (const vector<ZoneRec>*)
            sc.GetPersistentPointerFromChartStudy(m.Chart, m.Study, P_ZONES);
        const double tick = sc.GetPersistentDoubleFromChartStudy(m.Chart, m.Study, P_MTICK);
        if (magic != MergeMagic() || pz == nullptr || tick <= 0.0)
        {
            m.Status = MSRC_NOTREADY;
            continue;
        }

        m.Status = MSRC_OK;
        m.Gen    = sc.GetPersistentIntFromChartStudy(m.Chart, m.Study, P_MGEN);
        m.TMode  = sc.GetPersistentIntFromChartStudy(m.Chart, m.Study, P_MTMODE);
        m.Tick   = tick;
        m.Zones  = pz;
    }

    // [v4-M2] Two instances on the SAME chart (e.g. one Manual, one Auto) would
    // both label as "5m".  Suffix each with its threshold mode — "5m-M",
    // "5m-A", "5m-MA" (Both) — so the label says which of them agree.
    for (int k = 0; k <= MERGE_MAX_SOURCES; k++)
    {
        if (src[k].Status != MSRC_OK) continue;
        bool shared = false;
        for (int j = 0; j <= MERGE_MAX_SOURCES; j++)
            if (j != k && src[j].Status == MSRC_OK && src[j].Chart == src[k].Chart)
                shared = true;
        if (!shared) continue;
        const char* sfx = (src[k].TMode == TMODE_MANUAL + 1) ? "-M"
                        : (src[k].TMode == TMODE_AUTO   + 1) ? "-A"
                        : (src[k].TMode == TMODE_BOTH   + 1) ? "-MA"
                        : "";
        const size_t len = strlen(src[k].Label);
        snprintf(src[k].Label + len, sizeof(src[k].Label) - len, "%s", sfx);
    }
}

// Changes whenever any source is re-pointed, changes status or processes a bar.
// Never 0, so 0 can mean "not built yet".
static int MergeSignature(const MergeSource* src)
{
    uint32_t h = 2166136261u;
    for (int k = 0; k <= MERGE_MAX_SOURCES; k++)
    {
        h = FnvAddInt(h, src[k].Chart);
        h = FnvAddInt(h, src[k].Study);
        h = FnvAddInt(h, src[k].Status);
        h = FnvAddInt(h, src[k].Gen);
    }
    return (int)(h | 1u);
}

static void MergeCollect(SCStudyInterfaceRef sc, const vector<ZoneRec>& zs,
                         double srcTick, int srcIdx, vector<MergeMember>& out)
{
    const double ratio = srcTick / sc.TickSize;
    const bool   same  = fabs(ratio - 1.0) < 1e-9;

    for (size_t i = 0; i < zs.size(); i++)
    {
        const ZoneRec& z = zs[i];
        if (z.Pending) continue;                     // not confirmed yet
        if (!z.Valid && z.ErasedBar < 0) continue;   // rejected — never a zone
        const double start = z.StartDT.GetAsDouble();
        if (start <= 0.0) continue;                  // not stamped yet

        MergeMember m;
        if (same)
        {
            m.BotTick = z.BottomTick;
            m.TopTick = z.TopTick;
        }
        else
        {
            m.BotTick = (int)floor(z.BottomTick * ratio + 0.5);
            m.TopTick = (int)floor(z.TopTick    * ratio + 0.5);
        }
        if (m.TopTick < m.BotTick) std::swap(m.TopTick, m.BotTick);
        m.IsBuy    = z.IsBuyZone;
        m.Live     = z.Valid;
        m.StartDT  = start;
        m.EndDT    = z.Valid ? MERGE_LIVE_END : z.EndDT.GetAsDouble();
        if (m.EndDT < m.StartDT) m.EndDT = m.StartDT;
        m.AbsDelta = (float)fabs(z.TotalDelta);
        m.SrcIdx   = srcIdx;
        out.push_back(m);
    }
}

static void MergeAccInit(MergeAcc& a)
{
    a.N        = 0;
    a.MinBot   = INT_MAX;
    a.MaxTop   = INT_MIN;
    a.MaxBot   = INT_MIN;
    a.MinTop   = INT_MAX;
    a.MaxDelta = 0.0f;
    a.SrcMask  = 0;
}

static void MergeAccAdd(MergeAcc& a, const MergeMember& m)
{
    a.N++;
    a.MinBot   = min(a.MinBot, m.BotTick);
    a.MaxTop   = max(a.MaxTop, m.TopTick);
    a.MaxBot   = max(a.MaxBot, m.BotTick);
    a.MinTop   = min(a.MinTop, m.TopTick);
    a.MaxDelta = max(a.MaxDelta, m.AbsDelta);
    a.SrcMask |= (1u << m.SrcIdx);
}

static void MergeAccBounds(const MergeAcc& a, int mode, int& bot, int& top)
{
    if (mode == MBOUND_OVERLAP)
    {
        bot = a.MaxBot;
        top = a.MinTop;
        // No band is shared by every member when they were joined through the
        // gap tolerance, or through a chain (A-B and B-C overlap, A-C do not).
        // Fall back to the band between the inner edges: the tightest band
        // that still touches every member.
        if (bot > top) std::swap(bot, top);
    }
    else
    {
        bot = a.MinBot;
        top = a.MaxTop;
    }
}

static int MergeFind(vector<int>& parent, int x)
{
    while (parent[x] != x)
    {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

// Groups members into merged zones.  Two members are linked when they share a
// direction, overlap in price (within gapTicks) and overlap in time; a merged
// zone is a connected group of links.
static void MergeBuildClusters(vector<MergeMember>& m, int gapTicks, int boundsMode,
                               vector<MergeCluster>& out)
{
    out.clear();
    const int n = (int)m.size();
    if (n == 0) return;

    std::sort(m.begin(), m.end(), [](const MergeMember& a, const MergeMember& b)
    {
        if (a.IsBuy   != b.IsBuy)   return a.IsBuy < b.IsBuy;
        if (a.BotTick != b.BotTick) return a.BotTick < b.BotTick;
        if (a.TopTick != b.TopTick) return a.TopTick < b.TopTick;
        if (a.StartDT != b.StartDT) return a.StartDT < b.StartDT;
        return a.SrcIdx < b.SrcIdx;
    });

    vector<int> parent(n);
    for (int i = 0; i < n; i++) parent[i] = i;

    // Sorted by bottom, so for j > i the price test reduces to
    // bot[j] <= top[i] + gap (bot[i] <= bot[j] <= top[j] covers the other
    // half), and the inner loop stops at the first j that fails it.
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (m[j].IsBuy != m[i].IsBuy) break;
            if (m[j].BotTick > m[i].TopTick + gapTicks) break;
            if (m[i].StartDT <= m[j].EndDT && m[j].StartDT <= m[i].EndDT)
            {
                const int a = MergeFind(parent, i);
                const int b = MergeFind(parent, j);
                if (a != b) parent[b] = a;
            }
        }
    }

    vector<int>      clusterOf(n, -1);
    vector<MergeAcc> liveAcc, allAcc;
    for (int i = 0; i < n; i++)
    {
        const int r = MergeFind(parent, i);
        if (clusterOf[r] < 0)
        {
            clusterOf[r] = (int)out.size();
            MergeCluster c;
            c.IsBuy    = m[i].IsBuy;
            c.Live     = false;
            c.BotTick  = 0;
            c.TopTick  = 0;
            c.StartDT  = m[i].StartDT;
            c.EndDT    = 0.0;
            c.MaxDelta = 0.0f;
            c.SrcMask  = 0;
            out.push_back(c);
            MergeAcc a;
            MergeAccInit(a);
            liveAcc.push_back(a);
            allAcc.push_back(a);
        }
        const int ci = clusterOf[r];
        MergeCluster& c = out[ci];
        if (m[i].StartDT < c.StartDT) c.StartDT = m[i].StartDT;
        if (!m[i].Live && m[i].EndDT > c.EndDT) c.EndDT = m[i].EndDT;
        MergeAccAdd(allAcc[ci], m[i]);
        if (m[i].Live) MergeAccAdd(liveAcc[ci], m[i]);
    }

    for (size_t ci = 0; ci < out.size(); ci++)
    {
        MergeCluster& c = out[ci];
        c.Live = (liveAcc[ci].N > 0);
        // Live: bounds from live members only (shrinks as members die).
        // Dead: bounds from every member.
        const MergeAcc& a = c.Live ? liveAcc[ci] : allAcc[ci];
        MergeAccBounds(a, boundsMode, c.BotTick, c.TopTick);
        c.MaxDelta = a.MaxDelta;
        c.SrcMask  = a.SrcMask;
    }
}

// Rebuilds every merged drawing from the current state of all sources.  Slots
// are reassigned on every call (live zones first), and any slot above the new
// count is deleted, so no identity has to be tracked across calls.
static void RenderMergedZones(SCStudyInterfaceRef sc, const MergeSource* src,
                              const MergeDrawCfg& cfg)
{
    vector<MergeMember> mem;
    for (int k = 0; k <= MERGE_MAX_SOURCES; k++)
        if (src[k].Status == MSRC_OK && src[k].Zones != nullptr)
            MergeCollect(sc, *src[k].Zones, src[k].Tick, k, mem);

    vector<MergeCluster> all;
    MergeBuildClusters(mem, cfg.GapTicks, cfg.BoundsMode, all);

    // Live first (oldest first), then finished ones newest first, so a slot
    // shortage drops the oldest history and never a live zone.
    vector<MergeCluster> cl;
    vector<MergeCluster> dead;
    cl.reserve(all.size());
    for (size_t i = 0; i < all.size(); i++)
    {
        if (all[i].Live)
        {
            cl.push_back(all[i]);
        }
        else if (cfg.ShowErased)
        {
            // [v4-M5] Expiry by THIS chart's trading days: the bar holding the
            // cluster's end time is the bar it was erased on here.
            if (cfg.KeepErased > 0 && cfg.Days != nullptr)
            {
                const int eb = sc.GetContainingIndexForSCDateTime(sc.ChartNumber,
                                                                  SCDateTime(all[i].EndDT));
                if (ErasedBarExpired(*cfg.Days, eb, cfg.KeepErased)) continue;
            }
            dead.push_back(all[i]);
        }
    }
    std::sort(cl.begin(), cl.end(), [](const MergeCluster& a, const MergeCluster& b)
    {
        if (a.StartDT != b.StartDT) return a.StartDT < b.StartDT;
        if (a.IsBuy   != b.IsBuy)   return a.IsBuy < b.IsBuy;
        return a.BotTick < b.BotTick;
    });
    std::sort(dead.begin(), dead.end(), [](const MergeCluster& a, const MergeCluster& b)
    {
        if (a.EndDT   != b.EndDT)   return a.EndDT > b.EndDT;
        if (a.IsBuy   != b.IsBuy)   return a.IsBuy < b.IsBuy;
        return a.BotTick < b.BotTick;
    });
    cl.insert(cl.end(), dead.begin(), dead.end());
    if ((int)cl.size() > MAX_ZONE_SLOTS) cl.resize(MAX_ZONE_SLOTS);

    const int        count     = (int)cl.size();
    const SCDateTime rightEdge = sc.BaseDateTimeIn[sc.ArraySize - 1];

    for (int s = 0; s < count; s++)
    {
        const MergeCluster& c = cl[s];
        COLORREF fill   = c.IsBuy ? cfg.BuyFill : cfg.SellFill;
        COLORREF border = c.IsBuy ? cfg.BuyBord : cfg.SellBord;
        if (!c.Live)   // [v4-M5] erased colours
        {
            fill   = cfg.ErFill;
            border = cfg.ErBord;
        }
        const float botPrice  = (float)(c.BotTick * sc.TickSize);
        const float topPrice  = (float)(c.TopTick * sc.TickSize);

        double endD = c.Live ? rightEdge.GetAsDouble() : c.EndDT;
        if (endD < c.StartDT) endD = c.StartDT;
        const int transp = c.Live ? cfg.Transparency : min(cfg.Transparency + 30, 95);

        DrawZoneRectDT(sc, StudyLine(sc, BASE_MERGE) + s, SCDateTime(c.StartDT), SCDateTime(endD),
                       botPrice, topPrice, fill, border, cfg.Width, transp,
                       cfg.Lock);

        // Label on live merged zones: contributing timeframes, plus the
        // largest single-member delta when Show Delta Info is on.
        if (c.Live)
        {
            char buf[128];
            size_t used = 0;
            buf[0] = '\0';
            for (int k = 0; k <= MERGE_MAX_SOURCES; k++)
            {
                if ((c.SrcMask & (1u << k)) == 0) continue;
                const int w = snprintf(buf + used, sizeof(buf) - used, "%s%s",
                                       used > 0 ? "+" : "", src[k].Label);
                if (w < 0 || (size_t)w >= sizeof(buf) - used) break;
                used += (size_t)w;
            }
            if (cfg.ShowDelta && used < sizeof(buf))
                snprintf(buf + used, sizeof(buf) - used, "  %.0f", c.MaxDelta);

            s_UseTool T;
            T.Clear();
            T.ChartNumber            = sc.ChartNumber;
            T.DrawingType            = DRAWING_TEXT;
            T.BeginDateTime          = SCDateTime(c.StartDT);
            T.BeginValue             = c.IsBuy ? (double)botPrice : (double)topPrice;
            T.Text                   = buf;
            T.Color                  = COLOR_WHITE;
            T.FontSize               = 8;
            T.AddMethod              = UTAM_ADD_OR_ADJUST;
            T.LineNumber             = StudyLine(sc, BASE_MERGE_LBL) + s;
            T.TransparencyLevel      = 75;
            T.AddAsUserDrawnDrawing  = 1;
            T.AllowCopyToOtherCharts = 1;
            T.LockDrawing            = cfg.Lock ? 1 : 0;   // [v4-M4]
            sc.UseTool(T);
        }
        else
        {
            DeleteDrawing(sc, StudyLine(sc, BASE_MERGE_LBL) + s);
        }
    }

    const int prevDrawn = min(sc.GetPersistentInt(P_MDRAWN), MAX_ZONE_SLOTS);
    for (int s = count; s < prevDrawn; s++)
    {
        DeleteDrawing(sc, StudyLine(sc, BASE_MERGE)     + s);
        DeleteDrawing(sc, StudyLine(sc, BASE_MERGE_LBL) + s);
    }
    sc.SetPersistentInt(P_MDRAWN, count);

    // Problem line: only drawn while a configured source cannot be read, so a
    // healthy setup adds nothing to the chart.
    SCString problems;
    for (int k = 1; k <= MERGE_MAX_SOURCES; k++)
    {
        const char* why = nullptr;
        if      (src[k].Status == MSRC_NOTREADY) why = "not a v4 study, hidden, or not calculated yet";
        else if (src[k].Status == MSRC_SYMBOL)   why = "different symbol";
        else if (src[k].Status == MSRC_DUP)      why = "duplicate of an earlier source";
        if (why == nullptr) continue;
        SCString line;
        line.Format("%sSource %d (chart #%d): %s", problems.GetLength() > 0 ? "  |  " : "",
                    k, src[k].Chart, why);
        problems += line;
    }
    if (problems.GetLength() > 0)
    {
        SCString txt;
        txt.Format("Merge: %s", problems.GetChars());
        DrawStatusText(sc, StudyLine(sc, LINE_MERGE_ST), 2, 85, txt, RGB(255, 90, 90),
                       cfg.Lock);
        sc.SetPersistentInt(P_MSTDRAWN, 1);
    }
    else if (sc.GetPersistentInt(P_MSTDRAWN) != 0)
    {
        DeleteDrawing(sc, StudyLine(sc, LINE_MERGE_ST));
        sc.SetPersistentInt(P_MSTDRAWN, 0);
    }

    sc.SetPersistentInt(P_MSIG, MergeSignature(src));
}

// =============================================================================
// Slot allocation (line-number namespace management)
// =============================================================================

static int AllocSlot(SCStudyInterfaceRef sc, vector<char>& used)
{
    // First-fit scan is the portable-tested TtdSlotAlloc (Task 8); only the
    // Sierra high-water mark stays adapter-side.
    const int slot = TtdSlotAlloc(used.empty() ? nullptr : used.data(),
                                  (int)used.size());
    if (slot < 0)
        return -1;
    // High-water mark, so removal/hide can sweep only the range that was
    // actually used instead of all MAX_ZONE_SLOTS line numbers.
    if (slot + 1 > sc.GetPersistentInt(P_MAXSLOT))
        sc.SetPersistentInt(P_MAXSLOT, slot + 1);
    return slot;
}

static void FreeSlot(vector<char>& used, int slot)
{
    TtdSlotFree(used.empty() ? nullptr : used.data(), (int)used.size(), slot);
}

// =============================================================================
// Decision Map drawing ownership (v2.1, Phase 3 Task 9)
// =============================================================================

// Single owned POD allocation (no STL persistent members) holding this
// instance's automatic non-user drawing IDs. Separate study instances keep
// separate allocations, so one instance can never adjust/delete another's IDs.
struct TtdDrawOwnerState
{
    TtdOwnedDraw slots[TTD_DRAW_OWNER_MAX];
    int count;
    // Focus detail panel (v2.1, Phase 3 Task 10): one instance-owned
    // non-user text drawing for the first selected episode. The last drawn
    // text is cached so settled content is never redrawn each bar; the
    // detail/show-delta toggles only gate this drawing and never touch the
    // canonical episode/transition records.
    int panelLineId;      // Sierra-allocated LineNumber (> 0); 0 = none
    int panelDrawn;       // 1 once successfully drawn
    char panelText[512];  // last drawn text (dirty comparison)
};

static TtdDrawOwnerState* TtdGetDrawOwner(SCStudyInterfaceRef sc)
{
    return (TtdDrawOwnerState*)sc.GetPersistentPointer(P_DDRAW);
}

// Delete exactly the owned IDs (single-object deletes; never TOOL_DELETE_ALL)
// and recycle every slot. Used when leaving decision mode, hiding, or
// removing the study. Canonical episode/transition records are untouched.
static void TtdDeleteOwnedDrawings(SCStudyInterfaceRef sc, TtdDrawOwnerState* o)
{
    if (o == nullptr)
        return;
    for (int i = 0; i < o->count; i++)
        DeleteDrawing(sc, o->slots[i].lineId);
    o->count = 0;
    if (o->panelDrawn && o->panelLineId > 0)
        DeleteDrawing(sc, o->panelLineId);
    o->panelLineId = 0;
    o->panelDrawn = 0;
    o->panelText[0] = '\0';
}

// Compact state label: BUYING/SELLING FAILED plus FIRST RETURN / RETURN N /
// RETURN REJECTED / CROSSED ABOVE-or-BELOW (spec §7). Interpretations only;
// executions do not identify positions.
static void TtdDecisionLabel(const TtdDrawIntent& w, char* buf, int n)
{
    if (buf == nullptr || n <= 0)
        return;
    const char* side = (w.dir == 1) ? "BUYING FAILED" : "SELLING FAILED";
    if (w.hasCrossX)
    {
        const char* cross = (w.dir == 1) ? "CROSSED BELOW" : "CROSSED ABOVE";
        snprintf(buf, (size_t)n, "%s %s", side, cross);
    }
    else if (w.state == TTD_RETURN_REJECTED)
    {
        snprintf(buf, (size_t)n, "%s RETURN REJECTED", side);
    }
    else if (w.returnOrdinal > 1)
    {
        snprintf(buf, (size_t)n, "%s RETURN %d", side, w.returnOrdinal);
    }
    else if (w.returnOrdinal == 1)
    {
        snprintf(buf, (size_t)n, "%s FIRST RETURN", side);
    }
    else
    {
        snprintf(buf, (size_t)n, "%s", side);
    }
    buf[n - 1] = '\0';
}

// Allocates a slot, evicting the OLDEST retained erased zone of the same source
// if the pool is full.  Only reachable in "Show Erased Zones" mode on a very
// long chart, where erased zones are retained for display and would otherwise
// exhaust the pool and silently stop drawing new zones.  Degrading the far past
// is preferable to losing current signals; the evicted record is marked so the
// next prune removes it entirely.
//
// retainForExport: with CSV export on, records are never removed, so the victim
// must NOT be re-tagged as rejected — that would report an ERASED zone as
// REJECTED in the export purely because the drawing pool ran dry.  It keeps its
// ErasedBar and simply loses its slot; PASS 4 then legitimately counts it as a
// zone that wanted to be drawn and could not, which is what the slot-exhaustion
// warning is for.
static int AllocSlotEvicting(SCStudyInterfaceRef sc, vector<ZoneRec>& zones,
                             vector<char>& used, int source, bool retainForExport)
{
    int slot = AllocSlot(sc, used);
    if (slot >= 0) return slot;

    int victim   = -1;
    int oldestBar = INT_MAX;
    for (size_t i = 0; i < zones.size(); i++)
    {
        ZoneRec& z = zones[i];
        if (z.Source != source) continue;
        if (z.Valid || z.ErasedBar < 0) continue;   // only retained ERASED zones
        if (z.Slot < 0) continue;
        if (z.DetectedBar < oldestBar) { oldestBar = z.DetectedBar; victim = (int)i; }
    }
    if (victim < 0) return -1;

    ZoneRec& v = zones[victim];
    DeleteZoneDrawings(sc, v);
    FreeSlot(used, v.Slot);
    v.Slot      = -1;
    v.Valid     = false;
    if (!retainForExport)
        v.ErasedBar = -1;  // marks the record for removal at the next prune

    return AllocSlot(sc, used);
}

// =============================================================================
// Window aggregation
// =============================================================================

// Builds the two proximity bands for the window [winStart..winEnd].
// Each bar's VolumeAtPrice is iterated exactly once.  A band slot no VAP
// element ever touched stays Present=false and is skipped by every consumer —
// this reproduces v1's behaviour of only considering price levels that exist
// in the aggregated map (matters when a threshold is 0).
static void BuildBands(SCStudyInterfaceRef sc, int winStart, int winEnd,
                       int proxTol, BandScratch& B, bool needFullWindow)
{
    const int nSlots = proxTol + 1;

    B.Valid     = false;
    B.FullValid = false;
    if (winStart < 0 || winEnd < winStart) return;

    if ((int)B.HiDelta.size() != nSlots)
    {
        B.HiDelta.assign(nSlots, 0.0f);
        B.HiPresent.assign(nSlots, 0);
        B.LoDelta.assign(nSlots, 0.0f);
        B.LoPresent.assign(nSlots, 0);
    }
    else
    {
        std::fill(B.HiDelta.begin(),   B.HiDelta.end(),   0.0f);
        std::fill(B.HiPresent.begin(), B.HiPresent.end(), (char)0);
        std::fill(B.LoDelta.begin(),   B.LoDelta.end(),   0.0f);
        std::fill(B.LoPresent.begin(), B.LoPresent.end(), (char)0);
    }

    float wHigh = -1e30f;
    float wLow  =  1e30f;
    for (int bi = winStart; bi <= winEnd; bi++)
    {
        if (sc.High[bi] > wHigh) wHigh = sc.High[bi];
        if (sc.Low[bi]  < wLow)  wLow  = sc.Low[bi];
    }
    if (wHigh < wLow) return;

    B.HighTick = (int)round(wHigh / sc.TickSize);
    B.LowTick  = (int)round(wLow  / sc.TickSize);

    const int hiFloor = B.HighTick - proxTol;   // band = [hiFloor .. HighTick]
    const int loCeil  = B.LowTick  + proxTol;   // band = [LowTick .. loCeil]

    // Optional full-window aggregation for baseline sampling.
    int fullSpan = 0;
    if (needFullWindow)
    {
        fullSpan = B.HighTick - B.LowTick + 1;
        if (fullSpan > 0 && fullSpan <= MAX_WINDOW_TICKS)
        {
            B.FullBaseTick = B.LowTick;
            if ((int)B.AllDelta.size() < fullSpan)
            {
                B.AllDelta.assign(fullSpan, 0.0f);
                B.AllPresent.assign(fullSpan, 0);
            }
            else
            {
                std::fill(B.AllDelta.begin(),   B.AllDelta.begin()   + fullSpan, 0.0f);
                std::fill(B.AllPresent.begin(), B.AllPresent.begin() + fullSpan, (char)0);
            }
            B.FullValid = true;
        }
        else
        {
            fullSpan = 0;
        }
    }

    bool any = false;
    for (int bi = winStart; bi <= winEnd; bi++)
    {
        int PIT = INT_MIN;
        const s_VolumeAtPriceV2* pVAP = nullptr;
        while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement((unsigned int)bi, PIT, &pVAP))
        {
            if (!pVAP) continue;
            const float d = (float)((int64_t)pVAP->AskVolume - (int64_t)pVAP->BidVolume);

            if (PIT >= hiFloor && PIT <= B.HighTick)
            {
                const int idx = PIT - hiFloor;
                B.HiDelta[idx]   += d;
                B.HiPresent[idx]  = 1;
                any = true;
            }
            if (PIT >= B.LowTick && PIT <= loCeil)
            {
                const int idx = PIT - B.LowTick;
                B.LoDelta[idx]   += d;
                B.LoPresent[idx]  = 1;
                any = true;
            }

            if (B.FullValid && PIT >= B.FullBaseTick && PIT < B.FullBaseTick + fullSpan)
            {
                const int idx = PIT - B.FullBaseTick;
                B.AllDelta[idx]   += d;
                B.AllPresent[idx]  = 1;
            }
        }
    }

    B.FullSpan = fullSpan;
    B.Valid = any;
}

// Sell-side candidate: positive delta near the window HIGH.
// Mirrors v1's descending scan from the top level down to highTick - proxTol,
// keeping levels with delta >= minDelta, requiring count >= blocksMin and
// total >= totalThresh.
static Candidate ScanSell(const BandScratch& B, int proxTol,
                          int blocksMin, float minDelta, float totalThresh)
{
    Candidate c;
    c.Found = false; c.BottomTick = 0; c.TopTick = 0; c.TotalMagnitude = 0.0f;
    c.Count = 0;

    const int hiFloor = B.HighTick - proxTol;
    float total = 0.0f;
    int   count = 0;
    int   minT  = INT_MAX;
    int   maxT  = INT_MIN;

    for (int t = B.HighTick; t >= hiFloor; t--)
    {
        const int idx = t - hiFloor;
        if (!B.HiPresent[idx]) continue;
        const float d = B.HiDelta[idx];
        if (d < minDelta) continue;

        if (t < minT) minT = t;
        if (t > maxT) maxT = t;
        total += d;
        count++;
    }

    if (count >= blocksMin && total >= totalThresh)
    {
        c.Found = true;
        c.BottomTick = minT;
        c.TopTick = maxT;
        c.TotalMagnitude = total;
        c.Count = count;
    }
    return c;
}

// Long-side candidate: negative delta near the window LOW.
static Candidate ScanLong(const BandScratch& B, int proxTol,
                          int blocksMin, float minDelta, float totalThresh)
{
    Candidate c;
    c.Found = false; c.BottomTick = 0; c.TopTick = 0; c.TotalMagnitude = 0.0f;
    c.Count = 0;

    const int loCeil = B.LowTick + proxTol;
    float totalAbs = 0.0f;
    int   count    = 0;
    int   minT     = INT_MAX;
    int   maxT     = INT_MIN;

    for (int t = B.LowTick; t <= loCeil; t++)
    {
        const int idx = t - B.LowTick;
        if (!B.LoPresent[idx]) continue;
        const float absDelta = -B.LoDelta[idx];   // flip: neg delta -> pos magnitude
        if (absDelta < minDelta) continue;

        if (t < minT) minT = t;
        if (t > maxT) maxT = t;
        totalAbs += absDelta;
        count++;
    }

    if (count >= blocksMin && totalAbs >= totalThresh)
    {
        c.Found = true;
        c.BottomTick = minT;
        c.TopTick = maxT;
        c.TotalMagnitude = totalAbs;
        c.Count = count;
    }
    return c;
}

// Baseline sample collection for one window — NO threshold, NO proximity
// filter, NO direction filter.
//
// WHY NOT THE PROXIMITY BANDS: the first implementation sampled only the
// proxTol+1 ticks at each window extreme, and only levels whose sign matched
// that side.  That is the shape of a ZONE, which is by construction a rare
// event — a live NQ session produced 18 block and 4 zone samples for a whole
// trading day, far too few for a stable percentile.  A threshold is a property
// of the session's participation, not of the zones it happens to contain, so
// the baseline now measures every price level of every bar.
//
//   block sample : |aggregated delta| at each price level of the window,
//                  one sample per populated level  (~40-100 per window)
//   zone sample  : sum of the blocksMin LARGEST level magnitudes in the window,
//                  one sample per window
//
// The zone sample deliberately mirrors what Zone Delta Threshold gates — the
// combined magnitude of the strongest blocksMin levels — so the resulting
// number stays directly comparable to the manual value it replaces.
// Magnitudes are unsigned: both thresholds are magnitude tests applied to the
// positive side near highs and the negative side near lows alike.
static void CollectBaselineSamples(BandScratch& B, int blocksMin,
                                   vector<float>& blockSamples,
                                   vector<float>& zoneSamples)
{
    if (!B.FullValid) return;

    B.TmpMag.clear();

    for (int i = 0; i < B.FullSpan; i++)
    {
        if (!B.AllPresent[i]) continue;
        const float mag = (float)fabs(B.AllDelta[i]);
        if (mag <= 0.0f) continue;          // a level that netted exactly flat
        blockSamples.push_back(mag);
        B.TmpMag.push_back(mag);
    }

    if ((int)B.TmpMag.size() < blocksMin) return;

    // Sum the blocksMin largest magnitudes — partial sort, descending.
    std::partial_sort(B.TmpMag.begin(), B.TmpMag.begin() + blocksMin, B.TmpMag.end(),
                      std::greater<float>());

    float top = 0.0f;
    for (int i = 0; i < blocksMin; i++) top += B.TmpMag[i];

    zoneSamples.push_back(top);
}

// =============================================================================
// Trading-day segmentation
// =============================================================================

// Builds (full) or extends (incremental) the day-segment list so that it covers
// bars 0 .. upToBar inclusive.  Uses Sierra Chart's trading-day function, never
// calendar midnight.
static void BuildDaySegments(SCStudyInterfaceRef sc, vector<DaySeg>& days,
                             int upToBar, bool rebuild)
{
    if (upToBar < 0) { if (rebuild) days.clear(); return; }

    int startBar = 0;
    if (rebuild)
    {
        days.clear();
    }
    else if (!days.empty())
    {
        startBar = days.back().LastBar + 1;
        if (startBar > upToBar) return;
    }

    for (int bi = startBar; bi <= upToBar; bi++)
    {
        const int dayDate = sc.GetTradingDayDate(sc.BaseDateTimeIn[bi]);

        if (!days.empty() && days.back().DayDate == dayDate)
        {
            days.back().LastBar = bi;
            continue;
        }

        DaySeg d;
        memset(&d, 0, sizeof(d));
        d.DayDate        = dayDate;
        d.FirstBar       = bi;
        d.LastBar        = bi;
        // Segment 0 is always treated as partial: the chart's first bar is not
        // guaranteed to be that trading day's first bar, so its sample count is
        // not representative.  A partial day is never used as a baseline day.
        d.Partial        = days.empty();
        d.StatsComputed  = false;
        d.StatsValid     = false;
        d.BlockPct       = 0.0f;
        d.ZonePct        = 0.0f;
        d.ThreshComputed = false;
        d.ThreshReady    = false;
        d.AutoMinBlock   = 0.0f;
        d.AutoZoneTotal  = 0.0f;
        d.BaselineDays   = 0;
        days.push_back(d);
    }
}

// Does bar bi belong to the session the auto feature is restricted to?
// Filters the BASELINE and AUTO DETECTION alike, so the thresholds and the zones
// they gate always describe the same participation regime.  Manual detection is
// never filtered.
static bool BarInAutoSession(SCStudyInterfaceRef sc, int bi,
                             int sessionMode, int rthStartSec, int rthEndSec)
{
    if (sessionMode == SESS_BOTH) return true;

    const int t = sc.BaseDateTimeIn[bi].GetTimeInSeconds();
    const bool isRTH = IsTimeInRange(t, rthStartSec, rthEndSec);

    return (sessionMode == SESS_RTH) ? isRTH : !isRTH;
}

// A day is complete when a later segment exists (the day in progress never is).
static bool DayIsComplete(const vector<DaySeg>& days, int k)
{
    return (k >= 0) && (k + 1 < (int)days.size());
}

// Computes and caches one day's percentile statistics.  Called at most once per
// day per full recalculation (BuildSpec §3.7).
static void EnsureDayStats(SCStudyInterfaceRef sc, DaySeg& seg,
                           int lookback, int blocksMin, int proxTol,
                           float pctBlock, float pctZone,
                           int sessionMode, int rthStartSec, int rthEndSec,
                           BandScratch& scratch,
                           vector<float>& blockBuf, vector<float>& zoneBuf)
{
    if (seg.StatsComputed) return;
    seg.StatsComputed = true;
    seg.StatsValid    = false;

    blockBuf.clear();
    zoneBuf.clear();

    for (int winEnd = seg.FirstBar; winEnd <= seg.LastBar; winEnd++)
    {
        // Session filter: a window contributes to the baseline only when the bar
        // it ends on is inside the selected session.
        if (!BarInAutoSession(sc, winEnd, sessionMode, rthStartSec, rthEndSec))
            continue;

        int winStart = winEnd - lookback + 1;
        if (winStart < 0) winStart = 0;

        BuildBands(sc, winStart, winEnd, proxTol, scratch, true);
        if (!scratch.FullValid) continue;

        CollectBaselineSamples(scratch, blocksMin, blockBuf, zoneBuf);
    }

    seg.SampleBlockN = (int)blockBuf.size();
    seg.SampleZoneN  = (int)zoneBuf.size();

    if ((int)blockBuf.size() < MIN_BLOCK_SAMPLES_PER_DAY) return;
    if ((int)zoneBuf.size()  < MIN_ZONE_SAMPLES_PER_DAY)  return;

    std::sort(blockBuf.begin(), blockBuf.end());
    std::sort(zoneBuf.begin(),  zoneBuf.end());

    seg.BlockPct   = PercentileSorted(blockBuf, pctBlock);
    seg.ZonePct    = PercentileSorted(zoneBuf,  pctZone);
    seg.StatsValid = true;
}

// Computes and caches the auto thresholds ACTIVE during day k.
// Uses ONLY days k-N .. k-1 — all strictly earlier, all complete, never the
// current day and never a future day.  Equal weight per day: each day's
// percentile first, then the median across days.
static void EnsureDayThresholds(SCStudyInterfaceRef sc, vector<DaySeg>& days, int k,
                                int N, int lookback, int blocksMin, int proxTol,
                                float pctBlock, float pctZone,
                                int sessionMode, int rthStartSec, int rthEndSec,
                                BandScratch& scratch,
                                vector<float>& blockBuf, vector<float>& zoneBuf)
{
    if (k < 0 || k >= (int)days.size()) return;
    DaySeg& target = days[k];
    if (target.ThreshComputed) return;

    target.ThreshComputed = true;
    target.ThreshReady    = false;
    target.AutoMinBlock   = 0.0f;
    target.AutoZoneTotal  = 0.0f;
    target.BaselineDays   = 0;
    target.FailReason     = BFAIL_NONE;
    target.FailDayIdx     = -1;

    // -- Collect the N most recent USABLE sessions, walking backwards ---------
    //
    // A trading-day segment is not the same thing as a trading session.  Sierra
    // Chart's trading-day grouping produces a segment for any date carrying
    // bars, so a weekend stub or a holiday appears as its own "day" holding a
    // handful of ticks.  The first implementation took the five segments
    // immediately preceding the current one and demanded that every one of them
    // meet the sample gate; a Saturday in that span made the baseline
    // permanently unavailable.  Live evidence: 2026-08-29 (a Saturday) produced
    // 30 block and 3 zone samples for the whole segment.
    //
    // A thin segment is therefore SKIPPED, not fatal — the search simply reaches
    // further back for a real session.  Only running out of history is fatal.
    // Causality is unchanged: every day considered is strictly before k.
    const int maxLookBack = N * 4 + 10;   // enough to step over weekends/holidays

    vector<float> dayBlockPcts;
    vector<float> dayZonePcts;
    dayBlockPcts.reserve(N);
    dayZonePcts.reserve(N);

    int skipped = 0;
    int scanned = 0;
    int oldestUsed = -1;

    for (int j = k - 1; j >= 0 && (int)dayBlockPcts.size() < N && scanned < maxLookBack; j--)
    {
        scanned++;
        DaySeg& src = days[j];

        if (src.Partial)      break;      // reached the chart's partial first segment
        if (!DayIsComplete(days, j)) continue;   // defensive; always true for j < k

        EnsureDayStats(sc, src, lookback, blocksMin, proxTol,
                       pctBlock, pctZone, sessionMode, rthStartSec, rthEndSec,
                       scratch, blockBuf, zoneBuf);

        if (!src.StatsValid)
        {
            // Not a real session (weekend stub, holiday, truncated data).
            // [v3-C6] Remember the most recent unusable segment. FailDayIdx was
            // only ever written as -1, so the sample diagnostics below reported
            // "baseline day #-1" and could not name the day that was thin.
            skipped++;
            target.FailDayIdx = j;
            continue;
        }

        dayBlockPcts.push_back(src.BlockPct);
        dayZonePcts.push_back(src.ZonePct);
        oldestUsed = j;
    }

    target.BaselineDays = (int)dayBlockPcts.size();
    target.SkippedDays  = skipped;
    target.ScannedDays  = scanned;
    target.OldestUsedDay = oldestUsed;

    // Not enough real sessions in the chart's history — do NOT fabricate.
    if ((int)dayBlockPcts.size() < N)
    {
        target.FailReason = BFAIL_TOO_FEW;
        return;
    }

    target.AutoMinBlock  = MedianOf(dayBlockPcts);
    target.AutoZoneTotal = MedianOf(dayZonePcts);

    // A degenerate baseline (all-zero percentiles) is not a usable threshold.
    if (target.AutoMinBlock <= 0.0f || target.AutoZoneTotal <= 0.0f)
    {
        target.FailReason = BFAIL_DEGENERATE;
        return;
    }

    target.ThreshReady = true;
}

// =============================================================================
// Decision Map adapter helpers (v2.1, Phase 2)
// =============================================================================

// Materialize one closed bar for the decision core: integer-tick OHLC plus
// the bar's full VAP arrays (the core filters each episode's frozen [B,T]).
// Uses GetSizeAtBarIndex + GetVAPElementAtIndex per spec §5; volumes stay
// double (fractional values preserved). Missing/unreadable VAP sets
// vapMissing — the core then flags incomplete only when price overlaps.
static void FillDecisionBar(SCStudyInterfaceRef sc, int barIdx, TtdBar& out,
                            vector<int>& prices, vector<double>& ask,
                            vector<double>& bid)
{
    out.j = barIdx;
    out.closeTick = TtdPriceToTicks((double)sc.Close[barIdx], sc.TickSize);
    out.highTick  = TtdPriceToTicks((double)sc.High[barIdx],  sc.TickSize);
    out.lowTick   = TtdPriceToTicks((double)sc.Low[barIdx],   sc.TickSize);
    out.barStartSec = sc.BaseDateTimeIn[barIdx].GetAsDouble() * 86400.0;
    out.barEndSec   = sc.GetEndingDateTimeForBarIndex(barIdx).GetAsDouble() * 86400.0;
    out.spanValid = 1;  // deterministic market-time bound, not callback time
    out.vapPrices = nullptr;
    out.vapAsk = nullptr;
    out.vapBid = nullptr;
    out.vapN = 0;
    out.vapMissing = 1;

    c_VAPContainer* vap = sc.VolumeAtPriceForBars;
    if (vap == nullptr || barIdx < 0)
        return;
    const unsigned int n = vap->GetSizeAtBarIndex((unsigned int)barIdx);
    if (n == 0)
        return;  // no VAP held for this bar: missing, not zero effort
    prices.resize(n);
    ask.resize(n);
    bid.resize(n);
    for (unsigned int i = 0; i < n; i++)
    {
        const s_VolumeAtPriceV2* e = nullptr;
        if (!vap->GetVAPElementAtIndex((unsigned int)barIdx, (int)i, &e) || e == nullptr)
            return;  // unreadable element: missing, keep nothing partial
        prices[i] = e->PriceInTicks;
        ask[i]    = e->AskVolume;  // double per installed VAPContainer.h:35-43
        bid[i]    = e->BidVolume;
    }
    out.vapPrices = prices.data();
    out.vapAsk = ask.data();
    out.vapBid = bid.data();
    out.vapN = (int)n;
    out.vapMissing = 0;
}

// Build one frozen decision candidate from an original detector candidate
// BEFORE legacy mutable merging can change it (spec §3). Thresholds are the
// ones actually used for this window (manual or auto).
static void FeedDecisionCandidate(TtdEngine* eng, int winEnd, bool isBuySide,
                                  int bottomTick, int topTick, float magnitude,
                                  int blockCount, int source, int displTicks,
                                  int deadlineBars, float threshBlock,
                                  float threshZone)
{
    if (eng == nullptr)
        return;
    TtdCandidate dc;
    dc.dir = isBuySide ? 1 : -1;
    dc.d = winEnd;
    dc.bottomTick = bottomTick;
    dc.topTick = topTick;
    dc.origDelta = isBuySide ? -(double)magnitude : (double)magnitude;
    dc.origBlocks = blockCount;
    dc.source = source;
    dc.displTicks = displTicks;
    dc.deadlineBars = deadlineBars;
    dc.threshBlock = threshBlock;
    dc.threshZone = threshZone;
    TtdAddCandidate(eng, &dc);
}

// =============================================================================
// Main study function
// =============================================================================

// [v4-M3] Zone-to-zone consolidation.
//
// A candidate merges into the FIRST live zone it overlaps, and that zone grows
// to the union.  Nothing then checked whether the grown zone now overlapped
// OTHER live zones of the same source and direction, so two zones that were
// apart when detected ended up stacked on top of each other once a candidate
// between them widened one of them, and they stayed stacked for their whole
// life.  Called after every widening merge: absorbs every live same-source,
// same-direction zone that now overlaps, repeating until nothing changes (an
// absorption can widen the zone into yet another one).
//
// The OLDER zone survives (earliest DetectedBar, so the left edge and label
// stay where they were).  It takes the union range, the larger |delta| and
// block count (fix B6 rule: never summed), and is confirmed if either member
// was.  The absorbed record is deleted from the chart and its slot freed.
// maxWidthTicks is the same runaway cap the candidate merge obeys.
//
// Manual precedence (Both mode) needs no re-check: the survivor's range is the
// union of two OVERLAPPING ranges, so any manual zone intersecting it already
// intersected one of the two, and suppression guarantees neither did.
//
// Returns the index of the surviving zone.
static int AbsorbOverlappingZones(SCStudyInterfaceRef sc, vector<ZoneRec>& zones,
                                  const vector<int>& liveIdx, int keepIdx,
                                  int maxWidthTicks, vector<char>& slots,
                                  TtdDirtySet* dirty)
{
    int  cur     = keepIdx;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t li = 0; li < liveIdx.size(); li++)
        {
            const int oi = liveIdx[li];
            if (oi == cur) continue;
            ZoneRec& a = zones[cur];
            ZoneRec& o = zones[oi];
            if (!a.Valid || !o.Valid) continue;
            if (o.Source != a.Source || o.IsBuyZone != a.IsBuyZone) continue;
            if (!TicksOverlapInclusive(a.BottomTick, a.TopTick, o.BottomTick, o.TopTick))
                continue;

            const int newBot = min(a.BottomTick, o.BottomTick);
            const int newTop = max(a.TopTick,    o.TopTick);
            if ((newTop - newBot) > maxWidthTicks)
                continue;

            const bool keepA = (a.DetectedBar < o.DetectedBar) ||
                               (a.DetectedBar == o.DetectedBar && cur < oi);
            const int  si = keepA ? cur : oi;
            ZoneRec&   sv = keepA ? a : o;   // survivor
            ZoneRec&   vi = keepA ? o : a;   // absorbed

            TtdDirtyMark(dirty, sv.DetectedBar);
            TtdDirtyMark(dirty, vi.DetectedBar);

            sv.BottomTick = newBot;
            sv.TopTick    = newTop;
            {
                const float mag = max((float)fabs(sv.TotalDelta), (float)fabs(vi.TotalDelta));
                sv.TotalDelta = sv.IsBuyZone ? -mag : mag;
            }
            if (vi.BlockCount > sv.BlockCount) sv.BlockCount = vi.BlockCount;
            sv.Pending = sv.Pending && vi.Pending;   // confirmed if either was
            if (vi.CheckedTo > sv.CheckedTo) sv.CheckedTo = vi.CheckedTo;

            DeleteZoneDrawings(sc, vi);
            vi.Valid        = false;
            vi.Pending      = false;
            vi.ErasedBar    = -1;                    // never drawn again
            vi.AbsorbedInto = sv.ID;
            if (vi.Slot >= 0)
            {
                FreeSlot(slots, vi.Slot);
                vi.Slot = -1;
            }

            cur     = si;
            changed = true;
        }
    }
    return cur;
}

SCSFExport scsf_TrappedTraders_v4(SCStudyInterfaceRef sc)
{
    // -- Input references ------------------------------------------------------
    SCInputRef In_BarsLookback    = sc.Input[0];   // In:1
    SCInputRef In_BlocksPerZone   = sc.Input[1];   // In:2
    SCInputRef In_MinBlockDelta   = sc.Input[2];   // In:3
    SCInputRef In_ZoneDeltaThresh = sc.Input[3];   // In:4
    SCInputRef In_ProximityTol    = sc.Input[4];   // In:5
    SCInputRef In_SellFillColor   = sc.Input[5];   // In:6
    SCInputRef In_SellBordColor   = sc.Input[6];   // In:7
    SCInputRef In_BuyFillColor    = sc.Input[7];   // In:8
    SCInputRef In_BuyBordColor    = sc.Input[8];   // In:9
    SCInputRef In_BorderWidth     = sc.Input[9];   // In:10
    SCInputRef In_Transparency    = sc.Input[10];  // In:11
    SCInputRef In_ShowDelta       = sc.Input[11];  // In:12
    SCInputRef In_DisplayMode     = sc.Input[12];  // In:13
    // v2 — appended, no existing input renumbered
    SCInputRef In_ThresholdMode   = sc.Input[13];  // In:14
    SCInputRef In_AutoDays        = sc.Input[14];  // In:15
    SCInputRef In_AutoBlockPct    = sc.Input[15];  // In:16
    SCInputRef In_AutoZonePct     = sc.Input[16];  // In:17
    SCInputRef In_AutoSellFill    = sc.Input[17];  // In:18
    SCInputRef In_AutoSellBord    = sc.Input[18];  // In:19
    SCInputRef In_AutoBuyFill     = sc.Input[19];  // In:20
    SCInputRef In_AutoBuyBord     = sc.Input[20];  // In:21
    SCInputRef In_ShowAutoStatus  = sc.Input[21];  // In:22
    SCInputRef In_AutoSession     = sc.Input[22];  // In:23
    SCInputRef In_RTHStart        = sc.Input[23];  // In:24
    SCInputRef In_RTHEnd          = sc.Input[24];  // In:25
    SCInputRef In_ConfirmMode     = sc.Input[25];  // In:26
    SCInputRef In_ConfirmTicks    = sc.Input[26];  // In:27
    SCInputRef In_ConfirmDeadline = sc.Input[27];  // In:28
    SCInputRef In_ExportCSV       = sc.Input[28];  // In:29
    SCInputRef In_ExportFolder    = sc.Input[29];  // In:30
    SCInputRef In_ExportFile      = sc.Input[30];  // In:31
    SCInputRef In_DecEnable       = sc.Input[31];  // In:32 Decision Map
    SCInputRef In_DecMaxNearby     = sc.Input[32];  // In:33 Decision Map
    SCInputRef In_DecDetail       = sc.Input[33];  // In:34 Decision Map
    SCInputRef In_DecJournal      = sc.Input[34];  // In:35 Decision Map
    SCInputRef In_DecSnapshot     = sc.Input[35];  // In:36 Decision Map
    SCInputRef In_ZoneDisplay     = sc.Input[36];  // In:37 [v4-M1] merge
    SCInputRef In_MergeBounds     = sc.Input[37];  // In:38 [v4-M1] merge
    SCInputRef In_MergeGapTicks   = sc.Input[38];  // In:39 [v4-M1] merge
    // In:40..In:45 — Merge Source 1..6 are sc.Input[39 + k], k = 0..5
    const int  IN_MERGE_SRC_FIRST = 39;
    SCInputRef In_LockZones       = sc.Input[45];  // In:46 [v4-M4]
    SCInputRef In_ErFill          = sc.Input[46];  // In:47 [v4-M5]
    SCInputRef In_ErBord          = sc.Input[47];  // In:48 [v4-M5]
    SCInputRef In_KeepErased      = sc.Input[48];  // In:49 [v4-M5]

    // -- Subgraph references ---------------------------------------------------
    SCSubgraphRef SG_SellDelta   = sc.Subgraph[0];   // SG1 — unchanged
    SCSubgraphRef SG_BuyDelta    = sc.Subgraph[1];   // SG2 — unchanged
    SCSubgraphRef SG_AutoBlock   = sc.Subgraph[2];   // SG3 — v2
    SCSubgraphRef SG_AutoZone    = sc.Subgraph[3];   // SG4 — v2
    SCSubgraphRef SG_AutoReady   = sc.Subgraph[4];   // SG5 — v2
    SCSubgraphRef SG_ZoneSource  = sc.Subgraph[5];   // SG6 — v2
    SCSubgraphRef SG_DConfBuy    = sc.Subgraph[6];   // SG7  Decision Map
    SCSubgraphRef SG_DConfSell   = sc.Subgraph[7];   // SG8  Decision Map
    SCSubgraphRef SG_DRet1Buy    = sc.Subgraph[8];   // SG9  Decision Map
    SCSubgraphRef SG_DRet1Sell   = sc.Subgraph[9];   // SG10 Decision Map
    SCSubgraphRef SG_DRejBuy     = sc.Subgraph[10];  // SG11 Decision Map
    SCSubgraphRef SG_DRejSell    = sc.Subgraph[11];  // SG12 Decision Map
    SCSubgraphRef SG_DCrossBuy   = sc.Subgraph[12];  // SG13 Decision Map
    SCSubgraphRef SG_DCrossSell  = sc.Subgraph[13];  // SG14 Decision Map
    SCSubgraphRef SG_DSelId      = sc.Subgraph[14];  // SG15 Decision Map
    SCSubgraphRef SG_DSelState   = sc.Subgraph[15];  // SG16 Decision Map
    SCSubgraphRef SG_DSelBot     = sc.Subgraph[16];  // SG17 Decision Map
    SCSubgraphRef SG_DSelTop     = sc.Subgraph[17];  // SG18 Decision Map
    SCSubgraphRef SG_DStatus     = sc.Subgraph[18];  // SG19 Decision Map

    // =========================================================================
    // SetDefaults
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "Trapped Traders v4 - Merge";
        sc.StudyDescription = "Zones where aggressive participants pushed price "
                              "to an extreme within a rolling bar window but "
                              "failed to continue - trapping them on the wrong side. "
                              "v2 adds automatic thresholds derived from the previous "
                              "N completed trading days of this chart. "
                              "v2.1 adds an opt-in Decision Map (In:32): a causal "
                              "account of failed aggression and subsequent returns; "
                              "off by default, legacy behaviour unchanged. "
                              "v3 keeps those results and fixes seven defects plus "
                              "four loops whose cost grew with chart length; see the "
                              "v3 change list at the top of the source. "
                              "v4 adds cross-chart zone merge (In:37-45): one "
                              "zone per group of overlapping zones from up to six "
                              "other timeframes.";
        sc.AutoLoop              = 0;
        sc.GraphRegion           = 0;
        sc.ScaleRangeType        = SCALE_SAMEASREGION;
        sc.DrawZeros             = 0;
        sc.MaintainVolumeAtPriceData = 1;   // REQUIRED: footprint/VAP data

        // Subgraphs — data outputs only, not drawn on chart
        SG_SellDelta.Name       = "Sell Zone Delta";
        SG_SellDelta.DrawStyle  = DRAWSTYLE_IGNORE;
        SG_SellDelta.DrawZeros  = 0;

        SG_BuyDelta.Name        = "Buy Zone Delta";
        SG_BuyDelta.DrawStyle   = DRAWSTYLE_IGNORE;
        SG_BuyDelta.DrawZeros   = 0;

        SG_AutoBlock.Name       = "Active Auto Block Threshold";
        SG_AutoBlock.DrawStyle  = DRAWSTYLE_IGNORE;
        SG_AutoBlock.DrawZeros  = 0;

        SG_AutoZone.Name        = "Active Auto Zone Threshold";
        SG_AutoZone.DrawStyle   = DRAWSTYLE_IGNORE;
        SG_AutoZone.DrawZeros   = 0;

        SG_AutoReady.Name       = "Auto Baseline Ready";
        SG_AutoReady.DrawStyle  = DRAWSTYLE_IGNORE;
        SG_AutoReady.DrawZeros  = 0;

        SG_ZoneSource.Name      = "Zone Source Pulse";
        SG_ZoneSource.DrawStyle = DRAWSTYLE_IGNORE;
        SG_ZoneSource.DrawZeros = 0;

        // Decision Map outputs SG7..SG19 (BuildSpec §6): per-availability-bar
        // event counts split by side (pulses, never netted), per-bar
        // selected-state snapshots, and the exact data-status bitmask. All
        // DRAWSTYLE_IGNORE, DrawZeros=0. SG1..SG6 above are unrepurposed.
        SG_DConfBuy.Name      = "Failure Confirmed Buy Count";
        SG_DConfBuy.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DConfBuy.DrawZeros = 0;

        SG_DConfSell.Name      = "Failure Confirmed Sell Count";
        SG_DConfSell.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DConfSell.DrawZeros = 0;

        SG_DRet1Buy.Name      = "First Return Buy Count";
        SG_DRet1Buy.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DRet1Buy.DrawZeros = 0;

        SG_DRet1Sell.Name      = "First Return Sell Count";
        SG_DRet1Sell.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DRet1Sell.DrawZeros = 0;

        SG_DRejBuy.Name      = "Return Rejected Buy Count";
        SG_DRejBuy.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DRejBuy.DrawZeros = 0;

        SG_DRejSell.Name      = "Return Rejected Sell Count";
        SG_DRejSell.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DRejSell.DrawZeros = 0;

        SG_DCrossBuy.Name      = "Crossed Buy Count";
        SG_DCrossBuy.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DCrossBuy.DrawZeros = 0;

        SG_DCrossSell.Name      = "Crossed Sell Count";
        SG_DCrossSell.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DCrossSell.DrawZeros = 0;

        SG_DSelId.Name      = "Selected Decision ID";
        SG_DSelId.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DSelId.DrawZeros = 0;

        SG_DSelState.Name      = "Selected Decision State";
        SG_DSelState.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DSelState.DrawZeros = 0;

        SG_DSelBot.Name      = "Selected Decision Bottom Price";
        SG_DSelBot.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DSelBot.DrawZeros = 0;

        SG_DSelTop.Name      = "Selected Decision Top Price";
        SG_DSelTop.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DSelTop.DrawZeros = 0;

        SG_DStatus.Name      = "Decision Data Status";
        SG_DStatus.DrawStyle = DRAWSTYLE_IGNORE;
        SG_DStatus.DrawZeros = 0;

        // Inputs — In:1..In:13 unchanged from v1 (index, name, type, default)
        In_BarsLookback.Name    = "Numbers Bars Lookback";
        In_BarsLookback.SetInt(2);

        In_BlocksPerZone.Name   = "Number of Blocks Per Zone";
        In_BlocksPerZone.SetInt(3);

        In_MinBlockDelta.Name   = "Minimum Single Block Delta";
        In_MinBlockDelta.SetFloat(95.0f);

        In_ZoneDeltaThresh.Name = "Zone Delta Threshold";
        In_ZoneDeltaThresh.SetFloat(350.0f);

        In_ProximityTol.Name    = "Proximity Tolerance (blocks from extreme)";
        In_ProximityTol.SetInt(3);

        In_SellFillColor.Name   = "Sell Zone Fill Color";
        In_SellFillColor.SetColor(RGB(180, 40, 50));

        In_SellBordColor.Name   = "Sell Zone Border Color";
        In_SellBordColor.SetColor(RGB(220, 80, 90));

        In_BuyFillColor.Name    = "Long Zone Fill Color";
        In_BuyFillColor.SetColor(RGB(30, 80, 160));

        In_BuyBordColor.Name    = "Long Zone Border Color";
        In_BuyBordColor.SetColor(RGB(60, 130, 220));

        // [v3-C4] Limits added. Border width reached Sierra as (uint16_t)width,
        // so 0 or a negative value became an enormous line width; transparency
        // outside 0..100 is not a meaningful level either.
        In_BorderWidth.Name     = "Zone Border Width";
        In_BorderWidth.SetInt(1);
        In_BorderWidth.SetIntLimits(1, 10);

        In_Transparency.Name    = "Transparency Level";
        In_Transparency.SetInt(50);
        In_Transparency.SetIntLimits(0, 100);

        In_ShowDelta.Name       = "Show Delta Info (0=No, 1=Yes)";
        In_ShowDelta.SetYesNo(1);

        In_DisplayMode.Name     = "Show Erased Zones";
        In_DisplayMode.SetDescription("No = Clipped Zones (default): erased zones disappear. "
                                      "Yes = Show Erased Zones: invalidated zones remain visible "
                                      "from their origin to the bar they were erased, drawn at "
                                      "higher transparency to distinguish from active zones.");
        In_DisplayMode.SetYesNo(1);

        // -- v2 inputs, appended after all existing inputs ---------------------
        In_ThresholdMode.Name = "Threshold Mode (0=Manual, 1=Auto, 2=Both)";
        In_ThresholdMode.SetDescription("0 = Manual: use Minimum Single Block Delta and Zone Delta "
                                        "Threshold exactly as before. "
                                        "1 = Auto: ignore both manual thresholds and use values "
                                        "calculated from the previous N completed trading days; only "
                                        "auto-coloured zones are drawn. "
                                        "2 = Both: run manual and auto detection independently on the "
                                        "same closed-bar windows. Manual takes precedence - an auto "
                                        "zone overlapping a manual zone is suppressed.");
        In_ThresholdMode.SetInt(TMODE_MANUAL);
        In_ThresholdMode.SetIntLimits(TMODE_MANUAL, TMODE_BOTH);

        In_AutoDays.Name = "Auto Lookback Trading Days";
        In_AutoDays.SetDescription("Number of COMPLETE trading days preceding the day being "
                                   "evaluated that form the automatic baseline. The current "
                                   "trading day is always excluded from its own baseline.");
        In_AutoDays.SetInt(5);
        In_AutoDays.SetIntLimits(1, 20);

        In_AutoBlockPct.Name = "Auto Block Delta Percentile";
        In_AutoBlockPct.SetDescription("Percentile of each baseline day's per-level delta magnitudes "
                                       "that becomes that day's block value. The active threshold is "
                                       "the median of the daily values.");
        In_AutoBlockPct.SetFloat(85.0f);
        In_AutoBlockPct.SetFloatLimits(1.0f, 99.9f);

        In_AutoZonePct.Name = "Auto Zone Delta Percentile";
        In_AutoZonePct.SetDescription("Percentile of each baseline day's zone-total magnitudes that "
                                      "becomes that day's zone value. The active threshold is the "
                                      "median of the daily values.");
        In_AutoZonePct.SetFloat(80.0f);
        In_AutoZonePct.SetFloatLimits(1.0f, 99.9f);

        In_AutoSellFill.Name = "Auto Sell Zone Fill Color";
        In_AutoSellFill.SetColor(RGB(150, 90, 20));

        In_AutoSellBord.Name = "Auto Sell Zone Border Color";
        In_AutoSellBord.SetColor(RGB(230, 150, 40));

        In_AutoBuyFill.Name = "Auto Long Zone Fill Color";
        In_AutoBuyFill.SetColor(RGB(20, 120, 110));

        In_AutoBuyBord.Name = "Auto Long Zone Border Color";
        In_AutoBuyBord.SetColor(RGB(50, 200, 180));

        In_ShowAutoStatus.Name = "Show Active Auto Thresholds";
        In_ShowAutoStatus.SetDescription("Displays a small status label with the trading day, the "
                                         "active auto block and zone thresholds, and the number of "
                                         "completed baseline days. Ignored in Manual mode.");
        In_ShowAutoStatus.SetYesNo(1);

        In_AutoSession.Name = "Auto Session Filter";
        In_AutoSession.SetCustomInputStrings("Both (whole trading day);RTH only;ETH only");
        In_AutoSession.SetCustomInputIndex(SESS_BOTH);
        In_AutoSession.SetDescription("Restricts BOTH the automatic baseline and automatic zone "
                                      "detection to the chosen session. Baseline samples are taken "
                                      "only from bars in that session, and auto zones are only "
                                      "produced on bars in that session, so the thresholds and the "
                                      "zones they gate always describe the same participation "
                                      "regime. Manual detection is never filtered.");

        In_RTHStart.Name = "RTH Start Time";
        In_RTHStart.SetDescription("Start of the regular session, in the chart's time zone. "
                                   "Everything outside RTH Start..RTH End counts as ETH.");
        In_RTHStart.SetTime(SCDateTime(9, 30, 0, 0).GetTimeInSeconds());

        In_RTHEnd.Name = "RTH End Time";
        In_RTHEnd.SetDescription("End of the regular session, in the chart's time zone. "
                                 "Exclusive. May be earlier than the start, in which case the "
                                 "range wraps midnight.");
        In_RTHEnd.SetTime(SCDateTime(16, 0, 0, 0).GetTimeInSeconds());

        In_ConfirmMode.Name = "Zone Confirmation";
        // Order MUST match CONF_NEXTBAR = 0, CONF_DISPLACE = 1 — the dropdown
        // returns the string INDEX, so a mismatched order silently inverts the
        // setting (the label said one thing while the code ran the other).
        In_ConfirmMode.SetCustomInputStrings("Next bar open (v1);Displacement (close beyond zone)");
        In_ConfirmMode.SetCustomInputIndex(CONF_DISPLACE);
        In_ConfirmMode.SetDescription("Displacement: the zone only confirms once price CLOSES the "
                                      "configured number of ticks beyond it on the trapped side - "
                                      "a sell zone needs a close below its bottom, a long zone a "
                                      "close above its top. While waiting, a close through the zone "
                                      "the other way discards it. "
                                      "Next bar open: the v1 rule - the zone confirms unless the "
                                      "very next bar OPENS through it, which cannot see price "
                                      "trading through the zone afterwards.");

        In_ConfirmTicks.Name = "Confirmation Displacement (ticks)";
        In_ConfirmTicks.SetDescription("How far beyond the zone price must close to confirm it. "
                                       "1 tick means the zone appears as soon as one bar closes "
                                       "just past it. Displacement mode only.");
        In_ConfirmTicks.SetInt(1);
        In_ConfirmTicks.SetIntLimits(1, 100);

        In_ConfirmDeadline.Name = "Confirmation Deadline (bars, 0 = none)";
        In_ConfirmDeadline.SetDescription("Discard a zone that has not displaced within this many "
                                          "closed bars of detection. 0 leaves it waiting "
                                          "indefinitely - it is still discarded the moment price "
                                          "closes through it. Displacement mode only.");
        In_ConfirmDeadline.SetInt(0);
        In_ConfirmDeadline.SetIntLimits(0, 500);

        In_ExportCSV.Name = "Export Zones To CSV";
        In_ExportCSV.SetDescription("Writes every tracked zone - pending, confirmed and (when "
                                    "Show Erased Zones is on) erased - to a CSV file on each "
                                    "processing call. Both confirmation rules are scored on every "
                                    "zone, so the file supports comparing them over one identical "
                                    "zone population. Off by default: it rewrites the whole file "
                                    "on every bar close.");
        In_ExportCSV.SetYesNo(0);

        In_ExportFolder.Name = "CSV Export Folder";
        In_ExportFolder.SetDescription("Folder for the export file. Leave empty to use Sierra "
                                       "Chart's Data Files Folder. A trailing backslash is "
                                       "optional.");
        In_ExportFolder.SetString("");

        In_ExportFile.Name = "CSV Export Filename";
        In_ExportFile.SetDescription("Filename written inside CSV Export Folder. The file is "
                                     "TRUNCATED and rewritten in full each time, because a zone "
                                     "can be invalidated retroactively and an appended row could "
                                     "never be corrected.");
        // Default stem carries the version so v2/v3/v4 instances on one chart write
        // separate files even before the chart/instance suffix is applied.
        In_ExportFile.SetString("TrappedTraders_v4_zones.csv");

        // -- v2.1 Decision Map inputs, appended after all existing inputs ----
        // In:32 is structural (toggling it resets the decision generation).
        // In:33 is display only; In:34 export control only; In:35 snapshot
        // request (an increase requests one snapshot) — none of these three
        // enter the decision semantic fingerprint.
        In_DecEnable.Name = "Enable Decision Map";
        In_DecEnable.SetDescription("Opt-in causal account of failed aggression "
                                    "and subsequent returns. Requires Zone "
                                    "Confirmation = Displacement; with Next bar "
                                    "open selected the decision outputs stay "
                                    "inactive and legacy operation continues.");
        In_DecEnable.SetYesNo(0);

        In_DecMaxNearby.Name = "Maximum Nearby Decision Zones";
        In_DecMaxNearby.SetDescription("Display only: at most this many frozen "
                                      "decision zones are drawn/listed, ranked "
                                      "by last closed price intersection, then "
                                      "distance, newer confirmation, stable ID. "
                                      "Tracking of all events is unaffected.");
        In_DecMaxNearby.SetInt(6);
        In_DecMaxNearby.SetIntLimits(1, 30);

        In_DecDetail.Name = "Show Decision Detail Panel";
        In_DecDetail.SetDescription("Display only: detail panel for the first "
                                    "selected decision zone.");
        In_DecDetail.SetYesNo(1);

        In_DecJournal.Name = "Export Decision Event Journal";
        In_DecJournal.SetDescription("Export control only: append a journal row "
                                     "for each new decision transition batch. "
                                     "Off by default.");
        In_DecJournal.SetYesNo(0);

        In_DecSnapshot.Name = "Decision Snapshot Request";
        In_DecSnapshot.SetDescription("An increase requests one complete "
                                      "decision snapshot; an unchanged value "
                                      "never repeatedly rewrites. Excluded "
                                      "from the semantic fingerprint.");
        In_DecSnapshot.SetInt(0);
        In_DecSnapshot.SetIntLimits(0, 1000000);

        // -- [v4-M1] Cross-chart merge inputs, appended after all others ----
        In_ZoneDisplay.Name = "Zone Display";
        // Order MUST match ZDISP_OWN = 0, ZDISP_HIDDEN = 1, ZDISP_MERGED = 2.
        In_ZoneDisplay.SetCustomInputStrings("This chart's zones;Hidden (merge source only);"
                                             "Merged with other charts");
        In_ZoneDisplay.SetCustomInputIndex(ZDISP_OWN);
        In_ZoneDisplay.SetDescription("This chart's zones: normal drawing. "
                                      "Hidden: draws nothing, but other charts can still merge "
                                      "this chart's zones - use it on the source timeframes. "
                                      "Merged: draws ONE zone per group of overlapping same-"
                                      "direction zones from this chart and Merge Source 1-6. "
                                      "A merged zone shrinks as members are erased and ends only "
                                      "when every member is erased.");

        In_MergeBounds.Name = "Merge Bounds";
        // Order MUST match MBOUND_UNION = 0, MBOUND_OVERLAP = 1.
        In_MergeBounds.SetCustomInputStrings("Union (outer edges);Overlap (shared band)");
        In_MergeBounds.SetCustomInputIndex(MBOUND_UNION);
        In_MergeBounds.SetDescription("Union: the merged zone spans from the lowest bottom to "
                                      "the highest top of its live members. Overlap: only the "
                                      "band every live member shares; when members were joined "
                                      "through the gap tolerance and share no band, the band "
                                      "between their inner edges.");

        In_MergeGapTicks.Name = "Merge Gap Tolerance (ticks)";
        In_MergeGapTicks.SetDescription("0 = zones must share at least one price level. N = zones up to "
                                        "N ticks apart also merge (NQ/ES: 4 ticks = 1 point).");
        In_MergeGapTicks.SetInt(0);
        In_MergeGapTicks.SetIntLimits(0, 400);

        for (int k = 0; k < MERGE_MAX_SOURCES; k++)
        {
            SCInputRef in = sc.Input[IN_MERGE_SRC_FIRST + k];
            SCString nm;
            nm.Format("Merge Source %d", k + 1);
            in.Name = nm;
            in.SetChartStudyValues(0, 0);
            in.SetDescription("Another Trapped Traders v4 instance in this chartbook - on "
                              "another chart or on THIS chart (e.g. a Manual instance next "
                              "to an Auto one) - same symbol. Used only when Zone Display = "
                              "Merged. Leave empty to skip.");
        }

        // -- [v4-M4] Lock Zones --------------------------------------------
        In_LockZones.Name = "Lock Zones (block moving/editing)";
        In_LockZones.SetDescription("Yes: zone rectangles, labels and status text are locked, "
                                    "so they cannot be dragged or edited on the chart. They "
                                    "stay user drawings, so Chart Settings > Copy chart "
                                    "drawings still copies them to other charts.");
        In_LockZones.SetYesNo(1);

        // -- [v4-M5] Erased zone colours and expiry --------------------------
        // Used only with Show Erased Zones = Yes.  One colour for sell and
        // long, manual and auto alike: once erased, neither matters.
        In_ErFill.Name = "Erased Zone Fill Color";
        In_ErFill.SetColor(RGB(200, 200, 200));
        In_ErBord.Name = "Erased Zone Border Color";
        In_ErBord.SetColor(RGB(170, 170, 170));

        In_KeepErased.Name = "Keep Erased Zones (sessions, 0 = forever)";
        In_KeepErased.SetDescription("With Show Erased Zones = Yes: how many trading days an "
                                     "erased zone stays, COUNTING the day it was erased. "
                                     "1 = until the end of that day, gone tomorrow. "
                                     "3 = erased Monday, shown through Wednesday, gone Thursday. "
                                     "Weekends and holidays do not count. 0 = keep forever.");
        In_KeepErased.SetInt(3);
        In_KeepErased.SetIntLimits(0, 250);

        return;
    }

    // =========================================================================
    // Cleanup on study removal
    // =========================================================================
    if (sc.LastCallToFunction)
    {
        // Sweep the WHOLE line-number range rather than walking the zone list.
        // The zone list only describes zones this session created; drawings are
        // user-drawn and therefore saved in the chartbook, so a reloaded
        // chartbook has rectangles with no matching record.  Walking the list
        // left those behind — that is why zones survived removing the study.
        DeleteAllStudyDrawings(sc, MAX_ZONE_SLOTS);
        // [v4-M1] Withdraw from merging before the vector is freed, and forget
        // the merged drawings the sweep above just removed.
        sc.SetPersistentInt(P_MMAGIC, 0);
        sc.SetPersistentInt(P_MDRAWN, 0);
        sc.SetPersistentInt(P_MSTDRAWN, 0);
        sc.SetPersistentInt(P_MSIG, 0);

        vector<ZoneRec>* pZ = (vector<ZoneRec>*)sc.GetPersistentPointer(P_ZONES);
        if (pZ)
        {
            delete pZ;
            sc.SetPersistentPointer(P_ZONES, nullptr);
        }

        vector<DaySeg>* pD = (vector<DaySeg>*)sc.GetPersistentPointer(P_DAYS);
        if (pD) { delete pD; sc.SetPersistentPointer(P_DAYS, nullptr); }

        vector<char>* pSM = (vector<char>*)sc.GetPersistentPointer(P_SLOTS_MAN);
        if (pSM) { delete pSM; sc.SetPersistentPointer(P_SLOTS_MAN, nullptr); }

        vector<char>* pSA = (vector<char>*)sc.GetPersistentPointer(P_SLOTS_AUTO);
        if (pSA) { delete pSA; sc.SetPersistentPointer(P_SLOTS_AUTO, nullptr); }

        DeleteDrawing(sc, StudyLine(sc, LINE_STATUS));
        DeleteDrawing(sc, StudyLine(sc, LINE_WARNING));
        sc.SetPersistentInt(P_STATUS_DRAWN, 0);
        sc.SetPersistentInt(P_WARN_DRAWN, 0);
        sc.SetPersistentInt(P_MAXSLOT, 0);
        // Decision Map owned engine: free once, null the pointer.
        TtdEngine* pDec = (TtdEngine*)sc.GetPersistentPointer(P_DECISION);
        if (pDec)
        {
            delete pDec;
            sc.SetPersistentPointer(P_DECISION, nullptr);
        }
        // Decision Map owned drawings: delete exactly this instance's IDs
        // (never another instance's, never a global delete), then free.
        TtdDrawOwnerState* pDO = TtdGetDrawOwner(sc);
        if (pDO)
        {
            TtdDeleteOwnedDrawings(sc, pDO);
            delete pDO;
            sc.SetPersistentPointer(P_DDRAW, nullptr);
        }
        sc.SetPersistentInt(P_DMODE, 0);
        sc.SetPersistentInt(P_DCLRN, 0);   // [v3-P4] clear latch
        sc.SetPersistentInt(P_DCLRS, 0);
        // The CSV export owns no allocation — it opens and closes its FILE*
        // within one call — so there is nothing to free here, only the
        // one-shot log latch to clear for a future re-add of the study.
        sc.SetPersistentInt(P_LOGGED_CSV, 0);
        return;
    }

    // =========================================================================
    // Hide / unhide
    // =========================================================================
    // Sierra Chart does not remove ACSIL drawings when a study is hidden — the
    // study simply stops being asked to draw, so whatever is already on the
    // chart stays there.  v1 never handled this, which is why hiding the study
    // left every zone on screen.  Delete once on the transition into hidden
    // (latched so a hidden study does not re-sweep on every update), and force a
    // full rebuild on the way back out by clearing the settings fingerprint.
    if (sc.HideStudy)
    {
        if (sc.GetPersistentInt(P_HIDDEN) == 0)
        {
            sc.SetPersistentInt(P_HIDDEN, 1);
            // A study hidden at chartbook load returns here before the
            // first-initialisation sweep ever runs, so there is no high-water
            // mark to trust — fall back to the full range in that case.
            int sweepTo = sc.GetPersistentInt(P_MAXSLOT);
            if (sweepTo <= 0) sweepTo = MAX_ZONE_SLOTS;
            DeleteAllStudyDrawings(sc, sweepTo);
            DeleteMergedDrawings(sc);         // [v4-M1] may exceed sweepTo
            // [v4-M1] A hidden study stops processing, so its zones go stale:
            // stop other charts merging them.  Unhide rebuilds and republishes.
            sc.SetPersistentInt(P_MMAGIC, 0);
            sc.SetPersistentInt(P_STATUS_DRAWN, 0);
            sc.SetPersistentInt(P_WARN_DRAWN, 0);
            // Decision drawings are instance-owned non-user objects: remove
            // exactly this instance's IDs so nothing lingers while hidden.
            // Records stay canonical; unhide rebuilds and recreates.
            TtdDrawOwnerState* pDOw = TtdGetDrawOwner(sc);
            if (pDOw != nullptr)
                TtdDeleteOwnedDrawings(sc, pDOw);
            sc.SetPersistentInt(P_DMODE, 0);
        }
        return;
    }
    if (sc.GetPersistentInt(P_HIDDEN) != 0)
    {
        sc.SetPersistentInt(P_HIDDEN, 0);
        sc.SetPersistentInt(P_FP1, 0);   // force a full rebuild so zones return
        sc.SetPersistentInt(P_FP2, 0);
    }

    // =========================================================================
    // Guard: VAP data must be available
    // =========================================================================
    if (sc.VolumeAtPriceForBars == nullptr) return;

    // All zone geometry lives in integer ticks (price / sc.TickSize).  A zero or
    // negative tick size would make every conversion undefined, so bail rather
    // than produce garbage zones.
    if (sc.TickSize <= 0.0) return;

    // =========================================================================
    // Read inputs into locals
    // =========================================================================
    const int   lookback     = max(1, In_BarsLookback.GetInt());
    const int   blocksMin    = max(1, In_BlocksPerZone.GetInt());
    // Manual thresholds are absolute magnitudes — sign direction is handled
    // internally (positive delta for sell zones, negative for long zones).
    const float minDelta     = (float)fabs(In_MinBlockDelta.GetFloat());
    const float totalThresh  = (float)fabs(In_ZoneDeltaThresh.GetFloat());
    const int   proxTol      = max(1, In_ProximityTol.GetInt());
    const COLORREF sellFill  = In_SellFillColor.GetColor();
    const COLORREF sellBord  = In_SellBordColor.GetColor();
    const COLORREF buyFill   = In_BuyFillColor.GetColor();
    const COLORREF buyBord   = In_BuyBordColor.GetColor();
    // [v3-C4] Clamp as well as limit: a chartbook saved before the limits
    // existed still carries whatever value was stored then.
    const int   borderWidth  = min(10,  max(1, In_BorderWidth.GetInt()));
    const int   transparency = min(100, max(0, In_Transparency.GetInt()));
    const int   showDelta    = In_ShowDelta.GetYesNo();
    const int   showErased   = In_DisplayMode.GetYesNo();

    int thresholdMode = In_ThresholdMode.GetInt();
    if (thresholdMode < TMODE_MANUAL) thresholdMode = TMODE_MANUAL;
    if (thresholdMode > TMODE_BOTH)   thresholdMode = TMODE_BOTH;

    const int   autoDays     = min(20, max(1, In_AutoDays.GetInt()));
    float autoBlockPct       = In_AutoBlockPct.GetFloat();
    float autoZonePct        = In_AutoZonePct.GetFloat();
    if (autoBlockPct < 1.0f)  autoBlockPct = 1.0f;
    if (autoBlockPct > 99.9f) autoBlockPct = 99.9f;
    if (autoZonePct  < 1.0f)  autoZonePct  = 1.0f;
    if (autoZonePct  > 99.9f) autoZonePct  = 99.9f;

    const COLORREF autoSellFill = In_AutoSellFill.GetColor();
    const COLORREF autoSellBord = In_AutoSellBord.GetColor();
    const COLORREF autoBuyFill  = In_AutoBuyFill.GetColor();
    const COLORREF autoBuyBord  = In_AutoBuyBord.GetColor();
    const int   showAutoStatus  = In_ShowAutoStatus.GetYesNo();

    int autoSession = In_AutoSession.GetIndex();
    if (autoSession < SESS_BOTH) autoSession = SESS_BOTH;
    if (autoSession > SESS_ETH)  autoSession = SESS_BOTH;
    const int rthStartSec = In_RTHStart.GetTime();
    const int rthEndSec   = In_RTHEnd.GetTime();

    int confirmMode = (int)In_ConfirmMode.GetIndex();
    if (confirmMode != CONF_NEXTBAR) confirmMode = CONF_DISPLACE;
    const int confirmTicks    = max(1, In_ConfirmTicks.GetInt());
    const int confirmDeadline = max(0, In_ConfirmDeadline.GetInt());

    const int   exportCSV    = In_ExportCSV.GetYesNo();
    const char* exportFolder = In_ExportFolder.GetString();
    const char* exportFile   = In_ExportFile.GetString();
    if (exportFolder == nullptr) exportFolder = "";
    if (exportFile   == nullptr) exportFile   = "";

    // -- v2.1 Decision Map settings ------------------------------------------
    const int decEnabled   = In_DecEnable.GetYesNo();
    int       decMaxNearby = In_DecMaxNearby.GetInt();
    if (decMaxNearby < 1)  decMaxNearby = 1;
    if (decMaxNearby > 30) decMaxNearby = 30;
    const int decDetail    = In_DecDetail.GetYesNo();
    const int decJournal   = In_DecJournal.GetYesNo();
    const int decSnapshot  = max(0, In_DecSnapshot.GetInt());

    // -- [v4-M1] Cross-chart merge settings -----------------------------------
    int zoneDisplay = (int)In_ZoneDisplay.GetIndex();
    if (zoneDisplay < ZDISP_OWN || zoneDisplay > ZDISP_MERGED) zoneDisplay = ZDISP_OWN;
    const int mergeBounds = (In_MergeBounds.GetIndex() == MBOUND_OVERLAP)
                            ? MBOUND_OVERLAP : MBOUND_UNION;
    const int mergeGap    = min(400, max(0, In_MergeGapTicks.GetInt()));
    int mergeCharts[MERGE_MAX_SOURCES];
    int mergeStudies[MERGE_MAX_SOURCES];
    for (int k = 0; k < MERGE_MAX_SOURCES; k++)
    {
        mergeCharts[k]  = sc.Input[IN_MERGE_SRC_FIRST + k].GetChartNumber();
        mergeStudies[k] = (int)sc.Input[IN_MERGE_SRC_FIRST + k].GetStudyID();
    }
    MergeDrawCfg mergeCfg;
    mergeCfg.SellFill     = sellFill;
    mergeCfg.SellBord     = sellBord;
    mergeCfg.BuyFill      = buyFill;
    mergeCfg.BuyBord      = buyBord;
    mergeCfg.Width        = borderWidth;
    mergeCfg.Transparency = transparency;
    mergeCfg.ShowDelta    = showDelta;
    mergeCfg.ShowErased   = showErased;
    mergeCfg.BoundsMode   = mergeBounds;
    mergeCfg.GapTicks     = mergeGap;
    const int lockZones   = In_LockZones.GetYesNo();   // [v4-M4]
    mergeCfg.Lock         = lockZones;
    const COLORREF erFill = In_ErFill.GetColor();   // [v4-M5]
    const COLORREF erBord = In_ErBord.GetColor();
    const int keepErased  = min(250, max(0, In_KeepErased.GetInt()));
    mergeCfg.ErFill       = erFill;
    mergeCfg.ErBord       = erBord;
    mergeCfg.KeepErased   = keepErased;
    mergeCfg.Days         = nullptr;   // set once the day segments exist

    // Decision Map requires Displacement confirmation. With Next bar open
    // selected the decision outputs stay inactive and legacy operation
    // continues unchanged — never silently reinterpret a next-open survival.
    const int decDisplacement = (confirmMode == CONF_DISPLACE);
    const int decActive = TtdDecisionActive(decEnabled, decDisplacement);
    if (decEnabled && !decDisplacement && sc.GetPersistentInt(P_DLOGGED) == 0)
    {
        sc.SetPersistentInt(P_DLOGGED, 1);
        sc.AddMessageToLog("Trapped Traders Decision Map requires Displacement "
                           "(Zone Confirmation); decision outputs inactive.",
                           1);
    }
    if (!decEnabled || decDisplacement)
        sc.SetPersistentInt(P_DLOGGED, 0);

    // -- Research-mode zone retention -----------------------------------------
    //
    // Normally a rejected zone is deleted from the vector the moment the
    // selected confirmation rule kills it (R2 pruning).  That makes the CSV
    // describe only the survivors of whichever rule In:26 selected, so under
    // Displacement dp_outcome reads almost entirely CONFIRMED and under Next bar
    // open the bias flips — the population is pre-filtered by one of the two
    // things being compared, which makes the comparison meaningless.
    //
    // Exporting two runs and joining them does not fix it either: a pending zone
    // is still merge-eligible, so Displacement's longer pending life absorbs
    // candidates that Next-bar-open leaves as separate zones.  The two runs have
    // different zone TOPOLOGY, not just different verdicts, so no composite key
    // joins them reliably.  One pass over one population is the only fair
    // comparison.
    //
    // COST, and why this is gated on the export input rather than always on:
    // retention makes the zone vector grow to EVERY zone ever detected across
    // the scanned history instead of just the live ones, and the merge loops,
    // the manual-precedence sweep and the shadow tracker are all O(zones) per
    // bar.  A full recalculation over ~60 days with export on is measurably
    // slower than with it off.  That is an acceptable price in research mode and
    // an unacceptable one in normal use, hence the gate.  With In:29 = No the
    // pruning behaviour is exactly what it was before this feature existed.
    const bool retainForExport = (exportCSV != 0);

    const bool  wantManual = (thresholdMode == TMODE_MANUAL || thresholdMode == TMODE_BOTH);
    const bool  wantAuto   = (thresholdMode == TMODE_AUTO   || thresholdMode == TMODE_BOTH);

    // =========================================================================
    // Settings-change detection  (fix B7)
    // =========================================================================
    // v1 packed a few structural inputs into three 16-bit-per-field words:
    // values > 65535 aliased and float edits below 0.5 went undetected.
    // v2 hashes EVERY input, floats by exact bit pattern, into two FNV-1a words
    // with different seeds.  Display-only inputs are hashed too, so a colour,
    // mode, percentile or threshold change rebuilds immediately instead of
    // waiting for the next bar close or a Sierra Chart restart.
    uint32_t hA = 2166136261u;
    uint32_t hB = 40389u;
    for (int pass = 0; pass < 2; pass++)
    {
        uint32_t& h = (pass == 0) ? hA : hB;
        h = FnvAddInt(h,   lookback);
        h = FnvAddInt(h,   blocksMin);
        h = FnvAddFloat(h, minDelta);
        h = FnvAddFloat(h, totalThresh);
        h = FnvAddInt(h,   proxTol);
        h = FnvAddInt(h,   (int)sellFill);
        h = FnvAddInt(h,   (int)sellBord);
        h = FnvAddInt(h,   (int)buyFill);
        h = FnvAddInt(h,   (int)buyBord);
        h = FnvAddInt(h,   borderWidth);
        h = FnvAddInt(h,   transparency);
        h = FnvAddInt(h,   showDelta);
        h = FnvAddInt(h,   showErased);
        h = FnvAddInt(h,   thresholdMode);
        h = FnvAddInt(h,   autoDays);
        h = FnvAddFloat(h, autoBlockPct);
        h = FnvAddFloat(h, autoZonePct);
        h = FnvAddInt(h,   (int)autoSellFill);
        h = FnvAddInt(h,   (int)autoSellBord);
        h = FnvAddInt(h,   (int)autoBuyFill);
        h = FnvAddInt(h,   (int)autoBuyBord);
        h = FnvAddInt(h,   showAutoStatus);
        h = FnvAddInt(h,   autoSession);
        h = FnvAddInt(h,   rthStartSec);
        h = FnvAddInt(h,   rthEndSec);
        h = FnvAddInt(h,   confirmMode);
        h = FnvAddInt(h,   confirmTicks);
        h = FnvAddInt(h,   confirmDeadline);
        // Export settings are hashed too, so switching the export on writes a
        // complete file immediately instead of one that starts at the next bar
        // close, and retargeting the path cannot leave a half-populated file at
        // the old location looking current.
        h = FnvAddInt(h,   exportCSV);
        h = FnvAdd(h,      exportFolder, strlen(exportFolder));
        h = FnvAdd(h,      exportFile,   strlen(exportFile));
        h = FnvAddFloat(h, (float)sc.TickSize);
        // [v4-M1] Zone Display decides whether this chart's own rectangles
        // exist at all, so switching it must rebuild (the rebuild deletes
        // them).  The other merge inputs only change the merged drawing, which
        // is rebuilt from scratch on every render anyway; they are hashed so a
        // change is picked up on the very next call like every other input.
        h = FnvAddInt(h,   zoneDisplay);
        h = FnvAddInt(h,   mergeBounds);
        h = FnvAddInt(h,   mergeGap);
        h = FnvAddInt(h,   lockZones);   // [v4-M4] redraw locked/unlocked at once
        h = FnvAddInt(h,   (int)erFill);   // [v4-M5]
        h = FnvAddInt(h,   (int)erBord);
        h = FnvAddInt(h,   keepErased);
        for (int k = 0; k < MERGE_MAX_SOURCES; k++)
        {
            h = FnvAddInt(h, mergeCharts[k]);
            h = FnvAddInt(h, mergeStudies[k]);
        }
    }
    const int fpA = (int)hA;
    const int fpB = (int)hB;

    const bool settingsChanged = (sc.GetPersistentInt(P_FP1) != fpA) ||
                                 (sc.GetPersistentInt(P_FP2) != fpB);

    // =========================================================================
    // Persistent containers
    // =========================================================================
    vector<ZoneRec>* pZones = (vector<ZoneRec>*)sc.GetPersistentPointer(P_ZONES);
    vector<DaySeg>*  pDays  = (vector<DaySeg>*)sc.GetPersistentPointer(P_DAYS);
    vector<char>*    pSlotM = (vector<char>*)sc.GetPersistentPointer(P_SLOTS_MAN);
    vector<char>*    pSlotA = (vector<char>*)sc.GetPersistentPointer(P_SLOTS_AUTO);

    const bool firstInit = (pZones == nullptr);

    // Historical corrections inside already-processed closed history arrive
    // as UpdateStartIndex > 0 at or below the processed frontier (BuildSpec
    // §7/§8). They must rebuild semantic state and day caches; forming-only
    // ticks (index above the frontier) and normal appends stay incremental.
    const int prevLastBarEarly = sc.GetPersistentInt(P_LASTBAR);
    const int legacyHistCorr =
        (sc.UpdateStartIndex > 0 && prevLastBarEarly >= 0 &&
         sc.UpdateStartIndex <= prevLastBarEarly) ? 1 : 0;

    bool isFullRecalc = (sc.UpdateStartIndex == 0) || settingsChanged ||
                        (legacyHistCorr != 0);

    if (isFullRecalc || pZones == nullptr || pDays == nullptr ||
        pSlotM == nullptr || pSlotA == nullptr)
    {
        isFullRecalc = true;

        sc.SetPersistentInt(P_FP1, fpA);
        sc.SetPersistentInt(P_FP2, fpB);
        sc.SetPersistentInt(P_RESERVED, 0);
        sc.SetPersistentInt(P_LOGGED_HIST, 0);
        sc.SetPersistentInt(P_LOGGED_SLOTS, 0);
        sc.SetPersistentInt(P_LOGGED_CSV, 0);

        // Erase existing drawings, then rebuild from scratch.
        //   First initialisation (study just added, or chartbook just loaded):
        //     blanket sweep of every line number this study can own.  User-drawn
        //     drawings are saved in the chartbook, so rectangles from a previous
        //     session — including junk left behind by v1's broken delete — exist
        //     with no matching zone record and can only be removed this way.
        //   Later full recalcs (settings change): the live zone list is exact and
        //     the blanket sweep's 24000 API calls are not worth paying.
        if (firstInit)
        {
            DeleteAllStudyDrawings(sc, MAX_ZONE_SLOTS);
        }
        else if (pZones)
        {
            for (size_t i = 0; i < pZones->size(); i++)
                DeleteZoneDrawings(sc, (*pZones)[i]);
            DeleteDrawing(sc, StudyLine(sc, LINE_STATUS));
            DeleteDrawing(sc, StudyLine(sc, LINE_WARNING));
        }
        DeleteMergedDrawings(sc);   // [v4-M1] rebuilt by PASS 4M if still merging
        sc.SetPersistentInt(P_STATUS_DRAWN, 0);
        sc.SetPersistentInt(P_WARN_DRAWN, 0);

        if (pZones) delete pZones;
        if (pDays)  delete pDays;
        if (pSlotM) delete pSlotM;
        if (pSlotA) delete pSlotA;

        pZones = new vector<ZoneRec>();
        pDays  = new vector<DaySeg>();
        pSlotM = new vector<char>(MAX_ZONE_SLOTS, 0);
        pSlotA = new vector<char>(MAX_ZONE_SLOTS, 0);

        sc.SetPersistentPointer(P_ZONES,      pZones);
        sc.SetPersistentPointer(P_DAYS,       pDays);
        sc.SetPersistentPointer(P_SLOTS_MAN,  pSlotM);
        sc.SetPersistentPointer(P_SLOTS_AUTO, pSlotA);
        sc.SetPersistentInt(P_IDCTR, 1);
        sc.SetPersistentInt(P_LASTBAR, -1);
    }

    vector<ZoneRec>& zones  = *pZones;
    vector<DaySeg>&  days   = *pDays;
    vector<char>&    slotsM = *pSlotM;
    vector<char>&    slotsA = *pSlotA;

    if (sc.ArraySize <= 0) return;

    const int curBar  = sc.ArraySize - 1;   // currently forming bar
    const int lastBar = sc.ArraySize - 2;   // last CLOSED bar

    // =========================================================================
    // Decision Map engine lifecycle (v2.1, Phase 2)
    // =========================================================================
    // Semantic fingerprint covers STRUCTURAL inputs only: detector geometry /
    // thresholds / modes, displacement / deadline, and the enable switch.
    // Display-only (In:33, colors, transparency, Show Delta Info, Show Erased
    // Zones), export-control (In:34) and snapshot-request (In:35) inputs are
    // never fed, so cosmetic changes refresh drawings without resetting the
    // semantic event history. Rebuild (new generation) on: first use,
    // explicit full recalculation (UpdateStartIndex == 0), array rewind below
    // the processed frontier, or a structural fingerprint change. A legacy
    // display-only rescan does NOT rebuild: the per-bar watermark below lets
    // already-processed bars pass through as no-ops.
    unsigned int dFpA = 2166136261u;
    unsigned int dFpB = 40389u;
    for (int pass = 0; pass < 2; pass++)
    {
        unsigned int& dh = (pass == 0) ? dFpA : dFpB;
        dh = TtdFnv1aInt(dh,   decEnabled);
        dh = TtdFnv1aInt(dh,   thresholdMode);
        dh = TtdFnv1aInt(dh,   lookback);
        dh = TtdFnv1aInt(dh,   blocksMin);
        dh = TtdFnv1aFloat(dh, minDelta);
        dh = TtdFnv1aFloat(dh, totalThresh);
        dh = TtdFnv1aInt(dh,   proxTol);
        dh = TtdFnv1aInt(dh,   autoDays);
        dh = TtdFnv1aFloat(dh, autoBlockPct);
        dh = TtdFnv1aFloat(dh, autoZonePct);
        dh = TtdFnv1aInt(dh,   autoSession);
        dh = TtdFnv1aInt(dh,   rthStartSec);
        dh = TtdFnv1aInt(dh,   rthEndSec);
        dh = TtdFnv1aInt(dh,   confirmMode);
        dh = TtdFnv1aInt(dh,   confirmTicks);
        dh = TtdFnv1aInt(dh,   confirmDeadline);
        dh = TtdFnv1aFloat(dh, (float)sc.TickSize);
    }

    TtdEngine* pDec = (TtdEngine*)sc.GetPersistentPointer(P_DECISION);
    int decMark = sc.GetPersistentInt(P_DLASTBAR);
    const bool decFpChanged = (sc.GetPersistentInt(P_DFPA) != (int)dFpA) ||
                              (sc.GetPersistentInt(P_DFPB) != (int)dFpB);
    // Rebuild policy is the portable-tested TtdNeedsRebuildEx (Task 7):
    // first use, explicit full recalculation, rewind/shrink below the
    // processed frontier, historical correction at or below the frontier,
    // or structural fingerprint change. New bars and cosmetic-only rescans
    // continue incrementally. The adapter calls this exact function.
    const int decNeedRebuild = TtdNeedsRebuildEx(pDec != nullptr ? 1 : 0,
                                                 sc.UpdateStartIndex == 0 ? 1 : 0,
                                                 sc.UpdateStartIndex,
                                                 lastBar, decMark,
                                                 decFpChanged ? 1 : 0);
    if (!decActive && pDec != nullptr)
    {
        // Inactive: hold no engine. Toggling back on changes the fingerprint
        // (In:32 is structural) and rebuilds a fresh generation.
        delete pDec;
        sc.SetPersistentPointer(P_DECISION, nullptr);
        pDec = nullptr;
    }
    if (decActive && decNeedRebuild)
    {
        if (pDec == nullptr)
        {
            pDec = new (std::nothrow) TtdEngine();
            sc.SetPersistentPointer(P_DECISION, pDec);
        }
        if (pDec != nullptr)
        {
            TtdReset(pDec);
            decMark = -1;
            sc.SetPersistentInt(P_DLASTBAR, -1);
            sc.SetPersistentInt(P_DFPA, (int)dFpA);
            sc.SetPersistentInt(P_DFPB, (int)dFpB);
            sc.SetPersistentInt(P_DGEN, sc.GetPersistentInt(P_DGEN) + 1);
            sc.SetPersistentInt(P_DEXPFAIL, 0);
            sc.SetPersistentInt(P_DJSEQ, 0);  // new generation -> new journal
                                              // files; the seq watermark resets
            sc.SetPersistentInt(P_DJATT, 0);  // attempt high-waters reset with
            sc.SetPersistentInt(P_DSATT, 0);  // the generation (review-fix-2)
            sc.SetPersistentInt(P_DHALTLOG, 0);  // halt-log latch resets with
                                                 // the generation (review-fix-3)
            sc.SetPersistentInt(P_DVFYLOG, 0);   // [v3-P1] sweep cross-check latch
        }
    }
    // Engine allocation failure: visible internal-fault status (SG19 bit 16
    // in the Task 6 write path), legacy operation continues.
    bool decHalted = (pDec != nullptr) && (pDec->halted != 0);
    const bool decRebuiltTmp = (decActive && pDec != nullptr && decNeedRebuild);
    // Coordinated rebuild (review-fix-1): a decision-only rebuild must NOT
    // replay old windows atop live legacy zones — that would duplicate and
    // merge against already-current state. Promote it to a joint full
    // rebuild so both layers replay deterministically from scratch with the
    // same thresholds. Legacy outputs are preserved by construction (full
    // rebuild vs incremental equivalence), not by reusing live records.
    if (decRebuiltTmp && !isFullRecalc)
    {
        for (size_t i = 0; i < zones.size(); i++)
            DeleteZoneDrawings(sc, zones[i]);
        DeleteDrawing(sc, StudyLine(sc, LINE_STATUS));
        DeleteDrawing(sc, StudyLine(sc, LINE_WARNING));
        sc.SetPersistentInt(P_STATUS_DRAWN, 0);
        sc.SetPersistentInt(P_WARN_DRAWN, 0);
        sc.SetPersistentInt(P_RESERVED, 0);
        sc.SetPersistentInt(P_LOGGED_HIST, 0);
        sc.SetPersistentInt(P_LOGGED_SLOTS, 0);
        sc.SetPersistentInt(P_LOGGED_CSV, 0);
        zones.clear();
        days.clear();
        slotsM.assign(MAX_ZONE_SLOTS, 0);
        slotsA.assign(MAX_ZONE_SLOTS, 0);
        sc.SetPersistentInt(P_IDCTR, 1);
        sc.SetPersistentInt(P_LASTBAR, -1);
        isFullRecalc = true;
    }
    const bool decRebuilt = decRebuiltTmp;
    const int decMarkBefore = decMark;  // SG write frontier below (Task 6)
    vector<int> decVapPrices;
    vector<double> decVapAsk, decVapBid;

    // =========================================================================
    // Trading-day segmentation (covers the forming bar too, so the status label
    // and SG3/SG4/SG5 are correct on the right edge)
    // =========================================================================
    BuildDaySegments(sc, days, curBar, isFullRecalc);
    mergeCfg.Days = &days;   // [v4-M5] merged-zone expiry uses this chart's days

    // =========================================================================
    // Skip intrabar ticks — only process on a NEW CLOSED BAR or full recalc
    // =========================================================================
    // Fix B2: the guard now tracks the last processed CLOSED bar, and PASS 1
    // scans every window from prevLastBar+1 to lastBar.  v1 compared ArraySize
    // and then scanned exactly one window, so a reconnect/backfill delivering
    // several closed bars at once silently skipped the intermediate windows.
    // Review-fix-1: the early return sits AFTER the engine reset above, so a
    // rebuild always falls through to the full rescan (winEndStart covers
    // history when isFullRecalc). A no-new-bar callback with only
    // display/export-control changes (MaxNearby/detail/snapshot/journal,
    // outside both fingerprints) falls through as a visual-only refresh:
    // the PASS 1 window is empty (zero semantic/VAP work, engine untouched)
    // while the renderer/panel/journal/snapshot paths below still run.
    // Ordinary unchanged ticks return here with zero work.
    int prevLastBar = sc.GetPersistentInt(P_LASTBAR);
    bool visualOnly = false;
    if (!isFullRecalc && lastBar <= prevLastBar)
    {
        const int nowRendering = (decActive && pDec != nullptr) ? 1 : 0;
        // [v4-M1] Sources close bars on their own timeframes, not this one's.
        // Without this a 15m merging chart would show a zone a 1m source
        // confirmed up to 15 minutes late.  Resolving six sources is a
        // handful of API calls, and the redraw only runs when one of them
        // actually processed a bar, so ordinary ticks stay near-free.
        if (zoneDisplay == ZDISP_MERGED && !nowRendering)
        {
            MergeSource msrc[1 + MERGE_MAX_SOURCES];
            MergeResolveSources(sc, &zones, mergeCharts, mergeStudies, msrc);
            if (MergeSignature(msrc) != sc.GetPersistentInt(P_MSIG))
                RenderMergedZones(sc, msrc, mergeCfg);
        }
        if (nowRendering)
        {
            const int lastMaxN = sc.GetPersistentInt(P_DMAXN);
            const int lastDet = sc.GetPersistentInt(P_DDETL);
            // Attempt-aware high-waters (review-fix-2, BuildSpec §6/§7):
            // already-attempted failed work must not reschedule file
            // attempts on unchanged ticks; genuinely new rows or a new
            // request increase still refresh. Served watermarks stay
            // honest in storage; only the scheduling comparison folds in
            // the attempted high-water.
            const int lastJS = max(sc.GetPersistentInt(P_DJSEQ),
                                   sc.GetPersistentInt(P_DJATT));
            const int lastSnap = max(sc.GetPersistentInt(P_DSNAP),
                                     sc.GetPersistentInt(P_DSATT));
            if (TtdVisualRefreshDue(lastMaxN, decMaxNearby, lastDet,
                                    decDetail, lastSnap, decSnapshot,
                                    decJournal, lastJS,
                                    pDec->numTransitions))
            {
                visualOnly = true;  // empty semantic window, refresh visuals
            }
            else
            {
                return;  // intrabar tick — nothing to do
            }
        }
        else if (TtdDecisionOffCleanupDue(sc.GetPersistentInt(P_DMODE),
                                          nowRendering))
        {
            // Decision Map just turned OFF on an otherwise unchanged
            // callback (review-fix-2): fall through once with an empty
            // semantic window (winEndStart > lastBar below, zero
            // engine/VAP work) so PASS 3D flags SG7-SG19 inactive, PASS 3E
            // removes owned decision drawings and PASS 4 restores legacy
            // zone rendering from the preserved canonical records. Steady
            // OFF keeps returning below; the latch clears in PASS 3E.
            visualOnly = true;
        }
        else
        {
            return;  // intrabar tick — nothing to do
        }
    }

    if (lastBar < lookback - 1)
    {
        sc.SetPersistentInt(P_LASTBAR, lastBar);
        return;                                  // not enough closed bars yet
    }

    // =========================================================================
    // PASS 1 — Scan windows for new zone candidates
    // =========================================================================
    // Incremental : scan every window from prevLastBar+1 .. lastBar (fix B2).
    // Full recalc : slide across ALL history one bar at a time, replicating
    //               what would have happened had the study run live.
    //
    // Chain-merge guard (unchanged from v1): a zone can never grow wider than
    // proxTol ticks, otherwise consecutive overlapping windows in a trend keep
    // merging and the zone grows one tick per bar.
    // Runaway backstop only.  MERGING EXPANDS A ZONE TO THE UNION of itself and
    // the overlapping candidate — that is what merge means, and a candidate is
    // never discarded: if no live zone can absorb it, it becomes its own zone.
    //
    // v1 capped the merged span at proxTol, but a candidate already spans up to
    // proxTol, so a full-height zone could not absorb a candidate offset by even
    // one tick.  The real defence against a trend chain-merging a zone to 100
    // points is that price closing through a zone now ERASES it inline, mid
    // scan, so it stops absorbing candidates (see the inline lifecycle block in
    // PASS 1).  This cap is just a last-resort bound.
    const int maxZoneWidthTicks = proxTol * 4;

    int winEndStart;
    // Review-fix-1: a decision rebuild always implies isFullRecalc via the
    // coordinated promotion above, but the decRebuilt term is kept explicit
    // so the window scan is driven from the decision rebuild too (never a
    // zero-bar rescan over a cleared engine/SGs).
    if (isFullRecalc || decRebuilt)
        winEndStart = lookback - 1;
    else
        winEndStart = max(lookback - 1, prevLastBar + 1);
    if (winEndStart < 0) winEndStart = 0;

    BandScratch  scratch;
    vector<float> blockBuf, zoneBuf;
    blockBuf.reserve(4096);
    zoneBuf.reserve(1024);

    // Sparse legacy SG rewrite (Task 8): detection-bar cells whose zone
    // content changes are re-resolved individually on incremental calls.
    // Full-history clearing stays reserved for explicit rebuilds.
    TtdDirtySet sgDirty;
    TtdDirtyClear(&sgDirty);

    // Per-day threshold cache snapshot for flip detection (a day whose
    // readiness completes this call needs its whole bar range rewritten).
    struct DayCacheSnap
    {
        int   FirstBar;
        int   LastBar;
        int   Computed;
        int   Ready;
        float Block;
        float Zone;
    };
    vector<DayCacheSnap> daySnap;
    daySnap.reserve(days.size() + 1);
    for (size_t di = 0; di < days.size(); di++)
    {
        DayCacheSnap s;
        s.FirstBar = days[di].FirstBar;
        s.LastBar  = days[di].LastBar;
        s.Computed = days[di].ThreshComputed ? 1 : 0;
        s.Ready    = days[di].ThreshReady ? 1 : 0;
        s.Block    = days[di].AutoMinBlock;
        s.Zone     = days[di].AutoZoneTotal;
        daySnap.push_back(s);
    }

    int dayCursor = 0;   // monotonic index into days[] tracking winEnd's day

    // [v3-P2] Live / shadow-pending index lists.
    //
    // Every per-window loop below used to walk the WHOLE zone vector and skip
    // what it did not want. That vector is not the live picture: rejected and
    // suppressed records stay in it until the R2 prune, which runs only AFTER
    // this loop, and with CSV export on (retainForExport) they are never
    // removed at all. So on a full recalculation the record count grows with
    // history while five O(zones) loops run per bar — the scan was quadratic in
    // chart length, and export mode made it permanently so.
    //
    // The two lists below hold zone INDICES, which stay valid because PASS 1
    // only ever appends to `zones` (the prune runs after). They are compacted
    // once at the end of each window, so every loop is O(live) or O(still
    // undecided) instead of O(zones-ever). Iteration order is unchanged —
    // ascending zone index — so first-match merges pick the same zone as
    // before.
    vector<int> liveIdx;      // Valid zones
    vector<int> shadowIdx;    // zones with at least one unresolved shadow verdict
    liveIdx.reserve(zones.size() + 64);
    shadowIdx.reserve(zones.size() + 64);
    for (size_t zi = 0; zi < zones.size(); zi++)
    {
        if (zones[zi].Valid)
            liveIdx.push_back((int)zi);
        if (zones[zi].NB_Outcome == CONFOUT_PENDING ||
            zones[zi].DP_Outcome == CONFOUT_PENDING)
            shadowIdx.push_back((int)zi);
    }

    for (int winEnd = winEndStart; winEnd <= lastBar; winEnd++)
    {
        int winStart = winEnd - lookback + 1;
        if (winStart < 0) winStart = 0;

        // -- Which trading day does this window END in? ------------------------
        while (dayCursor + 1 < (int)days.size() && days[dayCursor].LastBar < winEnd)
            dayCursor++;
        while (dayCursor > 0 && days[dayCursor].FirstBar > winEnd)
            dayCursor--;

        // -- Auto thresholds active on that day (computed once, then cached) ---
        bool  autoReady     = false;
        float autoMinBlock  = 0.0f;
        float autoZoneTotal = 0.0f;
        if (wantAuto)
        {
            EnsureDayThresholds(sc, days, dayCursor, autoDays, lookback, blocksMin,
                                proxTol, autoBlockPct, autoZonePct,
                                autoSession, rthStartSec, rthEndSec,
                                scratch, blockBuf, zoneBuf);
            autoReady     = days[dayCursor].ThreshReady;
            autoMinBlock  = days[dayCursor].AutoMinBlock;
            autoZoneTotal = days[dayCursor].AutoZoneTotal;
        }

        // -- Shadow confirmation tracking: BOTH rules, EVERY zone --------------
        //
        // In:26 picks ONE rule to run the state machine, so on its own the study
        // can never answer "would the other rule have done better" — the two
        // rules would each be judged on a zone population they themselves
        // shaped, since a rejection removes a zone that would otherwise have
        // gone on absorbing candidates.  Scoring both rules against the SAME
        // population is the only fair comparison, so both verdicts are recorded
        // here and NEITHER is allowed to touch Pending, Valid, geometry or
        // merging.  The selected rule alone does that, in the block below.
        //
        // This runs before that block deliberately: it must see the zone list as
        // the selected rule found it, not as the selected rule left it.  It is
        // also why the loop does not skip !Valid zones — a zone the selected
        // rule has already killed still has an unfinished verdict under the
        // other rule, right up to the point the pruning pass removes the record.
        //
        // Coverage: every bar after a zone's DetectedBar passes through this
        // loop exactly once.  A full recalc scans from lookback-1, and an
        // incremental call scans prevLastBar+1..lastBar while zones carry their
        // verdicts across calls, so no bar is skipped and none is scored twice.
        for (size_t si = 0; si < shadowIdx.size(); si++)
        {
            ZoneRec& z = zones[shadowIdx[si]];
            if (winEnd <= z.DetectedBar) continue;
            if (z.NB_Outcome != CONFOUT_PENDING && z.DP_Outcome != CONFOUT_PENDING)
                continue;

            const float sTopP = (float)(z.TopTick    * sc.TickSize);
            const float sBotP = (float)(z.BottomTick * sc.TickSize);

            // Next-bar-open rule: decided once, on DetectedBar+1, and never
            // revisited — the rule has nothing to say about any later bar.
            if (z.NB_Outcome == CONFOUT_PENDING && winEnd == z.DetectedBar + 1)
            {
                const float openNext = sc.Open[winEnd];
                const bool  rejected = z.IsBuyZone ? (openNext < sBotP)
                                                   : (openNext > sTopP);
                z.NB_Outcome = rejected ? CONFOUT_REJECTED : CONFOUT_CONFIRMED;
                z.NB_Bar     = winEnd;
            }

            // Displacement rule: runs until it resolves, exactly as the live
            // displacement path does, including the optional In:28 deadline.
            if (z.DP_Outcome == CONFOUT_PENDING)
            {
                const float clP = sc.Close[winEnd];

                if (z.IsBuyZone ? (clP < sBotP) : (clP > sTopP))
                {
                    z.DP_Outcome = CONFOUT_REJECTED;   // closed through it the wrong way
                    z.DP_Bar     = winEnd;
                }
                else
                {
                    const float confirmP = z.IsBuyZone
                        ? (float)((z.TopTick    + confirmTicks) * sc.TickSize)
                        : (float)((z.BottomTick - confirmTicks) * sc.TickSize);

                    const bool displaced = z.IsBuyZone ? (clP >= confirmP)
                                                       : (clP <= confirmP);
                    if (displaced)
                    {
                        z.DP_Outcome = CONFOUT_CONFIRMED;
                        z.DP_Bar     = winEnd;
                    }
                    else if (confirmDeadline > 0 &&
                             winEnd >= z.DetectedBar + confirmDeadline)
                    {
                        z.DP_Outcome = CONFOUT_REJECTED;   // never displaced in time
                        z.DP_Bar     = winEnd;
                    }
                }
            }
        }

        // -- Inline zone lifecycle, against the bar that just closed -----------
        //
        // v1 scanned the ENTIRE history for candidates first and only then ran
        // confirmation and close-through erasure.  During that scan every zone
        // still looked alive, so a zone price had already broken kept absorbing
        // later candidates, and a zone that was about to be rejected swallowed
        // candidates that then vanished with it.  Two visible consequences:
        // zones disappearing at a LOWER percentile than they appeared at, and a
        // trend chain-merging one zone ever wider until the width cap fought it.
        //
        // Resolving each zone against bar winEnd as the scan reaches it makes a
        // full recalculation reproduce what the study would have done live —
        // which is what v1's own comment claimed it was doing.  A dead zone stops
        // absorbing candidates, so those candidates now become zones of their own.
        for (size_t li = 0; li < liveIdx.size(); li++)
        {
            ZoneRec& z = zones[liveIdx[li]];
            if (!z.Valid) continue;

            // Hoisted out of both branches below, which each tested it.
            if (winEnd <= z.DetectedBar) continue;

            // [v3-P3] This bar is now resolved against this zone under whichever
            // rule is active, so PASS 2 must not walk it again. Both the pending
            // displacement test and the confirmed clip test use the same
            // close-through predicate, so recording the bar here is sound for
            // either branch.
            z.CheckedTo = winEnd;

            const float topP = (float)(z.TopTick    * sc.TickSize);
            const float botP = (float)(z.BottomTick * sc.TickSize);

            if (z.Pending)
            {
                if (confirmMode == CONF_NEXTBAR)
                {
                    // v1 rule: did the very next bar OPEN through the zone?
                    // The step helper holds the legacy strict comparisons
                    // (portable-tested, Task 8); verdict 3 is the same-bar
                    // close-through bypass fix — the old unconditional
                    // `continue` below skipped erasure for a bar whose close
                    // had already broken the zone.
                    if (z.DetectedBar + 1 == winEnd)
                    {
                        const float openNext = sc.Open[winEnd];
                        const float clP = sc.Close[winEnd];
                        const int v = TtdLegacyNextBarStep(
                            z.IsBuyZone ? 1 : 0, winEnd, z.DetectedBar,
                            openNext, clP, topP, botP);
                        z.Pending = false;
                        if (v == 2)
                        {
                            z.Valid = false;   // ErasedBar stays -1
                            if (z.Slot >= 0)
                            {
                                FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM,
                                         z.Slot);
                                z.Slot = -1;
                            }
                            TtdDirtyMark(&sgDirty, z.DetectedBar);
                            continue;
                        }
                        if (v == 3)
                        {
                            z.Valid     = false;
                            z.ErasedBar = winEnd;
                            if (!showErased) DeleteZoneDrawings(sc, z);
                            if (z.Slot >= 0)
                            {
                                FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM,
                                         z.Slot);
                                z.Slot = -1;
                            }
                            TtdDirtyMark(&sgDirty, z.DetectedBar);
                            continue;
                        }
                        // v == 1 confirmed: same-bar close already evaluated.
                        TtdDirtyMark(&sgDirty, z.DetectedBar);
                    }
                    continue;
                }

                // ---- Displacement confirmation (proof of pain) ---------------
                // The open-only test cannot see price TRADING THROUGH the zone
                // after that open, so a zone could confirm and be drawn at prices
                // the market had already rejected it at.  A zone now has to earn
                // confirmation: price must CLOSE beyond it on the trapped side.
                //
                //   Sell zone (trapped longs, resistance)
                //       confirm  close <= BottomTick - K
                //       discard  close >  TopTick          (buyers were not trapped)
                //   Long zone (trapped shorts, support)
                //       confirm  close >= TopTick + K
                //       discard  close <  BottomTick       (sellers were not trapped)
                //
                // A zone waiting is NOT drawn, so nothing repaints; it either
                // earns its place or is discarded having never appeared.
                const float clP = sc.Close[winEnd];

                const bool brokeThrough = z.IsBuyZone ? (clP < botP) : (clP > topP);
                if (brokeThrough)
                {
                    z.Valid   = false;
                    z.Pending = false;      // ErasedBar stays -1: never drawn
                    if (z.Slot >= 0)        // recycle promptly under batches
                    {
                        FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                        z.Slot = -1;
                    }
                    TtdDirtyMark(&sgDirty, z.DetectedBar);
                    continue;
                }

                const float confirmP = z.IsBuyZone
                    ? (float)((z.TopTick    + confirmTicks) * sc.TickSize)
                    : (float)((z.BottomTick - confirmTicks) * sc.TickSize);

                const bool displaced = z.IsBuyZone ? (clP >= confirmP)
                                                   : (clP <= confirmP);
                if (displaced)
                {
                    z.Pending = false;      // CONFIRMED — drawn from PASS 4 onward
                    TtdDirtyMark(&sgDirty, z.DetectedBar);
                    continue;
                }

                if (confirmDeadline > 0 && winEnd >= z.DetectedBar + confirmDeadline)
                {
                    z.Valid   = false;      // never displaced in time
                    z.Pending = false;
                    if (z.Slot >= 0)
                    {
                        FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                        z.Slot = -1;
                    }
                    TtdDirtyMark(&sgDirty, z.DetectedBar);
                }
                continue;
            }

            const float cl = sc.Close[winEnd];
            const bool clipped = z.IsBuyZone ? (cl < botP) : (cl > topP);
            if (clipped)
            {
                z.Valid     = false;
                z.ErasedBar = winEnd;
                if (!showErased) DeleteZoneDrawings(sc, z);
                if (!showErased && z.Slot >= 0)  // clipped records leave the pool
                {
                    FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                    z.Slot = -1;
                }
                TtdDirtyMark(&sgDirty, z.DetectedBar);
            }
        }

        // -- Decision Map: chronological closed-bar step for winEnd ------------
        // Spec §3 order per bar: lifecycle pending/active episodes first,
        // candidates are fed later in this iteration (pre-merge sites below).
        // Already-processed bars are no-ops inside the core; the watermark
        // only advances for newly processed bars so display-only rescans can
        // never duplicate associations.
        const bool decDoBar = (decActive && pDec != nullptr && winEnd > decMark);
        if (decDoBar && !decHalted)
        {
            TtdBar dbar;
            FillDecisionBar(sc, winEnd, dbar, decVapPrices, decVapAsk, decVapBid);
            TtdProcessBar(pDec, &dbar);
            if (pDec->halted)
                decHalted = true;
        }

        // -- Aggregate the window's proximity bands once -----------------------
        BuildBands(sc, winStart, winEnd, proxTol, scratch, false);
        if (!scratch.Valid)
        {
            // Review-fix-3: the frontier advances even when halted (lifecycle
            // ran or the layer stopped on this bar); only the SG window below
            // publishes the halt status, so stalling here hid it forever.
            if (TtdHaltAdvanceMark(decDoBar ? 1 : 0, decHalted ? 1 : 0))
                decMark = winEnd;  // lifecycle ran; no candidates existed
            continue;
        }

        // Manual ranges created or widened by THIS window — at most one per
        // direction.  The precedence sweep only has to test auto zones against
        // these, not against every manual zone, which keeps the sweep O(auto
        // zones) per window instead of O(auto x manual).
        int  changedManBot[2] = { 0, 0 };
        int  changedManTop[2] = { 0, 0 };
        bool changedMan[2]    = { false, false };

        // -- Manual candidates first (deterministic manual precedence) ---------
        for (int side = 0; side < 2 && wantManual; side++)
        {
            const bool isBuySide = (side == 1);
            Candidate c = isBuySide
                          ? ScanLong(scratch, proxTol, blocksMin, minDelta, totalThresh)
                          : ScanSell(scratch, proxTol, blocksMin, minDelta, totalThresh);
            if (!c.Found) continue;

            // Decision feed: the ORIGINAL candidate, before mutable merging.
            if (decDoBar && !decHalted)
                FeedDecisionCandidate(pDec, winEnd, isBuySide,
                                      c.BottomTick, c.TopTick, c.TotalMagnitude,
                                      c.Count, TTD_SRC_MANUAL, confirmTicks,
                                      confirmDeadline, minDelta, totalThresh);

            bool merged = false;
            for (size_t li = 0; li < liveIdx.size(); li++)
            {
                ZoneRec& ez = zones[liveIdx[li]];
                if (!ez.Valid) continue;
                if (ez.Source != ZSRC_MANUAL) continue;      // same-source merge only
                if (ez.IsBuyZone != isBuySide) continue;     // same-direction merge only
                if (!TicksOverlapInclusive(c.BottomTick, c.TopTick, ez.BottomTick, ez.TopTick))
                    continue;

                const int newBot = min(ez.BottomTick, c.BottomTick);
                const int newTop = max(ez.TopTick,    c.TopTick);
                if ((newTop - newBot) > maxZoneWidthTicks)
                    continue;   // this zone cannot absorb it; try the next one

                TtdDirtyMark(&sgDirty, ez.DetectedBar);  // pre-merge cell
                ez.BottomTick = newBot;
                ez.TopTick    = newTop;
                if (winEnd < ez.DetectedBar) ez.DetectedBar = winEnd;
                TtdDirtyMark(&sgDirty, ez.DetectedBar);  // post-merge cell
                // Fix B6: keep the larger magnitude.  Summing every merge would
                // inflate TotalDelta without bound along a merge chain, because
                // consecutive windows overlap by lookback-1 bars and re-aggregate
                // the same VAP levels.  Max = strongest single-window observation.
                {
                    const float mag = max((float)fabs(ez.TotalDelta), c.TotalMagnitude);
                    ez.TotalDelta = isBuySide ? -mag : mag;
                }
                // Same reasoning as B6, applied to the level count: keep the
                // larger, because consecutive windows re-count the same levels.
                if (c.Count > ez.BlockCount) ez.BlockCount = c.Count;
                merged = true;
                // [v4-M3] The widened zone may now overlap other manual zones.
                const int svi = AbsorbOverlappingZones(sc, zones, liveIdx, liveIdx[li],
                                                       maxZoneWidthTicks, slotsM, &sgDirty);
                changedMan[side]    = true;
                changedManBot[side] = zones[svi].BottomTick;
                changedManTop[side] = zones[svi].TopTick;
                break;
            }

            if (!merged)
            {
                ZoneRec z;
                z.ID          = sc.GetPersistentInt(P_IDCTR);
                sc.SetPersistentInt(P_IDCTR, z.ID + 1);
                z.Slot        = AllocSlotEvicting(sc, zones, slotsM, ZSRC_MANUAL,
                                                  retainForExport);
                z.TopTick     = c.TopTick;
                z.BottomTick  = c.BottomTick;
                z.DetectedBar = winEnd;
                z.ErasedBar   = -1;
                z.IsBuyZone   = isBuySide;
                z.TotalDelta  = isBuySide ? -c.TotalMagnitude : c.TotalMagnitude;
                z.Valid       = true;
                z.Pending     = true;     // held for next-bar price confirmation
                z.Drawn       = false;
                z.Source      = ZSRC_MANUAL;
                z.NB_Outcome  = CONFOUT_PENDING;
                z.NB_Bar      = -1;
                z.DP_Outcome  = CONFOUT_PENDING;
                z.DP_Bar      = -1;
                z.BlockCount  = c.Count;
                z.CheckedTo   = winEnd;   // [v3-P3] nothing after detection yet
                z.AbsorbedInto = -1;      // [v4-M3]
                zones.push_back(z);
                liveIdx.push_back((int)zones.size() - 1);
                shadowIdx.push_back((int)zones.size() - 1);
                TtdDirtyMark(&sgDirty, z.DetectedBar);  // new cell when confirmed
                changedMan[side]    = true;
                changedManBot[side] = z.BottomTick;
                changedManTop[side] = z.TopTick;
            }
        }

        // -- Manual-precedence sweep -------------------------------------------
        // A manual zone created or widened in THIS window may now intersect an
        // auto zone that already existed.  Kill those auto zones so the
        // "never two overlapping rectangles" invariant holds at all times.
        // Runs before auto candidates are processed, so the new auto zones below
        // are only ever tested against the surviving manual picture.
        if (thresholdMode == TMODE_BOTH && (changedMan[0] || changedMan[1]))
        {
            for (size_t li = 0; li < liveIdx.size(); li++)
            {
                ZoneRec& az = zones[liveIdx[li]];
                if (!az.Valid || az.Source != ZSRC_AUTO) continue;

                bool hit = false;
                for (int s = 0; s < 2; s++)
                {
                    if (!changedMan[s]) continue;
                    if (TicksIntersectInclusive(az.BottomTick, az.TopTick,
                                                changedManBot[s], changedManTop[s]))
                    {
                        hit = true;
                        break;
                    }
                }
                if (!hit) continue;

                DeleteZoneDrawings(sc, az);
                az.Valid     = false;
                az.Pending   = false;
                az.ErasedBar = -1;    // suppressed: treated like a rejected zone
                if (az.Slot >= 0)     // recycle promptly under batches
                {
                    FreeSlot(slotsA, az.Slot);
                    az.Slot = -1;
                }
                TtdDirtyMark(&sgDirty, az.DetectedBar);
            }
        }

        // -- Auto candidates second, suppressed by live manual zones -----------
        // Auto detection is restricted to the same session as the baseline, so a
        // zone is never gated by thresholds measured in a different regime.
        const bool autoSessionOK =
            BarInAutoSession(sc, winEnd, autoSession, rthStartSec, rthEndSec);

        for (int side = 0; side < 2 && wantAuto && autoReady && autoSessionOK; side++)
        {
            const bool isBuySide = (side == 1);
            Candidate c = isBuySide
                          ? ScanLong(scratch, proxTol, blocksMin, autoMinBlock, autoZoneTotal)
                          : ScanSell(scratch, proxTol, blocksMin, autoMinBlock, autoZoneTotal);
            if (!c.Found) continue;

            // Decision feed: the ORIGINAL auto candidate with the thresholds
            // actually used, before mutable merging or manual suppression.
            if (decDoBar && !decHalted)
                FeedDecisionCandidate(pDec, winEnd, isBuySide,
                                      c.BottomTick, c.TopTick, c.TotalMagnitude,
                                      c.Count, TTD_SRC_AUTO, confirmTicks,
                                      confirmDeadline, autoMinBlock,
                                      autoZoneTotal);

            // Manual precedence: direction-agnostic, inclusive tick intersection.
            bool suppressed = false;
            for (size_t li = 0; li < liveIdx.size(); li++)
            {
                const ZoneRec& mz = zones[liveIdx[li]];
                if (!mz.Valid || mz.Source != ZSRC_MANUAL) continue;
                if (TicksIntersectInclusive(c.BottomTick, c.TopTick, mz.BottomTick, mz.TopTick))
                {
                    suppressed = true;
                    break;
                }
            }
            if (suppressed) continue;

            bool merged = false;
            for (size_t li = 0; li < liveIdx.size(); li++)
            {
                ZoneRec& ez = zones[liveIdx[li]];
                if (!ez.Valid) continue;
                if (ez.Source != ZSRC_AUTO) continue;
                if (ez.IsBuyZone != isBuySide) continue;
                if (!TicksOverlapInclusive(c.BottomTick, c.TopTick, ez.BottomTick, ez.TopTick))
                    continue;

                const int newBot = min(ez.BottomTick, c.BottomTick);
                const int newTop = max(ez.TopTick,    c.TopTick);
                if ((newTop - newBot) > maxZoneWidthTicks)
                    continue;   // this zone cannot absorb it; try the next one

                TtdDirtyMark(&sgDirty, ez.DetectedBar);  // pre-merge cell
                ez.BottomTick = newBot;
                ez.TopTick    = newTop;
                if (winEnd < ez.DetectedBar) ez.DetectedBar = winEnd;
                TtdDirtyMark(&sgDirty, ez.DetectedBar);  // post-merge cell
                {
                    const float mag = max((float)fabs(ez.TotalDelta), c.TotalMagnitude);
                    ez.TotalDelta = isBuySide ? -mag : mag;
                }
                if (c.Count > ez.BlockCount) ez.BlockCount = c.Count;   // as B6
                merged = true;

                // Widening an auto zone can push it into a manual zone.
                // Manual precedence still wins: kill the auto zone.
                if (thresholdMode == TMODE_BOTH)
                {
                    for (size_t lj = 0; lj < liveIdx.size(); lj++)
                    {
                        const ZoneRec& mz = zones[liveIdx[lj]];
                        if (!mz.Valid || mz.Source != ZSRC_MANUAL) continue;
                        if (!TicksIntersectInclusive(ez.BottomTick, ez.TopTick,
                                                     mz.BottomTick, mz.TopTick))
                            continue;
                        DeleteZoneDrawings(sc, ez);
                        ez.Valid     = false;
                        ez.Pending   = false;
                        ez.ErasedBar = -1;
                        if (ez.Slot >= 0)  // recycle promptly under batches
                        {
                            FreeSlot(slotsA, ez.Slot);
                            ez.Slot = -1;
                        }
                        TtdDirtyMark(&sgDirty, ez.DetectedBar);
                        break;
                    }
                }
                // [v4-M3] Still alive: the widened zone may now overlap other
                // auto zones.
                if (ez.Valid)
                    AbsorbOverlappingZones(sc, zones, liveIdx, liveIdx[li],
                                           maxZoneWidthTicks, slotsA, &sgDirty);
                break;
            }

            if (!merged)
            {
                ZoneRec z;
                z.ID          = sc.GetPersistentInt(P_IDCTR);
                sc.SetPersistentInt(P_IDCTR, z.ID + 1);
                z.Slot        = AllocSlotEvicting(sc, zones, slotsA, ZSRC_AUTO,
                                                  retainForExport);
                z.TopTick     = c.TopTick;
                z.BottomTick  = c.BottomTick;
                z.DetectedBar = winEnd;
                z.ErasedBar   = -1;
                z.IsBuyZone   = isBuySide;
                z.TotalDelta  = isBuySide ? -c.TotalMagnitude : c.TotalMagnitude;
                z.Valid       = true;
                z.Pending     = true;
                z.Drawn       = false;
                z.Source      = ZSRC_AUTO;
                z.NB_Outcome  = CONFOUT_PENDING;
                z.NB_Bar      = -1;
                z.DP_Outcome  = CONFOUT_PENDING;
                z.DP_Bar      = -1;
                z.BlockCount  = c.Count;
                z.CheckedTo   = winEnd;   // [v3-P3] nothing after detection yet
                z.AbsorbedInto = -1;      // [v4-M3]
                zones.push_back(z);
                liveIdx.push_back((int)zones.size() - 1);
                shadowIdx.push_back((int)zones.size() - 1);
                TtdDirtyMark(&sgDirty, z.DetectedBar);  // new cell when confirmed
            }
        }

        // Decision watermark: this winEnd's lifecycle ran and its original
        // candidates were fed — or the layer halted and feeds stopped
        // (review-fix-3: the frontier still advances past the halt so PASS
        // 3D publishes SG19 bit 4 at avail=j+1; lifecycle/candidates stay
        // stopped while halted, only the frontier moves).
        if (TtdHaltAdvanceMark(decDoBar ? 1 : 0, decHalted ? 1 : 0))
            decMark = winEnd;

        // [v3-P2] Drop what this window killed or decided. Both compactions are
        // stable, so the ascending-index iteration order the merge loops rely on
        // survives, and both are O(current list) rather than O(zones-ever).
        {
            size_t keep = 0;
            for (size_t li = 0; li < liveIdx.size(); li++)
            {
                if (zones[liveIdx[li]].Valid)
                    liveIdx[keep++] = liveIdx[li];
            }
            liveIdx.resize(keep);

            keep = 0;
            for (size_t si = 0; si < shadowIdx.size(); si++)
            {
                const ZoneRec& z = zones[shadowIdx[si]];
                if (z.NB_Outcome == CONFOUT_PENDING ||
                    z.DP_Outcome == CONFOUT_PENDING)
                    shadowIdx[keep++] = shadowIdx[si];
            }
            shadowIdx.resize(keep);
        }
    }

    // Review-fix-3: transition-storage halt must be visible (BuildSpec §3).
    // The frontier above advanced past the halting bar so PASS 3D publishes
    // SG19 bit 4 at avail=j+1 and the status persists on the right edge
    // while halted. Exactly one Message Log line per run generation marks
    // the stop (P_DHALTLOG latch, reset on rebuild). The engine stays
    // halted — no further lifecycle/candidates — until a rebuild resets the
    // generation. Journal/snapshot need no schema change: the journal holds
    // every record written before the halt, and the snapshot header already
    // carries halted + complete=0.
    if (decActive && pDec != nullptr &&
        TtdHaltLogDue(pDec->halted ? 1 : 0, sc.GetPersistentInt(P_DHALTLOG)))
    {
        sc.SetPersistentInt(P_DHALTLOG, 1);
        SCString haltMsg;
        haltMsg.Format("Trapped Traders v4: decision transition storage exhausted "
                       "(chart %d instance %d run %d). Decision layer halted at bar %d; "
                       "new bars are not processed (SG19 bit 4). "
                       "A full recalculation/rebuild starts a new generation.",
                       sc.ChartNumber, sc.StudyGraphInstanceID,
                       sc.GetPersistentInt(P_DGEN), decMark);
        sc.AddMessageToLog(haltMsg, 1);
    }

    // =========================================================================
    // PASS 1.5 — Price-confirmation filter for pending zones
    // =========================================================================
    // A freshly detected zone is held Pending for one bar close.  On the bar
    // immediately after detection we check whether price OPENED through the
    // zone, which would mean participants were NOT trapped:
    //   Sell zone: next bar opens ABOVE TopPrice    -> discard
    //   Long zone: next bar opens BELOW BottomPrice -> discard
    // Discarded zones have Valid=false and ErasedBar=-1 and are never drawn.
    //
    // [v3-C3] This is the backstop for a pending zone whose confirmation bar
    // PASS 1's window scan did not reach; PASS 1 resolves the ordinary case
    // inline. It now goes through the SAME TtdLegacyNextBarStep helper, so the
    // same-bar close-through fix (verdict 3: confirmed by the open, then erased
    // by that same bar's close) applies at both confirmation sites. Previously
    // only PASS 1 consulted the close here, and this path could leave a zone
    // ACTIVE at a price the confirmation bar had already closed through.
    for (size_t i = 0; i < zones.size() && confirmMode == CONF_NEXTBAR; i++)
    {
        ZoneRec& z = zones[i];
        if (!z.Valid || !z.Pending) continue;

        const int confirmBar = z.DetectedBar + 1;
        if (confirmBar > lastBar) continue;   // confirmation bar not closed yet

        const float topPrice = (float)(z.TopTick    * sc.TickSize);
        const float botPrice = (float)(z.BottomTick * sc.TickSize);

        const int v = TtdLegacyNextBarStep(z.IsBuyZone ? 1 : 0, confirmBar,
                                           z.DetectedBar,
                                           sc.Open[confirmBar],
                                           sc.Close[confirmBar],
                                           topPrice, botPrice);
        z.Pending = false;
        if (v == 2)
        {
            z.Valid = false;
            // ErasedBar stays -1: rejected before confirmation, never drawn.
            if (z.Slot >= 0)
            {
                FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                z.Slot = -1;
            }
        }
        else if (v == 3)
        {
            z.Valid     = false;
            z.ErasedBar = confirmBar;   // confirmed, then closed through same bar
            if (!showErased) DeleteZoneDrawings(sc, z);
            if (z.Slot >= 0)
            {
                FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                z.Slot = -1;
            }
        }
        // v == 1: confirmed and alive — drawn from PASS 4 onward.
        if (z.CheckedTo < confirmBar) z.CheckedTo = confirmBar;
        TtdDirtyMark(&sgDirty, z.DetectedBar);
    }

    // -- R2: prune rejected/suppressed zones (Valid=false, ErasedBar=-1) ------
    // Keeps merge, suppression and draw loops O(live zones) and recycles slots.
    //
    // With CSV export on the RECORD is kept (drawings deleted, slot returned,
    // Slot = -1) so the export can report both confirmation hypotheses over the
    // whole detected population rather than over one rule's survivors.  A
    // retained record is inert: every detection, merge, suppression, subgraph
    // and drawing site tests Valid first, and Slot = -1 keeps it out of the
    // drawing pool.  See the retainForExport comment for the cost.
    {
        size_t w = 0;
        for (size_t i = 0; i < zones.size(); i++)
        {
            ZoneRec& z = zones[i];
            if (!z.Valid && z.ErasedBar < 0)
            {
                DeleteZoneDrawings(sc, z);
                if (z.Source == ZSRC_AUTO) FreeSlot(slotsA, z.Slot);
                else                       FreeSlot(slotsM, z.Slot);
                z.Slot = -1;   // the slot is back in the pool; never reuse it here
                if (!retainForExport) continue;
            }
            if (w != i) zones[w] = zones[i];
            w++;
        }
        zones.resize(w);
    }

    // =========================================================================
    // PASS 2 — Invalidate zones that a CLOSED bar has closed through
    // =========================================================================
    // Fix B1: v1 iterated to curBar = ArraySize-1, the FORMING bar, so a
    // transient intrabar excursion permanently erased a zone even when the bar
    // closed back inside.  Erasure now uses closed bars only.
    for (size_t i = 0; i < zones.size(); i++)
    {
        ZoneRec& z = zones[i];
        if (!z.Valid || z.Pending) continue;

        int checkStart = z.DetectedBar + 1;
        // [v3-P3] PASS 1's inline lifecycle already applied this exact
        // close-through test to every bar up to CheckedTo, so restarting at
        // DetectedBar+1 re-read the whole history for every zone on every full
        // recalculation. Start after what PASS 1 resolved; on a full recalc
        // that leaves nothing to do, and on an incremental call it leaves
        // exactly the bars PASS 1 could not see.
        if (z.CheckedTo >= checkStart)
            checkStart = z.CheckedTo + 1;
        if (!isFullRecalc && prevLastBar + 1 > checkStart)
            checkStart = prevLastBar + 1;
        if (checkStart < 0) checkStart = 0;

        const float topPrice = (float)(z.TopTick    * sc.TickSize);
        const float botPrice = (float)(z.BottomTick * sc.TickSize);

        for (int bi = checkStart; bi <= lastBar; bi++)
        {
            const float cl = sc.Close[bi];
            const bool clipped = z.IsBuyZone ? (cl < botPrice)    // support broken
                                             : (cl > topPrice);   // resistance broken
            if (clipped)
            {
                z.Valid     = false;
                z.ErasedBar = bi;
                if (!showErased)
                    DeleteZoneDrawings(sc, z);
                // Show Erased Zones mode: PASS 4 redraws it frozen at ErasedBar.
                if (!showErased && z.Slot >= 0)  // clipped records leave the pool
                {
                    FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                    z.Slot = -1;
                }
                TtdDirtyMark(&sgDirty, z.DetectedBar);
                break;
            }
        }
        // [v3-P3] Everything up to lastBar has now been tested against this
        // zone, whether it clipped or not.
        if (z.CheckedTo < lastBar) z.CheckedTo = lastBar;
    }

    // -- R2: in Clipped mode, drop erased zones once their drawings are gone --
    // Same retention rule as the rejected-zone prune above: with export on the
    // record survives with Slot = -1 so its ErasedBar and both shadow verdicts
    // reach the CSV.  PASS 4 discards it at the "!Valid && !showErased" guard,
    // which sits BEFORE the Slot test, so a retained record cannot trip the
    // slot-exhaustion warning.
    if (!showErased)
    {
        size_t w = 0;
        for (size_t i = 0; i < zones.size(); i++)
        {
            ZoneRec& z = zones[i];
            if (!z.Valid)
            {
                DeleteZoneDrawings(sc, z);
                if (z.Source == ZSRC_AUTO) FreeSlot(slotsA, z.Slot);
                else                       FreeSlot(slotsM, z.Slot);
                z.Slot = -1;
                if (!retainForExport) continue;
            }
            if (w != i) zones[w] = zones[i];
            w++;
        }
        zones.resize(w);
    }

    // -- [v4-M5] Keep Erased Zones: drop erased zones past their sessions -------
    // Same retention rule as R2: with export on the record stays (Slot = -1)
    // so it reaches the CSV, and PASS 4 skips it with the same expiry test.
    if (showErased && keepErased > 0)
    {
        size_t w = 0;
        for (size_t i = 0; i < zones.size(); i++)
        {
            ZoneRec& z = zones[i];
            if (!z.Valid && z.ErasedBar >= 0 &&
                ErasedBarExpired(days, z.ErasedBar, keepErased))
            {
                if (z.Slot >= 0)
                {
                    DeleteZoneDrawings(sc, z);
                    FreeSlot(z.Source == ZSRC_AUTO ? slotsA : slotsM, z.Slot);
                    z.Slot = -1;
                }
                if (!retainForExport) continue;
            }
            if (w != i) zones[w] = zones[i];
            w++;
        }
        zones.resize(w);
    }

    // =========================================================================
    // Current trading day's auto thresholds
    // =========================================================================
    // PASS 1 computes the thresholds of every day a scanned window ends in.
    // When the forming bar has just opened a NEW trading day, that new segment
    // holds no closed bar yet and PASS 1 never touched it — compute it here so
    // SG3/SG4/SG5 and the status label are correct on the right edge from the
    // first bar of the day.
    const int curDayIdx = days.empty() ? -1 : (int)days.size() - 1;
    if (wantAuto && curDayIdx >= 0)
    {
        EnsureDayThresholds(sc, days, curDayIdx, autoDays, lookback, blocksMin,
                            proxTol, autoBlockPct, autoZonePct,
                            autoSession, rthStartSec, rthEndSec,
                            scratch, blockBuf, zoneBuf);
    }

    // =========================================================================
    // PASS 3 — Subgraph output
    // =========================================================================
    // Fix B5: v1 only zeroed SG[curBar], so a zone that was later invalidated
    // left a ghost delta at its old DetectedBar forever.
    // Full rebuild: all six subgraphs are cleared across the whole array and
    // rewritten from live state. Ordinary (incremental) callbacks only touch
    // dirty detection cells and the day range below (Task 8) — never a
    // whole-history clear. Both run on bar close / full recalc, never on an
    // intrabar tick.
    if (isFullRecalc)
    {
    for (int bi = 0; bi < sc.ArraySize; bi++)
    {
        SG_SellDelta[bi]  = 0.0f;
        SG_BuyDelta[bi]   = 0.0f;
        SG_AutoBlock[bi]  = 0.0f;
        SG_AutoZone[bi]   = 0.0f;
        SG_AutoReady[bi]  = 0.0f;
        SG_ZoneSource[bi] = 0.0f;
    }

    // Per-bar auto threshold state, written from the cached day segments.
    if (wantAuto)
    {
        for (size_t k = 0; k < days.size(); k++)
        {
            const DaySeg& d = days[k];
            if (!d.ThreshComputed || !d.ThreshReady) continue;
            const int hi = min(d.LastBar, sc.ArraySize - 1);
            for (int bi = d.FirstBar; bi <= hi; bi++)
            {
                SG_AutoBlock[bi] = d.AutoMinBlock;
                SG_AutoZone[bi]  = d.AutoZoneTotal;
                SG_AutoReady[bi] = 1.0f;
            }
        }
    }

    // Zone deltas and source pulse at each confirmed zone's DetectedBar.
    // Auto first, manual second, so manual wins when both share an index —
    // consistent with manual precedence everywhere else.
    for (int phase = 0; phase < 2; phase++)
    {
        const int wantSource = (phase == 0) ? ZSRC_AUTO : ZSRC_MANUAL;
        for (size_t i = 0; i < zones.size(); i++)
        {
            const ZoneRec& z = zones[i];
            if (!z.Valid || z.Pending) continue;
            if (z.Source != wantSource) continue;
            if (z.DetectedBar < 0 || z.DetectedBar >= sc.ArraySize) continue;

            if (!z.IsBuyZone) SG_SellDelta[z.DetectedBar] = z.TotalDelta;
            else              SG_BuyDelta[z.DetectedBar]  = z.TotalDelta;

            SG_ZoneSource[z.DetectedBar] =
                (z.Source == ZSRC_AUTO) ? 2.0f : 1.0f;
        }
    }
    }
    else
    {
        // Sparse incremental rewrite (Task 8). Detection cells whose zone
        // content changed are zeroed and re-resolved with the SAME
        // predicates and auto-then-manual order as the full path above
        // (keep the two in sync); removals resolve to zeros, so no ghost
        // deltas survive. Overflow falls back to the marked span only —
        // never a whole-history clear on an ordinary callback.
        if (sgDirty.overflow)
        {
            int lo = sgDirty.lo;
            int hi = sgDirty.hi;
            if (lo < 0) lo = 0;
            if (hi >= sc.ArraySize) hi = sc.ArraySize - 1;
            for (int bi = lo; bi <= hi && bi < sc.ArraySize; bi++)
            {
                SG_SellDelta[bi]  = 0.0f;
                SG_BuyDelta[bi]   = 0.0f;
                SG_ZoneSource[bi] = 0.0f;
            }
            for (int phase = 0; phase < 2; phase++)
            {
                const int wantSource = (phase == 0) ? ZSRC_AUTO : ZSRC_MANUAL;
                for (size_t i = 0; i < zones.size(); i++)
                {
                    const ZoneRec& z = zones[i];
                    if (!z.Valid || z.Pending) continue;
                    if (z.Source != wantSource) continue;
                    if (z.DetectedBar < 0 || z.DetectedBar >= sc.ArraySize) continue;

                    if (!z.IsBuyZone) SG_SellDelta[z.DetectedBar] = z.TotalDelta;
                    else              SG_BuyDelta[z.DetectedBar]  = z.TotalDelta;

                    SG_ZoneSource[z.DetectedBar] =
                        (z.Source == ZSRC_AUTO) ? 2.0f : 1.0f;
                }
            }
        }
        else
        {
            for (int di = 0; di < sgDirty.count; di++)
            {
                const int cell = sgDirty.idx[di];
                if (cell < 0 || cell >= sc.ArraySize) continue;
                SG_SellDelta[cell]  = 0.0f;
                SG_BuyDelta[cell]   = 0.0f;
                SG_ZoneSource[cell] = 0.0f;
                for (int phase = 0; phase < 2; phase++)
                {
                    const int wantSource = (phase == 0) ? ZSRC_AUTO : ZSRC_MANUAL;
                    for (size_t i = 0; i < zones.size(); i++)
                    {
                        const ZoneRec& z = zones[i];
                        if (!z.Valid || z.Pending) continue;
                        if (z.Source != wantSource) continue;
                        if (z.DetectedBar != cell) continue;

                        if (!z.IsBuyZone) SG_SellDelta[cell] = z.TotalDelta;
                        else              SG_BuyDelta[cell]  = z.TotalDelta;

                        SG_ZoneSource[cell] =
                            (z.Source == ZSRC_AUTO) ? 2.0f : 1.0f;
                    }
                }
            }
        }

        // Day cells: newly scanned bars always need their day's values;
        // segments whose threshold cache completed this call (or were
        // appended) need their whole range. Compare against the pre-PASS 1
        // snapshot; caches are deterministic, so unchanged tuples mean
        // unchanged values for those bars.
        int dayRewriteFrom = winEndStart;
        const size_t commonDays = min(daySnap.size(), days.size());
        for (size_t k = 0; k < commonDays; k++)
        {
            const DaySeg& d = days[k];
            const DayCacheSnap& s = daySnap[k];
            const int dComputed = d.ThreshComputed ? 1 : 0;
            const int dReady = d.ThreshReady ? 1 : 0;
            if (dComputed != s.Computed || dReady != s.Ready ||
                d.AutoMinBlock != s.Block || d.AutoZoneTotal != s.Zone)
            {
                if (d.FirstBar < dayRewriteFrom) dayRewriteFrom = d.FirstBar;
            }
        }
        for (size_t k = commonDays; k < days.size(); k++)
        {
            if (days[k].FirstBar < dayRewriteFrom)
                dayRewriteFrom = days[k].FirstBar;
        }
        if (dayRewriteFrom < 0) dayRewriteFrom = 0;
        if (wantAuto)
        {
            for (size_t k = 0; k < days.size(); k++)
            {
                const DaySeg& d = days[k];
                if (!d.ThreshComputed || !d.ThreshReady) continue;
                const int lo = max(d.FirstBar, dayRewriteFrom);
                const int hi = min(d.LastBar, sc.ArraySize - 1);
                for (int bi = lo; bi <= hi; bi++)
                {
                    SG_AutoBlock[bi] = d.AutoMinBlock;
                    SG_AutoZone[bi]  = d.AutoZoneTotal;
                    SG_AutoReady[bi] = 1.0f;
                }
            }
        }

        // Old-cell changes report the earliest touched index so downstream
        // studies can limit their own updates. The dirty span covers every
        // mark including post-overflow ones, so the bound stays accurate.
        int earliestSG = dayRewriteFrom;
        const int dirtyMin = TtdDirtyMin(&sgDirty);
        if (dirtyMin >= 0 && dirtyMin < earliestSG) earliestSG = dirtyMin;
        if (sgDirty.lo >= 0 && sgDirty.lo < earliestSG) earliestSG = sgDirty.lo;
        if (earliestSG < 0) earliestSG = 0;
        if (earliestSG < sc.ArraySize)
            sc.EarliestUpdateSubgraphDataArrayIndex = earliestSG;
    }

    // =========================================================================
    // PASS 3D — Decision Map SG7..SG19 (v2.1, Task 6)
    // =========================================================================
    // Transitions based on closed bar j are first knowable at availability
    // index j+1 (spec §6) and are written there — never backdated to the
    // detection bar and never using the forming bar's prices. Counts are
    // per-availability pulses split by side (the core never nets opposite
    // events); SG15..SG19 are frozen per-bar snapshots. Incremental calls
    // touch only newly available bars, so earlier outputs are never
    // rewritten; rebuilds (and the inactive state) clear the full range.
    // Legacy SG1..SG6 above are untouched.
    if (decActive && pDec != nullptr)
    {
        if (decRebuilt)
        {
            for (int bi = 0; bi < sc.ArraySize; bi++)
            {
                SG_DConfBuy[bi] = 0.0f;   SG_DConfSell[bi] = 0.0f;
                SG_DRet1Buy[bi] = 0.0f;   SG_DRet1Sell[bi] = 0.0f;
                SG_DRejBuy[bi] = 0.0f;    SG_DRejSell[bi] = 0.0f;
                SG_DCrossBuy[bi] = 0.0f;  SG_DCrossSell[bi] = 0.0f;
                SG_DSelId[bi] = 0.0f;     SG_DSelState[bi] = 0.0f;
                SG_DSelBot[bi] = 0.0f;    SG_DSelTop[bi] = 0.0f;
                SG_DStatus[bi] = 0.0f;
            }
        }

        int decIncomplete = 0;
        for (int ai = 0; ai < pDec->numActive; ai++)
        {
            const TtdEpisode& de = pDec->episodes[pDec->activeIdx[ai]];
            if (de.openReturn && de.meas.incomplete)
            {
                decIncomplete = 1;
                break;
            }
        }
        const int decStatus = TtdDataStatus(
            0, decIncomplete,
            (pDec->capacityLimited || pDec->halted) ? 1 : 0,
            sc.GetPersistentInt(P_DEXPFAIL) ? 1 : 0, 0);

        // PASS 3D window is the portable-tested TtdAvailWindowFor
        // (review-fix-3): incremental calls cover newly processed bars only,
        // so frozen per-bar snapshots behind the frontier are never
        // rewritten; the halted frontier keeps advancing so the halt status
        // persists on the right edge. decMark = last closed bar done.
        const TtdAvailWindow decWin =
            TtdAvailWindowFor(decMarkBefore, decMark, decRebuilt ? 1 : 0);
        const int decFirstAvail = decWin.firstAvail;
        const int decLastAvail = decWin.lastAvail;

        // [v3-P1] One forward sweep of the ledger instead of TtdSnapshotAt per
        // availability bar. The old shape cost O(episodes x transitions) PER
        // BAR, so a rebuilt chart paid O(bars x episodes x transitions) here —
        // on real history that is not slow, it is a hang. The sweep produces
        // the same counts, selected episode, state and geometry (see the
        // TtdSweep* block in the core for why the two agree) in one pass.
        //
        // The sweep always starts at availability 1 so its cursor and eligible
        // set are exact, but SUBGRAPH WRITES still happen only inside the
        // window: frozen snapshots behind the frontier are never rewritten, and
        // an incremental call publishes the one or two newly available bars it
        // always did. Walking the ledger from the start is O(transitions), not
        // O(bars x transitions), so that costs nothing.
        const int decEpN = (pDec->numEpisodes > 0) ? pDec->numEpisodes : 1;
        vector<int> swState(decEpN, 0);
        vector<int> swOrdinal(decEpN, 0);
        vector<int> swActive(decEpN, 0);
        TtdSweep sw;
        sw.cap = decEpN;
        sw.cursor = 0;
        sw.nActive = 0;
        sw.state = swState.data();
        sw.ordinal = swOrdinal.data();
        sw.active = swActive.data();

        if (TtdSweepInit(pDec, &sw) == TTD_SWEEP_OK)
        {
            const int decSweepEnd = min(decLastAvail, sc.ArraySize - 1);
            for (int avail = 1; avail <= decSweepEnd; avail++)
            {
                TtdAvailCounts counts;
                TtdSweepAdvance(pDec, &sw, avail, &counts);
                if (avail < decFirstAvail)
                    continue;   // frozen behind the frontier: consume, do not write

                const int decCloseTick =
                    TtdPriceToTicks((double)sc.Close[avail - 1], sc.TickSize);
                const int selSlot = TtdSweepSelect(pDec, &sw, decCloseTick);

                SG_DConfBuy[avail] = (float)counts.confBuy;
                SG_DConfSell[avail] = (float)counts.confSell;
                SG_DRet1Buy[avail] = (float)counts.ret1Buy;
                SG_DRet1Sell[avail] = (float)counts.ret1Sell;
                SG_DRejBuy[avail] = (float)counts.rejBuy;
                SG_DRejSell[avail] = (float)counts.rejSell;
                SG_DCrossBuy[avail] = (float)counts.crossBuy;
                SG_DCrossSell[avail] = (float)counts.crossSell;
                if (selSlot >= 0)
                {
                    const TtdEpisode& se = pDec->episodes[selSlot];
                    SG_DSelId[avail] = (float)se.id;
                    SG_DSelState[avail] = (float)sw.state[selSlot];
                    SG_DSelBot[avail] = (float)(se.bottomTick * sc.TickSize);
                    SG_DSelTop[avail] = (float)(se.topTick * sc.TickSize);
                }
                else
                {
                    SG_DSelId[avail] = 0.0f;
                    SG_DSelState[avail] = 0.0f;
                    SG_DSelBot[avail] = 0.0f;
                    SG_DSelTop[avail] = 0.0f;
                }
                SG_DStatus[avail] = (float)decStatus;
            }

            // [v3-P1] Reference cross-check, once per callback.
            //
            // TtdSnapshotAt is the specification this sweep replaces, so run it
            // for the LAST published availability and compare. One call is the
            // same order of cost as TtdDrawPlanAt, which already runs once per
            // callback below, and it makes "the sweep agrees with the
            // reference" something the study checks rather than something the
            // comment above asserts. A disagreement raises the internal-fault
            // bit on that bar (SG19 bit 16) and logs one line per generation;
            // it never silently publishes a value the reference disowns.
            const int vfyAvail = decSweepEnd;
            if (vfyAvail >= decFirstAvail && vfyAvail >= 1)
            {
                const int vfyCloseTick =
                    TtdPriceToTicks((double)sc.Close[vfyAvail - 1], sc.TickSize);
                TtdBarSnapshot ref;
                TtdSnapshotAt(pDec, vfyAvail, vfyCloseTick, decMaxNearby,
                              decStatus, &ref);
                const bool agree =
                    (int)SG_DSelId[vfyAvail]     == ref.selId &&
                    (int)SG_DSelState[vfyAvail]  == ref.selState &&
                    (int)SG_DConfBuy[vfyAvail]   == ref.counts.confBuy &&
                    (int)SG_DConfSell[vfyAvail]  == ref.counts.confSell &&
                    (int)SG_DRet1Buy[vfyAvail]   == ref.counts.ret1Buy &&
                    (int)SG_DRet1Sell[vfyAvail]  == ref.counts.ret1Sell &&
                    (int)SG_DRejBuy[vfyAvail]    == ref.counts.rejBuy &&
                    (int)SG_DRejSell[vfyAvail]   == ref.counts.rejSell &&
                    (int)SG_DCrossBuy[vfyAvail]  == ref.counts.crossBuy &&
                    (int)SG_DCrossSell[vfyAvail] == ref.counts.crossSell;
                if (!agree)
                {
                    SG_DStatus[vfyAvail] = (float)(decStatus | 16);
                    if (sc.GetPersistentInt(P_DVFYLOG) == 0)
                    {
                        sc.SetPersistentInt(P_DVFYLOG, 1);
                        SCString vfyMsg;
                        vfyMsg.Format("Trapped Traders v4: decision availability sweep "
                                      "disagreed with the reference snapshot at bar %d "
                                      "(chart %d instance %d run %d). SG19 bit 16 is set "
                                      "on that bar; please report the chart and settings.",
                                      vfyAvail, sc.ChartNumber, sc.StudyGraphInstanceID,
                                      sc.GetPersistentInt(P_DGEN));
                        sc.AddMessageToLog(vfyMsg, 1);
                    }
                }
            }
        }

        // [v3-P4] Active again: the inactive pattern is no longer on the array,
        // so the next inactive pass must clear from scratch.
        sc.SetPersistentInt(P_DCLRN, 0);
        sc.SetPersistentInt(P_DCLRS, 0);
    }
    else
    {
        // Inactive (off, Displacement missing, or engine allocation failure):
        // no decision events exist. Clear the full range so a previous
        // generation's values cannot linger; status bit 1 marks inactive
        // (bit 16 instead when the engine itself failed to allocate).
        //
        // [v3-P4] Latched. This branch is the DEFAULT configuration (Decision
        // Map off), and it used to rewrite thirteen subgraphs across the whole
        // array on every single bar close — work proportional to chart length,
        // repeated forever, to write the same constants back. P_DCLRN records
        // how much of the array already carries the inactive pattern and
        // P_DCLRS which status it carries, so an ordinary bar close writes only
        // the bars Sierra appended. A status change, a shrink, or an explicit
        // rebuild still rewrites the whole range.
        const float inactiveStatus =
            (decActive && pDec == nullptr) ? 16.0f : 1.0f;
        const int clearedTo = sc.GetPersistentInt(P_DCLRN);
        const int clearedStatus = sc.GetPersistentInt(P_DCLRS);
        const int wantStatusKey = (int)inactiveStatus + 1;

        int clearFrom = 0;
        if (!isFullRecalc && clearedStatus == wantStatusKey &&
            clearedTo > 0 && clearedTo <= sc.ArraySize)
            clearFrom = clearedTo;

        for (int bi = clearFrom; bi < sc.ArraySize; bi++)
        {
            SG_DConfBuy[bi] = 0.0f;   SG_DConfSell[bi] = 0.0f;
            SG_DRet1Buy[bi] = 0.0f;   SG_DRet1Sell[bi] = 0.0f;
            SG_DRejBuy[bi] = 0.0f;    SG_DRejSell[bi] = 0.0f;
            SG_DCrossBuy[bi] = 0.0f;  SG_DCrossSell[bi] = 0.0f;
            SG_DSelId[bi] = 0.0f;     SG_DSelState[bi] = 0.0f;
            SG_DSelBot[bi] = 0.0f;    SG_DSelTop[bi] = 0.0f;
            SG_DStatus[bi] = inactiveStatus;
        }
        sc.SetPersistentInt(P_DCLRN, sc.ArraySize);
        sc.SetPersistentInt(P_DCLRS, wantStatusKey);
    }

    // =========================================================================
    // PASS 3E — Decision Map compact renderer (v2.1, Phase 3 Task 9)
    // =========================================================================
    // Thin translucent frozen-boundary bands, concise state labels, and
    // confirmation point / first-return diamond / crossed X markers, all at
    // availability indices. Only selected (TtdDrawPlanAt) and dirty
    // (TtdOwnedNeedsDraw) objects touch the chart. Drawings are ordinary
    // non-user objects (AddAsUserDrawnDrawing=0): Sierra allocates each
    // LineNumber once (UseTool > 0, checked), the adapter retains exactly its
    // own IDs and deletes single objects only — never TOOL_DELETE_ALL, never
    // another instance's IDs. Full recalculation auto-deletes drawings, so a
    // rebuild invalidates owned IDs and recreates visible objects.
    // In decision mode legacy zone drawings are hidden (records preserved in
    // the zone list); turning decision mode off restores PASS 4 rendering
    // from those same canonical records.
    const int decRendering = (decActive && pDec != nullptr) ? 1 : 0;
    {
        TtdDrawOwnerState* pOwn = TtdGetDrawOwner(sc);
        const int wasRendering = sc.GetPersistentInt(P_DMODE);
        if (decRendering)
        {
            if (pOwn == nullptr)
            {
                pOwn = new (std::nothrow) TtdDrawOwnerState();
                if (pOwn != nullptr)
                {
                    pOwn->count = 0;
                    pOwn->panelLineId = 0;
                    pOwn->panelDrawn = 0;
                    pOwn->panelText[0] = '\0';
                    sc.SetPersistentPointer(P_DDRAW, pOwn);
                }
            }
            if (pOwn != nullptr && decRebuilt)
            {
                // [v3-C1] Delete, do not merely forget. The old code assumed a
                // decision rebuild is always a Sierra full recalculation (which
                // auto-removes a study's non-user drawings), but the decision
                // fingerprint has an input the legacy one does not — In:32 —
                // so toggling the Decision Map rebuilds the generation without
                // Sierra recalculating. Dropping the IDs there left band, label
                // and marker objects on the chart that nothing owned and
                // nothing could remove. These are single-object deletes of
                // exactly this instance's IDs; on a genuine full recalculation
                // they are cheap no-ops on already-removed objects.
                TtdDeleteOwnedDrawings(sc, pOwn);
            }
            if (!wasRendering)
            {
                // Entering decision mode: hide legacy drawings, keep records.
                int sweepTo = sc.GetPersistentInt(P_MAXSLOT);
                if (sweepTo <= 0) sweepTo = MAX_ZONE_SLOTS;
                DeleteAllStudyDrawings(sc, sweepTo);
            }
            sc.SetPersistentInt(P_DMODE, 1);

            if (pOwn != nullptr && decMark >= 0 && sc.ArraySize > 0)
            {
                const int availBar = decMark + 1;
                const int lastCloseTick =
                    TtdPriceToTicks((double)sc.Close[decMark], sc.TickSize);
                TtdDrawPlan plan;
                TtdDrawPlanAt(pDec, availBar, lastCloseTick, decMaxNearby,
                              showErased, &plan);

                for (int pi = 0; pi < plan.n; pi++)
                {
                    const TtdDrawIntent& w = plan.intents[pi];
                    if (!w.visible)
                    {
                        // Muted/deselected: release exactly this episode's
                        // owned objects, one single-object delete each.
                        for (int si = pOwn->count - 1; si >= 0; si--)
                        {
                            if (pOwn->slots[si].episodeId != w.episodeId)
                                continue;
                            int gone = 0;
                            const int sIdx = si;
                            if (TtdOwnedRelease(pOwn->slots, &pOwn->count,
                                                sIdx, &gone) == TTD_OK)
                                DeleteDrawing(sc, gone);
                        }
                        continue;
                    }

                    const int sBar = max(0, min(w.bandStartAvail, sc.ArraySize - 1));
                    const int eBar = max(0, min(w.bandEndAvail, sc.ArraySize - 1));
                    const double botP = (double)w.bottomTick * sc.TickSize;
                    const double topP = (double)w.topTick * sc.TickSize;
                    const COLORREF fill   = (w.dir == 1) ? buyFill : sellFill;
                    const COLORREF border = (w.dir == 1) ? buyBord : sellBord;
                    const int crossed  = (w.hasCrossX != 0) ? 1 : 0;
                    const int dimTransp = min(transparency + 30, 95);

                    // Wanted objects for this selected episode: band + label
                    // + point at confirmation availability + diamond at first
                    // return + X at crossing. Marker avails never move.
                    struct Want { int kind; int aBar; int bBar; double price; int marker; };
                    Want wants[5];
                    int nWant = 0;
                    wants[nWant++] = { TTD_DRAW_BAND, sBar, eBar, 0.0, 0 };
                    wants[nWant++] = { TTD_DRAW_LABEL, sBar, eBar, 0.0, 0 };
                    if (w.hasConfirmPoint)
                        wants[nWant++] = { TTD_DRAW_CONFIRM_POINT,
                                           max(0, min(w.confirmAvail, sc.ArraySize - 1)),
                                           0, (w.dir == 1) ? topP : botP, MARKER_POINT };
                    if (w.hasFirstDiamond)
                        wants[nWant++] = { TTD_DRAW_FIRST_DIAMOND,
                                           max(0, min(w.firstAvail, sc.ArraySize - 1)),
                                           0, (botP + topP) * 0.5, MARKER_DIAMOND };
                    if (w.hasCrossX)
                        wants[nWant++] = { TTD_DRAW_CROSS_X,
                                           max(0, min(w.crossAvail, sc.ArraySize - 1)),
                                           0, (w.dir == 1) ? botP : topP, MARKER_X };

                    for (int qi = 0; qi < nWant; qi++)
                    {
                        const Want& q = wants[qi];
                        // Minimal want-intent for the dirty check: bands and
                        // labels compare the band anchors; markers compare
                        // their own fixed availability.
                        TtdDrawIntent qw;
                        qw.episodeId = w.episodeId;
                        qw.visible = 1;
                        qw.bandStartAvail = q.aBar;
                        qw.bandEndAvail = (q.kind == TTD_DRAW_BAND ||
                                           q.kind == TTD_DRAW_LABEL) ? q.bBar : q.aBar;
                        qw.bottomTick = w.bottomTick;
                        qw.topTick = w.topTick;
                        qw.dir = w.dir;
                        qw.state = w.state;
                        qw.returnOrdinal = w.returnOrdinal;
                        qw.confirmAvail = -1;
                        qw.hasConfirmPoint = 0;
                        qw.firstAvail = -1;
                        qw.hasFirstDiamond = 0;
                        qw.crossAvail = -1;
                        qw.hasCrossX = 0;
                        if (q.kind == TTD_DRAW_CONFIRM_POINT)
                        { qw.confirmAvail = q.aBar; qw.hasConfirmPoint = 1; }
                        if (q.kind == TTD_DRAW_FIRST_DIAMOND)
                        { qw.firstAvail = q.aBar; qw.hasFirstDiamond = 1; }
                        if (q.kind == TTD_DRAW_CROSS_X)
                        { qw.crossAvail = q.aBar; qw.hasCrossX = 1; }

                        const int found =
                            TtdOwnedFind(pOwn->slots, pOwn->count,
                                         w.episodeId, q.kind);
                        if (found >= 0)
                        {
                            TtdOwnedDraw* s = &pOwn->slots[found];
                            if (!TtdOwnedNeedsDraw(s, &qw, availBar))
                                continue; // settled: no redraw
                            s_UseTool T;
                            T.Clear();
                            T.ChartNumber = sc.ChartNumber;
                            T.AddMethod = UTAM_ADD_OR_ADJUST;
                            T.LineNumber = s->lineId;
                            T.AddAsUserDrawnDrawing = 0;
                            if (q.kind == TTD_DRAW_BAND)
                            {
                                T.DrawingType = DRAWING_RECTANGLEHIGHLIGHT;
                                T.BeginDateTime = sc.BaseDateTimeIn[q.aBar];
                                T.EndDateTime = sc.BaseDateTimeIn[q.bBar];
                                T.BeginValue = min(botP, topP);
                                T.EndValue = max(botP, topP);
                                T.Color = border;
                                T.SecondaryColor = fill;
                                T.LineWidth = 1; // thin compact band
                                T.TransparencyLevel =
                                    crossed ? dimTransp : transparency;
                            }
                            else if (q.kind == TTD_DRAW_LABEL)
                            {
                                char lbl[64];
                                TtdDecisionLabel(w, lbl, (int)sizeof(lbl));
                                T.DrawingType = DRAWING_TEXT;
                                T.BeginDateTime = sc.BaseDateTimeIn[q.aBar];
                                T.BeginValue = (w.dir == 1) ? botP : topP;
                                T.Text = lbl;
                                T.Color = COLOR_WHITE;
                                T.FontSize = 8;
                                T.FontBold = 1;
                                T.TransparentLabelBackground = 1;
                            }
                            else
                            {
                                T.DrawingType = DRAWING_MARKER;
                                T.BeginDateTime = sc.BaseDateTimeIn[q.aBar];
                                T.BeginValue = q.price;
                                T.MarkerType = q.marker;
                                T.MarkerSize = 4;
                                T.Color = border;
                            }
                            if (sc.UseTool(T) > 0)
                            {
                                s->lastStart = qw.bandStartAvail;
                                s->lastEnd = qw.bandEndAvail;
                                s->drawn = 1;
                            }
                            continue;
                        }

                        // New object: let Sierra allocate the ID once.
                        s_UseTool T;
                        T.Clear();
                        T.ChartNumber = sc.ChartNumber;
                        T.AddMethod = UTAM_ADD_OR_ADJUST;
                        T.AddAsUserDrawnDrawing = 0;
                        if (q.kind == TTD_DRAW_BAND)
                        {
                            T.DrawingType = DRAWING_RECTANGLEHIGHLIGHT;
                            T.BeginDateTime = sc.BaseDateTimeIn[q.aBar];
                            T.EndDateTime = sc.BaseDateTimeIn[q.bBar];
                            T.BeginValue = min(botP, topP);
                            T.EndValue = max(botP, topP);
                            T.Color = border;
                            T.SecondaryColor = fill;
                            T.LineWidth = 1;
                            T.TransparencyLevel =
                                crossed ? dimTransp : transparency;
                        }
                        else if (q.kind == TTD_DRAW_LABEL)
                        {
                            char lbl[64];
                            TtdDecisionLabel(w, lbl, (int)sizeof(lbl));
                            T.DrawingType = DRAWING_TEXT;
                            T.BeginDateTime = sc.BaseDateTimeIn[q.aBar];
                            T.BeginValue = (w.dir == 1) ? botP : topP;
                            T.Text = lbl;
                            T.Color = COLOR_WHITE;
                            T.FontSize = 8;
                            T.FontBold = 1;
                            T.TransparentLabelBackground = 1;
                        }
                        else
                        {
                            T.DrawingType = DRAWING_MARKER;
                            T.BeginDateTime = sc.BaseDateTimeIn[q.aBar];
                            T.BeginValue = q.price;
                            T.MarkerType = q.marker;
                            T.MarkerSize = 4;
                            T.Color = border;
                        }
                        // Checked outcome: only a positive UseTool result
                        // with a positive allocated ID takes ownership;
                        // anything else retries on a later call.
                        if (sc.UseTool(T) > 0 && T.LineNumber > 0)
                        {
                            if (TtdOwnedClaim(pOwn->slots, TTD_DRAW_OWNER_MAX,
                                              &pOwn->count, w.episodeId,
                                              q.kind, T.LineNumber) == TTD_OK)
                            {
                                TtdOwnedDraw* s = &pOwn->slots[pOwn->count - 1];
                                s->lastStart = qw.bandStartAvail;
                                s->lastEnd = qw.bandEndAvail;
                                s->drawn = 1;
                            }
                            else
                            {
                                // Claim refused (duplicate/full): drop the
                                // orphaned drawing so no untracked object
                                // lingers under this instance.
                                DeleteDrawing(sc, T.LineNumber);
                            }
                        }
                    }
                }

                // Sweep owned objects whose episode left the selected plan
                // (deselected by cap/distance or retired): exactly their IDs.
                for (int si = pOwn->count - 1; si >= 0; si--)
                {
                    const int epId = pOwn->slots[si].episodeId;
                    int kept = 0;
                    for (int pi = 0; pi < plan.n && !kept; pi++)
                    {
                        if (plan.intents[pi].episodeId == epId &&
                            plan.intents[pi].visible)
                            kept = 1;
                    }
                    if (!kept)
                    {
                        int gone = 0;
                        if (TtdOwnedRelease(pOwn->slots, &pOwn->count,
                                            si, &gone) == TTD_OK)
                            DeleteDrawing(sc, gone);
                    }
                }

                // Focus detail panel (v2.1, Phase 3 Task 10): text for the
                // FIRST selected episode only. Original delta is shown
                // separately from the fresh return bid/ask, penetration, the
                // later-close move-away and the explicitly bar-based span;
                // ordinal 0 shows "No return yet" and missing VAP warns
                // honestly. Show Decision Detail Panel gates the whole panel;
                // Show Delta Info gates only the original-delta line. Both
                // are display-only and never touch the semantic ledger.
                // Anchored at the band end bar so it never overlaps the
                // start-anchored state label. Dirty-only: settled text is
                // never redrawn each bar.
                const int wantPanel =
                    (decDetail && plan.n > 0 && plan.intents[0].visible) ? 1 : 0;
                if (!wantPanel)
                {
                    if (pOwn->panelDrawn && pOwn->panelLineId > 0)
                        DeleteDrawing(sc, pOwn->panelLineId);
                    pOwn->panelLineId = 0;
                    pOwn->panelDrawn = 0;
                    pOwn->panelText[0] = '\0';
                }
                else
                {
                    const TtdDrawIntent& w0 = plan.intents[0];
                    TtdDetail det;
                    char panelBuf[512];
                    panelBuf[0] = '\0';
                    const int haveDetail =
                        TtdDetailAt(pDec, w0.episodeId, availBar, &det);
                    if (haveDetail)
                        TtdDetailText(&det, showDelta, panelBuf,
                                      (int)sizeof(panelBuf));
                    if (!haveDetail || panelBuf[0] == '\0')
                    {
                        if (pOwn->panelDrawn && pOwn->panelLineId > 0)
                            DeleteDrawing(sc, pOwn->panelLineId);
                        pOwn->panelLineId = 0;
                        pOwn->panelDrawn = 0;
                        pOwn->panelText[0] = '\0';
                    }
                    else if (!pOwn->panelDrawn ||
                             strcmp(pOwn->panelText, panelBuf) != 0)
                    {
                        const int pBar = max(0, min(w0.bandEndAvail,
                                                    sc.ArraySize - 1));
                        const double pBotP = (double)w0.bottomTick * sc.TickSize;
                        const double pTopP = (double)w0.topTick * sc.TickSize;
                        s_UseTool T;
                        T.Clear();
                        T.ChartNumber = sc.ChartNumber;
                        T.AddMethod = UTAM_ADD_OR_ADJUST;
                        T.LineNumber = pOwn->panelLineId; // 0 = allocate once
                        T.AddAsUserDrawnDrawing = 0;
                        T.DrawingType = DRAWING_TEXT;
                        T.BeginDateTime = sc.BaseDateTimeIn[pBar];
                        T.BeginValue = (w0.dir == 1) ? pBotP : pTopP;
                        T.Text = panelBuf;
                        T.Color = COLOR_WHITE;
                        T.FontSize = 8;
                        T.FontBold = 1;
                        T.TransparentLabelBackground = 1;
                        // Checked outcome: retain the ID only on a positive
                        // result with a positive allocated ID; anything else
                        // retries on a later call with the cache untouched.
                        if (sc.UseTool(T) > 0 && T.LineNumber > 0)
                        {
                            pOwn->panelLineId = T.LineNumber;
                            pOwn->panelDrawn = 1;
                            strncpy(pOwn->panelText, panelBuf,
                                    sizeof(pOwn->panelText) - 1);
                            pOwn->panelText[sizeof(pOwn->panelText) - 1] = '\0';
                        }
                    }
                }
            }
        }
        else
        {
            if (wasRendering)
            {
                // Leaving decision mode: remove exactly this instance's
                // owned drawings; PASS 4 below restores legacy rendering
                // from the preserved canonical records.
                if (pOwn != nullptr)
                    TtdDeleteOwnedDrawings(sc, pOwn);
                sc.SetPersistentInt(P_DMODE, 0);
            }
        }
    }

    // =========================================================================
    // PASS 4 — Draw zone rectangles
    // =========================================================================
    // Active zones extend to curBar (v1 behaviour, refreshed each processing
    // call).  Erased zones are frozen at ErasedBar and drawn dimmer.
    // Skipped while the decision renderer owns the screen (records above are
    // still maintained for compatibility and restored on toggle-off).
    bool slotExhausted = false;

    // -- [v4-M1] Publish zones for cross-chart merging ------------------------
    // Runs whatever Zone Display says: a Hidden source draws nothing but must
    // still publish.  Stamps times for every record (bar indices mean nothing
    // on another chart) and bumps the change counter merging charts watch.
    // Nothing between PASS 1 and here returns early, so a record is never left
    // unstamped when another chart reads it.
    for (size_t i = 0; i < zones.size(); i++)
    {
        ZoneRec& z = zones[i];
        if (z.DetectedBar >= 0 && z.DetectedBar < sc.ArraySize)
            z.StartDT = sc.BaseDateTimeIn[z.DetectedBar];
        if (!z.Valid && z.ErasedBar >= 0 && z.ErasedBar < sc.ArraySize)
        {
            // End of the erasing bar: a 15m zone killed by the 10:30 bar
            // lasts until 10:45 on a 1m chart, not 10:30.
            if (z.EndDT.GetAsDouble() <= 0.0)
                z.EndDT = sc.GetEndingDateTimeForBarIndex(z.ErasedBar);
        }
        else
            z.EndDT = 0.0;
    }
    sc.SetPersistentDouble(P_MTICK, sc.TickSize);
    sc.SetPersistentInt(P_MGEN, sc.GetPersistentInt(P_MGEN) + 1);
    sc.SetPersistentInt(P_MTMODE, thresholdMode + 1);   // [v4-M2] label suffix
    sc.SetPersistentInt(P_MMAGIC, MergeMagic());

    if (!decRendering && zoneDisplay == ZDISP_OWN)
    {
    for (size_t i = 0; i < zones.size(); i++)
    {
        ZoneRec& z = zones[i];

        if (z.Pending) continue;                       // not yet confirmed
        if (!z.Valid && z.ErasedBar < 0) continue;     // rejected — never draw
        if (!z.Valid && !showErased) continue;         // clipped mode
        if (!z.Valid && ErasedBarExpired(days, z.ErasedBar, keepErased))
            continue;                                  // [v4-M5] past Keep Erased Zones

        // Retry allocation: a zone refused a slot during a rejection-heavy
        // batch recovers once pruning recycles slots (Task 8). Plain first
        // fit, no eviction — eviction already ran at creation time.
        if (z.Slot < 0)
        {
            z.Slot = AllocSlot(sc, (z.Source == ZSRC_AUTO) ? slotsA : slotsM);
            if (z.Slot < 0) { slotExhausted = true; continue; }
        }

        const bool isAuto = (z.Source == ZSRC_AUTO);
        COLORREF fill, border;
        if (isAuto) { fill = z.IsBuyZone ? autoBuyFill : autoSellFill;
                      border = z.IsBuyZone ? autoBuyBord : autoSellBord; }
        else        { fill = z.IsBuyZone ? buyFill : sellFill;
                      border = z.IsBuyZone ? buyBord : sellBord; }
        if (!z.Valid) { fill = erFill; border = erBord; }   // [v4-M5]

        int endBar, drawTransp;
        if (z.Valid)
        {
            endBar     = curBar;
            drawTransp = transparency;
        }
        else
        {
            endBar     = z.ErasedBar;
            drawTransp = min(transparency + 30, 95);
        }
        if (endBar < z.DetectedBar) endBar = z.DetectedBar;
        if (endBar > curBar)        endBar = curBar;

        const float botPrice = (float)(z.BottomTick * sc.TickSize);
        const float topPrice = (float)(z.TopTick    * sc.TickSize);

        DrawZoneRect(sc, ZoneRectBase(sc, z) + z.Slot,
                     z.DetectedBar, endBar,
                     botPrice, topPrice,
                     fill, border, borderWidth, drawTransp, lockZones);
        z.Drawn = true;

        // Delta label — active zones only.
        // Fix B4: when Show Delta Info is off the label is DELETED, not merely
        // skipped.  v1 left labels of active zones on the chart forever.
        if (showDelta && z.Valid)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f", fabs(z.TotalDelta));

            s_UseTool T;
            T.Clear();
            T.ChartNumber            = sc.ChartNumber;
            T.DrawingType            = DRAWING_TEXT;
            T.BeginIndex             = z.DetectedBar;
            T.BeginValue             = z.IsBuyZone ? (double)botPrice : (double)topPrice;
            T.Text                   = buf;
            T.Color                  = COLOR_WHITE;
            T.FontSize               = 8;
            T.AddMethod              = UTAM_ADD_OR_ADJUST;
            T.LineNumber             = ZoneLabelBase(sc, z) + z.Slot;
            T.TransparencyLevel      = 75;
            T.AddAsUserDrawnDrawing  = 1;
            T.AllowCopyToOtherCharts = 1;
            T.LockDrawing            = lockZones ? 1 : 0;   // [v4-M4]
            sc.UseTool(T);
        }
        else
        {
            DeleteDrawing(sc, ZoneLabelBase(sc, z) + z.Slot);
        }
    }

    if (slotExhausted && sc.GetPersistentInt(P_LOGGED_SLOTS) == 0)
    {
        sc.SetPersistentInt(P_LOGGED_SLOTS, 1);
        sc.AddMessageToLog("Trapped Traders: drawing slot pool exhausted; some zones are "
                           "tracked in subgraphs but not drawn.", 0);
    }
    } // end if (!decRendering && own) — legacy renderer gated by decision mode

    // =========================================================================
    // PASS 4M — [v4-M1] Merged cross-chart zones
    // =========================================================================
    // Always a full rebuild of the merged drawing on a processing call: live
    // merged zones extend to the forming bar exactly like PASS 4's.  The
    // Decision Map owns the screen when it renders, so merging yields to it.
    if (!decRendering && zoneDisplay == ZDISP_MERGED)
    {
        MergeSource msrc[1 + MERGE_MAX_SOURCES];
        MergeResolveSources(sc, &zones, mergeCharts, mergeStudies, msrc);
        RenderMergedZones(sc, msrc, mergeCfg);
    }
    else if (sc.GetPersistentInt(P_MDRAWN) > 0 || sc.GetPersistentInt(P_MSTDRAWN) != 0)
    {
        DeleteMergedDrawings(sc);
    }

    // =========================================================================
    // PASS 5 — Auto status label and auto-unavailable warning
    // =========================================================================
    // Refreshed only on a processing call (bar close / full recalc), never on
    // every chart update.
    {
        bool  curReady        = false;
        float curBlock        = 0.0f;
        float curZone         = 0.0f;
        int   curBaselineDays = 0;
        int   curDayDate      = 0;
        int   failReason      = BFAIL_NONE;
        int   failDayIdx      = -1;
        int   curSkipped      = 0;
        int   curScanned      = 0;
        int   curOldestUsed   = -1;

        if (curDayIdx >= 0)
        {
            curReady        = days[curDayIdx].ThreshReady;
            curBlock        = days[curDayIdx].AutoMinBlock;
            curZone         = days[curDayIdx].AutoZoneTotal;
            curBaselineDays = days[curDayIdx].BaselineDays;
            curDayDate      = days[curDayIdx].DayDate;
            failReason      = days[curDayIdx].FailReason;
            failDayIdx      = days[curDayIdx].FailDayIdx;
            curSkipped      = days[curDayIdx].SkippedDays;
            curScanned      = days[curDayIdx].ScannedDays;
            curOldestUsed   = days[curDayIdx].OldestUsedDay;
        }

        // Human-readable reason the baseline was refused.  Without this the
        // chart can only say "need 5 days", which is misleading when the chart
        // has 30 days loaded but one of them is unusable.
        SCString reasonTxt;
        switch (failReason)
        {
            case BFAIL_TOO_FEW:
            {
                // [v3-C6] Name the most recent unusable segment and its sample
                // counts. FailDayIdx was only ever -1, so this line could say
                // how many days were skipped but never which, or why.
                SCString thinTxt;
                if (failDayIdx >= 0 && failDayIdx < (int)days.size())
                {
                    SCDateTime tdt(days[failDayIdx].DayDate, 0);
                    thinTxt.Format("; most recent unusable segment #%d (%04d-%02d-%02d) "
                                   "had %d block and %d zone samples, need %d and %d",
                                   failDayIdx, tdt.GetYear(), tdt.GetMonth(), tdt.GetDay(),
                                   days[failDayIdx].SampleBlockN,
                                   days[failDayIdx].SampleZoneN,
                                   MIN_BLOCK_SAMPLES_PER_DAY, MIN_ZONE_SAMPLES_PER_DAY);
                }
                reasonTxt.Format("found %d usable session(s) of the %d needed - scanned %d "
                                 "segment(s) back, skipped %d thin one(s) (weekend/holiday); "
                                 "chart has %d segment(s) total%s",
                                 curBaselineDays, autoDays, curScanned, curSkipped,
                                 (int)days.size(), thinTxt.GetChars());
                break;
            }
            case BFAIL_PARTIAL:
                reasonTxt.Format("baseline day #%d is the chart's first (partial) segment",
                                 failDayIdx);
                break;
            case BFAIL_INCOMPLETE:
                reasonTxt.Format("baseline day #%d is not a completed trading day", failDayIdx);
                break;
            case BFAIL_SAMPLES:
            {
                int bn = 0, zn = 0, dd = 0;
                if (failDayIdx >= 0 && failDayIdx < (int)days.size())
                {
                    bn = days[failDayIdx].SampleBlockN;
                    zn = days[failDayIdx].SampleZoneN;
                    dd = days[failDayIdx].DayDate;
                }
                SCDateTime fdt(dd, 0);
                reasonTxt.Format("baseline day #%d (%04d-%02d-%02d) had %d block and %d zone samples, "
                                 "need %d and %d",
                                 failDayIdx, fdt.GetYear(), fdt.GetMonth(), fdt.GetDay(),
                                 bn, zn, MIN_BLOCK_SAMPLES_PER_DAY, MIN_ZONE_SAMPLES_PER_DAY);
                break;
            }
            case BFAIL_DEGENERATE:
                reasonTxt = "median of the daily percentiles came out zero";
                break;
            default:
                reasonTxt = "ready";
                break;
        }

        // -- Status label ------------------------------------------------------
        const bool wantStatus = wantAuto && (showAutoStatus != 0) && (curDayIdx >= 0);
        if (wantStatus)
        {
            SCDateTime dt(curDayDate, 0);
            SCString txt;
            if (curReady)
            {
                int oy = 0, om = 0, od = 0;
                if (curOldestUsed >= 0 && curOldestUsed < (int)days.size())
                {
                    SCDateTime odt(days[curOldestUsed].DayDate, 0);
                    oy = odt.GetYear(); om = odt.GetMonth(); od = odt.GetDay();
                }
                const char* sessTxt = (autoSession == SESS_RTH) ? "RTH"
                                    : (autoSession == SESS_ETH) ? "ETH" : "BOTH";
                txt.Format("TT AUTO [%s]  day=%04d-%02d-%02d  block=%.1f  zone=%.1f  "
                           "baseline=%d sessions back to %04d-%02d-%02d  skipped=%d thin",
                           sessTxt, dt.GetYear(), dt.GetMonth(), dt.GetDay(),
                           curBlock, curZone, curBaselineDays, oy, om, od, curSkipped);
            }
            else
            {
                const char* sessTxt = (autoSession == SESS_RTH) ? "RTH"
                                    : (autoSession == SESS_ETH) ? "ETH" : "BOTH";
                txt.Format("TT AUTO [%s]  day=%04d-%02d-%02d  NOT READY  baseline days=%d  "
                           "segments=%d  reason: %s",
                           sessTxt, dt.GetYear(), dt.GetMonth(), dt.GetDay(),
                           curBaselineDays, (int)days.size(), reasonTxt.GetChars());
            }
            DrawStatusText(sc, StudyLine(sc, LINE_STATUS), 2, 95, txt,
                           curReady ? RGB(180, 220, 255) : RGB(180, 180, 180), lockZones);
            sc.SetPersistentInt(P_STATUS_DRAWN, 1);
        }
        else if (sc.GetPersistentInt(P_STATUS_DRAWN) != 0)
        {
            DeleteDrawing(sc, StudyLine(sc, LINE_STATUS));
            sc.SetPersistentInt(P_STATUS_DRAWN, 0);
        }

        // -- Warning -----------------------------------------------------------
        // Shown in Auto and Both mode whenever the current trading day has no
        // usable baseline.  No thresholds are fabricated and no fallback to the
        // manual values occurs; in Both mode manual zones keep printing.
        const bool wantWarn = wantAuto && !curReady;
        if (wantWarn)
        {
            SCString warn;
            warn.Format("TRAPPED TRADERS AUTO: NEED %d COMPLETE DAYS", autoDays);
            DrawStatusText(sc, StudyLine(sc, LINE_WARNING), 2, 90, warn, RGB(255, 90, 90), lockZones);
            sc.SetPersistentInt(P_WARN_DRAWN, 1);

            if (sc.GetPersistentInt(P_LOGGED_HIST) == 0)
            {
                sc.SetPersistentInt(P_LOGGED_HIST, 1);   // one entry, no spam
                SCString msg;
                msg.Format("Trapped Traders v4: automatic thresholds unavailable. "
                           "Need %d complete trading days before the current day. "
                           "Chart has %d trading day segment(s), %d usable baseline day(s). "
                           "Reason: %s. No auto zones will be produced until this is resolved.",
                           autoDays, (int)days.size(), curBaselineDays, reasonTxt.GetChars());
                sc.AddMessageToLog(msg, 0);
            }
        }
        else if (sc.GetPersistentInt(P_WARN_DRAWN) != 0)
        {
            // Baseline became available (or mode left Auto/Both) — remove it.
            DeleteDrawing(sc, StudyLine(sc, LINE_WARNING));
            sc.SetPersistentInt(P_WARN_DRAWN, 0);
        }
    }

    // =========================================================================
    // Decision Map journal + snapshot (v2.1, Phase 3 Task 11)
    // =========================================================================
    // Canonical checked exports through the SAME TtdFileOps seam the portable
    // export tests drive with failure injection. Journal (OFF by default):
    // appends rows only for new transition batches — an unchanged callback
    // performs zero file I/O (never a per-call full retained rewrite), and
    // the served seq watermark (P_DJSEQ) guarantees no duplicate append.
    // Attempts are gated separately by TtdJournalAttemptDue on the
    // attempted high-water (P_DJATT): after a failure, unchanged callbacks
    // perform zero further file attempts; one bounded retry happens only
    // on a new closed bar or genuinely new transitions. Snapshot: an
    // increase of In:36 over the served value (P_DSNAP) writes exactly
    // one complete file via tmp + atomic replace, even with zero episodes,
    // so an old nonempty export can never linger as stale evidence; a
    // decrease never re-requests (TtdSnapshotDue is increase-only) and a
    // stale decrease below the attempted high-water (P_DSATT) stays quiet
    // even on a new bar. Both
    // filenames reuse the legacy stem plus _decision_, chart number,
    // study-instance ID and run generation: the legacy schema is never
    // overwritten with event rows. Any checked I/O failure latches
    // P_DEXPFAIL (SG19 bit 8, cleared by the next successful checked op or a
    // rebuild) with one Message Log line per generation.
    if (decActive && pDec != nullptr && exportFile[0] != '\0')
    {
        const SCString decDataFolder = sc.DataFilesFolder();
        const char* decFolder = (exportFolder[0] != '\0') ? exportFolder
                                                          : decDataFolder.GetChars();
        if (decFolder == nullptr)
            decFolder = "";
        const size_t decFolderLen = strlen(decFolder);
        const bool decNeedSep = (decFolderLen > 0 &&
                                 decFolder[decFolderLen - 1] != '\\' &&
                                 decFolder[decFolderLen - 1] != '/');

        char decBase[256];
        char decBaseSnap[256];
        const int decGen = sc.GetPersistentInt(P_DGEN);
        const int decNameOk =
            (TtdDecisionFileName(exportFile, "journal", sc.ChartNumber,
                                 sc.StudyGraphInstanceID, decGen, decBase,
                                 (int)sizeof(decBase)) == TTD_OK) &&
            (TtdDecisionFileName(exportFile, "snapshot", sc.ChartNumber,
                                 sc.StudyGraphInstanceID, decGen, decBaseSnap,
                                 (int)sizeof(decBaseSnap)) == TTD_OK);
        if (!decNameOk)
        {
            sc.SetPersistentInt(P_DEXPFAIL, 1);
        }
        else
        {
            char decJournalPath[1024];
            char decSnapPath[1024];
            char decSnapTmp[1088];
            snprintf(decJournalPath, sizeof(decJournalPath), "%s%s%s",
                     decFolder, decNeedSep ? "\\" : "", decBase);
            snprintf(decSnapPath, sizeof(decSnapPath), "%s%s%s",
                     decFolder, decNeedSep ? "\\" : "", decBaseSnap);
            snprintf(decSnapTmp, sizeof(decSnapTmp), "%s.tmp", decSnapPath);

            TtdExportCtx decCtx;
            decCtx.runId = decGen;
            decCtx.fpA = sc.GetPersistentInt(P_DFPA);
            decCtx.fpB = sc.GetPersistentInt(P_DFPB);
            decCtx.chartNumber = sc.ChartNumber;
            decCtx.instanceId = sc.StudyGraphInstanceID;
            decCtx.symbol = sc.Symbol.GetChars();
            decCtx.tickSize = (double)sc.TickSize;
            decCtx.tsArg = (void*)&sc;
            decCtx.tsForBar = TtdProdTimestamps;

            TtdExportState decXst;
            decXst.lastJournalSeq = sc.GetPersistentInt(P_DJSEQ);
            decXst.lastSnapshotReq = sc.GetPersistentInt(P_DSNAP);
            decXst.exportFailed = sc.GetPersistentInt(P_DEXPFAIL);
            const int decFailBefore = decXst.exportFailed;

            TtdFileOps decOps = TtdProdFileOps();
            // Attempt gating (review-fix-2, BuildSpec §6/§7): unchanged
            // forming callbacks (visualOnly) attempt only genuinely new
            // work since the last attempt; new-bar / rebuild / full-recalc
            // callbacks allow one bounded retry of still-pending work.
            // Either way an unchanged callback after a failure performs
            // ZERO file attempts, and a stale snapshot decrease below the
            // attempted high-water never re-requests.
            const int decIsNewBar = visualOnly ? 0 : 1;
            if (TtdJournalAttemptDue(decJournal, decXst.lastJournalSeq,
                                     sc.GetPersistentInt(P_DJATT),
                                     pDec->numTransitions, decIsNewBar))
            {
                sc.SetPersistentInt(P_DJATT, pDec->numTransitions);
                TtdJournalSync(pDec, &decXst, &decCtx, &decOps, 1,
                               decJournalPath);
            }
            if (TtdSnapshotAttemptDue(decXst.lastSnapshotReq,
                                      sc.GetPersistentInt(P_DSATT),
                                      decSnapshot, decIsNewBar))
            {
                const int prevSAtt = sc.GetPersistentInt(P_DSATT);
                sc.SetPersistentInt(P_DSATT, max(prevSAtt, decSnapshot));
                if (TtdSnapshotWrite(pDec, &decXst, &decCtx, &decOps,
                                     decSnapTmp, decSnapPath) == TTD_OK)
                    decXst.lastSnapshotReq = decSnapshot;
                // On failure the request stays unserved while the attempted
                // high-water holds: no new file attempts until a new closed
                // bar (same value, one retry), new transitions, or an
                // explicit request increase — never a stale decrease.
            }

            sc.SetPersistentInt(P_DJSEQ, decXst.lastJournalSeq);
            sc.SetPersistentInt(P_DSNAP, decXst.lastSnapshotReq);
            sc.SetPersistentInt(P_DEXPFAIL, decXst.exportFailed);
            if (decXst.exportFailed && !decFailBefore)
            {
                SCString decMsg;
                decMsg.Format("Trapped Traders v4: decision journal/snapshot "
                              "write failed (chart %d instance %d run %d). "
                              "SG19 bit 8 is latched; no complete audit is "
                              "claimed until a later write succeeds.",
                              sc.ChartNumber, sc.StudyGraphInstanceID,
                              decGen);
                sc.AddMessageToLog(decMsg, 1);
            }
        }
    }

    // =========================================================================
    // CSV export — the full zone list, rewritten
    // =========================================================================
    // Placement is load-bearing: everything above has already settled this
    // call's zone state, and an intrabar tick returned long before reaching
    // here (the closed-bar guard), so the file is only ever touched on a bar
    // close or a full recalculation.
    //
    // The file is TRUNCATED and rewritten in full rather than appended to.  A
    // zone can be invalidated retroactively — erased by a later close, killed by
    // the manual-precedence sweep, or absorbed into a wider zone — so an already
    // written row can become wrong after the fact, and an append-only file would
    // accumulate rows that no longer describe anything.
    // Review-fix-1: a visual-only refresh (no new bar, no rebuild) leaves
    // legacy zones untouched, so the legacy CSV is skipped here — no
    // redundant file work on a decision display/export toggle.
    if (!visualOnly && exportCSV != 0 && exportFile[0] != '\0')
    {
        const SCString dataFolder = sc.DataFilesFolder();

        const char* folder = (exportFolder[0] != '\0') ? exportFolder
                                                       : dataFolder.GetChars();
        if (folder == nullptr) folder = "";

        const size_t folderLen = strlen(folder);
        const bool needSep = (folderLen > 0 &&
                              folder[folderLen - 1] != '\\' &&
                              folder[folderLen - 1] != '/');

        // Chart/instance suffix (Task 11): the configured stem is retained
        // but two charts — or two instances on one chart — no longer collide
        // on a single path. The legacy schema is untouched: decision event
        // rows live in their own _decision_ files (see above).
        char csvBase[256];
        if (TtdLegacyCsvName(exportFile, sc.ChartNumber,
                             sc.StudyGraphInstanceID, csvBase,
                             (int)sizeof(csvBase)) != TTD_OK)
            csvBase[0] = '\0';
        char csvPath[1024];
        if (csvBase[0] != '\0')
            snprintf(csvPath, sizeof(csvPath), "%s%s%s",
                     folder, needSep ? "\\" : "", csvBase);
        else
            snprintf(csvPath, sizeof(csvPath), "%s%s%s",
                     folder, needSep ? "\\" : "", exportFile);

        // An empty zone list writes a valid header-only table: leaving a
        // previous nonempty export in place would present stale rows as if
        // they described the current (empty) zone list. This full-rewrite
        // cadence is the documented expensive compatibility mode — with
        // retainForExport the list only grows, so this file costs O(zones)
        // per bar close; the decision journal above is the efficient path.
        {
            FILE* f = fopen(csvPath, "wb");
            if (f == nullptr)
            {
                // Same non-spamming pattern as the insufficient-history warning:
                // one entry, latched until a full recalculation clears it.  A bad
                // path would otherwise write a Message Log line on every bar
                // close for the rest of the session.
                if (sc.GetPersistentInt(P_LOGGED_CSV) == 0)
                {
                    sc.SetPersistentInt(P_LOGGED_CSV, 1);
                    SCString msg;
                    msg.Format("Trapped Traders v4: could not open CSV export file \"%s\" "
                               "for writing. Zone export is disabled until the path is "
                               "corrected and the study is recalculated.", csvPath);
                    sc.AddMessageToLog(msg, 1);
                }
            }
            else
            {
                char symBuf[128];
                CsvQuoted(sc.Symbol.GetChars(), symBuf, sizeof(symBuf));

                const SCString barPeriod = CsvBarPeriodText(sc);
                char perBuf[128];
                CsvQuoted(barPeriod.GetChars(), perBuf, sizeof(perBuf));

                const char* activeMode = (confirmMode == CONF_NEXTBAR) ? "NEXTBAR"
                                                                       : "DISPLACE";

                // Checked writes (Task 11): every fprintf and the final
                // fclose are verified; a short/failed write latches the same
                // one-shot log as an open failure instead of claiming a
                // complete export.
                bool csvOk = true;
                if (fprintf(f,
                    "zone_id,source,direction,trading_day,detect_bar,detect_datetime,"
                    "bottom_tick,top_tick,bottom_price,top_price,total_delta,block_count,"
                    "auto_block_threshold,auto_zone_threshold,auto_baseline_ready,"
                    "nb_outcome,nb_bar,nb_datetime,"
                    "dp_outcome,dp_bar,dp_datetime,"
                    "erase_bar,erase_datetime,active_mode,is_drawn,live_state,"
                    "session,symbol,chart_number,tick_size,bar_period\n") < 0)
                    csvOk = false;

                for (size_t i = 0; i < zones.size(); i++)
                {
                    const ZoneRec& z = zones[i];

                    // Auto threshold context is reported for the trading day the
                    // zone was DETECTED in, not the current day: that is the
                    // threshold that actually gated it, and it is the only one a
                    // downstream reconstruction can use.
                    float  zBlockThr = 0.0f;
                    float  zZoneThr  = 0.0f;
                    int    zReady    = 0;
                    char   tradingDay[16];
                    tradingDay[0] = '\0';

                    for (size_t k = 0; k < days.size(); k++)
                    {
                        if (z.DetectedBar < days[k].FirstBar) continue;
                        if (z.DetectedBar > days[k].LastBar)  continue;

                        const SCDateTime ddt(days[k].DayDate, 0);
                        snprintf(tradingDay, sizeof(tradingDay), "%04d-%02d-%02d",
                                 ddt.GetYear(), ddt.GetMonth(), ddt.GetDay());

                        if (days[k].ThreshComputed && days[k].ThreshReady)
                        {
                            zBlockThr = days[k].AutoMinBlock;
                            zZoneThr  = days[k].AutoZoneTotal;
                            zReady    = 1;
                        }
                        break;
                    }

                    char detectDT[32], nbDT[32], dpDT[32], eraseDT[32];
                    CsvBarDateTime(sc, z.DetectedBar, false, detectDT, sizeof(detectDT));
                    CsvBarDateTime(sc, z.NB_Bar,      false, nbDT,     sizeof(nbDT));
                    CsvBarDateTime(sc, z.DP_Bar,      false, dpDT,     sizeof(dpDT));
                    CsvBarDateTime(sc, z.ErasedBar,   false, eraseDT,  sizeof(eraseDT));

                    // The bar's ACTUAL session, not the In:23 filter: a consumer
                    // needs to be able to split by session even when the filter
                    // was set to Both, and the filter setting is recoverable
                    // from the study's own inputs anyway.
                    // What the study ACTUALLY did with this zone under the rule
                    // In:26 selected.  nb_outcome and dp_outcome are the two
                    // hypotheses; live_state is the outcome that was acted on,
                    // which is what makes a row self-describing without needing
                    // to know how the file was produced.
                    const char* liveState =
                        z.Valid ? (z.Pending ? "PENDING" : "ACTIVE")
                                : (z.ErasedBar >= 0 ? "ERASED"
                                   : (z.AbsorbedInto >= 0 ? "MERGED" : "REJECTED"));

                    const char* sessTxt = "";
                    if (z.DetectedBar >= 0 && z.DetectedBar < sc.ArraySize)
                    {
                        const int tod =
                            sc.BaseDateTimeIn[z.DetectedBar].GetTimeInSeconds();
                        sessTxt = IsTimeInRange(tod, rthStartSec, rthEndSec) ? "RTH" : "ETH";
                    }

                    if (fprintf(f,
                        "%d,%s,%s,%s,%d,%s,"
                        "%d,%d,%.5f,%.5f,%.1f,%d,"
                        "%.5f,%.5f,%d,"
                        "%s,%d,%s,"
                        "%s,%d,%s,"
                        "%d,%s,%s,%d,%s,"
                        "%s,%s,%d,%.5f,%s\n",
                        z.ID,
                        (z.Source == ZSRC_AUTO) ? "AUTO" : "MANUAL",
                        z.IsBuyZone ? "LONG" : "SELL",
                        tradingDay,
                        z.DetectedBar,
                        detectDT,
                        z.BottomTick,
                        z.TopTick,
                        (double)(z.BottomTick * sc.TickSize),
                        (double)(z.TopTick    * sc.TickSize),
                        (double)z.TotalDelta,
                        z.BlockCount,
                        (double)zBlockThr,
                        (double)zZoneThr,
                        zReady,
                        CsvOutcomeText(z.NB_Outcome),
                        z.NB_Bar,
                        nbDT,
                        CsvOutcomeText(z.DP_Outcome),
                        z.DP_Bar,
                        dpDT,
                        z.ErasedBar,
                        eraseDT,
                        activeMode,
                        z.Drawn ? 1 : 0,
                        liveState,
                        sessTxt,
                        symBuf,
                        sc.ChartNumber,
                        (double)sc.TickSize,
                        perBuf) < 0)
                        csvOk = false;
                }

                if (fclose(f) != 0)
                    csvOk = false;
                if (!csvOk && sc.GetPersistentInt(P_LOGGED_CSV) == 0)
                {
                    sc.SetPersistentInt(P_LOGGED_CSV, 1);
                    SCString wmsg;
                    wmsg.Format("Trapped Traders v4: short or failed write to CSV export "
                                "file \"%s\". The file may be incomplete; zone export is "
                                "disabled until the path is corrected and the study is "
                                "recalculated.", csvPath);
                    sc.AddMessageToLog(wmsg, 1);
                }
            }
        }
    }

    // Update the closed-bar guard for the next call. Visual-only refreshes
    // leave both frontiers untouched (empty semantic window) but latch the
    // served display controls so the next unchanged tick is a true no-op.
    sc.SetPersistentInt(P_LASTBAR, lastBar);
    if (decActive && pDec != nullptr)
    {
        sc.SetPersistentInt(P_DLASTBAR, decMark);
        // [v3-C5] Latch the display controls unconditionally. Skipping this
        // while halted meant P_DMAXN / P_DDETL never caught up, so
        // TtdVisualRefreshDue kept reporting a change and every intrabar tick
        // re-ran the whole PASS 3E renderer for as long as the halt lasted.
        // The halt stays visible where it belongs — SG19 bit 4 and the one
        // Message Log line — not in a render loop that never settles.
        sc.SetPersistentInt(P_DMAXN, decMaxNearby);
        sc.SetPersistentInt(P_DDETL, decDetail);
    }

}   // end scsf_TrappedTraders_v4
