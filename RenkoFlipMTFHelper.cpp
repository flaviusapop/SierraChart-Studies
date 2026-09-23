// RenkoFlipMTFHelper.cpp
// ---------------------------------------------------------------
// Helper study (NO order submission).
// - Layer 1: Renko flip signals on the chart it is applied to
//   (reversal after >= MinSwingLength opposite-color bricks).
// - Layer 2: Consecutive-close direction state computed on up to
//   three external time-based charts (e.g. 1m / 3m / 5m) in the
//   same chartbook, read by chart number.
// - Signals that agree with all enabled TF states print full size;
//   others print faded (still visible for A/B comparison).
// - Every signal is logged to a CSV file, and its MFE/MAE outcome
//   over the next OutcomeBricks bricks is appended when known.
// ---------------------------------------------------------------

#include "sierrachart.h"

SCDLLName("RenkoFlip MTF Helper")

// ----- persistent pending-signal record (fixed pool) -----------
struct PendingSignal
{
	int    BarIndex;      // renko bar index of the signal
	int    Direction;     // +1 long, -1 short
	float  EntryPrice;
	int    Agree;         // 1 if MTF-aligned at signal time
	int    Streak[3];     // TF streaks captured at signal time
	float  OtfVal;        // OTF filter subgraph value at signal time
	SCDateTime SigTime;
	int    Active;
	int    NoLog;         // 1 = queued during full recalculation; skip CSV write
};

const int MAX_PENDING = 64;

// ----- helper: consecutive close-direction streak on an array ---
// Returns +N for N consecutive up-closes ending at last confirmed
// bar, -N for down, 0 for flat/unknown.
static int GetCloseStreak(SCFloatArray& CloseArr, int LastConfirmedIndex)
{
	if (LastConfirmedIndex < 2)
		return 0;

	int i = LastConfirmedIndex;
	float c0 = CloseArr[i];
	float c1 = CloseArr[i - 1];

	int dir = 0;
	if (c0 > c1) dir = 1;
	else if (c0 < c1) dir = -1;
	else return 0;

	int streak = 1;
	for (int k = i - 1; k >= 1; --k)
	{
		float a = CloseArr[k];
		float b = CloseArr[k - 1];
		int d = 0;
		if (a > b) d = 1;
		else if (a < b) d = -1;

		if (d == dir)
			streak++;
		else
			break;
	}
	return dir * streak;
}

