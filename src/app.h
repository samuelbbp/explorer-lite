// app.h: what the resident (main.cpp) and the windows (window.cpp) share.
#pragma once
#include "util.h"
#include <string>
#include <vector>

extern HINSTANCE g_inst;
extern HWND g_host; // the resident's hidden window: tray icon, commands from other processes, timers

enum {
    WM_APP_TRAY = WM_APP + 1, // the tray icon's callback
    WM_APP_UPDATE,            // update.cpp has news
    WM_APP_LIST,              // a folder listing finished on its thread (window.cpp)
    WM_APP_TASKS,             // a new task manager sample (tasks.cpp)
    WM_APP_ICONS,             // icons of programs and shortcuts in a folder just listed (folder.cpp)
};

enum Tab { TAB_FILES, TAB_TASKS };

// What a window shows and where: also what is saved to reopen it after an update.
struct WindowSpec {
    std::wstring path;  // the folder ("" = Home)
    int tab = TAB_FILES;
    RECT rc = {};       // screen pixels; empty = the last place a window was
    bool maximized = false;
};

void window_open(const WindowSpec &spec);
int window_count();
void window_focus_last();                  // brings the last used window to the front
std::vector<WindowSpec> window_specs();    // the open windows, oldest first
void window_close_all();
void window_update_changed();              // the update state changed: repaint the notices
bool window_shot(const std::wstring &png);  // the last used window into a PNG
void window_command(const std::wstring &verb, const std::wstring &arg); // test commands (--cmd)

// The resident.
void app_install_update();                 // saves the open windows and runs the downloaded installer
void app_quit();                           // closes every window and the tray icon, and exits
