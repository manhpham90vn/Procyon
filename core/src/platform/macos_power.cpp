// macOS power: battery (IOPowerSources + AppleSmartBattery), sleep assertions (IOPMLib) and
// temperature sensors. Apple Silicon publishes its die and battery sensors as HID services; that
// event-system API is private, so it is resolved with dlsym and a miss only hides temperatures.
#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>

#include "../platform.hpp"

namespace procyon::platform {
namespace {

void copy_string(char *dst, size_t capacity, const std::string &src) {
    (void)std::snprintf(dst, capacity, "%s", src.c_str());
}

std::string cf_string(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    char buffer[512];
    return CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8)
               ? buffer
               : std::string();
}

bool cf_int(CFTypeRef value, int64_t &out) {
    return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
           CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &out);
}

bool cf_bool(CFTypeRef value) {
    if (!value) return false;
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    int64_t number = 0;
    return cf_int(value, number) && number != 0;
}

// ---- HID temperature sensors ----

using EventSystemRef = void *;
using ServiceRef = void *;
using EventRef = void *;
constexpr int64_t kTemperatureEvent = 15;  // kIOHIDEventTypeTemperature

struct HidApi {
    EventSystemRef (*create)(CFAllocatorRef) = nullptr;
    int (*set_matching)(EventSystemRef, CFDictionaryRef) = nullptr;
    CFArrayRef (*copy_services)(EventSystemRef) = nullptr;
    CFTypeRef (*copy_property)(ServiceRef, CFStringRef) = nullptr;
    EventRef (*copy_event)(ServiceRef, int64_t, int32_t, int64_t) = nullptr;
    double (*float_value)(EventRef, int32_t) = nullptr;

    bool load() {
        void *lib = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
        if (!lib) return false;
        auto symbol = [lib](auto &out, const char *name) {
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(dlsym(lib, name));
            return out != nullptr;
        };
        return symbol(create, "IOHIDEventSystemClientCreate") &&
               symbol(set_matching, "IOHIDEventSystemClientSetMatching") &&
               symbol(copy_services, "IOHIDEventSystemClientCopyServices") &&
               symbol(copy_property, "IOHIDServiceClientCopyProperty") &&
               symbol(copy_event, "IOHIDServiceClientCopyEvent") && symbol(float_value, "IOHIDEventGetFloatValue");
    }
};

struct Sensors {
    double cpu = -1;      // hottest die sensor
    double battery = -1;  // mean of the gas gauge readings
    double disk = -1;     // hottest NAND channel of the internal SSD
};

// Reads every temperature sensor once. The client and service list are created on first use.
Sensors read_sensors() {
    static std::mutex mutex;
    static HidApi api;
    static CFArrayRef services = nullptr;
    static const bool loaded = [] {
        if (!api.load()) return false;
        EventSystemRef client = api.create(kCFAllocatorDefault);
        if (!client) return false;
        int page = 0xff00, usage = 5;  // Apple vendor page, temperature sensors
        CFNumberRef page_number = CFNumberCreate(nullptr, kCFNumberIntType, &page);
        CFNumberRef usage_number = CFNumberCreate(nullptr, kCFNumberIntType, &usage);
        const void *keys[] = {CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage")};
        const void *values[] = {page_number, usage_number};
        CFDictionaryRef matching = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);
        api.set_matching(client, matching);
        services = api.copy_services(client);  // keeps the client alive for the app's lifetime
        CFRelease(matching);
        CFRelease(usage_number);
        CFRelease(page_number);
        return services != nullptr;
    }();

    Sensors sensors;
    if (!loaded) return sensors;
    std::lock_guard lock(mutex);
    double battery_sum = 0;
    int battery_count = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(services); ++i) {
        auto service = const_cast<void *>(CFArrayGetValueAtIndex(services, i));
        CFTypeRef product = api.copy_property(service, CFSTR("Product"));
        const std::string name = cf_string(product);
        if (product) CFRelease(product);
        const bool is_die = name.find("tdie") != std::string::npos;
        const bool is_battery = name.find("battery") != std::string::npos;
        const bool is_nand = name.find("NAND") != std::string::npos;  // "NAND CH0 temp"
        if (!is_die && !is_battery && !is_nand) continue;
        EventRef event = api.copy_event(service, kTemperatureEvent, 0, 0);
        if (!event) continue;
        const double celsius = api.float_value(event, static_cast<int32_t>(kTemperatureEvent << 16));
        CFRelease(event);
        if (celsius <= 0 || celsius > 150) continue;  // disconnected sensors read negative
        if (is_die) sensors.cpu = std::max(sensors.cpu, celsius);
        if (is_nand) sensors.disk = std::max(sensors.disk, celsius);
        if (is_battery) {
            battery_sum += celsius;
            ++battery_count;
        }
    }
    if (battery_count) sensors.battery = battery_sum / battery_count;
    return sensors;
}

