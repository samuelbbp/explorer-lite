# explorer-lite

One small native program in place of what Windows keeps running all the time: the file explorer, the task manager, and later the taskbar, tray, Start, notifications and desktop.

C++ Win32 + Direct2D/DirectWrite, built with `zig c++`. Per-user install, no admin, updates itself.

## Features

- Resident in the tray with almost no memory; windows open on demand
- File explorer with the task manager inside (in progress)
- Taskbar on every monitor (open windows grouped by app, pinned apps, clock) and Start with app search and power menu (`--shell`)
- Taskbar settings from its right-click menu: left or centered, Start kept at the far left, a custom Start icon, small size, titles or icons only, seconds on the clock, show-desktop corner, one or every display
- As the session's shell: the notification area (apps' icons, tooltips, balloons, overflow for hidden icons, appbars), Alt+Tab and Win+Tab with live thumbnails, and Win+D, Win+R, Win+X, Win+B and Win+1..9
- Start finds files and folders as you type through [Everything](https://www.voidtools.com/), below the apps
- English and Spanish; dark theme; per-monitor DPI
