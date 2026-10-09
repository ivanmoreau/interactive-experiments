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
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include "../src/motion.h"

// SPU usage IDs, wake commands, and nominal Q16 report layout reference:
// https://github.com/olvvier/apple-silicon-accelerometer/blob/main/macimu/_spu.py
namespace {
using Clock = std::chrono::steady_clock;
FILE* output = stdout;
bool streaming = false;
template<typename... Args> void diagnostic(const char* format, Args... args) {
    if constexpr (sizeof...(Args) == 0) std::fputs(format, output);
    else std::fprintf(output, format, args...);
}
void line(const char* text) { std::fprintf(output, "%s\n", text); }
bool owner_alive() {
    if (!streaming) return true;
    pollfd owner{STDIN_FILENO, POLLIN, 0};
    const int events = poll(&owner, 1, 0);
    if (events < 0) return errno == EINTR;
    if (owner.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;
    if (owner.revents & POLLIN) {
        char commands[32];
        const auto count = read(STDIN_FILENO, commands, sizeof(commands));
        if (count <= 0) return false;
        for (int i = 0; i < count; ++i) if (commands[i] == 'Q') return false;
    }
    return true;
}
bool send_packet(const MotionPacket& packet) {
    const auto bytes = write(STDOUT_FILENO, &packet, sizeof(packet));
    return bytes == sizeof(packet) || (bytes < 0 && (errno == EAGAIN || errno == EINTR));
}
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

bool matches(io_service_t service, int page, int usage) {
    return property(service, CFSTR("PrimaryUsagePage")) == page &&
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

    void enable(int page, int usage, const char* name) {
        io_iterator_t iterator = IO_OBJECT_NULL;
        const auto matched = IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("AppleSPUHIDDriver"), &iterator);
        if (matched != KERN_SUCCESS) {
            diagnostic("  %s driver lookup failed: 0x%08x (%s)\n",
                        name, matched, mach_error_string(matched));
            return;
        }
        while (const io_service_t service = IOIteratorNext(iterator)) {
            if (!driver && matches(service, page, usage)) driver = service;
            else IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        if (!driver) { diagnostic("  %s: no matching SPU driver to wake.\n", name); return; }
        // Snapshot every setting before the first command can affect the driver.
        for (auto& setting : settings)
            setting.previous = IORegistryEntryCreateCFProperty(driver, setting.key, kCFAllocatorDefault, 0);
        for (auto& setting : settings) {
            CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &setting.value);
            const auto result = IORegistryEntrySetCFProperty(driver, setting.key, value);
            CFRelease(value);
            setting.changed = result == kIOReturnSuccess;
            if (setting.changed)
                diagnostic("  %s wake %s=%d: OK\n", name, setting.name, setting.value);
            else
                diagnostic("  %s wake %s failed: 0x%08x (%s)\n",
                            name, setting.name, result, mach_error_string(result));
        }
    }
};

struct Sensor {
    const char* name = nullptr;
    int usage = 0, page = 0;
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
    if (sensor.page == 0x20 && sensor.usage == 138) {
        if (length < 3 || bytes[0] != 1) return;
        std::uint16_t angle;
        std::memcpy(&angle, bytes + 1, sizeof(angle));
        angle &= 0x1ff;
        if (angle > 180) return;
        sensor.xyz[0] = angle;
        ++sensor.samples;
        sensor.received = Clock::now();
        return;
    }
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
        diagnostic("%s: NO DATA (reports=%u, bytes=%ld, callback errors=%u)\n",
                    sensor.name, sensor.reports, sensor.last_length, sensor.errors);
        return;
    }
    const double magnitude = std::sqrt(sensor.xyz[0] * sensor.xyz[0] +
        sensor.xyz[1] * sensor.xyz[1] + sensor.xyz[2] * sensor.xyz[2]);
    const double age = std::chrono::duration<double>(Clock::now() - sensor.received).count();
    diagnostic("%s: x=%+.4f y=%+.4f z=%+.4f %s; magnitude=%.4f; samples=%u; age=%.3fs\n",
                sensor.name, sensor.xyz[0], sensor.xyz[1], sensor.xyz[2],
                sensor.usage == 3 ? "nominal g" : "nominal deg/s", magnitude, sensor.samples, age);
}

