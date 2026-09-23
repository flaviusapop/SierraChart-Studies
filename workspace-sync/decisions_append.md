# Decisions — 2026-06-18

[2026-06-18] Decision: Remove SG_MACD_LINE, SG_SCORE_LONG, SG_SCORE_SHORT from CoilingState.cpp to stay within SC's 60-subgraph limit. Why: SC silently clips subgraph indices > 59 to slot 59; the last SetDefaults write wins, making earlier subgraphs invisible. MACD line computed inline instead; score SGs were not required for alert logic.

[2026-06-18] Decision: Use sc.GraphName version string ("CoilingState v1.1") as a DLL load canary during debugging. Why: After SC restart, old subgraph layout persisted — needed a definitive signal to distinguish stale DLL from fresh DLL with wrong structure.

[2026-06-18] Decision: CoilThreshold default operating range is 0.70–0.85 for pre-ignition detection, not 1.0. Why: Threshold=1.0 means all sub-conditions are simultaneously fully met — triggers have already fired, not coiling. 0.70–0.85 catches the approach without requiring full confluence.
