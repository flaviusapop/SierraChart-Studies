// OFSignalAutoTrader.cpp
// ---------------------------------------------------------------
// Auto-trades the arrows produced by Orderflow Signal V3.
//
// Reads TWO OrderflowSignalV3 instances on this chart — one the user
// designates LONG, one SHORT. Direction comes purely from WHICH
// reference a signal arrived on; this study never inspects how either
// instance is configured. (OFSV3's "Trigger Position" input is
// cosmetic — it only sets arrow Y placement — so direction is not
// discoverable from the source study's settings.)
//
// Each accepted signal creates its OWN slot: an independent little
// state machine with its OWN stop. Slots are netted into one account
// position; a slot's stop closes only that slot's quantity.
//
//   entry : an arrow (L1/L2/L3) or a confluence zone ONSET
//   exit  : that slot's arrow-bar stop, its own ratcheting trail, or
//           an opposing signal (which flattens every slot on that side)
//   size  : per-level quantity, 0 = that level never trades
//
// LATE-PRINTING ARROWS are the central problem this solves. OFSV3
// backfills HTF triggers, so an arrow lands on a bar that is already
// 2-6 bars old by the time it exists. We scan back for arrows newer
// than a watermark and enter AT MARKET ON THE CURRENT BAR, keeping
// signalBar and entryBar as separate fields — signalBar anchors the
// stop, entryBar anchors the trail window and attribution.
//
// Spec: Trading/Futures_Day_Trading/OFSignalAutoTrader_BuildSpec.md
// ---------------------------------------------------------------

#include "sierrachart.h"

SCDLLName("OF Signal Auto Trader")

// Slot pool. Signals beyond this are dropped and flagged on the panel.
const int OFS_MAX_SLOTS = 16;

// Level ordinals. Used for the Opposing Min Level gate and the panel
// ONLY — cluster ranking is by configured QUANTITY, not by this.
const int OFS_LVL_1    = 1;
const int OFS_LVL_2    = 2;
const int OFS_LVL_3    = 3;
const int OFS_LVL_CONF = 4;

