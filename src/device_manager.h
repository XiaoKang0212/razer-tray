#pragma once
#include <windows.h>

namespace Device {

struct State {
    bool isConnected = false;
    bool isWired = false;
    bool isCharging = false;
    bool isRazer = false;
    bool receiverPresent = false;   // wireless receiver is plugged in
    bool hasBattery = false;
    bool hasDpi = false;
    bool hasPollingRate = false;
    int battery = -1;
    int dpiLevel = 1;
    int dpiX = 800;
    int dpiY = 800;
    int pollingHz = 0;
    int sleepMinutes = 0;
    WCHAR modelName[64] = {0};
};

typedef void (*StateCallback)(const State& state, DWORD changeMask);

constexpr DWORD CHANGE_CONNECTED = 0x01;
constexpr DWORD CHANGE_BATTERY   = 0x02;
constexpr DWORD CHANGE_DPI       = 0x04;
constexpr DWORD CHANGE_POLLING   = 0x08;
constexpr DWORD CHANGE_SLEEP     = 0x10;
constexpr DWORD CHANGE_REFRESHED = 0x20;

bool Start(HWND hNotifyWnd, StateCallback callback);
void Stop();

void NotifyDeviceChange();

// Reads battery, DPI and polling rate again. RequestRefresh is asynchronous,
// RefreshBlocking waits until the worker finished the read.
void RequestRefresh();
bool RefreshBlocking(DWORD timeoutMs);

bool SetPollingRate(int hz);
bool SetSleepTimeout(int minutes);
bool RefreshPollingRate();

// Full path of the diagnostic log written by the telemetry worker.
bool GetLogPath(WCHAR* buffer, size_t count);

State GetCurrentState();

} // namespace Device
