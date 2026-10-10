#include "pond.h"
#include "fish_brain.h"
#include "fish_genome.h"
#include "fluid_sim.h"
#include "pond_store.h"
#include <filesystem>
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {
using namespace JPH;

// Jolt works in metres with y up; the screen uses points with y down.
// 100 points per metre puts 950 point/s² gravity close to Earth's.
constexpr float metres_per_point = 0.01f;
constexpr float pi = 3.14159265f;
constexpr float water_density = 1000.0f;  // kg/m³
constexpr float duck_density = 280.0f;    // floats with about a quarter of its outline submerged
constexpr float duck_depth = 30.0f;       // thickness across the screen plane, in points at scale 1
constexpr float drag_rate = 5.0f;         // per second, relative to the displaced water's mass
constexpr float spin_damping = 3.0f;      // per second when fully submerged
constexpr float teleport_distance = 500.0f;
constexpr float wall_thickness = 100.0f;

// Fish hover thanks to their swim bladder, glide easily along their body
// and resist sideways motion, and float up when they die.
constexpr float fish_depth = 14.0f;
constexpr float fish_glide_drag = 2.0f;  // per second
constexpr float fish_side_drag = 9.0f;
constexpr float fish_turn_damping = 8.0f;
constexpr float max_pitch = 0.5f;      // radians the righting reflex allows
constexpr float righting_rate = 6.0f;  // per second
constexpr float level_rate = 1.5f;     // per second, below the limit
constexpr float dead_fish_lift = 1.5f;
constexpr float fish_start_energy = 0.65f;
constexpr float full_energy_seconds = 40 * 60;  // a full fish starves after about 40 minutes
constexpr float breath_seconds = 50;            // and suffocates after 50 seconds in air
constexpr float digestion_seconds = 150;
constexpr float meal_energy = 0.05f;   // per flake
constexpr float meal_fullness = 0.12f;
constexpr float sight_distance = 420;
constexpr float flip_turn = 1.75f;
constexpr float growth_seconds = 20 * 60;  // well fed, a hatchling is full grown in about 20 minutes
constexpr float meal_growth = 0.003f;
constexpr float hatchling_scale = 0.4f;    // a hatchling's length relative to its adult length
constexpr float juvenile_growth = 0.35f;
constexpr float adult_growth = 0.8f;
float fish_scale(float growth, float adult_size) { return adult_size * (hatchling_scale + (1 - hatchling_scale) * growth); }
FishStage fish_stage(float growth) {
    return growth < juvenile_growth ? FishStage::larva : growth < adult_growth ? FishStage::juvenile : FishStage::adult;
}
constexpr float typical_beat = 0.15f;  // a bout's motor activity added up over time, typically
constexpr float swim_window = 0.35f;   // seconds of tail beats each bout drives     // turns beyond this (about 100°) flip the fish around

// Flakes float until they soak up water, then sink, then dissolve.
constexpr float food_soak_seconds = 25;
constexpr float food_lifetime = 150;
constexpr float food_water_drag = 6.0f;

RVec3 to_world(FluidPoint p) { return RVec3(p.x * metres_per_point, -p.y * metres_per_point, 0); }
Vec3 to_world_vector(FluidPoint v) { return Vec3(v.x * metres_per_point, -v.y * metres_per_point, 0); }
FluidPoint operator+(FluidPoint a, FluidPoint b) { return {a.x + b.x, a.y + b.y}; }
FluidPoint operator-(FluidPoint a, FluidPoint b) { return {a.x - b.x, a.y - b.y}; }
FluidPoint operator*(FluidPoint a, float k) { return {a.x * k, a.y * k}; }
float dot(FluidPoint a, FluidPoint b) { return a.x * b.x + a.y * b.y; }
float length(FluidPoint a) { return std::hypot(a.x, a.y); }

namespace Layers {
constexpr ObjectLayer still = 0;     // screen walls
constexpr ObjectLayer window = 1;    // kinematic windows
constexpr ObjectLayer creature = 2;  // ducks and fish
constexpr ObjectLayer food = 3;      // flakes rest on walls and windows and pass through creatures
constexpr uint count = 4;
}

bool layers_collide(ObjectLayer a, ObjectLayer b) {
    if (a > b) std::swap(a, b);
    if (b <= Layers::window) return false;  // neither moves on its own
    if (b == Layers::creature) return true;
    return a <= Layers::window;
}

class BroadPhaseLayers final : public BroadPhaseLayerInterface {
public:
    uint GetNumBroadPhaseLayers() const override { return Layers::count; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override { return BroadPhaseLayer(static_cast<uint8>(layer)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer layer) const override {
        static const char* names[] = {"still", "window", "creature", "food"};
        return names[layer.GetValue()];
    }
#endif
};

class ObjectBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer object, BroadPhaseLayer broad_phase) const override {
        return layers_collide(object, broad_phase.GetValue());
    }
};

class ObjectPairFilter final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override { return layers_collide(a, b); }
};

void initialize_jolt() {
    static const bool ready = [] {
        RegisterDefaultAllocator();
        Factory::sInstance = new Factory();
        RegisterTypes();
        return true;
    }();
    (void)ready;
}

// An extruded convex outline of an ellipse in local points.
Ref<ShapeSettings> ellipse_hull(float cx, float cy, float rx, float ry, float facing, float scale, float depth, float density) {
    Array<Vec3> points;
    const float half_depth = depth * scale * 0.5f * metres_per_point;
    for (int i = 0; i < 16; ++i) {
        const float angle = i * 2.0f * pi / 16;
        const float x = (cx + rx * std::cos(angle)) * facing * scale * metres_per_point;
        const float y = -(cy + ry * std::sin(angle)) * scale * metres_per_point;
        points.push_back(Vec3(x, y, half_depth));
        points.push_back(Vec3(x, y, -half_depth));
    }
    auto* hull = new ConvexHullShapeSettings(points, 0.01f);
    hull->mDensity = density;
    return hull;
}

// Body, head, and beak.
RefConst<Shape> duck_shape(float facing, float scale) {
    // Shape settings are reference counted, so they live on the heap.
    Ref<StaticCompoundShapeSettings> compound = new StaticCompoundShapeSettings;
    compound->AddShape(Vec3::sZero(), Quat::sIdentity(),
                       ellipse_hull(DUCK_BODY_X, DUCK_BODY_Y, DUCK_BODY_RX, DUCK_BODY_RY, facing, scale, duck_depth, duck_density));
    compound->AddShape(Vec3::sZero(), Quat::sIdentity(),
                       ellipse_hull(DUCK_HEAD_X, DUCK_HEAD_Y, DUCK_HEAD_R, DUCK_HEAD_R, facing, scale, duck_depth, duck_density));
    compound->AddShape(Vec3::sZero(), Quat::sIdentity(),
                       ellipse_hull(DUCK_BEAK_X, DUCK_BEAK_Y, DUCK_BEAK_RX, DUCK_BEAK_RY, facing, scale, duck_depth, duck_density));
    // Like a weighted toy, the centre of mass sits under the middle of the
    // body rather than toward the head, so ducks float level and self-right.
    const RefConst<Shape> outline = compound->Create().Get();
    const Vec3 balance(DUCK_BODY_X * facing * scale * metres_per_point, -(DUCK_BODY_Y + 6.0f) * scale * metres_per_point, 0);
    Ref<OffsetCenterOfMassShapeSettings> weighted =
        new OffsetCenterOfMassShapeSettings(balance - outline->GetCenterOfMass(), outline.GetPtr());
    return weighted->Create().Get();
}

