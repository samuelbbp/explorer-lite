// tray.cpp: the notification area (see tray.h and trayproto.h). What was copied from ManagedShell (Cairo, RetroBar):
// the hidden Shell_TrayWnd with a TrayNotifyWnd child, kept on the taskbar's rectangle and never shown; the wire
// layouts; icons matched by GUID or by window and uID; a modify for an unknown icon fails (apps add it again);
// AllowSetForegroundWindow before every click so the app's menu can come to the front; icons whose window is gone
// dropped; ABM_GETTASKBARPOS written through SHLockShared. Not copied: its WPF collections and default icons, and
// the in-process SysTray object (the volume, network and battery icons: that is quick settings' job, later).
// Which icons are on the bar follows Windows' own choice (HKCU\Control Panel\NotifyIconSettings, IsPromoted) until
// the user moves one with Alt+click (conf tray_shown / tray_hidden).
// https://github.com/cairoshell/ManagedShell/tree/master/src/ManagedShell.WindowsTray
#include "tray.h"
#include "shell.h"
#include "app.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <vector>
#include "conf.h"
#include "gfx.h"
#include "idioma.h"
#include "trayproto.h"

namespace {

// layout, in DIPs
const float kCell = 28, kTrayIcon = 16, kChevronW = 24, kFlyCell = 40, kFlyPad = 8, kGap = 10, kBalloonW = 364, kBalloonPad = 14;
const int kFlyCols = 5;
const UINT_PTR kTimerTip = 1, kTimerBalloon = 2;
enum { H_CHEVRON = -2 };

struct TrayIcon : TrayIconState {
    Thumb *pic = nullptr;   // owned
    std::wstring exe, key;  // key: what the user's choice is saved by (the GUID, or the program's path)
    bool shown = true;      // on the bar; otherwise in the overflow
    D2D1_RECT_F r = {};     // on the bar (DIPs), when shown
};

struct Balloon {
    DWORD hwnd = 0, uid = 0;
    GUID guid = {};
    bool has_guid = false;
    std::wstring title, text;
    DWORD flags = 0;
    Thumb *pic = nullptr;   // owned: the balloon's own icon (NIIF_USER), else the tray icon's at the time
};

struct Fly {
    Surface s;
    std::vector<int> items; // into g_icons
    std::vector<D2D1_RECT_F> rects;
    int sel = -1, hover = -1, press = -1;
    bool sticky = false, keyboard = false, tracking = false;
};

struct BalloonWin {
    Surface s;
    Balloon b;
    float h = 0;
    bool hover_x = false, tracking = false;
};

struct AppBar {
    HWND hwnd;
    UINT callback;
    UINT edge = ABE_BOTTOM;
    RECT rc = {};
    bool placed = false;
};

TrayKind g_kind = TRAY_NONE;
HWND g_tray, g_notify, g_tip;
std::vector<TrayIcon> g_icons;
Surface *g_bar;           // the main bar's surface, where the tray is drawn
D2D1_RECT_F g_area = {}, g_chevron = {};
int g_hover = -1, g_press = -1;
bool g_popup_open;        // NIN_POPUPOPEN sent to the icon under the mouse
struct {
    int idx = -1, button = -1;
    LONG time = 0;
} g_last_down;            // double clicks
Fly *g_fly;
BalloonWin *g_bw;
std::vector<Balloon> g_queue;
std::vector<AppBar> g_appbars;

std::wstring cls_name(const wchar_t *base) { return std::wstring(base) + instance_suffix(); }

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

std::wstring guid_text(const GUID &g) {
    wchar_t b[64] = L"";
    StringFromGUID2(g, b, ARRAYSIZE(b));
    return lower(b);
}

HWND hwnd_of(DWORD h) { return (HWND)(ULONG_PTR)h; }

void drop(Thumb *&t) {
    if (!t) return;
    if (g_bar) surf_forget(*g_bar, t);
    if (g_fly) surf_forget(g_fly->s, t);
    if (g_bw) surf_forget(g_bw->s, t);
    delete t;
    t = nullptr;
}

Thumb *picture(DWORD icon) {
    if (!icon) return nullptr;
    Thumb *t = new Thumb{ 0, L"" };
    if (icon_pixels((HICON)(ULONG_PTR)icon, *t)) return t; // the pixels now: the sender may destroy it right after
    delete t;
    return nullptr;
}

// --- which icons are on the bar ----------------------------------------------------------------------------------

struct WinChoice {
    std::wstring exe; // lower case
    DWORD uid;
    bool has_uid, promoted;
};
std::vector<WinChoice> g_win_choice;

// Windows 11 keeps the user's choice per program in NotifyIconSettings; a path may start with a known folder's GUID
void load_windows_choice() {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        wchar_t name[256];
        DWORD nl = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        wchar_t path[1024] = L"";
        DWORD size = sizeof(path), promoted = 0, uid = 0, dsize = sizeof(DWORD);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, NULL, path, &size) != ERROR_SUCCESS) continue;
        RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, NULL, &promoted, &dsize);
        dsize = sizeof(DWORD);
        bool has_uid = RegGetValueW(root, name, L"UID", RRF_RT_REG_DWORD, NULL, &uid, &dsize) == ERROR_SUCCESS;
        std::wstring p = path;
        if (p.size() > 39 && p[0] == L'{' && p[37] == L'}' && p[38] == L'\\') {
            GUID kf;
            PWSTR base = nullptr;
            if (SUCCEEDED(CLSIDFromString(p.substr(0, 38).c_str(), &kf)) && SUCCEEDED(SHGetKnownFolderPath(kf, 0, NULL, &base)) && base)
                p = std::wstring(base) + p.substr(38);
            CoTaskMemFree(base);
        }
        g_win_choice.push_back({ lower(p), uid, has_uid, promoted != 0 });
    }
    RegCloseKey(root);
}

bool in_list(const char *key, const std::wstring &k) {
    std::wstring all = lower(wide(conf_get(key))) + L"|";
    return !k.empty() && all.find(k + L"|") != std::wstring::npos && (all.find(k + L"|") == 0 || all[all.find(k + L"|") - 1] == L'|');
}

void list_set(const char *key, const std::wstring &k, bool in) {
    std::wstring all = lower(wide(conf_get(key))), out;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(L'|', pos);
        if (end == std::wstring::npos) end = all.size();
        std::wstring item = all.substr(pos, end - pos);
        if (!item.empty() && item != k) out += (out.empty() ? L"" : L"|") + item;
        pos = end + 1;
    }
    if (in) out += (out.empty() ? L"" : L"|") + k;
    conf_set(key, utf8(out));
}

bool decide_shown(const TrayIcon &t) {
    if (in_list("tray_hidden", t.key)) return false;
    if (in_list("tray_shown", t.key)) return true;
    std::wstring exe = lower(t.exe);
    int says = -1;
    for (const WinChoice &c : g_win_choice)
        if (c.exe == exe && (!c.has_uid || c.uid == t.uid || says < 0)) says = c.promoted;
    return says != 0; // a program Windows never saw: on the bar
}

void toggle_shown(int i) {
    TrayIcon &t = g_icons[i];
    t.shown = !t.shown;
    list_set("tray_shown", t.key, t.shown);
    list_set("tray_hidden", t.key, !t.shown);
    log_line("tray: " + utf8(t.tip.substr(0, t.tip.find(L'\n'))) + (t.shown ? " on the bar" : " in the overflow"));
}

bool on_bar(const TrayIcon &t) { return t.shown && !t.hidden; }
bool in_overflow(const TrayIcon &t) { return !t.shown && !t.hidden; }

// --- telling the app ---------------------------------------------------------------------------------------------

void send(const TrayIcon &t, UINT msg, POINT pt) {
    if (!t.callback) return;
    WPARAM wp;
    LPARAM lp;
    tray_callback_args(t, msg, pt.x, pt.y, wp, lp);
    SendNotifyMessageW(hwnd_of(t.hwnd), t.callback, wp, lp); // never waits for a hung app
}

void close_balloon(UINT tell);
void fly_close();
void fly_refresh();
void tip_hide();

