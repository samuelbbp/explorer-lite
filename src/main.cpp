// main.cpp: the resident explorer-lite. One process per user (per --instance): a hidden window with the tray
// icon, the update checks and the commands that other launches send it (WM_COPYDATA). Windows are opened on
// demand (window.cpp); with none open it holds no Direct2D at all.
//
//   explorer-lite.exe [folder]         opens a window (starting the resident if it is not running)
//   explorer-lite.exe --tasks          opens a window on the Tasks tab (the one open, if there is one)
//   explorer-lite.exe --taskbar-settings   opens the taskbar's settings (taskbarset.cpp)
//   explorer-lite.exe --tray           only the resident (Windows startup)
//   explorer-lite.exe --quit           asks the running one to save its windows and exit (installer); exit code 10 if
//                                      there was one, 0 if not
//   explorer-lite.exe --updated        after an update: reopens the windows that were open
//   explorer-lite.exe --cmd "<verb> <arg>"   a test command for the running one: shot <name>.png (into <data>\shots), go <folder>, search <text>,
//                                      key <vk>, select <name>, newtab <folder>, tasks, taskpage <n>, size <w>x<h>, closeall,
//                                      newfolder, rename <name>, copyto|moveto <folder>, delete [permanent], view icons|details
//   explorer-lite.exe --unregister     undoes what explorer-lite registered for this user (uninstaller): Start
//                                      with Windows and the folders' open verb
//   explorer-lite.exe --everything <folder> <text>   a search through Everything, its results to the log (tests:
//                                      Everything only answers windows on its own desktop, and this needs none)
//   explorer-lite.exe --shell          the shell: a taskbar per monitor and Start (shell.cpp), a process of its own;
//                                      with --cmd or --quit, a command for the running shell
//   explorer-lite.exe --apps           the apps Start would list, to the log (tests)
//   --instance <name>                  its own data folder, mutex and window classes (tests)
#include "app.h"
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include "conf.h"
#include "folder.h"
#include "gfx.h"
#include "idioma.h"
#include "keys.h"
#include "shell.h"
#include "taskbar.h"
#include "update.h"

HINSTANCE g_inst;
HWND g_host;

namespace {

const UINT kTrayId = 1;
const UINT_PTR kTimerUpdate = 1;
const DWORD kFirstCheckMs = 30 * 1000, kCheckEveryMs = 4 * 60 * 60 * 1000;
UINT g_msg_taskbar_created;
bool g_tray_added;
bool g_quitting;

std::wstring host_class() { return L"ExplorerLiteHost" + instance_suffix(); }
std::wstring session_path() { return data_dir() + L"\\session.txt"; }
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
std::wstring run_value() { return L"explorer-lite" + instance_suffix(); }

// --- the command line ---------------------------------------------------------------------------

struct Command {
    bool tray = false, tasks = false, quit = false, updated = false, unregister = false, everything = false, shell = false, apps = false,
         taskbar = false;
    DWORD restarted = 0; // --restarted <pid>: the shell that crashed (shell.cpp)
    std::wstring path, cmd, text;
};

Command parse(int argc, wchar_t **argv) {
    Command c;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--tray") c.tray = true;
        else if (a == L"--tasks") c.tasks = true;
        else if (a == L"--taskbar-settings") c.taskbar = true;
        else if (a == L"--quit") c.quit = true;
        else if (a == L"--updated") c.updated = true;
        else if (a == L"--unregister") c.unregister = true;
        else if (a == L"--shell") c.shell = true;
        else if (a == L"--apps") c.apps = true;
        else if (a == L"--restarted" && i + 1 < argc) c.restarted = (DWORD)_wtoi(argv[++i]);
        else if (a == L"--instance" && i + 1 < argc) set_instance(argv[++i]);
        else if (a == L"--cmd" && i + 1 < argc) c.cmd = argv[++i];
        else if (a == L"--everything" && i + 2 < argc) {
            c.everything = true;
            c.path = argv[++i];
            c.text = argv[++i];
        }
        else if (!a.empty() && a[0] != L'-') c.path = a;
    }
    return c;
}

