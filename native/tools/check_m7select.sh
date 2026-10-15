#!/bin/bash
# Self-check for M7 selection and order cursors. Exit 0 only when the scripted
# box-select, move order, cursor state, and lock-camera shot all look right.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7select"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7select: bar-metal or spring is already running" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 60 \
  --lock-camera \
  --input "$ROOT/native/oracle/scenarios/select_commander.input" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/select.ppm" \
  | tee "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, json, re
from pathlib import Path
out = Path(sys.argv[1])
report = json.loads((out / "select.json").read_text())
assert report.get("sel") == [1], report
assert report.get("cursor") == "cursormove", report
assert report.get("cmd") == 10, report
log = (out / "run.log").read_text()
assert re.search(r"select box .* ids 1 cursor cursormove", log), "missing box select"
assert re.search(r"order move 8000 4900 unit 1 waypoints", log), "missing move order"
assert "sim frame 60 at" in log
# Commander must have left the spawn after the move.
m = re.search(r"sim frame 60 at ([0-9.]+)", log)
assert m and float(m.group(1)) > 7605.0, m
for name in ("select.ppm", "select-end.ppm"):
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
print("m7select self-check ok")
PY
