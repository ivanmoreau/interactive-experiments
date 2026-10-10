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

// Particle size trades detail for how much water the capacity holds.
enum class ParticleSize : int { fine, medium, coarse };
// How the liquid moves: runny water, a thicker liquid, or slow goo.
enum class LiquidFeel : int { water, thick, goo };
struct FluidSettings {
    ParticleSize size = ParticleSize::fine;
    LiquidFeel feel = LiquidFeel::water;
};

// GPU position-based fluid. Steps are recorded on the CPU and encoded into the
// next command buffer; particle state stays on the GPU.
class FluidSimulation {
public:
    static constexpr std::uint32_t capacity = 65536;
    static constexpr float step_time = 1.0f / 120.0f;
    static constexpr std::size_t max_windows = 64;

    FluidSimulation();
    ~FluidSimulation();
    FluidSimulation(const FluidSimulation&) = delete;
    FluidSimulation& operator=(const FluidSimulation&) = delete;

    bool initialize(const char* shader_file, float width, float height, FluidSettings settings = {});
    // Changing the particle size removes all water; the feel applies to the existing water.
    void configure(FluidSettings settings);
    FluidSettings settings() const;
    float spacing() const;       // rest distance between particle centres, in points
    float radius() const;        // collision radius
    float splat_radius() const;  // rendered footprint of one particle
    // Resizes the canvas and removes all water.
    bool reset(float width, float height);
    void clear();
    // Records fixed steps. Window motion since the previous call is spread
    // across all substeps, so moving windows push the water smoothly.
    void advance(unsigned steps, const std::vector<WindowObstacle>& windows, bool pouring, FluidPoint nozzle,
                 FluidPoint gravity = {0, 950});
    // Scales how fast water leaves the nozzle, and so how much is poured.
    static constexpr float min_flow = 0.25f, max_flow = 3.0f;
    void set_flow_rate(float flow);
    float flow_rate() const;
    std::uint32_t size() const;
    static constexpr std::size_t max_bodies = 128;
    // Ducks and fish stir the water inside their outlines. Each encoded frame
    // also measures the water overlapping each body; results arrive when that
    // frame's GPU work completes.
    void set_bodies(const std::vector<PondBody>& bodies);
    bool latest_body_samples(std::vector<BodySample>& output) const;
    float particle_area() const;  // water area one particle represents, in square points
    float width() const;
    float height() const;
    // Headless use: encode pending steps, wait, and read the particles back.
    bool flush();
    double last_flush_gpu_seconds() const;
    bool read_particles(std::vector<FluidParticle>& output);
    // Replaces the water with these particles (for restoring a saved pond).
    bool restore_particles(const std::vector<FluidParticle>& particles);

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
