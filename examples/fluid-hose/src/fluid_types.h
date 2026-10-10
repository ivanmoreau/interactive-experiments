#pragma once

#ifdef __METAL_VERSION__
typedef uint fluid_uint;
#else
#include <cstdint>
typedef std::uint32_t fluid_uint;
#endif

struct FluidPoint { float x; float y; };

// A window collider in canvas points. The tracker fills the current bounds,
// corner radius, and id; the solver fills the bounds at the start of the
// step, so each substep sees the window partway along its real motion.
struct WindowObstacle {
    float x, y, width, height;
    float from_x, from_y, from_width, from_height;
    float radius;
    fluid_uint id;
    float padding0, padding1;
};

// Persistent particle state. Predicted positions live in a separate buffer.
struct FluidParticle {
    float x, y;
    float vx, vy;
    float foam;
    float age;
    float padding0, padding1;
};

struct SolverUniforms {
    float width, height;
    float gravity_x, gravity_y;
    float dt;
    float step_from, step_to;  // fraction of the frame's window motion covered by this substep
    float cell_size;           // also the kernel support radius
    fluid_uint grid_width, grid_height, cell_count, count;
    fluid_uint window_count, spawn_count;
    float rest_density;        // kernel sum of a particle at rest spacing
    float relaxation;          // constraint-force mixing that keeps lambda bounded
    float correction_scale;    // artificial pressure that prevents clumping
    float correction_reference;
    float tension_limit;       // how far below rest density the fluid may pull together
    float viscosity;
    float vorticity;
    float friction;
    float max_speed;
    float particle_radius;
};

struct SceneUniforms {
    float width;
    float height;
    float nozzle_x;
    float nozzle_y;
    float show_nozzle;
    float time;
    float particle_radius;
    float scale;  // drawable pixels per point
    fluid_uint count;
    float has_background;  // 1 when a captured desktop frame is bound for refraction
    float flow;            // nozzle flow rate, 1 = normal
    float padding0;
    // The canvas origin inside the captured display, and the display size, in points.
    float background_x, background_y;
    float background_width, background_height;
};

#ifndef __METAL_VERSION__
static_assert(sizeof(WindowObstacle) == 48, "Window layout must match Metal");
static_assert(sizeof(FluidParticle) == 32, "Particle layout must match Metal");
static_assert(sizeof(SolverUniforms) == 96,"Solver layout must match Metal");
static_assert(sizeof(SceneUniforms) == 64, "Scene layout must match Metal");
#endif
