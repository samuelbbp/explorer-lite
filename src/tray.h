// tray.h: the notification area (tray.cpp). As the session's shell explorer-lite owns the window of class
// Shell_TrayWnd that Shell_NotifyIcon and SHAppBarMessage talk to (trayproto.h): the apps' icons go on the main
// monitor's bar, beside the clock, and the ones the user keeps out of sight in an overflow above a chevron. Balloons
// show above the bar, never over a fullscreen app. Alongside explorer the tray is explorer's and none of this runs; a
// test instance gets a window of its own class that only its own test (`traytest`) talks to.
#pragma once
#include "surface.h"
#include <string>

enum TrayKind { TRAY_NONE, TRAY_TEST, TRAY_SHELL };

void tray_start(TrayKind kind); // the window (and, as the shell, TaskbarCreated: running apps add their icons again)
void tray_stop();
float tray_width();             // DIPs the main bar keeps for it; 0 with no icons
void tray_layout(Surface &s, D2D1_RECT_F area); // where it goes on the main bar (its DIPs)
void tray_draw(Surface &s);
void tray_detach();             // the bars are about to be destroyed (monitors changed): nothing may point at them
// The main bar's mouse over the tray area (DIPs); WM_MOUSELEAVE as well. True if the tray took it.
bool tray_mouse(UINT msg, float x, float y);
void tray_window_gone(HWND h);  // a window was destroyed: its icons go
void tray_sweep();              // icons whose window is gone (some never say so), once a minute
void tray_place(const RECT &bar); // the main bar moved: Shell_TrayWnd stays on it (apps look for the taskbar there)
void tray_fullscreen_changed(); // balloons wait while a fullscreen app is in front
void tray_overflow(bool keyboard); // the overflow (Win+B: every icon, for the keyboard)
// The appbars other programs registered (only as the session's shell): what is left of a monitor's work area.
RECT tray_work_area(HMONITOR mon, RECT wa);
bool tray_command(const std::wstring &verb, const std::wstring &arg); // test commands; false if not one of them
