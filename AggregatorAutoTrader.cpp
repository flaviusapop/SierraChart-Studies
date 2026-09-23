#include "sierrachart.h"

SCDLLName("Aggregator Auto Trader")

// SCDLLName MUST STAY AT THE TOP, ABOVE THE DOC HEADER.
// SierraChart pre-scans only the first N lines of a selected file to find it,
// and fails the build with "None of the selected source files contains the
// SCDLLName() line at the top of the file" if it is further down. Keep new
// documentation BELOW this line, never above it.

// =============================================================================
// AggregatorAutoTrader.cpp
// Sierra Chart ACSIL Custom Study  —  v1.0
//
// Auto-trades Trigger Aggregator V2. A deliberate SIMPLIFICATION of
// OFSignalAutoTrader, not a port of it: the aggregator now owns direction,
// level, netting and episode dedup, so most of what the old study carried
// exists to solve problems that no longer reach this layer.
//
// =============================================================================
// WHAT THIS READS
// =============================================================================
//   Three independent sources, each its own reference + enable. All three may
//   run at once; overlapping signals land on the same bar and the consumed set
//   collapses them.
//
//     Episode Event   SG14   ONE number = direction + level
//                            +1/+2/+3 bull L1/L2/L3   -1/-2/-3 bear
//                            +4       bull confluence -4       bear
//                            TWO REFERENCES: Long and Short are separate
//                            inputs, because an aggregator revision may emit
//                            the long episode on one subgraph and the short
//                            episode on another instead of one signed SG.
//                            Leave Short unset (or pointing at the same
//                            study+subgraph as Long) and the single signed-SG
//                            behaviour is used unchanged. When Short IS set,
//                            each reference contributes only its own side and
//                            the value's MAGNITUDE gives the level, so a
//                            short SG may publish +2 or -2 with equal effect.
//     Arrows          SG1    base of six: SG1/2/3 bull L1/2/3,
//                            SG4/5/6 bear L1/2/3. Hold a PRICE; non-zero = fired.
//     Confluence      SG17   base of two: SG17 bull, SG18 bear. Hold a PRICE.
//
//   SUBGRAPH INDEXES ARE NOT HARDCODED. Each reference carries its own
//   subgraph and the study reads by OFFSET from it, so a future aggregator
//   revision cannot silently mis-read (the failure mode flagged in
//   ideas_backlog.md 2026-07-21, where SG1-SG4 were baked in).
//   Defaults are 0-based API indexes: SG14 -> 13, SG1 -> 0, SG17 -> 16.
//
//   All three sources derive from the aggregator's single signed rawLevel, so
//   they can never disagree about DIRECTION on a bar. They can differ in WHEN:
//   SG14 is always once-per-episode, arrows follow the aggregator's Signal
//   Mode. Signal Mode = Every Bar + Use Arrows therefore trades plateau bars
//   that SG14 alone would not.
//
// =============================================================================
// POSITION MODEL — EVERY LEG IS ITS OWN POSITION
// =============================================================================
//   An attached stop is a CHILD OF ITS ENTRY ORDER, so a later entry can never
//   extend an earlier entry's stop. Standalone stop-order exits do not work at
//   all here: proven from the Trade Activity Log (decisions.md 2026-07-23),
//   sc.BuyExit/sc.SellExit with SCT_ORDERTYPE_STOP fails before it reaches the
//   trade service. Attached stops are the only mechanism available.
//
//   v1.1 stops fighting that. An Add Entry is treated as A NEW POSITION that
//   happens to run alongside the others, not as size bolted onto an existing
//   one. Each leg carries:
//
//       its own entry price      its own attached stop price
//       its own quantity         its own BE / profit-lock state
//
//   All legs are always the same DIRECTION (opposite-side signals flatten, they
//   never reverse), so the account net is simply the sum of the legs.
//
//   WHY THIS AND NOT ONE SHARED PRICE. A shared price forced two rules into
//   conflict: "the stop only ever moves in favour" (ratchet) versus "adding
//   size must not tighten risk already on". An add whose structure stop was
//   wider than a ratcheted stop had to violate one of them, and because the
//   push throttle refuses non-improving prices the legs silently desynced
//   anyway — the shared price was already a fiction. Per-leg stops make it
//   true by construction.
//
//   CONSEQUENCE: legs stop out INDEPENDENTLY and partial closes are NORMAL,
//   not an error path. Leg bookkeeping is driven off each child stop's own
//   order status, and the broker position is the reconciliation authority.
//
// =============================================================================
// STOPS
// =============================================================================
//   Entry     attached stop at the signal bar's extreme (N-bar window ending
//             AT the signal bar, default N=1 = the arrow bar itself), offset
//             by Stop Offset ticks. Belongs to THAT leg only.
//   BE        after BE Trigger Ticks of open profit ON THAT LEG, the leg's
//             stop -> that leg's entry price.
//   Profit    after Profit Trigger Ticks on that leg, the leg's stop -> that
//             leg's entry + Profit Offset Ticks, in favour.
//   Session   crossing Session End flattens everything at market.
//   Opposing  an opposite-side signal at or above Opposing Min Level cancels
//             the stops and flattens at market. Never reverses.
//
//   Every per-leg move is a RATCHET — a leg's price only ever moves in favour.
//   No bar trailing: Trailing Bars is deliberately absent from this build.
//
//   StopSent[i] MIRRORS THE BROKER, never the intent. It is read back from the
//   child order's own Price1, because the child ID does not exist at the
//   instant the entry fills: a BE that triggers inside that gap is skipped for
//   want of an ID, and recording the intended price at resolve time would make
//   the study believe a move it never sent.
//
//   MIN / MAX STOP DISTANCE ARE NOT OPTIONAL. Under attached orders a bad stop
//   price rejects the ENTRY, not just the protection. Min widens a stop that
//   would sit through the market; Max rejects a signal whose structure stop is
//   further away than the account should risk.
//
// =============================================================================
// DELIBERATELY ABSENT  (present in OFSignalAutoTrader, removed here)
// =============================================================================
//   Trailing Bars / ratcheting bar trail   — excluded for this build
//   Max Entries Per Day                    — removed
//   Max Daily Loss                         — removed
//   Long / Short enables                   — both directions always trade
//   Two study references                   — direction is data now
//   One shared stop price                  — replaced by per-leg stops (v1.1)
//
// SUBGRAPHS
//   SG1 Entry Marker   SG2 Exit Marker   SG3 Stop Price (nearest leg)
//
// PERSISTENT INTS
//   1 PosDir, 2 PosQty, 3 SigBar, 4 EntBar, 7 NumLegs, 8 TradeDate,
//   9 ConsumeWindowPending, 10 LastEntBarL, 11 LastEntBarS, 12 RecalcHandled,
//   13 SessionFlatDate,
//   100+i EntryOrdID[i], 120+i StopOrdID[i], 140+i LegQty[i],
//   160+i LegBE[i], 180+i LegLock[i],
//   2000+j Consumed[LONG][j], 2100+j Consumed[SHORT][j]
//
// PERSISTENT FLOATS
//   3 Realized,
//   100+i StopSent[i], 120+i LegStop[i], 140+i LegEntry[i]
// =============================================================================

