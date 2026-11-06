// Modern self-drawn panel (OpenGL), modelled on how other X-Plane 12 plugins
// draw their windows (LockYourTexture / AviTab).
//
// The important discovery: the GL bridge does NOT set up a 2-D projection for
// plugin windows.  A plugin has to do it itself -
//
//     glViewport(window rect)
//     glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
//     glOrtho(window left/top/right/bottom)
//     glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
//
// Without that every vertex lands outside the default clip volume and nothing
// is visible (which is exactly what happened for a long time here).
#include "panel.h"
#include "gfx.h"
#include "font.h"
#include "vram_manager.h"

#include <windows.h>   // GetAsyncKeyState - polling drag fallback

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// palette (flat dark UI, same family as other XP12 plugin windows)
// ---------------------------------------------------------------------------
static const Rgba C_BG      = rgba(0.086f, 0.094f, 0.110f, 0.97f);
static const Rgba C_TITLE   = rgba(0.110f, 0.122f, 0.145f, 1.00f);
static const Rgba C_PANEL   = rgba(0.129f, 0.141f, 0.169f, 1.00f);
static const Rgba C_PANELHI = rgba(0.165f, 0.180f, 0.212f, 1.00f);
static const Rgba C_BTN     = rgba(0.169f, 0.196f, 0.243f, 1.00f);
static const Rgba C_BTNHI   = rgba(0.216f, 0.247f, 0.302f, 1.00f);
static const Rgba C_LINE    = rgba(1.000f, 1.000f, 1.000f, 0.090f);
static const Rgba C_TEXT    = rgba(0.910f, 0.925f, 0.945f, 1.00f);
static const Rgba C_SUB     = rgba(0.647f, 0.686f, 0.749f, 1.00f);
static const Rgba C_DIM     = rgba(0.443f, 0.478f, 0.541f, 1.00f);
static const Rgba C_ACCENT  = rgba(0.235f, 0.478f, 0.882f, 1.00f);
static const Rgba C_ACCHI   = rgba(0.325f, 0.565f, 0.945f, 1.00f);
static const Rgba C_TRACK   = rgba(0.196f, 0.216f, 0.251f, 1.00f);
static const Rgba C_GREEN   = rgba(0.239f, 0.827f, 0.549f, 1.00f);
static const Rgba C_AMBER   = rgba(0.949f, 0.663f, 0.180f, 1.00f);
static const Rgba C_RED     = rgba(0.949f, 0.376f, 0.376f, 1.00f);

// X-Plane reports mouse coordinates in "boxels", which are scaled relative to
// the units used by window geometry (a 1.5x UI scale gives coordinates 1/1.5 of
// the real pixel position).  Reading the OS cursor directly keeps the input and
// the drawing in the same space.
static void CursorPos(int* gx, int* gy) {
    POINT pt = { 0, 0 };
    GetCursorPos(&pt);
    int sw = 0, sh = 0;
    XPLMGetScreenSize(&sw, &sh);
    if (sh <= 0) sh = 1080;
    *gx = pt.x;
    *gy = sh - pt.y;      // GL style: y grows upward
}
// ---------------------------------------------------------------------------
// layout (logical units, top-left origin)
// ---------------------------------------------------------------------------
static const float W = 620.0f;
static const float H = 564.0f;
static const float PAD = 22.0f;
static const float TITLE_H = 34.0f;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

