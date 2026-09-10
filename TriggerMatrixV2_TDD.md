# TriggerMatrixV2 — TDD Record: Five Verified Defects Fix

Date: 2026-09-10. Method: strict RED→GREEN. Portable suite compiles with
the system C++ compiler only (`g++ -std=c++17 -Wall -Wextra -O2`), no
external packages. Only these files were touched: `TriggerMatrixV2.cpp`,
`TriggerMatrixV2_BuildSpec.md`, `tests/test_trigger_matrix_v2.cpp`,
`tests/run_trigger_matrix_v2_tests.sh` (not needed — unchanged), and this
record. No commit, no push, no `.muse_context` / review-JSON changes.

Defects (per task):
1. Producer subgraph indices off by one (catalog `ID{...}.SGn` is one-based,
   ACSIL `SubgraphIndex` is zero-based): SG1->0, SG2->1, SG3->2, SG4->3,
   SG59->58. Test-visible constants above the `TMV2_UNITTEST` boundary,
   used in `SetStudySubgraphValues` and `GetStudyArrayFromChartUsingID`.
2. Fail-closed history warm-ups (minimum zero-based current bar index):
   1:3, 2:3, 4:2, 5:0, 6:1, 7:22, 8:20, 9:2, 10:22, 11:21, 14:22, 15:22,
   16:2, 17:5, 18:2, 19:2. Pure helper + below-threshold clearing so
   zero/unpublished 20-period band values never read as valid.
3. Nonportable `Tmv2_IsFinite` fallback with MSVC constant-divide-by-zero
   risk → portable `std::isfinite` + direct NaN/Inf tests.
4. FNV-1a 64-bit offset basis corrected to `14695981039346656037ULL` and
   pinned by test.
5. `TriggerMatrixV2_BuildSpec.md` no longer references the temporary
   `.muse_context/design.md`; standalone + documents the SG translation
   and warm-up table.

## RED (tests added BEFORE any production change)

New tests `T23_SgMapping`, `T24_Warmup`, `T25_Finite`, `T26_FnvBasis`
appended to `tests/test_trigger_matrix_v2.cpp` (with `<limits>` include
and four new `main()` calls). They reference the not-yet-existing portable
API: `TMV2_SG_*`, `Tmv2_CatalogSgToIndex`, `Tmv2_MinBarIndex`,
`Tmv2_EvalRoleAt`, `Tmv2_FnvOffsetBasis`, plus direct `Tmv2_IsFinite`
NaN/Inf checks.

Command:

```
bash tests/run_trigger_matrix_v2_tests.sh; echo "EXIT=$?"
```

Exact result (compile-fail RED, `EXIT=1`; representative lines):

```
tests/test_trigger_matrix_v2.cpp: In function ‘void T23_SgMapping()’:
tests/test_trigger_matrix_v2.cpp:753:11: error: ‘TMV2_SG_DELTA’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:754:11: error: ‘TMV2_SG_BAND_UP’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:755:11: error: ‘TMV2_SG_BAND_LO’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:756:11: error: ‘TMV2_SG_VPOC’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:757:11: error: ‘TMV2_SG_VVAH’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:758:11: error: ‘TMV2_SG_VVAL’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:759:11: error: ‘TMV2_SG_DIAG’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:760:11: error: ‘Tmv2_CatalogSgToIndex’ was not declared in this scope
...
tests/test_trigger_matrix_v2.cpp: In function ‘void T24_Warmup()’:
tests/test_trigger_matrix_v2.cpp:770:11: error: ‘Tmv2_MinBarIndex’ was not declared in this scope
...
tests/test_trigger_matrix_v2.cpp:794:13: error: ‘Tmv2_EvalRoleAt’ was not declared in this scope; did you mean ‘Tmv2_EvalRole’?
...
tests/test_trigger_matrix_v2.cpp: In function ‘void T26_FnvBasis()’:
tests/test_trigger_matrix_v2.cpp:822:11: error: ‘Tmv2_FnvOffsetBasis’ was not declared in this scope
EXIT=1
```

Note: `T25_Finite` compiled (its target `Tmv2_IsFinite` already existed)
and its NaN/Inf assertions are new coverage for the finite replacement;
the suite as a whole was RED because the other three areas had no
implementation. A source-level RED companion: pre-fix `TriggerMatrixV2.cpp`
contained the nonportable `1.0 / 0.0` fallback and the wrong FNV literal
`1469598103934665603ULL`.

## GREEN (implementation)

`TriggerMatrixV2.cpp`:
- Portable seam now defines `TMV2_SG_DELTA 3`, `TMV2_SG_BAND_UP 0`,
  `TMV2_SG_BAND_LO 2`, `TMV2_SG_VPOC 0`, `TMV2_SG_VVAH 0`,
  `TMV2_SG_VVAL 1`, `TMV2_SG_DIAG 58`, `TMV2_FNV_OFFSET_BASIS
  14695981039346656037ULL`, plus `Tmv2_CatalogSgToIndex()` and
  `Tmv2_FnvOffsetBasis()`. Header comment documents `index = n - 1`.
