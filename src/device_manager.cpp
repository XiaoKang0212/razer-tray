#include "device_manager.h"
#include <hidsdi.h>
#include <setupapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "shell32.lib")

namespace Device {
namespace {

constexpr int RAZER_PAYLOAD_LENGTH = 90;   // Razer protocol payload, without the HID report id
constexpr int MAX_CANDIDATES = 12;
constexpr int MAX_PROBED_CANDIDATES = 4;
constexpr int MAX_LOG_LINES = 20000;
constexpr ULONGLONG TELEMETRY_INTERVAL_MS = 10000;

constexpr BYTE RAZER_CMD_SUCCESSFUL = 0x02;
constexpr BYTE RAZER_CMD_BUSY = 0x01;

struct ModelDef { const WCHAR* pid; const WCHAR* name; bool wireless; };

static const ModelDef MODELS[] = {
    {L"0054",L"Razer DeathAdder 3500",false}, {L"005c",L"Razer DeathAdder Elite",false},
    {L"0064",L"Razer Basilisk",false}, {L"0065",L"Razer Basilisk Essential",false},
    {L"006e",L"Razer DeathAdder Essential",false}, {L"0078",L"Razer Viper",false},
    {L"007a",L"Razer Viper Ultimate (Wired)",false}, {L"007b",L"Razer Viper Ultimate (Wireless)",true},
    {L"0083",L"Razer Basilisk X HyperSpeed",true}, {L"0084",L"Razer DeathAdder V2",false},
    {L"0085",L"Razer Basilisk V2",false}, {L"0086",L"Razer Basilisk Ultimate (Wired)",false},
    {L"0088",L"Razer Basilisk Ultimate (Receiver)",true}, {L"008a",L"Razer Viper Mini",false},
    {L"008c",L"Razer DeathAdder V2 Mini",false}, {L"0091",L"Razer Viper 8KHz",false},
    {L"0099",L"Razer Basilisk V3",false}, {L"009c",L"Razer DeathAdder V2 X HyperSpeed",true},
    {L"00a5",L"Razer Viper V2 Pro (Wired)",false}, {L"00a6",L"Razer Viper V2 Pro (Wireless)",true},
    {L"00aa",L"Razer Basilisk V3 Pro (Wired)",false}, {L"00ab",L"Razer Basilisk V3 Pro (Wireless)",true},
    {L"00b2",L"Razer DeathAdder V3",false}, {L"00b6",L"Razer DeathAdder V3 Pro (Wired)",false},
    {L"00b7",L"Razer DeathAdder V3 Pro (Wireless)",true}, {L"00b8",L"Razer Viper V3 HyperSpeed",true},
    {L"00b9",L"Razer Basilisk V3 X HyperSpeed",true}, {L"00be",L"Razer DeathAdder V4 Pro (Wired)",false},
    {L"00bf",L"Razer DeathAdder V4 Pro (Wireless)",true}, {L"00c0",L"Razer Viper V3 Pro (Wired)",false},
    {L"00c1",L"Razer Viper V3 Pro (Wireless)",true}, {L"00c2",L"Razer DeathAdder V3 Pro (Wired)",false},
    {L"00c3",L"Razer DeathAdder V3 Pro (Wireless)",true}, {L"00c4",L"Razer DeathAdder V3 HyperSpeed (Wired)",false},
    {L"00c5",L"Razer DeathAdder V3 HyperSpeed (Wireless)",true}, {L"00cb",L"Razer Basilisk V3 35K",false},
    {L"00cc",L"Razer Basilisk V3 Pro 35K (Wired)",false}, {L"00cd",L"Razer Basilisk V3 Pro 35K (Wireless)",true}
};

State g_state;
StateCallback g_callback = nullptr;
CRITICAL_SECTION g_stateLock;
CRITICAL_SECTION g_logLock;
bool g_logLockReady = false;
HANDLE g_worker = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_change = nullptr;
HANDLE g_refreshDone = nullptr;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
WCHAR g_logPath[MAX_PATH] = {};
int g_logLines = 0;
volatile LONG g_forceRefresh = 0;
std::vector<std::wstring> g_loggedPaths;

bool FirstLogFor(const WCHAR* path) {
    for (const auto& entry : g_loggedPaths) {
        if (!_wcsicmp(entry.c_str(), path)) return false;
    }
    g_loggedPaths.push_back(path ? path : L"");
    return true;
}

void LogLine(const WCHAR* format, ...);

// --------------------------------------------------------------------------
// Diagnostic log
// --------------------------------------------------------------------------

void LogStart() {
    if (!g_logLockReady) {
        InitializeCriticalSection(&g_logLock);
        g_logLockReady = true;
    }

    WCHAR appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, appData))) return;

    WCHAR folder[MAX_PATH] = {};
    StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\RazerTray", appData);
    CreateDirectoryW(folder, nullptr);
    StringCchPrintfW(g_logPath, ARRAYSIZE(g_logPath), L"%s\\telemetry.log", folder);

    g_logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) return;

    const BYTE bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(g_logFile, bom, sizeof(bom), &written, nullptr);
    g_logLines = 0;
    LogLine(L"razer-tray telemetry log opened");
}

