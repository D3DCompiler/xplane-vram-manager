// Minimal immediate-mode 2D drawing helpers on top of the legacy OpenGL that
// X-Plane's GL bridge exposes to plugins.
//
// Two things matter for speed through that bridge:
//
//   * it only reliably implements GL_QUADS (XPWidgets and AviTab use nothing
//     else), so every shape here is emitted as quads, using zero-width quads to
//     emulate a triangle fan;
//   * every glBegin/glEnd costs a bridge crossing, so ALL solid geometry of a
//     frame goes into ONE batch (colour changes are free inside a batch).
//
// Call Gfx_Flush() once per frame when the solid drawing is done; text drawing
// (font.cpp) flushes it automatically before switching to the font texture.
#include "gfx.h"
#include "vram_manager.h"

#include <cmath>

static void EmitVertex(float x, float y) { glVertex2i((GLint)lroundf(x), (GLint)lroundf(y)); }

// X-Plane 12's GL bridge drops geometry drawn while XPLMSetGraphicsState()
// reports zero texture units, so the solid batch asserts a one-unit state with
// GL_TEXTURE_2D disabled.
static int  sTexMode   = 0;   // 0 state(0,0)+disable, 1 state(0,1)+disable, 2 state(0,1)+enable
static int  sColorMode = 0;   // 0 glColor4f, 1 glColor3f
static bool sInBatch   = false;

void Gfx_SetDrawMode(int texMode, int colorMode) { sTexMode = texMode; sColorMode = colorMode; }

