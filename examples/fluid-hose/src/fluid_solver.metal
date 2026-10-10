// Position-based fluids (Macklin & Müller 2013) in two dimensions.
// Each substep predicts positions, sorts particles into a dense cell grid,
// runs Jacobi density-constraint iterations, then derives velocities and
// applies XSPH viscosity and vorticity confinement.
#include <metal_stdlib>
#include "fluid_types.h"
using namespace metal;

namespace {

float4 window_rect(WindowObstacle w, float t) {
    return mix(float4(w.from_x, w.from_y, w.from_width, w.from_height), float4(w.x, w.y, w.width, w.height), t);
}

// Signed distance to a rounded rectangle and the outward surface normal.
float rounded_rect_distance(float2 p, float4 rect, float radius, thread float2& normal) {
    const float2 half_size = rect.zw * 0.5;
    const float2 offset = p - (rect.xy + half_size);
    radius = min(radius, min(half_size.x, half_size.y));
    const float2 q = abs(offset) - (half_size - radius);
    const float2 side = select(float2(-1), float2(1), offset >= 0);
    if (q.x > 0 && q.y > 0) {
        const float length_q = length(q);
        normal = side * q / max(length_q, 1e-6);
        return length_q - radius;
    }
    if (q.x > q.y) { normal = float2(side.x, 0); return q.x - radius; }
    normal = float2(0, side.y);
    return q.y - radius;
}

// Where a point attached to the window surface moves between two rectangles,
// including movement of an edge while the window is resized.
float2 carried_point(float2 p, float4 before, float4 after) {
    const float2 local = clamp((p - before.xy) / max(before.zw, float2(1)), 0.0, 1.0);
    return after.xy + local * after.zw;
}

bool inside_any(float2 p, constant SolverUniforms& u, constant WindowObstacle* windows, float margin) {
    for (uint w = 0; w < u.window_count; ++w) {
        float2 normal;
        if (rounded_rect_distance(p, window_rect(windows[w], u.step_to), windows[w].radius, normal) < margin) return true;
    }
    return false;
}

// Walks a trapped point along one axis until it leaves every window it is in.
float2 escape(float2 p, float2 direction, constant SolverUniforms& u, constant WindowObstacle* windows) {
    const float clearance = u.particle_radius + 0.01;
    for (int pass = 0; pass < 8; ++pass) {
        bool inside = false;
        for (uint w = 0; w < u.window_count; ++w) {
            const float4 rect = window_rect(windows[w], u.step_to);
            float2 normal;
            if (rounded_rect_distance(p, rect, windows[w].radius, normal) >= u.particle_radius - 0.01) continue;
            inside = true;
            if (direction.x < 0) p.x = min(p.x, rect.x - clearance);
            else if (direction.x > 0) p.x = max(p.x, rect.x + rect.z + clearance);
            else if (direction.y < 0) p.y = min(p.y, rect.y - clearance);
            else p.y = max(p.y, rect.y + rect.w + clearance);
        }
        if (!inside) return p;
    }
    return float2(INFINITY);
}

float2 collide(float2 p, float2 previous, constant SolverUniforms& u, constant WindowObstacle* windows, bool friction) {
    const float r = u.particle_radius;
    const float2 low = float2(r), high = float2(u.width, u.height) - r;
    // Friction is a rate, so the grip does not depend on the substep length.
    const float grip = friction ? 1.0 - exp(-u.friction * u.dt) : 0.0;
    p = clamp(p, low, high);
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (uint w = 0; w < u.window_count; ++w) {
            const float4 rect = window_rect(windows[w], u.step_to);
            float2 normal;
            const float distance = rounded_rect_distance(p, rect, windows[w].radius, normal);
            if (distance >= r) continue;
            float2 pushed = p + normal * (r - distance);
            if (grip > 0) {
                // Damp sliding relative to the surface, so a dragged window
                // carries the water resting on it.
                const float2 surface = carried_point(previous, window_rect(windows[w], u.step_from), rect) - previous;
                const float2 relative = pushed - previous - surface;
                // Only faces that water rests on grip it; side faces let it slide.
                pushed -= (relative - dot(relative, normal) * normal) * grip * saturate(-normal.y);
            }
            // A face beyond the screen edge is not an exit.
            p = clamp(pushed, low, high);
            moved = true;
        }
        if (!moved) break;
    }
    // Overlapping windows, or a window reaching past the screen edge, can
    // trap a particle. Move it to the nearest on-screen exit along an axis.
    if (inside_any(p, u, windows, r - 0.01)) {
        const float2 directions[4] = {float2(0, -1), float2(-1, 0), float2(1, 0), float2(0, 1)};
        float2 best = p;
        float best_distance = INFINITY;
        for (int k = 0; k < 4; ++k) {
            const float2 exit = escape(p, directions[k], u, windows);
            if (any(exit < low) || any(exit > high)) continue;
            const float distance = length(exit - p);
            if (distance < best_distance) { best = exit; best_distance = distance; }
        }
        p = best;
    }
    // The desktop floor holds water more lightly than a window top.
    if (grip > 0 && p.y >= u.height - r - 0.01) p.x -= (p.x - previous.x) * grip * 0.3;
    return p;
}

