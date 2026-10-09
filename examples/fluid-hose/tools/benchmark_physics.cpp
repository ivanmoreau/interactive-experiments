#include "../src/fluid_sim.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr float step = 1.0f / 120.0f;

bool valid(const FluidSimulation& fluid) {
    for (const auto& point : fluid.vertices()) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            point.x < FluidSimulation::radius || point.x > 1440 - FluidSimulation::radius ||
            point.y < FluidSimulation::radius || point.y > 900 - FluidSimulation::radius) return false;
    }
    return true;
}

bool measure(const FluidSimulation& snapshot, const std::vector<WindowRect>& windows, int samples) {
    FluidSimulation settled = snapshot;
    for (int i = 0; i < 120; ++i) settled.step(step, windows, false, {720, 60});
    std::vector<double> milliseconds;
    milliseconds.reserve(samples * 3);
    for (int repeat = 0; repeat < 3; ++repeat) {
        FluidSimulation fluid = settled;
        for (int i = 0; i < samples; ++i) {
            const auto start = Clock::now();
            fluid.step(step, windows, false, {720, 60});
            milliseconds.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        if (fluid.size() != snapshot.size() || !valid(fluid)) return false;
    }
    double mean = 0;
    for (double value : milliseconds) mean += value;
    mean /= milliseconds.size();
    std::sort(milliseconds.begin(), milliseconds.end());
    const double median = milliseconds[milliseconds.size() / 2];
    const double p95 = milliseconds[(milliseconds.size() - 1) * 95 / 100];
    std::printf("%zu,%zu,%zu,%.4f,%.4f,%.4f,%.4f,%.2f\n", snapshot.size(), windows.size(),
                milliseconds.size(), mean, median, p95, milliseconds.back(), mean * 120 / 10);
    std::fflush(stdout);
    return true;
}
}

int main(int argc, char** argv) {
    bool quick = false;
    bool profile = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else if (std::strcmp(argv[i], "--profile") == 0) profile = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("Usage: benchmark-physics.sh [--quick] [--profile]\n"
                      "CPU-only benchmark: 1440x900 points, 120 Hz, three repeats per case.\n"
                      "--quick uses 2,000 particles and fewer samples.\n"
                      "--profile adds a 15-second steady-state workload for macOS sample/Instruments.\n"
                      "Reports simulation time only; excludes setup, copying, rendering, and window queries.");
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    std::fprintf(stderr, "Physics benchmark PID: %d; compiler optimization: O2\n", getpid());
    std::puts("particles,windows,samples,mean_ms,median_ms,p95_ms,max_ms,step_budget_percent_at_120hz");
    const std::vector<WindowRect> one_window{{500, 220, 440, 280}};
    std::vector<WindowRect> many_windows;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column)
            many_windows.push_back({100.0f + column * 310, 100.0f + row * 200, 210, 125});
    }
    FluidSimulation seeded(1440, 900);
    for (std::size_t count : {2000u, 4000u, 8000u}) {
        if (quick && count > 2000) break;
        std::fprintf(stderr, "Seeding %zu particles...\n", count);
        while (seeded.size() < count) seeded.step(step, one_window, true, {720, 60});
        const int samples = quick ? 60 : 180;
        if (!measure(seeded, {}, samples) || !measure(seeded, one_window, samples) ||
            !measure(seeded, many_windows, samples)) {
            std::fputs("FAIL: benchmark state contains invalid particles\n", stderr);
            return 1;
        }
    }
    if (profile) {
        std::fprintf(stderr, "Profiling %zu particles / 12 windows for 15 seconds (PID %d)...\n", seeded.size(), getpid());
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (Clock::now() < deadline) seeded.step(step, many_windows, false, {720, 60});
        if (!valid(seeded)) return 1;
    }
    return 0;
}