// ---- battery registry ----

struct SmartBattery {
    int64_t cycle_count = -1;
    int64_t design_capacity = -1;
    int64_t nominal_capacity = -1;
    int64_t voltage_mv = -1;
    int64_t amperage_ma = 0;
    std::string adapter;
};

SmartBattery read_smart_battery() {
    SmartBattery result;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
    if (!service) return result;
    CFMutableDictionaryRef props = nullptr;
    if (IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
        cf_int(CFDictionaryGetValue(props, CFSTR("CycleCount")), result.cycle_count);
        cf_int(CFDictionaryGetValue(props, CFSTR("Voltage")), result.voltage_mv);
        if (!cf_int(CFDictionaryGetValue(props, CFSTR("InstantAmperage")), result.amperage_ma))
            cf_int(CFDictionaryGetValue(props, CFSTR("Amperage")), result.amperage_ma);
        // Capacities in mAh live in BatteryData (top-level MaxCapacity is a percentage on Apple Silicon).
        auto data = CFDictionaryGetValue(props, CFSTR("BatteryData"));
        if (data && CFGetTypeID(data) == CFDictionaryGetTypeID()) {
            auto dict = static_cast<CFDictionaryRef>(data);
            cf_int(CFDictionaryGetValue(dict, CFSTR("DesignCapacity")), result.design_capacity);
            if (!cf_int(CFDictionaryGetValue(dict, CFSTR("NominalChargeCapacity")), result.nominal_capacity))
                cf_int(CFDictionaryGetValue(dict, CFSTR("FullChargeCapacity")), result.nominal_capacity);
        }
        if (result.design_capacity <= 0)
            cf_int(CFDictionaryGetValue(props, CFSTR("DesignCapacity")), result.design_capacity);
        if (result.nominal_capacity <= 0)
            cf_int(CFDictionaryGetValue(props, CFSTR("AppleRawMaxCapacity")), result.nominal_capacity);
        auto adapter = CFDictionaryGetValue(props, CFSTR("AdapterDetails"));
        if (adapter && CFGetTypeID(adapter) == CFDictionaryGetTypeID()) {
            auto dict = static_cast<CFDictionaryRef>(adapter);
            std::string name = cf_string(CFDictionaryGetValue(dict, CFSTR("Name")));
            while (!name.empty() && name.back() == ' ') name.pop_back();
            int64_t watts = 0;
            if (name.empty() && cf_int(CFDictionaryGetValue(dict, CFSTR("Watts")), watts) && watts > 0)
                name = std::to_string(watts) + "W adapter";
            result.adapter = name;
        }
        CFRelease(props);
    }
    IOObjectRelease(service);
    return result;
}

uint32_t assertion_kind(const std::string &type) {
    if (type == "PreventUserIdleSystemSleep" || type == "PreventSystemSleep" || type == "NoIdleSleepAssertion")
        return PC_ASSERT_SYSTEM_SLEEP;
    if (type == "PreventUserIdleDisplaySleep" || type == "NoDisplaySleepAssertion")
        return PC_ASSERT_DISPLAY_SLEEP | PC_ASSERT_SYSTEM_SLEEP;
    return 0;
}

}  // namespace

double cpu_temperature() { return read_sensors().cpu; }

void temperatures(double &cpu, double &disk) {
    const Sensors sensors = read_sensors();
    cpu = sensors.cpu;
    disk = sensors.disk;
}

