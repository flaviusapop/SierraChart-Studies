# Stochastic Multi-Agent Consensus Report
## ACSIL Coiling State + Trigger Velocity — Architecture Design

**Agents:** 4 (Neutral, Risk-Averse, First-Principles, Systems Thinker)  
**Date:** 2026-06-18  
**Problem:** How to architect an ACSIL C++ study implementing Coiling State detection and Trigger Velocity (first-derivative entry filter), with all active triggers potentially exposed as individual subgraphs with per-trigger settings.

---

## Consensus (4/4 or 3/4 agreement)

### Architecture: Option C — Three local studies + one aggregator
**4/4 agents** for Option C (risk-averse agent chose B; 3/4 for C, 1/4 for B).

Each chart type (Range Bar, Renko 6t, Renko 8t) gets its own local coiling/velocity study that runs on that chart's bar close rhythm. A fourth aggregator study on the Range Bar chart reads pre-computed proximity and velocity outputs from the local studies via `GetStudyArrayFromChartUsingID`. The aggregator never reads raw delta or BB values cross-chart — it reads only two exported scalars per local study (coiling_count + coil_intensity).

**Why not A:** A single study on Range Bar chart reads Renko mid-brick at every Range bar close — the cross-chart raw values are time-misaligned. Velocity computation (`prox[i] - prox[i-1]`) becomes undefined when the underlying bar index doesn't advance uniformly.  
**Why not B:** Three isolated studies produce no cross-chart coiling composite. The primary coiling use case is "Range Bar AND Renko 6t simultaneously near threshold." Option B defers the aggregation problem without solving it.

---

### Proximity Formulas — Per Trigger Family

#### Delta Slingshot (Range #13/14 + Renko 6t #27-30) — **4/4 agreement, high confidence**
```
proximity_long  = max(0, delta / BB_upper)    // clamp [0, 1]
proximity_short = max(0, delta / BB_lower)    // BB_lower is negative; ratio positive when delta < 0
```
Guard: `if (abs(BB_upper) < MIN_BB_THRESHOLD) proximity = 0.0f` — BB near-zero = data edge case, not coiling.  
This is the best-defined proximity in the system. The trigger fires exactly when this ratio crosses 1.0.

#### NYSE Tick (Range #17/18) — **4/4 agreement, high confidence**
```
proximity = abs(ID47.SG4) / 200.0f    // clamp [0, 1]
```
Guard: `if (ID47.SG4 == 0.0f) proximity = 0.0f` — zero Tick = data gap/outside session, not coiling.

#### Delta Rise/Drop (Range #3/4) — **4/4 agreement, high confidence**
```
proximity = consecutive_bar_count / 4.0f    // steps: 0.25, 0.50, 0.75, 1.00
```
Track a persistent bar counter that increments when the consecutive delta condition holds and resets to zero when it breaks. Pre-signal window is only bar 3 (proximity = 0.75). This is a step function, not a smooth ratio.

#### Renko 8t Long/Short (#33/34) — **3/4 agreement (divergence on aggregation method)**
```
// Count satisfied sub-conditions out of 5:
//   MACD, SMI structure, EMA50/200, ADX>20, VPOC
proximity_count = sub_conditions_met / 5.0f    // [0.0 to 1.0]
```
SMI gate (ID39.SG1 < -60 for long / >+60 for short) acts as a hard veto: if gate fails, cap proximity at 0.5 regardless of other conditions.

**Divergence (Agent 4 only):** Use minimum-of-components ("weakest link") instead of average. Rationale: `MACD=95%, ADX=40%` → average says 67% coiling, but the trigger won't fire until all conditions pass. Minimum says 40%, which is more honest about readiness. This is the superior interpretation for hard-AND triggers, though harder to visually communicate. Worth considering for display: show both count/5 and the weakest component.

#### Delta Trap (Range #19/20) — **3/4 agreement, medium confidence**
Three components with mixed types:
```
comp1 = bars_of_pattern_completed / 3.0f    // step counter (0, 0.33, 0.67, 1.0)
comp2 = (ID4.SG4[-1] + ID4.SG4[current]) / abs(ID4.SG4[-2])    // cumulative delta recovery ratio, clamp [0,1]
comp3 = va_direction_aligned ? 1.0f : 0.0f    // VA direction from ID55 — hard gate
proximity = comp3 * (0.25*comp1 + 0.75*comp2)    // VA gate is multiplicative; delta recovery dominates
```
The VA direction gate zeroes the proximity if misaligned — prevents coiling signals that can't fire.

