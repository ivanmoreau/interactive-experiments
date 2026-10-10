# Fluid Hose

A desktop fluid toy for macOS. Hold **Option** to pour blue water from a nozzle controlled by the mouse. Water lands on window tops, spills around their sides, and pools at the bottom of the usable desktop, above the Dock.

The overlay uses **AppKit + native Metal**, with C++ physics. It is transparent and lets mouse clicks pass through, so you can move and resize the real windows normally while the simulation runs.

## Run

From the workspace root:

```sh
./examples/fluid-hose/build.sh
./examples/fluid-hose/run.sh
```

Building requires macOS 14 or newer and full Xcode with its C++ and Metal tools. This example links only macOS system frameworks and requires no SDL SDK or third-party runtime library. The shared [devenv environment](../../README.md) provides the repository's development and checking tools; it is optional for building and running the hose itself.

Or open `build/Fluid Hose.app` from this example folder in Finder.

- Move the mouse above a window and **hold Option** to pour. Release it to stop.
- When the pointer is over a solid window body, the nozzle snaps to the nearest available edge.
- Use the **💧 menu-bar icon** to pour continuously, pause, clear the water, or quit.
- Choose a monitor through **💧 → Display**. Names, sizes, and a checkmark identify the active display. Your choice is remembered for the next launch.
- **Use Mac tilt** changes gravity using laptop motion; **Reverse tilt left/right** flips the horizontal direction if needed.
- **Ctrl+C** also quits when launched from Terminal.

For continuous pouring from launch:

```sh
./examples/fluid-hose/run.sh --pour
```

The app has no Dock icon or visible window frame. Its menu-bar icon provides the controls.

Switching displays clears the water and moves the overlay, hose, and window collisions to that display. If the selected monitor disconnects, the app falls back to an available display and returns to the saved monitor when it reconnects. The menu also refreshes when monitors are added, rearranged, or resized. The first launch defaults to the main display.

You can also select a display from Terminal:

```sh
./examples/fluid-hose/run.sh --list-displays
./examples/fluid-hose/run.sh --display 2
```

Display numbers match the current menu order; the saved choice uses the monitor's macOS UUID, so a different native display ID on reconnect does not lose the selection. The overlay uses each screen's logical desktop coordinates, including screens to the left of or above the main display.

For gravity controlled by Mac tilt from launch:

```sh
./examples/fluid-hose/run.sh --tilt
```

Tilt is off by default. Its menu status shows whether fresh motion is available. Without fresh data the water uses downward gravity. This mode is intended for the MacBook's own display: the sensor tracks the laptop body, rather than an external monitor.

## How it works

