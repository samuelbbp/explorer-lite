// surface.h: a window of the shell drawn with Direct2D: each taskbar and Start. Their render targets are software
// ones: a Direct3D device per window would cost the always-on shell several MB each for a few repaints a minute.
// Sizes are DIPs (the target's DPI is the monitor's), as in window.cpp.
#pragma once
#include "gfx.h"
#include "thumbs.h"
#include <string>
#include <unordered_map>
#include <vector>

struct Surface {
    HWND hwnd = NULL;
    UINT dpi = 96;
    ID2D1HwndRenderTarget *rt = nullptr;
    ID2D1RenderTarget *dc = nullptr;   // what is drawn on: rt, or a bitmap for a shot
    ID2D1SolidColorBrush *br = nullptr;
    IDWriteTextFormat *f_ui = nullptr, *f_ui_c = nullptr, *f_small = nullptr, *f_small_r = nullptr, *f_icon = nullptr, *f_big = nullptr;
    IDWriteTextFormat *f_wrap = nullptr, *f_bold = nullptr; // paragraphs (wrapped, from the top); a bold line
    std::unordered_map<const Thumb *, ID2D1Bitmap *> bmps; // the icons drawn, on dc
    bool shot = false;                 // drawing for a shot: the edit controls are not in the bitmap
};

inline float surf_scale(const Surface &s) { return s.dpi / 96.f; }
inline bool surf_inside(const D2D1_RECT_F &r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// Paints the window: BeginPaint/EndPaint, the target made if needed, draw(ctx) between BeginDraw and EndDraw.
void surf_paint(Surface &s, void (*draw)(void *ctx), void *ctx);
// The same frame into a PNG (tests: nothing has to be on the screen). Several surfaces: one under the other, each at
// its own DPI (the taskbars of every monitor in one picture).
struct ShotPart {
    Surface *s;
    void (*draw)(void *ctx);
    void *ctx;
};
bool surf_shot(const std::vector<ShotPart> &parts, const std::wstring &png);
inline bool surf_shot(Surface &s, const std::wstring &png, void (*draw)(void *ctx), void *ctx) { return surf_shot({ { &s, draw, ctx } }, png); }
void surf_formats(Surface &s);   // the text formats, again after a DPI change is not needed (DIPs)
void surf_free(Surface &s);      // target, bitmaps and formats

void surf_text(Surface &s, const std::wstring &t, D2D1_RECT_F r, IDWriteTextFormat *f, uint32_t rgb, float a = 1);
float surf_text_width(Surface &s, const std::wstring &t, IDWriteTextFormat *f);
float surf_text_height(const std::wstring &t, IDWriteTextFormat *f, float width); // wrapped at width
void surf_fill(Surface &s, D2D1_RECT_F r, uint32_t rgb, float a = 1);
void surf_round(Surface &s, D2D1_RECT_F r, float rad, uint32_t rgb, float a = 1);
void surf_icon(Surface &s, const Thumb *t, D2D1_RECT_F r, float a = 1); // nothing if t is null or empty
void surf_forget(Surface &s, const Thumb *t); // t is about to be deleted

// An icon's pixels (premultiplied BGRA, top-down) into t; false if it has none.
bool icon_pixels(HICON icon, Thumb &t);
