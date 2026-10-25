// trayproto.h: the notification area's wire protocol, as Shell_NotifyIcon and SHAppBarMessage speak it to the window of
// class Shell_TrayWnd (WM_COPYDATA), read without trusting the sender, and what each message does to the list of icons.
// Plain data in, plain data out: tray.cpp owns the windows, and `--shell --cmd traytest` feeds this crafted buffers.
//
// The layouts are ManagedShell's (Cairo, RetroBar), which matched them against explorer's:
// https://github.com/cairoshell/ManagedShell/blob/master/src/ManagedShell.Interop/NativeMethods.Shell32.cs
//   dwData 0  an appbar message: APPBARMSGDATAV3 (64 bytes), the APPBARDATA in shared memory (SHLockShared)
//   dwData 1  Shell_NotifyIcon: a signature, the NIM_ message and NOTIFYICONDATAW with every handle 32 bits wide
//             (user handles are, in 32- and 64-bit processes alike); old apps send shorter structs (cbSize)
//   dwData 3  Shell_NotifyIconGetRect: which icon, and whether its top-left (1) or its size (2) is wanted (ManagedShell
//             answers the bottom-right corner; Windows adds what comes back to the corner, so it is the size)
// The callbacks are ManagedShell's NotifyIcon.SendMessage / IconMouseUp: before NOTIFYICON_VERSION_4 wParam is the
// icon's uID and lParam the mouse message; from 4 on wParam is the anchor point and lParam MAKELPARAM(message, uID).
// From version 3 a left click also sends NIN_SELECT and a right click WM_CONTEXTMENU (explorer does it for 3 too).
#pragma once
#include "util.h"
#include <cstring>
#include <string>
#include <vector>

const ULONG_PTR kTrayAppBar = 0, kTrayNotify = 1, kTrayIconRect = 3; // COPYDATASTRUCT.dwData
const DWORD kTraySignature = 0x34753423;

// Shell_NotifyIcon's message and its NOTIFYICONDATAW (offsets on the wire)
enum : DWORD { kNimAdd = 0, kNimModify = 1, kNimDelete = 2, kNimSetFocus = 3, kNimSetVersion = 4 };
enum : DWORD { kNifMessage = 1, kNifIcon = 2, kNifTip = 4, kNifState = 8, kNifInfo = 0x10, kNifGuid = 0x20, kNifRealtime = 0x40, kNifShowTip = 0x80 };
enum : DWORD { kNisHidden = 1 };
enum : DWORD { kNiifNone = 0, kNiifInfo = 1, kNiifWarning = 2, kNiifError = 3, kNiifUser = 4, kNiifIconMask = 0xF };
enum : UINT { kNinSelect = WM_USER, kNinKeySelect = WM_USER + 1, kNinBalloonShow = WM_USER + 2, kNinBalloonHide = WM_USER + 3,
              kNinBalloonTimeout = WM_USER + 4, kNinBalloonUserClick = WM_USER + 5, kNinPopupOpen = WM_USER + 6, kNinPopupClose = WM_USER + 7 };

struct TrayData {
    DWORD msg = 0;            // kNim*
    DWORD size = 0;           // the sender's cbSize
    DWORD hwnd = 0, uid = 0, flags = 0, callback = 0, icon = 0;
    std::wstring tip;
    DWORD state = 0, state_mask = 0;
    std::wstring info, info_title;
    DWORD timeout_or_version = 0;
    DWORD info_flags = 0;
    GUID guid = {};
    DWORD balloon_icon = 0;
};

