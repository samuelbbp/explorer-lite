// gfx.cpp: see gfx.h.
#include "gfx.h"

ID2D1Factory *g_d2d;
IDWriteFactory *g_dw;

namespace {

int g_users;
HMODULE g_d2d_dll, g_dw_dll;

} // namespace

bool gfx_acquire() {
    if (g_users++ > 0) return g_d2d && g_dw;
    typedef HRESULT(WINAPI * PD2D)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
    typedef HRESULT(WINAPI * PDW)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);
    if (!g_d2d_dll) g_d2d_dll = LoadLibraryExW(L"d2d1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dw_dll) g_dw_dll = LoadLibraryExW(L"dwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PD2D make_d2d = g_d2d_dll ? (PD2D)(void *)GetProcAddress(g_d2d_dll, "D2D1CreateFactory") : nullptr;
    PDW make_dw = g_dw_dll ? (PDW)(void *)GetProcAddress(g_dw_dll, "DWriteCreateFactory") : nullptr;
    D2D1_FACTORY_OPTIONS opt = {};
    if (make_d2d) make_d2d(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void **)&g_d2d);
    if (make_dw) make_dw(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw);
    if (!g_d2d || !g_dw) log_line("gfx: Direct2D or DirectWrite could not be loaded");
    return g_d2d && g_dw;
}

void gfx_release() {
    if (g_users <= 0 || --g_users > 0) return;
    if (g_dw) g_dw->Release();
    if (g_d2d) g_d2d->Release();
    g_dw = nullptr;
    g_d2d = nullptr;
    // back to the resident: the pages the windows used go back to Windows now, not when it is short of memory
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

const wchar_t *icon_font() {
    static const wchar_t *name;
    if (!name) {
        // installed fonts: EnumFontFamilies would need a DC; the registry list is enough
        HKEY k;
        name = L"Segoe MDL2 Assets";
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExW(k, L"Segoe Fluent Icons (TrueType)", NULL, NULL, NULL, NULL) == ERROR_SUCCESS) name = L"Segoe Fluent Icons";
            RegCloseKey(k);
        }
    }
    return name;
}

HBITMAP glyph_bitmap(const wchar_t *glyph, int px, uint32_t rgb) {
    if (px <= 0) return NULL;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = px; // bottom-up: the layout Windows' own PARGB menu bitmaps use
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!dib || !dc || !bits) {
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        return NULL;
    }
    HGDIOBJ old = SelectObject(dc, dib);
    // white on black, grayscale antialiasing: the brightness of each pixel is its coverage
    HFONT font = CreateFontW(-(px * 7 / 8), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, icon_font());
    HGDIOBJ oldf = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc = { 0, 0, px, px };
    DrawTextW(dc, glyph, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    uint8_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    uint32_t *p = (uint32_t *)bits;
    for (int i = 0; i < px * px; i++) {
        uint32_t c = p[i];
        uint32_t a = ((c >> 16 & 255) + (c >> 8 & 255) + (c & 255)) / 3;
        p[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
    }
    SelectObject(dc, oldf);
    DeleteObject(font);
    SelectObject(dc, old);
    DeleteDC(dc);
    return dib;
}
