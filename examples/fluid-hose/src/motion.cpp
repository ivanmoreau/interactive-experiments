#include "motion.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace {
bool finite(MotionVector v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float length(MotionVector v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
MotionVector normalized(MotionVector v) {
    const float n = length(v);
    return n > 0.0001f ? MotionVector{v.x/n, v.y/n, v.z/n} : MotionVector{0, 0, -1};
}
void close_fd(int& fd) { if (fd >= 0) close(fd); fd = -1; }
bool make_pipe(int (&fds)[2]) {
    if (pipe(fds) != 0) return false;
    for (int& fd : fds) {
        const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) { close_fd(fds[0]); close_fd(fds[1]); return false; }
        close(fd);
        fd = copy;
    }
    return true;
}
}

void TiltGravity::reset() { direction_ = {0, 0, -1}; lid_angle_ = 110; timestamp_ = 0; }
bool TiltGravity::fresh(std::uint64_t now) const {
    return timestamp_ && now >= timestamp_ && now - timestamp_ < 500000000;
}
bool TiltGravity::update(const MotionPacket& packet) {
    if (packet.magic != motion_magic || !(packet.flags & motion_accel) ||
        !packet.timestamp || packet.timestamp <= timestamp_ || !finite(packet.acceleration)) return false;
    const float magnitude = length(packet.acceleration);
    if (magnitude < 0.5f || magnitude > 1.5f) return false;
    const MotionVector measured = normalized(packet.acceleration);
    const float dt = timestamp_ ? static_cast<float>(packet.timestamp - timestamp_) * 1e-9f : 0;
    if (!timestamp_ || dt > 0.5f) direction_ = measured;
    else {
        MotionVector predicted = direction_;
        if ((packet.flags & motion_gyro) && finite(packet.rotation)) {
            constexpr float radians = 0.01745329252f;
            const MotionVector w{packet.rotation.x * radians, packet.rotation.y * radians, packet.rotation.z * radians};
            // A fixed world vector rotates opposite the device's angular velocity.
            const MotionVector cross{w.y * direction_.z - w.z * direction_.y,
                                      w.z * direction_.x - w.x * direction_.z,
                                      w.x * direction_.y - w.y * direction_.x};
            predicted = normalized({direction_.x - cross.x * dt, direction_.y - cross.y * dt,
                                    direction_.z - cross.z * dt});
        }
        // Correct gyro drift with the gravity reference; reject strong bumps.
        const float blend = magnitude >= 0.85f && magnitude <= 1.15f ? 1.0f - std::exp(-dt / 0.15f) : 0;
        direction_ = normalized({predicted.x + (measured.x - predicted.x) * blend,
                                 predicted.y + (measured.y - predicted.y) * blend,
                                 predicted.z + (measured.z - predicted.z) * blend});
    }
    if ((packet.flags & motion_lid) && std::isfinite(packet.lid_angle) &&
        packet.lid_angle >= 0 && packet.lid_angle <= 180) lid_angle_ = packet.lid_angle;
    timestamp_ = packet.timestamp;
    return true;
}
FluidPoint TiltGravity::gravity(std::uint64_t now, bool reverse_horizontal) const {
    if (!fresh(now)) return {0, 950};
    constexpr float radians = 0.01745329252f;
    const float opening = lid_angle_ * radians;
    // Screen X follows the chassis X axis. Screen down rotates with the hinge.
    // Preserve the projection's magnitude: a horizontal display has no in-plane gravity.
    return {direction_.x * (reverse_horizontal ? -950.0f : 950.0f),
            (direction_.y * std::cos(opening) - direction_.z * std::sin(opening)) * 950.0f};
}

