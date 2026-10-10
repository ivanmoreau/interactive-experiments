#!/bin/bash
# Downloads the larval zebrafish hindbrain connectome (Vishwanathan et al.,
# Nature Neuroscience 2024) from a pinned commit of the authors' repository,
# checks it, and converts it into build/connectome/hindbrain.txt. The data has
# no stated license, so it is fetched for local builds and never committed.
# Without network access the fish fall back to a hand-wired hindbrain.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$PROJECT_DIR/build/connectome"
COMMIT=97d9cbeb65ab21205e5500970d0ea74fbc74c00f
URL="https://raw.githubusercontent.com/ashwinvish/Zfish_recon/$COMMIT/matFiles/ZConnectome_04292021.mat"
SHA256=a80c259f6b5a158e456505dd73a349bfde6433d0f95b749343899979c2a20be4

if [[ -f "$OUT_DIR/hindbrain.txt" && "$(cat "$OUT_DIR/source" 2>/dev/null)" == "$SHA256" ]]; then
    exit 0
fi
mkdir -p "$OUT_DIR"
if ! curl -fsSL --retry 2 -o "$OUT_DIR/ZConnectome.mat.part" "$URL"; then
    rm -f "$OUT_DIR/ZConnectome.mat.part"
    printf 'Could not download the hindbrain connectome; fish will use a hand-wired hindbrain.\n' >&2
    exit 0
fi
if [[ "$(shasum -a 256 "$OUT_DIR/ZConnectome.mat.part" | cut -d' ' -f1)" != "$SHA256" ]]; then
    rm -f "$OUT_DIR/ZConnectome.mat.part"
    printf 'The downloaded hindbrain connectome has an unexpected checksum; not using it.\n' >&2
    exit 0
fi
mv "$OUT_DIR/ZConnectome.mat.part" "$OUT_DIR/ZConnectome.mat"
python3 -I "$PROJECT_DIR/tools/convert_connectome.py" "$OUT_DIR/ZConnectome.mat" "$OUT_DIR/hindbrain.txt"
printf '%s\n' "$SHA256" > "$OUT_DIR/source"