AppKit owns the borderless, non-focusing overlay window and the application event loop. [NSScreen](https://developer.apple.com/documentation/appkit/nsscreen) provides connected displays and their usable frames, excluding the menu bar and Dock. AppKit's bottom-left screen coordinates are converted into the top-left desktop coordinates used by Quartz and the physics. Display notifications and a periodic refresh handle monitor changes; the view updates its Metal drawable size when its backing scale changes.

The native bridge reads normal on-screen windows using [Quartz Window Services](https://developer.apple.com/documentation/coregraphics/cgwindowlistcopywindowinfo%28_%3A_%3A%29?language=_5). Their bounding rectangles become collision obstacles in the selected display's desktop coordinate system. Window bounds refresh eight times per second; the app does not capture window contents or read window titles.

The C++ simulation applies gravity and particle density constraints, using a spatial grid for neighboring particles. Each relaxation iteration caches a particle's neighbors and distances for its pressure pass, with scratch memory reused across steps. Water is retained until you clear it, so it accumulates instead of disappearing. The current capacity is 8,000 particles.

Native Metal renders in two passes: particle splats add into a density texture, then a surface shader merges and shades that field into water. Water opacity is capped at 78% after merging, with a softer edge transition, so the desktop shows through drops and settled pools. The final output uses premultiplied alpha. Three particle buffers allow GPU work to overlap safely with CPU updates.

The native view directly owns its [CAMetalLayer](https://developer.apple.com/documentation/quartzcore/cametallayer). [CAMetalDisplayLink](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink) drives rendering on the AppKit main run loop, including while menus are open. Physics retains its fixed 120 Hz timestep. Signals, menu quit, and the duration limit are also checked by a small timer so shutdown works when no drawable is available. AppKit termination returns through C++ cleanup, which stops the motion helper.

The native window's [ignoresMouseEvents](https://developer.apple.com/documentation/appkit/nswindow/ignoresmouseevents) property keeps it click-through. The window, view, and Metal layer are non-opaque. This replaces the SDL input-shape workaround used by the earlier version.

After rebuilding, quit any running copy through the **💧 menu → Quit Fluid Hose** or **Ctrl+C**, then relaunch. An already running process continues using its original code.

## Scope

This is a 2D prototype on **one selected display at a time**. Windows are solid, axis-aligned bounding rectangles; rounded corners, shadows, pop-up menus, and nonstandard shapes are not modeled. A window that covers the entire canvas is omitted because it would leave no space for the simulation. The desktop floor is the usable screen edge above the Dock.

## Build and checks

```sh
./examples/fluid-hose/build.sh
./examples/fluid-hose/run.sh --physics-check
./examples/fluid-hose/run.sh --display-check
./examples/fluid-hose/run.sh --probe
./examples/fluid-hose/run.sh --smoke-test
./examples/fluid-hose/run.sh --window-check
"./examples/fluid-hose/build/Fluid Hose.app/Contents/MacOS/fluid-hose" --window-check
```

- `--physics-check` requires no graphics access and checks gravity, pooling, collision, hose placement, displacement by moving and overlapping windows, and clearing.
- `--motion-check` checks gravity projection, stale/invalid data, fluid moving toward side walls and the ceiling, and helper transport/shutdown using synthetic data without opening sensors.
- `--display-check` checks saved selection, disconnect fallback, reconnect with changed native IDs, and conversion between AppKit and Quartz coordinates without requiring graphics access.
- Interactive display switching requires a normal desktop session; `--list-displays` enumerates the screens without creating an overlay.
- `--probe` prints desktop bounds and the window collision rectangles without launching the overlay.
- `--smoke-test` checks Metal density accumulation, a translucent merged fluid surface, and transparent background pixels without creating a window.
- `--window-check` briefly shows a native overlay on each connected display. It checks actual WindowServer bounds, Metal drawable dimensions and frame delivery, display switching, menu actions, and transparent, click-through, non-focusing window settings. It requires a live desktop, does not save a display preference, and does not open sensors. Physical monitor disconnection and manual mouse interaction remain interactive checks.
- `--duration 10` runs the overlay for ten seconds and exits.

Requires macOS 14 or newer and full Xcode's C++ and Metal tools to rebuild. The root devenv shell supplies helper tools for the shared checks; the hose build does not read `SDL3_ROOT` or `SDL3_LIBRARY`.

On October 9, 2026, `devenv test` rebuilt both examples and passed the physics, display selection, helper, signing, and bundled-dependency checks after the AppKit migration. The hose also built outside devenv with the SDL environment variables removed. Native Metal pixel readback passed for both the CLI executable and packaged app. Both executables' `--window-check` moved the overlay across the built-in Retina display and two LG UltraFine monitors, verified actual window bounds and 2× drawable dimensions, rendered frames, and dispatched Display and Quit menu actions. Duration, SIGINT, and SIGTERM shutdown checks passed, as did a packaged launch through LaunchServices/Finder with a duration limit. The packaged hose links only macOS system libraries. GPU timing remains unmeasured.

## Motion sensor probe

The M1 Pro MacBook exposes SPU accelerometer and gyroscope devices. To check live readings from a normal Terminal:

```sh
./examples/fluid-hose/tools/probe-motion.sh --duration 10
```

This rebuilds `build/probe-motion`, so the previously used binary path also works. The probe opens the IMU non-exclusively and enables its driver reporting, power, and report interval, following the [SPU sensor reader's activation sequence](https://github.com/olvvier/apple-silicon-accelerometer/blob/main/macimu/_spu.py). It borrows an existing reporting session when samples are already arriving. When it enables reporting itself, it touches only the accelerometer and gyroscope, restores prior published settings when it exits, and defaults unpublished power/reporting commands back to off.

The probe prints live axes, report counts, and sample age. `NO DATA` means no sample was decoded; it does not mean measured acceleration is zero. Zero gyroscope values can be normal while stationary. The acceleration magnitude is nominally around one g at rest, with its components changing as you tilt. Wake-up errors are printed individually; opening a device does not establish that reporting is enabled. The protocol is undocumented and its Q16 scale is nominal.

`--self-test` checks signed sample decoding, short reports, and callback errors without opening hardware. Device opening is blocked in the development sandbox; the user confirmed fresh live readings near one g after driver activation was added.

To observe existing reporting without enabling any driver settings:

```sh
./examples/fluid-hose/tools/probe-motion.sh --observe-only --duration 3
```

## Tilt gravity and sensor lifetime

The app uses a short-lived reader process only while **Use Mac tilt** is enabled. The reader sends motion packets over local pipes. Closing the app or disabling tilt closes its control pipe; the reader detects that closure and restores its sensor settings. It uses a separate process group so it can also clean up after the main app is force-killed. No startup service is installed.

These are runtime driver settings, not settings automatically limited to one process. The standalone probe restores them on timeout, Ctrl+C, or SIGTERM. Killing the reader itself with SIGKILL, or a driver refusing restoration, can leave changes active. Restoring a prior active state can also legitimately leave sensors reporting for another consumer. After the user's completed probe, both IMU report intervals were observed back at their original value of zero.

Gravity estimation combines gyro rotation with smoothed acceleration, rejecting large bumps. It projects the resulting 3D direction into the display plane. The reader optionally obtains lid angle without changing that sensor's reporting or power settings; when the lid angle is unavailable, projection assumes a 110° opening. The projection keeps its physical magnitude, so a nearly horizontal display has less gravity along its surface.

Checks from the workspace root:

```sh
./examples/fluid-hose/run.sh --motion-check
python3 examples/fluid-hose/tools/check-motion-lifetime.py
```

The lifecycle check force-kills a test owner and verifies the mock reader stops when its control pipe closes. It does not access sensors. Physics, projection, transport, packaged executable, lifecycle, and signing checks passed; live tilt rendering still needs the normal desktop session.

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

- [src/main.cpp](src/main.cpp): command-line options and C++ fixed-step simulation callbacks.
- [src/fluid_sim.cpp](src/fluid_sim.cpp): gravity, density relaxation, collision, and nozzle placement.
- [src/desktop.mm](src/desktop.mm): AppKit application lifecycle and display link, native overlay window, display and window metadata, Option-key state, menu-bar controls, and Metal rendering.
- [src/fluid.metal](src/fluid.metal): density splats, water surface shading, and the nozzle.
- [src/fluid_types.h](src/fluid_types.h): shared CPU/GPU data layouts.
- [tools/probe_motion.cpp](tools/probe_motion.cpp): SPU motion sensor activation and live sample diagnostics.
- [src/motion.cpp](src/motion.cpp): gravity estimation, display projection, and ownership of the reader process.
- [src/displays.cpp](src/displays.cpp): selecting a connected display and recovering the saved monitor after reconnect.
