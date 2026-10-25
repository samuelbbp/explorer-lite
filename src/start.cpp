// start.cpp: Start (see shell.h). A search box with the apps as you type, Enter opens the first, arrows move, Esc
// closes; with nothing typed, every app by name. The apps are the shell's own Apps folder (FOLDERID_AppsFolder: what
// Windows' Start lists, Store apps included, launched through IApplicationActivationManager) plus any Start menu
// shortcut it lacks (%ProgramData% and %AppData% Microsoft\Windows\Start Menu\Programs), listed on a thread each time
// Start opens. As you type, files and folders too, from Everything (the explorer's own search, folder.cpp), below the
// apps: the most recently changed first, without Windows' and programs' own folders. Below, the user and the power
// menu: lock, sleep, sign out, restart, shut down. Its window and icons go when it closes; only the names stay (a few
// hundred strings).
#include "shell.h"
#include "app.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "folder.h"
#include "gfx.h"
#include "idioma.h"
#include "keys.h"
#include "surface.h"
#include "thumbs.h"

namespace {

// layout, in DIPs
const float kW = 540, kH = 620, kPad = 16, kSearchH = 38, kLabelH = 34, kRowH = 40, kFootH = 56, kAppIcon = 24, kGap = 12;
const int kIconPx = 32;
const wchar_t kAppsPrefix[] = L"shell:AppsFolder\\";
// FOLDERID_AppsFolder and BHID_EnumItems, spelled out: not every SDK's uuid library has them
const GUID kAppsFolder = { 0x1e87508d, 0x89c2, 0x42f0, { 0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58 } };
const GUID kEnumItems = { 0x94f60519, 0x2850, 0x4924, { 0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39 } };

struct App {
    std::wstring name, target, key; // key: the name in lower case
};

enum Hit { H_NONE = -1, H_POWER = -2, H_USER = -3 }; // >= 0: a row of the list

struct FileHit {
    std::wstring name, target, folder;
    bool dir;
};

enum RowKind { R_APP, R_FILE, R_HEAD, R_NOTE };
struct Row {
    int kind, idx; // idx: into g_apps or the files
    std::wstring text; // headers and notes
};
bool pickable(const Row &r) { return r.kind == R_APP || r.kind == R_FILE; }
const DWORD kMaxFiles = 30;

struct StartWin {
    Surface s;
    HWND search = NULL;
    HFONT font = NULL;
    HBRUSH edit_brush = NULL;
    HMONITOR mon = NULL;
    bool sticky = false;
    std::vector<int> shown; // indexes into g_apps, in order
    std::vector<FileHit> files; // Everything's answer to the query
    std::vector<Row> rows;  // what the list shows: the apps, then the files
    DWORD search_id = 0;    // the query whose answer is awaited (older answers are dropped)
    double search_t0 = 0;
    bool everything = true; // false: it is not running (a note says so)
    int sel = 0, hover = H_NONE, press = H_NONE;
    float scroll = 0;
    bool tracking = false;
    std::unordered_map<std::wstring, Thumb *> icons; // by target; nullptr = asked, not here yet
    std::wstring query;
};

StartWin *g_st;
std::vector<App> g_apps;  // the last list made (kept: the next opening shows it while a new one is made)
bool g_listing;
std::wstring start_class() { return L"ExplorerLiteStart" + instance_suffix(); }

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// --- the list of apps (its own thread) ------------------------------------------------------------------------------

void add_shortcuts(const std::wstring &dir, std::vector<App> &out, std::vector<std::wstring> &seen) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            add_shortcuts(path, out, seen);
            continue;
        }
        size_t dot = name.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        std::wstring ext = lower(name.substr(dot));
        if (ext != L".lnk" && ext != L".url") continue;
        App a{ name.substr(0, dot), path, lower(name.substr(0, dot)) };
        if (std::find(seen.begin(), seen.end(), a.key) != seen.end()) continue;
        seen.push_back(a.key);
        out.push_back(a);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The last list, saved: Start shows it at once after a restart (the Apps folder takes seconds, cold, after sign-in).
std::wstring cache_path() { return data_dir() + L"\\apps.txt"; }

void save_cache() {
    std::wstring text;
    for (const App &a : g_apps) text += a.name + L"\t" + a.target + L"\n";
    std::string bytes = utf8(text);
    std::wstring tmp = cache_path() + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, NULL);
    CloseHandle(f);
    if (put == bytes.size()) MoveFileExW(tmp.c_str(), cache_path().c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}

