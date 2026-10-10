#import <Metal/Metal.h>
#include "fluid_sim.h"
#include "desktop.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace {
constexpr float stream_speed = 320.0f;
constexpr float stream_width_points = 16.0f;
constexpr float teleport_distance = 500.0f;

float spacing_for(ParticleSize size) {
    switch (size) {
        case ParticleSize::fine: return 3.0f;
        case ParticleSize::medium: return 3.75f;
        case ParticleSize::coarse: return 4.5f;
    }
    return 3.0f;
}

struct Feel {
    float viscosity;   // XSPH blend toward neighbour velocity per substep
    float tension;     // allowed density deficit, i.e. cohesion
    float correction;  // artificial pressure, relative to the rest gradient sum
    float vorticity;
    float friction;    // per second, on surfaces water rests on
    int substeps;
    int iterations;
};

Feel feel_for(LiquidFeel feel) {
    switch (feel) {
        case LiquidFeel::water: return {0.015f, 0.002f, 0.03f, 0.0f, 6.0f, 6, 2};
        case LiquidFeel::thick: return {0.09f, 0.006f, 0.04f, 0.0f, 12.0f, 4, 2};
        case LiquidFeel::goo: return {0.3f, 0.03f, 0.02f, 0.0f, 30.0f, 4, 2};
    }
    return feel_for(LiquidFeel::water);
}

// SPH kernels for support h, as used by the solver kernels.
float poly6(float r2, float h) {
    const float x = h * h - r2;
    return x > 0 ? 4.0f / (static_cast<float>(M_PI) * std::pow(h, 8.0f)) * x * x * x : 0.0f;
}
float spiky_slope(float r, float h) {
    const float x = h - r;
    return x > 0 ? -30.0f / (static_cast<float>(M_PI) * std::pow(h, 5.0f)) * x * x : 0.0f;
}

struct RestState { float density; float gradient_sum; };
// Kernel sums for a particle inside a hexagonally packed fluid at rest spacing.
RestState rest_state(float s, float h) {
    RestState rest{0, 0};
    for (int row = -6; row <= 6; ++row) {
        for (int column = -6; column <= 6; ++column) {
            const float x = (column + (row & 1) * 0.5f) * s;
            const float y = row * s * std::sqrt(3.0f) * 0.5f;
            const float r2 = x * x + y * y;
            rest.density += poly6(r2, h);
            const float r = std::sqrt(r2);
            if (r > 0 && r < h) rest.gradient_sum += spiky_slope(r, h) * spiky_slope(r, h);
        }
    }
    rest.gradient_sum /= rest.density * rest.density;
    return rest;
}

bool inside(FluidPoint point, const WindowObstacle& rect, float margin) {
    return point.x > rect.x - margin && point.x < rect.x + rect.width + margin &&
           point.y > rect.y - margin && point.y < rect.y + rect.height + margin;
}
}

id<MTLDevice> hose_metal_device() {
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    return device;
}

id<MTLCommandQueue> hose_metal_queue() {
    static id<MTLCommandQueue> queue = [hose_metal_device() newCommandQueue];
    return queue;
}

id<MTLLibrary> hose_shader_library(const char* path) {
    static std::unordered_map<std::string, id<MTLLibrary>> libraries;
    const auto cached = libraries.find(path);
    if (cached != libraries.end()) return cached->second;
    NSString* filename = [NSString stringWithUTF8String:path];
    if (!filename) { set_desktop_error("Invalid shader path encoding"); return nil; }
    NSError* error = nil;
    id<MTLLibrary> library = nil;
    if ([filename.pathExtension isEqualToString:@"metal"]) {
        NSString* source = [NSString stringWithContentsOfFile:filename encoding:NSUTF8StringEncoding error:&error];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        options.languageVersion = MTLLanguageVersion2_4;
        if (source) library = [hose_metal_device() newLibraryWithSource:source options:options error:&error];
    } else {
        library = [hose_metal_device() newLibraryWithURL:[NSURL fileURLWithPath:filename] error:&error];
    }
    if (!library) { set_desktop_error("Load fluid shaders: %s", error.localizedDescription.UTF8String); return nil; }
    libraries[path] = library;
    return library;
}

