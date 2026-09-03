// =============================================================================
// EffortVsResult.cpp
// Sierra Chart ACSIL Custom Study — Effort vs Result v1.0
//
// PURPOSE:
//   A lightweight, closed-bar, bar-level measure of whether aggressive order
//   flow was rewarded by price. This is a filter/context study, not a trade
//   entry system and not a per-price trapped-zone detector.
//
//   Per closed bar:
//     BarDelta       = sum(AskVolume - BidVolume) across the bar's VAP levels
//     Effort        = BarDelta / EWMA(abs(BarDelta)), capped, no mean removal
//     ResultNorm    = (Close-Open et al.) / EWMA(True Range)
//     SignedReward  = sign(Effort) * ResultNorm  (positive = rewarded)
//     SignedFailure = -sign(Effort) * |Effort| * max(0, Stall - SignedReward)
//       negative/magenta below zero = buyers failed (short evidence)
//       positive/cyan    above zero = sellers failed (long evidence)
//
//   A separate one-bar-delayed confirmation channel prints buyer/seller
//   failed-aggression pulses and price-region arrows on bar i+1 only.
//
//   VAP shows executed volume, not trader identity or inventory, so all
//   labels/messages say "failed aggression evidence", never assert known
//   open positions.
//
// DATA:
//   Direct VAP source on the SAME chart (sc.MaintainVolumeAtPriceData = 1).
//   No external Numbers Bars study dependency.
//
// INPUTS (0-based):
//   In:0  Effort Length                  int        default 50
//   In:1  Result Length                  int        default 50
//   In:2  Smoothing Length               int        default 3
//   In:3  Minimum Effort                 float      default 1.25
//   In:4  Stall Allowance                float      default 0.20
//   In:5  Result Mode                    list       default 0 (Close-Open)
//   In:6  Max Normalized Value (0=off)   float      default 5.0
//   In:7  Confirmation Enabled           yes/no     default 1
//   In:8  Max Favorable Extension Ticks  int        default 2
//   In:9  Minimum Adverse Close Ticks    int        default 2
//   In:10 Show Price Arrows              yes/no     default 1
//   In:11 Arrow Offset Ticks             int        default 2
//   In:12 Show Rewarded Aggression       yes/no     default 0
//   In:13 Buyer Failure Color            color      magenta
//   In:14 Seller Failure Color           color      cyan
//   In:15 Buyer Arrow Color              color      magenta
//   In:16 Seller Arrow Color             color      cyan
//   In:17 Rewarded Aggression Color      color      gray-green
//   In:18 Enable Alerts                  yes/no     default 1
//   In:19 Alert Sound Number             alert      default 1
//
// SUBGRAPHS (0-based):
//   SG:0  Raw Bar Delta               LINE               BarDelta, contracts
//   SG:1  Effort Scale                LINE_SKIPZEROS     EWMA(abs(BarDelta))
//   SG:2  Normalized Effort           LINE               capped Effort
//   SG:3  Normalized Result           LINE               ResultNorm, TR units
//   SG:4  Signed Reward               LINE               sign(Effort)*ResultNorm
//   SG:5  Raw Signed Failure          BAR                SignedFailure
//   SG:6  Smoothed Signed Failure     BAR + DataColor    EMA failure, main view
//   SG:7  Buyer Failure Pulse         POINT              confirm value, else 0
//   SG:8  Seller Failure Pulse        POINT              confirm value, else 0
//   SG:9  Buyer Failure Arrow         ARROW_DOWN         price, else 0
//   SG:10 Seller Failure Arrow        ARROW_UP           price, else 0
//   SG:11 Rewarded Aggression         BAR                reward, else 0
//   SG:12 Ready Flag                  LINE               1 when warmed up
//
// PERSISTENT SLOTS:
//   Int 1 : lastKnownBars — new-bar / intrabar-tick guard
//   Int 2 : alert watermark — newest alerted buyer-failure bar index
//   Int 3 : alert watermark — newest alerted seller-failure bar index
//   Int 4-6 : settings fingerprint words (structural inputs only)
//   Ptr 10: S_EvrState* — EWMA/EMA carry state (plain C struct, no STL)
//
// AutoLoop: 0 (manual loop; incremental range covers backfill gaps)
// =============================================================================

