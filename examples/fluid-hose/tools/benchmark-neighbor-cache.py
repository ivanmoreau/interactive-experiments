#!/usr/bin/env python3
"""Build and measure a neighbor-cache experiment without editing app sources."""

from pathlib import Path
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
source_dir = project / "src"
output_dir = project / "build" / "performance"
output_dir.mkdir(parents=True, exist_ok=True)
source = (source_dir / "fluid_sim.cpp").read_text()
header = (source_dir / "fluid_sim.h").read_text()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit("Simulation source changed; review the experiment before running it.")
    return text.replace(old, new)


candidate = replace_once(source, """    correction_.assign(particles_.size(), {0, 0});
    for (std::size_t i = 0; i < particles_.size(); ++i) {""", """    correction_.assign(particles_.size(), {0, 0});
    struct Neighbor { int index; FluidPoint delta; float distance; float q; };
    std::vector<Neighbor> cached;
    cached.reserve(128);
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        cached.clear();""")
candidate = replace_once(candidate, """        neighbors([&](int, FluidPoint, float, float q) {
            density += q * q;
            near_density += q * q * q;
        });""", """        neighbors([&](int j, FluidPoint delta, float distance, float q) {
            density += q * q;
            near_density += q * q * q;
            cached.push_back({j, delta, distance, q});
        });""")
candidate = replace_once(candidate, """        neighbors([&](int j, FluidPoint delta, float distance, float q) {
            const float amount""", """        for (const auto& neighbor : cached) {
            const int j = neighbor.index;
            const auto delta = neighbor.delta;
            const float distance = neighbor.distance;
            const float q = neighbor.q;
            const float amount""")
candidate = replace_once(candidate, """            correction_[i].y -= shift.y;
        });""", """            correction_[i].y -= shift.y;
        }""")


def rename(text):
    return text.replace("FluidSimulation", "CandidateSimulation").replace(
        "run_physics_checks", "run_candidate_checks")


(output_dir / "candidate_sim.h").write_text(rename(header))
(output_dir / "candidate.cpp").write_text(rename(candidate).replace(
    '#include "fluid_sim.h"', '#include "candidate_sim.h"'))
driver = (project / "tools" / "benchmark_physics.cpp").read_text()
(output_dir / "candidate_benchmark.cpp").write_text(rename(driver).replace(
    '../src/fluid_sim.h', 'candidate_sim.h'))
(output_dir / "equivalence.cpp").write_text(r'''#include "fluid_sim.h"
#include "candidate_sim.h"
#include <cstdio>
#include <cstring>

int main() {
    if (!run_candidate_checks()) return 1;
    FluidSimulation original(1440, 900);
    CandidateSimulation candidate(1440, 900);
    std::vector<WindowRect> windows{{500, 220, 440, 280}};
    for (int tick = 0; tick < 1800; ++tick) {
        if (tick == 1320) windows = {{600, 450, 300, 448}, {700, 420, 350, 420}};
        const auto nozzle = original.place_hose({720, 60}, windows);
        const auto other_nozzle = candidate.place_hose({720, 60}, windows);
        if (nozzle.x != other_nozzle.x || nozzle.y != other_nozzle.y) return 1;
        original.step(1.0f / 120, windows, tick < 1200, nozzle);
        candidate.step(1.0f / 120, windows, tick < 1200, nozzle);
        if (original.size() != candidate.size() ||
            std::memcmp(original.vertices().data(), candidate.vertices().data(),
                        original.size() * sizeof(FluidVertex)) != 0) {
            std::fprintf(stderr, "FAIL: trajectory changed at tick %d\n", tick);
            return 1;
        }
    }
    std::printf("PASS: %zu particles have bit-identical positions across 1800 ticks, including moving overlapping windows.\n", original.size());
    return 0;
}
''')

compiler = ["xcrun", "clang++", "-std=c++17", "-O2", "-g", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-mmacosx-version-min=14.0",
            "-I", str(source_dir), "-I", str(output_dir)]
subprocess.run(compiler + [str(output_dir / "candidate_benchmark.cpp"),
                          str(output_dir / "candidate.cpp"),
                          "-o", str(output_dir / "candidate-benchmark")], check=True)
subprocess.run(compiler + [str(output_dir / "equivalence.cpp"),
                          str(source_dir / "fluid_sim.cpp"), str(output_dir / "candidate.cpp"),
                          "-o", str(output_dir / "equivalence-check")], check=True)
subprocess.run([str(output_dir / "equivalence-check")], check=True, stdout=sys.stderr)
subprocess.run([str(output_dir / "candidate-benchmark"), *sys.argv[1:]], check=True)
