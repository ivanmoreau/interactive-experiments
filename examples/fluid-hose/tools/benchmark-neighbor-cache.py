#!/usr/bin/env python3
"""Check the integrated solver against the original and benchmark either one."""

from pathlib import Path
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
repository = project.parents[1]
source_dir = project / "src"
output_dir = project / "build" / "performance"
output_dir.mkdir(parents=True, exist_ok=True)
baseline_commit = "ed0f543f20843db450074c2506a6213b531a264b"
arguments = sys.argv[1:]
use_baseline = "--baseline" in arguments
arguments = [argument for argument in arguments if argument != "--baseline"]
if "--help" in arguments:
    print("Usage: benchmark-neighbor-cache.py [--baseline] [--quick] [--profile]\n"
          "Checks integrated physics against the original commit, then benchmarks the app solver.\n"
          "--baseline benchmarks the original solver instead. Requires the repository's Git history.")
    sys.exit(0)
if (repository / ".git").exists():
    git = ["git", "-C", str(repository)]
else:
    git = ["git", f"--git-dir={repository / 'build' / 'repository.git'}",
           f"--work-tree={repository}"]


def original(filename):
    path = f"examples/fluid-hose/src/{filename}"
    return subprocess.check_output(git + ["show", f"{baseline_commit}:{path}"], text=True)


def rename(text):
    return text.replace("FluidSimulation", "BaselineSimulation").replace(
        "run_physics_checks", "run_baseline_checks")


(output_dir / "baseline_sim.h").write_text(rename(original("fluid_sim.h")))
(output_dir / "baseline_sim.cpp").write_text(rename(original("fluid_sim.cpp")).replace(
    '#include "fluid_sim.h"', '#include "baseline_sim.h"'))
driver = (project / "tools" / "benchmark_physics.cpp").read_text()
(output_dir / "baseline_benchmark.cpp").write_text(rename(driver).replace(
    '../src/fluid_sim.h', 'baseline_sim.h'))
(output_dir / "equivalence.cpp").write_text(r'''#include "fluid_sim.h"
#include "baseline_sim.h"
#include <cstdio>
#include <cstring>

bool same(const BaselineSimulation& original, const FluidSimulation& current) {
    return original.size() == current.size() &&
           (original.size() == 0 ||
            std::memcmp(original.vertices().data(), current.vertices().data(),
                        original.size() * sizeof(FluidVertex)) == 0);
}

int main() {
    if (!run_physics_checks() || !run_baseline_checks()) return 1;
    BaselineSimulation original(1440, 900);
    FluidSimulation current(1440, 900);
    std::vector<WindowRect> windows{{500, 220, 440, 280}};
    for (int tick = 0; tick < 1800; ++tick) {
        if (tick == 1320) windows = {{600, 450, 300, 448}, {700, 420, 350, 420}};
        const auto nozzle = original.place_hose({720, 60}, windows);
        const auto other_nozzle = current.place_hose({720, 60}, windows);
        if (nozzle.x != other_nozzle.x || nozzle.y != other_nozzle.y) return 1;
        original.step(1.0f / 120, windows, tick < 1200, nozzle);
        current.step(1.0f / 120, windows, tick < 1200, nozzle);
        if (!same(original, current)) {
            std::fprintf(stderr, "FAIL: trajectory changed at tick %d\n", tick);
            return 1;
        }
    }
    std::printf("PASS: %zu particles have bit-identical positions across 1800 ticks, including moving overlapping windows.\n", original.size());
    original.clear();
    current.clear();
    if (!same(original, current) || !current.vertices().empty()) return 1;
    for (int tick = 0; tick < 240; ++tick) {
        original.step(1.0f / 120, {}, true, {120, 60});
        current.step(1.0f / 120, {}, true, {120, 60});
        if (!same(original, current)) return 1;
    }
    std::puts("PASS: clearing and pouring again preserves the original trajectories.");
    return 0;
}
''')

compiler = ["xcrun", "clang++", "-std=c++17", "-O2", "-g", "-Wall", "-Wextra",
            "-Wpedantic", "-Werror", "-mmacosx-version-min=14.0",
            "-I", str(source_dir), "-I", str(output_dir)]
if use_baseline:
    inputs = [str(output_dir / "baseline_benchmark.cpp"), str(output_dir / "baseline_sim.cpp")]
    executable = output_dir / "baseline-benchmark"
else:
    inputs = [str(project / "tools" / "benchmark_physics.cpp"), str(source_dir / "fluid_sim.cpp")]
    executable = output_dir / "integrated-benchmark"
subprocess.run(compiler + inputs + ["-o", str(executable)], check=True)
subprocess.run(compiler + [str(output_dir / "equivalence.cpp"),
                          str(source_dir / "fluid_sim.cpp"), str(output_dir / "baseline_sim.cpp"),
                          "-o", str(output_dir / "equivalence-check")], check=True)
subprocess.run([str(output_dir / "equivalence-check")], check=True, stdout=sys.stderr)
subprocess.run([str(executable), *arguments], check=True)
