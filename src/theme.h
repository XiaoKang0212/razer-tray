#pragma once

#include <windows.h>

namespace Theme {

enum class Mode : DWORD {
    FollowSystem = 0,
    Light = 1,
    Dark = 2
};

inline Mode GetMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\razer-tray",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return Mode::FollowSystem;
    }

    DWORD value = static_cast<DWORD>(Mode::FollowSystem);
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, L"ThemeMode", nullptr,
                                             &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value) || value > 2) {
        return Mode::FollowSystem;
    }
    return static_cast<Mode>(value);
}

inline bool SetMode(Mode mode) {
    const DWORD value = static_cast<DWORD>(mode);
    if (value > static_cast<DWORD>(Mode::Dark)) return false;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\razer-tray", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const LSTATUS status = RegSetValueExW(key, L"ThemeMode", 0, REG_DWORD,
                                           reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

inline bool IsSystemDarkMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, L"SystemUsesLightTheme", nullptr,
                                             &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value) && value == 0;
}

inline bool IsDarkMode() {
    switch (GetMode()) {
        case Mode::Light: return false;
        case Mode::Dark: return true;
        case Mode::FollowSystem: return IsSystemDarkMode();
    }
    return IsSystemDarkMode();
}

} // namespace Theme
