#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    printf 'These examples require macOS and native Metal.\n' >&2
    exit 1
fi
MACOS_VERSION="$(sw_vers -productVersion)"
if (( ${MACOS_VERSION%%.*} < 14 )); then
    printf 'macOS 14 or newer is required (found %s).\n' "$MACOS_VERSION" >&2
    exit 1
fi
for tool in xcrun install_name_tool codesign; do
    if ! command -v "$tool" >/dev/null; then
        printf 'Missing %s. Install full Xcode with its Metal tools.\n' "$tool" >&2
        exit 1
    fi
done
for tool in clang++ metal metallib; do
    if ! xcrun --sdk macosx --find "$tool" >/dev/null; then
        printf 'Xcode tool %s is unavailable. Select full Xcode with xcode-select.\n' "$tool" >&2
        exit 1
    fi
done
printf 'Native tools available: macOS %s, %s.\n' "$MACOS_VERSION" "$(xcode-select -p)"