void load_cache() {
    HANDLE f = CreateFileW(cache_path().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string bytes;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got && bytes.size() < 4 * 1024 * 1024) bytes.append(buf, got);
    CloseHandle(f);
    std::wstring text = wide(bytes);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        size_t tab = text.find(L'\t', pos);
        if (tab != std::wstring::npos && tab < end && tab > pos) {
            std::wstring name = text.substr(pos, tab - pos);
            g_apps.push_back({ name, text.substr(tab + 1, end - tab - 1), lower(name) });
        }
        pos = end + 1;
    }
}

DWORD WINAPI list_thread(void *to) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    double t0 = now_ms();
    auto *out = new std::vector<App>;
    std::vector<std::wstring> seen;
    IShellItem *folder = nullptr;
    IEnumShellItems *en = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(kAppsFolder, KF_FLAG_DEFAULT, NULL, IID_PPV_ARGS(&folder))) &&
        SUCCEEDED(folder->BindToHandler(NULL, kEnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem *it = nullptr;
        while (en->Next(1, &it, NULL) == S_OK) {
            PWSTR name = nullptr, id = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && SUCCEEDED(it->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &id)) && name && *name) {
                App a{ name, kAppsPrefix + std::wstring(id), lower(name) };
                seen.push_back(a.key);
                out->push_back(a);
            }
            CoTaskMemFree(name);
            CoTaskMemFree(id);
            it->Release();
        }
    }
    if (en) en->Release();
    if (folder) folder->Release();
    // Start menu shortcuts the Apps folder does not have (it has almost all of them)
    for (const KNOWNFOLDERID *id : { &FOLDERID_CommonPrograms, &FOLDERID_Programs }) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, NULL, &p)) && p) add_shortcuts(p, *out, seen);
        CoTaskMemFree(p);
    }
    std::stable_sort(out->begin(), out->end(), [](const App &a, const App &b) { return a.key < b.key; });
    // the Apps folder lists some apps twice (a shortcut per user and for all users): one row per name
    out->erase(std::unique(out->begin(), out->end(), [](const App &a, const App &b) { return a.key == b.key; }), out->end());
    log_line("start: " + std::to_string(out->size()) + " apps listed in " + std::to_string((int)(now_ms() - t0)) + " ms");
    if (!PostMessageW((HWND)to, WM_APP_APPS, 0, (LPARAM)out)) delete out;
    CoUninitialize();
    return 0;
}

// --- what is shown -----------------------------------------------------------------------------------------------

D2D1_RECT_F list_rect() { return D2D1::RectF(kPad / 2, kPad + kSearchH + kLabelH, kW - kPad / 2, kH - kFootH); }
D2D1_RECT_F search_rect() { return D2D1::RectF(kPad, kPad, kW - kPad, kPad + kSearchH); }
D2D1_RECT_F power_rect() { return D2D1::RectF(kW - kPad - 40, kH - kFootH + 8, kW - kPad, kH - 8); }
D2D1_RECT_F user_rect() { return D2D1::RectF(kPad, kH - kFootH + 8, kW / 2, kH - 8); }

// the best first: the name starts with it, then a word in it does, then it is anywhere in it
void filter(StartWin *w) {
    w->shown.clear();
    std::wstring q = lower(w->query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    while (!q.empty() && q[0] == L' ') q.erase(0, 1);
    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < (int)g_apps.size(); i++) {
        const std::wstring &k = g_apps[i].key;
        if (q.empty()) {
            scored.push_back({ 0, i });
            continue;
        }
        size_t at = k.find(q);
        if (at == std::wstring::npos) continue;
        int score = at == 0 ? 0 : 2;
        for (size_t p = at; score == 2 && p != std::wstring::npos; p = k.find(q, p + 1))
            if (p > 0 && (k[p - 1] == L' ' || k[p - 1] == L'-' || k[p - 1] == L'(')) score = 1;
        scored.push_back({ score, i });
    }
    std::stable_sort(scored.begin(), scored.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) { return a.first < b.first; });
    for (auto &s : scored) w->shown.push_back(s.second);
    // the rows: the apps first, then the files and folders
    w->rows.clear();
    for (int i : w->shown) w->rows.push_back({ R_APP, i, L"" });
    if (!q.empty()) {
        if (w->shown.empty() && !g_apps.empty()) w->rows.push_back({ R_NOTE, 0, Tf(L"No apps match «{0}»", { w->query }) });
        if (!w->files.empty()) {
            w->rows.push_back({ R_HEAD, 0, T(L"Files and folders") });
            for (int i = 0; i < (int)w->files.size(); i++) w->rows.push_back({ R_FILE, i, L"" });
        } else if (!w->everything) w->rows.push_back({ R_NOTE, 0, T(L"Files: no file search is running (Hurón or Everything)") });
    }
    w->sel = 0;
    while (w->sel < (int)w->rows.size() && !pickable(w->rows[w->sel])) w->sel++;
    w->scroll = 0;
}

