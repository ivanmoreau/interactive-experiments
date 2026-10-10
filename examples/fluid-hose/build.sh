#!/bin/bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
APP_DIR="$BUILD_DIR/Fluid Hose.app"

"$PROJECT_DIR/../../tools/check-native-tools.sh" --runtime-shaders

mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
# Remove generated SDL artifacts when rebuilding a pre-migration checkout.
rm -f "$BUILD_DIR/lib/libSDL3.0.dylib" "$APP_DIR/Contents/Frameworks/libSDL3.0.dylib" \
    "$APP_DIR/Contents/Resources/SDL3-LICENSE.txt"

rm -f "$APP_DIR/Contents/Resources/fluid.metallib" "$APP_DIR/Contents/Resources/fluid.metal" \
    "$BUILD_DIR/fluid.metallib" "$BUILD_DIR/fluid.metal"
if xcrun -sdk macosx --find metal >/dev/null 2>&1; then
    printf 'Building fluid shaders...\n'
    for shader in fluid fluid_solver; do
        xcrun -sdk macosx metal -std=macos-metal2.4 -mmacosx-version-min=14.0 \
            -fmodules-cache-path="$BUILD_DIR/metal-module-cache" \
            -c "$PROJECT_DIR/src/$shader.metal" -o "$BUILD_DIR/$shader.air"
    done
    xcrun -sdk macosx metallib "$BUILD_DIR/fluid.air" "$BUILD_DIR/fluid_solver.air" -o "$BUILD_DIR/fluid.metallib"
    SHADERS="$BUILD_DIR/fluid.metallib"
else
    # Without full Xcode, bundle one self-contained source file; Metal
    # compiles it at launch.
    printf 'Offline Metal compiler unavailable; bundling shader source for runtime compilation...\n'
    cat "$PROJECT_DIR/src/fluid_types.h" "$PROJECT_DIR/src/fluid.metal" "$PROJECT_DIR/src/fluid_solver.metal" |
        grep -v '^#include "fluid_types.h"' | grep -v '^#pragma once' > "$BUILD_DIR/fluid.metal"
    SHADERS="$BUILD_DIR/fluid.metal"
fi

"$PROJECT_DIR/tools/build-jolt.sh"
JOLT_DIR="$PROJECT_DIR/third_party/JoltPhysics"

printf 'Building fluid application...\n'
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -mmacosx-version-min=14.0 \
    "$PROJECT_DIR/tools/probe_motion.cpp" -framework CoreFoundation -framework IOKit \
    -o "$BUILD_DIR/probe-motion"
for source in desktop fluid_sim window_tracker screen_capture; do
    xcrun clang++ -std=c++17 -O2 -g -fobjc-arc -Wall -Wextra -Wpedantic -Werror \
        -mmacosx-version-min=14.0 \
        -c "$PROJECT_DIR/src/$source.mm" -o "$BUILD_DIR/$source.o"
done
xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 \
    -isystem "$JOLT_DIR" \
    "$PROJECT_DIR/src/main.cpp" "$PROJECT_DIR/src/motion.cpp" "$PROJECT_DIR/src/displays.cpp" "$PROJECT_DIR/src/duck_world.cpp" \
    "$BUILD_DIR/desktop.o" "$BUILD_DIR/fluid_sim.o" "$BUILD_DIR/window_tracker.o" "$BUILD_DIR/screen_capture.o" \
    -framework AppKit -framework Foundation -framework Metal -framework QuartzCore -framework CoreGraphics -framework ImageIO \
    -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo \
    "$PROJECT_DIR/build/jolt/libJolt.a" \
    -o "$BUILD_DIR/fluid-hose"

cp "$BUILD_DIR/fluid-hose" "$APP_DIR/Contents/MacOS/fluid-hose"
cp "$BUILD_DIR/probe-motion" "$APP_DIR/Contents/MacOS/probe-motion"
codesign --force --sign - "$APP_DIR/Contents/MacOS/probe-motion"
cp "$SHADERS" "$APP_DIR/Contents/Resources/"
cp "$PROJECT_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"
cp "$JOLT_DIR/LICENSE" "$APP_DIR/Contents/Resources/JoltPhysics-LICENSE.txt"
codesign --force --sign - "$APP_DIR"
printf 'Built %s\n' "$APP_DIR"