// tab-separated, as it travels in WM_COPYDATA: "open", "tasks", "taskbar", "quit", "tray" or "cmd", and the path (or
// the command)
std::wstring serialize(const Command &c) {
    if (!c.cmd.empty()) return L"cmd\t" + c.cmd;
    std::wstring s = c.quit ? L"quit" : c.tasks ? L"tasks" : c.taskbar ? L"taskbar" : c.tray ? L"tray" : L"open";
    return s + L"\t" + c.path;
}

void run(const Command &c) {
    if (!c.cmd.empty()) {
        size_t sp = c.cmd.find(L' ');
        std::wstring verb = c.cmd.substr(0, sp), arg = sp == std::wstring::npos ? L"" : c.cmd.substr(sp + 1);
        if (verb == L"shot") log_line(std::string("shot ") + (window_shot(arg) ? "ok" : "failed"));
        else if (verb == L"closeall") window_close_all();
        else if (taskbar_settings_command(verb, arg)) {
        } else window_command(verb, arg);
        return;
    }
    if (c.quit) {
        app_quit();
        return;
    }
    if (c.taskbar) {
        taskbar_settings_open();
        return;
    }
    if (c.tray) return;
    if (c.tasks && window_focus_tasks()) return; // the task manager open somewhere: that one, as Ctrl+Shift+Esc
    WindowSpec s;
    s.path = c.path;
    s.tab = c.tasks ? TAB_TASKS : TAB_FILES;
    window_open(s);
}

// --- the windows saved for an update restart --------------------------------------------------------

void save_session() {
    std::wstring out;
    for (const WindowSpec &s : window_specs()) {
        wchar_t line[128];
        swprintf(line, 128, L"%d\t%ld\t%ld\t%ld\t%ld\t%d\t", s.tab, s.rc.left, s.rc.top, s.rc.right, s.rc.bottom, s.maximized ? 1 : 0);
        out += line + s.path + L"\n";
    }
    std::string bytes = utf8(out);
    HANDLE f = CreateFileW(session_path().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, NULL);
    CloseHandle(f);
}

// true if there was a session (even with no windows: the resident alone)
bool restore_session() {
    HANDLE f = CreateFileW(session_path().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got) bytes.append(buf, got);
    CloseHandle(f);
    DeleteFileW(session_path().c_str());
    std::wstring text = wide(bytes);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        WindowSpec s;
        int max = 0, used = 0;
        if (swscanf(line.c_str(), L"%d\t%ld\t%ld\t%ld\t%ld\t%d\t%n", &s.tab, &s.rc.left, &s.rc.top, &s.rc.right, &s.rc.bottom, &max, &used) < 6 || !used) continue;
        s.maximized = max != 0;
        s.path = line.substr(used);
        window_open(s);
    }
    return true;
}

// --- Start with Windows -------------------------------------------------------------------------

bool startup_on() {
    wchar_t v[MAX_PATH * 2];
    DWORD size = sizeof(v), type = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, run_value().c_str(), RRF_RT_REG_SZ, &type, v, &size) == ERROR_SUCCESS;
}

void set_startup(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + exe_path() + L"\" --tray";
        if (!instance().empty()) cmd += L" --instance " + instance();
        RegSetValueExW(k, run_value().c_str(), 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, run_value().c_str());
    }
    RegCloseKey(k);
}

// --- the tray icon --------------------------------------------------------------------------------

