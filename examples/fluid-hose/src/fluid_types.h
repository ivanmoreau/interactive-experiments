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
    fluid_uint body_count;
    float body_stir;           // per second: how quickly water inside a body takes on its motion
    float padding0, padding1;
};

// Something floating in or falling through the water, in screen space
// (points, y down): a rubber duck, a fish, a food flake, or a fish's water
// probe. angle is clockwise in radians and spin is the angular velocity in
// the same convention. facing is +scale for a duck facing right and -scale
// for one facing left; other kinds use +scale. look holds drawing details:
// a duck's colour variant in look[0]; for a fish, tail bend, eye vergence,
// health, and its belly side times how foreshortened it looks mid-turn;
// style holds a fish's inherited looks: stripe hue, stripe pattern, and its
// sex times its growth (negative for a female). tag identifies the body in
// its water sample.
struct PondBody {
    float x, y, angle, facing;
    float vx, vy, spin, kind;
    float look[4];
    fluid_uint tag;
    float style[3];
};

#define BODY_DUCK 0.0f
#define BODY_FISH 1.0f
#define BODY_FOOD 2.0f
#define BODY_PROBE 3.0f  // senses water for a fish and is never drawn
#define BODY_EGG 4.0f
// Only ducks and fish stir the water; food and probes just measure it.

// Water overlapping one body, measured on the GPU: particle count, the sums
// of their positions and velocities, and the body's tag.
struct BodySample {
    float count;
    float x, y;
    float vx, vy;
    fluid_uint tag;
    float padding0, padding1;
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

// A fish's body is an ellipse centred on its origin, head toward +x; the
// tail fin is drawn beyond it but does not displace water.
#define FISH_RX 22.0f
#define FISH_RY 7.5f
#define FISH_REACH 36.0f
#define FOOD_RADIUS 3.5f
#define EGG_RADIUS 3.4f
#define FOOD_WET_RADIUS 8.0f  // flakes are tiny, so they sense water over a wider circle
#define PROBE_RADIUS 9.0f

#ifdef __METAL_VERSION__
// Converts a screen point into a body's local frame.
inline float2 body_local(float2 p, PondBody body) {
    const float2 d = p - float2(body.x, body.y);
    const float c = metal::cos(body.angle), s = metal::sin(body.angle);
    return float2(c * d.x + s * d.y, -s * d.x + c * d.y) / body.facing * float2(1, metal::sign(body.facing));
}

inline float body_reach(PondBody body) {
    // Food and eggs sense water over the same small circle; probes over a wider one.
    const float reach = body.kind < 0.5f ? DUCK_REACH : body.kind < 1.5f ? FISH_RX :
                        (body.kind < 2.5f || body.kind > 3.5f) ? FOOD_WET_RADIUS : PROBE_RADIUS;
    return reach * metal::abs(body.facing);
}

inline bool body_contains(float2 local, float kind) {
    if (kind > 1.5f) {
        const float radius = (kind < 2.5f || kind > 3.5f) ? FOOD_WET_RADIUS : PROBE_RADIUS;
        return metal::dot(local, local) < radius * radius;
    }
    if (kind > 0.5f) {
        const float2 fish = local / float2(FISH_RX, FISH_RY);
        return metal::dot(fish, fish) < 1;
    }
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
static_assert(sizeof(PondBody) == 64, "Body layout must match Metal");
static_assert(sizeof(BodySample) == 32, "Body sample layout must match Metal");
#endif