void LogStop() {
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    if (g_logLockReady) {
        DeleteCriticalSection(&g_logLock);
        g_logLockReady = false;
    }
}

void LogLine(const WCHAR* format, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE || g_logLines >= MAX_LOG_LINES) return;

    WCHAR buffer[900] = {};
    va_list args;
    va_start(args, format);
    StringCchVPrintfW(buffer, ARRAYSIZE(buffer), format, args);
    va_end(args);

    char utf8[1400] = {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8) - 3, nullptr, nullptr);
    if (length <= 1) return;

    utf8[length - 1] = '\r';
    utf8[length] = '\n';

    EnterCriticalSection(&g_logLock);
    if (g_logLines < MAX_LOG_LINES) {
        DWORD written = 0;
        WriteFile(g_logFile, utf8, (DWORD)(length + 1), &written, nullptr);
        FlushFileBuffers(g_logFile);
        ++g_logLines;
    }
    LeaveCriticalSection(&g_logLock);
}

// --------------------------------------------------------------------------
// Razer HID report protocol
// --------------------------------------------------------------------------

struct Reply {
    bool ok = false;
    BYTE status = 0;
    BYTE transaction = 0;
    BYTE dataSize = 0;
    BYTE args[80] = {};
};

struct Candidate {
    WCHAR path[MAX_PATH] = {};
    WCHAR model[64] = {};
    bool wireless = false;
    bool knownModel = false;
    int featureLength = RAZER_PAYLOAD_LENGTH;
};

// Razer control interfaces are writable only: Windows blocks read/write access on
// mouse top level collections, so fall back to write-only and read-only handles.
HANDLE OpenControlInterface(const WCHAR* path, DWORD* accessUsed) {
    const DWORD modes[] = {GENERIC_READ | GENERIC_WRITE, GENERIC_WRITE, 0};
    for (DWORD mode : modes) {
        HANDLE hid = CreateFileW(path, mode, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hid != INVALID_HANDLE_VALUE) {
            if (accessUsed) *accessUsed = mode;
            return hid;
        }
    }
    if (accessUsed) *accessUsed = 0;
    return INVALID_HANDLE_VALUE;
}

const ModelDef* FindModel(const WCHAR* path) {
    for (const auto& model : MODELS) {
        WCHAR needle[16] = {};
        StringCchPrintfW(needle, ARRAYSIZE(needle), L"pid_%s", model.pid);
        if (wcsstr(path, needle)) return &model;
    }
    return nullptr;
}