void remove_icon(int i) {
    TrayIcon &t = g_icons[i];
    if (g_bw && g_bw->b.hwnd == t.hwnd && g_bw->b.uid == t.uid) close_balloon(0);
    drop(t.pic);
    g_icons.erase(g_icons.begin() + i);
    g_hover = g_press = -1; // the indexes moved
    g_popup_open = false;
    tip_hide();
}

// false (and the icon gone) if its window is
bool alive(int i) {
    if (i < 0 || i >= (int)g_icons.size()) return false;
    if (IsWindow(hwnd_of(g_icons[i].hwnd))) return true;
    remove_icon(i);
    fly_refresh();
    shell_tray_changed();
    return false;
}

void click(int i, int button, bool up, POINT pt) {
    if (!alive(i)) return;
    TrayIcon &t = g_icons[i];
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd_of(t.hwnd), &pid);
    AllowSetForegroundWindow(pid); // its menu or window may come to the front
    bool dbl = false;
    if (!up) {
        LONG now = GetMessageTime();
        dbl = g_last_down.idx == i && g_last_down.button == button && (DWORD)(now - g_last_down.time) <= GetDoubleClickTime();
        g_last_down = { dbl ? -1 : i, button, now };
    }
    UINT m[2];
    int n = tray_click_messages(t.version, button, up, dbl, m);
    for (int k = 0; k < n; k++) send(t, m[k], pt);
}

void key(int i, bool menu) {
    if (!alive(i)) return;
    TrayIcon &t = g_icons[i];
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd_of(t.hwnd), &pid);
    AllowSetForegroundWindow(pid);
    POINT pt;
    GetCursorPos(&pt);
    UINT m[2];
    int n = tray_key_messages(t.version, menu, m);
    for (int k = 0; k < n; k++) send(t, m[k], pt);
}

// --- the tooltip (Windows' own control, at the icon) --------------------------------------------------------------

RECT bar_screen(D2D1_RECT_F r) {
    RECT wr = {};
    if (g_bar) GetWindowRect(g_bar->hwnd, &wr);
    float k = g_bar ? surf_scale(*g_bar) : 1;
    return { wr.left + (LONG)(r.left * k), wr.top + (LONG)(r.top * k), wr.left + (LONG)(r.right * k), wr.top + (LONG)(r.bottom * k) };
}

void tip_hide() {
    if (!g_tip) return;
    TTTOOLINFOW ti = { sizeof(ti) };
    ti.hwnd = g_tray;
    ti.uId = 1;
    SendMessageW(g_tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

void tip_show(const std::wstring &text, RECT at) {
    TTTOOLINFOW ti = { sizeof(ti) };
    ti.hwnd = g_tray;
    ti.uId = 1;
    if (!g_tip) {
        g_tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0,
                                0, 0, g_tray, NULL, g_inst, NULL);
        if (!g_tip) return;
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.lpszText = (LPWSTR)L"";
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 480); // the tip's own line breaks
    }
    ti.lpszText = (LPWSTR)text.c_str();
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    DWORD size = (DWORD)SendMessageW(g_tip, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
    int w = LOWORD(size), h = HIWORD(size);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&at, MONITOR_DEFAULTTONEAREST), &mi);
    int x = std::max((int)mi.rcMonitor.left, std::min((int)mi.rcMonitor.right - w, (int)(at.left + at.right) / 2 - w / 2));
    SendMessageW(g_tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, at.top - h - 6));
    SendMessageW(g_tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

void set_hover(int i) {
    if (i == g_hover) return;
    POINT pt;
    GetCursorPos(&pt);
    if (g_hover >= 0 && g_hover < (int)g_icons.size() && g_popup_open) send(g_icons[g_hover], kNinPopupClose, pt);
    g_popup_open = false;
    tip_hide();
    KillTimer(g_tray, kTimerTip);
    g_hover = i;
    if (i >= 0) SetTimer(g_tray, kTimerTip, 500, NULL);
    if (g_bar) InvalidateRect(g_bar->hwnd, NULL, FALSE);
}

void on_tip_timer() {
    KillTimer(g_tray, kTimerTip);
    if (!alive(g_hover)) return;
    TrayIcon &t = g_icons[g_hover];
    POINT pt;
    GetCursorPos(&pt);
    if (t.version >= 4 && !t.show_tip) { // version 4 draws its own popup unless it asked for the standard tip
        send(t, kNinPopupOpen, pt);
        g_popup_open = true;
    } else if (!t.tip.empty()) tip_show(t.tip, bar_screen(t.r));
}

// --- the overflow --------------------------------------------------------------------------------------------------

std::wstring fly_class() { return cls_name(L"ExplorerLiteTrayOverflow"); }

void fly_draw(void *ctx) {
    Fly *f = (Fly *)ctx;
    Surface &s = f->s;
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    float k = surf_scale(s), w = rc.right / k, h = rc.bottom / k;
    surf_fill(s, D2D1::RectF(0, 0, w, h), kPane);
    for (size_t n = 0; n < f->items.size(); n++) {
        D2D1_RECT_F r = f->rects[n];
        bool sel = f->keyboard && (int)n == f->sel;
        if (sel || (int)n == f->hover) surf_round(s, r, 4, f->press == (int)n ? kPress : sel ? kSelect : kHover);
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, ic = kTrayIcon + 4;
        int i = f->items[n];
        if (i < (int)g_icons.size()) surf_icon(s, g_icons[i].pic, D2D1::RectF(cx - ic / 2, cy - ic / 2, cx + ic / 2, cy + ic / 2));
    }
    s.br->SetColor(color(kLine));
    s.dc->DrawRectangle(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), s.br, 1);
}

int fly_hit(Fly *f, float x, float y) {
    for (size_t n = 0; n < f->rects.size(); n++)
        if (surf_inside(f->rects[n], x, y)) return (int)n;
    return -1;
}

// what it shows, its size and its place (above the chevron, or the tray for the keyboard)
void fly_layout(Fly *f) {
    f->items.clear();
    f->rects.clear();
    for (int i = 0; i < (int)g_icons.size(); i++)
        if (f->keyboard ? !g_icons[i].hidden : in_overflow(g_icons[i])) f->items.push_back(i);
    int n = (int)f->items.size(), cols = std::max(1, std::min(kFlyCols, n)), rows = std::max(1, (n + cols - 1) / cols);
    for (int j = 0; j < n; j++) {
        float x = kFlyPad + (j % cols) * kFlyCell, y = kFlyPad + (j / cols) * kFlyCell;
        f->rects.push_back(D2D1::RectF(x + 2, y + 2, x + kFlyCell - 2, y + kFlyCell - 2));
    }
    if (f->sel >= n) f->sel = n - 1;
    if (f->sel < 0 && n) f->sel = 0;
    float k = surf_scale(f->s);
    int w = (int)((cols * kFlyCell + 2 * kFlyPad) * k), h = (int)((rows * kFlyCell + 2 * kFlyPad) * k), gap = (int)(kGap * k);
    RECT anchor = bar_screen(g_chevron.right > g_chevron.left && !f->keyboard ? g_chevron : g_area);
    RECT bar = bar_screen(D2D1::RectF(0, 0, 0, 0));
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = std::max((int)mi.rcMonitor.left + gap, std::min((int)mi.rcMonitor.right - w - gap, (int)(anchor.left + anchor.right) / 2 - w / 2));
    SetWindowPos(f->s.hwnd, HWND_TOPMOST, x, bar.top - gap - h, w, h, SWP_NOACTIVATE);
    InvalidateRect(f->s.hwnd, NULL, FALSE);
}

void fly_close() {
    Fly *f = g_fly;
    if (!f) return;
    g_fly = nullptr;
    surf_free(f->s);
    DestroyWindow(f->s.hwnd);
    delete f;
    if (g_bar) InvalidateRect(g_bar->hwnd, NULL, FALSE);
}

void fly_refresh() {
    if (!g_fly) return;
    fly_layout(g_fly);
    if (g_fly->items.empty()) fly_close();
}

