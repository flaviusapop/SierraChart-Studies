# TriggerMatrixV2 Review and Validation Status

## Delivered scope

- New standalone ACSIL study: `TriggerMatrixV2.cpp`
- Export: `scsf_TriggerMatrixV2`
- Graph name: `Trigger Matrix V2 Display v1.0`
- Display-only: no alerts, file exports, trading, scoring, weighting, confluence, blocked families, or optional-context variants
- 16 ready detector families, each with independent Bull/Bear outputs: 32 outputs in stable `SG0..SG31` order
- One instance per native chart role: Range, Renko 6t, or Renko 8t
- Catalog pin:
  - version `2.0.0-research-2026-09-09`
  - SHA-256 `ea562e4789cc16bae2f3529882a9832d1ba8d1a0dd9d95fbb02088f8f5a08618`

## Verified locally

### Portable suite

```text
TMV2 ALL GREEN: 485 checks
```

The suite covers the ledger and SG map, all 32 predicates, mirrored cases, one-clause-false cases, strict/equality boundaries, 20-contract vetoes, source and VAP fail-closed behavior, exact lags, family warm-ups, role isolation, stacking, fingerprinting, corrected producer indices, finite-value handling, and structural dependency summaries.

### Independent formula parity

Three bounded reviews covered exactly 32 unique primary variants with no omissions or extras:

- Mimo v2.5: ordinals 1, 2, 4, 5, 6, 7 — 12/12 pass
- Nemotron 3 Ultra: ordinals 8, 9, 10, 11, 14 — 10/10 pass
- Mimo v2.5: ordinals 15, 16, 17, 18, 19 — 10/10 pass

Result: 32/32 pass; zero blocking issues. The additional `Tmv2_TickOk` and finite/flow checks are fail-closed guards and do not change the signal set on valid Sierra data.

### Repository ACSIL gate

The project gate passed 12/12 checks: one export, balanced braces, no static/thread-local runtime state, no stray comments, no forbidden price-array equality or `sc.Stochastic`, all 17 inputs defaulted/read once, no duplicate inputs, subgraph bound ≤58, and consistent persistent-pointer handling.

### ACSIL/header gate

The final complete-source review passed 17/17 checked contracts with zero blockers. It verified the corrected catalog-to-ACSIL subgraph translation, VAP access, completed-bar boundary, warm-ups, stale/forming-bar clearing, role isolation, structural logging, dynamic producer fail-closed behavior, display geometry, fingerprinting, and the 32/60 subgraph budget.

A prior excerpt-only audit raised four supposed compile blockers because its packet omitted relevant source/header sections. All four were disproved by the complete Zander headers and actual compilation; the complete-source audit supersedes it.

### Windows-target compile/link

The post-fix source compiled and linked with MinGW against the headers copied from Zander's Sierra Chart installation (`SC_DLL_VERSION 2927`):

```text
object compile: exit 0
DLL link: exit 0
format: PE32+ Windows x86-64 DLL
exports: scdll_DLLName, scdll_DLLVersion, scsf_TriggerMatrixV2
```

Compiler warnings came from Sierra headers and portable test-only static accessors; there were no compile or link errors. The generated MinGW DLL is validation evidence, not the deployment artifact.

## Defects found and repaired before delivery

- Corrected catalog one-based producer notation to zero-based ACSIL indices:
  - SG1 → 0
  - SG2 → 1
  - SG3 → 2
  - SG4 → 3
  - SG59 → 58
- Added family-specific history warm-ups, including delayed 20-period delta-band publication.
- Replaced a nonportable finite check with `std::isfinite`.
- Corrected the FNV-1a 64-bit offset basis to `14695981039346656037ULL`.
- Replaced the generic ID log with one human-readable summary per changed structural fingerprint naming:
  - selected role;
  - missing role-relevant inputs/preconditions;
  - exact disabled ordinal/code families.
- Moved that summary before the tiny-chart return. Dynamic empty/short arrays still fail closed per bar without log spam.

## Not yet verified

These remain Sierra-side acceptance gates:

1. Compile with Sierra Chart's F5/remote build path.
2. Remove and re-add the study after introducing the new input/subgraph structure.
3. Configure separate Range, Renko 6t, and Renko 8t instances with their same-chart producer Study IDs.
4. Record the chart/symbol/bar/session/Sierra/VAP/source-study configuration manifest.
5. Run a full recalculation and compare all 32 outputs against the corresponding CBBOAC formulas on eligible completed bars.
6. Demonstrate zero mismatches across all three roles.
7. Verify replay/live forming-bar clearing and late producer publication.
8. Remove each producer and verify only its dependent families clear and recover.
9. Verify family 5 with valid VAP data and `Volume at Price Multiplier = 1`, plus family 14 with the configured SG59 diagonal-count producers.

Until those pass, this is research/display software—not backtested, event-parity approved, or live-approved.
