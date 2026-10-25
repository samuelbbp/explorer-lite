// surface.cpp: see surface.h.
#include "surface.h"
#include <wincodec.h>
#include <algorithm>
#include <vector>

namespace {

template <class T> void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

IDWriteTextFormat *make_format(const wchar_t *family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat *f = nullptr;
    if (!g_dw || FAILED(g_dw->CreateTextFormat(family, NULL, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetTextAlignment(align);
    IDWriteInlineObject *dots = nullptr;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(f, &dots))) {
        DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        f->SetTrimming(&t, dots);
        dots->Release();
    }
    return f;
}

bool make_target(Surface &s) {
    if (s.rt) return true;
    if (!g_d2d) return false;
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(), (float)s.dpi, (float)s.dpi);
    if (FAILED(g_d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(s.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &s.rt))) return false;
    s.rt->SetDpi((float)s.dpi, (float)s.dpi);
    s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    s.rt->CreateSolidColorBrush(color(kText), &s.br);
    s.dc = s.rt;
    return true;
}

void drop_target(Surface &s) {
    for (auto &kv : s.bmps) release(kv.second);
    s.bmps.clear();
    release(s.br);
    release(s.rt);
    s.dc = nullptr;
}

} // namespace

void surf_paint(Surface &s, void (*draw)(void *), void *ctx) {
    PAINTSTRUCT ps;
    BeginPaint(s.hwnd, &ps);
    EndPaint(s.hwnd, &ps);
    if (!make_target(s)) return;
    // the window may have been resized or moved to another monitor since the target was made
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    D2D1_SIZE_U px = s.rt->GetPixelSize();
    if (px.width != (UINT32)rc.right || px.height != (UINT32)rc.bottom) s.rt->Resize(D2D1::SizeU(rc.right, rc.bottom));
    s.rt->SetDpi((float)s.dpi, (float)s.dpi);
    s.dc->BeginDraw();
    draw(ctx);
    if (s.dc->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        drop_target(s);
        InvalidateRect(s.hwnd, NULL, FALSE);
    }
}

