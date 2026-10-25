// gfx.h: Direct2D and DirectWrite, alive only while a window is open. The resident (tray, hotkeys,
// updates) never loads them: d2d1.dll and dwrite.dll are loaded by hand on the first window and the
// factories are released with the last one, and then the working set is trimmed (see gfx_release).
#pragma once
#include "util.h"
#include <string>
#include <d2d1.h>
#include <dwrite.h>

extern ID2D1Factory *g_d2d;
extern IDWriteFactory *g_dw;

bool gfx_acquire();   // one per open window; false if Direct2D cannot be loaded
void gfx_release();   // the last one frees the factories and trims the working set

// Colours (0xRRGGBB) of the theme in use: dark (the default), light, or Windows' own choice (theme_apply).
extern uint32_t kBg,  // the list
    kPane,            // navigation pane, toolbar
    kTabBar,          // behind the tabs (the window's top edge)
    kTabOn,           // the place shown, in the navigation pane
    kHover, kPress, kLine, kText, kDim, kAccent,
    kOnAccent,        // text on kAccent
    kSelect, kDanger;

// conf "theme": "dark" (default), "light" or "system" (follows Windows' app mode, also when it changes)
std::string theme_setting();
void theme_set(const std::string &name);
bool theme_apply(); // sets the colours from the setting (and Windows); true if they changed
bool theme_light();

inline D2D1_COLOR_F color(uint32_t rgb, float a = 1) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

// The icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets (Windows 10).
const wchar_t *icon_font();

// A Segoe icon glyph as a 32bpp premultiplied bitmap for menu rows (GDI only: the tray menu
// opens without Direct2D). The caller deletes it.
HBITMAP glyph_bitmap(const wchar_t *glyph, int px, uint32_t rgb);