// Everything's answer: the files below the apps (the selection stays where it was if it was on an app)
void on_files(StartWin *w, const COPYDATASTRUCT *cds) {
    std::vector<Item> items;
    folder_search_items(cds, items);
    w->files.clear();
    for (const Item &it : items) w->files.push_back({ it.name, it.target, it.type, it.dir });
    int sel = w->sel;
    bool on_app = sel >= 0 && sel < (int)w->rows.size() && w->rows[sel].kind == R_APP;
    float scroll = w->scroll;
    filter(w);
    if (on_app && sel < (int)w->rows.size()) w->sel = sel, w->scroll = scroll;
    log_line("start: " + std::to_string(w->files.size()) + " files from Everything in " + std::to_string((int)(now_ms() - w->search_t0)) + " ms");
}

// the query to Everything, at once on each key (its answer comes in a few ms; a newer query makes the older moot)
void search_files(StartWin *w) {
    static DWORD ids;
    std::wstring q = w->query;
    while (!q.empty() && q.back() == L' ') q.pop_back();
    if (q.empty()) {
        w->search_id = 0;
        w->files.clear();
        return;
    }
    // Windows', programs' and the bin's own files are not what anyone looks for from Start
    std::wstring full = q + L" !C:\\Windows\\ !\\AppData\\ !\\$Recycle.Bin\\ !C:\\ProgramData\\ !\"C:\\Program Files\"";
    w->search_id = ++ids;
    w->search_t0 = now_ms();
    w->everything = folder_search(w->s.hwnd, w->search_id, L"", full, kMaxFiles, true);
    if (!w->everything) w->search_id = 0;
}

void clamp_scroll(StartWin *w) {
    D2D1_RECT_F l = list_rect();
    float max = std::max(0.f, w->rows.size() * kRowH - (l.bottom - l.top));
    w->scroll = std::max(0.f, std::min(max, w->scroll));
}

void ensure_visible(StartWin *w) {
    D2D1_RECT_F l = list_rect();
    float top = w->sel * kRowH, h = l.bottom - l.top;
    if (top < w->scroll) w->scroll = top;
    if (top + kRowH > w->scroll + h) w->scroll = top + kRowH - h;
    clamp_scroll(w);
}

Thumb *icon_for(StartWin *w, const std::wstring &target) {
    auto it = w->icons.find(target);
    if (it != w->icons.end()) return it->second;
    w->icons[target] = nullptr;
    thumbs_request(shell_host(), kGenStart, target, kIconPx);
    return nullptr;
}

std::wstring user_name() {
    wchar_t n[256] = L"";
    DWORD len = ARRAYSIZE(n);
    GetUserNameW(n, &len);
    return n;
}

