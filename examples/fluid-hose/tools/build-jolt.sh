#!/bin/bash
# Builds Jolt Physics from the pinned submodule into build/jolt/libJolt.a.
# The library is rebuilt only when the submodule commit changes.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOLT_DIR="$PROJECT_DIR/third_party/JoltPhysics"
OUT_DIR="$PROJECT_DIR/build/jolt"

if [[ ! -f "$JOLT_DIR/Jolt/Jolt.h" ]]; then
    printf 'Fetching the Jolt Physics submodule...\n'
    git -C "$PROJECT_DIR" submodule update --init third_party/JoltPhysics
fi
COMMIT="$(git -C "$JOLT_DIR" rev-parse HEAD)"
if [[ -f "$OUT_DIR/libJolt.a" && "$(cat "$OUT_DIR/commit" 2>/dev/null)" == "$COMMIT" ]]; then
    exit 0
fi

printf 'Building Jolt Physics %s...\n' "$(git -C "$JOLT_DIR" describe --tags 2>/dev/null || printf '%s' "$COMMIT")"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/objects"
# The optional GPU compute backends are excluded; Jolt builds them only on request.
find "$JOLT_DIR/Jolt" -name '*.cpp' -not -path '*/Compute/CPU/*' -not -path '*/Compute/DX12/*' \
    -not -path '*/Compute/MTL/*' -not -path '*/Compute/VK/*' -print0 |
    xargs -0 -P "$(sysctl -n hw.ncpu)" -I{} sh -c '
        object="$1/objects/$(printf "%s" "$2" | sed "s|.*/Jolt/||; s|/|_|g; s|\.cpp$|.o|")"
        xcrun clang++ -std=c++17 -O2 -g -mmacosx-version-min=14.0 -I "$3" -c "$2" -o "$object"
    ' _ "$OUT_DIR" {} "$JOLT_DIR"
xcrun ar rcs "$OUT_DIR/libJolt.a" "$OUT_DIR"/objects/*.o
printf '%s\n' "$COMMIT" > "$OUT_DIR/commit"