static bool PointIn(const Rect& r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

enum WidgetId {
    W_NONE = 0, W_TITLE, W_CLOSE, W_AUTO, W_AUTOPURGE,
    W_SL_TARGET, W_SL_MIN, W_SL_BUDGET, W_SL_PAGING, W_CLOUDTRIM, W_PURGE, W_RESET, W_SAVE, W_HIDE
};

struct Layout {
    Rect title, close, dot;
    Rect autoRow, autoPurgeRow, cloudRow;
    Rect slTarget, slMin, slBudget, slPaging;
    Rect btnPurge, btnReset, btnSave, btnHide;
};

static XPLMWindowID sWin = nullptr;
static bool   sVisible     = false;
static bool   sFontsOk     = false;
static bool   sFontsDue    = true;
static bool   sLoggedFrame = false;
static bool   sPhaseSeen   = false;
static Layout sLay;
static int    sHot = W_NONE, sActive = W_NONE, sActiveSlider = W_NONE;
// Slider values are applied at most every 350 ms while dragging: writing the
// texture-budget / paging datarefs makes X-Plane rebuild its texture pools, and
// doing that on every mouse-move froze the sim.
static bool   sPendingBudget   = false;
static bool   sPendingPaging   = false;
static double sLastSliderApply = 0.0;
static bool   sDragging = false;
static int    sDragDX = 0, sDragDY = 0;
static float  sScale = 1.0f;

static Font sFontSmall, sFontBody, sFontTitle, sFontBig;

struct Xf { float left, top, s; };
static Xf sXf;

// ---------------------------------------------------------------------------
static float PanelScale() {
    if (gSet.uiScale > 0.01f) return gSet.uiScale;
    int sw = 0, sh = 0;
    XPLMGetScreenSize(&sw, &sh);
    if (sh <= 0) sh = 1080;
    float s = (float)sh / 1440.0f * 1.15f;
    if (s < 0.95f) s = 0.95f;
    if (s > 1.70f) s = 1.70f;
    return s;
}

static void FreeFonts() {
    sFontSmall.tex = 0; sFontBody.tex = 0; sFontTitle.tex = 0; sFontBig.tex = 0;
    sFontsOk = false;
    sFontsDue = true;
}

static bool BuildFonts() {
    const wchar_t* face = L"Microsoft YaHei UI";
    int sSm = (int)lroundf(12.0f * sScale);
    int sBd = (int)lroundf(15.0f * sScale);
    int sTt = (int)lroundf(19.0f * sScale);
    int sBg = (int)lroundf(30.0f * sScale);
    if (sSm < 11) sSm = 11;
    bool ok = Font_Init(sFontSmall, sSm, 512, 512, face);
    ok = ok && Font_Init(sFontBody,  sBd, 512, 512, face);
    ok = ok && Font_Init(sFontTitle, sTt, 512, 512, face);
    ok = ok && Font_Init(sFontBig,   sBg, 512, 512, face);
    if (ok) {
        Font_Prewarm(sFontSmall); Font_Prewarm(sFontBody);
        Font_Prewarm(sFontTitle); Font_Prewarm(sFontBig);
    }
    if (ok && sFontBody.tex == 0) ok = false;
    sFontsOk = ok;
    sFontsDue = false;
    Log("VRAM: fonts %s (scale %.2f tex=%u sizes %d/%d/%d/%d)",
        ok ? "ready" : "FAILED", sScale, (unsigned)sFontBody.tex, sSm, sBd, sTt, sBg);
    return ok;
}

static void EnsureFonts() { if (!sFontsOk || sFontsDue) BuildFonts(); }

// ---------------------------------------------------------------------------
static void MapPt(float lx, float ly, float* gx, float* gy) {
    *gx = sXf.left + lx * sXf.s;
    *gy = sXf.top - ly * sXf.s;
}

static void RectGL(const Rect& r, float* ox, float* oy, float* ow, float* oh) {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    MapPt(r.x, r.y, &x0, &y0);
    MapPt(r.x + r.w, r.y + r.h, &x1, &y1);
    *ox = (x0 < x1) ? x0 : x1;
    *oy = (y0 < y1) ? y0 : y1;
    *ow = fabsf(x1 - x0);
    *oh = fabsf(y1 - y0);
}

static void FillRect(const Rect& r, Rgba c, float radius = 0.0f) {
    float x, y, w, h;
    RectGL(r, &x, &y, &w, &h);
    if (radius > 0.0f) Gfx_RoundRect(x, y, w, h, radius * sXf.s, c);
    else               Gfx_Rect(x, y, w, h, c);
}

static void TextAt(Font& f, float x, float baseline, const char* s, Rgba c) {
    float gx, gy;
    MapPt(x, baseline, &gx, &gy);
    Font_DrawText(f, gx, gy, s, c);
}

static float TextW(Font& f, const char* s) { return Font_TextWidth(f, s) / sXf.s; }

static void TextRight(Font& f, float rightX, float baseline, const char* s, Rgba c) {
    TextAt(f, rightX - TextW(f, s), baseline, s, c);
}

static float BaselineIn(const Font& f, const Rect& r) {
    return r.y + r.h * 0.5f + (float)f.ascent / (2.0f * sXf.s);
}

// ---------------------------------------------------------------------------
static void StatusLine(char* out, int n) {
    switch (gRt.statusId) {
        case ST_REDUCE:  snprintf(out, n, "Reducing render distance to %d%%", (int)gRt.statusArg); break;
        case ST_RECOVER: snprintf(out, n, "Restoring render distance to %d%%", (int)gRt.statusArg); break;
        case ST_HOLD:    snprintf(out, n, "Holding at %d%%", (int)gRt.statusArg); break;
        case ST_OFF:     snprintf(out, n, "%s", "Auto control is off"); break;
        case ST_YIELD:   snprintf(out, n, "%s", "Waiting for scenery to settle"); break;
        case ST_EXT:     snprintf(out, n, "%s", "Render distance changed elsewhere - backing off"); break;
        case ST_UNAVAIL: snprintf(out, n, "%s", "No VRAM data"); break;
        case ST_PURGE_LOD:
        case ST_PURGE_TEX: snprintf(out, n, "%s", "Purging (render distance only)"); break;
        case ST_LOADING: snprintf(out, n, "%s", "Loading scenery - paused"); break;
        case ST_SIMLOCK: snprintf(out, n, "%s", "X-Plane overrides LOD writes - monitor only"); break;
        case ST_WAITREF: snprintf(out, n, "%s", "Waiting for X-Plane to register the dataref..."); break;
        default:         snprintf(out, n, "%s", "Idle"); break;
    }
}

// ---------------------------------------------------------------------------
static void ComputeLayout() {
    sLay.title = Rect{ 0, 0, W, TITLE_H };
    sLay.dot   = Rect{ 14, 12, 10, 10 };
    sLay.close = Rect{ W - 34, 6, 22, 22 };

    float colW = (W - PAD * 2 - 14) * 0.5f;
    sLay.autoRow      = Rect{ PAD, 232, colW, 34 };
    sLay.autoPurgeRow = Rect{ PAD + colW + 14, 232, colW, 34 };

    sLay.cloudRow = Rect{ PAD, 274, W - PAD * 2, 34 };
    sLay.slTarget = Rect{ PAD, 318, W - PAD * 2, 42 };
    sLay.slMin    = Rect{ PAD, 362, W - PAD * 2, 42 };
    sLay.slBudget = Rect{ PAD, 406, W - PAD * 2, 42 };
    sLay.slPaging = Rect{ PAD, 450, W - PAD * 2, 42 };

    float by = 496, bh = 40;
    sLay.btnPurge = Rect{ PAD,        by, 150, bh };
    sLay.btnReset = Rect{ PAD + 162,  by, 132, bh };
    sLay.btnSave  = Rect{ PAD + 306,  by, 118, bh };
    sLay.btnHide  = Rect{ PAD + 436,  by, 118, bh };
}

static int HitTest(float lx, float ly) {
    if (PointIn(sLay.close, lx, ly))        return W_CLOSE;
    if (PointIn(sLay.btnPurge, lx, ly))     return W_PURGE;
    if (PointIn(sLay.btnReset, lx, ly))     return W_RESET;
    if (PointIn(sLay.btnSave, lx, ly))      return W_SAVE;
    if (PointIn(sLay.btnHide, lx, ly))      return W_HIDE;
    if (PointIn(sLay.autoRow, lx, ly))      return W_AUTO;
    if (PointIn(sLay.autoPurgeRow, lx, ly)) return W_AUTOPURGE;
    if (PointIn(sLay.cloudRow, lx, ly))     return W_CLOUDTRIM;
    { Rect t = sLay.slTarget; t.y -= 4; t.h += 8; if (PointIn(t, lx, ly)) return W_SL_TARGET; }
    { Rect m = sLay.slMin;    m.y -= 4; m.h += 8; if (PointIn(m, lx, ly)) return W_SL_MIN; }
    { Rect p = sLay.slBudget; p.y -= 4; p.h += 8; if (PointIn(p, lx, ly)) return W_SL_BUDGET; }
    { Rect q = sLay.slPaging; q.y -= 4; q.h += 8; if (PointIn(q, lx, ly)) return W_SL_PAGING; }
    if (PointIn(sLay.title, lx, ly))        return W_TITLE;
    return W_NONE;
}

// ---------------------------------------------------------------------------
static void SetUpGL(int l, int t, int r, int b) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glViewport(l, b, (r - l) > 0 ? (r - l) : 1, (t - b) > 0 ? (t - b) : 1);
    glOrtho((double)l, (double)r, (double)b, (double)t, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glScissor(l, b, (r - l) > 0 ? (r - l) : 1, (t - b) > 0 ? (t - b) : 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void RestoreGL() {
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}

// ---------------------------------------------------------------------------
static void DrawToggle(const Rect& row, const char* label, bool on, bool hover) {
    FillRect(row, hover ? C_PANELHI : C_PANEL, 6.0f);
    TextAt(sFontBody, row.x + 12.0f, BaselineIn(sFontBody, row), label, C_TEXT);
    Rect sw{ row.x + row.w - 56.0f, row.y + (row.h - 22.0f) * 0.5f, 44.0f, 22.0f };
    float sx, sy, swp, shp;
    RectGL(sw, &sx, &sy, &swp, &shp);
    Gfx_RoundRect(sx, sy, swp, shp, shp * 0.5f, on ? C_ACCENT : C_TRACK);
    float kr = shp * 0.5f - 3.0f * sXf.s;
    float kx = on ? (sx + swp - kr - 3.0f * sXf.s) : (sx + kr + 3.0f * sXf.s);
    Gfx_Circle(kx, sy + shp * 0.5f, kr, rgba(1, 1, 1, 1));
}

static void DrawSlider(const Rect& row, const char* label, float v01,
                       const char* value, bool hover) {
    TextAt(sFontBody, row.x, row.y + 22.0f, label, C_SUB);
    TextRight(sFontBody, row.x + row.w, row.y + 22.0f, value, C_TEXT);
    Rect track{ row.x, row.y + 30.0f, row.w, 10.0f };
    float tx, ty, tw, th;
    RectGL(track, &tx, &ty, &tw, &th);
    Gfx_RoundRect(tx, ty, tw, th, th * 0.5f, C_TRACK);
    float frac = v01 < 0 ? 0 : (v01 > 1 ? 1 : v01);
    float px = tx + tw * frac;
    if (px - tx > th) Gfx_RoundRect(tx, ty, px - tx, th, th * 0.5f, C_ACCENT);
    float kr = (hover ? 10.0f : 9.0f) * sXf.s;
    Gfx_Circle(px, ty + th * 0.5f, kr + 2.0f * sXf.s, rgba(0.235f, 0.478f, 0.882f, 0.30f));
    Gfx_Circle(px, ty + th * 0.5f, kr, rgba(1, 1, 1, 1));
}

static void DrawButton(const Rect& r, const char* label, bool primary, bool hover, bool pressed) {
    Rgba fill = primary ? (hover ? C_ACCHI : C_ACCENT) : (hover ? C_BTNHI : C_BTN);
    if (pressed) fill = rgba(fill.r * 0.82f, fill.g * 0.82f, fill.b * 0.82f, fill.a);
    FillRect(r, fill, 6.0f);
    float bw = TextW(sFontBody, label);
    TextAt(sFontBody, r.x + (r.w - bw) * 0.5f, BaselineIn(sFontBody, r), label,
           primary ? rgba(1, 1, 1, 1) : C_TEXT);
}

// ---------------------------------------------------------------------------
static void DrawPanel() {
    float gx, gy, gw, gh;
    Rect full{ 0, 0, W, H };
    RectGL(full, &gx, &gy, &gw, &gh);
    Gfx_RoundRect(gx, gy, gw, gh, 6.0f * sXf.s, C_BG);

    {
        float x, y, w, h;
        RectGL(Rect{ 0, 0, W, TITLE_H }, &x, &y, &w, &h);
        Gfx_Rect(x, y, w, h, C_TITLE);

        float dx, dy, dw, dh;
        RectGL(Rect{ 14, 12, 10, 10 }, &dx, &dy, &dw, &dh);
        Gfx_Circle(dx + dw * 0.5f, dy + dh * 0.5f, dw * 0.5f, C_RED);

        TextAt(sFontBody, 34.0f, 23.0f, "VRAM Manager", C_TEXT);
        TextRight(sFontSmall, W - 44.0f, 22.0f, "v" VRAMMGR_VERSION, C_DIM);

        bool hov = (sHot == W_CLOSE);
        if (hov) FillRect(sLay.close, rgba(1, 1, 1, 0.12f), 4.0f);
        float cx, cy;
        MapPt(sLay.close.x + 11.0f, sLay.close.y + 11.0f, &cx, &cy);
        Gfx_XMark(cx, cy, 5.5f * sXf.s, 2.0f * sXf.s, hov ? C_TEXT : C_SUB);
    }
    {
        float x, y, w, h;
        RectGL(Rect{ 0, TITLE_H, W, 1.0f }, &x, &y, &w, &h);
        Gfx_Rect(x, y, w, h, C_LINE);
    }

    char buf[256];
    float pct = gRt.vram.valid ? gRt.vram.pct : 0.0f;
    double usedGB = (double)gRt.vram.processBytes / 1073741824.0;
    double totGB  = (double)gRt.vram.vramBytes / 1073741824.0;

    if (gRt.vram.valid) {
        snprintf(buf, sizeof(buf), "%.2f GB", usedGB);
        TextAt(sFontBig, PAD, TITLE_H + 52.0f, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "%.0f%%", pct);
        TextRight(sFontBig, W - PAD, TITLE_H + 52.0f, buf,
                  pct >= 92.0f ? C_RED : (pct >= 80.0f ? C_AMBER : C_GREEN));
    } else {
        TextAt(sFontBig, PAD, TITLE_H + 52.0f, "-- GB", C_DIM);
    }

    {
        Rect track{ PAD, TITLE_H + 64.0f, W - PAD * 2, 12.0f };
        float tx, ty, tw, th;
        RectGL(track, &tx, &ty, &tw, &th);
        Gfx_RoundRect(tx, ty, tw, th, th * 0.5f, C_TRACK);
        float frac = pct / 100.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (gRt.vram.valid && frac > 0.0f) {
            Rgba c = pct >= 92.0f ? C_RED : (pct >= 80.0f ? C_AMBER : C_GREEN);
            float fw = tw * frac;
            if (fw < th) fw = th;
            Gfx_RoundRect(tx, ty, fw, th, th * 0.5f, c);
        }
    }

    if (gRt.vram.valid) {
        const char* src = "";
        if (gRt.vram.source == 3) src = "  (whole GPU)";
        else if (gRt.vram.source == 2) src = "  (dxgi)";
        snprintf(buf, sizeof(buf), "of %.2f GB dedicated VRAM%s", totGB, src);
    } else {
        snprintf(buf, sizeof(buf), "%s", "no VRAM data - see Log.txt");
    }
    TextAt(sFontSmall, PAD, TITLE_H + 96.0f, buf, C_SUB);

    {
        // Everything is expressed relative to X-Plane's own value, which is
        // shown as 100% - the raw LOD bias number means nothing to a user.
        int lodPct = (gRt.lodBase > 0.01f) ? (int)lroundf(gRt.lodNow / gRt.lodBase * 100.0f) : 100;
        if (lodPct < 0) lodPct = 0;
        if (lodPct > 999) lodPct = 999;
        const char* lim = (gRt.limiter == 1) ? "   [VRAM limited]"
                        : (gRt.limiter == 2) ? "   [FPS limited]"
                        : (gRt.limiter == 3) ? "   [textures]" : "";
        snprintf(buf, sizeof(buf), "Render distance %d%%      Texture level %d      FPS %.0f%s",
                 lodPct, gRt.texNow, (double)gRt.fps, lim);
    }
    TextAt(sFontBody, PAD, TITLE_H + 128.0f, buf, C_TEXT);

    {
        char hint[160];
        double now = XPLMGetElapsedTime();
        bool fresh = (gRt.purgeDoneSec > 0.0) && (now - gRt.purgeDoneSec < 45.0);
        if (gRt.texDropped) {
            snprintf(hint, sizeof(hint),
                     "X-Plane lowered textures to %d - trimming VRAM to help it recover", gRt.texNow);
            TextAt(sFontSmall, PAD, TITLE_H + 148.0f, hint, C_AMBER);
        } else if (fresh) {
            double freed = gRt.vramBeforeGB - gRt.vramAfterGB;
            double dpct  = (gRt.fpsBefore > 1.0f)
                         ? ((double)gRt.fpsAfter / (double)gRt.fpsBefore - 1.0) * 100.0 : 0.0;
            snprintf(hint, sizeof(hint),
                     "Last purge: VRAM %.2f -> %.2f GB (%+.2f GB), FPS %.0f -> %.0f (%+.0f%%)",
                     gRt.vramBeforeGB, gRt.vramAfterGB, -freed,
                     (double)gRt.fpsBefore, (double)gRt.fpsAfter, -dpct);
            TextAt(sFontSmall, PAD, TITLE_H + 148.0f, hint, freed > 0.02 ? C_GREEN : C_SUB);
        } else if (gRt.fpsFull > 1.0f && gRt.fpsReduced > 1.0f) {
            double gain = ((double)gRt.fpsReduced / (double)gRt.fpsFull - 1.0) * 100.0;
            snprintf(hint, sizeof(hint),
                     "Measured: FPS %.0f at 100%% vs %.0f reduced (%+.0f%%)",
                     (double)gRt.fpsFull, (double)gRt.fpsReduced, gain);
            TextAt(sFontSmall, PAD, TITLE_H + 148.0f, hint, gain > 1.0 ? C_GREEN : C_SUB);
        } else if (gSet.targetPct > 84.0f) {
            snprintf(hint, sizeof(hint),
                     "Target %.0f%% is high - X-Plane may thrash textures (78-80%% is safer)",
                     (double)gSet.targetPct);
            TextAt(sFontSmall, PAD, TITLE_H + 148.0f, hint, C_AMBER);
        } else if (gRt.lodBase > 0.01f) {
            snprintf(hint, sizeof(hint),
                     "100%% = X-Plane default (%.3f), now %.3f", (double)gRt.lodBase, (double)gRt.lodNow);
            TextAt(sFontSmall, PAD, TITLE_H + 148.0f, hint, C_DIM);
        }
    }

    DrawToggle(sLay.autoRow, "Auto manage render distance", gSet.autoManage != 0,
               sHot == W_AUTO || sActive == W_AUTO);
    DrawToggle(sLay.autoPurgeRow, "Auto rescue purge", gSet.autoPurge != 0,
               sHot == W_AUTOPURGE || sActive == W_AUTOPURGE);

    {
        static const char* kLevels[3] = { "Off", "Light", "Max" };
        char label[160];
        int mode = gSet.cloudTrim < 0 ? 0 : (gSet.cloudTrim > 3 ? 3 : gSet.cloudTrim);
        if (mode == 3) {
            if (gRt.cloudTrimmed > 0)
                snprintf(label, sizeof(label), "Cloud trim: Auto (now %s, %d knobs, follows VRAM)",
                         kLevels[gRt.cloudAuto], gRt.cloudTrimmed);
            else
                snprintf(label, sizeof(label), "Cloud trim: Auto (idle - VRAM is fine)");
        } else if (gRt.cloudTrimmed > 0) {
            snprintf(label, sizeof(label), "Cloud trim: %s  (%d knobs active)",
                     kLevels[mode], gRt.cloudTrimmed);
        } else {
            snprintf(label, sizeof(label), "Cloud trim: %s  (click: Off/Light/Max/Auto)", kLevels[mode]);
        }
        bool hov = (sHot == W_CLOUDTRIM || sActive == W_CLOUDTRIM);
        FillRect(sLay.cloudRow, hov ? C_PANELHI : C_PANEL, 6.0f);
        TextAt(sFontBody, sLay.cloudRow.x + 12.0f, BaselineIn(sFontBody, sLay.cloudRow), label, C_TEXT);
        char badge[24];
        if (mode == 3) snprintf(badge, sizeof(badge), "AUTO");
        else           snprintf(badge, sizeof(badge), "%s", kLevels[mode]);
        TextAt(sFontBody, sLay.cloudRow.x + sLay.cloudRow.w - 74.0f,
               BaselineIn(sFontBody, sLay.cloudRow), badge,
               (mode == 3 ? gRt.cloudTrimmed > 0 : mode > 0) ? C_GREEN : C_DIM);
    }

    {
        char v[32];
        snprintf(v, sizeof(v), "%d%%", (int)lroundf(gSet.targetPct));
        DrawSlider(sLay.slTarget, "Target VRAM  (purge above this)",
                   (gSet.targetPct - 50.0f) / (98.0f - 50.0f), v, sHot == W_SL_TARGET);
        snprintf(v, sizeof(v), "%d%%", (int)lroundf(gSet.minFactor * 100.0f));
        DrawSlider(sLay.slMin, "Minimum render distance",
                   (gSet.minFactor * 100.0f - 30.0f) / (100.0f - 30.0f), v, sHot == W_SL_MIN);
        if (gSet.texBudget <= 1.001f) snprintf(v, sizeof(v), "default");
        else                          snprintf(v, sizeof(v), "x%.2f", (double)gSet.texBudget);
        DrawSlider(sLay.slBudget, "Texture budget  (higher = sharper far textures, more VRAM)",
                   (gSet.texBudget - 1.0f) / 3.0f, v, sHot == W_SL_BUDGET || sActive == W_SL_BUDGET);
        if (gSet.pagingM <= 0.0f) snprintf(v, sizeof(v), "default");
        else                      snprintf(v, sizeof(v), "%.1f km", (double)gSet.pagingM / 1000.0);
        DrawSlider(sLay.slPaging, "Texture paging distance  (smaller = less VRAM)",
                   (gSet.pagingM > 0.0f) ? (gSet.pagingM - 1500.0f) / 8500.0f : 1.0f, v,
                   sHot == W_SL_PAGING || sActive == W_SL_PAGING);
    }

    DrawButton(sLay.btnPurge, "Purge now", true,  sHot == W_PURGE, sActive == W_PURGE);
    DrawButton(sLay.btnReset, "Restore",   false, sHot == W_RESET, sActive == W_RESET);
    DrawButton(sLay.btnSave,  "Save",      false, sHot == W_SAVE,  sActive == W_SAVE);
    DrawButton(sLay.btnHide,  "Hide",      false, sHot == W_HIDE,  sActive == W_HIDE);

    StatusLine(buf, sizeof(buf));
    TextAt(sFontSmall, PAD, H - 14.0f, buf, C_SUB);

}

// ---------------------------------------------------------------------------
// Frame cache
// ---------------------------------------------------------------------------
static GLuint   sCacheTex    = 0;
static int      sCacheW = 0, sCacheH = 0;
static bool     sCacheOk     = true;
static bool     sForceRedraw = true;
static uint64_t sCacheSig    = 0;
static bool     sCacheAllocated = false;

// Anything the panel shows goes into this signature; when it changes the panel
// is re-rendered and re-cached.
static uint64_t PanelSignature() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](long long v) {
        h ^= (uint64_t)v; h *= 1099511628211ull;
    };
    mix((long long)gRt.vram.valid);
    mix((long long)(gRt.vram.pct * 10.0f));
    mix((long long)(gRt.lodNow * 1000.0f));
    mix((long long)gRt.lodBase * 100);
    mix(gRt.texNow);
    mix((long long)gRt.fps);
    mix(gRt.statusId);
    mix((long long)gRt.statusArg);
    mix(gRt.limiter);
    mix(gRt.texDropped ? 1 : 0);
    mix(gRt.cloudTrimmed);
    mix(gRt.cloudAuto);
    mix((long long)(gRt.texBudget * 100.0f));
    mix(gSet.autoManage);
    mix(gSet.autoPurge);
    mix(gSet.cloudTrim);
    mix((long long)(gSet.targetPct * 10.0f));
    mix((long long)(gSet.minFactor * 1000.0f));
    mix((long long)gSet.pagingM);
    mix((long long)(gSet.texBudget * 100.0f));
    mix(sHot);
    mix(sActive);
    mix((long long)(gRt.vramBeforeGB * 100.0));
    mix((long long)(gRt.vramAfterGB * 100.0));
    mix((long long)(gRt.fpsBefore * 10.0f));
    mix((long long)(gRt.fpsAfter * 10.0f));
    mix((long long)(gRt.fpsFull * 10.0f));
    mix((long long)(gRt.fpsReduced * 10.0f));
    return h;
}
// ---------------------------------------------------------------------------
static void DrawContent(bool fromPhase) {
    if (!sWin || !sVisible) return;

    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(sWin, &l, &t, &r, &b);
    sXf.left = (float)l;
    sXf.top  = (float)t;
    sXf.s    = sScale;

    EnsureFonts();

    int mx = 0, my = 0;
    CursorPos(&mx, &my);
    sHot = HitTest(((float)mx - (float)l) / sScale, ((float)t - (float)my) / sScale);

    SetUpGL(l, t, r, b);

    // ---- frame cache --------------------------------------------------
    // X-Plane clears the window every frame, so a plugin normally redraws all
    // of it (~2400 vertices through the GL bridge).  The rendered panel is
    // copied into a texture and unchanged frames just draw that one quad.
    int w = r - l, h = t - b;
    uint64_t sig = PanelSignature();
    bool geomChanged = (w != sCacheW || h != sCacheH);
    if (geomChanged) {
        if (sCacheTex) glDeleteTextures(1, &sCacheTex);
        sCacheTex = 0;
        sCacheW = w; sCacheH = h; sCacheAllocated = false;
    }
    bool needDraw = !sCacheOk || sCacheTex == 0 || sig != sCacheSig ||
                    sForceRedraw || geomChanged;

    if (needDraw) {
        if (sFontsOk) DrawPanel();
        Font_FlushAll();
        Gfx_Flush();

        if (sCacheOk) {
            if (!sCacheTex) {
                glGenTextures(1, &sCacheTex);
                if (sCacheTex) {
                    glBindTexture(GL_TEXTURE_2D, sCacheTex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
                }
            }
            if (sCacheTex) {
                glBindTexture(GL_TEXTURE_2D, sCacheTex);
                if (sCacheAllocated) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, l, b, w, h);
                else { glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, l, b, w, h, 0); sCacheAllocated = true; }
                if (glGetError() != GL_NO_ERROR) {
                    sCacheOk = false;
                    Log("VRAM: panel frame cache unavailable - drawing directly");
                } else {
                    sCacheSig = sig;
                }
            } else {
                sCacheOk = false;
            }
        }
        sForceRedraw = false;
    } else {
        // one textured quad - the whole frame
        XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, sCacheTex);
        glDisable(GL_BLEND);
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2i(l, b);
        glTexCoord2f(1, 0); glVertex2i(r, b);
        glTexCoord2f(1, 1); glVertex2i(r, t);
        glTexCoord2f(0, 1); glVertex2i(l, t);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
    }

    RestoreGL();

    if (!sLoggedFrame) {
        sLoggedFrame = true;
        Log("VRAM: first frame (phase=%d) win=%d,%d-%d,%d scale=%.2f fonts=%d glerr=%d",
            fromPhase ? 1 : 0, l, t, r, b, sScale, sFontsOk ? 1 : 0, (int)glGetError());
    }
}

