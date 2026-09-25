#include "osd_window.h"
#include <dwmapi.h>
#include <strsafe.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "theme.h"

namespace Osd {

static HWND g_hOsdWnd = NULL;
static WCHAR g_textLine1[64] = {0};
static WCHAR g_textLine2[64] = {0};
static WCHAR g_textLine3[64] = {0};
static BYTE g_osdAlpha = 0;

static const UINT_PTR TIMER_OSD_HIDE = 101;
static const UINT_PTR TIMER_OSD_FADE = 102;
static const BYTE OSD_ALPHA_MAX = 240;

// Dark-mode cards use the same DWM acrylic backdrop as the tray menu. Light
// mode uses an opaque GDI surface to keep its text visible on affected systems.
enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};

struct ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

static inline int ScaleDpi(int val, int dpi) {
    return MulDiv(val, dpi, 96);
}

// Signed distance of a rounded rectangle, negative inside.
static float RoundedRectDistance(float x, float y, float left, float top, float right, float bottom, float radius) {
    const float centerX = (left + right) * 0.5f;
    const float centerY = (top + bottom) * 0.5f;
    const float halfWidth = (right - left) * 0.5f;
    const float halfHeight = (bottom - top) * 0.5f;
    const float dx = std::max(std::fabs(x - centerX) - (halfWidth - radius), 0.0f);
    const float dy = std::max(std::fabs(y - centerY) - (halfHeight - radius), 0.0f);
    return std::sqrt(dx * dx + dy * dy) - radius;
}

// Draws the card outline with per pixel coverage so the rounded corners stay
// smooth, then alpha blends it onto the acrylic surface.
static void DrawAntialiasedBorder(HDC target, int width, int height, float radius,
                                  float thickness, COLORREF color) {
    HDC memDc = CreateCompatibleDC(target);
    if (!memDc) return;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(memDc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        DeleteDC(memDc);
        return;
    }

    uint32_t* buffer = (uint32_t*)pixels;
    memset(buffer, 0, (size_t)width * height * sizeof(uint32_t));

    const float inset = thickness * 0.5f;
    const float half = thickness * 0.5f;
    const float red = (float)GetRValue(color);
    const float green = (float)GetGValue(color);
    const float blue = (float)GetBValue(color);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float distance = RoundedRectDistance((float)x + 0.5f, (float)y + 0.5f,
                                                       inset, inset, width - inset, height - inset, radius);
            const float coverage = std::clamp(half + 0.5f - std::fabs(distance), 0.0f, 1.0f);
            if (coverage <= 0.0f) continue;

            const uint32_t alpha = (uint32_t)std::lround(coverage * 255.0f);
            const uint32_t premulRed = (uint32_t)std::lround(red * coverage);
            const uint32_t premulGreen = (uint32_t)std::lround(green * coverage);
            const uint32_t premulBlue = (uint32_t)std::lround(blue * coverage);
            buffer[y * width + x] = (alpha << 24) | (premulRed << 16) | (premulGreen << 8) | premulBlue;
        }
    }

    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(target, 0, 0, width, height, memDc, 0, 0, width, height, blend);

    SelectObject(memDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDc);
}

static UINT GetCurrentWindowDpi(HWND hWnd) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
        pfnGetDpiForWindow fnGetDpi = (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
        if (fnGetDpi && hWnd) {
            UINT d = fnGetDpi(hWnd);
            if (d > 0) return d;
        }
    }
    return 96;
}

static void ApplyCardStyle(HWND hWnd, bool isDark) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        auto fnSetWindowCompositionAttribute =
            (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
        if (fnSetWindowCompositionAttribute) {
            ACCENT_POLICY policy = {};
            // Acrylic can be composed over the redirected GDI surface on some
            // Windows light-theme configurations, leaving the card visible but
            // hiding its text. Keep the light card opaque so its GDI content is
            // always presented; preserve acrylic for dark mode.
            policy.AccentState = isDark ? ACCENT_ENABLE_ACRYLICBLURBEHIND : ACCENT_DISABLED;
            policy.AccentFlags = 0;
            policy.GradientColor = isDark ? 0xCC1A1B20 : 0xD8F8F9FA; // AABBGGRR
            WINDOWCOMPOSITIONATTRIBDATA data = { 19, &policy, sizeof(policy) };
            fnSetWindowCompositionAttribute(hWnd, &data);
        }
    }

    BOOL dark = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, 20, &dark, sizeof(dark));   // DWMWA_USE_IMMERSIVE_DARK_MODE
    DWORD corner = 2;                                       // DWMWCP_ROUND, keeps the corners antialiased
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));
}

