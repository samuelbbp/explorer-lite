// shell.cpp: explorer-lite as the shell (see shell.h). A taskbar on every monitor with the open windows (Windows' own
// rules, taskfilter.h), grouped by app and kept in step by WinEvents (no polling), pinned apps, a clock and the Start
// button. Run alongside explorer, each bar is an appbar (SHAppBarMessage, answered by explorer's taskbar) so maximized
// windows leave room; as the session's shell nobody answers that, so it sets each monitor's work area itself, as
// ManagedShell does (AppBarManager.SetWorkArea). Then it also runs what starts with Windows, once per sign-in.
//
// The system tray (the Shell_TrayWnd protocol) is not here yet: explorer owns that class in the user's session, so it
// cannot be tested alongside it. It will get a window of its own (tray.cpp) that the bars leave room for.
#include "shell.h"
#include "app.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <windowsx.h>
#include <algorithm>
#include <cwctype>
#include <vector>
#include "conf.h"
#include "gfx.h"
#include "idioma.h"
#include "keys.h"
#include "surface.h"
#include "taskfilter.h"
#include "tasks.h"
#include "thumbs.h"
#include "update.h"

namespace {

// layout, in DIPs
const float kBarH = 40, kStartW = 44, kBtnMax = 200, kBtnMin = 44, kIcon = 20, kClockW = 92, kTitleMin = 96;
const UINT_PTR kTimerScan = 1, kTimerClock = 2, kTimerStartup = 3, kTimerDisplay = 4, kTimerApps = 5;
const int kIconPx = 32; // icons asked for at this size, drawn at kIcon DIPs

enum Mode {
    M_TEST,     // a test instance: bars drawn on its own desktop, no appbar, no work area, no Win key
    M_APPBAR,   // alongside explorer: appbars
    M_SHELL,    // the session's shell: the work area set by hand
};

struct Task {
    HWND hwnd = NULL;
    std::wstring title, exe, aumid, group;
    HMONITOR mon = NULL;
    Thumb *icon = nullptr;   // owned
    bool icon_dirty = true;  // asked again (WM_GETICON) on the next scan: apps change it (unread badges)
    bool icon_from_shell = false; // asked to thumbs.cpp (Store apps, windows without one)
};

struct Pin {
    std::wstring target, group; // a path or shell:AppsFolder\<id>; what its windows group by
    Thumb *icon = nullptr;
};

enum BtnKind { B_START, B_TASK, B_PIN, B_CLOCK };

struct Btn {
    D2D1_RECT_F r;
    int kind, idx; // idx: into g_tasks or g_pins
};

struct Bar {
    Surface s;
    HMONITOR mon = NULL;
    RECT mrc = {};          // the monitor, pixels
    RECT rc = {};           // the bar, pixels
    bool primary = false, appbar = false, low = false; // low: a fullscreen app is in front, the bar is not topmost
    std::vector<Btn> btns;
    int hover = -1, press = -1;
    bool tracking = false;
};

HWND g_shell;
Mode g_mode = M_TEST;
std::vector<Bar *> g_bars;
std::vector<Task> g_tasks; // in the bars' order
std::vector<Pin> g_pins;
HWND g_active;             // the window in front (its root owner), if it is not ours
std::vector<HWINEVENTHOOK> g_hooks;
UINT g_msg_taskbar_created;
bool g_quitting;
ULONGLONG g_started;
DWORD g_restarted;         // the pid this one replaced after a crash (0: a normal start)
const wchar_t *kBarClass = L"ExplorerLiteTaskbar"; // + instance_suffix(); never explorer's Shell_TrayWnd

std::wstring shell_class() { return L"ExplorerLiteShell" + instance_suffix(); }
std::wstring bar_class() { return kBarClass + instance_suffix(); }
std::wstring restart_marker() { return data_dir() + L"\\shell-restart"; }

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

const wchar_t kAppsPrefix[] = L"shell:AppsFolder\\";

UINT monitor_dpi(HMONITOR mon) {
    typedef HRESULT(WINAPI * PDpi)(HMONITOR, int, UINT *, UINT *);
    static HMODULE shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static PDpi dpi_for = shcore ? (PDpi)(void *)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    UINT x = 0, y = 0;
    if (dpi_for && SUCCEEDED(dpi_for(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) return x;
    return 96;
}

// --- the windows: which ones, and what they are -----------------------------------------------------------------

WinFacts facts_of(HWND h) {
    WinFacts f;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    f.visible = IsWindowVisible(h) != FALSE;
    f.owned = GetWindow(h, GW_OWNER) != NULL;
    f.appwindow = (ex & WS_EX_APPWINDOW) != 0;
    f.toolwindow = (ex & WS_EX_TOOLWINDOW) != 0;
    f.noactivate = (ex & WS_EX_NOACTIVATE) != 0;
    DWORD cloaked = 0;
    f.cloaked = SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
    f.deleted = GetPropW(h, L"ITaskList_Deleted") != NULL;
    if (!(ex & WS_EX_WINDOWEDGE)) {
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        for (const wchar_t *c : { L"ApplicationFrameWindow", L"Windows.UI.Core.CoreWindow", L"StartMenuSizingFrame", L"Shell_LightDismissOverlay" })
            if (!wcscmp(cls, c)) f.immersive = true;
    }
    return f;
}

BOOL CALLBACK collect(HWND h, LPARAM lp) {
    if (taskbar_shows(facts_of(h))) ((std::vector<HWND> *)lp)->push_back(h);
    return TRUE;
}

std::wstring window_title(HWND h) {
    wchar_t t[256] = L"";
    GetWindowTextW(h, t, ARRAYSIZE(t)); // another process's window: read from its title, no message sent
    return t;
}

std::wstring process_path(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t path[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(path);
    std::wstring out = QueryFullProcessImageNameW(p, 0, path, &n) ? std::wstring(path, n) : L"";
    CloseHandle(p);
    return out;
}

// System.AppUserModel.ID of the window: what it groups by (Store apps all live in ApplicationFrameHost.exe)
std::wstring window_aumid(HWND h) {
    static const PROPERTYKEY kAumid = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
    IPropertyStore *ps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps))) || !ps) return L"";
    std::wstring out;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(kAumid, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
    PropVariantClear(&v);
    ps->Release();
    return out;
}

bool is_frame_host(const Task &t) {
    size_t n = t.exe.size();
    return n >= 24 && !_wcsicmp(t.exe.c_str() + n - 24, L"ApplicationFrameHost.exe");
}

void forget_icon(Thumb *&t) {
    if (!t) return;
    for (Bar *b : g_bars) surf_forget(b->s, t);
    delete t;
    t = nullptr;
}

// the window's own icon (WM_GETICON, then its class's); Store apps and windows without one: their app's, from the shell
void fetch_icon(Task &t) {
    t.icon_dirty = false;
    if (t.icon_from_shell) return;
    HICON icon = NULL;
    if (!(is_frame_host(t) && !t.aumid.empty())) {
        DWORD_PTR r = 0;
        for (WPARAM which : { (WPARAM)ICON_BIG, (WPARAM)ICON_SMALL2 })
            if (!icon && SendMessageTimeoutW(t.hwnd, WM_GETICON, which, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &r) && r) icon = (HICON)r;
        if (!icon) icon = (HICON)GetClassLongPtrW(t.hwnd, GCLP_HICON);
        if (!icon) icon = (HICON)GetClassLongPtrW(t.hwnd, GCLP_HICONSM);
    }
    if (icon) {
        Thumb *th = new Thumb{ kGenBar, L"" };
        if (icon_pixels(icon, *th)) { // not destroyed: the window's
            forget_icon(t.icon);
            t.icon = th;
            return;
        }
        delete th;
    }
    if (t.icon) return;
    t.icon_from_shell = true;
    std::wstring target = !t.aumid.empty() && is_frame_host(t) ? kAppsPrefix + t.aumid : t.exe;
    if (!target.empty()) thumbs_request(g_shell, kGenBar, target, kIconPx);
}

Task *task_of(HWND h) {
    for (Task &t : g_tasks)
        if (t.hwnd == h) return &t;
    return nullptr;
}

void layout_all();
void redraw_all() {
    for (Bar *b : g_bars) InvalidateRect(b->s.hwnd, NULL, FALSE);
}

// What changed since the last time: windows gone, new ones (after the last of their app), titles and monitors.
void scan() {
    std::vector<HWND> now;
    EnumWindows(collect, (LPARAM)&now); // this desktop's top-level windows only: a test sees its own
    for (size_t i = 0; i < g_tasks.size();) {
        if (std::find(now.begin(), now.end(), g_tasks[i].hwnd) == now.end()) {
            forget_icon(g_tasks[i].icon);
            g_tasks.erase(g_tasks.begin() + i);
        } else i++;
    }
    for (HWND h : now) {
        Task *t = task_of(h);
        if (!t) {
            Task n;
            n.hwnd = h;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            n.exe = process_path(pid);
            n.aumid = window_aumid(h);
            n.group = lower(n.aumid.empty() ? n.exe : n.aumid);
            size_t at = g_tasks.size();
            for (size_t i = 0; i < g_tasks.size(); i++)
                if (g_tasks[i].group == n.group) at = i + 1;
            g_tasks.insert(g_tasks.begin() + at, n);
            t = &g_tasks[at];
        }
        t->title = window_title(h);
        t->mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (t->icon_dirty) fetch_icon(*t);
    }
    layout_all();
    redraw_all();
}

void schedule_scan() { SetTimer(g_shell, kTimerScan, 40, NULL); } // a burst of events: one scan

Bar *bar_of(HWND h) {
    for (Bar *b : g_bars)
        if (b->s.hwnd == h) return b;
    return nullptr;
}

// a fullscreen app (a game, a video) in front on a bar's monitor: that bar stops being topmost until it goes
void check_fullscreen() {
    HWND f = GetForegroundWindow();
    wchar_t cls[32] = L"";
    if (f) GetClassNameW(f, cls, ARRAYSIZE(cls));
    bool desktop = !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
    RECT r = {};
    bool any = f && !desktop && !bar_of(f) && GetWindowRect(f, &r);
    for (Bar *b : g_bars) {
        bool low = any && MonitorFromWindow(f, MONITOR_DEFAULTTONULL) == b->mon && r.left <= b->mrc.left && r.top <= b->mrc.top && r.right >= b->mrc.right &&
                   r.bottom >= b->mrc.bottom;
        if (low == b->low) continue;
        b->low = low;
        SetWindowPos(b->s.hwnd, low ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (low) SetWindowPos(b->s.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void CALLBACK on_event(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (ev == EVENT_SYSTEM_FOREGROUND) {
        HWND root = h ? GetAncestor(h, GA_ROOTOWNER) : NULL;
        DWORD pid = 0;
        if (root) GetWindowThreadProcessId(root, &pid);
        // Start or a menu of ours in front: the app that was in front stays the active one
        if (pid != GetCurrentProcessId() || task_of(root)) g_active = root;
        check_fullscreen();
        redraw_all();
        return;
    }
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    switch (ev) {
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND: redraw_all(); return;
    case EVENT_OBJECT_LOCATIONCHANGE:
        if (Task *t = task_of(h)) { // moved to another monitor: to that one's bar
            HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            if (m != t->mon) {
                t->mon = m;
                layout_all();
                redraw_all();
            }
        }
        return;
    case EVENT_OBJECT_NAMECHANGE:
        if (Task *t = task_of(h)) {
            t->icon_dirty = true;
            schedule_scan();
        }
        return;
    case EVENT_OBJECT_DESTROY:
        if (task_of(h)) schedule_scan();
        return;
    default: // created, shown, hidden, cloaked, uncloaked: only top-level windows matter
        if (GetAncestor(h, GA_PARENT) == GetDesktopWindow()) schedule_scan();
    }
}

void hooks_start() {
    const DWORD ranges[][2] = {
        { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
        { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND },
        { EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE }, // create, destroy, show, hide
        { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE },
        { EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED },
    };
    for (auto &r : ranges) {
        HWINEVENTHOOK h = SetWinEventHook(r[0], r[1], NULL, on_event, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);
        if (h) g_hooks.push_back(h);
    }
}

void hooks_stop() {
    for (HWINEVENTHOOK h : g_hooks) UnhookWinEvent(h);
    g_hooks.clear();
}

// --- pinned apps (conf "pinned": targets separated by |) ----------------------------------------------------------

std::wstring group_of_target(const std::wstring &target) {
    if (!_wcsnicmp(target.c_str(), kAppsPrefix, wcslen(kAppsPrefix))) return lower(target.substr(wcslen(kAppsPrefix)));
    return lower(target);
}

void load_pins() {
    for (Pin &p : g_pins) forget_icon(p.icon);
    g_pins.clear();
    std::wstring all = wide(conf_get("pinned"));
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(L'|', pos);
        if (end == std::wstring::npos) end = all.size();
        std::wstring t = all.substr(pos, end - pos);
        if (!t.empty()) {
            Pin p;
            p.target = t;
            p.group = group_of_target(t);
            g_pins.push_back(p);
            thumbs_request(g_shell, kGenBar, t, kIconPx);
        }
        pos = end + 1;
    }
}

void save_pins() {
    std::wstring all;
    for (const Pin &p : g_pins) all += (all.empty() ? L"" : L"|") + p.target;
    conf_set("pinned", utf8(all));
}

int pin_of_group(const std::wstring &group) {
    for (int i = 0; i < (int)g_pins.size(); i++)
        if (g_pins[i].group == group) return i;
    return -1;
}

std::wstring task_target(const Task &t) { return !t.aumid.empty() && is_frame_host(t) ? kAppsPrefix + t.aumid : t.exe; }

// --- the bars: layout and drawing ----------------------------------------------------------------------------------

D2D1_RECT_F bar_client(Bar *b) {
    float s = surf_scale(b->s);
    return D2D1::RectF(0, 0, (b->rc.right - b->rc.left) / s, (b->rc.bottom - b->rc.top) / s);
}

void layout(Bar *b) {
    b->btns.clear();
    D2D1_RECT_F c = bar_client(b);
    float x = 4;
    b->btns.push_back({ D2D1::RectF(x, 2, x + kStartW, c.bottom - 2), B_START, 0 });
    x += kStartW + 4;
    float right = c.right - kClockW - 8;
    // what goes on this bar, in order: pinned apps (their windows in their place) on the main one, then the rest
    std::vector<Btn> items;
    std::vector<bool> placed(g_tasks.size(), false);
    if (b->primary)
        for (int p = 0; p < (int)g_pins.size(); p++) {
            bool any = false;
            for (int i = 0; i < (int)g_tasks.size(); i++)
                if (g_tasks[i].group == g_pins[p].group && g_tasks[i].mon == b->mon) {
                    items.push_back({ {}, B_TASK, i });
                    placed[i] = any = true;
                }
            if (!any) items.push_back({ {}, B_PIN, p });
        }
    for (int i = 0; i < (int)g_tasks.size(); i++)
        if (!placed[i] && g_tasks[i].mon == b->mon) items.push_back({ {}, B_TASK, i });
    int n_tasks = 0;
    for (const Btn &it : items) n_tasks += it.kind == B_TASK;
    float pins_w = (items.size() - n_tasks) * kBtnMin;
    float w = n_tasks ? std::max(kBtnMin, std::min(kBtnMax, (right - x - pins_w) / n_tasks)) : 0;
    for (Btn &it : items) {
        float bw = it.kind == B_TASK ? w : kBtnMin;
        if (x + bw > right) break; // no room left (an overflow list comes later)
        it.r = D2D1::RectF(x, 2, x + bw - 2, c.bottom - 2);
        b->btns.push_back(it);
        x += bw;
    }
    b->btns.push_back({ D2D1::RectF(c.right - kClockW - 4, 2, c.right - 4, c.bottom - 2), B_CLOCK, 0 });
    if (b->hover >= (int)b->btns.size()) b->hover = -1;
    if (b->press >= (int)b->btns.size()) b->press = -1;
}

void layout_all() {
    for (Bar *b : g_bars) layout(b);
}

std::wstring clock_text(DWORD flags_time, bool date) {
    wchar_t buf[64] = L"";
    if (date) GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, NULL, NULL, buf, ARRAYSIZE(buf), NULL);
    else GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, flags_time, NULL, NULL, buf, ARRAYSIZE(buf));
    return buf;
}

void draw_start_logo(Surface &s, D2D1_RECT_F r, bool on) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, q = 7.5f, gap = 1.5f;
    for (int i = 0; i < 4; i++) {
        float x = cx + (i % 2 ? gap / 2 : -gap / 2 - q), y = cy + (i / 2 ? gap / 2 : -gap / 2 - q);
        surf_round(s, D2D1::RectF(x, y, x + q, y + q), 1.5f, kAccent, on ? 1 : 0.92f);
    }
}

void draw_bar(void *ctx) {
    Bar *b = (Bar *)ctx;
    Surface &s = b->s;
    D2D1_RECT_F c = bar_client(b);
    surf_fill(s, c, kPane); // not Clear: a shot draws every bar on one bitmap
    surf_fill(s, D2D1::RectF(0, 0, c.right, 1), kLine);
    for (int i = 0; i < (int)b->btns.size(); i++) {
        const Btn &bt = b->btns[i];
        bool hov = b->hover == i, down = b->press == i;
        switch (bt.kind) {
        case B_START:
            if (hov || start_is_open()) surf_round(s, bt.r, 4, down ? kPress : kHover);
            draw_start_logo(s, bt.r, start_is_open());
            break;
        case B_PIN: {
            if (hov) surf_round(s, bt.r, 4, down ? kPress : kHover);
            float cx = (bt.r.left + bt.r.right) / 2, cy = (bt.r.top + bt.r.bottom) / 2;
            surf_icon(s, g_pins[bt.idx].icon, D2D1::RectF(cx - kIcon / 2, cy - kIcon / 2, cx + kIcon / 2, cy + kIcon / 2));
            break;
        }
        case B_TASK: {
            const Task &t = g_tasks[bt.idx];
            bool active = t.hwnd == g_active && !IsIconic(t.hwnd);
            if (active || hov) surf_round(s, bt.r, 4, down ? kPress : active ? kTabOn : kHover);
            bool wide_btn = bt.r.right - bt.r.left >= kTitleMin;
            float cy = (bt.r.top + bt.r.bottom) / 2;
            float ix = wide_btn ? bt.r.left + 10 : (bt.r.left + bt.r.right - kIcon) / 2;
            surf_icon(s, t.icon, D2D1::RectF(ix, cy - kIcon / 2, ix + kIcon, cy + kIcon / 2), IsIconic(t.hwnd) ? 0.7f : 1);
            if (!t.icon || t.icon->w <= 0) surf_text(s, L"\xE737", D2D1::RectF(ix, cy - kIcon / 2, ix + kIcon, cy + kIcon / 2), s.f_icon, kDim);
            if (wide_btn) surf_text(s, t.title, D2D1::RectF(ix + kIcon + 8, bt.r.top, bt.r.right - 8, bt.r.bottom), s.f_ui, kText);
            // the running mark: long and accented for the window in front, short and dim for the others
            float mw = active ? std::min(28.f, (bt.r.right - bt.r.left) * 0.4f) : 6, mx = (bt.r.left + bt.r.right - mw) / 2;
            surf_round(s, D2D1::RectF(mx, bt.r.bottom - 3, mx + mw, bt.r.bottom), 1.5f, active ? kAccent : kDim);
            break;
        }
        case B_CLOCK: {
            if (hov) surf_round(s, bt.r, 4, kHover);
            float mid = (bt.r.top + bt.r.bottom) / 2;
            surf_text(s, clock_text(TIME_NOSECONDS, false), D2D1::RectF(bt.r.left, bt.r.top + 2, bt.r.right - 8, mid + 1), s.f_small_r, kText);
            surf_text(s, clock_text(0, true), D2D1::RectF(bt.r.left, mid - 1, bt.r.right - 8, bt.r.bottom - 2), s.f_small_r, kText);
            break;
        }
        }
    }
}

int btn_at(Bar *b, float x, float y) {
    for (int i = 0; i < (int)b->btns.size(); i++)
        if (surf_inside(b->btns[i].r, x, y)) return i;
    return -1;
}

// --- clicks and menus -------------------------------------------------------------------------------------------

void activate(Task &t) {
    HWND h = t.hwnd;
    if (h == g_active && !IsIconic(h)) { // the one in front: minimized
        ShowWindowAsync(h, SW_MINIMIZE);
        return;
    }
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    SetForegroundWindow(h); // a click on the bar was the last input: Windows lets us
}

enum MenuCmd { MC_NEW = 1, MC_PIN, MC_UNPIN, MC_CLOSE, MC_OPEN, MC_TASKS, MC_FILES, MC_QUIT };

void pin_menu(Bar *b, int pin_idx, const Task *task) {
    ShellMenu m(b->s.dpi);
    if (task) m.add(MC_NEW, L"\xE8A7", T(L"New window"));
    else m.add(MC_OPEN, L"\xE8A7", T(L"Open"));
    if (pin_idx >= 0) m.add(MC_UNPIN, L"\xE77A", T(L"Unpin from taskbar"));
    else m.add(MC_PIN, L"\xE718", T(L"Pin to taskbar"));
    if (task) {
        m.sep();
        m.add(MC_CLOSE, L"\xE8BB", T(L"Close window"));
    }
    HWND hwnd = task ? task->hwnd : NULL;
    std::wstring target = task ? task_target(*task) : g_pins[pin_idx].target, group = task ? task->group : g_pins[pin_idx].group;
    switch (m.show(b->s.hwnd)) {
    case MC_NEW:
    case MC_OPEN: shell_launch(target); break;
    case MC_PIN:
        if (pin_of_group(group) < 0 && !target.empty()) {
            Pin p;
            p.target = target;
            p.group = group;
            g_pins.push_back(p);
            thumbs_request(g_shell, kGenBar, target, kIconPx);
            save_pins();
        }
        break;
    case MC_UNPIN: {
        int i = pin_of_group(group);
        if (i >= 0) {
            forget_icon(g_pins[i].icon);
            g_pins.erase(g_pins.begin() + i);
            save_pins();
        }
        break;
    }
    case MC_CLOSE:
        if (IsWindow(hwnd)) PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        break;
    }
    layout_all();
    redraw_all();
}

void start_menu_right(Bar *b) {
    ShellMenu m(b->s.dpi);
    m.add(MC_TASKS, L"\xE9D9", T(L"Task manager"));
    m.add(MC_FILES, L"\xE8B7", T(L"File explorer"));
    if (g_mode != M_SHELL) { // as the session's shell there would be nothing left: it stays
        m.sep();
        m.add(MC_QUIT, L"\xE7E8", T(L"Close the taskbar"));
    }
    switch (m.show(b->s.hwnd)) {
    case MC_TASKS: shell_launch(exe_path(), L"--tasks"); break;
    case MC_FILES: shell_launch(exe_path()); break;
    case MC_QUIT: PostMessageW(g_shell, WM_CLOSE, 0, 0); break;
    }
}

void on_click(Bar *b, int i, int button) { // 0 left, 1 middle, 2 right
    if (i < 0 || i >= (int)b->btns.size()) return;
    Btn bt = b->btns[i];
    switch (bt.kind) {
    case B_START:
        if (button == 2) start_menu_right(b);
        else if (button == 0) start_toggle(b->mon);
        break;
    case B_PIN:
        if (button == 2) pin_menu(b, bt.idx, nullptr);
        else shell_launch(g_pins[bt.idx].target);
        break;
    case B_TASK: {
        Task t = g_tasks[bt.idx]; // a copy: a menu runs a message loop that may rescan
        if (button == 0) activate(g_tasks[bt.idx]);
        else if (button == 1) shell_launch(task_target(t)); // a new instance, as Windows' middle click
        else pin_menu(b, pin_of_group(t.group), &t);
        break;
    }
    }
}

// --- the bars: windows, appbars and the work area -------------------------------------------------------------------

void place(Bar *b) {
    int h = MulDiv((int)kBarH, (int)b->s.dpi, 96);
    b->rc = b->mrc;
    b->rc.top = b->rc.bottom - h;
    if (b->appbar) { // explorer says where: above its own taskbar if it is at the bottom too
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = b->s.hwnd;
        abd.uEdge = ABE_BOTTOM;
        abd.rc = b->rc;
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.top = abd.rc.bottom - h; // it only moves the edge it was asked about
        SHAppBarMessage(ABM_SETPOS, &abd);
        b->rc = abd.rc;
    } else if (g_mode == M_SHELL) {
        RECT wa = b->mrc;
        wa.bottom = b->rc.top;
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &wa, SPIF_SENDCHANGE);
    }
    SetWindowPos(b->s.hwnd, b->low ? HWND_NOTOPMOST : HWND_TOPMOST, b->rc.left, b->rc.top, b->rc.right - b->rc.left, b->rc.bottom - b->rc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    layout(b);
    InvalidateRect(b->s.hwnd, NULL, FALSE);
}

LRESULT CALLBACK bar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Bar *b = bar_of(h);
    if (!b) return DefWindowProcW(h, msg, wp, lp);
    float s = surf_scale(b->s);
    switch (msg) {
    case WM_PAINT: surf_paint(b->s, draw_bar, b); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // the app in front stays in front
    case WM_MOUSEMOVE: {
        int i = btn_at(b, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        if (i != b->hover) {
            b->hover = i;
            InvalidateRect(h, NULL, FALSE);
        }
        if (!b->tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            b->tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        b->tracking = false;
        b->hover = b->press = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        b->press = btn_at(b, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        int i = btn_at(b, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        bool same = i == b->press;
        b->press = -1;
        InvalidateRect(h, NULL, FALSE);
        if (same) on_click(b, i, msg == WM_LBUTTONUP ? 0 : msg == WM_MBUTTONUP ? 1 : 2);
        return 0;
    }
    case WM_DPICHANGED: // its own size, from the monitor (not the rectangle Windows suggests)
        b->s.dpi = HIWORD(wp);
        place(b);
        return 0;
    case WM_APP_APPBAR:
        if (wp == ABN_POSCHANGED) place(b);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

BOOL CALLBACK add_monitor(HMONITOR mon, HDC, LPRECT, LPARAM) {
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    Bar *b = new Bar;
    b->mon = mon;
    b->mrc = mi.rcMonitor;
    b->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    b->s.dpi = monitor_dpi(mon);
    g_bars.push_back(b);
    int h = MulDiv((int)kBarH, (int)b->s.dpi, 96);
    b->s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, bar_class().c_str(), L"explorer-lite taskbar", WS_POPUP,
                                mi.rcMonitor.left, mi.rcMonitor.bottom - h, mi.rcMonitor.right - mi.rcMonitor.left, h, NULL, NULL, g_inst, NULL);
    if (!b->s.hwnd) {
        g_bars.pop_back();
        delete b;
        return TRUE;
    }
    b->s.dpi = GetDpiForWindow(b->s.hwnd);
    surf_formats(b->s);
    if (g_mode == M_APPBAR) {
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = b->s.hwnd;
        abd.uCallbackMessage = WM_APP_APPBAR;
        b->appbar = SHAppBarMessage(ABM_NEW, &abd) != 0;
    }
    place(b);
    return TRUE;
}

void bars_destroy() {
    for (Bar *b : g_bars) {
        if (b->appbar) {
            APPBARDATA abd = { sizeof(abd) };
            abd.hWnd = b->s.hwnd;
            SHAppBarMessage(ABM_REMOVE, &abd);
        } else if (g_mode == M_SHELL) { // the whole monitor again
            RECT wa = b->mrc;
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &wa, SPIF_SENDCHANGE);
        }
        surf_free(b->s);
        DestroyWindow(b->s.hwnd);
        delete b;
    }
    g_bars.clear();
}

void bars_create() {
    bars_destroy();
    EnumDisplayMonitors(NULL, NULL, add_monitor, 0);
    // the main monitor's bar first: shots and Start's default monitor
    std::stable_sort(g_bars.begin(), g_bars.end(), [](Bar *a, Bar *b) { return a->primary && !b->primary; });
    check_fullscreen();
}

void clock_timer() { // at the next minute, not every second
    SYSTEMTIME t;
    GetLocalTime(&t);
    SetTimer(g_shell, kTimerClock, (60 - t.wSecond) * 1000 - t.wMilliseconds + 50, NULL);
}

// --- what starts with Windows (only as the session's shell; a test only logs it) -----------------------------------

void run_command(const std::wstring &line) {
    wchar_t exp[4096];
    DWORD n = ExpandEnvironmentStringsW(line.c_str(), exp, ARRAYSIZE(exp));
    std::wstring cmd = n && n <= ARRAYSIZE(exp) ? exp : line;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_DEFAULT_ERROR_MODE, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    // not a program: a document or a URL, as Windows' Run would open it
    std::wstring file = cmd;
    const wchar_t *args = PathGetArgsW(cmd.c_str());
    if (args && *args) file = cmd.substr(0, args - cmd.c_str());
    while (!file.empty() && file.back() == L' ') file.pop_back();
    std::vector<wchar_t> f(file.begin(), file.end());
    f.push_back(0);
    PathUnquoteSpacesW(f.data());
    ShellExecuteW(NULL, NULL, f.data(), args && *args ? args : NULL, NULL, SW_SHOWNORMAL);
}

// HKCU RunOnce: each value removed before it runs (after, with a leading !), as explorer does
void run_once(bool dry, int *count) {
    const wchar_t *key = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ | (dry ? 0 : KEY_SET_VALUE), &h) != ERROR_SUCCESS) return;
    std::vector<std::pair<std::wstring, std::wstring>> all;
    for (DWORD i = 0;; i++) {
        wchar_t name[512];
        wchar_t data[4096];
        DWORD nl = ARRAYSIZE(name), dl = sizeof(data) - sizeof(wchar_t), type = 0;
        LONG r = RegEnumValueW(h, i, name, &nl, NULL, &type, (BYTE *)data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        data[dl / sizeof(wchar_t)] = 0;
        all.push_back({ name, data });
    }
    for (auto &v : all) {
        (*count)++;
        if (dry) {
            log_line("startup (test, not run): RunOnce " + utf8(v.first));
            continue;
        }
        bool after = !v.first.empty() && v.first[0] == L'!';
        if (!after) RegDeleteValueW(h, v.first.c_str());
        run_command(v.second);
        if (after) RegDeleteValueW(h, v.first.c_str());
    }
    RegCloseKey(h);
}

DWORD WINAPI startup_thread(void *p) {
    bool dry = p != nullptr;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int count = 0, skipped = 0;
    run_once(dry, &count);
    for (const StartupEntry &e : startup_entries()) {
        if (!e.enabled) {
            skipped++;
            continue;
        }
        count++;
        if (dry) log_line("startup (test, not run): " + utf8(e.where) + " " + utf8(e.name));
        else if (e.folder) ShellExecuteW(NULL, NULL, e.command.c_str(), NULL, NULL, SW_SHOWNORMAL);
        else run_command(e.command);
    }
    log_line("startup: " + std::to_string(count) + (dry ? " entries listed (a test runs none)" : " started") + ", " + std::to_string(skipped) +
             " disabled");
    CoUninitialize();
    return 0;
}

// once per sign-in: a volatile key (gone at sign-out) says it was done, so a restarted shell does not run them again
bool first_in_session() {
    HKEY k;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\explorer-lite\\Session", 0, NULL, REG_OPTION_VOLATILE, KEY_READ, NULL, &k, &disp) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return disp == REG_CREATED_NEW_KEY;
}

// --- the filter, checked on real windows of each kind (--cmd filtertest) ----------------------------------------

LRESULT CALLBACK plain_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, msg, wp, lp); }

void filter_test() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = plain_proc;
    wc.hInstance = g_inst;
    std::wstring cls = L"ExplorerLiteFilterTest" + instance_suffix();
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    struct Case {
        const char *name;
        DWORD ex;
        bool owned, show, cloak, expect;
        HWND h;
    } cases[] = {
        { "plain", 0, false, true, false, true, NULL },
        { "tool window", WS_EX_TOOLWINDOW, false, true, false, false, NULL },
        { "owned", 0, true, true, false, false, NULL },
        { "owned + app window", WS_EX_APPWINDOW, true, true, false, true, NULL },
        { "no-activate", WS_EX_NOACTIVATE, false, true, false, false, NULL },
        { "no-activate + app window", WS_EX_NOACTIVATE | WS_EX_APPWINDOW, false, true, false, true, NULL },
        { "hidden", 0, false, false, false, false, NULL },
        { "cloaked", 0, false, true, true, false, NULL },
    };
    HWND owner = NULL;
    for (Case &c : cases) {
        std::wstring title = L"filtertest " + wide(c.name);
        c.h = CreateWindowExW(c.ex, cls.c_str(), title.c_str(), WS_OVERLAPPEDWINDOW, 40, 40, 320, 200, c.owned ? owner : NULL, NULL, g_inst, NULL);
        if (!owner) owner = c.h;
        if (c.cloak) {
            BOOL on = TRUE;
            DwmSetWindowAttribute(c.h, DWMWA_CLOAK, &on, sizeof(on));
        }
        if (c.show) ShowWindow(c.h, SW_SHOWNOACTIVATE);
    }
    scan(); // the real path: facts, filter and the bar's list
    int failed = 0, skipped = 0;
    std::string fails;
    for (Case &c : cases) {
        if (c.cloak && !facts_of(c.h).cloaked) { // DWM may not cloak on a desktop nobody looks at
            skipped++;
            continue;
        }
        bool shown = task_of(c.h) != nullptr;
        if (shown != c.expect) {
            failed++;
            fails += std::string("; ") + c.name + (shown ? " shown" : " missing");
        }
    }
    for (int i = ARRAYSIZE(cases) - 1; i >= 0; i--) DestroyWindow(cases[i].h);
    scan();
    log_line("filtertest: " + std::string(failed ? "FAILED" : "ok") + ", " + std::to_string(ARRAYSIZE(cases) - skipped) + " cases, " +
             std::to_string(failed) + " failed, " + std::to_string(skipped) + " skipped" + fails);
}

// --- the shell's own window: commands, timers, Windows' broadcasts --------------------------------------------------

void quit() {
    if (g_quitting) return;
    g_quitting = true;
    start_close();
    keys_stop();
    hooks_stop();
    bars_destroy();
    DestroyWindow(g_shell);
    PostQuitMessage(0);
}

void command(const std::wstring &line) {
    size_t tab = line.find(L'\t');
    std::wstring what = line.substr(0, tab), rest = tab == std::wstring::npos ? L"" : line.substr(tab + 1);
    if (what == L"quit") {
        if (rest == L"update") { // the installer: back with --updated
            HANDLE f = CreateFileW(restart_marker().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }
        quit();
        return;
    }
    if (what != L"cmd") return;
    size_t sp = rest.find(L' ');
    std::wstring verb = rest.substr(0, sp), arg = sp == std::wstring::npos ? L"" : rest.substr(sp + 1);
    if (verb == L"shot") { // every bar, the main monitor's first, one under the other
        std::vector<ShotPart> parts;
        for (Bar *b : g_bars) parts.push_back({ &b->s, draw_bar, b });
        bool ok = surf_shot(parts, arg);
        log_line(std::string("shell shot ") + (ok ? "ok" : "failed"));
    } else if (verb == L"startshot") log_line(std::string("start shot ") + (start_shot(arg) ? "ok" : "failed"));
    else if (verb == L"start") start_open(shell_primary(), true);
    else if (verb == L"startclose") start_close();
    else if (verb == L"filtertest") filter_test();
    else if (verb == L"tasks") { // what the bars show, into the log
        std::string s = "shell tasks:";
        for (const Task &t : g_tasks) {
            int bar = -1;
            for (int i = 0; i < (int)g_bars.size(); i++)
                if (g_bars[i]->mon == t.mon) bar = i;
            s += " [" + utf8(t.title) + ", bar " + std::to_string(bar) + "]";
        }
        log_line(s);
    } else start_command(verb, arg);
}

LRESULT CALLBACK shell_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msg_taskbar_created && g_msg_taskbar_created) {
        // explorer restarted: its appbar list is new, the bars register again
        if (g_mode == M_APPBAR) bars_create();
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case kTimerScan:
            KillTimer(h, kTimerScan);
            scan();
            break;
        case kTimerClock:
            clock_timer();
            redraw_all();
            break;
        case kTimerStartup: {
            KillTimer(h, kTimerStartup);
            HANDLE t = CreateThread(NULL, 0, startup_thread, g_mode == M_SHELL ? nullptr : (void *)1, 0, NULL);
            if (t) CloseHandle(t);
            break;
        }
        case kTimerApps: // Start's apps, while nobody waits for them: cold, after sign-in, the Apps folder takes seconds
            KillTimer(h, kTimerApps);
            start_prepare();
            break;
        case kTimerDisplay:
            KillTimer(h, kTimerDisplay);
            bars_create();
            scan();
            break;
        }
        return 0;
    case WM_APP_APPS: start_on_apps((void *)lp); return 0;
    case WM_APP_KEYS:
        if (wp == KEY_START) {
            HWND f = GetForegroundWindow();
            POINT pt;
            GetCursorPos(&pt);
            start_toggle(f && !bar_of(f) ? MonitorFromWindow(f, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY));
        }
        return 0;
    case WM_APP_THUMB: {
        Thumb *t = (Thumb *)lp;
        if (t->gen == kGenStart) {
            start_on_icon(t);
            return 0;
        }
        bool used = false;
        for (Pin &p : g_pins)
            if (!used && !p.icon && !_wcsicmp(p.target.c_str(), t->target.c_str())) p.icon = t, used = true;
        for (Task &k : g_tasks)
            if (!used && !k.icon && k.icon_from_shell && !_wcsicmp(task_target(k).c_str(), t->target.c_str())) k.icon = t, used = true;
        if (used) redraw_all();
        else delete t;
        return 0;
    }
    case WM_DISPLAYCHANGE: // monitors added, removed, rearranged: the bars again, once it settles
        SetTimer(h, kTimerDisplay, 500, NULL);
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") && theme_apply()) start_theme_changed();
        redraw_all(); // also the clock's formats (intl)
        return 0;
    case WM_TIMECHANGE:
        clock_timer();
        redraw_all();
        return 0;
    case WM_COPYDATA: {
        auto *cd = (COPYDATASTRUCT *)lp;
        if (!cd || cd->dwData != kCopyCommand || cd->cbData > 64 * 1024 || cd->cbData % sizeof(wchar_t)) return FALSE;
        // after returning: the sender is waiting in SendMessage
        auto *later = new std::wstring((const wchar_t *)cd->lpData, cd->cbData / sizeof(wchar_t));
        PostMessageW(h, WM_APP + 100, 0, (LPARAM)later);
        return TRUE;
    }
    case WM_APP + 100: {
        auto *line = (std::wstring *)lp;
        command(*line);
        delete line;
        return 0;
    }
    case WM_CLOSE: quit(); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) bars_destroy(); // the work area back, appbars removed
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// A crash: one new shell, which waits for this one to be gone; not again if it crashed within a minute of a restart.
LONG WINAPI on_crash(EXCEPTION_POINTERS *) {
    static LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    bool again = !g_restarted || GetTickCount64() - g_started > 60000;
    log_line(again ? "shell: crashed, starting again" : "shell: crashed again right after a restart, not starting again");
    if (again) {
        std::wstring cmd = L"\"" + exe_path() + L"\" --shell --restarted " + std::to_wstring(GetCurrentProcessId());
        if (!instance().empty()) cmd += L" --instance " + instance();
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

HWND shell_host() { return g_shell; }
UINT shell_monitor_dpi(HMONITOR mon) { return monitor_dpi(mon); }

RECT shell_bar_rect(HMONITOR mon) {
    for (Bar *b : g_bars)
        if (b->mon == mon) return b->rc;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT r = mi.rcMonitor;
    r.top = r.bottom - MulDiv((int)kBarH, (int)monitor_dpi(mon), 96);
    return r;
}

ShellMenu::ShellMenu(UINT dpi) : m(CreatePopupMenu()), px(MulDiv(16, (int)dpi, 96)) {
    MENUINFO mi = { sizeof(mi) };
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_CHECKORBMP; // icons in the check column, without an empty one beside them
    SetMenuInfo(m, &mi);
}

ShellMenu::~ShellMenu() {
    DestroyMenu(m); // menus do not own hbmpItem bitmaps
    for (HBITMAP b : bmps) DeleteObject(b);
}

void ShellMenu::add(UINT id, const wchar_t *glyph, const std::wstring &label) {
    HBITMAP b = glyph_bitmap(glyph, px, kText);
    if (b) bmps.push_back(b);
    std::wstring t = label;
    MENUITEMINFOW it = { sizeof(it) };
    it.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
    it.fType = MFT_STRING;
    it.wID = id;
    it.dwTypeData = &t[0];
    it.hbmpItem = b;
    InsertMenuItemW(m, (UINT)GetMenuItemCount(m), TRUE, &it);
}

void ShellMenu::sep() { AppendMenuW(m, MF_SEPARATOR, 0, NULL); }

UINT ShellMenu::show(HWND owner) {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner); // without it the menu does not close when clicking elsewhere
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, NULL);
    PostMessageW(owner, WM_NULL, 0, 0); // KB135788
    return cmd;
}
bool shell_real() { return instance().empty(); }

