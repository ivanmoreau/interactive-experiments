#include "duck_world.h"
#include "fluid_sim.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace {
using namespace JPH;

// Jolt works in metres with y up; the screen uses points with y down.
// 100 points per metre puts 950 point/s² gravity close to Earth's.
constexpr float metres_per_point = 0.01f;
constexpr float water_density = 1000.0f;  // kg/m³
constexpr float duck_density = 280.0f;    // floats with about a quarter of its outline submerged
constexpr float duck_depth = 30.0f;       // thickness across the screen plane, in points at scale 1
constexpr float drag_rate = 5.0f;         // per second, relative to the displaced water's mass
constexpr float spin_damping = 3.0f;      // per second when fully submerged
constexpr float teleport_distance = 500.0f;
constexpr float wall_thickness = 100.0f;

RVec3 to_world(FluidPoint p) { return RVec3(p.x * metres_per_point, -p.y * metres_per_point, 0); }
Vec3 to_world_vector(FluidPoint v) { return Vec3(v.x * metres_per_point, -v.y * metres_per_point, 0); }

namespace Layers {
constexpr ObjectLayer still = 0;   // screen walls
constexpr ObjectLayer moving = 1;  // ducks and windows
}

class BroadPhaseLayers final : public BroadPhaseLayerInterface {
public:
    uint GetNumBroadPhaseLayers() const override { return 2; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override { return BroadPhaseLayer(static_cast<uint8>(layer)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer layer) const override {
        return layer.GetValue() == 0 ? "still" : "moving";
    }
#endif
};

class ObjectBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer object, BroadPhaseLayer broad_phase) const override {
        return object == Layers::moving || broad_phase.GetValue() == Layers::moving;
    }
};

class ObjectPairFilter final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override { return a == Layers::moving || b == Layers::moving; }
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

// An extruded convex outline of an ellipse in local duck points.
Ref<ShapeSettings> ellipse_hull(float cx, float cy, float rx, float ry, float facing, float scale) {
    Array<Vec3> points;
    const float half_depth = duck_depth * scale * 0.5f * metres_per_point;
    for (int i = 0; i < 16; ++i) {
        const float angle = i * 2.0f * static_cast<float>(M_PI) / 16;
        const float x = (cx + rx * std::cos(angle)) * facing * scale * metres_per_point;
        const float y = -(cy + ry * std::sin(angle)) * scale * metres_per_point;
        points.push_back(Vec3(x, y, half_depth));
        points.push_back(Vec3(x, y, -half_depth));
    }
    auto* hull = new ConvexHullShapeSettings(points, 0.01f);
    hull->mDensity = duck_density;
    return hull;
}

// Body, head, and beak.
RefConst<Shape> duck_shape(float facing, float scale) {
    // Shape settings are reference counted, so they live on the heap.
    Ref<StaticCompoundShapeSettings> compound = new StaticCompoundShapeSettings;
    compound->AddShape(Vec3::sZero(), Quat::sIdentity(),
                       ellipse_hull(DUCK_BODY_X, DUCK_BODY_Y, DUCK_BODY_RX, DUCK_BODY_RY, facing, scale));
    compound->AddShape(Vec3::sZero(), Quat::sIdentity(),
                       ellipse_hull(DUCK_HEAD_X, DUCK_HEAD_Y, DUCK_HEAD_R, DUCK_HEAD_R, facing, scale));
    compound->AddShape(Vec3::sZero(), Quat::sIdentity(),
                       ellipse_hull(DUCK_BEAK_X, DUCK_BEAK_Y, DUCK_BEAK_RX, DUCK_BEAK_RY, facing, scale));
    // Like a weighted toy, the centre of mass sits under the middle of the
    // body rather than toward the head, so ducks float level and self-right.
    const RefConst<Shape> outline = compound->Create().Get();
    const Vec3 balance(DUCK_BODY_X * facing * scale * metres_per_point, -(DUCK_BODY_Y + 6.0f) * scale * metres_per_point, 0);
    Ref<OffsetCenterOfMassShapeSettings> weighted =
        new OffsetCenterOfMassShapeSettings(balance - outline->GetCenterOfMass(), outline.GetPtr());
    return weighted->Create().Get();
}

// Outline area at scale 1, used to judge how much of a duck is under water.
const float duck_area = static_cast<float>(M_PI) * (DUCK_BODY_RX * DUCK_BODY_RY + DUCK_HEAD_R * DUCK_HEAD_R +
                                               DUCK_BEAK_RX * DUCK_BEAK_RY);

