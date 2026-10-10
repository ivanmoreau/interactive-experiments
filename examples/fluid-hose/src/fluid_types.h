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
    fluid_uint duck_count;
    float duck_stir;           // per second: how quickly water inside a duck takes on its motion
    float padding0, padding1;
};

// A rubber duck in screen space (points, y down). angle is clockwise in
// radians, facing is +scale when the duck faces right and -scale when left,
// and spin is the angular velocity in the same convention as angle.
struct DuckBody {
    float x, y, angle, facing;
    float vx, vy, spin, variant;
};

// Water overlapping one duck, measured on the GPU: particle count, and the
// sums of their positions and velocities.
struct DuckSample {
    float count;
    float x, y;
    float vx, vy;
    float padding0, padding1, padding2;
};

// Duck silhouette in local points, facing right with y down: an elliptical
// body, a round head, and an elliptical beak. Physics, water sampling, and
// drawing all use these shapes.
#define DUCK_BODY_X (-2.0f)
#define DUCK_BODY_Y 4.0f
#define DUCK_BODY_RX 21.0f
#define DUCK_BODY_RY 13.0f
#define DUCK_HEAD_X 11.0f
#define DUCK_HEAD_Y (-12.0f)
#define DUCK_HEAD_R 9.0f
#define DUCK_BEAK_X 21.0f
#define DUCK_BEAK_Y (-10.0f)
#define DUCK_BEAK_RX 6.0f
#define DUCK_BEAK_RY 3.0f
#define DUCK_REACH 30.0f  // radius around the origin that contains the duck at scale 1

#ifdef __METAL_VERSION__
// Converts a screen point into a duck's local frame.
inline float2 duck_local(float2 p, DuckBody duck) {
    const float2 d = p - float2(duck.x, duck.y);
    const float c = metal::cos(duck.angle), s = metal::sin(duck.angle);
    return float2(c * d.x + s * d.y, -s * d.x + c * d.y) / duck.facing * float2(1, metal::sign(duck.facing));
}

inline bool duck_contains(float2 local) {
    const float2 body = (local - float2(DUCK_BODY_X, DUCK_BODY_Y)) / float2(DUCK_BODY_RX, DUCK_BODY_RY);
    const float2 beak = (local - float2(DUCK_BEAK_X, DUCK_BEAK_Y)) / float2(DUCK_BEAK_RX, DUCK_BEAK_RY);
    return metal::dot(body, body) < 1 || metal::length(local - float2(DUCK_HEAD_X, DUCK_HEAD_Y)) < DUCK_HEAD_R ||
           metal::dot(beak, beak) < 1;
}
#endif

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
static_assert(sizeof(SolverUniforms) == 112,"Solver layout must match Metal");
static_assert(sizeof(SceneUniforms) == 64, "Scene layout must match Metal");
static_assert(sizeof(DuckBody) == 32, "Duck layout must match Metal");
static_assert(sizeof(DuckSample) == 32, "Duck sample layout must match Metal");
#endif
