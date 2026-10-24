// window.cpp: the explorer-lite window. Tabs (a folder or the task manager), a toolbar with back / forward / up /
// refresh, the address (breadcrumbs; an edit box on click) and search, the navigation pane and the folder in
// Details view: only the rows on screen are drawn, so a folder of 100 000 files costs the same to paint as one of 10.
// Drawn with Direct2D in DIPs (rt->SetDpi): every size here is at 96 DPI and scales per monitor.
#include "app.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "conf.h"
#include "folder.h"
#include "gfx.h"
#include "idioma.h"
#include "tasks.h"
#include "update.h"

namespace {

// layout, in DIPs
const float kTabH = 34, kToolH = 40, kNavW = 210, kHeadH = 30, kRowH = 28, kStatusH = 26, kScrollW = 10;
const float kColDate = 150, kColType = 170, kColSize = 110, kNameMin = 180;

enum Hit {
    H_NONE, H_TAB, H_TAB_CLOSE, H_TAB_NEW, H_BACK, H_FWD, H_UP, H_REFRESH, H_CRUMB, H_ADDRESS, H_NAV, H_HEAD, H_ROW,
    H_SCROLL, H_LIST, H_UPDATE, H_TASKS,
};

struct Region {
    D2D1_RECT_F r;
    int id, arg;
};

struct View {
    int kind = TAB_FILES;
    std::wstring path;                 // "" = Home
    std::vector<std::wstring> back, fwd;
    std::vector<Item> all;             // what the folder holds
    std::vector<int> shown;            // indexes into all: the filter applied, in order
    std::vector<uint8_t> sel;          // per index in all
    int focus = -1, anchor = -1;       // positions in shown
    int sort = COL_NAME;
    bool desc = false;
    float scroll = 0;                  // DIPs
    uint64_t gen = 0;                  // the listing this view waits for
    bool loading = false;
    std::wstring error, filter;
    std::wstring select_after;         // after going up: the folder we came from is selected
    double list_ms = 0;
    TasksView *tasks = nullptr;
};

struct Win {
    HWND hwnd = NULL;
    ID2D1HwndRenderTarget *rt = nullptr;
    ID2D1RenderTarget *dc = nullptr;   // what is drawn on: rt, or a bitmap for --cmd shot
    ID2D1SolidColorBrush *br = nullptr;
    IDWriteTextFormat *f_ui = nullptr, *f_ui_c = nullptr, *f_small = nullptr, *f_icon = nullptr, *f_icon_s = nullptr, *f_title = nullptr;
    std::unordered_map<int, ID2D1Bitmap *> icons;
    std::vector<View> views;
    int cur = 0;
    std::vector<Region> regions;
    int hover = H_NONE, hover_arg = -1, press = H_NONE, press_arg = -1;
    bool tracking = false;
    HWND edit = NULL, search = NULL;   // the address while typing, and the search box
    HFONT edit_font = NULL;
    UINT dpi = 96;
    bool drag_scroll = false;
    float drag_dy = 0;
    std::wstring typed;                // type-ahead: what was typed in the last second
    DWORD typed_at = 0;
    uint64_t last_used = 0;
    double opened_at = 0;              // when window_open was called, until the first frame is drawn
    double t_created = 0, t_target = 0; // how long the window and the render target took (logged with the first frame)
    std::vector<Place> places, drives;
};

std::vector<Win *> g_wins;
bool g_shot; // drawing for --cmd shot: the edit controls are not in the bitmap, so their text is drawn
uint64_t g_gen, g_use;
IWICImagingFactory *g_wic;
HIMAGELIST g_sysicons;

std::wstring win_class() { return L"ExplorerLiteWindow" + instance_suffix(); }

Win *win_of(HWND h) { return (Win *)GetWindowLongPtrW(h, GWLP_USERDATA); }
float scale(Win *w) { return w->dpi / 96.f; }
View &cur_view(Win *w) { return w->views[w->cur]; }

template <class T> void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

// --- geometry --------------------------------------------------------------------------------------

D2D1_RECT_F client(Win *w) {
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    float s = scale(w);
    return D2D1::RectF(0, 0, rc.right / s, rc.bottom / s);
}

D2D1_RECT_F list_rect(Win *w) {
    D2D1_RECT_F c = client(w);
    return D2D1::RectF(kNavW, kTabH + kToolH + kHeadH, c.right, c.bottom - kStatusH);
}

bool inside(const D2D1_RECT_F &r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

float name_width(Win *w) {
    D2D1_RECT_F l = list_rect(w);
    return std::max(kNameMin, (l.right - l.left - kScrollW) - kColDate - kColType - kColSize - 16);
}

float content_h(View &v) { return v.shown.size() * kRowH; }

void clamp_scroll(Win *w, View &v) {
    D2D1_RECT_F l = list_rect(w);
    float max = std::max(0.f, content_h(v) - (l.bottom - l.top));
    v.scroll = std::min(std::max(v.scroll, 0.f), max);
}

void ensure_visible(Win *w, View &v, int pos) {
    if (pos < 0) return;
    D2D1_RECT_F l = list_rect(w);
    float top = pos * kRowH, bottom = top + kRowH, h = l.bottom - l.top;
    if (top < v.scroll) v.scroll = top;
    else if (bottom > v.scroll + h) v.scroll = bottom - h;
    clamp_scroll(w, v);
}

void redraw(Win *w) { InvalidateRect(w->hwnd, NULL, FALSE); }

// --- text ------------------------------------------------------------------------------------------

IDWriteTextFormat *make_format(const wchar_t *family, float size, DWRITE_FONT_WEIGHT weight, bool center) {
    IDWriteTextFormat *f = nullptr;
    if (FAILED(g_dw->CreateTextFormat(family, NULL, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteInlineObject *dots = nullptr;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(f, &dots))) {
        DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        f->SetTrimming(&t, dots);
        dots->Release();
    }
    return f;
}

void make_formats(Win *w) {
    const wchar_t *ui = L"Segoe UI Variable Text";
    w->f_ui = make_format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
    w->f_ui_c = make_format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    w->f_small = make_format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
    w->f_title = make_format(ui, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    w->f_icon = make_format(icon_font(), 14, DWRITE_FONT_WEIGHT_NORMAL, true);
    w->f_icon_s = make_format(icon_font(), 10, DWRITE_FONT_WEIGHT_NORMAL, true);
}

void text(Win *w, const std::wstring &s, D2D1_RECT_F r, IDWriteTextFormat *f, uint32_t rgb, float a = 1) {
    if (!f || s.empty() || r.right <= r.left) return;
    w->br->SetColor(color(rgb, a));
    w->dc->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, w->br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float text_width(Win *w, const std::wstring &s, IDWriteTextFormat *f) {
    IDWriteTextLayout *l = nullptr;
    if (!f || FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

void fill(Win *w, D2D1_RECT_F r, uint32_t rgb, float a = 1) {
    w->br->SetColor(color(rgb, a));
    w->dc->FillRectangle(r, w->br);
}

void fill_round(Win *w, D2D1_RECT_F r, float rad, uint32_t rgb, float a = 1) {
    w->br->SetColor(color(rgb, a));
    w->dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), w->br);
}

void region(Win *w, D2D1_RECT_F r, int id, int arg = 0) { w->regions.push_back({ r, id, arg }); }
bool hovered(Win *w, int id, int arg = 0) { return w->hover == id && w->hover_arg == arg; }

// an icon button: the glyph centred, a rounded hover
void button(Win *w, D2D1_RECT_F r, const wchar_t *glyph, int id, bool enabled = true) {
    if (enabled) {
        region(w, r, id);
        if (hovered(w, id)) fill_round(w, r, 4, w->press == id ? kPress : kHover);
    }
    text(w, glyph, r, w->f_icon, enabled ? kText : kDim, enabled ? 1 : 0.5f);
}

// --- system icons as Direct2D bitmaps ------------------------------------------------------------------

ID2D1Bitmap *icon_bitmap(Win *w, int index) {
    if (index < 0) return nullptr;
    auto it = w->icons.find(index);
    if (it != w->icons.end()) return it->second;
    ID2D1Bitmap *bmp = nullptr;
    if (!g_sysicons) {
        SHFILEINFOW fi = {};
        g_sysicons = (HIMAGELIST)SHGetFileInfoW(L"x", FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi), SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    }
    if (!g_wic) CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    HICON icon = g_sysicons ? ImageList_GetIcon(g_sysicons, index, ILD_NORMAL) : NULL;
    if (icon && g_wic) {
        IWICBitmap *wb = nullptr;
        IWICFormatConverter *conv = nullptr;
        if (SUCCEEDED(g_wic->CreateBitmapFromHICON(icon, &wb)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)))
            w->dc->CreateBitmapFromWicBitmap(conv, NULL, &bmp);
        release(conv);
        release(wb);
    }
    if (icon) DestroyIcon(icon);
    w->icons[index] = bmp; // also a failure: not asked again
    return bmp;
}

void draw_icon(Win *w, int index, float x, float y, float size) {
    ID2D1Bitmap *b = icon_bitmap(w, index);
    if (b) w->dc->DrawBitmap(b, D2D1::RectF(x, y, x + size, y + size), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void task_icon(void *ctx, int index, float x, float y, float size) { draw_icon((Win *)ctx, index, x, y, size); }

// --- the folder ----------------------------------------------------------------------------------------

std::wstring view_title(View &v) { return v.kind == TAB_TASKS ? std::wstring(T(L"Task manager")) : folder_title(v.path); }

void set_title(Win *w) {
    std::wstring t = view_title(cur_view(w)) + L" - explorer-lite";
    SetWindowTextW(w->hwnd, t.c_str());
}

void apply_filter(View &v) {
    // the selection and focus follow the items, not the positions
    int focus_item = v.focus >= 0 && v.focus < (int)v.shown.size() ? v.shown[v.focus] : -1;
    v.shown.clear();
    for (int i = 0; i < (int)v.all.size(); i++)
        if (v.filter.empty() || StrStrIW(v.all[i].name.c_str(), v.filter.c_str())) v.shown.push_back(i);
    v.focus = -1;
    for (int p = 0; p < (int)v.shown.size(); p++)
        if (v.shown[p] == focus_item) v.focus = p;
    v.anchor = v.focus;
}

void load(Win *w, View &v) {
    if (v.gen) folder_forget(v.gen);
    v.gen = ++g_gen;
    v.loading = true;
    folder_list(w->hwnd, v.gen, v.path);
    redraw(w);
}

void sync_search(Win *w);

void navigate(Win *w, const std::wstring &path, bool remember = true) {
    View &v = cur_view(w);
    if (v.kind != TAB_FILES) return;
    if (remember && _wcsicmp(path.c_str(), v.path.c_str())) {
        v.back.push_back(v.path);
        v.fwd.clear();
    }
    v.path = path;
    v.all.clear();
    v.shown.clear();
    v.sel.clear();
    v.focus = v.anchor = -1;
    v.scroll = 0;
    v.error.clear();
    v.filter.clear();
    sync_search(w);
    set_title(w);
    load(w, v);
}

void go_back(Win *w) {
    View &v = cur_view(w);
    if (v.back.empty()) return;
    std::wstring to = v.back.back();
    v.back.pop_back();
    v.fwd.push_back(v.path);
    v.select_after = v.path;
    navigate(w, to, false);
}

void go_forward(Win *w) {
    View &v = cur_view(w);
    if (v.fwd.empty()) return;
    std::wstring to = v.fwd.back();
    v.fwd.pop_back();
    v.back.push_back(v.path);
    navigate(w, to, false);
}

void go_up(Win *w) {
    View &v = cur_view(w);
    if (v.path.empty()) return;
    std::wstring from = v.path;
    navigate(w, folder_parent(v.path));
    cur_view(w).select_after = from;
}

void on_listing(Win *w, Listing *l) {
    log_line("listed " + std::to_string(l->items.size()) + " items in " + std::to_string((int)l->ms) + " ms");
    for (View &v : w->views) {
        if (v.gen != l->gen) continue;
        std::wstring keep_focus = v.focus >= 0 && v.focus < (int)v.shown.size() ? v.all[v.shown[v.focus]].name : L"";
        float keep_scroll = v.scroll;
        bool refresh = !v.all.empty();
        v.all = std::move(l->items);
        v.error = l->error;
        v.loading = false;
        v.list_ms = l->ms;
        if (!v.path.empty()) folder_sort(v.all, v.sort, v.desc); // Home keeps its order: places, then drives
        v.sel.assign(v.all.size(), 0);
        v.focus = -1;
        apply_filter(v);
        std::wstring want = !v.select_after.empty() ? v.select_after : keep_focus;
        for (int p = 0; p < (int)v.shown.size() && !want.empty(); p++) {
            const Item &it = v.all[v.shown[p]];
            if (!_wcsicmp(it.target.c_str(), want.c_str()) || !_wcsicmp(it.name.c_str(), want.c_str())) {
                v.focus = v.anchor = p;
                v.sel[v.shown[p]] = 1;
                break;
            }
        }
        v.select_after.clear();
        if (refresh) v.scroll = keep_scroll;
        clamp_scroll(w, v);
        if (!refresh) ensure_visible(w, v, v.focus);
    }
    delete l;
    redraw(w);
}

void open_item(Win *w, const Item &it, bool new_tab = false);

void new_tab(Win *w, const std::wstring &path, int kind = TAB_FILES) {
    View v;
    v.kind = kind;
    v.path = path;
    if (kind == TAB_TASKS) v.tasks = tasks_create(w->hwnd, task_icon, w);
    w->views.push_back(std::move(v));
    w->cur = (int)w->views.size() - 1;
    sync_search(w);
    set_title(w);
    if (kind == TAB_FILES) load(w, cur_view(w));
    redraw(w);
}

void close_tab(Win *w, int i) {
    if (i < 0 || i >= (int)w->views.size()) return;
    if (w->views.size() == 1) {
        DestroyWindow(w->hwnd);
        return;
    }
    if (w->views[i].tasks) tasks_destroy(w->views[i].tasks);
    if (w->views[i].gen) folder_forget(w->views[i].gen);
    w->views.erase(w->views.begin() + i);
    if (w->cur >= (int)w->views.size()) w->cur = (int)w->views.size() - 1;
    else if (w->cur > i) w->cur--;
    sync_search(w);
    set_title(w);
    redraw(w);
}

void switch_tab(Win *w, int i) {
    if (i < 0 || i >= (int)w->views.size() || i == w->cur) return;
    w->cur = i;
    sync_search(w);
    set_title(w);
    redraw(w);
}

void open_item(Win *w, const Item &it, bool in_new_tab) {
    if (it.dir) {
        if (in_new_tab) new_tab(w, it.target);
        else navigate(w, it.target);
        return;
    }
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = w->hwnd;
    sei.lpFile = it.target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    std::wstring dir = cur_view(w).path;
    sei.lpDirectory = dir.empty() ? NULL : dir.c_str();
    if (!ShellExecuteExW(&sei)) {
        sei.fMask = SEE_MASK_ASYNCOK; // no program for it: Windows' own «Open with» this time
        ShellExecuteExW(&sei);
    }
}

std::vector<int> selected(View &v) {
    std::vector<int> out;
    for (int p = 0; p < (int)v.shown.size(); p++)
        if (v.sel[v.shown[p]]) out.push_back(v.shown[p]);
    return out;
}

void select_only(View &v, int pos) {
    std::fill(v.sel.begin(), v.sel.end(), 0);
    if (pos >= 0 && pos < (int)v.shown.size()) v.sel[v.shown[pos]] = 1;
    v.focus = v.anchor = pos;
}

void select_range(View &v, int pos) {
    std::fill(v.sel.begin(), v.sel.end(), 0);
    int a = v.anchor < 0 ? pos : v.anchor;
    for (int p = std::min(a, pos); p <= std::max(a, pos); p++) v.sel[v.shown[p]] = 1;
    v.focus = pos;
}

void set_sort(Win *w, int col) {
    View &v = cur_view(w);
    if (v.sort == col) v.desc = !v.desc;
    else {
        v.sort = col;
        v.desc = col == COL_DATE || col == COL_SIZE; // newest and biggest first, as Windows does
    }
    std::vector<std::wstring> keep;
    for (int i : selected(v)) keep.push_back(v.all[i].name);
    std::wstring focus = v.focus >= 0 ? v.all[v.shown[v.focus]].name : L"";
    folder_sort(v.all, v.sort, v.desc);
    std::fill(v.sel.begin(), v.sel.end(), 0);
    for (size_t i = 0; i < v.all.size(); i++)
        if (std::find(keep.begin(), keep.end(), v.all[i].name) != keep.end()) v.sel[i] = 1;
    v.focus = -1;
    apply_filter(v);
    for (int p = 0; p < (int)v.shown.size(); p++)
        if (v.all[v.shown[p]].name == focus) v.focus = v.anchor = p;
    ensure_visible(w, v, v.focus);
    redraw(w);
}

// --- the address box and the search box (real edit controls, over the Direct2D surface) -------------------

HBRUSH g_edit_brush;

LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    Win *w = (Win *)data;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN && id == 1) { // the address: go there
            int n = GetWindowTextLengthW(h);
            std::wstring s(n, 0);
            GetWindowTextW(h, &s[0], n + 1);
            std::wstring path = folder_normalize(s);
            ShowWindow(h, SW_HIDE);
            SetFocus(w->hwnd);
            if (folder_exists(path)) navigate(w, path);
            else {
                // a file, a URL or a program: as Windows' Run would
                ShellExecuteW(w->hwnd, NULL, s.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (id == 1) ShowWindow(h, SW_HIDE);
            else SetWindowTextW(h, L"");
            SetFocus(w->hwnd);
            return 0;
        }
        if (wp == VK_DOWN || (wp == VK_RETURN && id == 2)) { // from search to the results
            SetFocus(w->hwnd);
            View &v = cur_view(w);
            if (v.focus < 0 && !v.shown.empty()) select_only(v, 0);
            redraw(w);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0; // no beep
    if (msg == WM_KILLFOCUS && id == 1) ShowWindow(h, SW_HIDE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, edit_proc, id);
    return DefSubclassProc(h, msg, wp, lp);
}

void place_edits(Win *w) {
    if (!w->search) return;
    float s = scale(w);
    D2D1_RECT_F c = client(w);
    float sw = std::min(280.f, std::max(140.f, (c.right - kNavW) * 0.28f));
    D2D1_RECT_F r = D2D1::RectF(c.right - sw - 10, kTabH + 7, c.right - 10, kTabH + kToolH - 7);
    // inset: the rounded box is drawn around it
    int x = (int)std::lround((r.left + 30) * s), y = (int)std::lround((r.top + 6) * s);
    SetWindowPos(w->search, NULL, x, y, (int)std::lround((r.right - 8) * s) - x, (int)std::lround((r.bottom - 5) * s) - y, SWP_NOZORDER | SWP_NOACTIVATE);
    bool files = cur_view(w).kind == TAB_FILES;
    ShowWindow(w->search, files ? SW_SHOWNA : SW_HIDE);
}

void sync_search(Win *w) {
    if (!w->search || w->views.empty()) return;
    View &v = cur_view(w);
    SetWindowTextW(w->search, v.filter.c_str());
    std::wstring cue = Tf(L"Search {0}", { view_title(v) });
    SendMessageW(w->search, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
    place_edits(w);
}

void edit_fonts(Win *w) {
    if (w->edit_font) DeleteObject(w->edit_font);
    w->edit_font = CreateFontW(-MulDiv(13, (int)w->dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (w->edit) SendMessageW(w->edit, WM_SETFONT, (WPARAM)w->edit_font, TRUE);
    if (w->search) SendMessageW(w->search, WM_SETFONT, (WPARAM)w->edit_font, TRUE);
}

D2D1_RECT_F address_rect(Win *w) {
    D2D1_RECT_F c = client(w);
    float sw = std::min(280.f, std::max(140.f, (c.right - kNavW) * 0.28f));
    return D2D1::RectF(4 * 36 + 16, kTabH + 7, c.right - sw - 20, kTabH + kToolH - 7);
}

void edit_address(Win *w) {
    View &v = cur_view(w);
    if (v.kind != TAB_FILES) return;
    float s = scale(w);
    D2D1_RECT_F r = address_rect(w);
    SetWindowTextW(w->edit, v.path.empty() ? T(L"Home") : v.path.c_str());
    int x = (int)std::lround((r.left + 8) * s), y = (int)std::lround((r.top + 6) * s);
    SetWindowPos(w->edit, HWND_TOP, x, y, (int)std::lround((r.right - 8) * s) - x, (int)std::lround((r.bottom - 5) * s) - y, SWP_SHOWWINDOW);
    SetFocus(w->edit);
    SendMessageW(w->edit, EM_SETSEL, 0, -1);
}

// --- painting ----------------------------------------------------------------------------------------------

void draw_tabs(Win *w, D2D1_RECT_F c) {
    fill(w, D2D1::RectF(0, 0, c.right, kTabH), kTabBar);
    float x = 8, tw = std::min(220.f, std::max(110.f, (c.right - 60) / std::max<size_t>(1, w->views.size())));
    for (int i = 0; i < (int)w->views.size(); i++) {
        D2D1_RECT_F r = D2D1::RectF(x, 5, x + tw, kTabH);
        bool on = i == w->cur;
        region(w, r, H_TAB, i);
        if (on) fill_round(w, D2D1::RectF(r.left, r.top, r.right, r.bottom + 6), 7, kPane);
        else if (hovered(w, H_TAB, i) || hovered(w, H_TAB_CLOSE, i)) fill_round(w, D2D1::RectF(r.left, r.top + 2, r.right, r.bottom - 3), 6, kHover);
        View &v = w->views[i];
        const wchar_t *glyph = v.kind == TAB_TASKS ? L"\xE9D9" : L"\xE8B7";
        text(w, glyph, D2D1::RectF(r.left + 8, r.top, r.left + 28, r.bottom), w->f_icon_s, on ? kAccent : kDim);
        text(w, view_title(v), D2D1::RectF(r.left + 32, r.top, r.right - 30, r.bottom), w->f_ui, on ? kText : kDim);
        D2D1_RECT_F cx = D2D1::RectF(r.right - 28, r.top + 5, r.right - 6, r.bottom - 5);
        region(w, cx, H_TAB_CLOSE, i);
        if (hovered(w, H_TAB_CLOSE, i)) fill_round(w, cx, 4, kPress);
        if (on || hovered(w, H_TAB, i) || hovered(w, H_TAB_CLOSE, i)) text(w, L"\xE711", cx, w->f_icon_s, kDim);
        x += tw + 2;
    }
    D2D1_RECT_F plus = D2D1::RectF(x + 4, 7, x + 32, kTabH - 3);
    region(w, plus, H_TAB_NEW);
    if (hovered(w, H_TAB_NEW)) fill_round(w, plus, 4, kHover);
    text(w, L"\xE710", plus, w->f_icon_s, kText);
}

void draw_toolbar(Win *w, D2D1_RECT_F c) {
    View &v = cur_view(w);
    fill(w, D2D1::RectF(0, kTabH, c.right, kTabH + kToolH), kPane);
    bool files = v.kind == TAB_FILES;
    float y0 = kTabH + 4, y1 = kTabH + kToolH - 4;
    button(w, D2D1::RectF(8, y0, 40, y1), L"\xE72B", H_BACK, files && !v.back.empty());
    button(w, D2D1::RectF(44, y0, 76, y1), L"\xE72A", H_FWD, files && !v.fwd.empty());
    button(w, D2D1::RectF(80, y0, 112, y1), L"\xE74A", H_UP, files && !v.path.empty());
    button(w, D2D1::RectF(116, y0, 148, y1), L"\xE72C", H_REFRESH, true);
    if (!files) {
        D2D1_RECT_F bar = D2D1::RectF(160, kTabH, c.right - 10, kTabH + kToolH);
        tasks_toolbar(v.tasks, w->dc, w->br, w->f_ui, w->f_icon_s, bar);
        region(w, bar, H_LIST); // its page buttons: tasks_click and tasks_row_at tell them apart
        return;
    }
    // the address: breadcrumbs (each part goes there); a click on the empty part edits it
    D2D1_RECT_F a = address_rect(w);
    fill_round(w, a, 5, kBg);
    region(w, a, H_ADDRESS);
    std::vector<std::pair<std::wstring, std::wstring>> parts; // label, path
    parts.push_back({ T(L"Home"), L"" });
    if (!v.path.empty()) {
        std::vector<std::wstring> chain;
        for (std::wstring p = v.path; !p.empty(); p = folder_parent(p)) chain.push_back(p);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) parts.push_back({ folder_title(*it), *it });
    }
    // too long: the first ones are dropped (the last ones are where you are)
    float avail = a.right - a.left - 16, total = 0;
    std::vector<float> widths;
    for (auto &p : parts) {
        widths.push_back(text_width(w, p.first, w->f_ui) + 26);
        total += widths.back();
    }
    size_t first = 0;
    while (total > avail && first + 1 < parts.size()) total -= widths[first++];
    float x = a.left + 8;
    for (size_t i = first; i < parts.size(); i++) {
        D2D1_RECT_F r = D2D1::RectF(x, a.top + 3, std::min(a.right - 4, x + widths[i] - 14), a.bottom - 3);
        region(w, r, H_CRUMB, (int)i);
        if (hovered(w, H_CRUMB, (int)i)) fill_round(w, r, 4, kHover);
        text(w, parts[i].first, D2D1::RectF(r.left + 6, r.top, r.right, r.bottom), w->f_ui, kText);
        x = r.right;
        if (i + 1 < parts.size()) text(w, L"\xE76C", D2D1::RectF(x, a.top, x + 14, a.bottom), w->f_icon_s, kDim);
        x += 14;
    }
    // the search box's frame (the edit control is inside)
    float sw = std::min(280.f, std::max(140.f, (c.right - kNavW) * 0.28f));
    D2D1_RECT_F s = D2D1::RectF(c.right - sw - 10, a.top, c.right - 10, a.bottom);
    fill_round(w, s, 5, kBg);
    text(w, L"\xE721", D2D1::RectF(s.left + 6, s.top, s.left + 28, s.bottom), w->f_icon_s, kDim);
    if (g_shot) {
        std::wstring t = v.filter.empty() ? Tf(L"Search {0}", { view_title(v) }) : v.filter;
        text(w, t, D2D1::RectF(s.left + 32, s.top, s.right - 8, s.bottom), w->f_ui, v.filter.empty() ? kDim : kText);
    }
}

void draw_nav(Win *w, D2D1_RECT_F c) {
    View &v = cur_view(w);
    D2D1_RECT_F pane = D2D1::RectF(0, kTabH + kToolH, kNavW, c.bottom - kStatusH);
    fill(w, pane, kPane);
    float y = pane.top + 8;
    int i = 0;
    auto row = [&](const Place &p, int idx) {
        D2D1_RECT_F r = D2D1::RectF(6, y, kNavW - 6, y + 30);
        bool on = v.kind == TAB_FILES && !_wcsicmp(p.path.c_str(), v.path.c_str());
        region(w, r, H_NAV, idx);
        if (on) fill_round(w, r, 5, kTabOn);
        else if (hovered(w, H_NAV, idx)) fill_round(w, r, 5, kHover);
        if (on) fill_round(w, D2D1::RectF(r.left, r.top + 8, r.left + 3, r.bottom - 8), 1.5f, kAccent);
        text(w, p.glyph, D2D1::RectF(r.left + 8, r.top, r.left + 32, r.bottom), w->f_icon, on ? kAccent : kDim);
        text(w, p.label, D2D1::RectF(r.left + 38, r.top, r.right - 4, r.bottom), w->f_ui, kText);
        y += 32;
    };
    for (const Place &p : w->places) row(p, i++);
    y += 6;
    fill(w, D2D1::RectF(14, y, kNavW - 14, y + 1), kLine);
    y += 8;
    for (const Place &p : w->drives) row(p, i++);
    y += 6;
    fill(w, D2D1::RectF(14, y, kNavW - 14, y + 1), kLine);
    y += 8;
    Place tm = { T(L"Task manager"), L"\x01tasks", L"\xE9D9" };
    D2D1_RECT_F r = D2D1::RectF(6, y, kNavW - 6, y + 30);
    region(w, r, H_TASKS);
    if (v.kind == TAB_TASKS) fill_round(w, r, 5, kTabOn);
    else if (hovered(w, H_TASKS)) fill_round(w, r, 5, kHover);
    text(w, tm.glyph, D2D1::RectF(r.left + 8, r.top, r.left + 32, r.bottom), w->f_icon, v.kind == TAB_TASKS ? kAccent : kDim);
    text(w, tm.label, D2D1::RectF(r.left + 38, r.top, r.right - 4, r.bottom), w->f_ui, kText);
}

void draw_list(Win *w, D2D1_RECT_F c) {
    View &v = cur_view(w);
    D2D1_RECT_F l = list_rect(w);
    fill(w, D2D1::RectF(kNavW, kTabH + kToolH, c.right, c.bottom - kStatusH), kBg);
    fill(w, D2D1::RectF(kNavW, kTabH + kToolH, kNavW + 1, c.bottom - kStatusH), kLine);
    // header
    float nw = name_width(w);
    float xs[COL_COUNT + 1];
    xs[COL_NAME] = l.left + 8;
    xs[COL_DATE] = xs[COL_NAME] + nw;
    xs[COL_TYPE] = xs[COL_DATE] + kColDate;
    xs[COL_SIZE] = xs[COL_TYPE] + kColType;
    xs[COL_COUNT] = xs[COL_SIZE] + kColSize;
    const wchar_t *heads[COL_COUNT] = { L"Name", L"Date modified", L"Type", L"Size" }; // translate
    float hy = kTabH + kToolH;
    for (int i = 0; i < COL_COUNT; i++) {
        D2D1_RECT_F r = D2D1::RectF(xs[i] - 4, hy + 2, xs[i + 1] - 4, hy + kHeadH - 2);
        region(w, r, H_HEAD, i);
        if (hovered(w, H_HEAD, i)) fill_round(w, r, 4, kHover);
        text(w, T(heads[i]), D2D1::RectF(r.left + 8, r.top, r.right - 18, r.bottom), w->f_small, kDim);
        if (v.sort == i) text(w, v.desc ? L"\xE70D" : L"\xE70E", D2D1::RectF(r.right - 18, r.top, r.right - 4, r.bottom), w->f_icon_s, kDim);
        if (i) fill(w, D2D1::RectF(xs[i] - 5, hy + 8, xs[i] - 4, hy + kHeadH - 8), kLine);
    }
    fill(w, D2D1::RectF(l.left, l.top - 1, l.right, l.top), kLine);
    region(w, l, H_LIST);

    w->dc->PushAxisAlignedClip(l, D2D1_ANTIALIAS_MODE_ALIASED);
    if (v.shown.empty()) {
        std::wstring msg = v.loading ? T(L"Loading…") : !v.error.empty() ? v.error : !v.filter.empty() ? T(L"No items match your search.") : T(L"This folder is empty.");
        text(w, msg, D2D1::RectF(l.left, l.top + 20, l.right, l.top + 60), w->f_ui_c, !v.error.empty() ? kDanger : kDim);
    }
    int first = (int)(v.scroll / kRowH), last = std::min((int)v.shown.size(), (int)((v.scroll + (l.bottom - l.top)) / kRowH) + 1);
    for (int p = std::max(0, first); p < last; p++) {
        const Item &it = v.all[v.shown[p]];
        float y = l.top + p * kRowH - v.scroll;
        D2D1_RECT_F row = D2D1::RectF(l.left + 4, y + 1, l.right - kScrollW - 2, y + kRowH - 1);
        bool sel = v.sel[v.shown[p]] != 0, hot = hovered(w, H_ROW, p);
        if (sel) fill_round(w, row, 4, kSelect, GetFocus() == w->hwnd ? 1 : 0.6f);
        else if (hot) fill_round(w, row, 4, kHover);
        if (p == v.focus && GetFocus() == w->hwnd && !sel) {
            w->br->SetColor(color(kDim, 0.6f));
            w->dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(row.left + 0.5f, row.top + 0.5f, row.right - 0.5f, row.bottom - 0.5f), 4, 4), w->br, 1);
        }
        draw_icon(w, it.icon, xs[COL_NAME] + 4, y + (kRowH - 16) / 2, 16);
        bool hidden_look = (it.attr & FILE_ATTRIBUTE_HIDDEN) != 0;
        text(w, it.name, D2D1::RectF(xs[COL_NAME] + 28, y, xs[COL_DATE] - 8, y + kRowH), w->f_ui, kText, hidden_look ? 0.6f : 1);
        text(w, it.date, D2D1::RectF(xs[COL_DATE] + 4, y, xs[COL_TYPE] - 8, y + kRowH), w->f_ui, kDim);
        text(w, it.type, D2D1::RectF(xs[COL_TYPE] + 4, y, xs[COL_SIZE] - 8, y + kRowH), w->f_ui, kDim);
        // sizes right-aligned, as numbers are
        if (!it.size.empty()) {
            float tw = text_width(w, it.size, w->f_ui);
            text(w, it.size, D2D1::RectF(std::max(xs[COL_SIZE] + 4, xs[COL_COUNT] - 12 - tw), y, xs[COL_COUNT] - 8, y + kRowH), w->f_ui, kDim);
        }
    }
    w->dc->PopAxisAlignedClip();

    // scrollbar
    float h = l.bottom - l.top, total = content_h(v);
    if (total > h) {
        float th = std::max(30.f, h * h / total), ty = l.top + (h - th) * (v.scroll / (total - h));
        D2D1_RECT_F track = D2D1::RectF(l.right - kScrollW, l.top, l.right, l.bottom);
        region(w, track, H_SCROLL);
        bool hot = hovered(w, H_SCROLL) || w->drag_scroll;
        fill_round(w, D2D1::RectF(l.right - (hot ? 8 : 5), ty + 2, l.right - 2, ty + th - 2), 3, kDim, hot ? 0.8f : 0.45f);
    }
}

void draw_status(Win *w, D2D1_RECT_F c) {
    View &v = cur_view(w);
    D2D1_RECT_F s = D2D1::RectF(0, c.bottom - kStatusH, c.right, c.bottom);
    fill(w, s, kPane);
    fill(w, D2D1::RectF(0, s.top, c.right, s.top + 1), kLine);
    std::wstring left;
    if (v.kind == TAB_FILES) {
        size_t n = v.shown.size(), k = 0;
        for (int i : v.shown) k += v.sel[i] ? 1 : 0;
        left = n == 1 ? std::wstring(T(L"1 item")) : Tf(L"{0} items", { std::to_wstring(n) });
        if (k) left += L"   " + (k == 1 ? std::wstring(T(L"1 selected")) : Tf(L"{0} selected", { std::to_wstring(k) }));
        if (!v.filter.empty()) left += L"   " + Tf(L"of {0}", { std::to_wstring(v.all.size()) });
    } else {
        left = tasks_status(v.tasks);
    }
    text(w, left, D2D1::RectF(s.left + 12, s.top, s.right * 0.6f, s.bottom), w->f_small, kDim);
    // the update: progress while it downloads, then «Restart to update»
    if (g_upd.state == UP_DOWNLOADING && g_upd.size > 0) {
        float frac = (float)g_upd.done / (float)g_upd.size, bw = 140;
        D2D1_RECT_F bar = D2D1::RectF(s.right - bw - 12, s.top + 11, s.right - 12, s.top + 15);
        fill_round(w, bar, 2, kLine);
        fill_round(w, D2D1::RectF(bar.left, bar.top, bar.left + bw * std::min(1.f, frac), bar.bottom), 2, kAccent);
        std::wstring t = Tf(L"Downloading {0}… {1}%", { wide(g_upd.version), std::to_wstring((int)(frac * 100)) });
        float tw = text_width(w, t, w->f_small);
        text(w, t, D2D1::RectF(bar.left - tw - 10, s.top, bar.left - 6, s.bottom), w->f_small, kDim);
    } else if (g_upd.state == UP_READY) {
        std::wstring t = Tf(L"Restart to update to {0}", { wide(g_upd.version) });
        float tw = text_width(w, t, w->f_small) + 20;
        D2D1_RECT_F b = D2D1::RectF(s.right - tw - 8, s.top + 3, s.right - 8, s.bottom - 3);
        region(w, b, H_UPDATE);
        fill_round(w, b, 4, kAccent, hovered(w, H_UPDATE) ? 1 : 0.85f);
        w->f_small->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(w, t, b, w->f_small, 0x000000);
        w->f_small->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

bool make_target(Win *w) {
    if (w->rt) return true;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    // render=software in explorer-lite.conf: no Direct3D device at all (less memory, more CPU per frame)
    bool soft = conf_get("render") == "software";
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(soft ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                    D2D1::PixelFormat(), (float)w->dpi, (float)w->dpi);
    if (FAILED(g_d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(w->hwnd, D2D1::SizeU(rc.right, rc.bottom)), &w->rt))) return false;
    w->rt->SetDpi((float)w->dpi, (float)w->dpi);
    w->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    w->rt->CreateSolidColorBrush(color(kText), &w->br);
    w->dc = w->rt;
    return true;
}

void drop_target(Win *w) {
    for (auto &kv : w->icons) release(kv.second);
    w->icons.clear();
    release(w->br);
    release(w->rt);
    w->dc = nullptr;
}

HRESULT draw_frame(Win *w) {
    D2D1_RECT_F c = client(w);
    w->regions.clear();
    w->dc->BeginDraw();
    w->dc->Clear(color(kBg));
    draw_tabs(w, c);
    draw_toolbar(w, c);
    draw_nav(w, c);
    View &v = cur_view(w);
    if (v.kind == TAB_FILES) draw_list(w, c);
    else {
        D2D1_RECT_F r = D2D1::RectF(kNavW, kTabH + kToolH, c.right, c.bottom - kStatusH);
        fill(w, r, kBg);
        fill(w, D2D1::RectF(kNavW, r.top, kNavW + 1, r.bottom), kLine);
        tasks_draw(v.tasks, w->dc, w->br, w->f_ui, w->f_small, w->f_icon_s, r, w->hover == H_LIST ? w->hover_arg : -1);
        region(w, r, H_LIST);
    }
    draw_status(w, c);
    return w->dc->EndDraw();
}

void paint(Win *w) {
    PAINTSTRUCT ps;
    BeginPaint(w->hwnd, &ps);
    EndPaint(w->hwnd, &ps);
    double t0 = now_ms();
    bool had = w->rt != nullptr;
    if (w->views.empty() || !make_target(w)) return;
    if (!had && w->opened_at) w->t_target = now_ms() - t0;
    if (draw_frame(w) == (HRESULT)D2DERR_RECREATE_TARGET) {
        drop_target(w);
        redraw(w);
    } else if (w->opened_at) {
        char b[160];
        snprintf(b, sizeof(b), "window painted in %d ms (window %d, render target %d, first frame %d)", (int)(now_ms() - w->opened_at), (int)w->t_created,
                 (int)w->t_target, (int)(now_ms() - w->opened_at - w->t_created - w->t_target));
        log_line(b);
        w->opened_at = 0;
    }
}

// --- input -------------------------------------------------------------------------------------------------

const Region *hit(Win *w, float x, float y) {
    // last drawn is on top
    for (auto it = w->regions.rbegin(); it != w->regions.rend(); ++it)
        if (inside(it->r, x, y)) return &*it;
    return nullptr;
}

int row_at(Win *w, float y) {
    View &v = cur_view(w);
    D2D1_RECT_F l = list_rect(w);
    int p = (int)((y - l.top + v.scroll) / kRowH);
    return p >= 0 && p < (int)v.shown.size() ? p : -1;
}

void on_move(Win *w, float x, float y) {
    if (w->drag_scroll) {
        View &v = cur_view(w);
        D2D1_RECT_F l = list_rect(w);
        float h = l.bottom - l.top, total = content_h(v), th = std::max(30.f, h * h / total);
        v.scroll = (y - w->drag_dy - l.top) / (h - th) * (total - h);
        clamp_scroll(w, v);
        redraw(w);
        return;
    }
    const Region *r = hit(w, x, y);
    int id = r ? r->id : H_NONE, arg = r ? r->arg : -1;
    if (id == H_LIST) {
        if (cur_view(w).kind == TAB_FILES) {
            int p = row_at(w, y);
            if (p >= 0) id = H_ROW, arg = p;
        } else {
            arg = tasks_row_at(cur_view(w).tasks, x, y);
        }
    }
    if (id != w->hover || arg != w->hover_arg) {
        w->hover = id;
        w->hover_arg = arg;
        redraw(w);
    }
    if (!w->tracking) {
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, w->hwnd, 0 };
        w->tracking = TrackMouseEvent(&t);
    }
}

void on_down(Win *w, float x, float y, bool dbl, bool right) {
    View &v = cur_view(w);
    const Region *r = hit(w, x, y);
    if (!r) return;
    w->press = r->id;
    w->press_arg = r->arg;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (r->id) {
    case H_SCROLL: {
        D2D1_RECT_F l = list_rect(w);
        float h = l.bottom - l.top, total = content_h(v), th = std::max(30.f, h * h / total), ty = l.top + (h - th) * (v.scroll / (total - h));
        if (y < ty || y > ty + th) { // a page up or down, as a scrollbar does
            v.scroll += (y < ty ? -1 : 1) * (h - kRowH);
            clamp_scroll(w, v);
        } else {
            w->drag_scroll = true;
            w->drag_dy = y - ty;
            SetCapture(w->hwnd);
        }
        break;
    }
    case H_LIST: {
        SetFocus(w->hwnd);
        if (v.kind == TAB_TASKS) {
            tasks_click(v.tasks, w->hwnd, x, y, dbl, right);
            break;
        }
        int p = row_at(w, y);
        if (p < 0) {
            if (!ctrl) select_only(v, -1);
            break;
        }
        if (dbl && !right) {
            open_item(w, v.all[v.shown[p]], ctrl);
            return;
        }
        if (shift) select_range(v, p);
        else if (ctrl && !right) {
            v.sel[v.shown[p]] ^= 1;
            v.focus = v.anchor = p;
        } else if (!(right && v.sel[v.shown[p]])) select_only(v, p);
        break;
    }
    }
    redraw(w);
}

void on_up(Win *w, float x, float y, bool middle) {
    if (w->drag_scroll) {
        w->drag_scroll = false;
        ReleaseCapture();
    }
    const Region *r = hit(w, x, y);
    int pressed = w->press, parg = w->press_arg;
    w->press = H_NONE;
    if (!r) return redraw(w);
    if (middle) { // the middle button: open in a new tab, close a tab
        if (r->id == H_TAB || r->id == H_TAB_CLOSE) close_tab(w, r->arg);
        else if (r->id == H_NAV) {
            size_t n = w->places.size();
            new_tab(w, r->arg < (int)n ? w->places[r->arg].path : w->drives[r->arg - n].path);
        } else if (r->id == H_LIST && cur_view(w).kind == TAB_FILES) {
            int p = row_at(w, y);
            if (p >= 0) open_item(w, cur_view(w).all[cur_view(w).shown[p]], true);
        }
        return redraw(w);
    }
    if (r->id != pressed || r->arg != parg) return redraw(w);
    View &v = cur_view(w);
    switch (r->id) {
    case H_TAB: switch_tab(w, r->arg); break;
    case H_TAB_CLOSE: close_tab(w, r->arg); return;
    case H_TAB_NEW: new_tab(w, v.kind == TAB_FILES ? v.path : L""); break;
    case H_BACK: go_back(w); break;
    case H_FWD: go_forward(w); break;
    case H_UP: go_up(w); break;
    case H_REFRESH:
        if (v.kind == TAB_FILES) load(w, v);
        else tasks_refresh(v.tasks);
        break;
    case H_ADDRESS: edit_address(w); break;
    case H_CRUMB: {
        std::wstring to;
        if (r->arg > 0) {
            std::vector<std::wstring> chain;
            for (std::wstring p = v.path; !p.empty(); p = folder_parent(p)) chain.push_back(p);
            std::reverse(chain.begin(), chain.end());
            if (r->arg - 1 < (int)chain.size()) to = chain[r->arg - 1];
        }
        if (_wcsicmp(to.c_str(), v.path.c_str())) navigate(w, to);
        else edit_address(w);
        break;
    }
    case H_NAV: {
        size_t n = w->places.size();
        std::wstring to = r->arg < (int)n ? w->places[r->arg].path : r->arg - n < w->drives.size() ? w->drives[r->arg - n].path : L"";
        if (v.kind == TAB_TASKS) { // from the task manager: a folder tab again
            int i = 0;
            for (; i < (int)w->views.size(); i++)
                if (w->views[i].kind == TAB_FILES) break;
            if (i < (int)w->views.size()) {
                switch_tab(w, i);
                navigate(w, to);
            } else new_tab(w, to);
        } else navigate(w, to);
        break;
    }
    case H_TASKS: {
        for (int i = 0; i < (int)w->views.size(); i++)
            if (w->views[i].kind == TAB_TASKS) {
                switch_tab(w, i);
                return;
            }
        new_tab(w, L"", TAB_TASKS);
        break;
    }
    case H_HEAD: set_sort(w, r->arg); break;
    case H_UPDATE: app_install_update(); return;
    }
    redraw(w);
}

void move_focus(Win *w, int to, bool shift, bool ctrl) {
    View &v = cur_view(w);
    if (v.shown.empty()) return;
    to = std::max(0, std::min((int)v.shown.size() - 1, to));
    if (shift) select_range(v, to);
    else if (ctrl) v.focus = to;
    else select_only(v, to);
    ensure_visible(w, v, to);
    redraw(w);
}

bool on_key(Win *w, WPARAM key) {
    View &v = cur_view(w);
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    D2D1_RECT_F l = list_rect(w);
    int page = std::max(1, (int)((l.bottom - l.top) / kRowH) - 1);
    if (ctrl && key == 'T') return new_tab(w, v.kind == TAB_FILES ? v.path : L""), true;
    if (ctrl && key == 'W') return close_tab(w, w->cur), true;
    if (ctrl && key == 'N') {
        WindowSpec s;
        s.path = v.path;
        window_open(s);
        return true;
    }
    if (ctrl && key == VK_TAB) return switch_tab(w, (w->cur + (shift ? -1 : 1) + (int)w->views.size()) % (int)w->views.size()), true;
    if (ctrl && shift && key == VK_ESCAPE) return new_tab(w, L"", TAB_TASKS), true;
    if (key == VK_F5) {
        if (v.kind == TAB_FILES) load(w, v);
        else tasks_refresh(v.tasks);
        return true;
    }
    if (v.kind == TAB_TASKS) return tasks_key(v.tasks, w->hwnd, key);
    if ((ctrl && key == 'L') || (alt && key == 'D') || key == VK_F4) return edit_address(w), true;
    if ((ctrl && (key == 'F' || key == 'E')) || key == VK_F3) {
        SetFocus(w->search);
        SendMessageW(w->search, EM_SETSEL, 0, -1);
        return true;
    }
    if ((alt && key == VK_LEFT) || key == VK_BROWSER_BACK || key == VK_BACK) return key == VK_BACK ? go_up(w) : go_back(w), true;
    if ((alt && key == VK_RIGHT) || key == VK_BROWSER_FORWARD) return go_forward(w), true;
    if (alt && key == VK_UP) return go_up(w), true;
    if (ctrl && key == 'A') {
        for (int i : v.shown) v.sel[i] = 1;
        redraw(w);
        return true;
    }
    switch (key) {
    case VK_DOWN: move_focus(w, v.focus + 1, shift, ctrl); return true;
    case VK_UP: move_focus(w, v.focus < 0 ? 0 : v.focus - 1, shift, ctrl); return true;
    case VK_NEXT: move_focus(w, v.focus + page, shift, ctrl); return true;
    case VK_PRIOR: move_focus(w, v.focus - page, shift, ctrl); return true;
    case VK_HOME: move_focus(w, 0, shift, ctrl); return true;
    case VK_END: move_focus(w, (int)v.shown.size() - 1, shift, ctrl); return true;
    case VK_SPACE:
        if (ctrl && v.focus >= 0) {
            v.sel[v.shown[v.focus]] ^= 1;
            redraw(w);
            return true;
        }
        return false;
    case VK_RETURN: {
        std::vector<int> s = selected(v);
        if (s.size() == 1 || (s.empty() && v.focus >= 0)) open_item(w, v.all[s.empty() ? v.shown[v.focus] : s[0]], ctrl);
        else
            for (int i : s)
                if (!v.all[i].dir) open_item(w, v.all[i]);
        return true;
    }
    case VK_ESCAPE:
        if (!v.filter.empty()) {
            SetWindowTextW(w->search, L"");
            return true;
        }
        select_only(v, -1);
        redraw(w);
        return true;
    }
    return false;
}

// type-ahead: the letters typed within a second select the next item that starts with them
void on_char(Win *w, wchar_t ch) {
    View &v = cur_view(w);
    if (v.kind != TAB_FILES || ch < 32 || v.shown.empty()) return;
    DWORD now = GetTickCount();
    if (now - w->typed_at > 1000) w->typed.clear();
    w->typed_at = now;
    w->typed += ch;
    int start = v.focus < 0 ? 0 : (w->typed.size() == 1 ? v.focus + 1 : v.focus);
    for (int k = 0; k < (int)v.shown.size(); k++) {
        int p = (start + k) % (int)v.shown.size();
        const std::wstring &n = v.all[v.shown[p]].name;
        if (n.size() >= w->typed.size() && !_wcsnicmp(n.c_str(), w->typed.c_str(), w->typed.size())) {
            move_focus(w, p, false, false);
            return;
        }
    }
}

// --- the window ----------------------------------------------------------------------------------------------

void save_place(Win *w) {
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(w->hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    char s[96];
    snprintf(s, sizeof(s), "%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    conf_set("window", s);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Win *w = win_of(h);
    if (!w) {
        if (msg == WM_NCCREATE) {
            w = (Win *)((CREATESTRUCTW *)lp)->lpCreateParams;
            w->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)w);
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    float s = scale(w);
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        COLORREF cap = RGB((kTabBar >> 16) & 255, (kTabBar >> 8) & 255, kTabBar & 255);
        DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap)); // the caption joins the tab bar
        w->dpi = GetDpiForWindow(h);
        w->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)1, g_inst, NULL);
        w->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)2, g_inst, NULL);
        SetWindowSubclass(w->edit, edit_proc, 1, (DWORD_PTR)w);
        SetWindowSubclass(w->search, edit_proc, 2, (DWORD_PTR)w);
        edit_fonts(w);
        w->places = folder_places();
        w->drives = folder_drives();
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(242, 242, 242));
        SetBkColor(dc, RGB((kBg >> 16) & 255, (kBg >> 8) & 255, kBg & 255));
        if (!g_edit_brush) g_edit_brush = CreateSolidBrush(RGB((kBg >> 16) & 255, (kBg >> 8) & 255, kBg & 255));
        return (LRESULT)g_edit_brush;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 2 && HIWORD(wp) == EN_CHANGE) { // the search box filters the folder as you type
            int n = GetWindowTextLengthW(w->search);
            std::wstring t(n, 0);
            GetWindowTextW(w->search, &t[0], n + 1);
            View &v = cur_view(w);
            if (v.kind == TAB_FILES && t != v.filter) {
                v.filter = t;
                apply_filter(v);
                v.scroll = 0;
                redraw(w);
            }
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(w); return 0;
    case WM_SIZE:
        if (w->rt) w->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        if (!w->views.empty()) {
            clamp_scroll(w, cur_view(w));
            place_edits(w);
        }
        if (IsWindowVisible(w->edit)) ShowWindow(w->edit, SW_HIDE);
        redraw(w);
        return 0;
    case WM_DPICHANGED: {
        w->dpi = HIWORD(wp);
        RECT *r = (RECT *)lp;
        if (w->rt) w->rt->SetDpi((float)w->dpi, (float)w->dpi);
        edit_fonts(w);
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        place_edits(w);
        redraw(w);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize = { MulDiv(640, (int)w->dpi, 96), MulDiv(400, (int)w->dpi, 96) };
        return 0;
    }
    case WM_MOUSEMOVE: on_move(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s); return 0;
    case WM_MOUSELEAVE:
        w->tracking = false;
        w->hover = H_NONE;
        w->hover_arg = -1;
        redraw(w);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        on_down(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, msg == WM_LBUTTONDBLCLK, msg == WM_RBUTTONDOWN);
        return 0;
    case WM_LBUTTONUP: on_up(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, false); return 0;
    case WM_MBUTTONDOWN: {
        const Region *r = hit(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        w->press = r ? r->id : H_NONE;
        return 0;
    }
    case WM_MBUTTONUP: on_up(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, true); return 0;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) go_back(w);
        else go_forward(w);
        return TRUE;
    case WM_MOUSEWHEEL: {
        View &v = cur_view(w);
        if (v.kind == TAB_TASKS) tasks_wheel(v.tasks, GET_WHEEL_DELTA_WPARAM(wp));
        else {
            v.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * kRowH * 3;
            clamp_scroll(w, v);
        }
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        on_move(w, pt.x / s, pt.y / s);
        redraw(w);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (on_key(w, wp)) return 0;
        break;
    case WM_CHAR: on_char(w, (wchar_t)wp); return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: redraw(w); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) w->last_used = ++g_use;
        break;
    case WM_DEVICECHANGE: w->drives = folder_drives(); redraw(w); return TRUE;
    case WM_APP_LIST: on_listing(w, (Listing *)lp); return 0;
    case WM_APP_ICONS: {
        auto *ic = (Icons *)lp;
        for (View &v : w->views) {
            if (v.gen != ic->gen) continue;
            std::unordered_map<std::wstring, int> by_target;
            for (auto &kv : ic->icons) by_target[kv.first] = kv.second;
            for (Item &it : v.all) {
                auto f = by_target.find(it.target);
                if (f != by_target.end()) it.icon = f->second;
            }
        }
        delete ic;
        redraw(w);
        return 0;
    }
    case WM_APP_TASKS:
        for (View &v : w->views)
            if (v.tasks) tasks_on_sample(v.tasks);
        if (cur_view(w).kind == TAB_TASKS) redraw(w);
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        save_place(w);
        return 0;
    case WM_NCDESTROY: {
        for (View &v : w->views) {
            if (v.tasks) tasks_destroy(v.tasks);
            if (v.gen) folder_forget(v.gen);
        }
        w->views.clear();
        drop_target(w);
        release(w->f_ui), release(w->f_ui_c), release(w->f_small), release(w->f_icon), release(w->f_icon_s), release(w->f_title);
        if (w->edit_font) DeleteObject(w->edit_font);
        g_wins.erase(std::remove(g_wins.begin(), g_wins.end(), w), g_wins.end());
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        delete w;
        if (g_wins.empty()) {
            release(g_wic);
            g_sysicons = NULL; // shell32's, not ours to destroy
            gfx_release();
            // the last window closed with an update waiting: now it bothers nobody
            if (g_upd.state == UP_READY && update_auto()) PostMessageW(g_host, WM_APP_UPDATE, 0, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool registered() {
    static bool done;
    if (done) return true;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    std::wstring cls = win_class();
    wc.lpszClassName = cls.c_str();
    done = RegisterClassExW(&wc) != 0;
    return done;
}

// where a new window goes: the given place, else the last one (cascaded if a window is there), else centred
RECT initial_rect(const WindowSpec &spec, bool *maximized) {
    *maximized = spec.maximized;
    if (spec.rc.right > spec.rc.left) return spec.rc;
    RECT r = {};
    int max = 0;
    std::string saved = conf_get("window");
    if (sscanf(saved.c_str(), "%ld,%ld,%ld,%ld,%d", &r.left, &r.top, &r.right, &r.bottom, &max) == 5 && r.right - r.left > 200) {
        *maximized = max != 0;
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (mon) {
            for (Win *w : g_wins) { // one already there: a bit lower and to the right
                RECT o;
                GetWindowRect(w->hwnd, &o);
                if (o.left == r.left && o.top == r.top) OffsetRect(&r, 32, 32);
            }
            return r;
        }
    }
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = 96;
    typedef HRESULT(WINAPI * PDpi)(HMONITOR, int, UINT *, UINT *);
    static HMODULE shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static PDpi dpi_for = shcore ? (PDpi)(void *)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    UINT dx = 96, dy = 96;
    if (dpi_for && SUCCEEDED(dpi_for(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), 0, &dx, &dy))) dpi = dx;
    int cw = MulDiv(1100, (int)dpi, 96), ch = MulDiv(700, (int)dpi, 96);
    RECT wa = mi.rcWork;
    cw = std::min(cw, (int)(wa.right - wa.left));
    ch = std::min(ch, (int)(wa.bottom - wa.top));
    r.left = wa.left + (wa.right - wa.left - cw) / 2;
    r.top = wa.top + (wa.bottom - wa.top - ch) / 2;
    r.right = r.left + cw;
    r.bottom = r.top + ch;
    return r;
}

} // namespace

void window_open(const WindowSpec &spec) {
    if (!registered() || !gfx_acquire()) {
        gfx_release();
        return;
    }
    Win *w = new Win;
    w->opened_at = now_ms();
    bool maximized = false;
    RECT r = initial_rect(spec, &maximized);
    HWND h = CreateWindowExW(0, win_class().c_str(), L"explorer-lite", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, NULL, NULL, g_inst, w);
    if (!h) {
        delete w;
        gfx_release();
        return;
    }
    g_wins.push_back(w);
    w->t_created = now_ms() - w->opened_at;
    w->last_used = ++g_use;
    make_formats(w);
    std::wstring path = spec.path.empty() ? L"" : folder_normalize(spec.path);
    if (spec.tab == TAB_TASKS) new_tab(w, L"", TAB_TASKS);
    else new_tab(w, folder_exists(path) ? path : L"");
    ShowWindow(h, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    SetForegroundWindow(h);
    SetFocus(h);
}

int window_count() { return (int)g_wins.size(); }

void window_focus_last() {
    Win *best = nullptr;
    for (Win *w : g_wins)
        if (!best || w->last_used > best->last_used) best = w;
    if (!best) return;
    if (IsIconic(best->hwnd)) ShowWindow(best->hwnd, SW_RESTORE);
    SetForegroundWindow(best->hwnd);
}

std::vector<WindowSpec> window_specs() {
    std::vector<WindowSpec> out;
    for (Win *w : g_wins) {
        WindowSpec s;
        View &v = cur_view(w);
        s.path = v.path;
        s.tab = v.kind;
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(w->hwnd, &wp);
        s.rc = wp.rcNormalPosition;
        s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
        out.push_back(s);
    }
    return out;
}

void window_close_all() {
    std::vector<Win *> all = g_wins;
    for (Win *w : all) DestroyWindow(w->hwnd);
}

// The last used window, drawn into a PNG (tests and screenshots: no screen capture, nothing has to be visible).
bool window_shot(const std::wstring &png) {
    Win *w = nullptr;
    for (Win *x : g_wins)
        if (!w || x->last_used > w->last_used) w = x;
    if (!w || w->views.empty() || !make_target(w)) return false;
    if (!g_wic) CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (!g_wic) return false;
    RECT rc;
    GetClientRect(w->hwnd, &rc);
    IWICBitmap *bmp = nullptr;
    ID2D1RenderTarget *rt = nullptr;
    ID2D1SolidColorBrush *br = nullptr;
    bool ok = false;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)w->dpi,
                                                                    (float)w->dpi);
    if (SUCCEEDED(g_wic->CreateBitmap(rc.right, rc.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
        SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(bmp, p, &rt)) && SUCCEEDED(rt->CreateSolidColorBrush(color(kText), &br))) {
        // the icons belong to the window's target: a set of their own for this one
        std::unordered_map<int, ID2D1Bitmap *> icons;
        icons.swap(w->icons);
        ID2D1RenderTarget *dc = w->dc;
        ID2D1SolidColorBrush *wbr = w->br;
        w->dc = rt;
        w->br = br;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g_shot = true;
        ok = SUCCEEDED(draw_frame(w));
        g_shot = false;
        for (auto &kv : w->icons) release(kv.second);
        w->icons.swap(icons);
        w->dc = dc;
        w->br = wbr;
    }
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    if (ok) {
        ok = SUCCEEDED(g_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) && SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->WriteSource(bmp, NULL)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    release(fr);
    release(enc);
    release(st);
    release(br);
    release(rt);
    release(bmp);
    return ok;
}

// Test commands (--cmd): drive the last used window like a user would, without input on screen.
void window_command(const std::wstring &verb, const std::wstring &arg) {
    Win *w = nullptr;
    for (Win *x : g_wins)
        if (!w || x->last_used > w->last_used) w = x;
    if (!w) return;
    View &v = cur_view(w);
    if (verb == L"go") navigate(w, folder_normalize(arg));
    else if (verb == L"search") SetWindowTextW(w->search, arg.c_str());
    else if (verb == L"key") on_key(w, (WPARAM)_wtoi(arg.c_str()));
    else if (verb == L"select") {
        for (int p = 0; p < (int)v.shown.size(); p++)
            if (!_wcsicmp(v.all[v.shown[p]].name.c_str(), arg.c_str())) move_focus(w, p, false, false);
    } else if (verb == L"newtab") new_tab(w, folder_normalize(arg));
    else if (verb == L"tasks") new_tab(w, L"", TAB_TASKS);
    else if (verb == L"taskpage") {
        // the page buttons sit in the toolbar: click the n-th one where tasks_toolbar put it
        if (v.kind == TAB_TASKS) {
            if (make_target(w)) draw_frame(w); // a frame first: the buttons exist once drawn
            for (float x = 160; x < 700; x += 4) {
                int id = tasks_row_at(v.tasks, x, kTabH + kToolH / 2);
                if (id == 10000 + _wtoi(arg.c_str())) {
                    tasks_click(v.tasks, w->hwnd, x, kTabH + kToolH / 2, false, false);
                    break;
                }
            }
        }
    } else if (verb == L"size") {
        int cw = 0, ch = 0;
        swscanf(arg.c_str(), L"%dx%d", &cw, &ch);
        if (cw > 0 && ch > 0) {
            RECT r = { 0, 0, MulDiv(cw, (int)w->dpi, 96), MulDiv(ch, (int)w->dpi, 96) };
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, w->dpi);
            SetWindowPos(w->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    redraw(w);
}

void window_update_changed() {
    for (Win *w : g_wins) {
        w->places = folder_places(); // the language may have changed: their names too
        if (!w->views.empty()) {
            set_title(w);
            sync_search(w);
        }
        redraw(w);
    }
}
