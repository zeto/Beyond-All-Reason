# M7 HUD FLIP bookmarks

Plan exit (`native/PLAN.md`): checklist complete + HUD bookmarks under
`m7_flip` 0.22. Shared bookmark defs: `native/oracle/hud_bookmarks.json`.

## Headed Recoil capture

The installed BAR Launcher spring **can** capture headed reference PNGs at
fixed bookmarks when launched without `-hidden`, with Zink+MoltenVK, and
with `MacRenderScale=1` / `SPRING_MAC_NO_RETINA=1` for 1280×720 pixels.

Oracle-only (not stock tree):

- `shot_gadget.lua` — select/build/damage HUD state + `gl.SaveImage`
- `shot_camera_widget.lua` — free-cam lock toward bar-metal commander pose
- `run_shot.sh` — packs mutator into `/tmp/bar-oracle` write-dir

## Scoring

`native/tools/check_m7flip.sh` captures bar-metal lock-camera PNGs and
headed Recoil refs into `native/oracle/out/m7flip/{bar,ref}/`, scores with
NVIDIA `flip-evaluator` (venv at `native/tools/.flip-venv`, not vendored
into the frame path), writes `flip.json`.

`gate --suite m7` reads that report; median must be ≤ `m7_flip` (0.22).

## Current blocker (do not loosen the bar)

Median FLIP is ~0.90. Fixes needed before a green gate:

1. Recoil free-cam lock still fails to frame the commander like bar-metal
   `--lock-camera` (world often purple/void or wrong look). Spring camera
   modes clamp or drift; needs a reliable lock that matches
   `native/map/camera.cpp` eye/target.
2. Full BAR LuaUI chrome (bottom panels, voice, menus) vs bar-metal’s
   native HUD reimplementation dominate the error map even when the 3D
   view is closer. Layout parity work on bar-metal HUD (or an oracle-only
   Recoil widget set that matches the five layers) is required.

Do not lower `m7_flip`. Do not enter M8 on a red m7.