// Outline area at scale 1, used to judge how much of a duck is under water.
const float duck_area = pi * (DUCK_BODY_RX * DUCK_BODY_RY + DUCK_HEAD_R * DUCK_HEAD_R + DUCK_BEAK_RX * DUCK_BEAK_RY);
const float fish_area = pi * FISH_RX * FISH_RY;
const float food_wet_area = pi * FOOD_WET_RADIUS * FOOD_WET_RADIUS;
const float probe_area = pi * PROBE_RADIUS * PROBE_RADIUS;

RefConst<Shape> box_shape(float width, float height, float radius) {
    const Vec3 half(width * 0.5f * metres_per_point, height * 0.5f * metres_per_point, 1.0f);
    const float convex = std::min({radius * metres_per_point, half.GetX() * 0.9f, half.GetY() * 0.9f, 0.05f});
    Ref<BoxShapeSettings> box = new BoxShapeSettings(half, std::max(convex, 0.0f));
    return box->Create().Get();
}

// Whether the segment from a to b crosses the rectangle.
bool segment_hits_rect(FluidPoint a, FluidPoint b, float x, float y, float w, float h) {
    float low = 0, high = 1;
    const float start[2] = {a.x, a.y}, delta[2] = {b.x - a.x, b.y - a.y};
    const float minimum[2] = {x, y}, maximum[2] = {x + w, y + h};
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(delta[axis]) < 1e-6f) {
            if (start[axis] < minimum[axis] || start[axis] > maximum[axis]) return false;
            continue;
        }
        float t0 = (minimum[axis] - start[axis]) / delta[axis], t1 = (maximum[axis] - start[axis]) / delta[axis];
        if (t0 > t1) std::swap(t0, t1);
        low = std::max(low, t0);
        high = std::min(high, t1);
        if (low > high) return false;
    }
    return true;
}
}

struct Pond::Impl {
    struct Duck { BodyID id; float facing; float variant; std::uint32_t tag; };
    struct Window {
        BodyID id;
        float width, height;
        FluidPoint from, to, now, velocity;
    };
    struct Fish {
        BodyID id;
        float scale;  // length relative to the standard adult; grows with the fish
        float mass;
        FishBrain brain;
        std::uint32_t tag, probe_tags[3];
        std::uint32_t number = 0;
        FishGenome genome{};
        bool female = false;
        float growth = 1;       // 0 hatchling to 1 full grown
        unsigned generation = 1;
        std::uint32_t mother = 0, father = 0;
        std::uint32_t brain_seed = 0;
        float shape_scale = 1;  // the scale of its current collision shape
        FluidPoint probes[3] = {};
        float energy = fish_start_energy, fullness = 0, oxygen = 1;
        bool alive = true, in_water = false;
        unsigned meals = 0, escapes = 0;
        float submerged = 0;
        float upright = 1;   // +1 while the back points up with the head right; -1 head left
        float foreshorten = 1;
        BoutKind bout_kind = BoutKind::swim;
        float bout_left = 0, bout_length = 0, turn_rate = 0, thrust = 0;
        float swim_left = 0;   // how long the bout's tail beats still push the fish
        float push_left = 0;   // how much more beat the bout's push may use
        float since_strike = 10;
        float tail_phase = 0, tail_amplitude = 0, bend = 0;
    };
    struct Food { BodyID id; float mass; float age; float hue; std::uint32_t tag; };

    BroadPhaseLayers broad_phase_layers;
    ObjectBroadPhaseFilter object_broad_phase_filter;
    ObjectPairFilter object_pair_filter;
    std::unique_ptr<TempAllocatorImpl> temp;
    std::unique_ptr<JobSystemSingleThreaded> jobs;
    std::unique_ptr<PhysicsSystem> system;
    std::vector<BodyID> walls;
    std::vector<Duck> ducks;
    std::vector<Fish> fish;
    std::vector<Food> food;
    std::unordered_map<std::uint32_t, Window> windows;
    std::vector<PondBody> bodies;
    std::unordered_map<std::uint32_t, BodySample> samples;
    float width = 0, height = 0;
    float metabolism = 1, breathing = 1, growing = 1;
    std::shared_ptr<const HindbrainConnectome> hindbrain;
    std::uint32_t random_state = 0x9e3779b9u;
    std::uint32_t next_tag = 1;
    std::uint32_t next_fish_number = 1;
    FishGenome::Random genes{0x51ed270bu};
    FluidPoint pointer{-1000, -1000}, pointer_seen{-1000, -1000}, pointer_velocity{0, 0};
    float pointer_elapsed = 0;

    float random() {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        return static_cast<float>(random_state & 0xffffffu) / 16777216.0f;
    }

    BodyInterface& bodies_interface() { return system->GetBodyInterface(); }

    void remove(BodyID id) {
        bodies_interface().RemoveBody(id);
        bodies_interface().DestroyBody(id);
    }

    FluidPoint position(BodyID id) {
        const RVec3 p = bodies_interface().GetCenterOfMassPosition(id);
        return {static_cast<float>(p.GetX()) / metres_per_point, -static_cast<float>(p.GetY()) / metres_per_point};
    }
    FluidPoint velocity(BodyID id) {
        const Vec3 v = bodies_interface().GetLinearVelocity(id);
        return {v.GetX() / metres_per_point, -v.GetY() / metres_per_point};
    }
    // Clockwise screen angle.
    float heading(BodyID id) { return -bodies_interface().GetRotation(id).GetRotationAngle(Vec3::sAxisZ()); }

    const BodySample* sample(std::uint32_t tag) const {
        const auto found = samples.find(tag);
        return found == samples.end() || found->second.count < 1 ? nullptr : &found->second;
    }

    bool solid(FluidPoint p) const {
        if (p.x < 4 || p.y < 4 || p.x > width - 4 || p.y > height - 4) return true;
        for (const auto& entry : windows) {
            const auto& w = entry.second;
            if (std::abs(p.x - w.now.x) < w.width * 0.5f + 3 && std::abs(p.y - w.now.y) < w.height * 0.5f + 3) return true;
        }
        return false;
    }

    // The fish's frame: heading, and the direction its back faces.
    struct Frame { FluidPoint ahead, back; };
    static Frame frame(float angle, float upright) {
        const FluidPoint ahead{std::cos(angle), std::sin(angle)};
        return {ahead, FluidPoint{ahead.y, -ahead.x} * upright};
    }
    static float bearing(const Frame& f, FluidPoint r) { return std::atan2(dot(r, f.back), dot(r, f.ahead)); }

