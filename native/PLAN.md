# Native Beyond All Reason on macOS 27

A single-player, Apple Silicon, Metal 4 client that plays this repository as the game. The runtime graphics path is Metal 4. The simulation is a job system whose hot loops are data-oriented C++. The game rules stay the Lua that already is Beyond All Reason. A scored gate, not a screenshot review, decides when each slice is done.

This file is the execution spec. A later session implements it by following the supervisor protocol in "The loop". It does not widen scope on its own.

## What is already true

This repository is the game, not the engine. Measured from the tree:

- 2,270 Lua files, 327 gadgets, 326 widgets, about 385,000 lines of gadget and widget code
- 969 unit definition files, 32 weapon files, 2,039 `.s3o` models
- 853 compiled COB animation scripts, 851 BOS sources, 29 Lua unit scripts
- 78 GLSL shaders in the game, 3 of them geometry shaders
- 590 distinct `Spring.*` functions (13,401 call sites) and 141 distinct `gl.*` functions (5,736 call sites)

The engine is Recoil, a GPL C++ RTS engine in a separate repository. Its main loop still runs input, the simulation tick, and the world draw on one thread. Pathfinding (QTPFS) is already multi-threaded. Unit update, weapons, projectiles, collisions, and Lua `GameFrame` are the single-thread ceiling people hit in large games.

A native Apple Silicon Recoil build already exists (community forks). It draws through OpenGL 4.6, Mesa Zink, then Vulkan, then MoltenVK or KosmicKrisp, and only then Metal. KosmicKrisp needs Metal 4 and macOS 26 or later. That stack is a translation layer on the frame path. It is out of scope for this project, including as a temporary renderer. Upstream Recoil issue #2869 (March 2026) is an SDL_GPU abstraction discussion, not a Metal 4 client.

macOS 27 is Apple Silicon only. The binary is arm64. A linkage gate rejects any build that pulls in OpenGL, Vulkan, MoltenVK, Mesa, Zink, KosmicKrisp, or Wine.

## Decision

Build a new process, `bar-metal`, in this repo under `native/`. It mounts the existing game directory and the player's existing map archive. It does not fork unit stats, models, or textures into a second game.

Language split:

- C++23 for the simulation, virtual file system, definition parser, COB virtual machine, and Lua host. Recoil's numeric rules are C++. Porting a pure function and checking it against a trace is the shortest path to the same game.
- Objective-C++ only for the process shell: `NSApplication`, `CAMetalLayer`, menus, and file dialogs. Metal 4 types do not leak past `native/rhi/`.
- Metal Shading Language for shaders, compiled by Apple's `metal` compiler into a Metal 4 pipeline library.
- The game stays Lua 5.1, the dialect these gadgets already are. PUC Lua 5.1, statically linked. LuaJIT is a later option only if a perf gate names the interpreter as the limiter.

A from-scratch rewrite of the 385,000 lines of rules would produce a different game. A Metal backend dropped into Recoil would keep the single-threaded `CUnit::Update` monolith and the raw `gl.*` call sites. The new process exists so the data layout and the graphics API can both be the ones this target needs, while the Lua stays the source of gameplay.

Internal reproducibility stays mandatory. Multiplayer lockstep does not. Same binary, same seed, same start script, same trace. Job completion order is not allowed to change results. That is what makes the gate able to correct the program. Cross-machine bit-matching with x86 Recoil is not a goal, which is what makes wide SIMD and a parallel reduction legal.

License: the engine is GPL-2.0-or-later, matching Recoil and this repo's code license. Algorithms and the COB virtual machine may be ported from Recoil with attribution in `native/NOTICE`. Models, textures, and animations in this repo are CC-BY-NC-ND. The client loads them from a BAR data directory. It does not repackage them into a new title.

## Definition of done

A score of at least 90, plus the two hard perf gates. Pixel match is not required.

| Area | Weight | Pass |
| --- | ---: | --- |
| Core loop on 3 maps: expand, build a lab, produce an army, defeat a Lua AI | 25 | all 3 finish with a winner, no crash |
| Unit roster does its job (builds, moves, fires or performs its utility) | 20 | at least 90% of non-holiday unit defs |
| Simulation corpus vs the Recoil oracle, at the final tolerances below | 15 | every scenario green |
| Picture a BAR player recognizes: terrain, models, team color, water, explosions, HUD | 20 | visual gates at the final FLIP bar |
| 4,000 units hold 30 sim Hz, and no thread owns the tick | 15 | both hard perf gates |
| Orders a player needs: select, queue, groups, facing, pause, gamespeed | 5 | checklist |

