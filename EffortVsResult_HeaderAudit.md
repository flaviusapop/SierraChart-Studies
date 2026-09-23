# EffortVsResult — Header Audit (Zander headers)

Date: 2026-09-03. Source: `EffortVsResult.cpp` (830 lines, commit `66aa8f7`).
Headers: `sierrachart_zander.h`, `scstructures_zander.h`, `scconstants_zander.h`
(exact copies from Zander). Method: checked every ACSIL method, enum/drawing
constant, struct field, overload, and argument type used by the study. No
generic or web API names trusted over these headers.

## Verdict: CLEAN — zero incompatibilities, no source/spec changes

- `SCDLLName`, `SCSFExport` — `scstructures_zander.h:119-122` — exact.
- `SCStudyInterfaceRef`, `SCInputRef`, `SCSubgraphRef` — typedefs — exact.
- `sc.SetDefaults`, `GraphName`/`StudyDescription` (`SCString`), `AutoLoop`
  (`:2556`), `GraphRegion` (`:2445`), `DrawZeros`, `MaintainVolumeAtPriceData`
  (`:2874`) — all `int`/`SCString` fields — exact.
- Subgraph fields `Name`/`DrawStyle:uint16_t`/`LineWidth:uint16_t`/
  `PrimaryColor:uint32`/`DrawZeros:int`/`Data:SCFloatArray`/
  `DataColor:SCColorArray`/`operator[](int)->float&`
  (`s_SCSubgraph_260`) — exact; `t_ChartArrayDataType == float`.
- `DRAWSTYLE_LINE`, `BAR`, `POINT`, `ARROW_UP`, `ARROW_DOWN` —
  `scconstants_zander.h` enum — exact. `DRAWSTYLE_LINE_SKIPZEROS` is the
  documented `#define` alias (`:515`) and matches `FlowConviction.cpp:274`
  precedent — exact.
- Inputs `SetInt`/`SetIntLimits`/`SetFloat`/`SetFloatLimits`/
  `SetCustomInputStrings(const char*)`/`SetCustomInputIndex(unsigned)`/
  `SetYesNo`/`SetColor`/`SetAlertSoundNumber` and reads `GetInt`/`GetFloat`/
  `GetIndex`/`GetYesNo`/`GetColor` — `s_SCInput_145` bodies — exact.
  `GetInt` explicitly handles `ALERT_SOUND_NUMBER_VALUE`, so
  `In_AlertSound.GetInt()` is correct (matches `ReconTape.cpp:856`,
  `ReconTapeV2.cpp:843`); `unsigned->int` narrowing on `GetIndex()` matches
  `ReconTape.cpp:740`, `FlowConviction.cpp:383` precedent.
- `sc.VolumeAtPriceForBars` (`c_VAPContainer*`, `:3025`) +
  `GetNextHigherVAPElement((unsigned)bar, int&, const s_VolumeAtPriceV2**)`
  with `(int64_t)Ask-(int64_t)Bid` — byte-identical to compiling baselines
  `TrappedTraders.cpp:409-414`, `LiquidityZones.cpp:233-238` — exact by
  precedent (`VAPContainer.h` body itself is outside the 3-file cache; the
  call site shows no deviation from the proven pattern).
- Persistent `GetPersistentInt->int&` / `SetPersistentInt(int,int)` /
  `GetPersistentPointer->void*&` + `reinterpret_cast<S_EvrState*>` /
  `SetPersistentPointer(int,void*)` + `nullptr` — `:3166-3181`, matches
  `FlowConviction.cpp:367-368,411-418` — exact.
- `sc.ArraySize` / `UpdateStartIndex` / `IsFullRecalculation` /
  `LastCallToFunction` (`int`), `sc.Open/High/Low/Close` (`SCFloatArray`),
  `sc.TickSize` (`float`) — exact.
- `sc.SetAlert(int,int,const SCString&)` — exact overload at
  `sierrachart_zander.h:1624`, anchored to `forming == ArraySize-1` per house
  pattern. `SCString::Format("%d bar(s) back", int)` — `SCString.h` is outside
  the 3-file cache; usage is in the same `%d/%.2f` family as
  `ReconTape.cpp:589,855`, `InterestMap.cpp:717` — precedent-exact.
- `RGB()`/`COLORREF`/`max(a,b)` macro (`scstructures_zander.h:90-98`)/
  `fabsf`/`memset`/`INT_MIN` — exact.
- Not used: `GetMovAvgType`, `UseTool`, `Stochastic`, `MovingAverage`, or any
  web-sourced overload — none appear in the source.

## Gate

`python3
/home/ubuntu/.hermes/skills/software-development/acsil-impl-loop/scripts/gate_check.py
EffortVsResult.cpp` → **12/12 PASS, 830 lines** (run 2026-09-03 from the
worktree root).

## Change log

No `.cpp` or `_BuildSpec.md` edits — audit is clean, so per instruction only
this file was added plus a matching Header Audit section in
`EffortVsResult_Review.md`. No existing studies modified.
