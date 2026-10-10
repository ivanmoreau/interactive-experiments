#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$REPO_DIR/examples/floating-triangle/build.sh"
"$REPO_DIR/examples/fluid-hose/build.sh"
"$REPO_DIR/examples/floating-triangle/build/hello-sdl-gpu" --help >/dev/null
"$REPO_DIR/examples/fluid-hose/build/fluid-hose" --physics-check
"$REPO_DIR/examples/fluid-hose/build/fluid-hose" --duck-check
"$REPO_DIR/examples/fluid-hose/build/fluid-hose" --display-check
"$REPO_DIR/examples/fluid-hose/build/fluid-hose" --motion-check
"$REPO_DIR/examples/fluid-hose/build/probe-motion" --self-test
python3 "$REPO_DIR/examples/fluid-hose/tools/check-motion-lifetime.py"
codesign --verify --deep --strict "$REPO_DIR/examples/floating-triangle/build/Hello SDL GPU.app"
codesign --verify --deep --strict "$REPO_DIR/examples/fluid-hose/build/Fluid Hose.app"
python3 "$REPO_DIR/tools/check-bundle-dependencies.py" \
    "$REPO_DIR/examples/floating-triangle/build/Hello SDL GPU.app" \
    "$REPO_DIR/examples/fluid-hose/build/Fluid Hose.app"
