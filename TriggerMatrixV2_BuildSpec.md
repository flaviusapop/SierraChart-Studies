# TriggerMatrixV2 BuildSpec — Display-Only Primary Detector Study v1.0

> This BuildSpec is standalone and normative for the build. Catalog pins
> below are stated inline; no external design document is required to
> implement, compile, or test this study.

- Catalog version: `2.0.0-research-2026-09-09`
- Catalog SHA-256: `ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618`
- Study identity: `TriggerMatrixV2` / `scsf_TriggerMatrixV2`
- GraphName: `Trigger Matrix V2 Display v1.0`
- Scope: display only. No alerts, no CSV, no trading, no confluence,
  no contexts (`bullWithContext`/`bearWithContext` excluded), no blocked
  families (ordinals 3, 12, 13 excluded).
- Legacy `TriggerMatrix.cpp` predicates are non-authoritative and unused.
  Only the 32 primary `bull`/`bear` catalog variants are ported,
  mechanically, without algebraic simplification.
- Subgraph index translation (normative): catalog Spreadsheet notation
  `ID{...}.SGn` is one-based; ACSIL `SubgraphIndex` /
  `SetStudySubgraphValues` / `GetStudyArrayFromChartUsingID` is zero-based,
  so `index = n - 1`: SG1->0, SG2->1, SG3->2, SG4->3, SG59->58.
  `TMV2_SG_*` constants in the portable seam carry the zero-based indices
  and are used at every producer call site.

## 1. Role-to-family map (16 ready families, 32 outputs)

Range (11): 1, 2, 4, 5, 6, 7, 8, 9(primary), 10, 11, 19.
Renko 6t (2): 14, 15.
Renko 8t (3): 16(primary), 17(primary), 18(primary core).
Non-selected roles output exactly zero.

## 2. Stable SG contract (even=Bull, odd=Bear; append-only after v1.0)

| SG | Ord | Code | Family | Style |
|----|----:|---|---|---|
| 0/1 | 1 | OED | Opposing Effort Decay | TEXT "OED+"/"OED-" |
| 2/3 | 2 | DES | Directional Effort Slope Response | ARROWUP/ARROWDOWN |
| 4/5 | 4 | EEF | Extreme Effort Failure | TEXT "EEF+"/"EEF-" |
| 6/7 | 5 | VGD | VAP Gradient Divergence | TEXT "VGD+"/"VGD-" |
| 8/9 | 6 | WDM | Whole-Distribution Migration | TEXT "WDM+"/"WDM-" |
| 10/11 | 7 | DRP | Delta Reversal Profile Response | TEXT "DRP+"/"DRP-" |
| 12/13 | 8 | PDR | POC Delta Reclaim | TEXT "PDR+"/"PDR-" |
| 14/15 | 9 | FVR | Frozen VPOC Reclaim | TEXT "FVR+"/"FVR-" |
| 16/17 | 10 | ERM | Effort Reversal with Value Migration | TEXT "ERM+"/"ERM-" |
| 18/19 | 11 | PBM | Fixed POC Balance Migration | SQUARE |
| 20/21 | 14 | OFR | Order-Flow Reversal | TEXT "OFR+"/"OFR-" |
| 22/23 | 15 | DVR | Delta Reversal VPOC Reclaim | TEXT "DVR+"/"DVR-" |
| 24/25 | 16 | VXC | Frozen VPOC Cross | TEXT "VXC+"/"VXC-" |
| 26/27 | 17 | PEV | Persistent Effort Volume Response | TEXT "PEV+"/"PEV-" |
| 28/29 | 18 | R8F | Renko-8 Effort Failure | TEXT "R8F+"/"R8F-" |
| 30/31 | 19 | FPR | Frozen POC Recovery | TEXT "FPR+"/"FPR-" |

Bull default cyan `RGB(0,200,255)` below low; Bear amber
`RGB(255,159,28)` above high. Text size 8, shape width 4, `DrawZeros=0`.

## 3. Inputs (17, stable order)

