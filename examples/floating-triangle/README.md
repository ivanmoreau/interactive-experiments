# Hello, SDL GPU

A small native C++17 macOS application using **SDL3 + SDL GPU's Metal backend** for the standard window and **SDL3 + native Metal** for the floating window. Both modes draw the same slowly rotating triangle with interpolated vertex colors and support Retina displays and vsync.

## Run

Enter the shared development environment from the repository root with `devenv shell`, then build this example with `./build.sh` from its directory (or use `build-examples` at the root). See the [repository setup](../../README.md). From this directory:

```sh
./run.sh
```

You can also open `build/Hello SDL GPU.app` in Finder. The app includes its SDL3 library and compiled shaders, so it does not depend on a global SDL installation.

Controls: **Space** pauses or resumes rotation, **R** resets rotation, and **Escape** closes the window.

### Floating triangle

```sh
./run.sh --floating
```

Floating mode uses a 360×360 transparent, borderless window that stays above ordinary windows. The GPU clears the background to zero alpha and draws the triangle with opaque colors, so only the triangle is visible. Drag it to move the window. Click it to focus the app before using **Space**, **R**, or **Escape**.

The OS still owns a window; its frame and background are invisible. SDL handles the window and dragging, while a small native Metal renderer presents to an SDL-created Metal view. SDL GPU explicitly rejects transparent windows in [its window-claim code](https://github.com/libsdl-org/SDL/blob/release-3.4.10/src/gpu/SDL_gpu.c), so floating mode does not claim the window through that API. Both renderers reuse the same `hello.metal` shader and uniform layout.

`./run.sh --windowed` selects the original resizable SDL GPU window. Floating mode does not implement a changing per-pixel input mask; its invisible window bounds can receive mouse input.

Check the GPU output's transparency with:

```sh
./run.sh --smoke-test --floating
```

That check uses the actual native Metal pipeline, renders to a 64×64 shared texture, and requires an opaque colored center and completely transparent black background pixels. It needs access to a Metal device but no display. Desktop compositing and dragging still require a live desktop session to verify.

## Device checked

The hardware inspection found:

| Item | Result |
| --- | --- |
| Computer | MacBook Pro, MacBookPro18,3 |
| CPU | Apple M1 Pro, arm64 |
| GPU | Apple M1 Pro, 14 cores |
| Memory | 16 GB |
| OS | macOS 15.7.7 |
| Metal | Supported, as reported by macOS |
| SDL3 runtime | 3.4.10; Metal GPU backend included |

Apple Silicon satisfies [SDL GPU's Metal hardware requirements](https://wiki.libsdl.org/SDL3/CategoryGPU). The local SDL library requires macOS 14, which this device meets.

On October 9, 2026, the locked devenv SDK built the app successfully on this machine. Pixel-readback checks passed for both the SDL GPU renderer and the native Metal floating renderer; the packaged app's floating readback check also passed. The floating window's compositing and dragging have been confirmed interactively. The machine's reported capabilities are recorded separately in [the device report](build/device-capabilities.json).

To repeat the inspection from your normal Terminal session:

```sh
python3 tools/check_device.py
./run.sh --probe
./run.sh --smoke-test
```

`--probe` creates an actual SDL GPU device and prints its backend and name. The standard `--smoke-test` renders into a 64×64 SDL GPU texture, downloads pixels, and checks both the colored triangle and cleared background. It creates no window, although SDL's video subsystem must still initialize. Adding `--floating` selects the native Metal offscreen check instead. Checks exit with an error if a device or a drawing operation fails.

For a short visible run:

```sh
./run.sh --frames 120
```

## Build

Requires macOS 14 or newer and Xcode with its C++ compiler and Metal tools. Those tools were available on this machine (Xcode 16.3, Apple Clang 17).

```sh
./build.sh
```

Run the build inside the repository's devenv shell. The script compiles the Metal shaders into `build/hello.metallib`, builds the C++ executable with optimization and warnings treated as errors, and assembles an app bundle with local ad hoc code signatures. It uses the SDL header and library paths exported by devenv. `SDL3_ROOT` and `SDL3_LIBRARY` can select a custom SDK; a standalone SDK containing `include/SDL3` and `lib/libSDL3.0.dylib` needs only `SDL3_ROOT`.

### Dependencies

The root `devenv.lock` pins Nixpkgs, which provides complete, matching SDL3 headers and its library for both examples. The current pin contains SDL 3.4.10. Bash, Git, and Python are provided by the same environment. Xcode supplies the native compiler and Metal tools.

From the repository root:

```sh
devenv shell -- build-examples
```

Each bundle includes its own SDL library, so running the built app does not require Nix. Existing `.deps/SDL3` bootstrap folders are ignored and remain a fallback outside devenv; they are not required in a fresh checkout. Dependencies and generated build output stay outside Git.

## Code

- [src/main.cpp](src/main.cpp): device creation, shaders and pipeline, event loop, frame submission, and the GPU readback check.
- [src/floating_metal.mm](src/floating_metal.mm): native Metal renderer and pixel-readback check for floating mode; Objective-C++ connects the C++ app to Apple's APIs.
- [src/hello.metal](src/hello.metal): vertex positions, rotation, aspect correction, and interpolated color.
- [src/frame_uniforms.h](src/frame_uniforms.h): the shared 16-byte parameter layout for both CPU renderers and the shader.
- [tools/metal_capabilities.mm](tools/metal_capabilities.mm): native Metal capability query, including GPU families and memory limits when the device is accessible.

The triangle's three vertices are generated in the vertex shader. Each frame sends a 16-byte uniform and one draw call; GPU resources and pipelines are reused. Shader resources are located relative to the executable, so launching from another working directory is supported.

This first example targets macOS and uses Metal shader binaries. SDL GPU supports other backends, but adding Windows/Linux builds would require their shader formats and build configuration.

## Validation

Completed: Metal shader compilation; C++ compilation/linking; CLI help and invalid-argument checks; runtime SDL version and compiled Metal backend inspection; app bundle signature and property-list validation; relative runtime library linking.

You confirmed that floating mode works on this Mac. The project was then moved to `examples/floating-triangle` and rebuilt successfully at its new location. Compilation, CLI help, and the relocated app signature were checked again. Other interactive checks and pixel readback still require your normal desktop session.

SDL's license is included in [SDL3-LICENSE.txt](SDL3-LICENSE.txt) and the app's Resources directory.
