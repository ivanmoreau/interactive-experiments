#include "fish_brain.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace {
constexpr float tick = 0.001f;            // seconds
constexpr float membrane_time = 0.020f;   // seconds
constexpr float excitation_time = 0.005f;
constexpr float inhibition_time = 0.010f;
constexpr float rate_time = 0.050f;       // smoothing of the population rates
constexpr int refractory_ticks = 2;
constexpr float pi = 3.14159265f;
constexpr float prey_seen = 0.25f;  // tectal vector strength that counts as seeing prey

float wrap(float angle) { return std::remainder(angle, 2 * pi); }

// Populations, named after the zebrafish structures they stand in for.
enum Pop {
    retina,       // prey-detecting retinal ganglion cells, 2 per direction
    tectum,       // optic tectum periventricular neurons, 3 per direction
    tectal_inhibition,
    strike_zone,  // pretectal cells for food in the binocular zone ahead
    looming,      // looming detectors, 2 per direction
    lateral_line,
    mauthner_dorsal,  // the two giant escape neurons
    mauthner_ventral,
    escape_inhibition,  // feedback inhibition that keeps escapes single
    hunger,       // hypothalamic hunger neurons
    satiety,
    hunt_command, // the hunting-state command, which persists for seconds
    eye_vergence, // oculomotor neurons that turn the eyes in to hunt
    turning_left,       // anterior rhombencephalic turning region: two halves that
    turning_right,      // take turns leading, so exploration turns come in streaks
    turning_inhibition_left,
    turning_inhibition_right,
    tilt_dorsal,        // vestibular: nose tipped toward the back's side
    tilt_ventral,
    blocked_dorsal,
    blocked_ventral,
    blocked_ahead,
    hypoxia,
    steer_dorsal,       // reticulospinal turn commands
    steer_ventral,
    reverse,
    swim_speed,         // nucleus of the medial longitudinal fasciculus
    bout_generator,     // hindbrain burst generator that times bouts
    strike,
    population_count
};

struct Population {
    const char* name;
    int size;
    float tonic;      // baseline input current; 1 is the firing threshold
    float noise;      // standard deviation of the membrane potential noise
    float adaptation; // current added per spike, subtracted from the input
    float adaptation_time;
};

const Population populations[population_count] = {
    {"retina", 2 * FishSenses::prey_bins, 0.4f, 0.25f, 0, 1},
    {"tectum", 3 * FishSenses::prey_bins, 0.2f, 0.25f, 0, 1},
    {"tectal inhibition", 8, 0.3f, 0.2f, 0, 1},
    {"strike zone", 6, 0.4f, 0.2f, 0, 1},
    {"looming", 2 * FishSenses::loom_bins, 0.3f, 0.2f, 0, 1},
    {"lateral line", 4, 0.3f, 0.2f, 0, 1},
    {"Mauthner (dorsal)", 1, 0, 0.05f, 0, 1},
    {"Mauthner (ventral)", 1, 0, 0.05f, 0, 1},
    {"escape inhibition", 4, 0, 0.05f, 0, 1},
    {"hunger", 16, 0.6f, 0.3f, 0, 1},
    {"satiety", 8, 0.5f, 0.25f, 0, 1},
    {"hunting", 16, 0.2f, 0.25f, 0.012f, 2.0f},
    {"vergence", 6, 0.0f, 0.2f, 0, 1},
    {"turning (left)", 16, 1.05f, 0.3f, 0.002f, 8.0f},
    {"turning (right)", 16, 1.05f, 0.3f, 0.002f, 8.0f},
    {"turning inhibition (left)", 4, 0.3f, 0.2f, 0, 1},
    {"turning inhibition (right)", 4, 0.3f, 0.2f, 0, 1},
    {"tilt (dorsal)", 6, 0.5f, 0.2f, 0, 1},
    {"tilt (ventral)", 6, 0.5f, 0.2f, 0, 1},
    {"blocked (dorsal)", 4, 0.4f, 0.2f, 0, 1},
    {"blocked (ventral)", 4, 0.4f, 0.2f, 0, 1},
    {"blocked (ahead)", 4, 0.4f, 0.2f, 0, 1},
    {"hypoxia", 6, 0.4f, 0.2f, 0, 1},
    {"steer (dorsal)", 12, 0.5f, 0.3f, 0, 1},
    {"steer (ventral)", 12, 0.5f, 0.3f, 0, 1},
    {"reverse", 8, 0.4f, 0.25f, 0, 1},
    {"swim speed", 12, 0.6f, 0.3f, 0, 1},
    {"bout generator", 10, 0.95f, 0.35f, 0.08f, 1.0f},
    {"strike", 6, 0.2f, 0.2f, 0, 1},
};

