// switcher.cpp: Alt+Tab and Win+Tab (see shell.h). The windows of the taskbar in the order they were last used
// (Windows' own z-order), each a live picture made by DWM (DwmRegisterThumbnail: DWM draws it into our window, nothing
// is copied or kept), its icon and its title. Alt+Tab is a row that never takes the focus, driven by the keyboard
// hook: Tab and Shift+Tab move, letting Alt go brings the chosen one to the front, Esc closes it, and it only shows
// if Alt is held a moment (a quick Alt+Tab just switches, as Windows does). Win+Tab is a grid over the monitor that
// takes the focus: arrows, Enter, Esc, or the mouse. A shot cannot hold what DWM draws, so a test shot draws a copy of
// each window instead (PrintWindow), only then.
#include "shell.h"
#include "app.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "gfx.h"
#include "idioma.h"
#include "surface.h"

namespace {

struct Card {
    HWND hwnd;
    std::wstring title;
    Thumb *icon = nullptr; // owned (a copy of the bar's)
    Thumb *copy = nullptr; // owned: PrintWindow's picture, for a shot
    HTHUMBNAIL thumb = NULL;
    D2D1_RECT_F r = {}, pic = {}; // the card, and where the picture goes (DIPs)
};

struct Sw {
    Surface s;
    std::vector<Card> cards;
    int sel = 0, hover = -1, cols = 1;
    bool grid = false, sticky = false, tracking = false;
    float w = 0, h = 0; // DIPs
};

Sw *g_sw;
const UINT_PTR kTimerShow = 1;
std::wstring sw_class() { return L"ExplorerLiteSwitcher" + instance_suffix(); }

// layout, in DIPs: Alt+Tab's cards, then Win+Tab's
const float kPad = 14, kGapC = 10, kTitleH = 30;
float card_w(bool grid) { return grid ? 340.f : 236.f; }
float thumb_h(bool grid) { return grid ? 210.f : 140.f; }

void free_card(Sw *w, Card &c) {
    if (c.thumb) DwmUnregisterThumbnail(c.thumb);
    c.thumb = NULL;
    for (Thumb **t : { &c.icon, &c.copy })
        if (*t) {
            surf_forget(w->s, *t);
            delete *t;
            *t = nullptr;
        }
}

// where each card and its picture go; the window's size and place (Alt+Tab: centred; Win+Tab: the whole work area)
void layout(Sw *w, HMONITOR mon) {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    float k = surf_scale(w->s);
    RECT wa = mi.rcWork;
    float aw = (wa.right - wa.left) / k, ah = (wa.bottom - wa.top) / k;
    float cw = card_w(w->grid), ch = kTitleH + thumb_h(w->grid) + 10;
    int n = (int)w->cards.size();
    int max_cols = std::max(1, (int)((aw * (w->grid ? 0.92f : 0.9f) - 2 * kPad + kGapC) / (cw + kGapC)));
    if (w->grid) max_cols = std::min(max_cols, std::max(1, (int)ceil(sqrt((double)n) * 1.5)));
    w->cols = std::max(1, std::min(n, max_cols));
    int rows = std::max(1, (n + w->cols - 1) / w->cols);
    float gw = w->cols * (cw + kGapC) - kGapC, gh = rows * (ch + kGapC) - kGapC;
    w->w = w->grid ? aw : gw + 2 * kPad;
    w->h = w->grid ? ah : std::min(ah * 0.9f, gh + 2 * kPad);
    float x0 = (w->w - gw) / 2, y0 = w->grid ? std::max(kPad, (w->h - gh) / 2) : kPad;
    for (int i = 0; i < n; i++) {
        Card &c = w->cards[i];
        float x = x0 + (i % w->cols) * (cw + kGapC), y = y0 + (i / w->cols) * (ch + kGapC);
        c.r = D2D1::RectF(x, y, x + cw, y + ch);
        c.pic = D2D1::RectF(x + 6, y + kTitleH, x + cw - 6, y + ch - 6);
    }
    int pw = (int)(w->w * k), ph = (int)(w->h * k);
    int x = w->grid ? wa.left : wa.left + ((wa.right - wa.left) - pw) / 2, y = w->grid ? wa.top : wa.top + ((wa.bottom - wa.top) - ph) / 2;
    SetWindowPos(w->s.hwnd, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE);
}

// the picture fitted into its place, keeping the window's shape
D2D1_RECT_F fit(D2D1_RECT_F area, float sw, float sh) {
    if (sw <= 0 || sh <= 0) return area;
    float aw = area.right - area.left, ah = area.bottom - area.top, s = std::min(aw / sw, ah / sh);
    float w = sw * s, h = sh * s, x = area.left + (aw - w) / 2, y = area.top + (ah - h) / 2;
    return D2D1::RectF(x, y, x + w, y + h);
}

void thumbs_place(Sw *w) {
    float k = surf_scale(w->s);
    for (Card &c : w->cards) {
        if (IsIconic(c.hwnd)) continue; // DWM has nothing to show for it: its icon instead
        if (!c.thumb && FAILED(DwmRegisterThumbnail(w->s.hwnd, c.hwnd, &c.thumb))) c.thumb = NULL;
        if (!c.thumb) continue;
        SIZE src = {};
        DwmQueryThumbnailSourceSize(c.thumb, &src);
        D2D1_RECT_F f = fit(c.pic, (float)src.cx, (float)src.cy);
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = { (LONG)(f.left * k), (LONG)(f.top * k), (LONG)(f.right * k), (LONG)(f.bottom * k) };
        p.fVisible = TRUE;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(c.thumb, &p);
    }
}

void draw(void *ctx) {
    Sw *w = (Sw *)ctx;
    Surface &s = w->s;
    surf_fill(s, D2D1::RectF(0, 0, w->w, w->h), w->grid ? kBg : kPane);
    if (w->cards.empty()) surf_text(s, T(L"No windows open"), D2D1::RectF(0, 0, w->w, w->h), s.f_ui_c, kDim);
    for (int i = 0; i < (int)w->cards.size(); i++) {
        const Card &c = w->cards[i];
        bool sel = i == w->sel;
        if (sel || i == w->hover) surf_round(s, c.r, 8, sel ? kSelect : kHover);
        if (sel) {
            s.br->SetColor(color(kAccent));
            s.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.r.left + 1, c.r.top + 1, c.r.right - 1, c.r.bottom - 1), 8, 8), s.br, 2);
        }
        float iy = c.r.top + (kTitleH - 16) / 2;
        surf_icon(s, c.icon, D2D1::RectF(c.r.left + 10, iy, c.r.left + 26, iy + 16));
        surf_text(s, c.title, D2D1::RectF(c.r.left + 34, c.r.top, c.r.right - 10, c.r.top + kTitleH), s.f_ui, kText);
        if (s.shot && c.copy) surf_icon(s, c.copy, fit(c.pic, (float)c.copy->w, (float)c.copy->h));
        else if (!c.thumb || IsIconic(c.hwnd)) { // minimized: its icon, big
            float cx = (c.pic.left + c.pic.right) / 2, cy = (c.pic.top + c.pic.bottom) / 2, z = 48;
            surf_icon(s, c.icon, D2D1::RectF(cx - z / 2, cy - z / 2, cx + z / 2, cy + z / 2), 0.85f);
        }
    }
}