struct FluidSimulation::Impl {
    struct Step { std::vector<FluidParticle> spawn; };
    struct Batch {
        std::vector<WindowObstacle> windows;
        std::vector<Step> steps;
        FluidPoint gravity;
    };

    id<MTLDevice> device = nil;
    id<MTLComputePipelineState> clear_cells, emit, predict, scan, reorder, lambda, density, velocity, viscosity, finish;
    id<MTLBuffer> particles[2] = {nil, nil};
    id<MTLBuffer> predicted[2] = {nil, nil};
    id<MTLBuffer> cell_of, lambdas, velocities, smoothed, vorticity_density;
    id<MTLBuffer> counts, starts, cursors;
    unsigned current = 0;
    float width = 0, height = 0;
    std::uint32_t grid_width = 0, grid_height = 0;
    std::uint32_t count = 0;          // particles after all recorded steps
    std::uint32_t encoded_count = 0;  // particles after all encoded steps
    float emit_distance = 0;
    std::uint32_t random_state = 0x4127a35u;
    std::vector<Batch> pending;
    std::unordered_map<std::uint32_t, WindowObstacle> previous_windows;
    SolverUniforms base{};
    double flush_gpu_seconds = 0;
    FluidSettings settings;
    float spacing = 3.0f;
    float support = 6.6f;  // kernel support, also the grid cell size
    int substeps = 4;
    int iterations = 2;
    float flow = 1.0f;

    void apply(FluidSettings next) {
        settings = next;
        spacing = spacing_for(next.size);
        // The kernel support covers about 18 neighbours at rest.
        support = spacing * 2.2f;
        const Feel feel = feel_for(next.feel);
        substeps = feel.substeps;
        iterations = feel.iterations;
        const RestState rest = rest_state(spacing, support);
        base.cell_size = support;
        base.dt = step_time / substeps;
        base.rest_density = rest.density;
        base.relaxation = rest.gradient_sum * 0.1f;
        base.correction_scale = feel.correction / rest.gradient_sum;
        base.correction_reference = poly6(0.2f * support * 0.2f * support, support);
        base.tension_limit = feel.tension;
        base.viscosity = feel.viscosity;
        base.vorticity = feel.vorticity;
        base.friction = feel.friction;
        base.max_speed = 2400.0f;
        base.particle_radius = spacing * 0.5f;
    }

    float random() {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        return static_cast<float>(random_state & 0xffffffu) / 16777216.0f;
    }

    id<MTLComputePipelineState> pipeline(id<MTLLibrary> library, NSString* name) {
        id<MTLFunction> function = [library newFunctionWithName:name];
        if (!function) { set_desktop_error("Missing fluid kernel %s", name.UTF8String); return nil; }
        NSError* error = nil;
        id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:function error:&error];
        if (!state) set_desktop_error("Create fluid kernel %s: %s", name.UTF8String, error.localizedDescription.UTF8String);
        return state;
    }

    id<MTLBuffer> buffer(std::size_t bytes) {
        return [device newBufferWithLength:std::max<std::size_t>(bytes, 16) options:MTLResourceStorageModeShared];
    }

    void dispatch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> state, std::uint32_t threads) {
        if (!threads) return;
        [encoder setComputePipelineState:state];
        const NSUInteger group = std::min<NSUInteger>(state.maxTotalThreadsPerThreadgroup, 256);
        [encoder dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    }
};

FluidSimulation::FluidSimulation() : impl_(std::make_unique<Impl>()) {}
FluidSimulation::~FluidSimulation() = default;

