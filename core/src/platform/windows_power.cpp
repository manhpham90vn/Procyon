// Battery (the battery class driver), temperatures (ACPI thermal zones through the Thermal Zone
// Information performance counters, WMI as the fallback; the drive through the storage stack) and
// sleep assertions on Windows.
#if defined(_WIN32)

#include "windows_internal.hpp"

#include <batclass.h>
#include <devguid.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <setupapi.h>
#include <wbemidl.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wbemuuid.lib")

namespace procyon::platform {

using namespace win;

namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

// Device interface class of batteries (poclass.h).
constexpr GUID kBatteryInterface = {0x72631E54, 0x78A4, 0x11D0, {0xBC, 0xF7, 0x00, 0xAA, 0x00, 0xB7, 0xB3, 0x2A}};

// Opens the first battery device; empty when the machine has none.
Handle open_battery() {
    HDEVINFO devices =
        SetupDiGetClassDevsW(&kBatteryInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return Handle();
    Handle battery;
    for (DWORD index = 0; index < 8 && !battery; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kBatteryInterface, index, &interface_data)) break;
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &size, nullptr);
        if (size == 0) continue;
        std::vector<char> buffer(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, size, nullptr, nullptr)) continue;
        battery =
            Handle(CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!battery)
            battery = Handle(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    }
    SetupDiDestroyDeviceInfoList(devices);
    return battery;
}

template <typename T>
bool battery_query(HANDLE battery, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level, T &out) {
    BATTERY_QUERY_INFORMATION query{};
    query.BatteryTag = tag;
    query.InformationLevel = level;
    DWORD returned = 0;
    return DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &out, sizeof(out),
                           &returned, nullptr) != 0;
}

// ---- Thermal zones through performance counters ----
//
// "Thermal Zone Information" publishes every ACPI thermal zone (_TZ.TZ00, ...) to any user; the WMI
// class below carries the same data but refuses non-administrators on most machines.

struct Zone {
    bool seen = false;
    bool live = false;  // has read more than one value: a sensor, not a placeholder
    double first = 0;
};

struct ThermalCounters {
    std::mutex mutex;
    bool tried = false;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER temperature = nullptr;  // tenths of a Kelvin, or a Kelvin on older builds
    bool tenths = false;
    std::vector<char> buffer;
    std::unordered_map<std::string, Zone> zones;  // by counter instance name
};

ThermalCounters &thermal_counters() {
    static ThermalCounters t;
    return t;
}

bool open_thermal_counters(ThermalCounters &t) {
    if (t.tried) return t.query != nullptr;
    t.tried = true;
    if (PdhOpenQueryW(nullptr, 0, &t.query) != ERROR_SUCCESS) {
        t.query = nullptr;
        return false;
    }
    // English names work on every locale. High Precision Temperature arrived with Windows 10 1709; the
    // whole-Kelvin counter exists since Windows 8.1.
    if (PdhAddEnglishCounterW(t.query, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0,
                              &t.temperature) == ERROR_SUCCESS) {
        t.tenths = true;
    } else if (PdhAddEnglishCounterW(t.query, L"\\Thermal Zone Information(*)\\Temperature", 0, &t.temperature) !=
               ERROR_SUCCESS) {
        PdhCloseQuery(t.query);
        t.query = nullptr;
        return false;
    }
    return true;
}

// The hottest thermal zone in Celsius from the performance counters, -1 when none is published.
// `require_live`: only zones whose reading has changed since they were first seen (see below).
double counter_zone_temperature(bool require_live) {
    ThermalCounters &t = thermal_counters();
    std::lock_guard lock(t.mutex);
    if (!open_thermal_counters(t)) return -1;
    // The counters are instantaneous values: one collection is enough.
    if (PdhCollectQueryData(t.query) != ERROR_SUCCESS) return -1;
    DWORD size = 0, count = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(t.temperature, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (status != PDH_MORE_DATA || size == 0) return -1;
    if (t.buffer.size() < size) t.buffer.resize(size);
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(t.buffer.data());
    status = PdhGetFormattedCounterArrayW(t.temperature, PDH_FMT_DOUBLE, &size, &count, items);
    if (status != ERROR_SUCCESS) return -1;
    double hottest = -1, hottest_live = -1;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
            continue;
        const double kelvin = t.tenths ? items[i].FmtValue.doubleValue / 10.0 : items[i].FmtValue.doubleValue;
        const double celsius = kelvin - 273.15;
        // Firmware that has no sensor behind a zone reports a constant near 0 K or an absurd value.
        if (!(celsius > -40 && celsius < 150)) continue;
        hottest = std::max(hottest, celsius);
        // Many desktop boards publish a zone with a fixed placeholder (27.8 °C is a common one) and
        // nothing behind it. A real sensor moves within seconds at this resolution: a zone counts as
        // live once it has read two different values.
        Zone &zone = t.zones[utf8(items[i].szName, wcslen(items[i].szName))];
        if (!zone.seen) {
            zone.seen = true;
            zone.first = celsius;
        } else if (celsius != zone.first) {
            zone.live = true;
        }
        if (zone.live) hottest_live = std::max(hottest_live, celsius);
    }
    return require_live ? hottest_live : hottest;
}