0 Chart Role (Range / Renko 6t / Renko 8t), default Range.
1 Range Delta Study ID (SG4), default 0 = unset.
2 Range Delta Bands Study ID (SG1 upper / SG3 lower, 20/0.9 SMA), 0.
3 Range VPOC A Study ID (SG1), 0.
4 Range VPOC B Study ID (SG1), 0.
5 Range VA 68 Study ID (SG1 VVAH / SG2 VVAL), 0.
6 Renko 6 Delta Study ID (SG4), 0.
7 Renko 6 Delta Bands Study ID (SG1/SG3), 0.
8 Renko 6 VPOC Study ID (SG1), 0.
9 Renko 6 Ask Diagonal Study ID (SG59 count, +300%), 0.
10 Renko 6 Bid Diagonal Study ID (SG59 count, -300%), 0.
11 Renko 8 VPOC Study ID (SG1), 0.
12 Bull Base Offset (ticks), default 2, display only.
13 Bear Base Offset (ticks), default 2, display only.
14 Stack Step (ticks), default 3, display only.
15 VAP Multiplier Is 1 (No/Yes), default No, family-5 precondition.
16 Source Configuration Revision, default 1.

Zero ID disables dependent families only. No silent catalog-candidate
defaults. Thresholds are hard-coded canonical constants.

## 4. Data contracts

- Native flow: `AV=sc.BaseData[SC_ASKVOL][i]`,
  `BV=sc.BaseData[SC_BIDVOL][i]`, `total=AV+BV`,
  `norm=delta/max(1,total)`. Invalid/nonfinite/negative AV/BV fails the
  flow primitive closed. `(AV+BV)>=20` stays a per-bar veto.
- Producers fetched once per update via current-header same-chart APIs
  (catalog SGn -> ACSIL index): Delta SG4->3; Bands SG1->0 / SG3->2;
  VPOC SG1->0; VA SG1->0 / SG2->1; Diagonal SG59->58.
  Structural vs dynamic fail-closed: a zero study ID or
  `VAP Multiplier Is 1=No` is structural and is reported once per new
  fingerprint by the role-specific summary below. An empty/too-short
  producer array at an individual bar is dynamic: that bar's outputs are
  cleared and the dependent family fails closed with no log (no per-bar
  spam).
- Structural summary (`Tmv2_StructDisabled` + `Tmv2_FamilyCode` +
  `Tmv2_SrcLabel`): at most one Message Log line per new structural
  fingerprint, emitted before the `ArraySize<2` early return so tiny charts
  still report. Format names the selected role (`Range` / `Renko 6t` /
  `Renko 8t`), each missing role-relevant structural input/precondition
  (`RDelta(id=..)`, `RBands`, `RVpA`, `RVpB`, `RVA`, `R6Delta`, `R6Bands`,
  `R6Vp`, `R6AskD`, `R6BidD`, `R8Vp`, `VAP Multiplier Is 1=No`), and each
  disabled family as `ord<code>` (`ord7/DRP`, ...). Role map: Range VAP-No
  disables 5; missing RDelta or RBands disables 7,8,10,11; missing RVpA
  disables 6,10,11; missing RVpB disables 7,8,9,19; missing RVA disables
  6,10,11,19; Renko6 missing delta/bands/vpoc disables 14+15 while missing
  either diagonal disables 14 only; Renko8 missing VPOC disables 16+17.
  Native-only families (Range 1,2,4; Renko8 18) stay enabled.
- Finite check is portable `std::isfinite` with no constant divide-by-zero
  expression, so it compiles cleanly on MSVC and GCC/Clang.
- FNV-1a 64-bit offset basis is `14695981039346656037ULL`
  (`TMV2_FNV_OFFSET_BASIS`, pinned by test).
- Family 5: `MaintainVolumeAtPriceData=1`, positive tick, multiplier
  confirmed Yes, all five exact tick rows exist with positive total
  volume; side-sum >= 20; exact-tick match only, no nearest-row fallback.
- VPOC/VA never recomputed; preserve A vs B; require positive values and
  `VAH>=VAL` where the formula does; no Close/High/Low/zero fallback.