struct Synapse { std::uint16_t target; float weight; };
}

struct FishBrain::Impl {
    int first[population_count + 1] = {};
    std::vector<int> owner;  // population of each neuron
    std::vector<float> voltage, excitation, inhibition, adaptation, input;
    std::vector<int> refractory;
    std::vector<std::uint32_t> synapse_start;
    std::vector<Synapse> synapses;
    std::vector<std::uint16_t> spiked;
    float rate[population_count] = {};
    float decay_adaptation[population_count] = {};
    std::uint32_t random_state;
    float since_bout = 1, since_escape = 1, since_strike = 1;
    float time_left = 0;
    float yaw = 0;  // left/right heading change since the fish last turned around

    explicit Impl(std::uint32_t seed) : random_state(seed ? seed : 0x2545f491u) {}

    float random() {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        return static_cast<float>(random_state & 0xffffffu) / 16777216.0f;
    }
    // Roughly normal, from the sum of three uniforms.
    float gaussian() { return (random() + random() + random() - 1.5f) * 2.0f; }

    int size(Pop p) const { return first[p + 1] - first[p]; }
    int neuron(Pop p, int index) const { return first[p] + index; }

    // Connects each pair with probability chance; weight may vary with the
    // two neurons' indices within their populations (0 skips the pair).
    void connect(Pop from, Pop to, float chance, float weight,
                 const std::function<float(int, int)>& shape = nullptr,
                 std::vector<std::vector<Synapse>>* lists = nullptr) {
        for (int i = 0; i < size(from); ++i)
            for (int j = 0; j < size(to); ++j) {
                if (from == to && i == j) continue;
                const float scale = shape ? shape(i, j) : 1.0f;
                if (scale == 0 || random() >= chance) continue;
                (*lists)[neuron(from, i)].push_back({static_cast<std::uint16_t>(neuron(to, j)), weight * scale});
            }
    }