// ---- WMI thermal zones (fallback: needs an administrator on most machines) ----

struct Wmi {
    std::mutex mutex;
    bool tried = false;
    IWbemServices *services = nullptr;
};

Wmi &wmi() {
    static Wmi w;
    return w;
}

IWbemServices *wmi_services() {
    Wmi &w = wmi();
    if (w.tried) return w.services;
    w.tried = true;
    // The monitor thread may already be in an apartment: a mode mismatch still leaves COM usable.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)init;
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                         EOAC_NONE, nullptr);  // fails harmlessly when the process already set it
    IWbemLocator *locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void **>(&locator))) ||
        !locator)
        return nullptr;
    IWbemServices *services = nullptr;
    BSTR path = SysAllocString(L"ROOT\\WMI");
    const HRESULT connected = locator->ConnectServer(path, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(path);
    locator->Release();
    if (FAILED(connected) || !services) return nullptr;
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    w.services = services;
    return services;
}

// The hottest ACPI thermal zone in Celsius, -1 when the firmware exposes none (or refuses).
double thermal_zone_temperature() {
    Wmi &w = wmi();
    std::lock_guard lock(w.mutex);
    IWbemServices *services = wmi_services();
    if (!services) return -1;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    IEnumWbemClassObject *rows = nullptr;
    const HRESULT executed =
        services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &rows);
    SysFreeString(query);
    SysFreeString(language);
    if (FAILED(executed) || !rows) return -1;
    double hottest = -1;
    for (;;) {
        IWbemClassObject *row = nullptr;
        ULONG returned = 0;
        if (rows->Next(2000, 1, &row, &returned) != S_OK || returned == 0 || !row) break;
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(row->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr)) &&
            (value.vt == VT_I4 || value.vt == VT_UI4)) {
            // Tenths of a Kelvin.
            const double celsius = (static_cast<double>(value.uintVal) - 2731.5) / 10.0;
            if (celsius > -40 && celsius < 150) hottest = std::max(hottest, celsius);
        }
        VariantClear(&value);
        row->Release();
    }
    rows->Release();
    return hottest;
}

// The generic temperature property of a drive; many NVMe drivers answer it with ERROR_IO_DEVICE.
double drive_property_temperature(HANDLE disk) {
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceTemperatureProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<char> buffer(sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) + 8 * sizeof(STORAGE_TEMPERATURE_INFO));
    DWORD returned = 0;
    if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                         static_cast<DWORD>(buffer.size()), &returned, nullptr))
        return -1;
    auto descriptor = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR *>(buffer.data());
    for (USHORT i = 0; i < descriptor->InfoCount && i < 8; ++i) {
        const SHORT temperature = descriptor->TemperatureInfo[i].Temperature;
        if (temperature > -40 && temperature < 150) return temperature;
    }
    return -1;
}