bool FluidSimulation::initialize(const char* shader_file, float width, float height, FluidSettings settings) {
    @autoreleasepool {
        auto& s = *impl_;
        s.device = hose_metal_device();
        if (!s.device || !hose_metal_queue()) return set_desktop_error("No Metal device is accessible to this process");
        id<MTLLibrary> library = hose_shader_library(shader_file);
        if (!library) return false;
        if (!(s.clear_cells = s.pipeline(library, @"clear_cells")) || !(s.emit = s.pipeline(library, @"emit_particles")) ||
            !(s.predict = s.pipeline(library, @"predict_positions")) || !(s.scan = s.pipeline(library, @"scan_cells")) ||
            !(s.reorder = s.pipeline(library, @"reorder_particles")) || !(s.lambda = s.pipeline(library, @"compute_lambda")) ||
            !(s.density = s.pipeline(library, @"apply_density")) || !(s.velocity = s.pipeline(library, @"derive_velocity")) ||
            !(s.viscosity = s.pipeline(library, @"apply_viscosity")) || !(s.finish = s.pipeline(library, @"finish_step")))
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            s.particles[i] = s.buffer(capacity * sizeof(FluidParticle));
            s.predicted[i] = s.buffer(capacity * sizeof(float) * 2);
        }
        s.cell_of = s.buffer(capacity * sizeof(std::uint32_t));
        s.lambdas = s.buffer(capacity * sizeof(float));
        s.velocities = s.buffer(capacity * sizeof(float) * 2);
        s.smoothed = s.buffer(capacity * sizeof(float) * 2);
        s.vorticity_density = s.buffer(capacity * sizeof(float) * 2);
        if (!s.particles[0] || !s.particles[1] || !s.predicted[0] || !s.predicted[1] || !s.cell_of || !s.lambdas ||
            !s.velocities || !s.smoothed || !s.vorticity_density)
            return set_desktop_error("Cannot allocate fluid particle storage");

        s.apply(settings);
        return reset(width, height);
    }
}

void FluidSimulation::configure(FluidSettings settings) {
    auto& s = *impl_;
    if (settings.size == s.settings.size) {
        // Recorded steps are encoded with the new feel; the water is kept.
        s.apply(settings);
        return;
    }
    s.apply(settings);
    reset(s.width, s.height);
}

FluidSettings FluidSimulation::settings() const { return impl_->settings; }
float FluidSimulation::spacing() const { return impl_->spacing; }
float FluidSimulation::radius() const { return impl_->spacing * 0.5f; }
float FluidSimulation::splat_radius() const { return impl_->spacing * 2.0f; }

bool FluidSimulation::reset(float width, float height) {
    auto& s = *impl_;
    const auto grid_width = static_cast<std::uint32_t>(std::ceil(width / s.support));
    const auto grid_height = static_cast<std::uint32_t>(std::ceil(height / s.support));
    const std::uint32_t cells = grid_width * grid_height;
    if (!s.counts || s.counts.length < (cells + 1) * sizeof(std::uint32_t)) {
        s.counts = s.buffer((cells + 1) * sizeof(std::uint32_t));
        s.starts = s.buffer((cells + 1) * sizeof(std::uint32_t));
        s.cursors = s.buffer((cells + 1) * sizeof(std::uint32_t));
        if (!s.counts || !s.starts || !s.cursors) return set_desktop_error("Cannot allocate the fluid neighbour grid");
    }
    s.width = width;
    s.height = height;
    s.grid_width = grid_width;
    s.grid_height = grid_height;
    s.base.width = width;
    s.base.height = height;
    s.base.grid_width = grid_width;
    s.base.grid_height = grid_height;
    s.base.cell_count = cells;
    clear();
    s.previous_windows.clear();
    return true;
}

void FluidSimulation::clear() {
    auto& s = *impl_;
    s.pending.clear();
    s.count = 0;
    s.encoded_count = 0;
    s.emit_distance = 0;
}