// What came in a dwData 1 buffer, bounded by both the buffer and the sender's cbSize (anything past either is absent).
inline bool tray_parse(const void *data, size_t n, TrayData &d) {
    const BYTE *p = (const BYTE *)data;
    if (!p || n < 8 + 24) return false; // signature, message, and NOTIFYICONDATA up to hIcon
    DWORD sig;
    memcpy(&sig, p, 4);
    memcpy(&d.msg, p + 4, 4);
    if (sig != kTraySignature) return false;
    const BYTE *nid = p + 8;
    memcpy(&d.size, nid, 4);
    size_t have = n - 8;
    if (d.size < have) have = d.size; // never past what the sender says it filled
    if (have < 24) return false;
    auto dword = [&](size_t off, DWORD &out) {
        if (off + 4 <= have) memcpy(&out, nid + off, 4);
    };
    auto text = [&](size_t off, size_t chars, std::wstring &out) {
        out.clear();
        for (size_t i = 0; i < chars && off + (i + 1) * 2 <= have; i++) { // up to its NUL, or its field's end
            wchar_t c;
            memcpy(&c, nid + off + i * 2, 2);
            if (!c) break;
            out += c;
        }
    };
    dword(4, d.hwnd);
    dword(8, d.uid);
    dword(12, d.flags);
    dword(16, d.callback);
    dword(20, d.icon);
    // the first NOTIFYICONDATA (152 bytes on the wire) had a tip of 64 characters and ended there
    bool v1 = have < 288;
    text(24, v1 ? 64 : 128, d.tip);
    if (v1) {
        d.flags &= kNifMessage | kNifIcon | kNifTip; // nothing else fits in it
        return true;
    }
    dword(280, d.state);
    dword(284, d.state_mask);
    text(288, 256, d.info);
    dword(800, d.timeout_or_version);
    text(804, 64, d.info_title);
    dword(932, d.info_flags);
    if (952 <= have) memcpy(&d.guid, nid + 936, 16);
    else d.flags &= ~(DWORD)kNifGuid; // a GUID flag without a GUID in the struct
    dword(952, d.balloon_icon);
    return true;
}

// dwData 3: WINNOTIFYICONIDENTIFIER
struct TrayRectQuery {
    DWORD msg = 0; // 1 top-left, 2 width and height
    DWORD hwnd = 0, uid = 0;
    GUID guid = {};
};
inline bool tray_parse_rect(const void *data, size_t n, TrayRectQuery &q) {
    const BYTE *p = (const BYTE *)data;
    if (!p || n < 40) return false;
    DWORD magic;
    memcpy(&magic, p, 4);
    if (magic != kTraySignature) return false;
    memcpy(&q.msg, p + 4, 4);
    memcpy(&q.hwnd, p + 16, 4);
    memcpy(&q.uid, p + 20, 4);
    memcpy(&q.guid, p + 24, 16);
    return true;
}

// dwData 0: APPBARMSGDATAV3 (with APPBARDATAV2 in it)
struct AppBarMsg {
    DWORD size = 0, hwnd = 0, callback = 0, edge = 0;
    RECT rc = {};
    int64_t lparam = 0;
    DWORD msg = 0;
    uint64_t shared = 0; // a handle in the sender's process (SHLockShared with its pid)
    DWORD pid = 0;
};
const size_t kAppBarMsgSize = 64, kAppBarDataSize = 40;
inline bool tray_parse_appbar(const void *data, size_t n, AppBarMsg &m) {
    const BYTE *p = (const BYTE *)data;
    if (!p || n != kAppBarMsgSize) return false;
    memcpy(&m.size, p, 4);
    memcpy(&m.hwnd, p + 4, 4);
    memcpy(&m.callback, p + 8, 4);
    memcpy(&m.edge, p + 12, 4);
    memcpy(&m.rc, p + 16, 16);
    memcpy(&m.lparam, p + 32, 8);
    memcpy(&m.msg, p + 40, 4);
    memcpy(&m.shared, p + 48, 8);
    memcpy(&m.pid, p + 56, 4);
    return m.size == kAppBarDataSize;
}

// --- what a message does to the icons ---------------------------------------------------------------------------

struct TrayIconState {
    DWORD hwnd = 0, uid = 0;
    GUID guid = {};
    bool has_guid = false;
    DWORD callback = 0;      // 0: no callback message (NIF_MESSAGE never given)
    UINT version = 0;        // NIM_SETVERSION: 0, 3 or 4
    std::wstring tip;
    bool show_tip = false;   // version 4 shows the standard tooltip only with NIF_SHOWTIP
    bool hidden = false;     // NIS_HIDDEN
};

struct TrayChange {
    bool ok = false;     // what Shell_NotifyIcon returns
    int idx = -1;        // the icon (before its removal, for deletes)
    bool added = false, removed = false;
    bool icon = false;   // NIF_ICON: a new picture (d.icon, 0 = none)
    bool balloon = false, balloon_hide = false; // NIF_INFO with text, or with none (the current one goes)
};

inline bool tray_same(const TrayIconState &s, const TrayData &d) {
    if (d.flags & kNifGuid) return s.has_guid && !memcmp(&s.guid, &d.guid, sizeof(GUID)); // with a GUID, uID is ignored
    return s.hwnd == d.hwnd && s.uid == d.uid;
}

