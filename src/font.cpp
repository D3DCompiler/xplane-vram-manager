#include "font.h"
#include "vram_manager.h"

#include <cstring>
#include <vector>
#include <cmath>

int Font_DecodeUtf8(const char* s, uint32_t* out) {
    if (!s || !*s) return 0;
    const unsigned char* p = (const unsigned char*)s;
    if (p[0] < 0x80) { *out = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) { *out = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *out = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *out = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *out = '?';
    return 1;
}

bool Font_Init(Font& f, int px, int texW, int texH, const wchar_t* face) {
    if (f.tex) return true;
    f.px = px;
    f.texW = texW;
    f.texH = texH;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = texW;
    bi.bmiHeader.biHeight      = -texH;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    f.dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!f.dc) return false;

    f.dib = CreateDIBSection(f.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!f.dib || !bits) { Font_Shutdown(f); return false; }
    f.pixels = (uint32_t*)bits;
    memset(f.pixels, 0, (size_t)texW * texH * 4);
    f.oldBmp = SelectObject(f.dc, f.dib);

    f.font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, face ? face : L"Microsoft YaHei UI");
    if (!f.font) { Font_Shutdown(f); return false; }
    f.oldFont = SelectObject(f.dc, f.font);

    SetBkMode(f.dc, TRANSPARENT);
    SetTextColor(f.dc, RGB(255, 255, 255));

    TEXTMETRICW tm = {};
    GetTextMetricsW(f.dc, &tm);
    f.ascent = tm.tmAscent;
    f.lineH  = tm.tmHeight + 2;
    f.penX = 1; f.penY = 1; f.rowH = f.lineH;

    glGenTextures(1, &f.tex);                    // same as other XP12 plugins use

    glBindTexture(GL_TEXTURE_2D, f.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.pixels);

    Log("VRAM: font '%ls' %dpx atlas %dx%d (ascent %d, line %d)",
        face ? face : L"?", px, texW, texH, f.ascent, f.lineH);
    return true;
}

void Font_Shutdown(Font& f) {
    if (f.oldFont && f.dc) SelectObject(f.dc, f.oldFont);
    if (f.font) { DeleteObject(f.font); f.font = nullptr; }
    if (f.oldBmp && f.dc) SelectObject(f.dc, f.oldBmp);
    if (f.dib) { DeleteObject(f.dib); f.dib = nullptr; }
    if (f.dc) { DeleteDC(f.dc); f.dc = nullptr; }
    f.tex = 0;   // texture is owned by X-Plane's texture pool now
    f.glyphs.clear();
    f.pixels = nullptr;
}

// Make sure the glyph exists in the atlas; uploads the new cell if needed.
static bool EnsureGlyph(Font& f, uint32_t cp, FontGlyph* out) {
    auto it = f.glyphs.find(cp);
    if (it != f.glyphs.end()) { if (out) *out = it->second; return true; }

    wchar_t wc[2] = { (wchar_t)cp, 0 };
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(f.dc, wc, 1, &sz);
    int adv = sz.cx > 0 ? sz.cx : f.px / 2;
    int cellW = adv + 2;
    int cellH = f.lineH;

    if (f.penX + cellW + 1 >= f.texW) { f.penX = 1; f.penY += f.rowH; f.rowH = 0; }
    if (f.penY + cellH + 1 >= f.texH) {
        Log("VRAM: font atlas full at codepoint U+%04X", (unsigned)cp);
        return false;
    }
    if (cellH > f.rowH) f.rowH = cellH;

    int x = f.penX, y = f.penY;
    // draw into the shared atlas bitmap (white on black -> coverage in RGB)
    RECT rc = { x, y, x + cellW, y + cellH };
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(f.dc, &rc, black);
    DeleteObject(black);

    TextOutW(f.dc, x + 1, y, wc, 1);

    // convert that cell to white + alpha (grayscale antialiasing)
    for (int row = 0; row < cellH; ++row) {
        uint32_t* line = f.pixels + (size_t)(y + row) * f.texW + x;
        for (int col = 0; col < cellW; ++col) {
            uint32_t p = line[col];
            unsigned b = (p >> 0) & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            unsigned a = (r > g ? (r > b ? r : b) : (g > b ? g : b));
            line[col] = (a << 24) | 0x00FFFFFF;
        }
    }

    Gfx_Flush();          // an upload must not happen inside a solid batch
    glBindTexture(GL_TEXTURE_2D, f.tex);
    // The source rows are f.texW pixels wide - without telling GL the real row
    // length it reads cellW-wide rows and uploads garbage (which looked like
    // fully transparent text).
    glPixelStorei(GL_UNPACK_ROW_LENGTH, f.texW);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, cellW, cellH, GL_RGBA, GL_UNSIGNED_BYTE,
                    f.pixels + (size_t)y * f.texW + x);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    FontGlyph g;
    g.w = cellW; g.h = cellH; g.adv = adv;
    g.u0 = (float)x / (float)f.texW;
    g.u1 = (float)(x + cellW) / (float)f.texW;
    // The DIB is uploaded top-down, so image row 0 lands at V = 0 (the GL
    // "bottom").  Therefore the glyph's top row is at y/texH, not 1 - y/texH.
    g.vTop = (float)y / (float)f.texH;
    g.vBot = (float)(y + cellH) / (float)f.texH;
    f.glyphs[cp] = g;
    f.penX += cellW;
    if (out) *out = g;
    return true;
}