Holiday gadgets, multiplayer, the lobby, replay exchange with Recoil clients, and spectator tooling are in the allowed remainder. They are listed under "Allowed remainder" and do not count against the roster percentage.

Final simulation tolerances, against a pinned Recoil oracle:

- Scripted unit counts: exact
- Scripted economy at fixed frames: metal and energy within 2%
- Scripted move waypoints: within 8 elmos
- Isolated weapon duels, time to kill: within 15%
- A 10-minute simple-AI skirmish: both sides still playing at frame 18,000, resource totals within 15%, no desync of the native trace against itself

Final visual bar: on the bookmark set, median [FLIP](https://research.nvidia.com/publication/2020-07_FLIP-A-Difference-Evaluator-for-Alternating-Images) at or below 0.15 on at least 80% of shots, and zero shots missing a whole unit category, the terrain, or the resource bar. Water, shadow softness, bloom, and particle shape may differ.

Hard perf gates, on the developer's macOS 27 machine, one frozen late-game save:

1. At 4,000 units, simulation p95 is at or under 33 ms. Doubling from 2,000 to 4,000 units increases p95 by at most 1.6×.
2. During that 4,000-unit tick, no single thread accounts for more than 40% of simulation CPU time, and an 8-worker run is at least 3× the 1-worker run.

If a gate is green by shrinking the scene, the gate is failed.

## Architecture

```
startscript + BAR.sdd + map archive
        │
        ▼
┌─────────────────── bar-metal (arm64) ───────────────────┐
│  Cocoa shell                                            │
│  Lua 5.1 host  ← gadgets, widgets, COB/LUS, AI         │
│       │ commands and events only                        │
│       ▼                                                 │
│  Sim, 30 Hz, job graph                                  │
│    SoA world, chunked                                   │
│    move, weapons, projectiles, LOS, path queue         │
│       │ triple-buffered snapshot                        │
│       ▼                                                 │
│  Render thread                                          │
│    Metal 4 command buffers, encoded on a pool          │
│    world pass + Lua draw-list replay                   │
└─────────────────────────────────────────────────────────┘
```

One sim tick is 1/30 s, the rate BAR is tuned for. The renderer runs at display rate and interpolates the two newest snapshots. A slow sim does not stretch weapon timings.

### World state

Units, features, and projectiles are structure-of-arrays, indexed by dense ids. Stable ids are a side table, because Lua stores unit ids. Chunks are 512 elmos square. Each chunk job reads its interior and a one-chunk halo, and writes only its interior. Cross-chunk damage and collision candidates are appended to per-job event arrays and merged in chunk-id order.

There is no virtual `Unit::Update()`. A unit's per-tick work is a sequence of systems over the arrays: script tick, movement, builders, weapons, death. Systems declare their read and write sets. The scheduler refuses two overlapping writers in the same phase.

Random numbers are a per-system counter keyed by `(seed, frame, system, entity id)`. Containers that affect gameplay are sorted by id before iteration. Debug builds hash the world at the end of each tick of the micro suite and compare to the previous run of the same binary.

### Systems, in the order they run

1. Commands. Player and Lua orders append into a command buffer on the sim thread. Small.
2. Path requests. One job per request. Results land in a waypoint cache. Movement never waits on a search inside the unit loop. A missing path means the unit continues its current waypoint or stops, the same idea as Recoil's raw-move fallback. The first pathfinder is grid A* per move class. If the movement corpus misses the 8-elmo bar because of flow around wrecks and terrain, port QTPFS's hierarchical search into this job queue. The port is justified by that gate, not by familiarity.
3. Movement. Ground, hover, ship, gunship, and airplane are separate systems. They already differ in Recoil (turn radius, strafing, crash). Each is its own slice.
4. Build and reclaim. Nanoframes, build power, metal and energy drain, wreck reclaim. Economy gadgets that duplicate this stay off until the native numbers miss the 2% bar, then the gadget is enabled or the native rule is fixed. The oracle says which.
5. Weapons and projectiles. Each Spring weapon family is a system: cannon ballistics, beam, laser, missile, starburst, torpedo, bomb, lightning, flame, dgun. Projectiles are their own SoA and their own parallel phase. Collision is a chunk broadphase against units and features.
6. Sensors. LOS, air LOS, radar, sonar, jammer, seismic. Grids, updated as tile jobs. Resolutions come from `gamedata/modrules.lua` (`losMipLevel` 3, `airMipLevel` 4, `radarMipLevel` 2).
7. Events. Merged in a stable order, then delivered to Lua as batched callins.
8. Publish. Copy the render-facing arrays into the snapshot the GPU will read. Interpolation data is previous plus current position.

### What stays in Lua, and what is not allowed to

Synced gadgets own rules that are really content: commander spawn, scavengers, raptors, unit-specific weapon exceptions, faction build trees that are not already in the unit defs, the simple AI. Unsynced widgets own the HUD.

Lua is not allowed to scan every unit every frame once M9 starts. The perf classifier enforces this. A gadget over 5% of the sim tick gets one of three treatments, in order:

1. The work is an engine concern (targeting, cloak detection, collision response). Move it into a system. Keep a callin so the gadget still observes the result.
2. The gadget is an accidental O(n) scan. A one-file override in `native/compat/` replaces it with an event-driven version. Each override is a row in `native/compat/README.md` with the reason. The override set is part of the allowed remainder and should stay small.
3. The work is inherently serial and cheap. Leave it.

The VFS prefers `native/compat/` over the stock file, so the stock tree stays clean.

### Lua host

Embed Lua 5.1. Implement `Spring.*`, `gl.*`, `GG`, `Script`, `VFS`, `math` extras, and the callin dispatcher.

Do not implement all 590 Spring functions up front. `native/tools/api_audit.lua` loads every gadget and widget under a logging metatable and writes `native/oracle/api_use.txt`, with functions ranked by how many files touch them. The binding backlog is that file. The first bindings are the ones the audit and the early scenarios actually hit: echo and log, config, game frame, mod options, ground height, unit def and position and health, orders, teams, resources, create unit, selected units, mouse and view geometry, trace ray.

Callins the host must deliver for the scoreboard: `Initialize`, `GameFrame`, `GameStart`, `UnitCreated`, `UnitFinished`, `UnitDestroyed`, `UnitDamaged`, `UnitIdle`, `UnitCommand`, `FeatureCreated`, `FeatureDestroyed`, `GamePreload`, widget `Update` and the draw callins below. More callins are added when a loaded gadget errors on a nil call, and that error is a failed gate, not a warning.

`gl.*` is a retained command list, not an OpenGL context. Widgets keep calling `gl.Color`, `gl.TexRect`, `gl.BeginEnd`, `gl.CreateList`, `gl.CallList`, `gl.CreateShader`, `gl.CreateTexture`, `gl.GetVBO`. The host records those into display lists. At `DrawScreen`, `DrawWorld`, `DrawWorldPreUnit`, and `DrawMiniMap`, the render thread encodes the lists into the current `MTL4RenderCommandEncoder`. Matrix stacks are applied on the CPU into the vertex or into a uniform. Shader source that arrives as GLSL is compiled to MSL at load by the shader compiler tool, then handed to the Metal compiler. The frame path never calls OpenGL.

Geometry shaders have no Metal stage. There are three in this repo (`decals_gl4`, `DrawPrimitiveAtUnit`, `HealthbarsGL4`). Each is rewritten as an instanced draw or a mesh shader when that effect comes on screen. The rewrite is a checked-in MSL file.

Text uses the fonts already in `fonts/` through Core Text glyph atlases. RmlUi, already used by `luaui/RmlWidgets/`, gets a render interface that emits the same command lists. That is how the newer UI comes along without a second HUD toolkit.

### Renderer

`native/rhi/metal4/` is the only place that includes Metal 4 headers.

Startup creates an `MTLDevice`, an `MTL4CommandQueue`, and a pool of `MTL4CommandAllocator`s. Command buffers come from the device and are independent, so several threads encode at once. One compute encoder covers culling, LOD select, instance compaction, light list build, and particle update. Blits that depend on those results sit in that same encoder, with an explicit barrier only where the writes and the reads meet. One render encoder draws the world. Its attachment map points fragment outputs at the G-buffer, then at the light target, then at the resolve target, without allocating a new encoder per pass.

World pass, in order:

- Depth pre-pass and Hi-Z
- Terrain. Clipmap mesh over the SMF heightmap. Diffuse, detail, and normal from the map archive. Metal spots and geo vents are map data, not a special case hacked into the mesh.
- Features and wrecks, instanced
- Units, instanced, team-color replaced in the shader the way s3o team-color texels already work. LOD picked by the cull compute.
- Projectiles and beams
- Water. A planar reflection plus a refraction of the scene color, with a depth-based shore fade. This is the look players recognize. The extra Recoil water modes are in the allowed remainder until a visual bookmark on a sea map fails.
- Shadows. One cascaded shadow map. Softness is free to differ.
- Lighting. Clustered, fed by the compute light list. Sun direction comes from the map's lighting values.
- Lua world draw lists (range rings, build preview, selected-unit overlay)
- HUD draw lists and RmlUi
- Present through the `CAMetalLayer`

Textures live in heaps and a residency set. A unit coming on screen makes its textures resident. A draw does not create a texture. Pipeline states are baked into a pipeline library at load, which is the Metal 4 loading path, so a match does not compile shaders on the first frame.

The render thread never calls into Lua and never takes the sim lock. It reads the published snapshot and the immutable draw lists the widget phase produced at the end of the previous sim tick. Widget draws are therefore one tick late. That is acceptable at 30 Hz.

### Assets

Read-only loaders, each with a byte-level unit test against fixtures copied from this tree (small fixtures, not the ND art set committed twice):

- VFS: directories and sd7/sdz zip archives, the Spring search order
- Start script parser, the format in `tools/StartScripts/`
- Unit, weapon, feature, move, and armor defs. They are Lua tables. Load them in the Lua state and pull the tables across. Do not write a second def language.
- `gamedata/modrules.lua`, already the numeric rules of construction, reclaim, sensors, movement, and experience
- s3o models and their piece tree
- DDS, TGA, and PNG
- SMF height, type map, metal map, minimap, tile archive
- COB bytecode. Port the Recoil interpreter. It is a stack machine with a few dozen opcodes, and it is the animation and aim system for almost every unit. LUS (29 files) runs on the same Lua state.
- Sounds: WAV and OGG into `AVAudioEngine`. Positional audio is a slice after the first playable duel, before the 90 score.

Map pin for the corpus: the map already used by `tools/headless_testing` (Supreme Isthmus v2.1) plus two more the player already has installed, one sea-heavy and one small. The gate stores camera bookmarks in map coordinates so a different resolution still lines up.

### Platform

- macOS 27 SDK, arm64 only, deployment target macOS 27
- Window and input through AppKit. No SDL, so the linkage list stays Apple frameworks plus the static Lua library.
- Game Controller framework for gamepads, after mouse and keyboard match the checklist
- Save a crash log and the last sim hash on an uncaught exception

Linkage gate, run on every binary the loop produces:

```
otool -L native/build/bar-metal
```

Fail if the output mentions GL, Vulkan, MoltenVK, Mesa, zink, kosmickrisp, wine, or SDL. Also fail if `lipo -archs` prints anything other than `arm64`.

## The loop

The loop is the way the program gets written. A slice is done when its gate exits 0. A red gate is the next task. The agent does not start a later milestone to get around a red one.

### Oracles

**Simulation oracle.** A pinned Recoil headless binary, version recorded in `native/oracle/PIN` (current upstream stable at the time of the pin; refresh only on a deliberate pin bump). For each scenario it writes `native/oracle/traces/<scenario>.jsonl`:

```json
{"f":300,"teams":[{"id":0,"m":1040.2,"e":880.0,"mu":12.0,"eu":-8.0}],
 "units":[{"id":14,"def":"armmex","t":0,"x":120.0,"y":8.0,"z":640.0,
           "hd":16384,"hp":320,"bp":1,"cmd":0}],
 "events":[{"e":"created","id":14,"def":"armmex"}]}
```

Dumped every 30 frames, plus every event on the frames where it happens. Scenarios live in `native/oracle/scenarios/` as ordinary Spring start scripts, so Recoil and `bar-metal` launch the same file.

Initial corpus, grown only when a milestone needs a new case:

| Id | What it locks |
| --- | --- |
| `boot` | defs load, map loads, commander spawns, no Lua error |
| `move_ground` | one tank, scripted waypoints |
| `move_kbot` | turn rate and footprint differ from the tank |
| `move_ship` | water depth and shore |
| `move_gunship` | hover height |
| `move_plane` | air turn radius and map exit rules from modrules |
| `eco_t1` | mex, solar, lab, one builder, 90 seconds |
| `build_tank` | lab production and a finished unit |
| `duel_direct` | two units, one weapon family, time to kill |
| `duel_indirect` | cannon arc and scatter |
| `duel_beam` | beam laser |
| `reclaim` | wreck metal returns |
| `los` | a unit outside LOS is not targetable |
| `ai_10min` | simple AI versus simple AI, must still be alive at 10 minutes |
| `crowd_2k`, `crowd_4k` | perf saves, not behavior-compared to Recoil frame by frame |

**Visual oracle.** For each bookmark in `native/oracle/cameras.json`, both engines freeze the sim at a frame, set the same camera, and write a PNG. Reference shots come from the headed Apple Silicon Recoil build once, and are re-shot only on a pin bump. Comparison is FLIP plus two cheap checks: the fraction of pixels near a team color, and a count of unit silhouettes in the frame from the sim trace (so a missing unit fails even when FLIP is dominated by shading).

**Performance oracle.** `crowd_2k` and `crowd_4k`. The native client writes `native/oracle/last_perf.json`: p50 and p95 of sim and render, RSS, and per-thread CPU shares sampled with `os_signpost` ranges around each system. The single-thread share is `(max thread sim time) / (sum of thread sim times)`.

**API oracle.** The audit file. A newly loaded gadget that calls an unbound function fails `boot` with the function name. That name is appended to the binding queue.

### Gate tool

`native/tools/gate` is a small C++ program. It does not call a model. It reads traces, images, and `last_perf.json`, applies the tolerances in `native/oracle/tolerances.toml`, and exits 0 or 1. On failure it writes `native/oracle/last_failure.md` with:

- the scenario id and the first diverging frame
- the entity id, the field, the oracle value, the native value, the tolerance
- for visual failures, the FLIP score and the path of a diff PNG
- for perf failures, the hottest system and, if the samples resolve it, the hottest Lua gadget
- the last 40 lines of `infolog.txt` from both sides

Tolerances only move in a commit that edits `tolerances.toml` and adds a paragraph to `native/oracle/RATCHET.md`. The supervisor is not allowed to loosen a tolerance inside a repair iteration. Tightening is allowed when a slice has been green for a full milestone run.

### Supervisor

State lives in `native/oracle/loop_state.json`: current milestone, current slice, iteration count, last gate result.

One iteration:

1. Read `loop_state.json` and `last_failure.md`. If there is no failure, take the next slice in the current milestone from the milestone list below.
2. Implement only inside that slice's directories, plus tests and the binding files the failure names. A slice lists its directories in the milestone section. Edits outside them are a failed iteration.
3. Build with the Debug preset for behavior slices, RelWithDebInfo for perf slices.
4. Run the slice's micro command. Behavior micros finish in under a minute. Perf micros are the crowd saves and are allowed to take longer.
5. If it fails and the iteration count is under 5, go to step 1 with the new failure packet. The repair reads the packet and changes the system the packet names. It does not retune the tolerance and it does not delete the test.
6. If it fails at iteration 5, stop. Write `native/oracle/escalations/<slice>.md` describing what was tried, and split the slice into two smaller slices in this plan file. Do not continue to the next feature.
7. If the micro is green, run the milestone suite once. Green milestone: advance, reset the iteration count, and append a line to `native/oracle/RATCHET.md` if any tolerance tightened. Red milestone: the new failure becomes the current slice, same 5-iteration budget.

Outer loop, after each milestone from M4 onward: launch `ai_10min` and a scripted camera tour. A crash, a Lua error, a sim p95 over 33 ms on that small game, or a failure to produce a winner classification is an escalation, even if the micro suite is green.

Debug builds run the micro suite under the thread sanitizer once per slice before the slice is marked green. A data race is a failed gate.

### Slice contract

Every slice begins as a short block at the top of its first commit, and as a section in `native/SLICES.md`:

- the behavior a player would notice
- the oracle scenarios that must be green
- the directories it may change
- the systems it may not rewrite
- the tolerance it is scored against

If a slice needs a system it was told not to rewrite, that is an escalation and a plan edit, not a quiet rewrite.

## Milestones

Slices inside a milestone are ordered. The loop runs them in that order.

### M0 — The gate exists before the game

Directories: `native/tools`, `native/oracle`, `native/shell`, `native/rhi/metal4`.

- CMake preset, arm64, macOS 27, a window that clears to a color through `MTL4CommandQueue` and presents a triangle with a hand-written MSL shader.
- Static Lua 5.1 that prints the game name from `modinfo.lua`.
- `api_audit` output checked in.
- Recoil pin captured. `boot` trace captured from the oracle. `gate` runs and returns 1 for an empty client, with a readable failure.
- Linkage gate script.

Exit: `gate` is red for an honest reason, the triangle is on screen, `otool` is clean.

### M1 — A map and a camera

Directories: `native/vfs`, `native/map`, `native/rhi`.

- Mount VFS, parse one start script, load Supreme Isthmus height, shading textures, metal map, minimap.
- Free camera in Spring's overhead control scheme: edge scroll, wheel zoom, middle-drag pan, tilt.
- Visual bookmarks: overview, shore, cliff, metal line. Terrain silhouette only. Units are not scored.

Exit: FLIP on the four terrain bookmarks under the loose M1 bar (0.35, silhouette and biome colors present).

### M2 — One unit, correctly placed

Directories: `native/defs`, `native/model`, `native/sim/spawn`.

- Load unit defs through Lua. Spawn the commander from the start script at the oracle position.
- Load its s3o, team color, pieces. COB idle is allowed to be a bind pose at this milestone.
- Sim trace: one unit, matching def, team, and position within 1 elmo at frame 0.

Exit: `boot` spawn section green, one visual bookmark shows the commander on the metal line.

### M3 — Orders and movement

Directories: `native/sim/command`, `native/sim/move`, `native/sim/path`.

SoA layout and the chunk job scheduler land here, running with one worker. Later milestones add workers. They do not get to invent the layout.

Slices: command queue, ground movement plus grid A*, kbot, ship, hover and gunship, airplane. Then the COB virtual machine, because aim and yard animations are how units look alive. COB is ported as the Recoil opcode set with a test that runs a tank's walk script for 300 frames and checks piece offsets against an oracle dump of piece matrices.

Exit: the five `move_*` scenarios green at 8 elmos. Piece-offset test green for one tank and one kbot.

### M4 — Economy

Directories: `native/sim/economy`, `native/sim/build`, `native/lua` (resource and unit-creation bindings only).

Slices: team metal and energy, mex income on the metal map, solar and wind, a builder constructing a nanoframe, a lab producing the unit from `build_tank`, reclaim of a wreck. Builder nano particles may be a colored quad. The construction gadget rules in modrules (decay, reclaim method, energy cost factors) are read from the file, not hard-coded.

A native debug HUD may draw metal, energy, and a selected unit's name as text. It is a temporary overlay, removed when the real resource bar widget passes its visual gate in M7.

Exit: `eco_t1`, `build_tank`, and `reclaim` green at 2%.

### M5 — Combat

Directories: `native/sim/weapon`, `native/sim/projectile`, `native/sim/los`.

Slices, one weapon family each, each with a duel scenario added to the corpus the first time that family is implemented: direct laser, cannon, beam, missile, lightning, flame, torpedo, bomb, dgun. Then LOS blocking, then wreck creation on death.

Explosions at this milestone are a billboard flash plus a wreck. CEG fidelity is M8.

Exit: each duel scenario within 15% time to kill, `los` green, unit count exact.

### M6 — The real rules

Directories: `native/lua`, `native/compat`.

Turn gadgets on in layers. A layer is green when `boot` and the M3–M5 corpus stay green with those gadgets loaded.

1. Spawn and team setup gadgets required by a normal skirmish (`game_initial_spawn` and the resource setup it depends on).
2. Unit behavior gadgets the duel corpus regresses without. The failure packet names them.
3. `ai_simpleai.lua`, then `ai_10min`.
4. Scavenger and raptor spawners, as their own scenarios, after simple AI is green. They are part of the 90 because they are standard single-player modes. If a mode needs a large unbound API surface, it can sit in the remainder only with an escalation note, and the roster score must still clear 90 without it.

Missing `Spring.*` functions are implemented when a layer hits them, taken from the audit ranking, not guessed.

Exit: `ai_10min` green. Outer loop plays a full game to a winner on the small map.

### M7 — The HUD

Directories: `native/lua/gl`, `native/ui`, `native/compat`.

Command-list `gl.*` in this order, driven by which widgets fail to draw: texture and color, rect and texrect, matrix stack, display lists, fonts, VBO and instancing table, shader load through the MSL compiler, render-to-texture.

Widget layers, each a visual bookmark:

1. Resource bar, selection, and order cursors. The game is playable with these.
2. Build menu and build preview on the terrain.
3. Minimap.
4. Health bars, range rings, tooltip.
5. The remaining widgets the audit marks as loaded by default. Widgets that are off by default and are not needed for the checklist go to the remainder.

Player checklist, scored here: left-click select, drag box, shift-queue, right-click move and attack, build facing with the existing key, control groups 1–9, pause, gamespeed 1–4, reclaim and repair orders.

Exit: checklist complete, HUD bookmarks under the M7 FLIP bar (0.22, layout and information present; font raster may differ).

### M8 — The picture

Directories: `native/rhi`, `native/shaders`, `native/sim/fx`.

Slices: cascaded shadow, clustered light, planar water, cloak as a dithered dissolve, projectile trails, explosion billboards replaced by the highest-traffic CEG patterns (generic unit explosion, plasma hit, laser hit, nuke as a scaled version of the same system). Nano spray as instanced quads. The three former geometry shaders, rewritten.

Default-on `gfx_*` gadgets that issue `gl.CreateShader` go through the shader compiler. A shader the compiler rejects is either hand-ported to MSL or the effect is simplified, and the simplification is a line in `RATCHET.md`.

Exit: the full bookmark set at the final visual bar. This is the 20-point picture score.

### M9 — Use the cores

Directories: `native/sim/job`, the system directories already created. No new gameplay.

Workers default to the number of performance cores. The scheduler runs movement, weapons, projectiles, and sensor tiles as parallel phases with the read and write sets from M3. Path searches are already individual jobs.

Work is driven by the perf packet, in order, and stops when both hard gates are green:

- sort and compact arrays so a chunk job is contiguous
- SIMD the projectile integrator and the sensor grid
- move any Lua gadget the packet names, using the three-step policy
- GPU-cull and instance compaction, if the render p95 is the one over budget
- residency-set fixes if the stall is a texture page fault, visible in the Metal system trace

A one-worker run must still match the behavior traces. Parallelism is not allowed to change the hash of a micro scenario.

Exit: both hard perf gates, TSan clean on the micro suite, behavior corpus still green.

### M10 — The rest of the roster

Directories: `native/sim`, `native/lua`, `native/compat`, wherever a specific broken unit requires.

A generated test builds every non-holiday unit def on a flat fixture map, orders it to move, and, if it has a weapon, fires at a target dummy. A unit that errors, fails to move, or fails to fire is a row in `native/oracle/roster.md`. Fix rows until 90% pass. Utility units pass by performing their utility (build, reclaim, cloak, radar, transport) instead of firing.

Then the two remaining maps of the core-loop score, Armada and Cortex and Legion each as the player faction on one map, and the outer-loop full game.

Exit: the scoreboard at or above 90, both hard perf gates still green on the same week.

## Repository layout

```
native/
  CMakeLists.txt
  NOTICE                      GPL attribution for ported Recoil algorithms
  SLICES.md
  shell/                      AppKit, main
  rhi/metal4/                 only Metal 4 includes
  shaders/                    MSL
  vfs/
  map/
  model/
  defs/
  audio/
  sim/
    job/  command/  move/  path/  economy/  build/
    weapon/  projectile/  los/  fx/  spawn/
  lua/                        host, gl command lists, callins
  cob/                        bytecode VM
  ui/
  compat/                     VFS overrides, each with a reason
  tools/                      api_audit, gate, shader compiler, linkage check
  oracle/
    PIN  scenarios/  traces/  cameras.json  tolerances.toml
    RATCHET.md  loop_state.json  last_failure.md  escalations/
```

The stock game tree is not modified. `native/compat/` is the only overlay.

## Build and run

Configure:

```
cmake -S native -B native/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build native/build --target bar-metal gate
```

Run a scenario:

```
native/build/bar-metal --startscript native/oracle/scenarios/eco_t1.txt \
  --gamedata "$BAR_DATA" --trace native/oracle/out/eco_t1.jsonl \
  --screenshot-dir native/oracle/out/shots --seed 1
```

Gate:

```
native/build/gate --oracle native/oracle --actual native/oracle/out \
  --suite m4
```

`BAR_DATA` is the player's existing install. The client reads `games/BAR.sdd` from this checkout when `devmode.txt` is present, matching the workflow in the root README, and reads maps from the install's map directory.

## Correction rules the agent is not free to reinterpret

- A red gate is the work. Do not skip to a later milestone.
- Do not loosen `tolerances.toml` inside a repair iteration.
- Do not delete a scenario to make a suite green.
- Do not link a translation layer to get a picture on screen. A red visual gate is fixed with MSL and the command-list encoder.
- Do not satisfy the perf gate by lowering unit counts, simulation rate, or draw distance.
- Do not satisfy the roster gate by deleting unit defs.
- A data race is a failure even when the hash happens to match.
- Edits stay inside the slice directories.
- At five failed iterations, escalate and split the slice.
- Small gameplay tweaks are recorded in `RATCHET.md`. An unrecorded behavior change is a failure.
- Stock files under `luarules/`, `luaui/`, `units/`, and `gamedata/` are not the place to fix engine bugs.

## Risks

| Risk | What the loop does |
| --- | --- |
| The Spring API is 590 functions and swallows the schedule | Bind from the audit and from real nil-call failures. Unbound functions fail `boot` by name. |
| Pathing feels wrong while positions are inside 8 elmos | The `ai_10min` outer loop and a flow bookmark are the backstop. QTPFS is the planned replacement, triggered by that failure. |
| Job races look like balance bugs | Single-worker hash compare, plus TSan, before a behavior slice can go green. |
| A widget's GLSL will not translate | Hand-written MSL for that shader, recorded in `RATCHET.md`. The frame path stays Metal. |
| Lua `GameFrame` scans keep the sim on one core | The 40% thread-share gate names the gadget. The three-step policy is mandatory. |
| Metal 4 headers move | They are included only in `native/rhi/metal4/`. |
| Scope creeps back into multiplayer | No netcode directory exists. Adding one is a plan edit, not a slice. |
| ND art gets repackaged | Loaders read the player's BAR data. No art import target copies models into `native/`. |
| The project stalls at "interesting engine" before it is the game | M4 is a playable economy and M6 is an AI game, on purpose, before water quality or the long tail of widgets. |

## Allowed remainder

These can be missing at a score of 90. They are not secretly required.

- Multiplayer, lobbies, autohost, Chobby, and replay compatibility with Recoil clients
- Bit-exact match with the Recoil simulation
- Pixel-identical water, SSAO, bloom, and particle shapes
- Holiday, easter-egg, and joke widgets
- Joystick camera tool, mission-editor edges, and widgets that are off by default and unused in a normal skirmish
- Extra Recoil water renderers beyond the planar reflection
- Voice chat and notification polish
- Intel Macs, iOS, and any non-Metal backend

## First session after this plan is accepted

1. Add the `native/` tree with CMake, the AppKit shell, a Metal 4 clear and triangle, and static Lua 5.1.
2. Add the linkage gate and make it fail the build on a dirty link line.
3. Run `api_audit` and check in the ranking.
4. Pin Recoil, capture the `boot` trace, and land `gate` so that it fails the triangle client with a clear missing-commander message.

That is M0. The loop starts at M1 only after M0's exit criteria are met.