bool battery(pc_battery &out) {
    out = {};
    out.minutes_to_empty = out.minutes_to_full = -1;
    out.cycle_count = out.design_capacity_mah = out.max_capacity_mah = -1;
    out.health = out.temperature = -1;

    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return false;
    CFArrayRef sources = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; sources && i < CFArrayGetCount(sources); ++i) {
        CFDictionaryRef source = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(sources, i));
        if (!source || cf_string(CFDictionaryGetValue(source, CFSTR(kIOPSTypeKey))) != kIOPSInternalBatteryType)
            continue;
        out.present = cf_bool(CFDictionaryGetValue(source, CFSTR(kIOPSIsPresentKey)));
        int64_t current = 0, max = 0, minutes = -1;
        if (cf_int(CFDictionaryGetValue(source, CFSTR(kIOPSCurrentCapacityKey)), current) &&
            cf_int(CFDictionaryGetValue(source, CFSTR(kIOPSMaxCapacityKey)), max) && max > 0)
            out.level = std::clamp(static_cast<double>(current) / static_cast<double>(max), 0.0, 1.0);
        out.on_ac_power = cf_string(CFDictionaryGetValue(source, CFSTR(kIOPSPowerSourceStateKey))) == kIOPSACPowerValue;
        out.charging = cf_bool(CFDictionaryGetValue(source, CFSTR(kIOPSIsChargingKey)));
        out.fully_charged = cf_bool(CFDictionaryGetValue(source, CFSTR(kIOPSIsChargedKey)));
        if (cf_int(CFDictionaryGetValue(source, CFSTR(kIOPSTimeToEmptyKey)), minutes) && minutes >= 0)
            out.minutes_to_empty = static_cast<int32_t>(minutes);
        if (cf_int(CFDictionaryGetValue(source, CFSTR(kIOPSTimeToFullChargeKey)), minutes) && minutes >= 0)
            out.minutes_to_full = static_cast<int32_t>(minutes);
        std::string condition = cf_string(CFDictionaryGetValue(source, CFSTR(kIOPSBatteryHealthConditionKey)));
        if (condition.empty()) condition = cf_string(CFDictionaryGetValue(source, CFSTR(kIOPSBatteryHealthKey)));
        if (condition == "Good") condition = "Normal";
        if (condition == "Poor") condition = "Service Recommended";
        copy_string(out.condition, sizeof(out.condition), condition);
        break;
    }
    if (sources) CFRelease(sources);
    CFRelease(info);
    if (!out.present) return true;

    const SmartBattery smart = read_smart_battery();
    out.cycle_count = static_cast<int32_t>(smart.cycle_count);
    if (smart.design_capacity > 0) out.design_capacity_mah = static_cast<int32_t>(smart.design_capacity);
    if (smart.nominal_capacity > 0) out.max_capacity_mah = static_cast<int32_t>(smart.nominal_capacity);
    // Like System Settings: maximum capacity relative to new, capped at 100%.
    if (smart.design_capacity > 0 && smart.nominal_capacity > 0)
        out.health =
            std::min(1.0, static_cast<double>(smart.nominal_capacity) / static_cast<double>(smart.design_capacity));
    if (smart.voltage_mv > 0) out.power_watts = static_cast<double>(smart.voltage_mv) * smart.amperage_ma / 1e6;
    if (out.on_ac_power) copy_string(out.adapter, sizeof(out.adapter), smart.adapter);
    out.temperature = read_sensors().battery;
    return true;
}

std::vector<PowerAssertion> power_assertions() {
    std::vector<PowerAssertion> result;
    CFDictionaryRef by_process = nullptr;
    if (IOPMCopyAssertionsByProcess(&by_process) != kIOReturnSuccess || !by_process) return result;
    const CFIndex count = CFDictionaryGetCount(by_process);
    std::vector<const void *> keys(static_cast<size_t>(count)), values(static_cast<size_t>(count));
    CFDictionaryGetKeysAndValues(by_process, keys.data(), values.data());
    for (CFIndex i = 0; i < count; ++i) {
        int64_t pid = 0;
        if (!cf_int(keys[i], pid) || CFGetTypeID(values[i]) != CFArrayGetTypeID()) continue;
        auto list = static_cast<CFArrayRef>(values[i]);
        for (CFIndex j = 0; j < CFArrayGetCount(list); ++j) {
            auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, j));
            if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) continue;
            PowerAssertion a;
            a.pid = static_cast<int32_t>(pid);
            a.type = cf_string(CFDictionaryGetValue(dict, kIOPMAssertionTypeKey));
            a.kind = assertion_kind(a.type);
            if (!a.kind) continue;
            int64_t level = 0;
            if (cf_int(CFDictionaryGetValue(dict, kIOPMAssertionLevelKey), level) && level == 0) continue;  // released
            int64_t behalf = -1;
            if (cf_int(CFDictionaryGetValue(dict, CFSTR("AssertionOnBehalfOfPID")), behalf))
                a.on_behalf_of = static_cast<int32_t>(behalf);
            a.reason = cf_string(CFDictionaryGetValue(dict, CFSTR("Details")));
            if (a.reason.empty()) a.reason = cf_string(CFDictionaryGetValue(dict, kIOPMAssertionNameKey));
            auto start = CFDictionaryGetValue(dict, CFSTR("AssertStartWhen"));
            if (start && CFGetTypeID(start) == CFDateGetTypeID())
                a.created = static_cast<int64_t>(CFDateGetAbsoluteTime(static_cast<CFDateRef>(start)) +
                                                 kCFAbsoluteTimeIntervalSince1970);
            result.push_back(std::move(a));
        }
    }
    CFRelease(by_process);
    return result;
}

}  // namespace procyon::platform

#endif  // __APPLE__
