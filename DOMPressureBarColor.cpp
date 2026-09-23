#include "sierrachart.h"

// ============================================================================
// DOMPressureBarColor.cpp
//
// Companion to DOM Pressure v3. Lives in the PRICE region (GraphRegion 0)
// and colors the main price bars from a referenced study subgraph — intended
// source: DOM Pressure v3 SG5 "Normalized Net" (adaptive units where ~1.0 is
// the average magnitude of recent net pull/stack).
//
// v1.1: the source is now a CHART + study + subgraph reference, so this study
// can run on charts where DOM Pressure v3 is NOT present (e.g. v3 on the
// 2-min chart feeding bar colors on a 500-tick chart). Chart Number 0 = this
// chart (v1.0 behaviour). When the source lives on another chart, each local
// bar is time-mapped to the containing source bar; several fast-chart bars
// sharing one slow-chart source bar will share its color. The source chart
// must be open in the same chartbook.
//
// Behaviour:
//   |value| <  threshold          -> bar left at its normal chart colors
//   |value| >= threshold          -> colored: positive = Up Color (bid-side
//                                    liquidity stepping in), negative = Down
//                                    Color (ask-side stacking / bids pulling)
//   Gradient Intensity = Yes      -> color fades in from muted at the
//                                    threshold to fully saturated at the
//                                    "Full Saturation At" value, so bar
//                                    intensity shows HOW hard liquidity is
//                                    stepping in, not just direction.
//
// Chart setup (same chart): add AFTER DOM Pressure v3 in the study list (SC
// calculates studies in order); set the source to chart 0 / DOM Pressure v3 /
// Normalized Net. (Other chart): set the source chart number to the chart
// hosting v3, then pick the study and Normalized Net subgraph.
// ============================================================================

SCDLLName("DOMPressureBarColor")

/*==========================================================================*/
SCSFExport scsf_DOMPressureBarColor(SCStudyInterfaceRef sc)
{
    SCSubgraphRef sgColor = sc.Subgraph[0];

    SCInputRef inSource   = sc.Input[0];
    SCInputRef inThresh   = sc.Input[1];
    SCInputRef inGradient = sc.Input[2];
    SCInputRef inSatAt    = sc.Input[3];
    SCInputRef inMinFade  = sc.Input[4];

    if (sc.SetDefaults)
    {
        sc.GraphName   = "DOM Pressure Bar Color";
        sc.GraphRegion = 0;              // price region: required for COLOR_BAR
        sc.AutoLoop    = 1;
        sc.UpdateAlways = 1;             // keep forming bar fresh from a cross-chart source
        sc.DrawStudyUnderneathMainPriceGraph = 0;

        sgColor.Name = "Pressure Bar Color";
        sgColor.DrawStyle = DRAWSTYLE_COLOR_BAR;
        sgColor.PrimaryColor   = RGB(40, 130, 230);   // up / bid-side stacking
        sgColor.SecondaryColor = RGB(220, 60, 60);    // down / ask-side stacking
        sgColor.SecondaryColorUsed = 1;
        sgColor.DrawZeros = 0;           // 0 = bar keeps its normal color

        inSource.Name = "Pressure Source (chart / study / subgraph)";
        inSource.SetChartStudySubgraphValues(0, 0, 4);  // chart 0 = this chart; SG5 Normalized Net

        inThresh.Name = "Color Threshold (abs value)";
        inThresh.SetFloat(1.0f);         // 1.0 = average recent magnitude

        inGradient.Name = "Gradient Intensity";
        inGradient.SetYesNo(1);

        inSatAt.Name = "Full Saturation At (abs value)";
        inSatAt.SetFloat(3.0f);

        inMinFade.Name = "Minimum Intensity Percent (at threshold)";
        inMinFade.SetInt(45); inMinFade.SetIntLimits(10, 100);

        return;
    }

    s_ChartStudySubgraphValues csv = inSource.GetChartStudySubgraphValues();
    if (csv.ChartNumber <= 0)
        csv.ChartNumber = sc.ChartNumber;   // 0 = this chart (v1.0 behaviour)

    SCFloatArray src;
    sc.GetStudyArrayFromChartUsingID(csv, src);
    if (src.GetArraySize() == 0)
    {
        sgColor[sc.Index] = 0.0f;
        return;
    }

    // Map this chart's bar to the source chart's containing bar by datetime.
    int srcIndex = sc.Index;
    if (csv.ChartNumber != sc.ChartNumber)
        srcIndex = sc.GetContainingIndexForSCDateTime(csv.ChartNumber,
                                                      sc.BaseDateTimeIn[sc.Index]);

    if (srcIndex < 0 || srcIndex >= src.GetArraySize())
    {
        sgColor[sc.Index] = 0.0f;
        return;
    }

    const double v   = src[srcIndex];
    const double a   = fabs(v);
    const double thr = inThresh.GetFloat();

    if (a < thr || thr <= 0.0)
    {
        sgColor[sc.Index] = 0.0f;        // below threshold: leave bar uncolored
        return;
    }

    COLORREF base = (v >= 0.0) ? sgColor.PrimaryColor : sgColor.SecondaryColor;
    COLORREF out  = base;

    if (inGradient.GetYesNo())
    {
        const double sat = inSatAt.GetFloat();
        double t = (sat > thr) ? (a - thr) / (sat - thr) : 1.0;
        if (t > 1.0) t = 1.0;
        if (t < 0.0) t = 0.0;

        // Fade from muted (blended toward mid-gray) at the threshold up to
        // the full base color at/above the saturation value.
        const double minF = inMinFade.GetInt() / 100.0;
        const double f = minF + (1.0 - minF) * t;
        int r = (int)(128.0 + (GetRValue(base) - 128.0) * f);
        int g = (int)(128.0 + (GetGValue(base) - 128.0) * f);
        int b = (int)(128.0 + (GetBValue(base) - 128.0) * f);
        out = RGB(r, g, b);
    }

    sgColor[sc.Index] = 1.0f;            // any non-zero value = color this bar
    sgColor.DataColor[sc.Index] = out;
}
