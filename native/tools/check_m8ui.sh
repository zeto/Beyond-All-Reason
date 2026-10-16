#!/bin/bash
# Self-check for live-input building and state orders through the hosted widgets.
# Clicks from scenarios/ui_click.ui take the live mouse path (widgets first, then build
# placement, selection, orders): ordermenu Fire at will -> Hold fire, gridmenu armmex pick,
# ground click places it (nanoframe), a second pick is dropped by a right click, and the
# ordermenu move state cycles Hold pos -> Maneuver. Exit 0 only when all of that happens.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m8ui"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m8ui: bar-metal or spring is already running" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 45 \
  --lock-camera \
  --input "$ROOT/native/oracle/scenarios/flip_select.input" \
  --ui-input "$ROOT/native/oracle/scenarios/ui_click.ui" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/ui.ppm" \
  | tee "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, json, re
from pathlib import Path
out = Path(sys.argv[1])
log = (out / "run.log").read_text()
def need(pattern, what):
    assert re.search(pattern, log, re.M), "missing " + what
need(r"^ui press bar_ui_order at 309 87 button 1$", "ordermenu took the fire state click")
need(r"^unit state 1 cmd 45 = 0$", "fire state Hold fire (engine 0) on the commander")
need(r"^ui press bar_ui_grid at 35 170 button 1$", "gridmenu took the build click")
need(r"^build pick armmex cmd -\d+$", "gridmenu SetActiveCommand(armmex)")
need(r"^build place armmex at ", "ground click placed the build")
need(r"^simpleai build armmex unit 1 nano \d+", "nanoframe for the placed mex")
need(r"^ui cancel active -\d+$", "right click dropped the second pick")
need(r"^unit state 1 cmd 50 = 1$", "move state Maneuver on the commander")
assert len(re.findall(r"^build place ", log, re.M)) == 1, "right click must not place"
build = json.loads((out / "build.json").read_text())
assert int(build.get("building", 0)) > 0, ("no nanoframe id", build)
assert int(build.get("placing", 1)) == 0, ("placement still active after the right click", build)
print("m8ui self-check ok")
PY