// The composite temperature from an NVMe drive's SMART / Health Information log page (log 02h),
// which the inbox NVMe driver serves to any user through the protocol-specific property.
double nvme_smart_temperature(HANDLE disk) {
    constexpr DWORD kLogPageBytes = 512;
    constexpr DWORD kHealthInfoLog = 2;
    std::vector<char> buffer(offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) +
                             sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + kLogPageBytes);
    auto query = reinterpret_cast<STORAGE_PROPERTY_QUERY *>(buffer.data());
    query->PropertyId = StorageDeviceProtocolSpecificProperty;
    query->QueryType = PropertyStandardQuery;
    auto request = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA *>(query->AdditionalParameters);
    request->ProtocolType = ProtocolTypeNvme;
    request->DataType = NVMeDataTypeLogPage;
    request->ProtocolDataRequestValue = kHealthInfoLog;
    request->ProtocolDataRequestSubValue = 0;
    request->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    request->ProtocolDataLength = kLogPageBytes;
    DWORD returned = 0;
    if (!DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, buffer.data(), static_cast<DWORD>(buffer.size()),
                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr))
        return -1;
    auto descriptor = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR *>(buffer.data());
    const STORAGE_PROTOCOL_SPECIFIC_DATA &data = descriptor->ProtocolSpecificData;
    if (data.ProtocolDataLength < 3) return -1;
    const size_t at = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData) + data.ProtocolDataOffset;
    if (at + 3 > buffer.size() || at + 3 > returned) return -1;
    // Bytes 1-2 of the log: composite temperature in Kelvin.
    const auto log = reinterpret_cast<const unsigned char *>(buffer.data() + at);
    const double celsius = (log[1] | (log[2] << 8)) - 273.15;
    return celsius > -40 && celsius < 150 ? celsius : -1;
}

// The first physical drive's temperature from the storage stack (NVMe; SATA drives only when their
// driver answers the temperature property, since SMART pass-through needs an administrator).
double drive_temperature() {
    for (int drive = 0; drive < 4; ++drive) {
        wchar_t path[32];
        (void)swprintf_s(path, L"\\\\.\\PhysicalDrive%d", drive);
        Handle disk(CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!disk) continue;
        double celsius = drive_property_temperature(disk.get());
        if (celsius < 0) celsius = nvme_smart_temperature(disk.get());
        if (celsius >= 0) return celsius;
    }
    return -1;
}

// Performance counters first; WMI only when the counter set is missing altogether (it is the same
// ACPI data, so an empty counter set is not worth a WMI round trip every tick).
double zone_temperature(bool require_live) {
    const double celsius = counter_zone_temperature(require_live);
    if (celsius >= 0) return celsius;
    ThermalCounters &t = thermal_counters();
    std::lock_guard lock(t.mutex);
    return t.query ? -1 : thermal_zone_temperature();
}

}  // namespace

// Whether a zone exists at all (the capability); whether it is a sensor is decided as it is read.
double cpu_temperature() { return zone_temperature(false); }

// The hottest zone that has shown a change (a sensor); when no zone has moved yet, the hottest
// zone there is, so a board with only a placeholder still shows what its firmware reports. The UI
// tells the two apart from the history: a reading that never moves is labelled as the board's.
void temperatures(double &cpu, double &disk) {
    cpu = zone_temperature(true);
    if (cpu < 0) cpu = zone_temperature(false);
    disk = drive_temperature();
}

bool battery(pc_battery &out) {
    std::memset(&out, 0, sizeof(out));
    out.minutes_to_empty = out.minutes_to_full = out.cycle_count = -1;
    out.health = out.temperature = -1;
    out.design_capacity_mah = out.max_capacity_mah = -1;

    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) return false;
    out.present = status.BatteryFlag != 128 && status.BatteryFlag != 255;  // no battery / unknown
    out.on_ac_power = status.ACLineStatus == 1;
    if (!out.present) return true;
    out.level = status.BatteryLifePercent <= 100 ? status.BatteryLifePercent / 100.0 : 0;
    out.charging = (status.BatteryFlag & 8) != 0;
    if (status.BatteryLifeTime != 0xFFFFFFFF) out.minutes_to_empty = static_cast<int32_t>(status.BatteryLifeTime / 60);
    if (out.on_ac_power) copy_string(out.adapter, sizeof(out.adapter), "AC power");

    Handle device = open_battery();
    if (!device) return true;
    BATTERY_QUERY_INFORMATION query{};
    DWORD wait = 0, returned = 0;
    if (!DeviceIoControl(device.get(), IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &query.BatteryTag,
                         sizeof(query.BatteryTag), &returned, nullptr))
        return true;
    BATTERY_INFORMATION info{};
    const bool has_info = battery_query(device.get(), query.BatteryTag, BatteryInformation, info);
    BATTERY_WAIT_STATUS wait_status{};
    wait_status.BatteryTag = query.BatteryTag;
    BATTERY_STATUS battery_status{};
    const bool has_status = DeviceIoControl(device.get(), IOCTL_BATTERY_QUERY_STATUS, &wait_status, sizeof(wait_status),
                                            &battery_status, sizeof(battery_status), &returned, nullptr) != 0;
    const double voltage =
        has_status && battery_status.Voltage != BATTERY_UNKNOWN_VOLTAGE ? battery_status.Voltage / 1000.0 : 0;
    if (has_info) {
        const bool mwh = (info.Capabilities & BATTERY_CAPACITY_RELATIVE) == 0;
        if (info.CycleCount) out.cycle_count = static_cast<int32_t>(info.CycleCount);
        if (info.DesignedCapacity && info.FullChargedCapacity) {
            out.health = std::min(1.0, static_cast<double>(info.FullChargedCapacity) / info.DesignedCapacity);
            // Capacities come in mWh; the ABI wants mAh.
            if (mwh && voltage > 0) {
                out.design_capacity_mah = static_cast<int32_t>(info.DesignedCapacity / voltage);
                out.max_capacity_mah = static_cast<int32_t>(info.FullChargedCapacity / voltage);
            }
            copy_string(out.condition, sizeof(out.condition), out.health >= 0.8 ? "Normal" : "Service Recommended");
        }
    }
    if (has_status) {
        out.fully_charged = out.on_ac_power && !out.charging && out.level >= 0.99;
        if (battery_status.Rate != BATTERY_UNKNOWN_RATE) out.power_watts = battery_status.Rate / 1000.0;
        if (has_info && out.charging && battery_status.Rate > 0 && info.FullChargedCapacity > battery_status.Capacity)
            out.minutes_to_full =
                static_cast<int32_t>((info.FullChargedCapacity - battery_status.Capacity) * 60.0 / battery_status.Rate);
    }
    ULONG tenths_kelvin = 0;
    if (battery_query(device.get(), query.BatteryTag, BatteryTemperature, tenths_kelvin) && tenths_kelvin > 0)
        out.temperature = (tenths_kelvin - 2731.5) / 10.0;
    return true;
}