void draw_start(void *ctx) {
    StartWin *w = (StartWin *)ctx;
    Surface &s = w->s;
    surf_fill(s, D2D1::RectF(0, 0, kW, kH), kBg);
    // the search box: the edit control sits in it (a shot draws its text instead)
    D2D1_RECT_F sr = search_rect();
    surf_round(s, sr, 6, kPane);
    s.br->SetColor(color(kAccent));
    s.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.5f, sr.top + 0.5f, sr.right - 0.5f, sr.bottom - 0.5f), 6, 6), s.br, 1);
    surf_text(s, L"\xE721", D2D1::RectF(sr.left + 6, sr.top, sr.left + 34, sr.bottom), s.f_icon, kDim);
    if (s.shot) {
        D2D1_RECT_F tr = D2D1::RectF(sr.left + 38, sr.top, sr.right - 10, sr.bottom);
        if (w->query.empty()) surf_text(s, T(L"Type to search apps"), tr, s.f_ui, kDim);
        else surf_text(s, w->query, tr, s.f_ui, kText);
    }
    // what the list is
    D2D1_RECT_F l = list_rect();
    std::wstring label = w->query.empty() ? std::wstring(T(L"All apps")) : Tf(L"Apps ({0})", { std::to_wstring(w->shown.size()) });
    surf_text(s, label, D2D1::RectF(kPad, l.top - kLabelH, kW - kPad, l.top - 4), s.f_big, kText);
    s.dc->PushAxisAlignedClip(l, D2D1_ANTIALIAS_MODE_ALIASED);
    if (g_apps.empty()) surf_text(s, g_listing ? T(L"Listing apps…") : T(L"No apps found"), D2D1::RectF(kPad, l.top, kW - kPad, l.top + kRowH), s.f_ui, kDim);
    int first = (int)(w->scroll / kRowH);
    float top = l.top + (g_apps.empty() ? kRowH : 0);
    for (int i = first; i < (int)w->rows.size(); i++) {
        float y = top + i * kRowH - w->scroll;
        if (y > l.bottom) break;
        const Row &row = w->rows[i];
        D2D1_RECT_F r = D2D1::RectF(l.left, y, l.right, y + kRowH);
        if (row.kind == R_HEAD) {
            surf_text(s, row.text, D2D1::RectF(kPad, y + 6, kW - kPad, y + kRowH), s.f_big, kText);
            continue;
        }
        if (row.kind == R_NOTE) {
            surf_text(s, row.text, D2D1::RectF(kPad, y, kW - kPad, y + kRowH), s.f_ui, kDim);
            continue;
        }
        if (i == w->sel) surf_round(s, r, 4, w->query.empty() && w->hover != i ? kHover : kSelect);
        else if (i == w->hover) surf_round(s, r, 4, kHover);
        float cy = y + kRowH / 2, tx = r.left + 12 + kAppIcon + 12;
        if (row.kind == R_APP) {
            const App &a = g_apps[row.idx];
            surf_icon(s, icon_for(w, a.target), D2D1::RectF(r.left + 12, cy - kAppIcon / 2, r.left + 12 + kAppIcon, cy + kAppIcon / 2));
            surf_text(s, a.name, D2D1::RectF(tx, y, r.right - 12, y + kRowH), s.f_ui, kText);
        } else { // a file: its name, and the folder it is in below
            const FileHit &f = w->files[row.idx];
            surf_icon(s, icon_for(w, f.target), D2D1::RectF(r.left + 12, cy - kAppIcon / 2, r.left + 12 + kAppIcon, cy + kAppIcon / 2));
            surf_text(s, f.name, D2D1::RectF(tx, y + 2, r.right - 12, cy + 1), s.f_ui, kText);
            surf_text(s, f.folder, D2D1::RectF(tx, cy - 1, r.right - 12, y + kRowH - 3), s.f_small, kDim);
        }
    }
    s.dc->PopAxisAlignedClip();
    // the user and the power menu
    surf_fill(s, D2D1::RectF(0, kH - kFootH, kW, kH), kPane);
    surf_fill(s, D2D1::RectF(0, kH - kFootH, kW, kH - kFootH + 1), kLine);
    D2D1_RECT_F ur = user_rect(), pr = power_rect();
    if (w->hover == H_POWER) surf_round(s, pr, 4, w->press == H_POWER ? kPress : kHover);
    surf_text(s, L"\xE7E8", pr, s.f_icon, kText);
    surf_text(s, L"\xE77B", D2D1::RectF(ur.left, ur.top, ur.left + 32, ur.bottom), s.f_icon, kText);
    surf_text(s, user_name(), D2D1::RectF(ur.left + 38, ur.top, ur.right, ur.bottom), s.f_ui, kText);
    // the edge
    s.br->SetColor(color(kLine));
    s.dc->DrawRectangle(D2D1::RectF(0.5f, 0.5f, kW - 0.5f, kH - 0.5f), s.br, 1);
}

void redraw(StartWin *w) { InvalidateRect(w->s.hwnd, NULL, FALSE); }

// --- the power menu ------------------------------------------------------------------------------------------------

enum Power { P_LOCK = 1, P_SLEEP, P_SIGNOUT, P_RESTART, P_SHUTDOWN };

bool shutdown_privilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp = { 1 };
    LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

} // namespace