RefConst<Shape> box_shape(float width, float height, float radius) {
    const Vec3 half(width * 0.5f * metres_per_point, height * 0.5f * metres_per_point, 1.0f);
    const float convex = std::min({radius * metres_per_point, half.GetX() * 0.9f, half.GetY() * 0.9f, 0.05f});
    Ref<BoxShapeSettings> box = new BoxShapeSettings(half, std::max(convex, 0.0f));
    return box->Create().Get();
}
}

struct DuckWorld::Impl {
    struct Duck { BodyID id; float facing; float variant; };
    struct Window { BodyID id; float width, height; FluidPoint from, to; };

    BroadPhaseLayers broad_phase_layers;
    ObjectBroadPhaseFilter object_broad_phase_filter;
    ObjectPairFilter object_pair_filter;
    std::unique_ptr<TempAllocatorImpl> temp;
    std::unique_ptr<JobSystemSingleThreaded> jobs;
    std::unique_ptr<PhysicsSystem> system;
    std::vector<BodyID> walls;
    std::vector<Duck> ducks;
    std::unordered_map<std::uint32_t, Window> windows;
    std::vector<DuckBody> bodies;
    float width = 0, height = 0;
    std::uint32_t random_state = 0x9e3779b9u;

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
                              -interface.GetAngularVelocity(duck.id).GetZ(), duck.variant});
        }
    }
};

DuckWorld::DuckWorld() : impl_(std::make_unique<Impl>()) {
    initialize_jolt();
    auto& s = *impl_;
    s.temp = std::make_unique<TempAllocatorImpl>(8 * 1024 * 1024);
    s.jobs = std::make_unique<JobSystemSingleThreaded>(cMaxPhysicsJobs);
    s.system = std::make_unique<PhysicsSystem>();
    s.system->Init(512, 0, 2048, 2048, s.broad_phase_layers, s.object_broad_phase_filter, s.object_pair_filter);
}

DuckWorld::~DuckWorld() {
    auto& s = *impl_;
    clear_ducks();
    for (const auto& window : s.windows) s.remove(window.second.id);
    for (const auto& wall : s.walls) s.remove(wall);
}

void DuckWorld::reset(float width, float height) {
    auto& s = *impl_;
    clear_ducks();
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
}

bool DuckWorld::add_duck(FluidPoint position) {
    auto& s = *impl_;
    if (s.ducks.size() >= max_ducks) return false;
    const float facing = s.random() < 0.5f ? -1.0f : 1.0f;
    const float scale = 0.9f + s.random() * 0.25f;
    // Mostly classic yellow, sometimes pink or mint, rarely gold.
    const float roll = s.random();
    const float variant = roll < 0.6f ? 0 : roll < 0.8f ? 1 : roll < 0.95f ? 2 : 3;
    position.x = std::clamp(position.x, DUCK_REACH, std::max(DUCK_REACH, s.width - DUCK_REACH));
    position.y = std::clamp(position.y, DUCK_REACH, std::max(DUCK_REACH, s.height - DUCK_REACH));
    BodyCreationSettings settings(duck_shape(facing, scale), to_world(position),
                                  Quat::sRotation(Vec3::sAxisZ(), (s.random() - 0.5f) * 0.6f), EMotionType::Dynamic, Layers::moving);
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
    s.ducks.push_back({id, facing * scale, variant});
    s.refresh_bodies();
    return true;
}

void DuckWorld::clear_ducks() {
    auto& s = *impl_;
    for (const auto& duck : s.ducks) s.remove(duck.id);
    s.ducks.clear();
    s.bodies.clear();
}

std::size_t DuckWorld::size() const { return impl_->ducks.size(); }
const std::vector<DuckBody>& DuckWorld::bodies() const { return impl_->bodies; }

void DuckWorld::set_windows(const std::vector<WindowObstacle>& windows) {
    auto& s = *impl_;
    auto& interface = s.bodies_interface();
    std::unordered_map<std::uint32_t, Impl::Window> next;
    for (const auto& w : windows) {
        const FluidPoint centre{w.x + w.width * 0.5f, w.y + w.height * 0.5f};
        auto known = s.windows.find(w.id);
        if (known == s.windows.end()) {
            BodyCreationSettings settings(box_shape(w.width, w.height, w.radius), to_world(centre), Quat::sIdentity(),
                                          EMotionType::Kinematic, Layers::moving);
            settings.mFriction = 0.6f;
            const BodyID id = interface.CreateAndAddBody(settings, EActivation::Activate);
            if (!id.IsInvalid()) next[w.id] = {id, w.width, w.height, centre, centre};
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
            window.from = centre;
        }
        next[w.id] = window;
    }
    for (const auto& gone : s.windows) s.remove(gone.second.id);
    s.windows = std::move(next);
}

