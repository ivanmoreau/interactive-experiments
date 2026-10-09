#!/bin/bash
set -euo pipefail

# Install the official macOS SDK into this project, leaving system packages alone.
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="3.4.10"
DEPS_DIR="$PROJECT_DIR/.deps"
MOUNT_DIR="$DEPS_DIR/sdl-sdk-mount"
SDK_DIR="$DEPS_DIR/SDL3"
IMAGE="$DEPS_DIR/SDL3-$VERSION.dmg"
URL="https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/SDL3-$VERSION.dmg"

mkdir -p "$DEPS_DIR" "$MOUNT_DIR"
if [[ ! -f "$IMAGE" ]]; then
    curl --fail --location --retry 2 --output "$IMAGE.partial" "$URL"
    mv "$IMAGE.partial" "$IMAGE"
fi

hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT_DIR" "$IMAGE" >/dev/null
trap 'hdiutil detach "$MOUNT_DIR" >/dev/null' EXIT

FRAMEWORK=""
for candidate in "$MOUNT_DIR/SDL3.xcframework/macos-arm64_x86_64/SDL3.framework" \
                 "$MOUNT_DIR/SDL3.framework"; do
    if [[ -f "$candidate/Headers/SDL_gpu.h" && -f "$candidate/SDL3" ]]; then
        FRAMEWORK="$candidate"
        break
    fi
done
if [[ -z "$FRAMEWORK" ]]; then
    printf 'Cannot locate the macOS SDL3 framework in %s.\n' "$IMAGE" >&2
    exit 1
fi

mkdir -p "$SDK_DIR/include/SDL3" "$SDK_DIR/lib"
cp -R "$FRAMEWORK/Headers/." "$SDK_DIR/include/SDL3/"
install -m 755 "$FRAMEWORK/SDL3" "$SDK_DIR/lib/libSDL3.0.dylib"
printf 'Official SDL %s macOS SDK installed in %s.\n' "$VERSION" "$SDK_DIR"