LRESULT CALLBACK fly_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Fly *f = g_fly && g_fly->s.hwnd == h ? g_fly : nullptr;
    if (!f) return DefWindowProcW(h, msg, wp, lp);
    float k = surf_scale(f->s);
    float x = GET_X_LPARAM(lp) / k, y = GET_Y_LPARAM(lp) / k;
    POINT pt;
    GetCursorPos(&pt);
    switch (msg) {
    case WM_PAINT: surf_paint(f->s, fly_draw, f); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !f->sticky) PostMessageW(h, WM_CLOSE, 0, 0); // clicked elsewhere, or a menu came
        return 0;
    case WM_CLOSE: fly_close(); return 0;
    case WM_MOUSEMOVE: {
        int n = fly_hit(f, x, y);
        if (n != f->hover) {
            f->hover = n;
            InvalidateRect(h, NULL, FALSE);
        }
        if (n >= 0 && f->items[n] < (int)g_icons.size()) send(g_icons[f->items[n]], WM_MOUSEMOVE, pt);
        if (!f->tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            f->tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        f->tracking = false;
        f->hover = f->press = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int button = msg == WM_LBUTTONDOWN ? 0 : msg == WM_MBUTTONDOWN ? 1 : 2;
        f->press = fly_hit(f, x, y);
        InvalidateRect(h, NULL, FALSE);
        if (f->press >= 0 && !(GetKeyState(VK_MENU) & 0x8000)) click(f->items[f->press], button, false, pt);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        int button = msg == WM_LBUTTONUP ? 0 : msg == WM_MBUTTONUP ? 1 : 2;
        int n = fly_hit(f, x, y);
        bool same = n >= 0 && n == f->press;
        f->press = -1;
        InvalidateRect(h, NULL, FALSE);
        if (!same) return 0;
        int i = f->items[n];
        if (GetKeyState(VK_MENU) & 0x8000) { // Alt+click: onto the bar
            toggle_shown(i);
            shell_tray_changed();
            fly_refresh();
        } else click(i, button, true, pt);
        return 0;
    }
    case WM_KEYDOWN: {
        int n = (int)f->items.size(), cols = std::max(1, std::min(kFlyCols, n));
        f->keyboard = true;
        switch (wp) {
        case VK_ESCAPE: fly_close(); return 0;
        case VK_LEFT: f->sel = std::max(0, f->sel - 1); break;
        case VK_RIGHT: f->sel = std::min(n - 1, f->sel + 1); break;
        case VK_UP: f->sel = std::max(0, f->sel - cols); break;
        case VK_DOWN: f->sel = std::min(n - 1, f->sel + cols); break;
        case VK_RETURN:
        case VK_SPACE:
            if (f->sel >= 0 && f->sel < n) key(f->items[f->sel], false);
            return 0;
        case VK_APPS:
        case VK_F10:
            if (wp == VK_F10 && !(GetKeyState(VK_SHIFT) & 0x8000)) break;
            if (f->sel >= 0 && f->sel < n) key(f->items[f->sel], true);
            return 0;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)) { // Shift+F10: the menu
            if (f->sel >= 0 && f->sel < (int)f->items.size()) key(f->items[f->sel], true);
            return 0;
        }
        break;
    case WM_DPICHANGED:
        f->s.dpi = HIWORD(wp);
        fly_layout(f);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void fly_open(bool keyboard, bool sticky) {
    if (g_fly) {
        fly_close();
        return;
    }
    if (!g_bar) return;
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = fly_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        std::wstring cls = fly_class();
        wc.lpszClassName = cls.c_str();
        registered = RegisterClassExW(&wc) != 0;
    }
    Fly *f = new Fly;
    f->keyboard = keyboard;
    f->sticky = sticky;
    g_fly = f;
    f->s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, fly_class().c_str(), L"Notification area overflow", WS_POPUP, 0, 0, 10, 10, NULL, NULL,
                                g_inst, NULL);
    if (!f->s.hwnd) {
        g_fly = nullptr;
        delete f;
        return;
    }
    f->s.dpi = GetDpiForWindow(f->s.hwnd);
    DWORD round = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(f->s.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    surf_formats(f->s);
    fly_layout(f);
    if (f->items.empty()) {
        fly_close();
        return;
    }
    // the place may have a different DPI than the window was made with
    UINT dpi = GetDpiForWindow(f->s.hwnd);
    if (dpi != f->s.dpi) {
        f->s.dpi = dpi;
        fly_layout(f);
    }
    ShowWindow(f->s.hwnd, SW_SHOW);
    SetForegroundWindow(f->s.hwnd);
    if (g_bar) InvalidateRect(g_bar->hwnd, NULL, FALSE);
}

// --- balloons ----------------------------------------------------------------------------------------------------

std::wstring balloon_class() { return cls_name(L"ExplorerLiteBalloon"); }

int icon_of(const Balloon &b) {
    for (int i = 0; i < (int)g_icons.size(); i++) {
        const TrayIcon &t = g_icons[i];
        if (b.has_guid ? t.has_guid && !memcmp(&t.guid, &b.guid, sizeof(GUID)) : t.hwnd == b.hwnd && t.uid == b.uid) return i;
    }
    return -1;
}

D2D1_RECT_F balloon_x() { return D2D1::RectF(kBalloonW - kBalloonPad - 24, 8, kBalloonW - 8, 32); }

void balloon_draw(void *ctx) {
    BalloonWin *w = (BalloonWin *)ctx;
    Surface &s = w->s;
    surf_fill(s, D2D1::RectF(0, 0, kBalloonW, w->h), kPane);
    float x = kBalloonPad, y = kBalloonPad;
    DWORD kind = w->b.flags & kNiifIconMask;
    D2D1_RECT_F ir = D2D1::RectF(x, y, x + 32, y + 32);
    if (kind == kNiifInfo) surf_text(s, L"\xE946", ir, s.f_icon, kAccent);
    else if (kind == kNiifWarning) surf_text(s, L"\xE7BA", ir, s.f_icon, 0xE8A317);
    else if (kind == kNiifError) surf_text(s, L"\xEA39", ir, s.f_icon, kDanger);
    else surf_icon(s, w->b.pic, D2D1::RectF(x + 4, y + 4, x + 28, y + 28));
    float tx = x + 44, tw = kBalloonW - tx - kBalloonPad - 24;
    float ty = y;
    if (!w->b.title.empty()) {
        surf_text(s, w->b.title, D2D1::RectF(tx, ty, tx + tw, ty + 20), s.f_bold, kText);
        ty += 24;
    }
    surf_text(s, w->b.text, D2D1::RectF(tx, ty, tx + tw + 20, w->h - kBalloonPad), s.f_wrap, kText);
    D2D1_RECT_F xr = balloon_x();
    if (w->hover_x) surf_round(s, xr, 4, kHover);
    surf_text(s, L"\xE711", xr, s.f_icon, kDim);
    s.br->SetColor(color(kLine));
    s.dc->DrawRectangle(D2D1::RectF(0.5f, 0.5f, kBalloonW - 0.5f, w->h - 0.5f), s.br, 1);
}

void tell_balloon(const Balloon &b, UINT what) {
    int i = icon_of(b);
    POINT pt = {};
    if (i >= 0) send(g_icons[i], what, pt);
}

void show_next_balloon();

// tell: what the app hears (NIN_BALLOONTIMEOUT, NIN_BALLOONUSERCLICK, NIN_BALLOONHIDE), 0 for nothing
void close_balloon(UINT tell) {
    BalloonWin *w = g_bw;
    if (!w) return;
    g_bw = nullptr;
    KillTimer(g_tray, kTimerBalloon);
    if (tell) tell_balloon(w->b, tell);
    surf_free(w->s);
    DestroyWindow(w->s.hwnd);
    drop(w->b.pic);
    delete w;
    show_next_balloon();
}

void balloon_timer() {
    UINT secs = 0;
    SystemParametersInfoW(SPI_GETMESSAGEDURATION, 0, &secs, 0); // Windows' "show notifications for"
    SetTimer(g_tray, kTimerBalloon, std::max(7u, secs) * 1000, NULL);
}

