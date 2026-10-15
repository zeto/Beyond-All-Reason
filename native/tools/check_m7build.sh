#!/bin/bash
# Self-check for M7 build menu and build preview. Exit 0 only when the scripted
# select → pick armmex → place path issues a player build and the lock-camera
# shot has no magenta missing-texture pixels.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7build"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7build: bar-metal or spring is already running" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 90 \
  --lock-camera \
  --input "$ROOT/native/oracle/scenarios/build_mex.input" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/build.ppm" \
  | tee "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, json, re
from pathlib import Path
out = Path(sys.argv[1])
build = json.loads((out / "build.json").read_text())
assert build.get("def") == "armmex", build
assert build.get("cmd") == -13, build
assert int(build.get("placing", 0)) == 1, build
assert abs(float(build.get("x", 0)) - 7416) < 1, build
assert abs(float(build.get("z", 0)) - 4888) < 1, build
# GetUnitIsBuilding(commander) must return the nanoframe unit id while building.
assert int(build.get("building", 0)) > 0, ("missing nano id", build)
assert int(build.get("buildingNow", 0)) == int(build.get("building")), build
assert int(build.get("builder", 0)) == 1, build
# Team rules param set by game_initial_spawn must be readable from the widget side.
assert build.get("startUnit") is not None, ("missing startUnit team rules", build)
assert int(build.get("startUnit")) > 0, build
sel = json.loads((out / "select.json").read_text())
assert sel.get("sel") == [1], sel
log = (out / "run.log").read_text()
assert re.search(r"select box .* ids 1 cursor cursormove", log), "missing box select"
assert re.search(r"build pick armmex", log), "missing pick"
assert re.search(r"player build armmex unit 1 at 7416 4888", log), "missing player build"
assert re.search(r"simpleai build armmex unit 1 nano \d+", log), "missing nano id in simpleai build"
assert "sim frame 90 at" in log
for name in ("build.ppm", "build-end.ppm"):
    path = out / name
    data = path.read_bytes()
    i = 0
    lines = []
    while len(lines) < 3:
        j = data.find(b"\n", i)
        line = data[i:j]
        i = j + 1
        if line.startswith(b"#"):
            continue
        lines.append(line)
    w, h = map(int, lines[1].split())
    rgb = data[i:]
    assert w == 1280 and h == 720, (name, w, h)
    magenta = 0
    for px in range(0, w * h * 3, 3):
        r, g, b = rgb[px], rgb[px + 1], rgb[px + 2]
        if r > 200 and g < 40 and b > 200:
            magenta += 1
    assert magenta == 0, (name, magenta)
print("m7build self-check ok")
PY
