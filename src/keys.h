// keys.h: what makes explorer-lite the one Windows opens. Win+E and Ctrl+Shift+Esc, taken with a low-level keyboard
// hook on a thread of its own (RegisterHotKey cannot take them while explorer holds them), and «open folders with
// explorer-lite»: the default open verb of folders and drives, for this user only (HKCU\Software\Classes).
#pragma once
#include "util.h"

enum { KEY_EXPLORER = 1, KEY_TASKS = 2 }; // wParam of the message

// The hook, if the setting is on: `msg` reaches `host` when one of the shortcuts is pressed.
void keys_start(HWND host, UINT msg);
void keys_stop();
bool keys_on();          // conf "shortcuts": on by default (off in test instances, which must not take the user's keys)
void keys_set(bool on);  // saves it and starts or stops the hook

bool folders_on();
void folders_set(bool on); // off also on --unregister