    void sense(Fish& f, FluidPoint p, FluidPoint v, const Frame& frame, FishSenses& senses) {
        const float s = f.scale;
        // Food: what each direction of the visual field sees, and whether a
        // flake is right at the mouth.
        const FluidPoint eye = p + frame.ahead * (14 * s), mouth = p + frame.ahead * (FISH_RX * s);
        const float bin_width = 2 * FishSenses::prey_field / FishSenses::prey_bins;
        for (const auto& flake : food) {
            const FluidPoint q = position(flake.id);
            const FluidPoint r = q - eye;
            const float distance = length(r);
            if (distance > sight_distance) continue;
            bool hidden = false;
            for (const auto& entry : windows) {
                const auto& w = entry.second;
                if (segment_hits_rect(eye, q, w.now.x - w.width * 0.5f, w.now.y - w.height * 0.5f, w.width, w.height)) {
                    hidden = true;
                    break;
                }
            }
            if (hidden) continue;
            const float angle = bearing(frame, r);
            if (std::abs(angle) > FishSenses::prey_field + bin_width) continue;
            const float salience = std::min(1.0f, 100.0f / (distance + 60.0f));
            for (int b = 0; b < FishSenses::prey_bins; ++b) {
                const float offset = (angle - FishSenses::prey_angle(b)) / bin_width;
                senses.prey[b] = std::max(senses.prey[b], salience * std::exp(-offset * offset / 0.72f));
            }
            if (std::abs(angle) < 0.45f)
                senses.strike_zone = std::max(senses.strike_zone, std::clamp(1.3f - length(q - mouth) / std::max(18 * s, 12.0f), 0.0f, 1.0f));
        }

        // Looming: the pointer, windows, and ducks rushing toward the fish.
        auto loom = [&](FluidPoint source, FluidPoint source_velocity, float radius) {
            const FluidPoint r = source - p;
            const float distance = length(r);
            if (distance < 1e-3f) return;
            const float gap = std::max(distance - radius, 15.0f);
            if (gap > 280) return;
            const float approach = -dot(source_velocity - v, r * (1 / distance));
            const float strength = std::clamp((approach / gap - 1.0f) / 3.0f, 0.0f, 1.0f);
            if (strength <= 0) return;
            const float angle = bearing(frame, r);
            const int bin = std::clamp(static_cast<int>((angle + pi) / (2 * pi) * FishSenses::loom_bins), 0, FishSenses::loom_bins - 1);
            senses.looming[bin] = std::max(senses.looming[bin], strength);
            for (int side : {-1, 1}) {
                const int next = (bin + side + FishSenses::loom_bins) % FishSenses::loom_bins;
                senses.looming[next] = std::max(senses.looming[next], strength * 0.5f);
            }
        };
        if (pointer.x > 0 && pointer.y > 0 && pointer.x < width && pointer.y < height) loom(pointer, pointer_velocity, 0);
        for (const auto& entry : windows) {
            const auto& w = entry.second;
            const FluidPoint nearest{std::clamp(p.x, w.now.x - w.width * 0.5f, w.now.x + w.width * 0.5f),
                                     std::clamp(p.y, w.now.y - w.height * 0.5f, w.now.y + w.height * 0.5f)};
            loom(nearest, w.velocity, 0);
        }
        for (const auto& duck : ducks) loom(position(duck.id), velocity(duck.id), 20 * std::abs(duck.facing));

        // Water, walls, and windows just ahead.
        auto blocked = [&](int probe) {
            const float hard = solid(f.probes[probe]) ? 1.0f : 0.0f;
            if (!f.in_water) return hard;
            const BodySample* water = sample(f.probe_tags[probe]);
            const float wet = water ? std::min(1.0f, water->count * particle_area / (0.7f * probe_area)) : 0.0f;
            return std::max(hard, 1 - wet);
        };
        senses.blocked_ahead = blocked(0);
        senses.blocked_dorsal = blocked(1);
        senses.blocked_ventral = blocked(2);
        if (f.in_water) {
            const BodySample* water = sample(f.tag);
            const FluidPoint flow = water ? FluidPoint{water->vx / water->count, water->vy / water->count} : v;
            senses.lateral_line = std::clamp((length(flow - v) - 80.0f) / 400.0f, 0.0f, 1.0f);
        }
        senses.pitch = std::asin(std::clamp(-frame.ahead.y, -1.0f, 1.0f));
        senses.hunger = std::clamp(1 - f.energy, 0.0f, 1.0f);
        senses.fullness = f.fullness;
        senses.hypoxia = f.in_water ? 1 - f.oxygen : std::max(0.7f, 1 - f.oxygen);
    }

    void start_bout(Fish& f, const FishBout& bout, float angle, FluidPoint v) {
        auto& interface = bodies_interface();
        const float cost = bout.kind == BoutKind::escape ? 0.0004f : 0.0001f;
        f.energy -= cost * metabolism;
        if (!f.in_water) {
            // Stranded: a flop, if it is lying on something.
            if (length(v) > 120) return;
            interface.SetLinearVelocity(f.id, to_world_vector({(random() - 0.5f) * 160, -(140 + random() * 120)}));
            interface.SetAngularVelocity(f.id, Vec3(0, 0, (random() - 0.5f) * 20));
            f.tail_amplitude = 1.4f;
            return;
        }
        float turn = bout.turn;
        if (std::abs(turn) > flip_turn) {
            // Turning around in a side view: the fish swings its head through
            // the screen, so it ends up mirrored, back still up.
            const float mirrored = pi - angle;
            interface.SetRotation(f.id, Quat::sRotation(Vec3::sAxisZ(), -mirrored), EActivation::Activate);
            f.upright = -f.upright;
            f.foreshorten = 0.15f;
            turn = std::copysign(pi, turn) - turn;
        }
        const float length = bout.kind == BoutKind::strike ? 0.09f : bout.kind == BoutKind::escape ? 0.12f :
                             bout.kind == BoutKind::reverse ? 0.2f : bout.kind == BoutKind::hunt ? 0.14f : 0.16f;
        const float strength = std::clamp(0.4f + f.energy * 3, 0.4f, 1.0f);  // starving fish are weak
        f.bout_kind = bout.kind;
        f.bout_length = f.bout_left = length;
        f.turn_rate = turn / (0.6f * length);
        // The tail beats come from the spinal neurons; each push is scaled so
        // a typical bout's beats add up to the bout's speed.
        f.thrust = bout.speed * strength * f.scale / typical_beat;  // bigger fish swim faster
        f.swim_left = swim_window;
        f.push_left = typical_beat;
        f.tail_amplitude = std::clamp(bout.speed / 220.0f, 0.35f, 1.3f);
        if (bout.kind == BoutKind::strike) f.since_strike = 0;
        if (bout.kind == BoutKind::escape) ++f.escapes;
    }