bool LooksLikeMouse(const WCHAR* product) {
    static const WCHAR* keywords[] = {
        L"mouse", L"deathadder", L"death adder", L"viper", L"basilisk",
        L"naga", L"cobra", L"orochi", L"mamba", L"abyssus",
        L"lancehead", L"krait", L"taipan", L"diamondback",
        L"pro click", L"proclick", L"ouroboros"
    };
    if (!product || !product[0]) return false;
    WCHAR lower[256] = {};
    StringCchCopyW(lower, ARRAYSIZE(lower), product);
    _wcslwr_s(lower, ARRAYSIZE(lower));
    for (const WCHAR* keyword : keywords) {
        if (wcsstr(lower, keyword)) return true;
    }
    return false;
}

void CollectCandidates(std::vector<Candidate>& out) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO info = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA iface = {};
    iface.cbSize = sizeof(iface);
    for (DWORD index = 0; (int)out.size() < MAX_CANDIDATES &&
         SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, index, &iface); ++index) {
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &bytes, nullptr);
        if (!bytes) continue;

        std::vector<BYTE> storage(bytes);
        auto detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)storage.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, bytes, nullptr, nullptr)) continue;

        WCHAR lower[MAX_PATH] = {};
        StringCchCopyW(lower, ARRAYSIZE(lower), detail->DevicePath);
        _wcslwr_s(lower, ARRAYSIZE(lower));
        if (!wcsstr(lower, L"vid_1532")) continue;

        const ModelDef* model = FindModel(lower);

        DWORD access = 0;
        HANDLE probe = OpenControlInterface(detail->DevicePath, &access);
        if (probe == INVALID_HANDLE_VALUE) {
            if (model && FirstLogFor(detail->DevicePath)) {
                LogLine(L"skip %s (interface cannot be opened)", model->name);
            }
            continue;
        }

        int featureLength = 0;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(probe, &preparsed)) {
            HIDP_CAPS caps = {};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                featureLength = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsed);
        }

        // The Razer control channel is the interface carrying the 90 byte feature report.
        if (featureLength < RAZER_PAYLOAD_LENGTH || featureLength > RAZER_PAYLOAD_LENGTH + 1) {
            if (model && FirstLogFor(detail->DevicePath)) {
                LogLine(L"skip %s featureLength=%d (not the control interface)", model->name, featureLength);
            }
            CloseHandle(probe);
            continue;
        }

        Candidate candidate;
        StringCchCopyW(candidate.path, ARRAYSIZE(candidate.path), detail->DevicePath);
        candidate.featureLength = featureLength;
        candidate.knownModel = (model != nullptr);
        candidate.wireless = model ? model->wireless : false;

        if (model) {
            StringCchCopyW(candidate.model, ARRAYSIZE(candidate.model), model->name);
        } else {
            WCHAR product[128] = {};
            HidD_GetProductString(probe, product, sizeof(product));
            if (!product[0]) {
                HANDLE readable = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (readable != INVALID_HANDLE_VALUE) {
                    HidD_GetProductString(readable, product, sizeof(product));
                    CloseHandle(readable);
                }
            }
            if (!LooksLikeMouse(product)) {
                CloseHandle(probe);
                continue;
            }
            StringCchCopyW(candidate.model, ARRAYSIZE(candidate.model), product);
        }
        CloseHandle(probe);
        if (FirstLogFor(detail->DevicePath)) {
            LogLine(L"candidate model=%s featureLength=%d wireless=%d known=%d",
                    candidate.model, featureLength, candidate.wireless ? 1 : 0, candidate.knownModel ? 1 : 0);
        }
        out.push_back(candidate);
    }
    SetupDiDestroyDeviceInfoList(info);
}

