#include "../src/fluid_sim.h"
#include "../src/desktop.h"
#include <mach-o/dyld.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

// GPU solver benchmark: 1440 × 900 points, one 120 Hz step per command
// buffer, timed with Metal's GPU start and end timestamps.
namespace {
constexpr float width = 1440, height = 900;

WindowObstacle window(std::uint32_t id, float x, float y, float w, float h) {
    return {x, y, w, h, x, y, w, h, 10, id, 0, 0};
}

bool measure(FluidSimulation& fluid, const std::vector<WindowObstacle>& windows, int samples) {
    for (int i = 0; i < 60; ++i) fluid.advance(1, windows, false, {0, 0});
    if (!fluid.flush()) return false;
    std::vector<double> milliseconds;
    for (int i = 0; i < samples; ++i) {
        fluid.advance(1, windows, false, {0, 0});
        if (!fluid.flush()) return false;
        milliseconds.push_back(fluid.last_flush_gpu_seconds() * 1000);
    }
    double mean = 0;
    for (const double value : milliseconds) mean += value;
    mean /= milliseconds.size();
    std::sort(milliseconds.begin(), milliseconds.end());
    std::printf("%u,%zu,%zu,%.4f,%.4f,%.4f,%.4f,%.2f\n", fluid.size(), windows.size(), milliseconds.size(), mean,
                milliseconds[milliseconds.size() / 2], milliseconds[(milliseconds.size() - 1) * 95 / 100],
                milliseconds.back(), mean / (FluidSimulation::step_time * 1000) * 100);
    std::fflush(stdout);
    return true;
}
}

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) quick = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("Usage: benchmark-physics.sh [--quick]\n"
                      "GPU solver benchmark: 1440x900 points, one 120 Hz step per command buffer.\n"
                      "Reports GPU execution time per step; excludes CPU encoding, rendering, and window tracking.\n"
                      "--quick stops at 4,096 particles. Uses the default fine particles and water feel.");
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    _NSGetExecutablePath(path.data(), &size);
    const auto directory = std::filesystem::canonical(path.data()).parent_path();
    auto shaders = directory / "fluid.metallib";
    if (!std::filesystem::exists(shaders)) shaders = directory / "fluid.metal";

    FluidSimulation fluid;
    if (!fluid.initialize(shaders.c_str(), width, height)) {
        std::fprintf(stderr, "%s\n", desktop_error());
        return 1;
    }
    const std::vector<WindowObstacle> one{window(1, 500, 220, 440, 280)};
    std::vector<WindowObstacle> many;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            many.push_back(window(many.size() + 1, 100.0f + column * 310, 100.0f + row * 200, 210, 125));
    std::puts("particles,windows,samples,mean_ms,median_ms,p95_ms,max_ms,step_budget_percent_at_120hz");
    unsigned step = 0;
    for (const std::uint32_t count : {4096u, 16384u, FluidSimulation::capacity}) {
        if (quick && count > 4096) break;
        std::fprintf(stderr, "Seeding %u particles...\n", count);
        // Sweep the nozzle across the screen so the water settles as a broad pool.
        // Rows stop short of capacity when the stream width does not divide it.
        while (fluid.size() + 16 < count) {
            const float x = 60 + std::fmod(step * 2.7f, width - 120);
            fluid.advance(1, {}, true, {x, 60});
            if (++step % 240 == 0 && !fluid.flush()) return 1;
        }
        const int samples = quick ? 60 : 240;
        if (!measure(fluid, {}, samples) || !measure(fluid, one, samples) || !measure(fluid, many, samples)) {
            std::fprintf(stderr, "%s\n", desktop_error());
            return 1;
        }
    }
    return 0;
}