void FluidSimulation::set_flow_rate(float flow) { impl_->flow = std::clamp(flow, min_flow, max_flow); }
float FluidSimulation::flow_rate() const { return impl_->flow; }
std::uint32_t FluidSimulation::size() const { return impl_->count; }
float FluidSimulation::width() const { return impl_->width; }
float FluidSimulation::height() const { return impl_->height; }

void FluidSimulation::advance(unsigned steps, const std::vector<WindowObstacle>& windows, bool pouring, FluidPoint nozzle,
                              FluidPoint gravity) {
    auto& s = *impl_;
    if (!steps) return;
    Impl::Batch batch;
    batch.gravity = gravity;
    batch.windows.assign(windows.begin(), windows.begin() + std::min(windows.size(), max_windows));
    for (auto& window : batch.windows) {
        window.from_x = window.x;
        window.from_y = window.y;
        window.from_width = window.width;
        window.from_height = window.height;
        const auto previous = s.previous_windows.find(window.id);
        if (previous == s.previous_windows.end()) continue;
        const auto& p = previous->second;
        // Minimizing, restoring, or switching spaces moves a window without
        // sweeping through the space between; do not fling water across it.
        if (std::abs(p.x - window.x) > teleport_distance || std::abs(p.y - window.y) > teleport_distance ||
            std::abs(p.width - window.width) > teleport_distance || std::abs(p.height - window.height) > teleport_distance)
            continue;
        window.from_x = p.x;
        window.from_y = p.y;
        window.from_width = p.width;
        window.from_height = p.height;
    }
    s.previous_windows.clear();
    for (const auto& window : batch.windows) s.previous_windows[window.id] = window;

    const float spacing = s.spacing;
    const float radius = spacing * 0.5f;
    const int stream_width = std::max(2, static_cast<int>(std::lround(stream_width_points / spacing)));
    const bool can_pour = pouring && nozzle.x > radius && nozzle.x < s.width - radius &&
                          nozzle.y > radius && nozzle.y < s.height - radius;
    for (unsigned step = 0; step < steps; ++step) {
        Impl::Step record;
        if (can_pour) {
            // Rows leave the nozzle at rest spacing; rows emitted earlier in
            // this step have already travelled part of the way down.
            const float speed = stream_speed * s.flow;
            s.emit_distance += speed * step_time;
            while (s.emit_distance >= spacing && s.count + record.spawn.size() + stream_width <= capacity) {
                s.emit_distance -= spacing;
                for (int k = 0; k < stream_width; ++k) {
                    const float x = nozzle.x + (k - (stream_width - 1) * 0.5f) * spacing + (s.random() - 0.5f) * 0.6f;
                    const float y = nozzle.y + 4.0f + s.emit_distance;
                    record.spawn.push_back({x, y, (s.random() - 0.5f) * 12.0f, speed, 0, 0, 0, 0});
                }
            }
            s.emit_distance = std::min(s.emit_distance, spacing);
        } else s.emit_distance = 0;
        s.count += static_cast<std::uint32_t>(record.spawn.size());
        batch.steps.push_back(std::move(record));
    }
    s.pending.push_back(std::move(batch));
}