// ---- Power requests: what keeps the machine or the display awake ----
//
// What `powercfg /requests` lists. The kernel's own answer (CallNtPowerInformation's
// GetPowerRequestList) has no public layout and refuses the call on current builds, so powercfg
// itself is run, hidden, and its report is parsed: one section per request type in a fixed order
// (display, system, away mode, execution, performance boost, active lock screen; the headers may
// be localized, their order is not), entries tagged [PROCESS], [SERVICE] or [DRIVER], each followed
// by its reason. powercfg needs administrator rights: without them the list is empty.

namespace {

// Runs `command` without a window and returns what it printed (OEM code page, as UTF-8); empty
// when it couldn't run or took longer than `timeout_ms`.
std::string run_hidden(std::wstring command, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_raw = nullptr, write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &inheritable, 0)) return {};
    Handle read_end(read_raw), write_end(write_raw);
    SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_end.get();
    startup.hStdError = write_end.get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process))
        return {};
    Handle child(process.hProcess);
    CloseHandle(process.hThread);
    write_end.reset();  // our copy: the read sees EOF once the child is gone
    std::string output;
    char buffer[4096];
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_end.get(), nullptr, 0, nullptr, &available, nullptr)) break;  // closed
        if (available == 0) {
            if (WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0) {
                PeekNamedPipe(read_end.get(), nullptr, 0, nullptr, &available, nullptr);
                if (available == 0) break;
            } else if (GetTickCount64() > deadline) {
                TerminateProcess(child.get(), 1);
                break;
            } else {
                Sleep(20);
                continue;
            }
        }
        DWORD read = 0;
        if (!ReadFile(read_end.get(), buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        output.append(buffer, read);
    }
    if (output.empty()) return {};
    const int size = MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring text(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), text.data(), size);
    return utf8(text);
}

// "\Device\HarddiskVolume3\Users\me\app.exe" -> "C:\Users\me\app.exe" (unchanged when no drive matches).
std::string dos_path_of(const std::string &native) {
    const DWORD drives = GetLogicalDrives();
    const std::string lowered = lower_ascii(native);
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t drive[] = {static_cast<wchar_t>(L'A' + i), L':', L'\0'};
        wchar_t target[512] = {};
        if (QueryDosDeviceW(drive, target, 512) == 0) continue;
        const std::string device = lower_ascii(utf8(target, wcslen(target)));
        if (device.empty() || lowered.size() <= device.size() || lowered.compare(0, device.size(), device) != 0 ||
            lowered[device.size()] != '\\')
            continue;
        return utf8(drive, 2) + native.substr(device.size());
    }
    return native;
}

std::string trim(const std::string &text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

}  // namespace

