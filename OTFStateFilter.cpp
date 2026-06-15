// =============================================================================
// OTFStateFilter.cpp
// Sierra Chart ACSIL Custom Study
//
// SETUP:
//   Set Source Chart Number = 0 to run on this chart's own data.
//   Set Source Chart Number = N to pull Low/High from chart N (e.g. your
//   renko chart) while the study lives on your trading chart.
//
// BAR CLASSIFICATION — strict Higher Lows / Lower Highs:
//
//   OTF-Up entry   : L[2] < L[1]  AND  L[1] < L[0]   (two strict Higher Lows)
//   OTF-Up exit    : L[0] < LSHL
//     where LSHL[entry] = L[entry]
//           LSHL[t]     = max(LSHL[t-1], L[t-1])   for t > entry
//     (LSHL ratchets up to the highest low seen since entry;
//      exit fires the first time current Low breaks below it)
//
//   OTF-Down entry : H[2] > H[1]  AND  H[1] > H[0]   (two strict Lower Highs)
//   OTF-Down exit  : H[0] > LSLH
//     where LSLH[entry] = H[entry]
//           LSLH[t]     = min(LSLH[t-1], H[t-1])   for t > entry
//     (LSLH ratchets down to the lowest high seen since entry;
//      exit fires the first time current High breaks above it)
//
//   State always passes through Neutral when switching direction.
//   After exit the neutral check runs on the same bar, so an immediate
//   entry in the opposite direction is possible.
//
// OUTPUT:
//   SG0  State           : +1.0 = Up,  0.0 = Neutral,  -1.0 = Down
//   SG1  Up Context      : background green  (state == +1)
//   SG2  Down Context    : background red    (state == -1)
//
// INPUT:
//   [0]  Source Chart Number  (0 = this chart)
//
// AUXILIARY ARRAYS (sg_State.Arrays[]):
//   [0]  state per bar
//   [1]  LSHL per bar  (Last Strict Higher Low — running max of lows)
//   [2]  LSLH per bar  (Last Strict Lower High — running min of highs)
// =============================================================================

#include "sierrachart.h"

SCDLLName("OTFStateFilter")

