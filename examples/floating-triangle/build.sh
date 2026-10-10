#!/bin/bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
SDL_DIR="${SDL3_ROOT:-$PROJECT_DIR/.deps/SDL3}"
SDL_LIBRARY="${SDL3_LIBRARY:-$SDL_DIR/lib/libSDL3.0.dylib}"
APP_DIR="$BUILD_DIR/Hello SDL GPU.app"

if [[ ! -f "$SDL_DIR/include/SDL3/SDL_gpu.h" || ! -f "$SDL_LIBRARY" ]]; then
    printf 'SDL3 headers/library are missing. Enter the repository devenv shell, or set SDL3_ROOT and SDL3_LIBRARY.\n' >&2
    exit 1
fi
"$PROJECT_DIR/../../tools/check-native-tools.sh"

mkdir -p "$BUILD_DIR" "$BUILD_DIR/lib" "$APP_DIR/Contents/MacOS" \
    "$APP_DIR/Contents/Resources" "$APP_DIR/Contents/Frameworks"

printf 'Building Metal shaders...\n'
xcrun -sdk macosx metal -std=macos-metal2.4 -mmacosx-version-min=14.0 \
    -fmodules-cache-path="$BUILD_DIR/metal-module-cache" \
    -c "$PROJECT_DIR/src/hello.metal" -o "$BUILD_DIR/hello.air"
xcrun -sdk macosx metallib "$BUILD_DIR/hello.air" -o "$BUILD_DIR/hello.metallib"

install -m 755 "$SDL_LIBRARY" "$BUILD_DIR/lib/libSDL3.0.dylib"
install_name_tool -id '@rpath/libSDL3.0.dylib' "$BUILD_DIR/lib/libSDL3.0.dylib"
codesign --force --sign - "$BUILD_DIR/lib/libSDL3.0.dylib"

printf 'Building C++ application...\n'
xcrun clang++ -std=c++17 -O2 -g -fobjc-arc -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 -I "$SDL_DIR/include" \
    -c "$PROJECT_DIR/src/floating_metal.mm" -o "$BUILD_DIR/floating_metal.o"
xcrun clang++ -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror \
    -mmacosx-version-min=14.0 -I "$SDL_DIR/include" \
    "$PROJECT_DIR/src/main.cpp" "$BUILD_DIR/floating_metal.o" "$BUILD_DIR/lib/libSDL3.0.dylib" \
    -framework Foundation -framework Metal -framework QuartzCore \
    -Wl,-rpath,@executable_path/lib -Wl,-rpath,@executable_path/../Frameworks \
    -o "$BUILD_DIR/hello-sdl-gpu"

xcrun clang++ -std=c++17 -O2 -fobjc-arc -Wall -Wextra -Werror \
    -mmacosx-version-min=14.0 -framework Foundation -framework Metal \
    "$PROJECT_DIR/tools/metal_capabilities.mm" -o "$BUILD_DIR/metal-capabilities"

cp "$BUILD_DIR/hello-sdl-gpu" "$APP_DIR/Contents/MacOS/hello-sdl-gpu"
cp "$BUILD_DIR/hello.metallib" "$APP_DIR/Contents/Resources/hello.metallib"
cp "$PROJECT_DIR/SDL3-LICENSE.txt" "$APP_DIR/Contents/Resources/SDL3-LICENSE.txt"
install -m 755 "$BUILD_DIR/lib/libSDL3.0.dylib" "$APP_DIR/Contents/Frameworks/libSDL3.0.dylib"
cp "$PROJECT_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"
codesign --force --sign - "$APP_DIR"
printf 'Built %s\n' "$APP_DIR"
