// thumbs.h: thumbnails for the Large icons view, made by Windows' own thumbnail handlers (IShellItemImageFactory,
// the same cache explorer uses) on a thread of their own. Only what is on screen is asked for; the newest request
// goes first, so scrolling fast never waits behind rows already gone.
#pragma once
#include "util.h"
#include <string>
#include <vector>

struct Thumb {
    uint64_t gen;              // the listing it was asked for
    std::wstring target;
    int w = 0, h = 0;          // pixels; 0 = none could be made (the icon stays)
    std::vector<uint32_t> px;  // premultiplied BGRA, top-down
};

// Asks for target's thumbnail at size pixels; WM_APP_THUMB arrives with lParam = a Thumb (the window owns it).
// Requests of a gen that folder_forget dropped are skipped.
void thumbs_request(HWND wnd, uint64_t gen, const std::wstring &target, int size);
void thumbs_cancel(uint64_t gen); // the folder was left: its pending requests go