SCSFExport scsf_OFSignalAutoTrader(SCStudyInterfaceRef sc)
{
	// ---------------- Subgraphs ----------------
	SCSubgraphRef BuyEx  = sc.Subgraph[0];
	SCSubgraphRef SellEx = sc.Subgraph[1];

	// ---------------- Inputs ----------------
	SCInputRef In_Enable        = sc.Input[0];
	SCInputRef In_SendToService = sc.Input[1];

	SCInputRef In_LongStudy     = sc.Input[3];
	SCInputRef In_ShortStudy    = sc.Input[4];
	SCInputRef In_ScanBars      = sc.Input[5];

	SCInputRef In_Qty1          = sc.Input[6];
	SCInputRef In_Qty2          = sc.Input[7];
	SCInputRef In_Qty3          = sc.Input[8];
	SCInputRef In_QtyConf       = sc.Input[9];
	SCInputRef In_LongOn        = sc.Input[10];
	SCInputRef In_ShortOn       = sc.Input[11];

	SCInputRef In_ReSignalMode  = sc.Input[12];
	SCInputRef In_MaxEntries    = sc.Input[13];
	SCInputRef In_MaxNet        = sc.Input[14];

	// sc.Input[15] was "Use Arrow Bar Stop" (Yes/No). Removed: set to No
	// with Trail Bars = 0 it left an open position with NO stop of any
	// kind, which is the exact state this study exists to prevent. Its
	// name had also gone stale — it gated all three Stop Basis modes, not
	// just the arrow bar. Slot left unused so stored input indices do not
	// shift on existing charts.
	SCInputRef In_ArrowOffset   = sc.Input[16];
	SCInputRef In_MaxStopDist   = sc.Input[17];
	SCInputRef In_TrailBars     = sc.Input[18];
	SCInputRef In_OpposingExit  = sc.Input[19];
	SCInputRef In_OpposingMin   = sc.Input[20];
	SCInputRef In_Disaster      = sc.Input[21];
	SCInputRef In_DisasterBars  = sc.Input[22];

	SCInputRef In_SessionStart  = sc.Input[23];
	SCInputRef In_SessionEnd    = sc.Input[24];
	SCInputRef In_FlattenTime   = sc.Input[25];
	SCInputRef In_MaxEntriesDay = sc.Input[26];
	SCInputRef In_MaxDailyLoss  = sc.Input[27];
	SCInputRef In_OnRecalc      = sc.Input[28];

	SCInputRef In_ShowPanel     = sc.Input[29];
	SCInputRef In_ShowLagLines  = sc.Input[30];
	SCInputRef In_ClusterBars   = sc.Input[31];
	// sc.Input[32] was "Stop Handling" (Resting vs Synthetic). Removed:
	// a stop is a stop. It rests at the broker from the moment the entry
	// is submitted, always. The slot is left deliberately unused so no
	// existing chart's stored input values shift index.
	SCInputRef In_AddMinBars    = sc.Input[33];
	SCInputRef In_OneEntryWin   = sc.Input[34];
	SCInputRef In_LogSkips      = sc.Input[35];
	SCInputRef In_MinStopDist   = sc.Input[36];
	SCInputRef In_StopBasis     = sc.Input[37];
	SCInputRef In_StopStructBars= sc.Input[38];
	SCInputRef In_FixedStopPts  = sc.Input[39];
	SCInputRef In_SignalAnchor  = sc.Input[40];

	if (sc.SetDefaults)
	{
		sc.GraphName = "OF Signal Auto Trader";
		sc.AutoLoop = 0;                 // manual loop
		sc.GraphRegion = 0;
		sc.ValueFormat = 2;

		// ---- auto trading configuration ----
		sc.AllowMultipleEntriesInSameDirection = 1;
		sc.MaximumPositionAllowed = 30;  // re-derived each call
		sc.SupportReversals = 1;
		sc.AllowOnlyOneTradePerBar = 0;
		// 0 = do NOT apply the Trade Window's attached-order template to
		// study orders. Must stay 0 (see RenkoFlipAutoTrader notes).
		sc.SupportAttachedOrdersForTrading = 0;
		// Both MUST be 0 once we rest real stop orders at the broker:
		// either flag would have Sierra cancel our resting stops the
		// moment another entry or exit goes through, silently removing
		// protection from every other open slot. We cancel our own stops
		// explicitly instead, by stored order ID.
		sc.CancelAllOrdersOnEntriesAndReversals = 0;
		sc.CancelAllWorkingOrdersOnExit = 0;
		sc.MaintainTradeStatisticsAndTradesData = 1;
		sc.SendOrdersToTradeService = 0;

		BuyEx.Name = "Net Buy";
		BuyEx.DrawStyle = DRAWSTYLE_ARROW_UP;
		BuyEx.PrimaryColor = RGB(0, 220, 120);
		BuyEx.LineWidth = 3;
		BuyEx.DrawZeros = false;

		SellEx.Name = "Net Sell";
		SellEx.DrawStyle = DRAWSTYLE_ARROW_DOWN;
		SellEx.PrimaryColor = RGB(255, 70, 70);
		SellEx.LineWidth = 3;
		SellEx.DrawZeros = false;

		In_Enable.Name = "Enable Trading";
		In_Enable.SetYesNo(0);           // safe default: compute only

		In_SendToService.Name = "Send Orders To Trade Service (No = local simulation)";
		In_SendToService.SetYesNo(0);

		// Only the STUDY ID is used from these; subgraphs SG1..SG4 are
		// read by fixed index (L1 / L2 / L3 / Confluence), matching the
		// OrderflowSignalV3 subgraph layout.
		In_LongStudy.Name = "Long Signal Study (OFSV3 instance — subgraph choice IGNORED, all of SG1-SG4 are read)";
		In_LongStudy.SetStudySubgraphValues(0, 0);
		In_ShortStudy.Name = "Short Signal Study (OFSV3 instance — subgraph choice IGNORED, all of SG1-SG4 are read)";
		In_ShortStudy.SetStudySubgraphValues(0, 0);

		In_ScanBars.Name = "Signal Scan Bars (how far back a late arrow still counts)";
		In_ScanBars.SetInt(7);
		In_ScanBars.SetIntLimits(1, 100);

		// The window is a dedup horizon, not a permission to trade every
		// arrow in it. OFSV3 backfills several HTF triggers at once, so a
		// 7-bar window routinely holds 2-4 arrows describing ONE move.
		// With this on, the scan takes the best of them and consumes the
		// rest, so widening the window buys reach without buying size.
		In_OneEntryWin.Name = "One Entry Per Scan Window (multiple signals in the window collapse to ONE entry)";
		In_OneEntryWin.SetYesNo(1);

		// Every discard path used to be silent, which made "the arrow was
		// there and nothing happened" impossible to diagnose from the
		// chart. Each signal logs its outcome exactly once.
		In_LogSkips.Name = "Log Skipped Signals (why an arrow did not trade)";
		In_LogSkips.SetYesNo(1);

		In_ClusterBars.Name = "Signal Cluster Bars (+/- N bars collapse to ONE entry; 0 = off)";
		In_ClusterBars.SetInt(1);
		In_ClusterBars.SetIntLimits(0, 50);

		// Quantity IS the on/off switch. All default to 1: the study
		// holds no opinion on which level deserves more size, and a flat
		// table is the only setting where level-vs-level comparison is
		// undistorted. Note every level is therefore live by default —
		// the safety is Enable Trading = No, not pre-disabled levels.
		In_Qty1.Name = "L1 Quantity (0 = level disabled)";
		In_Qty1.SetInt(1);
		In_Qty1.SetIntLimits(0, 100);
		In_Qty2.Name = "L2 Quantity (0 = level disabled)";
		In_Qty2.SetInt(1);
		In_Qty2.SetIntLimits(0, 100);
		In_Qty3.Name = "L3 Quantity (0 = level disabled)";
		In_Qty3.SetInt(1);
		In_Qty3.SetIntLimits(0, 100);
		In_QtyConf.Name = "Confluence Quantity (0 = level disabled)";
		In_QtyConf.SetInt(1);
		In_QtyConf.SetIntLimits(0, 100);

		In_LongOn.Name = "Long Signals Enabled";
		In_LongOn.SetYesNo(1);
		In_ShortOn.Name = "Short Signals Enabled";
		In_ShortOn.SetYesNo(1);

		In_ReSignalMode.Name = "Re-Signal Mode (signal in the direction already held)";
		In_ReSignalMode.SetCustomInputStrings("Ignore While In Position;Add Entry");
		In_ReSignalMode.SetCustomInputIndex(0);

		In_AddMinBars.Name = "Add Entry Min Bars (spacing between stacked entries; 0 = every arrow bar)";
		In_AddMinBars.SetInt(3);
		In_AddMinBars.SetIntLimits(0, 200);

		In_MaxEntries.Name = "Max Open Entries (0 = unlimited)";
		In_MaxEntries.SetInt(3);
		In_MaxEntries.SetIntLimits(0, OFS_MAX_SLOTS);

		In_MaxNet.Name = "Max Net Contracts (0 = unlimited)";
		In_MaxNet.SetInt(5);
		In_MaxNet.SetIntLimits(0, 500);

		In_ArrowOffset.Name = "Arrow Bar Stop Offset (ticks beyond that bar's extreme)";
		In_ArrowOffset.SetInt(2);
		In_ArrowOffset.SetIntLimits(0, 400);

		// Renamed: this is an ENTRY FILTER, not a stop. The old name
		// ("Max Arrow Stop Distance") read like a cap that would clamp the
		// stop level. It never clamped anything — it declines the trade.
		In_MaxStopDist.Name = "Max Risk Per Entry (points; SKIP the trade if the stop is further than this; 0 = no limit)";
		In_MaxStopDist.SetFloat(15.0f);

		// There was a MAX but no MIN, and the missing floor is what left
		// positions unprotected. When the arrow prints on the entry bar and
		// that bar closes at its own extreme, the arrow-bar stop lands
		// within a tick or two of the fill — live logs show 0.50 points on
		// ES. A stop that close is through the market before the order
		// reaches the broker, so it is rejected, retried, rejected again,
		// and the position simply never gets a stop. Widen anything tighter
		// than this to this distance.
		In_MinStopDist.Name = "Min Stop Distance (points; widen a too-tight stop; 0 = off)";
		In_MinStopDist.SetFloat(3.0f);

		// WHY A BASIS CHOICE EXISTS AT ALL.
		// The arrow-bar stop assumed the arrow was several bars old by the
		// time we entered, leaving the bar's extreme some distance away.
		// Live logs show the opposite: the arrow lands on the bar that just
		// closed, EVERY time ("arrow 0 bars back"). We then enter at that
		// same bar's close. On a Range chart a bar completes AT an extreme,
		// so for a short the close sits on the high — and high + 2 ticks is
		// half a point from the fill. The stop was unplaceable by
		// construction, not by accident.
		//
		// The N-bar extreme is the honest replacement: it prices the stop
		// off recent structure rather than off one bar that, by the nature
		// of a Range bar, ends exactly where the risk starts.
		In_StopBasis.Name = "Stop Basis";
		In_StopBasis.SetCustomInputStrings(
			"N-Bar Extreme (structure);"
			"Arrow Bar Extreme only (original);"
			"Fixed Points From Entry");
		In_StopBasis.SetCustomInputIndex(0);

		In_StopStructBars.Name = "Stop Structure Bars (N-bar extreme ending at the arrow bar)";
		In_StopStructBars.SetInt(4);
		In_StopStructBars.SetIntLimits(1, 100);

		In_FixedStopPts.Name = "Fixed Stop Points (used only when Stop Basis = Fixed)";
		In_FixedStopPts.SetFloat(6.0f);

		// OFSV3 rescans ALL history on every bar close
		// (OrderflowSignalV3.cpp:409) and detects triggers by RISING EDGE.
		// An HTF trigger's value only becomes non-zero at the bar where its
		// higher-timeframe bar completes — so the edge is found at an OLD
		// index. The arrow belongs to the bar it formed on; it merely
		// became visible later. Anchor there, and the stop is priced off
		// the structure that actually produced the signal.
		In_SignalAnchor.Name = "Signal Anchor (which arrow in the window to trade)";
		In_SignalAnchor.SetCustomInputStrings(
			"Oldest un-acted arrow (where the signal formed);"
			"Newest arrow in the window (original)");
		In_SignalAnchor.SetCustomInputIndex(0);

		In_TrailBars.Name = "Trail Bars (ratcheting N-bar extreme; 0 = no trail)";
		In_TrailBars.SetInt(0);
		In_TrailBars.SetIntLimits(0, 200);

		In_OpposingExit.Name = "Opposing Signal Exit (flattens ALL slots on the held side; never reverses)";
		In_OpposingExit.SetYesNo(1);

		In_OpposingMin.Name = "Opposing Min Level (1=L1 2=L2 3=L3 4=Confluence)";
		In_OpposingMin.SetInt(1);
		In_OpposingMin.SetIntLimits(1, 4);

		In_Disaster.Name = "Disaster Stop on NET position (points; 0 = off)";
		In_Disaster.SetFloat(0.0f);

		In_DisasterBars.Name = "Disaster Lockout Bars (bar counter, not signal-cleared)";
		In_DisasterBars.SetInt(20);
		In_DisasterBars.SetIntLimits(0, 5000);

		In_SessionStart.Name = "Session Start (chart timezone)";
		In_SessionStart.SetTime(HMS_TIME(9, 30, 0));
		In_SessionEnd.Name = "Session End — last entry (chart timezone)";
		In_SessionEnd.SetTime(HMS_TIME(15, 45, 0));
		In_FlattenTime.Name = "Flatten & Stop Time (chart timezone)";
		In_FlattenTime.SetTime(HMS_TIME(15, 55, 0));

		In_MaxEntriesDay.Name = "Max Entries Per Day (0 = off)";
		In_MaxEntriesDay.SetInt(0);
		In_MaxEntriesDay.SetIntLimits(0, 500);

		In_MaxDailyLoss.Name = "Max Daily Loss (points; 0 = off)";
		In_MaxDailyLoss.SetFloat(0.0f);

		In_OnRecalc.Name = "On Recalculation";
		In_OnRecalc.SetCustomInputStrings("Adopt Account Position (live);Rebuild From Session Open (replay)");
		In_OnRecalc.SetCustomInputIndex(0);

		In_ShowPanel.Name = "Show status panel";
		In_ShowPanel.SetYesNo(1);

		In_ShowLagLines.Name = "Show signal-to-entry lag lines + synthetic stop levels";
		In_ShowLagLines.SetYesNo(1);

		return;
	}

	if (sc.LastCallToFunction)
		return;

	// ---------------- persistent globals ----------------
	int& LastProcessed = sc.GetPersistentInt(1);
	int& TradeDate     = sc.GetPersistentInt(2);
	int& EntriesToday  = sc.GetPersistentInt(3);
	int& StoppedForDay = sc.GetPersistentInt(4);
	int& LastRecon     = sc.GetPersistentInt(5);
	int& DisasterBar   = sc.GetPersistentInt(6);   // bar index of the last disaster stop, -1 = none
	// (7) and (8) were WmLong / WmShort — a monotonic "newest bar already
	// decided" watermark. That model is WRONG against OrderflowSignalV3.
	// OFSV3 runs a FULL PASS FROM BAR 0 on every bar close
	// (OrderflowSignalV3.cpp:409) and rewrites SG1-SG4 across all history,
	// so an arrow can materialise at ANY bar index — including bars older
	// than anything we have already looked at — whenever an upstream HTF
	// trigger backfills. A watermark makes every such arrow permanently
	// invisible, because the scan only ever starts at wm + 1.
	//
	// Replaced by an explicit per-side set of CONSUMED bar indices. The
	// full window is re-scanned every pass; a bar acts unless it is in the
	// set. Dedup is by membership, not by position.
	int& Unused7       = sc.GetPersistentInt(7);
	int& Unused8       = sc.GetPersistentInt(8);
	(void)Unused7; (void)Unused8;
	int& SuppressRecon = sc.GetPersistentInt(9);   // adopt-mode guard, see below
	int& AdoptPending  = sc.GetPersistentInt(10);  // a recalc left a live position untracked
	int& ConsumeWindowPending = sc.GetPersistentInt(15);  // recalc: swallow the window before trading again
	int& LastEntLong   = sc.GetPersistentInt(11);  // bar of the last entry taken, long side (-1 = none)
	int& LastEntShort  = sc.GetPersistentInt(12);  // ditto, short side
	int& LastDrawBar   = sc.GetPersistentInt(13);  // bar the chart drawings were last refreshed on
	int& DrawSig       = sc.GetPersistentInt(14);  // signature of the drawn state, forces a mid-bar refresh
	float& Realized    = sc.GetPersistentFloat(1);

	// ---------------- per-slot state ----------------
	// Persistent slots are loaded into real local arrays and written back
	// ONCE at the end. Do NOT take the address of a persistent variable
	// and index it — persistent storage is a container lookup, not a
	// contiguous array, so (&sc.GetPersistentInt(100))[k] is undefined
	// behaviour and crashes Sierra on apply.
	int   SlotOn[OFS_MAX_SLOTS], Dir[OFS_MAX_SLOTS], Qty[OFS_MAX_SLOTS];
	int   SigBar[OFS_MAX_SLOTS], EntBar[OFS_MAX_SLOTS], Level[OFS_MAX_SLOTS];
	int   HaveTrail[OFS_MAX_SLOTS];
	int   StopOrdID[OFS_MAX_SLOTS];   // broker order ID of this slot's resting stop, 0 = none
	int   EntryOrdID[OFS_MAX_SLOTS];  // parent entry order; its child IS the stop
	float EntryPx[OFS_MAX_SLOTS], StopPx[OFS_MAX_SLOTS];
	float BestPx[OFS_MAX_SLOTS], TrailPx[OFS_MAX_SLOTS];
	float StopSent[OFS_MAX_SLOTS];    // price last actually sent to the broker

	for (int k = 0; k < OFS_MAX_SLOTS; ++k)
	{
		SlotOn[k]    = sc.GetPersistentInt(100 + k);
		Dir[k]       = sc.GetPersistentInt(200 + k);
		Qty[k]       = sc.GetPersistentInt(300 + k);
		SigBar[k]    = sc.GetPersistentInt(400 + k);
		EntBar[k]    = sc.GetPersistentInt(500 + k);
		Level[k]     = sc.GetPersistentInt(600 + k);
		HaveTrail[k] = sc.GetPersistentInt(700 + k);
		StopOrdID[k] = sc.GetPersistentInt(900 + k);
		EntryOrdID[k]= sc.GetPersistentInt(1000 + k);
		EntryPx[k]   = sc.GetPersistentFloat(100 + k);
		StopPx[k]    = sc.GetPersistentFloat(200 + k);
		BestPx[k]    = sc.GetPersistentFloat(300 + k);
		TrailPx[k]   = sc.GetPersistentFloat(400 + k);
		StopSent[k]  = sc.GetPersistentFloat(500 + k);
	}

	int TradesByLevel[5];
	for (int L = 0; L < 5; ++L)
		TradesByLevel[L] = sc.GetPersistentInt(800 + L);

	// ---------------- consumed-signal set ----------------
	// Bar indices already decided on, per side. -1 = empty. Entries older
	// than the scan window are purged each pass, so 64 is far more than
	// the window can ever hold.
	const int OFS_CONSUMED_N = 64;
	int Consumed[2][OFS_CONSUMED_N];
	for (int j = 0; j < OFS_CONSUMED_N; ++j)
	{
		Consumed[0][j] = sc.GetPersistentInt(2000 + j);
		Consumed[1][j] = sc.GetPersistentInt(2100 + j);
	}

	auto IsConsumed = [&](int side, int bar) -> bool
	{
		for (int j = 0; j < OFS_CONSUMED_N; ++j)
			if (Consumed[side][j] == bar) return true;
		return false;
	};

	auto MarkConsumed = [&](int side, int bar)
	{
		if (bar < 0 || IsConsumed(side, bar))
			return;
		// Prefer an empty cell; otherwise evict the oldest bar.
		int slot = -1, oldest = 0;
		for (int j = 0; j < OFS_CONSUMED_N; ++j)
		{
			if (Consumed[side][j] < 0) { slot = j; break; }
			if (Consumed[side][j] < Consumed[side][oldest]) oldest = j;
		}
		Consumed[side][(slot >= 0) ? slot : oldest] = bar;
	};

	const bool rebuildOnRecalc = (In_OnRecalc.GetIndex() == 1);

	if (sc.IsFullRecalculation)
	{
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
		{
			SlotOn[k] = 0; Dir[k] = 0; Qty[k] = 0;
			SigBar[k] = -1; EntBar[k] = -1; Level[k] = 0;
			HaveTrail[k] = 0;
			EntryPx[k] = 0; StopPx[k] = 0; BestPx[k] = 0; TrailPx[k] = 0;
		}
		Realized = 0;
		for (int L = 0; L < 5; ++L) TradesByLevel[L] = 0;
		LastRecon = -1;
		DisasterBar = -1;

		if (rebuildOnRecalc)
		{
			// Replay mode: reconstruct the session's decisions from the
			// start of the loaded data. The bar loop is date-gated, so
			// only bars belonging to the current trading day can act.
			LastProcessed = -1;
			LastEntLong = -1; LastEntShort = -1;
			SuppressRecon = 0;
			for (int j = 0; j < OFS_CONSUMED_N; ++j)
				{ Consumed[0][j] = -1; Consumed[1][j] = -1; }
		}
		else
		{
			// Live default: never rebuild. Adopt the account's current
			// position as truth and wait for the next genuine signal.
			// A settings edit must never move the position.
			LastProcessed = -1;   // fixed up below once lastBar is known
			LastEntLong = -1; LastEntShort = -1;
			// Consumed set is deliberately NOT cleared here. A settings
			// edit forces a full recalculation, and clearing it would make
			// every arrow still inside the scan window look brand new — the
			// study would re-enter trades it had already taken or already
			// declined, purely because the user changed an input.
			ConsumeWindowPending = 1;
			SuppressRecon = 1;    // a rebuilt-flat target must not flatten a live position
			// ...but "do not rebuild" must NOT mean "forget the position".
			// Clearing the slots above leaves any open position with no
			// slot, and therefore NO STOP AT ALL. Adopt it into a slot
			// below, once the position and signal arrays are readable.
			AdoptPending = 1;
		}
	}

	sc.SendOrdersToTradeService = In_SendToService.GetYesNo();

	// Last CLOSED bar. Signals are only read from closed bars: OFSV3's
	// forming-bar value is a proxy computed from bar i-1 and can resolve
	// differently once the bar closes. Acting on it would trade a guess.
	const int lastBar = sc.ArraySize - 2;

	// single exit point below, so the write-back always runs
	if (lastBar >= 1)
	{

	// In adopt mode a full recalc must not replay history as live
	// decisions — jump the cursor to the present.
	if (LastProcessed < 0 && !rebuildOnRecalc)
		LastProcessed = lastBar;

	// ---------------- daily reset ----------------
	// Clears ALL state, not just counters, and is NOT gated on trading
	// being enabled: if the study is off or Sierra is not running at
	// flatten time, yesterday's slots must not survive into the morning.
	const int today = sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.ArraySize - 1]);
	if (today != TradeDate)
	{
		TradeDate = today;
		EntriesToday = 0;
		StoppedForDay = 0;
		DisasterBar = -1;
		LastEntLong = -1; LastEntShort = -1;
		Realized = 0;
		for (int L = 0; L < 5; ++L) TradesByLevel[L] = 0;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
		{
			SlotOn[k] = 0; Dir[k] = 0; Qty[k] = 0;
			SigBar[k] = -1; EntBar[k] = -1; Level[k] = 0;
			HaveTrail[k] = 0;
			StopOrdID[k] = 0; EntryOrdID[k] = 0;   // yesterday's IDs are meaningless today
			EntryPx[k] = 0; StopPx[k] = 0; BestPx[k] = 0; TrailPx[k] = 0;
		}
	}

	const bool realTime = !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0;
	const bool tradingEnabled = In_Enable.GetYesNo() != 0 && realTime;

	s_SCPositionData Position;
	sc.GetTradePosition(Position);

	const double perPoint = (sc.TickSize > 0) ? (sc.CurrencyValuePerTick / sc.TickSize) : 0;
	const double dailyPnLPts = (perPoint > 0) ? (Position.DailyProfitLoss / perPoint) : 0;

	const int curTime = sc.BaseDateTimeIn[sc.ArraySize - 1].GetTime();

	// ---------------- signal sources ----------------
	// Only the study ID is taken from each reference; SG1..SG4 are read
	// by fixed index to match OrderflowSignalV3's subgraph layout:
	//   SG1 = Level 1, SG2 = Level 2, SG3 = Level 3, SG4 = Confluence
	const int longID  = In_LongStudy.GetStudyID();
	const int shortID = In_ShortStudy.GetStudyID();

	SCFloatArray SigArr[2][4];   // [side][level-1]; side 0 = long, 1 = short
	bool sideReady[2] = { false, false };

	if (longID != 0 && In_LongOn.GetYesNo())
	{
		for (int g = 0; g < 4; ++g)
			sc.GetStudyArrayUsingID(longID, g, SigArr[0][g]);
		sideReady[0] = SigArr[0][0].GetArraySize() > 0;
	}
	if (shortID != 0 && In_ShortOn.GetYesNo())
	{
		for (int g = 0; g < 4; ++g)
			sc.GetStudyArrayUsingID(shortID, g, SigArr[1][g]);
		sideReady[1] = SigArr[1][0].GetArraySize() > 0;
	}

	// ---------------- settings ----------------
	const int   levelQty[5]  = { 0, In_Qty1.GetInt(), In_Qty2.GetInt(),
	                             In_Qty3.GetInt(), In_QtyConf.GetInt() };
	const bool  addEntries   = (In_ReSignalMode.GetIndex() == 1);
	const int   addMinBars   = In_AddMinBars.GetInt();
	const int   maxEntries   = In_MaxEntries.GetInt();
	const int   maxNet       = In_MaxNet.GetInt();
	const float arrowOffset  = In_ArrowOffset.GetInt() * sc.TickSize;
	const float maxStopDist  = In_MaxStopDist.GetFloat();
	const float minStopDist  = In_MinStopDist.GetFloat();
	const int   stopBasis    = In_StopBasis.GetIndex();
	const bool  anchorOldest = (In_SignalAnchor.GetIndex() == 0);
	const int   trailBars    = In_TrailBars.GetInt();
	const bool  opposingExit = In_OpposingExit.GetYesNo() != 0;
	const int   opposingMin  = In_OpposingMin.GetInt();
	const int   clusterBars  = In_ClusterBars.GetInt();
	const int   scanBars     = In_ScanBars.GetInt();

	int sumQty = 0;
	for (int L = 1; L <= 4; ++L) sumQty += levelQty[L];
	sc.MaximumPositionAllowed = (maxNet > 0) ? maxNet
	                          : ((sumQty > 0) ? sumQty * OFS_MAX_SLOTS : 1);


	// ---------------- resting stop helpers ----------------
	// The stop rests at the broker at an ABSOLUTE price — the arrow
	// bar's own low/high — so it does not depend on the entry fill and
	// does not depend on this study still running.

	auto StopLevelFor = [&](int k) -> float
	{
		// Tighter of the two wins, same rule as the synthetic path.
		const bool haveArrow = StopPx[k] != 0;
		const bool haveTrail = trailBars > 0 && HaveTrail[k];
		if (haveArrow && haveTrail)
		{
			const bool trailTighter = (Dir[k] > 0) ? (TrailPx[k] > StopPx[k])
			                                       : (TrailPx[k] < StopPx[k]);
			return trailTighter ? TrailPx[k] : StopPx[k];
		}
		if (haveArrow) return StopPx[k];
		if (haveTrail) return TrailPx[k];
		return 0.0f;
	};

	auto CancelStop = [&](int k)
	{
		if (StopOrdID[k] != 0)
		{
			sc.CancelOrder(StopOrdID[k]);
			StopOrdID[k] = 0;
		}
		StopSent[k] = 0.0f;
	};

	// ---------------- attached-stop maintenance ----------------
	// The stop is NOT placed here — SierraChart created it as a child of
	// the entry order the moment that order was submitted. All this does
	// is (a) learn the child's order ID, and (b) move it when the trail
	// ratchets. There is no placement, no retry, and no window in which a
	// filled position has no stop.
	//
	// Still throttled: the study runs on EVERY TICK (sc.AutoLoop = 0), so
	// an unconditional sc.ModifyOrder here would re-send the same price
	// hundreds of times a second and freeze SierraChart.
	auto SyncStop = [&](int k)
	{
		if (!tradingEnabled || !SlotOn[k] || EntryOrdID[k] == 0)
			return;

		// Resolve the attached stop's ID from its parent. s_SCTradeOrder
		// carries StopChildInternalOrderID, so no order-list scan is
		// needed — and the child does not exist until the parent is
		// working, which is why this is re-checked rather than done once.
		if (StopOrdID[k] == 0)
		{
			s_SCTradeOrder Parent;
			if (sc.GetOrderByOrderID(EntryOrdID[k], Parent) == SCTRADING_ORDER_ERROR)
				return;
			if (Parent.StopChildInternalOrderID == 0)
				return;

			StopOrdID[k] = Parent.StopChildInternalOrderID;
			StopSent[k]  = StopPx[k];

			SCString msg;
			msg.Format("OF Signal Auto Trader: slot %d attached stop is live at %.2f (order %u)",
				k, StopPx[k], (unsigned)StopOrdID[k]);
			sc.AddMessageToLog(msg, 0);
		}

		const float want = StopLevelFor(k);
		if (want == 0.0f)
			return;

		// Nothing changed — say nothing.
		if (want == StopSent[k])
			return;

		// RATCHET: only ever moved in favour. Never loosen a stop.
		const bool improves = (Dir[k] > 0) ? (want > StopSent[k])
		                                   : (want < StopSent[k]);
		if (!improves)
			return;

		s_SCNewOrder Mod;
		Mod.InternalOrderID = StopOrdID[k];
		Mod.Price1 = want;
		const int mr = sc.ModifyOrder(Mod);
		if (mr > 0)
		{
			StopSent[k] = want;
		}
		else
		{
			// Leave StopSent alone so the drawing keeps showing the price
			// the broker actually holds, not the one we wanted.
			SCString msg;
			msg.Format("OF Signal Auto Trader: trail MODIFY FAILED slot %d to %.2f, error %d (%s) — stop still rests at %.2f",
				k, want, mr, sc.GetTradingErrorTextMessage(mr), StopSent[k]);
			sc.AddMessageToLog(msg, 1);
		}
	};

	// ---------------- flatten time / daily loss ----------------
	if (tradingEnabled && !StoppedForDay)
	{
		const bool lossHit = In_MaxDailyLoss.GetFloat() > 0 &&
		                     dailyPnLPts <= -In_MaxDailyLoss.GetFloat();
		if (curTime >= In_FlattenTime.GetTime() || lossHit)
		{
			if (Position.PositionQuantity != 0 || Position.WorkingOrdersExist)
				sc.FlattenAndCancelAllOrders();
			for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			{
				if (SlotOn[k])
					Realized += (sc.Close[lastBar] - EntryPx[k]) * Dir[k] * Qty[k];
				SlotOn[k] = 0; Dir[k] = 0;
				StopOrdID[k] = 0; EntryOrdID[k] = 0;   // FlattenAndCancelAllOrders killed them
			}
			StoppedForDay = 1;
			if (lossHit)
				sc.AddMessageToLog("OF Signal Auto Trader: max daily loss hit — flattened and stopped for the day.", 1);
		}
	}

	// =========================================================
	// 0. ADOPT AN UNTRACKED POSITION AFTER A RECALCULATION
	//
	// A full recalculation (ANY settings change, chart reload, or an
	// explicit Recalculate) clears every slot. In adopt mode we do not
	// rebuild the session's decisions — but if the account is holding a
	// position, having no slot means having NO STOP: nothing is tracked,
	// so nothing is ever checked, and the position runs naked while the
	// panel reads "flat".
	//
	// So adopt the live position into a single slot. The arrow bar is
	// recovered by scanning back for the most recent signal on the held
	// side and re-deriving the stop from that bar's extreme — the same
	// level the original slot would have carried. If no arrow can be
	// found the slot is still created (so the trail and disaster stop
	// apply and the panel is honest), with no arrow stop and a loud
	// panel warning.
	// =========================================================
	if (AdoptPending)
	{
		AdoptPending = 0;

		bool anySlot = false;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (SlotOn[k]) { anySlot = true; break; }

		const double posQty = Position.PositionQuantity;

		if (posQty != 0 && !anySlot)
		{
			// DO NOT CANCEL ANYTHING HERE.
			//
			// With attached stops, the stop belongs to SierraChart, not to
			// this study. A recalculation wipes our slot state but leaves
			// the position AND its attached stop untouched at the broker —
			// the position was never unprotected, we merely stopped being
			// able to see the protection. The old code cancelled every
			// stored stop ID at this point, which under attached orders
			// would strip a live position of a perfectly good stop and
			// then be unable to replace it (a stop can only be attached to
			// an entry, and that entry already filled).
			//
			// So adopt the EXISTING stop instead: find the working stop
			// order on this symbol and take it over.
			const int d = (posQty > 0) ? 1 : -1;

			int adoptedStopID = 0;
			float adoptedStopPx = 0.0f;
			{
				s_SCTradeOrder TO;
				for (int oi = 0; sc.GetOrderByIndex(oi, TO) != SCTRADING_ORDER_ERROR; ++oi)
				{
					if (!TO.IsWorking() || TO.OrderTypeAsInt != SCT_ORDERTYPE_STOP)
						continue;
					adoptedStopID = TO.InternalOrderID;
					adoptedStopPx = (float)TO.Price1;
					break;
				}
			}

			const int side = (d > 0) ? 0 : 1;

			// Look back further than the normal scan window: the
			// position may have been opened well before the recalc.
			const int ADOPT_LOOKBACK = 500;
			int floorBar = lastBar - ADOPT_LOOKBACK + 1;
			if (floorBar < 0) floorBar = 0;

			int foundBar = -1, foundLevel = 0;
			if (sideReady[side])
			{
				for (int b = lastBar; b >= floorBar && foundBar < 0; --b)
				{
					for (int L = OFS_LVL_1; L <= OFS_LVL_CONF; ++L)
					{
						const SCFloatArray& arr = SigArr[side][L - 1];
						if (arr.GetArraySize() <= b)
							continue;

						const bool fired = (L == OFS_LVL_CONF)
							? ((arr[b] > 0.5f) && (b == 0 || arr[b - 1] < 0.5f))
							: (arr[b] != 0.0f);

						if (fired) { foundBar = b; foundLevel = L; break; }
					}
				}
			}

			SlotOn[0]    = 1;
			Dir[0]       = d;
			Qty[0]       = (int)((posQty > 0) ? posQty : -posQty);
			EntryPx[0]   = Position.AveragePrice;
			EntBar[0]    = lastBar;   // trail window restarts here
			SigBar[0]    = (foundBar >= 0) ? foundBar : lastBar;
			Level[0]     = foundLevel;
			BestPx[0]    = sc.Close[lastBar];
			TrailPx[0]   = 0; HaveTrail[0] = 0;
			// The REAL resting stop wins over any re-derivation. Sierra
			// still holds it; re-deriving a level from bar data would only
			// describe a stop we cannot place, since the entry it would
			// have attached to has already filled.
			if (adoptedStopID != 0)
			{
				StopOrdID[0] = adoptedStopID;
				StopPx[0]    = adoptedStopPx;
				StopSent[0]  = adoptedStopPx;
			}
			else
			{
				StopPx[0]   = 0.0f;
				StopSent[0] = 0.0f;
			}

			SuppressRecon = 0;   // the target is real again, exits may act

			// Anchor the add-spacing gate and the dedup watermark to the
			// adopted bar so a fresh arrow can still stack onto it.
			if (d > 0) LastEntLong  = SigBar[0];
			else       LastEntShort = SigBar[0];

			SCString msg;
			if (adoptedStopID != 0)
				msg.Format("OF Signal Auto Trader: adopted position %.0f after recalculation — its attached stop is still working at %.2f (order %u), L%d arrow %d bars back",
					posQty, adoptedStopPx, (unsigned)adoptedStopID,
					foundLevel, (foundBar >= 0) ? (lastBar - foundBar) : -1);
			else
				msg.Format("OF Signal Auto Trader: adopted position %.0f after recalculation — *** NO WORKING STOP FOUND, POSITION IS UNPROTECTED *** place one manually or flatten",
					posQty);
			sc.AddMessageToLog(msg, 1);
		}
	}

	// =========================================================
	// 1. STOPS — evaluated per closed bar, once each
	//    Burst-safe: several bars can close between study calls
	//    under replay or fast live load, and each slot's stop must
	//    see every one of them rather than only the newest.
	// =========================================================
	for (int i = LastProcessed + 1; i <= lastBar; ++i)
	{
		const float px = sc.Close[i];

		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
		{
			if (!SlotOn[k] || i < EntBar[k])
				continue;

			// The BROKER owns every trigger. This loop computes stop
			// LEVELS only and never closes a slot — a slot closes when
			// its resting order reports filled. A study-side market exit
			// alongside a resting order would close the slot twice.
			//
			// The arrow-bar stop is fixed at entry and needs nothing here.
			// Only the trail moves, and moving it means modifying the
			// resting order, not firing anything.

			// ---- ratcheting N-bar trail ----
			// ARMING: the trail stays off until the slot has been open for
			// a FULL trailBars window. Without this the window is clamped
			// to EntBar, so one bar after entry the "N-bar extreme" is a
			// TWO-bar extreme — the entry bar's own low. On a range chart
			// that is 2-4 points, tighter than any arrow stop, and the
			// ratchet means it can never give the room back. The trail
			// would silently replace the arrow stop with a scratch stop and
			// every trade would die in a bar or two. The arrow stop owns
			// the first N bars; the trail takes over with real room behind
			// it.
			if (trailBars > 0 && (i - EntBar[k]) >= trailBars)
			{
				// Window never reaches back before this slot's entry.
				int start = i - trailBars + 1;
				if (start < EntBar[k]) start = EntBar[k];

				float ext = (Dir[k] > 0) ? sc.Low[start] : sc.High[start];
				for (int j = start + 1; j <= i; ++j)
				{
					if (Dir[k] > 0) { if (sc.Low[j]  < ext) ext = sc.Low[j];  }
					else            { if (sc.High[j] > ext) ext = sc.High[j]; }
				}

				// RATCHET: the level moves only in favour. A rolling
				// extreme would let the stop loosen again, which makes
				// no sense underneath a fixed arrow-bar stop.
				if (!HaveTrail[k])
				{
					TrailPx[k] = ext;
					HaveTrail[k] = 1;
				}
				else if (Dir[k] > 0) { if (ext > TrailPx[k]) TrailPx[k] = ext; }
				else                 { if (ext < TrailPx[k]) TrailPx[k] = ext; }
			}

			if (Dir[k] > 0) { if (px > BestPx[k]) BestPx[k] = px; }
			else            { if (px < BestPx[k]) BestPx[k] = px; }
		}
	}
	LastProcessed = lastBar;

	// ---- resting stops: detect fills, then place/move the orders ----
	// A resting stop can fire at any moment, including between study
	// calls and intrabar — that is the whole point of it. The slot is
	// closed when its ORDER reports filled, not when a bar closes.
	{
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
		{
			if (!SlotOn[k] || StopOrdID[k] == 0)
				continue;

			s_SCTradeOrder TO;
			// Not-found is SCTRADING_ORDER_ERROR (-1), not 0. Comparing
			// against 0 let a failed lookup fall through and read a stale
			// TO — a slot could be closed on another order's status.
			if (sc.GetOrderByOrderID(StopOrdID[k], TO) == SCTRADING_ORDER_ERROR)
				continue;   // order not found yet — leave the slot alone

			if (TO.OrderStatusCode == SCT_OSC_FILLED)
			{
				// Attribution uses the ACTUAL fill price here, unlike
				// the decision-price attribution used elsewhere: for a
				// resting stop the fill is knowable and specific to this
				// slot, so use the better number.
				const float fill = (TO.AvgFillPrice != 0)
					? (float)TO.AvgFillPrice : sc.Close[lastBar];
				Realized += (fill - EntryPx[k]) * Dir[k] * Qty[k];
				SlotOn[k] = 0; Dir[k] = 0; HaveTrail[k] = 0;
				StopOrdID[k] = 0;

				SCString msg;
				msg.Format("OF Signal Auto Trader: slot %d stopped out at %.2f (resting order)", k, fill);
				sc.AddMessageToLog(msg, 0);
			}
			// There is no REJECTED status code — a rejected order comes
			// back as SCT_OSC_ERROR (scconstants.h SCOrderStatusCodeEnum).
			else if (TO.OrderStatusCode == SCT_OSC_CANCELED ||
			         TO.OrderStatusCode == SCT_OSC_ERROR)
			{
				// Something killed our stop while the slot is still
				// open. Drop the ID so SyncStop re-places it below,
				// rather than leaving the slot silently unprotected.
				StopOrdID[k] = 0;
				StopSent[k]  = 0.0f;   // else the change-guard blocks the re-place
			}
		}

	}

	// =========================================================
	// 2. SIGNAL SCAN — the late-print handler
	//    Runs once per pass at the newest closed bar. Collects every
	//    signal above the watermark, collapses them per the cluster
	//    rule, and produces AT MOST ONE action per side.
	// =========================================================
	const bool inSession =
		sc.BaseDateTimeIn[lastBar].GetTime() >= In_SessionStart.GetTime() &&
		sc.BaseDateTimeIn[lastBar].GetTime() <= In_SessionEnd.GetTime() &&
		sc.BaseDateTimeIn[lastBar].GetTime() <  In_FlattenTime.GetTime() &&
		sc.GetTradingDayDate(sc.BaseDateTimeIn[lastBar]) == TradeDate;   // date-gated, not time-only

	const bool disasterLocked =
		DisasterBar >= 0 && (lastBar - DisasterBar) < In_DisasterBars.GetInt();

	const bool oneEntryWin = In_OneEntryWin.GetYesNo() != 0;
	const bool logSkips    = In_LogSkips.GetYesNo() != 0;

	// Every discard path below used to be silent. A skipped signal is
	// indistinguishable from a signal that never printed, which is exactly
	// the ambiguity that made "the arrow was there but nothing traded"
	// undiagnosable. One line per consumed signal, with the reason.
	auto LogSkip = [&](int side, int bar, int level, const char* why)
	{
		if (!logSkips)
			return;
		SCString msg;
		msg.Format("OF Signal Auto Trader: %s L%d arrow %d bars back NOT TRADED — %s",
			(side == 0) ? "LONG" : "SHORT", level, lastBar - bar, why);
		sc.AddMessageToLog(msg, 0);
	};

	// Slots opened during THIS pass. Side 0 is scanned before side 1, so
	// without this a short arrow found later in the same call would run the
	// opposing-exit branch against a long that was created moments earlier
	// and has not even been reconciled into a position yet — the signal
	// fires, the slot appears and vanishes, and no trade ever reaches the
	// broker. Order of scanning must not decide the trade.
	int FreshSlot[OFS_MAX_SLOTS];
	for (int k = 0; k < OFS_MAX_SLOTS; ++k) FreshSlot[k] = 0;

	for (int side = 0; side < 2; ++side)
	{
		if (!sideReady[side])
			continue;

		int& lastEnt = (side == 0) ? LastEntLong : LastEntShort;
		const int sideDir = (side == 0) ? 1 : -1;

		int floorBar = lastBar - scanBars + 1;
		if (floorBar < 0) floorBar = 0;

		// Purge consumed bars that have fallen out of the window, so the
		// set stays small and a bar index can never be matched twice.
		for (int j = 0; j < OFS_CONSUMED_N; ++j)
			if (Consumed[side][j] >= 0 && Consumed[side][j] < floorBar)
				Consumed[side][j] = -1;

		// ---- rescan the FULL window every pass ----
		// Not "everything newer than a watermark". OFSV3 rewrites its whole
		// output on every bar close, so an arrow can appear at a bar we
		// already scanned past. Only membership in the consumed set may
		// exclude a bar from acting.
		//
		// Ranking by configured QUANTITY rather than level ordinal is
		// deliberate: confluence is a different kind of evidence than L3,
		// not obviously stronger, so the sizing table declares priority.
		int   bestBar = -1, bestLevel = 0, bestQty = 0;
		int   firedBars[OFS_CONSUMED_N];
		int   firedCount = 0;

		for (int b = floorBar; b <= lastBar; ++b)
		{
			bool barFired = false;

			for (int L = OFS_LVL_1; L <= OFS_LVL_CONF; ++L)
			{
				const SCFloatArray& arr = SigArr[side][L - 1];
				if (arr.GetArraySize() <= b)
					continue;

				bool fired = false;
				if (L == OFS_LVL_CONF)
				{
					// Confluence is a multi-bar background zone, not an
					// arrow: fire on ONSET only, one entry per zone.
					fired = (arr[b] > 0.5f) && (b == 0 || arr[b - 1] < 0.5f);
				}
				else
				{
					fired = (arr[b] != 0.0f);
				}

				if (!fired || levelQty[L] == 0)
					continue;

				barFired = true;

				// A consumed bar still counts as "fired" for bookkeeping,
				// but can never be chosen again.
				if (IsConsumed(side, b))
					continue;

				// Quantity ranks first. TIES ARE BROKEN BY THE ANCHOR RULE,
				// and that tie-break decides almost everything: all four
				// level quantities default to 1, so every comparison is a
				// tie and the rule alone picks the bar.
				//
				// The old rule was "b >= bestBar" — always the NEWEST fired
				// bar. That is why every live entry logged "arrow 0 bars
				// back" and every stop came out half a point wide: OFSV3
				// backfills an arrow onto the bar where it FORMED (bar 2)
				// while the chart is at bar 6, and we were anchoring to
				// bar 6 regardless. The stop then belonged to a bar that,
				// on a Range chart, had just closed at its own extreme.
				bool better = false;
				if (levelQty[L] > bestQty)
					better = true;
				else if (levelQty[L] == bestQty)
					better = (bestBar < 0) ? true
					       : (anchorOldest ? (b < bestBar) : (b >= bestBar));

				if (better)
				{
					bestQty = levelQty[L];
					bestBar = b;
					bestLevel = L;
				}
			}

			if (barFired && firedCount < OFS_CONSUMED_N)
				firedBars[firedCount++] = b;
		}

		// Swallow the whole window once after a live recalculation: those
		// arrows were already decided on before the settings edit.
		if (ConsumeWindowPending)
		{
			for (int f = 0; f < firedCount; ++f)
				MarkConsumed(side, firedBars[f]);
			if (side == 1) ConsumeWindowPending = 0;
			continue;
		}

		if (bestBar < 0)
			continue;

		// Consuming is decision-specific:
		//   DISCARD (opposing, ignore, capped) — consume every fired bar in
		//     the window; we deliberately want none of them.
		//   ACT — consume the whole window when One Entry Per Scan Window is
		//     on, otherwise only the bar acted on.
		auto ConsumeAll = [&]()
		{
			for (int f = 0; f < firedCount; ++f)
				MarkConsumed(side, firedBars[f]);
		};

		// ---- opposing signal: exit only, never a reversal ----
		int heldDir = 0, openSlots = 0, netQty = 0;
		bool heldIsFresh = false;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (SlotOn[k])
			{
				heldDir = Dir[k]; ++openSlots; netQty += Dir[k] * Qty[k];
				if (FreshSlot[k]) heldIsFresh = true;
			}

		if (heldDir != 0 && heldDir != sideDir)
		{
			// A position opened in this very pass is not something an
			// opposing arrow gets to undo: both arrows are backfilled
			// prints of the same closed bar, so which one "wins" would be
			// decided by scan order rather than by the market. Consume the
			// opposing window and let the next bar decide.
			if (heldIsFresh)
			{
				LogSkip(side, bestBar, bestLevel,
					"opposing signal on a position opened this same bar — deferred");
				ConsumeAll();
				continue;
			}

			if (opposingExit && bestLevel >= opposingMin)
			{
				for (int k = 0; k < OFS_MAX_SLOTS; ++k)
				{
					if (!SlotOn[k] || Dir[k] != heldDir)
						continue;
					// Cancel this slot's resting stop BEFORE the exit is
					// reconciled: leaving it working after the slot is
					// gone would put on a fresh position in the opposite
					// direction the next time price touched that level.
					CancelStop(k);
					Realized += (sc.Close[lastBar] - EntryPx[k]) * Dir[k] * Qty[k];
					SlotOn[k] = 0; Dir[k] = 0; HaveTrail[k] = 0;
				}
			}
			// No reversing: the signal that flattens does not position.
			// It must print again to enter; consume everything so the same
			// arrow cannot immediately re-enter on the opposite side.
			ConsumeAll();
			continue;
		}

		// ---- same direction already held, Ignore mode ----
		if (heldDir == sideDir && !addEntries)
		{
			LogSkip(side, bestBar, bestLevel, "already in position (Ignore While In Position)");
			ConsumeAll();   // discard — Ignore While In Position
			continue;
		}

		// ---- spacing gate ----
		// Adds are spaced by Add Entry Min Bars; a fresh (initial) entry is
		// spaced by the cluster rule. Both measured from the last entry
		// taken on THIS side. Too-close signals are discarded up to the bar
		// examined, but newer arrows remain eligible next pass.
		// One Entry Per Scan Window: an initial entry is additionally
		// spaced by the FULL window. Without this, collapsing only happens
		// within a single pass — the arrows OFSV3 backfills for one move
		// land on different bars, so the leftovers would each become an
		// entry on subsequent passes as soon as the position closed.
		const int windowGap = (oneEntryWin && heldDir != sideDir) ? scanBars : 0;
		int reqGap = (heldDir == sideDir) ? addMinBars : clusterBars;
		if (windowGap > reqGap) reqGap = windowGap;

		if (reqGap > 0 && lastEnt >= 0 && (bestBar - lastEnt) < reqGap)
		{
			LogSkip(side, bestBar, bestLevel,
				(windowGap > 0 && (bestBar - lastEnt) < windowGap)
					? "collapsed into the previous entry (One Entry Per Scan Window)"
					: "too close to the previous entry on this side (spacing gate)");
			MarkConsumed(side, bestBar);
			continue;
		}

		// ---- entry gates ----
		// A signal blocked here is spent — consume the whole window so a
		// stale arrow cannot fire late once a cap or the session frees up.
		if (!inSession || StoppedForDay || disasterLocked)
		{
			LogSkip(side, bestBar, bestLevel,
				!inSession ? "outside session window"
				           : (StoppedForDay ? "stopped for the day" : "disaster lockout"));
			ConsumeAll();
			continue;
		}
		if (In_MaxEntriesDay.GetInt() > 0 && EntriesToday >= In_MaxEntriesDay.GetInt())
		{
			LogSkip(side, bestBar, bestLevel, "Max Entries Per Day reached");
			ConsumeAll();
			continue;
		}
		if (maxEntries > 0 && openSlots >= maxEntries)
		{
			LogSkip(side, bestBar, bestLevel, "Max Open Entries reached");
			ConsumeAll();
			continue;
		}
		if (maxNet > 0 && (netQty * sideDir) + bestQty > maxNet)
		{
			LogSkip(side, bestBar, bestLevel, "Max Net Contracts would be exceeded");
			ConsumeAll();
			continue;
		}

		int free = -1;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (!SlotOn[k]) { free = k; break; }
		if (free < 0)
		{
			LogSkip(side, bestBar, bestLevel, "slot pool full");
			ConsumeAll();   // pool full — discard; surfaced on the panel
			continue;
		}

		// ---- arrow-bar stop, anchored to the SIGNAL bar ----
		// The whole point is that entry happens several bars after the
		// signal, so the stop belongs to bar bestBar, not to now.
		const float entryPx = sc.Close[lastBar];

		float stopLvl;
		if (stopBasis == 2)
		{
			stopLvl = entryPx - sideDir * In_FixedStopPts.GetFloat();
		}
		else
		{
			// Basis 0 = N-bar extreme ending AT the arrow bar, basis 1 =
			// that one bar alone. The window ends at the arrow bar, not at
			// now, so the stop still belongs to the signal.
			const int look = (stopBasis == 0) ? In_StopStructBars.GetInt() : 1;
			int sStart = bestBar - look + 1;
			if (sStart < 0) sStart = 0;

			float ext = (sideDir > 0) ? sc.Low[sStart] : sc.High[sStart];
			for (int j = sStart + 1; j <= bestBar; ++j)
			{
				if (sideDir > 0) { if (sc.Low[j]  < ext) ext = sc.Low[j];  }
				else             { if (sc.High[j] > ext) ext = sc.High[j]; }
			}
			stopLvl = (sideDir > 0) ? (ext - arrowOffset) : (ext + arrowOffset);
		}

		// MINIMUM DISTANCE. When the arrow prints on the entry bar and that
		// bar closes at its own extreme, the raw level sits a tick or two
		// from the fill. Such a stop is through the market before the order
		// is accepted, so it is rejected and the position runs naked. Widen
		// it — never tighten — so the order is placeable.
		if (minStopDist > 0 && (entryPx - stopLvl) * sideDir < minStopDist)
			stopLvl = entryPx - sideDir * minStopDist;

		// Staleness / stop-size guard. Applies even when the arrow stop
		// itself is off: a signal whose bar is now far away is one where
		// the move already happened, worth declining on any config.
		// This is the gate that widening the window makes bite hardest: the
		// further back the arrow, the further price has already travelled
		// from its bar's extreme. It is logged with the actual distance so
		// the limit can be set against measured numbers instead of guessed
		// ones — if this line dominates the log, the limit is too tight for
		// the window, not the other way round.
		const float stopDist = (entryPx - stopLvl) * sideDir;
		if (maxStopDist > 0 && stopDist > maxStopDist)
		{
			if (logSkips)
			{
				SCString msg;
				msg.Format("OF Signal Auto Trader: %s L%d arrow %d bars back NOT TRADED — stop distance %.2f pts exceeds Max Risk Per Entry %.2f",
					(side == 0) ? "LONG" : "SHORT", bestLevel, lastBar - bestBar,
					stopDist, maxStopDist);
				sc.AddMessageToLog(msg, 0);
			}
			ConsumeAll();   // stale / too-far signal — discard
			continue;
		}

		// ---- SUBMIT THE ENTRY WITH ITS STOP ATTACHED ----
		// A standalone sc.BuyExit/SellExit stop order is rejected by ACSIL
		// before it ever reaches the trade service — the Trade Activity Log
		// for a whole session contains market orders only, and not one stop.
		// An ATTACHED stop is created by SierraChart itself as a child of
		// the entry, so it exists the instant the fill does. There is no
		// window in which the position is naked, no placement race, and
		// nothing to retry. Same mechanism as RenkoFlipAutoTrader:104-661,
		// which has never had a stop-placement failure. Note attached
		// orders work with SupportAttachedOrdersForTrading = 0; that flag
		// governs the Trade Window's template, not programmatic ones.
		if (!tradingEnabled)
		{
			ConsumeAll();
			continue;
		}

		s_SCNewOrder Entry;
		Entry.OrderType = SCT_ORDERTYPE_MARKET;
		Entry.OrderQuantity = bestQty;
		Entry.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;
		Entry.Stop1Price = stopLvl;   // absolute price, not an offset

		const int er = (sideDir > 0) ? sc.BuyEntry(Entry) : sc.SellEntry(Entry);
		if (er <= 0)
		{
			SCString msg;
			msg.Format("OF Signal Auto Trader: %s L%d ENTRY REJECTED at %.2f, stop %.2f, qty %d, error %d (%s)",
				(side == 0) ? "LONG" : "SHORT", bestLevel, entryPx, stopLvl,
				bestQty, er, sc.GetTradingErrorTextMessage(er));
			sc.AddMessageToLog(msg, 1);
			ConsumeAll();   // do not retry a rejected signal forever
			continue;
		}

		SlotOn[free]  = 1;
		FreshSlot[free] = 1;
		Dir[free]     = sideDir;
		Qty[free]     = bestQty;
		SigBar[free]  = bestBar;
		EntBar[free]  = lastBar;
		Level[free]   = bestLevel;
		EntryPx[free] = entryPx;
		StopPx[free]  = stopLvl;
		BestPx[free]  = entryPx;
		TrailPx[free] = 0.0f;
		HaveTrail[free] = 0;
		EntryOrdID[free] = Entry.InternalOrderID;   // parent; the stop is its child
		StopOrdID[free]  = 0;                       // resolved from the parent below
		StopSent[free]   = stopLvl;

		if (sideDir > 0) BuyEx[lastBar]  = sc.Low[lastBar]  - sc.TickSize * 4;
		else             SellEx[lastBar] = sc.High[lastBar] + sc.TickSize * 4;

		++EntriesToday;
		if (bestLevel >= 1 && bestLevel <= 4) ++TradesByLevel[bestLevel];
		SuppressRecon = 0;   // a genuine signal has now set the target

		// Acted. One Entry Per Scan Window decides what happens to the rest:
		//   ON  — consume EVERY fired bar in the window. The others were
		//         backfilled prints of the same move; none may also trade.
		//         This is what makes a wide window safe.
		//   OFF — consume only the bar acted on, so any other arrow stays
		//         eligible next pass and can stack a further contract,
		//         subject to the Add Entry Min Bars gap anchored here.
		lastEnt = bestBar;
		if (oneEntryWin) ConsumeAll(); else MarkConsumed(side, bestBar);

		if (logSkips)
		{
			SCString msg;
			msg.Format("OF Signal Auto Trader: %s L%d ENTERED q%d — arrow %d bars back, stop %.2f (dist %.2f pts)",
				(side == 0) ? "LONG" : "SHORT", bestLevel, bestQty,
				lastBar - bestBar, stopLvl, stopDist);
			sc.AddMessageToLog(msg, 0);
		}

		if (In_ShowLagLines.GetYesNo() && lastBar > bestBar)
		{
			s_UseTool Line;
			Line.Clear();
			Line.ChartNumber = sc.ChartNumber;
			Line.DrawingType = DRAWING_LINE;
			Line.LineNumber = 84212000 + (free * 2) + side;
			Line.AddAsUserDrawnDrawing = 0;
			Line.AddMethod = UTAM_ADD_OR_ADJUST;
			Line.BeginDateTime = sc.BaseDateTimeIn[bestBar];
			Line.EndDateTime = sc.BaseDateTimeIn[lastBar];
			Line.BeginValue = (sideDir > 0) ? sc.Low[bestBar] : sc.High[bestBar];
			Line.EndValue = entryPx;
			Line.Color = (sideDir > 0) ? RGB(0, 220, 120) : RGB(255, 70, 70);
			Line.LineWidth = 1;
			Line.LineStyle = LINESTYLE_DOT;
			sc.UseTool(Line);
		}
	}

	// =========================================================
	// 3. RECONCILE the netted position
	// =========================================================
	if (tradingEnabled && !StoppedForDay && lastBar != LastRecon)
	{
		// Claim this bar BEFORE the order walk below. Previously LastRecon
		// was only set on the success path, so while a foreign order was
		// working the whole GetOrderByIndex scan re-ran on every tick — and
		// the replay order list grows into the thousands. The reconcile was
		// always a once-per-bar operation; the retry now waits for the next
		// bar, which is what "let the next bar self-heal" already meant.
		LastRecon = lastBar;

		// Working-order guard: if the previous bar's order has not
		// filled, the delta would be recomputed and submitted AGAIN.
		// Skip the pass entirely and let the next bar self-heal.
		//
		// Position.WorkingOrdersExist is NOT usable here once we rest
		// stops at the broker — our own stops are working orders, so it
		// would be permanently true and the reconcile would never run
		// again. Count only working orders that are not ours.
		bool foreignWorkingOrder = false;
		{
			s_SCTradeOrder TO;
			// MUST terminate on SCTRADING_ORDER_ERROR (-1), NOT on 0.
			// Past the end of the list GetOrderByIndex returns -1, so a
			// "!= 0" condition never becomes false and this loop spins
			// forever inside Sierra's thread — a hard UI freeze, not a
			// slowdown. Same form as TradingSystem.cpp:2052.
			for (int oi = 0; sc.GetOrderByIndex(oi, TO) != SCTRADING_ORDER_ERROR; ++oi)
			{
				// IsWorking() is a member of s_SCTradeOrder, not an sc.
				// method; it wraps IsWorkingOrderStatus() from scconstants.h.
				if (!TO.IsWorking())
					continue;

				// Both our attached stops AND our parent entry orders are
				// ours. Counting either as foreign would block the flatten
				// safety net for as long as they are working.
				bool ours = false;
				for (int k = 0; k < OFS_MAX_SLOTS; ++k)
					if ((StopOrdID[k]  != 0 && TO.InternalOrderID == (uint32_t)StopOrdID[k]) ||
					    (EntryOrdID[k] != 0 && TO.InternalOrderID == (uint32_t)EntryOrdID[k]))
						{ ours = true; break; }

				if (!ours) { foreignWorkingOrder = true; break; }
			}
		}

		if (foreignWorkingOrder)
		{
			// deliberately no state change
		}
		else
		{
		double posQty = Position.PositionQuantity;

		// disaster stop on the NET position; lockout is a BAR COUNTER,
		// never cleared by a signal (a signal-cleared flag proved
		// nearly useless in OTFMultiTrader)
		if (posQty != 0 && In_Disaster.GetFloat() > 0)
		{
			const int posDir = (posQty > 0) ? 1 : -1;
			if ((sc.Close[lastBar] - Position.AveragePrice) * posDir <= -In_Disaster.GetFloat())
			{
				sc.FlattenAndCancelAllOrders();
				for (int k = 0; k < OFS_MAX_SLOTS; ++k)
				{
					if (SlotOn[k])
						Realized += (sc.Close[lastBar] - EntryPx[k]) * Dir[k] * Qty[k];
					SlotOn[k] = 0; Dir[k] = 0; HaveTrail[k] = 0;
					StopOrdID[k] = 0; EntryOrdID[k] = 0;   // FlattenAndCancelAllOrders killed them
				}
				DisasterBar = lastBar;
				sc.AddMessageToLog("OF Signal Auto Trader: disaster stop — net flattened, locked out by bar count.", 1);
				posQty = 0;
			}
		}

		int targetNet = 0;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (SlotOn[k]) targetNet += Dir[k] * Qty[k];

		// A recalc-produced flat target must never flatten a live
		// position. In adopt mode the rebuild is a no-op until a real
		// signal sets the target.
		const bool blocked = disasterLocked || (targetNet == 0 && SuppressRecon);

		// RECONCILE IS NOW EXIT-ONLY.
		//
		// Each slot submits its OWN entry, with its own stop attached, at
		// the moment its signal is accepted. Netting the entries into one
		// delta order is what made per-slot attached stops impossible —
		// one market order cannot carry four different stop prices — and
		// that is the whole reason stops had to be standalone, which ACSIL
		// rejects. So the entry half of the reconcile is gone.
		//
		// What remains is the safety net that must not: if every slot has
		// closed but the account still holds a position, flatten it. That
		// covers a slot closed by an opposing signal, a partial fill, or
		// any drift between our view and the broker's.
		if (!blocked && targetNet == 0 && posQty != 0)
		{
			sc.FlattenAndCancelAllOrders();
			SCString msg;
			msg.Format("OF Signal Auto Trader: no slots open but account holds %.0f — flattened", posQty);
			sc.AddMessageToLog(msg, 1);
		}
		}
	}

	// =========================================================
	// 3a. PLACE / MOVE THE RESTING STOPS
	//
	// Deliberately AFTER the reconcile: an exit stop cannot rest
	// against a position that does not exist yet, so a new slot's stop
	// goes in only once its entry has been submitted. On rejection
	// StopOrdID stays 0 and SyncStop retries on the next pass, with the
	// rejection logged — a slot must never be quietly unprotected.
	// =========================================================
	{
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			SyncStop(k);
	}

	// =========================================================
	// DRAWING THROTTLE
	//
	// Everything below issues chart-drawing calls, and this study runs on
	// EVERY TICK (sc.AutoLoop = 0). Live that is ~10 refreshes a second and
	// unnoticeable. Under Replay, ticks arrive thousands of times faster,
	// and up to 16 UseTool/DeleteACSChartDrawing calls plus a panel rewrite
	// per tick saturate the UI thread — Sierra stops responding. The
	// drawings were redrawn per tick even when nothing had changed and even
	// when flat.
	//
	// Refresh only when there is something new to show: a new bar, or a
	// change in what would actually be drawn. The signature covers slot
	// occupancy, direction and the live stop level in ticks, so a ratcheting
	// trail still updates the moment it moves, mid-bar.
	// =========================================================
	int drawSigNow = 0;
	for (int k = 0; k < OFS_MAX_SLOTS; ++k)
	{
		if (!SlotOn[k])
			continue;
		const float lvl = StopSent[k];
		const int lvlTicks = (sc.TickSize > 0) ? (int)(lvl / sc.TickSize) : (int)lvl;
		drawSigNow = drawSigNow * 31 + (k + 1) * 7 + Dir[k] * 3 + lvlTicks;
	}

	const bool redraw = (lastBar != LastDrawBar) || (drawSigNow != DrawSig);
	if (redraw)
	{
		LastDrawBar = lastBar;
		DrawSig = drawSigNow;
	}

	// =========================================================
	// 3b. DRAW THE STOPS
	//
	// The line shows the price the BROKER is actually holding — StopSent,
	// the last level a place or modify confirmed — NOT the level the study
	// would like it to be. Those two diverge whenever a modify fails, and
	// drawing the wish instead of the fact is what made price appear to
	// trade straight through a stop that never fired: the line had
	// ratcheted, the resting order had not.
	//
	// Anchored back at the ARROW bar so it lines up with the signal that
	// produced it, and deleted the moment the slot closes.
	// =========================================================
	if (In_ShowLagLines.GetYesNo() && redraw)
	{
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
		{
			const int lineNum = 84213000 + k;

			const float shown = SlotOn[k] ? StopSent[k] : 0.0f;
			const bool  haveLevel = (shown != 0.0f);
			// Label only — which of the two levels the resting price came
			// from. Ties go to the arrow stop.
			const bool  fromTrail = haveLevel && trailBars > 0 && HaveTrail[k] &&
				((Dir[k] > 0) ? (TrailPx[k] > StopPx[k] || StopPx[k] == 0)
				              : (TrailPx[k] < StopPx[k] || StopPx[k] == 0));

			if (!haveLevel)
			{
				sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNum);
				continue;
			}

			s_UseTool Stop;
			Stop.Clear();
			Stop.ChartNumber = sc.ChartNumber;
			Stop.DrawingType = DRAWING_LINE;
			Stop.LineNumber = lineNum;
			Stop.AddAsUserDrawnDrawing = 0;
			Stop.AddMethod = UTAM_ADD_OR_ADJUST;
			// start at the arrow bar, not the entry bar — the stop
			// belongs to the signal, and the gap between them is the
			// lag that needs to be visible
			Stop.BeginDateTime = sc.BaseDateTimeIn[(SigBar[k] >= 0) ? SigBar[k] : EntBar[k]];
			Stop.EndDateTime = sc.BaseDateTimeIn[lastBar];
			Stop.BeginValue = shown;
			Stop.EndValue = shown;
			Stop.Color = fromTrail ? RGB(255, 190, 60) : RGB(255, 70, 70);
			Stop.LineWidth = 2;
			Stop.LineStyle = fromTrail ? LINESTYLE_DASH : LINESTYLE_SOLID;
			Stop.ShowPrice = 1;
			Stop.Text.Format("stop %s L%d q%d", fromTrail ? "trail" : "arrow", Level[k], Qty[k]);
			sc.UseTool(Stop);
		}
	}

	// =========================================================
	// 4. STATUS PANEL
	// =========================================================
	if (In_ShowPanel.GetYesNo() && redraw)
	{
		int targetNet = 0, openSlots = 0;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (SlotOn[k]) { targetNet += Dir[k] * Qty[k]; ++openSlots; }

		SCString slots;
		if (openSlots == 0)
		{
			slots = "  (flat)";
		}
		else
		{
			for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			{
				if (!SlotOn[k])
					continue;

				// The RESTING price, not the wanted one. "want" is shown
				// alongside only when they disagree, which is the state
				// that needs to be visible.
				const float shown = StopSent[k];
				const float want  = StopLevelFor(k);
				const bool haveTrail = trailBars > 0 && HaveTrail[k];
				const char* owner = (shown == 0) ? "NONE"
					: ((haveTrail && ((Dir[k] > 0) ? (TrailPx[k] > StopPx[k] || StopPx[k] == 0)
					                               : (TrailPx[k] < StopPx[k] || StopPx[k] == 0)))
						? "trail" : "arrow");

				const float open = (sc.Close[lastBar] - EntryPx[k]) * Dir[k] * Qty[k];

				SCString one;
				if (want != 0 && shown != want)
					one.Format("  [%s L%d q%d lag%d stop %.2f(%s) !!WANT %.2f!! %+.2f]",
						Dir[k] > 0 ? "L" : "S",
						Level[k], Qty[k],
						EntBar[k] - SigBar[k],
						shown, owner, want, open);
				else
					one.Format("  [%s L%d q%d lag%d stop %.2f(%s) %+.2f]",
						Dir[k] > 0 ? "L" : "S",
						Level[k], Qty[k],
						EntBar[k] - SigBar[k],
						shown, owner, open);
				slots += one.GetChars();
			}
		}

		bool poolFull = true;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (!SlotOn[k]) { poolFull = false; break; }

		// A slot with no working stop order is unprotected — that is the
		// exact condition that went unnoticed live, so it gets its own
		// loud panel state rather than being inferable from the detail.
		bool unprotected = false;
		for (int k = 0; k < OFS_MAX_SLOTS; ++k)
			if (SlotOn[k] && StopOrdID[k] == 0 && StopLevelFor(k) != 0)
				{ unprotected = true; break; }

		const char* state = StoppedForDay ? " (stopped for day)"
		                  : (disasterLocked ? " (disaster lockout)"
		                  : (unprotected ? " *** SLOT WITH NO RESTING STOP ***"
		                  : (poolFull ? " (POOL FULL)" : "")));

		SCString txt;
		txt.Format("OFSignal AT: %s%s | Net target %+d | Pos %.0f | Entries %d | L1/L2/L3/Cf %d/%d/%d/%d | Realized %+.2f pts | Acct day %+.2f pts |%s",
			In_Enable.GetYesNo() ? "ON" : "OFF",
			state,
			targetNet,
			Position.PositionQuantity,
			EntriesToday,
			TradesByLevel[1], TradesByLevel[2], TradesByLevel[3], TradesByLevel[4],
			Realized,
			dailyPnLPts,
			slots.GetChars());

		s_UseTool Tool;
		Tool.Clear();
		Tool.ChartNumber = sc.ChartNumber;
		Tool.DrawingType = DRAWING_TEXT;
		Tool.LineNumber = 84212001;
		Tool.AddAsUserDrawnDrawing = 0;
		Tool.AddMethod = UTAM_ADD_OR_ADJUST;
		Tool.UseRelativeVerticalValues = 1;
		Tool.BeginDateTime = 3;
		Tool.BeginValue = 90;
		Tool.Color = In_Enable.GetYesNo() ? RGB(0, 220, 120) : RGB(180, 180, 180);
		Tool.FontSize = 12;
		Tool.FontBold = 1;
		Tool.Text = txt;
		sc.UseTool(Tool);
	}

	} // end if (lastBar >= 1)

	// ---------------- write persistent state back ----------------
	// Single write-back covering every path, so slot state survives to
	// the next study call.
	for (int k = 0; k < OFS_MAX_SLOTS; ++k)
	{
		// A closed slot must not carry a stale sent-price or attempt-bar
		// into its next occupant — the change-guard would then suppress
		// that slot's very first stop placement. Cleared in ONE place so
		// every close path (stop hit, opposing exit, flatten, disaster,
		// daily reset, recalc) is covered without repeating this.
		if (!SlotOn[k]) { StopSent[k] = 0.0f; EntryOrdID[k] = 0; }

		sc.GetPersistentInt(100 + k)   = SlotOn[k];
		sc.GetPersistentInt(200 + k)   = Dir[k];
		sc.GetPersistentInt(300 + k)   = Qty[k];
		sc.GetPersistentInt(400 + k)   = SigBar[k];
		sc.GetPersistentInt(500 + k)   = EntBar[k];
		sc.GetPersistentInt(600 + k)   = Level[k];
		sc.GetPersistentInt(700 + k)   = HaveTrail[k];
		sc.GetPersistentInt(900 + k)   = StopOrdID[k];
		sc.GetPersistentInt(1000 + k)  = EntryOrdID[k];
		sc.GetPersistentFloat(100 + k) = EntryPx[k];
		sc.GetPersistentFloat(200 + k) = StopPx[k];
		sc.GetPersistentFloat(300 + k) = BestPx[k];
		sc.GetPersistentFloat(400 + k) = TrailPx[k];
		sc.GetPersistentFloat(500 + k) = StopSent[k];
	}
	for (int L = 0; L < 5; ++L)
		sc.GetPersistentInt(800 + L) = TradesByLevel[L];

	for (int j = 0; j < OFS_CONSUMED_N; ++j)
	{
		sc.GetPersistentInt(2000 + j) = Consumed[0][j];
		sc.GetPersistentInt(2100 + j) = Consumed[1][j];
	}
}