**Important:** Do NOT replicate the Delta Trap pattern logic independently. Instead, **expose an intermediate subgraph from the existing trigger study** (e.g., `DRAWSTYLE_IGNORE` SG for `pattern_bars_completed`). Have the coiling study read it via `GetStudyArrayFromChartUsingID`. This converts a logic-duplication maintenance problem into a data-dependency problem — when the trigger study's pattern logic changes, the coiling study automatically tracks it.

#### POCL/POCS (Range #21/22) — **3/4 agreement, medium confidence**
```
vpoc_stability = (ID22.SG1[i] == ID22.SG1[i-1]) ? 0.5f : 0.0f    // 1-bar partial, 2-bar = full
vpoc_stability += (ID22.SG1[i-1] == ID22.SG1[i-2]) ? 0.5f : 0.0f
delta_prox = abs(ID4.SG4[i]) / abs(applicable_BB_band[i])    // same as slingshot
va_gate = va_direction_aligned_from_ID55 ? 1.0f : 0.0f
proximity = va_gate * (0.4f * vpoc_stability + 0.6f * delta_prox)
```
VA direction from ID55 is a hard gate (multiplicative). VPOC stability contributes in halves (1 stable bar = 0.5, 2 stable bars = 1.0).

#### POC Wave (Range #39/40) — **3/4 recommend limited treatment**
Best proxy: VPOC oscillation extent = `abs(ID37.SG1[current] - ID37.SG1[pattern_start]) / rolling_50bar_max_VPOC_move`.

**Agent 2 (risk-averse) says: skip a continuous proximity for POC Wave.** The primary condition is a 3-bar OHLC pattern (binary match) with VPOC oscillation as confirmation. Proximity for an OHLC pattern string-match is not meaningful — a bar that doesn't match is not "67% of the way to matching." Use `bars_completed / 3` as a coarse counter only, or contribute to coiling count as a binary flag rather than a continuous ratio.

---

### Velocity Computation — **3/4 consensus on scope, 4/4 on method**

**Velocity is meaningful for:**
- Delta Slingshot / Renko 6t OF (all agents agree — primary velocity signal)
- NYSE Tick (all agents agree)
- Renko 8t sub-condition count (3/4 agents, low confidence)

**Velocity is NOT meaningful for (3/4 agreement):**
- Delta Trap — step function, velocity is just "+1 when pattern advances"
- POC Wave — same
- Delta Rise/Drop — discrete counter, velocity is always 0.25 or reset; this is not a signal

**Formula:**
```cpp
velocity = proximity[i] - proximity[i-1]    // 1-bar difference, normalized [−1, +1]
```
Store `proximity[i-1]` in `sc.GetPersistentFloat(N)`. Use 1-bar difference, not 2 or 3 — smoothing adds lag that defeats the early-warning purpose.

**Critical unique insight (Agent 3 first-principles, Agent 4 systems thinker):**  
The proximity ratio `delta / BB_upper` can increase NOT because delta is surging, but because BB_upper is **compressing** (narrowing). This occurs during low-volatility conditions. Velocity would then show a strong "approaching threshold" signal at exactly the moment the market is quietest — a false acceleration. Mitigation: when you compute velocity, also track `BB_width = abs(ID5.SG1 - ID5.SG3)` and suppress velocity display when BB_width drops below its 20th percentile over the last 50 bars.

**Compute velocity in local studies, not in the aggregator.** Renko studies close bricks at different times than Range bars. If the aggregator computes `prox[i] - prox[i-1]` using Range Bar indices on values that only change at Renko bar closes, velocity appears as 0 for several bars then spikes — sampling aliasing, not signal. Compute velocity inside each local study using its own bar index. Expose the pre-computed velocity subgraph. The aggregator reads it.

---

### Composite Coiling State — **Divergence: two schools, no clear consensus**

**School 1 (Agents 1 + 2):** Single composite score with fire condition.  
`coiling_score = Σ(proximity_i × weight_i) for triggers where prox_i ≥ 0.80`  
Fire when score ≥ threshold AND at least one trigger ≥ 0.90. Include 2-bar hysteresis.

**School 2 (Agents 3 + 4 — recommend):** Two separate outputs.
- `coiling_count` — integer count of triggers in [0.80, 1.00) zone, per chart type
- `lead_proximity` — highest proximity within the Delta Slingshot family (Range + Renko 6t), since these share the same formula and are directly comparable

