# Prompt Contract — CoilingState.cpp

**Date:** 2026-06-18  
**Derived from:** consensus_coiling_velocity.md + reverse-prompting session  
**Architecture revision:** Self-contained — no GetStudyArrayFromChartUsingID. Delta and BB computed natively from ACSIL built-ins.  
**Scope:** Coiling State study only. TriggerVelocity is a separate contract.

---

## GOAL

A single compilable ACSIL study (`CoilingState.cpp`) that:
1. Computes bar delta directly from `sc.AskVolume[i] - sc.BidVolume[i]` — no dependency on any other study
2. Computes Bollinger Bands on that delta internally using `sc.MovingAverage` + `sc.StdDeviation` with user-configurable length and multiplier
3. Produces a per-bar proximity score `p ∈ [0.0, 1.0]` where `p = delta / BB_upper` (long side) or `p = delta / BB_lower` (short side), measuring how much of the BB band the current bar's delta has consumed
4. Fires a **rising-edge alert** exactly once when proximity first enters the coiling zone `[CoilThreshold, 1.0)` per onset event
5. Loads unchanged on Range Bar, Renko 6t, and Renko 8t charts without any reconfiguration — all inputs are signal parameters, not chart-type selectors

Success metric: zero compiler errors; proximity values fall in [0, 1] on live bars; alert fires once at onset and does not re-fire while sustained; BB near-zero guard verifiable in code review.

---

## CONSTRAINTS

- Single `.cpp` file, single study function `scsf_CoilingState`
- ACSIL API only — no STL containers, no `std::min` / `std::max` (use ternary or manual comparisons)
- No external build system — must compile with F5 inside Sierra Chart
- **No `GetStudyArrayFromChartUsingID`** — all inputs come from native ACSIL arrays (`sc.AskVolume`, `sc.BidVolume`) and internal computation
- Must use the **intrabar guard pattern** from CLAUDE.md: `sc.GetPersistentInt(1)` tracks `lastKnownBars`; skip if `!isFullRecalc && !isNewBar`
- All **structural inputs** (BB length, multiplier, coil threshold, BB guard) included in **settings fingerprint** (slots 4–6 per CLAUDE.md)
- Display-only inputs (colors, point size) NOT in fingerprint
- Alert uses the **standard watermark pattern** (CLAUDE.md): `sc.GetPersistentInt(2/3)` for long/short alert watermarks; alert anchored to `lastBar`, not the historical coiling bar index
- Persistent slots used: 1 (lastKnownBars), 2 (alertedBarLong), 3 (alertedBarShort), 4–6 (fingerprint). Documented at top of file.

---

## FORMAT

### File
`CoilingState.cpp` → `C:\Users\flavi\Documents\Claude\Projects\SierraChart Studies\`

### Inputs (SetDefaults block)

| Index | Name | Type | Default | Structural? |
|-------|------|------|---------|-------------|
| 0 | BB Length | Int | 20 | ✓ |
| 1 | BB Multiplier | Float | 0.9 | ✓ |
| 2 | Coil Threshold (0–1) | Float | 0.80 | ✓ |
| 3 | Min BB Width Guard | Float | 5.0 | ✓ |
| 4 | Alert Sound ID | Int | 2 | ✓ |
| 5 | Long Proximity Color | Color | Green | ✗ |
| 6 | Short Proximity Color | Color | Red | ✗ |

### Subgraphs (SetDefaults block)

| Index | Name | DrawStyle | Panel |
|-------|------|-----------|-------|
| 0 | Delta | DRAWSTYLE_IGNORE | — (internal feed for BB) |
| 1 | BB_Upper | DRAWSTYLE_IGNORE | — |
| 2 | BB_Lower | DRAWSTYLE_IGNORE | — |
| 3 | BB_Middle | DRAWSTYLE_IGNORE | — |
| 4 | ProxLong | DRAWSTYLE_LINE | Separate (0–1 scale) |
| 5 | ProxShort | DRAWSTYLE_LINE | Same as SG4 |
| 6 | CoilAlertLong | DRAWSTYLE_POINT | Main price panel |
| 7 | CoilAlertShort | DRAWSTYLE_POINT | Main price panel |

### Settings fingerprint

```cpp
int fp0 = sc.Input[0].GetInt() ^ ((int)(sc.Input[1].GetFloat() * 1000));
int fp1 = (int)(sc.Input[2].GetFloat() * 1000) ^ (int)(sc.Input[3].GetFloat() * 100);
int fp2 = sc.Input[4].GetInt();
int& storedFp0 = sc.GetPersistentInt(4);
int& storedFp1 = sc.GetPersistentInt(5);
int& storedFp2 = sc.GetPersistentInt(6);
bool settingsChanged = (storedFp0 != fp0 || storedFp1 != fp1 || storedFp2 != fp2);
if (settingsChanged) {
    isFullRecalc = true;
    storedFp0 = fp0; storedFp1 = fp1; storedFp2 = fp2;
}
```

### Core logic structure

```cpp
// 1. Fill delta subgraph from native ACSIL arrays
for (int i = sc.UpdateStartIndex; i < sc.ArraySize; i++) {
    sc.Subgraph[0][i] = sc.AskVolume[i] - sc.BidVolume[i];
}

