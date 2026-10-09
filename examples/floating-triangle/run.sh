#!/bin/bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -x "$PROJECT_DIR/build/hello-sdl-gpu" ]]; then
    "$PROJECT_DIR/build.sh"
fi
exec "$PROJECT_DIR/build/hello-sdl-gpu" "$@"