LRESULT CALLBACK balloon_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    BalloonWin *w = g_bw && g_bw->s.hwnd == h ? g_bw : nullptr;
    if (!w) return DefWindowProcW(h, msg, wp, lp);
    float k = surf_scale(w->s);
    switch (msg) {
    case WM_PAINT: surf_paint(w->s, balloon_draw, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // never takes the focus
    case WM_MOUSEMOVE: {
        bool on = surf_inside(balloon_x(), GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k);
        if (on != w->hover_x) {
            w->hover_x = on;
            InvalidateRect(h, NULL, FALSE);
        }
        balloon_timer(); // read: it stays while the mouse is on it
        if (!w->tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            w->tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        w->tracking = false;
        w->hover_x = false;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        bool x = surf_inside(balloon_x(), GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k);
        if (!x) { // the app opens what the balloon was about: it may come to the front
            int i = icon_of(w->b);
            DWORD pid = 0;
            if (i >= 0) GetWindowThreadProcessId(hwnd_of(g_icons[i].hwnd), &pid);
            if (pid) AllowSetForegroundWindow(pid);
        }
        close_balloon(x ? kNinBalloonTimeout : kNinBalloonUserClick);
        return 0;
    }
    case WM_RBUTTONUP: close_balloon(kNinBalloonTimeout); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void show_next_balloon() {
    if (g_bw || g_queue.empty() || !g_bar || !g_tray) return;
    if (shell_fullscreen(MonitorFromWindow(g_bar->hwnd, MONITOR_DEFAULTTOPRIMARY))) return; // after the game or the video
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = balloon_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_HAND);
        std::wstring cls = balloon_class();
        wc.lpszClassName = cls.c_str();
        registered = RegisterClassExW(&wc) != 0;
    }
    BalloonWin *w = new BalloonWin;
    w->b = g_queue.front();
    g_queue.erase(g_queue.begin());
    w->s.dpi = g_bar->dpi;
    surf_formats(w->s);
    float tw = kBalloonW - kBalloonPad * 2 - 44 - 4;
    float text_h = surf_text_height(w->b.text, w->s.f_wrap, tw);
    w->h = std::max(64.f, kBalloonPad * 2 + (w->b.title.empty() ? 0 : 24) + std::min(text_h, 160.f) + 4);
    float k = surf_scale(w->s);
    RECT bar = {};
    GetWindowRect(g_bar->hwnd, &bar);
    int cw = (int)(kBalloonW * k), ch = (int)(w->h * k), gap = (int)(kGap * k);
    int x = bar.right - cw - gap;
    g_bw = w;
    w->s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, balloon_class().c_str(), L"Notification", WS_POPUP, x,
                                bar.top - gap - ch, cw, ch, NULL, NULL, g_inst, NULL);
    if (!w->s.hwnd) {
        g_bw = nullptr;
        surf_free(w->s);
        drop(w->b.pic);
        delete w;
        return;
    }
    DWORD round = 2;
    DwmSetWindowAttribute(w->s.hwnd, 33, &round, sizeof(round));
    ShowWindow(w->s.hwnd, SW_SHOWNOACTIVATE); // and no sound, whatever the app asked for
    balloon_timer();
    tell_balloon(w->b, kNinBalloonShow);
    log_line("tray: balloon " + utf8(w->b.title));
}

// a balloon asked for (NIF_INFO with text); NIF_REALTIME ones are dropped if they cannot show now
void queue_balloon(const TrayIcon &t, const TrayData &d) {
    Balloon b;
    b.hwnd = t.hwnd;
    b.uid = t.uid;
    b.guid = t.guid;
    b.has_guid = t.has_guid;
    b.title = d.info_title;
    b.text = d.info;
    b.flags = d.info_flags;
    if ((d.info_flags & kNiifIconMask) == kNiifUser) b.pic = picture(d.balloon_icon ? d.balloon_icon : d.icon);
    if (!b.pic && t.pic) b.pic = new Thumb(*t.pic);
    bool busy = g_bw || shell_fullscreen(g_bar ? MonitorFromWindow(g_bar->hwnd, MONITOR_DEFAULTTOPRIMARY) : NULL);
    if ((d.flags & kNifRealtime) && busy) {
        drop(b.pic);
        return;
    }
    // a new one from the same icon replaces the one it had waiting
    for (size_t i = 0; i < g_queue.size(); i++)
        if (g_queue[i].hwnd == b.hwnd && g_queue[i].uid == b.uid) {
            drop(g_queue[i].pic);
            g_queue.erase(g_queue.begin() + i);
            break;
        }
    g_queue.push_back(b);
    if (g_bw && g_bw->b.hwnd == b.hwnd && g_bw->b.uid == b.uid) close_balloon(kNinBalloonHide); // replaced on screen
    else show_next_balloon();
}

void forget_balloons(const TrayIcon &t, bool tell) {
    for (size_t i = 0; i < g_queue.size();)
        if (g_queue[i].hwnd == t.hwnd && g_queue[i].uid == t.uid) {
            drop(g_queue[i].pic);
            g_queue.erase(g_queue.begin() + i);
        } else i++;
    if (g_bw && g_bw->b.hwnd == t.hwnd && g_bw->b.uid == t.uid) close_balloon(tell ? kNinBalloonHide : 0);
}

// --- the messages: Shell_NotifyIcon, its rectangle, appbars ---------------------------------------------------------

LRESULT on_notify(const void *data, size_t n) {
    TrayData d;
    if (!tray_parse(data, n, d)) return FALSE;
    if (d.msg == kNimDelete) // its picture and balloons first: tray_apply only takes it out of the list
        for (int i = 0; i < (int)g_icons.size(); i++)
            if (tray_same(g_icons[i], d)) {
                forget_balloons(g_icons[i], false);
                drop(g_icons[i].pic);
                g_hover = g_press = -1; // the indexes move
                g_popup_open = false;
                tip_hide();
                break;
            }
    bool was_bar = false, was_hidden = false;
    for (const TrayIcon &t : g_icons)
        if (tray_same(t, d)) was_bar = on_bar(t), was_hidden = t.hidden;
    TrayChange c = tray_apply(g_icons, d);
    if (!c.ok) return FALSE;
    if (c.removed) {
        fly_refresh();
        shell_tray_changed();
        return TRUE;
    }
    if (c.idx < 0) return TRUE;
    TrayIcon &t = g_icons[c.idx];
    if (c.added) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd_of(t.hwnd), &pid);
        t.exe = shell_process_path(pid);
        t.key = t.has_guid ? guid_text(t.guid) : lower(t.exe);
        t.shown = decide_shown(t);
        log_line("tray: " + utf8(t.tip.substr(0, t.tip.find(L'\n'))) + " added (" + utf8(t.exe.substr(t.exe.rfind(L'\\') + 1)) + (t.shown ? ")" : ", overflow)"));
    }
    if (c.icon) {
        drop(t.pic);
        t.pic = picture(d.icon);
    }
    if (c.balloon) queue_balloon(t, d);
    else if (c.balloon_hide) forget_balloons(t, true);
    if (g_hover == c.idx && (d.flags & kNifTip) && g_tip && IsWindowVisible(g_tip)) tip_show(t.tip, bar_screen(t.r));
    if (c.added || on_bar(t) != was_bar || t.hidden != was_hidden) {
        fly_refresh();
        shell_tray_changed();
    } else if (c.icon) { // only its picture (an animation, a badge): only the bar it is on
        if (g_bar && on_bar(t)) InvalidateRect(g_bar->hwnd, NULL, FALSE);
        if (g_fly) InvalidateRect(g_fly->s.hwnd, NULL, FALSE);
    }
    return TRUE;
}