#include "sierrachart.h"
#include <cmath>
#include <cstring>
#include <climits>
#include <cstdint>

SCDLLName("EffortVsResult")

// Persistent slot assignments (see header block)
static const int EVR_INT_BARS   = 1;
static const int EVR_INT_ALERTB = 2;
static const int EVR_INT_ALERTS = 3;
static const int EVR_INT_FP0    = 4;
static const int EVR_INT_FP1    = 5;
static const int EVR_INT_FP2    = 6;
static const int EVR_PTR_STATE  = 10;

// Scan window for the post-loop alert watermark scan
static const int EVR_ALERT_SCAN = 10;

// Neutral histogram color for zero / warming values
static const COLORREF EVR_GRAY = RGB(80, 80, 80);

// =============================================================================
// Carry state between calls (plain C floats — no STL in persistent state)
// =============================================================================
struct S_EvrState
{
	double effortScale;   // EWMA(abs(BarDelta))
	double trScale;       // EWMA(TrueRange)
	double smooth;        // EMA of SignedFailure (last closed bar value)
	int lastProcessed;    // last closed bar index fully processed
};

// =============================================================================
// Helpers (file scope so the static-gate body check does not apply)
// =============================================================================

// Tri-state sign without any float equality test
static float EVR_Sign(float x)
{
	if (x > 0.0f) return 1.0f;
	if (x < 0.0f) return -1.0f;
	return 0.0f;
}

// Linear blend from a neutral base color to a user target color by t in [0,1]
static COLORREF EVR_Blend(COLORREF base, COLORREF target, float t)
{
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	int br = (int)(base & 0xFF);
	int bg = (int)((base >> 8) & 0xFF);
	int bb = (int)((base >> 16) & 0xFF);
	int tr = (int)(target & 0xFF);
	int tg = (int)((target >> 8) & 0xFF);
	int tb = (int)((target >> 16) & 0xFF);
	int r = br + (int)((float)(tr - br) * t);
	int g = bg + (int)((float)(tg - bg) * t);
	int b = bb + (int)((float)(tb - bb) * t);
	return RGB(r, g, b);
}

// Sum (AskVolume - BidVolume) across one bar's VAP levels with int64 math
static float EVR_BarDelta(SCStudyInterfaceRef sc, int barIndex)
{
	int priceInTicks = INT_MIN;
	const s_VolumeAtPriceV2* pVAP = nullptr;
	int64_t deltaSum = 0;
	while (sc.VolumeAtPriceForBars->GetNextHigherVAPElement(
		(unsigned int)barIndex, priceInTicks, &pVAP))
	{
		if (pVAP == nullptr)
			continue;
		deltaSum += (int64_t)pVAP->AskVolume - (int64_t)pVAP->BidVolume;
	}
	return (float)deltaSum;
}

