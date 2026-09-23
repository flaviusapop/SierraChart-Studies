# Model Chat Synthesis — Formula Access Implementation Priorities

**Topic:** Which ideas to implement first when getting raw orderflow trigger formulas in Sierra Chart ACSIL C++
**Agents:** 5 (systems-thinker, pragmatist, edge-case-finder, contrarian, risk-analyst)
**Rounds:** 3 + synthesizer
**Date:** 2026-06-15

---

### Consensus

**1. Idea 3 (Trigger Correlation Decomposition / CSV logging) comes first.** Every participant converged on this, though with different conditions attached. The logic is consistent: you cannot calibrate thresholds (Idea 4) against a baseline you have not measured, and you cannot score cross-TF cascades (Idea 1) before knowing which triggers carry independent signal. All diagnostic and calibration work downstream depends on this foundation.

**2. Idea 4 (Dynamic Session-Aware Threshold Calibration) is the correct second step.** Four of five participants landed here. The dissent (edge-case-finder initially preferring Idea 6) collapsed by Round 2. The dependency is clean: Idea 4's EWMA buffer needs the per-trigger hit-rate data from Idea 3 to avoid tuning noise.

**3. Intrabar guard is a cross-cutting prerequisite, not a per-study concern.** The edge-case-finder named it; systems-thinker turned it into a concrete audit step. Every participant accepted that CSV writes, EWMA updates, and coiling state reads must all gate on bar-close. This is not optional per-study hygiene — it is a system-wide architectural invariant that must be verified before any logging begins.

**4. Ideas 1, 2, and 6 should be deferred.** No participant successfully defended any of them as a first or second priority. Idea 6 (exit timing) has an onset-detection problem that makes it structurally harder than it looks. Idea 2 (velocity) is potentially redundant if Idea 5 (coiling) is implemented. Idea 1 (cross-TF cascade scoring) requires knowing which individual triggers are valid before scoring their interaction — it is downstream of Idea 3 by definition.

**5. The validation gate must be falsifiable and binding.** Contrarian and risk-analyst both insisted — and no one disputed — that Idea 3 is worthless unless the team commits in advance to removing triggers that fail the gate. A logging exercise that never filters anything is not a prerequisite; it is delay.

---

### Key Disagreements

**Tension 1: Time threshold vs. occurrence threshold for the validation window.**

- Risk-analyst specified 30 days live OR 30 days replay.
- Contrarian argued: use 40 occurrences per trigger minimum, not a fixed time window. A trigger firing 3x/day reaches 40 in ~2 weeks; a rare trigger might need 5 months. Time-boxing creates false confidence on rare triggers and unnecessary delay on common ones.

**Assessment:** Contrarian is correct. Occurrence-based gates are statistically coherent; time-based gates are a proxy that fails at both ends of the frequency distribution. The practical implementation is to log a per-trigger fire count in the CSV and enforce n≥40 per trigger before that trigger's hit rate is considered meaningful. The risk-analyst's performance bar (>55% directional accuracy at N=10) is the right threshold metric — apply it to occurrence count, not calendar days.

**Tension 2: Whether Idea 3 should be skipped if live P&L attribution data already exists.**

- Contrarian: if you have actual live P&L data attributed by trigger, Idea 3 duplicates what you already know. Go directly to Idea 4.
- Risk-analyst and systems-thinker: live P&L is confounded by execution quality, sizing, and discretionary overlays. Per-trigger forward return at N=5,10,20 bars in isolation is a different and cleaner signal than realized P&L.

**Assessment:** Risk-analyst and systems-thinker are correct. P&L attribution is a composite measure; Idea 3 produces an unconfounded marginal signal per trigger. The contrarian's point stands as a conditional: if P&L attribution is already granular enough to identify which specific triggers contribute and which do not, the silent period can be shortened. The commitment condition (willingness to remove underperformers) remains binding regardless.

**Tension 3: Whether session boundary contamination is a day-one blocker.**

- Pragmatist: sc.IsFullRecalculation already handles it, same pattern as BigTradesTape.
- Risk-analyst: partial-session recalc (UpdateStartIndex < last written bar) is the specific residual failure mode.

**Assessment:** Pragmatist is too optimistic. sc.IsFullRecalculation handles full recalcs but not partial-session recalcs triggered mid-day. Risk-analyst's fix: store last-written bar index in a persistent int, skip CSV write if current bar index ≤ stored value. This resolves both cases. Day-one design requirement, not deferred documentation.

---

### Surprising Insights

**Regime-matching invalidates pooled baselines.** A trigger firing in a trending session has a non-trivial unconditional expected forward move regardless of the trigger itself. Pool trending and mean-reverting sessions together and every hit rate reflects session character, not trigger quality. The CSV must include a session-regime label column (from OTFStateFilter) from day one. Do not add it later — retrofitting unlabeled historical rows defeats the purpose.

