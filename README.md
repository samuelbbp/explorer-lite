# explorer-lite

One small native program in place of what Windows keeps running all the time: the file explorer, the task manager, and later the taskbar, tray, Start, notifications and desktop.

C++ Win32 + Direct2D/DirectWrite, built with `zig c++`. Per-user install, no admin, updates itself.

## Features

- Resident in the tray with almost no memory; windows open on demand
- File explorer with the task manager inside (in progress)
- English and Spanish; dark theme; per-monitor DPI
