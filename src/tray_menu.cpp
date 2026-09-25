#include "tray_menu.h"
#include "osd_window.h"
#include <dwmapi.h>
#include <strsafe.h>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <vector>
#include "acrylic_surface.h"
#include "theme.h"

namespace Tray {

static const WCHAR* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* APP_NAME = L"razer-tray";

static HWND g_hParentAppWnd = NULL;
static HWND g_hAcrylicMenu = NULL;
static HWND g_hAcrylicSubMenu = NULL;
static HHOOK g_hMenuMouseHook = NULL;
static bool g_bModalLoop = false;

static int g_curDpi = 96;
static bool g_curDark = false;
static bool g_acrylicEnabled = false;
static int g_activeSubId = 0; // 0: None, 1: Polling Hz, 2: Sleep Slider, 3: Battery, 4: Theme
static int g_mainHover = -1;
static int g_subHover = -1;

static const Theme::Mode THEME_MODES[] = {
    Theme::Mode::Light,
    Theme::Mode::Dark,
    Theme::Mode::FollowSystem
};
static const WCHAR* THEME_LABELS[] = { L"浅色", L"深色", L"跟随系统" };

static int GetThemeModeIndex(Theme::Mode mode) {
    for (int i = 0; i < (int)ARRAYSIZE(THEME_MODES); ++i) {
        if (THEME_MODES[i] == mode) return i;
    }
    return 2;
}

static const WCHAR* GetThemeModeLabel(Theme::Mode mode) {
    return THEME_LABELS[GetThemeModeIndex(mode)];
}

// Sleep slider state
static bool g_isDraggingSlider = false;
static int g_sliderVal = 10;
static const int PRESET_SLEEP[] = { 5, 10, 15, 30, 60 };

static inline int S(int val) {
    return MulDiv(val, g_curDpi, 96);
}

// Windows 11 / 10 Acrylic & Theme Hooks
typedef enum _ACCENT_STATE {
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
} ACCENT_STATE;

typedef struct _ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
} ACCENT_POLICY;

typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;

typedef BOOL (WINAPI *pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
typedef BOOL (WINAPI *pfnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *pfnFlushMenuThemes)();

static pfnSetWindowCompositionAttribute fnSetWindowCompositionAttribute = NULL;
static pfnAllowDarkModeForWindow fnAllowDarkModeForWindow = NULL;
static pfnFlushMenuThemes fnFlushMenuThemes = NULL;

void InitTheme() {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        fnSetWindowCompositionAttribute = (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
    }
    HMODULE hUx = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUx) {
        fnAllowDarkModeForWindow = (pfnAllowDarkModeForWindow)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
        fnFlushMenuThemes = (pfnFlushMenuThemes)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    }
}

static void ApplyModernWindowStyle(HWND hWnd, bool isDark, int w = 0, int h = 0) {
    if (fnAllowDarkModeForWindow) {
        fnAllowDarkModeForWindow(hWnd, isDark ? TRUE : FALSE);
    }
    BOOL darkVal = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hWnd, 20, &darkVal, sizeof(darkVal)); // DWMWA_USE_IMMERSIVE_DARK_MODE

    // Request Windows 11 rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33)
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hWnd, 33, &corner, sizeof(corner));

    // Also clip window region with rounded corners for compatibility
    RECT rc = {0};
    GetWindowRect(hWnd, &rc);
    int width = (w > 0) ? w : (rc.right - rc.left);
    int height = (h > 0) ? h : (rc.bottom - rc.top);
    if (width > 0 && height > 0) {
        HRGN hRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, S(12), S(12));
        SetWindowRgn(hWnd, hRgn, TRUE);
    }

    if (fnSetWindowCompositionAttribute) {
        ACCENT_POLICY policy = {};
        policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
        policy.AccentFlags = 0; // No DWM system rectangular border (eliminated white streaks)
        policy.GradientColor = isDark ? 0xCC1A1B20 : 0xD8F8F9FA; // AABBGGRR
        WINDOWCOMPOSITIONATTRIBDATA data = { 19, &policy, sizeof(policy) };
        g_acrylicEnabled = fnSetWindowCompositionAttribute(hWnd, &data) != FALSE;
    } else {
        g_acrylicEnabled = false;
    }
}

static const WCHAR* SETTINGS_KEY = L"Software\\razer-tray";
static int g_batteryDisplayStyle = -1;

int GetBatteryDisplayStyle() {
    if (g_batteryDisplayStyle < 0) {
        g_batteryDisplayStyle = Tray::BATTERY_STYLE_RING;
        HKEY hKey;
        DWORD val = 0, size = sizeof(DWORD), type = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExW(hKey, L"BatteryStyle", NULL, &type, (LPBYTE)&val, &size) == ERROR_SUCCESS) {
                g_batteryDisplayStyle = (int)(val % 3);
            }
            RegCloseKey(hKey);
        }
    }
    return g_batteryDisplayStyle;
}

void SetBatteryDisplayStyle(int style) {
    g_batteryDisplayStyle = std::clamp(style, 0, 2);
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = (DWORD)g_batteryDisplayStyle;
        RegSetValueExW(hKey, L"BatteryStyle", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

bool IsAutoRunEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path), type = 0;
        LSTATUS st = RegQueryValueExW(hKey, APP_NAME, NULL, &type, (LPBYTE)path, &len);
        RegCloseKey(hKey);
        return (st == ERROR_SUCCESS);
    }
    return false;
}