    void build() {
        for (int p = 0; p < population_count; ++p) {
            first[p + 1] = first[p] + populations[p].size;
            decay_adaptation[p] = std::exp(-tick / populations[p].adaptation_time);
        }
        const int count = first[population_count];
        owner.resize(count);
        for (int p = 0; p < population_count; ++p)
            for (int i = first[p]; i < first[p + 1]; ++i) owner[i] = p;
        voltage.assign(count, 0);
        for (auto& v : voltage) v = random() * 0.8f;
        excitation.assign(count, 0);
        inhibition.assign(count, 0);
        adaptation.assign(count, 0);
        input.assign(count, 0);
        refractory.assign(count, 0);

        std::vector<std::vector<Synapse>> lists(count);
        auto wire = [&](Pop from, Pop to, float chance, float weight, const std::function<float(int, int)>& shape = nullptr) {
            connect(from, to, chance, weight, shape, &lists);
        };
        auto prey_angle = [&](int tectal) { return FishSenses::prey_angle(tectal / 3); };

        // Vision: a topographic map from retina to tectum, with pooled
        // inhibition so the strongest target wins.
        wire(retina, tectum, 0.8f, 1.1f, [](int r, int t) {
            const int distance = std::abs(r / 2 - t / 3);
            return distance == 0 ? 1.0f : distance == 1 ? 0.35f : 0.0f;
        });
        wire(tectum, tectal_inhibition, 0.5f, 0.5f);
        wire(tectal_inhibition, tectum, 0.5f, -0.5f);
        // Hunger and the hunting state make prey more salient.
        wire(hunger, tectum, 0.1f, 0.25f);
        wire(hunt_command, tectum, 0.2f, 0.3f);

        // Hunting: prey in view plus hunger switch it on; it sustains itself
        // for a while and satiety switches it off.
        wire(tectum, hunt_command, 0.3f, 2.1f, [&](int t, int) { return std::abs(prey_angle(t)) < 2.2f ? 1.0f : 0.0f; });
        wire(strike_zone, hunt_command, 0.5f, 0.6f);
        wire(hunger, hunt_command, 0.25f, 0.2f);
        wire(satiety, hunt_command, 0.4f, -1.0f);
        wire(hunt_command, hunt_command, 1.0f, 0.22f);
        wire(hunt_command, eye_vergence, 1.0f, 0.8f);
        // Food at the surface or on the floor is worth approaching.
        wire(hunt_command, blocked_dorsal, 1.0f, -0.8f);
        wire(hunt_command, blocked_ventral, 1.0f, -0.8f);
        wire(hunt_command, blocked_ahead, 1.0f, -0.8f);

        // Strike when food is right ahead and the fish is hunting.
        wire(strike_zone, strike, 0.7f, 0.9f);
        wire(hunt_command, strike, 0.3f, 0.5f);
        wire(satiety, strike, 0.4f, -0.8f);

        // Escape: looming on one side drives that side's Mauthner cell; each
        // escape recruits inhibition that silences both cells for a while.
        wire(looming, mauthner_dorsal, 1.0f, 0.55f, [&](int l, int) { return FishSenses::loom_angle(l / 2) > 0 ? 1.0f : 0.0f; });
        wire(looming, mauthner_ventral, 1.0f, 0.55f, [&](int l, int) { return FishSenses::loom_angle(l / 2) < 0 ? 1.0f : 0.0f; });
        wire(lateral_line, mauthner_dorsal, 1.0f, 0.25f);
        wire(lateral_line, mauthner_ventral, 1.0f, 0.25f);
        wire(mauthner_dorsal, escape_inhibition, 1.0f, 3.0f);
        wire(mauthner_ventral, escape_inhibition, 1.0f, 3.0f);
        wire(escape_inhibition, mauthner_dorsal, 1.0f, -6.0f);
        wire(escape_inhibition, mauthner_ventral, 1.0f, -6.0f);

        // Hunger and satiety inhibit each other.
        wire(hunger, satiety, 0.3f, -0.4f);
        wire(satiety, hunger, 0.3f, -0.6f);

        // The turning region: self-excitation, crossed inhibition, and slow
        // adaptation make its two halves trade the lead every several seconds.
        // It steers left and right, which a side view cannot show until the
        // turns add up to turning around; see the readout.
        wire(turning_left, turning_left, 1.0f, 0.16f);
        wire(turning_right, turning_right, 1.0f, 0.16f);
        wire(turning_left, turning_inhibition_left, 0.6f, 0.6f);
        wire(turning_right, turning_inhibition_right, 0.6f, 0.6f);
        wire(turning_inhibition_left, turning_right, 0.6f, -1.0f);
        wire(turning_inhibition_right, turning_left, 0.6f, -1.0f);

        // Steering in the screen plane: the tilt sense levels the fish, and
        // blocked directions push it the other way.
        wire(tilt_ventral, steer_dorsal, 1.0f, 0.5f);
        wire(tilt_dorsal, steer_ventral, 1.0f, 0.5f);
        wire(blocked_ventral, steer_dorsal, 1.0f, 0.6f);
        wire(blocked_dorsal, steer_ventral, 1.0f, 0.6f);
        wire(steer_dorsal, steer_ventral, 1.0f, -0.12f);
        wire(steer_ventral, steer_dorsal, 1.0f, -0.12f);
        wire(blocked_ahead, reverse, 1.0f, 0.6f);
        wire(tectum, reverse, 0.3f, 0.7f, [&](int t, int) { return std::abs(prey_angle(t)) > 1.9f ? 1.0f : 0.0f; });
        wire(hunt_command, reverse, 0.2f, 0.2f);

        // Bout timing and vigour.
        wire(bout_generator, bout_generator, 1.0f, 0.6f);
        wire(hunger, bout_generator, 0.2f, 0.25f);
        wire(hunt_command, bout_generator, 0.3f, 0.4f);
        wire(hypoxia, bout_generator, 0.5f, 0.6f);
        wire(lateral_line, bout_generator, 0.5f, 0.4f);
        wire(blocked_ahead, bout_generator, 0.5f, 0.3f);
        wire(satiety, bout_generator, 0.3f, -0.25f);
        wire(hunger, swim_speed, 0.3f, 0.3f);
        wire(hypoxia, swim_speed, 0.5f, 0.6f);
        wire(hunt_command, swim_speed, 0.3f, -0.5f);  // stalking is slow
        wire(satiety, swim_speed, 0.3f, -0.3f);

        synapse_start.assign(count + 1, 0);
        for (int i = 0; i < count; ++i) synapse_start[i + 1] = synapse_start[i] + static_cast<std::uint32_t>(lists[i].size());
        synapses.reserve(synapse_start[count]);
        for (auto& list : lists) synapses.insert(synapses.end(), list.begin(), list.end());
    }

