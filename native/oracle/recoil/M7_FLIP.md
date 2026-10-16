# M7 HUD FLIP bookmarks

Plan exit (`native/PLAN.md`): checklist complete + HUD bookmarks under
`m7_flip` 0.22. Shared defs: `native/oracle/hud_bookmarks.json`.

## Headed Recoil capture

Works without `-hidden`, with Zink+MoltenVK, `MacRenderScale=1`,
`SPRING_MAC_NO_RETINA=1`. Oracle mutator only (`/tmp/bar-oracle`).

### Camera lock (checkpoint)

`shot_camera_widget.lua` locks every frame to the bar-metal
`--lock-camera` pose from `native/map/camera.cpp`:

- Spring **spring** cam (`mode=2`), not free (free reports correct eye
  but renders purple void on this Mac/Zink path).
- `rx = π - tilt` (tilt 1 → rx ≈ 2.142, inside Spring’s
  `[0.51π, 0.99π]` clamp in `SpringController.cpp`).
- `ry = 0`, `dist = 400`, focus at commander xz.
- Eye XZ matches want within 1 elmo; eye Y is ~58 elmos high (Spring
  focus sits on ground height, not unit_y+30).

Median FLIP after lock: **~0.45** (was ~0.90 with unlocked/free cam).

## Remaining for ≤ 0.22

Host full BAR LuaUI chrome in bar-metal to match the refs (not trim
Recoil). First batch still outstanding:

- `gui_ordermenu`, `gui_info`, `gui_buildmenu`
- `gui_top_bar_buttons`, `gui_idle_builders`
- chat/voice/console (`gui_chat` and related)

Already loaded in `widget_host.cpp`: fonthandler, flowui, top_bar,
selectionbox, gridmenu, tooltip, relative, minimap, volume, cursor,
screen_mode_info.

Add Spring/gl only as widgets hit them; no stubs. GL4 via `rhi/metal4`.

## Scoring

`native/tools/check_m7flip.sh` + `flip-evaluator` in
`native/tools/.flip-venv`. `gate --suite m7` reads
`oracle/out/m7flip/flip.json`. Do not lower `m7_flip`.
