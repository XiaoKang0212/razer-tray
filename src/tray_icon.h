#pragma once
#include <windows.h>

namespace Tray {

// Battery indicator styles for the notification area icon.
enum BatteryDisplayStyle {
    BATTERY_STYLE_RING = 0,     // circular gauge around the three headed snake
    BATTERY_STYLE_BATTERY = 1,  // classic battery shape with a level fill
    BATTERY_STYLE_NUMBER = 2,   // battery percentage as text
};

// Builds the notification area icon for the requested display style.
HICON CreateBatteryIcon(int battery, bool isCharging, bool isConnected, int size, bool isDark,
                        int style = BATTERY_STYLE_RING);

} // namespace Tray