    void step_fish(Fish& f, float dt, Vec3 gravity) {
        auto& interface = bodies_interface();
        const FluidPoint p = position(f.id), v = velocity(f.id);
        float angle = heading(f.id);
        const BodySample* water = sample(f.tag);
        f.submerged = water ? std::min(1.0f, water->count * particle_area / (0.85f * fish_area * f.scale * f.scale)) : 0.0f;
        f.in_water = f.submerged > 0.5f;
        // Which way is up for the fish: keep its back up as it pitches past
        // vertical, unless it is dead and floating belly up.
        if (std::cos(angle) * f.upright < -0.17f) {
            f.upright = -f.upright;
            if (f.alive) f.foreshorten = 0.15f;
        }
        f.foreshorten = std::min(1.0f, f.foreshorten + dt / 0.22f);

        if (f.alive) {
            // Bigger fish burn more; some fish burn faster than others.
            f.energy -= metabolism * f.genome.metabolism * (0.5f + 0.5f * f.scale) * dt / full_energy_seconds;
            // Well-fed fish grow.
            if (f.growth < 1)
                f.growth = std::min(1.0f, f.growth + growing * dt / growth_seconds * std::clamp((f.energy - 0.35f) / 0.4f, 0.0f, 1.0f));
            f.fullness = std::max(0.0f, f.fullness - dt / digestion_seconds);
            f.oxygen = f.in_water ? std::min(1.0f, f.oxygen + 0.3f * dt) : f.oxygen - breathing * dt / breath_seconds;
            if (f.energy <= 0 || f.oxygen <= 0) {
                f.alive = false;
                f.energy = std::max(f.energy, 0.0f);
                f.oxygen = std::max(f.oxygen, 0.0f);
                f.bout_left = 0;
                f.foreshorten = 0.15f;  // rolls over
            }
        }
        // Growing: the body follows the fish's growth, with a new collision
        // shape whenever it has grown noticeably.
        f.scale = fish_scale(f.growth, f.genome.size);
        if (std::abs(f.scale - f.shape_scale) > 0.02f) {
            interface.SetShape(f.id, ellipse_hull(0, 0, FISH_RX, FISH_RY, 1, f.scale, fish_depth, water_density)->Create().Get(), true,
                               EActivation::Activate);
            f.shape_scale = f.scale;
            f.mass = interface.GetShape(f.id)->GetMassProperties().mMass;
        }
        if (f.alive) {
            FishSenses senses;
            sense(f, p, v, frame(angle, f.upright), senses);
            FishBout bout;
            if (f.brain.advance(senses, dt, bout)) {
                start_bout(f, bout, angle, v);
                angle = heading(f.id);
            }
        }
        f.since_strike += dt;

        // Water: lift, drag that is low along the body, and a damped spin.
        const Frame now = frame(angle, f.upright);
        if (water) {
            const FluidPoint flow{water->vx / water->count, water->vy / water->count};
            const FluidPoint relative = flow - v;
            const FluidPoint side{-now.ahead.y, now.ahead.x};
            const FluidPoint drag = now.ahead * (fish_glide_drag * dot(relative, now.ahead)) + side * (fish_side_drag * dot(relative, side));
            const float lift = f.alive ? 1.0f : dead_fish_lift;
            interface.AddForce(f.id, -f.mass * lift * f.submerged * gravity + to_world_vector(drag * (f.mass * f.submerged)));
            interface.SetAngularVelocity(f.id, interface.GetAngularVelocity(f.id) * std::exp(-fish_turn_damping * f.submerged * dt));
        }
        const bool turning = f.bout_left > 0.4f * f.bout_length;
        if (f.bout_left > 0 && f.in_water && turning) {
            // A turn toward the back is anticlockwise on screen when upright.
            interface.SetAngularVelocity(f.id, Vec3(0, 0, f.upright * f.turn_rate));
        }
        if (f.swim_left > 0 && f.in_water && f.alive) {
            // Each beat pushes; a bout pushes no more than a typical bout's
            // beats would, so strong beats push sooner rather than further.
            const float beat = std::min(f.brain.beat() * dt, f.push_left);
            f.push_left -= beat;
            interface.AddForce(f.id, to_world_vector(now.ahead * (f.mass * f.thrust * beat / dt)));
        }
        f.swim_left = std::max(0.0f, f.swim_left - dt);
        if (f.alive && f.in_water && !turning) {
            // Righting reflex: fins ease the fish back toward level, and pull
            // hard once it is steeply pitched. Hunting fish tip toward prey.
            const float hunting = f.brain.hunting();
            const float pitch = std::asin(std::clamp(-now.ahead.y, -1.0f, 1.0f));
            const float allowed = max_pitch + 0.7f * hunting;
            const float excess = pitch - std::clamp(pitch, -allowed, allowed);
            const float levelling = level_rate * (1 - hunting) * pitch + righting_rate * excess;
            const float spin = interface.GetAngularVelocity(f.id).GetZ();
            interface.SetAngularVelocity(f.id, Vec3(0, 0, spin + (-f.upright * levelling - spin) * std::min(1.0f, 10 * dt)));
        }
        f.bout_left = std::max(0.0f, f.bout_left - dt);

        // Tail: bent by the spinal motor neurons, curled into the turn at the
        // start of a bout, with a lazy wave while gliding and thrashing when
        // stranded.
        const bool flopping = !f.in_water && f.tail_amplitude > 0.3f;
        f.tail_phase += dt * 2 * pi * (flopping ? 14.0f : 2.5f);
        const float idle = f.alive ? (f.in_water ? 0.12f : 0.05f) : 0.0f;
        f.tail_amplitude = idle + (f.tail_amplitude - idle) * std::exp(-4.0f * dt);
        const float curl = turning ? -std::clamp(f.turn_rate * 0.25f, -4.0f, 4.0f) : 0.0f;
        const float neural = f.alive ? 6.0f * f.brain.tail() : 0.0f;
        f.bend = f.bend + (f.tail_amplitude * 4.5f * std::sin(f.tail_phase) + neural + curl - f.bend) * std::min(1.0f, dt * 40);
    }

    void eat(Fish& f) {
        if (!f.alive || f.since_strike > 0.2f) return;
        const FluidPoint mouth = position(f.id) + frame(heading(f.id), f.upright).ahead * (FISH_RX * f.scale);
        for (std::size_t i = 0; i < food.size();) {
            if (length(position(food[i].id) - mouth) < std::max(11 * f.scale, 7.0f)) {
                remove(food[i].id);
                food.erase(food.begin() + i);
                f.energy = std::min(1.0f, f.energy + meal_energy);
                f.growth = std::min(1.0f, f.growth + meal_growth);
                f.fullness = std::min(1.0f, f.fullness + meal_fullness);
                ++f.meals;
            } else ++i;
        }
    }

    void step_food(Food& flake, float dt, Vec3 gravity) {
        auto& interface = bodies_interface();
        flake.age += dt;
        const FluidPoint v = velocity(flake.id);
        Vec3 force = to_world_vector(v * (-0.8f * flake.mass));  // air
        if (const BodySample* water = sample(flake.tag)) {
            const float wet = std::min(1.0f, water->count * particle_area / food_wet_area);
            const float density = 0.6f + 0.55f * std::min(1.0f, flake.age / food_soak_seconds);
            const FluidPoint flow{water->vx / water->count, water->vy / water->count};
            force += -flake.mass * wet / density * gravity + to_world_vector((flow - v) * (food_water_drag * wet * flake.mass));
        }
        interface.AddForce(flake.id, force);
    }

    float particle_area = 0;

    bool create_duck(FluidPoint position, float signed_scale, float variant);

    void refresh_bodies() {
        bodies.clear();
        auto& interface = bodies_interface();
        for (const auto& duck : ducks) {
            const RVec3 position = interface.GetPosition(duck.id);
            const Vec3 velocity = interface.GetPointVelocity(duck.id, position);
            const float angle = interface.GetRotation(duck.id).GetRotationAngle(Vec3::sAxisZ());
            // Screen y points down, so angles and spins change sign.
            bodies.push_back({static_cast<float>(position.GetX()) / metres_per_point,
                              -static_cast<float>(position.GetY()) / metres_per_point, -angle, duck.facing,
                              velocity.GetX() / metres_per_point, -velocity.GetY() / metres_per_point,
                              -interface.GetAngularVelocity(duck.id).GetZ(), BODY_DUCK, {duck.variant, 0, 0, 0}, duck.tag, {0, 0, 0}});
        }
        for (auto& f : fish) {
            const FluidPoint p = position(f.id), v = velocity(f.id);
            const float angle = heading(f.id);
            const float health = f.alive ? std::clamp(0.3f + f.energy * 3, 0.3f, 1.0f) : 0.0f;
            const float belly = f.alive ? f.upright : -f.upright;
            bodies.push_back({p.x, p.y, angle, f.scale, v.x, v.y, -interface.GetAngularVelocity(f.id).GetZ(), BODY_FISH,
                              {f.bend, f.alive ? std::clamp(f.brain.vergence() + 0.6f * f.brain.eye_position(), -1.0f, 1.0f) : 0.0f,
                               health, belly * f.foreshorten}, f.tag,
                              {f.genome.hue, f.genome.stripes, (f.female ? -1.0f : 1.0f) * (0.01f + f.growth)}});
            const Frame now = frame(angle, f.upright);
            f.probes[0] = p + now.ahead * (40 * f.scale);
            f.probes[1] = p + now.ahead * (30 * f.scale) + now.back * (22 * f.scale);
            f.probes[2] = p + now.ahead * (30 * f.scale) - now.back * (22 * f.scale);
        }
        for (const auto& flake : food) {
            const FluidPoint p = position(flake.id), v = velocity(flake.id);
            const float fade = std::clamp((food_lifetime - flake.age) / 5.0f, 0.0f, 1.0f);
            bodies.push_back({p.x, p.y, heading(flake.id), 1, v.x, v.y, -interface.GetAngularVelocity(flake.id).GetZ(), BODY_FOOD,
                              {flake.hue, std::min(1.0f, flake.age / food_soak_seconds), fade, 0}, flake.tag, {0, 0, 0}});
        }
        for (const auto& f : fish)
            for (int i = 0; i < 3; ++i)
                bodies.push_back({f.probes[i].x, f.probes[i].y, 0, 1, 0, 0, 0, BODY_PROBE, {0, 0, 0, 0}, f.probe_tags[i], {0, 0, 0}});
    }
};