// =============================================================================
SCSFExport scsf_OTFStateFilter(SCStudyInterfaceRef sc)
{
    // -------------------------------------------------------------------------
    // SUBGRAPHS
    // -------------------------------------------------------------------------
    SCSubgraphRef sg_State = sc.Subgraph[0]; // +1 / 0 / -1
    SCSubgraphRef sg_Up    = sc.Subgraph[1]; // Up context background
    SCSubgraphRef sg_Down  = sc.Subgraph[2]; // Down context background

    // sg_State.Arrays[0] = state per bar
    // sg_State.Arrays[1] = LSHL per bar
    // sg_State.Arrays[2] = LSLH per bar

    // -------------------------------------------------------------------------
    // INPUT
    // -------------------------------------------------------------------------
    SCInputRef in_SourceChart = sc.Input[0];

    // =========================================================================
    // SET DEFAULTS
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "OTF State Filter";
        sc.StudyDescription =
            "Trend state machine based on strict Higher Lows (Up) and Lower Highs "
            "(Down). Entry on two consecutive strict HLs or LHs. Exit when current "
            "Low breaks the running max of lows since entry (LSHL), or current High "
            "breaks the running min of highs since entry (LSLH). "
            "Set Source Chart Number = 0 for native data, or enter the chart number "
            "of your OTF/renko chart to pull data cross-chart.";
        sc.AutoLoop         = 1;
        sc.GraphRegion      = 1;   // sub-panel — move to 0 to overlay on price
        sc.ScaleRangeType   = SCALE_USERDEFINED;
        sc.ScaleRangeTop    = 2.0f;
        sc.ScaleRangeBottom = -2.0f;
        sc.FreeDLL          = 0;
        sc.DrawZeros        = 1;
        sc.UpdateAlways     = 1;

        sg_State.Name         = "OTF State  (+1 / 0 / -1)";
        sg_State.DrawStyle    = DRAWSTYLE_STAIR_STEP;
        sg_State.LineWidth    = 2;
        sg_State.PrimaryColor = RGB(180, 180, 180);
        sg_State.DrawZeros    = 1;

        sg_Up.Name                = "Up Context";
        sg_Up.DrawStyle           = DRAWSTYLE_BACKGROUND;
        sg_Up.PrimaryColor        = RGB(0, 180, 80);
        sg_Up.DrawZeros           = 0;
        sg_Up.DisplayNameValueInWindowsFlags = 0;

        sg_Down.Name              = "Down Context";
        sg_Down.DrawStyle         = DRAWSTYLE_BACKGROUND;
        sg_Down.PrimaryColor      = RGB(200, 50, 50);
        sg_Down.DrawZeros         = 0;
        sg_Down.DisplayNameValueInWindowsFlags = 0;

        in_SourceChart.Name = "Source Chart  (0 = this chart)";
        in_SourceChart.SetChartNumber(0);

        return;
    }

    // =========================================================================
    // PER-BAR CALCULATION
    // =========================================================================
    const int idx = sc.Index;

    if (idx < 2)
    {
        sg_State.Arrays[0][idx] = 0.0f;
        sg_State.Arrays[1][idx] = 0.0f;
        sg_State.Arrays[2][idx] = 0.0f;
        sg_State[idx] = 0.0f;
        sg_Up[idx]    = 0.0f;
        sg_Down[idx]  = 0.0f;
        return;
    }

    // -------------------------------------------------------------------------
    // Resolve Low and High arrays — native or cross-chart
    // Sierra Chart base price study (ID=1): SG1=High (idx 1), SG2=Low (idx 2)
    // -------------------------------------------------------------------------
    const int sourceChart = (in_SourceChart.GetChartNumber() <= 0)
                            ? sc.ChartNumber
                            : in_SourceChart.GetChartNumber();

    float newState = 0.0f;

    if (sourceChart == sc.ChartNumber)
    {
        // ---- NATIVE: per-bar incremental state machine ----------------------
        const float L0 = sc.Low[idx],      L1 = sc.Low[idx - 1],      L2 = sc.Low[idx - 2];
        const float H0 = sc.High[idx],     H1 = sc.High[idx - 1],     H2 = sc.High[idx - 2];

        const float prevState = sg_State.Arrays[0][idx - 1];
        const float prevLSHL  = sg_State.Arrays[1][idx - 1];
        const float prevLSLH  = sg_State.Arrays[2][idx - 1];

        newState          = prevState;
        float newLSHL     = prevLSHL;
        float newLSLH     = prevLSLH;

        if (prevState == 1.0f)
        {
            newLSHL = (prevLSHL > L1) ? prevLSHL : L1;
            if (L0 < newLSHL) { newState = 0.0f; newLSHL = 0.0f; }
        }
        else if (prevState == -1.0f)
        {
            newLSLH = (prevLSLH < H1) ? prevLSLH : H1;
            if (H0 > newLSLH) { newState = 0.0f; newLSLH = 0.0f; }
        }

        if (newState == 0.0f)
        {
            if      (L2 < L1 && L1 < L0) { newState =  1.0f; newLSHL = L0; newLSLH = 0.0f; }
            else if (H2 > H1 && H1 > H0) { newState = -1.0f; newLSLH = H0; newLSHL = 0.0f; }
        }

        sg_State.Arrays[1][idx] = newLSHL;
        sg_State.Arrays[2][idx] = newLSLH;
    }  // end native block
    else
    {
        // ---- CROSS-CHART: full loop through source chart history ------------
        // Run the complete state machine on all source chart bars so that
        // LSHL/LSLH accumulate correctly from the beginning of history.
        SCGraphData baseData;
        sc.GetChartBaseData(sourceChart, baseData);

        SCFloatArrayRef highArr = baseData[SC_HIGH];
        SCFloatArrayRef lowArr  = baseData[SC_LOW];

        const int n = highArr.GetArraySize();
        if (n < 3)
        {
            // Source chart not ready — carry previous state forward
            newState = sg_State.Arrays[0][idx - 1];
            sg_State.Arrays[1][idx] = sg_State.Arrays[1][idx - 1];
            sg_State.Arrays[2][idx] = sg_State.Arrays[2][idx - 1];
            sg_State[idx] = newState;
            sg_Up[idx]    = (newState ==  1.0f) ? 1.0f : 0.0f;
            sg_Down[idx]  = (newState == -1.0f) ? 1.0f : 0.0f;
            return;
        }

        int   state = 0;
        float LSHL  = 0.0f;
        float LSLH  = 0.0f;

        for (int i = 2; i < n; i++)
        {
            const bool hl2 = lowArr[i]  > lowArr[i-1]  && lowArr[i-1]  > lowArr[i-2];
            const bool lh2 = highArr[i] < highArr[i-1] && highArr[i-1] < highArr[i-2];

            if (state == 1)
            {
                LSHL = (LSHL > lowArr[i-1]) ? LSHL : lowArr[i-1];
                if (LSHL > lowArr[i]) { state = 0; LSHL = 0.0f; }
            }
            else if (state == -1)
            {
                LSLH = (LSLH < highArr[i-1]) ? LSLH : highArr[i-1];
                if (LSLH < highArr[i]) { state = 0; LSLH = 0.0f; }
            }
            else  // neutral — else prevents same-bar re-entry after exit
            {
                if      (hl2) { state =  1; LSHL = lowArr[i]; }
                else if (lh2) { state = -1; LSLH = highArr[i]; }
            }
        }

        newState = (float)state;  // 0, +1, or -1
        sg_State.Arrays[1][idx] = 0.0f;  // not used in cross-chart mode
        sg_State.Arrays[2][idx] = 0.0f;
    }

    // -------------------------------------------------------------------------
    // Persist and output
    // -------------------------------------------------------------------------
    sg_State.Arrays[0][idx] = newState;

    sg_State[idx] = newState;
    sg_Up[idx]    = (newState ==  1.0f) ? 1.0f : 0.0f;
    sg_Down[idx]  = (newState == -1.0f) ? 1.0f : 0.0f;
}
