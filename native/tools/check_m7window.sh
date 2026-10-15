#!/bin/bash
# Unlocked window (real drawable, not lock-camera 1280x720). Exercises Retina-like
# ViewResize and the interactive HUD path: minimap marker, top bar, ground ring.
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
  --frames 60 \
  --input "$ROOT/native/oracle/scenarios/window_hud.input" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/window.ppm" \
  | tee "$OUT/run.log"

# Second short run for top-bar stability across two end frames (f30 + f45 via screenshot-dir).
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 75 \
  --input "$ROOT/native/oracle/scenarios/window_hud.input" \
  --screenshot-dir "$OUT/shots" \
  --trace "$OUT/boot2.jsonl" \
  | tee -a "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, re, json, math
from pathlib import Path
out = Path(sys.argv[1])
log = (out / "run.log").read_text()
assert "attempt to perform arithmetic on local" not in log
assert re.search(r"drawable: (\d+)x(\d+)", log)
m = re.search(r"drawable: (\d+)x(\d+)", log)
dw, dh = int(m.group(1)), int(m.group(2))
assert dw >= 2800 and dh >= 1700, ("expected Retina-like drawable", dw, dh)
assert re.search(r"widgets view (\d+)x(\d+)", log)
assert "select box" in log
sel = json.loads((out / "select.json").read_text())
assert sel.get("sel"), ("expected commander selected for ring check", sel)

def load_ppm(path):
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
    return w, h, data[i:i + w * h * 3]

def px(rgb, w, h, x, y):
    x = max(0, min(w - 1, int(round(x))))
    sy = h - 1 - max(0, min(h - 1, int(round(y))))
    o = (sy * w + x) * 3
    return rgb[o], rgb[o + 1], rgb[o + 2]

# End-frame screenshot (unlocked size).
end = out / "window-end.ppm"
assert end.exists(), end
w, h, rgb = load_ppm(end)
assert w == dw and h == dh, ((w, h), (dw, dh))

mmap = json.loads((out / "minimap.json").read_text())
msx, msy = int(mmap["sx"]), int(mmap["sy"])
# 1) Minimap marker: green-ish pixels inside the minimap panel.
marker_hits = 0
for y in range(h - msy, h):
    for x in range(0, msx):
        r, g, b = px(rgb, w, h, x, y)
        if g > 140 and g > r + 40 and g > b + 40:
            marker_hits += 1
assert marker_hits >= 8, ("no minimap unit marker", marker_hits, msx, msy)

# 2) No oversized top-bar rectangle: contiguous grey under the pods must stay
#    within a thin strip (topbar is ~vsy*0.0425 ≈ 78px at 1846).
top_flat = 0
for y in range(h - 1, max(0, h - int(h * 0.35)), -1):
    row_grey = 0
    for x in range(int(w * 0.30), int(w * 0.85), 3):
        r, g, b = px(rgb, w, h, x, y)
        if abs(r - g) < 18 and abs(g - b) < 18 and 25 <= r <= 100 and b <= r + 10:
            row_grey += 1
    if row_grey > (w * 0.55) / 3 * 0.40:
        top_flat += 1
    elif top_flat > 0:
        break  # end of contiguous top grey band
assert top_flat <= int(h * 0.10), ("oversized top bar grey slab", top_flat, h)

# Top bar itself should occupy a thin strip near the top (reuse top_flat).
topbar_rows = top_flat
assert 4 <= topbar_rows <= int(h * 0.10), ("top bar height odd", topbar_rows, h)

# 3) Selection ring occluded by unit: sample near unit center — should NOT be
#    bright screen-space green ring overlay (no thick elliptical HUD ring).
#    Ground ring may show at feet; center of unit body should be dark model pixels.
cx, cy = w // 2, int(h * 0.42)
body = [px(rgb, w, h, cx + dx, cy + dy) for dx in range(-6, 7, 3) for dy in range(-6, 7, 3)]
# Screen-space ring was neon green (0.2,1,0.35); body should not be dominated by that.
neon = sum(1 for r, g, b in body if g > 180 and g > r + 80 and b < 120)
assert neon <= 2, ("selection ring cuts through unit body", neon, body[:5])

# 4) Top bar panel stable: dark chrome must stay put (no rotating background square).
#    Compare only dark panel pixels (skip bright text/bars and translucent bleed).
shots = sorted((out / "shots").glob("f*.ppm"))
assert len(shots) >= 2, shots
w1, h1, a = load_ppm(shots[0])
w2, h2, b = load_ppm(shots[-1])
assert (w1, h1) == (w2, h2)
# Image y=0 is top. Top ~90px covers the resource pods at Retina size.
diff = 0
n = 0
wind_white = [0, 0]
for yi in range(0, min(90, h1)):
    for x in range(int(w1 * 0.30), int(w1 * 0.75)):
        o = (yi * w1 + x) * 3
        r, g, bv = a[o], a[o + 1], a[o + 2]
        r2, g2, b2 = b[o], b[o + 1], b[o + 2]
        # Wind region bright solid (rotating square was near-white)
        if x > int(w1 * 0.58):
            if r + g + bv > 500:
                wind_white[0] += 1
            if r2 + g2 + b2 > 500:
                wind_white[1] += 1
        if not (abs(r - g) < 12 and abs(g - bv) < 12 and 22 <= r <= 55):
            continue
        if not (abs(r2 - g2) < 12 and abs(g2 - b2) < 12 and 22 <= r2 <= 55):
            continue
        diff += abs(r - r2) + abs(g - g2) + abs(bv - b2)
        n += 1
mean_diff = diff / max(1, n)
assert mean_diff < 25.0, ("top bar panel unstable / rotating background", mean_diff, n)
# Rotating solid square moved a large bright footprint across frames. Text may be
# absent on an early shot; only fail when both frames show a big bright blob that jumps.
if min(wind_white) >= 200 and abs(wind_white[0] - wind_white[1]) >= 800:
    raise AssertionError(("rotating wind square", wind_white))

print("m7window self-check ok", f"drawable {dw}x{dh}", f"marker {marker_hits}", f"topbar_rows {topbar_rows}", f"stable {mean_diff:.2f}")
PY