Pond::Pond() : impl_(std::make_unique<Impl>()) {
    initialize_jolt();
    auto& s = *impl_;
    s.temp = std::make_unique<TempAllocatorImpl>(8 * 1024 * 1024);
    s.jobs = std::make_unique<JobSystemSingleThreaded>(cMaxPhysicsJobs);
    s.system = std::make_unique<PhysicsSystem>();
    s.system->Init(512, 0, 2048, 2048, s.broad_phase_layers, s.object_broad_phase_filter, s.object_pair_filter);
}

Pond::~Pond() {
    auto& s = *impl_;
    clear_ducks();
    clear_fish();
    for (const auto& flake : s.food) s.remove(flake.id);
    for (const auto& window : s.windows) s.remove(window.second.id);
    for (const auto& wall : s.walls) s.remove(wall);
}

void Pond::reset(float width, float height) {
    auto& s = *impl_;
    clear_ducks();
    clear_fish();
    clear_food();
    for (const auto& window : s.windows) s.remove(window.second.id);
    s.windows.clear();
    for (const auto& wall : s.walls) s.remove(wall);
    s.walls.clear();
    s.width = width;
    s.height = height;
    const float t = wall_thickness;
    const struct { float x, y, w, h; } walls[4] = {
        {-t, height, width + 2 * t, t}, {-t, -t, width + 2 * t, t}, {-t, -t, t, height + 2 * t}, {width, -t, t, height + 2 * t}};
    for (const auto& wall : walls) {
        BodyCreationSettings settings(box_shape(wall.w, wall.h, 0), to_world({wall.x + wall.w * 0.5f, wall.y + wall.h * 0.5f}),
                                      Quat::sIdentity(), EMotionType::Static, Layers::still);
        settings.mFriction = 0.6f;
        s.walls.push_back(s.bodies_interface().CreateAndAddBody(settings, EActivation::DontActivate));
    }
    s.bodies.clear();
    s.samples.clear();
}

bool Pond::add_duck(FluidPoint position) {
    auto& s = *impl_;
    const float facing = s.random() < 0.5f ? -1.0f : 1.0f;
    const float scale = 0.9f + s.random() * 0.25f;
    // Mostly classic yellow, sometimes pink or mint, rarely gold.
    const float roll = s.random();
    const float variant = roll < 0.6f ? 0 : roll < 0.8f ? 1 : roll < 0.95f ? 2 : 3;
    return s.create_duck(position, facing * scale, variant);
}

bool Pond::Impl::create_duck(FluidPoint position, float signed_scale, float variant) {
    auto& s = *this;
    if (s.ducks.size() >= max_ducks) return false;
    const float facing = signed_scale < 0 ? -1.0f : 1.0f;
    const float scale = std::abs(signed_scale);
    position.x = std::clamp(position.x, DUCK_REACH, std::max(DUCK_REACH, s.width - DUCK_REACH));
    position.y = std::clamp(position.y, DUCK_REACH, std::max(DUCK_REACH, s.height - DUCK_REACH));
    BodyCreationSettings settings(duck_shape(facing, scale), to_world(position),
                                  Quat::sRotation(Vec3::sAxisZ(), (s.random() - 0.5f) * 0.6f), EMotionType::Dynamic, Layers::creature);
    settings.mAllowedDOFs = EAllowedDOFs::Plane2D;
    settings.mAllowSleeping = false;  // buoyancy keeps changing
    settings.mMotionQuality = EMotionQuality::LinearCast;  // fast windows must not tunnel through ducks
    settings.mFriction = 0.9f;      // rubber grips
    settings.mRestitution = 0.35f;  // and bounces
    settings.mLinearDamping = 0.05f;
    settings.mAngularDamping = 1.0f;  // rocking on the rounded bottom settles quickly
    settings.mAngularVelocity = Vec3(0, 0, (s.random() - 0.5f) * 4.0f);
    const BodyID id = s.bodies_interface().CreateAndAddBody(settings, EActivation::Activate);
    if (id.IsInvalid()) return false;
    s.ducks.push_back({id, facing * scale, variant, s.next_tag++});
    s.refresh_bodies();
    return true;
}

PondState Pond::state() const {
    const auto& s = *impl_;
    PondState state;
    state.width = s.width;
    state.height = s.height;
    state.next_fish_number = s.next_fish_number;
    for (const auto& f : s.fish)
        state.fish.push_back({f.number, {f.genome, f.female, f.growth, f.generation, f.mother, f.father, f.brain_seed},
                              impl_->position(f.id), f.energy, f.fullness, f.oxygen, f.alive, f.meals});
    for (const auto& duck : s.ducks) state.ducks.push_back({impl_->position(duck.id), duck.facing, duck.variant});
    return state;
}

void Pond::restore(const PondState& state) {
    auto& s = *impl_;
    const float sx = state.width > 0 ? s.width / state.width : 1, sy = state.height > 0 ? s.height / state.height : 1;
    for (const auto& duck : state.ducks) s.create_duck({duck.position.x * sx, duck.position.y * sy}, duck.facing, duck.variant);
    for (const auto& saved : state.fish) {
        if (!add_fish({saved.position.x * sx, saved.position.y * sy}, saved.birth)) break;
        auto& f = s.fish.back();
        f.number = saved.number;
        f.energy = saved.energy;
        f.fullness = saved.fullness;
        f.oxygen = saved.oxygen;
        f.alive = saved.alive;
        f.meals = saved.meals;
        if (!f.alive) f.foreshorten = 1;
    }
    s.next_fish_number = std::max(s.next_fish_number, state.next_fish_number);
    s.refresh_bodies();
}

void Pond::clear_ducks() {
    auto& s = *impl_;
    for (const auto& duck : s.ducks) s.remove(duck.id);
    s.ducks.clear();
    s.refresh_bodies();
}

bool Pond::add_fish(FluidPoint position) {
    auto& s = *impl_;
    FishBirth birth;
    birth.genome = FishGenome::founder(s.genes);
    int balance = 0;  // females minus males among living adults
    for (const auto& f : s.fish)
        if (f.alive && f.growth >= adult_growth) balance += f.female ? 1 : -1;
    birth.female = balance < 0 || (balance == 0 && s.random() < 0.5f);
    birth.growth = 0.85f;  // shops sell young adults
    return add_fish(position, birth);
}