bool SendCommand(HANDLE hid, int featureLength, BYTE transaction, BYTE commandClass, BYTE commandId,
                 const BYTE* args, BYTE argLength, Reply& reply) {
    BYTE payload[RAZER_PAYLOAD_LENGTH] = {};
    payload[0] = 0x00;                              // status: new command
    payload[1] = transaction;                       // transaction id
    payload[2] = 0x00; payload[3] = 0x00;           // remaining packets
    payload[4] = 0x00;                              // protocol type
    payload[5] = argLength;                         // data size
    payload[6] = commandClass;
    payload[7] = (BYTE)(commandId | 0x80);          // host to device
    if (args && argLength) memcpy(payload + 8, args, std::min<int>(argLength, 80));

    BYTE crc = 0;
    for (int i = 2; i < 88; ++i) crc ^= payload[i];
    payload[88] = crc;
    payload[89] = 0x00;

    // Windows HID buffers carry the report id when the descriptor declares one.
    const int offset = featureLength - RAZER_PAYLOAD_LENGTH;
    BYTE outbound[RAZER_PAYLOAD_LENGTH + 1] = {};
    memcpy(outbound + offset, payload, RAZER_PAYLOAD_LENGTH);
    if (!HidD_SetFeature(hid, outbound, featureLength)) return false;

    Sleep(2);
    for (int attempt = 0; attempt < 8; ++attempt) {
        BYTE inbound[RAZER_PAYLOAD_LENGTH + 1] = {};
        if (!HidD_GetFeature(hid, inbound, featureLength)) {
            Sleep(3);
            continue;
        }
        const BYTE* response = inbound + offset;
        if (response[0] == RAZER_CMD_BUSY) {
            Sleep(4);
            continue;
        }
        reply.status = response[0];
        reply.transaction = response[1];
        reply.dataSize = response[5];
        if (response[0] != RAZER_CMD_SUCCESSFUL) return false;
        if (response[1] != transaction) continue;
        if (response[6] != commandClass || response[7] != (BYTE)(commandId | 0x80)) continue;
        memcpy(reply.args, response + 8, 80);
        reply.ok = true;
        return true;
    }
    return false;
}

bool QueryRazer(HANDLE hid, int featureLength, BYTE commandClass, BYTE commandId,
                const BYTE* args, BYTE argLength, Reply& reply, const WCHAR* label, bool logSuccess = true) {
    const BYTE transactions[] = {0x1F, 0x3F, 0xFF};
    BYTE lastStatus = 0x00;
    for (BYTE transaction : transactions) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            Reply candidate;
            if (SendCommand(hid, featureLength, transaction, commandClass, commandId, args, argLength, candidate)) {
                if (logSuccess) {
                    LogLine(L"%s ok class=%02X id=%02X tx=%02X size=%02d args=%02X %02X %02X %02X %02X %02X",
                            label, commandClass, commandId, transaction, candidate.dataSize,
                            candidate.args[0], candidate.args[1], candidate.args[2],
                            candidate.args[3], candidate.args[4], candidate.args[5]);
                }
                reply = candidate;
                return true;
            }
            lastStatus = candidate.status;
            Sleep(3);
        }
    }
    LogLine(L"%s failed class=%02X id=%02X lastStatus=%02X", label, commandClass, commandId, lastStatus);
    return false;
}

bool PlausibleDpi(int value) {
    return value >= 100 && value <= 45000;
}

struct TelemetryResult {
    bool battery = false;
    bool dpi = false;
    bool polling = false;

    bool Any() const { return battery || dpi || polling; }
};

