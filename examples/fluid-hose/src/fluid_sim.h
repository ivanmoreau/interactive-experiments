#pragma once
#include "fluid_types.h"
#include <cstdint>
#include <memory>
#include <vector>

#ifdef __OBJC__
#import <Metal/Metal.h>
// The renderer and the solver share one device and queue, so simulation and
// drawing for a frame run in order inside the same command buffer.
id<MTLDevice> hose_metal_device();
id<MTLCommandQueue> hose_metal_queue();
// Loads a compiled .metallib, or compiles bundled .metal source at run time
// for builds made without Xcode's offline Metal compiler. Cached per path.
id<MTLLibrary> hose_shader_library(const char* path);
#endif

// GPU position-based fluid. Steps are recorded on the CPU and encoded into the
// next command buffer; particle state stays on the GPU.
class FluidSimulation {
public:
    static constexpr std::uint32_t capacity = 65536;
    static constexpr float spacing = 4.5f;  // rest distance between particle centres, in points
    static constexpr float radius = spacing * 0.5f;
    static constexpr float splat_radius = spacing * 2.0f;  // rendered footprint of one particle
    static constexpr float step_time = 1.0f / 120.0f;
    static constexpr std::size_t max_windows = 64;

    FluidSimulation();
    ~FluidSimulation();
    FluidSimulation(const FluidSimulation&) = delete;
    FluidSimulation& operator=(const FluidSimulation&) = delete;

    bool initialize(const char* shader_file, float width, float height);
    // Resizes the canvas and removes all water.
    bool reset(float width, float height);
    void clear();
    // Records fixed steps. Window motion since the previous call is spread
    // across all substeps, so moving windows push the water smoothly.
    void advance(unsigned steps, const std::vector<WindowObstacle>& windows, bool pouring, FluidPoint nozzle,
                 FluidPoint gravity = {0, 950});
    std::uint32_t size() const;
    float width() const;
    float height() const;
    // Headless use: encode pending steps, wait, and read the particles back.
    bool flush();
    double last_flush_gpu_seconds() const;
    bool read_particles(std::vector<FluidParticle>& output);

#ifdef __OBJC__
    bool encode(id<MTLCommandBuffer> commands);
    id<MTLBuffer> particle_buffer() const;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

FluidPoint place_hose(FluidPoint pointer, const std::vector<WindowObstacle>& windows, float width, float height);
bool run_physics_checks(const char* shader_file);