bool Pond::add_fish(FluidPoint position, const FishBirth& birth) {
    auto& s = *impl_;
    if (s.fish.size() >= max_fish) return false;
    const float scale = fish_scale(birth.growth, birth.genome.size);
    position.x = std::clamp(position.x, FISH_REACH, std::max(FISH_REACH, s.width - FISH_REACH));
    position.y = std::clamp(position.y, FISH_REACH, std::max(FISH_REACH, s.height - FISH_REACH));
    const bool left = s.random() < 0.5f;
    BodyCreationSettings settings(ellipse_hull(0, 0, FISH_RX, FISH_RY, 1, scale, fish_depth, water_density)->Create().Get(),
                                  to_world(position), Quat::sRotation(Vec3::sAxisZ(), left ? pi : 0), EMotionType::Dynamic,
                                  Layers::creature);
    settings.mAllowedDOFs = EAllowedDOFs::Plane2D;
    settings.mAllowSleeping = false;
    settings.mMotionQuality = EMotionQuality::LinearCast;
    settings.mFriction = 0.4f;     // slippery
    settings.mRestitution = 0.2f;
    settings.mAngularDamping = 0.5f;
    const BodyID id = s.bodies_interface().CreateAndAddBody(settings, EActivation::Activate);
    if (id.IsInvalid()) return false;
    const FishTraits traits{birth.genome.boldness, birth.genome.appetite, birth.genome.curiosity};
    const std::uint32_t brain_seed = birth.brain_seed ? birth.brain_seed : static_cast<std::uint32_t>(s.random() * 4e9f) | 1u;
    Impl::Fish fish{id, scale, s.bodies_interface().GetShape(id)->GetMassProperties().mMass,
                    FishBrain(brain_seed, s.hindbrain, false, traits), s.next_tag,
                    {s.next_tag + 1, s.next_tag + 2, s.next_tag + 3}};
    s.next_tag += 4;
    fish.number = s.next_fish_number++;
    fish.genome = birth.genome;
    fish.female = birth.female;
    fish.growth = birth.growth;
    fish.generation = birth.generation;
    fish.mother = birth.mother;
    fish.father = birth.father;
    fish.brain_seed = brain_seed;
    fish.shape_scale = scale;
    fish.upright = left ? -1.0f : 1.0f;
    s.fish.push_back(std::move(fish));
    s.refresh_bodies();
    return true;
}

void Pond::clear_fish() {
    auto& s = *impl_;
    for (const auto& fish : s.fish) s.remove(fish.id);
    s.fish.clear();
    s.refresh_bodies();
}

void Pond::add_food(FluidPoint position) {
    auto& s = *impl_;
    if (s.food.size() >= max_food) {
        s.remove(s.food.front().id);
        s.food.erase(s.food.begin());
    }
    position.x = std::clamp(position.x, 8.0f, std::max(8.0f, s.width - 8));
    position.y = std::clamp(position.y, 8.0f, std::max(8.0f, s.height - 8));
    Ref<SphereShapeSettings> sphere = new SphereShapeSettings(FOOD_RADIUS * metres_per_point);
    sphere->mDensity = 600;
    BodyCreationSettings settings(sphere->Create().Get(), to_world(position), Quat::sRotation(Vec3::sAxisZ(), s.random() * 2 * pi),
                                  EMotionType::Dynamic, Layers::food);
    settings.mAllowedDOFs = EAllowedDOFs::Plane2D;
    settings.mAllowSleeping = false;
    settings.mMotionQuality = EMotionQuality::LinearCast;
    settings.mFriction = 0.8f;
    settings.mRestitution = 0.1f;
    settings.mLinearVelocity = to_world_vector({(s.random() - 0.5f) * 60, s.random() * 40});
    settings.mAngularVelocity = Vec3(0, 0, (s.random() - 0.5f) * 10);
    const BodyID id = s.bodies_interface().CreateAndAddBody(settings, EActivation::Activate);
    if (id.IsInvalid()) return;
    s.food.push_back({id, s.bodies_interface().GetShape(id)->GetMassProperties().mMass, 0, s.random(), s.next_tag++});
    s.refresh_bodies();
}

void Pond::clear_food() {
    auto& s = *impl_;
    for (const auto& flake : s.food) s.remove(flake.id);
    s.food.clear();
    s.refresh_bodies();
}

std::size_t Pond::duck_count() const { return impl_->ducks.size(); }
std::size_t Pond::fish_count() const { return impl_->fish.size(); }
std::size_t Pond::food_count() const { return impl_->food.size(); }
const std::vector<PondBody>& Pond::bodies() const { return impl_->bodies; }
void Pond::set_pointer(FluidPoint position) { impl_->pointer = position; }
void Pond::set_seed(std::uint32_t seed) { impl_->random_state = seed ? seed : 1; }
void Pond::set_hindbrain(std::shared_ptr<const HindbrainConnectome> hindbrain) { impl_->hindbrain = std::move(hindbrain); }
void Pond::set_metabolism(float hunger, float breath, float growth) {
    impl_->metabolism = hunger;
    impl_->breathing = breath;
    impl_->growing = growth;
}

FishStatus Pond::fish_status() const {
    FishStatus status;
    for (const auto& f : impl_->fish) {
        if (!f.alive) {
            ++status.dead;
            continue;
        }
        ++status.alive;
        if (f.growth < adult_growth) ++status.young;
        status.generations = std::max(status.generations, f.generation);
        if (f.energy < 0.12f) ++status.starving;
        else if (f.energy < 0.35f) ++status.hungry;
        if (!f.in_water) ++status.gasping;
    }
    return status;
}

const FishBrain* Pond::fish_brain(std::size_t index) const {
    return index < impl_->fish.size() ? &impl_->fish[index].brain : nullptr;
}

std::vector<FishVitals> Pond::fish_vitals() const {
    std::vector<FishVitals> result;
    for (const auto& f : impl_->fish)
        result.push_back({f.number, fish_name(f.number), {f.genome, f.female, f.growth, f.generation, f.mother, f.father, f.brain_seed},
                          f.growth, fish_stage(f.growth), impl_->position(f.id), impl_->velocity(f.id), f.energy, f.oxygen, f.alive,
                          std::asin(std::clamp(-std::sin(impl_->heading(f.id)), -1.0f, 1.0f)), f.in_water, f.meals, f.escapes,
                          f.alive ? f.brain.hunting() : 0.0f});
    return result;
}

void Pond::set_windows(const std::vector<WindowObstacle>& windows) {
    auto& s = *impl_;
    auto& interface = s.bodies_interface();
    std::unordered_map<std::uint32_t, Impl::Window> next;
    for (const auto& w : windows) {
        const FluidPoint centre{w.x + w.width * 0.5f, w.y + w.height * 0.5f};
        auto known = s.windows.find(w.id);
        if (known == s.windows.end()) {
            BodyCreationSettings settings(box_shape(w.width, w.height, w.radius), to_world(centre), Quat::sIdentity(),
                                          EMotionType::Kinematic, Layers::window);
            settings.mFriction = 0.6f;
            const BodyID id = interface.CreateAndAddBody(settings, EActivation::Activate);
            if (!id.IsInvalid()) next[w.id] = {id, w.width, w.height, centre, centre, centre, {0, 0}};
            continue;
        }
        Impl::Window window = known->second;
        s.windows.erase(known);
        if (std::abs(window.width - w.width) > 0.5f || std::abs(window.height - w.height) > 0.5f) {
            interface.SetShape(window.id, box_shape(w.width, w.height, w.radius), false, EActivation::Activate);
            window.width = w.width;
            window.height = w.height;
        }
        window.from = window.to;
        window.to = centre;
        // A minimized or restored window jumps instead of sweeping ducks along.
        if (std::abs(window.to.x - window.from.x) > teleport_distance || std::abs(window.to.y - window.from.y) > teleport_distance) {
            interface.SetPositionAndRotation(window.id, to_world(centre), Quat::sIdentity(), EActivation::Activate);
            window.from = window.now = centre;
        }
        next[w.id] = window;
    }
    for (const auto& gone : s.windows) s.remove(gone.second.id);
    s.windows = std::move(next);
}

