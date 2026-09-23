// OTFMultiTrader.cpp
// ---------------------------------------------------------------
// Up to three INDEPENDENT OTF-switch sub-traders running inside one
// chart, netted into a single order flow on a single account.
//
// This is NOT a consensus sizer. Each sub-trader i is a separate
// state machine (flat / long / short) that knows nothing about the
// others:
//   - entry : its OTF switches direction (debounced)
//   - exit  : its own trailing stop (its OWN Trail Bars, measured on
//             its OWN OTF chart), or its OTF switching against it
//   - retry : re-arms immediately, takes the next switch of its OTF
//   - net target = sum(Dir_i * Qty_i) over ACTIVE sub-traders only;
//     net zero is a valid state
//
// Each OTF slot is independently enabled, and carries its own
// quantity and its own trail length. Any SUBSET works — e.g. OTF-1
// and OTF-3 enabled with OTF-2 off is a valid configuration; the
// slots are independent, not a ladder.
//
// Multi-entry comes from the enabled timeframes aligning at different
// moments, not from stacking on one signal.
//
// Deployment: enable ONE OTF and run three copies on three charts,
// or enable several and run them netted here. Same code.
//
// Spec: Trading/Futures_Day_Trading/OTFMultiTrader_BuildSpec.md
// ---------------------------------------------------------------

#include "sierrachart.h"

SCDLLName("OTF Multi Trader")