SCSFExport scsf_RenkoFlipMTFHelper(SCStudyInterfaceRef sc)
{
	// ---------------- Subgraphs ----------------
	SCSubgraphRef BuyFull   = sc.Subgraph[0];
	SCSubgraphRef SellFull  = sc.Subgraph[1];
	SCSubgraphRef BuyFaded  = sc.Subgraph[2];
	SCSubgraphRef SellFaded = sc.Subgraph[3];
	SCSubgraphRef HypoPnL      = sc.Subgraph[4]; // running hypothetical flip P&L (pts)
	SCSubgraphRef HypoPnLLong  = sc.Subgraph[5]; // long-side portion
	SCSubgraphRef HypoPnLShort = sc.Subgraph[6]; // short-side portion
	SCSubgraphRef FiltPnL      = sc.Subgraph[7]; // filtered strategy P&L (stop-based)
	SCSubgraphRef StopViz      = sc.Subgraph[8];  // trailing stop level while in a trade
	SCSubgraphRef EntryMark    = sc.Subgraph[9];  // "Entry Long" text
	SCSubgraphRef EntryShortMark = sc.Subgraph[13]; // "Entry Short" text
	SCSubgraphRef RevMark      = sc.Subgraph[14]; // "Flip" text on signal-reversal exits
	SCSubgraphRef ExitStopMark = sc.Subgraph[10]; // "Stop" text on stop-loss exits
	SCSubgraphRef ExitOTFMark  = sc.Subgraph[11]; // "OTF" text on OTF-flip exits
	SCSubgraphRef BEMark       = sc.Subgraph[12]; // "BE" text when stop moves to breakeven

	// ---------------- Inputs ----------------
	SCInputRef In_MinSwing        = sc.Input[0];
	SCInputRef In_TF1Chart        = sc.Input[1];
	SCInputRef In_TF2Chart        = sc.Input[2];
	SCInputRef In_TF3Chart        = sc.Input[3];
	SCInputRef In_TF1MinConsec    = sc.Input[4];
	SCInputRef In_TF2MinConsec    = sc.Input[5];
	SCInputRef In_TF3MinConsec    = sc.Input[6];
	SCInputRef In_UseTFGateForFull= sc.Input[7];
	SCInputRef In_SessionStart    = sc.Input[8];
	SCInputRef In_SessionEnd      = sc.Input[9];
	SCInputRef In_OutcomeBricks   = sc.Input[10];
	SCInputRef In_LogPath         = sc.Input[11];
	SCInputRef In_ShowTable       = sc.Input[12];
	SCInputRef In_ConfirmBricks   = sc.Input[13];
	SCInputRef In_UseOTFGate      = sc.Input[14];
	SCInputRef In_OTFStudy        = sc.Input[15];
	SCInputRef In_StopLookback    = sc.Input[16];
	SCInputRef In_BEBricks        = sc.Input[17];
	SCInputRef In_StopMode        = sc.Input[18];

	if (sc.SetDefaults)
	{
		sc.GraphName = "RenkoFlip MTF Helper";
		sc.AutoLoop = 0;                 // manual loop
		sc.GraphRegion = 0;
		sc.ValueFormat = 2;

		BuyFull.Name = "Buy (aligned)";
		BuyFull.DrawStyle = DRAWSTYLE_ARROW_UP;
		BuyFull.PrimaryColor = RGB(0, 220, 120);
		BuyFull.LineWidth = 3;
		BuyFull.DrawZeros = false;

		SellFull.Name = "Sell (aligned)";
		SellFull.DrawStyle = DRAWSTYLE_ARROW_DOWN;
		SellFull.PrimaryColor = RGB(255, 70, 70);
		SellFull.LineWidth = 3;
		SellFull.DrawZeros = false;

		BuyFaded.Name = "Buy (blocked by TF gate)";
		BuyFaded.DrawStyle = DRAWSTYLE_ARROW_UP;
		BuyFaded.PrimaryColor = RGB(80, 120, 100);
		BuyFaded.LineWidth = 1;
		BuyFaded.DrawZeros = false;

		SellFaded.Name = "Sell (blocked by TF gate)";
		SellFaded.DrawStyle = DRAWSTYLE_ARROW_DOWN;
		SellFaded.PrimaryColor = RGB(130, 80, 80);
		SellFaded.LineWidth = 1;
		SellFaded.DrawZeros = false;

		HypoPnL.Name = "Hypothetical Flip PnL (pts)";
		HypoPnL.DrawStyle = DRAWSTYLE_IGNORE;

		HypoPnLLong.Name = "Hypothetical Flip PnL Longs (pts)";
		HypoPnLLong.DrawStyle = DRAWSTYLE_IGNORE;

		HypoPnLShort.Name = "Hypothetical Flip PnL Shorts (pts)";
		HypoPnLShort.DrawStyle = DRAWSTYLE_IGNORE;

		FiltPnL.Name = "Filtered PnL (pts, trailing stop)";
		FiltPnL.DrawStyle = DRAWSTYLE_IGNORE;

		StopViz.Name = "Filtered Trade Stop";
		StopViz.DrawStyle = DRAWSTYLE_DASH;
		StopViz.PrimaryColor = RGB(255, 165, 0);
		StopViz.LineWidth = 2;
		StopViz.DrawZeros = false;

		EntryMark.Name = "Filtered Entry Long";
		EntryMark.DrawStyle = DRAWSTYLE_TEXT;
		EntryMark.TextDrawStyleText = "Entry Long";
		EntryMark.PrimaryColor = RGB(0, 220, 120);
		EntryMark.LineWidth = 10;
		EntryMark.DrawZeros = false;

		EntryShortMark.Name = "Filtered Entry Short";
		EntryShortMark.DrawStyle = DRAWSTYLE_TEXT;
		EntryShortMark.TextDrawStyleText = "Entry Short";
		EntryShortMark.PrimaryColor = RGB(255, 100, 100);
		EntryShortMark.LineWidth = 10;
		EntryShortMark.DrawZeros = false;

		RevMark.Name = "Filtered Exit (Signal Flip)";
		RevMark.DrawStyle = DRAWSTYLE_TEXT;
		RevMark.TextDrawStyleText = "Flip";
		RevMark.PrimaryColor = RGB(200, 140, 255);
		RevMark.LineWidth = 10;
		RevMark.DrawZeros = false;

		ExitStopMark.Name = "Filtered Exit (Stop)";
		ExitStopMark.DrawStyle = DRAWSTYLE_TEXT;
		ExitStopMark.TextDrawStyleText = "Stop";
		ExitStopMark.PrimaryColor = RGB(255, 120, 120);
		ExitStopMark.LineWidth = 10;
		ExitStopMark.DrawZeros = false;

		ExitOTFMark.Name = "Filtered Exit (OTF flip)";
		ExitOTFMark.DrawStyle = DRAWSTYLE_TEXT;
		ExitOTFMark.TextDrawStyleText = "OTF";
		ExitOTFMark.PrimaryColor = RGB(255, 200, 80);
		ExitOTFMark.LineWidth = 10;
		ExitOTFMark.DrawZeros = false;

		BEMark.Name = "Stop To Breakeven";
		BEMark.DrawStyle = DRAWSTYLE_TEXT;
		BEMark.TextDrawStyleText = "BE";
		BEMark.PrimaryColor = RGB(120, 200, 255);
		BEMark.LineWidth = 10;
		BEMark.DrawZeros = false;

		In_MinSwing.Name = "Min Swing Length (bricks before reversal)";
		In_MinSwing.SetInt(2);
		In_MinSwing.SetIntLimits(1, 50);

		In_TF1Chart.Name = "TF1 Chart Number (e.g. 1 min) — 0 = off";
		In_TF1Chart.SetChartNumber(0);
		In_TF2Chart.Name = "TF2 Chart Number (e.g. 3 min) — 0 = off";
		In_TF2Chart.SetChartNumber(0);
		In_TF3Chart.Name = "TF3 Chart Number (e.g. 5 min) — 0 = off";
		In_TF3Chart.SetChartNumber(0);

		In_TF1MinConsec.Name = "TF1 Min Consecutive Closes";
		In_TF1MinConsec.SetInt(1);
		In_TF2MinConsec.Name = "TF2 Min Consecutive Closes";
		In_TF2MinConsec.SetInt(1);
		In_TF3MinConsec.Name = "TF3 Min Consecutive Closes";
		In_TF3MinConsec.SetInt(1);

		In_UseTFGateForFull.Name = "Full arrows require all enabled TFs aligned";
		In_UseTFGateForFull.SetYesNo(1);

		In_SessionStart.Name = "Session Start (chart timezone)";
		In_SessionStart.SetTime(HMS_TIME(16, 40, 0));  // 09:40 NY = 16:40 Bucharest
		In_SessionEnd.Name = "Session End (chart timezone)";
		In_SessionEnd.SetTime(HMS_TIME(17, 40, 0));

		In_OutcomeBricks.Name = "Outcome window (bricks) for MFE/MAE logging";
		In_OutcomeBricks.SetInt(10);
		In_OutcomeBricks.SetIntLimits(1, 200);

		In_LogPath.Name = "CSV Log Path";
		In_LogPath.SetString("C:\\SierraChart\\Data\\RenkoFlipSignals.csv");

		In_ShowTable.Name = "Show MTF state table";
		In_ShowTable.SetYesNo(1);

		In_ConfirmBricks.Name = "Confirmation Bricks (new color; 1 = arrow on flip brick)";
		In_ConfirmBricks.SetInt(2);
		In_ConfirmBricks.SetIntLimits(1, 20);

		In_UseOTFGate.Name = "Use OTF Filter gate (full arrows require agreement)";
		In_UseOTFGate.SetYesNo(0);

		In_OTFStudy.Name = "OTF Filter: chart + study subgraph (can be another chart)";
		In_OTFStudy.SetChartStudySubgraphValues(0, 0, 0);

		In_StopLookback.Name = "Filtered PnL: Stop Lookback Bricks (X, trailing)";
		In_StopLookback.SetInt(3);
		In_StopLookback.SetIntLimits(1, 50);

		In_BEBricks.Name = "Move Stop To Breakeven After X Bricks Profit (0 = off)";
		In_BEBricks.SetInt(2);
		In_BEBricks.SetIntLimits(0, 50);

		In_StopMode.Name = "Stop Mode";
		In_StopMode.SetCustomInputStrings("Static + Breakeven;Trailing");
		In_StopMode.SetCustomInputIndex(0);

		return;
	}

	// ---------------- persistent state ----------------
	int&   LastDir        = sc.GetPersistentInt(1);   // current brick color run direction
	int&   RunLen         = sc.GetPersistentInt(2);   // length of current same-color run
	int&   PrevRunLen     = sc.GetPersistentInt(3);   // length of the completed previous run
	int&   LastProcessed  = sc.GetPersistentInt(4);   // last fully processed bar index
	int&   HeaderWritten  = sc.GetPersistentInt(5);
	int&   PosDir         = sc.GetPersistentInt(6);   // hypothetical flip position
	int&   FlipValid      = sc.GetPersistentInt(7);   // current run began with a qualifying flip
	int&   FPosDir        = sc.GetPersistentInt(8);   // filtered-strategy position (+1/-1/0)
	float& PosEntry       = sc.GetPersistentFloat(1);
	float& CumPnL         = sc.GetPersistentFloat(2);
	float& CumPnLLong     = sc.GetPersistentFloat(3);
	float& CumPnLShort    = sc.GetPersistentFloat(4);
	float& FPosEntry      = sc.GetPersistentFloat(5);
	float& FStop          = sc.GetPersistentFloat(6);
	float& FCumPnL        = sc.GetPersistentFloat(7);
	float& FCumPnLLong    = sc.GetPersistentFloat(8);
	float& FCumPnLShort   = sc.GetPersistentFloat(9);

	PendingSignal* Pend = (PendingSignal*)sc.GetPersistentPointer(1);
	if (Pend == NULL)
	{
		Pend = (PendingSignal*)sc.AllocateMemory(sizeof(PendingSignal) * MAX_PENDING);
		if (Pend == NULL)
			return;
		memset(Pend, 0, sizeof(PendingSignal) * MAX_PENDING);
		sc.SetPersistentPointer(1, Pend);
	}

	if (sc.LastCallToFunction)
	{
		sc.FreeMemory(Pend);
		sc.SetPersistentPointer(1, NULL);
		return;
	}

	if (sc.IsFullRecalculation)
	{
		LastDir = 0; RunLen = 0; PrevRunLen = 0; LastProcessed = -1;
		FlipValid = 0;
		PosDir = 0; PosEntry = 0; CumPnL = 0;
		CumPnLLong = 0; CumPnLShort = 0;
		FPosDir = 0; FPosEntry = 0; FStop = 0;
		FCumPnL = 0; FCumPnLLong = 0; FCumPnLShort = 0;
		memset(Pend, 0, sizeof(PendingSignal) * MAX_PENDING);
	}

	// ---------------- MTF state (current, from external charts) ----------------
	int TFChartNums[3]  = { In_TF1Chart.GetChartNumber(), In_TF2Chart.GetChartNumber(), In_TF3Chart.GetChartNumber() };
	int TFMinConsec[3]  = { In_TF1MinConsec.GetInt(), In_TF2MinConsec.GetInt(), In_TF3MinConsec.GetInt() };
	int TFStreak[3]     = { 0, 0, 0 };   // signed streak
	int TFState[3]      = { 0, 0, 0 };   // -1 / 0 / +1 after MinConsec threshold

	for (int t = 0; t < 3; ++t)
	{
		if (TFChartNums[t] == 0)
			continue;

		SCGraphData RefData;
		sc.GetChartBaseData(TFChartNums[t], RefData);
		SCFloatArrayRef RefClose = RefData[SC_LAST];
		int RefSize = RefClose.GetArraySize();
		if (RefSize < 3)
			continue;

		// use last CONFIRMED bar of the reference chart (skip forming bar)
		int lastConfirmed = RefSize - 2;
		TFStreak[t] = GetCloseStreak(RefClose, lastConfirmed);

		if (TFStreak[t] >= TFMinConsec[t])       TFState[t] = 1;
		else if (-TFStreak[t] >= TFMinConsec[t]) TFState[t] = -1;
		else                                     TFState[t] = 0;
	}

	// ---------------- process confirmed renko bars ----------------
	int lastBar = sc.ArraySize - 2; // last CLOSED brick
	if (lastBar < 1)
		return;

	int minSwing = In_MinSwing.GetInt();
	int confirmBricks = In_ConfirmBricks.GetInt();
	int outWindow = In_OutcomeBricks.GetInt();
	int stopLookback = In_StopLookback.GetInt();

	// OTF Filter gate: referenced study subgraph, optionally on another chart
	// in the chartbook (values mapped to bricks by time).
	// Sign convention: > 0 bullish, < 0 bearish, 0 neutral/blocks both.
	int useOTF = In_UseOTFGate.GetYesNo();
	s_ChartStudySubgraphValues OTFRef = In_OTFStudy.GetChartStudySubgraphValues();
	bool otfExternal = OTFRef.ChartNumber != 0 && OTFRef.ChartNumber != sc.ChartNumber;
	SCFloatArray OTFArr;
	if (useOTF)
	{
		if (otfExternal)
			sc.GetStudyArrayFromChartUsingID(OTFRef.ChartNumber, OTFRef.StudyID, OTFRef.SubgraphIndex, OTFArr);
		else
			sc.GetStudyArrayUsingID(OTFRef.StudyID, OTFRef.SubgraphIndex, OTFArr);
	}

	// On Renko charts the box open/close (what the chart colors by) live in
	// dedicated arrays; sc.Open/sc.Close hold actual trade prices and can
	// disagree with the visual brick color. Fall back for non-Renko charts.
	SCFloatArrayRef RenkoOpen  = sc.BaseData[SC_RENKO_OPEN];
	SCFloatArrayRef RenkoClose = sc.BaseData[SC_RENKO_CLOSE];
	bool haveRenko = RenkoClose.GetArraySize() > 0 && RenkoClose[lastBar] != 0;

	// brick size in points (for the breakeven-profit measure)
	float brickPts = haveRenko ? (RenkoClose[lastBar] - RenkoOpen[lastBar])
	                           : (sc.Close[lastBar] - sc.Open[lastBar]);
	if (brickPts < 0) brickPts = -brickPts;

	for (int i = LastProcessed + 1; i <= lastBar; ++i)
	{
		// ---- filtered-strategy trade management (every closed brick) ----
		// order: hard stop (intrabar) -> OTF-flip exit (at close)
		//        -> trail stop -> breakeven move
		if (FPosDir != 0)
		{
			int fdir = FPosDir;

			// OTF value for this brick (external chart mapped by time)
			bool otfAvail = false;
			float otfNow = 0;
			if (useOTF)
			{
				int oi = otfExternal
					? sc.GetContainingIndexForSCDateTime(OTFRef.ChartNumber, sc.BaseDateTimeIn[i])
					: i;
				if (oi >= 0 && OTFArr.GetArraySize() > oi)
				{
					otfAvail = true;
					otfNow = OTFArr[oi];
				}
			}

			// 1) hard stop first, assumed filled at stop price
			if ((fdir > 0 && sc.Low[i] <= FStop) || (fdir < 0 && sc.High[i] >= FStop))
			{
				float pnl = (fdir > 0) ? (FStop - FPosEntry) : (FPosEntry - FStop);
				FCumPnL += pnl;
				if (fdir > 0) FCumPnLLong += pnl; else FCumPnLShort += pnl;
				ExitStopMark[i] = FStop;
				FPosDir = 0;
			}
			// 2) OTF filter flipped against the position: exit at brick close
			else if (useOTF && otfAvail &&
			         ((fdir > 0 && otfNow <= 0) || (fdir < 0 && otfNow >= 0)))
			{
				float pnl = (sc.Close[i] - FPosEntry) * fdir;
				FCumPnL += pnl;
				if (fdir > 0) FCumPnLLong += pnl; else FCumPnLShort += pnl;
				ExitOTFMark[i] = sc.Close[i];
				FPosDir = 0;
			}
			else
			{
				// 3) stop maintenance. Static mode (default): stop stays at
				//    the entry-time level until the breakeven move — exits
				//    happen only at the stop or on an OTF flip.
				//    Trailing mode: tighten to last-X-bricks extreme.
				if (In_StopMode.GetIndex() == 1)
				{
					if (fdir > 0)
					{
						float lo = sc.Low[i];
						for (int k = i - stopLookback + 1; k < i; ++k)
							if (k >= 0 && sc.Low[k] < lo) lo = sc.Low[k];
						if (lo > FStop) FStop = lo;
					}
					else
					{
						float hi = sc.High[i];
						for (int k = i - stopLookback + 1; k < i; ++k)
							if (k >= 0 && sc.High[k] > hi) hi = sc.High[k];
						if (hi < FStop) FStop = hi;
					}
				}

				// 4) breakeven: after X bricks of profit, stop to entry
				//    (only if entry is tighter than the current trail)
				int beBricks = In_BEBricks.GetInt();
				if (beBricks > 0 && brickPts > 0)
				{
					float prof = (sc.Close[i] - FPosEntry) * fdir;
					if (prof >= beBricks * brickPts &&
					    ((fdir > 0 && FStop < FPosEntry) || (fdir < 0 && FStop > FPosEntry)))
					{
						FStop = FPosEntry;
						BEMark[i] = FPosEntry;
					}
				}
			}
		}

		float bOpen  = haveRenko ? RenkoOpen[i]  : sc.Open[i];
		float bClose = haveRenko ? RenkoClose[i] : sc.Close[i];
		int dir = 0;
		if (bClose > bOpen) dir = 1;
		else if (bClose < bOpen) dir = -1;
		if (dir == 0)
		{
			HypoPnL[i] = CumPnL;
			HypoPnLLong[i] = CumPnLLong;
			HypoPnLShort[i] = CumPnLShort;
			FiltPnL[i] = FCumPnL;
			if (FPosDir != 0)
				StopViz[i] = FStop;
			continue;
		}

		int signal = 0;
		if (dir != LastDir && LastDir != 0)
		{
			// color flip: run qualifies as a reversal candidate only if the
			// completed opposite run was long enough (chop filter)
			PrevRunLen = RunLen;
			FlipValid = (PrevRunLen >= minSwing) ? 1 : 0;
			RunLen = 1;
		}
		else
		{
			RunLen++;
		}
		LastDir = dir;

		// signal fires on the Nth brick of the new color (confirmation);
		// with Confirmation Bricks = 1 this is the flip brick itself
		if (FlipValid && RunLen == confirmBricks)
		{
			signal = dir;
			FlipValid = 0; // one signal per qualifying flip
		}

		// session filter
		int barTime = sc.BaseDateTimeIn[i].GetTime();
		bool inSession = (barTime >= In_SessionStart.GetTime() && barTime <= In_SessionEnd.GetTime());

		if (signal != 0 && inSession)
		{
			// MTF alignment at signal time (current TF states — live approximation;
			// on full recalculation historical TF states are not reconstructed)
			bool aligned = true;
			for (int t = 0; t < 3; ++t)
			{
				if (TFChartNums[t] == 0)
					continue;
				if (TFState[t] != signal)
					aligned = false;
			}

			float otfVal = 0;
			if (useOTF)
			{
				int oi = otfExternal
					? sc.GetContainingIndexForSCDateTime(OTFRef.ChartNumber, sc.BaseDateTimeIn[i])
					: i;
				if (oi >= 0 && OTFArr.GetArraySize() > oi)
				{
					otfVal = OTFArr[oi];
					if ((signal > 0 && otfVal <= 0) || (signal < 0 && otfVal >= 0))
						aligned = false;
				}
				else
				{
					// OTF gate enabled but study subgraph not configured or
					// unavailable — block rather than silently pass everything
					aligned = false;
				}
			}

			// ---- filtered-strategy entry / reversal ----
			// flat -> enter; opposite position -> close at this brick's
			// close ("Flip") and reverse. Same-direction signals while in
			// a trade are ignored.
			if (aligned && FPosDir != signal)
			{
				if (FPosDir != 0)
				{
					float pnl = (sc.Close[i] - FPosEntry) * FPosDir;
					FCumPnL += pnl;
					if (FPosDir > 0) FCumPnLLong += pnl; else FCumPnLShort += pnl;
					RevMark[i] = sc.Close[i];
				}
				FPosDir = signal;
				FPosEntry = sc.Close[i];
				if (signal > 0)
					EntryMark[i] = sc.Low[i] - sc.TickSize * 10;
				else
					EntryShortMark[i] = sc.High[i] + sc.TickSize * 10;
				if (signal > 0)
				{
					float lo = sc.Low[i];
					for (int k = i - stopLookback + 1; k < i; ++k)
						if (k >= 0 && sc.Low[k] < lo) lo = sc.Low[k];
					FStop = lo;
				}
				else
				{
					float hi = sc.High[i];
					for (int k = i - stopLookback + 1; k < i; ++k)
						if (k >= 0 && sc.High[k] > hi) hi = sc.High[k];
					FStop = hi;
				}
			}

			bool showFull = aligned || In_UseTFGateForFull.GetYesNo() == 0;

			if (signal > 0)
			{
				if (showFull) BuyFull[i]  = sc.Low[i]  - sc.TickSize * 4;
				else          BuyFaded[i] = sc.Low[i]  - sc.TickSize * 4;
			}
			else
			{
				if (showFull) SellFull[i]  = sc.High[i] + sc.TickSize * 4;
				else          SellFaded[i] = sc.High[i] + sc.TickSize * 4;
			}

			// hypothetical always-in flip P&L (Approach 1, ungated)
			if (PosDir != 0)
			{
				float tradePnL = (sc.Close[i] - PosEntry) * PosDir;
				CumPnL += tradePnL;
				if (PosDir > 0) CumPnLLong  += tradePnL;
				else            CumPnLShort += tradePnL;
			}
			PosDir = signal;
			PosEntry = sc.Close[i];

			// queue for outcome logging
			for (int p = 0; p < MAX_PENDING; ++p)
			{
				if (!Pend[p].Active)
				{
					Pend[p].Active = 1;
					Pend[p].BarIndex = i;
					Pend[p].Direction = signal;
					Pend[p].EntryPrice = sc.Close[i];
					Pend[p].Agree = aligned ? 1 : 0;
					Pend[p].Streak[0] = TFStreak[0];
					Pend[p].Streak[1] = TFStreak[1];
					Pend[p].Streak[2] = TFStreak[2];
					Pend[p].OtfVal = otfVal;
					Pend[p].SigTime = sc.BaseDateTimeIn[i];
					// historical signals re-seen on a full recalculation were
					// already logged live — suppress their CSV rows
					Pend[p].NoLog = sc.IsFullRecalculation ? 1 : 0;
					break;
				}
			}
		}

		HypoPnL[i] = CumPnL;
		HypoPnLLong[i] = CumPnLLong;
		HypoPnLShort[i] = CumPnLShort;
		FiltPnL[i] = FCumPnL;
		if (FPosDir != 0)
			StopViz[i] = FStop;
	}
	LastProcessed = lastBar;

	// ---------------- resolve pending signals: MFE/MAE after N bricks ----------
	for (int p = 0; p < MAX_PENDING; ++p)
	{
		if (!Pend[p].Active)
			continue;
		int endBar = Pend[p].BarIndex + outWindow;
		if (endBar > lastBar)
			continue; // outcome window not complete yet

		if (Pend[p].NoLog)
		{
			Pend[p].Active = 0;
			continue;
		}

		float mfe = 0, mae = 0;
		for (int k = Pend[p].BarIndex + 1; k <= endBar; ++k)
		{
			float fav = (Pend[p].Direction > 0) ? (sc.High[k] - Pend[p].EntryPrice)
			                                    : (Pend[p].EntryPrice - sc.Low[k]);
			float adv = (Pend[p].Direction > 0) ? (Pend[p].EntryPrice - sc.Low[k])
			                                    : (sc.High[k] - Pend[p].EntryPrice);
			if (fav > mfe) mfe = fav;
			if (adv > mae) mae = adv;
		}

		// ---- write CSV row ----
		int fileHandle = 0;
		const char* path = In_LogPath.GetString();

		if (!HeaderWritten)
		{
			// write header only if file does not exist yet
			if (sc.OpenFile(path, n_ACSIL::FILE_MODE_OPEN_EXISTING_FOR_SEQUENTIAL_READING, fileHandle))
				sc.CloseFile(fileHandle);
			else
			{
				if (sc.OpenFile(path, n_ACSIL::FILE_MODE_CREATE_AND_OPEN_FOR_READ_WRITE, fileHandle))
				{
					SCString hdr("DateTime,Direction,EntryPrice,MTF_Aligned,TF1_Streak,TF2_Streak,TF3_Streak,OTF_Value,MFE_pts,MAE_pts,OutcomeBricks\r\n");
					unsigned int written = 0;
					sc.WriteFile(fileHandle, hdr.GetChars(), hdr.GetLength(), &written);
					sc.CloseFile(fileHandle);
				}
			}
			HeaderWritten = 1;
		}

		if (sc.OpenFile(path, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle))
		{
			SCString row;
			SCString dt = sc.DateTimeToString(Pend[p].SigTime, FLAG_DT_COMPLETE_DATETIME);
			row.Format("%s,%s,%.2f,%d,%d,%d,%d,%.2f,%.2f,%.2f,%d\r\n",
				dt.GetChars(),
				Pend[p].Direction > 0 ? "LONG" : "SHORT",
				Pend[p].EntryPrice,
				Pend[p].Agree,
				Pend[p].Streak[0], Pend[p].Streak[1], Pend[p].Streak[2],
				Pend[p].OtfVal,
				mfe, mae, outWindow);
			unsigned int written = 0;
			sc.WriteFile(fileHandle, row.GetChars(), row.GetLength(), &written);
			sc.CloseFile(fileHandle);
		}

		Pend[p].Active = 0;
	}

	// ---------------- on-chart MTF state table ----------------
	if (In_ShowTable.GetYesNo())
	{
		SCString txt;
		const char* names[3] = { "TF1", "TF2", "TF3" };
		SCString line;
		txt.Format("Flip PnL: %+.2f pts (L: %+.2f | S: %+.2f)",
			CumPnL, CumPnLLong, CumPnLShort);
		if (useOTF)
		{
			int oi = otfExternal
				? sc.GetContainingIndexForSCDateTime(OTFRef.ChartNumber, sc.BaseDateTimeIn[lastBar])
				: lastBar;
			if (oi >= 0 && OTFArr.GetArraySize() > oi)
			{
				line.Format("  |  OTF: %+.2f", OTFArr[oi]);
				txt += line;
			}
		}
		line.Format("  ||  Filt PnL: %+.2f (L: %+.2f | S: %+.2f)%s",
			FCumPnL, FCumPnLLong, FCumPnLShort,
			FPosDir > 0 ? " [LONG]" : (FPosDir < 0 ? " [SHORT]" : ""));
		txt += line;
		for (int t = 0; t < 3; ++t)
		{
			if (TFChartNums[t] == 0)
				continue;
			line.Format("  |  %s: %s%d", names[t],
				TFStreak[t] > 0 ? "+" : "", TFStreak[t]);
			txt += line;
		}

		s_UseTool Tool;
		Tool.Clear();
		Tool.ChartNumber = sc.ChartNumber;
		Tool.DrawingType = DRAWING_TEXT;
		Tool.LineNumber = 84211001;
		Tool.AddAsUserDrawnDrawing = 0;
		Tool.AddMethod = UTAM_ADD_OR_ADJUST;
		Tool.UseRelativeVerticalValues = 1;
		Tool.BeginDateTime = 3;   // % from left
		Tool.BeginValue = 95;     // % from bottom
		Tool.Color = RGB(230, 230, 230);
		Tool.FontSize = 12;
		Tool.FontBold = 1;
		Tool.Text = txt;
		sc.UseTool(Tool);
	}
}