LRESULT on_rect(const void *data, size_t n) {
    TrayRectQuery q;
    if (!tray_parse_rect(data, n, q)) return 0;
    static const GUID none = {};
    bool by_guid = memcmp(&q.guid, &none, sizeof(GUID)) != 0;
    for (const TrayIcon &t : g_icons) {
        if (by_guid ? !(t.has_guid && !memcmp(&t.guid, &q.guid, sizeof(GUID))) : !(t.hwnd == q.hwnd && t.uid == q.uid)) continue;
        // on the bar: its own place; in the overflow: the chevron's (or the tray's, with none)
        RECT r = bar_screen(on_bar(t) ? t.r : g_chevron.right > g_chevron.left ? g_chevron : g_area);
        // 1: the top-left corner; 2: the size (Shell_NotifyIconGetRect adds it to the corner itself)
        return q.msg == 2 ? MAKELONG(r.right - r.left, r.bottom - r.top) : MAKELONG(r.left, r.top);
    }
    return 0;
}

typedef void *(WINAPI *PLockShared)(HANDLE, DWORD);
typedef BOOL(WINAPI *PUnlockShared)(void *);
typedef HANDLE(WINAPI *PAllocShared)(const void *, DWORD, DWORD);
typedef BOOL(WINAPI *PFreeShared)(HANDLE, DWORD);

