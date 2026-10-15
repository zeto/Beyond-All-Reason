#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7hud"
MAPDIR="${MAPDIR:-$HOME/Library/Application Support/Beyond-All-Reason-mac/maps}"
BIN="$ROOT/native/build/bar-metal"

if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7hud: bar-metal or spring is already running" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --mapdir "$MAPDIR" \
  --frames 60 \
  --lock-camera \
  --input "$ROOT/native/oracle/scenarios/hud_status.input" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/hud.ppm" \
  | tee "$OUT/run.log"

python3 - "$OUT" <<'PY'
import sys, json, re, math
from pathlib import Path
out = Path(sys.argv[1])
report = json.loads((out / "hud.json").read_text())
assert int(report.get("hover", 0)) == 1, report
assert report.get("tooltip") == "armcom", report
assert float(report.get("maxHealth", 0)) > 100, report
assert abs(float(report["health"]) - 0.5 * float(report["maxHealth"])) < 1.0, report
assert abs(float(report.get("range", 0)) - 300) < 1.0, report
assert abs(float(report.get("barFrac", 0)) - 0.5) < 0.05, report
log = (out / "run.log").read_text()
assert re.search(r"hud hover unit 1 tooltip armcom", log), "missing hover"
assert re.search(r"hud damage unit 1 hp ", log), "missing damage"
assert "tooltip DrawScreen:" not in log, "tooltip DrawScreen failed"

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
    rgb = data[i:]
    assert w == 1280 and h == 720, (path.name, w, h)
    assert len(rgb) >= w * h * 3
    return w, h, rgb

def px(rgb, w, h, x, y):
    x = max(0, min(w - 1, int(round(x))))
    sy = h - 1 - max(0, min(h - 1, int(round(y))))
    o = (sy * w + x) * 3
    return rgb[o], rgb[o + 1], rgb[o + 2]

def is_orange(r, g, b):
    return r > 140 and g < 120 and b < 100 and r > g + 30 and r > b + 40

def is_green_fill(r, g, b):
    return g > 120 and g > r + 40 and g > b + 40

def is_flat_grey(r, g, b):
    # UI chrome is flat mid-dark grey; rocky cliffs are darker/noisier.
    return abs(r - g) < 12 and abs(g - b) < 12 and 40 <= r <= 100

for name in ("hud.ppm", "hud-end.ppm"):
    path = out / name
    w, h, rgb = load_ppm(path)
    magenta = sum(1 for px_i in range(0, w * h * 3, 3)
                  if rgb[px_i] > 200 and rgb[px_i + 1] < 40 and rgb[px_i + 2] > 200)
    assert magenta == 0, (name, magenta)

w, h, rgb = load_ppm(out / "hud-end.ppm")
ux, uy = 640.0, 371.0

# 1) Range ring: thin orange band around projected 300-radius (spread, not a filled blob).
orange = []
for y in range(h):
    row = y * w * 3
    for x in range(w):
        o = row + x * 3
        r, g, b = rgb[o], rgb[o + 1], rgb[o + 2]
        if is_orange(r, g, b):
            orange.append((x, h - 1 - y))
assert 800 <= len(orange) <= 30000, ("ring orange count", len(orange))
xs = [p[0] for p in orange]
ys = [p[1] for p in orange]
assert max(xs) - min(xs) > 400, ("ring not wide enough", min(xs), max(xs))
dists = sorted(math.hypot(x - ux, y - uy) for x, y in orange)
med = dists[len(dists) // 2]
assert med > 100, ("ring too close to unit / filled ellipse", med)
# Samples from the report should land near the orange band (allow generous radius).
samples = [s for s in (report.get("ringSamples") or []) if 0 <= s[0] < w and 0 <= s[1] < h]
near = 0
for sx, sy in samples:
    for dx in range(-14, 15):
        hit = False
        for dy in range(-14, 15):
            if dx * dx + dy * dy > 196:
                continue
            r, g, b = px(rgb, w, h, sx + dx, sy + dy)
            if is_orange(r, g, b) or (r > 100 and r > g + 15 and r > b + 20 and b < 90):
                near += 1
                hit = True
                break
        if hit:
            break
assert near >= max(1, len(samples) // 4) or len(samples) == 0, ("ring samples miss band", near, len(samples))

# 2) Health bar fill ~50%.
bl = float(report["barLeft"])
br = float(report["barRight"])
bb = float(report["barBottom"])
bt = float(report["barTop"])
track_w = br - bl
assert track_w > 40, track_w
assert abs(float(report["barFillRight"]) - bl - 0.5 * track_w) < 0.15 * track_w, report
mid_y = 0.5 * (bb + bt)
fill_green = sum(1 for t in (0.15, 0.3, 0.45) if is_green_fill(*px(rgb, w, h, bl + track_w * t, mid_y)))
empty_ok = sum(1 for t in (0.65, 0.8, 0.92) if not is_green_fill(*px(rgb, w, h, bl + track_w * t, mid_y)))
assert fill_green >= 2, ("health fill not ~50% green", fill_green)
assert empty_ok >= 2, ("health empty track still green", empty_ok)

# 3) Tooltip text near cursor.
hx = float(report.get("hoverX", 640))
hy = float(report.get("hoverY", 360))
tip_hits = 0
for y in range(int(hy - 40), int(hy + 100)):
    for x in range(int(hx - 40), int(hx + 280)):
        if x < 0 or y < 0 or x >= w or y >= h:
            continue
        r, g, b = px(rgb, w, h, x, y)
        if r > 180 and g > 180 and b > 170:
            tip_hits += 1
        elif r > 160 and g > 200 and b > 160:
            tip_hits += 1
assert tip_hits >= 30, ("no tooltip text pixels near cursor", tip_hits, hx, hy)

# 4) No large flat empty UI panel mid-left (ignore rocky cliffs via uniformity).
flat_panel = 0
win = 40
for y0 in range(int(h * 0.28), int(h * 0.62), win):
    for x0 in range(8, int(w * 0.26), win):
        cells = [px(rgb, w, h, x0 + dx, y0 + dy) for dy in range(0, win, 4) for dx in range(0, win, 4)]
        if len(cells) < 10:
            continue
        if not all(is_flat_grey(r, g, b) for r, g, b in cells):
            continue
        mean = sum(c[0] for c in cells) / len(cells)
        var = sum((c[0] - mean) ** 2 for c in cells) / len(cells)
        if var < 40:
            flat_panel += 1
assert flat_panel == 0, ("large empty flat grey panel mid-left", flat_panel)

# 5) No grey vertical strips on minimap left.
mmap = json.loads((out / "minimap.json").read_text())
msx = int(mmap.get("sx", 200))
msy = int(mmap.get("sy", 200))
strip_cols = 0
for x in range(0, min(18, msx)):
    greys = 0
    for y in range(h - msy, h):
        r, g, b = px(rgb, w, h, x, y)
        if is_flat_grey(r, g, b):
            greys += 1
    if greys > msy * 0.5:
        strip_cols += 1
assert strip_cols < 3, ("minimap left grey vertical strips", strip_cols)

print("m7hud self-check ok")
PY