bool self_test() {
    Sensor sensor;
    sensor.page = 0xff00; sensor.usage = 3;
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
    bool mock = false;
    bool observe_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--observe-only") { observe_only = true; continue; }
        if (argument == "--stream" || argument == "--stream-mock") {
            streaming = true; output = stderr; mock = argument == "--stream-mock"; continue;
        }
        if (argument == "--help") {
            line("Usage: probe-motion [--duration SECONDS] [--observe-only] [--self-test]\n"
                      "Wakes only the SPU accelerometer/gyroscope and prints live readings.\n"
                      "Default: 10 seconds; accepted duration: 1..60. Ctrl+C restores sensor settings.\n"
                      "--observe-only reads existing reports without enabling any driver settings.\n"
                      "Missing samples are reported as NO DATA, never as measured zeros.");
            return 0;
        }
        if (argument == "--self-test") {
            const bool passed = self_test();
            line(passed ? "PASS: signed Q16 decoding, short reports, and callback errors." : "FAIL: report decoding.");
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
    std::signal(SIGPIPE, SIG_IGN);
    if (streaming) {
        struct stat owner{};
        if (fstat(STDIN_FILENO, &owner) != 0 || !S_ISFIFO(owner.st_mode) || isatty(STDOUT_FILENO)) {
            std::fprintf(stderr, "Stream mode requires an owner control pipe and a binary output pipe.\n");
            return 1;
        }
        fcntl(STDOUT_FILENO, F_SETFL, fcntl(STDOUT_FILENO, F_GETFL) | O_NONBLOCK);
        if (!owner_alive()) return 0;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    if (mock) {
        line("Mock motion reader: no sensor hardware is opened.");
        while (!stopped && owner_alive()) {
            MotionPacket packet;
            packet.flags = motion_accel | motion_gyro | motion_lid;
            packet.timestamp = motion_now(); packet.acceleration = {0, 0, -1}; packet.lid_angle = 90;
            if (!send_packet(packet)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        line("Mock motion reader stopped after owner shutdown.");
        return 0;
    }
    io_iterator_t iterator = IO_OBJECT_NULL;
    const auto matched = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("AppleSPUHIDDevice"), &iterator);
    if (matched != KERN_SUCCESS) {
        diagnostic("Sensor enumeration failed: 0x%08x (%s)\n", matched, mach_error_string(matched));
        return 2;
    }
    std::vector<std::unique_ptr<Sensor>> sensors;
    while (const io_service_t service = IOIteratorNext(iterator)) {
        const int page = property(service, CFSTR("PrimaryUsagePage"));
        const int usage = property(service, CFSTR("PrimaryUsage"));
        const bool is_imu = page == 0xff00 && (usage == 3 || usage == 9);
        const bool is_lid = streaming && page == 0x20 && usage == 138;
        if (!is_imu && !is_lid) { IOObjectRelease(service); continue; }
        auto sensor = std::make_unique<Sensor>();
        sensor->name = is_lid ? "Lid angle" : usage == 3 ? "Accelerometer" : "Gyroscope";
        sensor->usage = usage; sensor->page = page;
        sensor->device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
        IOObjectRelease(service);
        diagnostic("%s: present, usage page=0x%04x usage=%d\n", sensor->name, page, usage);
        if (!sensor->device) { line("  Cannot create HID device interface in this process."); continue; }
        const auto access = IOHIDDeviceOpen(sensor->device, kIOHIDOptionsTypeNone);
        if (access != kIOReturnSuccess) {
            diagnostic("  Open failed: 0x%08x (%s)\n", access, mach_error_string(access));
            continue;
        }
        sensor->open = true;
        line("  Opened non-exclusively.");
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(sensor->device,
            sensor->buffer.data(), sensor->buffer.size(), report, sensor.get());
        IOHIDDeviceScheduleWithRunLoop(sensor->device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        if (is_lid) {
            // Read the system's existing lid sensor without changing its driver state.
            std::array<std::uint8_t, 3> angle{{1, 0, 0}};
            CFIndex size = angle.size();
            if (IOHIDDeviceGetReport(sensor->device, kIOHIDReportTypeInput, 1, angle.data(), &size) == kIOReturnSuccess)
                report(sensor.get(), kIOReturnSuccess, nullptr, kIOHIDReportTypeInput, 1, angle.data(), size, 0);
        }
        sensors.push_back(std::move(sensor));
    }
    IOObjectRelease(iterator);
    if (sensors.empty()) {
        line("No readable IMU device opened. Sensor access must be checked in your normal Terminal.");
        return 2;
    }
    // Borrow an existing reporting session when possible; otherwise wake only IMU.
    const auto observe = Clock::now() + std::chrono::milliseconds(100);
    while (!stopped && owner_alive() && Clock::now() < observe)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    for (const auto& sensor : sensors)
        if (!observe_only && !stopped && owner_alive() && sensor->page == 0xff00 && !sensor->samples)
            sensor->wake.enable(sensor->page, sensor->usage, sensor->name);
    diagnostic(streaming ? "Motion reader attached to app lifetime.\n" :
        "Listening for %u seconds. Tilt the Mac gently; Ctrl+C stops.\n", duration);
    std::fflush(output);
    const auto deadline = Clock::now() + std::chrono::seconds(duration);
    auto next_output = Clock::now();
    const auto began = Clock::now();
    while (!stopped && owner_alive() && (streaming || Clock::now() < deadline)) {
        const auto run = CFRunLoopRunInMode(kCFRunLoopDefaultMode, streaming ? 0.01 : 0.05, false);
        if (Clock::now() >= next_output) {
            if (streaming) {
                MotionPacket packet;
                for (const auto& sensor : sensors) {
                    const float age = std::chrono::duration<float>(Clock::now() - sensor->received).count();
                    if (!sensor->samples) continue;
                    if (sensor->usage == 3 && sensor->page == 0xff00 && age < 0.5f) {
                        packet.flags |= motion_accel;
                        packet.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            sensor->received.time_since_epoch()).count();
                        packet.acceleration = {static_cast<float>(sensor->xyz[0]),
                                               static_cast<float>(sensor->xyz[1]), static_cast<float>(sensor->xyz[2])};
                    } else if (sensor->usage == 9 && sensor->page == 0xff00 && age < 0.5f) {
                        packet.flags |= motion_gyro;
                        packet.rotation = {static_cast<float>(sensor->xyz[0]),
                                           static_cast<float>(sensor->xyz[1]), static_cast<float>(sensor->xyz[2])};
                    } else if (sensor->usage == 138 && sensor->page == 0x20) {
                        packet.flags |= motion_lid; packet.lid_angle = sensor->xyz[0];
                    }
                }
                if (!send_packet(packet)) break;
                if (!(packet.flags & motion_accel) && Clock::now() - began > std::chrono::seconds(2)) {
                    line("Motion reader received no fresh acceleration; stopping sensor session.");
                    break;
                }
            } else {
                for (const auto& sensor : sensors) show(*sensor);
                std::fflush(output);
            }
            next_output = Clock::now() + (streaming ? std::chrono::milliseconds(16) : std::chrono::milliseconds(500));
        }
        if (run == kCFRunLoopRunFinished || run == kCFRunLoopRunStopped) {
            line("Sensor run loop has no active sources or was stopped.");
            break;
        }
    }
    if (streaming) line("Motion reader stopped; restoring sensor settings.");
    bool acceleration = false;
    for (const auto& sensor : sensors) {
        show(*sensor);
        if (sensor->usage == 3 && sensor->samples &&
            std::chrono::duration<double>(Clock::now() - sensor->received).count() < 1.0) acceleration = true;
    }
    if (!acceleration)
        line("No fresh acceleration samples arrived. Check wake-up status above; this is missing data, not zero gravity.");
    return acceleration ? 0 : 3;
}
