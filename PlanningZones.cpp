#include "sierrachart.h"
SCDLLName("PlanningZones")

// PlanningZones v1.2 — overlay HTF planning levels from json\{Symbol}.json
// Folder default: C:\Users\flavi\Documents\SC_PlanningZones
// ACSIL cannot read xlsx. Excel is the human master; sync_xlsx_to_json.py
// writes the JSON the study loads. Reload on bar close and/or poll timer.
// Cash quotes stay native. ETF rows multiply by ratio; index rows add basis:
//   NQ = QQQ * (QQQ to NQ Ratio) + (NQ - NDX)
//   NQ = NDX + (NQ - NDX)
//   ES = SPY * (SPY to ES Ratio) + (ES - SPX)
//   ES = SPX + (ES - SPX)
// Ratio 0 means 1.0. Basis is points, may be negative. Round to tick.
// Drawings: DRAWING_RECTANGLE_EXT_HIGHLIGHT (fill+outline) + DRAWING_TEXT
// labels. Rectangles do not print usable comments; labels are separate.
// Study-owned drawings (AddAsUserDrawnDrawing=0), LineNumber namespace
// 820000 rect / 821000 text / 829999 status. Persistent struct slot 1.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const int PZ_MAX_ZONES     = 64;
static const int PZ_MAX_FILE      = 65536;
static const int PZ_PTR_SLOT      = 1;
static const int PZ_BASE_RECT     = 820000;
static const int PZ_BASE_TEXT     = 821000;
static const int PZ_LINE_STATUS   = 829999;
static const int PZ_NAME_LEN      = 64;
static const int PZ_NOTE_LEN      = 96;
static const int PZ_ID_LEN        = 48;
static const int PZ_PATH_LEN      = 260;
static const int PZ_SYM_LEN       = 32;

static const int PZ_KIND_LEVEL    = 0;
static const int PZ_KIND_RANGE    = 1;
static const int PZ_KIND_GAP      = 2;
static const int PZ_KIND_SUPPORT  = 3;
static const int PZ_KIND_RESIST   = 4;
static const int PZ_KIND_WATCH    = 5;

static const int PZ_QUOTE_NATIVE  = 0;
static const int PZ_QUOTE_QQQ     = 1;
static const int PZ_QUOTE_NDX     = 2;
static const int PZ_QUOTE_SPY     = 3;
static const int PZ_QUOTE_SPX     = 4;

struct PZZone
{
    char   Id[PZ_ID_LEN];
    char   Name[PZ_NAME_LEN];
    char   Note[PZ_NOTE_LEN];
    double Low;
    double High;
    int    Kind;
    int    Quote;
    int    Active;
    unsigned Color;
};

struct PZState
{
    PZZone Zones[PZ_MAX_ZONES];
    int    Count;
    int    DrawnCount;
    int    LastArraySize;
    int    LastReloadTog;
    int    LastHide;
    int    FileOk;
    unsigned Checksum;
    long   FileSize;
    double LastPoll;
    int    LastVisLeft;
    int    DrawFp;
    float  LastQqqR;
    float  LastNdxR;
    float  LastSpyR;
    float  LastSpxR;
    char   Path[PZ_PATH_LEN];
    char   Symbol[PZ_SYM_LEN];
};

static int PZMinI(int a, int b) { return a < b ? a : b; }
static int PZMaxI(int a, int b) { return a > b ? a : b; }
static double PZMinD(double a, double b) { return a < b ? a : b; }
static double PZMaxD(double a, double b) { return a > b ? a : b; }

static unsigned PZChecksum(const char *buf, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; ++i)
    {
        h ^= (unsigned char)buf[i];
        h *= 16777619u;
    }
    return h;
}

static void PZCopyTrunc(char *dst, int cap, const char *src)
{
    if (cap <= 0)
        return;
    int n = 0;
    if (src)
    {
        while (src[n] != 0 && n + 1 < cap)
            ++n;
        memcpy(dst, src, (size_t)n);
    }
    dst[n] = 0;
}

