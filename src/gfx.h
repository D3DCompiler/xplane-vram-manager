// Minimal immediate-mode 2D drawing helpers on top of the legacy OpenGL that
// X-Plane's GL bridge exposes to plugins (same path XPWidgets and AviTab use).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

struct Rgba {
    float r, g, b, a;
};

inline Rgba rgba(float r, float g, float b, float a = 1.0f) { return Rgba{ r, g, b, a }; }
inline Rgba rgbaMix(Rgba x, Rgba y, float t) {
    return Rgba{ x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t,
                 x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t };
}

void Gfx_BeginFrame();                 // 2D state, blending on, starts the solid batch
void Gfx_Flush();                      // close the solid batch (call once per frame)
void Gfx_SetDrawMode(int texMode, int colorMode);
void Gfx_BeginText();                  // adds one texture unit
void Gfx_EndText();

void Gfx_Rect(float x, float y, float w, float h, Rgba c);
void Gfx_RoundRect(float x, float y, float w, float h, float r, Rgba c);
void Gfx_RoundRectOutline(float x, float y, float w, float h, float r, float thickness, Rgba c);
void Gfx_TexQuad(float x, float y, float w, float h, Rgba tint);
void Gfx_Circle(float cx, float cy, float radius, Rgba c);
void Gfx_XMark(float cx, float cy, float half, float thickness, Rgba c);
void Gfx_Shadow(float x, float y, float w, float h, float r);

// A cheap drop shadow: a few translucent rounded rectangles behind the panel.
void Gfx_PanelShadow(float x, float y, float w, float h, float r);
