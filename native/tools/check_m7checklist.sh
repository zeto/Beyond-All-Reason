#!/bin/bash
# M7 player checklist self-check. Exit 0 only when every plan checklist item is true
# and sim-side Recoil oracle cmd/facing traces match.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/native/oracle/out/m7checklist"
BIN="$ROOT/native/build/bar-metal"
if pgrep -x bar-metal >/dev/null || pgrep -x spring >/dev/null; then
  echo "check_m7checklist: bar-metal or spring is already running" >&2
  exit 1
fi
mkdir -p "$OUT"
rm -f "$OUT"/*
"$BIN" \
  --startscript "$ROOT/native/oracle/scenarios/boot.txt" \
  --input "$ROOT/native/oracle/scenarios/m7_checklist.input" \
  --trace "$OUT/boot.jsonl" \
  --screenshot "$OUT/checklist.ppm" \
  --lock-camera \
  --frames 120 \
  >"$OUT/run.log" 2>&1

python3 - "$OUT" "$ROOT/native/oracle/traces" <<'PY'
import json, re, sys
from pathlib import Path
out = Path(sys.argv[1])
traces = Path(sys.argv[2])
log = (out / "run.log").read_text()
check_path = out / "checklist.json"
assert check_path.exists(), f"checklist.json missing in {list(out.iterdir())}"
report = json.loads(check_path.read_text())
required = [
    "click_select", "drag_box", "shift_queue", "attack", "facing",
    "control_groups", "pause", "gamespeed", "reclaim", "repair",
]
print("checklist report:", json.dumps(report, indent=2))
assert re.search(r"select click .* ids 1", log), "missing click select"
assert re.search(r"select box .* ids 1", log), "missing drag box"
assert "group set 1" in log and "group recall 1" in log, "missing control groups"
assert re.search(r"order move .* shift", log), "missing shift queue"
assert "build facing" in log, "missing facing"
assert "pause 1" in log and "pause 0" in log, "missing pause"
assert "gamespeed 4" in log, "missing gamespeed"
assert re.search(r"order attack .* cmd 20", log), "missing attack"
assert re.search(r"order reclaim .* cmd 90", log), "missing reclaim"
assert re.search(r"order repair .* cmd 40", log), "missing repair"
failed = [k for k in required if not report.get(k)]

def last_with(path, key):
    rows = [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
    for row in reversed(rows):
        if key in row:
            return row
    return rows[-1] if rows else {}

assert report.get("queued_waypoints", 0) >= 2
assert (traces / "shift_queue.jsonl").exists(), "missing shift_queue trace"
att = last_with(traces / "attack.jsonl", "cmd")
assert int(att.get("cmd", 0)) == 20, att
assert report.get("last_attack_target", 0) > 0
rec = last_with(traces / "reclaim.jsonl", "cmd")
assert int(rec.get("cmd", 0)) == 90, rec
rep = last_with(traces / "repair.jsonl", "cmd")
assert int(rep.get("cmd", 0)) == 40, rep
fac = last_with(traces / "facing.jsonl", "facing")
assert int(fac.get("facing", -1)) == 2, fac
assert int(report.get("last_facing", -1)) == 2
assert (traces / "pause_speed.jsonl").exists()
clk = last_with(traces / "click_select.jsonl", "cmd")
assert int(clk.get("cmd", 0)) == 10, clk
print("oracle checklist: attack", att.get("cmd"), "reclaim", rec.get("cmd"),
      "repair", rep.get("cmd"), "facing", fac.get("facing"), "ok")
print("oracle note: drag_box + control_groups are self-only (client UI)")

if failed:
    print("FAIL items:", failed)
    sys.exit(1)
print("m7checklist self-check ok")
PY