// =============================================================================
// Main study function
// =============================================================================
SCSFExport scsf_EffortVsResult(SCStudyInterfaceRef sc)
{
	// ── Input references ─────────────────────────────────────────────────
	SCInputRef In_EffortLen   = sc.Input[0];
	SCInputRef In_ResultLen   = sc.Input[1];
	SCInputRef In_SmoothLen   = sc.Input[2];
	SCInputRef In_MinEffort   = sc.Input[3];
	SCInputRef In_Stall       = sc.Input[4];
	SCInputRef In_ResultMode  = sc.Input[5];
	SCInputRef In_MaxNorm     = sc.Input[6];
	SCInputRef In_ConfOn      = sc.Input[7];
	SCInputRef In_FavTicks    = sc.Input[8];
	SCInputRef In_AdvTicks    = sc.Input[9];
	SCInputRef In_ShowArrows  = sc.Input[10];
	SCInputRef In_ArrowOffset = sc.Input[11];
	SCInputRef In_ShowReward  = sc.Input[12];
	SCInputRef In_BuyColor    = sc.Input[13];
	SCInputRef In_SellColor   = sc.Input[14];
	SCInputRef In_BuyArrowCol = sc.Input[15];
	SCInputRef In_SellArrCol  = sc.Input[16];
	SCInputRef In_RewardCol   = sc.Input[17];
	SCInputRef In_AlertOn     = sc.Input[18];
	SCInputRef In_AlertSound  = sc.Input[19];

	// ── Subgraph references ──────────────────────────────────────────────
	SCSubgraphRef SG_Delta   = sc.Subgraph[0];
	SCSubgraphRef SG_Scale   = sc.Subgraph[1];
	SCSubgraphRef SG_Effort  = sc.Subgraph[2];
	SCSubgraphRef SG_Result  = sc.Subgraph[3];
	SCSubgraphRef SG_Reward  = sc.Subgraph[4];
	SCSubgraphRef SG_SFail   = sc.Subgraph[5];
	SCSubgraphRef SG_SFailS  = sc.Subgraph[6];
	SCSubgraphRef SG_BuyPuls = sc.Subgraph[7];
	SCSubgraphRef SG_SellPul = sc.Subgraph[8];
	SCSubgraphRef SG_BuyArr  = sc.Subgraph[9];
	SCSubgraphRef SG_SellArr = sc.Subgraph[10];
	SCSubgraphRef SG_RewAgg  = sc.Subgraph[11];
	SCSubgraphRef SG_Ready   = sc.Subgraph[12];

	// =====================================================================
	// SetDefaults
	// =====================================================================
	if (sc.SetDefaults)
	{
		sc.GraphName = "Effort vs Result v1.0";
		sc.StudyDescription = "Bar-level effort-vs-result: whether aggressive "
			"order flow (VAP delta) was rewarded by price. Signed failure "
			"histogram plus one-bar-delayed failed-aggression confirmation. "
			"Filter/context study, not an entry system.";
		sc.AutoLoop = 0;
		sc.GraphRegion = 1;
		sc.DrawZeros = 0;
		sc.MaintainVolumeAtPriceData = 1;

		SG_Delta.Name = "Raw Bar Delta";
		SG_Delta.DrawStyle = DRAWSTYLE_LINE;
		SG_Delta.LineWidth = 1;
		SG_Delta.PrimaryColor = RGB(255, 255, 255);
		SG_Delta.DrawZeros = 1;

		SG_Scale.Name = "Effort Scale";
		SG_Scale.DrawStyle = DRAWSTYLE_LINE_SKIPZEROS;
		SG_Scale.LineWidth = 1;
		SG_Scale.PrimaryColor = RGB(130, 130, 130);
		SG_Scale.DrawZeros = 0;

		SG_Effort.Name = "Normalized Effort";
		SG_Effort.DrawStyle = DRAWSTYLE_LINE;
		SG_Effort.LineWidth = 2;
		SG_Effort.PrimaryColor = RGB(230, 230, 0);
		SG_Effort.DrawZeros = 1;

		SG_Result.Name = "Normalized Result";
		SG_Result.DrawStyle = DRAWSTYLE_LINE;
		SG_Result.LineWidth = 1;
		SG_Result.PrimaryColor = RGB(180, 180, 180);
		SG_Result.DrawZeros = 1;

		SG_Reward.Name = "Signed Reward";
		SG_Reward.DrawStyle = DRAWSTYLE_LINE;
		SG_Reward.LineWidth = 1;
		SG_Reward.PrimaryColor = RGB(120, 200, 120);
		SG_Reward.DrawZeros = 1;

		SG_SFail.Name = "Raw Signed Failure";
		SG_SFail.DrawStyle = DRAWSTYLE_BAR;
		SG_SFail.LineWidth = 1;
		SG_SFail.PrimaryColor = RGB(150, 150, 150);
		SG_SFail.DrawZeros = 0;

		SG_SFailS.Name = "Smoothed Signed Failure";
		SG_SFailS.DrawStyle = DRAWSTYLE_BAR;
		SG_SFailS.LineWidth = 3;
		SG_SFailS.PrimaryColor = RGB(150, 150, 150);
		SG_SFailS.DrawZeros = 1;

		SG_BuyPuls.Name = "Buyer Failure Pulse";
		SG_BuyPuls.DrawStyle = DRAWSTYLE_POINT;
		SG_BuyPuls.LineWidth = 4;
		SG_BuyPuls.PrimaryColor = RGB(255, 0, 255);
		SG_BuyPuls.DrawZeros = 0;

		SG_SellPul.Name = "Seller Failure Pulse";
		SG_SellPul.DrawStyle = DRAWSTYLE_POINT;
		SG_SellPul.LineWidth = 4;
		SG_SellPul.PrimaryColor = RGB(0, 220, 220);
		SG_SellPul.DrawZeros = 0;

		SG_BuyArr.Name = "Buyer Failure Arrow";
		SG_BuyArr.DrawStyle = DRAWSTYLE_ARROW_DOWN;
		SG_BuyArr.LineWidth = 2;
		SG_BuyArr.PrimaryColor = RGB(255, 0, 255);
		SG_BuyArr.DrawZeros = 0;

		SG_SellArr.Name = "Seller Failure Arrow";
		SG_SellArr.DrawStyle = DRAWSTYLE_ARROW_UP;
		SG_SellArr.LineWidth = 2;
		SG_SellArr.PrimaryColor = RGB(0, 220, 220);
		SG_SellArr.DrawZeros = 0;

		SG_RewAgg.Name = "Rewarded Aggression";
		SG_RewAgg.DrawStyle = DRAWSTYLE_BAR;
		SG_RewAgg.LineWidth = 2;
		SG_RewAgg.PrimaryColor = RGB(0, 150, 70);
		SG_RewAgg.DrawZeros = 0;

		SG_Ready.Name = "Ready Flag";
		SG_Ready.DrawStyle = DRAWSTYLE_LINE;
		SG_Ready.LineWidth = 1;
		SG_Ready.PrimaryColor = RGB(255, 255, 255);
		SG_Ready.DrawZeros = 0;

		In_EffortLen.Name = "Effort Length (EWMA bars)";
		In_EffortLen.SetInt(50);
		In_EffortLen.SetIntLimits(5, 500);

		In_ResultLen.Name = "Result Length (EWMA bars)";
		In_ResultLen.SetInt(50);
		In_ResultLen.SetIntLimits(5, 500);

		In_SmoothLen.Name = "Smoothing Length (EMA bars)";
		In_SmoothLen.SetInt(3);
		In_SmoothLen.SetIntLimits(1, 50);

		In_MinEffort.Name = "Minimum Effort (|Effort| gate)";
		In_MinEffort.SetFloat(1.25f);
		In_MinEffort.SetFloatLimits(0.0f, 20.0f);

		In_Stall.Name = "Stall Allowance";
		In_Stall.SetFloat(0.20f);
		In_Stall.SetFloatLimits(0.0f, 2.0f);

		In_ResultMode.Name = "Result Mode";
		In_ResultMode.SetCustomInputStrings(
			"Close-Open;Close-PreviousClose;Excursion-CloseLocation");
		In_ResultMode.SetCustomInputIndex(0);

		In_MaxNorm.Name = "Max Normalized Value (0 = no cap)";
		In_MaxNorm.SetFloat(5.0f);
		In_MaxNorm.SetFloatLimits(0.0f, 20.0f);

		In_ConfOn.Name = "Confirmation Enabled";
		In_ConfOn.SetYesNo(1);

		In_FavTicks.Name = "Max Favorable Extension Ticks";
		In_FavTicks.SetInt(2);
		In_FavTicks.SetIntLimits(0, 50);

		In_AdvTicks.Name = "Minimum Adverse Close Ticks";
		In_AdvTicks.SetInt(2);
		In_AdvTicks.SetIntLimits(0, 50);

		In_ShowArrows.Name = "Show Price Arrows";
		In_ShowArrows.SetYesNo(1);

		In_ArrowOffset.Name = "Arrow Offset Ticks";
		In_ArrowOffset.SetInt(2);
		In_ArrowOffset.SetIntLimits(0, 100);

		In_ShowReward.Name = "Show Rewarded Aggression";
		In_ShowReward.SetYesNo(0);

		In_BuyColor.Name = "Buyer Failure Color";
		In_BuyColor.SetColor(RGB(255, 0, 255));

		In_SellColor.Name = "Seller Failure Color";
		In_SellColor.SetColor(RGB(0, 220, 220));

		In_BuyArrowCol.Name = "Buyer Arrow Color";
		In_BuyArrowCol.SetColor(RGB(255, 0, 255));

		In_SellArrCol.Name = "Seller Arrow Color";
		In_SellArrCol.SetColor(RGB(0, 220, 220));

		In_RewardCol.Name = "Rewarded Aggression Color";
		In_RewardCol.SetColor(RGB(0, 150, 70));

		In_AlertOn.Name = "Enable Alerts";
		In_AlertOn.SetYesNo(1);

		In_AlertSound.Name = "Alert Sound Number";
		In_AlertSound.SetAlertSoundNumber(1);

		return;
	}

	// =====================================================================
	// Cleanup on study removal
	// =====================================================================
	if (sc.LastCallToFunction)
	{
		S_EvrState* pOld = reinterpret_cast<S_EvrState*>(
			sc.GetPersistentPointer(EVR_PTR_STATE));
		if (pOld != nullptr)
		{
			delete pOld;
			sc.SetPersistentPointer(EVR_PTR_STATE, nullptr);
		}
		return;
	}

	// =====================================================================
	// Read inputs into locals (every declared input is read here)
	// =====================================================================
	const int effortLen   = max(5, In_EffortLen.GetInt());
	const int resultLen   = max(5, In_ResultLen.GetInt());
	const int smoothLen   = max(1, In_SmoothLen.GetInt());
	const float minEffort = max(0.0f, In_MinEffort.GetFloat());
	const float stall     = max(0.0f, In_Stall.GetFloat());
	const int resultMode  = In_ResultMode.GetIndex();
	const float maxNorm   = max(0.0f, In_MaxNorm.GetFloat());
	const int confOn      = In_ConfOn.GetYesNo();
	const int favTicks    = max(0, In_FavTicks.GetInt());
	const int advTicks    = max(0, In_AdvTicks.GetInt());
	const int showArrows  = In_ShowArrows.GetYesNo();
	const int arrowOffset = max(0, In_ArrowOffset.GetInt());
	const int showReward  = In_ShowReward.GetYesNo();
	const COLORREF buyColor   = In_BuyColor.GetColor();
	const COLORREF sellColor  = In_SellColor.GetColor();
	const COLORREF buyArrowC  = In_BuyArrowCol.GetColor();
	const COLORREF sellArrowC = In_SellArrCol.GetColor();
	const COLORREF rewardC    = In_RewardCol.GetColor();
	const int alertOn     = In_AlertOn.GetYesNo();
	const int alertSound  = In_AlertSound.GetInt();

	float tickSize = sc.TickSize;
	if (tickSize <= 0.0f)
		tickSize = 0.0001f;

	const float alphaEffort = 2.0f / ((float)effortLen + 1.0f);
	const float alphaResult = 2.0f / ((float)resultLen + 1.0f);
	const float alphaSmooth = 2.0f / ((float)smoothLen + 1.0f);
	const int warmupBars = (effortLen > resultLen) ? effortLen : resultLen;
	const float capRef = (maxNorm > 0.0f) ? maxNorm : 5.0f;

	// =====================================================================
	// VAP guard — no VAP source, no outputs (zeros, never stale)
	// =====================================================================
	if (sc.VolumeAtPriceForBars == nullptr)
	{
		for (int i = 0; i < sc.ArraySize; i++)
		{
			SG_Delta[i] = 0.0f;
			SG_Scale[i] = 0.0f;
			SG_Effort[i] = 0.0f;
			SG_Result[i] = 0.0f;
			SG_Reward[i] = 0.0f;
			SG_SFail[i] = 0.0f;
			SG_SFailS[i] = 0.0f;
			SG_SFailS.DataColor[i] = EVR_GRAY;
			SG_BuyPuls[i] = 0.0f;
			SG_SellPul[i] = 0.0f;
			SG_BuyArr[i] = 0.0f;
			SG_SellArr[i] = 0.0f;
			SG_RewAgg[i] = 0.0f;
			SG_Ready[i] = 0.0f;
		}
		return;
	}

	// =====================================================================
	// Settings fingerprint — structural inputs force a full rebuild.
	// Display-only inputs (colors, alert enable/sound) are excluded.
	// =====================================================================
	const unsigned int ufp0 =
		((unsigned int)(effortLen & 0xFFFF)) |
		(((unsigned int)(resultLen & 0xFFFF)) << 16);
	const unsigned int ufp1 =
		((unsigned int)(smoothLen & 0xFFFF)) |
		(((unsigned int)(resultMode & 0xFF)) << 16) |
		(((unsigned int)(confOn & 0x1)) << 24) |
		(((unsigned int)(showArrows & 0x1)) << 25) |
		(((unsigned int)(showReward & 0x1)) << 26);
	unsigned int ufp2 = (unsigned int)((int)(minEffort * 1000.0f));
	ufp2 = ufp2 * 31u + (unsigned int)((int)(stall * 1000.0f));
	ufp2 = ufp2 * 31u + (unsigned int)((int)(maxNorm * 100.0f));
	ufp2 = ufp2 * 31u + (unsigned int)favTicks;
	ufp2 = ufp2 * 31u + (unsigned int)advTicks;
	ufp2 = ufp2 * 31u + (unsigned int)arrowOffset;
	const int fp0 = (int)ufp0;
	const int fp1 = (int)ufp1;
	const int fp2 = (int)ufp2;

	const bool settingsChanged =
		(sc.GetPersistentInt(EVR_INT_FP0) != fp0) ||
		(sc.GetPersistentInt(EVR_INT_FP1) != fp1) ||
		(sc.GetPersistentInt(EVR_INT_FP2) != fp2);

	// =====================================================================
	// Persistent state + full-recalc detection
	// =====================================================================
	S_EvrState* pS = reinterpret_cast<S_EvrState*>(
		sc.GetPersistentPointer(EVR_PTR_STATE));

	bool isFullRecalc =
		(sc.UpdateStartIndex == 0) ||
		(sc.IsFullRecalculation != 0) ||
		settingsChanged ||
		(pS == nullptr);

	const int lastClosed = sc.ArraySize - 2;  // ArraySize-1 is forming
	if (!isFullRecalc && pS != nullptr && lastClosed < pS->lastProcessed)
		isFullRecalc = true;  // array shrank (reload) — rebuild from scratch

	if (isFullRecalc)
	{
		sc.SetPersistentInt(EVR_INT_FP0, fp0);
		sc.SetPersistentInt(EVR_INT_FP1, fp1);
		sc.SetPersistentInt(EVR_INT_FP2, fp2);
		if (pS != nullptr)
			delete pS;
		pS = new S_EvrState();
		memset(pS, 0, sizeof(S_EvrState));
		pS->effortScale = 0.0;
		pS->trScale = 0.0;
		pS->smooth = 0.0;
		pS->lastProcessed = -1;
		sc.SetPersistentPointer(EVR_PTR_STATE, pS);
	}

	if (pS == nullptr)
		return;  // allocation failure guard; retry next call

	// =====================================================================
	// Intrabar guard — only run on new closed bars or full recalc
	// =====================================================================
	if (!isFullRecalc)
	{
		if (sc.ArraySize == sc.GetPersistentInt(EVR_INT_BARS))
			return;
	}

	if (lastClosed < 0)
		return;

	// =====================================================================
	// Bar loop — full recalc processes 0..lastClosed; incremental covers
	// every newly closed bar so reconnect/backfill gaps replay identically.
	// =====================================================================
	const int loopStart = isFullRecalc ? 0 : (pS->lastProcessed + 1);

	for (int j = loopStart; j <= lastClosed; j++)
	{
		if (j < 0)
			continue;

		// ── BarDelta from the bar's own VAP levels ──────────────────────
		const float barDelta = EVR_BarDelta(sc, j);
		const float absDelta = fabsf(barDelta);

		// ── EffortScale: EWMA(abs(BarDelta)), seeded, floored ───────────
		if (j == 0 || (isFullRecalc && j == loopStart && pS->lastProcessed < 0))
			pS->effortScale = (double)absDelta;
		else
			pS->effortScale += (double)alphaEffort * ((double)absDelta - pS->effortScale);
		float scale = (float)pS->effortScale;
		if (scale < 1.0f)
			scale = 1.0f;

		// ── Normalized effort, optionally capped ────────────────────────
		float effort = barDelta / scale;
		if (maxNorm > 0.0f)
		{
			if (effort > maxNorm)
				effort = maxNorm;
			else if (effort < -maxNorm)
				effort = -maxNorm;
		}

		// ── ResultRaw per Result Mode ───────────────────────────────────
		float rawResult = 0.0f;
		if (resultMode == 1)
		{
			const float prevClose = (j > 0) ? sc.Close[j - 1] : sc.Open[j];
			rawResult = sc.Close[j] - prevClose;
		}
		else if (resultMode == 2)
		{
			const float mid = (sc.High[j] + sc.Low[j]) * 0.5f;
			rawResult = 0.5f * (sc.Close[j] - sc.Open[j])
				+ 0.5f * (sc.Close[j] - mid);
		}
		else
		{
			rawResult = sc.Close[j] - sc.Open[j];
		}

		// ── True Range + EWMA scale with TickSize floor ─────────────────
		float barRange = sc.High[j] - sc.Low[j];
		if (barRange < 0.0f)
			barRange = 0.0f;
		float tr = barRange;
		if (j > 0)
		{
			const float hpc = fabsf(sc.High[j] - sc.Close[j - 1]);
			const float lpc = fabsf(sc.Low[j] - sc.Close[j - 1]);
			if (hpc > tr)
				tr = hpc;
			if (lpc > tr)
				tr = lpc;
		}
		if (j == 0 || (isFullRecalc && j == loopStart && pS->lastProcessed < 0))
			pS->trScale = (double)tr;
		else
			pS->trScale += (double)alphaResult * ((double)tr - pS->trScale);
		float trScaleF = (float)pS->trScale;
		if (trScaleF < tickSize)
			trScaleF = tickSize;

		const float resultNorm = rawResult / trScaleF;

		// ── Signed reward / failure ─────────────────────────────────────
		const float eSign = EVR_Sign(effort);
		const float sReward = eSign * resultNorm;
		float failMag = 0.0f;
		if (fabsf(effort) >= minEffort)
		{
			const float gap = stall - sReward;
			if (gap > 0.0f)
				failMag = fabsf(effort) * gap;
		}
		const float sFail = -eSign * failMag;

		// ── Smoothed failure (EMA, seeded on first processed bar) ───────
		if (j == 0 || (isFullRecalc && j == loopStart && pS->lastProcessed < 0))
			pS->smooth = (double)sFail;
		else
			pS->smooth += (double)alphaSmooth * ((double)sFail - pS->smooth);
		const float sfSmooth = (float)pS->smooth;

		// ── Write continuous subgraphs ──────────────────────────────────
		SG_Delta[j] = barDelta;
		SG_Scale[j] = scale;
		SG_Effort[j] = effort;
		SG_Result[j] = resultNorm;
		SG_Reward[j] = sReward;
		SG_SFail[j] = sFail;
		SG_SFailS[j] = sfSmooth;
		if (sfSmooth > 0.0f)
			SG_SFailS.DataColor[j] = EVR_Blend(EVR_GRAY, sellColor,
				fabsf(sfSmooth) / capRef);
		else if (sfSmooth < 0.0f)
			SG_SFailS.DataColor[j] = EVR_Blend(EVR_GRAY, buyColor,
				fabsf(sfSmooth) / capRef);
		else
			SG_SFailS.DataColor[j] = EVR_GRAY;

		SG_BuyPuls[j] = 0.0f;
		SG_SellPul[j] = 0.0f;
		SG_BuyArr[j] = 0.0f;
		SG_SellArr[j] = 0.0f;

		float rewarded = 0.0f;
		if (showReward != 0 && fabsf(effort) >= minEffort && sReward > 0.0f)
			rewarded = sReward;
		SG_RewAgg[j] = rewarded;
		if (rewarded != 0.0f)
			SG_RewAgg.DataColor[j] = rewardC;

		SG_Ready[j] = (j >= warmupBars) ? 1.0f : 0.0f;

		// ── One-bar-delayed confirmation (prints on j, reads j-1 max) ───
		if (confOn != 0 && j >= 1)
		{
			const float candEffort = SG_Effort[j - 1];
			const int readyPrev = ((j - 1) >= warmupBars) ? 1 : 0;
			if (fabsf(candEffort) >= minEffort && readyPrev != 0)
			{
				const float midPrev =
					(sc.High[j - 1] + sc.Low[j - 1]) * 0.5f;
				if (candEffort > 0.0f)
				{
					// Candidate buyers: no extension over candidate high,
					// adverse close below candidate midpoint.
					const float extLim =
						sc.High[j - 1] + (float)favTicks * tickSize;
					const float advLim =
						midPrev - (float)advTicks * tickSize;
					if (sc.High[j] <= extLim && sc.Close[j] <= advLim)
					{
						SG_BuyPuls[j] = sfSmooth;
						SG_BuyPuls.DataColor[j] = buyArrowC;
						if (showArrows != 0)
						{
							SG_BuyArr[j] = sc.High[j]
								+ (float)arrowOffset * tickSize;
							SG_BuyArr.DataColor[j] = buyArrowC;
						}
					}
				}
				else if (candEffort < 0.0f)
				{
					// Candidate sellers: mirror at candidate low/midpoint.
					const float extLim =
						sc.Low[j - 1] - (float)favTicks * tickSize;
					const float advLim =
						midPrev + (float)advTicks * tickSize;
					if (sc.Low[j] >= extLim && sc.Close[j] >= advLim)
					{
						SG_SellPul[j] = sfSmooth;
						SG_SellPul.DataColor[j] = sellArrowC;
						if (showArrows != 0)
						{
							SG_SellArr[j] = sc.Low[j]
								- (float)arrowOffset * tickSize;
							SG_SellArr.DataColor[j] = sellArrowC;
						}
					}
				}
			}
		}
	}

	// ── Forming bar: explicit zeros, never a signal source ───────────────
	const int forming = sc.ArraySize - 1;
	if (forming > lastClosed)
	{
		SG_Delta[forming] = 0.0f;
		SG_Scale[forming] = 0.0f;
		SG_Effort[forming] = 0.0f;
		SG_Result[forming] = 0.0f;
		SG_Reward[forming] = 0.0f;
		SG_SFail[forming] = 0.0f;
		SG_SFailS[forming] = 0.0f;
		SG_SFailS.DataColor[forming] = EVR_GRAY;
		SG_BuyPuls[forming] = 0.0f;
		SG_SellPul[forming] = 0.0f;
		SG_BuyArr[forming] = 0.0f;
		SG_SellArr[forming] = 0.0f;
		SG_RewAgg[forming] = 0.0f;
		SG_Ready[forming] = 0.0f;
	}

	pS->lastProcessed = lastClosed;
	sc.SetPersistentInt(EVR_INT_BARS, sc.ArraySize);

	// =====================================================================
	// Alerts — post-loop watermark scan (house pattern). Full recalc
	// fast-forwards watermarks and never alerts historical bars.
	// =====================================================================
	int& alertedBuy = sc.GetPersistentInt(EVR_INT_ALERTB);
	int& alertedSell = sc.GetPersistentInt(EVR_INT_ALERTS);

	if (isFullRecalc || alertedBuy == 0 || alertedSell == 0)
	{
		alertedBuy = lastClosed;
		alertedSell = lastClosed;
		return;
	}

	const bool canAlert = (alertOn != 0) && (alertSound > 0) && (confOn != 0);
	if (!canAlert)
	{
		alertedBuy = lastClosed;
		alertedSell = lastClosed;
		return;
	}

	int scanFloor = lastClosed - (EVR_ALERT_SCAN - 1);
	if (scanFloor < 0)
		scanFloor = 0;

	int startB = (alertedBuy + 1 > scanFloor) ? (alertedBuy + 1) : scanFloor;
	for (int b = lastClosed; b >= startB; b--)
	{
		if (SG_BuyPuls[b] != 0.0f)
		{
			SCString msg;
			msg.Format("Effort vs Result: buyer failed aggression evidence "
				"(%d bar(s) back)", lastClosed - b);
			sc.SetAlert(alertSound, forming, msg);
			alertedBuy = b;
			break;
		}
	}

	int startS = (alertedSell + 1 > scanFloor) ? (alertedSell + 1) : scanFloor;
	for (int b = lastClosed; b >= startS; b--)
	{
		if (SG_SellPul[b] != 0.0f)
		{
			SCString msg;
			msg.Format("Effort vs Result: seller failed aggression evidence "
				"(%d bar(s) back)", lastClosed - b);
			sc.SetAlert(alertSound, forming, msg);
			alertedSell = b;
			break;
		}
	}
}