TelemetryResult ReadTelemetry(const Candidate& device, State& state) {
    TelemetryResult result;
    HANDLE hid = OpenControlInterface(device.path, nullptr);
    if (hid == INVALID_HANDLE_VALUE) {
        LogLine(L"open failed for %s", device.model);
        return result;
    }

    BYTE args[80] = {};
    Reply reply;

    // Battery level: raw 0-255 from the device, 0 means "no reading".
    if (QueryRazer(hid, device.featureLength, 0x07, 0x80, args, 2, reply, L"battery", false)) {
        const BYTE raw = reply.args[1];
        if (raw > 0) {
            state.battery = (raw * 100 + 127) / 255;
            state.hasBattery = true;
            result.battery = true;
        }
    }

    // Charging status has its own command, the flag lives in the second argument.
    memset(args, 0, sizeof(args));
    if (QueryRazer(hid, device.featureLength, 0x07, 0x84, args, 2, reply, L"charging", false)) {
        state.isCharging = reply.args[1] != 0;
    }

    // DPI: 0x04/0x85 with 7 argument bytes, X and Y are shorts in arguments 1..4.
    const BYTE dpiVariables[] = {0x00, 0x01}; // NOSTORE, then VARSTORE
    for (BYTE variable : dpiVariables) {
        memset(args, 0, sizeof(args));
        args[0] = variable;
        if (!QueryRazer(hid, device.featureLength, 0x04, 0x85, args, 7, reply, L"dpi", false)) continue;

        int x = (reply.args[1] << 8) | (reply.args[2] & 0xFF);
        int y = (reply.args[3] << 8) | (reply.args[4] & 0xFF);
        if (!PlausibleDpi(x)) {
            const int altX = (reply.args[0] << 8) | (reply.args[1] & 0xFF);
            const int altY = (reply.args[2] << 8) | (reply.args[3] & 0xFF);
            if (PlausibleDpi(altX)) { x = altX; y = altY; }
        }
        if (PlausibleDpi(x)) {
            state.dpiX = x;
            state.dpiY = PlausibleDpi(y) ? y : x;
            state.hasDpi = true;
            result.dpi = true;
            break;
        }
    }

    // Polling rate: classic command first, high polling rate command as fallback.
    memset(args, 0, sizeof(args));
    if (QueryRazer(hid, device.featureLength, 0x00, 0x85, args, 1, reply, L"polling", false)) {
        switch (reply.args[0]) {
            case 0x01: state.pollingHz = 1000; break;
            case 0x02: state.pollingHz = 500; break;
            case 0x04: state.pollingHz = 250; break;
            case 0x08: state.pollingHz = 125; break;
            default: state.pollingHz = 0; break;
        }
        state.hasPollingRate = state.pollingHz != 0;
        result.polling = state.hasPollingRate;
    }

    if (!state.hasPollingRate) {
        memset(args, 0, sizeof(args));
        if (QueryRazer(hid, device.featureLength, 0x00, 0xC0, args, 1, reply, L"polling8k", false)) {
            switch (reply.args[1]) {
                case 0x01: state.pollingHz = 8000; break;
                case 0x02: state.pollingHz = 4000; break;
                case 0x04: state.pollingHz = 2000; break;
                case 0x08: state.pollingHz = 1000; break;
                case 0x10: state.pollingHz = 500; break;
                case 0x40: state.pollingHz = 125; break;
                default: state.pollingHz = 0; break;
            }
            state.hasPollingRate = state.pollingHz != 0;
            result.polling = state.hasPollingRate;
        }
    }

    LogLine(L"telemetry model=%s battery=%s dpi=%s polling=%s charging=%d",
            device.model,
            result.battery ? L"ok" : L"--",
            result.dpi ? L"ok" : L"--",
            result.polling ? L"ok" : L"--",
            state.isCharging ? 1 : 0);

    CloseHandle(hid);
    return result;
}

void CopyTelemetry(State& destination, const State& source) {
    destination.isCharging = source.isCharging;
    destination.hasBattery = source.hasBattery;
    destination.battery = source.battery;
    destination.hasDpi = source.hasDpi;
    destination.dpiX = source.dpiX;
    destination.dpiY = source.dpiY;
    destination.hasPollingRate = source.hasPollingRate;
    destination.pollingHz = source.pollingHz;
}

void ClearTelemetry(State& state) {
    state.isCharging = false;
    state.hasBattery = false;
    state.battery = -1;
    state.hasDpi = false;
    state.hasPollingRate = false;
    state.pollingHz = 0;
}