void ToggleAutoRun() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (IsAutoRunEnabled()) {
            RegDeleteValueW(hKey, APP_NAME);
        } else {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(hKey, APP_NAME, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(WCHAR)));
        }
        RegCloseKey(hKey);
    }
}

// --------------------------------------------------------------------------
// Submenu Window (Polling Rate & Sleep Slider)
// --------------------------------------------------------------------------

static void DismissSubMenu() {
    if (g_hAcrylicSubMenu) {
        HWND h = g_hAcrylicSubMenu;
        g_hAcrylicSubMenu = NULL;
        DestroyWindow(h);
    }
    g_activeSubId = 0;
    g_subHover = -1;
    g_isDraggingSlider = false;
}

void DismissAllMenus() {
    if (g_hMenuMouseHook) {
        UnhookWindowsHookEx(g_hMenuMouseHook);
        g_hMenuMouseHook = NULL;
    }
    DismissSubMenu();
    if (g_hAcrylicMenu) {
        HWND h = g_hAcrylicMenu;
        g_hAcrylicMenu = NULL;
        DestroyWindow(h);
    }
    g_bModalLoop = false;
    if (g_hParentAppWnd) {
        PostMessageW(g_hParentAppWnd, WM_NULL, 0, 0);
    }
}

static LRESULT CALLBACK MenuMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || 
            wParam == WM_NCLBUTTONDOWN || wParam == WM_NCRBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN) {
            MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
            POINT pt = p->pt;
            RECT rM = {0}, rS = {0};
            if (g_hAcrylicMenu && IsWindow(g_hAcrylicMenu)) GetWindowRect(g_hAcrylicMenu, &rM);
            if (g_hAcrylicSubMenu && IsWindow(g_hAcrylicSubMenu)) GetWindowRect(g_hAcrylicSubMenu, &rS);
            bool inM = (g_hAcrylicMenu && IsWindow(g_hAcrylicMenu)) && PtInRect(&rM, pt);
            bool inS = (g_hAcrylicSubMenu && IsWindow(g_hAcrylicSubMenu)) && PtInRect(&rS, pt);
            if (!inM && !inS) {
                DismissAllMenus();
            }
        }
    }
    return CallNextHookEx(g_hMenuMouseHook, nCode, wParam, lParam);
}