void start_power(int what) {
    const char *names[] = { "", "lock", "sleep", "sign out", "restart", "shut down" };
    if (!shell_real()) { // a test instance never locks or turns anything off
        log_line(std::string("power: ") + names[what] + " (a test instance does nothing)");
        return;
    }
    log_line(std::string("power: ") + names[what]);
    const DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    switch (what) {
    case P_LOCK: LockWorkStation(); break;
    case P_SLEEP: {
        typedef BOOLEAN(WINAPI * PSuspend)(BOOLEAN, BOOLEAN, BOOLEAN);
        HMODULE pp = LoadLibraryExW(L"powrprof.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto suspend = pp ? (PSuspend)(void *)GetProcAddress(pp, "SetSuspendState") : nullptr;
        if (suspend) suspend(FALSE, FALSE, FALSE);
        break;
    }
    case P_SIGNOUT: ExitWindowsEx(EWX_LOGOFF, reason); break;
    case P_RESTART:
        shutdown_privilege();
        ExitWindowsEx(EWX_REBOOT, reason);
        break;
    case P_SHUTDOWN: // with fast startup, as Windows' Shut down
        shutdown_privilege();
        ExitWindowsEx(EWX_POWEROFF | EWX_HYBRID_SHUTDOWN, reason);
        break;
    }
}

namespace {

void power_menu(StartWin *w) {
    ShellMenu m(w->s.dpi);
    m.add(P_LOCK, L"\xE72E", T(L"Lock"));
    m.add(P_SLEEP, L"\xE708", T(L"Sleep"));
    m.add(P_SIGNOUT, L"\xF3B1", T(L"Sign out"));
    m.sep();
    m.add(P_RESTART, L"\xE777", T(L"Restart"));
    m.add(P_SHUTDOWN, L"\xE7E8", T(L"Shut down"));
    w->sticky = true; // the menu takes the focus: Start stays until it closes
    UINT cmd = m.show(w->s.hwnd);
    w->sticky = false;
    if (!cmd) return;
    start_close();
    start_power((int)cmd);
}

// --- input -----------------------------------------------------------------------------------------------------------

void launch(StartWin *w, int i) {
    if (i < 0 || i >= (int)w->rows.size() || !pickable(w->rows[i])) return;
    const Row &r = w->rows[i];
    if (r.kind == R_APP) {
        std::wstring target = g_apps[r.idx].target;
        start_close();
        shell_launch(target);
        return;
    }
    FileHit f = w->files[r.idx];
    start_close();
    if (f.dir) shell_launch(exe_path(), L"\"" + f.target + L"\""); // a folder: in explorer-lite
    else shell_launch(f.target);
}

int hit(StartWin *w, float x, float y) {
    if (surf_inside(power_rect(), x, y)) return H_POWER;
    D2D1_RECT_F l = list_rect();
    if (!surf_inside(l, x, y)) return H_NONE;
    float top = l.top + (g_apps.empty() ? kRowH : 0);
    int i = (int)floorf((y - top + w->scroll) / kRowH);
    return i >= 0 && i < (int)w->rows.size() && pickable(w->rows[i]) ? i : H_NONE;
}

// the next row that can be chosen, `d` rows away at least (headers and notes are skipped)
int step_sel(StartWin *w, int d) {
    int n = (int)w->rows.size(), i = std::max(0, std::min(n - 1, w->sel + d));
    int dir = d < 0 ? -1 : 1;
    while (i >= 0 && i < n && !pickable(w->rows[i])) i += dir;
    if (i < 0 || i >= n) { // none that way: the nearest the other way
        i = std::max(0, std::min(n - 1, w->sel + d));
        while (i >= 0 && i < n && !pickable(w->rows[i])) i -= dir;
    }
    return i >= 0 && i < n ? i : w->sel;
}

bool on_key(StartWin *w, WPARAM key) {
    int page = std::max(1, (int)((list_rect().bottom - list_rect().top) / kRowH) - 1);
    switch (key) {
    case VK_ESCAPE: start_close(); return true;
    case VK_RETURN: launch(w, w->sel); return true;
    case VK_DOWN: w->sel = step_sel(w, 1); break;
    case VK_UP: w->sel = step_sel(w, -1); break;
    case VK_NEXT: w->sel = step_sel(w, page); break;
    case VK_PRIOR: w->sel = step_sel(w, -page); break;
    default: return false;
    }
    if (w->sel < 0) w->sel = 0;
    ensure_visible(w);
    redraw(w);
    return true;
}

LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    StartWin *w = (StartWin *)data;
    if (msg == WM_KEYDOWN && wp != VK_HOME && wp != VK_END && wp != VK_LEFT && wp != VK_RIGHT && on_key(w, wp)) return 0;
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0; // no beep
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, edit_proc, id);
    return DefSubclassProc(h, msg, wp, lp);
}

