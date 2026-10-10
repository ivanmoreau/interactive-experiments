#include "pond_store.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
constexpr int format_version = 1;

bool replace_file(const std::filesystem::path& path, const std::string& contents) {
    const auto temporary = path.string() + ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!out) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

void write_genome(std::ostream& out, const FishGenome& g) {
    out << g.boldness << ' ' << g.appetite << ' ' << g.curiosity << ' ' << g.size << ' ' << g.metabolism << ' ' << g.hue << ' '
        << g.stripes;
}

bool read_genome(std::istream& in, FishGenome& g) {
    in >> g.boldness >> g.appetite >> g.curiosity >> g.size >> g.metabolism >> g.hue >> g.stripes;
    if (!in) return false;
    g = g.clamped();
    return true;
}
}

std::filesystem::path default_pond_directory() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / "Library" / "Application Support" / "Fluid Hose";
}

bool save_pond(const std::filesystem::path& dir, const PondState& state, const std::vector<FluidParticle>& water) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) return false;
    std::ostringstream text;
    text.precision(7);
    text << "fluid-hose-pond " << format_version << '\n';
    text << "size " << state.width << ' ' << state.height << '\n';
    text << "next-fish " << state.next_fish_number << '\n';
    for (const auto& duck : state.ducks)
        text << "duck " << duck.position.x << ' ' << duck.position.y << ' ' << duck.facing << ' ' << duck.variant << '\n';
    for (const auto& fish : state.fish) {
        const auto& b = fish.birth;
        text << "fish " << fish.number << ' ' << (b.female ? 'f' : 'm') << ' ' << b.growth << ' ' << b.generation << ' ' << b.mother
             << ' ' << b.father << ' ' << b.brain_seed << ' ' << fish.position.x << ' ' << fish.position.y << ' ' << fish.energy << ' '
             << fish.fullness << ' ' << fish.oxygen << ' ' << (fish.alive ? 1 : 0) << ' ' << fish.meals << ' ';
        write_genome(text, b.genome);
        text << '\n';
    }
    // Water: positions and velocities, four floats per particle.
    std::string binary;
    const std::uint32_t count = static_cast<std::uint32_t>(water.size());
    binary.append(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& p : water) {
        const float values[4] = {p.x, p.y, p.vx, p.vy};
        binary.append(reinterpret_cast<const char*>(values), sizeof(values));
    }
    return replace_file(dir / "water.bin", binary) && replace_file(dir / "pond.txt", text.str());
}

bool load_pond(const std::filesystem::path& dir, PondState& state, std::vector<FluidParticle>& water) {
    std::ifstream in(dir / "pond.txt");
    if (!in) return false;
    std::string word;
    int version = 0;
    if (!(in >> word >> version) || word != "fluid-hose-pond" || version != format_version) return false;
    PondState loaded;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::istringstream entry(line);
        if (!(entry >> word)) continue;
        if (word == "size") entry >> loaded.width >> loaded.height;
        else if (word == "next-fish") entry >> loaded.next_fish_number;
        else if (word == "duck") {
            PondState::Duck duck{};
            if (entry >> duck.position.x >> duck.position.y >> duck.facing >> duck.variant) loaded.ducks.push_back(duck);
        } else if (word == "fish") {
            PondState::Fish fish{};
            char sex = 'm';
            int alive = 1;
            auto& b = fish.birth;
            entry >> fish.number >> sex >> b.growth >> b.generation >> b.mother >> b.father >> b.brain_seed >> fish.position.x >>
                fish.position.y >> fish.energy >> fish.fullness >> fish.oxygen >> alive >> fish.meals;
            if (!entry || !read_genome(entry, b.genome)) continue;
            b.female = sex == 'f';
            fish.alive = alive != 0;
            loaded.fish.push_back(fish);
        }
        if (!entry && word != "fish") return false;
    }
    if (!(loaded.width > 0 && loaded.height > 0)) return false;

    water.clear();
    std::ifstream binary(dir / "water.bin", std::ios::binary);
    std::uint32_t count = 0;
    if (binary && binary.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        for (std::uint32_t i = 0; i < count; ++i) {
            float values[4];
            if (!binary.read(reinterpret_cast<char*>(values), sizeof(values))) break;
            water.push_back({values[0], values[1], values[2], values[3], 0, 0, 0, 0});
        }
    }
    state = std::move(loaded);
    return true;
}

bool run_pond_store_checks(const std::filesystem::path& scratch) {
    PondState state;
    state.width = 800;
    state.height = 600;
    state.next_fish_number = 9;
    state.ducks.push_back({{100, 200}, -1.1f, 2});
    PondState::Fish fish{};
    fish.number = 7;
    fish.birth.female = true;
    fish.birth.growth = 0.6f;
    fish.birth.generation = 3;
    fish.birth.mother = 2;
    fish.birth.father = 5;
    fish.birth.brain_seed = 12345;
    fish.birth.genome.stripes = 0.1f;
    fish.position = {300, 400};
    fish.energy = 0.42f;
    fish.alive = true;
    fish.meals = 11;
    state.fish.push_back(fish);
    const std::vector<FluidParticle> water{{10, 20, 1, 2, 0, 0, 0, 0}, {30, 40, -3, 4, 0, 0, 0, 0}};
    if (!save_pond(scratch, state, water)) {
        std::fprintf(stderr, "FAIL: a pond can be saved\n");
        return false;
    }
    PondState loaded;
    std::vector<FluidParticle> loaded_water;
    const bool ok = load_pond(scratch, loaded, loaded_water) && loaded.fish.size() == 1 && loaded.ducks.size() == 1 &&
                    loaded.next_fish_number == 9 && loaded.fish[0].number == 7 && loaded.fish[0].birth.female &&
                    loaded.fish[0].birth.generation == 3 && loaded.fish[0].birth.brain_seed == 12345 &&
                    std::abs(loaded.fish[0].birth.genome.stripes - 0.1f) < 1e-5f && loaded.fish[0].meals == 11 &&
                    loaded_water.size() == 2 && loaded_water[1].vy == 4 && loaded.ducks[0].facing < 0;
    if (ok) std::printf("PASS: a saved pond loads back the same\n");
    else std::fprintf(stderr, "FAIL: a saved pond loads back the same\n");
    return ok;
}
