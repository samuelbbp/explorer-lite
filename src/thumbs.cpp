// thumbs.cpp: see thumbs.h.
#include "thumbs.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include "app.h"

namespace {

struct Req {
    HWND wnd;
    uint64_t gen;
    std::wstring target;
    int size;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Req> g_queue; // the newest at the back: taken first
bool g_running;           // the thread is alive (it ends after a few idle seconds: nothing stays at rest)
HANDLE g_wake;

// the bitmap's pixels, premultiplied (thumbnails come either way; an all-transparent one is opaque: JPEGs)
bool pixels(HBITMAP bmp, Thumb &t) {
    BITMAP bm = {};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    t.w = bm.bmWidth;
    t.h = bm.bmHeight;
    t.px.resize((size_t)t.w * t.h);
    BITMAPINFO bi = {};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), t.w, -t.h, 1, 32, BI_RGB };
    HDC dc = GetDC(NULL);
    int got = GetDIBits(dc, bmp, 0, t.h, t.px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (got != t.h) return false;
    bool any_alpha = false, straight = false;
    for (uint32_t p : t.px) {
        uint32_t a = p >> 24;
        if (a) any_alpha = true;
        if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) straight = true;
    }
    for (uint32_t &p : t.px) {
        uint32_t a = p >> 24;
        if (!any_alpha) p |= 0xFF000000;
        else if (straight && a < 255)
            p = a << 24 | (((p >> 16) & 255) * a / 255) << 16 | (((p >> 8) & 255) * a / 255) << 8 | ((p & 255) * a / 255);
    }
    return true;
}

DWORD WINAPI thumb_thread(void *) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_queue.empty()) {
            ReleaseSRWLockExclusive(&g_lock);
            if (WaitForSingleObject(g_wake, 5000) == WAIT_TIMEOUT) {
                AcquireSRWLockExclusive(&g_lock);
                if (g_queue.empty()) { // idle: the thread goes, a new request starts another
                    g_running = false;
                    ReleaseSRWLockExclusive(&g_lock);
                    break;
                }
                ReleaseSRWLockExclusive(&g_lock);
            }
            continue;
        }
        Req r = std::move(g_queue.back());
        g_queue.pop_back();
        ReleaseSRWLockExclusive(&g_lock);

        auto *t = new Thumb{ r.gen, r.target };
        IShellItemImageFactory *f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(r.target.c_str(), NULL, IID_PPV_ARGS(&f)))) {
            HBITMAP bmp = NULL;
            // the thumbnail if it has one, its big icon otherwise
            if (SUCCEEDED(f->GetImage({ r.size, r.size }, SIIGBF_RESIZETOFIT, &bmp)) && bmp) {
                if (!pixels(bmp, *t)) {
                    t->w = t->h = 0;
                    t->px.clear();
                }
                DeleteObject(bmp);
            }
            f->Release();
        }
        if (!PostMessageW(r.wnd, WM_APP_THUMB, 0, (LPARAM)t)) delete t;
    }
    CoUninitialize();
    return 0;
}

} // namespace

void thumbs_request(HWND wnd, uint64_t gen, const std::wstring &target, int size) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_wake) g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_queue.push_back({ wnd, gen, target, size });
    bool start = !g_running;
    if (start) g_running = true;
    ReleaseSRWLockExclusive(&g_lock);
    if (start) {
        HANDLE t = CreateThread(NULL, 0, thumb_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else {
            AcquireSRWLockExclusive(&g_lock);
            g_running = false;
            ReleaseSRWLockExclusive(&g_lock);
        }
    } else SetEvent(g_wake);
}

void thumbs_cancel(uint64_t gen) {
    AcquireSRWLockExclusive(&g_lock);
    g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(), [&](const Req &r) { return r.gen == gen; }), g_queue.end());
    ReleaseSRWLockExclusive(&g_lock);
}