int hit(Sw *w, float x, float y) {
    for (int i = 0; i < (int)w->cards.size(); i++)
        if (surf_inside(w->cards[i].r, x, y)) return i;
    return -1;
}

void sw_close() {
    Sw *w = g_sw;
    if (!w) return;
    g_sw = nullptr;
    for (Card &c : w->cards) free_card(w, c);
    surf_free(w->s);
    DestroyWindow(w->s.hwnd);
    delete w;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); // back to the bars alone
}

void choose(int i) {
    HWND h = g_sw && i >= 0 && i < (int)g_sw->cards.size() ? g_sw->cards[i].hwnd : NULL;
    sw_close();
    if (h && IsWindow(h)) shell_activate(h);
}

void sw_move(Sw *w, int d) {
    int n = (int)w->cards.size();
    if (!n) return;
    w->sel = ((w->sel + d) % n + n) % n;
    InvalidateRect(w->s.hwnd, NULL, FALSE);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Sw *w = g_sw && g_sw->s.hwnd == h ? g_sw : nullptr;
    if (!w) return DefWindowProcW(h, msg, wp, lp);
    float k = surf_scale(w->s);
    switch (msg) {
    case WM_PAINT: surf_paint(w->s, draw, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == kTimerShow) { // Alt still held: now it shows
            KillTimer(h, kTimerShow);
            ShowWindow(h, SW_SHOWNOACTIVATE);
        }
        return 0;
    case WM_MOUSEACTIVATE: return w->grid ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (w->grid && LOWORD(wp) == WA_INACTIVE && !w->sticky) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE: sw_close(); return 0;
    case WM_MOUSEMOVE: {
        int i = hit(w, GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k);
        if (i != w->hover) {
            w->hover = i;
            InvalidateRect(h, NULL, FALSE);
        }
        if (!w->tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            w->tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        w->tracking = false;
        w->hover = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int i = hit(w, GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k);
        if (i >= 0) choose(i);
        else if (w->grid) sw_close(); // the empty space: back where it was
        return 0;
    }
    case WM_MBUTTONUP: { // middle click: that window closes (as in Windows' Task view)
        int i = hit(w, GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k);
        if (i >= 0) {
            PostMessageW(w->cards[i].hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
            free_card(w, w->cards[i]);
            w->cards.erase(w->cards.begin() + i);
            if (w->sel >= (int)w->cards.size()) w->sel = (int)w->cards.size() - 1;
            w->hover = -1;
            layout(w, MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY));
            thumbs_place(w);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: sw_close(); return 0;
        case VK_RETURN:
        case VK_SPACE: choose(w->sel); return 0;
        case VK_LEFT: sw_move(w, -1); return 0;
        case VK_RIGHT:
        case VK_TAB: sw_move(w, (GetKeyState(VK_SHIFT) & 0x8000) && wp == VK_TAB ? -1 : 1); return 0;
        case VK_UP: sw_move(w, -w->cols); return 0;
        case VK_DOWN: sw_move(w, w->cols); return 0;
        }
        return 0;
    case WM_DPICHANGED:
        w->s.dpi = HIWORD(wp);
        layout(w, MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY));
        thumbs_place(w);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool sw_open(bool grid, bool sticky, bool show_now) {
    if (g_sw) sw_close();
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        std::wstring cls = sw_class();
        wc.lpszClassName = cls.c_str();
        registered = RegisterClassExW(&wc) != 0;
    }
    std::vector<ShellWindow> wins = shell_windows_by_use();
    if (wins.empty() && !grid) return false;
    Sw *w = new Sw;
    w->grid = grid;
    w->sticky = sticky;
    for (ShellWindow &sw : wins) {
        Card c;
        c.hwnd = sw.hwnd;
        c.title = sw.title;
        if (sw.icon && sw.icon->w > 0) c.icon = new Thumb(*sw.icon);
        w->cards.push_back(c);
    }
    // on the monitor of the window in front (or the mouse's)
    HWND f = GetForegroundWindow();
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = f && IsWindowVisible(f) ? MonitorFromWindow(f, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    g_sw = w;
    w->s.dpi = shell_monitor_dpi(mon);
    w->s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (grid ? 0 : WS_EX_NOACTIVATE), sw_class().c_str(), grid ? L"Task view" : L"Switch windows",
                                WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_inst, NULL);
    if (!w->s.hwnd) {
        g_sw = nullptr;
        for (Card &c : w->cards) free_card(w, c);
        delete w;
        return false;
    }
    if (!grid) {
        DWORD round = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(w->s.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    }
    w->s.dpi = GetDpiForWindow(w->s.hwnd);
    surf_formats(w->s);
    layout(w, mon);
    UINT dpi = GetDpiForWindow(w->s.hwnd); // moved to a monitor with another scale
    if (dpi != w->s.dpi) {
        w->s.dpi = dpi;
        layout(w, mon);
    }
    thumbs_place(w);
    if (grid) {
        w->sel = 0;
        ShowWindow(w->s.hwnd, SW_SHOW);
        SetForegroundWindow(w->s.hwnd);
    } else if (show_now) ShowWindow(w->s.hwnd, SW_SHOWNOACTIVATE);
    else SetTimer(w->s.hwnd, kTimerShow, 120, NULL);
    return true;
}

// a copy of the window as it looks (PrintWindow, Windows' own render of it): only for a shot
Thumb *copy_of(HWND h) {
    RECT r;
    if (!GetWindowRect(h, &r) || IsIconic(h)) return nullptr;
    int w = r.right - r.left, ht = r.bottom - r.top;
    if (w <= 0 || ht <= 0 || w > 8192 || ht > 8192) return nullptr;
    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB };
    void *bits = nullptr;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    Thumb *t = nullptr;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        if (PrintWindow(h, dc, 2 /* PW_RENDERFULLCONTENT */)) {
            t = new Thumb{ 0, L"" };
            t->w = w;
            t->h = ht;
            t->px.assign((const uint32_t *)bits, (const uint32_t *)bits + (size_t)w * ht);
            for (uint32_t &p : t->px) p |= 0xFF000000;
            // one colour all over: Windows had nothing to give (a desktop nobody looks at): the icon instead
            if (std::all_of(t->px.begin(), t->px.end(), [&](uint32_t p) { return p == t->px[0]; })) {
                delete t;
                t = nullptr;
            }
        }
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return t;
}

} // namespace

void switcher_step(int step) {
    if (!g_sw) {
        if (!sw_open(false, false, false)) return;
        int n = (int)g_sw->cards.size();
        // the window in front is the first: one step away is the one used before it
        g_sw->sel = n > 1 ? (step > 0 ? 1 : n - 1) : 0;
        InvalidateRect(g_sw->s.hwnd, NULL, FALSE);
        return;
    }
    sw_move(g_sw, step);
}

void switcher_done() {
    if (g_sw && !g_sw->grid) choose(g_sw->sel);
}

void switcher_cancel() {
    if (g_sw && !g_sw->grid) sw_close();
}

void taskview_toggle() {
    if (g_sw && g_sw->grid) sw_close();
    else sw_open(true, false, true);
}

bool switcher_is_open() { return g_sw != nullptr; }

void switcher_command(const std::wstring &verb, const std::wstring &arg) {
    if (verb == L"switcher") { // Alt+Tab's row, shown at once and kept (tests on their own desktop)
        if (sw_open(false, true, true)) g_sw->sel = g_sw->cards.size() > 1 ? 1 : 0;
    } else if (verb == L"taskview") sw_open(true, true, true);
    else if (verb == L"switchstep") {
        if (g_sw) sw_move(g_sw, _wtoi(arg.c_str()));
    } else if (verb == L"switchdone") {
        if (g_sw) choose(g_sw->sel);
    } else if (verb == L"switcherclose") sw_close();
    else if (verb == L"switchershot") {
        bool ok = false;
        if (g_sw) {
            for (Card &c : g_sw->cards)
                if (!c.copy) c.copy = copy_of(c.hwnd);
            ok = surf_shot(g_sw->s, arg, draw, g_sw);
            for (Card &c : g_sw->cards)
                if (c.copy) {
                    surf_forget(g_sw->s, c.copy);
                    delete c.copy;
                    c.copy = nullptr;
                }
        }
        log_line(std::string("switcher shot ") + (ok ? "ok" : "failed") + (g_sw ? ", " + std::to_string(g_sw->cards.size()) + " windows" : ""));
    }
}
