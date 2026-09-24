#include "tray_icon.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <strsafe.h>

namespace Tray {
namespace {

// Everything is composed at a multiple of the requested size and then box
// filtered back down, which keeps the shapes smooth at 16 px taskbar sizes.
constexpr int SUPERSAMPLE = 16;
constexpr WORD LOGO_MASK_RESOURCE_ID = 2;
constexpr WORD RT_RCDATA_ID = 10;   // RT_RCDATA, spelled out so UNICODE is not required
constexpr float PI = 3.14159265f;

struct LogoMask {
    const BYTE* pixels = nullptr;
    int width = 0;
    int height = 0;

    bool Valid() const { return pixels != nullptr && width > 1 && height > 1; }
};

LogoMask LoadLogoMask() {
    LogoMask mask;
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(LOGO_MASK_RESOURCE_ID), MAKEINTRESOURCEW(RT_RCDATA_ID));
    if (!resource) return mask;

    HGLOBAL handle = LoadResource(nullptr, resource);
    if (!handle) return mask;

    const BYTE* data = (const BYTE*)LockResource(handle);
    const DWORD size = SizeofResource(nullptr, resource);
    if (!data || size < 8 || memcmp(data, "RMK1", 4) != 0) return mask;

    const WORD width = (WORD)(data[4] | (data[5] << 8));
    const WORD height = (WORD)(data[6] | (data[7] << 8));
    if (width < 2 || height < 2 || size < 8 + (DWORD)width * height) return mask;

    mask.pixels = data + 8;
    mask.width = width;
    mask.height = height;
    return mask;
}

// Bilinear sample of the alpha mask, u/v in the 0..1 range.
float SampleMask(const LogoMask& mask, float u, float v) {
    if (!mask.Valid()) return 0.0f;

    const float fx = std::clamp(u, 0.0f, 1.0f) * (mask.width - 1);
    const float fy = std::clamp(v, 0.0f, 1.0f) * (mask.height - 1);
    const int x0 = (int)fx;
    const int y0 = (int)fy;
    const int x1 = std::min(x0 + 1, mask.width - 1);
    const int y1 = std::min(y0 + 1, mask.height - 1);
    const float tx = fx - x0;
    const float ty = fy - y0;

    auto at = [&](int x, int y) { return (float)mask.pixels[y * mask.width + x]; };
    const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
    return ((top * (1.0f - ty) + bottom * ty) / 255.0f);
}

// Signed distance of a rounded rectangle, negative inside.
float RoundedRectDistance(float x, float y, float left, float top, float right, float bottom, float radius) {
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float halfWidth = (right - left) * 0.5f;
    const float halfHeight = (bottom - top) * 0.5f;
    const float dx = std::max(std::fabs(x - centerX) - (halfWidth - radius), 0.0f);
    const float dy = std::max(std::fabs(y - centerY) - (halfHeight - radius), 0.0f);
    return std::sqrt(dx * dx + dy * dy) - radius;
}

} // namespace

