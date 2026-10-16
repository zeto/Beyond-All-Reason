# M7 Recoil traces

Recorded with `native/oracle/recoil/run_trace.sh` against PIN spring
`2026.07.04` / game `Beyond All Reason test-31578-8247b61`.

## Widget-layer (m7widgets)

| Scenario | Driver | Trace |
|---|---|---|
| select_commander | CMD.MOVE @30 to 8000,4900 | `traces/select_commander.jsonl` |
| build_mex | build armmex @7416,4888 | `traces/build_mex.jsonl` |
| minimap_move | **No minimap click.** Equivalent CMD.MOVE @25 | `traces/minimap_move.jsonl` |
| health_range | SetUnitHealth 50% @35 | `traces/health_range.jsonl` |

## Checklist (sim-side)

| Scenario | Driver | Trace |
|---|---|---|
| click_select | CMD.MOVE (sel = ordered commander) | `traces/click_select.jsonl` |
| shift_queue | MOVE then MOVE+shift | `traces/shift_queue.jsonl` |
| attack | CreateUnit enemy then CMD.ATTACK | `traces/attack.jsonl` |
| pause_speed | pause + setterspeed | `traces/pause_speed.jsonl` |
| reclaim | CMD.RECLAIM at xz | `traces/reclaim.jsonl` |
| repair | CreateUnit ally then CMD.REPAIR | `traces/repair.jsonl` |
| facing | build facing echo | `traces/facing.jsonl` |

Self-only (no Recoil oracle): drag_box geometry, control_groups 1–9 (client UI).
boot and move_kbot unchanged.