**The intrabar guard is an architectural invariant, not a study-level pattern.** The guard must live at the *source* study (where data is produced), not at the consumer. If a consumer reads a subgraph mid-bar, the source's guard being correct does not protect the consumer from stale values. Project-wide rule: every study that writes to a subgraph gates on isNewBar; every study that reads from another study's subgraph also gates on isNewBar independently.

**Idea 4's EWMA buffer can be built now and activated later.** The circular buffer implementation can be written and tested in parallel with Idea 3's logging period. It does not write to persistent state until triggers pass the validation gate. This eliminates the perceived serial dependency and shortens the overall timeline.

**Cold-start on session boundary is distinct from recalc contamination.** The buffer starting from zero on session open produces artificially low EWMA values in the first N bars. This requires either (a) persisting end-of-session EWMA values across the boundary, or (b) explicitly flagging early-session bars as cold-start and falling back to static baseline during warm-up.

---

### Final Recommendation

**Sequence: Step 0 → Idea 3 → Idea 4 (built in parallel with Idea 3, activated after validation gate)**

---

**Step 0: Intrabar audit (1 day, no code shipped)**

Verify every study in the project gates on `isNewBar` before any subgraph write. Read-and-confirm pass only, no rewrite. Done when: zero studies write to subgraphs or persistent state on intrabar ticks.

---

**Step 1: Idea 3 — Baseline logging**

*Run until n≥40 occurrences per trigger (not calendar days).*

Critical design decisions:

1. **Bar-close only.** CSV write is in the post-loop watermark block, never inside the bar loop. Write guard: `current bar index > last-written bar index` stored in `sc.GetPersistentInt(10)`. Handles both full recalc and partial-session recalc contamination.

2. **Per-trigger rows, not composite rows.** Columns: `timestamp, bar_index, trigger_id, price, session_regime, fwd_return_5, fwd_return_10, fwd_return_20`. Forward returns computed offline by joining on bar_index.

3. **Session regime column is mandatory from day one.** Label each row from OTFStateFilter (0=ranging, 1=trending, 2=transitional). Do not add later — retrofitting unlabeled rows defeats the purpose.

4. **Duplicate write prevention.** `sc.GetPersistentInt(10)` = last written bar index. On each call: if `sc.ArraySize - 1 <= storedBarIndex`, skip. On write: update storedBarIndex.

*Validation gate:* Per-trigger, per-regime hit rate at N=10 computed once n≥40 occurrences. Threshold: >55% directional accuracy. Triggers that clear the gate in at least one regime advance to Step 2. Triggers that don't: stay in logging mode. If no trigger clears the gate: stop, do not proceed, diagnose the trigger formulas.

Done when: study emits rows in live session, no duplicates on Ctrl+Insert recalc, no rows on intrabar ticks, regime column populated.

---

**Step 2: Idea 4 — EWMA threshold calibration**

*Build during Idea 3 logging period. Activate per-trigger after validation gate.*

Critical design decisions:

1. **EWMA buffer in 4-6 persistent floats per trigger.** Store: running EWMA of signal magnitude, EWMA of direction rate, session-open snapshot for cold-start detection. Use same alpha as FlowConviction — document in file header.

2. **Cold-start handling.** Flag first K bars as warm-up where K = ceil(3 / (1 − alpha)). During warm-up, fall back to static baseline from Idea 3 hit-rate table. Do not use EWMA-derived thresholds during cold-start.

3. **Session boundary persistence.** Write end-of-session EWMA values to sidecar CSV keyed by symbol + trigger ID (BigTradesTape pattern). On session open, load these values as starting point instead of zero.

4. **Activation is per-trigger, not system-wide.** Validated triggers get EWMA buffer activated. Triggers still in logging mode use static baseline. Study must support mixed-activation state simultaneously.

5. **Residual session recalc guard.** Store last-written bar index in a dedicated persistent int. On partial recalc (UpdateStartIndex > 0), verify current bar index > stored value before buffer update.

Done when: EWMA subgraph for at least one validated trigger tracks signal direction across session restart without resetting to zero, threshold levels update intraday without requiring recalc.

---

**What not to build yet**

Ideas 1, 2, 5, and 6 are deferred until Idea 3's validation gate produces a filtered trigger set with known per-regime hit rates. Cross-TF cascade scoring (Idea 1) is architecturally blocked until Idea 3 completes — it requires knowing which individual triggers are valid. Coiling state (Idea 5) and velocity (Idea 2) can be reconsidered once you know whether the existing trigger set has signal at all.