HMONITOR shell_primary() {
    if (!g_bars.empty()) return g_bars[0]->mon;
    POINT pt = { 0, 0 };
    return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
}

void shell_launch(const std::wstring &target, const std::wstring &args) {
    if (target.empty()) return;
    if (!_wcsicmp(target.c_str(), exe_path().c_str())) { // ourselves: this instance's (a test never opens the user's)
        std::wstring cmd = L"\"" + exe_path() + L"\"" + (args.empty() ? L"" : L" " + args);
        if (!instance().empty()) cmd += L" --instance " + instance();
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return;
    }
    size_t np = wcslen(kAppsPrefix);
    if (!_wcsnicmp(target.c_str(), kAppsPrefix, np) && target.find(L'!') != std::wstring::npos && args.empty()) {
        // a Store app: activated as explorer does, with no explorer needed
        IApplicationActivationManager *am = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, NULL, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am)))) {
            DWORD pid = 0;
            HRESULT hr = am->ActivateApplication(target.c_str() + np, NULL, AO_NONE, &pid);
            am->Release();
            if (SUCCEEDED(hr)) return;
        }
    }
    SHELLEXECUTEINFOW ei = { sizeof(ei) };
    ei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    ei.lpFile = target.c_str();
    ei.lpParameters = args.empty() ? NULL : args.c_str();
    ei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&ei)) log_line("shell: could not open " + utf8(target) + " (" + std::to_string(GetLastError()) + ")");
}

