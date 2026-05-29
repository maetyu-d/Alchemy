#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-0.1.0}"
BUILD_DIR="$ROOT/build"
APP="$BUILD_DIR/AlchemyGui_artefacts/Release/Alchemy.app"
DIST="$ROOT/dist"
STAGE="$DIST/Alchemy-v$VERSION"
FRAMEWORKS="$STAGE/Alchemy.app/Contents/Frameworks"
RESOURCES="$STAGE/Alchemy.app/Contents/Resources"

cmake --build "$BUILD_DIR" --target AlchemyGui --config Release

rm -rf "$STAGE"
mkdir -p "$FRAMEWORKS" "$RESOURCES"
ditto "$APP" "$STAGE/Alchemy.app"
ditto "$ROOT/Examples" "$RESOURCES/Examples"
ditto "$ROOT/Packaging/DEPENDENCIES.md" "$RESOURCES/DEPENDENCIES.md"

copy_if_exists() {
    local src="$1"
    local dest="$2"
    if [[ -e "$src" ]]; then
        mkdir -p "$(dirname "$dest")"
        ditto "$src" "$dest"
        echo "bundled: $src -> $dest" >> "$STAGE/DEPENDENCY-MANIFEST.txt"
    fi
}

: > "$STAGE/DEPENDENCY-MANIFEST.txt"
echo "Alchemy v$VERSION" >> "$STAGE/DEPENDENCY-MANIFEST.txt"
echo "Built: $(date -u +"%Y-%m-%dT%H:%M:%SZ")" >> "$STAGE/DEPENDENCY-MANIFEST.txt"

copy_if_exists "${WELD_CSOUND_LIBRARY:-}" "$FRAMEWORKS/$(basename "${WELD_CSOUND_LIBRARY:-}")"
copy_if_exists "$ROOT/build-csound/CsoundLib64.framework" "$FRAMEWORKS/CsoundLib64.framework"
copy_if_exists "$ROOT/build-csound/libcsound64.dylib" "$FRAMEWORKS/libcsound64.dylib"
copy_if_exists "$ROOT/third_party/rtcmix/src/rtcmix/librtcmix_embedded.dylib" "$FRAMEWORKS/librtcmix_embedded.dylib"
copy_if_exists "$ROOT/build-supercollider-host/server/scsynth/libscsynth.dylib" "$FRAMEWORKS/libscsynth.dylib"
copy_if_exists "$ROOT/build-supercollider-host/lang/libweldsclang.dylib" "$FRAMEWORKS/libweldsclang.dylib"

if [[ -d "$ROOT/build-supercollider-host/server/plugins" ]]; then
    ditto "$ROOT/build-supercollider-host/server/plugins" "$RESOURCES/SuperCollider/plugins"
    echo "bundled: SuperCollider plugins -> Contents/Resources/SuperCollider/plugins" >> "$STAGE/DEPENDENCY-MANIFEST.txt"
fi

ditto -c -k --sequesterRsrc --keepParent "$STAGE/Alchemy.app" "$DIST/Alchemy-v$VERSION-macOS.zip"
echo "$DIST/Alchemy-v$VERSION-macOS.zip"
