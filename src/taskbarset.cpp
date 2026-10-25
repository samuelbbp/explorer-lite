// taskbarset.cpp: the taskbar's settings window (see taskbar.h). A small window in the app's style, drawn with
// Direct2D: a card per option, each with two choices or a switch; a click saves it and the bars change at once.
// Arrows, Space and Esc work too. Direct2D is taken while it is open (gfx_acquire) and given back when it closes.
#include "taskbar.h"
#include "app.h"
#include <dwmapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include "conf.h"
#include "gfx.h"
#include "idioma.h"
#include "shell.h"
#include "surface.h"

TaskbarConf taskbar_conf() {
    TaskbarConf c;
    c.center = conf_get("taskbar_align") == "center";
    c.start_left = conf_get("taskbar_start") == "left";
    c.start_icon = wide(conf_get("taskbar_start_icon"));
    c.small = conf_get("taskbar_size") == "small";
    c.labels = conf_get("taskbar_labels") != "0";
    c.seconds = conf_get("taskbar_seconds") == "1";
    c.corner = conf_get("taskbar_corner") != "0";
    c.all_displays = conf_get("taskbar_all_displays") != "0";
    return c;
}

namespace {

// layout, in DIPs
const float kW = 560, kPad = 20, kHeadH = 34, kCardH = 52, kCardGap = 4, kOptH = 30, kSwitchW = 40, kSwitchH = 20, kPreview = 16;

enum RowId { R_ALIGN, R_START, R_ICON, R_SIZE, R_LABELS, R_SECONDS, R_CORNER, R_DISPLAYS, R_COUNT };
struct RowDef {
    const wchar_t *glyph, *label, *a, *b; // a, b: the two choices (0 and 1); a switch has none
};
const RowDef kRows[R_COUNT] = {
    { L"\xE8E4", L"Taskbar alignment", L"Left", L"Center" },              // translate
    { L"\xE8FC", L"Start button", L"With the apps", L"Far left" },        // translate
    { L"\xE8B9", L"Start icon", L"Default", L"Custom…" },                 // translate
    { L"\xE740", L"Taskbar size", L"Normal", L"Small" },                  // translate
    { L"\xE8D2", L"Show window titles on the buttons", nullptr, nullptr }, // translate
    { L"\xE916", L"Show seconds on the clock", nullptr, nullptr },        // translate
    { L"\xE7C4", L"Show the desktop from the far corner", nullptr, nullptr }, // translate
    { L"\xE7F4", L"Show the taskbar on all displays", nullptr, nullptr }, // translate
};
const int kLayoutRows = R_LABELS; // the first section: how it looks; then how it behaves

struct SetWin {
    Surface s;
    TaskbarConf c;
    D2D1_RECT_F cards[R_COUNT], opts[R_COUNT][2]; // a switch: both choices are the switch
    float head2 = 0, height = 0;                  // the second section's title; the content's height
    int hover = -1, hover_opt = -1, focus = 0;
    bool keyboard = false, tracking = false, picking = false;
    Thumb *icon = nullptr; // the custom Start icon, as the Start icon row shows it
};

SetWin *g_set;
std::wstring set_class() { return L"ExplorerLiteTaskbarSettings" + instance_suffix(); }

bool is_switch(int r) { return !kRows[r].a; }
bool disabled(const SetWin *w, int r) { return r == R_START && !w->c.center; } // only centered can it stay apart

int value(const TaskbarConf &c, int r) {
    switch (r) {
    case R_ALIGN: return c.center;
    case R_START: return c.start_left;
    case R_ICON: return !c.start_icon.empty();
    case R_SIZE: return c.small;
    case R_LABELS: return c.labels;
    case R_SECONDS: return c.seconds;
    case R_CORNER: return c.corner;
    case R_DISPLAYS: return c.all_displays;
    }
    return 0;
}

void load_preview(SetWin *w) {
    if (w->icon) {
        surf_forget(w->s, w->icon);
        delete w->icon;
        w->icon = nullptr;
    }
    if (w->c.start_icon.empty()) return;
    Thumb *t = new Thumb{ 0, w->c.start_icon };
    if (image_pixels(w->c.start_icon, (int)std::ceil(kPreview * w->s.dpi / 96.f), *t)) w->icon = t;
    else delete t;
}

void layout(SetWin *w) {
    Surface &s = w->s;
    float y = kPad / 2 + kHeadH, right = kW - kPad;
    for (int r = 0; r < R_COUNT; r++) {
        if (r == kLayoutRows) {
            w->head2 = y + 8;
            y += 8 + kHeadH;
        }
        w->cards[r] = D2D1::RectF(kPad, y, right, y + kCardH);
        float cy = y + kCardH / 2;
        if (is_switch(r)) {
            D2D1_RECT_F sw = D2D1::RectF(right - 16 - kSwitchW, cy - kSwitchH / 2, right - 16, cy + kSwitchH / 2);
            w->opts[r][0] = w->opts[r][1] = sw;
        } else { // two pills of the same width, the second on the right
            float tw = std::max(surf_text_width(s, T(kRows[r].a), s.f_ui), surf_text_width(s, T(kRows[r].b), s.f_ui)) + 28;
            if (r == R_ICON) tw += kPreview + 6;
            float x1 = right - 12, x0 = x1 - 2 * tw;
            w->opts[r][0] = D2D1::RectF(x0, cy - kOptH / 2, x0 + tw, cy + kOptH / 2);
            w->opts[r][1] = D2D1::RectF(x0 + tw, cy - kOptH / 2, x1, cy + kOptH / 2);
        }
        y += kCardH + kCardGap;
    }
    w->height = y + kPad / 2;
}

// the window's size for the content at its DPI (the caption and borders added)
void fit(SetWin *w, bool move) {
    RECT r = { 0, 0, (LONG)std::ceil(kW * w->s.dpi / 96.f), (LONG)std::ceil(w->height * w->s.dpi / 96.f) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, w->s.dpi);
    int cw = r.right - r.left, ch = r.bottom - r.top;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | (move ? 0 : SWP_NOMOVE);
    int x = 0, y = 0;
    if (move) { // the middle of the work area of the monitor with the pointer
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = (mi.rcWork.left + mi.rcWork.right - cw) / 2;
        y = (mi.rcWork.top + mi.rcWork.bottom - ch) / 2;
    }
    SetWindowPos(w->s.hwnd, NULL, x, y, cw, ch, flags);
}

void draw_switch(Surface &s, D2D1_RECT_F r, bool on, float a) {
    float rad = (r.bottom - r.top) / 2, k = rad - 5, cy = (r.top + r.bottom) / 2, cx = on ? r.right - rad : r.left + rad;
    if (on) surf_round(s, r, rad, kAccent, a);
    else {
        s.br->SetColor(color(kDim, a));
        s.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad), s.br, 1);
    }
    surf_round(s, D2D1::RectF(cx - k, cy - k, cx + k, cy + k), k, on ? kOnAccent : kDim, a);
}