// Bake the printable ASCII range so normal frames never have to upload a glyph
// (an upload touches GL state and would break the frame batch).
void Font_Prewarm(Font& f) {
    for (uint32_t cp = 32; cp < 127; ++cp) {
        FontGlyph g;
        EnsureGlyph(f, cp, &g);
    }
}

float Font_TextWidth(Font& f, const char* utf8) {
    float w = 0.0f;
    const char* p = utf8;
    while (p && *p) {
        uint32_t cp = 0;
        int n = Font_DecodeUtf8(p, &cp);
        p += n;
        if (cp == '\n') { continue; }
        FontGlyph g;
        if (EnsureGlyph(f, cp, &g)) w += (float)g.adv;
    }
    return w;
}

// ---------------------------------------------------------------------------
// Deferred text batch
//
// Every glBegin/glEnd costs a crossing through X-Plane's GL bridge, so glyphs
// are collected here during the frame and drawn in one batch per font texture
// after the solid geometry has been flushed (text always ends up on top, which
// is what the layout wants anyway).
// ---------------------------------------------------------------------------
struct TextQuad {
    GLuint tex;
    float  x0, y0, x1, y1;
    float  u0, v0, u1, v1;
    float  r, g, b, a;
};

static std::vector<TextQuad> sText;

void Font_FlushAll() {
    if (sText.empty()) return;
    Gfx_Flush();                     // close the solid batch first
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLuint texes[8];
    int    texCount = 0;
    for (size_t i = 0; i < sText.size(); ++i) {
        bool known = false;
        for (int k = 0; k < texCount; ++k) if (texes[k] == sText[i].tex) known = true;
        if (!known && texCount < 8) texes[texCount++] = sText[i].tex;
    }
    for (int k = 0; k < texCount; ++k) {
        glBindTexture(GL_TEXTURE_2D, texes[k]);
        glBegin(GL_QUADS);
        for (size_t i = 0; i < sText.size(); ++i) {
            const TextQuad& q = sText[i];
            if (q.tex != texes[k]) continue;
            glColor4f(q.r, q.g, q.b, q.a);
            glTexCoord2f(q.u0, q.v0); glVertex2i((GLint)lroundf(q.x0), (GLint)lroundf(q.y0));
            glTexCoord2f(q.u1, q.v0); glVertex2i((GLint)lroundf(q.x1), (GLint)lroundf(q.y0));
            glTexCoord2f(q.u1, q.v1); glVertex2i((GLint)lroundf(q.x1), (GLint)lroundf(q.y1));
            glTexCoord2f(q.u0, q.v1); glVertex2i((GLint)lroundf(q.x0), (GLint)lroundf(q.y1));
        }
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    sText.clear();
}
void Font_DrawText(Font& f, float x, float baselineY, const char* utf8, Rgba color) {
    if (!utf8 || !*utf8) return;

    // Make sure every glyph exists first.  Uploading touches GL state, so the
    // solid batch must be closed before that happens.
    bool needUpload = false;
    {
        const char* p = utf8;
        while (*p) {
            uint32_t cp = 0;
            p += Font_DecodeUtf8(p, &cp);
            if (f.glyphs.find(cp) == f.glyphs.end()) { needUpload = true; break; }
        }
    }
    if (needUpload) {
        Gfx_Flush();
        const char* p = utf8;
        FontGlyph g;
        while (*p) {
            uint32_t cp = 0;
            p += Font_DecodeUtf8(p, &cp);
            EnsureGlyph(f, cp, &g);
        }
    }

    float pen = x;
    const float topY = baselineY + (float)f.ascent;
    const char* p = utf8;
    while (*p) {
        uint32_t cp = 0;
        p += Font_DecodeUtf8(p, &cp);
        auto it = f.glyphs.find(cp);
        if (it == f.glyphs.end()) continue;
        const FontGlyph& g = it->second;
        TextQuad q;
        q.tex = f.tex;
        q.x0 = pen;             q.x1 = pen + (float)g.w;
        q.y0 = topY;            q.y1 = topY - (float)g.h;
        q.u0 = g.u0;            q.u1 = g.u1;
        q.v0 = g.vTop;          q.v1 = g.vBot;
        q.r = color.r; q.g = color.g; q.b = color.b; q.a = color.a;
        sText.push_back(q);
        pen += (float)g.adv;
    }
}
