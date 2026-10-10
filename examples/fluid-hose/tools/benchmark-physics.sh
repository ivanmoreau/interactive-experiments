#!/bin/bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
# Uses the app's compiled objects and shaders.
"$PROJECT_DIR/build.sh" >/dev/null
xcrun clang++ -std=c++17 -O2 -g -fobjc-arc -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 "$PROJECT_DIR/tools/benchmark_physics.mm" \
    "$BUILD_DIR/fluid_sim.o" "$BUILD_DIR/desktop.o" "$PROJECT_DIR/src/displays.cpp" \
    -framework AppKit -framework Metal -framework QuartzCore -framework CoreGraphics -framework ImageIO \
    -o "$BUILD_DIR/benchmark-physics"
exec "$BUILD_DIR/benchmark-physics" "$@"