// Submenu Window Procedure
static LRESULT CALLBACK AcrylicSubWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND hOther = (HWND)lParam;
                if (hOther != g_hAcrylicMenu && hOther != g_hAcrylicSubMenu) {
                    DismissAllMenus();
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEWHEEL: {
            if (g_activeSubId == 2) { // Sleep Slider
                short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int step = (delta > 0) ? 1 : -1;
                if (GetKeyState(VK_SHIFT) & 0x8000) step *= 5;
                g_sliderVal = std::clamp(g_sliderVal + step, 2, 120);
                Device::SetSleepTimeout(g_sliderVal);

                Device::State st = Device::GetCurrentState();
                WCHAR l1[64], l2[64], l3[64];
                StringCchPrintfW(l1, ARRAYSIZE(l1), L"休眠时间: %d 分钟", g_sliderVal);
                StringCchPrintfW(l2, ARRAYSIZE(l2), L"设置已即时生效并已保存");
                const WCHAR* modeStr = st.isWired ? L"USB" : L"2.4G";
                const WCHAR* batIcon = st.isCharging ? L"⚡" : L"🔋";
                StringCchPrintfW(l3, ARRAYSIZE(l3), L"%s  |  第 %d 档  |  %s %d%%  |  %d Hz",
                    modeStr, st.dpiLevel, batIcon, st.battery, st.pollingHz);
                Osd::Show(l1, l2, l3);

                InvalidateRect(hWnd, NULL, FALSE);
                if (g_hAcrylicMenu) InvalidateRect(g_hAcrylicMenu, NULL, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);
            RECT rc;
            GetClientRect(hWnd, &rc);

            if (g_activeSubId == 2) {
                // Sleep Slider Track interaction
                int trackX0 = S(20);
                int trackX1 = rc.right - S(20);
                int trackY = S(58);

                if (my >= trackY - S(14) && my <= trackY + S(14) && mx >= trackX0 - S(8) && mx <= trackX1 + S(8)) {
                    g_isDraggingSlider = true;
                    SetCapture(hWnd);
                    double ratio = (double)(mx - trackX0) / (double)(trackX1 - trackX0);
                    ratio = std::clamp(ratio, 0.0, 1.0);
                    g_sliderVal = std::clamp(2 + (int)(ratio * (120 - 2) + 0.5), 2, 120);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }

                // Preset Chips interaction (y = S(98), h = S(26))
                int chipY0 = S(96);
                int chipY1 = chipY0 + S(26);
                if (my >= chipY0 && my <= chipY1) {
                    int count = 5;
                    int chipW = (rc.right - S(40) - (count - 1) * S(6)) / count;
                    int curX = S(20);
                    for (int i = 0; i < count; i++) {
                        if (mx >= curX && mx <= curX + chipW) {
                            g_sliderVal = PRESET_SLEEP[i];
                            Device::SetSleepTimeout(g_sliderVal);

                            Device::State st = Device::GetCurrentState();
                            WCHAR l1[64], l2[64], l3[64];
                            StringCchPrintfW(l1, ARRAYSIZE(l1), L"休眠时间: %d 分钟", g_sliderVal);
                            StringCchPrintfW(l2, ARRAYSIZE(l2), L"设置已即时生效并已保存");
                            const WCHAR* modeStr = st.isWired ? L"USB" : L"2.4G";
                            const WCHAR* batIcon = st.isCharging ? L"⚡" : L"🔋";
                            StringCchPrintfW(l3, ARRAYSIZE(l3), L"%s  |  第 %d 档  |  %s %d%%  |  %d Hz",
                                modeStr, st.dpiLevel, batIcon, st.battery, st.pollingHz);
                            Osd::Show(l1, l2, l3);

                            InvalidateRect(hWnd, NULL, FALSE);
                            if (g_hAcrylicMenu) InvalidateRect(g_hAcrylicMenu, NULL, FALSE);
                            return 0;
                        }
                        curX += chipW + S(6);
                    }
                }
            }
            break;
        }

        case WM_MOUSEMOVE: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);
            RECT rc;
            GetClientRect(hWnd, &rc);

            if (g_activeSubId == 2 && g_isDraggingSlider) {
                int trackX0 = S(20);
                int trackX1 = rc.right - S(20);
                double ratio = (double)(mx - trackX0) / (double)(trackX1 - trackX0);
                ratio = std::clamp(ratio, 0.0, 1.0);
                int newVal = std::clamp(2 + (int)(ratio * (120 - 2) + 0.5), 2, 120);
                if (newVal != g_sliderVal) {
                    g_sliderVal = newVal;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }

            if (g_activeSubId == 1 || g_activeSubId == 3 || g_activeSubId == 4) { // List submenus
                int count = (g_activeSubId == 1) ? 7 : 3;
                int itemH = S(28);
                int y = S(8);
                int newH = -1;
                for (int i = 0; i < count; i++) {
                    if (my >= y && my < y + itemH) {
                        newH = i;
                        break;
                    }
                    y += itemH;
                }
                if (newH != g_subHover) {
                    g_subHover = newH;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g_activeSubId == 2 && g_isDraggingSlider) {
                g_isDraggingSlider = false;
                ReleaseCapture();

                Device::SetSleepTimeout(g_sliderVal);

                Device::State st = Device::GetCurrentState();
                WCHAR l1[64], l2[64], l3[64];
                StringCchPrintfW(l1, ARRAYSIZE(l1), L"休眠时间: %d 分钟", g_sliderVal);
                StringCchPrintfW(l2, ARRAYSIZE(l2), L"设置已即时生效并已保存");
                const WCHAR* modeStr = st.isWired ? L"USB" : L"2.4G";
                const WCHAR* batIcon = st.isCharging ? L"⚡" : L"🔋";
                StringCchPrintfW(l3, ARRAYSIZE(l3), L"%s  |  第 %d 档  |  %s %d%%  |  %d Hz",
                    modeStr, st.dpiLevel, batIcon, st.battery, st.pollingHz);
                Osd::Show(l1, l2, l3);

                if (g_hAcrylicMenu) InvalidateRect(g_hAcrylicMenu, NULL, FALSE);
                return 0;
            }

            if (g_activeSubId == 3 && g_subHover >= 0) {
                static const WCHAR* labels[] = { L"环形电量", L"电池图标", L"数字显示" };
                const int chosen = std::clamp(g_subHover, 0, 2);
                SetBatteryDisplayStyle(chosen);
                DismissAllMenus();
                Device::RequestRefresh();
                Osd::Show(labels[chosen], L"托盘图标已切换为所选显示方式", L"右键菜单可随时再次切换");
                return 0;
            }

            if (g_activeSubId == 4 && g_subHover >= 0) {
                const int chosen = std::clamp(g_subHover, 0, 2);
                if (!Theme::SetMode(THEME_MODES[chosen])) {
                    Osd::Show(L"主题设置失败", L"无法保存主题选项", L"请稍后重试");
                    DismissAllMenus();
                    return 0;
                }

                if (g_hParentAppWnd) PostMessageW(g_hParentAppWnd, WM_SETTINGCHANGE, 0, 0);
                DismissAllMenus();
                Osd::Show(L"主题显示", THEME_LABELS[chosen], L"设置已保存");
                return 0;
            }

            if (g_activeSubId == 1 && g_subHover >= 0) {
                const int hzVals[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
                int hz = hzVals[g_subHover];
                DismissAllMenus();

                Device::SetPollingRate(hz);

                Device::State st = Device::GetCurrentState();
                WCHAR l1[64], l2[64], l3[64];
                StringCchPrintfW(l1, ARRAYSIZE(l1), L"轮询率: %d Hz", hz);
                StringCchPrintfW(l2, ARRAYSIZE(l2), L"设置已即时生效");
                const WCHAR* modeStr = st.isWired ? L"USB" : L"2.4G";
                const WCHAR* batIcon = st.isCharging ? L"⚡" : L"🔋";
                StringCchPrintfW(l3, ARRAYSIZE(l3), L"%s  |  第 %d 档  |  %s %d%%  |  %d Hz",
                    modeStr, st.dpiLevel, batIcon, st.battery, hz);
                Osd::Show(l1, l2, l3);
                return 0;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            COLORREF bgCol = g_curDark ? RGB(24, 26, 32) : RGB(248, 248, 252);
            COLORREF borderCol = g_curDark ? RGB(50, 54, 65) : RGB(218, 222, 230);
            COLORREF hoverCol = g_curDark ? RGB(52, 58, 72) : RGB(228, 232, 242);
            COLORREF textCol = g_curDark ? RGB(235, 240, 248) : RGB(30, 35, 45);
            COLORREF checkCol = g_curDark ? RGB(68, 214, 44) : RGB(38, 150, 34);
            COLORREF mutedCol = g_curDark ? RGB(140, 150, 165) : RGB(120, 130, 145);
            COLORREF trackBgCol = g_curDark ? RGB(45, 50, 60) : RGB(215, 220, 230);

            AcrylicSurface::Buffer surface;
            if (!AcrylicSurface::Create(hdc, rc.right, rc.bottom, surface)) {
                HBRUSH fallback = CreateSolidBrush(bgCol);
                FillRect(hdc, &rc, fallback);
                DeleteObject(fallback);
                EndPaint(hWnd, &ps);
                return 0;
            }
            HDC memDC = surface.dc;

            HBRUSH bgBrush = CreateSolidBrush(bgCol);
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            HPEN borderPen = CreatePen(PS_SOLID, 1, borderCol);
            HGDIOBJ oldPen = SelectObject(memDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
            RoundRect(memDC, 0, 0, rc.right, rc.bottom, S(12), S(12));
            SelectObject(memDC, oldPen);
            DeleteObject(borderPen);

            HFONT hFontNormal = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HFONT hFontBold = CreateFontW(-S(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HFONT hFontSmall = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            SetBkMode(memDC, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(memDC, hFontNormal);

            if (g_activeSubId == 1) { // Polling Rate list
                const WCHAR* labels[] = { L"125 Hz", L"250 Hz", L"500 Hz", L"1000 Hz", L"2000 Hz", L"4000 Hz", L"8000 Hz" };
                const int hzVals[] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
                int currentHz = Device::GetCurrentState().pollingHz;

                int count = 7;
                int itemH = S(28);
                int y = S(8);
                for (int i = 0; i < count; i++) {
                    RECT rItem = { S(6), y, rc.right - S(6), y + itemH };
                    if (i == g_subHover) {
                        HBRUSH hH = CreateSolidBrush(hoverCol);
                        HPEN hP = CreatePen(PS_SOLID, 1, hoverCol);
                        HGDIOBJ oP = SelectObject(memDC, hP);
                        HGDIOBJ oB = SelectObject(memDC, hH);
                        RoundRect(memDC, rItem.left, rItem.top + 1, rItem.right, rItem.bottom - 1, S(6), S(6));
                        SelectObject(memDC, oP);
                        SelectObject(memDC, oB);
                        DeleteObject(hP);
                        DeleteObject(hH);
                    }

                    SetTextColor(memDC, textCol);
                    RECT rText = { S(14), y, rc.right - S(32), y + itemH };
                    DrawTextW(memDC, labels[i], -1, &rText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    if (hzVals[i] == currentHz) {
                        SetTextColor(memDC, checkCol);
                        RECT rCheck = { rc.right - S(28), y, rc.right - S(10), y + itemH };
                        DrawTextW(memDC, L"✓", -1, &rCheck, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                    y += itemH;
                }
            } else if (g_activeSubId == 3 || g_activeSubId == 4) { // Battery style / Theme
                static const WCHAR* batteryLabels[] = { L"环形电量", L"电池图标", L"数字显示" };
                const bool isThemeMenu = (g_activeSubId == 4);
                const int current = isThemeMenu
                    ? GetThemeModeIndex(Theme::GetMode())
                    : GetBatteryDisplayStyle();

                int itemH = S(28);
                int y = S(8);
                for (int i = 0; i < 3; ++i) {
                    RECT rItem = { S(6), y, rc.right - S(6), y + itemH };
                    if (i == g_subHover) {
                        HBRUSH hH = CreateSolidBrush(hoverCol);
                        HPEN hP = CreatePen(PS_SOLID, 1, hoverCol);
                        HGDIOBJ oP = SelectObject(memDC, hP);
                        HGDIOBJ oB = SelectObject(memDC, hH);
                        RoundRect(memDC, rItem.left, rItem.top + 1, rItem.right, rItem.bottom - 1, S(6), S(6));
                        SelectObject(memDC, oP);
                        SelectObject(memDC, oB);
                        DeleteObject(hP);
                        DeleteObject(hH);
                    }

                    SetTextColor(memDC, textCol);
                    RECT rText = { S(14), y, rc.right - S(32), y + itemH };
                    const WCHAR* label = isThemeMenu ? THEME_LABELS[i] : batteryLabels[i];
                    DrawTextW(memDC, label, -1, &rText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    if (i == current) {
                        SetTextColor(memDC, checkCol);
                        RECT rCheck = { rc.right - S(28), y, rc.right - S(10), y + itemH };
                        DrawTextW(memDC, L"✓", -1, &rCheck, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                    y += itemH;
                }
            } else if (g_activeSubId == 2) { // Modern 2~120 min Interactive Sleep Slider
                // Header Row: Label + Value
                SelectObject(memDC, hFontNormal);
                SetTextColor(memDC, textCol);
                RECT rHeader = { S(16), S(14), rc.right - S(80), S(38) };
                DrawTextW(memDC, L"休眠等待时间", -1, &rHeader, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                SelectObject(memDC, hFontBold);
                SetTextColor(memDC, checkCol);
                WCHAR szVal[32];
                StringCchPrintfW(szVal, ARRAYSIZE(szVal), L"%d 分钟", g_sliderVal);
                RECT rVal = { rc.right - S(90), S(14), rc.right - S(16), S(38) };
                DrawTextW(memDC, szVal, -1, &rVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

                // Slider Track
                int trackX0 = S(20);
                int trackX1 = rc.right - S(20);
                int trackY = S(58);
                int trackH = S(4);

                double ratio = (double)(g_sliderVal - 2) / (double)(120 - 2);
                ratio = std::clamp(ratio, 0.0, 1.0);
                int thumbX = trackX0 + (int)(ratio * (trackX1 - trackX0));

                // Draw background track (inactive portion)
                HBRUSH hTrackBg = CreateSolidBrush(trackBgCol);
                HGDIOBJ oP = SelectObject(memDC, GetStockObject(NULL_PEN));
                HGDIOBJ oB = SelectObject(memDC, hTrackBg);
                RoundRect(memDC, thumbX, trackY - trackH / 2, trackX1, trackY + trackH / 2 + 1, S(4), S(4));

                // Draw filled track (active portion)
                HBRUSH hTrackActive = CreateSolidBrush(checkCol);
                SelectObject(memDC, hTrackActive);
                RoundRect(memDC, trackX0, trackY - trackH / 2, thumbX, trackY + trackH / 2 + 1, S(4), S(4));

                // Draw thumb
                int thumbR = S(8);
                HBRUSH hThumbBg = CreateSolidBrush(g_curDark ? RGB(255, 255, 255) : RGB(255, 255, 255));
                HPEN hThumbBorder = CreatePen(PS_SOLID, 2, checkCol);
                SelectObject(memDC, hThumbBorder);
                SelectObject(memDC, hThumbBg);
                Ellipse(memDC, thumbX - thumbR, trackY - thumbR, thumbX + thumbR, trackY + thumbR);

                DeleteObject(hThumbBg);
                DeleteObject(hThumbBorder);
                DeleteObject(hTrackActive);
                DeleteObject(hTrackBg);

                // Preset Chips Row
                SelectObject(memDC, hFontSmall);
                int count = 5;
                int chipW = (rc.right - S(40) - (count - 1) * S(6)) / count;
                int curX = S(20);
                int chipY0 = S(96);
                int chipY1 = chipY0 + S(26);

                for (int i = 0; i < count; i++) {
                    RECT rChip = { curX, chipY0, curX + chipW, chipY1 };
                    bool isCur = (g_sliderVal == PRESET_SLEEP[i]);

                    HBRUSH hChipBg = CreateSolidBrush(isCur ? checkCol : (g_curDark ? RGB(36, 40, 50) : RGB(232, 236, 244)));
                    HPEN hChipPen = CreatePen(PS_SOLID, 1, isCur ? checkCol : borderCol);
                    SelectObject(memDC, hChipPen);
                    SelectObject(memDC, hChipBg);
                    RoundRect(memDC, rChip.left, rChip.top, rChip.right, rChip.bottom, S(6), S(6));
                    DeleteObject(hChipPen);
                    DeleteObject(hChipBg);

                    SetTextColor(memDC, isCur ? (g_curDark ? RGB(10, 12, 16) : RGB(255, 255, 255)) : textCol);
                    WCHAR szChip[16];
                    StringCchPrintfW(szChip, ARRAYSIZE(szChip), L"%d分", PRESET_SLEEP[i]);
                    DrawTextW(memDC, szChip, -1, &rChip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    curX += chipW + S(6);
                }
            }

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBrush);
            DeleteObject(hFontNormal);
            DeleteObject(hFontBold);
            DeleteObject(hFontSmall);

            AcrylicSurface::Present(hdc, surface, bgCol, g_acrylicEnabled);
            AcrylicSurface::Destroy(surface);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowSubMenuWindow(int subType, RECT rItemScreen) {
    if (g_activeSubId == subType && g_hAcrylicSubMenu) return;
    DismissSubMenu();
    g_activeSubId = subType;

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(g_hAcrylicMenu, GWLP_HINSTANCE);
    static bool s_subRegistered = false;
    if (!s_subRegistered) {
        WNDCLASSEXW wcSub = {0};
        wcSub.cbSize = sizeof(wcSub);
        wcSub.lpfnWndProc = AcrylicSubWndProc;
        wcSub.hInstance = hInst;
        wcSub.lpszClassName = L"RazerModernSubMenuWnd";
        wcSub.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wcSub);
        s_subRegistered = true;
    }

    int subW = (subType == 1) ? S(140) : ((subType == 3 || subType == 4) ? S(168) : S(260));
    int subH = (subType == 1) ? S(212) : ((subType == 3 || subType == 4) ? S(100) : S(138));

    HMONITOR hMon = MonitorFromPoint({ rItemScreen.left, rItemScreen.top }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    int subX = rItemScreen.right + S(4);
    if (subX + subW > mi.rcWork.right) {
        subX = rItemScreen.left - subW - S(4);
    }
    int subY = rItemScreen.top - S(4);
    if (subY + subH > mi.rcWork.bottom) {
        subY = mi.rcWork.bottom - subH - S(4);
    }
    if (subY < mi.rcWork.top) subY = mi.rcWork.top;

    if (subType == 2) {
        g_sliderVal = Device::GetCurrentState().sleepMinutes;
    }

    g_hAcrylicSubMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"RazerModernSubMenuWnd",
        L"",
        WS_POPUP,
        subX, subY, subW, subH,
        g_hAcrylicMenu, NULL, hInst, NULL
    );

    ApplyModernWindowStyle(g_hAcrylicSubMenu, g_curDark, subW, subH);
    ShowWindow(g_hAcrylicSubMenu, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hAcrylicSubMenu);
}

// --------------------------------------------------------------------------
// Main Acrylic Context Menu
// --------------------------------------------------------------------------

struct MenuItemData {
    int id;
    const WCHAR* label;
    WCHAR value[32];
    bool isHeader;
    bool isSeparator;
    bool isInteractive;
    bool isDisabled;
    int submenuType; // 0: None, 1: Polling, 2: Sleep
};

static std::vector<MenuItemData> g_mainItems;

static int GetItemH(int idx) {
    if (idx < 0 || idx >= (int)g_mainItems.size()) return 0;
    if (g_mainItems[idx].isHeader) return S(52);
    if (g_mainItems[idx].isSeparator) return S(9);
    return S(32);
}

static int GetItemY(int targetIdx) {
    int y = S(8);
    for (int i = 0; i < targetIdx; i++) {
        y += GetItemH(i);
    }
    return y;
}

static int HitTestMain(int my) {
    int y = S(8);
    for (size_t i = 0; i < g_mainItems.size(); i++) {
        int h = GetItemH((int)i);
        if (my >= y && my < y + h) {
            if (g_mainItems[i].isHeader || g_mainItems[i].isSeparator || g_mainItems[i].isDisabled) return -1;
            return (int)i;
        }
        y += h;
    }
    return -1;
}

static LRESULT CALLBACK AcrylicMainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND hOther = (HWND)lParam;
                if (hOther != g_hAcrylicMenu && hOther != g_hAcrylicSubMenu) {
                    DismissAllMenus();
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            int my = HIWORD(lParam);
            int newH = HitTestMain(my);
            if (newH != g_mainHover) {
                g_mainHover = newH;
                InvalidateRect(hWnd, NULL, FALSE);

                if (newH >= 0 && g_mainItems[newH].submenuType > 0 && !g_mainItems[newH].isDisabled) {
                    RECT rcItem = { 0, GetItemY(newH), S(236), GetItemY(newH) + GetItemH(newH) };
                    POINT ptTopLeft = { rcItem.left, rcItem.top };
                    POINT ptBottomRight = { rcItem.right, rcItem.bottom };
                    ClientToScreen(hWnd, &ptTopLeft);
                    ClientToScreen(hWnd, &ptBottomRight);
                    RECT rScreen = { ptTopLeft.x, ptTopLeft.y, ptBottomRight.x, ptBottomRight.y };
                    ShowSubMenuWindow(g_mainItems[newH].submenuType, rScreen);
                } else if (newH >= 0 && g_mainItems[newH].submenuType == 0) {
                    DismissSubMenu();
                }
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            POINT pt;
            GetCursorPos(&pt);
            if (g_hAcrylicSubMenu) {
                RECT rSub;
                GetWindowRect(g_hAcrylicSubMenu, &rSub);
                if (PtInRect(&rSub, pt)) return 0;
            }
            if (g_mainHover != -1) {
                g_mainHover = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int idx = HitTestMain(HIWORD(lParam));
            if (idx >= 0 && g_mainItems[idx].isInteractive && !g_mainItems[idx].isDisabled) {
                int cmd = g_mainItems[idx].id;
                DismissAllMenus();
                if (cmd == 1001) { // Toggle Auto Run
                    ToggleAutoRun();
                } else if (cmd == 1002) { // Exit
                    PostQuitMessage(0);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            COLORREF bgCol = g_curDark ? RGB(24, 26, 32) : RGB(248, 248, 252);
            COLORREF borderCol = g_curDark ? RGB(50, 54, 65) : RGB(218, 222, 230);
            COLORREF hoverCol = g_curDark ? RGB(52, 58, 72) : RGB(228, 232, 242);
            COLORREF textCol = g_curDark ? RGB(235, 240, 248) : RGB(30, 35, 45);
            COLORREF mutedCol = g_curDark ? RGB(155, 165, 180) : RGB(100, 110, 125);
            COLORREF greenCol = g_curDark ? RGB(34, 197, 94) : RGB(22, 163, 74);
            COLORREF sepCol = g_curDark ? RGB(40, 44, 54) : RGB(225, 228, 236);
            COLORREF checkCol = g_curDark ? RGB(68, 214, 44) : RGB(38, 150, 34);

            AcrylicSurface::Buffer surface;
            if (!AcrylicSurface::Create(hdc, rc.right, rc.bottom, surface)) {
                HBRUSH fallback = CreateSolidBrush(bgCol);
                FillRect(hdc, &rc, fallback);
                DeleteObject(fallback);
                EndPaint(hWnd, &ps);
                return 0;
            }
            HDC memDC = surface.dc;

            HBRUSH bgBrush = CreateSolidBrush(bgCol);
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            HPEN borderPen = CreatePen(PS_SOLID, 1, borderCol);
            HGDIOBJ oldPen = SelectObject(memDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
            RoundRect(memDC, 0, 0, rc.right, rc.bottom, S(12), S(12));
            SelectObject(memDC, oldPen);
            DeleteObject(borderPen);

            HFONT hFontNormal = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HFONT hFontBold = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            HFONT hFontSub = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            SetBkMode(memDC, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(memDC, hFontNormal);

            int y = S(8);
            for (size_t i = 0; i < g_mainItems.size(); i++) {
                int h = GetItemH((int)i);
                if (g_mainItems[i].isHeader) {
                    SelectObject(memDC, hFontBold);
                    SetTextColor(memDC, textCol);
                    RECT rTitle = { S(16), y + S(4), rc.right - S(16), y + S(26) };
                    // Ellipsis only kicks in if the name is wider than the screen allows.
                    DrawTextW(memDC, g_mainItems[i].label, -1, &rTitle,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                    SelectObject(memDC, hFontSub);
                    Device::State st = Device::GetCurrentState();
                    SetTextColor(memDC, st.isConnected ? greenCol : mutedCol);
                    RECT rSub = { S(16), y + S(26), rc.right - S(16), y + S(46) };
                    DrawTextW(memDC, g_mainItems[i].value, -1, &rSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    y += h;
                } else if (g_mainItems[i].isSeparator) {
                    HPEN pSep = CreatePen(PS_SOLID, 1, sepCol);
                    HGDIOBJ oP = SelectObject(memDC, pSep);
                    MoveToEx(memDC, S(12), y + S(4), NULL);
                    LineTo(memDC, rc.right - S(12), y + S(4));
                    SelectObject(memDC, oP);
                    DeleteObject(pSep);
                    y += h;
                } else {
                    RECT rItem = { S(6), y, rc.right - S(6), y + h };
                    if ((int)i == g_mainHover && g_mainItems[i].isInteractive && !g_mainItems[i].isDisabled) {
                        HBRUSH hH = CreateSolidBrush(hoverCol);
                        HPEN hP = CreatePen(PS_SOLID, 1, hoverCol);
                        HGDIOBJ oP = SelectObject(memDC, hP);
                        HGDIOBJ oB = SelectObject(memDC, hH);
                        RoundRect(memDC, rItem.left, rItem.top + 1, rItem.right, rItem.bottom - 1, S(6), S(6));
                        SelectObject(memDC, oP);
                        SelectObject(memDC, oB);
                        DeleteObject(hP);
                        DeleteObject(hH);
                    }

                    SelectObject(memDC, hFontNormal);
                    SetTextColor(memDC, g_mainItems[i].isDisabled ? mutedCol : textCol);
                    RECT rLabel = { S(14), y, rc.right - S(90), y + h };
                    DrawTextW(memDC, g_mainItems[i].label, -1, &rLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                    if (g_mainItems[i].value[0]) {
                        bool isCheck = (wcscmp(g_mainItems[i].value, L"✓ 已开启") == 0);
                        SetTextColor(memDC, isCheck ? checkCol : mutedCol);
                        RECT rVal = { rc.right - S(115), y, rc.right - S(14), y + h };
                        DrawTextW(memDC, g_mainItems[i].value, -1, &rVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    }
                    y += h;
                }
            }

            SelectObject(memDC, oldFont);
            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBrush);
            DeleteObject(hFontNormal);
            DeleteObject(hFontBold);
            DeleteObject(hFontSub);

            AcrylicSurface::Present(hdc, surface, bgCol, g_acrylicEnabled);
            AcrylicSurface::Destroy(surface);

            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowMenu(HWND hWndOwner) {
    if (g_bModalLoop) return;
    Device::RefreshPollingRate();
    g_hParentAppWnd = hWndOwner;
    g_curDark = Theme::IsDarkMode();

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);
        pfnGetDpiForWindow fnGetDpi = (pfnGetDpiForWindow)GetProcAddress(hUser, "GetDpiForWindow");
        if (fnGetDpi && hWndOwner) g_curDpi = fnGetDpi(hWndOwner);
    }
    if (g_curDpi == 0) g_curDpi = 96;

    Device::State st = Device::GetCurrentState();

    // Populate Menu Items
    g_mainItems.clear();

    MenuItemData itemHeader = { 0 };
    itemHeader.isHeader = true;
    itemHeader.label = st.modelName[0] ? st.modelName : L"Razer gaming mouse";
    if (st.isConnected) {
        StringCchCopyW(itemHeader.value, 32, st.isWired ? L"● 已连接 · USB 有线" : L"● 已连接 · 2.4 GHz 无线");
    } else if (st.receiverPresent) {
        StringCchCopyW(itemHeader.value, 32, L"● 接收器已就绪 · 鼠标未连接");
    } else {
        StringCchCopyW(itemHeader.value, 32, L"● 未连接 · 检查鼠标或接收器");
    }
    g_mainItems.push_back(itemHeader);

    MenuItemData itemBat = { 0 };
    itemBat.label = L"电池电量";
    itemBat.isDisabled = !st.isConnected || !st.hasBattery;
    if (st.isConnected && st.hasBattery) {
        if (st.isCharging) {
            StringCchPrintfW(itemBat.value, 32, L"%d%% 充电中", st.battery);
        } else {
            StringCchPrintfW(itemBat.value, 32, L"%d%%", st.battery);
        }
    } else {
        StringCchCopyW(itemBat.value, 32, L"--");
    }
    g_mainItems.push_back(itemBat);

    MenuItemData itemDpi = { 0 };
    itemDpi.label = L"灵敏度 · DPI";
    itemDpi.isDisabled = !st.isConnected || !st.hasDpi;
    if (st.isConnected && st.hasDpi) {
        StringCchPrintfW(itemDpi.value, 32, L"%d DPI", st.dpiX);
    } else {
        StringCchCopyW(itemDpi.value, 32, L"--");
    }
    g_mainItems.push_back(itemDpi);

    MenuItemData itemPoll = { 0 };
    itemPoll.label = L"轮询率";
    itemPoll.isDisabled = !st.isConnected || !st.hasPollingRate;
    if (st.isConnected && st.hasPollingRate) {
        StringCchPrintfW(itemPoll.value, 32, L"%d Hz", st.pollingHz);
    } else {
        StringCchCopyW(itemPoll.value, 32, L"--");
    }
    g_mainItems.push_back(itemPoll);

    static const WCHAR* styleNames[] = { L"环形电量", L"电池图标", L"数字显示" };
    MenuItemData itemBatteryStyle = { 0 };
    itemBatteryStyle.label = L"电量显示";
    itemBatteryStyle.isInteractive = true;
    itemBatteryStyle.submenuType = 3;
    StringCchPrintfW(itemBatteryStyle.value, 32, L"%s ›", styleNames[GetBatteryDisplayStyle()]);
    g_mainItems.push_back(itemBatteryStyle);

    MenuItemData itemTheme = { 0 };
    itemTheme.label = L"主题显示";
    itemTheme.isInteractive = true;
    itemTheme.submenuType = 4;
    StringCchPrintfW(itemTheme.value, 32, L"%s ›", GetThemeModeLabel(Theme::GetMode()));
    g_mainItems.push_back(itemTheme);

    MenuItemData sep1 = { 0 };
    sep1.isSeparator = true;
    g_mainItems.push_back(sep1);

    MenuItemData itemAuto = { 1001 };
    itemAuto.label = L"开机自启动";
    itemAuto.isInteractive = true;
    StringCchCopyW(itemAuto.value, 32, IsAutoRunEnabled() ? L"✓ 已开启" : L"未开启");
    g_mainItems.push_back(itemAuto);

    MenuItemData sep2 = { 0 };
    sep2.isSeparator = true;
    g_mainItems.push_back(sep2);

    MenuItemData itemExit = { 1002 };
    itemExit.label = L"退出程序";
    itemExit.isInteractive = true;
    g_mainItems.push_back(itemExit);

    int totalH = S(16);
    for (size_t i = 0; i < g_mainItems.size(); i++) {
        totalH += GetItemH((int)i);
    }
    // Menu width follows the device name so the header title is never clipped.
    int menuW = S(236);
    if (!g_mainItems.empty() && g_mainItems[0].isHeader) {
        HDC measureDc = GetDC(NULL);
        HFONT titleFont = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT subFont = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        RECT rTitle = {0, 0, 0, 0};
        RECT rSub = {0, 0, 0, 0};
        HGDIOBJ oldMeasureFont = SelectObject(measureDc, titleFont);
        DrawTextW(measureDc, g_mainItems[0].label, -1, &rTitle, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(measureDc, subFont);
        DrawTextW(measureDc, g_mainItems[0].value, -1, &rSub, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(measureDc, oldMeasureFont);
        DeleteObject(titleFont);
        DeleteObject(subFont);
        ReleaseDC(NULL, measureDc);

        const int requiredW = std::max(rTitle.right - rTitle.left, rSub.right - rSub.left) + S(34);
        const int workWidth = mi.rcWork.right - mi.rcWork.left;
        const int limitW = std::max(S(236), std::min(workWidth - S(24), S(460)));
        menuW = std::clamp(requiredW, S(236), limitW);
    }

    int posX = pt.x - S(10);
    int posY = pt.y - totalH - S(10);
    if (posX + menuW > mi.rcWork.right) posX = mi.rcWork.right - menuW - S(8);
    if (posX < mi.rcWork.left) posX = mi.rcWork.left + S(8);
    if (posY < mi.rcWork.top) posY = pt.y + S(10);
    if (posY + totalH > mi.rcWork.bottom) posY = mi.rcWork.bottom - totalH - S(8);

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hWndOwner, GWLP_HINSTANCE);
    static bool s_mainRegistered = false;
    if (!s_mainRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AcrylicMainWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"RazerModernMainMenuWnd";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassExW(&wc);
        s_mainRegistered = true;
    }

    g_mainHover = -1;
    g_activeSubId = 0;

    g_hAcrylicMenu = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"RazerModernMainMenuWnd",
        L"",
        WS_POPUP,
        posX, posY, menuW, totalH,
        hWndOwner, NULL, hInst, NULL
    );

    ApplyModernWindowStyle(g_hAcrylicMenu, g_curDark, menuW, totalH);
    SetForegroundWindow(g_hAcrylicMenu);
    ShowWindow(g_hAcrylicMenu, SW_SHOW);
    UpdateWindow(g_hAcrylicMenu);

    g_hMenuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MenuMouseHookProc, hInst, 0);

    g_bModalLoop = true;
    MSG msg;
    while (g_bModalLoop && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DismissAllMenus();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) {
        PostQuitMessage((int)msg.wParam);
    }
}

void UpdateTooltip(NOTIFYICONDATAW& nid, const Device::State& state) {
    const WCHAR* model = state.modelName[0] ? state.modelName : L"Razer 鼠标";
    if (!state.isConnected) {
        if (state.receiverPresent) {
            StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip),
                             L"%s\n接收器已就绪 · 鼠标未连接\n打开鼠标电源或移动鼠标唤醒", model);
        } else {
            StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip), L"%s\n未连接 · 检查鼠标或接收器", model);
        }
        return;
    }
    const WCHAR* mode = state.isWired ? L"USB 有线" : L"2.4 GHz 无线";
    WCHAR battery[32], dpi[32], polling[32];
    if (!state.hasBattery) StringCchCopyW(battery, ARRAYSIZE(battery), L"电量 --");
    else StringCchPrintfW(battery, ARRAYSIZE(battery), L"电量 %d%%%s", state.battery, state.isCharging ? L" 充电中" : L"");
    if (!state.hasDpi) StringCchCopyW(dpi, ARRAYSIZE(dpi), L"DPI --");
    else StringCchPrintfW(dpi, ARRAYSIZE(dpi), L"DPI %d", state.dpiX);
    if (!state.hasPollingRate) StringCchCopyW(polling, ARRAYSIZE(polling), L"轮询率 --");
    else StringCchPrintfW(polling, ARRAYSIZE(polling), L"轮询率 %d Hz", state.pollingHz);
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip), L"%s\n已连接 · %s\n%s · %s · %s", model, mode, battery, dpi, polling);
}

} // namespace Tray
