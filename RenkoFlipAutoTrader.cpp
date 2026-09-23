// RenkoFlipAutoTrader.cpp
// ---------------------------------------------------------------
// Automated execution of the Renko flip strategy.
// Signal engine identical to RenkoFlipMTFHelper (which remains the
// no-orders analysis layer):
//   - brick color from SC_RENKO_OPEN/SC_RENKO_CLOSE (fallback O/C)
//   - flip after a completed opposite run >= MinSwingLength
//   - entry on the Nth confirmation brick of the new color
//   - optional MTF consecutive-close gate on external charts
// Trading:
//   - market order at signal-brick close, reverses via
//     sc.SupportReversals; optional attached stop/target
//   - real-time only (never on full recalculation); Chart Replay OK
//   - risk guards: Enable switch, max trades/day, max daily loss,
//     flatten time
// Spec: Trading/Futures_Day_Trading/RenkoFlipAutoTrader_BuildSpec.md
// ---------------------------------------------------------------

#include "sierrachart.h"

SCDLLName("RenkoFlip Auto Trader")

// consecutive close-direction streak; +N up / -N down / 0 flat
static int GetCloseStreak(SCFloatArrayRef CloseArr, int LastConfirmedIndex)
{
	if (LastConfirmedIndex < 2)
		return 0;

	int i = LastConfirmedIndex;
	int dir = 0;
	if (CloseArr[i] > CloseArr[i - 1]) dir = 1;
	else if (CloseArr[i] < CloseArr[i - 1]) dir = -1;
	else return 0;

	int streak = 1;
	for (int k = i - 1; k >= 1; --k)
	{
		int d = 0;
		if (CloseArr[k] > CloseArr[k - 1]) d = 1;
		else if (CloseArr[k] < CloseArr[k - 1]) d = -1;

		if (d == dir)
			streak++;
		else
			break;
	}
	return dir * streak;
}