static int DrawPhaseCB(XPLMDrawingPhase inPhase, int inIsBefore, void* inRefcon) {
    (void)inPhase; (void)inIsBefore; (void)inRefcon;
    if (!sPhaseSeen) {
        sPhaseSeen = true;
        Log("VRAM: xplm_Phase_Window draw callback active");
    }
    DrawContent(true);
    return 1;
}

static void DrawWindowCB(XPLMWindowID w, void* ref) {
    (void)w; (void)ref;
    if (!sPhaseSeen) DrawContent(false);
}

// ---------------------------------------------------------------------------
static void ApplySlider(int which, float lx) {
    if (which == W_SL_PAGING) {
        float f = (lx - sLay.slPaging.x) / (sLay.slPaging.w > 1.0f ? sLay.slPaging.w : 1.0f);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        // left end = X-Plane default, right end = 10000 m; snapped to 500 m
        float m = 1500.0f + f * (10000.0f - 1500.0f);
        m = floorf(m / 500.0f + 0.5f) * 500.0f;
        float pm = (m >= 9500.0f) ? 0.0f : m;
        if (fabsf(pm - gSet.pagingM) > 0.5f) {
            gSet.pagingM = pm;
            sPendingPaging = true;          // applied by the throttled pump
        }
        return;
    }
    if (which == W_SL_BUDGET) {
        float f = (lx - sLay.slBudget.x) / (sLay.slBudget.w > 1.0f ? sLay.slBudget.w : 1.0f);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        float m = floorf((1.0f + f * 3.0f) * 20.0f + 0.5f) / 20.0f;   // 1.00 .. 4.00, 0.05 steps
        if (fabsf(m - gSet.texBudget) > 0.001f) {
            gSet.texBudget = m;
            sPendingBudget = true;          // applied by the throttled pump
        }
        return;
    }
    const Rect& row = (which == W_SL_TARGET) ? sLay.slTarget : sLay.slMin;
    float frac = (lx - row.x) / (row.w > 1.0f ? row.w : 1.0f);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (which == W_SL_TARGET) {
        gSet.targetPct = 50.0f + frac * (98.0f - 50.0f);
        if (gSet.restorePct > gSet.targetPct - 4.0f) gSet.restorePct = gSet.targetPct - 4.0f;
        if (gSet.restorePct < 30.0f) gSet.restorePct = 30.0f;
    } else {
        gSet.minFactor = (30.0f + frac * 70.0f) / 100.0f;
        if (gRt.factor < gSet.minFactor) gRt.factor = gSet.minFactor;
    }
}

