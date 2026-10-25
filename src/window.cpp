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
#include "shellops.h"
#include "tasks.h"
#include "thumbs.h"
#include "update.h"

namespace {

// layout, in DIPs
const float kTabH = 34, kToolH = 40, kNavW = 210, kHeadH = 30, kRowH = 28, kStatusH = 26, kScrollW = 10;
const float kColDate = 150, kColType = 170, kColSize = 110, kNameMin = 180;
const float kTileW = 124, kTileH = 150, kThumb = 96; // Large icons: a tile, and the picture in it
const float kDriveW = 280, kDriveH = 70, kDriveIcon = 48; // This PC: a drive's tile
const float kCapW = 46;                                 // the window's minimize / maximize / close buttons
const size_t kThumbsKept = 200;                       // thumbnails kept per window (the ones on screen are asked again)

enum Hit {
    H_NONE, H_TAB, H_TAB_CLOSE, H_TAB_NEW, H_BACK, H_FWD, H_UP, H_REFRESH, H_CRUMB, H_ADDRESS, H_NAV, H_HEAD, H_ROW,
    H_SCROLL, H_LIST, H_UPDATE, H_TASKS, H_VIEW, H_MIN, H_MAX, H_CLOSE,
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
    bool grid = false;                 // Large icons (thumbnails) instead of Details
    float scroll = 0;                  // DIPs
    uint64_t gen = 0;                  // the listing this view waits for
    bool loading = false;
    std::wstring error, filter;
    std::wstring select_after;         // after going up: the folder we came from is selected
    bool rename_after = false;         // a new folder: renamed as soon as it shows up
    bool results = false;              // all holds Everything's results for the search, not the folder
    DWORD search_id = 0;               // the search whose answer this view waits for
    double typed_at = 0;               // when the search text last changed (now_ms): how long its answer took
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
    std::unordered_map<std::wstring, Thumb *> thumbs;          // by target; nullptr = asked, not here yet
    std::unordered_map<std::wstring, ID2D1Bitmap *> thumb_bmps; // the ones drawn, on the render target
    uint64_t thumbs_gen = 0;                                   // the listing they belong to
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
    HWND rename = NULL;                // the name being edited (F2), over its row
    std::wstring rename_target;
    ULONG watch = 0;                   // change notifications for the folder shown
    std::wstring watched;
    DropHandler *drop = nullptr;
    int drop_row = -1;                 // the folder row a drag hovers (highlighted)
    bool drag_maybe = false;           // a row pressed: a drag starts if the mouse moves far enough
    POINT drag_from = {};
    int collapse_to = -1;              // a click on a selected row: the selection collapses on release, unless dragged
    // the marquee: pressed on empty space in the list, a drag selects what its rectangle touches
    int mq = 0;                        // 0 none, 1 pressed (not moved enough yet), 2 dragging
    bool mq_clear = false;             // pressed on empty space without Ctrl: released without moving, nothing is selected
    int mq_mode = 0;                   // MQ_REPLACE, MQ_ADD (Shift) or MQ_TOGGLE (Ctrl)
    float mq_ax = 0, mq_ay = 0;        // the anchor: client x, content y (y + scroll), so it scrolls with the list
    float mq_x = 0, mq_y = 0;          // the pointer, client DIPs (also what auto-scroll goes by)
    std::vector<uint8_t> mq_before;    // the selection at the press: Esc restores it, Shift and Ctrl build on it
    int mq_focus = -1, mq_anchor = -1;
    int mq_l0 = 0, mq_l1 = -1, mq_c0 = 0, mq_c1 = -1; // lines and columns applied last (l1 < l0: none)
    uint64_t mq_gen = 0;               // the listing it began on: another one ends it
    int mq_tab = 0;
    bool mq_grid = false;              // and its layout (Ctrl+wheel may change it mid-drag)
};

enum { MQ_REPLACE, MQ_ADD, MQ_TOGGLE };

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

bool is_drives(View &v) { return v.kind == TAB_FILES && v.path == kThisPC; }
bool is_grid(View &v) { return v.kind == TAB_FILES && (v.grid || is_drives(v)); } // tiles: Large icons or This PC
float tile_w(View &v) { return is_drives(v) ? kDriveW : kTileW; }
float tile_h(View &v) { return is_drives(v) ? kDriveH : kTileH; }

// Large icons: tiles per line; Details: one row per line
int per_line(Win *w, View &v) {
    if (!is_grid(v)) return 1;
    D2D1_RECT_F l = list_rect(w);
    return std::max(1, (int)((l.right - l.left - kScrollW - 8) / tile_w(v)));
}
float line_h(View &v) { return is_grid(v) ? tile_h(v) : kRowH; }

float content_h(Win *w, View &v) {
    int per = per_line(w, v);
    return ((v.shown.size() + per - 1) / per) * line_h(v);
}

// where item pos is drawn (DIPs, before the list's clip)
D2D1_RECT_F item_rect(Win *w, View &v, int pos) {
    D2D1_RECT_F l = list_rect(w);
    int per = per_line(w, v);
    float y = l.top + (pos / per) * line_h(v) - v.scroll;
    if (!is_grid(v)) return D2D1::RectF(l.left + 4, y + 1, l.right - kScrollW - 2, y + kRowH - 1);
    float x = l.left + 6 + (pos % per) * tile_w(v);
    return D2D1::RectF(x + 2, y + 4, x + tile_w(v) - 2, y + tile_h(v) - 2);
}

void clamp_scroll(Win *w, View &v) {
    D2D1_RECT_F l = list_rect(w);
    float max = std::max(0.f, content_h(w, v) - (l.bottom - l.top));
    v.scroll = std::min(std::max(v.scroll, 0.f), max);
}

void ensure_visible(Win *w, View &v, int pos) {
    if (pos < 0) return;
    D2D1_RECT_F l = list_rect(w);
    float top = (pos / per_line(w, v)) * line_h(v), bottom = top + line_h(v), h = l.bottom - l.top;
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

void forget_thumbs(Win *w) {
    for (auto &kv : w->thumbs) delete kv.second;
    for (auto &kv : w->thumb_bmps) release(kv.second);
    w->thumbs.clear();
    w->thumb_bmps.clear();
}

// before a frame of tiles: another folder, or too many kept, and they start again (never in the middle of a frame)
void prepare_thumbs(Win *w, View &v) {
    if (w->thumbs_gen == v.gen && w->thumbs.size() <= kThumbsKept) return;
    if (w->thumbs_gen != v.gen) thumbs_cancel(w->thumbs_gen);
    forget_thumbs(w);
    w->thumbs_gen = v.gen;
}

// the thumbnail of an item on screen: drawn if here, asked for otherwise
ID2D1Bitmap *thumb_bitmap(Win *w, View &v, const Item &it) {
    auto b = w->thumb_bmps.find(it.target);
    if (b != w->thumb_bmps.end()) return b->second;
    auto t = w->thumbs.find(it.target);
    if (t == w->thumbs.end()) {
        w->thumbs[it.target] = nullptr;
        thumbs_request(w->hwnd, v.gen, it.target, (int)std::lround(kThumb * scale(w)));
        return nullptr;
    }
    if (!t->second) return nullptr;
    ID2D1Bitmap *bmp = nullptr;
    Thumb *th = t->second;
    if (th->w > 0)
        w->dc->CreateBitmap(D2D1::SizeU(th->w, th->h), th->px.data(), th->w * 4,
                            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale(w), 96 * scale(w)), &bmp);
    w->thumb_bmps[it.target] = bmp; // also a failure: the icon stays
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
        if (v.filter.empty() || v.results || StrStrIW(v.all[i].name.c_str(), v.filter.c_str())) v.shown.push_back(i);
    v.focus = -1;
    for (int p = 0; p < (int)v.shown.size(); p++)
        if (v.shown[p] == focus_item) v.focus = p;
    v.anchor = v.focus;
}

// the folder on screen gets Windows' change notifications: what other programs (or our own operations) do shows up
void watch(Win *w) {
    std::wstring path = w->views.empty() || cur_view(w).kind != TAB_FILES ? L"" : cur_view(w).path;
    if (w->watch && !_wcsicmp(path.c_str(), w->watched.c_str())) return;
    shell_unwatch(w->watch);
    w->watched = path;
    w->watch = shell_watch(w->hwnd, WM_APP_CHANGED, path);
}

void load(Win *w, View &v) {
    if (v.results && !v.filter.empty()) { // refreshing a search: asked again
        SetTimer(w->hwnd, 8, 0, NULL);
        return;
    }
    v.results = false;
    v.search_id = 0;
    if (v.gen) {
        folder_forget(v.gen);
        thumbs_cancel(v.gen);
    }
    v.gen = ++g_gen;
    v.loading = true;
    folder_list(w->hwnd, v.gen, v.path);
    watch(w);
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
    v.results = false;
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

void start_rename(Win *w);

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
        if (!folder_virtual(v.path)) folder_sort(v.all, v.sort, v.desc); // Home and This PC keep their order
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
        if (!refresh || v.rename_after) ensure_visible(w, v, v.focus);
        if (v.rename_after && &v == &cur_view(w) && v.focus >= 0) start_rename(w);
        v.rename_after = false;
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
    else watch(w);
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
    watch(w);
    redraw(w);
}

void switch_tab(Win *w, int i) {
    if (i < 0 || i >= (int)w->views.size() || i == w->cur) return;
    w->cur = i;
    sync_search(w);
    set_title(w);
    watch(w);
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
    if (id == 3) { // renaming: Enter or leaving it renames, Esc cancels
        if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
            if (wp == VK_ESCAPE) w->rename_target.clear();
            SetFocus(w->hwnd); // WM_KILLFOCUS does the rest
            return 0;
        }
        if (msg == WM_KILLFOCUS) {
            int n = GetWindowTextLengthW(h);
            std::wstring name(n, 0);
            GetWindowTextW(h, &name[0], n + 1);
            std::wstring target = w->rename_target;
            w->rename_target.clear();
            ShowWindow(h, SW_HIDE);
            if (!target.empty() && !name.empty() && name != PathFindFileNameW(target.c_str())) {
                cur_view(w).select_after = name;
                shell_rename(w->hwnd, target, name);
            }
        }
    }
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
    if (w->rename) SendMessageW(w->rename, WM_SETFONT, (WPARAM)w->edit_font, TRUE);
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
    SetWindowTextW(w->edit, folder_virtual(v.path) ? folder_title(v.path).c_str() : v.path.c_str());
    int x = (int)std::lround((r.left + 8) * s), y = (int)std::lround((r.top + 6) * s);
    SetWindowPos(w->edit, HWND_TOP, x, y, (int)std::lround((r.right - 8) * s) - x, (int)std::lround((r.bottom - 5) * s) - y, SWP_SHOWWINDOW);
    SetFocus(w->edit);
    SendMessageW(w->edit, EM_SETSEL, 0, -1);
}

