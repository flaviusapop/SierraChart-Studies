# EffortVsResult.cpp — Independent Review v1.0

Scope: `TASK_EFFORT_VS_RESULT.md` (66 lines) + `EffortVsResult_BuildSpec.md` v1.0
(201 lines) + `EffortVsResult.cpp` (768 lines, commit `d11c342`).
Method: full source read, gate run, Python replica sign-check, pattern diff
against `TrappedTraders.cpp` / house conventions. No implementation file was
edited. No Sierra compiler is available on Linux; everything below is static.

## Static gate

```
PASS exactly one SCSFExport
PASS braces balanced
PASS no static/thread_local locals
PASS no stray '#' comments
PASS no float == on price arrays
PASS no sc.Stochastic(
PASS all inputs defaulted
PASS all inputs read in logic
PASS no duplicate input slots
PASS S-> fields all in struct
PASS SG max <= 58
PASS persist-ptr paired w/ LastCallToFunction

12/12 checks pass | 768 lines
```

Gate command: `python3
/home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/scripts/gate_check.py
EffortVsResult.cpp` (run from the worktree root). Green, but the gate is
shallow — it does not check signs, lookahead, determinism, stale `DataColor`,
or alert semantics. The findings below are what the gate cannot catch.

## Independent formula sign-check (replica, not just re-reading)

Re-implemented §2.4 from the spec in Python and compared against the C++
at `EffortVsResult.cpp:590-600`:

| Case | Effort | ResultNorm | SignedReward | FailureMag | SignedFailure | Verdict |
|---|---|---|---|---|---|---|
| +500 delta / scale 200, result −0.5 / TR 2 | +2.500 | −0.250 | −0.250 | 1.125 | **−1.125 (magenta)** | correct: buyer fail negative |
| −500 delta / scale 200, result +0.5 / TR 2 | −2.500 | +0.250 | −0.250 | 1.125 | **+1.125 (cyan)** | correct: seller fail positive |
| +500, strong up (+4/TR 2) | +2.5 | +2.0 | +2.0 | 0 | 0 | correct: rewarded → zero |
| −500, strong down (−4/TR 2) | −2.5 | −2.0 | +2.0 | 0 | 0 | correct: rewarded → zero |
| weak effort 100/scale 200 | +0.5 | −1.0 | −1.0 | 0 | 0 | correct: below 1.25 gate |
| boundary reward == stall 0.20 | +2.5 | +0.2 | +0.2 | 0 | 0 | correct: `gap > 0` strict |

The C++ matches the replica exactly: `sReward = eSign * resultNorm`
(`:591-592`), `gap = stall - sReward; if (gap > 0) failMag = |effort|*gap`
(`:596-598`), `sFail = -eSign * failMag` (`:600`). Sign convention holds.
`EVR_Sign` (`:116-121`) uses strict `>`/`<` with no float equality. Good.

## Acceptance-criteria disposition

