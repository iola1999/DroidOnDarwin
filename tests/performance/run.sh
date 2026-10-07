#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/work/performance/tests"
mkdir -p "$OUT"
swiftc -O "$ROOT/src/app/Husk/HuskFrameMetrics.swift" \
    "$ROOT/tests/performance/main.swift" -o "$OUT/frame-metrics"
"$OUT/frame-metrics"
swiftc -parse-as-library -Xfrontend -import-module -Xfrontend Combine \
    "$ROOT/src/app/Husk/GuestImage.swift" "$ROOT/src/app/Husk/GuestManifest.swift" \
    "$ROOT/tests/performance/GuestImageTests.swift" -o "$OUT/guest-image"
"$OUT/guest-image"