void DuckWorld::step(float dt, float progress, FluidPoint gravity, const std::vector<DuckSample>& water, float particle_area) {
    auto& s = *impl_;
    auto& interface = s.bodies_interface();
    for (const auto& entry : s.windows) {
        const auto& w = entry.second;
        const FluidPoint target{w.from.x + (w.to.x - w.from.x) * progress, w.from.y + (w.to.y - w.from.y) * progress};
        interface.MoveKinematic(w.id, to_world(target), Quat::sIdentity(), dt);
    }
    const Vec3 g = to_world_vector(gravity);
    s.system->SetGravity(g);
    // Samples describe the ducks in order; a mismatched set is from before ducks were added or removed.
    if (water.size() == s.ducks.size()) {
        for (std::size_t i = 0; i < s.ducks.size(); ++i) {
            const auto& sample = water[i];
            if (sample.count < 1) continue;
            const auto& duck = s.ducks[i];
            const float scale = std::abs(duck.facing);
            // Displaced water: submerged outline area times the duck's thickness.
            const float area = sample.count * particle_area;
            const float volume = area * duck_depth * scale * metres_per_point * metres_per_point * metres_per_point;
            const float mass = water_density * volume;
            const FluidPoint centroid{sample.x / sample.count, sample.y / sample.count};
            const RVec3 point = to_world(centroid);
            const Vec3 flow = to_world_vector({sample.vx / sample.count, sample.vy / sample.count});
            const Vec3 relative = flow - interface.GetPointVelocity(duck.id, point);
            // Buoyancy acts at the centre of the submerged water, against gravity.
            interface.AddForce(duck.id, -mass * g + drag_rate * mass * relative, point);
            const float submerged = std::min(1.0f, area / (duck_area * scale * scale));
            interface.SetAngularVelocity(duck.id, interface.GetAngularVelocity(duck.id) * std::exp(-spin_damping * submerged * dt));
        }
    }
    s.system->Update(dt, 1, s.temp.get(), s.jobs.get());
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
void run(FluidSimulation& fluid, DuckWorld& ducks, int steps, const std::vector<WindowObstacle>& windows,
         bool pouring = false, FluidPoint nozzle = {0, 0}) {
    std::vector<DuckSample> water;
    for (int i = 0; i < steps; ++i) {
        ducks.set_windows(windows);
        if (!fluid.latest_duck_samples(water)) water.clear();
        ducks.step(FluidSimulation::step_time, 1, {0, 950}, water, fluid.particle_area());
        fluid.set_ducks(ducks.bodies());
        fluid.advance(1, windows, pouring, nozzle);
        fluid.flush();
    }
}
}

bool run_duck_checks(const char* shader_file) {
    FluidSimulation fluid;
    if (!fluid.initialize(shader_file, 640, 480)) return false;
    DuckWorld ducks;
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
    fluid.clear();
    for (int i = 0; i < 1500; ++i) {
        const float x = 40 + std::fmod(i * 4.1f, 560.0f);
        fluid.advance(1, {}, true, {x, 60});
        if (i % 120 == 119) fluid.flush();
    }
    run(fluid, ducks, 240, {});
    std::vector<FluidParticle> particles;
    if (!fluid.read_particles(particles)) return false;
    float surface = 480;
    for (const auto& p : particles)
        if (p.x > 120 && p.x < 520) surface = std::min(surface, p.y);
    ducks.add_duck({320, surface - 120});
    run(fluid, ducks, 720, {});
    duck = ducks.bodies().at(0);
    std::printf("  pool surface %.0f, duck at %.0f, tilt %.2f rad, speed %.0f\n", surface, duck.y,
                std::remainder(duck.angle, 2 * static_cast<float>(M_PI)), std::hypot(duck.vx, duck.vy));
    if (!check(duck.y > surface - 25 && duck.y < surface + 15, "a duck floats at the water surface")) return false;
    if (!check(std::abs(std::remainder(duck.angle, 2 * static_cast<float>(M_PI))) < 0.25f, "a floating duck stays level"))
        return false;

    // Without water it sinks to the floor.
    fluid.clear();
    run(fluid, ducks, 360, {});
    return check(ducks.bodies().at(0).y > 440, "a duck falls to the floor when the water is cleared");
}