static LRESULT CALLBACK OsdWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            const UINT dpi = GetCurrentWindowDpi(hWnd);
            auto S = [dpi](int v) { return ScaleDpi(v, dpi); };
            const bool darkMode = Theme::IsSystemDarkMode();

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            const COLORREF bgCol = darkMode ? RGB(24, 26, 32) : RGB(248, 248, 252);
            // A clearly visible 1 px frame with antialiased corners.
            const COLORREF borderCol = darkMode ? RGB(104, 112, 130) : RGB(186, 192, 204);
            const COLORREF titleCol = darkMode ? RGB(235, 240, 248) : RGB(30, 35, 45);
            const COLORREF subCol = darkMode ? RGB(180, 192, 210) : RGB(90, 100, 115);
            const COLORREF accentCol = darkMode ? RGB(68, 214, 44) : RGB(22, 163, 74);

            HBRUSH bgBrush = CreateSolidBrush(bgCol);
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            // Radius matches the DWM corner rounding so the frame follows the card.
            DrawAntialiasedBorder(memDC, rc.right, rc.bottom,
                                  (float)std::min(S(8), (int)(rc.bottom / 2)),
                                  (float)std::max(1, S(1)), borderCol);

            RECT rcBox = rc;
            InflateRect(&rcBox, -S(2), -S(2));
            SetBkMode(memDC, TRANSPARENT);

            const bool hasLine3 = (g_textLine3[0] != 0);

            HFONT hFontBig = CreateFontW(
                -S(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI");
            HGDIOBJ oldFont = SelectObject(memDC, hFontBig);
            SetTextColor(memDC, titleCol);

            RECT rcTop = rcBox;
            rcTop.top = rcBox.top + S(8);
            rcTop.bottom = rcTop.top + S(26);
            DrawTextW(memDC, g_textLine1, -1, &rcTop, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            HFONT hFontMid = CreateFontW(
                -S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI");
            SelectObject(memDC, hFontMid);
            SetTextColor(memDC, subCol);

            RECT rcMid = rcBox;
            rcMid.top = rcTop.bottom + S(2);
            rcMid.bottom = rcMid.top + S(22);
            DrawTextW(memDC, g_textLine2, -1, &rcMid, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            HFONT hFontSub = NULL;
            if (hasLine3) {
                hFontSub = CreateFontW(
                    -S(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI");
                SelectObject(memDC, hFontSub);
                SetTextColor(memDC, accentCol);

                RECT rcBot = rcBox;
                rcBot.top = rcMid.bottom + S(2);
                rcBot.bottom = rcBot.top + S(22);
                DrawTextW(memDC, g_textLine3, -1, &rcBot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            DeleteObject(hFontBig);
            DeleteObject(hFontMid);
            if (hFontSub) DeleteObject(hFontSub);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_OSD_HIDE) {
                KillTimer(hWnd, TIMER_OSD_HIDE);
                ShowWindow(hWnd, SW_HIDE);
                g_osdAlpha = 0;
            }
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool Init(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OsdWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RazerOsdPopupWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hOsdWnd = CreateWindowExW(
        // Deliberately not layered: DWM only applies the acrylic backdrop to
        // regular window surfaces, and the menu uses exactly the same setup.
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName,
        L"RazerOSD",
        WS_POPUP,
        0, 0, 300, 96,
        NULL, NULL, hInstance, NULL
    );

    return (g_hOsdWnd != NULL);
}

void Cleanup() {
    if (g_hOsdWnd && IsWindow(g_hOsdWnd)) {
        DestroyWindow(g_hOsdWnd);
        g_hOsdWnd = NULL;
    }
}

HWND GetWindowHandle() {
    return g_hOsdWnd;
}

void Show(const WCHAR* line1, const WCHAR* line2, const WCHAR* line3) {
    if (!g_hOsdWnd) return;

    StringCchCopyW(g_textLine1, ARRAYSIZE(g_textLine1), line1 ? line1 : L"");
    StringCchCopyW(g_textLine2, ARRAYSIZE(g_textLine2), line2 ? line2 : L"");
    if (line3 && line3[0]) {
        StringCchCopyW(g_textLine3, ARRAYSIZE(g_textLine3), line3);
    } else {
        g_textLine3[0] = 0;
    }

    // Determine target monitor based on cursor position
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    RECT rcWork = mi.rcWork;

    UINT dpi = GetCurrentWindowDpi(g_hOsdWnd);
    auto S = [dpi](int v) { return ScaleDpi(v, dpi); };

    // Dynamic width & height calculation via DT_CALCRECT
    HDC screenDC = GetDC(NULL);
    HFONT hFontBig = CreateFontW(
        -S(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
    );
    HFONT hFontMid = CreateFontW(
        -S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
    );
    HFONT hFontSub = CreateFontW(
        -S(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Microsoft YaHei UI"
    );

    RECT r1 = {0, 0, 0, 0};
    RECT r2 = {0, 0, 0, 0};
    RECT r3 = {0, 0, 0, 0};

    HGDIOBJ oldF = SelectObject(screenDC, hFontBig);
    DrawTextW(screenDC, g_textLine1, -1, &r1, DT_CALCRECT | DT_SINGLELINE);

    SelectObject(screenDC, hFontMid);
    DrawTextW(screenDC, g_textLine2, -1, &r2, DT_CALCRECT | DT_SINGLELINE);

    int maxTextW = std::max(r1.right - r1.left, r2.right - r2.left);

    bool hasLine3 = (g_textLine3[0] != 0);
    if (hasLine3) {
        SelectObject(screenDC, hFontSub);
        DrawTextW(screenDC, g_textLine3, -1, &r3, DT_CALCRECT | DT_SINGLELINE);
        maxTextW = std::max(maxTextW, (int)(r3.right - r3.left));
    }

    SelectObject(screenDC, oldF);
    DeleteObject(hFontBig);
    DeleteObject(hFontMid);
    DeleteObject(hFontSub);
    ReleaseDC(NULL, screenDC);

    // Add safe horizontal padding (18px each side)
    int targetW = maxTextW + S(36);
    targetW = std::max(targetW, S(210));
    targetW = std::min(targetW, S(420));

    int targetH = hasLine3 ? S(88) : S(66);

    int x = rcWork.left + ((rcWork.right - rcWork.left) - targetW) / 2;
    int y = rcWork.bottom - targetH - S(70);

    KillTimer(g_hOsdWnd, TIMER_OSD_HIDE);
    KillTimer(g_hOsdWnd, TIMER_OSD_FADE);

    ApplyCardStyle(g_hOsdWnd, Theme::IsSystemDarkMode());

    g_osdAlpha = OSD_ALPHA_MAX;
    SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, targetW, targetH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hOsdWnd, NULL, TRUE);
    UpdateWindow(g_hOsdWnd);

    SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, 1600, NULL);
}

void ShowDeviceStatus(const Device::State& state) {
    WCHAR l1[96], l2[96], l3[160];
    StringCchCopyW(l1, ARRAYSIZE(l1), state.modelName[0] ? state.modelName : L"Razer 鼠标");
    if (!state.isConnected) {
        if (state.receiverPresent) {
            Show(l1, L"接收器已就绪 · 鼠标未连接", L"打开鼠标电源或移动鼠标唤醒后会自动恢复");
        } else {
            Show(l1, L"未连接 · 请检查鼠标或接收器", L"检测到设备后会显示电量、DPI 和轮询率");
        }
        return;
    }
    StringCchPrintfW(l2, ARRAYSIZE(l2), L"已连接 · %s", state.isWired ? L"USB 有线" : L"2.4 GHz 无线");
    WCHAR battery[40], dpi[48], polling[40];
    if (!state.hasBattery) StringCchCopyW(battery, ARRAYSIZE(battery), L"电量 --");
    else StringCchPrintfW(battery, ARRAYSIZE(battery), L"电量 %d%%%s", state.battery, state.isCharging ? L" · 充电中" : L"");
    if (!state.hasDpi) StringCchCopyW(dpi, ARRAYSIZE(dpi), L"DPI --");
    else StringCchPrintfW(dpi, ARRAYSIZE(dpi), L"DPI %d", state.dpiX);
    if (!state.hasPollingRate) StringCchCopyW(polling, ARRAYSIZE(polling), L"轮询率 --");
    else StringCchPrintfW(polling, ARRAYSIZE(polling), L"轮询率 %d Hz", state.pollingHz);
    StringCchPrintfW(l3, ARRAYSIZE(l3), L"%s   |   %s   |   %s", battery, dpi, polling);
    Show(l1, l2, l3);
}

} // namespace Osd
