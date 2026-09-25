#pragma once

#include <windows.h>

namespace Theme {

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

} // namespace Theme
