#pragma once
#include "fish_genome.h"
#include "fluid_types.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct FishStatus {
    unsigned alive = 0;
    unsigned hungry = 0;    // could use a meal
    unsigned starving = 0;  // will die soon without one
    unsigned gasping = 0;   // out of the water
    unsigned dead = 0;
    unsigned young = 0;     // larvae and juveniles
    unsigned generations = 1;  // the highest generation alive
};

// How a fish starts life.
struct FishBirth {
    FishGenome genome;
    bool female = false;
    float growth = 1;         // 0 hatchling to 1 full grown
    unsigned generation = 1;  // founders are generation 1
    std::uint32_t mother = 0, father = 0;  // parents' numbers; 0 for a founder
};

enum class FishStage { larva, juvenile, adult };

// One fish's vital signs, for checks and the menu.
struct FishVitals {
    std::uint32_t number;  // unique within the pond
    std::string name;
    FishBirth birth;
    float growth;
    FishStage stage;
    FluidPoint position;
    FluidPoint velocity;
    float energy;  // 0 starved to 1 well fed
    float oxygen;  // 0 suffocated to 1
    bool alive;
    float pitch;  // radians, positive nose up
    bool in_water;
    unsigned meals;
    unsigned escapes;
    float hunting;  // 0 to 1
};

// Rubber ducks, fish, and fish food simulated with Jolt Physics, locked to
// the screen plane. Windows are kinematic bodies that follow the window
// tracker, the screen edges are walls, and the water measured on the GPU
// supplies buoyancy and drag. Each fish has a spiking brain (fish_brain.h)
// that sees food and threats and swims in bouts.
class Pond {
public:
    static constexpr std::size_t max_ducks = 32;
    static constexpr std::size_t max_fish = 16;
    static constexpr std::size_t max_food = 48;

    Pond();
    ~Pond();
    Pond(const Pond&) = delete;
    Pond& operator=(const Pond&) = delete;

    // Rebuilds the screen walls for a new canvas and removes everything else.
    void reset(float width, float height);
    // Drops a duck with its centre at position. Returns false when full.
    bool add_duck(FluidPoint position);
    void clear_ducks();
    std::size_t duck_count() const;
    // Drops a young adult fish, as bought: a founder of either sex,
    // whichever the pond has fewer adults of. Returns false when full.
    bool add_fish(FluidPoint position);
    // Adds a fish with a given start in life. Returns false when full.
    bool add_fish(FluidPoint position, const FishBirth& birth);
    void clear_fish();
    std::size_t fish_count() const;
    // Drops one food flake; the oldest flake goes when there are too many.
    void add_food(FluidPoint position);
    void clear_food();
    std::size_t food_count() const;
    // Fish watch the mouse pointer and flee when it rushes at them.
    void set_pointer(FluidPoint position);
    // Reseeds the pond's randomness, which wires new fishes' brains (for checks).
    void set_seed(std::uint32_t seed);
    // Fish added from now on build their eye-position circuit from this
    // measured wiring (null: hand-wired brains only).
    void set_hindbrain(std::shared_ptr<const struct HindbrainConnectome> hindbrain);
    // Multiplies how fast fish get hungry and run out of air (for checks).
    void set_metabolism(float hunger, float breath = 1, float growth = 1);
    FishStatus fish_status() const;
    // A fish's brain, for watching it think; null past the last fish.
    const class FishBrain* fish_brain(std::size_t index) const;
    std::vector<FishVitals> fish_vitals() const;

    // Sets the windows' positions for this frame; steps move them there
    // gradually from where the previous frame left them.
    void set_windows(const std::vector<WindowObstacle>& windows);
    // Advances one fixed step. progress is how far through the frame's window
    // motion this step ends (0 to 1). water holds the latest GPU measurement
    // for each body, and particle_area the water area of one particle.
    void step(float dt, float progress, FluidPoint gravity, const std::vector<BodySample>& water, float particle_area);

    // Ducks, then fish, then food, then each fish's water probes.
    const std::vector<PondBody>& bodies() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool run_duck_checks(const char* shader_file);
// With a hindbrain connectome, also checks the circuit built from it, and
// the pond's fish use it.
bool run_fish_checks(const char* shader_file, std::shared_ptr<const struct HindbrainConnectome> hindbrain = nullptr);