    void set_inputs(const FishSenses& s) {
        auto set = [&](Pop p, float current) {
            for (int i = first[p]; i < first[p + 1]; ++i) input[i] = current;
        };
        for (int p = 0; p < population_count; ++p) set(static_cast<Pop>(p), populations[p].tonic);
        for (int i = 0; i < size(retina); ++i) input[neuron(retina, i)] += 2.2f * s.prey[i / 2];
        for (int i = 0; i < size(looming); ++i) input[neuron(looming, i)] += 3.0f * s.looming[i / 2];
        set(strike_zone, populations[strike_zone].tonic + 3.5f * s.strike_zone);
        set(lateral_line, populations[lateral_line].tonic + 3.0f * s.lateral_line);
        set(tilt_dorsal, populations[tilt_dorsal].tonic + 4.0f * std::max(0.0f, s.pitch - 0.2f));
        set(tilt_ventral, populations[tilt_ventral].tonic + 4.0f * std::max(0.0f, -s.pitch - 0.2f));
        set(blocked_dorsal, populations[blocked_dorsal].tonic + 2.5f * s.blocked_dorsal);
        set(blocked_ventral, populations[blocked_ventral].tonic + 2.5f * s.blocked_ventral);
        set(blocked_ahead, populations[blocked_ahead].tonic + 2.5f * s.blocked_ahead);
        set(hunger, populations[hunger].tonic + 1.3f * s.hunger);
        set(satiety, populations[satiety].tonic + 1.6f * s.fullness);
        set(hypoxia, populations[hypoxia].tonic + 2.0f * s.hypoxia);
    }

    void step() {
        const int count = static_cast<int>(voltage.size());
        const float leak = tick / membrane_time;
        const float jitter = std::sqrt(2 * leak);
        spiked.clear();
        for (int i = 0; i < count; ++i) {
            const Population& p = populations[owner[i]];
            if (refractory[i] > 0) {
                --refractory[i];
                continue;
            }
            const float current = input[i] + excitation[i] + inhibition[i] - adaptation[i];
            voltage[i] += leak * (current - voltage[i]) + p.noise * jitter * gaussian();
            if (voltage[i] >= 1.0f) {
                voltage[i] = 0;
                refractory[i] = refractory_ticks;
                adaptation[i] += p.adaptation;
                spiked.push_back(static_cast<std::uint16_t>(i));
            }
        }
        const float decay_excitation = std::exp(-tick / excitation_time);
        const float decay_inhibition = std::exp(-tick / inhibition_time);
        for (int i = 0; i < count; ++i) {
            excitation[i] *= decay_excitation;
            inhibition[i] *= decay_inhibition;
            adaptation[i] *= decay_adaptation[owner[i]];
        }
        // A synaptic current that decays with time constant tau, starting at
        // weight / tau, delivers a total charge of the weight.
        int counts[population_count] = {};
        for (const auto i : spiked) {
            ++counts[owner[i]];
            for (std::uint32_t k = synapse_start[i]; k < synapse_start[i + 1]; ++k) {
                const auto& synapse = synapses[k];
                if (synapse.weight > 0) excitation[synapse.target] += synapse.weight * membrane_time / excitation_time / 4;
                else inhibition[synapse.target] += synapse.weight * membrane_time / inhibition_time / 4;
            }
        }
        const float keep = std::exp(-tick / rate_time);
        for (int p = 0; p < population_count; ++p)
            rate[p] = rate[p] * keep + counts[p] / static_cast<float>(populations[p].size) / rate_time;
    }

