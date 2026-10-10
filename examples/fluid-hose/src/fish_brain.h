#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
    std::array<float, prey_bins> mate{};  // adults of the other sex in view, by direction like prey, 0 to 1
    float spawning = 0;  // the morning urge to spawn, 0 to 1

    static float prey_angle(int bin) { return -prey_field + (bin + 0.5f) * 2 * prey_field / prey_bins; }
    static float loom_angle(int bin) { return -3.14159265f + (bin + 0.5f) * 2 * 3.14159265f / loom_bins; }
};

// A measured wiring diagram that part of the brain is built from: one side of
// the larval zebrafish hindbrain (Vishwanathan et al., Nature Neuroscience
// 2024), as converted by tools/convert_connectome.py.
struct HindbrainConnectome {
    struct Synapse { std::uint32_t pre, post, count; };
    std::vector<std::string> kinds;  // per cell: integrator, vestibular, abducens_motor, ...
    std::vector<Synapse> synapses;
    // Null when the file is missing or malformed.
    static std::shared_ptr<const HindbrainConnectome> load(const std::string& path);
};

// Inherited differences in how a brain behaves, each 1 for an average fish.
// They tune strengths in the brain, not its wiring.
struct FishTraits {
    float boldness = 1;   // weakens the looming input to the escape cells
    float appetite = 1;   // strengthens hunger's drive
    float curiosity = 1;  // more bouts, and more turning around to explore
    bool female = false;  // in courtship, males chase and females lead
};

// A swim bout: larval fish move in discrete flicks rather than continuously.
enum class BoutKind { swim, hunt, strike, escape, reverse, court };
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
    // With a hindbrain connectome, the eye-position circuit is built from its
    // measured wiring; shuffle_hindbrain rewires it at random, keeping each
    // cell type's synapse counts (a control for checks).
    explicit FishBrain(std::uint32_t seed, std::shared_ptr<const HindbrainConnectome> hindbrain = nullptr,
                       bool shuffle_hindbrain = false, FishTraits traits = {});
    ~FishBrain();
    FishBrain(FishBrain&&) noexcept;
    FishBrain& operator=(FishBrain&&) noexcept;

    // Runs the network for dt seconds. Returns true when a bout starts.
    bool advance(const FishSenses& senses, float dt, FishBout& bout);
    float vergence() const;  // eyes turned in to hunt, 0 to 1
    float hunting() const;   // hunting state, 0 to 1
    float courtship() const;  // courting a mate, 0 to 1
    // Where the visible eye looks: -1 back toward the tail to 1 forward, from
    // the abducens neurons. Zero without a hindbrain connectome.
    float eye_position() const;
    bool measured_hindbrain() const;
    // The spinal motor neurons right now: which way they bend the tail (-1
    // right to 1 left; drawn as toward the back and toward the belly in the
    // side view), and how hard both sides are working, 0 and up.
    float tail() const;
    float beat() const;
    std::size_t neuron_count() const;
    std::size_t synapse_count() const;

    struct Rate { const char* name; float hertz; };
    std::vector<Rate> rates() const;  // each population's recent firing rate

    // For drawing the brain: each population's place on a side view of a
    // larval zebrafish brain (0 to 1, snout to the left, top up), the kind of
    // circuit it belongs to, its neurons, and its recent firing rate.
    enum class Circuit { sense, decide, move, drive };
    struct Area {
        const char* name;
        const char* label;  // short, for the diagram
        std::size_t first, size;
        float x, y;
        Circuit circuit;
        bool topographic;  // neurons ordered by the direction they respond to
        bool measured;     // wired from the connectome rather than by hand
        bool spinal;       // in the spinal cord, drawn apart from the brain
        float hertz;
    };
    std::vector<Area> layout() const;
    // Each neuron's recent spikes, decaying over about 0.1 s.
    const std::vector<float>& activity() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool run_hindbrain_checks(std::shared_ptr<const HindbrainConnectome> hindbrain);
    friend bool run_spinal_checks(std::uint32_t seed);
};

bool run_brain_checks(std::uint32_t seed = 7);
bool run_spinal_checks(std::uint32_t seed = 7);
// Checks the eye-position circuit built from the measured hindbrain wiring
// against the same circuit wired at random.
bool run_hindbrain_checks(std::shared_ptr<const HindbrainConnectome> hindbrain);