void place_edit(StartWin *w) {
    float s = surf_scale(w->s);
    D2D1_RECT_F r = search_rect();
    int x = (int)((r.left + 38) * s), top = (int)((r.top + 10) * s);
    SetWindowPos(w->search, NULL, x, top, (int)((r.right - 10) * s) - x, (int)((r.bottom - 8) * s) - top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void edit_style(StartWin *w) {
    if (w->font) DeleteObject(w->font);
    w->font = CreateFontW(-MulDiv(14, (int)w->s.dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(w->search, WM_SETFONT, (WPARAM)w->font, TRUE);
    if (w->edit_brush) DeleteObject(w->edit_brush);
    w->edit_brush = CreateSolidBrush(RGB(kPane >> 16, (kPane >> 8) & 255, kPane & 255));
    SendMessageW(w->search, EM_SETCUEBANNER, TRUE, (LPARAM)T(L"Type to search apps"));
}

LRESULT CALLBACK start_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    StartWin *w = g_st && g_st->s.hwnd == h ? g_st : nullptr;
    if (!w) return DefWindowProcW(h, msg, wp, lp);
    float s = surf_scale(w->s);
    switch (msg) {
    case WM_PAINT: surf_paint(w->s, draw_start, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !w->sticky) PostMessageW(h, WM_CLOSE, 0, 0); // clicked elsewhere
        else if (LOWORD(wp) != WA_INACTIVE) SetFocus(w->search);
        return 0;
    case WM_CLOSE: start_close(); return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, RGB(kText >> 16, (kText >> 8) & 255, kText & 255));
        SetBkColor((HDC)wp, RGB(kPane >> 16, (kPane >> 8) & 255, kPane & 255));
        return (LRESULT)w->edit_brush;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == w->search) {
            int n = GetWindowTextLengthW(w->search);
            std::wstring q(n, 0);
            GetWindowTextW(w->search, &q[0], n + 1);
            w->query = q;
            search_files(w); // the files a few ms later (or the note that Everything is not running)
            filter(w);       // the apps at once
            redraw(w);
        }
        return 0;
    case WM_COPYDATA: { // Everything's answer
        auto *cds = (COPYDATASTRUCT *)lp;
        if (!cds || !w->search_id || cds->dwData != w->search_id) return FALSE;
        on_files(w, cds);
        redraw(w);
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        int i = hit(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        if (i != w->hover) {
            w->hover = i;
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
        w->hover = w->press = H_NONE;
        redraw(w);
        return 0;
    case WM_LBUTTONDOWN:
        w->press = hit(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        redraw(w);
        return 0;
    case WM_LBUTTONUP: {
        int i = hit(w, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        bool same = i == w->press;
        w->press = H_NONE;
        if (same && i == H_POWER) power_menu(w);
        else if (same && i >= 0) launch(w, i);
        else redraw(w);
        return 0;
    }
    case WM_MOUSEWHEEL:
        w->scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * kRowH * 3;
        clamp_scroll(w);
        redraw(w);
        return 0;
    case WM_DPICHANGED: {
        w->s.dpi = HIWORD(wp);
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        edit_style(w);
        place_edit(w);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool start_is_open() { return g_st != nullptr; }

void start_list_probe() { list_thread(NULL); }

void start_prepare() {
    if (g_listing) return;
    if (g_apps.empty()) load_cache();
    g_listing = true;
    HANDLE t = CreateThread(NULL, 0, list_thread, shell_host(), 0, NULL);
    if (t) CloseHandle(t);
    else g_listing = false;
}

void start_on_apps(void *list) {
    auto *apps = (std::vector<App> *)list;
    g_listing = false;
    if (apps->empty() && !g_apps.empty()) { // the Apps folder failed: the last list stays
        delete apps;
        return;
    }
    g_apps.swap(*apps);
    delete apps;
    save_cache();
    if (g_st) {
        filter(g_st);
        redraw(g_st);
    }
}

void start_toggle(HMONITOR mon) {
    if (g_st) start_close();
    else start_open(mon, false);
}

void start_open(HMONITOR mon, bool sticky) {
    if (g_st) {
        g_st->sticky = g_st->sticky || sticky;
        SetForegroundWindow(g_st->s.hwnd);
        return;
    }
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = start_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        std::wstring cls = start_class();
        wc.lpszClassName = cls.c_str();
        registered = RegisterClassExW(&wc) != 0;
    }
    StartWin *w = new StartWin;
    w->mon = mon;
    w->sticky = sticky;
    UINT dpi = shell_monitor_dpi(mon);
    RECT bar = shell_bar_rect(mon);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int cw = MulDiv((int)kW, (int)dpi, 96), ch = MulDiv((int)kH, (int)dpi, 96), gap = MulDiv((int)kGap, (int)dpi, 96);
    int x = mi.rcMonitor.left + gap, y = bar.top - gap - ch;
    g_st = w;
    w->s.dpi = dpi;
    w->s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, start_class().c_str(), L"Start", WS_POPUP | WS_CLIPCHILDREN, x, y, cw, ch, NULL, NULL,
                                g_inst, NULL);
    if (!w->s.hwnd) {
        g_st = nullptr;
        delete w;
        return;
    }
    w->s.dpi = GetDpiForWindow(w->s.hwnd);
    DWORD round = 2; // DWMWCP_ROUND (Windows 11; ignored before)
    DwmSetWindowAttribute(w->s.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    surf_formats(w->s);
    ChangeWindowMessageFilterEx(w->s.hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL); // Everything may run elevated
    w->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, w->s.hwnd, (HMENU)1, g_inst, NULL);
    SetWindowSubclass(w->search, edit_proc, 1, (DWORD_PTR)w);
    edit_style(w);
    place_edit(w);
    if (g_apps.empty()) load_cache();
    filter(w);
    // a new list each time (apps come and go); the last one shows meanwhile
    if (!g_listing) {
        g_listing = true;
        HANDLE t = CreateThread(NULL, 0, list_thread, shell_host(), 0, NULL);
        if (t) CloseHandle(t);
        else g_listing = false;
    }
    ShowWindow(w->s.hwnd, SW_SHOW);
    SetForegroundWindow(w->s.hwnd);
    SetFocus(w->search);
    for (HWND b = FindWindowExW(NULL, NULL, (L"ExplorerLiteTaskbar" + instance_suffix()).c_str(), NULL); b;
         b = FindWindowExW(NULL, b, (L"ExplorerLiteTaskbar" + instance_suffix()).c_str(), NULL))
        InvalidateRect(b, NULL, FALSE); // the Start button shows it open
}

void start_close() {
    StartWin *w = g_st;
    if (!w) return;
    g_st = nullptr; // first: closing sends messages that must not find it
    for (auto &kv : w->icons) {
        if (kv.second) surf_forget(w->s, kv.second);
        delete kv.second;
    }
    surf_free(w->s);
    DestroyWindow(w->s.hwnd);
    if (w->font) DeleteObject(w->font);
    if (w->edit_brush) DeleteObject(w->edit_brush);
    delete w;
    for (HWND b = FindWindowExW(NULL, NULL, (L"ExplorerLiteTaskbar" + instance_suffix()).c_str(), NULL); b;
         b = FindWindowExW(NULL, b, (L"ExplorerLiteTaskbar" + instance_suffix()).c_str(), NULL))
        InvalidateRect(b, NULL, FALSE);
    // back to the bars alone: what Start used goes back to Windows now
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

bool start_shot(const std::wstring &png) { return g_st && surf_shot(g_st->s, png, draw_start, g_st); }

void start_command(const std::wstring &verb, const std::wstring &arg) {
    if (!g_st) return;
    if (verb == L"search") SetWindowTextW(g_st->search, arg.c_str()); // EN_CHANGE filters
    else if (verb == L"key") on_key(g_st, (WPARAM)_wtoi(arg.c_str()));
    redraw(g_st);
}

void start_on_icon(Thumb *t) {
    if (g_st) {
        auto it = g_st->icons.find(t->target);
        if (it != g_st->icons.end() && !it->second) {
            it->second = t;
            redraw(g_st);
            return;
        }
    }
    delete t;
}

void start_theme_changed() {
    if (!g_st) return;
    edit_style(g_st);
    InvalidateRect(g_st->search, NULL, TRUE);
    redraw(g_st);
}