bool FluidSimulation::encode(id<MTLCommandBuffer> commands) {
    auto& s = *impl_;
    if (s.pending.empty()) return true;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (!encoder) return set_desktop_error("Cannot begin fluid simulation commands");
    encoder.label = @"Fluid solver";
    const WindowObstacle placeholder{};
    for (const auto& batch : s.pending) {
        SolverUniforms u = s.base;
        u.gravity_x = batch.gravity.x;
        u.gravity_y = batch.gravity.y;
        u.window_count = static_cast<std::uint32_t>(batch.windows.size());
        const void* windows = batch.windows.empty() ? static_cast<const void*>(&placeholder) : batch.windows.data();
        const NSUInteger window_bytes = std::max<std::size_t>(batch.windows.size(), 1) * sizeof(WindowObstacle);
        const int substeps = s.substeps, iterations = s.iterations;
        const float total = static_cast<float>(batch.steps.size() * substeps);
        int substep_index = 0;
        for (const auto& step : batch.steps) {
            if (!step.spawn.empty()) {
                u.count = s.encoded_count;
                u.spawn_count = static_cast<std::uint32_t>(step.spawn.size());
                [encoder setBuffer:s.particles[s.current] offset:0 atIndex:0];
                [encoder setBytes:step.spawn.data() length:step.spawn.size() * sizeof(FluidParticle) atIndex:1];
                [encoder setBytes:&u length:sizeof(u) atIndex:2];
                s.dispatch(encoder, s.emit, u.spawn_count);
                s.encoded_count += u.spawn_count;
            }
            u.count = s.encoded_count;
            for (int sub = 0; sub < substeps; ++sub, ++substep_index) {
                if (!u.count) continue;
                u.step_from = substep_index / total;
                u.step_to = (substep_index + 1) / total;
                [encoder setBuffer:s.counts offset:0 atIndex:0];
                [encoder setBytes:&u length:sizeof(u) atIndex:1];
                s.dispatch(encoder, s.clear_cells, u.cell_count + 1);

                [encoder setBuffer:s.particles[s.current] offset:0 atIndex:0];
                [encoder setBuffer:s.predicted[0] offset:0 atIndex:1];
                [encoder setBuffer:s.cell_of offset:0 atIndex:2];
                [encoder setBuffer:s.counts offset:0 atIndex:3];
                [encoder setBytes:&u length:sizeof(u) atIndex:4];
                [encoder setBytes:windows length:window_bytes atIndex:5];
                s.dispatch(encoder, s.predict, u.count);

                [encoder setComputePipelineState:s.scan];
                [encoder setBuffer:s.counts offset:0 atIndex:0];
                [encoder setBuffer:s.starts offset:0 atIndex:1];
                [encoder setBuffer:s.cursors offset:0 atIndex:2];
                [encoder setBytes:&u length:sizeof(u) atIndex:3];
                const NSUInteger scan_threads = std::min<NSUInteger>(s.scan.maxTotalThreadsPerThreadgroup, 1024);
                [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(scan_threads, 1, 1)];

                const unsigned sorted = 1 - s.current;
                [encoder setBuffer:s.particles[s.current] offset:0 atIndex:0];
                [encoder setBuffer:s.predicted[0] offset:0 atIndex:1];
                [encoder setBuffer:s.cell_of offset:0 atIndex:2];
                [encoder setBuffer:s.cursors offset:0 atIndex:3];
                [encoder setBuffer:s.particles[sorted] offset:0 atIndex:4];
                [encoder setBuffer:s.predicted[1] offset:0 atIndex:5];
                [encoder setBytes:&u length:sizeof(u) atIndex:6];
                s.dispatch(encoder, s.reorder, u.count);
                s.current = sorted;

                unsigned position = 1;
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    [encoder setBuffer:s.predicted[position] offset:0 atIndex:0];
                    [encoder setBuffer:s.starts offset:0 atIndex:1];
                    [encoder setBuffer:s.lambdas offset:0 atIndex:2];
                    [encoder setBytes:&u length:sizeof(u) atIndex:3];
                    s.dispatch(encoder, s.lambda, u.count);

                    [encoder setBuffer:s.predicted[position] offset:0 atIndex:0];
                    [encoder setBuffer:s.lambdas offset:0 atIndex:1];
                    [encoder setBuffer:s.starts offset:0 atIndex:2];
                    [encoder setBuffer:s.predicted[1 - position] offset:0 atIndex:3];
                    [encoder setBytes:&u length:sizeof(u) atIndex:4];
                    [encoder setBytes:windows length:window_bytes atIndex:5];
                    s.dispatch(encoder, s.density, u.count);
                    position = 1 - position;
                }

                [encoder setBuffer:s.particles[s.current] offset:0 atIndex:0];
                [encoder setBuffer:s.predicted[position] offset:0 atIndex:1];
                [encoder setBuffer:s.velocities offset:0 atIndex:2];
                [encoder setBytes:&u length:sizeof(u) atIndex:3];
                s.dispatch(encoder, s.velocity, u.count);

                [encoder setBuffer:s.predicted[position] offset:0 atIndex:0];
                [encoder setBuffer:s.velocities offset:0 atIndex:1];
                [encoder setBuffer:s.starts offset:0 atIndex:2];
                [encoder setBuffer:s.smoothed offset:0 atIndex:3];
                [encoder setBuffer:s.vorticity_density offset:0 atIndex:4];
                [encoder setBytes:&u length:sizeof(u) atIndex:5];
                s.dispatch(encoder, s.viscosity, u.count);

                [encoder setBuffer:s.particles[s.current] offset:0 atIndex:0];
                [encoder setBuffer:s.predicted[position] offset:0 atIndex:1];
                [encoder setBuffer:s.smoothed offset:0 atIndex:2];
                [encoder setBuffer:s.vorticity_density offset:0 atIndex:3];
                [encoder setBuffer:s.starts offset:0 atIndex:4];
                [encoder setBytes:&u length:sizeof(u) atIndex:5];
                s.dispatch(encoder, s.finish, u.count);
            }
        }
    }
    [encoder endEncoding];
    s.pending.clear();
    return true;
}