// 2. Compute BB on delta (SC built-in functions, full array pass)
int bbLength = sc.Input[0].GetInt();
float bbMult  = sc.Input[1].GetFloat();
sc.MovingAverage(sc.Subgraph[0], sc.Subgraph[3], MOVAVGTYPE_SIMPLE, bbLength);  // BB_Middle
sc.StdDeviation(sc.Subgraph[0], sc.Subgraph[1], bbLength);  // repurpose as stddev buffer

for (int i = sc.UpdateStartIndex; i < sc.ArraySize; i++) {
    sc.Subgraph[1][i] = sc.Subgraph[3][i] + bbMult * sc.Subgraph[1][i];  // BB_Upper
    sc.Subgraph[2][i] = sc.Subgraph[3][i] - bbMult * sc.Subgraph[1][i];  // BB_Lower (negative)
}

// 3. Proximity computation
float minBBWidth = sc.Input[3].GetFloat();
float threshold  = sc.Input[2].GetFloat();

for (int i = sc.UpdateStartIndex; i < sc.ArraySize; i++) {
    float delta    = sc.Subgraph[0][i];
    float bbUpper  = sc.Subgraph[1][i];
    float bbLower  = sc.Subgraph[2][i];
    float bbWidth  = bbUpper - bbLower;

    // BB near-zero guard
    if (bbWidth < minBBWidth) {
        sc.Subgraph[4][i] = 0.0f;
        sc.Subgraph[5][i] = 0.0f;
        continue;
    }

    // Proximity (long: delta positive, short: delta negative)
    float proxLong  = (delta > 0.0f && bbUpper > 0.0f) ? (delta / bbUpper) : 0.0f;
    float proxShort = (delta < 0.0f && bbLower < 0.0f) ? (delta / bbLower) : 0.0f;
    if (proxLong  > 1.0f) proxLong  = 1.0f;
    if (proxShort > 1.0f) proxShort = 1.0f;

    sc.Subgraph[4][i] = proxLong;
    sc.Subgraph[5][i] = proxShort;

    // Alert point subgraphs (visible when in coiling zone but not yet fired)
    sc.Subgraph[6][i] = (proxLong  >= threshold && proxLong  < 1.0f) ? sc.High[i] : 0.0f;
    sc.Subgraph[7][i] = (proxShort >= threshold && proxShort < 1.0f) ? sc.Low[i]  : 0.0f;
}

// 4. Rising-edge alert (post-loop watermark — standard CLAUDE.md pattern)
int& alertedLong  = sc.GetPersistentInt(2);
int& alertedShort = sc.GetPersistentInt(3);
const int lastBar  = sc.ArraySize - 1;
const int scanBars = 5;

