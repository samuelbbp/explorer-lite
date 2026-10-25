// taskfilter.h: which top-level windows get a taskbar button, as Windows decides it. Plain facts in, a yes or no out
// (shell.cpp gathers them; `--shell --cmd filtertest` checks both on real windows of each kind). The rules are
// Windows' own (visible, unowned or WS_EX_APPWINDOW, not WS_EX_TOOLWINDOW, not cloaked) plus three that ManagedShell
// (Cairo, RetroBar) learned the hard way, copied from its ApplicationWindow.CanAddToTaskbar / IsImmersiveShellWindow:
//   - WS_EX_NOACTIVATE windows only with WS_EX_APPWINDOW (on-screen keyboards, overlays);
//   - the "ITaskList_Deleted" window property (ITaskbarList::DeleteTab) hides a window;
//   - UWP frames without WS_EX_WINDOWEDGE (ApplicationFrameWindow, CoreWindow) are the shell's, not an app's.
// https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.WindowsTasks/ApplicationWindow.cs
#pragma once

struct WinFacts {
    bool visible = false;
    bool owned = false;       // GetWindow(GW_OWNER) is not NULL
    bool appwindow = false;   // WS_EX_APPWINDOW
    bool toolwindow = false;  // WS_EX_TOOLWINDOW
    bool noactivate = false;  // WS_EX_NOACTIVATE
    bool cloaked = false;     // DWMWA_CLOAKED: UWP windows not shown, other virtual desktops
    bool deleted = false;     // the ITaskList_Deleted property
    bool immersive = false;   // class ApplicationFrameWindow, Windows.UI.Core.CoreWindow... without WS_EX_WINDOWEDGE
};

inline bool taskbar_shows(const WinFacts &f) {
    if (!f.visible || f.cloaked || f.deleted || f.immersive || f.toolwindow) return false;
    if (f.owned && !f.appwindow) return false;
    if (f.noactivate && !f.appwindow) return false;
    return true;
}
