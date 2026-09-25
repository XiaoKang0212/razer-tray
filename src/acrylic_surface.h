#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace AcrylicSurface {

struct Buffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
};

inline bool Create(HDC reference, int width, int height, Buffer& buffer) {
    buffer.width = width;
    buffer.height = height;
    buffer.dc = CreateCompatibleDC(reference);
    if (!buffer.dc) return false;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    buffer.bitmap = CreateDIBSection(buffer.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!buffer.bitmap || !bits) {
        if (buffer.bitmap) DeleteObject(buffer.bitmap);
        DeleteDC(buffer.dc);
        buffer = {};
        return false;
    }

    buffer.pixels = static_cast<uint32_t*>(bits);
    buffer.previousBitmap = SelectObject(buffer.dc, buffer.bitmap);
    if (!buffer.previousBitmap || buffer.previousBitmap == HGDI_ERROR) {
        DeleteObject(buffer.bitmap);
        DeleteDC(buffer.dc);
        buffer = {};
        return false;
    }
    return true;
}

inline void Destroy(Buffer& buffer) {
    if (buffer.dc && buffer.previousBitmap && buffer.previousBitmap != HGDI_ERROR) {
        SelectObject(buffer.dc, buffer.previousBitmap);
    }
    if (buffer.bitmap) DeleteObject(buffer.bitmap);
    if (buffer.dc) DeleteDC(buffer.dc);
    buffer = {};
}

inline uint32_t PackRgb(COLORREF color) {
    return (static_cast<uint32_t>(GetRValue(color)) << 16) |
           (static_cast<uint32_t>(GetGValue(color)) << 8) |
           static_cast<uint32_t>(GetBValue(color));
}

// DWM uses zero-alpha pixels as the acrylic backdrop. GDI writes RGB without
// reliable alpha, so reconstruct it: untouched base pixels expose acrylic,
// while every GDI-drawn pixel (especially dark text) stays fully visible.
inline void SetAcrylicAlpha(Buffer& buffer, COLORREF baseColor) {
    const uint32_t baseRgb = PackRgb(baseColor);
    const std::size_t pixelCount = static_cast<std::size_t>(buffer.width) * buffer.height;
    for (std::size_t i = 0; i < pixelCount; ++i) {
        uint32_t& pixel = buffer.pixels[i];
        const uint32_t rgb = pixel & 0x00FFFFFFu;
        pixel = (rgb == baseRgb) ? 0u : (0xFF000000u | rgb);
    }
}

inline bool Present(HDC target, Buffer& buffer, COLORREF baseColor, bool acrylicEnabled) {
    GdiFlush();
    if (acrylicEnabled) {
        SetAcrylicAlpha(buffer, baseColor);
    } else {
        const std::size_t pixelCount = static_cast<std::size_t>(buffer.width) * buffer.height;
        for (std::size_t i = 0; i < pixelCount; ++i) {
            buffer.pixels[i] |= 0xFF000000u;
        }
    }

    if (BitBlt(target, 0, 0, buffer.width, buffer.height,
               buffer.dc, 0, 0, SRCCOPY)) return true;

    // If copying the alpha surface fails, restore an opaque card background so
    // the interface remains readable even without acrylic composition.
    const uint32_t background = PackRgb(baseColor);
    const std::size_t pixelCount = static_cast<std::size_t>(buffer.width) * buffer.height;
    for (std::size_t i = 0; i < pixelCount; ++i) {
        uint32_t& pixel = buffer.pixels[i];
        if ((pixel & 0xFF000000u) == 0) pixel = background;
        pixel |= 0xFF000000u;
    }
    return BitBlt(target, 0, 0, buffer.width, buffer.height,
                  buffer.dc, 0, 0, SRCCOPY) != FALSE;
}

} // namespace AcrylicSurface