UINT cursor_dpi() {
    POINT pt;
    GetCursorPos(&pt);
    typedef HRESULT(WINAPI * PDpi)(HMONITOR, int, UINT *, UINT *);
    static HMODULE shcore = LoadLibraryExW(L"shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static PDpi dpi_for = shcore ? (PDpi)(void *)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
    UINT x = 0, y = 0;
    if (dpi_for && SUCCEEDED(dpi_for(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) return x;
    return 96;
}

std::wstring tray_tip() {
    std::wstring tip = L"explorer-lite";
    if (g_upd.state == UP_DOWNLOADING && g_upd.size > 0)
        tip += L"\n" + Tf(L"Downloading {0}… {1}%", { wide(g_upd.version), std::to_wstring((int)(g_upd.done * 100 / g_upd.size)) });
    else if (g_upd.state == UP_READY) tip += L"\n" + Tf(L"Version {0} is ready to install", { wide(g_upd.version) });
    return tip;
}

void tray_set(DWORD msg) {
    NOTIFYICONDATAW nd = { sizeof(nd) };
    nd.hWnd = g_host;
    nd.uID = kTrayId;
    nd.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nd.uCallbackMessage = WM_APP_TRAY;
    UINT dpi = cursor_dpi();
    int px = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    nd.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
    wcsncpy(nd.szTip, tray_tip().c_str(), ARRAYSIZE(nd.szTip) - 1);
    if (Shell_NotifyIconW(msg, &nd) && msg == NIM_ADD) {
        g_tray_added = true;
        nd.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nd);
    }
    if (nd.hIcon) DestroyIcon(nd.hIcon);
}

void tray_remove() {
    if (!g_tray_added) return;
    NOTIFYICONDATAW nd = { sizeof(nd) };
    nd.hWnd = g_host;
    nd.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nd);
    g_tray_added = false;
}

void theme_menus();

enum TrayCmd { TC_THEME = 90, TC_VERSION = 1, TC_OPEN, TC_TASKS, TC_STARTUP, TC_KEYS, TC_FOLDERS, TC_AUTO, TC_UPDATE, TC_QUIT, TC_LANG = 100 };

void tray_menu() {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    MENUINFO mi = { sizeof(mi) };
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_CHECKORBMP; // icons in the check column, without an empty one beside them
    SetMenuInfo(m, &mi);
    std::vector<HBITMAP> bmps;
    int px = MulDiv(16, (int)cursor_dpi(), 96);
    auto add = [&](UINT id, const wchar_t *glyph, const std::wstring &label, bool disabled) {
        HBITMAP b = glyph_bitmap(glyph, px, disabled ? kDim : kText);
        if (b) bmps.push_back(b);
        std::wstring t = label;
        MENUITEMINFOW it = { sizeof(it) };
        it.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_BITMAP | MIIM_FTYPE;
        it.fType = MFT_STRING;
        it.fState = disabled ? MFS_DISABLED : MFS_ENABLED;
        it.wID = id;
        it.dwTypeData = &t[0];
        it.hbmpItem = b;
        InsertMenuItemW(m, (UINT)GetMenuItemCount(m), TRUE, &it);
    };
    add(TC_VERSION, L"\xE946", Tf(L"explorer-lite {0}", { wide(EXPLORER_LITE_VERSION) }), true);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add(TC_OPEN, L"\xE8B7", T(L"Open a window"), false);
    add(TC_TASKS, L"\xE9D9", T(L"Task manager"), false);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    // toggles: a ticked or empty box as the icon (with MNS_CHECKORBMP a check mark would replace the icon)
    add(TC_STARTUP, startup_on() ? L"\xE73A" : L"\xE739", T(L"Start with Windows"), false);
    add(TC_KEYS, keys_on() ? L"\xE73A" : L"\xE739", T(L"Win+E and Ctrl+Shift+Esc open explorer-lite"), false);
    add(TC_FOLDERS, folders_on() ? L"\xE73A" : L"\xE739", T(L"Open folders with explorer-lite"), false);
    add(TC_AUTO, update_auto() ? L"\xE73A" : L"\xE739", T(L"Update automatically"), false);
    switch (g_upd.state) {
    case UP_CHECKING: add(TC_UPDATE, L"\xE895", T(L"Checking for updates…"), true); break;
    case UP_DOWNLOADING:
        add(TC_UPDATE, L"\xE896", Tf(L"Downloading {0}… {1}%", { wide(g_upd.version), std::to_wstring(g_upd.size > 0 ? (int)(g_upd.done * 100 / g_upd.size) : 0) }),
            true);
        break;
    case UP_AVAILABLE: add(TC_UPDATE, L"\xE896", Tf(L"Install version {0}", { wide(g_upd.version) }), false); break;
    case UP_READY: add(TC_UPDATE, L"\xE777", Tf(L"Restart to update to {0}", { wide(g_upd.version) }), false); break;
    case UP_LATEST: add(TC_UPDATE, L"\xE73E", T(L"explorer-lite is up to date"), false); break;
    case UP_FAILED: add(TC_UPDATE, L"\xE7BA", Tf(L"Update failed: {0}", { g_upd.error }), false); break;
    default: add(TC_UPDATE, L"\xE895", T(L"Check for updates"), false);
    }
    // Language: a submenu with each one, the current one ticked
    HMENU langs = CreatePopupMenu();
    SetMenuInfo(langs, &mi);
    int nl = 0;
    const UiLanguage *ls = ui_languages(&nl);
    for (int i = 0; i < nl; i++) {
        HBITMAP b = glyph_bitmap(!strcmp(ls[i].code, ui_lang()) ? L"\xE73E" : L" ", px, kText);
        if (b) bmps.push_back(b);
        std::wstring t = ls[i].name;
        MENUITEMINFOW it = { sizeof(it) };
        it.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
        it.fType = MFT_STRING;
        it.wID = TC_LANG + i;
        it.dwTypeData = &t[0];
        it.hbmpItem = b;
        InsertMenuItemW(langs, (UINT)i, TRUE, &it);
    }
    {
        HBITMAP b = glyph_bitmap(L"\xF2B7", px, kText);
        if (b) bmps.push_back(b);
        std::wstring t = T(L"Language");
        MENUITEMINFOW it = { sizeof(it) };
        it.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
        it.fType = MFT_STRING;
        it.hSubMenu = langs;
        it.dwTypeData = &t[0];
        it.hbmpItem = b;
        InsertMenuItemW(m, (UINT)GetMenuItemCount(m), TRUE, &it);
    }
    // Theme: dark, light or as Windows has it, the current one ticked
    HMENU themes = CreatePopupMenu();
    SetMenuInfo(themes, &mi);
    const char *theme_ids[3] = { "dark", "light", "system" };
    const wchar_t *theme_names[3] = { L"Dark", L"Light", L"Same as Windows" }; // translate
    for (int i = 0; i < 3; i++) {
        HBITMAP b = glyph_bitmap(theme_setting() == theme_ids[i] ? L"\xE73E" : L" ", px, kText);
        if (b) bmps.push_back(b);
        std::wstring t = T(theme_names[i]);
        MENUITEMINFOW it = { sizeof(it) };
        it.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
        it.fType = MFT_STRING;
        it.wID = TC_THEME + i;
        it.dwTypeData = &t[0];
        it.hbmpItem = b;
        InsertMenuItemW(themes, (UINT)i, TRUE, &it);
    }
    {
        HBITMAP b = glyph_bitmap(L"\xE790", px, kText);
        if (b) bmps.push_back(b);
        std::wstring t = T(L"Theme");
        MENUITEMINFOW it = { sizeof(it) };
        it.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
        it.fType = MFT_STRING;
        it.hSubMenu = themes;
        it.dwTypeData = &t[0];
        it.hbmpItem = b;
        InsertMenuItemW(m, (UINT)GetMenuItemCount(m), TRUE, &it);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    add(TC_QUIT, L"\xE7E8", T(L"Quit"), false);
    SetMenuDefaultItem(m, TC_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_host); // without it the menu does not close when clicking elsewhere
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_host, NULL);
    PostMessageW(g_host, WM_NULL, 0, 0); // KB135788
    DestroyMenu(m);                      // menus do not own hbmpItem bitmaps
    for (HBITMAP b : bmps) DeleteObject(b);

    switch (cmd) {
    case TC_OPEN: window_open(WindowSpec()); break;
    case TC_TASKS: {
        WindowSpec s;
        s.tab = TAB_TASKS;
        window_open(s);
        break;
    }
    case TC_STARTUP: set_startup(!startup_on()); break;
    case TC_KEYS: keys_set(!keys_on()); break;
    case TC_FOLDERS: folders_set(!folders_on()); break;
    case TC_AUTO:
        update_set_auto(!update_auto());
        if (update_auto() && g_upd.state == UP_AVAILABLE) update_download();
        break;
    case TC_UPDATE:
        if (g_upd.state == UP_READY) app_install_update();
        else if (g_upd.state == UP_AVAILABLE || (g_upd.state == UP_FAILED && !g_upd.url.empty())) update_download();
        else update_check(true);
        break;
    case TC_QUIT: app_quit(); break;
    case TC_THEME:
    case TC_THEME + 1:
    case TC_THEME + 2: {
        const char *ids[3] = { "dark", "light", "system" };
        theme_set(ids[cmd - TC_THEME]);
        if (theme_apply()) {
            theme_menus();
            window_update_changed();
            taskbar_settings_theme();
        }
        break;
    }
    default:
        if (cmd >= TC_LANG && (int)cmd < TC_LANG + nl) {
            set_ui_lang(ls[cmd - TC_LANG].code);
            window_update_changed();
            taskbar_settings_theme(); // repainted in the new language
        }
    }
}

// --- updates --------------------------------------------------------------------------------------

void on_update() {
    if (!update_pump()) return;
    if (g_upd.state == UP_AVAILABLE && update_auto()) update_download();
    // ready: right away if no window is open (nothing to interrupt); otherwise the windows offer «Restart»
    // and it installs itself when the last one closes (window.cpp calls app_install_update)
    else if (g_upd.state == UP_READY && update_auto() && window_count() == 0) app_install_update();
    if (g_tray_added) tray_set(NIM_MODIFY);
    window_update_changed();
}

LRESULT CALLBACK host_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msg_taskbar_created && g_msg_taskbar_created) {
        // Explorer (or whichever shell) restarted: the icon has to be added again
        g_tray_added = false;
        tray_set(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (window_count()) window_focus_last();
            else window_open(WindowSpec());
            return 0;
        case WM_CONTEXTMENU: tray_menu(); return 0;
        }
        return 0;
    case WM_APP_UPDATE: on_update(); return 0;
    case WM_APP_KEYS: { // Win+E: a new window, as explorer opens one; Ctrl+Shift+Esc: the task manager
        if (!(wp == KEY_TASKS && window_focus_tasks())) {
            WindowSpec s;
            if (wp == KEY_TASKS) s.tab = TAB_TASKS;
            window_open(s);
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        log_line(std::string(wp == KEY_TASKS ? "keys: Ctrl+Shift+Esc" : "keys: Win+E") + (pid == GetCurrentProcessId() ? ", in front" : ", NOT in front"));
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerUpdate) {
            SetTimer(h, kTimerUpdate, kCheckEveryMs, NULL);
            update_check(false);
        }
        return 0;
    case WM_COPYDATA: {
        auto *cd = (COPYDATASTRUCT *)lp;
        if (!cd || cd->dwData != kCopyCommand || cd->cbData > 64 * 1024 || cd->cbData % sizeof(wchar_t)) return FALSE;
        std::wstring s((const wchar_t *)cd->lpData, cd->cbData / sizeof(wchar_t));
        Command c;
        size_t tab = s.find(L'\t');
        std::wstring verb = s.substr(0, tab);
        if (tab != std::wstring::npos) c.path = s.substr(tab + 1);
        if (verb == L"cmd") c.cmd = c.path, c.path.clear();
        c.quit = verb == L"quit";
        c.tasks = verb == L"tasks";
        c.taskbar = verb == L"taskbar";
        c.tray = verb == L"tray";
        // after returning: the sender is waiting in SendMessage, and quitting or opening takes a while
        auto *later = new Command(c);
        PostMessageW(h, WM_APP + 100, 0, (LPARAM)later);
        return TRUE;
    }
    case WM_APP + 100: {
        auto *c = (Command *)lp;
        run(*c);
        delete c;
        return 0;
    }
    case WM_SETTINGCHANGE: // Windows switched between light and dark apps: followed if the theme is "system"
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet") && theme_apply()) {
            theme_menus();
            window_update_changed();
            taskbar_settings_theme();
        }
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) save_session();
        return 0;
    case WM_DESTROY: tray_remove(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Menus in the theme's colours (the tray's, and the shell's context menus): uxtheme's SetPreferredAppMode, by
// ordinal (undocumented, stable since 1903).
void theme_menus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int(WINAPI * PMode)(int);
    typedef void(WINAPI * PFlush)();
    auto set_mode = (PMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (PFlush)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(theme_light() ? 3 : 2); // ForceLight, ForceDark
    if (flush) flush();
}

int send_to_running(const Command &c) { return app_send(host_class(), serialize(c), c.quit, 5000); }

// the probe types the text one key at a time, as a window would ask: each answer timed with what the window does to it
struct Probe {
    std::wstring path, text;
    size_t typed = 0;
    int pass = 1;    // typed twice: the first time the file types are new to the process (a window's are not, after a while)
    double t0 = 0, worst = 0;
} g_probe;

bool probe_next(HWND h) {
    g_probe.typed++;
    g_probe.t0 = now_ms();
    return folder_search(h, (DWORD)(g_probe.pass * 1000 + g_probe.typed), g_probe.path, g_probe.text.substr(0, g_probe.typed));
}

LRESULT CALLBACK probe_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        auto *cds = (COPYDATASTRUCT *)lp;
        if (cds->dwData != g_probe.pass * 1000 + g_probe.typed) return FALSE;
        double got = now_ms();
        std::vector<Item> items;
        folder_search_items(cds, items);
        folder_sort(items, 0, false);
        double ms = now_ms() - g_probe.t0, ours = now_ms() - got;
        g_probe.worst = std::max(g_probe.worst, ms);
        std::string line = "everything: \"" + utf8(g_probe.text.substr(0, g_probe.typed)) + "\" " + std::to_string(items.size()) + " results in " +
                           std::to_string((int)(ms + 0.5)) + " ms (" + std::to_string((int)(ours + 0.5)) + " ours)";
        if (g_probe.typed == g_probe.text.size()) {
            for (size_t i = 0; i < items.size() && i < 5; i++) line += "; " + utf8(items[i].target) + (items[i].dir ? " (folder)" : " " + utf8(items[i].size));
            log_line(line);
            log_line("everything: typing \"" + utf8(g_probe.text) + "\"" + (g_probe.pass == 1 ? "" : " again") + ", the slowest answer " +
                     std::to_string((int)(g_probe.worst + 0.5)) + " ms");
            if (g_probe.pass++ == 1) {
                g_probe.typed = 0;
                g_probe.worst = 0;
                PostMessageW(h, WM_APP, 0, 0);
            } else PostQuitMessage(0);
        } else {
            log_line(line);
            PostMessageW(h, WM_APP, 0, 0); // the next key once Everything has its answer back (it waits on this one)
        }
        return TRUE;
    }
    if (msg == WM_APP) {
        if (!probe_next(h)) PostQuitMessage(2);
        return 0;
    }
    if (msg == WM_TIMER) {
        log_line("everything: no answer");
        PostQuitMessage(1);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --everything: the search code of the windows, from a message-only window (nothing on screen)
int everything_probe(const Command &c) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = probe_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"ExplorerLiteProbe";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);
    g_probe.path = c.path;
    g_probe.text = c.text;
    if (!h || c.text.empty() || !probe_next(h)) return 2;
    SetTimer(h, 1, 5000, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return (int)m.wParam;
}

} // namespace