// The parsing half of power_assertions(): `report` is powercfg's output. Exposed for the tests.
std::vector<PowerAssertion> parse_power_requests(const std::string &report) {
    struct Section {
        uint32_t kind;
        const char *type;
    };
    static const Section kSections[] = {
        {PC_ASSERT_DISPLAY_SLEEP | PC_ASSERT_SYSTEM_SLEEP, "DisplayRequired"},
        {PC_ASSERT_SYSTEM_SLEEP, "SystemRequired"},
        {PC_ASSERT_SYSTEM_SLEEP, "AwayModeRequired"},
        {PC_ASSERT_SYSTEM_SLEEP, "ExecutionRequired"},
        {0, "PerfBoost"},
        {0, "ActiveLockScreen"},
    };
    struct Parsed {
        PowerAssertion assertion;
        std::string holder;  // the executable or device the request belongs to
    };
    std::vector<Parsed> parsed;
    int section = -1;
    int current = -1;  // index into parsed of the entry collecting reason lines, -1 none
    size_t start = 0;
    while (start <= report.size()) {
        size_t end = report.find('\n', start);
        if (end == std::string::npos) end = report.size();
        const std::string line = trim(report.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;
        if (line.back() == ':' && line.find('[') == std::string::npos) {
            ++section;
            current = -1;
            continue;
        }
        if (line[0] == '[') {
            current = -1;
            const size_t close = line.find(']');
            if (close == std::string::npos || section < 0 || section >= static_cast<int>(std::size(kSections)))
                continue;
            const Section &s = kSections[static_cast<size_t>(section)];
            if (!s.kind) continue;  // boosts and lock-screen holds keep nothing awake
            const std::string tag = line.substr(1, close - 1);
            const std::string rest = trim(line.substr(close + 1));
            Parsed p;
            p.assertion.kind = s.kind;
            p.assertion.type = s.type;
            if (tag == "PROCESS" || tag == "SERVICE") {
                // "\Device\HarddiskVolume3\...\app.exe", for a service followed by "(name)".
                std::string path = rest;
                if (const size_t paren = path.find(" ("); tag == "SERVICE" && paren != std::string::npos)
                    path.resize(paren);
                p.holder = path;
                p.assertion.pid = pid_of_executable(dos_path_of(path));
                if (tag == "SERVICE") p.assertion.reason = "Windows service " + rest.substr(path.size());
            } else {
                p.assertion.pid = 0;  // a driver: no process behind it
                std::string description = rest;
                if (const size_t paren = description.find(" ("); paren != std::string::npos)
                    description.resize(paren);  // drop the device instance id
                p.holder = rest;
                p.assertion.reason = description;
            }
            parsed.push_back(std::move(p));
            current = static_cast<int>(parsed.size()) - 1;
            continue;
        }
        if (current < 0) continue;  // "None." under an empty section
        std::string &reason = parsed[static_cast<size_t>(current)].assertion.reason;
        reason += (reason.empty() ? "" : " · ") + line;
    }
    // One holder with both a display and a system request shows once, with both kinds.
    std::vector<Parsed> merged;
    for (Parsed &p : parsed) {
        bool joined = false;
        for (Parsed &m : merged) {
            if (m.holder != p.holder || m.assertion.reason != p.assertion.reason) continue;
            m.assertion.kind |= p.assertion.kind;
            if (m.assertion.type.find(p.assertion.type) == std::string::npos)
                m.assertion.type += "+" + p.assertion.type;
            joined = true;
            break;
        }
        if (!joined) merged.push_back(std::move(p));
    }
    std::vector<PowerAssertion> result;
    result.reserve(merged.size());
    for (Parsed &m : merged) result.push_back(std::move(m.assertion));
    return result;
}

std::vector<PowerAssertion> power_assertions() {
    if (!is_elevated()) return {};  // powercfg refuses the listing to a standard user
    wchar_t system32[MAX_PATH] = {};
    GetSystemDirectoryW(system32, MAX_PATH);
    const std::string report = run_hidden(std::wstring(L"\"") + system32 + L"\\powercfg.exe\" /requests", 5000);
    if (report.empty()) return {};
    std::vector<PowerAssertion> result = parse_power_requests(report);
    const int64_t now = filetime_to_unix([] {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return filetime_value(ft);
    }());
    for (PowerAssertion &a : result) a.created = now;  // powercfg doesn't say when it was taken
    return result;
}

}  // namespace procyon::platform

#endif  // _WIN32