// F2: the focused item's name becomes an edit box over its row (the extension left out of the selection)
void start_rename(Win *w) {
    View &v = cur_view(w);
    if (v.kind != TAB_FILES || folder_virtual(v.path) || v.focus < 0 || !w->rename) return;
    const Item &it = v.all[v.shown[v.focus]];
    ensure_visible(w, v, v.focus);
    float s = scale(w);
    D2D1_RECT_F r = item_rect(w, v, v.focus);
    D2D1_RECT_F box = is_grid(v) ? D2D1::RectF(r.left + 2, r.top + 6 + kThumb + 2, r.right - 2, r.top + 6 + kThumb + 26)
                                 : D2D1::RectF(r.left + 4 + 26, r.top + 2, r.left + 4 + 26 + name_width(w) - 30, r.bottom - 2);
    int x = (int)std::lround(box.left * s), top = (int)std::lround(box.top * s);
    SetWindowPos(w->rename, HWND_TOP, x, top, (int)std::lround(box.right * s) - x, (int)std::lround(box.bottom * s) - top, SWP_SHOWWINDOW);
    w->rename_target = it.target;
    SetWindowTextW(w->rename, it.name.c_str());
    SetFocus(w->rename);
    size_t dot = it.dir ? std::wstring::npos : it.name.rfind(L'.');
    SendMessageW(w->rename, EM_SETSEL, 0, dot == std::wstring::npos || dot == 0 ? -1 : (LPARAM)dot);
    redraw(w);
}

std::vector<std::wstring> selected_targets(View &v) {
    std::vector<std::wstring> out;
    for (int i : selected(v)) out.push_back(v.all[i].target);
    return out;
}

void new_folder(Win *w) {
    View &v = cur_view(w);
    std::wstring name = shell_new_folder(v.path, T(L"New folder"));
    if (name.empty()) return;
    v.select_after = name;
    v.rename_after = true;
    load(w, v);
}

// --- painting ----------------------------------------------------------------------------------------------

void draw_tabs(Win *w, D2D1_RECT_F c) {
    fill(w, D2D1::RectF(0, 0, c.right, kTabH), kTabBar);
    float x = 8, tw = std::min(220.f, std::max(110.f, (c.right - 100 - 3 * kCapW) / std::max<size_t>(1, w->views.size())));
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
    // the window has no title bar: the tabs are it, with Windows' three buttons at the right (WM_NCHITTEST)
    bool zoomed = IsZoomed(w->hwnd);
    const wchar_t *glyphs[3] = { L"\xE921", zoomed ? L"\xE923" : L"\xE922", L"\xE8BB" };
    int ids[3] = { H_MIN, H_MAX, H_CLOSE };
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = D2D1::RectF(c.right - (3 - i) * kCapW, 0, c.right - (2 - i) * kCapW, kTabH);
        region(w, r, ids[i]);
        bool hot = w->hover == ids[i], close = ids[i] == H_CLOSE;
        if (hot) fill(w, r, close ? 0xC42B1C : w->press == ids[i] ? kPress : kHover, close && w->press == ids[i] ? 0.85f : 1);
        text(w, glyphs[i], r, w->f_icon_s, hot && close ? 0xFFFFFF : kText);
    }
}