// The mouse coordinate space reported to window callbacks does not always
// match the window geometry space on X-Plane 12.  Try the known conventions and
// use the first one that lands inside the panel; the chosen mode is logged.
static void ToPanel(int x, int y, float l, int t, float* lx, float* ly, int* mode) {
    struct Cand { float a, b; } c[3];
    c[0].a = ((float)x - l) / sScale;       c[0].b = ((float)t - (float)y) / sScale;
    c[1].a = (float)x - l;                  c[1].b = (float)t - (float)y;
    c[2].a = (float)x / sScale;             c[2].b = (float)y / sScale;
    for (int i = 0; i < 3; ++i) {
        if (c[i].a >= -60.0f && c[i].a <= W + 60.0f &&
            c[i].b >= -60.0f && c[i].b <= H + 60.0f) {
            *lx = c[i].a; *ly = c[i].b; *mode = i; return;
        }
    }
    *lx = c[0].a; *ly = c[0].b; *mode = -1;
}

// ---------------------------------------------------------------------------
// Input is polled, not event driven.
//
// On this X-Plane 12 build the window never receives mouse callbacks at all
// (no xplm_MouseDown/Up/Drag), so buttons and sliders are driven from the OS
// cursor + key state here.  This also makes dragging the window work.
// ---------------------------------------------------------------------------
static bool sWasDown     = false;
static bool sPressInside = false;
static int  sPressWidget = W_NONE;
static int  sPollGX = 0, sPollGY = 0;

