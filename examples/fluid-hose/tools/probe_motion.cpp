#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <mach/mach_error.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// SPU usage IDs, wake commands, and nominal Q16 report layout reference:
// https://github.com/olvvier/apple-silicon-accelerometer/blob/main/macimu/_spu.py
namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }

int property(io_service_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    int number = -1;
    if (value) {
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &number);
        CFRelease(value);
    }
    return number;
}

bool imu(io_service_t service, int usage) {
    return property(service, CFSTR("PrimaryUsagePage")) == 0xff00 &&
           property(service, CFSTR("PrimaryUsage")) == usage;
}

struct DriverWake {
    struct Setting { CFStringRef key; const char* name; int value; CFTypeRef previous; bool changed; };
    io_service_t driver = IO_OBJECT_NULL;
    std::array<Setting, 3> settings{{
        {CFSTR("SensorPropertyReportingState"), "reporting", 1, nullptr, false},
        {CFSTR("SensorPropertyPowerState"), "power", 1, nullptr, false},
        {CFSTR("ReportInterval"), "report interval (microseconds)", 1000, nullptr, false}
    }};

    ~DriverWake() {
        // Restore published values; absent power/reporting commands default to off.
        // Only the selected IMU drivers are touched, never the ALS or other devices.
        for (auto setting = settings.rbegin(); setting != settings.rend(); ++setting) {
            if (driver && setting->changed) {
                const int off = 0;
                CFTypeRef restore = setting->previous ? CFRetain(setting->previous) :
                    CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &off);
                const auto result = IORegistryEntrySetCFProperty(driver, setting->key, restore);
                if (result != kIOReturnSuccess)
                    std::fprintf(stderr, "Restore %s failed: 0x%08x (%s)\n",
                                 setting->name, result, mach_error_string(result));
                CFRelease(restore);
            }
            if (setting->previous) CFRelease(setting->previous);
        }
        if (driver) IOObjectRelease(driver);
    }

    void enable(int usage, const char* name) {
        io_iterator_t iterator = IO_OBJECT_NULL;
        const auto matched = IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("AppleSPUHIDDriver"), &iterator);
        if (matched != KERN_SUCCESS) {
            std::printf("  %s driver lookup failed: 0x%08x (%s)\n",
                        name, matched, mach_error_string(matched));
            return;
        }
        while (const io_service_t service = IOIteratorNext(iterator)) {
            if (!driver && imu(service, usage)) driver = service;
            else IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        if (!driver) { std::printf("  %s: no matching SPU driver to wake.\n", name); return; }
        // Snapshot every setting before the first command can affect the driver.
        for (auto& setting : settings)
            setting.previous = IORegistryEntryCreateCFProperty(driver, setting.key, kCFAllocatorDefault, 0);
        for (auto& setting : settings) {
            CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &setting.value);
            const auto result = IORegistryEntrySetCFProperty(driver, setting.key, value);
            CFRelease(value);
            setting.changed = result == kIOReturnSuccess;
            if (setting.changed)
                std::printf("  %s wake %s=%d: OK\n", name, setting.name, setting.value);
            else
                std::printf("  %s wake %s failed: 0x%08x (%s)\n",
                            name, setting.name, result, mach_error_string(result));
        }
    }
};

struct Sensor {
    const char* name = nullptr;
    int usage = 0;
    IOHIDDeviceRef device = nullptr;
    bool open = false;
    std::array<std::uint8_t, 4096> buffer{};
    unsigned reports = 0, samples = 0, errors = 0;
    CFIndex last_length = 0;
    Clock::time_point received{};
    std::array<double, 3> xyz{};
    DriverWake wake;

    ~Sensor() {
        if (open) {
            IOHIDDeviceUnscheduleFromRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        }
        if (device) CFRelease(device);
    }
};

void report(void* context, IOReturn result, void*, IOHIDReportType,
            std::uint32_t, std::uint8_t* bytes, CFIndex length, std::uint64_t) {
    auto& sensor = *static_cast<Sensor*>(context);
    if (result != kIOReturnSuccess) { ++sensor.errors; return; }
    ++sensor.reports;
    sensor.last_length = length;
    if (length != 22) return;
    for (int axis = 0; axis < 3; ++axis) {
        std::int32_t value;
        std::memcpy(&value, bytes + 6 + 4 * axis, sizeof(value));
        sensor.xyz[axis] = value / 65536.0;
    }
    ++sensor.samples;
    sensor.received = Clock::now();
}

void show(const Sensor& sensor) {
    if (!sensor.samples) {
        std::printf("%s: NO DATA (reports=%u, bytes=%ld, callback errors=%u)\n",
                    sensor.name, sensor.reports, sensor.last_length, sensor.errors);
        return;
    }
    const double magnitude = std::sqrt(sensor.xyz[0] * sensor.xyz[0] +
        sensor.xyz[1] * sensor.xyz[1] + sensor.xyz[2] * sensor.xyz[2]);
    const double age = std::chrono::duration<double>(Clock::now() - sensor.received).count();
    std::printf("%s: x=%+.4f y=%+.4f z=%+.4f %s; magnitude=%.4f; samples=%u; age=%.3fs\n",
                sensor.name, sensor.xyz[0], sensor.xyz[1], sensor.xyz[2],
                sensor.usage == 3 ? "nominal g" : "nominal deg/s", magnitude, sensor.samples, age);
}

