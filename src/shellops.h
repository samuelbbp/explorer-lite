// shellops.h: what Windows' own shell does with files, used as is so behaviour matches it: the context menu (with
// every extension's entries), copy / move / delete / rename through IFileOperation (its progress dialog, the recycle
// bin, conflicts), the clipboard, drag and drop both ways, and change notifications for the open folders.
#pragma once
#include "util.h"
#include <oleidl.h>
#include <string>
#include <vector>

// The context menu of `items` (full paths, all in one folder) or, with none, of the folder's background (New,
// Paste, ...). Returns the verb chosen if explorer-lite has to do it itself ("rename"), "" otherwise.
std::string shell_menu(HWND wnd, const std::wstring &folder, const std::vector<std::wstring> &items, POINT screen);
// While a shell menu is open its owner window forwards these messages (submenus drawn by the extensions).
bool shell_menu_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *res);

void shell_clipboard(HWND wnd, const std::vector<std::wstring> &items, bool cut);
bool shell_can_paste();
void shell_paste(HWND wnd, const std::wstring &folder);

// On a thread of their own (Windows' progress dialog, owned by wnd); the folder refreshes through its notifications.
void shell_delete(HWND wnd, const std::vector<std::wstring> &items, bool permanent, bool confirm = true);
void shell_rename(HWND wnd, const std::wstring &item, const std::wstring &name);
void shell_transfer(HWND wnd, IDataObject *data, const std::wstring &to, bool move); // AddRef'd here
void shell_copy_to(HWND wnd, const std::vector<std::wstring> &items, const std::wstring &to, bool move); // no clipboard
std::wstring shell_new_folder(const std::wstring &folder, const std::wstring &base); // "base", "base (2)"...; returns its name
// Windows' Properties of an item.
void shell_properties(HWND wnd, const std::wstring &item);

// Dragging the selection out (to another folder, the desktop, another app).
void shell_drag(HWND wnd, const std::vector<std::wstring> &items);

// Dropping into a window: the callbacks say which folder is under the point (screen coordinates; "" = none) and
// get told when the hover changes, to highlight it.
struct DropHandler {
    virtual std::wstring drop_folder(POINT screen) = 0;
    virtual void drop_hover(POINT screen, bool inside) = 0;
};
void shell_register_drop(HWND wnd, DropHandler *h);
void shell_revoke_drop(HWND wnd);

// SHChangeNotifyRegister on `folder`: `msg` reaches wnd when something in it changes. Returns the id (0 = none).
ULONG shell_watch(HWND wnd, UINT msg, const std::wstring &folder);
void shell_unwatch(ULONG id);