int shell_quit_running() {
    if (!FindWindowW(shell_class().c_str(), NULL)) return 0;
    return app_send(shell_class(), L"quit\tupdate", true, 0);
}

void shell_relaunch_after_update() {
    if (GetFileAttributesW(restart_marker().c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(restart_marker().c_str());
    shell_launch(exe_path(), L"--shell");
}

int shell_main(const std::wstring &cmd, bool quit_it, DWORD restarted) {
    if (restarted) { // the crashed one may not be gone yet: its mutex with it
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, restarted);
        if (old) {
            WaitForSingleObject(old, 5000);
            CloseHandle(old);
        }
    }
    HANDLE mutex = CreateMutexW(NULL, TRUE, (L"Local\\explorer-lite-shell" + instance_suffix()).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) { // one is running (Winlogon restarting it, or a command for it)
        int code = 0;
        if (quit_it) code = app_send(shell_class(), L"quit\t", true, 0);
        else if (!cmd.empty()) code = app_send(shell_class(), L"cmd\t" + cmd, false, 5000);
        if (mutex) CloseHandle(mutex);
        return code;
    }
    if (quit_it || !cmd.empty()) { // nothing running
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    g_started = GetTickCount64();
    g_restarted = restarted;
    SetUnhandledExceptionFilter(on_crash);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    theme_apply();
    if (!gfx_acquire()) {
        log_line("shell: Direct2D could not be loaded");
        return 1;
    }
    // alongside explorer (its taskbar answers appbars), or in its place; a test instance is neither
    g_mode = !shell_real() ? M_TEST : FindWindowW(L"Shell_TrayWnd", NULL) ? M_APPBAR : M_SHELL;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance = g_inst;
    wc.lpfnWndProc = shell_proc;
    std::wstring cls = shell_class();
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    wc.lpfnWndProc = bar_proc;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    std::wstring bcls = bar_class();
    wc.lpszClassName = bcls.c_str();
    RegisterClassExW(&wc);
    // a hidden top-level window, not a message-only one: those miss broadcasts (displays, settings, TaskbarCreated)
    g_shell = CreateWindowExW(WS_EX_TOOLWINDOW, cls.c_str(), L"explorer-lite shell", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!g_shell) return 1;
    g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_shell, g_msg_taskbar_created, MSGFLT_ALLOW, NULL);

    load_pins();
    bars_create();
    hooks_start();
    scan();
    HWND f = GetForegroundWindow();
    g_active = f ? GetAncestor(f, GA_ROOTOWNER) : NULL;
    clock_timer();
    SetTimer(g_shell, kTimerApps, 4000, NULL);
    // the Win key: as the session's shell, or when asked for (conf start_key=1); never in a test instance
    if (g_mode == M_SHELL || (g_mode == M_APPBAR && conf_get("start_key") == "1")) keys_start_win(g_shell, WM_APP_KEYS);
    // what starts with Windows: as the session's shell, once per sign-in; a test lists it without running anything
    if ((g_mode == M_SHELL && first_in_session()) || g_mode == M_TEST) SetTimer(g_shell, kTimerStartup, 1500, NULL);
    // files, the task manager and the tray icon: the resident (started if it is not running; a no-op if it is)
    shell_launch(exe_path(), L"--tray");
    const char *modes[] = { "test (no appbar, no work area, no Win key)", "appbar, alongside explorer", "the session's shell" };
    log_line("shell: start " EXPLORER_LITE_VERSION " in " + std::to_string((int)since_process_start_ms()) + " ms, " + std::to_string(g_bars.size()) +
             " bars, " + std::to_string(g_tasks.size()) + " windows, " + modes[g_mode] + (restarted ? ", after a crash" : ""));

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    gfx_release();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