// shlwapi's shared memory (SHAppBarMessage's way of passing the rectangle): by name, or by its old ordinal
FARPROC shlwapi_fn(const char *name, int ordinal) {
    HMODULE m = GetModuleHandleW(L"shlwapi.dll");
    if (!m) m = LoadLibraryExW(L"shlwapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return nullptr;
    FARPROC f = GetProcAddress(m, name);
    return f ? f : GetProcAddress(m, (LPCSTR)(ULONG_PTR)ordinal);
}

// the APPBARDATA the caller passed (40 bytes: cbSize, hWnd, uCallbackMessage, uEdge, rc, lParam), to read and write
bool with_shared(const AppBarMsg &m, void (*fn)(BYTE *abd, void *ctx), void *ctx) {
    static auto lock = (PLockShared)(void *)shlwapi_fn("SHLockShared", 8);
    static auto unlock = (PUnlockShared)(void *)shlwapi_fn("SHUnlockShared", 9);
    if (!lock || !unlock || !m.shared) return false;
    BYTE *p = (BYTE *)lock((HANDLE)(ULONG_PTR)m.shared, m.pid);
    if (!p) return false;
    fn(p, ctx);
    unlock(p);
    return true;
}

AppBar *appbar_of(HWND h) {
    for (AppBar &a : g_appbars)
        if (a.hwnd == h) return &a;
    return nullptr;
}

void appbars_sweep() {
    g_appbars.erase(std::remove_if(g_appbars.begin(), g_appbars.end(), [](const AppBar &a) { return !IsWindow(a.hwnd); }), g_appbars.end());
}

// The rectangle an appbar may have on its edge: past the taskbar and the appbars already on that edge, as explorer
// does it (only the docked side moves; the app sets the other from its own size).
void appbar_fit(HWND self, UINT edge, RECT &rc) {
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT bar = shell_bar_rect(mon);
    RECT free_rc = mi.rcMonitor;
    free_rc.bottom = bar.top;
    for (const AppBar &a : g_appbars) {
        if (a.hwnd == self) break; // only those registered before it
        if (!a.placed || MonitorFromRect(&a.rc, MONITOR_DEFAULTTONEAREST) != mon) continue;
        if (a.edge == ABE_TOP) free_rc.top = std::max(free_rc.top, a.rc.bottom);
        else if (a.edge == ABE_BOTTOM) free_rc.bottom = std::min(free_rc.bottom, a.rc.top);
        else if (a.edge == ABE_LEFT) free_rc.left = std::max(free_rc.left, a.rc.right);
        else if (a.edge == ABE_RIGHT) free_rc.right = std::min(free_rc.right, a.rc.left);
    }
    switch (edge) {
    case ABE_TOP: rc.top = std::max(rc.top, free_rc.top); break;
    case ABE_BOTTOM: rc.bottom = std::min(rc.bottom, free_rc.bottom); break;
    case ABE_LEFT: rc.left = std::max(rc.left, free_rc.left); break;
    case ABE_RIGHT: rc.right = std::min(rc.right, free_rc.right); break;
    }
    if (edge == ABE_LEFT || edge == ABE_RIGHT) rc.top = std::max(rc.top, free_rc.top), rc.bottom = std::min(rc.bottom, free_rc.bottom);
}

void appbars_tell(HWND except) {
    for (const AppBar &a : g_appbars)
        if (a.hwnd != except && a.callback) PostMessageW(a.hwnd, a.callback, ABN_POSCHANGED, 0);
}

struct PosCtx {
    HWND self;
    UINT edge;
    bool set;
};

LRESULT on_appbar(const void *data, size_t n) {
    AppBarMsg m;
    if (!tray_parse_appbar(data, n, m)) return 0;
    HWND h = hwnd_of(m.hwnd);
    appbars_sweep();
    switch (m.msg) {
    case ABM_NEW:
        if (appbar_of(h) || !IsWindow(h)) return FALSE;
        g_appbars.push_back({ h, m.callback });
        return TRUE;
    case ABM_REMOVE: {
        bool was = appbar_of(h) != nullptr;
        g_appbars.erase(std::remove_if(g_appbars.begin(), g_appbars.end(), [h](const AppBar &a) { return a.hwnd == h; }), g_appbars.end());
        if (was) {
            shell_work_area_update();
            appbars_tell(NULL);
        }
        return TRUE;
    }
    case ABM_QUERYPOS:
    case ABM_SETPOS: {
        if (!appbar_of(h)) return FALSE;
        PosCtx ctx = { h, m.edge, m.msg == ABM_SETPOS };
        bool ok = with_shared(m, [](BYTE *abd, void *c) {
            PosCtx *x = (PosCtx *)c;
            RECT rc;
            memcpy(&rc, abd + 16, 16);
            appbar_fit(x->self, x->edge, rc);
            memcpy(abd + 16, &rc, 16);
            if (x->set)
                if (AppBar *a = appbar_of(x->self)) a->edge = x->edge, a->rc = rc, a->placed = true;
        }, &ctx);
        if (ok && ctx.set) {
            shell_work_area_update();
            appbars_tell(h);
        }
        return ok;
    }
    case ABM_GETSTATE: return ABS_ALWAYSONTOP; // never hides itself
    case ABM_GETTASKBARPOS:
        return with_shared(m, [](BYTE *abd, void *) {
            RECT rc = shell_bar_rect(shell_primary());
            DWORD edge = ABE_BOTTOM, me = (DWORD)(ULONG_PTR)g_tray;
            memcpy(abd + 4, &me, 4);
            memcpy(abd + 12, &edge, 4);
            memcpy(abd + 16, &rc, 16);
        }, nullptr);
    case ABM_ACTIVATE:
    case ABM_WINDOWPOSCHANGED: return TRUE;
    case ABM_GETAUTOHIDEBAR:
    case 11 /* ABM_GETAUTOHIDEBAREX */: return 0;
    case ABM_SETAUTOHIDEBAR:
    case 12 /* ABM_SETAUTOHIDEBAREX */: return FALSE; // no auto-hide bars yet
    case ABM_SETSTATE: return 0;
    }
    return 0;
}

LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COPYDATA: {
        auto *cd = (COPYDATASTRUCT *)lp;
        if (!cd || !cd->lpData || cd->cbData > 64 * 1024) return FALSE;
        switch (cd->dwData) {
        case kTrayNotify: return on_notify(cd->lpData, cd->cbData);
        case kTrayIconRect: return on_rect(cd->lpData, cd->cbData);
        case kTrayAppBar: return on_appbar(cd->lpData, cd->cbData);
        }
        return FALSE;
    }
    case WM_TIMER:
        if (wp == kTimerTip) on_tip_timer();
        else if (wp == kTimerBalloon) close_balloon(kNinBalloonTimeout);
        return 0;
    case WM_WINDOWPOSCHANGING: // apps may show the taskbar they found: it stays hidden (the bars are the taskbar)
        ((WINDOWPOS *)lp)->flags &= ~SWP_SHOWWINDOW;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --- traytest: the protocol checked with crafted buffers, on a window of its own (`--shell --cmd traytest`) ---------

std::vector<std::pair<WPARAM, LPARAM>> g_heard; // what the test's icon window was told
const UINT kTestCallback = WM_APP + 50;
HWND g_test_wnd;

LRESULT CALLBACK test_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kTestCallback) {
        g_heard.push_back({ wp, lp });
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

std::vector<BYTE> wire(DWORD msg, DWORD size, DWORD hwnd, DWORD uid, DWORD flags, HICON icon, const wchar_t *tip, const wchar_t *info = L"",
                       const wchar_t *title = L"", DWORD ver = 0, const GUID *guid = nullptr, DWORD state = 0, DWORD mask = 0) {
    std::vector<BYTE> b(8 + 956, 0);
    DWORD sig = kTraySignature, cb = kTestCallback, ic = (DWORD)(ULONG_PTR)icon;
    auto put = [&](size_t off, const void *v, size_t n) { memcpy(b.data() + off, v, n); };
    auto text = [&](size_t off, const wchar_t *t, size_t chars) { memcpy(b.data() + 8 + off, t, std::min(wcslen(t), chars) * 2); };
    put(0, &sig, 4);
    put(4, &msg, 4);
    put(8, &size, 4);
    put(12, &hwnd, 4);
    put(16, &uid, 4);
    put(20, &flags, 4);
    put(24, &cb, 4);
    put(28, &ic, 4);
    text(24, tip, size < 288 ? 64 : 128);
    put(8 + 280, &state, 4);
    put(8 + 284, &mask, 4);
    text(288, info, 256);
    put(8 + 800, &ver, 4);
    text(804, title, 64);
    if (guid) put(8 + 936, guid, 16);
    return b;
}

LRESULT deliver(ULONG_PTR kind, const std::vector<BYTE> &b, size_t n = 0) {
    COPYDATASTRUCT cds = { kind, (DWORD)(n ? n : b.size()), (void *)b.data() };
    return SendMessageW(g_tray, WM_COPYDATA, (WPARAM)g_test_wnd, (LPARAM)&cds);
}

void tray_test() {
    int failed = 0, checks = 0;
    std::string fails;
    auto check = [&](bool ok, const char *what) {
        checks++;
        if (!ok) failed++, fails += std::string("; ") + what;
    };
    if (!g_test_wnd) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = test_proc;
        wc.hInstance = g_inst;
        std::wstring cls = cls_name(L"ExplorerLiteTrayTest");
        wc.lpszClassName = cls.c_str();
        RegisterClassExW(&wc);
        g_test_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls.c_str(), L"traytest", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    }
    DWORD me = (DWORD)(ULONG_PTR)g_test_wnd;
    const GUID ga = { 0x6a1b6f2e, 0x51c4, 0x4c4a, { 0x9e, 0x52, 0x1b, 0x6d, 0x8a, 0x0c, 0x3f, 0x01 } };
    const GUID gb = { 0x6a1b6f2e, 0x51c4, 0x4c4a, { 0x9e, 0x52, 0x1b, 0x6d, 0x8a, 0x0c, 0x3f, 0x02 } };
    HICON app = LoadIconW(g_inst, MAKEINTRESOURCEW(1)), info = LoadIconW(NULL, IDI_INFORMATION), shield = LoadIconW(NULL, IDI_SHIELD);
    size_t base = g_icons.size();

    // the parser alone: what a short, cut or forged buffer gives
    TrayData d;
    auto v1 = wire(kNimAdd, 152, me, 1, kNifTip | kNifInfo | kNifGuid | kNifState, app, L"0123456789012345678901234567890123456789012345678901234567890123456789");
    check(tray_parse(v1.data(), v1.size(), d) && d.tip.size() == 64 && d.flags == kNifTip, "v1 struct: tip 64, newer flags dropped");
    check(tray_parse(v1.data(), 8 + 24 + 10, d) && d.tip == L"01234", "cut buffer: tip bounded by it");
    check(!tray_parse(v1.data(), 20, d), "too short");
    auto bad = v1;
    bad[0] ^= 1;
    check(!tray_parse(bad.data(), bad.size(), d), "bad signature");
    auto v2 = wire(kNimAdd, 936, me, 1, kNifGuid | kNifTip, app, L"x", L"", L"", 0, &ga);
    check(tray_parse(v2.data(), v2.size(), d) && !(d.flags & kNifGuid), "GUID flag without a GUID");
    auto zero = wire(kNimAdd, 0, me, 1, kNifTip, app, L"x");
    check(!tray_parse(zero.data(), zero.size(), d), "cbSize 0");

    // through the window, as Shell_NotifyIcon sends it
    check(deliver(kTrayNotify, wire(kNimAdd, 956, me, 1, kNifMessage | kNifIcon | kNifTip, app, L"explorer-lite test\nversion 0")) == TRUE, "add");
    check(g_icons.size() == base + 1 && g_icons.back().pic, "added with its picture");
    check(deliver(kTrayNotify, wire(kNimModify, 956, me, 9, kNifTip, NULL, L"nobody")) == FALSE, "modify of an unknown icon fails");
    check(deliver(kTrayNotify, wire(kNimAdd, 956, 0, 2, kNifTip, NULL, L"no window")) == FALSE, "add without a window fails");
    check(deliver(kTrayNotify, wire(kNimAdd, 956, me, 7, kNifMessage | kNifIcon | kNifTip | kNifGuid | kNifShowTip, info, L"Version 4, GUID", L"", L"", 0, &ga)) ==
              TRUE, "add with a GUID");
    check(deliver(kTrayNotify, wire(kNimSetVersion, 956, me, 7, kNifGuid, NULL, L"", L"", L"", 4, &ga)) == TRUE, "set version 4");
    check(deliver(kTrayNotify, wire(kNimSetVersion, 956, me, 7, kNifGuid, NULL, L"", L"", L"", 5, &ga)) == FALSE, "version 5 refused");
    check(deliver(kTrayNotify, wire(kNimAdd, 956, me, 3, kNifMessage | kNifIcon | kNifTip | kNifGuid, shield, L"Kept in the overflow", L"", L"", 0, &gb)) == TRUE, "third icon");
    check(deliver(kTrayNotify, wire(kNimAdd, 956, me, 4, kNifMessage | kNifTip | kNifState, NULL, L"hidden by its app", L"", L"", 0, nullptr, kNisHidden, kNisHidden)) == TRUE,
          "hidden icon");
    int a = -1, b = -1, c = -1, hid = -1;
    for (int i = 0; i < (int)g_icons.size(); i++) {
        if (g_icons[i].hwnd != me) continue;
        if (g_icons[i].uid == 1) a = i;
        if (g_icons[i].uid == 7) b = i;
        if (g_icons[i].uid == 3) c = i;
        if (g_icons[i].uid == 4) hid = i;
    }
    check(a >= 0 && b >= 0 && c >= 0 && hid >= 0 && g_icons[b].version == 4 && g_icons[b].has_guid && g_icons[hid].hidden, "the list after the adds");
    if (a < 0 || b < 0 || c < 0 || hid < 0) {
        log_line("traytest: FAILED, " + std::to_string(checks) + " checks, " + std::to_string(failed) + " failed" + fails);
        return;
    }
    if (g_icons[c].shown) toggle_shown(c); // what Alt+click does: the third goes to the overflow (saved in conf)
    check(!g_icons[c].shown && in_list("tray_hidden", g_icons[c].key), "Alt+click keeps it in the overflow");

    // the callbacks: version 0, then 3, then 4 (wParam the point, lParam the message and the uID)
    POINT pt = { 1200, 1050 };
    g_heard.clear();
    g_last_down = {};
    click(a, 0, false, pt);
    click(a, 0, true, pt);
    check(g_heard.size() == 2 && g_heard[0] == std::make_pair((WPARAM)1, (LPARAM)WM_LBUTTONDOWN) && g_heard[1] == std::make_pair((WPARAM)1, (LPARAM)WM_LBUTTONUP),
          "version 0 left click");
    deliver(kTrayNotify, wire(kNimSetVersion, 956, me, 1, 0, NULL, L"", L"", L"", 3));
    g_heard.clear();
    click(a, 2, true, pt);
    check(g_heard.size() == 2 && g_heard[0].second == WM_RBUTTONUP && g_heard[1].second == WM_CONTEXTMENU, "version 3 right click: the menu");
    g_heard.clear();
    click(a, 0, true, pt);
    check(g_heard.size() == 2 && g_heard[1].second == kNinSelect, "version 3 left click: NIN_SELECT");
    g_heard.clear();
    click(b, 2, true, pt);
    check(g_heard.size() == 2 && g_heard[1].first == MAKEWPARAM(1200, 1050) && g_heard[1].second == MAKELPARAM(WM_CONTEXTMENU, 7), "version 4 menu: point and uID");
    g_heard.clear();
    key(b, false);
    check(g_heard.size() == 1 && LOWORD(g_heard[0].second) == kNinKeySelect, "version 4 keyboard: NIN_KEYSELECT");
    g_heard.clear();
    g_last_down = {};
    click(a, 0, false, pt);
    click(a, 0, false, pt);
    check(g_heard.size() == 2 && g_heard[1].second == WM_LBUTTONDBLCLK, "double click");

    // a balloon: shown at once (the test's bar is never behind a fullscreen app), the app told
    g_heard.clear();
    deliver(kTrayNotify, wire(kNimModify, 956, me, 7, kNifInfo | kNifGuid, NULL, L"", L"Your files are ready: 3 folders were copied to D:\\Backup.", L"Copy finished",
                              0, &ga));
    check(g_bw != nullptr && !g_heard.empty() && LOWORD(g_heard.back().second) == kNinBalloonShow, "balloon shown, NIN_BALLOONSHOW");

    // where an icon is (Shell_NotifyIconGetRect)
    std::vector<BYTE> q(40, 0);
    DWORD magic = kTraySignature, one = 1, uid = 1;
    memcpy(q.data(), &magic, 4);
    memcpy(q.data() + 4, &one, 4);
    memcpy(q.data() + 16, &me, 4);
    memcpy(q.data() + 20, &uid, 4);
    check(deliver(kTrayIconRect, q) != 0, "icon rectangle");
    check(deliver(kTrayIconRect, q, 39) == 0, "icon rectangle: short buffer refused");
    check(deliver(kTrayAppBar, std::vector<BYTE>(63, 0)) == 0, "appbar message: wrong size refused");
    {
        std::wstring line = L"cmd\tdesktop"; // an app command sent to the tray: never taken
        std::vector<BYTE> c((BYTE *)line.data(), (BYTE *)(line.data() + line.size()));
        check(deliver(kCopyCommand, c) == FALSE, "a command on the tray window refused");
    }

    // ABM_GETTASKBARPOS through shared memory, as SHAppBarMessage passes it
    auto alloc = (PAllocShared)(void *)shlwapi_fn("SHAllocShared", 7);
    auto lock = (PLockShared)(void *)shlwapi_fn("SHLockShared", 8);
    auto unlock = (PUnlockShared)(void *)shlwapi_fn("SHUnlockShared", 9);
    auto free_sh = (PFreeShared)(void *)shlwapi_fn("SHFreeShared", 10);
    if (alloc && lock && unlock && free_sh) {
        BYTE abd[40] = {};
        DWORD sz = 40;
        memcpy(abd, &sz, 4);
        HANDLE sh = alloc(abd, 40, GetCurrentProcessId());
        std::vector<BYTE> m(64, 0);
        DWORD msg = ABM_GETTASKBARPOS, pid = GetCurrentProcessId();
        uint64_t hs = (uint64_t)(ULONG_PTR)sh;
        memcpy(m.data(), &sz, 4);
        memcpy(m.data() + 4, &me, 4);
        memcpy(m.data() + 40, &msg, 4);
        memcpy(m.data() + 48, &hs, 8);
        memcpy(m.data() + 56, &pid, 4);
        check(deliver(kTrayAppBar, m) == TRUE, "ABM_GETTASKBARPOS answered");
        RECT got = {}, want = shell_bar_rect(shell_primary());
        if (BYTE *p = (BYTE *)lock(sh, pid)) {
            memcpy(&got, p + 16, 16);
            unlock(p);
        }
        check(EqualRect(&got, &want) != FALSE, "ABM_GETTASKBARPOS: the main bar's rectangle");
        free_sh(sh, pid);
    } else check(false, "shlwapi's shared memory functions");

    // delete by GUID only (no window): the icon and its balloon go
    check(deliver(kTrayNotify, wire(kNimAdd, 956, me, 8, kNifMessage | kNifTip | kNifGuid, app, L"to delete", L"", L"", 0, &gb)) == TRUE, "add again by GUID is a modify");
    check(deliver(kTrayNotify, wire(kNimDelete, 956, 0, 0, kNifGuid, NULL, L"", L"", L"", 0, &ga)) == TRUE && g_bw == nullptr, "delete by GUID, balloon gone");
    check(deliver(kTrayNotify, wire(kNimDelete, 956, me, 1, 0, NULL, L"")) == TRUE, "delete by window and uID");
    check(deliver(kTrayNotify, wire(kNimDelete, 956, me, 1, 0, NULL, L"")) == FALSE, "delete twice fails");
    // left for the pictures: one on the bar, one in the overflow, one hidden by its app, and a balloon
    deliver(kTrayNotify, wire(kNimAdd, 956, me, 1, kNifMessage | kNifIcon | kNifTip, app, L"explorer-lite test"));
    deliver(kTrayNotify, wire(kNimAdd, 956, me, 7, kNifMessage | kNifIcon | kNifTip | kNifGuid | kNifShowTip, info, L"Version 4, GUID", L"", L"", 0, &ga));
    deliver(kTrayNotify, wire(kNimModify, 956, me, 7, kNifInfo | kNifGuid, NULL, L"", L"Your files are ready: 3 folders were copied to D:\\Backup.", L"Copy finished", 0,
                              &ga));
    log_line("traytest: " + std::string(failed ? "FAILED" : "ok") + ", " + std::to_string(checks) + " checks, " + std::to_string(failed) + " failed" + fails);
}

} // namespace