HICON CreateBatteryIcon(int battery, bool isCharging, bool isConnected, int size, bool isDark, int style) {
    if (size <= 0) size = 16;
    if (style < BATTERY_STYLE_RING || style > BATTERY_STYLE_NUMBER) style = BATTERY_STYLE_RING;

    const int W = size * SUPERSAMPLE;
    const int H = W;
    const float center = W * 0.5f;

    const bool hasLevel = isConnected && battery >= 0;
    uint32_t accent = 0xFF44D62C;                        // Razer green
    if (hasLevel && battery < 15) accent = 0xFFFF453A;    // critical
    else if (hasLevel && battery < 30) accent = 0xFFFFB020;

    // The unlit part of the ring stays clearly green so the icon reads as a full
    // ring like the official mark, while the lit arc is still brighter.
    const uint32_t trackColor = isDark ? 0xFF2F7A32 : 0xFF9DC79D;
    const uint32_t dimColor = isDark ? 0xFF646E64 : 0xFF97A197;
    const float progress = hasLevel ? std::clamp(battery / 100.0f, 0.0f, 1.0f) : 0.0f;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP canvas = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!canvas || !pixels) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    uint32_t* buffer = (uint32_t*)pixels;
    memset(buffer, 0, (size_t)W * H * sizeof(uint32_t));

    auto Blend = [&](int x, int y, uint32_t color, float alpha) {
        if (x < 0 || y < 0 || x >= W || y >= H || alpha <= 0.0f) return;

        uint32_t& dst = buffer[y * W + x];
        const float srcA = std::min(alpha, 1.0f);
        const float dstA = ((dst >> 24) & 0xFF) / 255.0f;
        const float outA = srcA + dstA * (1.0f - srcA);
        if (outA <= 0.0001f) {
            dst = 0;
            return;
        }

        auto channel = [&](int shift) {
            const float src = ((color >> shift) & 0xFF) / 255.0f;
            const float d = ((dst >> shift) & 0xFF) / 255.0f;
            return (src * srcA + d * dstA * (1.0f - srcA)) / outA;
        };

        const uint32_t r = (uint32_t)std::lround(std::clamp(channel(16), 0.0f, 1.0f) * 255.0f);
        const uint32_t g = (uint32_t)std::lround(std::clamp(channel(8), 0.0f, 1.0f) * 255.0f);
        const uint32_t b = (uint32_t)std::lround(std::clamp(channel(0), 0.0f, 1.0f) * 255.0f);
        const uint32_t a = (uint32_t)std::lround(std::clamp(outA, 0.0f, 1.0f) * 255.0f);
        dst = (a << 24) | (r << 16) | (g << 8) | b;
    };

    if (style == BATTERY_STYLE_BATTERY) {
        // Classic battery: body + tip, level fill from the left.
        const float bodyLeft = W * 0.02f;
        const float bodyRight = W * 0.84f;
        const float bodyTop = H * 0.16f;
        const float bodyBottom = H * 0.84f;
        const float radius = (bodyBottom - bodyTop) * 0.28f;
        const float borderWidth = std::max(1.0f, size * 0.12f) * SUPERSAMPLE;
        const uint32_t borderColor = isConnected ? (isDark ? 0xFFCBD8CB : 0xFF4A544A) : dimColor;

        const float tipLeft = bodyRight - borderWidth * 0.5f;
        const float tipRight = W * 0.98f;
        const float tipTop = H * 0.34f;
        const float tipBottom = H * 0.66f;
        const float tipRadius = (tipBottom - tipTop) * 0.35f;

        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < (int)W; ++x) {
                const float px = x + 0.5f;
                const float py = y + 0.5f;

                const float tipDistance = RoundedRectDistance(px, py, tipLeft, tipTop, tipRight, tipBottom, tipRadius);
                const float tipCoverage = std::clamp(0.75f - tipDistance, 0.0f, 1.0f);
                if (tipCoverage > 0.0f) {
                    Blend(x, y, borderColor, tipCoverage);
                }

                const float distance = RoundedRectDistance(px, py, bodyLeft, bodyTop, bodyRight, bodyBottom, radius);
                const float coverage = std::clamp(0.75f - distance, 0.0f, 1.0f);
                if (coverage <= 0.0f) continue;

                const float inside = std::clamp(0.75f - (distance + borderWidth), 0.0f, 1.0f);
                if (inside <= 0.0f) {
                    Blend(x, y, borderColor, coverage);
                    continue;
                }

                const float levelRight = bodyLeft + borderWidth +
                    (bodyRight - bodyLeft - borderWidth * 2.0f) * progress;
                const bool filled = hasLevel && px <= levelRight;
                Blend(x, y, filled ? accent : (isConnected ? trackColor : dimColor), coverage * inside);
            }
        }
    } else if (style == BATTERY_STYLE_NUMBER) {
        // Percentage as text, recoloured so the glyph is a clean single colour.
        WCHAR text[8] = {};
        int digits = 2;
        if (!isConnected || battery < 0) {
            StringCchCopyW(text, ARRAYSIZE(text), L"--");
        } else {
            StringCchPrintfW(text, ARRAYSIZE(text), L"%d", battery);
            digits = (battery >= 100) ? 3 : ((battery >= 10) ? 2 : 1);
        }

        HFONT font = CreateFontW(
            -(int)(H * (digits >= 3 ? 0.60f : 0.82f)), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Segoe UI");

        HGDIOBJ oldCanvas = SelectObject(memDc, canvas);
        HGDIOBJ oldFont = SelectObject(memDc, font);
        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, RGB(255, 255, 255));
        RECT rcText = {0, 0, W, H};
        DrawTextW(memDc, text, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        SelectObject(memDc, oldFont);
        SelectObject(memDc, oldCanvas);
        DeleteObject(font);

        // GDI leaves the alpha channel untouched, so rebuild it from the coverage.
        for (int i = 0; i < W * H; ++i) {
            const uint32_t pixel = buffer[i];
            const uint32_t coverage = std::max({(pixel >> 16) & 0xFF, (pixel >> 8) & 0xFF, pixel & 0xFF});
            if (!coverage) {
                buffer[i] = 0;
                continue;
            }
            buffer[i] = (coverage << 24) | (accent & 0x00FFFFFF);
        }
    } else {
        // Ring gauge: the full circle is the track, the arc from the top is the level.
        // Snapping the band to whole pixels keeps it crisp at 16 px.
        // Fill the whole 16 px cell like the native Razer icon does: a thick band
        // that reaches the outer edge, so the mark does not look small and dusty.
        const float ringThicknessPx = std::max(2.4f, size * 0.15f);
        const float ringOuterPx = size * 0.5f - 0.2f;
        const float ringRadiusPx = ringOuterPx - ringThicknessPx * 0.5f;
        const float ringRadius = ringRadiusPx * SUPERSAMPLE;
        const float ringThickness = ringThicknessPx * SUPERSAMPLE;
        const uint32_t logoColor = isConnected ? accent : dimColor;

        const int ringExtent = (int)std::ceil(ringRadius + ringThickness * 0.5f) + 1;
        for (int y = (int)center - ringExtent; y <= (int)center + ringExtent; ++y) {
            for (int x = (int)center - ringExtent; x <= (int)center + ringExtent; ++x) {
                const float dx = x + 0.5f - center;
                const float dy = y + 0.5f - center;
                const float distance = std::sqrt(dx * dx + dy * dy);
                // 1.5 px wide coverage ramp keeps the band smooth instead of stepped.
                const float coverage = std::clamp((ringThickness * 0.5f + 0.75f) - std::fabs(distance - ringRadius), 0.0f, 1.0f);
                if (coverage <= 0.0f) continue;

                float angle = std::atan2(dy, dx) + PI * 0.5f;   // 0 at the top
                if (angle < 0.0f) angle += PI * 2.0f;           // clockwise in 0..2pi

                const bool inProgress = hasLevel && (angle / (PI * 2.0f)) <= progress;
                const uint32_t color = inProgress ? accent : trackColor;
                const float alpha = inProgress ? coverage : coverage * (hasLevel ? 1.0f : 0.85f);
                Blend(x, y, color, alpha);
            }
        }

        const LogoMask mask = LoadLogoMask();
        if (mask.Valid()) {
            const float innerRadiusPx = std::max(ringRadiusPx - ringThicknessPx * 0.5f, 1.0f);
            const float boxSize = std::min(W * 0.70f, (innerRadiusPx * 2.0f - 0.4f) * SUPERSAMPLE);
            const float boxLeft = center - boxSize * 0.5f;
            const float boxTop = center - boxSize * 0.5f;
            const int from = (int)std::floor(boxLeft);
            const int to = (int)std::ceil(boxLeft + boxSize);

            for (int y = from; y < to; ++y) {
                for (int x = from; x < to; ++x) {
                    const float u = (x + 0.5f - boxLeft) / boxSize;
                    const float v = (y + 0.5f - boxTop) / boxSize;
                    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;

                    float alpha = SampleMask(mask, u, v);
                    if (alpha <= 0.0f) continue;
                    // Nearly binary coverage: a solid bright glyph with only a hint
                    // of antialiasing, so thin parts do not fade into the taskbar.
                    alpha = std::clamp((alpha - 0.05f) / 0.20f, 0.0f, 1.0f);
                    Blend(x, y, logoColor, alpha);
                }
            }
        }
    }

    // Box filter the supersampled canvas down to the requested icon size.
    BITMAPINFO outputInfo = {};
    outputInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    outputInfo.bmiHeader.biWidth = size;
    outputInfo.bmiHeader.biHeight = -size;
    outputInfo.bmiHeader.biPlanes = 1;
    outputInfo.bmiHeader.biBitCount = 32;
    outputInfo.bmiHeader.biCompression = BI_RGB;

    void* outputPixels = nullptr;
    HBITMAP output = CreateDIBSection(memDc, &outputInfo, DIB_RGB_COLORS, &outputPixels, nullptr, 0);
    if (!output || !outputPixels) {
        DeleteObject(canvas);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    uint32_t* out = (uint32_t*)outputPixels;
    const int samples = SUPERSAMPLE * SUPERSAMPLE;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t alphaSum = 0, redSum = 0, greenSum = 0, blueSum = 0;
            for (int sy = 0; sy < SUPERSAMPLE; ++sy) {
                const uint32_t* row = buffer + (y * SUPERSAMPLE + sy) * W + x * SUPERSAMPLE;
                for (int sx = 0; sx < SUPERSAMPLE; ++sx) {
                    const uint32_t pixel = row[sx];
                    const uint32_t a = (pixel >> 24) & 0xFF;
                    if (!a) continue;
                    alphaSum += a;
                    redSum += (pixel >> 16) & 0xFF;
                    greenSum += (pixel >> 8) & 0xFF;
                    blueSum += pixel & 0xFF;
                }
            }

            const uint32_t alpha = alphaSum / samples;
            if (!alpha) {
                out[y * size + x] = 0;
                continue;
            }

            const uint32_t red = (redSum / samples) * alpha / 255;
            const uint32_t green = (greenSum / samples) * alpha / 255;
            const uint32_t blue = (blueSum / samples) * alpha / 255;
            out[y * size + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
        }
    }

    const int maskPitch = ((size + 15) / 16) * 2;
    BYTE maskBits[256] = {};
    for (int y = 0; y < size && y * maskPitch + maskPitch <= (int)sizeof(maskBits); ++y) {
        for (int x = 0; x < size; ++x) {
            if (((out[y * size + x] >> 24) & 0xFF) < 32) {
                maskBits[y * maskPitch + (x / 8)] |= (BYTE)(1 << (7 - (x % 8)));
            }
        }
    }
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, maskBits);

    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = output;
    info.hbmMask = maskBitmap;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(canvas);
    DeleteObject(output);
    DeleteObject(maskBitmap);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return icon;
}

} // namespace Tray
