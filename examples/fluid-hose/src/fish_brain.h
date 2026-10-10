#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// What a fish senses, refreshed every physics step. Directions are relative
// to the fish's heading, in radians, positive toward its back (dorsal side).
// The screen is a side view, so the fish's two eyes see the same plane; the
// brain's two halves cover the half above its heading and the half below.
struct FishSenses {
    static constexpr int prey_bins = 16;  // -165° to 165°; the 30° behind the tail is blind
    static constexpr int loom_bins = 8;   // all around, 45° each
    static constexpr float prey_field = 2.88f;  // radians on each side of the heading
    std::array<float, prey_bins> prey{};       // food seen in each direction, 0 to 1
    std::array<float, loom_bins> looming{};    // something rushing at the fish, 0 to 1
    float strike_zone = 0;     // food right in front of the mouth, 0 to 1
    float lateral_line = 0;    // sudden water movement, 0 to 1
    float pitch = 0;           // radians; positive when the nose points toward the back's side
    float blocked_dorsal = 0;  // air, a window, or the screen edge ahead and above, 0 to 1
    float blocked_ventral = 0;
    float blocked_ahead = 0;
    float hunger = 0;    // energy missing, 0 to 1
    float fullness = 0;  // food still in the gut, 0 to 1
    float hypoxia = 0;   // oxygen missing, 0 to 1

    static float prey_angle(int bin) { return -prey_field + (bin + 0.5f) * 2 * prey_field / prey_bins; }
    static float loom_angle(int bin) { return -3.14159265f + (bin + 0.5f) * 2 * 3.14159265f / loom_bins; }
};

// A swim bout: larval fish move in discrete flicks rather than continuously.
enum class BoutKind { swim, hunt, strike, escape, reverse };
struct FishBout {
    BoutKind kind = BoutKind::swim;
    float turn = 0;   // radians toward the back's side; beyond ±100° the fish flips around
    float speed = 0;  // points per second gained along the new heading
};

// A spiking network laid out like the larval zebrafish circuits for hunting,
// escape, exploration, and hunger, at about 300 leaky integrate-and-fire
// neurons simulated in 1 ms ticks. The wiring is hand-built from the
// published circuit diagrams, not traced from a connectome.
class FishBrain {
public:
    explicit FishBrain(std::uint32_t seed);
    ~FishBrain();
    FishBrain(FishBrain&&) noexcept;
    FishBrain& operator=(FishBrain&&) noexcept;

    // Runs the network for dt seconds. Returns true when a bout starts.
    bool advance(const FishSenses& senses, float dt, FishBout& bout);
    float vergence() const;  // eyes turned in to hunt, 0 to 1
    float hunting() const;   // hunting state, 0 to 1
    std::size_t neuron_count() const;
    std::size_t synapse_count() const;

    struct Rate { const char* name; float hertz; };
    std::vector<Rate> rates() const;  // each population's recent firing rate

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool run_brain_checks(std::uint32_t seed = 7);