// Windows' resize border, in pixels: also what a maximized window hangs off the screen
int frame_px(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

int caption_hit(int id) { return id == H_MIN ? HTMINBUTTON : id == H_MAX ? HTMAXBUTTON : id == H_CLOSE ? HTCLOSE : 0; }
int caption_id(WPARAM hit) { return hit == HTMINBUTTON ? H_MIN : hit == HTMAXBUTTON ? H_MAX : hit == HTCLOSE ? H_CLOSE : H_NONE; }

void apply_theme(Win *w) {
    BOOL dark = !theme_light();
    DwmSetWindowAttribute(w->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
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

// the navigation pane's second group: This PC, and its drives under it
std::vector<Place> nav_drives() {
    std::vector<Place> out = { { T(L"This PC"), kThisPC, L"\xE7F4" } };
    for (Place &p : folder_drives()) out.push_back(p);
    return out;
}

void draw_nav(Win *w, D2D1_RECT_F c) {
    View &v = cur_view(w);
    D2D1_RECT_F pane = D2D1::RectF(0, kTabH + kToolH, kNavW, c.bottom - kStatusH);
    fill(w, pane, kPane);
    float y = pane.top + 8;
    int i = 0;
    auto row = [&](const Place &p, int idx, float indent = 0) {
        D2D1_RECT_F r = D2D1::RectF(6 + indent, y, kNavW - 6, y + 30);
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
    for (size_t d = 0; d < w->drives.size(); d++) row(w->drives[d], i++, d ? 16.f : 0.f); // the drives under This PC
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
    if (is_drives(v)) text(w, T(L"Devices and drives"), D2D1::RectF(l.left + 12, hy, l.right, hy + kHeadH), w->f_small, kDim);
    else for (int i = 0; i < COL_COUNT; i++) {
        D2D1_RECT_F r = D2D1::RectF(xs[i] - 4, hy + 2, xs[i + 1] - 4, hy + kHeadH - 2);
        region(w, r, H_HEAD, i);
        if (hovered(w, H_HEAD, i)) fill_round(w, r, 4, kHover);
        text(w, T(i == COL_TYPE && v.results ? L"Folder" : heads[i]), D2D1::RectF(r.left + 8, r.top, r.right - 18, r.bottom), w->f_small, kDim);
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
    int per = per_line(w, v);
    int first = (int)(v.scroll / line_h(v)) * per, last = std::min((int)v.shown.size(), ((int)((v.scroll + (l.bottom - l.top)) / line_h(v)) + 1) * per);
    if (is_grid(v)) {
        prepare_thumbs(w, v);
        for (int p = std::max(0, first); p < last; p++) {
            const Item &it = v.all[v.shown[p]];
            D2D1_RECT_F tile = item_rect(w, v, p);
            bool sel = v.sel[v.shown[p]] != 0, hot = hovered(w, H_ROW, p);
            if (sel) fill_round(w, tile, 5, kSelect, GetFocus() == w->hwnd ? 1 : 0.6f);
            else if (hot) fill_round(w, tile, 5, kHover);
            if (p == w->drop_row || (p == v.focus && GetFocus() == w->hwnd && !sel)) {
                w->br->SetColor(p == w->drop_row ? color(kAccent) : color(kDim, 0.6f));
                w->dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f, tile.right - 0.5f, tile.bottom - 0.5f), 5, 5), w->br,
                                            p == w->drop_row ? 2.f : 1.f);
            }
            if (is_drives(v)) { // the drive's icon, its name, how full it is and what is left
                float iy = (tile.top + tile.bottom - kDriveIcon) / 2;
                if (ID2D1Bitmap *b = thumb_bitmap(w, v, it))
                    w->dc->DrawBitmap(b, D2D1::RectF(tile.left + 8, iy, tile.left + 8 + kDriveIcon, iy + kDriveIcon), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                else draw_icon(w, it.icon, tile.left + 8 + 8, iy + 8, 32);
                float x0 = tile.left + 16 + kDriveIcon, x1 = tile.right - 10;
                text(w, it.name, D2D1::RectF(x0, tile.top + 6, x1, tile.top + 26), w->f_ui, kText);
                if (it.bytes) {
                    D2D1_RECT_F bar = D2D1::RectF(x0, tile.top + 30, x1, tile.top + 38);
                    float used = 1 - (float)((double)it.free_bytes / (double)it.bytes);
                    fill(w, bar, kLine);
                    fill(w, D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * used, bar.bottom), used > 0.9f ? kDanger : kAccent);
                    text(w, it.size, D2D1::RectF(x0, tile.top + 40, x1, tile.top + 60), w->f_small, kDim);
                } else text(w, it.type, D2D1::RectF(x0, tile.top + 30, x1, tile.top + 50), w->f_small, kDim);
                continue;
            }
            float cx = (tile.left + tile.right) / 2, top = tile.top + 6;
            if (ID2D1Bitmap *b = thumb_bitmap(w, v, it)) {
                D2D1_SIZE_F sz = b->GetSize(); // DIPs, already fitted in kThumb
                float bw = std::min(sz.width, kThumb), bh = std::min(sz.height, kThumb);
                float x = cx - bw / 2, y = top + (kThumb - bh) / 2;
                w->dc->DrawBitmap(b, D2D1::RectF(x, y, x + bw, y + bh), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else draw_icon(w, it.icon, cx - 16, top + kThumb / 2 - 16, 32);
            // the name under it, two lines at most
            D2D1_RECT_F nr = D2D1::RectF(tile.left + 4, top + kThumb + 2, tile.right - 4, tile.bottom - 2);
            w->f_small->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            w->f_small->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            w->f_small->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            text(w, it.name, nr, w->f_small, kText, (it.attr & FILE_ATTRIBUTE_HIDDEN) ? 0.6f : 1);
            w->f_small->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            w->f_small->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            w->f_small->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        first = last; // no rows
    }
    for (int p = std::max(0, first); p < last; p++) {
        const Item &it = v.all[v.shown[p]];
        D2D1_RECT_F row = item_rect(w, v, p);
        float y = row.top - 1;
        bool sel = v.sel[v.shown[p]] != 0, hot = hovered(w, H_ROW, p);
        if (sel) fill_round(w, row, 4, kSelect, GetFocus() == w->hwnd ? 1 : 0.6f);
        else if (hot) fill_round(w, row, 4, kHover);
        if (p == w->drop_row) {
            w->br->SetColor(color(kAccent));
            w->dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(row.left + 1, row.top + 1, row.right - 1, row.bottom - 1), 4, 4), w->br, 2);
        }
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
    if (w->mq == 2) { // the marquee: the accent, see-through, with a 1 px border on whole pixels (kept near the list)
        float s = scale(w), ay = w->mq_ay - v.scroll + l.top;
        auto px = [&](float d, float lo, float hi) { return std::round(std::max(lo - 1, std::min(hi + 1, d)) * s); };
        float x0 = px(std::min(w->mq_ax, w->mq_x), l.left, l.right), x1 = px(std::max(w->mq_ax, w->mq_x), l.left, l.right);
        float y0 = px(std::min(ay, w->mq_y), l.top, l.bottom), y1 = px(std::max(ay, w->mq_y), l.top, l.bottom);
        fill(w, D2D1::RectF(x0 / s, y0 / s, x1 / s, y1 / s), kAccent, 0.25f);
        if (x1 - x0 >= 1 && y1 - y0 >= 1) {
            w->br->SetColor(color(kAccent, 0.9f));
            w->dc->DrawRectangle(D2D1::RectF((x0 + 0.5f) / s, (y0 + 0.5f) / s, (x1 - 0.5f) / s, (y1 - 0.5f) / s), w->br, 1 / s);
        }
    }
    w->dc->PopAxisAlignedClip();
    if (g_shot && IsWindowVisible(w->rename)) { // the rename box is a child window: not in the bitmap otherwise
        RECT r;
        GetWindowRect(w->rename, &r);
        MapWindowPoints(NULL, w->hwnd, (POINT *)&r, 2);
        float s = scale(w);
        D2D1_RECT_F b = D2D1::RectF(r.left / s, r.top / s, r.right / s, r.bottom / s);
        fill(w, b, kBg);
        w->br->SetColor(color(kAccent));
        w->dc->DrawRectangle(b, w->br, 1);
        int n = GetWindowTextLengthW(w->rename);
        std::wstring t(n, 0);
        GetWindowTextW(w->rename, &t[0], n + 1);
        text(w, t, D2D1::RectF(b.left + 4, b.top, b.right - 2, b.bottom), w->f_ui, kText);
    }

    // scrollbar
    float h = l.bottom - l.top, total = content_h(w, v);
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
        if (v.results) left = Tf(L"{0} found by Everything", { std::to_wstring(n) });
        if (k) left += L"   " + (k == 1 ? std::wstring(T(L"1 selected")) : Tf(L"{0} selected", { std::to_wstring(k) }));
        if (!v.filter.empty() && !v.results) left += L"   " + Tf(L"of {0}", { std::to_wstring(v.all.size()) });
    } else {
        left = tasks_status(v.tasks);
    }
    text(w, left, D2D1::RectF(s.left + 12, s.top, s.right * 0.6f, s.bottom), w->f_small, kDim);
    if (v.kind == TAB_FILES && !is_drives(v)) {
        const wchar_t *glyphs[2] = { L"\xE8FD", L"\xE8A9" }; // Details, Large icons
        for (int i = 0; i < 2; i++) {
            D2D1_RECT_F b = D2D1::RectF(s.right - 62 + i * 28, s.top + 2, s.right - 36 + i * 28, s.bottom - 2);
            region(w, b, H_VIEW, i);
            if ((i == 1) == v.grid) fill_round(w, b, 4, kTabOn);
            else if (hovered(w, H_VIEW, i)) fill_round(w, b, 4, kHover);
            text(w, glyphs[i], b, w->f_icon_s, (i == 1) == v.grid ? kAccent : kDim);
        }
        s.right -= 70;
    }
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
        text(w, t, b, w->f_small, kOnAccent);
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
    for (auto &kv : w->thumb_bmps) release(kv.second);
    w->thumb_bmps.clear();
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

int row_at(Win *w, float x, float y) {
    View &v = cur_view(w);
    D2D1_RECT_F l = list_rect(w);
    int per = per_line(w, v), line = (int)std::floor((y - l.top + v.scroll) / line_h(v));
    int col = is_grid(v) ? (int)std::floor((x - l.left - 6) / tile_w(v)) : 0;
    if (line < 0 || col < 0 || col >= per) return -1;
    int p = line * per + col;
    if (p >= (int)v.shown.size()) return -1;
    if (is_grid(v)) {
        D2D1_RECT_F r = item_rect(w, v, p);
        if (!inside(r, x, y)) return -1; // the gap between tiles is background
    }
    return p;
}

// Details: only the icon and the name's text are the item; the rest of the row is space a marquee starts from (as File
// Explorer does). A tile is the item whole.
bool on_name(Win *w, View &v, int p, float x) {
    if (is_grid(v)) return true;
    float x0 = list_rect(w).left + 8;
    float end = std::min(x0 + name_width(w) - 8, x0 + 28 + text_width(w, v.all[v.shown[p]].name, w->f_ui) + 4);
    return x >= x0 && x < end;
}

// --- the marquee: a rectangle dragged from empty space selects what it touches --------------------------------------
// Everything is worked out from the selection at the press (mq_before), and on each move only the lines that came into
// or left the rectangle are set again: a drag over 100 000 rows costs what the rows it crossed since the last move cost.

// along one axis, the items k = 0..count-1, each [origin + k * pitch + in0, origin + (k + 1) * pitch - in1): those
// [lo, hi] touches are a..b (b < a: none)
void touched(float lo, float hi, float origin, float pitch, float in0, float in1, int count, int &a, int &b) {
    a = std::max(0, (int)std::floor((lo - origin - pitch + in1) / pitch) + 1);
    b = std::min(count - 1, (int)std::ceil((hi - origin - in0) / pitch) - 1);
}

// the items of one line, columns c0..c1, inside the rectangle or not
void mq_set(Win *w, View &v, int line, int c0, int c1, bool in) {
    int per = per_line(w, v), n = (int)v.shown.size();
    for (int c = c0; c <= c1; c++) {
        int p = line * per + c;
        if (p >= n) break;
        int i = v.shown[p];
        uint8_t was = w->mq_before[i];
        v.sel[i] = in ? (w->mq_mode == MQ_TOGGLE ? !was : 1) : (w->mq_mode == MQ_REPLACE ? 0 : was);
    }
}

// the view it began on, with the same listing (a refresh or another folder ends it)
bool mq_valid(Win *w) {
    View &v = cur_view(w);
    return w->cur == w->mq_tab && v.kind == TAB_FILES && v.gen == w->mq_gen && v.sel.size() == w->mq_before.size() && is_grid(v) == w->mq_grid;
}

void marquee_end(Win *w, bool cancel) {
    if (!w->mq) return;
    View &v = cur_view(w);
    bool valid = mq_valid(w), dragged = w->mq == 2;
    w->mq = 0; // before ReleaseCapture: its WM_CAPTURECHANGED comes back here
    KillTimer(w->hwnd, 9);
    if (GetCapture() == w->hwnd) ReleaseCapture();
    if (valid && cancel) { // Esc: the selection as it was
        v.sel = w->mq_before;
        v.focus = w->mq_focus;
        v.anchor = w->mq_anchor;
    } else if (valid && dragged && w->mq_l0 <= w->mq_l1) { // the focus on the touched item nearest the pointer
        D2D1_RECT_F l = list_rect(w);
        int per = per_line(w, v), line = (int)std::floor((w->mq_y - l.top + v.scroll) / line_h(v));
        int col = is_grid(v) ? (int)std::floor((w->mq_x - l.left - 6) / tile_w(v)) : 0;
        line = std::max(w->mq_l0, std::min(w->mq_l1, line));
        col = std::max(w->mq_c0, std::min(w->mq_c1, col));
        int p = std::min((int)v.shown.size() - 1, line * per + col);
        if (p >= 0) v.focus = v.anchor = p;
    }
    std::vector<uint8_t>().swap(w->mq_before);
    redraw(w);
}

// the rectangle from the anchor to the pointer: what it touches now; false when nothing changed
bool marquee_update(Win *w) {
    View &v = cur_view(w);
    if (!mq_valid(w)) return marquee_end(w, false), false;
    D2D1_RECT_F l = list_rect(w);
    float total = content_h(w, v), py = w->mq_y - l.top + v.scroll;
    float y0 = std::max(0.f, std::min(w->mq_ay, py)), y1 = std::min(total, std::max(w->mq_ay, py));
    float x0 = std::min(w->mq_ax, w->mq_x), x1 = std::max(w->mq_ax, w->mq_x);
    int per = per_line(w, v), lines = ((int)v.shown.size() + per - 1) / per, l0, l1, c0, c1;
    if (is_grid(v)) {
        touched(y0, y1, 0, line_h(v), 4, 2, lines, l0, l1); // item_rect's tile, inside its cell
        touched(x0, x1, l.left + 6, tile_w(v), 2, 2, per, c0, c1);
    } else {
        touched(y0, y1, 0, kRowH, 1, 1, lines, l0, l1);
        c0 = 0, c1 = x1 > l.left + 4 && x0 < l.right - kScrollW - 2 ? 0 : -1; // the row, across all its columns
    }
    if (l1 < l0 || c1 < c0) l0 = 0, l1 = -1, c0 = 0, c1 = -1;
    int o0 = w->mq_l0, o1 = w->mq_l1, oc0 = w->mq_c0, oc1 = w->mq_c1;
    if (l0 == o0 && l1 == o1 && c0 == oc0 && c1 == oc1) return false;
    // lines that left, lines that came in, and (if the columns changed) the lines in both
    auto outside = [](int a0, int a1, int b0, int b1, auto f) { // each line of a0..a1 not in b0..b1
        if (b1 < b0) {
            for (int k = a0; k <= a1; k++) f(k);
            return;
        }
        for (int k = a0; k <= std::min(a1, b0 - 1); k++) f(k);
        for (int k = std::max(a0, b1 + 1); k <= a1; k++) f(k);
    };
    outside(o0, o1, l0, l1, [&](int k) { mq_set(w, v, k, oc0, oc1, false); });
    outside(l0, l1, o0, o1, [&](int k) { mq_set(w, v, k, c0, c1, true); });
    if (c0 != oc0 || c1 != oc1)
        for (int k = std::max(o0, l0); k <= std::min(o1, l1); k++) {
            mq_set(w, v, k, oc0, oc1, false);
            mq_set(w, v, k, c0, c1, true);
        }
    w->mq_l0 = l0, w->mq_l1 = l1, w->mq_c0 = c0, w->mq_c1 = c1;
    return true;
}

// pressed on empty space: a marquee if the mouse moves far enough (the selection kept to build on, or restore)
void marquee_press(Win *w, float x, float y, int mode) {
    View &v = cur_view(w);
    w->mq = 1;
    w->mq_mode = mode;
    w->mq_ax = w->mq_x = x;
    w->mq_y = y;
    w->mq_ay = y - list_rect(w).top + v.scroll;
    w->mq_before = v.sel;
    w->mq_focus = v.focus;
    w->mq_anchor = v.anchor;
    w->mq_l0 = w->mq_c0 = 0, w->mq_l1 = w->mq_c1 = -1;
    w->mq_gen = v.gen;
    w->mq_tab = w->cur;
    w->mq_grid = is_grid(v);
    SetCapture(w->hwnd);
}

// past the list's top or bottom (or within a few DIPs of it), how far: the list scrolls that way, faster the further
float mq_overshoot(Win *w) {
    D2D1_RECT_F l = list_rect(w);
    const float band = 8;
    return w->mq_y < l.top + band ? w->mq_y - (l.top + band) : w->mq_y > l.bottom - band ? w->mq_y - (l.bottom - band) : 0;
}

// true while the marquee has the mouse
bool marquee_move(Win *w, float x, float y) {
    if (!w->mq) return false;
    w->mq_x = x;
    w->mq_y = y;
    View &v = cur_view(w);
    if (w->mq == 1) {
        float s = scale(w), ay = w->mq_ay - v.scroll + list_rect(w).top;
        if (std::fabs(x - w->mq_ax) * s <= GetSystemMetrics(SM_CXDRAG) && std::fabs(y - ay) * s <= GetSystemMetrics(SM_CYDRAG)) return true;
        if (!mq_valid(w)) return marquee_end(w, false), true;
        w->mq = 2;
        w->collapse_to = -1;
        if (w->mq_mode == MQ_REPLACE) std::fill(v.sel.begin(), v.sel.end(), 0);
        else v.sel = w->mq_before; // the press on a row may have changed it
        w->hover = H_NONE, w->hover_arg = -1;
    }
    marquee_update(w);
    if (w->mq && mq_overshoot(w)) SetTimer(w->hwnd, 9, 30, NULL);
    redraw(w); // the rectangle moved even when the selection did not
    return true;
}

// timer 9: the list scrolls under a marquee held past its edge, and the rectangle grows with it
void marquee_scroll(Win *w) {
    float d = w->mq == 2 ? mq_overshoot(w) : 0;
    if (!d) {
        KillTimer(w->hwnd, 9);
        return;
    }
    View &v = cur_view(w);
    float was = v.scroll, step = std::max(4.f, std::min(20 * kRowH, std::fabs(d)));
    v.scroll += d < 0 ? -step : step;
    clamp_scroll(w, v);
    if (v.scroll == was) { // the end of the list: again when the pointer moves
        KillTimer(w->hwnd, 9);
        return;
    }
    marquee_update(w);
    redraw(w);
}

void on_move(Win *w, float x, float y) {
    if (marquee_move(w, x, y)) return;
    if (w->drag_maybe && GetKeyState(VK_LBUTTON) < 0) {
        POINT pt;
        GetCursorPos(&pt);
        if (abs(pt.x - w->drag_from.x) > GetSystemMetrics(SM_CXDRAG) || abs(pt.y - w->drag_from.y) > GetSystemMetrics(SM_CYDRAG)) {
            w->drag_maybe = false;
            w->collapse_to = -1;
            w->press = H_NONE;
            shell_drag(w->hwnd, selected_targets(cur_view(w))); // modal until dropped
            return;
        }
    }
    if (w->drag_scroll) {
        View &v = cur_view(w);
        D2D1_RECT_F l = list_rect(w);
        float h = l.bottom - l.top, total = content_h(w, v), th = std::max(30.f, h * h / total);
        v.scroll = (y - w->drag_dy - l.top) / (h - th) * (total - h);
        clamp_scroll(w, v);
        redraw(w);
        return;
    }
    const Region *r = hit(w, x, y);
    int id = r ? r->id : H_NONE, arg = r ? r->arg : -1;
    if (id == H_LIST) {
        if (cur_view(w).kind == TAB_FILES) {
            int p = row_at(w, x, y);
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

// keys: the message's MK_ flags (Ctrl and Shift as they were when pressed)
void on_down(Win *w, float x, float y, bool dbl, bool right, UINT keys) {
    View &v = cur_view(w);
    const Region *r = hit(w, x, y);
    if (!r || w->mq) return;
    w->press = r->id;
    w->press_arg = r->arg;
    bool ctrl = (keys & MK_CONTROL) != 0, shift = (keys & MK_SHIFT) != 0;
    switch (r->id) {
    case H_SCROLL: {
        D2D1_RECT_F l = list_rect(w);
        float h = l.bottom - l.top, total = content_h(w, v), th = std::max(30.f, h * h / total), ty = l.top + (h - th) * (v.scroll / (total - h));
        if (y < ty || y > ty + th) { // a page up or down, as a scrollbar does
            v.scroll += (y < ty ? -1 : 1) * (h - line_h(v));
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
        int p = row_at(w, x, y), mode = ctrl ? MQ_TOGGLE : shift ? MQ_ADD : MQ_REPLACE;
        if (p < 0) {
            if (right) {
                if (!ctrl) select_only(v, -1);
            } else {
                marquee_press(w, x, y, mode);
                w->mq_clear = !ctrl; // a click without moving: nothing selected (on release)
            }
            break;
        }
        if (dbl && !right) {
            open_item(w, v.all[v.shown[p]], ctrl);
            return;
        }
        // the row's empty part: a click there is a click on the row, a drag a marquee (the name drags the files)
        bool marquee = !right && !on_name(w, v, p, x);
        if (marquee) {
            marquee_press(w, x, y, mode); // before the click changes the selection
            w->mq_clear = false;
        }
        if (shift) select_range(v, p);
        else if (ctrl && !right) {
            v.sel[v.shown[p]] ^= 1;
            v.focus = v.anchor = p;
        } else if (v.sel[v.shown[p]] && selected(v).size() > 1) {
            if (!right) w->collapse_to = p; // on release, if it was not a drag
        } else select_only(v, p);
        if (!right && !marquee && v.sel[v.shown[p]]) {
            w->drag_maybe = true;
            GetCursorPos(&w->drag_from);
        }
        break;
    }
    }
    redraw(w);
}

void set_grid(Win *w, bool on);

void on_up(Win *w, float x, float y, bool middle) {
    if (!middle) {
        w->drag_maybe = false;
        if (w->mq) {
            bool clicked = w->mq == 1 && w->mq_clear && mq_valid(w);
            marquee_end(w, false);
            if (clicked) select_only(cur_view(w), -1);
        }
        if (w->collapse_to >= 0 && cur_view(w).kind == TAB_FILES && row_at(w, x, y) == w->collapse_to) select_only(cur_view(w), w->collapse_to);
        w->collapse_to = -1;
    }
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
            int p = row_at(w, x, y);
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
    case H_VIEW: set_grid(w, r->arg == 1); break;
    case H_UPDATE: app_install_update(); return;
    }
    redraw(w);
}

void set_grid(Win *w, bool on) {
    View &v = cur_view(w);
    if (v.kind != TAB_FILES || v.grid == on) return;
    v.grid = on;
    clamp_scroll(w, v);
    ensure_visible(w, v, v.focus);
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
    if (w->mq && key == VK_ESCAPE) return marquee_end(w, true), true; // the drag undone
    View &v = cur_view(w);
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    D2D1_RECT_F l = list_rect(w);
    int per = per_line(w, v), page = std::max(1, (int)((l.bottom - l.top) / line_h(v)) - 1) * per;
    if (ctrl && key == 'T') return new_tab(w, v.kind == TAB_FILES ? v.path : L""), true;
    if (ctrl && key == 'W') return close_tab(w, w->cur), true;
    if (ctrl && shift && key == 'N' && v.kind == TAB_FILES && !folder_virtual(v.path)) return new_folder(w), true;
    if (ctrl && shift && (key == '2' || key == '6')) return set_grid(w, key == '2'), true; // Windows' keys: Large icons, Details
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
    // what Windows' shell does with the selection (Home's entries are places, not files to delete or rename)
    bool in_folder = !folder_virtual(v.path);
    if (key == VK_DELETE && in_folder) return shell_delete(w->hwnd, selected_targets(v), shift, conf_get("confirm_delete") != "0"), true;
    if (key == VK_F2 && in_folder) return start_rename(w), true;
    if (ctrl && key == 'C') return shell_clipboard(w->hwnd, selected_targets(v), false), true;
    if (ctrl && key == 'X' && in_folder) return shell_clipboard(w->hwnd, selected_targets(v), true), true;
    if (ctrl && key == 'V' && in_folder) return shell_paste(w->hwnd, v.path), true;
    if (alt && key == VK_RETURN) {
        std::vector<std::wstring> t = selected_targets(v);
        if (!t.empty()) shell_properties(w->hwnd, t[0]);
        else if (in_folder) shell_properties(w->hwnd, v.path);
        return true;
    }
    switch (key) {
    case VK_DOWN: move_focus(w, v.focus < 0 ? 0 : v.focus + per, shift, ctrl); return true;
    case VK_UP: move_focus(w, v.focus < 0 ? 0 : v.focus - per, shift, ctrl); return true;
    case VK_RIGHT:
        if (!is_grid(v)) return false;
        move_focus(w, v.focus + 1, shift, ctrl);
        return true;
    case VK_LEFT:
        if (!is_grid(v)) return false;
        move_focus(w, v.focus < 0 ? 0 : v.focus - 1, shift, ctrl);
        return true;
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

// the shell's context menu: of the selection on a row, of the folder elsewhere in the list; lp -1 = from the keyboard
bool context_menu(Win *w, LPARAM lp) {
    View &v = cur_view(w);
    if (v.kind != TAB_FILES) return false;
    float s = scale(w);
    D2D1_RECT_F l = list_rect(w);
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    std::vector<std::wstring> items;
    if (lp == -1) {
        items = selected_targets(v);
        D2D1_RECT_F r = v.focus >= 0 ? item_rect(w, v, v.focus) : D2D1::RectF(l.left, l.top, l.left + 30, l.top + 10);
        pt = { (LONG)((r.left + 30) * s), (LONG)(r.bottom * s) };
        ClientToScreen(w->hwnd, &pt);
    } else {
        POINT c = pt;
        ScreenToClient(w->hwnd, &c);
        if (!inside(l, c.x / s, c.y / s)) return false;
        if (row_at(w, c.x / s, c.y / s) >= 0) items = selected_targets(v);
    }
    if (shell_menu(w->hwnd, v.path, items, pt) == "rename") start_rename(w);
    return true;
}

// dropping on the window: on a folder row, into it; on a place in the navigation pane, there; else the folder shown
struct WinDrop final : DropHandler {
    Win *w;
    explicit WinDrop(Win *win) : w(win) {}
    std::wstring drop_folder(POINT screen) override {
        View &v = cur_view(w);
        if (v.kind != TAB_FILES) return L"";
        POINT c = screen;
        ScreenToClient(w->hwnd, &c);
        float x = c.x / scale(w), y = c.y / scale(w);
        const Region *r = hit(w, x, y);
        if (r && r->id == H_NAV) {
            size_t n = w->places.size(), i = (size_t)r->arg;
            return i < n ? w->places[i].path : i - n < w->drives.size() ? w->drives[i - n].path : L"";
        }
        if (inside(list_rect(w), x, y)) {
            int p = row_at(w, x, y);
            if (p >= 0 && v.all[v.shown[p]].dir) return v.all[v.shown[p]].target;
        }
        return v.path;
    }
    void drop_hover(POINT screen, bool in) override {
        int row = -1;
        View &v = cur_view(w);
        if (in && v.kind == TAB_FILES) {
            POINT c = screen;
            ScreenToClient(w->hwnd, &c);
            float x = c.x / scale(w), y = c.y / scale(w);
            int p = inside(list_rect(w), x, y) ? row_at(w, x, y) : -1;
            if (p >= 0 && v.all[v.shown[p]].dir) row = p;
        }
        if (row != w->drop_row) {
            w->drop_row = row;
            redraw(w);
        }
    }
};

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
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR: {
        LRESULT res = 0;
        if (shell_menu_message(msg, wp, lp, &res)) return res; // a shell extension's submenu
        break;
    }
    case WM_CONTEXTMENU:
        if (context_menu(w, lp)) return 0;
        break;
    case WM_NCCALCSIZE: // no title bar: the client reaches the top edge (the sides and bottom keep their borders)
        if (wp) {
            auto *p = (NCCALCSIZE_PARAMS *)lp;
            LONG top = p->rgrc[0].top;
            LRESULT r = DefWindowProcW(h, msg, wp, lp);
            p->rgrc[0].top = top + (IsZoomed(h) ? frame_px(h) : 0); // maximized, the window hangs off the screen by that
            return r;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (r != HTCLIENT) return r;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        if (!IsZoomed(h) && pt.y < frame_px(h)) return HTTOP;
        float x = pt.x / s, y = pt.y / s;
        if (y >= kTabH) return HTCLIENT;
        if (const Region *rg = hit(w, x, y)) return caption_hit(rg->id) ? caption_hit(rg->id) : HTCLIENT; // a tab, +
        return HTCAPTION; // the empty part of the tab bar moves the window (and a double click maximizes it)
    }
    case WM_NCMOUSEMOVE: {
        int id = caption_id(wp);
        if (id != w->hover || (id && w->hover_arg)) {
            w->hover = id;
            w->hover_arg = 0;
            redraw(w);
        }
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE | TME_NONCLIENT, h, 0 };
        TrackMouseEvent(&t);
        if (id) return 0;
        break;
    }
    case WM_NCMOUSELEAVE:
        if (caption_hit(w->hover)) {
            w->hover = H_NONE;
            w->press = H_NONE;
            redraw(w);
        }
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (int id = caption_id(wp)) { // our buttons: Windows would draw its old ones
            w->press = id;
            redraw(w);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (int id = caption_id(wp)) {
            if (w->press == id)
                PostMessageW(h, WM_SYSCOMMAND, id == H_MIN ? SC_MINIMIZE : id == H_CLOSE ? SC_CLOSE : IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0);
            w->press = H_NONE;
            redraw(w);
            return 0;
        }
        break;
    case WM_CREATE: {
        apply_theme(w);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); // WM_NCCALCSIZE again
        w->dpi = GetDpiForWindow(h);
        w->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)1, g_inst, NULL);
        w->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)2, g_inst, NULL);
        SetWindowSubclass(w->edit, edit_proc, 1, (DWORD_PTR)w);
        w->rename = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)3, g_inst, NULL);
        SetWindowSubclass(w->search, edit_proc, 2, (DWORD_PTR)w);
        SetWindowSubclass(w->rename, edit_proc, 3, (DWORD_PTR)w);
        edit_fonts(w);
        w->drop = new WinDrop(w);
        ChangeWindowMessageFilterEx(h, WM_COPYDATA, MSGFLT_ALLOW, NULL); // Everything may run elevated
        shell_register_drop(h, w->drop);
        w->places = folder_places();
        w->drives = nav_drives();
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB((kText >> 16) & 255, (kText >> 8) & 255, kText & 255));
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
                if (!v.results) apply_filter(v); // the folder filtered at once; Everything's results a moment later
                v.scroll = 0;
                v.typed_at = now_ms();
                // Everything answers in a few ms: asked right away (the timer only joins keys that come together, a paste)
                SetTimer(h, 8, USER_TIMER_MINIMUM, NULL);
                redraw(w);
            }
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(w); return 0;
    case WM_SIZE:
        marquee_end(w, false); // its lines and columns were of the old layout
        if (w->rt) w->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        if (!w->views.empty()) {
            clamp_scroll(w, cur_view(w));
            place_edits(w);
        }
        if (IsWindowVisible(w->edit)) ShowWindow(w->edit, SW_HIDE);
        if (IsWindowVisible(w->rename)) SetFocus(h); // the row moves: renamed with what was typed
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
        on_down(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s, msg == WM_LBUTTONDBLCLK, msg == WM_RBUTTONDOWN, (UINT)wp);
        return 0;
    case WM_CAPTURECHANGED: marquee_end(w, false); return 0; // taken by someone else: the marquee stops where it is
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
        if (IsWindowVisible(w->rename)) SetFocus(h);
        View &v = cur_view(w);
        if (v.kind == TAB_TASKS) tasks_wheel(v.tasks, GET_WHEEL_DELTA_WPARAM(wp));
        else if (GetKeyState(VK_CONTROL) < 0) set_grid(w, GET_WHEEL_DELTA_WPARAM(wp) > 0);
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
    case WM_DEVICECHANGE: w->drives = nav_drives(); redraw(w); return TRUE;
    case WM_APP_LIST: on_listing(w, (Listing *)lp); return 0;
    case WM_COPYDATA: { // Everything's answer to a search
        auto *cds = (COPYDATASTRUCT *)lp;
        for (View &v : w->views) {
            if (!v.search_id || v.search_id != cds->dwData || v.filter.empty()) continue;
            v.all.clear();
            folder_search_items(cds, v.all);
            folder_sort(v.all, v.sort, v.desc);
            v.results = true;
            v.loading = false;
            v.error.clear();
            v.sel.assign(v.all.size(), 0);
            v.focus = v.anchor = -1;
            v.scroll = 0;
            apply_filter(v);
            log_line("everything: " + std::to_string(v.all.size()) + " results, " + std::to_string((int)(now_ms() - v.typed_at)) + " ms after the key");
            redraw(w);
            return TRUE;
        }
        return FALSE;
    }
    case WM_APP_CHANGED: {
        PIDLIST_ABSOLUTE *pidls = nullptr;
        LONG ev = 0;
        if (HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &ev)) SHChangeNotification_Unlock(lock);
        SetTimer(h, 7, 150, NULL); // a burst of changes (a copy of 1000 files): one refresh
        return 0;
    }
    case WM_TIMER:
        if (wp == 8) { // the search typed: everything under the folder, through Everything
            KillTimer(h, 8);
            View &v = cur_view(w);
            if (v.kind != TAB_FILES) return 0;
            static DWORD ids;
            if (v.filter.empty()) {
                if (v.results) load(w, v); // back to the folder
            } else if (!folder_search(h, v.search_id = ++ids, folder_virtual(v.path) ? L"" : v.path, v.filter)) v.search_id = 0;
            return 0;
        }
        if (wp == 9) {
            marquee_scroll(w);
            return 0;
        }
        if (wp == 7) {
            View &v = cur_view(w);
            if (IsWindowVisible(w->rename) || w->mq) return 0; // after the rename, or the marquee
            KillTimer(h, 7);
            if (v.kind == TAB_FILES && !v.path.empty() && !v.results) load(w, v);
            return 0;
        }
        break;
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
    case WM_APP_THUMB: {
        auto *t = (Thumb *)lp;
        auto it = w->thumbs.find(t->target);
        if (t->gen == w->thumbs_gen && it != w->thumbs.end() && !it->second) {
            it->second = t;
            redraw(w);
        } else delete t;
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
        shell_revoke_drop(h);
        shell_unwatch(w->watch);
        w->watch = 0;
        return 0;
    case WM_NCDESTROY: {
        for (View &v : w->views) {
            if (v.tasks) tasks_destroy(v.tasks);
            if (v.gen) folder_forget(v.gen);
        }
        w->views.clear();
        thumbs_cancel(w->thumbs_gen);
        forget_thumbs(w);
        delete static_cast<WinDrop *>(w->drop);
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

bool window_focus_tasks() {
    for (Win *w : g_wins)
        for (int i = 0; i < (int)w->views.size(); i++)
            if (w->views[i].kind == TAB_TASKS) {
                switch_tab(w, i);
                if (IsIconic(w->hwnd)) ShowWindow(w->hwnd, SW_RESTORE);
                SetForegroundWindow(w->hwnd);
                return true;
            }
    return false;
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
bool window_shot(const std::wstring &name) {
    std::wstring png = shot_path(name);
    if (png.empty()) return false;
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
        std::unordered_map<std::wstring, ID2D1Bitmap *> thumb_bmps;
        thumb_bmps.swap(w->thumb_bmps);
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
        for (auto &kv : w->thumb_bmps) release(kv.second);
        w->thumb_bmps.swap(thumb_bmps);
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
    else if (verb == L"newfolder") new_folder(w);
    else if (verb == L"view") set_grid(w, arg == L"icons");
    else if (verb == L"rename") { // the focused item renamed to arg, as typed in the box
        start_rename(w);
        if (IsWindowVisible(w->rename)) {
            SetWindowTextW(w->rename, arg.c_str());
            SendMessageW(w->rename, WM_KILLFOCUS, 0, 0); // as leaving the box: renames
            SetFocus(w->hwnd);
        }
    } else if (verb == L"copyto" || verb == L"moveto") // the clipboard is the user's: tests copy without it
        shell_copy_to(w->hwnd, selected_targets(v), folder_normalize(arg), verb == L"moveto");
    else if (verb == L"delete") shell_delete(w->hwnd, selected_targets(v), arg == L"permanent", conf_get("confirm_delete") != "0");
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
    } else if (verb == L"mdown" || verb == L"mmove" || verb == L"mup") {
        // the left button at "x,y" (client DIPs; mdown also ",ctrl" or ",shift"), through the window's own handlers
        float x = 0, y = 0;
        swscanf(arg.c_str(), L"%f,%f", &x, &y);
        if (make_target(w)) draw_frame(w); // the regions a press finds exist once drawn
        if (verb == L"mdown")
            on_down(w, x, y, false, false, MK_LBUTTON | (arg.find(L"ctrl") != std::wstring::npos ? MK_CONTROL : 0) | (arg.find(L"shift") != std::wstring::npos ? MK_SHIFT : 0));
        else if (verb == L"mmove") on_move(w, x, y);
        else on_up(w, x, y, false);
    } else if (verb == L"selcount") { // into the log, for logcheck: "selcount <arg>: N selected, first <name>"
        std::vector<int> s = selected(v);
        log_line("selcount " + utf8(arg) + ": " + std::to_string(s.size()) + " selected, first " + (s.empty() ? std::string("-") : utf8(v.all[s[0]].name)) +
                 ", scroll " + std::to_string((int)v.scroll));
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
    if (g_edit_brush) { // the theme may have changed: the edit boxes' background too
        DeleteObject(g_edit_brush);
        g_edit_brush = NULL;
    }
    for (Win *w : g_wins) {
        apply_theme(w);
        for (HWND e : { w->edit, w->search, w->rename })
            if (e) InvalidateRect(e, NULL, TRUE);
        w->places = folder_places(); // the language may have changed: their names too
        w->drives = nav_drives();
        if (!w->views.empty()) {
            set_title(w);
            sync_search(w);
        }
        redraw(w);
    }
}
