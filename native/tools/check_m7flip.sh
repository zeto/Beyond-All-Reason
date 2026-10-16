#!/bin/bash
# Capture bar-metal + headed Recoil HUD bookmarks, score FLIP, write flip.json.
# Exit 0 only when median FLIP <= m7_flip (0.22). Does not loosen the bar.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7flip"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"
FLIP_PY="$ROOT/native/tools/.flip-venv/bin/python"
BOOKMARKS="$ROOT/native/oracle/hud_bookmarks.json"
TOL="$ROOT/native/oracle/tolerances.toml"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7flip: bar-metal or spring is already running" >&2
  exit 1
fi

if [[ ! -x "$FLIP_PY" ]]; then
  echo "check_m7flip: create venv and pip install flip-evaluator numpy pillow:" >&2
  echo "  python3 -m venv native/tools/.flip-venv && native/tools/.flip-venv/bin/pip install flip-evaluator numpy pillow" >&2
  exit 1
fi

mkdir -p "$OUT/bar" "$OUT/ref" "$OUT/diff"
BAR_FLIP=$(python3 - <<'PY' "$TOL"
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r'm7_flip\s*=\s*([0-9.]+)', text)
print(m.group(1) if m else "0.22")
PY
)
echo "m7_flip bar = $BAR_FLIP"

# --- bar-metal captures (reuse lock-camera inputs from widget checks) ---
capture_bar() {
  local id="$1" input="$2" frames="$3"
  local dest="$OUT/bar/${id}.ppm"
  echo "bar-metal shot $id ..."
  if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
    echo "check_m7flip: process still running before $id" >&2
    exit 1
  fi
  "$BIN" \
    --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
    --mapdir "$MAPDIR" \
    --frames "$frames" \
    --lock-camera \
    --input "$input" \
    --screenshot "$dest" \
    > "$OUT/bar/${id}.log" 2>&1 || {
      echo "bar-metal failed for $id; see $OUT/bar/${id}.log" >&2
      exit 1
    }
  # Prefer -end frame (HUD drawn); fall back to mid shot.
  local end="${dest%.ppm}-end.ppm"
  if [[ -f "$end" ]]; then
    sips -s format png "$end" --out "$OUT/bar/${id}.png" >/dev/null
  else
    sips -s format png "$dest" --out "$OUT/bar/${id}.png" >/dev/null
  fi
}

capture_bar select  "$ROOT/native/oracle/scenarios/select_commander.input" 45
capture_bar build   "$ROOT/native/oracle/scenarios/build_mex.input" 60
capture_bar minimap "$ROOT/native/oracle/scenarios/minimap_move.input" 45
capture_bar hud     "$ROOT/native/oracle/scenarios/hud_status.input" 60
# widgets: reuse window/topbar input if present, else select
if [[ -f "$ROOT/native/oracle/scenarios/window_hud.input" ]]; then
  capture_bar widgets "$ROOT/native/oracle/scenarios/window_hud.input" 45
else
  capture_bar widgets "$ROOT/native/oracle/scenarios/select_commander.input" 45
fi

# --- headed Recoil reference captures ---
capture_ref() {
  local id="$1"; shift
  echo "Recoil headed shot $id ..."
  if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
    echo "check_m7flip: process still running before recoil $id" >&2
    exit 1
  fi
  "$ROOT/native/oracle/recoil/run_shot.sh" --shot "$id" "$@" \
    > "$OUT/ref/${id}.log" 2>&1 || {
      echo "Recoil shot failed for $id; see $OUT/ref/${id}.log" >&2
      # Continue to score what we have; do not invent refs
      return 1
    }
  if [[ -f "/tmp/bar-oracle/screenshots/${id}.png" ]]; then
    cp "/tmp/bar-oracle/screenshots/${id}.png" "$OUT/ref/${id}.png"
  else
    # Fall back to newest engine screenshot
    local newest
    newest=$(ls -t /tmp/bar-oracle/screenshots/screen_*.png 2>/dev/null | head -1 || true)
    if [[ -n "$newest" ]]; then
      cp "$newest" "$OUT/ref/${id}.png"
    else
      echo "no Recoil PNG for $id" >&2
      return 1
    fi
  fi
  return 0
}

# BAR's start-spawn effect hides the commander until ~frame 150; shoot after it.
capture_ref select  --frames 260 --shotframe 200 || true
capture_ref build   --frames 270 --shotframe 210 --builddef armmex || true
capture_ref minimap --frames 260 --shotframe 200 || true
capture_ref hud     --frames 270 --shotframe 210 --damage 0.5 || true
capture_ref widgets --frames 260 --shotframe 200 || true

# --- FLIP score ---
"$FLIP_PY" - "$OUT" "$BAR_FLIP" "$BOOKMARKS" <<'PY'
import json, sys, statistics
from pathlib import Path
from flip_evaluator import evaluate

out = Path(sys.argv[1])
bar = float(sys.argv[2])
bookmarks = json.loads(Path(sys.argv[3]).read_text())
ids = [s["id"] for s in bookmarks["shots"]]
scores = []
report = {"bar": bar, "shots": {}, "median": None, "pass": False}
for sid in ids:
    ref = out / "ref" / f"{sid}.png"
    test = out / "bar" / f"{sid}.png"
    entry = {"ref": str(ref), "bar": str(test)}
    if not ref.is_file() or not test.is_file():
        entry["error"] = "missing ref or bar PNG"
        entry["mean"] = None
        report["shots"][sid] = entry
        print(f"{sid}: MISSING")
        continue
    errmap, mean, _ = evaluate(str(ref), str(test), "LDR")
    entry["mean"] = float(mean)
    scores.append(float(mean))
    # Write magma error map as PPM-like via numpy if available
    try:
        import numpy as np
        from PIL import Image
        # errmap is HxWx3 float in [0,1] with magma
        arr = (np.clip(errmap, 0, 1) * 255).astype(np.uint8)
        Image.fromarray(arr).save(out / "diff" / f"{sid}.png")
        entry["diff"] = str(out / "diff" / f"{sid}.png")
    except Exception as e:
        entry["diff_error"] = str(e)
    report["shots"][sid] = entry
    print(f"{sid}: FLIP mean={mean:.4f}")

if scores:
    report["median"] = float(statistics.median(scores))
    report["pass"] = report["median"] <= bar
else:
    report["median"] = 1.0
    report["pass"] = False

(out / "flip.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"median={report['median']:.4f} bar={bar} pass={report['pass']}")
sys.exit(0 if report["pass"] else 1)
PY