static void ReleasePanelPress() {
    sPressWidget = W_NONE;
    sPressInside = false;
    sActive = W_NONE;
    sActiveSlider = W_NONE;
    sDragging = false;
}

static float DragLoopCB(float, float, int, void*) {
    if (!sWin || !sVisible) { sWasDown = false; ReleasePanelPress(); return -1.0f; }

    int mx = 0, my = 0;
    CursorPos(&mx, &my);
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(sWin, &l, &t, &r, &b);
    bool inside = (mx >= l && mx <= r && my <= t && my >= b);
    float lx = ((float)mx - (float)l) / sScale;
    float ly = ((float)t - (float)my) / sScale;

    if (down && !sWasDown) {
        // ---- press -------------------------------------------------------
        sPressInside = inside;
        sPressWidget = inside ? HitTest(lx, ly) : W_NONE;
        sActive = sPressWidget;
        if (sPressWidget == W_TITLE) {
            sDragging = true;
            sPollGX = mx - l;
            sPollGY = t - my;
        } else if (sPressWidget == W_SL_TARGET || sPressWidget == W_SL_MIN ||
                   sPressWidget == W_SL_BUDGET || sPressWidget == W_SL_PAGING) {
            sActiveSlider = sPressWidget;
            ApplySlider(sPressWidget, lx);
        }
    } else if (down && sWasDown) {
        // ---- hold --------------------------------------------------------
        if (sDragging) {
            int nl = mx - sPollGX;
            int nt = my + sPollGY;
            XPLMSetWindowGeometry(sWin, nl, nt,
                                  nl + (int)lroundf(W * sScale),
                                  nt - (int)lroundf(H * sScale));
        } else if (sActiveSlider != W_NONE) {
            ApplySlider(sActiveSlider, lx);
        }
    } else if (!down && sWasDown) {
        // ---- release -----------------------------------------------------
        int hit = inside ? HitTest(lx, ly) : W_NONE;
        if (sPressWidget != W_NONE && hit == sPressWidget) {
            switch (hit) {
                case W_PURGE:     Ctrl_Purge(); break;
                case W_RESET:     Ctrl_Reset(); break;
                case W_SAVE:      Config_Save(); break;
                case W_HIDE:      Panel_Show(false); break;
                case W_CLOSE:     Panel_Show(false); break;
                case W_AUTO:      Ctrl_SetAuto(gSet.autoManage == 0); break;
                case W_AUTOPURGE: gSet.autoPurge = gSet.autoPurge ? 0 : 1; Config_Save(); break;
                case W_CLOUDTRIM: {
                    int next = (gSet.cloudTrim + 1) % 4;   // Off / Light / Max / Auto
                    Ctrl_ApplyCloudTrim(next);
                    Config_Save();
                    break;
                }
                default: break;
            }
        }
        if (sDragging) {
            XPLMGetWindowGeometry(sWin, &l, &t, &r, &b);
            gSet.winX = l;
            gSet.winY = t;
            Config_Save();
        }
        ReleasePanelPress();
    }

    // Throttled application of the pool-affecting sliders.  Writing those
    // datarefs makes X-Plane rebuild its texture pools, so they are only
    // pushed on release or every 350 ms while dragging.
    if (sPendingBudget || sPendingPaging) {
        double tnow = (double)XPLMGetElapsedTime();
        bool released = (!down && sWasDown);
        if (released || (tnow - sLastSliderApply) > 0.35) {
            sLastSliderApply = tnow;
            if (sPendingBudget) { sPendingBudget = false; Ctrl_SetTextureBudget(gSet.texBudget); }
            if (sPendingPaging) { sPendingPaging = false; Ctrl_SetPagingDistance(gSet.pagingM); }
        }
    }

    sWasDown = down;
    return -1.0f;      // every frame
}