SCSFExport scsf_RenkoFlipAutoTrader(SCStudyInterfaceRef sc)
{
	// ---------------- Subgraphs ----------------
	SCSubgraphRef BuyEx  = sc.Subgraph[0];
	SCSubgraphRef SellEx = sc.Subgraph[1];

	// ---------------- Inputs ----------------
	SCInputRef In_Enable        = sc.Input[0];
	SCInputRef In_SendToService = sc.Input[1];
	SCInputRef In_Quantity      = sc.Input[2];
	SCInputRef In_MinSwing      = sc.Input[3];
	SCInputRef In_ConfirmBricks = sc.Input[4];
	SCInputRef In_SessionStart  = sc.Input[5];
	SCInputRef In_SessionEnd    = sc.Input[6];
	SCInputRef In_FlattenTime   = sc.Input[7];
	SCInputRef In_StopTicks     = sc.Input[8];
	SCInputRef In_TargetTicks   = sc.Input[9];
	SCInputRef In_MaxTradesDay  = sc.Input[10];
	SCInputRef In_MaxDailyLoss  = sc.Input[11];
	SCInputRef In_TF1Chart      = sc.Input[12];
	SCInputRef In_TF2Chart      = sc.Input[13];
	SCInputRef In_TF3Chart      = sc.Input[14];
	SCInputRef In_TF1MinConsec  = sc.Input[15];
	SCInputRef In_TF2MinConsec  = sc.Input[16];
	SCInputRef In_TF3MinConsec  = sc.Input[17];
	SCInputRef In_ShowPanel     = sc.Input[18];
	SCInputRef In_Strategy      = sc.Input[19];
	SCInputRef In_OTFStudy      = sc.Input[20];
	SCInputRef In_StopLookback  = sc.Input[21];
	SCInputRef In_BEBricks      = sc.Input[22];
	SCInputRef In_FAReentry     = sc.Input[23];
	SCInputRef In_FALookback    = sc.Input[24];
	SCInputRef In_FADebounce    = sc.Input[25];
	SCInputRef In_FilterRead    = sc.Input[26];
	SCInputRef In_FAPolicy      = sc.Input[27];
	SCInputRef In_FADisaster    = sc.Input[28];
	SCInputRef In_OTF2Study     = sc.Input[29];
	SCInputRef In_OTF3Study     = sc.Input[30];

	if (sc.SetDefaults)
	{
		sc.GraphName = "RenkoFlip Auto Trader";
		sc.AutoLoop = 0;                 // manual loop
		sc.GraphRegion = 0;
		sc.ValueFormat = 2;

		// ---- auto trading configuration ----
		sc.AllowMultipleEntriesInSameDirection = 0;
		sc.MaximumPositionAllowed = 10;  // tightened to order quantity below
		sc.SupportReversals = 1;         // signals flip the position
		sc.AllowOnlyOneTradePerBar = 1;
		// 0 = do NOT apply the Trade Window's attached orders to study
		// orders. The study defines its own attached stop programmatically
		// (s_SCNewOrder) where needed; that works regardless of this flag.
		sc.SupportAttachedOrdersForTrading = 0;
		sc.CancelAllOrdersOnEntriesAndReversals = 1;
		sc.CancelAllWorkingOrdersOnExit = 1;
		sc.MaintainTradeStatisticsAndTradesData = 1;
		sc.SendOrdersToTradeService = 0; // overridden per call from input

		BuyEx.Name = "Buy Entry";
		BuyEx.DrawStyle = DRAWSTYLE_ARROW_UP;
		BuyEx.PrimaryColor = RGB(0, 220, 120);
		BuyEx.LineWidth = 3;
		BuyEx.DrawZeros = false;

		SellEx.Name = "Sell Entry";
		SellEx.DrawStyle = DRAWSTYLE_ARROW_DOWN;
		SellEx.PrimaryColor = RGB(255, 70, 70);
		SellEx.LineWidth = 3;
		SellEx.DrawZeros = false;

		In_Enable.Name = "Enable Trading";
		In_Enable.SetYesNo(0);           // safe default: compute only

		In_SendToService.Name = "Send Orders To Trade Service (No = local simulation)";
		In_SendToService.SetYesNo(0);

		In_Quantity.Name = "Order Quantity";
		In_Quantity.SetInt(1);
		In_Quantity.SetIntLimits(1, 10);

		In_MinSwing.Name = "Min Swing Length (bricks before reversal)";
		In_MinSwing.SetInt(2);
		In_MinSwing.SetIntLimits(1, 50);

		In_ConfirmBricks.Name = "Confirmation Bricks (new color; 1 = enter on flip brick)";
		In_ConfirmBricks.SetInt(2);
		In_ConfirmBricks.SetIntLimits(1, 20);

		In_SessionStart.Name = "Session Start (chart timezone)";
		In_SessionStart.SetTime(HMS_TIME(9, 30, 0));
		In_SessionEnd.Name = "Session End — last entry (chart timezone)";
		In_SessionEnd.SetTime(HMS_TIME(15, 30, 0));
		In_FlattenTime.Name = "(Unused — Flatten & Stop Time removed)";
		In_FlattenTime.SetTime(HMS_TIME(15, 55, 0));

		In_StopTicks.Name = "Attached Stop (ticks; 0 = none)";
		In_StopTicks.SetInt(0);
		In_StopTicks.SetIntLimits(0, 1000);

		In_TargetTicks.Name = "Attached Target (ticks; 0 = none)";
		In_TargetTicks.SetInt(0);
		In_TargetTicks.SetIntLimits(0, 1000);

		In_MaxTradesDay.Name = "Max Entries Per Day (0 = off)";
		In_MaxTradesDay.SetInt(0);
		In_MaxTradesDay.SetIntLimits(0, 500);

		In_MaxDailyLoss.Name = "Max Daily Loss (points; 0 = off)";
		In_MaxDailyLoss.SetFloat(0.0f);

		In_TF1Chart.Name = "TF1 Chart Number (gate) — 0 = off";
		In_TF1Chart.SetChartNumber(0);
		In_TF2Chart.Name = "TF2 Chart Number (gate) — 0 = off";
		In_TF2Chart.SetChartNumber(0);
		In_TF3Chart.Name = "TF3 Chart Number (gate) — 0 = off";
		In_TF3Chart.SetChartNumber(0);

		In_TF1MinConsec.Name = "TF1 Min Consecutive Closes";
		In_TF1MinConsec.SetInt(1);
		In_TF2MinConsec.Name = "TF2 Min Consecutive Closes";
		In_TF2MinConsec.SetInt(1);
		In_TF3MinConsec.Name = "TF3 Min Consecutive Closes";
		In_TF3MinConsec.SetInt(1);

		In_ShowPanel.Name = "Show status panel";
		In_ShowPanel.SetYesNo(1);

		In_Strategy.Name = "Strategy";
		In_Strategy.SetCustomInputStrings("Always-In Flip;OTF Filtered (stop + OTF exit);Filtered Always-In (target-state);OTF Only (always in on filter);OTF Consensus (scaled always-in)");
		In_Strategy.SetCustomInputIndex(0);

		In_OTFStudy.Name = "OTF Filter: chart + study subgraph (can be another chart)";
		In_OTFStudy.SetChartStudySubgraphValues(0, 0, 0);

		In_StopLookback.Name = "OTF mode: Stop Lookback Bricks (X; 0 = no stop)";
		In_StopLookback.SetInt(2);
		In_StopLookback.SetIntLimits(0, 50);

		In_BEBricks.Name = "OTF mode: Breakeven After X Bricks Profit (0 = off)";
		In_BEBricks.SetInt(0);
		In_BEBricks.SetIntLimits(0, 50);

		In_FAReentry.Name = "FA mode: Re-Entry";
		In_FAReentry.SetCustomInputStrings("Next Signal Only;Immediate On Filter Alignment");
		In_FAReentry.SetCustomInputIndex(1);

		In_FALookback.Name = "FA mode: Signal Lookback For Filter-Triggered Entry (bricks; 0 = unlimited)";
		In_FALookback.SetInt(3);
		In_FALookback.SetIntLimits(0, 200);

		In_FADebounce.Name = "FA mode: Filter Debounce (bricks)";
		In_FADebounce.SetInt(1);
		In_FADebounce.SetIntLimits(1, 20);

		In_FilterRead.Name = "Filter Read (external chart)";
		In_FilterRead.SetCustomInputStrings("Last Closed Bar (stable);Forming Bar (live, repaints)");
		In_FilterRead.SetCustomInputIndex(0);

		In_FAPolicy.Name = "FA mode: On Disagreement";
		In_FAPolicy.SetCustomInputStrings("Go Flat;Hold Position (always in)");
		In_FAPolicy.SetCustomInputIndex(0);

		In_FADisaster.Name = "FA mode: Disaster Stop (points; 0 = off — REQUIRED for Hold)";
		In_FADisaster.SetFloat(0.0f);

		In_OTF2Study.Name = "Consensus: OTF-2 higher TF chart+study (study 0 = off)";
		In_OTF2Study.SetChartStudySubgraphValues(0, 0, 0);

		In_OTF3Study.Name = "Consensus: OTF-3 highest TF chart+study (study 0 = off)";
		In_OTF3Study.SetChartStudySubgraphValues(0, 0, 0);

		return;
	}

	// ---------------- persistent state ----------------
	int& LastDir       = sc.GetPersistentInt(1);
	int& RunLen        = sc.GetPersistentInt(2);
	int& PrevRunLen    = sc.GetPersistentInt(3);
	int& LastProcessed = sc.GetPersistentInt(4);
	int& FlipValid     = sc.GetPersistentInt(5);
	int& TradeDate     = sc.GetPersistentInt(6);   // trading day the counters belong to
	int& TradesToday   = sc.GetPersistentInt(7);
	int& StoppedForDay = sc.GetPersistentInt(8);   // 1 = daily loss hit or flatten time passed
	int& BEDone        = sc.GetPersistentInt(9);   // OTF mode: breakeven move done for current trade
	int& FA_SDir       = sc.GetPersistentInt(10);  // FA mode: standing signal direction
	int& FA_LastSigBar = sc.GetPersistentInt(11);
	int& FA_FCur       = sc.GetPersistentInt(12);  // FA mode: debounced filter state
	int& FA_FPend      = sc.GetPersistentInt(13);
	int& FA_FPendCount = sc.GetPersistentInt(14);
	int& FA_LastRecon  = sc.GetPersistentInt(15);  // FA mode: last brick reconciled (once per brick)
	int& FA_Due        = sc.GetPersistentInt(16);  // FA mode: desired position per helper rules (burst-safe)
	int& FA_DStopBlock = sc.GetPersistentInt(17);  // FA/OTF-only: re-entry lockout after disaster stop
	int& FA_DStopDir   = sc.GetPersistentInt(18);  // direction that was disaster-stopped
	int& F2Cur         = sc.GetPersistentInt(19);  // consensus: OTF-2 debounced state
	int& F2Pend        = sc.GetPersistentInt(20);
	int& F2PendCount   = sc.GetPersistentInt(21);
	int& F3Cur         = sc.GetPersistentInt(22);  // consensus: OTF-3 debounced state
	int& F3Pend        = sc.GetPersistentInt(23);
	int& F3PendCount   = sc.GetPersistentInt(24);
	int& DiagLastState = sc.GetPersistentInt(25);  // diagnostic: last logged tradingEnabled state (-1 = never logged)

	if (sc.LastCallToFunction)
		return;

	if (sc.IsFullRecalculation)
	{
		LastDir = 0; RunLen = 0; PrevRunLen = 0; LastProcessed = -1;
		FlipValid = 0;
		FA_SDir = 0; FA_LastSigBar = -1;
		FA_FCur = 0; FA_FPend = 0; FA_FPendCount = 0; FA_LastRecon = -1;
		FA_Due = 0; FA_DStopBlock = 0; FA_DStopDir = 0;
		F2Cur = 0; F2Pend = 0; F2PendCount = 0;
		F3Cur = 0; F3Pend = 0; F3PendCount = 0;
		// Reset daily guards on every full recalculation. A replay restart
		// on the SAME calendar day is a full recalculation but not a date
		// change, so the date-based reset below never fires on its own —
		// without this, StoppedForDay latched from a prior replay run (or
		// a prior day's flatten) would carry into every subsequent test.
		TradeDate = 0; TradesToday = 0; StoppedForDay = 0;
		DiagLastState = -1;
	}

	sc.SendOrdersToTradeService = In_SendToService.GetYesNo();
	sc.MaximumPositionAllowed = In_Quantity.GetInt();

	int lastBar = sc.ArraySize - 2; // last CLOSED brick
	if (lastBar < 1)
		return;

	// ---------------- daily counter reset ----------------
	int today = sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.ArraySize - 1]);
	if (today != TradeDate)
	{
		TradeDate = today;
		TradesToday = 0;
		StoppedForDay = 0;
	}

	// ---------------- trading permission ----------------
	bool realTime = !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0;
	bool tradingEnabled = In_Enable.GetYesNo() != 0 && realTime;

	// diagnostic: log every time the gate flips, so a "zero trades" report
	// can be traced to WHICH condition is false without guessing
	if ((int)tradingEnabled != DiagLastState)
	{
		SCString msg;
		msg.Format("RenkoFlip Auto Trader DIAG: tradingEnabled=%d (Enable=%d IsFullRecalc=%d DownloadingHist=%d)",
			(int)tradingEnabled, In_Enable.GetYesNo(), (int)sc.IsFullRecalculation,
			sc.DownloadingHistoricalData);
		sc.AddMessageToLog(msg, 0);
		DiagLastState = (int)tradingEnabled;
	}

	s_SCPositionData Position;
	sc.GetTradePosition(Position);

	// dollars per point, for the daily-loss check
	double perPoint = (sc.TickSize > 0) ? (sc.CurrencyValuePerTick / sc.TickSize) : 0;
	double dailyPnLPts = (perPoint > 0) ? (Position.DailyProfitLoss / perPoint) : 0;

	// daily loss enforcement (every call, real time only)
	if (tradingEnabled && !StoppedForDay)
	{
		bool lossHit = In_MaxDailyLoss.GetFloat() > 0 &&
		               dailyPnLPts <= -In_MaxDailyLoss.GetFloat();
		if (lossHit)
		{
			if (Position.PositionQuantity != 0 || Position.WorkingOrdersExist)
				sc.FlattenAndCancelAllOrders();
			StoppedForDay = 1;
			sc.AddMessageToLog("RenkoFlip Auto Trader: max daily loss hit — flattened and stopped for the day.", 1);
		}
	}

	// ---------------- MTF gate state ----------------
	int TFChartNums[3] = { In_TF1Chart.GetChartNumber(), In_TF2Chart.GetChartNumber(), In_TF3Chart.GetChartNumber() };
	int TFMinConsec[3] = { In_TF1MinConsec.GetInt(), In_TF2MinConsec.GetInt(), In_TF3MinConsec.GetInt() };
	int TFState[3]     = { 0, 0, 0 };

	for (int t = 0; t < 3; ++t)
	{
		if (TFChartNums[t] == 0)
			continue;

		SCGraphData RefData;
		sc.GetChartBaseData(TFChartNums[t], RefData);
		SCFloatArrayRef RefClose = RefData[SC_LAST];
		if (RefClose.GetArraySize() < 3)
			continue;

		int streak = GetCloseStreak(RefClose, RefClose.GetArraySize() - 2);
		if (streak >= TFMinConsec[t])       TFState[t] = 1;
		else if (-streak >= TFMinConsec[t]) TFState[t] = -1;
	}

	// ---------------- brick color source ----------------
	SCFloatArrayRef RenkoOpen  = sc.BaseData[SC_RENKO_OPEN];
	SCFloatArrayRef RenkoClose = sc.BaseData[SC_RENKO_CLOSE];
	bool haveRenko = RenkoClose.GetArraySize() > 0 && RenkoClose[lastBar] != 0;

	// brick size in points (for the breakeven-profit measure)
	float brickPts = haveRenko ? (RenkoClose[lastBar] - RenkoOpen[lastBar])
	                           : (sc.Close[lastBar] - sc.Open[lastBar]);
	if (brickPts < 0) brickPts = -brickPts;

	// ---------------- strategy mode / OTF filter ----------------
	int strategyMode = In_Strategy.GetIndex(); // 0 flip, 1 OTF, 2 FA, 3 OTF-only, 4 consensus
	s_ChartStudySubgraphValues OTFRef = In_OTFStudy.GetChartStudySubgraphValues();
	bool otfExternal = OTFRef.ChartNumber != 0 && OTFRef.ChartNumber != sc.ChartNumber;
	SCFloatArray OTFArr;
	if (strategyMode >= 1)
	{
		if (otfExternal)
			sc.GetStudyArrayFromChartUsingID(OTFRef.ChartNumber, OTFRef.StudyID, OTFRef.SubgraphIndex, OTFArr);
		else
			sc.GetStudyArrayUsingID(OTFRef.StudyID, OTFRef.SubgraphIndex, OTFArr);
	}

	// consensus mode: higher-TF OTF references + scale-in permission
	s_ChartStudySubgraphValues OTF2Ref = In_OTF2Study.GetChartStudySubgraphValues();
	s_ChartStudySubgraphValues OTF3Ref = In_OTF3Study.GetChartStudySubgraphValues();
	bool otf2On = OTF2Ref.StudyID != 0;
	bool otf3On = OTF3Ref.StudyID != 0;
	bool otf2External = OTF2Ref.ChartNumber != 0 && OTF2Ref.ChartNumber != sc.ChartNumber;
	bool otf3External = OTF3Ref.ChartNumber != 0 && OTF3Ref.ChartNumber != sc.ChartNumber;
	SCFloatArray OTF2Arr, OTF3Arr;
	if (strategyMode == 4)
	{
		if (otf2On)
		{
			if (otf2External)
				sc.GetStudyArrayFromChartUsingID(OTF2Ref.ChartNumber, OTF2Ref.StudyID, OTF2Ref.SubgraphIndex, OTF2Arr);
			else
				sc.GetStudyArrayUsingID(OTF2Ref.StudyID, OTF2Ref.SubgraphIndex, OTF2Arr);
		}
		if (otf3On)
		{
			if (otf3External)
				sc.GetStudyArrayFromChartUsingID(OTF3Ref.ChartNumber, OTF3Ref.StudyID, OTF3Ref.SubgraphIndex, OTF3Arr);
			else
				sc.GetStudyArrayUsingID(OTF3Ref.StudyID, OTF3Ref.SubgraphIndex, OTF3Arr);
		}
		sc.MaximumPositionAllowed = In_Quantity.GetInt() * 3;
	}
	sc.AllowMultipleEntriesInSameDirection = (strategyMode == 4) ? 1 : 0;

	int minSwing      = In_MinSwing.GetInt();
	int confirmBricks = In_ConfirmBricks.GetInt();

	// ---------------- process closed bricks ----------------
	for (int i = LastProcessed + 1; i <= lastBar; ++i)
	{
		float bOpen  = haveRenko ? RenkoOpen[i]  : sc.Open[i];
		float bClose = haveRenko ? RenkoClose[i] : sc.Close[i];
		int dir = 0;
		if (bClose > bOpen) dir = 1;
		else if (bClose < bOpen) dir = -1;
		if (dir == 0)
			continue;

		if (dir != LastDir && LastDir != 0)
		{
			PrevRunLen = RunLen;
			FlipValid = (PrevRunLen >= minSwing) ? 1 : 0;
			RunLen = 1;
		}
		else
		{
			RunLen++;
		}
		LastDir = dir;

		int signal = 0;
		if (FlipValid && RunLen == confirmBricks)
		{
			signal = dir;
			FlipValid = 0;
		}

		// Filtered Always-In / OTF Only: track signal + debounced filter
		// state on every closed brick; orders happen in the reconcile block
		if (strategyMode >= 2)
		{
			int fDir = 0;
			int fi = otfExternal
				? sc.GetContainingIndexForSCDateTime(OTFRef.ChartNumber, sc.BaseDateTimeIn[i])
				: i;
			if (otfExternal && In_FilterRead.GetIndex() == 0)
				fi -= 1; // last closed bar: live == recalculated
			if (fi >= 0 && OTFArr.GetArraySize() > fi)
			{
				if (OTFArr[fi] > 0) fDir = 1;
				else if (OTFArr[fi] < 0) fDir = -1;
			}
			if (fDir != 0 && fDir != FA_FCur)
			{
				if (fDir == FA_FPend) FA_FPendCount++;
				else { FA_FPend = fDir; FA_FPendCount = 1; }
				if (FA_FPendCount >= In_FADebounce.GetInt())
				{
					FA_FCur = fDir;
					FA_FPend = 0; FA_FPendCount = 0;
				}
			}
			else
			{
				FA_FPend = 0; FA_FPendCount = 0;
			}

			// OTF Consensus: direction from primary OTF; desired SIZE level
			// grows with each agreeing higher-TF OTF (1..3)
			if (strategyMode == 4)
			{
				if (otf2On)
				{
					int f2 = 0;
					int fj = otf2External
						? sc.GetContainingIndexForSCDateTime(OTF2Ref.ChartNumber, sc.BaseDateTimeIn[i])
						: i;
					if (otf2External && In_FilterRead.GetIndex() == 0)
						fj -= 1;
					if (fj >= 0 && OTF2Arr.GetArraySize() > fj)
					{
						if (OTF2Arr[fj] > 0) f2 = 1;
						else if (OTF2Arr[fj] < 0) f2 = -1;
					}
					if (f2 != 0 && f2 != F2Cur)
					{
						if (f2 == F2Pend) F2PendCount++;
						else { F2Pend = f2; F2PendCount = 1; }
						if (F2PendCount >= In_FADebounce.GetInt())
						{
							F2Cur = f2;
							F2Pend = 0; F2PendCount = 0;
						}
					}
					else { F2Pend = 0; F2PendCount = 0; }
				}
				if (otf3On)
				{
					int f3 = 0;
					int fj = otf3External
						? sc.GetContainingIndexForSCDateTime(OTF3Ref.ChartNumber, sc.BaseDateTimeIn[i])
						: i;
					if (otf3External && In_FilterRead.GetIndex() == 0)
						fj -= 1;
					if (fj >= 0 && OTF3Arr.GetArraySize() > fj)
					{
						if (OTF3Arr[fj] > 0) f3 = 1;
						else if (OTF3Arr[fj] < 0) f3 = -1;
					}
					if (f3 != 0 && f3 != F3Cur)
					{
						if (f3 == F3Pend) F3PendCount++;
						else { F3Pend = f3; F3PendCount = 1; }
						if (F3PendCount >= In_FADebounce.GetInt())
						{
							F3Cur = f3;
							F3Pend = 0; F3PendCount = 0;
						}
					}
					else { F3Pend = 0; F3PendCount = 0; }
				}

				int cdir = FA_FCur;
				if (FA_DStopBlock && cdir != 0 && cdir != FA_DStopDir)
					FA_DStopBlock = 0;
				int levels = 0;
				if (cdir != 0 && !FA_DStopBlock)
				{
					levels = 1;
					if (otf2On && F2Cur == cdir) levels++;
					if (otf3On && F3Cur == cdir) levels++;
				}
				FA_Due = cdir * levels; // signed level count (-3..+3)
				continue;
			}

			// OTF Only: desired position IS the debounced filter direction
			if (strategyMode == 3)
			{
				// disaster lockout clears when the filter changes direction
				if (FA_DStopBlock && FA_FCur != 0 && FA_FCur != FA_DStopDir)
					FA_DStopBlock = 0;
				FA_Due = FA_DStopBlock ? 0 : FA_FCur;
				continue;
			}

			if (signal != 0)
			{
				int sigTime = sc.BaseDateTimeIn[i].GetTime();
				if (sigTime >= In_SessionStart.GetTime() && sigTime <= In_SessionEnd.GetTime())
				{
					FA_SDir = signal;
					FA_LastSigBar = i;
					FA_DStopBlock = 0; // fresh signal clears the disaster lockout
				}
			}

			// desired position per brick, with entry permission evaluated
			// AT THIS BRICK (burst-safe: several bricks may close between
			// study calls — the reconcile block just executes FA_Due)
			{
				int tgt;
				if (FA_SDir != 0 && FA_SDir == FA_FCur)
					tgt = FA_SDir;
				else
					// disagreement: Go Flat exits; Hold keeps the current
					// desired position (always-in character)
					tgt = (In_FAPolicy.GetIndex() == 1) ? FA_Due : 0;

				if (tgt == 0)
				{
					FA_Due = 0;
				}
				else if (tgt != FA_Due)
				{
					bool sigBrick = (signal != 0 && signal == tgt);
					bool allowed = true;
					if (!sigBrick)
					{
						if (In_FAReentry.GetIndex() == 0)
							allowed = false; // Next Signal Only
						else if (In_FALookback.GetInt() > 0 && FA_LastSigBar >= 0 &&
						         (i - FA_LastSigBar) > In_FALookback.GetInt())
							allowed = false;
					}
					if (FA_DStopBlock)
						allowed = false; // wait for a fresh signal after a disaster stop
					FA_Due = allowed ? tgt : 0;
				}
			}
			continue; // mode 0/1 order path below does not apply
		}

		if (signal == 0)
			continue;

		// entries only on the freshest closed brick, in real time
		if (!tradingEnabled || i != lastBar)
			continue;

		int barTime = sc.BaseDateTimeIn[i].GetTime();
		bool inSession = barTime >= In_SessionStart.GetTime() &&
		                 barTime <= In_SessionEnd.GetTime();

		if (!inSession || StoppedForDay)
			continue;
		if (In_MaxTradesDay.GetInt() > 0 && TradesToday >= In_MaxTradesDay.GetInt())
			continue;

		// MTF gate: all enabled TFs must agree with the signal
		bool aligned = true;
		for (int t = 0; t < 3; ++t)
		{
			if (TFChartNums[t] == 0)
				continue;
			if (TFState[t] != signal)
				aligned = false;
		}
		if (!aligned)
			continue;

		// OTF Filtered mode: OTF gate + one trade at a time
		if (strategyMode == 1)
		{
			int oi = otfExternal
				? sc.GetContainingIndexForSCDateTime(OTFRef.ChartNumber, sc.BaseDateTimeIn[i])
				: i;
			if (otfExternal && In_FilterRead.GetIndex() == 0)
				oi -= 1; // last closed bar: live == recalculated
			// missing/unconfigured OTF data blocks entries (visible failure)
			if (oi < 0 || OTFArr.GetArraySize() <= oi)
				continue;
			float otf = OTFArr[oi];
			if ((signal > 0 && otf <= 0) || (signal < 0 && otf >= 0))
				continue;
			// same-direction position: no add-ons. Opposite position falls
			// through and the entry below reverses it (sc.SupportReversals).
			if (Position.PositionQuantity != 0 &&
			    (Position.PositionQuantity > 0) == (signal > 0))
				continue;
		}

		// ---- submit ----
		s_SCNewOrder Order;
		Order.OrderQuantity = In_Quantity.GetInt();
		Order.OrderType = SCT_ORDERTYPE_MARKET;
		if (strategyMode == 1)
		{
			// stop at the last-X-bricks extreme (static; BE move later)
			// 0 = no stop order: exits only via OTF flip / flatten time
			if (In_StopLookback.GetInt() > 0)
			{
				float stopPx;
				if (signal > 0)
				{
					stopPx = sc.Low[i];
					for (int k = i - In_StopLookback.GetInt() + 1; k < i; ++k)
						if (k >= 0 && sc.Low[k] < stopPx) stopPx = sc.Low[k];
				}
				else
				{
					stopPx = sc.High[i];
					for (int k = i - In_StopLookback.GetInt() + 1; k < i; ++k)
						if (k >= 0 && sc.High[k] > stopPx) stopPx = sc.High[k];
				}
				Order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;
				Order.Stop1Price = stopPx;
			}
		}
		else
		{
			if (In_StopTicks.GetInt() > 0)
			{
				Order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;
				Order.Stop1Offset = In_StopTicks.GetInt() * sc.TickSize;
			}
			if (In_TargetTicks.GetInt() > 0)
			{
				Order.AttachedOrderTarget1Type = SCT_ORDERTYPE_LIMIT;
				Order.Target1Offset = In_TargetTicks.GetInt() * sc.TickSize;
			}
		}

		int result = (signal > 0) ? sc.BuyEntry(Order) : sc.SellEntry(Order);
		if (result > 0)
		{
			TradesToday++;
			BEDone = 0;
			if (signal > 0)
				BuyEx[i] = sc.Low[i] - sc.TickSize * 4;
			else
				SellEx[i] = sc.High[i] + sc.TickSize * 4;
		}
		else
		{
			SCString msg;
			msg.Format("RenkoFlip Auto Trader: entry rejected, error %d", result);
			sc.AddMessageToLog(msg, 1);
		}
	}
	LastProcessed = lastBar;

	// ---------------- OTF Filtered mode: open-position management ----------
	if (strategyMode == 1 && tradingEnabled && Position.PositionQuantity != 0)
	{
		int posDir = (Position.PositionQuantity > 0) ? 1 : -1;

		// exit when the OTF filter flips against the position (checked on
		// the last closed brick)
		bool otfExit = false;
		{
			int oi = otfExternal
				? sc.GetContainingIndexForSCDateTime(OTFRef.ChartNumber, sc.BaseDateTimeIn[lastBar])
				: lastBar;
			if (otfExternal && In_FilterRead.GetIndex() == 0)
				oi -= 1; // last closed bar: live == recalculated
			if (oi >= 0 && OTFArr.GetArraySize() > oi)
			{
				float otf = OTFArr[oi];
				if ((posDir > 0 && otf <= 0) || (posDir < 0 && otf >= 0))
					otfExit = true;
			}
		}

		if (otfExit)
		{
			sc.FlattenAndCancelAllOrders();
			sc.AddMessageToLog("RenkoFlip Auto Trader: OTF flip — position flattened.", 0);
		}
		else
		{
			// breakeven: move the attached stop to entry after X bricks profit
			int beBricks = In_BEBricks.GetInt();
			if (beBricks > 0 && !BEDone && brickPts > 0)
			{
				double prof = (sc.Close[lastBar] - Position.AveragePrice) * posDir;
				if (prof >= beBricks * brickPts)
				{
					int Index = 0;
					s_SCTradeOrder ExistingOrder;
					while (sc.GetOrderByIndex(Index, ExistingOrder) != SCTRADING_ORDER_ERROR)
					{
						Index++;
						if (ExistingOrder.OrderTypeAsInt == SCT_ORDERTYPE_STOP &&
						    ExistingOrder.ParentInternalOrderID != 0 &&
						    IsWorkingOrderStatus(ExistingOrder.OrderStatusCode))
						{
							s_SCNewOrder ModifyOrder;
							ModifyOrder.InternalOrderID = ExistingOrder.InternalOrderID;
							ModifyOrder.Price1 = Position.AveragePrice;
							if (sc.ModifyOrder(ModifyOrder) > 0)
							{
								BEDone = 1;
								sc.AddMessageToLog("RenkoFlip Auto Trader: stop moved to breakeven.", 0);
							}
							break;
						}
					}
				}
			}
		}
	}

	// ---------------- Filtered Always-In: reconcile position ---------------
	// Target = S when S == F (debounced), else FLAT. Runs once per newly
	// closed brick in real time; self-heals missed fills on the next brick.
	if (strategyMode >= 2 && tradingEnabled && !StoppedForDay &&
	    lastBar != FA_LastRecon)
	{
		FA_LastRecon = lastBar;

		int reconTime = sc.BaseDateTimeIn[lastBar].GetTime();
		bool inSession = reconTime >= In_SessionStart.GetTime() &&
		                 reconTime <= In_SessionEnd.GetTime();
		if (inSession)
		{
			// FA_Due already carries the per-brick entry-permission decision
			int target = FA_Due;

			int posDir = 0;
			if (Position.PositionQuantity > 0) posDir = 1;
			else if (Position.PositionQuantity < 0) posDir = -1;

			// disaster stop: flatten and lock out until a fresh signal
			if (posDir != 0 && In_FADisaster.GetFloat() > 0 &&
			    (sc.Close[lastBar] - Position.AveragePrice) * posDir <= -In_FADisaster.GetFloat())
			{
				sc.FlattenAndCancelAllOrders();
				FA_DStopBlock = 1;
				FA_DStopDir = posDir;
				FA_Due = 0;
				target = 0;
				posDir = 0;
				sc.AddMessageToLog("RenkoFlip Auto Trader [FA]: disaster stop — flattened, waiting for fresh signal.", 1);
			}

			// FA_Due is a signed LEVEL: modes 2/3 use +/-1; consensus -3..+3.
			// Target quantity = level * Order Quantity.
			double targetQty = (double)target * In_Quantity.GetInt();
			double posQty = Position.PositionQuantity;

			if (targetQty != posQty)
			{
				bool tradesLeft = !(In_MaxTradesDay.GetInt() > 0 &&
				                    TradesToday >= In_MaxTradesDay.GetInt());

				if (targetQty == 0)
				{
					// exit is never gated by entry permission
					if (posQty != 0)
						sc.FlattenAndCancelAllOrders();
				}
				else if (posQty != 0 && (posQty > 0) == (targetQty > 0))
				{
					// same direction: scale in/out to the consensus size
					double diff = targetQty - posQty;
					s_SCNewOrder Order;
					Order.OrderType = SCT_ORDERTYPE_MARKET;
					if (diff > 0 && tradesLeft)
					{
						Order.OrderQuantity = diff;
						int result = (targetQty > 0) ? sc.BuyEntry(Order) : sc.SellEntry(Order);
						if (result > 0)
							TradesToday++;
					}
					else if (diff < 0)
					{
						Order.OrderQuantity = -diff;
						if (posQty > 0) sc.SellExit(Order);
						else            sc.BuyExit(Order);
					}
				}
				else if (tradesLeft)
				{
					// flat or opposite: one entry order establishes the target
					// (SupportReversals auto-sizes across zero)
					s_SCNewOrder Order;
					Order.OrderQuantity = (targetQty > 0) ? targetQty : -targetQty;
					Order.OrderType = SCT_ORDERTYPE_MARKET;
					int result = (targetQty > 0) ? sc.BuyEntry(Order) : sc.SellEntry(Order);
					if (result > 0)
					{
						TradesToday++;
						if (targetQty > 0)
							BuyEx[lastBar] = sc.Low[lastBar] - sc.TickSize * 4;
						else
							SellEx[lastBar] = sc.High[lastBar] + sc.TickSize * 4;
					}
					else
					{
						SCString msg;
						msg.Format("RenkoFlip Auto Trader [FA]: entry rejected, error %d", result);
						sc.AddMessageToLog(msg, 1);
					}
				}
			}
		}
	}

	// ---------------- status panel ----------------
	if (In_ShowPanel.GetYesNo())
	{
		SCString txt;
		txt.Format("AutoTrader[%s]: %s%s | Pos: %.0f | Trades today: %d | Day PnL: %+.2f pts",
			strategyMode == 4 ? "OTF-CONS" : (strategyMode == 3 ? "OTF-ONLY" : (strategyMode == 2 ? "FA" : (strategyMode == 1 ? "OTF" : "FLIP"))),
			In_Enable.GetYesNo() ? "ON" : "OFF",
			StoppedForDay ? " (stopped for day)" : "",
			Position.PositionQuantity,
			TradesToday,
			dailyPnLPts);

		s_UseTool Tool;
		Tool.Clear();
		Tool.ChartNumber = sc.ChartNumber;
		Tool.DrawingType = DRAWING_TEXT;
		Tool.LineNumber = 84211002;
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
}
