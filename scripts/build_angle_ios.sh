#!/bin/bash
# Build the WebKit ANGLE Metal backend for iPhone or macOS validation.
set -euo pipefail

DOD_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GPU="$DOD_ROOT/third_party/gpu"
WK="$GPU/webkit"
REVISION=2edaa071178a43a44dfaa4c37716247c43642c66
SDK="${1:-iphoneos}"
case "$SDK" in
    iphoneos) PREFIX="$DOD_ROOT/build/ios-arm64/sysroot" ;;
    macosx) PREFIX="$DOD_ROOT/work/angle-macos/prefix" ;;
    *) echo "Usage: $0 [iphoneos|macosx]" >&2; exit 2 ;;
esac
LOG="$DOD_ROOT/build/logs/angle-build-$SDK.log"
mkdir -p "$GPU" "$(dirname "$LOG")"
if [ ! -d "$WK" ]; then
    git init "$WK"
    git -C "$WK" remote add origin https://github.com/WebKit/WebKit.git
    git -C "$WK" sparse-checkout init --cone
    git -C "$WK" fetch --depth 1 --filter=blob:none origin "$REVISION"
    git -C "$WK" checkout --detach FETCH_HEAD
fi
[ "$(git -C "$WK" rev-parse HEAD)" = "$REVISION" ] || { echo "Unexpected WebKit revision" >&2; exit 1; }
git -C "$WK" sparse-checkout set Source/ThirdParty/ANGLE Configurations Tools/Scripts Tools/ccache
cd "$WK/Source/ThirdParty/ANGLE"
ALIASES="$GPU/angle-aliases.txt"

angle_build () {
    local ldflags=()
    if [ -n "${1:-}" ]; then
        ldflags=("OTHER_LDFLAGS=\$(inherited) -Wl,-alias_list,$1")
    fi
    # This sparse source tree does not generate WebKit's availability overlay.
    env -i PATH="$PATH" HOME="$HOME" xcodebuild archive \
        -archivePath "ANGLE-$SDK" -scheme ANGLE -sdk "$SDK" -arch arm64 -configuration Release \
        WEBCORE_LIBRARY_DIR=/usr/local/lib NORMAL_UMBRELLA_FRAMEWORKS_DIR="" \
        CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO ANGLE_ALLOWABLE_CLIENTS="" \
        WK_AVAILABILITY_OVERLAY_FLAGS="" WK_AVAILABILITY_OVERLAY_SWIFT_FLAGS="" \
        IPHONEOS_DEPLOYMENT_TARGET=16.4 MACOSX_DEPLOYMENT_TARGET=15.0 \
        ${ldflags[@]+"${ldflags[@]}"} > "$LOG" 2>&1 \
        || { tail -60 "$LOG" >&2; exit 1; }
}

echo "Building ANGLE for $SDK (log: $LOG)"
angle_build ""
DYLIB="ANGLE-$SDK.xcarchive/Products/usr/local/lib/libANGLE-shared.dylib"
# WebKit renames EGL exports. GL functions are resolved through eglGetProcAddress.
nm -g "$DYLIB" | awk '$2=="T" && $3 ~ /^_EGL_/ {o=$3; a=$3; sub(/^_EGL_/,"_egl",a); print o, a}' > "$ALIASES"
angle_build "$ALIASES"
install -d "$PREFIX/lib" "$PREFIX/include"
cp "$DYLIB" "$PREFIX/lib/"
rsync -a include/ "$PREFIX/include/"
install_name_tool -id @rpath/libANGLE-shared.dylib "$PREFIX/lib/libANGLE-shared.dylib"
python3 - "$DOD_ROOT" "$PREFIX" "$REVISION" "$SDK" <<'PY'
import hashlib, json, subprocess, sys
from pathlib import Path
root, prefix = map(Path, sys.argv[1:3])
library = prefix / 'lib/libANGLE-shared.dylib'
script = root / 'scripts/build_angle_ios.sh'
(prefix / 'angle-manifest.json').write_text(json.dumps({
    'revision': sys.argv[3], 'platform': sys.argv[4],
    'sdk': subprocess.check_output(['xcrun', '--sdk', sys.argv[4], '--show-sdk-version'], text=True).strip(),
    'library': str(library), 'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
    'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
}, indent=2) + '\n')
PY
echo "Built $PREFIX/lib/libANGLE-shared.dylib"