MotionProcess::~MotionProcess() { stop(); }
bool MotionProcess::start(const std::string& executable, bool mock) {
    stop();
    int control[2]{-1, -1}, data[2]{-1, -1};
    if (!make_pipe(control) || !make_pipe(data)) {
        close_fd(control[0]); close_fd(control[1]); close_fd(data[0]); close_fd(data[1]);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, control[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, data[1], STDOUT_FILENO);
    for (int fd : {control[0], control[1], data[0], data[1]}) posix_spawn_file_actions_addclose(&actions, fd);
    char* arguments[]{const_cast<char*>(executable.c_str()),
                      const_cast<char*>(mock ? "--stream-mock" : "--stream"), nullptr};
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    const int error = posix_spawn(&pid_, executable.c_str(), &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close_fd(control[0]); close_fd(data[1]);
    if (error) {
        pid_ = -1; close_fd(control[1]); close_fd(data[0]);
        std::fprintf(stderr, "Start motion reader: %s\n", std::strerror(error));
        return false;
    }
    control_ = control[1]; data_ = data[0]; used_ = 0;
    fcntl(data_, F_SETFL, fcntl(data_, F_GETFL) | O_NONBLOCK);
    return true;
}
void MotionProcess::stop() {
    // EOF is the reader's lifetime signal, including when the app is force-quit.
    close_fd(control_); close_fd(data_); used_ = 0;
    if (pid_ <= 0) return;
    auto wait = [&] {
        for (int attempt = 0; attempt < 200; ++attempt) {
            const auto result = waitpid(pid_, nullptr, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) { pid_ = -1; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    if (wait()) return;
    kill(pid_, SIGTERM);
    if (wait()) return;
    kill(pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
}
bool MotionProcess::poll(MotionPacket& latest) {
    bool received = false;
    while (data_ >= 0) {
        const auto count = read(data_, bytes_.data() + used_, bytes_.size() - used_);
        if (count > 0) {
            used_ += static_cast<std::size_t>(count);
            if (used_ == bytes_.size()) {
                MotionPacket packet;
                std::memcpy(&packet, bytes_.data(), sizeof(packet));
                if (packet.magic == motion_magic) { latest = packet; received = true; }
                used_ = 0;
            }
        } else if (count == 0) { close_fd(data_); close_fd(control_); break; }
        else if (errno != EINTR) { if (errno != EAGAIN) close_fd(data_); break; }
    }
    if (pid_ > 0) {
        const auto result = waitpid(pid_, nullptr, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) {
            pid_ = -1; close_fd(control_); close_fd(data_);
        }
    }
    return received;
}

bool run_motion_checks(const std::string& helper) {
    TiltGravity tilt;
    MotionPacket packet;
    packet.flags = motion_accel | motion_gyro | motion_lid;
    packet.timestamp = motion_now();
    packet.acceleration = {0, 0, -1}; packet.lid_angle = 90;
    if (!tilt.update(packet)) return false;
    auto g = tilt.gravity(packet.timestamp);
    if (std::abs(g.x) > 0.01f || std::abs(g.y - 950) > 0.01f) return false;
    packet.lid_angle = 180; packet.timestamp += 10000000;
    tilt.update(packet); g = tilt.gravity(packet.timestamp);
    if (std::abs(g.y) > 0.01f) return false;
    tilt.reset(); packet.lid_angle = 90; packet.acceleration = {0.5f, 0, -std::sqrt(0.75f)};
    tilt.update(packet); g = tilt.gravity(packet.timestamp);
    if (std::abs(g.x - 475) > 0.01f || std::abs(tilt.gravity(packet.timestamp, true).x + 475) > 0.01f) return false;
    const auto stale = tilt.gravity(packet.timestamp + 600000000);
    if (stale.x != 0 || stale.y != 950) return false;
    packet.timestamp += 10000000; packet.acceleration.x = NAN;
    if (tilt.update(packet)) return false;
    tilt.reset(); packet.timestamp += 10000000;
    packet.acceleration = {0, 0, 1}; packet.rotation = {0, 0, 0};
    tilt.update(packet);
    if (std::abs(tilt.gravity(packet.timestamp).y + 950) > 0.01f) return false;
    tilt.reset(); packet.timestamp += 10000000; packet.acceleration = {0, 0, -1};
    tilt.update(packet); packet.timestamp += 10000000; packet.rotation = {0, -90, 0};
    tilt.update(packet);
    if (tilt.gravity(packet.timestamp).x >= 0) return false;
    std::puts("PASS: screen gravity projection, horizontal reversal, stale data fallback, and invalid samples.");

    MotionProcess process;
    if (!process.start(helper, true)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool received = false;
    while (std::chrono::steady_clock::now() < deadline && !received) {
        received = process.poll(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!received || !(packet.flags & motion_accel)) return false;
    process.stop();
    if (process.running()) return false;
    std::puts("PASS: sensor helper transport and shutdown without opening sensor hardware.");
    return true;
}