    // Direction read from a topographic population: the activity-weighted
    // sum of its neurons' preferred directions, from per-neuron recent spikes.
    // Background firing points every way and mostly cancels, so the length
    // of the sum says how clearly the tectum sees something.
    std::vector<float> trace;
    float tectal_direction(float& strength) const {
        float x = 0, y = 0;
        for (int i = 0; i < size(tectum); ++i) {
            const float activity = trace[neuron(tectum, i)];
            const float angle = FishSenses::prey_angle(i / 3);
            x += activity * std::cos(angle);
            y += activity * std::sin(angle);
        }
        strength = std::hypot(x, y) / size(tectum);
        return std::atan2(y, x);
    }
    float threat_direction() const {
        float x = 0, y = 0;
        for (int i = 0; i < size(looming); ++i) {
            const float activity = trace[neuron(looming, i)];
            const float angle = FishSenses::loom_angle(i / 2);
            x += activity * std::cos(angle);
            y += activity * std::sin(angle);
        }
        return std::atan2(y, x);
    }
};

FishBrain::FishBrain(std::uint32_t seed) : impl_(std::make_unique<Impl>(seed)) {
    impl_->build();
    impl_->trace.assign(impl_->voltage.size(), 0);
}
FishBrain::~FishBrain() = default;
FishBrain::FishBrain(FishBrain&&) noexcept = default;
FishBrain& FishBrain::operator=(FishBrain&&) noexcept = default;

std::size_t FishBrain::neuron_count() const { return impl_->voltage.size(); }
std::size_t FishBrain::synapse_count() const { return impl_->synapses.size(); }
float FishBrain::vergence() const { return std::clamp((impl_->rate[eye_vergence] - 6.0f) / 34.0f, 0.0f, 1.0f); }
float FishBrain::hunting() const { return std::clamp(impl_->rate[hunt_command] / 40.0f, 0.0f, 1.0f); }

std::vector<FishBrain::Rate> FishBrain::rates() const {
    std::vector<Rate> result;
    for (int p = 0; p < population_count; ++p) result.push_back({populations[p].name, impl_->rate[p]});
    return result;
}

bool FishBrain::advance(const FishSenses& senses, float dt, FishBout& bout) {
    auto& s = *impl_;
    s.set_inputs(senses);
    s.time_left += dt;
    bool started = false;
    const float keep = std::exp(-tick / 0.1f);
    while (s.time_left >= tick) {
        s.time_left -= tick;
        s.step();
        for (auto& t : s.trace) t *= keep;
        for (const auto i : s.spiked) s.trace[i] += 1;
        s.since_bout += tick;
        s.since_escape += tick;
        s.since_strike += tick;
        if (started) continue;

        // Motor readout. A Mauthner spike is an escape, at once.
        bool escape = false;
        for (const auto i : s.spiked)
            if (s.owner[i] == mauthner_dorsal || s.owner[i] == mauthner_ventral) escape = true;
        if (escape && s.since_escape > 0.8f) {
            bout = {BoutKind::escape, wrap(s.threat_direction() + pi), 430.0f};
            s.since_escape = s.since_bout = 0;
            started = true;
            continue;
        }
        if (s.since_bout < 0.18f) continue;
        float strength = 0;
        const float prey = s.tectal_direction(strength);
        if (s.rate[strike] > 50.0f && s.since_strike > 0.4f) {
            bout = {BoutKind::strike, std::clamp(prey, -0.5f, 0.5f), 230.0f};
            s.since_strike = s.since_bout = 0;
            started = true;
            continue;
        }
        if (s.rate[bout_generator] < 45.0f || s.since_bout < 0.3f) continue;
        const float vigour = std::clamp(s.rate[swim_speed] / 60.0f, 0.0f, 1.5f);
        const float noise = 0.12f * s.gaussian();
        // Exploration turns left or right, out of the screen plane. When they
        // add up to more than a right angle, the fish has turned around.
        const float left = s.rate[turning_left], right = s.rate[turning_right];
        s.yaw += 0.6f * (left - right) / (left + right + 10.0f) + 0.05f * s.gaussian();
        const float dorsal = s.rate[steer_dorsal], ventral = s.rate[steer_ventral];
        const float steer = 0.7f * (dorsal - ventral) / (dorsal + ventral + 30.0f) + noise;
        if (s.rate[reverse] > 45.0f || std::abs(s.yaw) > 1.0f) {
            // Turn around toward prey behind, away from a dead end, or at the
            // end of a streak of same-side turns.
            const float side = strength > prey_seen && std::abs(prey) > 1.9f ? prey : std::copysign(pi, s.yaw + steer);
            bout = {BoutKind::reverse, side + noise, 60.0f + 80.0f * vigour};
            s.yaw = 0;
        } else if (s.rate[hunt_command] > 8.0f && strength > prey_seen) {
            // Orient toward the prey the tectum reports, slightly under-turning.
            bout = {BoutKind::hunt, 0.8f * prey + noise * 0.5f, 45.0f + 50.0f * vigour};
            if (std::abs(bout.turn) > 1.75f) s.yaw = 0;
        } else {
            bout = {BoutKind::swim, steer, 70.0f + 160.0f * vigour};
        }
        s.since_bout = 0;
        started = true;
    }
    return started;
}

