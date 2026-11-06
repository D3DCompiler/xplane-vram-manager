// GDI-rendered glyph atlas + text drawing with the same OpenGL path X-Plane
// exposes to plugins.  This replaces the SDK's 8 px bitmap font, which cannot
// be scaled without turning blurry and has no CJK glyphs at all.
#pragma once

#include <windows.h>
#include <GL/gl.h>
#include <cstdint>
#include <map>
#include <string>

#include "gfx.h"

struct FontGlyph {
    float u0, u1;   // left / right
    float vTop;     // texture V of the glyph's top row
    float vBot;     // texture V of the glyph's bottom row
    int   w, h;     // pixel size of the glyph cell
    int   adv;      // advance width
};

struct Font {
    GLuint   tex      = 0;
    int      texW     = 0, texH = 0;
    int      px       = 16;
    int      ascent   = 0;
    int      lineH    = 0;
    int      penX     = 1, penY = 1, rowH = 0;
    HDC      dc       = nullptr;
    HBITMAP  dib      = nullptr;
    HGDIOBJ  oldBmp   = nullptr;
    HFONT    font     = nullptr;
    HGDIOBJ  oldFont  = nullptr;
    uint32_t* pixels  = nullptr;   // top-down BGRA
    std::map<uint32_t, FontGlyph> glyphs;
};

// px = pixel height of the font; atlas is texW x texH (power of two)
bool Font_Init(Font& f, int px, int texW, int texH, const wchar_t* face);
void Font_Shutdown(Font& f);

float Font_TextWidth(Font& f, const char* utf8);
inline int Font_LineHeight(const Font& f) { return f.lineH; }
void  Font_DrawText(Font& f, float x, float baselineY, const char* utf8, Rgba color);
void  Font_FlushAll();   // draw the deferred glyph batch (once per frame)
void  Font_Prewarm(Font& f);  // bake the printable ASCII range up front

// UTF-8 helper shared with the panel
int Font_DecodeUtf8(const char* s, uint32_t* out);