bool self_test() {
    Sensor sensor;
    std::array<std::uint8_t, 22> bytes{};
    const std::array<std::int32_t, 3> values{{32768, -16384, 65536}};
    for (int axis = 0; axis < 3; ++axis)
        std::memcpy(bytes.data() + 6 + axis * 4, &values[axis], sizeof(values[axis]));
    report(&sensor, kIOReturnSuccess, nullptr, kIOHIDReportTypeInput, 0, bytes.data(), 17, 0);
    if (sensor.samples != 0 || sensor.reports != 1) return false;
    report(&sensor, kIOReturnSuccess, nullptr, kIOHIDReportTypeInput, 0, bytes.data(), bytes.size(), 0);
    if (sensor.samples != 1 || sensor.xyz != std::array<double, 3>{{0.5, -0.25, 1.0}}) return false;
    report(&sensor, kIOReturnError, nullptr, kIOHIDReportTypeInput, 0, bytes.data(), bytes.size(), 0);
    return sensor.samples == 1 && sensor.errors == 1;
}
}

int main(int argc, char** argv) {
    unsigned duration = 10;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            std::puts("Usage: probe-motion [--duration SECONDS] [--self-test]\n"
                      "Wakes only the SPU accelerometer/gyroscope and prints live readings.\n"
                      "Default: 10 seconds; accepted duration: 1..60. Ctrl+C restores sensor settings.\n"
                      "Missing samples are reported as NO DATA, never as measured zeros.");
            return 0;
        }
        if (argument == "--self-test") {
            const bool passed = self_test();
            std::puts(passed ? "PASS: signed Q16 decoding, short reports, and callback errors." : "FAIL: report decoding.");
            return passed ? 0 : 1;
        }
        if (argument == "--duration" && i + 1 < argc) {
            const char* value = argv[++i];
            const auto parsed = std::from_chars(value, value + std::strlen(value), duration);
            if (parsed.ec == std::errc{} && *parsed.ptr == '\0' && duration >= 1 && duration <= 60) continue;
        }
        std::fprintf(stderr, "Invalid or incomplete option: %s; use --help.\n", argv[i]);
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto matched = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("AppleSPUHIDDevice"), &iterator);
    if (matched != KERN_SUCCESS) {
        std::printf("Sensor enumeration failed: 0x%08x (%s)\n", matched, mach_error_string(matched));
        return 2;
    }
    std::vector<std::unique_ptr<Sensor>> sensors;
    while (const io_service_t service = IOIteratorNext(iterator)) {
        const int page = property(service, CFSTR("PrimaryUsagePage"));
        const int usage = property(service, CFSTR("PrimaryUsage"));
        if (page != 0xff00 || (usage != 3 && usage != 9)) { IOObjectRelease(service); continue; }
        auto sensor = std::make_unique<Sensor>();
        sensor->name = usage == 3 ? "Accelerometer" : "Gyroscope";
        sensor->usage = usage;
        sensor->device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
        IOObjectRelease(service);
        std::printf("%s: present, usage page=0x%04x usage=%d\n", sensor->name, page, usage);
        if (!sensor->device) { std::puts("  Cannot create HID device interface in this process."); continue; }
        const auto access = IOHIDDeviceOpen(sensor->device, kIOHIDOptionsTypeNone);
        if (access != kIOReturnSuccess) {
            std::printf("  Open failed: 0x%08x (%s)\n", access, mach_error_string(access));
            continue;
        }
        sensor->open = true;
        std::puts("  Opened non-exclusively.");
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(sensor->device,
            sensor->buffer.data(), sensor->buffer.size(), report, sensor.get());
        IOHIDDeviceScheduleWithRunLoop(sensor->device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        sensor->wake.enable(usage, sensor->name);
        sensors.push_back(std::move(sensor));
    }
    IOObjectRelease(iterator);
    if (sensors.empty()) {
        std::puts("No readable IMU device opened. Sensor access must be checked in your normal Terminal.");
        return 2;
    }
    std::printf("Listening for %u seconds. Tilt the Mac gently; Ctrl+C stops.\n", duration);
    std::fflush(stdout);
    const auto deadline = Clock::now() + std::chrono::seconds(duration);
    auto next_output = Clock::now();
    while (!stopped && Clock::now() < deadline) {
        const auto run = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        if (Clock::now() >= next_output) {
            for (const auto& sensor : sensors) show(*sensor);
            std::fflush(stdout);
            next_output = Clock::now() + std::chrono::milliseconds(500);
        }
        if (run == kCFRunLoopRunFinished || run == kCFRunLoopRunStopped) {
            std::puts("Sensor run loop has no active sources or was stopped.");
            break;
        }
    }
    bool acceleration = false;
    for (const auto& sensor : sensors) {
        show(*sensor);
        if (sensor->usage == 3 && sensor->samples &&
            std::chrono::duration<double>(Clock::now() - sensor->received).count() < 1.0) acceleration = true;
    }
    if (!acceleration)
        std::puts("No fresh acceleration samples arrived. Check wake-up status above; this is missing data, not zero gravity.");
    return acceleration ? 0 : 3;
}