SCSFExport scsf_OTFMultiTrader(SCStudyInterfaceRef sc)
{
	// ---------------- Subgraphs ----------------
	SCSubgraphRef BuyEx  = sc.Subgraph[0];
	SCSubgraphRef SellEx = sc.Subgraph[1];

	// ---------------- Inputs ----------------
	SCInputRef In_Enable        = sc.Input[0];
	SCInputRef In_SendToService = sc.Input[1];
	SCInputRef In_FilterRead    = sc.Input[2];
	SCInputRef In_Debounce      = sc.Input[3];
	SCInputRef In_ExitOnSwitch  = sc.Input[4];
	SCInputRef In_Disaster      = sc.Input[5];
	SCInputRef In_SessionStart  = sc.Input[6];
	SCInputRef In_SessionEnd    = sc.Input[7];
	SCInputRef In_FlattenTime   = sc.Input[8];
	SCInputRef In_MaxEntriesDay = sc.Input[9];
	SCInputRef In_MaxDailyLoss  = sc.Input[10];
	SCInputRef In_ShowPanel     = sc.Input[11];

	// one independent group per OTF slot
	SCInputRef In_OTF1On    = sc.Input[12];
	SCInputRef In_OTF1Study = sc.Input[13];
	SCInputRef In_OTF1Qty   = sc.Input[14];
	SCInputRef In_OTF1Trail = sc.Input[15];

	SCInputRef In_OTF2On    = sc.Input[16];
	SCInputRef In_OTF2Study = sc.Input[17];
	SCInputRef In_OTF2Qty   = sc.Input[18];
	SCInputRef In_OTF2Trail = sc.Input[19];

	SCInputRef In_OTF3On    = sc.Input[20];
	SCInputRef In_OTF3Study = sc.Input[21];
	SCInputRef In_OTF3Qty   = sc.Input[22];
	SCInputRef In_OTF3Trail = sc.Input[23];

	if (sc.SetDefaults)
	{
		sc.GraphName = "OTF Multi Trader";
		sc.AutoLoop = 0;                 // manual loop
		sc.GraphRegion = 0;
		sc.ValueFormat = 2;

		// ---- auto trading configuration ----
		// netted scaling across independent sub-traders needs both of
		// these; MaximumPositionAllowed is re-derived each call from the
		// ACTIVE slots' quantities
		sc.AllowMultipleEntriesInSameDirection = 1;
		sc.MaximumPositionAllowed = 30;
		sc.SupportReversals = 1;
		sc.AllowOnlyOneTradePerBar = 0;  // several sub-traders may act on one bar
		// 0 = do NOT apply the Trade Window's attached-order template to
		// study orders. Must stay 0 (see RenkoFlipAutoTrader notes).
		sc.SupportAttachedOrdersForTrading = 0;
		sc.CancelAllOrdersOnEntriesAndReversals = 1;
		sc.CancelAllWorkingOrdersOnExit = 1;
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

		In_FilterRead.Name = "Filter Read (external chart)";
		In_FilterRead.SetCustomInputStrings("Last Closed Bar (stable);Forming Bar (live, repaints)");
		In_FilterRead.SetCustomInputIndex(0);

		In_Debounce.Name = "OTF Switch Debounce (bars, all slots)";
		In_Debounce.SetInt(1);
		In_Debounce.SetIntLimits(1, 20);

		In_ExitOnSwitch.Name = "Exit Sub-Trader When Its OTF Switches Against It";
		In_ExitOnSwitch.SetYesNo(1);

		In_Disaster.Name = "Disaster Stop on NET position (points; 0 = off)";
		In_Disaster.SetFloat(0.0f);

		In_SessionStart.Name = "Session Start (chart timezone)";
		In_SessionStart.SetTime(HMS_TIME(9, 30, 0));
		In_SessionEnd.Name = "Session End — last entry (chart timezone)";
		In_SessionEnd.SetTime(HMS_TIME(15, 30, 0));
		In_FlattenTime.Name = "Flatten & Stop Time (chart timezone)";
		In_FlattenTime.SetTime(HMS_TIME(15, 55, 0));

		In_MaxEntriesDay.Name = "Max Entries Per Day (0 = off)";
		In_MaxEntriesDay.SetInt(0);
		In_MaxEntriesDay.SetIntLimits(0, 500);

		In_MaxDailyLoss.Name = "Max Daily Loss (points; 0 = off)";
		In_MaxDailyLoss.SetFloat(0.0f);

		In_ShowPanel.Name = "Show status panel";
		In_ShowPanel.SetYesNo(1);

		// ---- OTF-1 ----
		In_OTF1On.Name = "OTF-1: Enabled";
		In_OTF1On.SetYesNo(1);
		In_OTF1Study.Name = "OTF-1: chart + study subgraph";
		In_OTF1Study.SetChartStudySubgraphValues(0, 0, 0);
		In_OTF1Qty.Name = "OTF-1: Quantity";
		In_OTF1Qty.SetInt(1);
		In_OTF1Qty.SetIntLimits(1, 100);
		In_OTF1Trail.Name = "OTF-1: Trail Bars (on OTF-1's own chart; 0 = no trail)";
		In_OTF1Trail.SetInt(3);
		In_OTF1Trail.SetIntLimits(0, 200);

		// ---- OTF-2 ----
		In_OTF2On.Name = "OTF-2: Enabled";
		In_OTF2On.SetYesNo(0);
		In_OTF2Study.Name = "OTF-2: chart + study subgraph";
		In_OTF2Study.SetChartStudySubgraphValues(0, 0, 0);
		In_OTF2Qty.Name = "OTF-2: Quantity";
		In_OTF2Qty.SetInt(1);
		In_OTF2Qty.SetIntLimits(1, 100);
		In_OTF2Trail.Name = "OTF-2: Trail Bars (on OTF-2's own chart; 0 = no trail)";
		In_OTF2Trail.SetInt(3);
		In_OTF2Trail.SetIntLimits(0, 200);

		// ---- OTF-3 ----
		In_OTF3On.Name = "OTF-3: Enabled";
		In_OTF3On.SetYesNo(0);
		In_OTF3Study.Name = "OTF-3: chart + study subgraph";
		In_OTF3Study.SetChartStudySubgraphValues(0, 0, 0);
		In_OTF3Qty.Name = "OTF-3: Quantity";
		In_OTF3Qty.SetInt(1);
		In_OTF3Qty.SetIntLimits(1, 100);
		In_OTF3Trail.Name = "OTF-3: Trail Bars (on OTF-3's own chart; 0 = no trail)";
		In_OTF3Trail.SetInt(3);
		In_OTF3Trail.SetIntLimits(0, 200);

		return;
	}

	// ---------------- persistent state (globals) ----------------
	int& LastProcessed  = sc.GetPersistentInt(1);
	int& TradeDate      = sc.GetPersistentInt(2);
	int& EntriesToday   = sc.GetPersistentInt(3);
	int& StoppedForDay  = sc.GetPersistentInt(4);
	int& DisasterBlock  = sc.GetPersistentInt(5);
	int& LastRecon      = sc.GetPersistentInt(6);

	if (sc.LastCallToFunction)
		return;

	// per sub-trader (k = 0..2)
	//  Dir  : current position direction, -1 / 0 / +1
	//  Cur  : debounced OTF state
	//  Pend / PendCount : debounce accumulator
	//  Trades : entries taken today by this sub-trader
	//  EntryPx / Realized : attribution (points)
	//
	// Persistent slots are loaded into real local arrays and written
	// back once at the end. Do NOT take the address of a persistent
	// variable and index it — persistent storage is a container lookup,
	// not a contiguous array, so (&sc.GetPersistentInt(10))[k] is
	// undefined behaviour and crashes Sierra on apply.
	//  LastFi : last OTF-chart bar index already folded into the
	//           debounce, so the debounce counts the OTF's OWN bars
	int   Dir[3], Cur[3], Pend[3], PendCount[3], Trades[3], LastFi[3];
	float EntryPx[3], Realized[3];
	for (int k = 0; k < 3; ++k)
	{
		Dir[k]       = sc.GetPersistentInt(10 + k);
		Cur[k]       = sc.GetPersistentInt(20 + k);
		Pend[k]      = sc.GetPersistentInt(30 + k);
		PendCount[k] = sc.GetPersistentInt(40 + k);
		Trades[k]    = sc.GetPersistentInt(50 + k);
		LastFi[k]    = sc.GetPersistentInt(60 + k);
		EntryPx[k]   = sc.GetPersistentFloat(10 + k);
		Realized[k]  = sc.GetPersistentFloat(20 + k);
	}

	if (sc.IsFullRecalculation)
	{
		LastProcessed = -1; LastRecon = -1; DisasterBlock = 0;
		for (int k = 0; k < 3; ++k)
		{
			Dir[k] = 0; Cur[k] = 0; Pend[k] = 0; PendCount[k] = 0;
			Trades[k] = 0; EntryPx[k] = 0; Realized[k] = 0;
			LastFi[k] = -1;
		}
		// TradeDate/EntriesToday/StoppedForDay intentionally kept:
		// a chart reload must not reset the day's risk limits
	}

	sc.SendOrdersToTradeService = In_SendToService.GetYesNo();

	int lastBar = sc.ArraySize - 2; // last CLOSED bar on this chart

	// single exit point below, so the write-back always runs
	if (lastBar >= 1)
	{

	// ---------------- daily counter reset ----------------
	int today = sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.ArraySize - 1]);
	if (today != TradeDate)
	{
		TradeDate = today;
		EntriesToday = 0;
		StoppedForDay = 0;
		DisasterBlock = 0;
		for (int k = 0; k < 3; ++k) { Trades[k] = 0; Realized[k] = 0; }
	}

	bool realTime = !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0;
	bool tradingEnabled = In_Enable.GetYesNo() != 0 && realTime;

	s_SCPositionData Position;
	sc.GetTradePosition(Position);

	double perPoint = (sc.TickSize > 0) ? (sc.CurrencyValuePerTick / sc.TickSize) : 0;
	double dailyPnLPts = (perPoint > 0) ? (Position.DailyProfitLoss / perPoint) : 0;

	int curTime = sc.BaseDateTimeIn[sc.ArraySize - 1].GetTime();

	// flatten time / daily loss enforcement (every call, real time only)
	if (tradingEnabled && !StoppedForDay)
	{
		bool lossHit = In_MaxDailyLoss.GetFloat() > 0 &&
		               dailyPnLPts <= -In_MaxDailyLoss.GetFloat();
		if (curTime >= In_FlattenTime.GetTime() || lossHit)
		{
			if (Position.PositionQuantity != 0 || Position.WorkingOrdersExist)
				sc.FlattenAndCancelAllOrders();
			for (int k = 0; k < 3; ++k) Dir[k] = 0;
			StoppedForDay = 1;
			if (lossHit)
				sc.AddMessageToLog("OTF Multi Trader: max daily loss hit — flattened and stopped for the day.", 1);
		}
	}

	// ---------------- per-slot configuration ----------------
	// Slots are fully independent: any subset may be enabled (1+3 with
	// 2 off is valid). A slot contributes to the net position only when
	// it is BOTH enabled and pointed at a real study.
	s_ChartStudySubgraphValues Ref[3];
	Ref[0] = In_OTF1Study.GetChartStudySubgraphValues();
	Ref[1] = In_OTF2Study.GetChartStudySubgraphValues();
	Ref[2] = In_OTF3Study.GetChartStudySubgraphValues();

	bool Enabled[3] = { In_OTF1On.GetYesNo() != 0,
	                    In_OTF2On.GetYesNo() != 0,
	                    In_OTF3On.GetYesNo() != 0 };
	int  Qty[3]     = { In_OTF1Qty.GetInt(),
	                    In_OTF2Qty.GetInt(),
	                    In_OTF3Qty.GetInt() };
	int  Trail[3]   = { In_OTF1Trail.GetInt(),
	                    In_OTF2Trail.GetInt(),
	                    In_OTF3Trail.GetInt() };

	bool         Active[3];
	bool         MisConfig[3];   // enabled but no study reference
	bool         External[3];
	SCFloatArray OTFArr[3];
	SCGraphData  OTFBase[3];

	int maxPos = 0;

	for (int k = 0; k < 3; ++k)
	{
		bool haveRef = Ref[k].StudyID != 0;
		Active[k]    = Enabled[k] && haveRef;
		MisConfig[k] = Enabled[k] && !haveRef;
		External[k]  = Ref[k].ChartNumber != 0 && Ref[k].ChartNumber != sc.ChartNumber;

		if (!Active[k])
			continue;

		maxPos += Qty[k];

		if (External[k])
		{
			sc.GetStudyArrayFromChartUsingID(Ref[k].ChartNumber, Ref[k].StudyID, Ref[k].SubgraphIndex, OTFArr[k]);
			// base bars of the OTF's own chart — the trailing stop for
			// sub-trader k rides THIS timeframe, with THIS slot's Trail
			sc.GetChartBaseData(Ref[k].ChartNumber, OTFBase[k]);
		}
		else
		{
			sc.GetStudyArrayUsingID(Ref[k].StudyID, Ref[k].SubgraphIndex, OTFArr[k]);
		}
	}
	sc.MaximumPositionAllowed = (maxPos > 0) ? maxPos : 1;

	// A slot switched off mid-session must stop contributing size.
	// Close its book at the current price so its attribution stays
	// honest, and zero its direction so the net target drops it.
	for (int k = 0; k < 3; ++k)
	{
		if (!Active[k] && Dir[k] != 0)
		{
			Realized[k] += (sc.Close[lastBar] - EntryPx[k]) * Dir[k] * Qty[k];
			Dir[k] = 0;
			Cur[k] = 0; Pend[k] = 0; PendCount[k] = 0;
			LastFi[k] = -1;
		}
	}

	int debounce = In_Debounce.GetInt();
	bool exitOnSwitch = In_ExitOnSwitch.GetYesNo() != 0;

	// ---------------- process closed bars ----------------
	// All sub-trader decisions are made HERE, once per bar. The
	// reconcile block below only executes the already-decided net
	// target — this is what keeps it burst-safe when several bars
	// close between study calls (replay / fast live).
	for (int i = LastProcessed + 1; i <= lastBar; ++i)
	{
		int barTime = sc.BaseDateTimeIn[i].GetTime();
		bool inSession = barTime >= In_SessionStart.GetTime() &&
		                 barTime <= In_SessionEnd.GetTime() &&
		                 barTime < In_FlattenTime.GetTime();
		float px = sc.Close[i];

		for (int k = 0; k < 3; ++k)
		{
			if (!Active[k])
				continue;

			// ---- map this bar onto the OTF's chart ----
			int fi = External[k]
				? sc.GetContainingIndexForSCDateTime(Ref[k].ChartNumber, sc.BaseDateTimeIn[i])
				: i;
			if (External[k] && In_FilterRead.GetIndex() == 0)
				fi -= 1; // last closed bar: live == recalculated
			if (fi < 0 || OTFArr[k].GetArraySize() <= fi)
				continue;

			// ---- debounce the OTF state ----
			// Only fold a given OTF bar in ONCE. This study loops over
			// its own (fast) chart, so many fast bars map to the same
			// slow OTF bar; counting per fast bar would let a debounce
			// of N be satisfied by re-reading a single OTF bar N times,
			// which is no debounce at all. Gate on fi advancing so the
			// setting means N consecutive bars of the OTF's OWN chart.
			bool switched = false;
			if (fi != LastFi[k])
			{
				LastFi[k] = fi;

				int fDir = 0;
				if (OTFArr[k][fi] > 0) fDir = 1;
				else if (OTFArr[k][fi] < 0) fDir = -1;

				int prevCur = Cur[k];
				if (fDir != 0 && fDir != Cur[k])
				{
					if (fDir == Pend[k]) PendCount[k]++;
					else { Pend[k] = fDir; PendCount[k] = 1; }
					if (PendCount[k] >= debounce)
					{
						Cur[k] = fDir;
						Pend[k] = 0; PendCount[k] = 0;
					}
				}
				else
				{
					Pend[k] = 0; PendCount[k] = 0;
				}
				switched = (Cur[k] != 0 && Cur[k] != prevCur);
			}

			// a fresh switch on any active slot clears the stand-down
			if (switched)
				DisasterBlock = 0;

			// ---- trailing stop: this slot's Trail, on its own chart ----
			if (Dir[k] != 0 && Trail[k] > 0)
			{
				float stopPx = 0;
				bool haveStop = false;

				if (External[k])
				{
					// a misconfigured / closed OTF chart yields no base
					// arrays — check the array COUNT before indexing.
					// If they are missing we leave haveStop false rather
					// than falling back to this chart's bars: trailing a
					// high-TF sub-trader on fast bricks would be wrong,
					// and silently so.
					if (OTFBase[k].GetArraySize() > SC_HIGH)
					{
						SCFloatArrayRef Hi = OTFBase[k][SC_HIGH];
						SCFloatArrayRef Lo = OTFBase[k][SC_LOW];
						if (Lo.GetArraySize() > fi && Hi.GetArraySize() > fi)
						{
							int start = fi - Trail[k] + 1;
							if (start < 0) start = 0;
							stopPx = (Dir[k] > 0) ? Lo[start] : Hi[start];
							for (int j = start + 1; j <= fi; ++j)
							{
								if (Dir[k] > 0) { if (Lo[j] < stopPx) stopPx = Lo[j]; }
								else            { if (Hi[j] > stopPx) stopPx = Hi[j]; }
							}
							haveStop = true;
						}
					}
				}
				else
				{
					int start = fi - Trail[k] + 1;
					if (start < 0) start = 0;
					stopPx = (Dir[k] > 0) ? sc.Low[start] : sc.High[start];
					for (int j = start + 1; j <= fi; ++j)
					{
						if (Dir[k] > 0) { if (sc.Low[j] < stopPx) stopPx = sc.Low[j]; }
						else            { if (sc.High[j] > stopPx) stopPx = sc.High[j]; }
					}
					haveStop = true;
				}

				if (haveStop &&
				    ((Dir[k] > 0 && px <= stopPx) || (Dir[k] < 0 && px >= stopPx)))
				{
					Realized[k] += (px - EntryPx[k]) * Dir[k] * Qty[k];
					Dir[k] = 0;
				}
			}

			// ---- switch handling: exit against, then (re-)enter ----
			if (switched)
			{
				if (Dir[k] != 0 && Dir[k] != Cur[k] && exitOnSwitch)
				{
					Realized[k] += (px - EntryPx[k]) * Dir[k] * Qty[k];
					Dir[k] = 0;
				}

				bool entriesLeft = !(In_MaxEntriesDay.GetInt() > 0 &&
				                     EntriesToday >= In_MaxEntriesDay.GetInt());

				if (Dir[k] == 0 && inSession && !StoppedForDay &&
				    !DisasterBlock && entriesLeft)
				{
					Dir[k] = Cur[k];
					EntryPx[k] = px;
					Trades[k]++;
					EntriesToday++;
				}
			}
		}
	}
	LastProcessed = lastBar;

	// ---------------- reconcile net position ----------------
	// Net target = sum over ACTIVE sub-traders. Runs once per newly
	// closed bar; self-heals missed fills on the next bar.
	if (tradingEnabled && !StoppedForDay && lastBar != LastRecon)
	{
		LastRecon = lastBar;

		double posQty = Position.PositionQuantity;

		// disaster stop on the NET position: flatten everything and
		// stand down until the next OTF switch
		if (posQty != 0 && In_Disaster.GetFloat() > 0)
		{
			int posDir = (posQty > 0) ? 1 : -1;
			if ((sc.Close[lastBar] - Position.AveragePrice) * posDir <= -In_Disaster.GetFloat())
			{
				sc.FlattenAndCancelAllOrders();
				for (int k = 0; k < 3; ++k) Dir[k] = 0;
				DisasterBlock = 1;
				sc.AddMessageToLog("OTF Multi Trader: disaster stop — net flattened, standing down until next OTF switch.", 1);
				posQty = 0;
			}
		}

		int targetNet = 0;
		for (int k = 0; k < 3; ++k)
			if (Active[k]) targetNet += Dir[k] * Qty[k];

		if (!DisasterBlock && targetNet != posQty)
		{
			s_SCNewOrder Order;
			Order.OrderType = SCT_ORDERTYPE_MARKET;

			if (targetNet == 0)
			{
				// exits are never gated by entry permission
				if (posQty != 0)
					sc.FlattenAndCancelAllOrders();
			}
			else if (posQty != 0 && (posQty > 0) == (targetNet > 0))
			{
				// same direction: scale in or out to the netted size
				double diff = targetNet - posQty;
				if (diff > 0)
				{
					Order.OrderQuantity = diff;
					int result = (targetNet > 0) ? sc.BuyEntry(Order) : sc.SellEntry(Order);
					if (result <= 0)
					{
						SCString msg;
						msg.Format("OTF Multi Trader: scale-in rejected, error %d", result);
						sc.AddMessageToLog(msg, 1);
					}
				}
				else
				{
					Order.OrderQuantity = -diff;
					if (posQty > 0) sc.SellExit(Order);
					else            sc.BuyExit(Order);
				}
			}
			else
			{
				// flat or opposite: one entry establishes the target
				// (SupportReversals auto-sizes across zero)
				Order.OrderQuantity = (targetNet > 0) ? targetNet : -targetNet;
				int result = (targetNet > 0) ? sc.BuyEntry(Order) : sc.SellEntry(Order);
				if (result > 0)
				{
					if (targetNet > 0) BuyEx[lastBar]  = sc.Low[lastBar]  - sc.TickSize * 4;
					else               SellEx[lastBar] = sc.High[lastBar] + sc.TickSize * 4;
				}
				else
				{
					SCString msg;
					msg.Format("OTF Multi Trader: entry rejected, error %d", result);
					sc.AddMessageToLog(msg, 1);
				}
			}
		}
	}

	// ---------------- status panel ----------------
	if (In_ShowPanel.GetYesNo())
	{
		int targetNet = 0;
		for (int k = 0; k < 3; ++k)
			if (Active[k]) targetNet += Dir[k] * Qty[k];

		// per-slot attribution: realized + open, in points
		SCString perOTF;
		for (int k = 0; k < 3; ++k)
		{
			SCString one;
			if (MisConfig[k])
			{
				one.Format("  OTF%d[ON but NO STUDY SET]", k + 1);
			}
			else if (!Active[k])
			{
				one.Format("  OTF%d[off]", k + 1);
			}
			else
			{
				float open = (Dir[k] != 0)
					? (sc.Close[lastBar] - EntryPx[k]) * Dir[k] * Qty[k]
					: 0.0f;
				one.Format("  OTF%d[%s q%d t%d] %d trades %+.2f pts",
					k + 1,
					Dir[k] > 0 ? "LONG" : (Dir[k] < 0 ? "SHORT" : "flat"),
					Qty[k],
					Trail[k],
					Trades[k],
					Realized[k] + open);
			}
			perOTF += one.GetChars();
		}

		SCString txt;
		txt.Format("OTFMulti: %s%s | Net target: %+d | Pos: %.0f | Day PnL: %+.2f pts |%s",
			In_Enable.GetYesNo() ? "ON" : "OFF",
			StoppedForDay ? " (stopped for day)" : (DisasterBlock ? " (stand-down)" : ""),
			targetNet,
			Position.PositionQuantity,
			dailyPnLPts,
			perOTF.GetChars());

		s_UseTool Tool;
		Tool.Clear();
		Tool.ChartNumber = sc.ChartNumber;
		Tool.DrawingType = DRAWING_TEXT;
		Tool.LineNumber = 84211003;
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
	// Single write-back covering every path, so sub-trader state
	// survives to the next study call.
	for (int k = 0; k < 3; ++k)
	{
		sc.GetPersistentInt(10 + k)   = Dir[k];
		sc.GetPersistentInt(20 + k)   = Cur[k];
		sc.GetPersistentInt(30 + k)   = Pend[k];
		sc.GetPersistentInt(40 + k)   = PendCount[k];
		sc.GetPersistentInt(50 + k)   = Trades[k];
		sc.GetPersistentInt(60 + k)   = LastFi[k];
		sc.GetPersistentFloat(10 + k) = EntryPx[k];
		sc.GetPersistentFloat(20 + k) = Realized[k];
	}
}
