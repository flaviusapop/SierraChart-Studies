# Sierra Chart ACSIL Studies — Project Context

## What this project is
Custom Sierra Chart studies written in C++ using the ACSIL (Advanced Custom Study Interface and Language) API. Studies are compiled inside Sierra Chart (F5) and loaded as DLLs. No external build system.

## Studies in this project
| File | Purpose |
|---|---|
| AbsorptionGradient.cpp | Detects absorption at key levels |
| AVWAPRotation.cpp | Anchored VWAP rotation signals |
| BigTradesTape.cpp | Large trade detection with CSV persistence |
| BreadthCompositeO... | Breadth oscillator composite |
| DeltaReversalTrigger.cpp | Delta-based reversal detection |
| DeltaVelocityProfile.cpp | Delta velocity + acceleration (dual-source) |
| DOMReader.cpp / DOMReaderV2.cpp | DOM imbalance reader |
| FlowConviction.cpp | Order flow conviction composite (EWMA-normalized) |
| InterestMap.cpp | Open interest mapping |
| LiquidityZones.cpp | Supply/demand zone detection |
| MTFCloseFilter.cpp | Multi-timeframe close filter |
| OrderflowConfluence.cpp | Confluence of orderflow signals |
| OrderflowSignalV2.cpp / V3.cpp | Main orderflow signal with HTF triggers |
| OTFStateFilter.cpp | Orderflow trend state filter |
| ReconTape.cpp / ReconTapeV2.cpp | Tape reconstruction / T&S analysis |
| TapeReader.cpp | Raw tape reading |
| TrappedTraders.cpp | Trapped trader detection |
| PROJECT_HISTORY.md | Changelog — update this when making significant changes |

---

## ACSIL Core Patterns

### Study function signature
```cpp
SCSFExport scsf_StudyName(SCStudyInterfaceRef sc)
```

### Subgraphs and Inputs
```cpp
sc.Subgraph[0]  // access subgraph by index
sc.Input[0]     // access input by index
// Always set Name, DrawStyle, PrimaryColor in sc.SetDefaults block
```

### Intrabar guard (standard pattern used across all studies)
```cpp
int& lastKnownBars = sc.GetPersistentInt(1);
const bool isFullRecalc = (sc.UpdateStartIndex == 0);
const bool isNewBar     = (sc.ArraySize > lastKnownBars);

if (!isFullRecalc && !isNewBar)
    return;  // skip intrabar ticks

lastKnownBars = sc.ArraySize;
```

### Full recalculation detection
```cpp
sc.IsFullRecalculation  // true when SC requests full pass
sc.UpdateStartIndex == 0  // also indicates full recalc
```

---

## Persistent Storage — Slot Conventions
Persistent slots survive across calls. Document slot usage per study to avoid conflicts.

| Slot | Type | Common usage |
|---|---|---|
| 1 | Int | `lastKnownBars` — new bar detection |
| 2 | Int | Alert watermark — newest alerted signal bar index |
| 3 | Int | Alert watermark — newest alerted confluence bar |
| 4–6 | Int | Settings fingerprint words (structural input change detection) |

**Settings fingerprint pattern** (used in LiquidityZones, TrappedTraders):
```cpp
// Compute fingerprint from structural inputs
int fp0 = (int)sc.Input[0].GetInt() ^ ((int)sc.Input[1].GetFloat() * 1000);
// etc for each structural input
int& storedFp0 = sc.GetPersistentInt(4);
bool settingsChanged = (storedFp0 != fp0);
if (settingsChanged) isFullRecalc = true;
storedFp0 = fp0;
// Display-only inputs (colors, transparency) are intentionally excluded
```

---

## Alert Pattern
**Problem**: HTF triggers backfill onto historical bars, so `sc.SetAlert(sound, i, ...)` with a stale bar index is suppressed by SC during real-time updates.

**Solution**: Move alerts out of the bar loop into a post-loop watermark scan:
```cpp
// After bar loop:
int& alertedSignalBar = sc.GetPersistentInt(2);
const int scanBars = 10;
const int lastBar = sc.ArraySize - 1;

for (int i = lastBar; i >= max(0, lastBar - scanBars); i--) {
    if (signalPresentAtBar(i) && i > alertedSignalBar) {
        sc.SetAlert(soundID, lastBar, alertMessage);  // anchor to CURRENT last bar
        alertedSignalBar = i;
        break;
    }
}
```

---

## Known Compiler Gotchas

### GetMovAvgType() requires explicit cast
```cpp
// WRONG — returns unsigned int, won't compile
const MovAvgTypeEnum maType = sc.Input[0].GetMovAvgType();

// CORRECT
const MovAvgTypeEnum maType = (MovAvgTypeEnum)sc.Input[0].GetMovAvgType();
```

### T&S timestamps ≠ chart bar timestamps
T&S clock is in a different timezone than chart bars. Do not use raw T&S timestamps to anchor drawn objects. Map to bar index using bar start time instead.

### sc.MovingAverage subgraph-ref args
Passing subgraph refs to `sc.MovingAverage` passes the compiler's type check — confirmed safe.

---

## Normalization Conventions

### FlowConviction / composite signals
- Use **EWMA normalization**, not Welford variance (Welford had amplitude drift issues)
- **Z-score without mean subtraction** for Aggression, Velocity, Slope — mean subtraction causes sustained signals to decay toward zero, which is wrong for trending conditions
- Composite: 3-bar EMA smoothing + 2-bar color hysteresis
- Orange/purple divergence colors only fire when composite is opposite sign to price for 2 consecutive bars

---

## BigTradesTape — Persistence
Large trade ledger is persisted to CSV:
- File location: Sierra Chart Data Files folder → `BigTradesTape_<symbol>.csv`
- On first run each SC session: file loads into ledger, old entries (beyond retention window) dropped
- Default retention: 5 days (`History File Keep (days)` input, 0 = disable)

---

## Build & Workflow
- **Compile**: F5 inside Sierra Chart
- **Full recalculate**: Ctrl+Insert
- **After structural changes** (new subgraph, changed input order): remove and re-add the study — saved settings reference by index and will misalign otherwise
- **After alert watermark changes**: restart SC session to clear in-memory state

---

## Project conventions
- Keep `PROJECT_HISTORY.md` updated when making significant logic changes
- Note which persistent slots each study uses at the top of the file
- Display-only inputs (colors, transparency, border width) never go into settings fingerprints
- Structural inputs (lookback windows, thresholds, mode selectors) always trigger full recalc on change
