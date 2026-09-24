#pragma once
#include <windows.h>
#include <shellapi.h>
#include "device_manager.h"
#include "tray_icon.h"

namespace Tray {

void InitTheme();
void ShowMenu(HWND hWndOwner);
void DismissAllMenus();

void UpdateTooltip(NOTIFYICONDATAW& nid, const Device::State& state);

bool IsAutoRunEnabled();
void ToggleAutoRun();

// Battery indicator style of the notification area icon (0 ring, 1 battery, 2 number).
int GetBatteryDisplayStyle();
void SetBatteryDisplayStyle(int style);

} // namespace Tray