// Picks the interface that answers Razer queries, preferring a known mouse model.
int ChooseCandidate(const std::vector<Candidate>& candidates) {
    int bestKnown = -1, bestKnownScore = -1;
    int bestAny = -1, bestAnyScore = -1;
    const int limit = std::min<int>((int)candidates.size(), MAX_PROBED_CANDIDATES);

    for (int i = 0; i < limit; ++i) {
        HANDLE hid = OpenControlInterface(candidates[i].path, nullptr);
        if (hid == INVALID_HANDLE_VALUE) continue;

        int score = 0;
        BYTE args[80] = {};
        Reply reply;
        if (QueryRazer(hid, candidates[i].featureLength, 0x04, 0x85, args, 7, reply, L"probe-dpi")) ++score;
        memset(args, 0, sizeof(args));
        if (QueryRazer(hid, candidates[i].featureLength, 0x00, 0x85, args, 1, reply, L"probe-polling")) ++score;
        memset(args, 0, sizeof(args));
        if (QueryRazer(hid, candidates[i].featureLength, 0x07, 0x80, args, 2, reply, L"probe-battery")) ++score;
        CloseHandle(hid);

        if (candidates[i].knownModel && score > bestKnownScore) { bestKnown = i; bestKnownScore = score; }
        if (score > bestAnyScore) { bestAny = i; bestAnyScore = score; }
    }

    if (bestKnown >= 0 && bestKnownScore > 0) return bestKnown;
    if (bestAny >= 0 && bestAnyScore > 0) return bestAny;
    if (bestKnown >= 0) return bestKnown;
    return bestAny;
}

bool Different(const State& a, const State& b) {
    return a.isConnected != b.isConnected || a.isWired != b.isWired || a.isCharging != b.isCharging ||
        a.receiverPresent != b.receiverPresent ||
        a.hasBattery != b.hasBattery || a.battery != b.battery ||
        a.hasDpi != b.hasDpi || a.dpiX != b.dpiX || a.dpiY != b.dpiY ||
        a.hasPollingRate != b.hasPollingRate || a.pollingHz != b.pollingHz ||
        _wcsicmp(a.modelName, b.modelName) != 0;
}