// The running process whose hidden window is of class `cls` gets `line` (WM_COPYDATA). For a quit, waits until it
// exits: 10 if it did, 11 if it is still there (the installer kills it), 0 if none was running.
int app_send(const std::wstring &cls, const std::wstring &line, bool quit, int find_ms) {
    HWND other = NULL;
    for (int waited = 0; !other; waited += 100) { // it may still be starting
        other = FindWindowW(cls.c_str(), NULL);
        if (other || waited >= find_ms) break;
        Sleep(100);
    }
    if (!other) return quit ? 0 : 1;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    AllowSetForegroundWindow(pid); // the window it opens comes to the front
    COPYDATASTRUCT cd = { kCopyCommand, (DWORD)(line.size() * sizeof(wchar_t)), (void *)line.c_str() };
    DWORD_PTR res = 0;
    SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 10000, &res);
    int code = 0;
    if (quit) {
        code = 10;
        if (proc && WaitForSingleObject(proc, 15000) != WAIT_OBJECT_0) code = 11;
    }
    if (proc) CloseHandle(proc);
    return code;
}

void app_install_update() {
    if (g_upd.state != UP_READY) return;
    save_session();
    if (!update_install()) {
        DeleteFileW(session_path().c_str());
        log_line("update: the installer could not be started");
        return;
    }
    // the installer reopens us with --updated (it was launched from data_dir()\tmp)
    g_quitting = true;
    window_close_all();
    taskbar_settings_close();
    tray_remove();
    DestroyWindow(g_host);
    PostQuitMessage(0);
}