void draw(void *ctx) {
    SetWin *w = (SetWin *)ctx;
    Surface &s = w->s;
    surf_fill(s, D2D1::RectF(0, 0, kW, w->height), kBg);
    surf_text(s, T(L"Layout"), D2D1::RectF(kPad + 2, kPad / 2, kW - kPad, kPad / 2 + kHeadH), s.f_bold, kText);
    surf_text(s, T(L"Behavior"), D2D1::RectF(kPad + 2, w->head2, kW - kPad, w->head2 + kHeadH), s.f_bold, kText);
    for (int r = 0; r < R_COUNT; r++) {
        D2D1_RECT_F c = w->cards[r];
        bool off = disabled(w, r);
        float a = off ? 0.45f : 1;
        surf_round(s, c, 6, w->hover == r && !off ? kHover : kPane);
        if (w->keyboard && w->focus == r) {
            s.br->SetColor(color(kAccent));
            s.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 1, c.top + 1, c.right - 1, c.bottom - 1), 6, 6), s.br, 2);
        }
        surf_text(s, kRows[r].glyph, D2D1::RectF(c.left + 12, c.top, c.left + 40, c.bottom), s.f_icon, kText, a);
        float text_r = (is_switch(r) ? w->opts[r][0].left - 48 : w->opts[r][0].left) - 12;
        surf_text(s, T(kRows[r].label), D2D1::RectF(c.left + 52, c.top, text_r, c.bottom), s.f_ui, kText, a);
        int v = value(w->c, r);
        if (is_switch(r)) {
            D2D1_RECT_F sw = w->opts[r][0];
            surf_text(s, v ? T(L"On") : T(L"Off"), D2D1::RectF(sw.left - 48, c.top, sw.left - 10, c.bottom), s.f_small_r, kDim, a);
            draw_switch(s, sw, v != 0, a);
            continue;
        }
        D2D1_RECT_F both = D2D1::RectF(w->opts[r][0].left, w->opts[r][0].top, w->opts[r][1].right, w->opts[r][1].bottom);
        surf_round(s, both, 6, kBg, a);
        for (int o = 0; o < 2; o++) {
            D2D1_RECT_F p = w->opts[r][o];
            bool on = v == o, hov = w->hover == r && w->hover_opt == o && !off;
            if (on) surf_round(s, D2D1::RectF(p.left + 2, p.top + 2, p.right - 2, p.bottom - 2), 5, kAccent, a);
            else if (hov) surf_round(s, D2D1::RectF(p.left + 2, p.top + 2, p.right - 2, p.bottom - 2), 5, kHover, a);
            const wchar_t *label = T(o ? kRows[r].b : kRows[r].a);
            if (r == R_ICON && o == 1 && w->icon) { // the custom icon before its name
                float tw = surf_text_width(s, label, s.f_ui), x = (p.left + p.right - tw - kPreview - 6) / 2, cy = (p.top + p.bottom) / 2;
                surf_icon(s, w->icon, D2D1::RectF(x, cy - kPreview / 2, x + kPreview, cy + kPreview / 2), a);
                surf_text(s, label, D2D1::RectF(x + kPreview + 6, p.top, p.right, p.bottom), s.f_ui, on ? kOnAccent : kText, a);
            } else surf_text(s, label, p, s.f_ui_c, on ? kOnAccent : kText, a);
        }
    }
}