- Delta-band lags preserved exactly (offsets -1, -2, -3 per formula).
- Family 14 requires both diagonal IDs; SG59 is a level count; no
  `AskVolume>=300` approximation.

## 5. Completed-bar engine (`AutoLoop=0`, region 0)

`lastClosed = ArraySize-2`; forming bar always zero; no 400-bar warm-up.
Fail-closed history warm-ups (`Tmv2_MinBarIndex`, minimum zero-based
current bar index; `Tmv2_EvalRoleAt`/`Tmv2_ApplyWarmup` clear ineligible
pairs before publishing):

| Ord | Min bar | Ord | Min bar |
|----:|--------:|----:|--------:|
| 1 | 3 | 11 | 21 |
| 2 | 3 | 14 | 22 |
| 4 | 2 | 15 | 22 |
| 5 | 0 | 16 | 2 |
| 6 | 1 | 17 | 5 |
| 7 | 22 | 18 | 2 |
| 8 | 20 | 19 | 2 |
| 9 | 2 | (blocked 3,12,13) | -1 |
| 10 | 22 | | |

Band families need up to 23 completed bars for a 20-period band at offset
-3, so zero/unpublished band values can never read as valid. Incremental
pass from `min(UpdateStartIndex,lastClosed)` is idempotent; no
ArraySize-unchanged early return (late producer publication recovers via
rescan).

## 6. Rendering

Per completed bar: clear 32 outputs, evaluate role-eligible predicates
into `fired[32]`, assign dense side-specific lanes in SG/catalog order,
`DRAWSTYLE_IGNORE` outputs keep nonzero truth but consume no lane.
`BullY=Low-(base+lane*step)*tick`,
`BearY=High+(base+lane*step)*tick`. No merging/count badges.

## 7. Fingerprint

Deterministic 64-bit structural fingerprint stored as two persistent
ints; covers schema version, catalog hash/version, role, 11 source IDs,
VAP flag, config revision, display offsets, tick-size bits. Any change
triggers full local rebuild. Does not prove external study settings;
those go in a separate Sierra validation manifest.

## 8. Portable seam and test ledger (`TMV2_UNITTEST`)

`TriggerMatrixV2.cpp` compiles its pure core under `TMV2_UNITTEST`
without Sierra headers: `Tmv2Window` (offset-indexed OHLC/AVBV +
producer slots + VAP rows), `Tmv2_Norm`, `Tmv2_FlowValid`,
`Tmv2_IsFinite`, 32 `Tmv2_F<ord>Bull/Bear` predicates, role/SG map
queries, `Tmv2_CatalogSgToIndex` + `TMV2_SG_*` producer indices,
`Tmv2_MinBarIndex` / `Tmv2_ApplyWarmup` / `Tmv2_EvalRoleAt`,
`Tmv2_FamilyCode` / `Tmv2_SrcLabel` / `Tmv2_StructDisabled` (structural
summary), `Tmv2_StackLanes`, `Tmv2_Fingerprint` + `Tmv2_FnvOffsetBasis`, catalog pin
accessors. `tests/` asserts: 16 families, 32 outputs, SG0..31
unique/stable (<=59), blocked {3,12,13} + all 8 contexts absent, catalog
SGn->index translation (SG1->0, SG2->1, SG3->2, SG4->3, SG59->58),
warm-up thresholds and below-threshold clearing, NaN/Inf finite gates,
FNV offset-basis pin, threshold equality boundaries, 20-veto, lag
 fidelity, missing-source fail-closed, VAP gates, Bull/Bear mirrors,
 all-true / all-false / one-clause-false, stacking geometry, fingerprint
 determinism, recalc idempotence contract, structural-disabled summary
 (T27: code/label pins, per-dependency sets, native-only enabled).

## 9. Honest status

`native_sierra_compile=false`, `sierra_runtime_parity=false` until the
user-side Sierra F5 compile and on-chart 32-formula parity gate pass.
Parity gate: zero mismatches per eligible completed bar across Range,
Renko 6t, Renko 8t; replay/live/forming-bar behavior; producer-removal
clearing; family-5/family-14 enablement gates.