namespace {
bool check(bool condition, const char* message) {
    if (condition) std::printf("PASS: %s\n", message);
    else std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

struct Tally {
    int bouts = 0, escapes = 0, strikes = 0, hunts = 0, reversals = 0;
    int steered = 0;    // bouts that turn within the screen plane
    float turn_sum = 0;  // over those bouts
    float escape_turn = 0;
    float first_escape = -1;
    float vergence = 0;  // mean over the run
};

Tally simulate(FishBrain& brain, const FishSenses& senses, float seconds) {
    Tally tally;
    const float dt = 1.0f / 120;
    int steps = 0;
    for (float t = 0; t < seconds; t += dt) {
        FishBout bout;
        const bool started = brain.advance(senses, dt, bout);
        tally.vergence += brain.vergence();
        ++steps;
        if (!started) continue;
        ++tally.bouts;
        if (bout.kind == BoutKind::swim || bout.kind == BoutKind::hunt) {
            ++tally.steered;
            tally.turn_sum += bout.turn;
        }
        if (bout.kind == BoutKind::escape) {
            ++tally.escapes;
            if (tally.first_escape < 0) {
                tally.first_escape = t;
                tally.escape_turn = bout.turn;
            }
        }
        if (bout.kind == BoutKind::strike) ++tally.strikes;
        if (bout.kind == BoutKind::hunt) ++tally.hunts;
        if (bout.kind == BoutKind::reverse) ++tally.reversals;
    }
    tally.vergence /= std::max(steps, 1);
    return tally;
}

void print_rates(const FishBrain& brain) {
    for (const auto& rate : brain.rates()) std::printf("    %-30s %6.1f Hz\n", rate.name, rate.hertz);
}
}

bool run_brain_checks(std::uint32_t seed) {
    FishBrain brain(seed);
    std::printf("  fish brain: %zu neurons, %zu synapses\n", brain.neuron_count(), brain.synapse_count());

    // A mildly hungry fish with nothing in view explores in bouts.
    FishSenses calm;
    calm.hunger = 0.3f;
    const Tally explore = simulate(brain, calm, 60);
    std::printf("  exploring: %d bouts in 60 s, %d reversals, %d escapes, vergence %.2f\n", explore.bouts, explore.reversals,
                explore.escapes, explore.vergence);
    if (!check(explore.bouts >= 20 && explore.bouts <= 150, "an idle fish swims in regular bouts")) {
        print_rates(brain);
        return false;
    }
    if (!check(explore.escapes == 0, "nothing in view, nothing to escape from")) return false;

    // Streaks of same-side exploration turns add up to turning around every
    // so often, so an idle fish patrols back and forth.
    const Tally patrol = simulate(brain, calm, 240);
    std::printf("  patrolling: %d turnarounds in 4 minutes, %d bouts, mean pitch turn %.2f\n", patrol.reversals, patrol.bouts,
                patrol.turn_sum / std::max(patrol.steered, 1));
    if (!check(patrol.reversals >= 6 && patrol.reversals * 6 <= patrol.bouts, "an idle fish turns around every so often"))
        return false;

    // A tilted fish levels out.
    FishSenses tilted = calm;
    tilted.pitch = 0.9f;
    const Tally level = simulate(brain, tilted, 20);
    if (!check(level.steered > 3 && level.turn_sum / level.steered < -0.15f, "a fish pitched up turns back toward level")) {
        std::printf("  mean turn %.2f over %d bouts\n", level.turn_sum / std::max(level.steered, 1), level.steered);
        print_rates(brain);
        return false;
    }

    // A hungry fish that sees food above it hunts and turns toward it.
    FishSenses food = calm;
    food.hunger = 0.8f;
    food.prey[11] = 0.8f;
    food.prey[12] = 0.4f;
    const float food_angle = FishSenses::prey_angle(11);
    const Tally hunt = simulate(brain, food, 8);
    std::printf("  food at %.0f°: %d bouts, %d hunting, mean turn %.0f°, vergence %.2f\n", food_angle * 180 / pi, hunt.bouts,
                hunt.hunts, hunt.turn_sum / std::max(hunt.steered, 1) * 180 / pi, hunt.vergence);
    if (!check(hunt.hunts >= 3 && hunt.turn_sum / std::max(hunt.steered, 1) > 0.3f,
               "a hungry fish hunts food it sees and turns toward it")) {
        print_rates(brain);
        return false;
    }
    if (!check(hunt.vergence > 0.25f && explore.vergence < 0.1f, "a hunting fish turns its eyes in")) return false;

    // Food right ahead triggers a strike.
    FishSenses ahead = food;
    ahead.prey = {};
    ahead.prey[7] = ahead.prey[8] = 1.0f;
    ahead.strike_zone = 0.9f;
    const Tally bite = simulate(brain, ahead, 3);
    if (!check(bite.strikes >= 1, "food in front of the mouth draws a strike")) {
        print_rates(brain);
        return false;
    }

    // A full fish ignores food.
    FishBrain sated_brain(seed + 4);
    FishSenses sated = food;
    sated.hunger = 0.05f;
    sated.fullness = 1.0f;
    simulate(sated_brain, sated, 3);
    const Tally ignore = simulate(sated_brain, sated, 10);
    if (!check(ignore.hunts + ignore.strikes <= 1 && sated_brain.hunting() < 0.3f, "a full fish ignores food")) {
        std::printf("  %d hunting bouts, %d strikes, hunting %.2f\n", ignore.hunts, ignore.strikes, sated_brain.hunting());
        print_rates(sated_brain);
        return false;
    }

    // Something looming below triggers an escape within a fraction of a
    // second, turning away from it.
    FishBrain startled(seed + 8);
    simulate(startled, calm, 2);
    FishSenses threat = calm;
    threat.looming[1] = threat.looming[2] = 1.0f;  // below and behind
    const Tally escape = simulate(startled, threat, 1);
    std::printf("  looming below: escape after %.0f ms, turn %.0f°\n", escape.first_escape * 1000, escape.escape_turn * 180 / pi);
    if (!check(escape.escapes >= 1 && escape.first_escape < 0.15f && escape.escape_turn > 0,
               "a looming threat triggers a fast escape away from it")) {
        print_rates(startled);
        return false;
    }

    // Out of water, the fish thrashes.
    FishBrain gasping(seed + 12);
    FishSenses air = calm;
    air.hypoxia = 1.0f;
    const Tally flop = simulate(gasping, air, 20);
    std::printf("  suffocating: %d bouts in 20 s (calm: %d in 60 s)\n", flop.bouts, explore.bouts);
    return check(flop.bouts > explore.bouts / 3 * 1.5f, "a suffocating fish thrashes more often");
}
