#pragma once
#include <windows.h>
#include "device_manager.h"

namespace Osd {

bool Init(HINSTANCE hInstance);
void Cleanup();

// Shows custom 2-line or 3-line notification with auto-sizing geometry
void Show(const WCHAR* line1, const WCHAR* line2, const WCHAR* line3 = nullptr);

void ShowDeviceStatus(const Device::State& state);

HWND GetWindowHandle();

} // namespace Osd