void Pond::step(float dt, float progress, FluidPoint gravity, const std::vector<BodySample>& water, float particle_area) {
    auto& s = *impl_;
    auto& interface = s.bodies_interface();
    for (auto& entry : s.windows) {
        auto& w = entry.second;
        const FluidPoint target{w.from.x + (w.to.x - w.from.x) * progress, w.from.y + (w.to.y - w.from.y) * progress};
        w.velocity = (target - w.now) * (1 / dt);
        w.now = target;
        interface.MoveKinematic(w.id, to_world(target), Quat::sIdentity(), dt);
    }
    // The pointer is read once a frame, so its velocity spans the steps since it last moved.
    s.pointer_elapsed += dt;
    if (s.pointer.x != s.pointer_seen.x || s.pointer.y != s.pointer_seen.y) {
        s.pointer_velocity = s.pointer_seen.x < -999 ? FluidPoint{0, 0} : (s.pointer - s.pointer_seen) * (1 / s.pointer_elapsed);
        s.pointer_seen = s.pointer;
        s.pointer_elapsed = 0;
    } else if (s.pointer_elapsed > 0.1f) s.pointer_velocity = {0, 0};

    const Vec3 g = to_world_vector(gravity);
    s.system->SetGravity(g);
    // Samples are matched to bodies by tag; bodies added since have none yet.
    s.samples.clear();
    for (const auto& sample : water) s.samples[sample.tag] = sample;
    s.particle_area = particle_area;
    for (const auto& duck : s.ducks) {
        const BodySample* sample = s.sample(duck.tag);
        if (!sample) continue;
        const float scale = std::abs(duck.facing);
        // Displaced water: submerged outline area times the duck's thickness.
        const float area = sample->count * particle_area;
        const float volume = area * duck_depth * scale * metres_per_point * metres_per_point * metres_per_point;
        const float mass = water_density * volume;
        const FluidPoint centroid{sample->x / sample->count, sample->y / sample->count};
        const RVec3 point = to_world(centroid);
        const Vec3 flow = to_world_vector({sample->vx / sample->count, sample->vy / sample->count});
        const Vec3 relative = flow - interface.GetPointVelocity(duck.id, point);
        // Buoyancy acts at the centre of the submerged water, against gravity.
        interface.AddForce(duck.id, -mass * g + drag_rate * mass * relative, point);
        const float submerged = std::min(1.0f, area / (duck_area * scale * scale));
        interface.SetAngularVelocity(duck.id, interface.GetAngularVelocity(duck.id) * std::exp(-spin_damping * submerged * dt));
    }
    for (auto& fish : s.fish) s.step_fish(fish, dt, g);
    for (auto& flake : s.food) s.step_food(flake, dt, g);
    s.system->Update(dt, 1, s.temp.get(), s.jobs.get());
    for (auto& fish : s.fish) s.eat(fish);
    for (std::size_t i = 0; i < s.food.size();) {
        if (s.food[i].age > food_lifetime) {
            s.remove(s.food[i].id);
            s.food.erase(s.food.begin() + i);
        } else ++i;
    }
    s.refresh_bodies();
}