DWORD WINAPI Worker(LPVOID) {
    WCHAR lastPath[MAX_PATH] = {};
    ULONGLONG lastTelemetry = 0;
    std::vector<Candidate> candidates;
    HANDLE waits[2] = { g_stop, g_change };
    // Config queries must reach the mouse itself, so they are the most reliable
    // proof that a wireless mouse is switched on (a receiver may cache the level).
    bool configQueriesWork = false;

    for (;;) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 1000);
        if (wait == WAIT_OBJECT_0) break;   // stop requested

        candidates.clear();
        CollectCandidates(candidates);

        const bool forced = InterlockedExchange(&g_forceRefresh, 0) != 0;
        State previous = GetCurrentState();
        State next = {};
        next.battery = -1;
        bool refreshDone = false;

        int chosen = -1;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (lastPath[0] && _wcsicmp(candidates[i].path, lastPath) == 0) { chosen = (int)i; break; }
        }
        if (chosen < 0 && !candidates.empty()) chosen = ChooseCandidate(candidates);

        if (chosen >= 0) {
            const Candidate& device = candidates[chosen];
            next.isRazer = true;
            next.receiverPresent = true;
            next.isWired = !device.wireless;
            StringCchCopyW(next.modelName, ARRAYSIZE(next.modelName), device.model);

            const bool sameInterface = lastPath[0] && _wcsicmp(device.path, lastPath) == 0;
            if (!sameInterface) configQueriesWork = false;
            const ULONGLONG now = GetTickCount64();
            const bool due = now - lastTelemetry >= TELEMETRY_INTERVAL_MS;
            const bool readNow = (!sameInterface || forced || due);
            TelemetryResult result;

            if (readNow) {
                State fresh = next;
                result = ReadTelemetry(device, fresh);
                if (!result.Any() && device.wireless) {
                    // A sleeping mouse needs a moment before it answers again.
                    Sleep(250);
                    result = ReadTelemetry(device, fresh);
                }
                CopyTelemetry(next, fresh);
                if (result.dpi || result.polling) configQueriesWork = true;
                refreshDone = true;
                lastTelemetry = GetTickCount64();
            } else {
                CopyTelemetry(next, previous);
            }

            // A plugged in receiver is not the same as a switched on mouse:
            // wireless devices only answer while the mouse itself is awake.
            if (device.wireless) {
                if (!readNow) {
                    next.isConnected = previous.isConnected;
                } else if (configQueriesWork) {
                    next.isConnected = result.dpi || result.polling;
                } else {
                    next.isConnected = result.Any();
                }
            } else {
                next.isConnected = true;
            }
            if (!next.isConnected) {
                ClearTelemetry(next);
            }
            StringCchCopyW(lastPath, ARRAYSIZE(lastPath), device.path);
        } else {
            lastPath[0] = 0;
            lastTelemetry = 0;
        }

        EnterCriticalSection(&g_stateLock);
        DWORD changed = 0;
        if (Different(g_state, next)) {
            if (g_state.isConnected != next.isConnected) changed |= CHANGE_CONNECTED;
            if (g_state.battery != next.battery || g_state.isCharging != next.isCharging ||
                g_state.hasBattery != next.hasBattery) changed |= CHANGE_BATTERY;
            if (g_state.dpiX != next.dpiX || g_state.dpiY != next.dpiY ||
                g_state.hasDpi != next.hasDpi) changed |= CHANGE_DPI;
            if (g_state.pollingHz != next.pollingHz ||
                g_state.hasPollingRate != next.hasPollingRate) changed |= CHANGE_POLLING;
            g_state = next;
        }
        const State copy = g_state;
        LeaveCriticalSection(&g_stateLock);
        if ((changed || forced) && g_callback) {
            g_callback(copy, changed | (forced ? CHANGE_REFRESHED : 0));
        }
        if (refreshDone && g_refreshDone) SetEvent(g_refreshDone);
    }
    return 0;
}

} // namespace

bool Start(HWND, StateCallback callback) {
    g_callback = callback;
    InitializeCriticalSection(&g_stateLock);
    LogStart();
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_change = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_refreshDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_worker != nullptr;
}

void Stop() {
    if (g_stop) SetEvent(g_stop);
    if (g_worker) { WaitForSingleObject(g_worker, 5000); CloseHandle(g_worker); g_worker = nullptr; }
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
    if (g_change) { CloseHandle(g_change); g_change = nullptr; }
    if (g_refreshDone) { CloseHandle(g_refreshDone); g_refreshDone = nullptr; }
    LogLine(L"razer-tray telemetry log closed");
    LogStop();
    DeleteCriticalSection(&g_stateLock);
}

void NotifyDeviceChange() { if (g_change) SetEvent(g_change); }

void RequestRefresh() {
    InterlockedExchange(&g_forceRefresh, 1);
    if (g_change) SetEvent(g_change);
}

bool RefreshBlocking(DWORD timeoutMs) {
    if (!g_refreshDone) return false;
    ResetEvent(g_refreshDone);
    RequestRefresh();
    return WaitForSingleObject(g_refreshDone, timeoutMs) == WAIT_OBJECT_0;
}

bool SetPollingRate(int) { return false; }
bool SetSleepTimeout(int) { return false; }
bool RefreshPollingRate() { RequestRefresh(); return true; }

bool GetLogPath(WCHAR* buffer, size_t count) {
    if (!buffer || !count || !g_logPath[0]) return false;
    return SUCCEEDED(StringCchCopyW(buffer, count, g_logPath));
}

State GetCurrentState() {
    EnterCriticalSection(&g_stateLock);
    State copy = g_state;
    LeaveCriticalSection(&g_stateLock);
    return copy;
}

} // namespace Device