void tray_start(TrayKind kind) {
    if (g_tray || kind == TRAY_NONE) return;
    g_kind = kind;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = tray_proc;
    wc.hInstance = g_inst;
    std::wstring cls = kind == TRAY_SHELL ? L"Shell_TrayWnd" : cls_name(L"ExplorerLiteTray"), ncls = kind == TRAY_SHELL ? L"TrayNotifyWnd" : cls_name(L"ExplorerLiteTrayNotify");
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = ncls.c_str();
    RegisterClassExW(&wc);
    RECT r = shell_bar_rect(shell_primary());
    // hidden, on the taskbar's rectangle: apps that look for the taskbar find it there
    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.c_str(), L"", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, NULL, NULL, g_inst, NULL);
    if (!g_tray) {
        log_line("tray: the window could not be made (" + std::to_string(GetLastError()) + ")");
        return;
    }
    g_notify = CreateWindowExW(0, ncls.c_str(), NULL, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0, g_tray, NULL, g_inst, NULL);
    ChangeWindowMessageFilterEx(g_tray, WM_COPYDATA, MSGFLT_ALLOW, NULL); // also from sandboxed apps
    load_windows_choice();
    if (kind == TRAY_SHELL) { // every running app adds its icons again (they did it for explorer, or for us before a crash)
        SendNotifyMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
        log_line("tray: Shell_TrayWnd is ours, TaskbarCreated sent");
    }
}

void tray_stop() {
    close_balloon(0);
    for (Balloon &b : g_queue) drop(b.pic);
    g_queue.clear();
    fly_close();
    for (TrayIcon &t : g_icons) drop(t.pic);
    g_icons.clear();
    if (g_tip) DestroyWindow(g_tip);
    if (g_test_wnd) DestroyWindow(g_test_wnd);
    if (g_tray) DestroyWindow(g_tray);
    g_tip = g_test_wnd = g_tray = g_notify = NULL;
    g_bar = nullptr;
}

