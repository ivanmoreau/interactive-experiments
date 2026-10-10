# Fluid Hose

A desktop fluid toy for macOS. Hold **Option** to pour water from a nozzle controlled by the mouse. Water lands on window tops, drips off their edges, and pools at the bottom of the usable desktop, above the Dock. Drag a window through the water and it sloshes; water resting on a window rides along with it.

The overlay uses **AppKit + native Metal**. The fluid is a position-based fluid solver running in Metal compute shaders, and windows are tracked live through WindowServer notifications. The overlay is transparent and lets mouse clicks pass through, so you can move and resize the real windows normally while the simulation runs.

## Run

From the workspace root:

```sh
./examples/fluid-hose/build.sh
./examples/fluid-hose/run.sh
```

Building requires macOS 14 or newer and Apple's C++ compiler. With full Xcode the build compiles the shaders into `fluid.metallib`; with only the Command Line Tools it bundles the shader source instead, and Metal compiles it when the app launches. This example links only macOS system frameworks and requires no SDL SDK or third-party runtime library. The shared [devenv environment](../../README.md) provides the repository's development and checking tools; it is optional for building and running the hose itself.

Or open `build/Fluid Hose.app` from this example folder in Finder.

- Move the mouse above a window and **hold Option** to pour. Release it to stop.
- When the pointer is over a solid window body, the nozzle snaps to the nearest available edge.
- Use the **💧 menu-bar icon** to pour continuously, pause, clear the water, or quit.
- Choose a monitor through **💧 → Display**. Names, sizes, and a checkmark identify the active display. Your choice is remembered for the next launch.
- **💧 → Liquid** sets how the water moves: **Water** is runny and splashy, **Thick** flows slowly, and **Goo** clings and stretches. It applies to the water already on screen.
- **💧 → Particle size** trades detail for volume. **Fine** gives the most detailed streams and drops; **Coarse** holds the most water. Changing the size clears the water.
- **💧 → Refract the desktop** bends the desktop seen through the water, like a lens. It needs the Screen Recording permission; see [Refraction](#refraction).
- **Use Mac tilt** changes gravity using laptop motion; **Reverse tilt left/right** flips the horizontal direction if needed.
- **Ctrl+C** also quits when launched from Terminal.

For continuous pouring from launch:

```sh
./examples/fluid-hose/run.sh --pour
```

The liquid, particle size, and refraction choices are remembered. The defaults are fine water without refraction. They can also be chosen for a single launch:

```sh
./examples/fluid-hose/run.sh --liquid thick --size coarse
./examples/fluid-hose/run.sh --refraction
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

### Window tracking

[src/window_tracker.mm](src/window_tracker.mm) follows the approach of [JankyBorders](https://github.com/FelixKratz/JankyBorders), which draws borders that track windows in real time. JankyBorders is GPL-3.0; it served only as a reference for which WindowServer calls to use, and no code was copied. The app subscribes to the private SkyLight notifications for window move (806) and resize (807), and for windows being created, destroyed, hidden, reordered, or changing level or space. Move and resize events arrive on the main thread as the window moves, and only that window's bounds are re-read with `SLSGetWindowBounds`. Structural events trigger a full read of the window list through [Quartz Window Services](https://developer.apple.com/documentation/coregraphics/cgwindowlistcopywindowinfo%28_%3A_%3A%29?language=_5), which also runs once per second as a safety net. On macOS 26 and later, `SLSWindowIteratorGetCornerRadii` supplies each window's real corner radius; earlier versions use the standard 10-point radius.

SkyLight is a private framework, so every symbol is resolved with `dlopen`/`dlsym`. If any required symbol is missing, the tracker falls back to reading the window list eight times per second, as earlier versions did. `--probe` reports which mode is active. The app does not capture window contents or read window titles.

### Fluid simulation

The solver ([src/fluid_solver.metal](src/fluid_solver.metal)) implements [position-based fluids](https://mmacklin.com/pbf_sig_preprint.pdf) in two dimensions. Each 120 Hz step runs several substeps. Each substep predicts positions under gravity, sorts particles into a grid of kernel-sized cells with a counting sort, and then runs two Jacobi iterations of the density constraint, with a small artificial-pressure term that prevents clumping. A slight negative-pressure allowance gives the water cohesion to form streams and drops, and XSPH viscosity damps motion between neighbours.

The particle size sets the rest spacing: 3, 3.75, or 4.5 points. The kernel support, neighbour grid, and rest density are derived from it, and the stream stays about 16 points wide at any size. The liquid presets set viscosity, cohesion, surface friction, and substeps:

| Liquid | XSPH viscosity | Cohesion (density deficit) | Friction | Substeps |
| --- | ---: | ---: | ---: | ---: |
| Water | 0.015 | 0.2% | 6 per second | 6 |
| Thick | 0.09 | 0.6% | 12 per second | 4 |
| Goo | 0.3 | 3% | 30 per second | 4 |

Larger particles and high viscosity made the earlier version look like gel. Runny water uses low viscosity, little cohesion, and more substeps to stay stable without that damping. Particle state never leaves the GPU; the CPU records spawn rows and window positions, and [src/fluid_sim.mm](src/fluid_sim.mm) encodes the steps into the same command buffer as the frame's drawing.

Windows are rounded-rectangle colliders. Each substep places a window partway between its position at the previous frame and its current position, so a dragged window sweeps through the water instead of teleporting. Collision pushes water out along the surface normal. Surfaces water rests on (window tops and the desktop floor) apply friction relative to the surface's own motion, so water on a moving window is carried with it, while water on window sides slides off. Particles trapped by overlapping windows, or by a window reaching past the screen edge, move to the nearest on-screen exit. A window that jumps more than 500 points in one frame, such as when it is minimized or restored, is treated as a teleport and does not fling water.

Water is retained until you clear it. The capacity is 65,536 particles, which is about 510,000 square points of water with fine particles, or 1.15 million with coarse ones.

### Rendering

Native Metal renders in two passes ([src/fluid.metal](src/fluid.metal)). Particle splats add coverage, foam, and speed into a floating-point field at point resolution. A separable Gaussian blur, with a sigma of 60% of a splat radius, then smooths the field so the surface follows the water's shape instead of bumps a few particles wide, and a blit builds its mip chain. The surface pass thresholds the smoothed field and estimates thickness from coarse levels. Normals come from a height that rises only across the surface band, so highlights sit on the visible edge rather than on a second line inside the water; deeper inside they come from coarser levels, so the interior refracts like calm water. Field lookups are clamped a little inside the screen edges, so water resting against the floor or a side wall continues past the edge instead of drawing a bright false surface there. Thicker water absorbs more red and green light, so thin streams are pale and clear while deep pools turn a dark, more opaque blue. The surface also gets a specular highlight, a Fresnel rim, a bright meniscus along every edge that is strongest at the top of a pool, a faint glow under the surface, moving caustics inside deeper water, travelling ripples on the highlights, and white foam where fast water is thin and broken. The output uses premultiplied alpha.

### Refraction

With **Refract the desktop** on, [src/screen_capture.mm](src/screen_capture.mm) streams the selected display through [ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit) at its native resolution and up to 60 frames per second, excluding this app's own windows. Frames arrive as IOSurface-backed Metal textures through a `CVMetalTextureCache`; each frame stays alive until the GPU commands that sample it complete. The surface shader treats the water as a lens: it offsets its desktop lookup along the surface normal, more where the surface tilts and the water is deeper, and offsets red, green, and blue by slightly different amounts. Depth absorbs red light first, so deep pools tint slightly toward cyan while the desktop stays clearly visible through them. Thin water bends the view only a little, so edges do not pull in content from just beyond them. The surface also reflects a bright sky at grazing angles. Inside the water the overlay is opaque, because it draws the refracted desktop itself; the edges blend with the real desktop. Both the capture and the overlay use sRGB, so refracted pixels match their surroundings.

Capture requires the Screen Recording permission. The first time refraction is turned on, macOS asks for it; after granting it in System Settings → Privacy & Security → Screen & System Audio Recording, relaunch the app. The menu status shows whether refraction is active or waiting for permission. Launched from Terminal, the permission belongs to the terminal app instead. The app bundle is signed ad hoc, so macOS may ask again after a rebuild. Captured frames are used only for drawing; they are not saved or sent anywhere.

The refracted image lags the real desktop by about one or two frames, which shows when content scrolls behind the water. Capture runs whenever refraction is on. Without refraction, the shader tints and lights the water and lets the real desktop show through thinner water.

The native view directly owns its [CAMetalLayer](https://developer.apple.com/documentation/quartzcore/cametallayer). [CAMetalDisplayLink](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink) drives rendering on the AppKit main run loop, including while menus are open. Signals, menu quit, and the duration limit are also checked by a small timer so shutdown works when no drawable is available. AppKit termination returns through C++ cleanup, which stops the motion helper.

The native window's [ignoresMouseEvents](https://developer.apple.com/documentation/appkit/nswindow/ignoresmouseevents) property keeps it click-through. The window, view, and Metal layer are non-opaque.

After rebuilding, quit any running copy through the **💧 menu → Quit Fluid Hose** or **Ctrl+C**, then relaunch. An already running process continues using its original code.
## Scope

This is a 2D toy on **one selected display at a time**. Windows are solid rounded rectangles; shadows, pop-up menus, and nonstandard shapes are not modeled. A window that covers the entire canvas is omitted because it would leave no space for the simulation. At most 64 windows on the selected display are colliders. The desktop floor is the usable screen edge above the Dock.
## Build and checks

```sh
./examples/fluid-hose/build.sh
./examples/fluid-hose/run.sh --physics-check
./examples/fluid-hose/run.sh --display-check
./examples/fluid-hose/run.sh --probe
./examples/fluid-hose/run.sh --smoke-test
./examples/fluid-hose/run.sh --snapshot water.png
./examples/fluid-hose/run.sh --snapshot refraction.png --refraction --duration 30
./examples/fluid-hose/run.sh --capture-check
./examples/fluid-hose/run.sh --window-check
"./examples/fluid-hose/build/Fluid Hose.app/Contents/MacOS/fluid-hose" --window-check
```

- `--physics-check` runs the GPU solver without a window, once for each particle size and liquid. It checks gravity and pooling, that a settled pool comes to rest without internal jitter, particle spacing, collision with rounded windows, hose placement, displacement by moving and overlapping windows, that a dragged window pushes water along, rotated tilt gravity, and clearing. It needs a Metal device but no desktop session.
- `--motion-check` checks gravity projection, stale/invalid data, and helper transport/shutdown using synthetic data without opening sensors. Fluid response to rotated gravity is part of `--physics-check`.
- `--display-check` checks saved selection, disconnect fallback, reconnect with changed native IDs, and conversion between AppKit and Quartz coordinates without requiring graphics access.
- Interactive display switching requires a normal desktop session; `--list-displays` enumerates the screens without creating an overlay.
- `--probe` prints desktop bounds, the window tracking mode, and each window collider with its corner radius, without launching the overlay.
- `--snapshot FILE.png` simulates a scripted scene offscreen (a stream pouring onto one window while a second window slides into the pool) and writes it over a synthetic desktop. `--duration` sets the simulated seconds; the default is 8. `--size` and `--liquid` select the preset; `--refraction` uses the synthetic desktop, which has grid lines, rings, and text-like rows, as the refracted image. It is useful for tuning the solver and shaders without a live desktop.
- `--capture-check` captures the main display briefly and reports the size of the first frame received; it saves nothing. It requires the Screen Recording permission.
- `--smoke-test` checks that splats merge into a translucent, premultiplied water surface on transparent background pixels without creating a window.
- `--window-check` briefly shows a native overlay on each connected display. It checks actual WindowServer bounds, Metal drawable dimensions and frame delivery, display switching, menu actions, and transparent, click-through, non-focusing window settings. It requires a live desktop, does not save a display preference, and does not open sensors. Physical monitor disconnection and manual mouse interaction remain interactive checks.
- `--duration 10` runs the overlay for ten seconds and exits.

On October 9, 2026, after the GPU solver and window tracking were added, the hose was built with only the Command Line Tools (runtime shader compilation) on an Apple M3 Pro running macOS 14.3. `--physics-check`, `--display-check`, `--motion-check`, the helper self-test and lifetime check, `--smoke-test`, code signing, and bundled-dependency checks passed. `--probe` reported live WindowServer tracking, and a pouring launch with `--duration` exited cleanly. SkyLight move notifications were confirmed to arrive for every move of another process's window. `--window-check` crashes inside `CAMetalDisplayLink` on this macOS 14.3 machine; the commit before these changes crashes the same way, so the crash is not caused by them. Visual results were checked with `--snapshot`. Live window dragging, rendering on a Retina display, and the macOS 26 corner-radius path still need checking interactively.

After the liquid presets, particle sizes, and refraction were added, the same machine passed `--physics-check` across all nine size and liquid combinations in ten consecutive runs, plus `--smoke-test`, `--display-check`, `--motion-check`, code signing, and bundled-dependency checks. A launch with saved preferences exited cleanly. Refraction was checked with `--snapshot --refraction`. `--capture-check` could not run there because that process lacked the Screen Recording permission, so live capture and refraction still need checking on a desktop where the permission is granted.

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

## Performance — October 9, 2026

GPU time per 120 Hz step on an Apple M3 Pro with the default fine water (six substeps), measured with Metal command-buffer timestamps. The benchmark uses a 1440 × 900 point canvas, seeds water by sweeping the nozzle across it, lets it settle, then times 240 steps, each in its own command buffer. CPU encoding, rendering, desktop capture, and window tracking are excluded.

| Particles | No windows median | 1 window median | 12 windows median | 12 windows p95 | Budget used (12 windows) |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4,080 | 0.75 ms | 0.75 ms | 0.80 ms | 0.82 ms | 9.5% |
| 16,370 | 1.14 ms | 1.16 ms | 1.31 ms | 1.34 ms | 15.7% |
| 65,520 | 3.01 ms | 3.05 ms | 3.46 ms | 3.51 ms | 41.4% |

Thick and goo use four substeps, so they cost about two thirds as much. A trial capacity of 131,072 fine particles took 7.0 ms per step with 12 windows, 84% of the budget before rendering, so the capacity stays at 65,536.

The previous CPU solver needed about 10 ms per step for 8,000 particles on an M1 Pro, which exceeded the 8.33 ms step budget. Its benchmarks and the neighbor-cache comparison tool remain in the Git history (commit `58eba65`). Rendering cost is not yet measured; the splat pass runs at point resolution, so a 2× Retina display shades a quarter of its pixels there.

Reproduce from the workspace root:

```sh
./examples/fluid-hose/tools/benchmark-physics.sh
./examples/fluid-hose/tools/benchmark-physics.sh --quick
```

Raw results: [benchmarks/2026-10-09-gpu-solver.csv](benchmarks/2026-10-09-gpu-solver.csv).
## Code

- [src/main.cpp](src/main.cpp): command-line options and the fixed-step frame loop.
- [src/fluid_sim.mm](src/fluid_sim.mm): particle sizes and liquid presets, step recording and encoding, hose placement, and physics checks.
- [src/fluid_solver.metal](src/fluid_solver.metal): position-based fluid compute kernels and rounded-window collision.
- [src/window_tracker.mm](src/window_tracker.mm): live window tracking through SkyLight notifications, with a polling fallback.
- [src/desktop.mm](src/desktop.mm): AppKit application lifecycle and display link, native overlay window, display metadata, Option-key state, menu-bar controls and saved preferences, Metal rendering, and offscreen snapshots.
- [src/screen_capture.mm](src/screen_capture.mm): ScreenCaptureKit desktop capture for refraction.
- [src/fluid.metal](src/fluid.metal): particle splats, water surface shading, and the nozzle.
- [src/fluid_types.h](src/fluid_types.h): shared CPU/GPU data layouts.
- [tools/probe_motion.cpp](tools/probe_motion.cpp): SPU motion sensor activation and live sample diagnostics.
- [src/motion.cpp](src/motion.cpp): gravity estimation, display projection, and ownership of the reader process.
- [src/displays.cpp](src/displays.cpp): selecting a connected display and recovering the saved monitor after reconnect.