// The window's own callbacks are kept only to swallow clicks so they do not
// fall through to the sim; all real handling happens in DragLoopCB.
static int MouseCB(XPLMWindowID inWindow, int x, int y, XPLMMouseStatus inMouse, void* ref) {
    (void)inWindow; (void)x; (void)y; (void)inMouse; (void)ref;
    return 1;
}

static XPLMCursorStatus CursorCB(XPLMWindowID inWindow, int x, int y, void* ref) {
    (void)inWindow; (void)x; (void)y; (void)ref;
    return xplm_CursorArrow;
}

static void KeyCB(XPLMWindowID inWindow, char inKey, XPLMKeyFlags inFlags,
                  char inVirtualKey, void* ref, int losingFocus) {
    (void)inWindow; (void)inKey; (void)inFlags; (void)inVirtualKey; (void)ref;
    (void)losingFocus;
}

static int WheelCB(XPLMWindowID inWindow, int x, int y, int wheel, int clicks, void* ref) {
    (void)inWindow; (void)x; (void)y; (void)wheel; (void)clicks; (void)ref;
    return 1;
}

// ---------------------------------------------------------------------------
bool Panel_Active() { return sWin != nullptr; }

bool Panel_Init() {
    if (sWin) return true;
    sScale = PanelScale();

    int sw = 0, sh = 0;
    XPLMGetScreenSize(&sw, &sh);
    if (sw <= 0 || sh <= 0) { sw = 1920; sh = 1080; }

    int w = (int)lroundf(W * sScale);
    int h = (int)lroundf(H * sScale);
    int left = gSet.winX >= 0 ? gSet.winX : (sw - w) / 2;
    int top  = gSet.winY >= 0 ? gSet.winY : (sh + h) / 2 - sh / 10;
    if (left < 0) left = 0;
    if (left + w > sw) left = sw - w;
    if (top > sh) top = sh;
    if (top - h < 0) top = h;

    ComputeLayout();

    XPLMCreateWindow_t p = {};
    p.structSize = sizeof(p);
    p.left = left;
    p.top = top;
    p.right = left + w;
    p.bottom = top - h;
    p.visible = 1;   // create visible so X-Plane registers it for input
    p.drawWindowFunc = DrawWindowCB;
    p.handleMouseClickFunc = MouseCB;
    p.handleKeyFunc = KeyCB;
    p.handleCursorFunc = CursorCB;
    p.handleMouseWheelFunc = WheelCB;
    p.refcon = nullptr;
    // SelfDecorated: X-Plane draws no frame (we draw our own title bar) but it
    // DOES capture clicks inside the window bounds.  WindowDecorationNone makes
    // the window completely click-through, which is why no mouse events arrived.
    p.decorateAsFloatingWindow = xplm_WindowDecorationSelfDecorated;
    p.layer = xplm_WindowLayerFloatingWindows;

    sWin = XPLMCreateWindowEx(&p);
    if (!sWin) { Log("VRAM: panel window creation failed"); return false; }
    XPLMSetWindowPositioningMode(sWin, xplm_WindowPositionFree, -1);
    XPLMBringWindowToFront(sWin);

    sVisible = false;
    sFontsDue = true;
    XPLMRegisterDrawCallback(DrawPhaseCB, xplm_Phase_Window, 0, nullptr);
    XPLMRegisterFlightLoopCallback(DragLoopCB, -1.0f, nullptr);
    {
        int px = GetSystemMetrics(SM_CXSCREEN), py = GetSystemMetrics(SM_CYSCREEN);
        Log("VRAM: screen xplm=%dx%d os=%dx%d ratio=%.3f  window=%d,%d-%d,%d scale=%.3f",
            sw, sh, px, py, (sw > 0) ? (float)px / (float)sw : 1.0f,
            left, top, left + w, top - h, sScale);
    }
    Log("VRAM: panel created at %d,%d size %dx%d (scale %.2f) visible=%d",
        left, top, w, h, sScale, XPLMGetWindowIsVisible(sWin));
    if (gSet.showOnStart) Panel_Show(true);
    else XPLMSetWindowIsVisible(sWin, 0);
    return true;
}

