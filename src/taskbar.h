// taskbar.h: the taskbar's settings (explorer-lite.conf), read by the bars (shell.cpp) and changed in their settings
// window (taskbarset.cpp). The window lives in the resident, opened on demand (--taskbar-settings) and gone when
// closed, so the always-on shell never holds it nor the file dialog; each change is saved and the running shell is
// told to read them again (shell_settings_changed).
#pragma once
#include <string>

struct TaskbarConf {
    bool center = false;      // taskbar_align=center: the buttons in the middle of the bar (else from the left)
    bool start_left = false;  // taskbar_start=left: centered, the Start button stays at the far left
    std::wstring start_icon;  // taskbar_start_icon: a .png or .ico for the Start button ("": the logo)
    bool small = false;       // taskbar_size=small: a lower bar, smaller icons, the clock on one line
    bool labels = true;       // taskbar_labels=0: icons only, without the windows' titles
    bool seconds = false;     // taskbar_seconds=1: the clock with seconds (a repaint a second, only then)
    bool corner = true;       // taskbar_corner=0: no strip at the far right that shows the desktop
    bool all_displays = true; // taskbar_all_displays=0: only the main monitor's bar, with every window
};
TaskbarConf taskbar_conf(); // as saved now (the shell calls conf_reload first: the resident writes it)

// taskbarset.cpp, in the resident
void taskbar_settings_open();   // the window (to the front if it is open)
void taskbar_settings_close();
void taskbar_settings_theme();  // the theme changed: repainted
bool taskbar_settings_command(const std::wstring &verb, const std::wstring &arg); // tests (--cmd); false if not one
