#!/bin/bash
# Exercises the unlocked-window ViewResize path (real drawable size, not the
# lock-camera 1280x720 constant). Catches gui_fonthandler vsy-nil crashes that
# the locked m7 checks never hit. Interactive fullscreen uses the same resize.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7window"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7window: bar-metal or spring is already running" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 45 \
  --trace "$OUT/boot.jsonl" \
  | tee "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, re
from pathlib import Path
out = Path(sys.argv[1])
log = (out / "run.log").read_text()
assert "sim:" not in log or "attempt to perform arithmetic on local" not in log, "fonthandler/view crash"
assert "gui_fonthandler" not in log or "attempt to" not in log, log[-500:]
assert re.search(r"drawable: (\d+)x(\d+)", log), "missing drawable size"
m = re.search(r"drawable: (\d+)x(\d+)", log)
dw, dh = int(m.group(1)), int(m.group(2))
assert dw >= 640 and dh >= 360, (dw, dh)
assert re.search(r"widgets view (\d+)x(\d+)", log), "missing widgets view resize"
vm = re.search(r"widgets view (\d+)x(\d+)", log)
vw, vh = int(vm.group(1)), int(vm.group(2))
assert vw == dw and vh == dh, ((vw, vh), (dw, dh))
assert "presented 45" in log or re.search(r"presented 45\b", log), "did not present 45 frames"
assert "gadget spawn" in log
print("m7window self-check ok", f"drawable {dw}x{dh}")
PY