for (int i = lastBar; i >= (lastBar - scanBars < 0 ? 0 : lastBar - scanBars); i--) {
    if (sc.Subgraph[6][i] != 0.0f && i > alertedLong) {
        sc.SetAlert(sc.Input[4].GetInt(), lastBar, "Coil Long onset");
        alertedLong = i;
        break;
    }
}
for (int i = lastBar; i >= (lastBar - scanBars < 0 ? 0 : lastBar - scanBars); i--) {
    if (sc.Subgraph[7][i] != 0.0f && i > alertedShort) {
        sc.SetAlert(sc.Input[4].GetInt(), lastBar, "Coil Short onset");
        alertedShort = i;
        break;
    }
}
```

> **Note on BB computation:** The two-pass approach above (MovingAverage + StdDeviation then recompute Upper/Lower) may need adjustment if `sc.StdDeviation` does not exist under that name in this SC build. Alternative: use `sc.BollingerBands(In, Upper, Lower, Middle, Length, Mult, MAType)` if available, or compute variance manually in the loop. Verify function names against the SC ACSIL reference before finalizing.

### File header comment block
```cpp
// CoilingState.cpp
// Computes proximity of bar delta to its Bollinger Band threshold.
// Delta = sc.AskVolume - sc.BidVolume (no external study dependency).
// BB computed internally on delta. Chart-type agnostic.
//
// Persistent slots: 1=lastKnownBars, 2=alertedBarLong, 3=alertedBarShort,
//                   4-6=settings fingerprint
// Subgraphs 0-3: internal (IGNORE). Subgraphs 4-7: output.
```

### PROJECT_HISTORY.md entry
Add entry: new study `CoilingState.cpp`, date, purpose, inputs, subgraph layout.

---

## FAILURE (any of these = not done)

### Compilation
- [ ] Any compiler error or warning under F5 in Sierra Chart
- [ ] `std::min` / `std::max` used anywhere
- [ ] Missing `SCSFExport` on the study function

### Logic
- [ ] `proxLong` or `proxShort` can exceed 1.0 (no upper clamp)
- [ ] `proxLong` is non-zero when delta ≤ 0, or `proxShort` non-zero when delta ≥ 0
- [ ] Division when `bbWidth < minBBWidth` (near-zero guard missing)
- [ ] NaN or infinity reaches `sc.Subgraph[4][i]` or `[5][i]`
- [ ] Delta not computed as `sc.AskVolume[i] - sc.BidVolume[i]` (external study ID referenced instead)

### Intrabar
- [ ] Missing intrabar guard — processes every tick, not bar closes only
- [ ] `lastKnownBars` not stored in `sc.GetPersistentInt(1)`

### Alert
- [ ] Alert fires on every sustained coiling bar, not just onset (rising edge missing)
- [ ] Alert fires on historical bars during full recalculation
- [ ] Alert anchored to historical bar index `i` instead of `sc.ArraySize - 1`
- [ ] Long and short share a single watermark int (must be separate: Int(2) long, Int(3) short)

### Configuration
- [ ] Any structural input (BB length, multiplier, threshold, BB guard, sound ID) missing from fingerprint
- [ ] Any color input included in fingerprint
- [ ] Settings change does not trigger full recalculate

### Documentation
- [ ] Persistent slot usage not documented at top of file
- [ ] `PROJECT_HISTORY.md` not updated

---

## Prerequisite (tracked separately)

Pattern-based trigger families (Delta Trap #19/20, POCL/POCS #21/22) require intermediate hidden subgraphs added to the existing trigger studies, exposing `pattern_bars_completed` counters as `DRAWSTYLE_IGNORE` SGs. This contract covers only the delta/BB proximity family. Pattern-based proximity is a v2 extension.

---

## Verification checklist

```
- [ ] F5 compile: zero errors, zero warnings
- [ ] On any chart type: ProxLong and ProxShort show 0–1 values on live bars
- [ ] ProxLong and ProxShort never simultaneously > 0 (mutually exclusive)
- [ ] Set CoilThreshold = 0.00: alert fires once, does not repeat while sustained
- [ ] Set CoilThreshold = 1.00: no alert fires under normal conditions
- [ ] Change BB Length or Multiplier: study triggers full recalculate
- [ ] Change a color input: no full recalculate
- [ ] Set BB Length to 1 (degenerate): near-zero guard prevents NaN
- [ ] PROJECT_HISTORY.md updated
```