void Panel_Shutdown() {
    if (!sWin) return;
    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(sWin, &l, &t, &r, &b);
    gSet.winX = l; gSet.winY = t;
    XPLMUnregisterDrawCallback(DrawPhaseCB, xplm_Phase_Window, 0, nullptr);
    XPLMUnregisterFlightLoopCallback(DragLoopCB, nullptr);
    XPLMDestroyWindow(sWin);
    sWin = nullptr;
    sVisible = false;
    FreeFonts();
    Log("VRAM: panel destroyed");
}

void Panel_Show(bool show) {
    if (!sWin) return;
    XPLMSetWindowIsVisible(sWin, show ? 1 : 0);
    // Remember the state: hiding the panel keeps the screen clean on the next
    // start instead of popping the window up again.
    gSet.showOnStart = show ? 1 : 0;
    if (show) XPLMBringWindowToFront(sWin);
    else {
        int l = 0, t = 0, r = 0, b = 0;
        XPLMGetWindowGeometry(sWin, &l, &t, &r, &b);
        gSet.winX = l; gSet.winY = t;
        Config_Save();
    }
    sVisible = show;
    sForceRedraw = true;
    Menu_Refresh();
}

void Panel_Toggle() { Panel_Show(!sVisible); }
bool Panel_Visible() { return sVisible; }
void Panel_Tick() {}

void Panel_CycleScale() {
    static const float kScales[] = { 0.0f, 0.85f, 1.0f, 1.15f, 1.3f, 1.5f, 1.7f };
    int cur = 0;
    for (int i = 0; i < 7; ++i)
        if (fabsf(kScales[i] - gSet.uiScale) < 0.01f) cur = i;
    cur = (cur + 1) % 7;
    gSet.uiScale = kScales[cur];
    Config_Save();
    Panel_Rebuild();
}

void Panel_Rebuild() {
    float ns = PanelScale();
    if (!sWin) { sScale = ns; return; }
    int l = 0, t = 0, r = 0, b = 0;
    XPLMGetWindowGeometry(sWin, &l, &t, &r, &b);
    gSet.winX = l; gSet.winY = t;
    bool wasVisible = sVisible;
    XPLMUnregisterDrawCallback(DrawPhaseCB, xplm_Phase_Window, 0, nullptr);
    XPLMUnregisterFlightLoopCallback(DragLoopCB, nullptr);
    XPLMDestroyWindow(sWin);
    sWin = nullptr;
    FreeFonts();
    sScale = ns;
    if (!Panel_Init()) return;
    Panel_Show(wasVisible);
}