id<MTLBuffer> FluidSimulation::particle_buffer() const { return impl_->particles[impl_->current]; }

bool FluidSimulation::flush() {
    @autoreleasepool {
        id<MTLCommandBuffer> commands = [hose_metal_queue() commandBuffer];
        if (!commands) return set_desktop_error("Cannot allocate fluid commands");
        if (!encode(commands)) return false;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError)
            return set_desktop_error("Fluid simulation failed: %s", commands.error.localizedDescription.UTF8String);
        impl_->flush_gpu_seconds = commands.GPUEndTime - commands.GPUStartTime;
        return true;
    }
}

double FluidSimulation::last_flush_gpu_seconds() const { return impl_->flush_gpu_seconds; }

bool FluidSimulation::read_particles(std::vector<FluidParticle>& output) {
    if (!flush()) return false;
    const auto* particles = static_cast<const FluidParticle*>(particle_buffer().contents);
    output.assign(particles, particles + impl_->encoded_count);
    return true;
}

FluidPoint place_hose(FluidPoint pointer, const std::vector<WindowObstacle>& windows, float width, float height) {
    auto free = [&](FluidPoint point) {
        if (point.x < 8 || point.y < 8 || point.x > width - 8 || point.y > height - 8) return false;
        return std::none_of(windows.begin(), windows.end(), [&](const auto& rect) {
            return inside(point, rect, 3.0f);
        });
    };
    if (free(pointer)) return pointer;
    if (pointer.x < 0 || pointer.y < 0 || pointer.x > width || pointer.y > height) return {-100, -100};
    FluidPoint best{-100, -100};
    float best_distance = 1e20f;
    for (const auto& rect : windows) {
        const FluidPoint candidates[8] = {
            {pointer.x, rect.y - 12}, {pointer.x, rect.y + rect.height + 12},
            {rect.x - 12, pointer.y}, {rect.x + rect.width + 12, pointer.y},
            {rect.x - 12, rect.y - 12}, {rect.x + rect.width + 12, rect.y - 12},
            {rect.x - 12, rect.y + rect.height + 12}, {rect.x + rect.width + 12, rect.y + rect.height + 12}
        };
        for (const auto candidate : candidates) {
            const float dx = candidate.x - pointer.x;
            const float dy = candidate.y - pointer.y;
            const float distance = dx * dx + dy * dy;
            if (distance < best_distance && free(candidate)) { best = candidate; best_distance = distance; }
        }
    }
    return best;
}