| Criterion (task) | Status |
|---|---|
| Identity: file, `SCDLLName("EffortVsResult")` (`:83`), `scsf_EffortVsResult` (`:159`), GraphName (`:203`), `AutoLoop=0` (`:208`), `GraphRegion=1` (`:209`), `MaintainVolumeAtPriceData=1` (`:211`), no Numbers Bars dep | PASS |
| BarDelta int64-safe across bar's VAP levels (`:141-154`) | PASS (see LOW-3 on `float` cast) |
| EffortScale EWMA(50) no mean subtraction, 1.0 floor (`:529-536`) | PASS, house convention honored |
| Effort cap 5.0, 0 disables (`:540-546`) | PASS (see MEDIUM-4 interaction) |
| ResultRaw 3 modes (`:549-564`), EWMA TR(50) TickSize floor (`:566-588`) | PASS |
| SignedReward / FailureMagnitude gate 1.25 / Stall 0.20 (`:591-600`) | PASS per sign table above |
| SignedFailure polarity + EMA(3) raw+smoothed exposed (`:603-607`, SG5/SG6) | PASS |
| Confirmation channel on i+1, Fav 2 / Adv 2, no future leak (`:640-689`) | PASS on timing, HIGH-1 on pulse *value* |
| Histogram cyan-above / magenta-below + DataColor intensity (`:617-624`) | PASS (see MEDIUM-1 on stale colors) |
| Rewarded subgraph off by default (`:338-339`, `:631-636`) | PASS |
| Price arrows at offsets, no rectangles/UseTool | PASS — no `UseTool`/drawing API anywhere; drawing-namespace N/A |
| "failed aggression evidence" wording (`:747-748`, `:760-761`, header `:22-24`) | PASS |
| 20 inputs, all defaulted + read (gate confirms), fingerprint covers structural 0-12 (`:443-460`) | PASS (see LOW-1 on packing resolution) |
| 13 SGs < 60, documented (`:52-65`, `:213-289`) | PASS |
| Closed bars only, forming zeroed never signal (`:479`, `:693-710`) | PASS |
| Backfill gap loop in order (`:518-520`) | PASS (see MEDIUM-2 on mid-history corrections) |
| No STL in persistent state, no price `==`, no `#` comments | PASS |
| Alert watermark post-loop, anchored to current bar (`:719-767`) | PASS on pattern (see MEDIUM-3 notes) |
| VAP-null zeros, no fallback (`:417-437`) | PASS on values (see MEDIUM-5 on cost) |
| Ready flag + confirmation gating (`:638`, `:644-645`) | PASS |

**Missing/stubbed features: none. Zero BLOCKERs.** Every requested input,
subgraph, mode, and channel exists and is wired. The rest of this review is
correctness/robustness defects ranked below.

---

## Findings

### HIGH-1 — Confirmation pulses hold the *print-bar* smoothed value, which can carry the opposite sign to the confirmed side — `EffortVsResult.cpp:659, 678`

```cpp
SG_BuyPuls[j] = sfSmooth;   // :659 — sfSmooth is bar j's EMA, not candidate j-1's
SG_SellPul[j] = sfSmooth;   // :678 — same
```

BuildSpec §3 says each pulse "holds the smoothed failure value on its bar",
and the code does exactly that, so this is a spec-literal implementation —
but it is semantically wrong. `sfSmooth[j]` is bar `j`'s own failure EMA. When
bar `j` confirms a buyer failure (candidate `Effort[j-1] > 0` + price fails to
extend + adverse close), bar `j` itself can simultaneously be a strong
*seller*-failure bar (negative effort, poor/up result → positive `sFail[j]`),
flipping `smooth` positive. The buyer pulse then stores a **positive** value
in the magenta/below-zero series, and the alert scan (`:742-753`,
`SG_BuyPuls[b] != 0.0f`) fires a "buyer failed aggression evidence" alert
whose magnitude/sign belongs to sellers. Mirror case for seller pulses.
Magnitude is likewise decoupled: a valid confirmation can print a near-zero
residue when bar `j`'s own `sFail` is 0, understating a large candidate
failure.

Concrete fix (pick one, update BuildSpec §3 accordingly):

```cpp
// Option A (recommended): hold the CANDIDATE's smoothed failure.
SG_BuyPuls[j] = SG_SFailS[j - 1];   // candidate bar's EMA value (negative)
// and
SG_SellPul[j] = SG_SFailS[j - 1];   // candidate bar's EMA value (positive)
```

Option A keeps sign consistent with the confirmed side by construction
(candidate effort sign determines which branch fires, and the candidate's own
`sFail`/`smooth` shares that sign or is zero). Alternatively hold the
candidate raw failure `SG_SFail[j-1]`, or `min`/`max` the two (e.g. buyer
pulse = `fminf(sfSmoothCandidate, sfSmoothCurrent)` clamped ≤ 0). At minimum,
skip the pulse when the held value has the wrong sign or is exactly 0:

```cpp
float v = (float)pS->smooth; // or SG_SFailS[j-1]
if (v < 0.0f) { SG_BuyPuls[j] = v; ... }  // buyer branch only publishes negative
```

Also gate the alert scan on sign (`SG_BuyPuls[b] < 0`, `SG_SellPul[b] > 0`)
instead of `!= 0.0f` so a future sign slip can never alert.

### MEDIUM-1 — Stale `DataColor` on full recalc for pulses, arrows, rewarded — `:626-636, :659-665, :679-685`

On the confirm-false and `showReward==0`/unrewarded paths the code zeroes the
*value* but never resets the *color*:

- `:626-629` zeroes pulse/arrow values; `DataColor` for SG 7-10 is only
  written inside the confirm branches (`:660`, `:665`, `:679`, `:684`).
- `:634` writes `SG_RewAgg[j]`; `DataColor` (`:636`) is only written when
  `rewarded != 0.0f`.

After a settings change (e.g. widen `favTicks` so a prior confirmation
vanishes, or toggle `Show Price Arrows` / `Show Rewarded Aggression` — both
correctly in the fingerprint), a full rebuild leaves the old `DataColor` on
bars whose value is now 0. Hidden today by `DrawZeros=0`, but the stale color
reappears the moment a value is re-set without a color write, and it violates
the "zero stale outputs on recalc" criterion for the color channel.

Concrete fix — clear colors alongside values in the loop and forming-bar
block:

```cpp
SG_BuyPuls[j] = 0.0f; SG_BuyPuls.DataColor[j] = buyArrowC;   // or a neutral
SG_SellPul[j] = 0.0f; SG_SellPul.DataColor[j] = sellArrowC;
SG_BuyArr[j]  = 0.0f; SG_BuyArr.DataColor[j]  = buyArrowC;
SG_SellArr[j] = 0.0f; SG_SellArr.DataColor[j] = sellArrowC;
SG_RewAgg[j]  = rewarded;
SG_RewAgg.DataColor[j] = (rewarded != 0.0f) ? rewardC : EVR_GRAY;
```

Same for the `forming` block (`:704-708`).

