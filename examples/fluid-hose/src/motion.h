#pragma once
#include "fluid_types.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <sys/types.h>

struct MotionVector { float x, y, z; };
constexpr std::uint32_t motion_magic = 0x4d4f544e;
constexpr std::uint32_t motion_accel = 1, motion_gyro = 2, motion_lid = 4;
struct MotionPacket {
    std::uint32_t magic = motion_magic;
    std::uint32_t flags = 0;
    std::uint64_t timestamp = 0;
    MotionVector acceleration{0, 0, 0};
    MotionVector rotation{0, 0, 0};
    float lid_angle = 110;
    float padding = 0;
};
static_assert(sizeof(MotionPacket) == 48, "Helper and app must share the motion packet layout");
inline std::uint64_t motion_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class TiltGravity {
public:
    void reset();
    bool update(const MotionPacket& packet);
    bool fresh(std::uint64_t now) const;
    FluidPoint gravity(std::uint64_t now, bool reverse_horizontal = false) const;
private:
    MotionVector direction_{0, 0, -1};
    float lid_angle_ = 110;
    std::uint64_t timestamp_ = 0;
};

class MotionProcess {
public:
    ~MotionProcess();
    bool start(const std::string& executable, bool mock = false);
    void stop();
    bool poll(MotionPacket& latest);
    bool running() const { return pid_ > 0; }
private:
    pid_t pid_ = -1;
    int control_ = -1, data_ = -1;
    std::array<unsigned char, sizeof(MotionPacket)> bytes_{};
    std::size_t used_ = 0;
};

bool run_motion_checks(const std::string& helper);