namespace {
WindowObstacle obstacle(std::uint32_t id, float x, float y, float width, float height, float radius = 10) {
    return {x, y, width, height, x, y, width, height, radius, id, 0, 0};
}

bool finite(const std::vector<FluidParticle>& particles) {
    return std::all_of(particles.begin(), particles.end(), [](const auto& p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.vx) && std::isfinite(p.vy);
    });
}

// Deepest penetration into any window; the solver keeps it below a small tolerance.
float worst_penetration(const std::vector<FluidParticle>& particles, const std::vector<WindowObstacle>& windows) {
    float worst = 0;
    for (const auto& p : particles) {
        for (const auto& w : windows) {
            if (!inside({p.x, p.y}, w, 0)) continue;
            const float depth = std::min({p.x - w.x, w.x + w.width - p.x, p.y - w.y, w.y + w.height - p.y});
            // Corners are rounded, so the rectangle overstates depth there; use the SDF.
            const float half_w = w.width * 0.5f, half_h = w.height * 0.5f;
            const float r = std::min({w.radius, half_w, half_h});
            const float qx = std::abs(p.x - w.x - half_w) - (half_w - r);
            const float qy = std::abs(p.y - w.y - half_h) - (half_h - r);
            const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
            const float distance = outside + std::min(std::max(qx, qy), 0.0f) - r;
            if (distance < 0) worst = std::max(worst, std::min(depth, -distance));
        }
    }
    return worst;
}

bool check(bool condition, const char* message) {
    if (condition) std::printf("PASS: %s\n", message);
    else std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}
}