static int PZKindFromName(const char *s)
{
    if (s == 0 || s[0] == 0)
        return PZ_KIND_LEVEL;
    if (strcmp(s, "range") == 0) return PZ_KIND_RANGE;
    if (strcmp(s, "gap") == 0) return PZ_KIND_GAP;
    if (strcmp(s, "support") == 0) return PZ_KIND_SUPPORT;
    if (strcmp(s, "resistance") == 0) return PZ_KIND_RESIST;
    if (strcmp(s, "watch") == 0) return PZ_KIND_WATCH;
    if (strcmp(s, "level") == 0) return PZ_KIND_LEVEL;
    return PZ_KIND_LEVEL;
}

static int PZQuoteFromName(const char *s)
{
    if (s == 0 || s[0] == 0)
        return PZ_QUOTE_NATIVE;
    char b[8];
    int n = 0;
    while (s[n] != 0 && n < 7)
    {
        char c = s[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        b[n++] = c;
    }
    b[n] = 0;
    if (strcmp(b, "QQQ") == 0) return PZ_QUOTE_QQQ;
    if (strcmp(b, "NDX") == 0) return PZ_QUOTE_NDX;
    if (strcmp(b, "SPY") == 0) return PZ_QUOTE_SPY;
    if (strcmp(b, "SPX") == 0) return PZ_QUOTE_SPX;
    return PZ_QUOTE_NATIVE;
}

static double PZToFutures(double px, int quote, double qqqR, double ndxB,
                          double spyR, double spxB, double tick)
{
    double v = px;
    if (quote == PZ_QUOTE_QQQ)
    {
        double r = qqqR;
        if (r <= 0.0)
            r = 1.0;
        v = px * r + ndxB;
    }
    else if (quote == PZ_QUOTE_NDX)
        v = px + ndxB;
    else if (quote == PZ_QUOTE_SPY)
    {
        double r = spyR;
        if (r <= 0.0)
            r = 1.0;
        v = px * r + spxB;
    }
    else if (quote == PZ_QUOTE_SPX)
        v = px + spxB;

    if (tick > 0.0)
        v = floor(v / tick + 0.5) * tick;
    return v;
}

static COLORREF PZDefaultColor(int kind)
{
    if (kind == PZ_KIND_RANGE)   return RGB(58, 123, 213);
    if (kind == PZ_KIND_GAP)     return RGB(156, 89, 209);
    if (kind == PZ_KIND_SUPPORT) return RGB(46, 160, 90);
    if (kind == PZ_KIND_RESIST)  return RGB(200, 70, 70);
    if (kind == PZ_KIND_WATCH)   return RGB(220, 200, 80);
    return RGB(232, 163, 23);
}

static unsigned PZParseHexColor(const char *s)
{
    if (s == 0 || s[0] == 0)
        return 0;
    if (s[0] == '#')
        ++s;
    unsigned v = 0;
    int n = 0;
    for (; s[n] != 0 && n < 6; ++n)
    {
        char c = s[n];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (unsigned)d;
    }
    if (n != 6)
        return 0;
    int r = (int)((v >> 16) & 255);
    int g = (int)((v >> 8) & 255);
    int b = (int)(v & 255);
    return (unsigned)RGB(r, g, b);
}

static const char *PZSkipWs(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        ++p;
    return p;
}

static int PZExtractString(const char *obj, const char *key, char *out, int cap)
{
    if (out && cap > 0)
        out[0] = 0;
    char pat[72];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (p == 0)
        return 0;
    p = strchr(p + (int)strlen(pat), ':');
    if (p == 0)
        return 0;
    p = PZSkipWs(p + 1);
    if (*p != '"')
        return 0;
    ++p;
    int n = 0;
    while (*p != 0 && *p != '"' && n + 1 < cap)
    {
        if (*p == '\\' && p[1] != 0)
            ++p;
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

static int PZExtractNumber(const char *obj, const char *key, double *out)
{
    char pat[72];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (p == 0)
        return 0;
    p = strchr(p + (int)strlen(pat), ':');
    if (p == 0)
        return 0;
    p = PZSkipWs(p + 1);
    *out = atof(p);
    return 1;
}

static int PZExtractBool(const char *obj, const char *key, int *out)
{
    char pat[72];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (p == 0)
        return 0;
    p = strchr(p + (int)strlen(pat), ':');
    if (p == 0)
        return 0;
    p = PZSkipWs(p + 1);
    if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
    *out = atoi(p) != 0;
    return 1;
}

static void PZSanitizeSymbol(const char *in, char *out, int cap)
{
    int n = 0;
    for (int i = 0; in && in[i] != 0 && n + 1 < cap; ++i)
    {
        char c = in[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out[n++] = c;
    }
    out[n] = 0;
}

static void PZFromChartSymbol(const char *sym, char *out, int cap)
{
    out[0] = 0;
    if (sym == 0)
        return;
    while (*sym != 0 && !((*sym >= 'A' && *sym <= 'Z') || (*sym >= 'a' && *sym <= 'z')))
        ++sym;
    int n = 0;
    while (*sym != 0 && n + 1 < cap
           && ((*sym >= 'A' && *sym <= 'Z') || (*sym >= 'a' && *sym <= 'z')))
    {
        char c = *sym++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        out[n++] = c;
    }
    out[n] = 0;
}

static void PZJoinPath(char *out, int cap, const char *folder, const char *symbol)
{
    int n = (int)strlen(folder);
    while (n > 0 && (folder[n - 1] == '\\' || folder[n - 1] == '/'))
        --n;
    snprintf(out, cap, "%.*s\\json\\%s.json", n, folder, symbol);
}

static int PZReadFile(const char *path, char *buf, int cap, int *sizeOut)
{
    FILE *f = fopen(path, "rb");
    if (f == 0)
        return 0;
    int n = (int)fread(buf, 1, (size_t)(cap - 1), f);
    fclose(f);
    if (n < 0)
        n = 0;
    buf[n] = 0;
    if (sizeOut)
        *sizeOut = n;
    if (n >= 3 && (unsigned char)buf[0] == 0xEF
        && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    {
        memmove(buf, buf + 3, (size_t)(n - 3 + 1));
        n -= 3;
        if (sizeOut)
            *sizeOut = n;
    }
    return 1;
}

static int PZParseZones(const char *buf, PZZone *zones, int maxZones, int defQuote)
{
    const char *zkey = strstr(buf, "\"zones\"");
    if (zkey == 0)
        return 0;
    const char *p = strchr(zkey, '[');
    if (p == 0)
        return 0;
    ++p;
    int count = 0;
    while (*p != 0 && *p != ']' && count < maxZones)
    {
        p = PZSkipWs(p);
        if (*p == ']')
            break;
        if (*p != 123)
        {
            ++p;
            continue;
        }
        const char *start = p;
        int depth = 0;
        do
        {
            if (*p == 123) ++depth;
            else if (*p == 125) --depth;
            ++p;
        } while (*p != 0 && depth > 0);

        char obj[2048];
        int len = (int)(p - start);
        if (len > 2047)
            len = 2047;
        memcpy(obj, start, (size_t)len);
        obj[len] = 0;

        PZZone z;
        memset(&z, 0, sizeof(z));
        z.Active = 1;
        char tmp[PZ_NOTE_LEN];
        PZExtractString(obj, "id", z.Id, PZ_ID_LEN);
        PZExtractString(obj, "name", z.Name, PZ_NAME_LEN);
        PZExtractString(obj, "note", z.Note, PZ_NOTE_LEN);
        PZExtractNumber(obj, "low", &z.Low);
        PZExtractNumber(obj, "high", &z.High);
        if (PZExtractString(obj, "kind", tmp, PZ_NOTE_LEN))
            z.Kind = PZKindFromName(tmp);
        else
            z.Kind = (z.Low == z.High) ? PZ_KIND_LEVEL : PZ_KIND_RANGE;
        z.Quote = defQuote;
        if (PZExtractString(obj, "quote", tmp, PZ_NOTE_LEN))
            z.Quote = PZQuoteFromName(tmp);
        PZExtractBool(obj, "active", &z.Active);
        if (PZExtractString(obj, "color", tmp, PZ_NOTE_LEN))
            z.Color = PZParseHexColor(tmp);
        if (z.Color == 0)
            z.Color = (unsigned)PZDefaultColor(z.Kind);
        if (z.Name[0] == 0)
            PZCopyTrunc(z.Name, PZ_NAME_LEN, z.Id);
        if (z.Active)
            zones[count++] = z;
        p = PZSkipWs(p);
        if (*p == ',')
            ++p;
    }
    return count;
}

static void PZDeleteLine(SCStudyInterfaceRef sc, int lineNumber)
{
    sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNumber);
}

static void PZDeleteAll(SCStudyInterfaceRef sc, PZState *S)
{
    const int n = S ? PZMaxI(S->DrawnCount, S->Count) : PZ_MAX_ZONES;
    for (int i = 0; i < n; ++i)
    {
        PZDeleteLine(sc, PZ_BASE_RECT + i);
        PZDeleteLine(sc, PZ_BASE_TEXT + i);
    }
    PZDeleteLine(sc, PZ_LINE_STATUS);
    if (S)
        S->DrawnCount = 0;
}

static void PZDrawStatus(SCStudyInterfaceRef sc, int last, const char *text, COLORREF color)
{
    if (last < 0)
        return;
    s_UseTool T;
    T.Clear();
    T.ChartNumber   = sc.ChartNumber;
    T.DrawingType   = DRAWING_TEXT;
    T.LineNumber    = PZ_LINE_STATUS;
    T.BeginIndex    = last;
    T.BeginValue    = sc.High[last];
    T.Color         = color;
    T.FontSize      = 8;
    T.Text          = text;
    T.TextAlignment = DT_RIGHT | DT_TOP;
    T.AddMethod     = UTAM_ADD_OR_ADJUST;
    T.AddAsUserDrawnDrawing = 0;
    sc.UseTool(T);
}

static void PZDrawZones(SCStudyInterfaceRef sc, PZState *S, int last, int left,
                        int showLabels, int labelSize, COLORREF labelColor,
                        int transparency, int levelTicks, COLORREF fallback,
                        double qqqR, double ndxB, double spyR, double spxB)
{
    if (last < 0)
        return;
    double tick = sc.TickSize;
    if (tick <= 0.0)
        tick = 0.25;
    if (levelTicks < 1)
        levelTicks = 1;
    const double band = tick * (double)levelTicks;

    for (int i = 0; i < S->Count; ++i)
    {
        const PZZone &z = S->Zones[i];
        double lo = PZToFutures(PZMinD(z.Low, z.High), z.Quote, qqqR, ndxB, spyR, spxB, tick);
        double hi = PZToFutures(PZMaxD(z.Low, z.High), z.Quote, qqqR, ndxB, spyR, spxB, tick);
        if (hi - lo < band)
        {
            const double mid = 0.5 * (lo + hi);
            lo = mid - 0.5 * band;
            hi = mid + 0.5 * band;
        }
        COLORREF col = z.Color ? (COLORREF)z.Color : fallback;

        s_UseTool R;
        R.Clear();
        R.ChartNumber          = sc.ChartNumber;
        R.DrawingType          = DRAWING_RECTANGLE_EXT_HIGHLIGHT;
        R.LineNumber           = PZ_BASE_RECT + i;
        R.BeginIndex           = left;
        R.EndIndex             = last;
        R.BeginValue           = hi;
        R.EndValue             = lo;
        R.Color                = col;
        R.SecondaryColor       = col;
        R.TransparencyLevel    = transparency;
        R.LineWidth            = 1;
        R.AddMethod            = UTAM_ADD_OR_ADJUST;
        R.AddAsUserDrawnDrawing = 0;
        sc.UseTool(R);

        if (showLabels)
        {
            char label[160];
            if (z.Note[0])
                snprintf(label, sizeof(label), "%s  %s", z.Name, z.Note);
            else
                snprintf(label, sizeof(label), "%s", z.Name);

            s_UseTool T;
            T.Clear();
            T.ChartNumber   = sc.ChartNumber;
            T.DrawingType   = DRAWING_TEXT;
            T.LineNumber    = PZ_BASE_TEXT + i;
            T.BeginIndex    = last;
            T.BeginValue    = hi;
            T.Color         = labelColor;
            T.FontSize      = labelSize;
            T.Text          = label;
            T.TextAlignment = DT_RIGHT | DT_VCENTER;
            T.AddMethod     = UTAM_ADD_OR_ADJUST;
            T.AddAsUserDrawnDrawing = 0;
            sc.UseTool(T);
        }
        else
            PZDeleteLine(sc, PZ_BASE_TEXT + i);
    }

    for (int i = S->Count; i < S->DrawnCount; ++i)
    {
        PZDeleteLine(sc, PZ_BASE_RECT + i);
        PZDeleteLine(sc, PZ_BASE_TEXT + i);
    }
    S->DrawnCount = S->Count;
}

SCSFExport scsf_PlanningZones(SCStudyInterfaceRef sc)
{
    SCInputRef In_Folder       = sc.Input[0];
    SCInputRef In_Symbol       = sc.Input[1];
    SCInputRef In_AutoSymbol   = sc.Input[2];
    SCInputRef In_ShowLabels   = sc.Input[3];
    SCInputRef In_LabelSize    = sc.Input[4];
    SCInputRef In_LabelColor   = sc.Input[5];
    SCInputRef In_Transparency = sc.Input[6];
    SCInputRef In_LeftAnchor   = sc.Input[7];
    SCInputRef In_ReloadMode   = sc.Input[8];
    SCInputRef In_PollSec      = sc.Input[9];
    SCInputRef In_ForceReload  = sc.Input[10];
    SCInputRef In_LevelTicks   = sc.Input[11];
    SCInputRef In_DefaultColor = sc.Input[12];
    SCInputRef In_QqqNq        = sc.Input[13];
    SCInputRef In_NdxNq        = sc.Input[14];
    SCInputRef In_SpyEs        = sc.Input[15];
    SCInputRef In_SpxEs        = sc.Input[16];

    if (sc.SetDefaults)
    {
        sc.GraphName      = "Planning Zones v1.2";
        sc.StudyDescription = "Loads json\\{Symbol}.json and draws extending zone rectangles plus labels. QQQ/SPY multiply by ratio; NDX/SPX add daily basis (NQ-NDX, ES-SPX). Excel is the human master; ACSIL reads JSON only.";
        sc.GraphRegion    = 0;
        sc.AutoLoop       = 0;
        sc.ScaleRangeType = SCALE_SAMEASREGION;
        sc.DrawZeros      = 0;
        sc.ValueFormat    = VALUEFORMAT_INHERITED;
        sc.UpdateAlways   = 1;

        sc.Subgraph[0].Name      = "Zones Loaded";
        sc.Subgraph[0].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[0].DrawZeros = 0;

        In_Folder.Name = "Zones Folder";
        In_Folder.SetString("C:\\Users\\flavi\\Documents\\SC_PlanningZones");

        In_Symbol.Name = "Symbol";
        In_Symbol.SetString("NQ");

        In_AutoSymbol.Name = "Auto Symbol From Chart";
        In_AutoSymbol.SetYesNo(0);

        In_ShowLabels.Name = "Show Labels";
        In_ShowLabels.SetYesNo(1);

        In_LabelSize.Name = "Label Font Size";
        In_LabelSize.SetInt(8);
        In_LabelSize.SetIntLimits(4, 24);

        In_LabelColor.Name = "Label Color";
        In_LabelColor.SetColor(RGB(230, 230, 230));

        In_Transparency.Name = "Fill Transparency (0-90)";
        In_Transparency.SetInt(70);
        In_Transparency.SetIntLimits(0, 90);

        In_LeftAnchor.Name = "Left Anchor";
        In_LeftAnchor.SetCustomInputStrings("Chart Start;Visible Left");
        In_LeftAnchor.SetCustomInputIndex(0);

        In_ReloadMode.Name = "Reload On";
        In_ReloadMode.SetCustomInputStrings("Bar Close;Timer;Both");
        In_ReloadMode.SetCustomInputIndex(2);

        In_PollSec.Name = "File Poll Interval (sec)";
        In_PollSec.SetInt(300);
        In_PollSec.SetIntLimits(5, 3600);

        In_ForceReload.Name = "Force Reload (toggle)";
        In_ForceReload.SetYesNo(0);

        In_LevelTicks.Name = "Level Band (ticks)";
        In_LevelTicks.SetInt(2);
        In_LevelTicks.SetIntLimits(1, 20);

        In_DefaultColor.Name = "Fallback Color";
        In_DefaultColor.SetColor(RGB(232, 163, 23));

        In_QqqNq.Name = "QQQ to NQ Ratio";
        In_QqqNq.SetFloat(41.75f);
        In_QqqNq.SetFloatLimits(0.0f, 100.0f);

        In_NdxNq.Name = "NDX to NQ Basis (NQ-NDX)";
        In_NdxNq.SetFloat(0.0f);
        In_NdxNq.SetFloatLimits(-200.0f, 200.0f);

        In_SpyEs.Name = "SPY to ES Ratio";
        In_SpyEs.SetFloat(10.0f);
        In_SpyEs.SetFloatLimits(0.0f, 100.0f);

        In_SpxEs.Name = "SPX to ES Basis (ES-SPX)";
        In_SpxEs.SetFloat(0.0f);
        In_SpxEs.SetFloatLimits(-200.0f, 200.0f);
        return;
    }

    PZState *S = (PZState *)sc.GetPersistentPointer(PZ_PTR_SLOT);

    if (sc.LastCallToFunction)
    {
        if (S)
        {
            PZDeleteAll(sc, S);
            delete S;
            sc.SetPersistentPointer(PZ_PTR_SLOT, 0);
        }
        return;
    }

    if (S == 0)
    {
        S = new PZState();
        memset(S, 0, sizeof(PZState));
        sc.SetPersistentPointer(PZ_PTR_SLOT, S);
    }

    const int last = sc.ArraySize - 1;
    if (last < 0)
        return;

    const char *folder = In_Folder.GetString();
    const int autoSym = In_AutoSymbol.GetYesNo();
    const int showLabels = In_ShowLabels.GetYesNo();
    const int labelSize = In_LabelSize.GetInt();
    const COLORREF labelColor = In_LabelColor.GetColor();
    int transparency = In_Transparency.GetInt();
    const int leftMode = In_LeftAnchor.GetIndex();
    const int reloadMode = In_ReloadMode.GetIndex();
    const int pollSec = In_PollSec.GetInt();
    const int reloadTog = In_ForceReload.GetYesNo();
    const int levelTicks = In_LevelTicks.GetInt();
    const COLORREF fallback = In_DefaultColor.GetColor();
    const double qqqR = In_QqqNq.GetFloat();
    const double ndxB = In_NdxNq.GetFloat();
    const double spyR = In_SpyEs.GetFloat();
    const double spxB = In_SpxEs.GetFloat();

    char symbol[PZ_SYM_LEN];
    symbol[0] = 0;
    if (autoSym)
        PZFromChartSymbol(sc.Symbol.GetChars(), symbol, PZ_SYM_LEN);
    if (symbol[0] == 0)
        PZSanitizeSymbol(In_Symbol.GetString(), symbol, PZ_SYM_LEN);
    if (symbol[0] == 0)
        PZCopyTrunc(symbol, PZ_SYM_LEN, "NQ");

    char path[PZ_PATH_LEN];
    PZJoinPath(path, PZ_PATH_LEN, folder, symbol);

    const bool hidden = (sc.HideStudy != 0);
    if (hidden)
    {
        if (S->LastHide == 0)
            PZDeleteAll(sc, S);
        S->LastHide = 1;
        return;
    }
    if (S->LastHide)
    {
        S->LastHide = 0;
        S->Checksum = 0;
        S->FileOk = 0;
    }

    const bool newBar = (sc.ArraySize != S->LastArraySize);
    const bool fullRecalc = (sc.IsFullRecalculation != 0 && sc.UpdateStartIndex == 0);
    const bool pathChanged = (strcmp(S->Path, path) != 0 || strcmp(S->Symbol, symbol) != 0);
    const bool togChanged = (reloadTog != S->LastReloadTog);
    S->LastReloadTog = reloadTog;
    S->LastArraySize = sc.ArraySize;

    const double nowD = sc.CurrentSystemDateTime.GetAsDouble();
    const bool timerMode = (reloadMode == 1 || reloadMode == 2);
    const bool barMode = (reloadMode == 0 || reloadMode == 2);
    bool timeDue = false;
    if (timerMode && (nowD - S->LastPoll) * 86400.0 >= (double)pollSec)
        timeDue = true;

    bool needLoad = fullRecalc || pathChanged || togChanged || S->FileOk == 0;
    if (!needLoad && barMode && newBar)
        needLoad = true;
    if (!needLoad && timeDue)
        needLoad = true;

    if (needLoad)
    {
        S->LastPoll = nowD;
        char buf[PZ_MAX_FILE];
        int nbytes = 0;
        int opened = PZReadFile(path, buf, PZ_MAX_FILE, &nbytes);
        if (!opened)
        {
            char alt[PZ_PATH_LEN];
            int n = (int)strlen(folder);
            while (n > 0 && (folder[n - 1] == '\\' || folder[n - 1] == '/'))
                --n;
            snprintf(alt, PZ_PATH_LEN, "%.*s\\%s.json", n, folder, symbol);
            opened = PZReadFile(alt, buf, PZ_MAX_FILE, &nbytes);
            if (opened)
                PZCopyTrunc(path, PZ_PATH_LEN, alt);
        }

        unsigned cs = opened ? PZChecksum(buf, nbytes) : 0;
        const bool changed = (!opened) || pathChanged || togChanged || fullRecalc
            || cs != S->Checksum || nbytes != (int)S->FileSize;

        PZCopyTrunc(S->Path, PZ_PATH_LEN, path);
        PZCopyTrunc(S->Symbol, PZ_SYM_LEN, symbol);

        if (!opened)
        {
            PZDeleteAll(sc, S);
            S->Count = 0;
            S->FileOk = 2;
            S->Checksum = 0;
            S->FileSize = -1;
            sc.Subgraph[0][last] = 0.f;
            SCString msg;
            msg.Format("PlanningZones: cannot open %s", path);
            sc.AddMessageToLog(msg, 0);
            PZDrawStatus(sc, last, msg.GetChars(), RGB(220, 60, 60));
            return;
        }

        if (changed)
        {
            char fileSym[PZ_SYM_LEN];
            fileSym[0] = 0;
            PZExtractString(buf, "symbol", fileSym, PZ_SYM_LEN);
            if (fileSym[0] == 0)
                PZCopyTrunc(fileSym, PZ_SYM_LEN, symbol);
            S->Count = PZParseZones(buf, S->Zones, PZ_MAX_ZONES, PZQuoteFromName(fileSym));
            S->Checksum = cs;
            S->FileSize = nbytes;
            S->FileOk = 1;
            SCString msg;
            msg.Format("PlanningZones: loaded %d zones from %s", S->Count, path);
            sc.AddMessageToLog(msg, 0);
        }
        else
            S->FileOk = 1;
    }

    int left = 0;
    if (leftMode == 1)
    {
        left = sc.IndexOfFirstVisibleBar;
        if (left < 0)
            left = 0;
        if (left > last)
            left = last;
    }

    const int visLeft = (leftMode == 1) ? left : 0;
    const int drawFp = showLabels
        ^ (labelSize << 1)
        ^ ((int)labelColor)
        ^ (transparency << 8)
        ^ (leftMode << 16)
        ^ (levelTicks << 20);
    const bool visChanged = (leftMode == 1 && visLeft != S->LastVisLeft);
    const bool ratioChanged = (S->LastQqqR != (float)qqqR
        || S->LastNdxR != (float)ndxB
        || S->LastSpyR != (float)spyR
        || S->LastSpxR != (float)spxB);
    const bool drawChanged = (drawFp != S->DrawFp) || ratioChanged;
    S->LastVisLeft = visLeft;
    S->DrawFp = drawFp;
    S->LastQqqR = (float)qqqR;
    S->LastNdxR = (float)ndxB;
    S->LastSpyR = (float)spyR;
    S->LastSpxR = (float)spxB;

    sc.Subgraph[0][last] = (float)S->Count;

    if (!(needLoad || newBar || fullRecalc || visChanged || drawChanged))
        return;

    PZDrawZones(sc, S, last, left, showLabels, labelSize, labelColor,
                transparency, levelTicks, fallback, qqqR, ndxB, spyR, spxB);

    SCString st;
    st.Format("PZ %s  %d zones", symbol, S->Count);
    PZDrawStatus(sc, last, st.GetChars(), RGB(180, 180, 180));
}