**Rationale for School 2 (compelling):** Aggregating step-function proximity (Delta Trap, POC Wave, Delta Rise/Drop) with continuous-ratio proximity (Slingshot, NYSE Tick) produces a number without a clean semantic interpretation. Pattern-based triggers spend more bars in partial states, systematically inflating the count vs. the cleaner threshold-based triggers. Separating them gives the trader two actionable questions: "How many triggers are coiling?" (breadth) and "How close is the primary delta signal?" (intensity).

**Recommended approach:** Expose both. `coiling_count` as a bar histogram (by chart type: Range / Renko 6t / Renko 8t in different colors). `lead_proximity` as a line. Let the trader read confluence from both.

---

## Divergences (split 2/2 or unique to 1 agent)

### Architecture — Option B vs Option C (1/4 vs 3/4)
Risk-averse agent chose Option B to avoid the aggregator's extra bar-latency hop and ID-dependency chain. The 3-agent majority chose C because Option B cannot produce cross-chart confluence without effectively becoming C. If implementation complexity is the concern, start with Option B and add the aggregator only after the local studies are stable.

### Renko 8t proximity — average vs minimum-of-components (1/4 vs 3/4)
Agent 4 (systems thinker) argues for minimum/weakest-link, which better reflects the AND-gate structure of trigger #33/34. The majority used count/N for simplicity. Consider: expose both `count_prox` (all agents) and `min_prox` (weakest component name in a label) so the trader can see which component is the bottleneck.

### Pattern-based trigger treatment (2/4 recommend skipping continuous proximity)
Agents 2 and 4 say: treat Delta Trap, POC Wave, Delta Rise/Drop as binary coiling-count contributors only — don't compute continuous proximity ratios for them. Agents 1 and 3 compute ratios but acknowledge they are step functions. The 2/2 split is genuine — the step-function ratios have limited information value beyond what the binary flag provides.

---

## Key Outliers (unique high-value insights)

### Agent 4 (Systems Thinker): OrderflowSignalV3 behavioral interaction risk
The most important non-implementation risk. If the trader uses coiling state to pre-position before OrderflowSignalV3 fires, they are trading on an unvalidated entry signal. The selection bias in memory (remembering wins where coiling preceded a confirmed signal) will lead to over-weighting coiling state over time, effectively bypassing the validated OrderflowSignalV3 system. **Mitigation:** use distinct alert sounds for coiling vs. confirmed entry. Treat coiling as "readiness" (order size, focus), never as a standalone entry trigger.

### Agent 3 (First-Principles) + Agent 4: BB compression confounds velocity
Fast-market conditions cause BB compression (delta BB narrows). The proximity ratio surges toward 1.0 without delta actually moving faster. Velocity filter then fires during cooling markets — the opposite of intent. Track BB_width separately; suppress velocity when BB is in a compression regime (below 20th percentile of recent width).

### Agent 2 (Risk-Averse): Expose intermediate subgraphs from trigger studies
Instead of replicating Delta Trap / POCL/POCS pattern logic in the coiling study, expose a `DRAWSTYLE_IGNORE` subgraph from the existing trigger study that publishes the pattern_bars_completed counter. The coiling study reads it via `GetStudyArrayFromChartUsingID`. This eliminates the highest-probability maintenance divergence: when trigger logic changes, coiling proximity tracks it automatically.

### Agent 4: Heartbeat subgraph for stale-data detection
Each local study should expose a subgraph with value `sc.ArraySize % 10000` (or a timestamp fraction). The aggregator monitors this value; if it doesn't change for N consecutive Range Bar closes, discount or zero that chart's contribution to the aggregate. Renko 8t can go 5–15 minutes without a new brick in low-volatility conditions. Without this check, stale proximity values appear as sustained coiling when the market has actually cooled.

---

## Top 3 Implementation Risks (near-unanimous)

### Risk 1: BB near-zero division — silent NaN propagation *(all 4 agents)*
`proximity = delta / BB_upper` where BB_upper approaches zero at session start or in dead-flat markets. NaN propagates through composite score and fires false coiling alerts.

```cpp
float bb_upper = ID5_SG1[i];
if (fabsf(bb_upper) < 5.0f) {    // tune 5.0f to instrument's typical delta range
    prox[i] = 0.0f;
    continue;
}
prox[i] = delta[i] / bb_upper;
if (prox[i] < 0.0f) prox[i] = 0.0f;
if (prox[i] > 1.0f) prox[i] = 1.0f;
```
Apply this guard at **every** division site. A single unguarded division in the velocity normalization path will corrupt the whole composite silently.

