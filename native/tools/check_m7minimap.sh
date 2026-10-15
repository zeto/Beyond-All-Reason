#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7minimap"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7minimap: bar-metal or spring is already running" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 60 \
  --lock-camera \
  --input "$ROOT/native/oracle/scenarios/minimap_move.input" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/minimap.ppm" \
  | tee "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, json, re
from pathlib import Path
out = Path(sys.argv[1])
report = json.loads((out / "minimap.json").read_text())
assert int(report.get("minimized", 1)) == 0, report
assert int(report.get("sx", 0)) > 100, report
assert int(report.get("sy", 0)) > 100, report
assert int(report.get("slave", 0)) == 1, report
assert abs(float(report["worldX"]) - 8000) < 250, report
assert abs(float(report["worldZ"]) - 4900) < 250, report
assert int(report.get("moved", 0)) == 1, report
log = (out / "run.log").read_text()
assert re.search(r"select box .* ids 1", log), "missing select"
assert re.search(r"minimap click .* world ", log), "missing minimap click"
assert re.search(r"order move ", log), "missing move order"
for name in ("minimap.ppm", "minimap-end.ppm"):
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
    magenta = sum(1 for px in range(0, w*h*3, 3) if rgb[px]>200 and rgb[px+1]<40 and rgb[px+2]>200)
    assert magenta == 0, (name, magenta)
print("m7minimap self-check ok")
PY
