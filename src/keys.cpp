// keys.cpp: see keys.h.
#include "keys.h"
#include <shlobj.h>
#include <string>
#include "conf.h"
#include "idioma.h"

namespace {

HWND g_host;
UINT g_msg;
DWORD g_thread_id;
HANDLE g_thread;
DWORD g_swallow_up; // the key whose press was taken: its release is taken too
bool g_shortcuts;   // Win+E and Ctrl+Shift+Esc (the resident)
bool g_win;         // the Win key alone (the shell)
bool g_shell_keys;  // Win+ shortcuts and Alt+Tab (the shell)
bool g_switching;   // Alt+Tab's switcher is open: Alt is still held
DWORD g_shell_up;   // a key the shell took: its release is taken too
bool g_win_down, g_win_combo; // Win is held; another key went down meanwhile (also an injected one: the resident's
                              // hook swallows Win+E's E and sends 0xE8, so this hook may never see the E)

bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// A key nobody uses, pressed and released: Windows does not open Start when Win is let go after another key, and
// the process that sent the last input may bring its window to the front.
void dummy_key() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8; // unassigned
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

LRESULT CALLBACK hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto *k = (KBDLLHOOKSTRUCT *)lp;
        bool press = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN, release = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        bool win_key = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
        if (g_shell_keys && !(k->flags & LLKHF_INJECTED)) {
            DWORD vk = k->vkCode;
            if (release && vk == g_shell_up) {
                g_shell_up = 0;
                return 1;
            }
            bool alt_key = vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
            if (release && alt_key && g_switching) { // Alt let go: the window chosen comes to the front
                g_switching = false;
                PostMessageW(g_host, g_msg, KEY_SWITCH_DONE, 0);
                return CallNextHookEx(NULL, code, wp, lp);
            }
            if (press) {
                bool win = down(VK_LWIN) || down(VK_RWIN), ctrl = down(VK_CONTROL), shift = down(VK_SHIFT), alt = (k->flags & LLKHF_ALTDOWN) != 0;
                int what = 0;
                LPARAM arg = 0;
                if (g_switching && vk == VK_ESCAPE) what = KEY_SWITCH_CANCEL;
                else if (g_switching && vk == VK_RETURN) what = KEY_SWITCH_DONE;
                else if (g_switching && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN))
                    what = KEY_SWITCH, arg = vk == VK_LEFT || vk == VK_UP ? -1 : 1;
                else if (vk == VK_TAB && alt && !win && !ctrl) what = KEY_SWITCH, arg = shift ? -1 : 1;
                else if (vk == VK_ESCAPE && ctrl && !alt && !win && !shift) what = KEY_START; // Ctrl+Esc: Start, as Windows' own
                else if (win && !alt && !ctrl && !shift &&
                           (vk == 'D' || vk == 'R' || vk == 'X' || vk == 'B' || vk == VK_TAB || (vk >= '1' && vk <= '9'))) {
                    what = KEY_WIN;
                    arg = (LPARAM)vk;
                }
                if (what && !IsHungAppWindow(g_host)) { // explorer-lite hung: the keys stay Windows'
                    if (what == KEY_WIN) g_win_combo = true; // Win's release is not Start
                    g_switching = what == KEY_SWITCH;
                    g_shell_up = vk;
                    // a key between Alt (or Win) going down and up: no menu bar, no Start; and the last input is ours
                    dummy_key();
                    PostMessageW(g_host, g_msg, what, arg);
                    return 1;
                }
            }
        }
        if (g_win) {
            if (press && win_key && !(k->flags & LLKHF_INJECTED)) {
                if (!g_win_down) g_win_down = true, g_win_combo = false;
            } else if (press && g_win_down) {
                g_win_combo = true;
            } else if (release && win_key && g_win_down && !(k->flags & LLKHF_INJECTED)) {
                g_win_down = false;
                // explorer-lite hung: the key stays Windows'
                if (!g_win_combo && !IsHungAppWindow(g_host)) {
                    // the release is taken and sent again after the unused key: what is sent from here arrives
                    // after the event being decided, so Windows has to see the key before it sees Win go up
                    INPUT in[3] = {};
                    for (INPUT &i : in) i.type = INPUT_KEYBOARD;
                    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[2].ki.wVk = (WORD)k->vkCode;
                    in[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                    SendInput(3, in, sizeof(INPUT));
                    PostMessageW(g_host, g_msg, KEY_START, 0);
                    return 1;
                }
            }
        }
        if (g_shortcuts && !(k->flags & LLKHF_INJECTED)) {
            if (release && k->vkCode == g_swallow_up) {
                g_swallow_up = 0;
                return 1;
            }
            if (press) {
                bool win = down(VK_LWIN) || down(VK_RWIN), ctrl = down(VK_CONTROL), shift = down(VK_SHIFT), alt = down(VK_MENU);
                int what = 0;
                if (k->vkCode == 'E' && win && !ctrl && !shift && !alt) what = KEY_EXPLORER;
                else if (k->vkCode == VK_ESCAPE && ctrl && shift && !win && !alt) what = KEY_TASKS;
                // explorer-lite hung: the keys stay Windows' (its Task Manager has to be reachable)
                if (what && !IsHungAppWindow(g_host)) {
                    g_swallow_up = k->vkCode;
                    dummy_key();
                    PostMessageW(g_host, g_msg, what, 0);
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// the hook lives on its own thread: Windows waits for it on every key, so it never queues behind a window's paint
DWORD WINAPI hook_thread(void *) {
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, hook, GetModuleHandleW(NULL), 0);
    log_line(h ? (g_shell_keys ? "keys: the Win key, its shortcuts and Alt+Tab taken" : g_win ? "keys: the Win key taken (Start)" : "keys: Win+E and Ctrl+Shift+Esc taken") : "keys: the hook failed (" + std::to_string(GetLastError()) + ")");
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    if (h) UnhookWindowsHookEx(h);
    log_line("keys: released");
    return 0;
}

const wchar_t *kVerb = L"explorer-lite";
const wchar_t *kTypes[] = { L"Software\\Classes\\Directory\\shell", L"Software\\Classes\\Drive\\shell" };

std::wstring reg_default(const wchar_t *key) {
    wchar_t v[256] = L"";
    DWORD size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, key, NULL, RRF_RT_REG_SZ, NULL, v, &size) != ERROR_SUCCESS) return L"";
    return v;
}

bool reg_set(const std::wstring &key, const wchar_t *name, const std::wstring &value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

} // namespace

void keys_start(HWND host, UINT msg) {
    g_host = host;
    g_msg = msg;
    if (g_thread || !keys_on()) return;
    g_shortcuts = true;
    g_thread = CreateThread(NULL, 0, hook_thread, NULL, 0, &g_thread_id);
}

void keys_start_shell(HWND host, UINT msg, bool win_alone, bool shortcuts) {
    g_host = host;
    g_msg = msg;
    if (g_thread || (!win_alone && !shortcuts)) return;
    g_win = win_alone;
    g_shell_keys = shortcuts;
    g_thread = CreateThread(NULL, 0, hook_thread, NULL, 0, &g_thread_id);
}

void keys_stop() {
    if (!g_thread) return;
    PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
}

bool keys_on() {
    std::string v = conf_get("shortcuts");
    return v.empty() ? instance().empty() : v == "1";
}

void keys_set(bool on) {
    conf_set("shortcuts", on ? "1" : "0");
    if (on) keys_start(g_host, g_msg);
    else keys_stop();
}

bool folders_on() { return reg_default(kTypes[0]) == kVerb; }

void folders_set(bool on) {
    for (const wchar_t *type : kTypes) {
        std::wstring verb = std::wstring(type) + L"\\" + kVerb;
        if (on) {
            std::wstring cmd = L"\"" + exe_path() + L"\" \"%1\"";
            if (!instance().empty()) cmd += L" --instance " + instance();
            reg_set(verb, L"MUIVerb", T(L"Open in explorer-lite"));
            reg_set(verb, L"Icon", L"\"" + exe_path() + L"\",0");
            reg_set(verb + L"\\command", NULL, cmd);
            reg_set(type, NULL, kVerb); // the default verb: double-clicking a folder anywhere
        } else {
            if (reg_default(type) == kVerb) RegDeleteKeyValueW(HKEY_CURRENT_USER, type, NULL);
            RegDeleteTreeW(HKEY_CURRENT_USER, verb.c_str());
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}