float tray_width() {
    if (!g_tray) return 0;
    int n = 0;
    bool over = false;
    for (const TrayIcon &t : g_icons) {
        if (on_bar(t)) n++;
        if (in_overflow(t)) over = true;
    }
    return n * kCell + (over ? kChevronW : 0);
}

void tray_layout(Surface &s, D2D1_RECT_F area) {
    g_bar = &s;
    g_area = area;
    float x = area.left, top = area.top + 2, bottom = area.bottom - 2;
    bool over = false;
    for (const TrayIcon &t : g_icons) over = over || in_overflow(t);
    g_chevron = over ? D2D1::RectF(x, top, x + kChevronW, bottom) : D2D1::RectF(0, 0, 0, 0);
    if (over) x += kChevronW;
    for (TrayIcon &t : g_icons) {
        if (!on_bar(t)) continue;
        t.r = D2D1::RectF(x, top, x + kCell, bottom);
        x += kCell;
    }
    if (g_notify && g_tray) { // TrayNotifyWnd over the icons, in the taskbar window's coordinates
        float k = surf_scale(s);
        SetWindowPos(g_notify, NULL, (int)(area.left * k), (int)(area.top * k), (int)((area.right - area.left) * k), (int)((area.bottom - area.top) * k),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void tray_draw(Surface &s) {
    if (!g_tray || &s != g_bar) return;
    if (g_chevron.right > g_chevron.left) {
        if (g_hover == H_CHEVRON || g_fly) surf_round(s, g_chevron, 4, g_press == H_CHEVRON ? kPress : kHover);
        surf_text(s, g_fly && !g_fly->keyboard ? L"\xE70D" : L"\xE70E", g_chevron, s.f_icon, kText); // chevron down / up
    }
    for (int i = 0; i < (int)g_icons.size(); i++) {
        const TrayIcon &t = g_icons[i];
        if (!on_bar(t)) continue;
        if (g_hover == i) surf_round(s, t.r, 4, g_press == i ? kPress : kHover);
        float cx = (t.r.left + t.r.right) / 2, cy = (t.r.top + t.r.bottom) / 2;
        if (t.pic) surf_icon(s, t.pic, D2D1::RectF(cx - kTrayIcon / 2, cy - kTrayIcon / 2, cx + kTrayIcon / 2, cy + kTrayIcon / 2));
        else surf_round(s, D2D1::RectF(cx - 3, cy - 3, cx + 3, cy + 3), 3, kDim); // an icon without a picture still takes clicks
    }
}

bool tray_mouse(UINT msg, float x, float y) {
    if (!g_tray || !g_bar) return false;
    if (msg == WM_MOUSELEAVE) {
        set_hover(-1);
        g_press = -1;
        return false;
    }
    bool inside = surf_inside(g_area, x, y);
    int i = -1;
    if (inside) {
        if (surf_inside(g_chevron, x, y)) i = H_CHEVRON;
        for (int j = 0; j < (int)g_icons.size() && i == -1; j++)
            if (on_bar(g_icons[j]) && surf_inside(g_icons[j].r, x, y)) i = j;
    }
    POINT pt;
    GetCursorPos(&pt);
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    switch (msg) {
    case WM_MOUSEMOVE:
        set_hover(i);
        if (i >= 0) send(g_icons[i], WM_MOUSEMOVE, pt);
        return inside;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (!inside) return false;
        g_press = i;
        tip_hide();
        KillTimer(g_tray, kTimerTip);
        if (i >= 0 && !alt) click(i, msg == WM_LBUTTONDOWN ? 0 : msg == WM_MBUTTONDOWN ? 1 : 2, false, pt);
        InvalidateRect(g_bar->hwnd, NULL, FALSE);
        return true;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        if (!inside) return false;
        bool same = i == g_press && i != -1;
        g_press = -1;
        InvalidateRect(g_bar->hwnd, NULL, FALSE);
        if (!same) return true;
        if (i == H_CHEVRON) {
            if (msg == WM_LBUTTONUP) fly_open(false, false);
        } else if (alt) { // Alt+click: into the overflow (or out of it)
            toggle_shown(i);
            g_hover = -1;
            fly_refresh();
            shell_tray_changed();
        } else click(i, msg == WM_LBUTTONUP ? 0 : msg == WM_MBUTTONUP ? 1 : 2, true, pt);
        return true;
    }
    }
    return inside;
}

void tray_window_gone(HWND h) {
    bool any = false;
    for (int i = (int)g_icons.size() - 1; i >= 0; i--)
        if (hwnd_of(g_icons[i].hwnd) == h) {
            forget_balloons(g_icons[i], false);
            remove_icon(i);
            any = true;
        }
    if (any) {
        log_line("tray: an icon's window is gone, its icon too");
        fly_refresh();
        shell_tray_changed();
    }
}

void tray_sweep() {
    bool any = false;
    for (int i = (int)g_icons.size() - 1; i >= 0; i--)
        if (!IsWindow(hwnd_of(g_icons[i].hwnd))) {
            forget_balloons(g_icons[i], false);
            remove_icon(i);
            any = true;
        }
    appbars_sweep();
    if (any) {
        fly_refresh();
        shell_tray_changed();
    }
}

void tray_place(const RECT &bar) {
    if (g_tray) SetWindowPos(g_tray, NULL, bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void tray_fullscreen_changed() { show_next_balloon(); }

void tray_detach() {
    close_balloon(0);
    fly_close();
    set_hover(-1);
    g_press = -1;
    g_bar = nullptr;
}

void tray_overflow(bool keyboard) { fly_open(keyboard, false); }

RECT tray_work_area(HMONITOR mon, RECT wa) {
    for (const AppBar &a : g_appbars) {
        if (!a.placed || MonitorFromRect(&a.rc, MONITOR_DEFAULTTONEAREST) != mon) continue;
        if (a.edge == ABE_TOP) wa.top = std::max(wa.top, a.rc.bottom);
        else if (a.edge == ABE_BOTTOM) wa.bottom = std::min(wa.bottom, a.rc.top);
        else if (a.edge == ABE_LEFT) wa.left = std::max(wa.left, a.rc.right);
        else if (a.edge == ABE_RIGHT) wa.right = std::min(wa.right, a.rc.left);
    }
    return wa;
}

bool tray_command(const std::wstring &verb, const std::wstring &arg) {
    if (verb == L"traytest") {
        if (g_tray && g_kind == TRAY_TEST) tray_test();
        else log_line("traytest: only in a test instance");
    } else if (verb == L"overflow") { // the overflow, open without the focus (tests on their own desktop)
        if (!g_fly) fly_open(arg == L"keyboard", true);
    } else if (verb == L"overflowshot") {
        log_line(std::string("overflow shot ") + (g_fly && surf_shot(g_fly->s, arg, fly_draw, g_fly) ? "ok" : "failed"));
    } else if (verb == L"overflowclose") fly_close();
    else if (verb == L"balloonshot") {
        log_line(std::string("balloon shot ") + (g_bw && surf_shot(g_bw->s, arg, balloon_draw, g_bw) ? "ok" : "failed"));
    } else if (verb == L"trayicons") { // what the tray holds, into the log
        std::string s = "tray icons:";
        for (const TrayIcon &t : g_icons)
            s += " [" + utf8(t.tip.substr(0, t.tip.find(L'\n'))) + ", v" + std::to_string(t.version) + (t.hidden ? ", hidden" : on_bar(t) ? ", bar" : ", overflow") +
                 (t.pic ? "" : ", no picture") + "]";
        log_line(s);
    } else if (verb == L"trayclick") { // trayclick <n> [left|right]: the nth icon on the bar, as a click there
        int n = _wtoi(arg.c_str()), seen = 0;
        int button = arg.find(L"right") != std::wstring::npos ? 2 : 0;
        POINT pt;
        GetCursorPos(&pt);
        for (int i = 0; i < (int)g_icons.size(); i++)
            if (on_bar(g_icons[i]) && seen++ == n) {
                click(i, button, false, pt);
                click(i, button, true, pt);
                break;
            }
    } else return false;
    return true;
}
