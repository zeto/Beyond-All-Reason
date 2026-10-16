#!/bin/bash
# One grass timing run: bench_grass.sh VIEW PATH OUTDIR  (VIEW select|dense|wide, PATH vertex|mesh|none).
# none runs --no-grass; grass-pass cost = GPU ms with grass - GPU ms with none (same view).
# 1200 frames, windowed 1280x720, vsync off (displaySyncEnabled NO), locked camera, no screenshot.
# Writes OUTDIR/VIEW-PATH.csv: per-frame GPU ms (commit feedback) and CPU encode ms.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
VIEW="$1"; GPATH="$2"; OUT="$3"
if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "bench_grass: bar-metal or spring is already running" >&2
  exit 1
fi
mkdir -p "$OUT"
GRASS=(--grass-path="$GPATH")
if [ "$GPATH" = none ]; then
  GRASS=(--no-grass)
fi
CAM=()
if [ "$VIEW" != select ]; then
  CAM=(--fixed-camera "$ROOT/native/oracle/grass_bench_cameras.json#$VIEW")
fi
"$ROOT/native/build/bar-metal" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" --mapdir "$MAPDIR" \
  --frames 1200 --lock-camera ${CAM[@]+"${CAM[@]}"} \
  --input "$ROOT/native/oracle/scenarios/flip_select.input" \
  "${GRASS[@]}" --gpu-timing "$OUT/$VIEW-$GPATH.csv" > "$OUT/$VIEW-$GPATH.log" 2>&1
tail -1 "$OUT/$VIEW-$GPATH.log"
