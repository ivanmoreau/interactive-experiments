#!/bin/bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
SDL_DIR="${SDL3_ROOT:-$PROJECT_DIR/.deps/SDL3}"
SDL_LIBRARY="$SDL_DIR/lib/libSDL3.0.dylib"
APP_DIR="$BUILD_DIR/Fluid Hose.app"

if [[ ! -f "$SDL_DIR/include/SDL3/SDL_gpu.h" || ! -f "$SDL_LIBRARY" ]]; then
    printf 'SDL3 is missing. Run ./tools/setup-sdl.sh or set SDL3_ROOT.\n' >&2
    exit 1
fi

mkdir -p "$BUILD_DIR/lib" "$APP_DIR/Contents/MacOS" \
    "$APP_DIR/Contents/Resources" "$APP_DIR/Contents/Frameworks"

printf 'Building fluid shaders...\n'
xcrun -sdk macosx metal -std=macos-metal2.4 -mmacosx-version-min=14.0 \
    -fmodules-cache-path="$BUILD_DIR/metal-module-cache" \
    -c "$PROJECT_DIR/src/fluid.metal" -o "$BUILD_DIR/fluid.air"
xcrun -sdk macosx metallib "$BUILD_DIR/fluid.air" -o "$BUILD_DIR/fluid.metallib"

install -m 755 "$SDL_LIBRARY" "$BUILD_DIR/lib/libSDL3.0.dylib"
install_name_tool -id '@rpath/libSDL3.0.dylib' "$BUILD_DIR/lib/libSDL3.0.dylib"
codesign --force --sign - "$BUILD_DIR/lib/libSDL3.0.dylib"

printf 'Building fluid application...\n'
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -mmacosx-version-min=14.0 \
    "$PROJECT_DIR/tools/probe_motion.cpp" -framework CoreFoundation -framework IOKit \
    -o "$BUILD_DIR/probe-motion"
xcrun clang++ -std=c++17 -O2 -g -fobjc-arc -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 -I "$SDL_DIR/include" \
    -c "$PROJECT_DIR/src/desktop.mm" -o "$BUILD_DIR/desktop.o"
xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 -I "$SDL_DIR/include" \
    "$PROJECT_DIR/src/main.cpp" "$PROJECT_DIR/src/fluid_sim.cpp" "$PROJECT_DIR/src/motion.cpp" \
    "$BUILD_DIR/desktop.o" "$BUILD_DIR/lib/libSDL3.0.dylib" \
    -framework AppKit -framework Foundation -framework Metal -framework QuartzCore -framework CoreGraphics \
    -Wl,-rpath,@executable_path/lib -Wl,-rpath,@executable_path/../Frameworks \
    -o "$BUILD_DIR/fluid-hose"

cp "$BUILD_DIR/fluid-hose" "$APP_DIR/Contents/MacOS/fluid-hose"
cp "$BUILD_DIR/probe-motion" "$APP_DIR/Contents/MacOS/probe-motion"
codesign --force --sign - "$APP_DIR/Contents/MacOS/probe-motion"
cp "$BUILD_DIR/fluid.metallib" "$APP_DIR/Contents/Resources/fluid.metallib"
cp "$PROJECT_DIR/SDL3-LICENSE.txt" "$APP_DIR/Contents/Resources/SDL3-LICENSE.txt"
install -m 755 "$BUILD_DIR/lib/libSDL3.0.dylib" "$APP_DIR/Contents/Frameworks/libSDL3.0.dylib"
cp "$PROJECT_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"
codesign --force --sign - "$APP_DIR"
printf 'Built %s\n' "$APP_DIR"
