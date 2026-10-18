// shellicon.cpp
#include "shellicon.h"

#include "decode.h"

#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// Draws the icon into a 32bpp top-down DIB and returns straight-alpha BGRA.
std::vector<BYTE> RenderIcon(HICON icon, UINT size) {
    std::vector<BYTE> pixels(static_cast<size_t>(size) * size * 4, 0);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(size);
    info.bmiHeader.biHeight = -static_cast<LONG>(size);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) return pixels;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    if (dib && memory) {
        HGDIOBJ previous = SelectObject(memory, dib);
        std::memset(bits, 0, pixels.size());
        DrawIconEx(memory, 0, 0, icon, static_cast<int>(size), static_cast<int>(size), 0, nullptr,
                   DI_NORMAL);
        SelectObject(memory, previous);
        std::memcpy(pixels.data(), bits, pixels.size());
    }

    if (memory) DeleteDC(memory);
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen);

    // Some legacy icons carry no alpha at all; treat every painted pixel as opaque.
    bool anyAlpha = false;
    for (UINT i = 0; i < size * size && !anyAlpha; ++i) {
        anyAlpha = pixels[static_cast<size_t>(i) * 4 + 3] != 0;
    }
    for (UINT i = 0; i < size * size; ++i) {
        BYTE* pixel = pixels.data() + static_cast<size_t>(i) * 4;
        if (!anyAlpha) {
            const bool painted = pixel[0] || pixel[1] || pixel[2];
            pixel[3] = painted ? 255 : 0;
        }
        // Direct2D wants premultiplied alpha.
        const UINT alpha = pixel[3];
        pixel[0] = static_cast<BYTE>(pixel[0] * alpha / 255);
        pixel[1] = static_cast<BYTE>(pixel[1] * alpha / 255);
        pixel[2] = static_cast<BYTE>(pixel[2] * alpha / 255);
    }
    return pixels;
}

}  // namespace

HRESULT CreateShellIconBitmap(ID2D1RenderTarget* target, const std::wstring& path,
                              ComPtr<ID2D1Bitmap>& bitmap, UINT* pixelSize) {
    if (!target || path.empty()) return E_POINTER;

    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX)) return E_FAIL;

    HICON icon = nullptr;
    UINT size = 0;

    ComPtr<IImageList> imageList;
    if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&imageList))) && imageList) {
        if (SUCCEEDED(imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
            int cx = 0, cy = 0;
            if (SUCCEEDED(imageList->GetIconSize(&cx, &cy))) {
                size = static_cast<UINT>(std::max(cx, cy));
            }
        }
    }
    if (!icon) {
        SHFILEINFOW large{};
        if (!SHGetFileInfoW(path.c_str(), 0, &large, sizeof(large), SHGFI_ICON | SHGFI_LARGEICON) ||
            !large.hIcon) {
            return E_FAIL;
        }
        icon = large.hIcon;
        size = static_cast<UINT>(GetSystemMetrics(SM_CXICON));
    }
    if (size == 0) size = 32;

    std::vector<BYTE> pixels = RenderIcon(icon, size);
    DestroyIcon(icon);

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CreateWicFactory(&wic);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmap> wicBitmap;
    hr = wic->CreateBitmapFromMemory(size, size, GUID_WICPixelFormat32bppPBGRA, size * 4,
                                     static_cast<UINT>(pixels.size()), pixels.data(), &wicBitmap);
    if (FAILED(hr)) return hr;

    hr = target->CreateBitmapFromWicBitmap(wicBitmap.Get(), &bitmap);
    if (SUCCEEDED(hr) && pixelSize) *pixelSize = size;
    return hr;
}
