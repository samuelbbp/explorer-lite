// gfx.h: Direct2D and DirectWrite, alive only while a window is open. The resident (tray, hotkeys,
// updates) never loads them: d2d1.dll and dwrite.dll are loaded by hand on the first window and the
// factories are released with the last one, and then the working set is trimmed (see gfx_release).
#pragma once
#include "util.h"
#include <d2d1.h>
#include <dwrite.h>

extern ID2D1Factory *g_d2d;
extern IDWriteFactory *g_dw;

bool gfx_acquire();   // one per open window; false if Direct2D cannot be loaded
void gfx_release();   // the last one frees the factories and trims the working set

// Colours (0xRRGGBB): the dark theme, the default.
enum : uint32_t {
    kBg = 0x1C1C1C,       // the list
    kPane = 0x202020,     // navigation pane, toolbar
    kTabBar = 0x161616,   // behind the tabs
    kTabOn = 0x2B2B2B,    // the current tab
    kHover = 0x2D2D2D,
    kPress = 0x383838,
    kLine = 0x333333,
    kText = 0xF2F2F2,
    kDim = 0x9E9E9E,
    kAccent = 0x4CC2FF,
    kSelect = 0x1F4A6E,
    kDanger = 0xFF6B6B,
};

inline D2D1_COLOR_F color(uint32_t rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

// The icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets (Windows 10).
const wchar_t *icon_font();

// A Segoe icon glyph as a 32bpp premultiplied bitmap for menu rows (GDI only: the tray menu
// opens without Direct2D). The caller deletes it.
HBITMAP glyph_bitmap(const wchar_t *glyph, int px, uint32_t rgb);