static void SolidState() {
    if (sTexMode == 0) {
        XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
        glDisable(GL_TEXTURE_2D);
    } else if (sTexMode == 1) {
        XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
        glDisable(GL_TEXTURE_2D);
    } else {
        XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
        glEnable(GL_TEXTURE_2D);
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void SetColor(Rgba c) {
    if (sColorMode == 0) glColor4f(c.r, c.g, c.b, c.a);
    else glColor3f(c.r, c.g, c.b);
}

// Open the single solid batch on first use.
static void EnsureBatch() {
    if (sInBatch) return;
    SolidState();
    glBegin(GL_QUADS);
    sInBatch = true;
}

void Gfx_Flush() {
    if (!sInBatch) return;
    glEnd();
    sInBatch = false;
}

static void EmitFanQuad(float cx, float cy, float a0, float a1, float r) {
    float x0 = cx + cosf(a0) * r, y0 = cy + sinf(a0) * r;
    float x1 = cx + cosf(a1) * r, y1 = cy + sinf(a1) * r;
    EmitVertex(cx, cy);
    EmitVertex(x0, y0);
    EmitVertex(x1, y1);
    EmitVertex(cx, cy);
}

void Gfx_BeginFrame() {
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    glDisable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    EnsureBatch();
}

void Gfx_BeginText() {
    Gfx_Flush();
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Gfx_EndText() { glDisable(GL_TEXTURE_2D); }

void Gfx_Rect(float x, float y, float w, float h, Rgba c) {
    if (w <= 0.0f || h <= 0.0f) return;
    EnsureBatch();
    SetColor(c);
    EmitVertex(x, y);
    EmitVertex(x + w, y);
    EmitVertex(x + w, y + h);
    EmitVertex(x, y + h);
}

void Gfx_RoundRect(float x, float y, float w, float h, float r, Rgba c) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (r < 0.5f) { Gfx_Rect(x, y, w, h, c); return; }
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;

    EnsureBatch();
    SetColor(c);

    // middle + two straight sides
    EmitVertex(x + r, y);           EmitVertex(x + w - r, y);
    EmitVertex(x + w - r, y + h);   EmitVertex(x + r, y + h);
    EmitVertex(x, y + r);           EmitVertex(x + r, y + r);
    EmitVertex(x + r, y + h - r);   EmitVertex(x, y + h - r);
    EmitVertex(x + w - r, y + r);   EmitVertex(x + w, y + r);
    EmitVertex(x + w, y + h - r);   EmitVertex(x + w - r, y + h - r);

    const float Q = 1.5707963f;
    const int   steps = 6;
    struct Corner { float cx, cy, a0; };
    const Corner corners[4] = {
        { x + r,     y + r,         3.1415927f },
        { x + w - r, y + r,         3.1415927f + Q },
        { x + w - r, y + h - r,     0.0f },
        { x + r,     y + h - r,     Q },
    };
    for (int ci = 0; ci < 4; ++ci)
        for (int i = 0; i < steps; ++i) {
            float a0 = corners[ci].a0 + Q * (float)i / (float)steps;
            float a1 = corners[ci].a0 + Q * (float)(i + 1) / (float)steps;
            EmitFanQuad(corners[ci].cx, corners[ci].cy, a0, a1, r);
        }
}

void Gfx_RoundRectOutline(float x, float y, float w, float h, float r, float thickness, Rgba c) {
    if (thickness <= 0.0f) return;
    Gfx_Rect(x + r, y, w - 2.0f * r, thickness, c);
    Gfx_Rect(x + r, y + h - thickness, w - 2.0f * r, thickness, c);
    Gfx_Rect(x, y + r, thickness, h - 2.0f * r, c);
    Gfx_Rect(x + w - thickness, y + r, thickness, h - 2.0f * r, c);

    if (r < 0.5f) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    float ri = r - thickness;
    if (ri < 0.0f) ri = 0.0f;

    EnsureBatch();
    SetColor(c);
    const float Q = 1.5707963f;
    const int   steps = 6;
    struct Corner { float cx, cy, a0; };
    const Corner corners[4] = {
        { x + r,     y + r,         3.1415927f },
        { x + w - r, y + r,         3.1415927f + Q },
        { x + w - r, y + h - r,     0.0f },
        { x + r,     y + h - r,     Q },
    };
    for (int ci = 0; ci < 4; ++ci) {
        for (int i = 0; i < steps; ++i) {
            float a0 = corners[ci].a0 + Q * (float)i / (float)steps;
            float a1 = corners[ci].a0 + Q * (float)(i + 1) / (float)steps;
            float ca0 = cosf(a0), sa0 = sinf(a0);
            float ca1 = cosf(a1), sa1 = sinf(a1);
            EmitVertex(corners[ci].cx + ca0 * ri, corners[ci].cy + sa0 * ri);
            EmitVertex(corners[ci].cx + ca0 * r,  corners[ci].cy + sa0 * r);
            EmitVertex(corners[ci].cx + ca1 * r,  corners[ci].cy + sa1 * r);
            EmitVertex(corners[ci].cx + ca1 * ri, corners[ci].cy + sa1 * ri);
        }
    }
}

void Gfx_TexQuad(float x, float y, float w, float h, Rgba tint) {
    if (w <= 0.0f || h <= 0.0f) return;
    Gfx_BeginText();
    SetColor(tint);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); EmitVertex(x, y);
    glTexCoord2f(1.0f, 0.0f); EmitVertex(x + w, y);
    glTexCoord2f(1.0f, 1.0f); EmitVertex(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f); EmitVertex(x, y + h);
    glEnd();
    Gfx_EndText();
}

void Gfx_Circle(float cx, float cy, float radius, Rgba c) {
    if (radius <= 0.0f) return;
    EnsureBatch();
    SetColor(c);
    const int steps = 12;
    for (int i = 0; i < steps; ++i) {
        float a0 = 6.2831853f * (float)i / (float)steps;
        float a1 = 6.2831853f * (float)(i + 1) / (float)steps;
        EmitFanQuad(cx, cy, a0, a1, radius);
    }
}

void Gfx_XMark(float cx, float cy, float half, float thickness, Rgba c) {
    EnsureBatch();
    SetColor(c);
    const float d = 0.7071f;
    const float t = thickness * 0.5f;
    for (int s = -1; s <= 1; s += 2) {
        float dx = d * half * (float)s, dy = d * half;
        float ax = cx - dx, ay = cy - dy;
        float bx = cx + dx, by = cy + dy;
        float px = -dy, py = dx;
        float len = sqrtf(px * px + py * py);
        if (len < 0.0001f) continue;
        px = px / len * t; py = py / len * t;
        EmitVertex(ax + px, ay + py);
        EmitVertex(bx + px, by + py);
        EmitVertex(bx - px, by - py);
        EmitVertex(ax - px, ay - py);
    }
}

void Gfx_PanelShadow(float x, float y, float w, float h, float r) {
    // three soft layers instead of six - it is a shadow, not a feature
    for (int i = 3; i >= 1; --i) {
        float s = (float)i * 3.0f;
        float a = 0.045f * (float)(4 - i);
        Gfx_RoundRect(x - s, y - s, w + s * 2.0f, h + s * 2.0f, r + s, rgba(0, 0, 0, a));
    }
}

void Gfx_Shadow(float x, float y, float w, float h, float r) { Gfx_PanelShadow(x, y, w, h, r); }
