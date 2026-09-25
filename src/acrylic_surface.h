#pragma once

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AcrylicSurface {

struct Buffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
};

struct TextCommand {
    std::wstring text;
    COLORREF color = RGB(0, 0, 0);
    HFONT font = nullptr;
    RECT bounds = {};
    UINT format = 0;
};

using TextCommands = std::vector<TextCommand>;

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

// DWM uses zero-alpha pixels as the acrylic backdrop. Reconstruct the alpha
// for the GDI-painted background and controls; text is layered afterward with
// its own grayscale antialias coverage.
inline void SetAcrylicAlpha(Buffer& buffer, COLORREF baseColor) {
    const uint32_t baseRgb = PackRgb(baseColor);
    const std::size_t pixelCount = static_cast<std::size_t>(buffer.width) * buffer.height;
    for (std::size_t i = 0; i < pixelCount; ++i) {
        uint32_t& pixel = buffer.pixels[i];
        const uint32_t rgb = pixel & 0x00FFFFFFu;
        pixel = (rgb == baseRgb) ? 0u : (0xFF000000u | rgb);
    }
}

inline void Prepare(Buffer& buffer, COLORREF baseColor, bool acrylicEnabled) {
    GdiFlush();
    if (acrylicEnabled) {
        SetAcrylicAlpha(buffer, baseColor);
    } else {
        const std::size_t pixelCount = static_cast<std::size_t>(buffer.width) * buffer.height;
        for (std::size_t i = 0; i < pixelCount; ++i) {
            buffer.pixels[i] |= 0xFF000000u;
        }
    }
}

inline void QueueText(TextCommands& commands, HFONT font, COLORREF color,
                      const WCHAR* text, const RECT& bounds, UINT format) {
    if (!font || !text || !text[0] || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    commands.push_back({text, color, font, bounds, format});
}

// Render a grayscale antialias mask, then composite premultiplied glyph pixels
// over the prepared acrylic surface. This avoids treating GDI's matte-blended
// edge pixels as opaque, which otherwise makes text look blurred or tinted.
inline bool DrawTextAlpha(HDC target, HFONT font, COLORREF color, const WCHAR* text,
                          const RECT& bounds, UINT format) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (!target || !font || !text || !text[0] || width <= 0 || height <= 0) return false;

    Buffer mask;
    if (!Create(target, width, height, mask)) return false;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    std::fill(mask.pixels, mask.pixels + pixelCount, 0u);

    SetBkMode(mask.dc, TRANSPARENT);
    SetTextColor(mask.dc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(mask.dc, font);
    RECT localBounds = {0, 0, width, height};
    DrawTextW(mask.dc, text, -1, &localBounds, format);
    SelectObject(mask.dc, oldFont);
    GdiFlush();

    const uint32_t red = GetRValue(color);
    const uint32_t green = GetGValue(color);
    const uint32_t blue = GetBValue(color);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const uint32_t pixel = mask.pixels[i];
        const uint32_t coverage = std::max({
            pixel & 0xFFu,
            (pixel >> 8) & 0xFFu,
            (pixel >> 16) & 0xFFu
        });
        if (coverage == 0) continue;

        const uint32_t premulRed = (red * coverage + 127u) / 255u;
        const uint32_t premulGreen = (green * coverage + 127u) / 255u;
        const uint32_t premulBlue = (blue * coverage + 127u) / 255u;
        mask.pixels[i] = (coverage << 24) | (premulRed << 16) |
                         (premulGreen << 8) | premulBlue;
    }

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const bool drawn = AlphaBlend(target, bounds.left, bounds.top, width, height,
                                  mask.dc, 0, 0, width, height, blend) != FALSE;
    Destroy(mask);
    return drawn;
}

inline bool DrawTextCommands(Buffer& target, const TextCommands& commands) {
    bool success = true;
    for (const TextCommand& command : commands) {
        if (!DrawTextAlpha(target.dc, command.font, command.color,
                           command.text.c_str(), command.bounds, command.format)) {
            success = false;

            SetBkMode(target.dc, TRANSPARENT);
            SetTextColor(target.dc, command.color);
            HGDIOBJ oldFont = SelectObject(target.dc, command.font);
            RECT textBounds = command.bounds;
            DrawTextW(target.dc, command.text.c_str(), -1, &textBounds, command.format);
            SelectObject(target.dc, oldFont);
            GdiFlush();

            const int left = std::clamp(static_cast<int>(command.bounds.left), 0, target.width);
            const int right = std::clamp(static_cast<int>(command.bounds.right), 0, target.width);
            const int top = std::clamp(static_cast<int>(command.bounds.top), 0, target.height);
            const int bottom = std::clamp(static_cast<int>(command.bounds.bottom), 0, target.height);
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    uint32_t& pixel = target.pixels[y * target.width + x];
                    if ((pixel & 0x00FFFFFFu) != 0) pixel |= 0xFF000000u;
                }
            }
        }
    }
    return success;
}

inline bool PresentPrepared(HDC target, Buffer& buffer, COLORREF fallbackColor) {
    GdiFlush();
    if (BitBlt(target, 0, 0, buffer.width, buffer.height,
               buffer.dc, 0, 0, SRCCOPY)) return true;

    // Flatten the premultiplied surface onto an opaque fallback if copying the
    // alpha surface fails, preserving both controls and antialiased text.
    const uint32_t backgroundRed = GetRValue(fallbackColor);
    const uint32_t backgroundGreen = GetGValue(fallbackColor);
    const uint32_t backgroundBlue = GetBValue(fallbackColor);
    const std::size_t pixelCount = static_cast<std::size_t>(buffer.width) * buffer.height;
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const uint32_t pixel = buffer.pixels[i];
        const uint32_t alpha = pixel >> 24;
        const uint32_t inverseAlpha = 255u - alpha;
        const uint32_t red = std::min(255u, ((pixel >> 16) & 0xFFu) +
                                            (backgroundRed * inverseAlpha + 127u) / 255u);
        const uint32_t green = std::min(255u, ((pixel >> 8) & 0xFFu) +
                                              (backgroundGreen * inverseAlpha + 127u) / 255u);
        const uint32_t blue = std::min(255u, (pixel & 0xFFu) +
                                             (backgroundBlue * inverseAlpha + 127u) / 255u);
        buffer.pixels[i] = 0xFF000000u | (red << 16) | (green << 8) | blue;
    }
    return BitBlt(target, 0, 0, buffer.width, buffer.height,
                  buffer.dc, 0, 0, SRCCOPY) != FALSE;
}

inline bool Present(HDC target, Buffer& buffer, COLORREF baseColor, bool acrylicEnabled) {
    Prepare(buffer, baseColor, acrylicEnabled);
    return PresentPrepared(target, buffer, baseColor);
}

} // namespace AcrylicSurface