namespace {
bool check(bool condition, const char* message) {
    if (condition) std::printf("PASS: %s\n", message);
    else std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

WindowObstacle window(std::uint32_t id, float x, float y, float w, float h) {
    return {x, y, w, h, x, y, w, h, 10, id, 0, 0};
}

// Couples the two simulations one fixed step at a time, as the app does.
void run(FluidSimulation& fluid, Pond& pond, int steps, const std::vector<WindowObstacle>& windows,
         bool pouring = false, FluidPoint nozzle = {0, 0}) {
    std::vector<BodySample> water;
    for (int i = 0; i < steps; ++i) {
        pond.set_windows(windows);
        if (!fluid.latest_body_samples(water)) water.clear();
        pond.step(FluidSimulation::step_time, 1, {0, 950}, water, fluid.particle_area());
        fluid.set_bodies(pond.bodies());
        fluid.advance(1, windows, pouring, nozzle);
        fluid.flush();
    }
}

// Pours a pool across the bottom of the canvas and returns its surface height.
float fill_pool(FluidSimulation& fluid, Pond& pond, float width, float height, int pour_steps = 1500) {
    fluid.clear();
    for (int i = 0; i < pour_steps; ++i) {
        const float x = 40 + std::fmod(i * 4.1f, width - 80);
        fluid.advance(1, {}, true, {x, 60});
        if (i % 120 == 119) fluid.flush();
    }
    run(fluid, pond, 240, {});
    std::vector<FluidParticle> particles;
    if (!fluid.read_particles(particles)) return height;
    // The surface is near the top of the water; stray splash drops above it
    // are ignored.
    std::vector<float> heights;
    for (const auto& p : particles)
        if (p.x > width * 0.2f && p.x < width * 0.8f) heights.push_back(p.y);
    if (heights.empty()) return height;
    std::sort(heights.begin(), heights.end());
    return heights[heights.size() / 100];
}
}

bool run_duck_checks(const char* shader_file) {
    FluidSimulation fluid;
    if (!fluid.initialize(shader_file, 640, 480)) return false;
    Pond ducks;
    ducks.reset(640, 480);

    // A dry duck falls onto a window and comes to rest on top of it.
    const std::vector<WindowObstacle> shelf{window(1, 200, 300, 240, 100)};
    ducks.add_duck({320, 150});
    run(fluid, ducks, 360, shelf);
    auto duck = ducks.bodies().at(0);
    if (!check(duck.y > 240 && duck.y < 300 && std::hypot(duck.vx, duck.vy) < 20,
               "a duck lands on a window and rests on top of it")) {
        std::fprintf(stderr, "  duck at (%.0f, %.0f), angle %.2f, speed %.0f\n", duck.x, duck.y, duck.angle, std::hypot(duck.vx, duck.vy));
        return false;
    }

    // Dragging the window sideways at a moderate speed carries the duck along.
    // (A sharp enough yank pulls the window out from under it instead.)
    std::vector<WindowObstacle> sliding = shelf;
    for (int i = 0; i < 120; ++i) {
        sliding[0].x = 200 + i * 1.0f;
        run(fluid, ducks, 1, sliding);
    }
    const auto carried = ducks.bodies().at(0);
    if (!check(carried.x > duck.x + 60 && carried.y < 300, "a dragged window carries the duck with it")) {
        std::fprintf(stderr, "  duck moved from (%.0f, %.0f) to (%.0f, %.0f)\n", duck.x, duck.y, carried.x, carried.y);
        return false;
    }

    // Fill a pool, then drop a duck in: it must float at the surface, upright.
    ducks.reset(640, 480);
    const float surface = fill_pool(fluid, ducks, 640, 480);
    ducks.add_duck({320, surface - 120});
    run(fluid, ducks, 720, {});
    duck = ducks.bodies().at(0);
    std::printf("  pool surface %.0f, duck at %.0f, tilt %.2f rad, speed %.0f\n", surface, duck.y,
                std::remainder(duck.angle, 2 * pi), std::hypot(duck.vx, duck.vy));
    if (!check(duck.y > surface - 25 && duck.y < surface + 15, "a duck floats at the water surface")) return false;
    if (!check(std::abs(std::remainder(duck.angle, 2 * pi)) < 0.25f, "a floating duck stays level"))
        return false;

    // Without water it sinks to the floor.
    fluid.clear();
    run(fluid, ducks, 360, {});
    return check(ducks.bodies().at(0).y > 440, "a duck falls to the floor when the water is cleared");
}

bool run_fish_checks(const char* shader_file, std::shared_ptr<const HindbrainConnectome> hindbrain) {
    {
        // Saving and loading.
        const auto scratch = std::filesystem::temp_directory_path() / "fluid-hose-pond-check";
        const bool stored = run_pond_store_checks(scratch);
        std::error_code ignored;
        std::filesystem::remove_all(scratch, ignored);
        if (!stored) return false;
    }
    // Every fish's brain is wired from its own seed, so check several.
    for (const std::uint32_t seed : {7u, 101u, 2024u, 4242u, 31337u, 99991u}) {
        std::printf("Brain seed %u\n", seed);
        if (!run_brain_checks(seed) || !run_spinal_checks(seed)) return false;
    }
    if (hindbrain) {
        std::printf("Measured hindbrain\n");
        if (!run_hindbrain_checks(hindbrain)) return false;
    } else std::printf("No hindbrain connectome; skipping its checks (run tools/fetch-connectome.sh)\n");
    FluidSimulation fluid;
    constexpr float width = 800, height = 480;
    if (!fluid.initialize(shader_file, width, height)) return false;
    Pond pond;
    pond.reset(width, height);
    pond.set_hindbrain(hindbrain);
    const float surface = fill_pool(fluid, pond, width, height, 5000);
    std::printf("  pool surface at %.0f of %.0f\n", surface, height);

    // A fish dropped into the pool lives in it and swims around.
    // An average adult; the brain checks cover genetic differences. Brains
    // differ even so: of eleven wirings tried, one often missed the food, so
    // the check names its wiring (FISH_SEED picks another).
    const FishBirth standard;
    const char* seed = std::getenv("FISH_SEED");
    pond.set_seed(seed ? static_cast<std::uint32_t>(std::atoi(seed)) : 1u);
    pond.add_fish({width * 0.5f, (surface + height) * 0.5f}, standard);
    float travelled = 0, lowest = height, highest = 0, tilt = 0;
    unsigned dry_steps = 0;
    for (int i = 0; i < 1200; ++i) {
        run(fluid, pond, 1, {});
        const auto fish = pond.fish_vitals().at(0);
        travelled += length(fish.velocity) * FluidSimulation::step_time;
        tilt += std::abs(fish.pitch) / 1200;
        lowest = std::min(lowest, fish.position.y);
        highest = std::max(highest, fish.position.y);
        if (!fish.in_water) ++dry_steps;
    }
    auto fish = pond.fish_vitals().at(0);
    std::printf("  10 s in the pool: swam %.0f points, depth %.0f to %.0f, mean pitch %.0f°, %u steps out of water\n", travelled,
                lowest, highest, tilt * 180 / pi, dry_steps);
    if (!check(fish.alive && travelled > 100, "a fish in water swims around")) return false;
    if (!check(dry_steps < 60, "a swimming fish stays in the water")) return false;
    if (!check(tilt < 0.4f, "a swimming fish keeps roughly level")) return false;

    // Food sprinkled on the surface: the fish finds it and eats.
    fish = pond.fish_vitals().at(0);
    for (int i = 0; i < 6; ++i) pond.add_food({fish.position.x + (fish.position.x < width / 2 ? 1 : -1) * (60 + i * 12.0f), surface - 30});
    int seconds = 0;
    for (; seconds < 60 && pond.fish_vitals().at(0).meals < 2; ++seconds) run(fluid, pond, 120, {});
    fish = pond.fish_vitals().at(0);
    std::printf("  food: %u flakes eaten after %d s, energy %.2f, %zu flakes left\n", fish.meals, seconds, fish.energy, pond.food_count());
    if (!check(fish.meals >= 2, "a hungry fish finds food and eats it")) return false;

    // A pointer rushing at the fish startles it. Leftover food goes first, so
    // the fish settles back into the water. Like real fish, it does not
    // always react, so the pointer gets three tries.
    pond.clear_food();
    for (int i = 0; i < 10 && !(pond.fish_vitals().at(0).in_water && pond.fish_vitals().at(0).position.y > surface + 30); ++i)
        run(fluid, pond, 60, {});
    // A try counts when the fish escapes and darts off; fleeing into a
    // nearby wall does not.
    int darts = 0, tries = 0;
    float fastest = 0;
    for (; tries < 3 && !darts; ++tries) {
        fish = pond.fish_vitals().at(0);
        const unsigned escapes = fish.escapes;
        const float side = fish.position.x < width / 2 ? 1.0f : -1.0f;  // from the open side
        float top = 0;
        for (int i = 0; i < 40; ++i) {
            const float approach = std::max(0.0f, 1 - i / 30.0f);
            pond.set_pointer({fish.position.x + side * (260 * approach + 10), fish.position.y});
            run(fluid, pond, 1, {});
            top = std::max(top, length(pond.fish_vitals().at(0).velocity));
        }
        pond.set_pointer({-1000, -1000});
        run(fluid, pond, 120, {});
        fastest = std::max(fastest, top);
        if (pond.fish_vitals().at(0).escapes > escapes && top > 150) ++darts;
    }
    std::printf("  startle: darted away on try %d, top speed %.0f points/s\n", darts ? tries : 0, fastest);
    if (!check(darts > 0, "a pointer rushing at the fish makes it dart away")) return false;

    // Without food it eventually starves, then floats belly up.
    pond.set_metabolism(1500);
    for (int i = 0; i < 10 && pond.fish_vitals().at(0).alive; ++i) run(fluid, pond, 60, {});
    pond.set_metabolism(1);
    fish = pond.fish_vitals().at(0);
    if (!check(!fish.alive && fish.energy <= 0, "a fish without food starves")) return false;
    run(fluid, pond, 960, {});
    fish = pond.fish_vitals().at(0);
    std::printf("  dead fish at %.0f, surface %.0f\n", fish.position.y, surface);
    if (!check(fish.position.y < surface + 25, "a dead fish floats to the surface")) return false;

    // A well-fed hatchling grows into an adult, its body with it.
    pond.clear_fish();
    fill_pool(fluid, pond, width, height, 5000);
    FishBirth hatchling;
    hatchling.growth = 0.05f;
    hatchling.female = true;
    pond.add_fish({width * 0.5f, height - 80}, hatchling);
    const FishVitals young = pond.fish_vitals().at(0);
    pond.set_metabolism(1, 1, 120);  // 20 minutes of growing in 10 seconds
    for (int i = 0; i < 12; ++i) {
        for (int k = 0; k < 4; ++k) pond.add_food({width * (0.3f + 0.13f * k), surface - 20});
        run(fluid, pond, 120, {});
    }
    pond.set_metabolism(1);
    const FishVitals grown = pond.fish_vitals().at(0);
    std::printf("  growing: %s at %.2f grown to %s at %.2f, %u meals\n", young.stage == FishStage::larva ? "larva" : "?", young.growth,
                grown.stage == FishStage::adult ? "adult" : grown.stage == FishStage::juvenile ? "juvenile" : "larva", grown.growth,
                grown.meals);
    if (!check(grown.alive && grown.stage == FishStage::adult, "a well-fed hatchling grows into an adult")) return false;
    pond.clear_food();

    // A pond's state brings back the same fish in a new pond: their
    // genomes, growth, numbers, and brain wiring.
    {
        Pond copy;
        copy.reset(width, height);
        copy.restore(pond.state());
        const auto a = pond.fish_vitals(), b = copy.fish_vitals();
        const bool same = a.size() == b.size() && !a.empty() && a[0].number == b[0].number &&
                          std::abs(a[0].growth - b[0].growth) < 1e-4f && a[0].birth.female == b[0].birth.female &&
                          a[0].birth.brain_seed == b[0].birth.brain_seed && a[0].birth.brain_seed != 0;
        if (!check(same, "a restored pond has the same fish")) return false;
    }

    // A fish out of water flops about and suffocates.
    pond.clear_fish();
    fluid.clear();
    pond.add_fish({width * 0.5f, height - 60}, standard);
    pond.set_metabolism(1, 25);  // two seconds of air
    float highest_flop = height;
    for (int i = 0; i < 360; ++i) {
        run(fluid, pond, 1, {});
        if (i > 60) highest_flop = std::min(highest_flop, pond.fish_vitals().at(0).position.y);
    }
    fish = pond.fish_vitals().at(0);
    std::printf("  stranded: flopped up to %.0f points, oxygen %.2f\n", height - highest_flop, fish.oxygen);
    if (!check(height - highest_flop > 20, "a stranded fish flops")) return false;
    return check(!fish.alive && fish.oxygen <= 0, "a stranded fish suffocates");
}