bool surf_shot(const std::vector<ShotPart> &parts, const std::wstring &png) {
    if (!g_d2d || parts.empty()) return false;
    IWICImagingFactory *wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!wic) return false;
    LONG width = 0, height = 0;
    for (const ShotPart &p : parts) {
        RECT rc;
        GetClientRect(p.s->hwnd, &rc);
        width = std::max(width, rc.right);
        height += rc.bottom;
    }
    IWICBitmap *bmp = nullptr;
    ID2D1RenderTarget *rt = nullptr;
    ID2D1SolidColorBrush *br = nullptr;
    bool ok = false;
    // at 96 DPI: each part is scaled by its own DPI and moved down below the one before
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (width > 0 && height > 0 && SUCCEEDED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
        SUCCEEDED(g_d2d->CreateWicBitmapRenderTarget(bmp, props, &rt)) && SUCCEEDED(rt->CreateSolidColorBrush(color(kText), &br))) {
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        float y = 0;
        for (const ShotPart &p : parts) {
            Surface &s = *p.s;
            // the icons belong to the window's target: a set of their own for this one
            std::unordered_map<const Thumb *, ID2D1Bitmap *> bmps;
            bmps.swap(s.bmps);
            ID2D1RenderTarget *dc = s.dc;
            ID2D1SolidColorBrush *sbr = s.br;
            s.dc = rt;
            s.br = br;
            s.shot = true;
            float k = surf_scale(s);
            rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(0, y));
            p.draw(p.ctx);
            s.shot = false;
            for (auto &kv : s.bmps) release(kv.second);
            s.bmps.swap(bmps);
            s.dc = dc;
            s.br = sbr;
            RECT rc;
            GetClientRect(s.hwnd, &rc);
            y += rc.bottom;
        }
        ok = SUCCEEDED(rt->EndDraw());
    }
    IWICStream *st = nullptr;
    IWICBitmapEncoder *enc = nullptr;
    IWICBitmapFrameEncode *fr = nullptr;
    if (ok) {
        ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(enc->CreateNewFrame(&fr, NULL)) && SUCCEEDED(fr->Initialize(NULL)) && SUCCEEDED(fr->WriteSource(bmp, NULL)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    release(fr);
    release(enc);
    release(st);
    release(br);
    release(rt);
    release(bmp);
    release(wic);
    return ok;
}

void surf_formats(Surface &s) {
    if (s.f_ui) return;
    const wchar_t *ui = L"Segoe UI Variable Text";
    s.f_ui = make_format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    s.f_ui_c = make_format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    s.f_small = make_format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    s.f_small_r = make_format(ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    s.f_big = make_format(ui, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    s.f_icon = make_format(icon_font(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    s.f_bold = make_format(ui, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    if ((s.f_wrap = make_format(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING))) {
        s.f_wrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        s.f_wrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
}

void surf_free(Surface &s) {
    drop_target(s);
    for (IDWriteTextFormat **f : { &s.f_ui, &s.f_ui_c, &s.f_small, &s.f_small_r, &s.f_icon, &s.f_big, &s.f_wrap, &s.f_bold }) release(*f);
}

void surf_text(Surface &s, const std::wstring &t, D2D1_RECT_F r, IDWriteTextFormat *f, uint32_t rgb, float a) {
    if (!f || t.empty() || r.right <= r.left) return;
    s.br->SetColor(color(rgb, a));
    s.dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float surf_text_width(Surface &s, const std::wstring &t, IDWriteTextFormat *f) {
    IDWriteTextLayout *l = nullptr;
    if (!f || !g_dw || FAILED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, 10000, 100, &l))) return 0;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}

float surf_text_height(const std::wstring &t, IDWriteTextFormat *f, float width) {
    IDWriteTextLayout *l = nullptr;
    if (!f || !g_dw || FAILED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, width, 10000, &l))) return 0;
    DWRITE_TEXT_METRICS m = {};
    l->GetMetrics(&m);
    l->Release();
    return m.height;
}

void surf_fill(Surface &s, D2D1_RECT_F r, uint32_t rgb, float a) {
    s.br->SetColor(color(rgb, a));
    s.dc->FillRectangle(r, s.br);
}

void surf_round(Surface &s, D2D1_RECT_F r, float rad, uint32_t rgb, float a) {
    s.br->SetColor(color(rgb, a));
    s.dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), s.br);
}

void surf_icon(Surface &s, const Thumb *t, D2D1_RECT_F r, float a) {
    if (!t || t->w <= 0) return;
    ID2D1Bitmap *bmp = nullptr;
    auto it = s.bmps.find(t);
    if (it != s.bmps.end()) bmp = it->second;
    else {
        s.dc->CreateBitmap(D2D1::SizeU(t->w, t->h), t->px.data(), t->w * 4,
                           D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
        s.bmps[t] = bmp; // also a failure: not tried again
    }
    if (bmp) s.dc->DrawBitmap(bmp, r, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void surf_forget(Surface &s, const Thumb *t) {
    auto it = s.bmps.find(t);
    if (it == s.bmps.end()) return;
    release(it->second);
    s.bmps.erase(it);
}

bool icon_pixels(HICON icon, Thumb &t) {
    ICONINFO ii = {};
    if (!icon || !GetIconInfo(icon, &ii)) return false;
    bool ok = false;
    BITMAP bm = {};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        t.w = bm.bmWidth;
        t.h = bm.bmHeight;
        t.px.assign((size_t)t.w * t.h, 0);
        BITMAPINFO bi = {};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), t.w, -t.h, 1, 32, BI_RGB };
        HDC dc = GetDC(NULL);
        ok = GetDIBits(dc, ii.hbmColor, 0, t.h, t.px.data(), &bi, DIB_RGB_COLORS) == t.h;
        bool alpha = false;
        for (uint32_t p : t.px)
            if (p >> 24) alpha = true;
        if (ok && !alpha) { // an old icon: the mask says what is transparent
            std::vector<uint32_t> mask((size_t)t.w * t.h, 0);
            if (ii.hbmMask && GetDIBits(dc, ii.hbmMask, 0, t.h, mask.data(), &bi, DIB_RGB_COLORS) == t.h)
                for (size_t i = 0; i < t.px.size(); i++) t.px[i] = (mask[i] & 0xFFFFFF) ? 0 : (t.px[i] | 0xFF000000);
            else
                for (uint32_t &p : t.px) p |= 0xFF000000;
        } else if (ok) { // straight alpha: premultiplied for Direct2D
            for (uint32_t &p : t.px) {
                uint32_t a = p >> 24;
                p = a << 24 | (((p >> 16) & 255) * a / 255) << 16 | (((p >> 8) & 255) * a / 255) << 8 | ((p & 255) * a / 255);
            }
        }
        ReleaseDC(NULL, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (!ok) {
        t.w = t.h = 0;
        t.px.clear();
    }
    return ok;
}