void app_quit() {
    if (g_quitting) return;
    g_quitting = true;
    save_session(); // the installer asks with --quit, and reopens with --updated
    window_close_all();
    taskbar_settings_close();
    tray_remove();
    DestroyWindow(g_host);
    PostQuitMessage(0);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Command cmd = parse(argc, argv);
    LocalFree(argv);
    if (cmd.everything) return everything_probe(cmd);
    if (cmd.apps) {
        start_list_probe();
        return 0;
    }
    if (cmd.unregister) {
        set_startup(false);
        folders_set(false);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (cmd.shell) return shell_main(cmd.cmd, cmd.quit, cmd.restarted);
    // the installer's --quit: the shell goes too (and comes back with --updated), so nothing keeps the exe in use
    int shell_quit = cmd.quit ? shell_quit_running() : 0;

    HANDLE mutex = CreateMutexW(NULL, TRUE, (L"Local\\explorer-lite" + instance_suffix()).c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        int code = send_to_running(cmd);
        if (mutex) CloseHandle(mutex);
        return code == 0 && shell_quit ? shell_quit : code;
    }
    if (cmd.quit) return shell_quit; // no resident running (10 or 11 if the shell was)
    if (!cmd.cmd.empty()) return 0;  // nothing running

    OleInitialize(NULL); // drag and drop, the shell's context menus and dialogs
    theme_apply();
    theme_menus();
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = host_proc;
    wc.hInstance = inst;
    std::wstring cls = host_class();
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    // a hidden top-level window, not a message-only one: those miss the TaskbarCreated broadcast
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, cls.c_str(), L"explorer-lite", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_host) return 1;
    g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_host, g_msg_taskbar_created, MSGFLT_ALLOW, NULL);
    tray_set(NIM_ADD);
    update_init(g_host, WM_APP_UPDATE);
    SetTimer(g_host, kTimerUpdate, kFirstCheckMs, NULL);
    keys_start(g_host, WM_APP_KEYS);
    log_line("start " EXPLORER_LITE_VERSION " in " + std::to_string((int)since_process_start_ms()) + " ms");

    if (cmd.updated) {
        shell_relaunch_after_update();
        if (!restore_session() && !cmd.tray) window_open(WindowSpec());
    } else {
        run(cmd);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    keys_stop();
    OleUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
