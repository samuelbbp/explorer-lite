// keys.h: what makes explorer-lite the one Windows opens. Win+E and Ctrl+Shift+Esc, taken with a low-level keyboard
// hook on a thread of its own (RegisterHotKey cannot take them while explorer holds them), and «open folders with
// explorer-lite»: the default open verb of folders and drives, for this user only (HKCU\Software\Classes).
#pragma once
#include "util.h"

// wParam of the message; KEY_WIN's lParam is the key pressed with Win, KEY_SWITCH's the step (+1, -1)
enum { KEY_EXPLORER = 1, KEY_TASKS = 2, KEY_START = 3, KEY_WIN = 4, KEY_SWITCH = 5, KEY_SWITCH_DONE = 6, KEY_SWITCH_CANCEL = 7 };

// The hook, if the setting is on: `msg` reaches `host` when one of the shortcuts is pressed.
void keys_start(HWND host, UINT msg);
// The shell's, never in a test instance (shell.cpp decides):
//   win_alone   the Win key pressed and released alone (KEY_START), with a key nobody uses sent before its release so
//               Windows' own Start stays closed;
//   shortcuts   Win+D, R, X, B, Tab and 1..9 (KEY_WIN), Ctrl+Esc (KEY_START), and Alt+Tab: KEY_SWITCH on each Tab (Shift: back; arrows too
//               while it is open), KEY_SWITCH_DONE when Alt is let go or Enter, KEY_SWITCH_CANCEL on Esc. Win+L and
//               everything else stay Windows'.
void keys_start_shell(HWND host, UINT msg, bool win_alone, bool shortcuts);
void keys_stop();
bool keys_on();          // conf "shortcuts": on by default (off in test instances, which must not take the user's keys)
void keys_set(bool on);  // saves it and starts or stops the hook

bool folders_on();
void folders_set(bool on); // off also on --unregister
