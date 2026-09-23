// RenkoFlipFilteredAlwaysIn.cpp
// ---------------------------------------------------------------
// Helper study (NO orders). Clean implementation of the
// "Filtered Always-In" target-state model — see
// Trading/Futures_Day_Trading/RenkoFlip_FilteredAlwaysIn_Design.md
//
// Per closed brick:
//   S = last confirmed flip-signal direction (standing renko trend)
//   F = filter direction (debounced sign of a referenced study
//       subgraph, optionally on another chart, time-mapped)
//   Target = S when S == F; on disagreement: Flat or Hold (input)
// Entries happen when the position differs from Target:
//   - on the signal brick itself (signal + filter aligned), or
//   - when the filter flips into alignment with a recent signal
//     ("use the signal as confirmation, enter on the OTF change"),
//     gated by a signal-lookback window (input, 0 = unlimited)
//     and only in re-entry mode Immediate.
// Exits: any confirmed opposite signal always exits (X=Flat), or
// position held through disagreement (X=Hold) with an optional
// disaster stop in points as insurance.
// Tracks BOTH P&Ls for comparison: raw always-in flip vs filtered.
// ---------------------------------------------------------------

#include "sierrachart.h"

SCDLLName("RenkoFlip Filtered Always-In")

SCSFExport scsf_RenkoFlipFilteredAlwaysIn(SCStudyInterfaceRef sc)
{
	// ---------------- Subgraphs ----------------
	SCSubgraphRef BuySig     = sc.Subgraph[0];  // raw signal arrows (all)
	SCSubgraphRef SellSig    = sc.Subgraph[1];
	SCSubgraphRef EntryLong  = sc.Subgraph[2];  // filtered-track entries on signal bricks
	SCSubgraphRef EntryShort = sc.Subgraph[3];
	SCSubgraphRef FilterIn   = sc.Subgraph[4];  // filter-triggered (deferred) entries
	SCSubgraphRef CutMark    = sc.Subgraph[5];  // exit to flat on disagreement
	SCSubgraphRef DStopMark  = sc.Subgraph[6];  // disaster stop exit
	SCSubgraphRef FlipPnL    = sc.Subgraph[7];  // raw always-in P&L (pts)
	SCSubgraphRef FiltPnL    = sc.Subgraph[8];  // filtered track P&L (pts)
	SCSubgraphRef FiltPnLL   = sc.Subgraph[9];
	SCSubgraphRef FiltPnLS   = sc.Subgraph[10];

	// ---------------- Inputs ----------------
	SCInputRef In_MinSwing     = sc.Input[0];
	SCInputRef In_Confirm      = sc.Input[1];
	SCInputRef In_SessionStart = sc.Input[2];
	SCInputRef In_SessionEnd   = sc.Input[3];
	SCInputRef In_UseFilter    = sc.Input[4];
	SCInputRef In_FilterRef    = sc.Input[5];
	SCInputRef In_Debounce     = sc.Input[6];
	SCInputRef In_Policy       = sc.Input[7];
	SCInputRef In_ReentryMode  = sc.Input[8];
	SCInputRef In_SigLookback  = sc.Input[9];
	SCInputRef In_DisasterPts  = sc.Input[10];
	SCInputRef In_ShowPanel    = sc.Input[11];
	SCInputRef In_FilterRead   = sc.Input[12];

	if (sc.SetDefaults)
	{
		sc.GraphName = "RenkoFlip Filtered Always-In";
		sc.AutoLoop = 0;
		sc.GraphRegion = 0;
		sc.ValueFormat = 2;

		BuySig.Name = "Buy Signal";
		BuySig.DrawStyle = DRAWSTYLE_ARROW_UP;
		BuySig.PrimaryColor = RGB(0, 180, 100);
		BuySig.LineWidth = 2;
		BuySig.DrawZeros = false;

		SellSig.Name = "Sell Signal";
		SellSig.DrawStyle = DRAWSTYLE_ARROW_DOWN;
		SellSig.PrimaryColor = RGB(220, 60, 60);
		SellSig.LineWidth = 2;
		SellSig.DrawZeros = false;

		EntryLong.Name = "Entry Long";
		EntryLong.DrawStyle = DRAWSTYLE_TEXT;
		EntryLong.TextDrawStyleText = "Entry Long";
		EntryLong.PrimaryColor = RGB(0, 220, 120);
		EntryLong.LineWidth = 10;
		EntryLong.DrawZeros = false;

		EntryShort.Name = "Entry Short";
		EntryShort.DrawStyle = DRAWSTYLE_TEXT;
		EntryShort.TextDrawStyleText = "Entry Short";
		EntryShort.PrimaryColor = RGB(255, 100, 100);
		EntryShort.LineWidth = 10;
		EntryShort.DrawZeros = false;

		FilterIn.Name = "Filter-Triggered Entry";
		FilterIn.DrawStyle = DRAWSTYLE_TEXT;
		FilterIn.TextDrawStyleText = "OTF In";
		FilterIn.PrimaryColor = RGB(120, 200, 255);
		FilterIn.LineWidth = 10;
		FilterIn.DrawZeros = false;

		CutMark.Name = "Exit To Flat (Cut)";
		CutMark.DrawStyle = DRAWSTYLE_TEXT;
		CutMark.TextDrawStyleText = "Cut";
		CutMark.PrimaryColor = RGB(255, 200, 80);
		CutMark.LineWidth = 10;
		CutMark.DrawZeros = false;

		DStopMark.Name = "Disaster Stop Exit";
		DStopMark.DrawStyle = DRAWSTYLE_TEXT;
		DStopMark.TextDrawStyleText = "DStop";
		DStopMark.PrimaryColor = RGB(255, 80, 80);
		DStopMark.LineWidth = 10;
		DStopMark.DrawZeros = false;

		FlipPnL.Name  = "Always-In Flip PnL (pts)";
		FlipPnL.DrawStyle = DRAWSTYLE_IGNORE;
		FiltPnL.Name  = "Filtered PnL (pts)";
		FiltPnL.DrawStyle = DRAWSTYLE_IGNORE;
		FiltPnLL.Name = "Filtered PnL Longs (pts)";
		FiltPnLL.DrawStyle = DRAWSTYLE_IGNORE;
		FiltPnLS.Name = "Filtered PnL Shorts (pts)";
		FiltPnLS.DrawStyle = DRAWSTYLE_IGNORE;

		In_MinSwing.Name = "Min Swing Length (bricks before reversal)";
		In_MinSwing.SetInt(1);
		In_MinSwing.SetIntLimits(1, 50);

		In_Confirm.Name = "Confirmation Bricks (new color; 1 = flip brick)";
		In_Confirm.SetInt(2);
		In_Confirm.SetIntLimits(1, 20);

		In_SessionStart.Name = "Session Start (chart timezone)";
		In_SessionStart.SetTime(HMS_TIME(9, 30, 0));
		In_SessionEnd.Name = "Session End (chart timezone)";
		In_SessionEnd.SetTime(HMS_TIME(15, 30, 0));

		In_UseFilter.Name = "Use Direction Filter";
		In_UseFilter.SetYesNo(1);

		In_FilterRef.Name = "Filter: chart + study subgraph (sign: >0 long, <0 short)";
		In_FilterRef.SetChartStudySubgraphValues(0, 0, 0);

		In_Debounce.Name = "Filter Debounce (bricks new direction must hold)";
		In_Debounce.SetInt(1);
		In_Debounce.SetIntLimits(1, 20);

		In_Policy.Name = "On Disagreement";
		In_Policy.SetCustomInputStrings("Go Flat;Hold Position");
		In_Policy.SetCustomInputIndex(0);

		In_ReentryMode.Name = "Re-Entry Mode";
		In_ReentryMode.SetCustomInputStrings("Next Signal Only;Immediate On Filter Alignment");
		In_ReentryMode.SetCustomInputIndex(1);

		In_SigLookback.Name = "Signal Lookback For Filter-Triggered Entry (bricks; 0 = unlimited)";
		In_SigLookback.SetInt(3);
		In_SigLookback.SetIntLimits(0, 200);

		In_DisasterPts.Name = "Disaster Stop (points; 0 = off)";
		In_DisasterPts.SetFloat(0.0f);

		In_ShowPanel.Name = "Show status panel";
		In_ShowPanel.SetYesNo(1);

		In_FilterRead.Name = "Filter Read (external chart)";
		In_FilterRead.SetCustomInputStrings("Last Closed Bar (stable);Forming Bar (live, repaints)");
		In_FilterRead.SetCustomInputIndex(0);

		return;
	}

	// ---------------- persistent state ----------------
	int&   LastDir       = sc.GetPersistentInt(1);
	int&   RunLen        = sc.GetPersistentInt(2);
	int&   PrevRunLen    = sc.GetPersistentInt(3);
	int&   LastProcessed = sc.GetPersistentInt(4);
	int&   FlipValid     = sc.GetPersistentInt(5);
	int&   SDir          = sc.GetPersistentInt(6);   // standing signal direction
	int&   LastSigBar    = sc.GetPersistentInt(7);
	int&   FCur          = sc.GetPersistentInt(8);   // accepted (debounced) filter state
	int&   FPend         = sc.GetPersistentInt(9);
	int&   FPendCount    = sc.GetPersistentInt(10);
	int&   Pos           = sc.GetPersistentInt(11);  // filtered-track position
	int&   BasePos       = sc.GetPersistentInt(12);  // raw always-in position
	int&   FlatBricks    = sc.GetPersistentInt(13);  // in-session bricks spent flat
	int&   PosChanges    = sc.GetPersistentInt(14);  // filtered-track position changes
	int&   DStopBlock    = sc.GetPersistentInt(15);  // re-entry lockout after disaster stop
	float& PosEntry      = sc.GetPersistentFloat(1);
	float& BaseEntry     = sc.GetPersistentFloat(2);
	float& BaseCum       = sc.GetPersistentFloat(3);
	float& FiltCum       = sc.GetPersistentFloat(4);
	float& FiltCumL      = sc.GetPersistentFloat(5);
	float& FiltCumS      = sc.GetPersistentFloat(6);

	if (sc.LastCallToFunction)
		return;

	if (sc.IsFullRecalculation)
	{
		LastDir = 0; RunLen = 0; PrevRunLen = 0; LastProcessed = -1;
		FlipValid = 0; SDir = 0; LastSigBar = -1;
		FCur = 0; FPend = 0; FPendCount = 0;
		Pos = 0; BasePos = 0; FlatBricks = 0; PosChanges = 0;
		DStopBlock = 0;
		PosEntry = 0; BaseEntry = 0;
		BaseCum = 0; FiltCum = 0; FiltCumL = 0; FiltCumS = 0;
	}

	int lastBar = sc.ArraySize - 2; // last CLOSED brick
	if (lastBar < 1)
		return;

	// ---------------- brick color source ----------------
	SCFloatArrayRef RenkoOpen  = sc.BaseData[SC_RENKO_OPEN];
	SCFloatArrayRef RenkoClose = sc.BaseData[SC_RENKO_CLOSE];
	bool haveRenko = RenkoClose.GetArraySize() > 0 && RenkoClose[lastBar] != 0;

	// ---------------- filter source ----------------
	int useFilter = In_UseFilter.GetYesNo();
	s_ChartStudySubgraphValues FRef = In_FilterRef.GetChartStudySubgraphValues();
	bool fExternal = FRef.ChartNumber != 0 && FRef.ChartNumber != sc.ChartNumber;
	SCFloatArray FArr;
	if (useFilter)
	{
		if (fExternal)
			sc.GetStudyArrayFromChartUsingID(FRef.ChartNumber, FRef.StudyID, FRef.SubgraphIndex, FArr);
		else
			sc.GetStudyArrayUsingID(FRef.StudyID, FRef.SubgraphIndex, FArr);
	}

	int minSwing   = In_MinSwing.GetInt();
	int confirm    = In_Confirm.GetInt();
	int debounce   = In_Debounce.GetInt();
	int policyHold = In_Policy.GetIndex() == 1;
	int modeImmed  = In_ReentryMode.GetIndex() == 1;
	int lookback   = In_SigLookback.GetInt();
	float disaster = In_DisasterPts.GetFloat();

	// ---------------- process closed bricks ----------------
	for (int i = LastProcessed + 1; i <= lastBar; ++i)
	{
		float bOpen  = haveRenko ? RenkoOpen[i]  : sc.Open[i];
		float bClose = haveRenko ? RenkoClose[i] : sc.Close[i];
		int dir = 0;
		if (bClose > bOpen) dir = 1;
		else if (bClose < bOpen) dir = -1;

		// ---- signal engine ----
		int signal = 0;
		if (dir != 0)
		{
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

			if (FlipValid && RunLen == confirm)
			{
				signal = dir;
				FlipValid = 0;
			}
		}

		int barTime = sc.BaseDateTimeIn[i].GetTime();
		bool inSession = barTime >= In_SessionStart.GetTime() &&
		                 barTime <= In_SessionEnd.GetTime();

		if (signal != 0 && inSession)
		{
			SDir = signal;
			LastSigBar = i;
			DStopBlock = 0; // a fresh signal clears the disaster-stop lockout
			if (signal > 0) BuySig[i]  = sc.Low[i]  - sc.TickSize * 4;
			else            SellSig[i] = sc.High[i] + sc.TickSize * 4;

			// raw always-in track: every signal reverses
			if (BasePos != 0)
				BaseCum += (sc.Close[i] - BaseEntry) * BasePos;
			BasePos = signal;
			BaseEntry = sc.Close[i];
		}

		// ---- filter state (debounced sign; neutral = keep current) ----
		if (useFilter)
		{
			int fDir = 0;
			int fi = fExternal
				? sc.GetContainingIndexForSCDateTime(FRef.ChartNumber, sc.BaseDateTimeIn[i])
				: i;
			// "Last Closed Bar": step back from the containing (possibly
			// forming) bar so live and recalculated states are identical
			if (fExternal && In_FilterRead.GetIndex() == 0)
				fi -= 1;
			if (fi >= 0 && FArr.GetArraySize() > fi)
			{
				if (FArr[fi] > 0) fDir = 1;
				else if (FArr[fi] < 0) fDir = -1;
			}

			if (fDir != 0 && fDir != FCur)
			{
				if (fDir == FPend) FPendCount++;
				else { FPend = fDir; FPendCount = 1; }
				if (FPendCount >= debounce)
				{
					FCur = fDir;
					FPend = 0; FPendCount = 0;
				}
			}
			else
			{
				FPend = 0; FPendCount = 0;
			}
		}

		// ---- filtered track: disaster stop (insurance, mostly for Hold) ----
		if (Pos != 0 && disaster > 0 &&
		    (sc.Close[i] - PosEntry) * Pos <= -disaster)
		{
			float pnl = (sc.Close[i] - PosEntry) * Pos;
			FiltCum += pnl;
			if (Pos > 0) FiltCumL += pnl; else FiltCumS += pnl;
			DStopMark[i] = sc.Close[i];
			Pos = 0;
			PosChanges++;
			DStopBlock = 1; // no filter-triggered re-entry; wait for a fresh signal
		}

		// ---- filtered track: reconcile position to target ----
		if (inSession)
		{
			int target;
			if (!useFilter)
				target = SDir;                       // filter off = pure always-in
			else if (SDir != 0 && SDir == FCur)
				target = SDir;                       // agreement
			else
				target = policyHold ? Pos : 0;       // disagreement policy

			if (target != Pos)
			{
				bool signalBrick = (signal != 0 && signal == target);
				bool entryWanted = (target != 0);
				bool entryAllowed = true;

				if (entryWanted && !signalBrick)
				{
					// filter-triggered (deferred) entry: needs Immediate mode,
					// a recent-enough standing signal, and no disaster lockout
					if (!modeImmed)
						entryAllowed = false;
					else if (lookback > 0 && LastSigBar >= 0 && (i - LastSigBar) > lookback)
						entryAllowed = false;
					if (DStopBlock)
						entryAllowed = false;
				}

				// close existing position whenever the target is flat or
				// opposite — NEVER gated by entry permission, so the track
				// can't sit positioned against both signal and filter
				if (Pos != 0)
				{
					float pnl = (sc.Close[i] - PosEntry) * Pos;
					FiltCum += pnl;
					if (Pos > 0) FiltCumL += pnl; else FiltCumS += pnl;
					if (!(entryWanted && entryAllowed))
						CutMark[i] = (Pos > 0) ? sc.Low[i] - sc.TickSize * 10
						                       : sc.High[i] + sc.TickSize * 10;
					Pos = 0;
					PosChanges++;
				}

				// open new position
				if (entryWanted && entryAllowed && Pos == 0)
				{
					Pos = target;
					PosEntry = sc.Close[i];
					PosChanges++;
					if (signalBrick)
					{
						if (target > 0) EntryLong[i]  = sc.Low[i]  - sc.TickSize * 10;
						else            EntryShort[i] = sc.High[i] + sc.TickSize * 10;
					}
					else
					{
						FilterIn[i] = (target > 0) ? sc.Low[i]  - sc.TickSize * 10
						                           : sc.High[i] + sc.TickSize * 10;
					}
				}
			}

			if (Pos == 0 && SDir != 0)
				FlatBricks++; // only meaningful once a standing signal exists
		}

		FlipPnL[i]  = BaseCum;
		FiltPnL[i]  = FiltCum;
		FiltPnLL[i] = FiltCumL;
		FiltPnLS[i] = FiltCumS;
	}
	LastProcessed = lastBar;

	// ---------------- status panel ----------------
	if (In_ShowPanel.GetYesNo())
	{
		SCString txt;
		txt.Format("Flip: %+.2f  ||  Filt: %+.2f (L: %+.2f | S: %+.2f)%s  |  F: %+d  |  Flat: %d bricks  |  Changes: %d",
			BaseCum, FiltCum, FiltCumL, FiltCumS,
			Pos > 0 ? " [LONG]" : (Pos < 0 ? " [SHORT]" : ""),
			FCur, FlatBricks, PosChanges);

		s_UseTool Tool;
		Tool.Clear();
		Tool.ChartNumber = sc.ChartNumber;
		Tool.DrawingType = DRAWING_TEXT;
		Tool.LineNumber = 84211003;
		Tool.AddAsUserDrawnDrawing = 0;
		Tool.AddMethod = UTAM_ADD_OR_ADJUST;
		Tool.UseRelativeVerticalValues = 1;
		Tool.BeginDateTime = 3;
		Tool.BeginValue = 95;
		Tool.Color = RGB(230, 230, 230);
		Tool.FontSize = 12;
		Tool.FontBold = 1;
		Tool.Text = txt;
		sc.UseTool(Tool);
	}
}