Related cosmetic staleness (same fix family): display-only color inputs are
correctly excluded from the fingerprint per house convention, but unlike
`TrappedTraders` (which redraws every drawing each pass, so colors apply
immediately), the incremental path here never rewrites history `DataColor`.
Changing Buyer/Seller Failure Color therefore leaves history in the old color
until Ctrl+Insert. Acceptable per convention, but document it in the spec
("color changes apply to new bars immediately, history on next full
recalculate") or rewrite SG6 `DataColor` for all bars when only colors
changed.

### MEDIUM-2 — Mid-history data corrections are silently ignored — `:505-518`

`isFullRecalc` is true only for `UpdateStartIndex == 0`,
`IsFullRecalculation`, settings change, null state, or array shrink
(`:473-481`). A data correction that SC reports as `UpdateStartIndex = k > 0`
with `k <= pS->lastProcessed` (corrected bar already processed) takes the
incremental path with `loopStart = lastProcessed + 1 > k`, so the corrected
bar — and every EWMA value downstream of it — stays wrong forever. History
then diverges from a fresh full recalc, breaking the "historical full
recalculation equals sequential live results" requirement for the correction
case. (`TrappedTraders.cpp:337` shares this limitation; do not copy it.)

Concrete fix (cheap for a 13-SG study — a correction triggers one full
rebuild, identical arithmetic to live):

```cpp
if (!isFullRecalc && pS != nullptr
    && sc.UpdateStartIndex > 0
    && sc.UpdateStartIndex <= pS->lastProcessed)
    isFullRecalc = true;
```

Normal new-bar calls have `UpdateStartIndex` at/above the new bar
(`> lastProcessed`), and intrabar ticks return early anyway, so this does not
cause rebuild loops. Re-check ordering: the rewind test must run *before*
the `isFullRecalc` branch that reallocates state (`:483-497`).

### MEDIUM-3 — Alert watermark notes: `== 0` sentinel + scan-floor jump — `:722-741`

1. `if (isFullRecalc || alertedBuy == 0 || alertedSell == 0)` (`:722`):
   persistent ints zero-initialize, so `0` doubles as "never set" and as valid
   bar index 0. Harmless today only because a pulse on bar 0 is impossible
   (`j >= 1` at `:641`). If the confirmation rule ever changes, the sentinel
   collides with a real watermark. Use `-1` explicitly: after the full-recalc
   branch, watermarks are always `>= 0` or `-1`; test `<= -1`… actually
   simplest is to drop the `== 0` clause entirely — the first call always takes
   the `pS == nullptr → isFullRecalc` path, which fast-forwards watermarks
   anyway. The clause only fires for pre-upgrade persisted state; keep it but
   compare `< 0` after initializing to `-1` on first run.
2. `startB = max(alertedBuy + 1, scanFloor)` with scan-forward (`:741-753`):
   if a confirmation sits between `scanFloor` and `alertedBuy` it is skipped
   by design (already alerted or outside window) — correct. But when alerts are
   disabled and re-enabled (`canAlert` false path `:730-735`), watermarks jump
   to `lastClosed`, silently swallowing confirmations that printed while
   disabled. That matches "never alert historical" but means toggling alerts on
   never catches up the last 10 bars. Document the behavior in the spec; if
   catch-up is desired, only advance watermarks to `lastClosed` on full
   recalc, and on `!canAlert` leave them untouched.
3. `sc.SetAlert(alertSound, forming, …)` (`:749`, `:763`): anchored to the
   forming bar per house pattern — correct. Keep.

No change required for correctness of the "never alert historical" guarantee:
full recalc fast-forwards with no alerts (`:722-727`). The items above are
robustness/documentation.

### MEDIUM-4 — `MaxNorm < MinEffort` silently kills all failure + confirmation output — `:383-389, :594-599, :645`

Both the failure gate (`:594`, `fabsf(effort) >= minEffort`) and the candidate
gate (`:645`) test the *capped* effort. With e.g. `MaxNorm = 1.0` and
`MinEffort = 1.25`, `|effort| ≤ 1.0 < 1.25` always → `failMag` identically 0,
pulses/arrows never fire, histogram flat — with no warning. The UI allows the
combo (`MaxNorm` 0..20, `MinEffort` 0..20). Spec-mandated (cap applies before
gate), so not a code bug per se, but a footgun.

Concrete fix — clamp in the input-read block and note it:

```cpp
float maxNorm = max(0.0f, In_MaxNorm.GetFloat());
if (maxNorm > 0.0f && maxNorm < minEffort)
    maxNorm = minEffort;   // cap must not strangle the effort gate
```

or `SetFloatLimits` floor coupling + a BuildSpec note. At minimum document the
interaction in `EffortVsResult_BuildSpec.md` §2.2.

### MEDIUM-5 — VAP-null path does O(bars) work on every intrabar tick — `:417-437`

The null guard sits *before* the intrabar guard (`:505-509`). On a chart with
VAP unavailable (wrong bar type / study on a chart without footprint support),
every tick rewrites 14 arrays × `ArraySize` bars. On a 50k-bar chart that is a
per-tick drag for output that is already zero.

Concrete fix — move the intrabar early-out above the VAP branch, or scope the
zeroing to full recalc / new bars:

```cpp
// before the VAP guard:
if (!isFullRecalc /* computed from fingerprint first */
    && sc.ArraySize == sc.GetPersistentInt(EVR_INT_BARS))
    return;
```

Requires computing the fingerprint before the VAP branch (it already does not
depend on VAP). Alternatively, track a `vapNulled` persistent flag and only
zero newly added bars. Either preserves the "zeros, never stale" guarantee.

### LOW-1 — Fingerprint float resolution coarser than the UI step — `:452-457`

`minEffort`/`stall` pack at ×1000 (0.001), `maxNorm` at ×100 (0.01). A change
smaller than the quantum (e.g. `MaxNorm` 5.000 → 5.005 via typing) keeps the
same fingerprint → no rebuild → stale outputs until manual recalc. Raise
`maxNorm` to ×1000 for consistency, and note the quantum in the spec. Unsigned
wrap arithmetic itself is fine for a fingerprint.

### LOW-2 — Pulse/alert presence tests use exact `!= 0.0f` on floats — `:744, :758`

Values compared are assigned `0.0f` or an EMA result, so exact comparison is
*functionally* correct here (unlike price `==`, which the gate already
excludes). Still fragile next to HIGH-1: an EMA residue of `1e-9` counts as a
confirmation. Prefer sign tests (`< 0` buyer / `> 0` seller) plus a small
epsilon, e.g. `fabsf(SG_BuyPuls[b]) > 1e-6f`, combined with the HIGH-1 sign
fix.

### LOW-3 — `BarDelta` int64 sum narrowed to `float` — `:153, :526-527`

`deltaSum` is correctly accumulated in `int64_t` (`:146`), then `(float)deltaSum`
(`:153`). Beyond 2²⁴ (~16.7M contracts net per bar) the float loses integer
precision. ES footprint bars never approach that; the EWMA/scale math is float
throughout by design. No action needed, but a one-word comment stating the
assumption (or returning `double` and narrowing at the SG write) would stop
the next reader from "fixing" it into something slower.

### LOW-4 — Tiny-chart churn: `lastClosed < 0` returns before updating `EVR_INT_BARS` — `:511-512, :713`

With `ArraySize < 2` the function returns after allocating state but before
`sc.SetPersistentInt(EVR_INT_BARS, …)`. The next tick re-enters with
`isFullRecalc` possibly true again (if `UpdateStartIndex == 0`) and rebuilds
trivially. Correct, just marginally chatty on empty charts. No fix needed;
noted for completeness.

### LOW-5 — Header/doc nits

- Header (`:67-72`) and spec document ptr slot 10 as "outside the int-slot
  convention range to avoid collision". Verified safe: `BigTradesTape.cpp`
  shares index 1 across int/pointer namespaces without effect, and slots 1-6
  vs 10 do not overlap under either namespace model. The comment is accurate;
  keep it.
- `SG_SFailS.DrawZeros = 1` (`:253`) draws the zero baseline of the main
  histogram while `SG_SFail.DrawZeros = 0` hides raw zeros — intentional and
  fine, but one line in the spec (§6) should say so.
- `In_AlertSound.GetInt()` (`:402`) for a `SetAlertSoundNumber` input matches
  `ReconTape.cpp:843` precedent. Fine.

## VAP iteration — verified against TrappedTraders

`EVR_BarDelta` (`:141-154`): `priceInTicks = INT_MIN`, loop
`GetNextHigherVAPElement((unsigned int)barIndex, priceInTicks, &pVAP)`,
`nullptr` skip, `(int64_t)Ask - (int64_t)Bid` accumulation. Identical to
`TrappedTraders.cpp:408-416` including the cast order. Per-bar (not windowed)
is correct here — the task explicitly wants per-chart-bar sums, and summing
the bar's own VAP levels equals the bar's delta by construction. Null-VAP
bars yield 0 (`deltaSum` stays 0), outer `VolumeAtPriceForBars == nullptr`
guard zeroes everything. No fallback source — as specified.

## Closed-bar / lookahead / replay — verified

- `lastClosed = ArraySize - 2` (`:479`); forming bar (`:693-710`) zeroed
  including `DataColor`, never read as input. Confirmation at `j` reads at
  most `j` and `j-1` (`:643-657`, `:672-676`) and prints on `j` — one-bar
  delay with no future leak. EWMA/EMA updates use only bar `≤ j`. No
  lookahead found.
- Incremental `loopStart = lastProcessed + 1` (`:518`) replays every missed
  closed bar in order after reconnect/backfill with identical arithmetic to
  full recalc (state carried in `S_EvrState`, seeded identically on bar 0 via
  `:530`/`:580`/`:603`). Shrink/null/settings-change forces rebuild
  (`:480-481`). Subject only to MEDIUM-2 (mid-history corrections).
- Intrabar guard (`:505-509` + `:713`) matches the house pattern with the
  full-recalc bypass. Provisional forming values intentionally omitted —
  permitted by the task ("default off" satisfied by zeros).

## What to fix before Sierra-side validation

1. HIGH-1 (pulse value source + sign-gated alerts). Changes signal semantics.
2. MEDIUM-1 (stale `DataColor` clearing). One-line-per-SG fix.
3. MEDIUM-2 (correction rewind). Three-line fix, prevents history/live
   divergence on data corrections.
4. MEDIUM-4 (MaxNorm/MinEffort clamp or doc). Prevents silent-dead study.
5. MEDIUM-5 (VAP-null guard ordering). Perf on footprint-less charts.
6. Document: color-change history behavior, alerts-disabled watermark jump,
   float packing quantum (LOW-1), `DrawZeros` split (LOW-5).

## Sierra compile / replay checklist (unchanged — no build system on Linux)

1. F5 compile `EffortVsResult.cpp` in Sierra; expect zero errors (no STL in
   state, `max`/`fabsf`/`RGB`/`SCString::Format` per house precedent).
2. Add to a footprint chart (own VAP; no Numbers Bars needed). Ctrl+Insert
   full recalc; confirm 13 SGs, lower region, histogram around zero.
3. Replay: buyer-aggression-no-progress → magenta/below-zero; seller analog →
   cyan/above-zero; rewarded bars → zero failure + (optional) rewarded
   subgraph only. Confirm candidate→confirmation pairs print on `i+1` with
   `Fav/Adv = 2` defaults.
4. Backfill/reconnect test + data-correction test (post MEDIUM-2 fix).
5. Alert test: enable, trigger confirmation, one alert per channel anchored to
   current bar; Ctrl+Insert produces no historical storm.
6. Remove/re-add study after any structural input-order change (saved settings
   index by position).

*Review verdict: no BLOCKERs — spec-complete, gate 12/12, signs verified.
Fix HIGH-1 before calling v1.0 final; MEDIUM-1/2/4/5 before live use.*

---

## Fix disposition (2026-09-03, fixing engineer)

All findings were valid. No BLOCKERs existed. Disposition:

- HIGH-1 FIXED — pulses hold candidate failure (`SG_SFailS[j-1]` → `SG_SFail[j-1]` → current smoothed), sign-gated; arrows independent; alert scan sign-gated with 1e-6 epsilon.
- MEDIUM-1 FIXED — DataColor cleared with values in loop, forming, and VAP-null paths.
- MEDIUM-2 FIXED — correction rewind (`UpdateStartIndex <= lastProcessed` → full rebuild).
- MEDIUM-3 FIXED (robustness/docs) — `== 0` sentinel dropped; disabled-alert watermark advance documented in spec §7.
- MEDIUM-4 FIXED — MaxNorm clamped up to MinEffort + spec note.
- MEDIUM-5 FIXED — fingerprint moved above VAP guard; VAP-null intrabar early-out + BARS update.
- LOW-1 FIXED — maxNorm packing x1000, quantum documented. LOW-2 FIXED via sign-gated scan. LOW-3 documented in code (no-action). LOW-4 no-action (noted). LOW-5 addressed (DrawZeros split + color-history behavior in spec).
- No invalid findings to dispute. No existing studies modified.