// Applies one Shell_NotifyIcon message. T derives from TrayIconState (tray.cpp keeps the pictures beside it).
template <class T> TrayChange tray_apply(std::vector<T> &icons, const TrayData &d) {
    TrayChange c;
    int at = -1;
    for (int i = 0; i < (int)icons.size(); i++)
        if (tray_same(icons[i], d)) {
            at = i;
            break;
        }
    switch (d.msg) {
    case kNimAdd:
    case kNimModify: {
        if (at < 0 && d.msg == kNimModify) return c; // apps add again when a modify fails (a restarted tray)
        if (at < 0) {
            if (!d.hwnd) return c;                     // an icon nobody could be told about
            icons.push_back(T());
            at = (int)icons.size() - 1;
            icons[at].hwnd = d.hwnd;
            icons[at].uid = d.uid;
            c.added = true;
        }
        // an add for one already there: taken as a modify (ManagedShell does the same; apps re-add to be sure)
        T &s = icons[at];
        if (d.flags & kNifGuid) {
            s.guid = d.guid;
            s.has_guid = true;
        }
        if (d.hwnd) s.hwnd = d.hwnd; // a GUID icon may move to another window
        if (d.flags & kNifMessage) s.callback = d.callback;
        if (d.flags & kNifTip) s.tip = d.tip;
        if (d.msg == kNimAdd || (d.flags & kNifShowTip)) s.show_tip = (d.flags & kNifShowTip) != 0;
        if ((d.flags & kNifState) && (d.state_mask & kNisHidden)) s.hidden = (d.state & kNisHidden) != 0;
        c.icon = (d.flags & kNifIcon) != 0;
        if (d.flags & kNifInfo) {
            if (!d.info.empty()) c.balloon = true;
            else c.balloon_hide = true;
        }
        c.ok = true;
        c.idx = at;
        return c;
    }
    case kNimDelete:
        if (at < 0) return c;
        icons.erase(icons.begin() + at);
        c.ok = c.removed = true;
        c.idx = at;
        return c;
    case kNimSetVersion:
        if (at < 0 || d.timeout_or_version > 4) return c;
        icons[at].version = d.timeout_or_version;
        c.ok = true;
        c.idx = at;
        return c;
    case kNimSetFocus: // the keyboard focus back to the notification area: it has none to give
        c.ok = at >= 0;
        c.idx = at;
        return c;
    }
    return c;
}

// The callback's wParam and lParam for `msg` (a mouse message or a NIN_ one) at screen point x, y.
inline void tray_callback_args(const TrayIconState &s, UINT msg, int x, int y, WPARAM &wp, LPARAM &lp) {
    if (s.version >= 4) {
        wp = MAKEWPARAM((WORD)(short)x, (WORD)(short)y);
        lp = MAKELPARAM(msg, s.uid);
    } else {
        wp = s.uid;
        lp = msg;
    }
}

// The messages a click sends, in order (button 0 left, 1 middle, 2 right): at most 2.
inline int tray_click_messages(UINT version, int button, bool up, bool dbl, UINT out[2]) {
    static const UINT down_m[] = { WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN }, dbl_m[] = { WM_LBUTTONDBLCLK, WM_MBUTTONDBLCLK, WM_RBUTTONDBLCLK },
                      up_m[] = { WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP };
    if (button < 0 || button > 2) return 0;
    if (!up) {
        out[0] = dbl ? dbl_m[button] : down_m[button];
        return 1;
    }
    out[0] = up_m[button];
    if (version >= 3 && button == 0) {
        out[1] = kNinSelect;
        return 2;
    }
    if (version >= 3 && button == 2) {
        out[1] = WM_CONTEXTMENU;
        return 2;
    }
    return 1;
}

// The keyboard (the overflow's and Win+B's): Enter or Space selects, the menu key or Shift+F10 asks for the menu.
inline int tray_key_messages(UINT version, bool menu, UINT out[2]) {
    if (version >= 3) {
        out[0] = menu ? (UINT)WM_CONTEXTMENU : kNinKeySelect;
        return 1;
    }
    out[0] = menu ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    out[1] = menu ? WM_RBUTTONUP : WM_LBUTTONUP;
    return 2;
}
