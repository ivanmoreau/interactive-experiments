#!/bin/bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
APP_DIR="$BUILD_DIR/Fluid Hose.app"

"$PROJECT_DIR/../../tools/check-native-tools.sh"

mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
# Remove generated SDL artifacts when rebuilding a pre-migration checkout.
rm -f "$BUILD_DIR/lib/libSDL3.0.dylib" "$APP_DIR/Contents/Frameworks/libSDL3.0.dylib" \
    "$APP_DIR/Contents/Resources/SDL3-LICENSE.txt"

printf 'Building fluid shaders...\n'
xcrun -sdk macosx metal -std=macos-metal2.4 -mmacosx-version-min=14.0 \
    -fmodules-cache-path="$BUILD_DIR/metal-module-cache" \
    -c "$PROJECT_DIR/src/fluid.metal" -o "$BUILD_DIR/fluid.air"
xcrun -sdk macosx metallib "$BUILD_DIR/fluid.air" -o "$BUILD_DIR/fluid.metallib"

printf 'Building fluid application...\n'
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -mmacosx-version-min=14.0 \
    "$PROJECT_DIR/tools/probe_motion.cpp" -framework CoreFoundation -framework IOKit \
    -o "$BUILD_DIR/probe-motion"
xcrun clang++ -std=c++17 -O2 -g -fobjc-arc -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 \
    -c "$PROJECT_DIR/src/desktop.mm" -o "$BUILD_DIR/desktop.o"
xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 \
    "$PROJECT_DIR/src/main.cpp" "$PROJECT_DIR/src/fluid_sim.cpp" "$PROJECT_DIR/src/motion.cpp" "$PROJECT_DIR/src/displays.cpp" \
    "$BUILD_DIR/desktop.o" \
    -framework AppKit -framework Foundation -framework Metal -framework QuartzCore -framework CoreGraphics \
    -o "$BUILD_DIR/fluid-hose"

cp "$BUILD_DIR/fluid-hose" "$APP_DIR/Contents/MacOS/fluid-hose"
cp "$BUILD_DIR/probe-motion" "$APP_DIR/Contents/MacOS/probe-motion"
codesign --force --sign - "$APP_DIR/Contents/MacOS/probe-motion"
cp "$BUILD_DIR/fluid.metallib" "$APP_DIR/Contents/Resources/fluid.metallib"
cp "$PROJECT_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"
codesign --force --sign - "$APP_DIR"
printf 'Built %s\n' "$APP_DIR"