void redraw(SetWin *w) { InvalidateRect(w->s.hwnd, NULL, FALSE); }

std::wstring pick_image(HWND owner) {
    Com<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(d.put())))) return L"";
    std::wstring name = T(L"Images");
    COMDLG_FILTERSPEC types[] = { { name.c_str(), L"*.png;*.ico" } };
    d->SetFileTypes(1, types);
    d->SetTitle(T(L"Start icon"));
    d->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    Com<IShellItem> it;
    PWSTR p = nullptr;
    if (FAILED(d->Show(owner)) || FAILED(d->GetResult(it.put())) || FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) return L"";
    std::wstring out = p;
    CoTaskMemFree(p);
    return out;
}

// A choice made (a click, a key or a test): saved, and the running shell reads the settings again. The Start icon's
// custom choice takes `path` (a test), else asks for a file; an image that cannot be read is not saved.
void choose(SetWin *w, int r, int v, const std::wstring &path = L"") {
    if (r < 0 || r >= R_COUNT || disabled(w, r)) return;
    switch (r) {
    case R_ALIGN: conf_set("taskbar_align", v ? "center" : "left"); break;
    case R_START: conf_set("taskbar_start", v ? "left" : "buttons"); break;
    case R_ICON: {
        std::wstring file = path;
        if (v && file.empty()) {
            if (w->picking) return;
            w->picking = true;
            file = pick_image(w->s.hwnd);
            w->picking = false;
            if (!g_set || file.empty()) return; // closed meanwhile, or cancelled
        }
        Thumb probe{ 0, file };
        if (v && !image_pixels(file, 32, probe)) {
            log_line("taskbar settings: not an image that can be read: " + utf8(file));
            return;
        }
        conf_set("taskbar_start_icon", v ? utf8(file) : "");
        break;
    }
    case R_SIZE: conf_set("taskbar_size", v ? "small" : "normal"); break;
    case R_LABELS: conf_set("taskbar_labels", v ? "1" : "0"); break;
    case R_SECONDS: conf_set("taskbar_seconds", v ? "1" : "0"); break;
    case R_CORNER: conf_set("taskbar_corner", v ? "1" : "0"); break;
    case R_DISPLAYS: conf_set("taskbar_all_displays", v ? "1" : "0"); break;
    }
    w->c = taskbar_conf();
    if (r == R_ICON) load_preview(w);
    shell_settings_changed();
    redraw(w);
}

void hit(SetWin *w, float x, float y, int *row, int *opt) {
    *row = *opt = -1;
    for (int r = 0; r < R_COUNT; r++)
        if (surf_inside(w->cards[r], x, y)) {
            *row = r;
            for (int o = 0; o < 2; o++)
                if (surf_inside(w->opts[r][o], x, y)) *opt = o;
        }
}