namespace {
bool check_liquid(const char* shader_file, FluidSettings settings) {
    constexpr FluidPoint down{0, 950};
    FluidSimulation fluid;
    if (!fluid.initialize(shader_file, 640, 480, settings)) return false;
    std::vector<FluidParticle> particles;

    // Runny water keeps spreading for a few seconds before it settles.
    for (int i = 0; i < 1200; ++i) fluid.advance(1, {}, i < 60, {320, 40}, down);
    if (!fluid.read_particles(particles) || particles.empty() || !finite(particles)) return false;
    float lowest = 480;
    FluidPoint drift{0, 0};
    for (const auto& p : particles) {
        lowest = std::min(lowest, p.y);
        drift.x += p.vx / particles.size();
        drift.y += p.vy / particles.size();
    }
    // Jitter is motion within the pool; the puddle as a whole may still glide slowly.
    float internal = 0;
    for (const auto& p : particles) internal += std::hypot(p.vx - drift.x, p.vy - drift.y) / particles.size();
    if (!check(lowest > 440, "gravity and pooling at the desktop floor")) return false;
    if (!check(internal < 10 && std::hypot(drift.x, drift.y) < 40, "a settled pool comes to rest without jitter")) {
        std::fprintf(stderr, "  internal motion %.1f, drift %.1f points per second\n", internal, std::hypot(drift.x, drift.y));
        return false;
    }
    std::size_t crowded = 0;
    for (std::size_t i = 0; i < particles.size(); ++i) {
        for (std::size_t j = i + 1; j < particles.size(); ++j) {
            if (std::hypot(particles[i].x - particles[j].x, particles[i].y - particles[j].y) < fluid.spacing() * 0.45f)
                ++crowded;
        }
    }
    // Thin films allow a few close pairs; clumping would pull many together.
    if (!check(crowded * 12 < particles.size(), "particles keep their spacing instead of clumping")) {
        std::fprintf(stderr, "  %zu close pairs among %zu particles\n", crowded, particles.size());
        return false;
    }

    std::vector<WindowObstacle> windows{obstacle(1, 200, 180, 240, 160)};
    if (!fluid.reset(640, 480)) return false;
    for (int i = 0; i < 480; ++i) {
        fluid.advance(1, windows, i < 240, {320, 50}, down);
        if (i % 40 == 39) {
            if (!fluid.read_particles(particles) || !finite(particles)) return false;
            if (worst_penetration(particles, windows) > 0.75f) return check(false, "water stays outside the window");
        }
    }
    if (!check(true, "water stays outside the window")) return false;
    const auto nozzle = place_hose({320, 200}, windows, 640, 480);
    if (!check(!inside(nozzle, windows[0], 3.0f) && nozzle.y < windows[0].y,
               "the hose snaps outside solid window bodies")) return false;
    windows[0] = obstacle(1, 250, 250, 200, 170);
    fluid.advance(1, windows, false, {0, 0}, down);
    if (!fluid.read_particles(particles) || !finite(particles)) return false;
    if (!check(worst_penetration(particles, windows) < 0.75f, "moving a window pushes overlapping water out")) return false;

    if (!fluid.reset(800, 600)) return false;
    for (int i = 0; i < 360; ++i) fluid.advance(1, {}, i < 90, {400, 50}, down);
    const std::vector<WindowObstacle> overlapping{obstacle(1, 100, 100, 400, 600), obstacle(2, 350, 100, 300, 600)};
    fluid.advance(1, overlapping, false, {0, 0}, down);
    if (!fluid.read_particles(particles) || !finite(particles)) return false;
    if (!check(worst_penetration(particles, overlapping) < 0.75f, "water is displaced outside overlapping windows")) return false;

    // Sweep a window sideways through a settled pool; the water must be carried along.
    if (!fluid.reset(800, 600)) return false;
    for (int i = 0; i < 480; ++i) fluid.advance(1, {}, i < 120, {200, 300}, down);
    std::vector<WindowObstacle> paddle{obstacle(7, 80, 480, 40, 200)};
    for (int i = 0; i < 30; ++i) {
        paddle[0].x = 80 + i * 8.0f;
        fluid.advance(1, paddle, false, {0, 0}, down);
    }
    if (!fluid.read_particles(particles) || !finite(particles)) return false;
    float momentum = 0;
    for (const auto& p : particles) momentum += p.vx;
    if (!check(momentum / particles.size() > 20 && worst_penetration(particles, paddle) < 0.75f,
               "a dragged window pushes water in its direction of motion")) return false;

    // Tilt gravity: the water must collect against the side wall and the ceiling.
    if (!fluid.reset(640, 480)) return false;
    for (int i = 0; i < 240; ++i) fluid.advance(1, {}, i < 40, {320, 100}, down);
    for (int i = 0; i < 480; ++i) fluid.advance(1, {}, false, {0, 0}, {-950, 0});
    if (!fluid.read_particles(particles) || particles.empty() || !finite(particles)) return false;
    float average_x = 0;
    for (const auto& p : particles) average_x += p.x;
    for (int i = 0; i < 480; ++i) fluid.advance(1, {}, false, {0, 0}, {0, -950});
    if (!fluid.read_particles(particles) || !finite(particles)) return false;
    float average_y = 0;
    for (const auto& p : particles) average_y += p.y;
    if (!check(average_x / particles.size() < 80 && average_y / particles.size() < 80,
               "fluid accumulates toward the left and ceiling under rotated gravity")) return false;

    fluid.clear();
    if (!fluid.read_particles(particles)) return false;
    return check(fluid.size() == 0 && particles.empty(), "clearing the water resets the simulation");
}
}

bool run_physics_checks(const char* shader_file) {
    const char* sizes[] = {"fine", "medium", "coarse"};
    const char* feels[] = {"water", "thick", "goo"};
    for (int size = 0; size < 3; ++size) {
        for (int feel = 0; feel < 3; ++feel) {
            std::printf("%s particles, %s:\n", sizes[size], feels[feel]);
            if (!check_liquid(shader_file, {static_cast<ParticleSize>(size), static_cast<LiquidFeel>(feel)})) return false;
        }
    }
    return true;
}
