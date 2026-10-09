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

The C++ simulation applies gravity and particle density constraints, using a spatial grid for neighboring particles. Water is retained until you clear it, so it accumulates instead of disappearing. The current capacity is 8,000 particles.

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

Compilation, physics checks, CLI checks, app signing, and property-list validation passed. A separate flow check confirmed water spilling below a window. Live window enumeration and Metal pixel readback could not be tested in the sandbox because it cannot access the display or a Metal device; run those checks from your normal desktop session.

## Code

- [src/main.cpp](src/main.cpp): fixed-step simulation loop and desktop overlay setup.
- [src/fluid_sim.cpp](src/fluid_sim.cpp): gravity, density relaxation, collision, and nozzle placement.
- [src/desktop.mm](src/desktop.mm): window metadata, Option-key state, click-through window, menu-bar controls, and native Metal rendering.
- [src/fluid.metal](src/fluid.metal): density splats, water surface shading, and the nozzle.
- [src/fluid_types.h](src/fluid_types.h): shared CPU/GPU data layouts.

SDL's license is in [SDL3-LICENSE.txt](SDL3-LICENSE.txt) and the app bundle.