const int AAT_MAX_LEGS    = 8;    // stacked entries; each carries one child stop
const int AAT_CONSUMED_N  = 32;   // consumed-bar ring per side

const int AAT_LVL_1    = 1;
const int AAT_LVL_2    = 2;
const int AAT_LVL_3    = 3;
const int AAT_LVL_CONF = 4;

// =============================================================================
SCSFExport scsf_AggregatorAutoTrader(SCStudyInterfaceRef sc)
{
	SCSubgraphRef sg_Entry = sc.Subgraph[0];
	SCSubgraphRef sg_Exit  = sc.Subgraph[1];
	SCSubgraphRef sg_Stop  = sc.Subgraph[2];

	SCInputRef In_Enable        = sc.Input[0];
	SCInputRef In_SendToService = sc.Input[1];

	SCInputRef In_EventRef      = sc.Input[2];
	SCInputRef In_UseEvent      = sc.Input[3];
	SCInputRef In_ArrowRef      = sc.Input[4];
	SCInputRef In_UseArrows     = sc.Input[5];
	SCInputRef In_ConfRef       = sc.Input[6];
	SCInputRef In_UseConf       = sc.Input[7];

	SCInputRef In_ScanBars      = sc.Input[8];
	SCInputRef In_ClusterBars   = sc.Input[9];
	SCInputRef In_SignalAnchor  = sc.Input[10];

	SCInputRef In_Qty1          = sc.Input[11];
	SCInputRef In_Qty2          = sc.Input[12];
	SCInputRef In_Qty3          = sc.Input[13];
	SCInputRef In_QtyConf       = sc.Input[14];

	SCInputRef In_ReSignalMode  = sc.Input[15];
	SCInputRef In_AddMinBars    = sc.Input[16];
	SCInputRef In_MaxPosition   = sc.Input[17];

	SCInputRef In_OpposingExit  = sc.Input[18];
	SCInputRef In_OpposingMin   = sc.Input[19];

	SCInputRef In_StopStructBars= sc.Input[20];
	SCInputRef In_StopOffset    = sc.Input[21];
	SCInputRef In_MinStopDist   = sc.Input[22];
	SCInputRef In_MaxStopDist   = sc.Input[23];

	SCInputRef In_BETicks       = sc.Input[24];
	SCInputRef In_ProfitTicks   = sc.Input[25];
	SCInputRef In_ProfitOffset  = sc.Input[26];

	SCInputRef In_SessionStart  = sc.Input[27];
	SCInputRef In_SessionEnd    = sc.Input[28];
	SCInputRef In_OnRecalc      = sc.Input[29];
	SCInputRef In_ShowPanel     = sc.Input[30];
	SCInputRef In_LogSkips      = sc.Input[31];

	// APPENDED, not inserted next to In_EventRef. SierraChart stores an input's
	// value against its INDEX in the chart's saved settings, so inserting here
	// would silently re-map every input after it on charts already configured.
	// Dialog order is the price paid for that.
	SCInputRef In_EventRefShort = sc.Input[32];

	// =========================================================================
	if (sc.SetDefaults)
	{
		sc.GraphName        = "Aggregator Auto Trader v1.1";
		sc.StudyDescription =
			"Auto-trades Trigger Aggregator V2. Every entry leg is its own "
			"position with its own attached stop, BE and profit lock. Entry on "
			"episode events / arrows / confluence; exits on the leg's own stop, "
			"an opposing signal, or Session End. AutoLoop=0.";

		sc.AutoLoop      = 0;
		sc.GraphRegion   = 0;
		sc.FreeDLL       = 0;
		sc.DrawZeros     = 0;
		sc.UpdateAlways  = 1;
		sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL;

		sc.MaintainTradeStatisticsAndTradesData = 1;
		sc.AllowMultipleEntriesInSameDirection  = 1;
		sc.SupportAttachedOrdersForTrading      = 0;   // programmatic ones still work
		sc.CancelAllOrdersOnEntriesAndReversals = 0;
		sc.AllowOppositeEntryWithOpposingPositionOrOrders = 0;

		sg_Entry.Name         = "Entry";
		sg_Entry.DrawStyle    = DRAWSTYLE_POINT;
		sg_Entry.LineWidth    = 4;
		sg_Entry.PrimaryColor = RGB(0, 220, 220);
		sg_Entry.DrawZeros    = 0;

		sg_Exit.Name         = "Exit";
		sg_Exit.DrawStyle    = DRAWSTYLE_POINT;
		sg_Exit.LineWidth    = 4;
		sg_Exit.PrimaryColor = RGB(240, 240, 0);
		sg_Exit.DrawZeros    = 0;

		sg_Stop.Name         = "Stop Price";
		sg_Stop.DrawStyle    = DRAWSTYLE_DASH;
		sg_Stop.LineWidth    = 1;
		sg_Stop.PrimaryColor = RGB(220, 80, 80);
		sg_Stop.DrawZeros    = 0;

		In_Enable.Name = "Enable Trading";
		In_Enable.SetYesNo(0);              // safe default: compute only

		In_SendToService.Name = "Send Orders To Trade Service (No = local simulation)";
		In_SendToService.SetYesNo(0);

		// Defaults are 0-BASED API indexes. The settings dialog shows subgraphs
		// 1-based, so 13 displays as SG14. This codebase has tripped on that.
		In_EventRef.Name = "Episode Event LONG Subgraph (Trigger Aggregator SG14)";
		In_EventRef.SetStudySubgraphValues(0, 13);
		In_UseEvent.Name = "Use Episode Event";
		In_UseEvent.SetYesNo(1);

		In_ArrowRef.Name = "Arrows Base Subgraph (SG1; reads SG1-SG6 by offset)";
		In_ArrowRef.SetStudySubgraphValues(0, 0);
		In_UseArrows.Name = "Use Arrows (all levels, both directions)";
		In_UseArrows.SetYesNo(1);

		In_ConfRef.Name = "Confluence Base Subgraph (SG17; reads SG17-SG18 by offset)";
		In_ConfRef.SetStudySubgraphValues(0, 16);
		In_UseConf.Name = "Use Confluence Signals";
		In_UseConf.SetYesNo(1);

		In_ScanBars.Name = "Signal Scan Bars (how far back a late signal still counts)";
		In_ScanBars.SetInt(7);
		In_ScanBars.SetIntLimits(1, 100);

		In_ClusterBars.Name = "Signal Cluster Bars (+/- N bars collapse to ONE entry; 0 = off)";
		In_ClusterBars.SetInt(1);
		In_ClusterBars.SetIntLimits(0, 50);

		In_SignalAnchor.Name = "Signal Anchor (which signal in the window to trade)";
		In_SignalAnchor.SetCustomInputStrings("Oldest In Window;Newest In Window");
		In_SignalAnchor.SetCustomInputIndex(0);

		// Quantity IS the on/off switch, and it also RANKS competing signals in
		// a scan window. All default to 1: a flat table is the only setting
		// where level-vs-level comparison is undistorted. Every level is
		// therefore live by default — the safety is Enable Trading = No.
		In_Qty1.Name = "L1 Quantity (0 = level disabled)";
		In_Qty1.SetInt(1);   In_Qty1.SetIntLimits(0, 100);
		In_Qty2.Name = "L2 Quantity (0 = level disabled)";
		In_Qty2.SetInt(1);   In_Qty2.SetIntLimits(0, 100);
		In_Qty3.Name = "L3 Quantity (0 = level disabled)";
		In_Qty3.SetInt(1);   In_Qty3.SetIntLimits(0, 100);
		In_QtyConf.Name = "Confluence Quantity (0 = level disabled)";
		In_QtyConf.SetInt(1); In_QtyConf.SetIntLimits(0, 100);

		In_ReSignalMode.Name = "Re-Signal Mode (signal in the direction already held)";
		In_ReSignalMode.SetCustomInputStrings("Ignore While In Position;Add Entry");
		In_ReSignalMode.SetCustomInputIndex(0);

		In_AddMinBars.Name = "Add Entry Min Bars (spacing between stacked entries)";
		In_AddMinBars.SetInt(3);
		In_AddMinBars.SetIntLimits(0, 200);

		In_MaxPosition.Name = "Max Position Size (contracts)";
		In_MaxPosition.SetInt(3);
		In_MaxPosition.SetIntLimits(1, 100);

		In_OpposingExit.Name = "Opposing Signal Exit (flatten on a signal the other way)";
		In_OpposingExit.SetYesNo(1);

		In_OpposingMin.Name = "Opposing Min Level (1-3 arrows, 4 = confluence only)";
		In_OpposingMin.SetInt(1);
		In_OpposingMin.SetIntLimits(1, 4);

		// Default 1 = the signal bar itself, matching "attach the stop to the
		// arrow bar extreme". Widen it if a single range bar's extreme sits too
		// close to be a meaningful structural level.
		In_StopStructBars.Name = "Stop Structure Bars (N-bar extreme ending AT the signal bar)";
		In_StopStructBars.SetInt(1);
		In_StopStructBars.SetIntLimits(1, 50);

		In_StopOffset.Name = "Stop Offset (ticks beyond the extreme)";
		In_StopOffset.SetInt(4);
		In_StopOffset.SetIntLimits(0, 200);

		In_MinStopDist.Name = "Min Stop Distance (points; widens a stop that sits through the market)";
		In_MinStopDist.SetFloat(3.0f);

		In_MaxStopDist.Name = "Max Stop Distance (points; signal rejected if structure stop is wider)";
		In_MaxStopDist.SetFloat(15.0f);

		// Per leg, measured against THAT leg's own entry price.
		In_BETicks.Name = "Move Stop To BE After (ticks of open profit on the leg; 0 = off)";
		In_BETicks.SetInt(0);
		In_BETicks.SetIntLimits(0, 2000);

		In_ProfitTicks.Name = "Move Stop To Profit After (ticks of open profit on the leg; 0 = off)";
		In_ProfitTicks.SetInt(0);
		In_ProfitTicks.SetIntLimits(0, 2000);

		In_ProfitOffset.Name = "Profit Lock Offset (ticks beyond the leg's entry the stop lands)";
		In_ProfitOffset.SetInt(4);
		In_ProfitOffset.SetIntLimits(1, 2000);

		// HMS_TIME is deprecated in scdatetime.h:985; using the replacement so
		// the build output stays free of warning noise that would hide errors.
		In_SessionStart.Name = "Session Start";
		In_SessionStart.SetTime(SCDateTime(9, 30, 0, 0).GetTime());
		In_SessionEnd.Name = "Session End (entries stop; open legs are flattened)";
		In_SessionEnd.SetTime(SCDateTime(16, 0, 0, 0).GetTime());

		In_OnRecalc.Name = "On Recalculation";
		In_OnRecalc.SetCustomInputStrings("Adopt Existing Position;Flatten;Ignore");
		In_OnRecalc.SetCustomInputIndex(0);

		In_ShowPanel.Name = "Show Status Panel";
		In_ShowPanel.SetYesNo(1);

		In_LogSkips.Name = "Log Skipped Signals (why a signal did not trade)";
		In_LogSkips.SetYesNo(1);

		// Unset by default = one signed subgraph, exactly as before. Point this
		// at the short episode subgraph when the aggregator splits the sides;
		// the LONG input then supplies longs only.
		In_EventRefShort.Name = "Episode Event SHORT Subgraph (leave unset if LONG is one signed SG)";
		In_EventRefShort.SetStudySubgraphValues(0, 0);

		return;
	}

	// =========================================================================
	// SETUP
	// =========================================================================
	const int totalBars = sc.ArraySize;
	if (totalBars < 2)
		return;
	const int lastBar = totalBars - 1;

	sc.SendOrdersToTradeService = In_SendToService.GetYesNo() != 0;

	const bool  tradingEnabled = In_Enable.GetYesNo() != 0;
	const int   scanBars       = In_ScanBars.GetInt();
	const int   clusterBars    = In_ClusterBars.GetInt();
	const bool  anchorOldest   = (In_SignalAnchor.GetIndex() == 0);
	const bool  addEntries     = (In_ReSignalMode.GetIndex() == 1);
	const int   addMinBars     = In_AddMinBars.GetInt();
	const int   maxPosition    = In_MaxPosition.GetInt();
	const bool  opposingExit   = In_OpposingExit.GetYesNo() != 0;
	const int   opposingMin    = In_OpposingMin.GetInt();
	const int   structBars     = In_StopStructBars.GetInt();
	const float stopOffset     = In_StopOffset.GetInt() * sc.TickSize;
	const float minStopDist    = In_MinStopDist.GetFloat();
	const float maxStopDist    = In_MaxStopDist.GetFloat();
	const float beDist         = In_BETicks.GetInt()     * sc.TickSize;
	const float profitDist     = In_ProfitTicks.GetInt() * sc.TickSize;
	const float profitOffset   = In_ProfitOffset.GetInt()* sc.TickSize;
	const bool  logSkips       = In_LogSkips.GetYesNo() != 0;

	const int levelQty[5] = { 0, In_Qty1.GetInt(), In_Qty2.GetInt(),
	                          In_Qty3.GetInt(), In_QtyConf.GetInt() };

	// ---------------- persistent state ----------------
	int&   PosDir      = sc.GetPersistentInt(1);
	int&   PosQty      = sc.GetPersistentInt(2);   // derived: sum of LegQty
	int&   SigBar      = sc.GetPersistentInt(3);
	int&   EntBar      = sc.GetPersistentInt(4);
	int&   NumLegs     = sc.GetPersistentInt(7);
	int&   TradeDate   = sc.GetPersistentInt(8);
	int&   ConsumePend = sc.GetPersistentInt(9);
	int&   LastEntBarL = sc.GetPersistentInt(10);
	int&   LastEntBarS = sc.GetPersistentInt(11);
	int&   RecalcDone  = sc.GetPersistentInt(12);
	int&   SessFlatDay = sc.GetPersistentInt(13);

	float& Realized    = sc.GetPersistentFloat(3);

	// ---------------- consumed-signal set ----------------
	// Membership, NOT a watermark. A signal can settle onto a bar OLDER than
	// one already acted on (decisions.md 2026-07-23), and a watermark makes
	// every such signal permanently invisible at any Scan Bars value.
	int Consumed[2][AAT_CONSUMED_N];
	for (int j = 0; j < AAT_CONSUMED_N; ++j)
	{
		Consumed[0][j] = sc.GetPersistentInt(2000 + j);
		Consumed[1][j] = sc.GetPersistentInt(2100 + j);
	}
	auto IsConsumed = [&](int side, int bar) -> bool
	{
		for (int j = 0; j < AAT_CONSUMED_N; ++j)
			if (Consumed[side][j] == bar) return true;
		return false;
	};
	auto MarkConsumed = [&](int side, int bar)
	{
		if (bar < 0 || IsConsumed(side, bar)) return;
		int slot = -1, oldest = 0;
		for (int j = 0; j < AAT_CONSUMED_N; ++j)
		{
			if (Consumed[side][j] < 0) { slot = j; break; }
			if (Consumed[side][j] < Consumed[side][oldest]) oldest = j;
		}
		Consumed[side][(slot >= 0) ? slot : oldest] = bar;
	};
	auto SaveConsumed = [&]()
	{
		for (int j = 0; j < AAT_CONSUMED_N; ++j)
		{
			sc.GetPersistentInt(2000 + j) = Consumed[0][j];
			sc.GetPersistentInt(2100 + j) = Consumed[1][j];
		}
	};

	// ---------------- day roll ----------------
	const int today = sc.GetTradingDayDate(sc.BaseDateTimeIn[lastBar]);
	if (today != TradeDate)
	{
		TradeDate = today;
		Realized  = 0.0f;
		for (int j = 0; j < AAT_CONSUMED_N; ++j)
		{ Consumed[0][j] = -1; Consumed[1][j] = -1; }
		LastEntBarL = -100000;
		LastEntBarS = -100000;
	}

	// =========================================================================
	// SIGNAL SOURCES
	// Read by OFFSET from each reference's own subgraph, never by baked index.
	// =========================================================================
	SCFloatArray EventArr;
	SCFloatArray EventShortArr;
	SCFloatArray ArrowArr[6];
	SCFloatArray ConfArr[2];

	const bool useEvent  = In_UseEvent.GetYesNo()  != 0;
	const bool useArrows = In_UseArrows.GetYesNo() != 0;
	const bool useConf   = In_UseConf.GetYesNo()   != 0;

	// "Split sides" is inferred, not a separate Yes/No: a second reference that
	// points somewhere real and DIFFERENT is the only thing that could mean it.
	// Pointing both at the same study+subgraph is treated as not split, so a
	// copy-paste of the long reference cannot double-count one signed SG.
	const bool eventSplit = useEvent &&
		In_EventRefShort.GetStudyID() != 0 &&
		!(In_EventRefShort.GetStudyID()       == In_EventRef.GetStudyID() &&
		  In_EventRefShort.GetSubgraphIndex() == In_EventRef.GetSubgraphIndex());

	if (useEvent && In_EventRef.GetStudyID() != 0)
		sc.GetStudyArrayUsingID(In_EventRef.GetStudyID(),
		                        In_EventRef.GetSubgraphIndex(), EventArr);

	if (eventSplit)
		sc.GetStudyArrayUsingID(In_EventRefShort.GetStudyID(),
		                        In_EventRefShort.GetSubgraphIndex(), EventShortArr);

	if (useArrows && In_ArrowRef.GetStudyID() != 0)
	{
		const int base = In_ArrowRef.GetSubgraphIndex();
		for (int g = 0; g < 6; ++g)
			sc.GetStudyArrayUsingID(In_ArrowRef.GetStudyID(), base + g, ArrowArr[g]);
	}

	if (useConf && In_ConfRef.GetStudyID() != 0)
	{
		const int base = In_ConfRef.GetSubgraphIndex();
		for (int g = 0; g < 2; ++g)
			sc.GetStudyArrayUsingID(In_ConfRef.GetStudyID(), base + g, ConfArr[g]);
	}

	const bool anyReady = (EventArr.GetArraySize()      > 0) ||
	                      (EventShortArr.GetArraySize() > 0) ||
	                      (ArrowArr[0].GetArraySize()   > 0) ||
	                      (ConfArr[0].GetArraySize()    > 0);

	// Resolve a bar to at most ONE (side, level). All three sources derive from
	// the aggregator's single signed level, so they cannot disagree about
	// direction — only about whether a given bar carries a signal at all.
	// Highest level wins when sources differ, since confluence (4) is the
	// coarsest statement about the bar.
	auto SignalAt = [&](int b, int& outSide, int& outLevel) -> bool
	{
		outSide = -1; outLevel = 0;

		if (useEvent && !eventSplit && EventArr.GetArraySize() > b)
		{
			// One signed subgraph: the sign IS the direction.
			const int ev = static_cast<int>(EventArr[b]);
			if (ev != 0)
			{
				outSide  = (ev > 0) ? 0 : 1;
				outLevel = (ev > 0) ? ev : -ev;
			}
		}
		else if (useEvent && eventSplit)
		{
			// Split subgraphs: the SOURCE is the direction and the sign carries
			// no information, so the magnitude alone gives the level. A short SG
			// that publishes -2 and one that publishes +2 both mean short L2.
			if (EventArr.GetArraySize() > b)
			{
				int L = static_cast<int>(EventArr[b]);
				if (L < 0) L = -L;
				if (L > outLevel) { outLevel = L; outSide = 0; }
			}
			if (EventShortArr.GetArraySize() > b)
			{
				int L = static_cast<int>(EventShortArr[b]);
				if (L < 0) L = -L;
				if (L > outLevel) { outLevel = L; outSide = 1; }
			}
		}

		if (useArrows && ArrowArr[0].GetArraySize() > b)
		{
			for (int g = 0; g < 6; ++g)
			{
				if (ArrowArr[g].GetArraySize() <= b || ArrowArr[g][b] == 0.0f)
					continue;
				const int s = (g < 3) ? 0 : 1;
				const int L = (g % 3) + 1;
				if (L > outLevel) { outLevel = L; outSide = s; }
			}
		}

		if (useConf && ConfArr[0].GetArraySize() > b)
		{
			for (int g = 0; g < 2; ++g)
			{
				if (ConfArr[g].GetArraySize() <= b || ConfArr[g][b] == 0.0f)
					continue;
				if (AAT_LVL_CONF > outLevel)
				{ outLevel = AAT_LVL_CONF; outSide = (g == 0) ? 0 : 1; }
			}
		}

		return (outLevel > 0 && outSide >= 0);
	};

	// =========================================================================
	// BROKER STATE
	// =========================================================================
	s_SCPositionData Position;
	sc.GetTradePosition(Position);
	const int posQty = static_cast<int>(Position.PositionQuantity);

	// ---------------- legs ----------------
	// Each leg is a self-contained position: its own entry, quantity, stop and
	// ladder state. They share only a direction.
	int   EntryOrdID[AAT_MAX_LEGS], StopOrdID[AAT_MAX_LEGS], LegQty[AAT_MAX_LEGS];
	int   LegBE[AAT_MAX_LEGS], LegLock[AAT_MAX_LEGS];
	float StopSent[AAT_MAX_LEGS], LegStop[AAT_MAX_LEGS], LegEntry[AAT_MAX_LEGS];
	for (int i = 0; i < AAT_MAX_LEGS; ++i)
	{
		EntryOrdID[i] = sc.GetPersistentInt(100 + i);
		StopOrdID[i]  = sc.GetPersistentInt(120 + i);
		LegQty[i]     = sc.GetPersistentInt(140 + i);
		LegBE[i]      = sc.GetPersistentInt(160 + i);
		LegLock[i]    = sc.GetPersistentInt(180 + i);
		StopSent[i]   = sc.GetPersistentFloat(100 + i);
		LegStop[i]    = sc.GetPersistentFloat(120 + i);
		LegEntry[i]   = sc.GetPersistentFloat(140 + i);
	}
	auto SaveLegs = [&]()
	{
		for (int i = 0; i < AAT_MAX_LEGS; ++i)
		{
			sc.GetPersistentInt(100 + i)   = EntryOrdID[i];
			sc.GetPersistentInt(120 + i)   = StopOrdID[i];
			sc.GetPersistentInt(140 + i)   = LegQty[i];
			sc.GetPersistentInt(160 + i)   = LegBE[i];
			sc.GetPersistentInt(180 + i)   = LegLock[i];
			sc.GetPersistentFloat(100 + i) = StopSent[i];
			sc.GetPersistentFloat(120 + i) = LegStop[i];
			sc.GetPersistentFloat(140 + i) = LegEntry[i];
		}
	};
	auto ClearLeg = [&](int i)
	{
		EntryOrdID[i] = 0; StopOrdID[i] = 0; LegQty[i] = 0;
		LegBE[i] = 0; LegLock[i] = 0;
		StopSent[i] = 0.0f; LegStop[i] = 0.0f; LegEntry[i] = 0.0f;
	};
	// Legs are kept packed: removing one shifts the tail down so NumLegs is
	// always a contiguous count and the entry path can append at NumLegs.
	auto RemoveLeg = [&](int i)
	{
		for (int k = i; k < NumLegs - 1; ++k)
		{
			EntryOrdID[k] = EntryOrdID[k + 1]; StopOrdID[k] = StopOrdID[k + 1];
			LegQty[k]     = LegQty[k + 1];     LegBE[k]     = LegBE[k + 1];
			LegLock[k]    = LegLock[k + 1];    StopSent[k]  = StopSent[k + 1];
			LegStop[k]    = LegStop[k + 1];    LegEntry[k]  = LegEntry[k + 1];
		}
		if (NumLegs > 0) { ClearLeg(NumLegs - 1); --NumLegs; }
	};
	auto SumLegQty = [&]() -> int
	{
		int q = 0;
		for (int i = 0; i < NumLegs; ++i) q += LegQty[i];
		return q;
	};
	// Display only. Real risk lives per leg; this is what goes on the panel.
	auto AvgEntryPx = [&]() -> float
	{
		const int q = SumLegQty();
		if (q <= 0) return 0.0f;
		float acc = 0.0f;
		for (int i = 0; i < NumLegs; ++i) acc += LegEntry[i] * LegQty[i];
		return acc / q;
	};
	auto ClearPosition = [&]()
	{
		PosDir = 0; PosQty = 0; NumLegs = 0;
		SigBar = -1; EntBar = -1;
		for (int i = 0; i < AAT_MAX_LEGS; ++i) ClearLeg(i);
	};

	// =========================================================================
	// RECALCULATION
	// The attached stop belongs to SierraChart, not to this study — it survives
	// a recalc untouched. Only our VIEW of it is lost, so adopt must never
	// cancel stored stop IDs: under attached orders that strips a live position
	// of a working stop that cannot be replaced, because a stop can only attach
	// to an entry and that entry has already filled (decisions.md 2026-07-23).
	// =========================================================================
	const bool isFullRecalc = (sc.UpdateStartIndex == 0) || sc.IsFullRecalculation;
	if (isFullRecalc)
		RecalcDone = 0;

	if (!RecalcDone)
	{
		RecalcDone = 1;
		const int mode = In_OnRecalc.GetIndex();

		if (posQty == 0)
		{
			ClearPosition();
		}
		else if (mode == 1)                      // Flatten
		{
			if (tradingEnabled) sc.FlattenAndCancelAllOrders();
			ClearPosition();
		}
		else if (mode == 0)                      // Adopt
		{
			ClearPosition();
			PosDir = (posQty > 0) ? 1 : -1;
			EntBar = lastBar;

			const float avgPx = static_cast<float>(Position.AveragePrice);
			const int   absQty = (posQty > 0) ? posQty : -posQty;

			// Take over whatever stops are actually working. Do NOT cancel.
			// Each working stop becomes one leg; a leg reconstructed this way
			// has no known entry price of its own, so it inherits the position
			// average — the only figure the broker actually gives us. Its
			// ladder starts fresh from there.
			int found = 0;
			s_SCTradeOrder TO;
			for (int idx = 0; sc.GetOrderByIndex(idx, TO) != SCTRADING_ORDER_ERROR; ++idx)
			{
				if (TO.OrderTypeAsInt != SCT_ORDERTYPE_STOP) continue;
				// IsWorking() covers open AND the pending-child states a
				// freshly attached stop passes through; enumerating status
				// codes by hand would miss SCT_OSC_PENDING_CHILD_CLIENT /
				// _SERVER and adopt would report an unprotected position.
				if (!TO.IsWorking())                         continue;
				if (found >= AAT_MAX_LEGS)                   break;

				StopOrdID[found] = TO.InternalOrderID;
				LegStop[found]   = static_cast<float>(TO.Price1);
				StopSent[found]  = static_cast<float>(TO.Price1);
				EntryOrdID[found]= 0;   // parent already filled; child stands alone
				LegEntry[found]  = avgPx;
				LegQty[found]    = static_cast<int>(TO.OrderQuantity);
				if (LegQty[found] <= 0) LegQty[found] = 0;
				++found;
			}
			NumLegs = found;

			// The broker's quantity wins over the sum of the stop quantities:
			// an adopted position may be partly unprotected, and pretending
			// otherwise would under-report size to Max Position Size.
			int stopQty = SumLegQty();
			if (found > 0 && stopQty != absQty)
			{
				// Push the difference onto the first leg so PosQty reconciles.
				LegQty[0] += (absQty - stopQty);
				if (LegQty[0] < 0) LegQty[0] = 0;
			}
			PosQty = (found > 0) ? SumLegQty() : absQty;

			SCString m;
			if (found == 0)
			{
				PosQty = absQty;
				m.Format("Aggregator Auto Trader: *** ADOPTED %d @ %.2f WITH NO WORKING STOP — POSITION IS UNPROTECTED ***",
					posQty, avgPx);
			}
			else
			{
				m.Format("Aggregator Auto Trader: adopted %d @ %.2f as %d leg(s), stops %.2f..%.2f",
					posQty, avgPx, found, LegStop[0], LegStop[found - 1]);
			}
			sc.AddMessageToLog(m, (found == 0) ? 1 : 0);

			// Signals already in the window were decided before the recalc.
			ConsumePend = 1;
		}
	}

	// =========================================================================
	// LEG RECONCILIATION — legs stop out independently, so this is a NORMAL
	// path, not an error path. A leg is closed when its child stop is no longer
	// working; the fill price the broker reports is the honest P&L figure.
	// =========================================================================
	if (PosDir != 0)
	{
		for (int i = NumLegs - 1; i >= 0; --i)
		{
			if (StopOrdID[i] == 0)
				continue;                        // child not live yet — not a fill

			s_SCTradeOrder SO;
			const bool gone = (sc.GetOrderByOrderID(StopOrdID[i], SO) == SCTRADING_ORDER_ERROR);
			if (!gone && SO.IsWorking())
				continue;                        // still protecting the leg

			const bool filled = !gone && (SO.FilledQuantity > 0);
			const float exitPx = filled
				? static_cast<float>(SO.AvgFillPrice)
				: sc.Close[lastBar];             // cancelled/unknown: mark to market

			Realized += (exitPx - LegEntry[i]) * PosDir * LegQty[i];
			sg_Exit[lastBar] = exitPx;

			SCString m;
			m.Format("Aggregator Auto Trader: leg %d CLOSED %d @ %.2f (entry %.2f, %s), realized %.2f",
				i, LegQty[i], exitPx, LegEntry[i],
				filled ? "stop filled" : "stop gone — marked to market", Realized);
			sc.AddMessageToLog(m, filled ? 0 : 1);

			RemoveLeg(i);
		}
		PosQty = SumLegQty();
		if (NumLegs == 0 || PosQty <= 0)
			ClearPosition();
	}

	// Broker is the authority. If it says flat and we still hold legs, the
	// legs are stale — book them at the close rather than losing the P&L.
	if (posQty == 0 && PosDir != 0)
	{
		for (int i = 0; i < NumLegs; ++i)
			Realized += (sc.Close[lastBar] - LegEntry[i]) * PosDir * LegQty[i];
		sg_Exit[lastBar] = sc.Close[lastBar];
		sc.AddMessageToLog("Aggregator Auto Trader: broker flat with legs open — reconciled at close", 1);
		ClearPosition();
	}

	// =========================================================================
	// STOP MANAGEMENT — one stop per leg, each moving on its own schedule
	// =========================================================================
	// StopSent[i] is read back from the CHILD ORDER, never assumed. The child
	// does not exist at the instant the entry fills, so a BE that triggers in
	// that gap has no order to modify; if resolve then recorded the intended
	// price, StopSent would equal LegStop and the move would never be sent —
	// the stop would rest at the structure level while the panel showed BE.
	auto ResolveStopIDs = [&]()
	{
		for (int i = 0; i < NumLegs; ++i)
		{
			if (StopOrdID[i] != 0 || EntryOrdID[i] == 0)
				continue;
			s_SCTradeOrder Parent;
			if (sc.GetOrderByOrderID(EntryOrdID[i], Parent) == SCTRADING_ORDER_ERROR)
				continue;
			if (Parent.StopChildInternalOrderID == 0)
				continue;
			StopOrdID[i] = Parent.StopChildInternalOrderID;

			s_SCTradeOrder Child;
			if (sc.GetOrderByOrderID(StopOrdID[i], Child) != SCTRADING_ORDER_ERROR)
				StopSent[i] = static_cast<float>(Child.Price1);
			else
				StopSent[i] = 0.0f;   // unknown resting price: force the next push

			SCString m;
			m.Format("Aggregator Auto Trader: leg %d attached stop live at %.2f, target %.2f (order %u)",
				i, StopSent[i], LegStop[i], (unsigned)StopOrdID[i]);
			sc.AddMessageToLog(m, 0);
		}
	};

	// Throttled: AutoLoop = 0 means this runs on EVERY tick, and an
	// unconditional ModifyOrder would re-send the same price hundreds of times
	// a second and freeze SierraChart. Only a CHANGED, IMPROVING price is sent.
	auto PushStops = [&]()
	{
		if (!tradingEnabled || PosDir == 0)
			return;
		for (int i = 0; i < NumLegs; ++i)
		{
			if (StopOrdID[i] == 0)          continue;
			if (LegStop[i] == 0.0f)         continue;
			if (LegStop[i] == StopSent[i])  continue;

			// StopSent == 0 means "resting price unknown" — send unconditionally.
			if (StopSent[i] != 0.0f)
			{
				const bool improves = (PosDir > 0) ? (LegStop[i] > StopSent[i])
				                                   : (LegStop[i] < StopSent[i]);
				if (!improves) continue;
			}

			s_SCNewOrder Mod;
			Mod.InternalOrderID = StopOrdID[i];
			Mod.Price1 = LegStop[i];
			if (sc.ModifyOrder(Mod) > 0)
			{
				StopSent[i] = LegStop[i];
			}
			else
			{
				SCString m;
				m.Format("Aggregator Auto Trader: stop MODIFY FAILED leg %d to %.2f — still resting at %.2f",
					i, LegStop[i], StopSent[i]);
				sc.AddMessageToLog(m, 1);
			}
		}
	};

	auto FlattenAll = [&](const char* why)
	{
		if (PosDir == 0) return;
		if (tradingEnabled)
			sc.FlattenAndCancelAllOrders();

		const float exitPx = sc.Close[lastBar];
		const int   qty    = SumLegQty();
		for (int i = 0; i < NumLegs; ++i)
			Realized += (exitPx - LegEntry[i]) * PosDir * LegQty[i];
		sg_Exit[lastBar] = exitPx;

		SCString m;
		m.Format("Aggregator Auto Trader: FLAT (%s) %d in %d leg(s) @ %.2f, realized %.2f",
			why, qty, NumLegs, exitPx, Realized);
		sc.AddMessageToLog(m, 0);
		ClearPosition();
	};

	ResolveStopIDs();

	// ---- BE and profit locks, PER LEG ---------------------------------------
	// Each leg is judged against ITS OWN entry price, so an add never drags an
	// older leg's breakeven around and an older leg's ratchet never constrains
	// a new one. Each fires once per leg; a leg's price only moves in favour.
	if (PosDir != 0)
	{
		for (int i = 0; i < NumLegs; ++i)
		{
			if (LegEntry[i] == 0.0f) continue;
			const float openProfit = (sc.Close[lastBar] - LegEntry[i]) * PosDir;

			if (!LegLock[i] && profitDist > 0 && openProfit >= profitDist)
			{
				const float want = LegEntry[i] + PosDir * profitOffset;
				const bool improves = (LegStop[i] == 0.0f) ? true
				                    : ((PosDir > 0) ? (want > LegStop[i]) : (want < LegStop[i]));
				if (improves)
				{
					LegStop[i] = want; LegLock[i] = 1; LegBE[i] = 1;
					SCString m; m.Format("Aggregator Auto Trader: leg %d PROFIT LOCK -> %.2f", i, want);
					sc.AddMessageToLog(m, 0);
				}
			}
			else if (!LegBE[i] && beDist > 0 && openProfit >= beDist)
			{
				const float want = LegEntry[i];
				const bool improves = (LegStop[i] == 0.0f) ? true
				                    : ((PosDir > 0) ? (want > LegStop[i]) : (want < LegStop[i]));
				if (improves)
				{
					LegStop[i] = want; LegBE[i] = 1;
					SCString m; m.Format("Aggregator Auto Trader: leg %d BREAKEVEN -> %.2f", i, want);
					sc.AddMessageToLog(m, 0);
				}
			}
		}

		PushStops();
	}

	// =========================================================================
	// SESSION GATE
	// =========================================================================
	const int nowT   = sc.BaseDateTimeIn[lastBar].GetTime();
	const int startT = In_SessionStart.GetTime();
	const int endT   = In_SessionEnd.GetTime();
	const bool inSession = (startT <= endT) ? (nowT >= startT && nowT <= endT)
	                                        : (nowT >= startT || nowT <= endT);

	// Session End flattens, it does not merely stop entries. Without this an
	// entry at 15:59 rides overnight while the day roll below zeroes Realized
	// and the consumed set under it. Once per trading day: SessFlatDay stops a
	// re-flatten from firing on every subsequent out-of-session tick, and the
	// date comparison resets it at the roll without extra bookkeeping.
	if (!inSession && PosDir != 0 && SessFlatDay != today)
	{
		SessFlatDay = today;
		FlattenAll("session end");
	}

	// =========================================================================
	// SIGNAL SCAN
	// =========================================================================
	auto LogSkip = [&](int side, int bar, int level, const char* why)
	{
		if (!logSkips) return;
		SCString m;
		m.Format("Aggregator Auto Trader: %s L%d at bar %d (%d back) SKIPPED — %s",
			(side == 0) ? "LONG" : "SHORT", level, bar, lastBar - bar, why);
		sc.AddMessageToLog(m, 0);
	};

	if (anyReady)
	{
		int floorBar = lastBar - scanBars + 1;
		if (floorBar < 0) floorBar = 0;

		for (int side = 0; side < 2; ++side)
		{
			for (int j = 0; j < AAT_CONSUMED_N; ++j)
				if (Consumed[side][j] >= 0 && Consumed[side][j] < floorBar)
					Consumed[side][j] = -1;
		}

		// Rescan the FULL window every pass. Only membership in the consumed
		// set may exclude a bar — never position, because a signal can land
		// behind one already acted on.
		int bestBar = -1, bestLevel = 0, bestQty = 0, bestSide = -1;
		int firedBars[AAT_CONSUMED_N][2];
		int firedCount = 0;

		for (int b = floorBar; b <= lastBar; ++b)
		{
			int s = -1, L = 0;
			if (!SignalAt(b, s, L))
				continue;
			// levelQty has 5 slots. L comes from ANOTHER study's subgraph, so a
			// value outside 1..4 (a revision adding a level, a subgraph pointed
			// at the wrong place) would read past the array rather than be
			// ignored. Bound it here, once, for every source.
			if (L < 1 || L > AAT_LVL_CONF)
				continue;
			if (levelQty[L] == 0)
				continue;

			if (firedCount < AAT_CONSUMED_N)
			{ firedBars[firedCount][0] = s; firedBars[firedCount][1] = b; ++firedCount; }

			if (IsConsumed(s, b))
				continue;

			// Quantity ranks first; ties break by the anchor rule. With a flat
			// quantity table every comparison IS a tie, so the anchor rule
			// alone picks the bar — which is why it is an input and not a
			// hardcoded "newest".
			bool better = false;
			if (levelQty[L] > bestQty)
				better = true;
			else if (levelQty[L] == bestQty)
				better = (bestBar < 0) ? true
				       : (anchorOldest ? (b < bestBar) : (b >= bestBar));

			if (better)
			{ bestQty = levelQty[L]; bestBar = b; bestLevel = L; bestSide = s; }
		}

		auto ConsumeAll = [&]()
		{
			for (int f = 0; f < firedCount; ++f)
				MarkConsumed(firedBars[f][0], firedBars[f][1]);
		};

		if (ConsumePend)
		{
			ConsumeAll();
			ConsumePend = 0;
		}
		else if (bestBar >= 0)
		{
			const int sideDir = (bestSide == 0) ? 1 : -1;

			// ---- opposing signal: exit only, never a reversal ----
			if (PosDir != 0 && sideDir != PosDir)
			{
				if (opposingExit && bestLevel >= opposingMin)
					FlattenAll("opposing signal");
				else
					LogSkip(bestSide, bestBar, bestLevel, "opposing but below Opposing Min Level");

				// The signal that flattens does not position. It must print
				// again to enter, so nothing here is left actionable.
				ConsumeAll();
			}
			else if (!inSession)
			{
				LogSkip(bestSide, bestBar, bestLevel, "outside session");
				ConsumeAll();
			}
			else if (!tradingEnabled)
			{
				LogSkip(bestSide, bestBar, bestLevel, "trading disabled");
				ConsumeAll();
			}
			else if (PosDir != 0 && !addEntries)
			{
				LogSkip(bestSide, bestBar, bestLevel, "already in position (Re-Signal = Ignore)");
				ConsumeAll();
			}
			else if (PosDir != 0 && NumLegs >= AAT_MAX_LEGS)
			{
				LogSkip(bestSide, bestBar, bestLevel, "leg pool full");
				ConsumeAll();
			}
			else if (PosDir != 0 &&
			         (lastBar - ((PosDir > 0) ? LastEntBarL : LastEntBarS)) < addMinBars)
			{
				LogSkip(bestSide, bestBar, bestLevel, "inside Add Entry Min Bars");
				ConsumeAll();
			}
			else if (PosQty + bestQty > maxPosition)
			{
				LogSkip(bestSide, bestBar, bestLevel, "would exceed Max Position Size");
				ConsumeAll();
			}
			else
			{
				// ---- structure stop, anchored to the SIGNAL bar --------------
				// Entry happens now; the stop belongs to bestBar. The window
				// ends AT the signal bar, not at the current bar.
				int sStart = bestBar - structBars + 1;
				if (sStart < 0) sStart = 0;
				float ext = (sideDir > 0) ? sc.Low[sStart] : sc.High[sStart];
				for (int j = sStart + 1; j <= bestBar; ++j)
				{
					if (sideDir > 0) { if (sc.Low[j]  < ext) ext = sc.Low[j];  }
					else             { if (sc.High[j] > ext) ext = sc.High[j]; }
				}

				const float entryPx = sc.Close[lastBar];
				float stopLvl = (sideDir > 0) ? (ext - stopOffset) : (ext + stopOffset);

				if (maxStopDist > 0 && (entryPx - stopLvl) * sideDir > maxStopDist)
				{
					LogSkip(bestSide, bestBar, bestLevel, "structure stop wider than Max Stop Distance");
					ConsumeAll();
				}
				else
				{
					// Widen a stop that would sit through the market. Under
					// attached orders a bad stop rejects the ENTRY, so this is
					// what keeps a signal tradeable at all.
					if (minStopDist > 0 && (entryPx - stopLvl) * sideDir < minStopDist)
						stopLvl = entryPx - sideDir * minStopDist;

					// An add is a NEW POSITION alongside the others: it takes
					// its own structure stop and nothing else's. No widening of
					// an existing stop, no tightening of one either.
					s_SCNewOrder Entry;
					Entry.OrderType     = SCT_ORDERTYPE_MARKET;
					Entry.OrderQuantity = bestQty;
					Entry.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;
					Entry.Stop1Price    = stopLvl;   // absolute price

					const int er = (sideDir > 0) ? sc.BuyEntry(Entry) : sc.SellEntry(Entry);
					if (er <= 0)
					{
						SCString m;
						m.Format("Aggregator Auto Trader: %s L%d ENTRY REJECTED at %.2f, stop %.2f, qty %d, error %d (%s)",
							(bestSide == 0) ? "LONG" : "SHORT", bestLevel, entryPx, stopLvl,
							bestQty, er, sc.GetTradingErrorTextMessage(er));
						sc.AddMessageToLog(m, 1);
						ConsumeAll();   // never retry a rejected signal forever
					}
					else
					{
						const int leg = NumLegs;
						EntryOrdID[leg] = er;
						StopOrdID[leg]  = 0;
						StopSent[leg]   = 0.0f;   // child not live: resting price unknown
						LegStop[leg]    = stopLvl;
						LegEntry[leg]   = entryPx;
						LegQty[leg]     = bestQty;
						LegBE[leg]      = 0;
						LegLock[leg]    = 0;
						++NumLegs;

						PosDir  = sideDir;
						PosQty  = SumLegQty();
						SigBar  = bestBar;
						EntBar  = lastBar;

						if (sideDir > 0) LastEntBarL = lastBar; else LastEntBarS = lastBar;

						sg_Entry[lastBar] = entryPx;

						SCString m;
						m.Format("Aggregator Auto Trader: %s L%d leg %d qty %d @ %.2f, stop %.2f (signal %d bar(s) back), position %d",
							(bestSide == 0) ? "LONG" : "SHORT", bestLevel, leg, bestQty,
							entryPx, stopLvl, lastBar - bestBar, PosQty);
						sc.AddMessageToLog(m, 0);

						// One entry per scan window: everything else in the
						// window describes the same move.
						ConsumeAll();
					}
				}
			}
		}

		// Cluster collapse: consume neighbours of the bar acted on so a burst
		// of signals describing ONE move cannot stack entries.
		// BACKWARD ONLY. Consuming bestBar+d for d > 0 marks bars that do not
		// exist yet; the prune above only clears entries BELOW floorBar, so the
		// phantom survives and silently swallows the next bar's real signal.
		if (clusterBars > 0 && bestBar >= 0)
			for (int d = -clusterBars; d <= clusterBars; ++d)
			{
				const int cb = bestBar + d;
				if (cb > lastBar) continue;
				MarkConsumed(bestSide, cb);
			}
	}

	// =========================================================================
	// OUTPUT
	// =========================================================================
	// With per-leg stops there is no single stop price. Plot the NEAREST one —
	// the next price at which the position changes size.
	float nearStop = 0.0f;
	int   legsBE = 0, legsLock = 0;
	for (int i = 0; i < NumLegs; ++i)
	{
		if (LegStop[i] == 0.0f) continue;
		if (nearStop == 0.0f)
			nearStop = LegStop[i];
		else
			nearStop = (PosDir > 0) ? ((LegStop[i] > nearStop) ? LegStop[i] : nearStop)
			                        : ((LegStop[i] < nearStop) ? LegStop[i] : nearStop);
		if (LegBE[i])   ++legsBE;
		if (LegLock[i]) ++legsLock;
	}

	if (PosDir != 0 && nearStop != 0.0f)
		sg_Stop[lastBar] = nearStop;

	SaveConsumed();
	SaveLegs();

	if (In_ShowPanel.GetYesNo())
	{
		SCString txt;
		if (PosDir == 0)
			txt.Format("AGG AUTOTRADER | FLAT | realized %.2f | %s%s",
				Realized,
				tradingEnabled ? "ARMED" : "DISABLED",
				inSession ? "" : " | OUT OF SESSION");
		else
			txt.Format("AGG AUTOTRADER | %s %d @ %.2f | %d leg(s), nearest stop %.2f | BE %d LOCK %d | realized %.2f | %s%s",
				(PosDir > 0) ? "LONG" : "SHORT", PosQty, AvgEntryPx(),
				NumLegs, nearStop, legsBE, legsLock, Realized,
				tradingEnabled ? "ARMED" : "DISABLED",
				inSession ? "" : " | OUT OF SESSION");

		s_UseTool Tool;
		Tool.Clear();
		Tool.ChartNumber   = sc.ChartNumber;
		// s_UseTool has no StudyID member; the study-owned field is
		// AssociatedStudyID (scstructures.h:693).
		Tool.AssociatedStudyID = sc.StudyGraphInstanceID;
		Tool.DrawingType   = DRAWING_TEXT;
		Tool.LineNumber    = 90210;
		Tool.UseRelativeVerticalValues = 1;
		Tool.BeginDateTime = 2;
		Tool.BeginValue    = 96;
		Tool.Text          = txt;
		Tool.Color         = (PosDir == 0) ? RGB(180, 180, 180)
		                   : ((PosDir > 0) ? RGB(0, 220, 120) : RGB(240, 110, 110));
		Tool.FontSize      = 10;
		Tool.FontBold      = 1;
		Tool.AddMethod     = UTAM_ADD_OR_ADJUST;
		sc.UseTool(Tool);
	}
}
