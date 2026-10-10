#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// Heritable differences between fish. Behavioural genes are around 1 for an
// average fish; the rest are in their natural units.
struct FishGenome {
    float boldness = 1;    // higher: escapes less readily
    float appetite = 1;    // higher: hunger drives hunting harder
    float curiosity = 1;   // higher: swims and explores more
    float size = 1;        // adult length relative to the standard 50 points
    float metabolism = 1;  // how fast energy burns
    float hue = 0;         // -1 to 1: shifts the stripes from teal through blue to violet
    float stripes = 0.6f;  // 0 to 1: low values break the stripes into spots, like the leopard mutant

    // A small deterministic generator, so a pond can be replayed in checks.
    struct Random {
        std::uint32_t state;
        float uniform() {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<float>(state & 0xffffffu) / 16777216.0f;
        }
        float normal() { return (uniform() + uniform() + uniform() - 1.5f) * 2.0f; }
    };

    // A founder fish, as bought: varied around the standard type.
    static FishGenome founder(Random& random) {
        FishGenome g;
        g.boldness = 1 + 0.15f * random.normal();
        g.appetite = 1 + 0.15f * random.normal();
        g.curiosity = 1 + 0.15f * random.normal();
        g.size = 1 + 0.06f * random.normal();
        g.metabolism = 1 + 0.08f * random.normal();
        g.hue = 0.25f * random.normal();
        g.stripes = 0.6f + 0.12f * random.normal();
        return g.clamped();
    }

    // A child: each gene comes from somewhere between the parents' values,
    // plus a small mutation.
    static FishGenome child(const FishGenome& mother, const FishGenome& father, Random& random) {
        auto mix = [&](float a, float b, float mutation) {
            const float t = random.uniform();
            return a + (b - a) * t + mutation * random.normal();
        };
        FishGenome g;
        g.boldness = mix(mother.boldness, father.boldness, 0.05f);
        g.appetite = mix(mother.appetite, father.appetite, 0.05f);
        g.curiosity = mix(mother.curiosity, father.curiosity, 0.05f);
        g.size = mix(mother.size, father.size, 0.02f);
        g.metabolism = mix(mother.metabolism, father.metabolism, 0.03f);
        g.hue = mix(mother.hue, father.hue, 0.06f);
        g.stripes = mix(mother.stripes, father.stripes, 0.05f);
        return g.clamped();
    }

    FishGenome clamped() const {
        FishGenome g = *this;
        g.boldness = std::clamp(g.boldness, 0.5f, 1.6f);
        g.appetite = std::clamp(g.appetite, 0.5f, 1.6f);
        g.curiosity = std::clamp(g.curiosity, 0.5f, 1.6f);
        g.size = std::clamp(g.size, 0.8f, 1.25f);
        g.metabolism = std::clamp(g.metabolism, 0.7f, 1.3f);
        g.hue = std::clamp(g.hue, -1.0f, 1.0f);
        g.stripes = std::clamp(g.stripes, 0.0f, 1.0f);
        return g;
    }
};

// A short name for a fish, made from its number.
inline std::string fish_name(std::uint32_t number) {
    static const char* const starts[] = {"B", "D", "F", "G", "K", "L", "M", "N", "P", "R", "S", "T", "V", "Z", "Bl", "Fl", "Gl", "Sp"};
    static const char* const vowels[] = {"a", "e", "i", "o", "u", "ai", "ee", "oo"};
    static const char* const ends[] = {"", "n", "x", "ly", "bo", "pi", "ra", "zy", "mo", "ble"};
    std::uint32_t h = number * 2654435761u + 0x9e3779b9u;
    h ^= h >> 15;
    std::string name = starts[h % 18];
    name += vowels[(h / 18) % 8];
    name += ends[(h / 144) % 10];
    return name;
}
