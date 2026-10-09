#!/bin/bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PROJECT_DIR/build"
xcrun clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -mmacosx-version-min=14.0 \
    "$PROJECT_DIR/tools/probe_motion.cpp" -framework CoreFoundation -framework IOKit \
    -o "$PROJECT_DIR/build/probe-motion"
exec "$PROJECT_DIR/build/probe-motion" "$@"