- Deleted the duplicated off-by-one `#define` block below the
  `TMV2_UNITTEST` boundary (`4/1/3/1/1/2/59`); all
  `SetStudySubgraphValues` / `GetStudyArrayFromChartUsingID` call sites
  now resolve to the corrected portable constants.
- `Tmv2_IsFinite` is `return std::isfinite(x) ? 1 : 0;` (no
  `#ifdef isfinite`, no `1.0 / 0.0` expression).
- `Tmv2_Fingerprint` seeds with `TMV2_FNV_OFFSET_BASIS`.
- Added `Tmv2_MinBarIndex()` (table above; `-1` for blocked/unknown),
  `Tmv2_ApplyWarmup()` (clears the ordinal's Bull/Bear pair when
  `barIndex < min`), and `Tmv2_EvalRoleAt()` (evaluate + gate + recount).
  The ACSIL bar loop calls `Tmv2_EvalRoleAt(role, &wBull, i, firedB)` and
  `Tmv2_EvalRoleAt(role, &wBear, i, firedR)` so sub-threshold bars publish
  zeros.

`TriggerMatrixV2_BuildSpec.md`:
- Removed the `.muse_context/design.md` reference; the spec declares
  itself standalone and normative.
- §Data contracts documents the SGn→index translation, the portable
  `std::isfinite` rule, and the pinned FNV basis.
- §5 carries the normative warm-up table and the
  `Tmv2_MinBarIndex`/`Tmv2_EvalRoleAt`/`Tmv2_ApplyWarmup` contract.
- §8 lists the new seam surface and test coverage.

Commands:

```
bash tests/run_trigger_matrix_v2_tests.sh; echo "EXIT=$?"
g++ -std=c++17 -Wall -Wextra -O2 -o /tmp/tmv2_tests tests/test_trigger_matrix_v2.cpp; echo "BUILD_EXIT=$?"
git diff --check; echo "DIFFCHECK_EXIT=$?"
grep -rPn ' +$' TriggerMatrixV2.cpp tests/test_trigger_matrix_v2.cpp TriggerMatrixV2_BuildSpec.md; echo "GREP_EXIT=$?"
```

Exact results:

```
TMV2 ALL GREEN: 435 checks
EXIT=0
BUILD_EXIT=0
DIFFCHECK_EXIT=0
GREP_EXIT=1
```

`GREP_EXIT=1` means no trailing-whitespace lines. Build is warning-free
under `-Wall -Wextra`. No commit or push performed.

## Coverage map (new tests → defects)

- `T23_SgMapping` → defect 1 (all seven `TMV2_SG_*` values + five
  `Tmv2_CatalogSgToIndex` translations).
- `T24_Warmup` → defect 2 (all 16 thresholds, blocked ordinals `-1`,
  `Tmv2_EvalRoleAt` clearing/keeping at 2/3 for ord 1, 21/22 for ord 7,
  19/20 for ord 8).
- `T25_Finite` → defect 3 (finite zero/value/negative true; NaN/±Inf
  false via `std::numeric_limits<double>::infinity()`).
- `T26_FnvBasis` → defect 4 (exact `14695981039346656037ULL` pin).
- BuildSpec edits → defect 5 (standalone; SG translation; warm-up table).

## Follow-up RED → GREEN: structural dependency summary

Acceptance gap: the original Message Log line listed raw IDs but did not name the missing role-relevant dependencies or the exact disabled families, and it was emitted after the `ArraySize < 2` return.

### RED

`T27_StructDisabled` was added before the production helper. It pins all 16 family codes, all 11 structural source labels, every role/dependency disabled set, and the native-only families that must remain enabled.

Command:

```bash
bash tests/run_trigger_matrix_v2_tests.sh; echo "EXIT=$?"
```

Observed result before implementation:

```text
tests/test_trigger_matrix_v2.cpp:848:23: error: ‘Tmv2_FamilyCode’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:865:23: error: ‘Tmv2_SrcLabel’ was not declared in this scope
tests/test_trigger_matrix_v2.cpp:877:30: error: ‘Tmv2_StructDisabled’ was not declared in this scope
EXIT=1
```

### GREEN

Implemented `Tmv2_FamilyCode`, `Tmv2_SrcLabel`, and `Tmv2_StructDisabled`; replaced the generic ID dump with one role-specific line per changed structural fingerprint; moved that line before the tiny-chart return. Empty or short runtime producer arrays continue to fail closed silently per bar.

Command:

```bash
bash tests/run_trigger_matrix_v2_tests.sh
git diff --check
```

Independently rerun result:

```text
TMV2 ALL GREEN: 485 checks
EXIT=0
git diff --check: clean
```
