# M7 Recoil traces

Recorded with `native/oracle/recoil/run_trace.sh` against the PIN spring
binary (`2026.07.04`) and game `Beyond All Reason test-31578-8247b61`.
The oracle mutator gadget writes `trace.jsonl` into the Spring write dir.

| Scenario | Driver in Recoil | Trace file |
|---|---|---|
| select_commander | `CMD.MOVE` at frame 30 to 8000,4900; sel = ordered commander | `traces/select_commander.jsonl` |
| build_mex | build order `-UnitDefNames.armmex.id` at 7416,4888 on frame 30 | `traces/build_mex.jsonl` |
| minimap_move | **No minimap click in Recoil.** Equivalent `CMD.MOVE` at frame 25 to 8000,4900 | `traces/minimap_move.jsonl` |
| health_range | `SetUnitHealth(0.5 * maxHealth)` at frame 35; samples health/maxHealth/maxWeaponRange | `traces/health_range.jsonl` |

boot and move_kbot are unchanged.
