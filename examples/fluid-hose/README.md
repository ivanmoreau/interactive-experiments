# Fluid Hose

A desktop fluid toy for macOS. Hold **Option** to pour blue water from a nozzle controlled by the mouse. Water lands on window tops, spills around their sides, and pools at the bottom of the usable desktop, above the Dock.

The overlay is transparent and lets mouse clicks pass through. You can move and resize the real windows normally while the simulation runs.

## Run

From the workspace root:

```sh
./examples/fluid-hose/run.sh
```

Or open `build/Fluid Hose.app` from this example folder in Finder.

- Move the mouse above a window and **hold Option** to pour. Release it to stop.
- When the pointer is over a solid window body, the nozzle snaps to the nearest available edge.
- Use the **💧 menu-bar icon** to pour continuously, pause, clear the water, or quit.
- **Ctrl+C** also quits when launched from Terminal.

For continuous pouring from launch:

```sh
./examples/fluid-hose/run.sh --pour
```

The app has no Dock icon or visible window frame. Its menu-bar icon provides the controls.

## How it works

SDL handles the overlay window and event loop. The native macOS bridge reads normal on-screen windows using [Quartz Window Services](https://developer.apple.com/documentation/coregraphics/cgwindowlistcopywindowinfo%28_%3A_%3A%29?language=_5). Their bounding rectangles become collision obstacles in the primary display's desktop coordinate system. Bounds refresh eight times per second; the app does not capture window contents or read window titles.

The C++ simulation applies gravity and particle density constraints, using a spatial grid for neighboring particles. Each relaxation iteration caches a particle's neighbors and distances for its pressure pass, with scratch memory reused across steps. Water is retained until you clear it, so it accumulates instead of disappearing. The current capacity is 8,000 particles.

Native Metal renders in two passes: particle splats add into a density texture, then a surface shader merges and shades that field into water. The final output uses premultiplied alpha so the desktop remains visible through empty areas. Three particle buffers allow GPU work to overlap safely with CPU updates.

SDL GPU rejects transparent windows, so this example uses SDL's supported native Metal-view API, following the working floating triangle's approach.

On macOS, a fully transparent [SDL window shape](https://wiki.libsdl.org/SDL3/SDL_SetWindowShape) supplies the input mask while the native Metal layer supplies the visible water. SDL updates mouse interception from this shape on pointer movement, so setting the native window's `ignoresMouseEvents` flag alone is insufficient.

After rebuilding, quit any running copy through the **💧 menu → Quit Fluid Hose** or **Ctrl+C**, then relaunch. An already running process continues using its original code.

## Scope

This is a 2D prototype on the **primary display**. Windows are solid, axis-aligned bounding rectangles; rounded corners, shadows, pop-up menus, and nonstandard shapes are not modeled. A window that covers the entire canvas is omitted because it would leave no space for the simulation. The desktop floor is the usable screen edge above the Dock.

## Build and checks

```sh
./examples/fluid-hose/build.sh
./examples/fluid-hose/run.sh --physics-check
./examples/fluid-hose/run.sh --probe
./examples/fluid-hose/run.sh --smoke-test
```

- `--physics-check` requires no graphics access and checks gravity, pooling, collision, hose placement, displacement by moving and overlapping windows, and clearing.
- `--probe` prints desktop bounds and the window collision rectangles without launching the overlay.
- `--smoke-test` checks Metal density accumulation, a merged fluid surface, and transparent background pixels without creating a window.
- `--duration 10` runs the overlay for ten seconds and exits.

Requires macOS 14 or newer and Xcode's C++ and Metal tools to rebuild. The SDL3 SDK and library are cached under `.deps/SDL3`; `tools/setup-sdl.sh` installs the official SDK locally if needed.

Compilation, physics checks, CLI checks, app signing, and property-list validation passed. A separate flow check confirmed water spilling below a window. The fluid overlay and mouse pass-through have also been confirmed in a normal desktop session. GPU pixel readback and timing require that session because the development sandbox cannot access a Metal device or the display.

## Performance assessment — October 8, 2026

Neighbor caching is integrated into the app and the rebuilt app bundle. It reduced CPU physics step time by approximately 38–43% in these benchmarks, with the same particle trajectories as the original solver. Scratch memory is retained across particles, relaxation iterations, and simulation steps.

The benchmark uses a 1440 × 900 logical canvas, deterministic pouring to seed particles, and 120 settling steps before timing. Each case measures three runs of 180 steps from the same snapshot, with pouring stopped. Setup, snapshot copying, SDL, window enumeration, and Metal rendering are excluded. Apple clang 17 compiles with the app's `-O2` setting on the project's M1 Pro Mac. Times below are milliseconds per step with one window obstacle.

| Particles | Original median | Original p95 | Integrated median | Integrated p95 | Median time reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2,000 | 2.90 | 4.37 | 1.66 | 1.79 | 43% |
| 4,000 | 7.32 | 11.76 | 4.31 | 4.58 | 41% |
| 8,000 | 16.40 | 17.95 | 10.09 | 11.36 | 38% |

A 120 Hz simulation has **8.33 ms per step**, with additional CPU work needed for input and rendering submission. At 8,000 particles, the loop's catch-up steps can still consume substantial time before presenting a frame. Further improvement is needed to meet that budget at full capacity. Increasing obstacles from one to twelve changed the original 8,000-particle median by approximately 3% in this scene.

The original solver traversed the same neighbors twice in each density-relaxation iteration, recomputing distances both times. Particle positions are fixed until all corrections have been accumulated, so the app now computes and caches those results once per particle. The integrated solver passed the existing physics checks and produced bit-identical particle vertices throughout a 1,800-step, 2,000-particle comparison, including displacement by moving, overlapping windows. Clearing and pouring again also preserves the original trajectories.

The next changes to consider, in priority order:

1. **Reduce idle rendering.** The current loop renders both passes even with no water or when paused. Retain the last frame when the visible state is unchanged, while continuing to poll the global pointer, Option key, menu controls, and window bounds.
2. **Measure and reduce GPU work.** Both passes currently use the drawable's full pixel dimensions. On a 2× Retina display, a density field at logical resolution would contain 75% fewer pixels while the final output could remain native resolution. The surface shader can also skip its four gradient samples and lighting on empty pixels, provided it preserves the nozzle. These are unmeasured candidates; density resolution can affect edge detail and lighting. Record [GPU start](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime) and [end](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpuendtime) timestamps in command-buffer completion handlers before choosing a rendering change.
3. **For a larger solver improvement, process unordered neighbor pairs once or redesign for parallel updates.** Current corrections write into other particles, so simply threading the loop would introduce data races. A 60 Hz physics preset would reduce tick count, but the pressure terms depend on `dt²` and require behavior and stability checks.

Reproduce the CPU measurements from the workspace root:

```sh
./examples/fluid-hose/tools/benchmark-physics.sh
./examples/fluid-hose/tools/benchmark-neighbor-cache.py
./examples/fluid-hose/tools/benchmark-neighbor-cache.py --baseline
```

Both tools accept `--quick` for a 2,000-particle check. `--profile` adds a 15-second steady workload for sampling in a normal desktop session. The neighbor-cache tool loads the original solver from commit `ed0f543`, generates comparison files under `build/performance`, and verifies equivalence before benchmarking the integrated solver. `--baseline` benchmarks the original instead. This comparison tool requires the repository's Git history; the plain physics benchmark requires no Git or SDL dependencies.

Raw results: [original solver](benchmarks/2026-10-08-physics-baseline.csv), [neighbor-cache prototype](benchmarks/2026-10-08-physics-cached-neighbors.csv), and [integrated solver](benchmarks/2026-10-08-physics-integrated.csv). These are standalone wall-clock timings from the development sandbox, with scheduling outliers and no CPU affinity or thermal isolation; they are not live app frame rates. The CSV budget percentage is mean step time divided by 8.33 ms, rather than measured OS CPU utilization. GPU timings and the cost of Quartz window enumeration remain unmeasured. External process sampling was denied by the sandbox.

## Code

- [src/main.cpp](src/main.cpp): fixed-step simulation loop and desktop overlay setup.
- [src/fluid_sim.cpp](src/fluid_sim.cpp): gravity, density relaxation, collision, and nozzle placement.
- [src/desktop.mm](src/desktop.mm): window metadata, Option-key state, click-through window, menu-bar controls, and native Metal rendering.
- [src/fluid.metal](src/fluid.metal): density splats, water surface shading, and the nozzle.
- [src/fluid_types.h](src/fluid_types.h): shared CPU/GPU data layouts.

SDL's license is in [SDL3-LICENSE.txt](SDL3-LICENSE.txt) and the app bundle.