### Risk 2: Cross-chart array staleness — no error return from GetStudyArrayFromChartUsingID *(all 4 agents)*
When a source study hasn't loaded yet (full recalc, SC restart, study removed/re-added), `GetStudyArrayFromChartUsingID` returns zeros or a stale array — there is no error code. The coiling score silently collapses to zero for all cross-chart triggers.

```cpp
SCFloatArray renkoArr;
sc.GetStudyArrayFromChartUsingID(chartB, studyID, sgIndex, renkoArr);
if (renkoArr.GetArraySize() == 0) {
    sc.AddMessageToLog("CoilingState: Renko 6t source not ready", 1);
    return;  // or continue with zero contribution
}
```
Add a diagnostic log on the first bar of each session that counts successfully non-empty arrays. If any are zero, IDs have drifted.

### Risk 3: Sequential pattern state corruption on full recalc *(Agents 1 + 2)*
Delta Trap and Delta Rise/Drop use persistent int counters. On full recalc, `sc.UpdateStartIndex` is 0 but the persistent int holds end-of-prior-session state. The counter carries the wrong value for the first bars of the recalc window.

Fix: when `sc.UpdateStartIndex == 0`, reset all pattern counters to zero at bar 0 before beginning the loop. The loop re-derives them correctly from bar 0 forward.

```cpp
if (sc.UpdateStartIndex == 0) {
    sc.GetPersistentInt(STREAK_SLOT) = 0;
    sc.GetPersistentInt(PATTERN_SLOT) = 0;
}
```

---

## Subgraph Layout Summary

**Total: ~36 subgraphs across all 4 studies — well within 64-per-study limit.**

### Local Study A (Range Bar) — ~20 SGs
| SG | Name | Default Style |
|----|------|--------------|
| 0 | CoilScore_Range | BAR (panel) |
| 1 | CoilAlert | POINT |
| 2 | Prox_Slingshot_L | IGNORE |
| 3 | Prox_Slingshot_S | IGNORE |
| 4 | Prox_DeltaTrap_L | IGNORE |
| 5 | Prox_DeltaTrap_S | IGNORE |
| 6 | Prox_POCL | IGNORE |
| 7 | Prox_POCS | IGNORE |
| 8 | Prox_POCWave_L | IGNORE |
| 9 | Prox_POCWave_S | IGNORE |
| 10 | Prox_DeltaRise | IGNORE |
| 11 | Prox_DeltaDrop | IGNORE |
| 12 | Prox_Tick | IGNORE |
| 13 | Vel_Slingshot | BAR (panel) |
| 14 | Vel_Tick | IGNORE |
| 15 | Heartbeat | IGNORE |
| 16–19 | (reserved) | IGNORE |

### Local Study B (Renko 6t) — ~8 SGs
| SG | Name | Default Style |
|----|------|--------------|
| 0 | CoilScore_Renko6t | BAR |
| 1 | Prox_OF_L | IGNORE |
| 2 | Prox_OF_S | IGNORE |
| 3 | Vel_OF | BAR |
| 4 | Heartbeat | IGNORE |
| 5–7 | (reserved) | IGNORE |

### Local Study C (Renko 8t) — ~6 SGs
| SG | Name | Default Style |
|----|------|--------------|
| 0 | CoilScore_Renko8t | BAR |
| 1 | Prox_Long | IGNORE |
| 2 | Prox_Short | IGNORE |
| 3 | Vel_CondCount | IGNORE |
| 4 | Heartbeat | IGNORE |
| 5 | (reserved) | IGNORE |

### Aggregator Study (Range Bar chart) — ~10 SGs
| SG | Name | Default Style |
|----|------|--------------|
| 0 | CoilCount_Total | BAR (panel) |
| 1 | LeadProximity | LINE |
| 2 | LeadVelocity | BAR (panel) |
| 3 | CoilCount_Range | IGNORE |
| 4 | CoilCount_Renko6t | IGNORE |
| 5 | CoilCount_Renko8t | IGNORE |
| 6 | CoilAlert_Master | POINT |
| 7–9 | (reserved) | IGNORE |

---

## Next Steps
1. Run `/reverse-prompting` to surface assumptions before locking the spec
2. Run `/prompt-contracts` to formalize success criteria, constraints, and failure conditions
3. Implementation sequence (from prior roadmap): Step 0 (intrabar audit) → Step 1 (baseline CSV capture of proximity values) → Step 2 (local study A, slingshot + NYSE Tick only) → Step 3 (add aggregator) → Step 4 (pattern-based triggers)