uint2 cell_of_point(float2 p, constant SolverUniforms& u) {
    const int2 cell = int2(floor(p / u.cell_size));
    return uint2(clamp(cell, int2(0), int2(u.grid_width - 1, u.grid_height - 1)));
}

float poly6(float r2, float h2, float h) { const float x = h2 - r2; return 4.0 / (M_PI_F * pow(h, 8.0)) * x * x * x; }
float spiky_slope(float r, float h) { const float x = h - r; return -30.0 / (M_PI_F * pow(h, 5.0)) * x * x; }

float2 limit_length(float2 v, float maximum) {
    const float length_v = length(v);
    return length_v > maximum ? v * (maximum / length_v) : v;
}

// Visit every particle in the 3 × 3 cells around p, including the particle itself.
#define FOR_EACH_NEIGHBOR(p, u, start, j)                                                       \
    const uint2 home_##j = cell_of_point(p, u);                                                  \
    for (uint cy = max(home_##j.y, 1u) - 1; cy <= min(home_##j.y + 1, u.grid_height - 1); ++cy) \
    for (uint cx = max(home_##j.x, 1u) - 1; cx <= min(home_##j.x + 1, u.grid_width - 1); ++cx)  \
    for (uint j = start[cy * u.grid_width + cx], end_##j = start[cy * u.grid_width + cx + 1]; j < end_##j; ++j)

}

kernel void clear_cells(device uint* counts [[buffer(0)]], constant SolverUniforms& u [[buffer(1)]],
                        uint i [[thread_position_in_grid]]) {
    if (i <= u.cell_count) counts[i] = 0;
}

kernel void emit_particles(device FluidParticle* particles [[buffer(0)]], constant FluidParticle* spawn [[buffer(1)]],
                           constant SolverUniforms& u [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    if (i < u.spawn_count) particles[u.count + i] = spawn[i];
}

kernel void predict_positions(device const FluidParticle* particles [[buffer(0)]], device float2* predicted [[buffer(1)]],
                              device uint* cell_of [[buffer(2)]], device atomic_uint* counts [[buffer(3)]],
                              constant SolverUniforms& u [[buffer(4)]], constant WindowObstacle* windows [[buffer(5)]],
                              uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    const FluidParticle particle = particles[i];
    const float2 x = float2(particle.x, particle.y);
    const float2 v = limit_length(float2(particle.vx, particle.vy) + float2(u.gravity_x, u.gravity_y) * u.dt, u.max_speed);
    const float2 p = collide(x + v * u.dt, x, u, windows, true);
    predicted[i] = p;
    const uint2 cell = cell_of_point(p, u);
    const uint index = cell.y * u.grid_width + cell.x;
    cell_of[i] = index;
    atomic_fetch_add_explicit(&counts[index], 1, memory_order_relaxed);
}

// One threadgroup turns per-cell counts into cell start offsets.
kernel void scan_cells(device const uint* counts [[buffer(0)]], device uint* start [[buffer(1)]],
                       device uint* cursor [[buffer(2)]], constant SolverUniforms& u [[buffer(3)]],
                       uint tid [[thread_index_in_threadgroup]], uint threads [[threads_per_threadgroup]]) {
    threadgroup uint partial[1024];
    const uint n = u.cell_count;
    const uint chunk = (n + threads - 1) / threads;
    const uint begin = min(tid * chunk, n), end = min(begin + chunk, n);
    uint sum = 0;
    for (uint i = begin; i < end; ++i) sum += counts[i];
    partial[tid] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint offset = 1; offset < threads; offset <<= 1) {
        const uint add = tid >= offset ? partial[tid - offset] : 0;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        partial[tid] += add;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    uint running = partial[tid] - sum;
    for (uint i = begin; i < end; ++i) {
        start[i] = running;
        cursor[i] = running;
        running += counts[i];
    }
    if (tid == threads - 1) start[n] = partial[tid];
}

kernel void reorder_particles(device const FluidParticle* source [[buffer(0)]], device const float2* predicted_source [[buffer(1)]],
                              device const uint* cell_of [[buffer(2)]], device atomic_uint* cursor [[buffer(3)]],
                              device FluidParticle* target [[buffer(4)]], device float2* predicted_target [[buffer(5)]],
                              constant SolverUniforms& u [[buffer(6)]], uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    const uint slot = atomic_fetch_add_explicit(&cursor[cell_of[i]], 1, memory_order_relaxed);
    target[slot] = source[i];
    predicted_target[slot] = predicted_source[i];
}

kernel void compute_lambda(device const float2* predicted [[buffer(0)]], device const uint* start [[buffer(1)]],
                           device float* lambda [[buffer(2)]], constant SolverUniforms& u [[buffer(3)]],
                           uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    const float2 p = predicted[i];
    const float h = u.cell_size, h2 = h * h;
    float density = 0;
    float2 gradient_i = 0;
    float gradient_sum = 0;
    FOR_EACH_NEIGHBOR(p, u, start, j) {
        const float2 d = p - predicted[j];
        const float r2 = dot(d, d);
        if (r2 >= h2) continue;
        density += poly6(r2, h2, h);
        const float r = sqrt(r2);
        if (r < 1e-5) continue;
        const float2 gradient = spiky_slope(r, h) * d / (r * u.rest_density);
        gradient_i += gradient;
        gradient_sum += dot(gradient, gradient);
    }
    const float constraint = max(density / u.rest_density - 1.0, -u.tension_limit);
    lambda[i] = -constraint / (gradient_sum + dot(gradient_i, gradient_i) + u.relaxation);
}

kernel void apply_density(device const float2* predicted [[buffer(0)]], device const float* lambda [[buffer(1)]],
                          device const uint* start [[buffer(2)]], device float2* corrected [[buffer(3)]],
                          constant SolverUniforms& u [[buffer(4)]], constant WindowObstacle* windows [[buffer(5)]],
                          uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    const float2 p = predicted[i];
    const float h = u.cell_size, h2 = h * h;
    const float lambda_i = lambda[i];
    float2 delta = 0;
    FOR_EACH_NEIGHBOR(p, u, start, j) {
        if (j == i) continue;
        float2 d = p - predicted[j];
        float r2 = dot(d, d);
        if (r2 >= h2) continue;
        if (r2 < 1e-8) {
            // Coincident particles get a deterministic separation direction.
            d = float2(i < j ? -1e-3 : 1e-3, 0);
            r2 = 1e-6;
        }
        const float r = sqrt(r2);
        const float ratio = poly6(r2, h2, h) / u.correction_reference;
        const float artificial_pressure = -u.correction_scale * ratio * ratio * ratio * ratio;
        delta += (lambda_i + lambda[j] + artificial_pressure) * spiky_slope(r, h) * d / (r * u.rest_density);
    }
    corrected[i] = collide(p + limit_length(delta, u.particle_radius), p, u, windows, false);
}

kernel void derive_velocity(device const FluidParticle* particles [[buffer(0)]], device const float2* predicted [[buffer(1)]],
                            device float2* velocity [[buffer(2)]], constant SolverUniforms& u [[buffer(3)]],
                            uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    velocity[i] = limit_length((predicted[i] - float2(particles[i].x, particles[i].y)) / u.dt, u.max_speed);
}

kernel void apply_viscosity(device const float2* predicted [[buffer(0)]], device const float2* velocity [[buffer(1)]],
                            device const uint* start [[buffer(2)]], device float2* smoothed [[buffer(3)]],
                            device float2* vorticity_density [[buffer(4)]], constant SolverUniforms& u [[buffer(5)]],
                            uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    const float2 p = predicted[i], v = velocity[i];
    const float h = u.cell_size, h2 = h * h;
    float2 average = 0;
    float curl = 0;
    float density = 0;
    FOR_EACH_NEIGHBOR(p, u, start, j) {
        const float2 d = p - predicted[j];
        const float r2 = dot(d, d);
        if (r2 >= h2) continue;
        const float weight = poly6(r2, h2, h) / u.rest_density;
        density += weight;
        if (j == i) continue;
        const float2 relative = velocity[j] - v;
        average += relative * weight;
        const float r = sqrt(r2);
        if (r < 1e-5) continue;
        const float2 gradient = spiky_slope(r, h) * d / (r * u.rest_density);
        curl += relative.x * gradient.y - relative.y * gradient.x;
    }
    smoothed[i] = v + u.viscosity * average;
    vorticity_density[i] = float2(curl, density);
}

kernel void finish_step(device FluidParticle* particles [[buffer(0)]], device const float2* predicted [[buffer(1)]],
                        device const float2* velocity [[buffer(2)]], device const float2* vorticity_density [[buffer(3)]],
                        device const uint* start [[buffer(4)]], constant SolverUniforms& u [[buffer(5)]],
                        uint i [[thread_position_in_grid]]) {
    if (i >= u.count) return;
    const float2 p = predicted[i];
    const float h = u.cell_size, h2 = h * h;
    const float curl = vorticity_density[i].x;
    float2 toward_vortex = 0;
    FOR_EACH_NEIGHBOR(p, u, start, j) {
        if (j == i) continue;
        const float2 d = p - predicted[j];
        const float r2 = dot(d, d);
        if (r2 >= h2 || r2 < 1e-10) continue;
        const float r = sqrt(r2);
        toward_vortex += abs(vorticity_density[j].x) * spiky_slope(r, h) * d / r;
    }
    float2 v = velocity[i];
    const float strength = length(toward_vortex);
    if (strength > 1e-6) {
        const float2 n = toward_vortex / strength;
        v += u.vorticity * float2(n.y * curl, -n.x * curl) * u.dt;
    }
    v = limit_length(v, u.max_speed);

    FluidParticle particle = particles[i];
    // Fast water at a thin, broken surface turns white and fades as it settles.
    const float speed = length(v);
    const float density = vorticity_density[i].y;
    // A fast but intact jet stays clear; only broken, sparse water foams.
    const float splash = saturate((speed - 700.0) / 900.0) * saturate(1.1 - density);
    particle.foam = max(particle.foam * exp(-u.dt * 1.4), splash);
    particle.age += u.dt;
    particle.x = p.x;
    particle.y = p.y;
    particle.vx = v.x;
    particle.vy = v.y;
    particles[i] = particle;
}