void click(SetWin *w, int r, int o) {
    if (r < 0) return;
    int v = value(w->c, r);
    if (is_switch(r)) choose(w, r, !v); // anywhere on its card
    else if (o >= 0 && (o != v || (r == R_ICON && o == 1))) choose(w, r, o); // Custom… again: another picture
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SetWin *w = g_set && g_set->s.hwnd == h ? g_set : nullptr;
    if (!w) return DefWindowProcW(h, msg, wp, lp);
    float k = surf_scale(w->s), x = GET_X_LPARAM(lp) / k, y = GET_Y_LPARAM(lp) / k;
    switch (msg) {
    case WM_PAINT: surf_paint(w->s, draw, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int r, o;
        hit(w, x, y, &r, &o);
        if (r != w->hover || o != w->hover_opt) {
            w->hover = r;
            w->hover_opt = o;
            redraw(w);
        }
        if (!w->tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            w->tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        w->tracking = false;
        w->hover = w->hover_opt = -1;
        redraw(w);
        return 0;
    case WM_LBUTTONUP: {
        int r, o;
        hit(w, x, y, &r, &o);
        if (r >= 0) w->focus = r;
        w->keyboard = false;
        click(w, r, o);
        return 0;
    }
    case WM_KEYDOWN: {
        int &f = w->focus, v = value(w->c, f);
        w->keyboard = true;
        switch (wp) {
        case VK_ESCAPE: PostMessageW(h, WM_CLOSE, 0, 0); return 0;
        case VK_UP: f = (f + R_COUNT - 1) % R_COUNT; break;
        case VK_DOWN:
        case VK_TAB: f = (f + 1) % R_COUNT; break;
        case VK_LEFT:
        case VK_RIGHT: // the Start icon's file is only asked for with Space, Enter or a click
            if ((wp == VK_RIGHT) != (v != 0) && !(f == R_ICON && wp == VK_RIGHT)) choose(w, f, wp == VK_RIGHT);
            break;
        case VK_SPACE:
        case VK_RETURN: choose(w, f, f == R_ICON ? 1 : !v); break;
        }
        redraw(w);
        return 0;
    }
    case WM_DPICHANGED: {
        w->s.dpi = HIWORD(wp);
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        load_preview(w);
        layout(w);
        fit(w, false);
        redraw(w);
        return 0;
    }
    case WM_CLOSE: taskbar_settings_close(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void apply_theme(SetWin *w) {
    BOOL dark = !theme_light();
    DwmSetWindowAttribute(w->s.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

} // namespace

void taskbar_settings_open() {
    if (g_set) {
        if (IsIconic(g_set->s.hwnd)) ShowWindow(g_set->s.hwnd, SW_RESTORE);
        SetForegroundWindow(g_set->s.hwnd);
        return;
    }
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        std::wstring cls = set_class();
        wc.lpszClassName = cls.c_str();
        registered = RegisterClassExW(&wc) != 0;
    }
    if (!gfx_acquire()) {
        gfx_release();
        return;
    }
    SetWin *w = new SetWin;
    w->c = taskbar_conf();
    g_set = w;
    w->s.hwnd = CreateWindowExW(0, set_class().c_str(), T(L"Taskbar settings"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                CW_USEDEFAULT, 100, 100, NULL, NULL, g_inst, NULL);
    if (!w->s.hwnd) {
        g_set = nullptr;
        delete w;
        gfx_release();
        return;
    }
    w->s.dpi = GetDpiForWindow(w->s.hwnd);
    apply_theme(w);
    surf_formats(w->s);
    load_preview(w);
    layout(w);
    fit(w, true);
    UINT dpi = GetDpiForWindow(w->s.hwnd); // moved to a monitor of another DPI
    if (dpi != w->s.dpi) {
        w->s.dpi = dpi;
        load_preview(w);
        layout(w);
        fit(w, false);
    }
    ShowWindow(w->s.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(w->s.hwnd);
    log_line("taskbar settings: open");
}

void taskbar_settings_close() {
    SetWin *w = g_set;
    if (!w) return;
    g_set = nullptr; // first: closing sends messages that must not find it
    if (w->icon) surf_forget(w->s, w->icon);
    delete w->icon;
    surf_free(w->s);
    DestroyWindow(w->s.hwnd);
    delete w;
    gfx_release(); // the last window: Direct2D goes and the working set is trimmed
}

void taskbar_settings_theme() {
    if (!g_set) return;
    apply_theme(g_set);
    redraw(g_set);
}

bool taskbar_settings_command(const std::wstring &verb, const std::wstring &arg) {
    if (verb == L"settingsshot") log_line(std::string("taskbar settings shot ") + (g_set && surf_shot(g_set->s, arg, draw, g_set) ? "ok" : "failed"));
    else if (verb == L"settingsclose") taskbar_settings_close();
    else if (verb == L"settingsset" && !shell_real()) { // settingsset <row> <0|1|image path>: as a click on that choice
        size_t sp = arg.find(L' ');
        std::wstring name = arg.substr(0, sp), v = sp == std::wstring::npos ? L"" : arg.substr(sp + 1);
        const wchar_t *names[R_COUNT] = { L"align", L"start", L"icon", L"size", L"labels", L"seconds", L"corner", L"displays" };
        int r = -1;
        for (int i = 0; i < R_COUNT; i++)
            if (name == names[i]) r = i;
        if (!g_set || r < 0) log_line("taskbar settings: settingsset needs the window open and a known row");
        else if (r == R_ICON && v.size() > 1) choose(g_set, r, 1, v);
        else choose(g_set, r, _wtoi(v.c_str()) != 0);
    } else return false;
    return true;
}
