// shell.h: explorer-lite as the shell (`--shell`): a taskbar per monitor (shell.cpp) and Start (start.cpp), in a
// process of its own. Files, the task manager and the tray icon stay in the resident (main.cpp), so a hang there
// never takes the bar down, and the shell only asks for them by starting the exe.
#pragma once
#include "util.h"
#include <string>
#include <vector>

struct Thumb;

// main.cpp
int shell_main(const std::wstring &cmd, bool quit, DWORD restarted); // the process (or a command for the running one)
int shell_quit_running();           // the installer's --quit: 10 if a shell was running and quit (11 if it is stuck), 0 if none
void shell_relaunch_after_update(); // --updated: the shell comes back if the update closed it

// between shell.cpp and start.cpp
const uint64_t kGenBar = 1, kGenStart = 2; // whose icon a Thumb is (thumbs_request's gen)
HWND shell_host();
// The user's own shell (no --instance): only it may take the Win key, set the work area, register appbars, run what
// starts with Windows or lock, sleep, restart and shut down. A test instance draws and logs instead.
bool shell_real();
// Opens a program, file, folder or app: a path, a command's arguments, or shell:AppsFolder\<id> (Store apps through
// IApplicationActivationManager). Our own exe gets --instance, so a test never reaches the user's explorer-lite.
void shell_launch(const std::wstring &target, const std::wstring &args = L"");
HMONITOR shell_primary();
UINT shell_monitor_dpi(HMONITOR mon);
RECT shell_bar_rect(HMONITOR mon); // pixels: where the monitor's taskbar is (or would be)

std::wstring shell_process_path(DWORD pid);
bool shell_fullscreen(HMONITOR mon); // a fullscreen app (a game, a video) is in front there
void shell_tray_changed();          // tray icons came, went or moved: the bars' layout again
void shell_work_area_update();      // an appbar moved: each monitor's work area again (the session's shell only)
struct ShellWindow {
    HWND hwnd;
    std::wstring title;
    const Thumb *icon; // the bar's (may be null)
};
std::vector<ShellWindow> shell_windows_by_use(); // the taskbar's windows, the one in front first
void shell_activate(HWND h);       // restored if minimized, to the front

// A menu with an icon on each row (as the tray's), opened at the cursor above it (or above `at`).
struct ShellMenu {
    HMENU m;
    std::vector<HBITMAP> bmps;
    int px;
    explicit ShellMenu(UINT dpi);
    ~ShellMenu();
    void add(UINT id, const wchar_t *glyph, const std::wstring &label);
    void sep();
    UINT show(HWND owner, const POINT *at = nullptr); // the id chosen, 0 if none
};

// start.cpp
void start_toggle(HMONITOR mon);
void start_open(HMONITOR mon, bool sticky); // sticky: stays open without the focus (tests on their own desktop)
void start_close();
bool start_is_open();
bool start_shot(const std::wstring &png);
void start_command(const std::wstring &verb, const std::wstring &arg); // search <text>, key <vk>
void start_on_icon(Thumb *t); // an icon Start asked for (takes it)
void start_theme_changed();
void start_prepare();            // the list of apps made in the background (the saved one loaded first)
void start_on_apps(void *list);  // WM_APP_APPS reached the shell's window: the new list (takes it)
void start_list_probe(); // --apps: the list Start would show, into the log (tests: no window needed)
void start_power(int what); // 1 lock, 2 sleep, 3 sign out, 4 restart, 5 shut down (a test instance only logs it)

// switcher.cpp: Alt+Tab (the keyboard hook drives it) and Win+Tab
void switcher_step(int step);
void switcher_done();
void switcher_cancel();
void taskview_toggle();
bool switcher_is_open();
void switcher_command(const std::wstring &verb, const std::wstring &arg); // switcher, taskview, switchstep, switchershot...
