# Ideas — 2026-06-18

[2026-06-18] Idea: Build an exact CBBOAC trigger replication as a single self-contained ACSIL study. Context: User asked if CoilingState at threshold=1.0 would produce the same prints as CBBOAC. It won't — CBBOAC uses exact logic (SMI, per-price VAP bid/ask thresholds, NYSE Tick) vs proximity approximations. Analysis confirmed all 38 active triggers (minus T17/T18 NYSE Tick) can be replicated exactly with additions: per-price VAP lookups via GetNextHigherVAPElement, proper SMI, price BB for T37/T38, ±300 vol-at-price for T28/T30.

[2026-06-18] Idea: Use sc.GraphName as a DLL freshness canary when debugging subgraph structure issues. Context: After SC restart, old subgraph layout persisted and it was unclear if recompiles were taking effect. Changing GraphName to a version string ("v1.1") gives a definitive visible signal in the SC title bar that the new DLL loaded — separates "DLL not loading" from "DLL loaded but structure is wrong."
