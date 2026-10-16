# Slices

## M0 scaffold

A player would see a window with a single triangle on a dark background.
The process prints the game name from `modinfo.lua` and then exits when
asked to render a fixed number of frames.

Scenarios: none are green. `gate --suite m0` must exit 1 because no
commander is spawned and the Recoil boot trace is not on disk.

Directories: `native/shell`, `native/rhi/metal4`, `native/shaders`,
`native/lua`, `native/tools`, `native/oracle`, `native/third_party/lua-5.1.5`.

Do not rewrite: anything under `native/sim`. That tree does not exist yet.

Tolerance: there is no numeric tolerance. The linkage check rejects a
binary that is not arm64-only or that links a translation layer.

## M1 terrain view

A player who passes a start script sees Supreme Isthmus: the height mesh,
the minimap as ground color, metal spots tinted yellow, and a flat water
plane. The camera orbits the way Spring's overhead camera does. Bookmark
mode writes four 1280x720 shots and exits. Without a start script the M0
triangle still runs.

Scenarios: `terrain_isthmus` renders. `gate --suite m1` stays red until
Recoil reference shots exist and a FLIP score meets the bar. A varied image
is not a pass. `gate --suite m0` stays red because no commander is spawned.

Directories: `native/vfs`, `native/map`, `native/rhi/metal4`,
`native/shaders/terrain.metal`, `native/oracle/scenarios`,
`native/oracle/cameras.json`.

Do not rewrite: the stock game tree, and anything under `native/sim`.

Tolerance: bookmark size 1280x720, luminance stddev at least 8, FLIP
required. Do not lower these to make the gate green.

## M2 commander spawn

A boot start script places one Armada commander on team 0 at the Supreme
Isthmus start (7600, 4900), on the ground, in bind pose. The model is the
player's ARMCOM.s3o with its DXT5 textures. Team color follows tex1 alpha,
the same mix BAR's unit shader uses. The commander camera sits south of the
start and aims at the body, because the ground just north of that start is
a cliff.
Medals and the crown stay out of the bind pose. The commander bookmark
writes `oracle/out/m2/commander.ppm`. The client trace is
`oracle/out/boot.jsonl`. That file is not copied into `oracle/traces`.

Scenarios: `boot` renders. `gate --suite m2` stays red until a Recoil boot
trace exists. A commander in the client trace and red pixels in the shot
are not a pass by themselves. `gate --suite m1` stays red on FLIP.

Directories: `native/defs`, `native/model`, `native/sim`,
`native/vfs`, `native/rhi/metal4`, `native/shaders/terrain.metal`,
`native/oracle/scenarios/boot.txt`.

Do not rewrite: orders, movement, pathing, COB, or the stock game tree.
Do not vendor the model or the map.

Tolerance: spawn xz within 1 elmo of the start script, y inside the map
height range, commander shot 1280x720 with stddev at least 8 and at least
80 team-colored pixels. The Recoil trace is required. Do not lower these
to make the gate green.

## Self-checks

The agent runs the slice's self gate on engine output: piece samples and
framebuffer shots. A red Recoil oracle gate does not block the next slice
once that self gate exits 0. Do not invent `oracle/traces`. Do not copy a
client trace into that directory. Do not loosen a tolerance to turn a red
gate green. The user is not the visual oracle.

## M3 ground move

A right-click on the ground orders the commander to walk there. It turns
to face the goal, then moves at the def's speed, and the move class decides
which slopes it can climb. A scripted order walks it from the Armada start
to 8000, 4900. The Armada commander runs `scripts/Units/armcom_lus.lua`:
the legs and arms step while it is moving, and the dish keeps spinning when
it stands. That host is the unit's Lua script, not a COB bytecode VM.
Ships, gunships, planes, weapons, and economy are not in this slice.

Scenarios: `move_kbot` reaches the waypoint. `gate --suite m3` stays red
until a Recoil move trace exists. Arriving on time is not a pass by itself.
`gate --suite m3walk` is the self-check and can exit 0 without that trace.
`gate --suite m2` stays red on the missing boot trace.

Directories: `native/sim/command`, `native/sim/move`, `native/sim/path`,
`native/sim/job`, `native/sim/anim`, `native/defs`, `native/shell`,
`native/shaders/terrain.metal`.

Do not rewrite: economy, weapons, or the stock game tree.

Tolerance: frame 30 is between 2 and 40 elmos from the start, frame 450 is
within 8 elmos of the waypoint. The Recoil trace is required for suite m3.
Suite m3walk checks thigh motion, a spinning dish while idle, and
team-colored pixels on a locked camera. Do not lower these to make a gate
green.

## M3 ship

Armada `armdecade` (BOAT3) spawns in deep water on Supreme Isthmus and sails
to another deep-water point. A later order onto land is refused, or the
ship stays where the depth is at least 8. It floats on the waterline. The
script is `Units/ARMDECADE.cob`; this slice does not run COB, so the mesh
is the bind pose. The commander still runs `armcom_lus.lua`.

Passability uses the move class, not the unit's build `minwaterdepth` of 12.
BOAT3 sets `minwaterdepth` 8 and omits max water and max slope. A missing
minimum is -1e6 and a missing maximum is 1e6. A missing slope on a positive
minimum depth is 90. A cell is legal when its height is at most -min and at
least -max. `floater` holds y at `waterline` (0). The trace field `gy` is
the terrain height under the unit.

Scenario: `move_ship` starts at 1800, 1000, orders 1800, 1400, then 2200,
1000. `gate --suite m3` stays red. `gate --suite m3ship` is the self-check
and can exit 0 without a Recoil trace.

Directories: `native/sim/command`, `native/sim/move`, `native/sim/path`,
`native/defs`, `native/sim/spawn.cpp`, `native/map/startscript.cpp`,
`native/model/s3o.cpp`, `native/sim/anim`, `native/shell`.

Do not rewrite: the commander walk host, economy, weapons, hover, aircraft,
or the stock game tree.

Tolerance: frame 30 is 20 to 80 elmos from the start, frame 240 is within 8
elmos of the water waypoint, terrain height stays at or below -8, and the
land order does not put the ship on that point. The locked shot counts dark
hull pixels in the middle of the frame. Do not lower these to make the gate
green.

## Metal spot HUD

An interactive window draws a cyan ring on each mex spot, and it enables
Apple's Metal performance HUD (`MTL_HUD_ENABLED`) on that same window. The
Apple overlay shows device, resolution, FPS, GPU time, and frame pacing.
Bookmark shots, lock-camera shots, screenshots, and `--frames` runs leave
both off so those checks stay stable. A metal map draws no rings. The gold
tint on the terrain is the metal-map albedo. Occupied spots stay cyan until
extractors exist.

Directories: `native/map/smf.cpp`, `native/rhi/metal4/TerrainRenderer.mm`,
`native/shaders/terrain.metal`.

Do not rewrite: economy numbers, the resource bar, extractors, or the stock
widget. Do not recapture the ship or walk shots to prove the rings. Cyan
does not count as team color or as a dark hull.

## M3 hover and gunship

Armada `armsh` (HOVER2) rides the surface. Water is height 0, so the sea
floor's slope does not block it, and a land slope still uses the class
limit of 33 degrees. `armkam` is a gunship: `canfly` and `hoverattack`,
cruise altitude 70. It flies a straight line and a cliff does not path it.
An airplane (`canfly` with `hoverattack` false) follows this slice. Neither
script is hosted. `ARMSH.cob` and `ARMKAM.cob` stay bind pose. The
commander still runs `armcom_lus.lua`.

Scenario `move_hover` starts at 5960, 3680, orders 5704, 3680, then the
cliff cell 7248, 4016. Scenario `move_gunship` starts at 7600, 4900 and
orders 7600, 4300. `gate --suite m3` stays red. `gate --suite m3hover` and
`gate --suite m3gunship` are the self-checks and can exit 0 without a
Recoil trace.

The locked eye is `cos(tilt) * distance` above world y=0. A craft above
66 elmos lifts that eye by its own y minus 66. The commander walk and the
ship stay under 66, so those shots do not move.

Directories: `native/sim/move`, `native/sim/path`, `native/defs`,
`native/sim/spawn.cpp`, `native/shell`, `native/map/camera.cpp`.

Do not rewrite: the commander walk host, the ship waterline, economy,
weapons, airplane banking, or the stock game tree.

Tolerance: hover frame 30 is 4 to 20 elmos from the start, frame 150 is
within 8 of the water point, and from frame 210 the cliff order has not
moved it. Water samples stay at or below gy -30 with y within 1 of 0, and
no sample climbs above 12. Gunship frame 30 is 4 to 20 elmos from the
start, frame 180 is within 8 of the goal, and every sample stays within 1
elmo of the surface plus 70. Locked shots count dark pixels in the middle.
Do not lower these to make a gate green.

## M3 airplane

Armada `armfig` is the raw unit file, not the air-rework table. It can fly,
it does not hover-attack, and its cruise altitude is 125. It yaws while
moving, at most `maxrudder` and at most speed over the turn radius of 64.
It banks toward that yaw, up to 0.8, and the roll lowers the inside wing.
It does not stop or snap when it reaches a goal. Climb is capped by speed
times tan of max pitch. `Units/ARMFIG.cob` is not hosted, so the mesh stays
in the bind pose. The commander still runs `armcom_lus.lua`.

Scenario `move_plane` starts at 7600, 4900, orders 7600, 6096.21875, then
at frame 180 orders 10100, 6385.42285. `gate --suite m3` stays red.
`gate --suite m3plane` is the self-check and can exit 0 without a Recoil
trace.

Directories: `native/sim/move`, `native/defs`, `native/sim/spawn.cpp`,
`native/shell`, `native/shaders/terrain.metal`.

Do not rewrite: the commander walk host, the ship waterline, the hover
surface ride, the gunship cruise, economy, weapons, or the stock game tree.

Tolerance: frame 30 is 70 to 100 elmos from the start, frame 150 is within
8 of 7600, 6096.21875 with speed still at least 5, every later sample stays
at least that fast and within 1 elmo of the surface plus 125, and frame 240
banks at least 0.7. The locked shot counts dark pixels in the middle. Do
not lower these to make a gate green.

## M3 cob

Armada `armbull` is the tank. Its script is version-4 `scripts/Units/armbull.cob`.
The host is an integer stack machine for that file only. The commander still
runs `armcom_lus.lua`. Ship, hover, gunship, and airplane stay on the bind
pose. Create hides smoke1. StartMoving spins lwheel and rwheel on X at 20020
cob units per second, gaining 910 per frame. The piece matrices are unchanged.
A cob Z angle is negated when it is stored, so the Lua walk is not touched.

Scenario `move_tank` starts at 7600, 4900 and orders 8400, 4900. The existing
ground mover carries the hull. `gate --suite m3` stays red. `gate --suite m3cob`
is the self-check and can exit 0 without a Recoil trace.

Directories: `native/sim/anim`, `native/shell`, `native/tools/gate`.

Do not rewrite: the commander walk host, the spin integrate formula, the ship
waterline, the hover surface, the gunship cruise, the airplane bank, economy,
weapons, or the stock game tree.

Tolerance: at frame 300 the left-wheel spin has been integrated at least 40
times, both wheel X angles match the 910/20020 sum within 0.02 radians, and
smoke1 is hidden. The locked shot counts dark pixels in the middle. Do not
lower these to make a gate green.

## M3 tank class

Armada `armbull` uses move class HTANK4. The path limit is 27 degrees and
20 elmos of water. The unit file's slope 12 and water depth 15 are not
loaded. The hull stands on the ground sample. A cell whose center is deeper
than 20 has no ground path, and a refused order leaves the previous goal
in place.

Scenario `move_htank` starts at 1040, 2352, orders the shelf at 1200, 2352,
then at frame 180 orders 1200, 2224. `gate --suite m3` stays red.
`gate --suite m3htank` is the self-check and can exit 0 without a Recoil
trace. `gate --suite m3cob` still covers the wheels on `move_tank`.

Directories: `native/oracle`, `native/tools/gate`.

Do not rewrite: the path slope test, the commander walk host, the cob
integrate, the ship waterline, the hover surface, the gunship cruise, the
airplane bank, economy, weapons, or the stock game tree.

Tolerance: frame 90 is 40 to 90 elmos from the start, frame 150 is within
8 of the shelf with gy between -19.5 and -16 and y within 0.05 of gy, and
from frame 210 the deeper order has not moved the tank. The locked shot
counts dark pixels in the middle. Do not lower these to make a gate green.

## M4 team resources

A team starts with the metal and energy from `modoptions.lua`, overridden
by the start script. Storage is the maximum of the floor in
`luarules/gadgets/game_team_resources.lua`, the modoption storage, the
starting stock, and the commander's own storage. The commander's storage
is not added a second time. Each frame adds `metalmake` and `energymake`
divided by 30 and clamps the stock to storage. Numbers stay in the trace.
Nothing is drawn on the frame.

Scenario `eco_team` spawns the Armada commander at 7600, 4900 with 100
metal and 970 energy. Storage stays 1000. `gate --suite m3` stays red.
`gate --suite m4eco` is the self-check and can exit 0 without a Recoil
trace. The M4 milestone stays red until mex income, construction, and
reclaim are gated.

Directories: `native/sim/economy`, `native/defs`, `native/map`,
`native/shell`, `native/tools/gate`.

Do not rewrite: movement, the commander walk host, the cob integrate, the
path slope test, weapons, or the stock game tree.

Tolerance: frame 30 matches one second of income clamped to storage, and
frame 60 stays on the energy cap. The slop is 0.05. Do not lower it to
make a gate green.

## M4 mex income

An Armada mex reads `extractsmetal` and `energyupkeep` from its unit file.
mapinfo.lua lowercases its keys, so the metal scale is `maxmetal` and the
dig radius is `extractorradius`. On Supreme Isthmus those are 0.93 and 90.
A metal-map byte times 0.93 is the metal on that 16-elmo square. The mex
adds `extractsmetal` times that sum for every square whose center lies
inside the radius. A second mex on the same square only gets the depth the
first one did not take. The depth is scaled by `multiplier_metalextraction`
times `multiplier_resourceincome` when the unit's `metal_extractor` custom
is set. Both default to 1. The extractor pays its energy upkeep in the same
frame as the team's income, and the ground metal is added only when that
upkeep is covered. The stock then clamps.

Scenario `eco_mex` keeps the commander and places one mex on a spot whose
raw byte sum is 4629 and one mex on empty ground. The empty mex still costs
3 energy. Frame 30 is metal 106.305 and energy 994. Frame 60 is metal
112.61 and energy 1000. `gate --suite m4mex` is the self-check and can exit
0 without a Recoil trace. `gate --suite m3` stays red. The M4 milestone
stays red until solar, construction, and reclaim are gated.

Directories: `native/sim/economy`, `native/map`, `native/defs`,
`native/shell`, `native/tools/gate`.

Do not rewrite: the team tick for a commander with no upkeep, movement, the
commander walk host, the cob integrate, the path slope test, weapons, or
the stock game tree.

Tolerance: the spot raw is 4629, the empty raw is 0, both cover 97 squares,
the depth is 0.001, and the scale is 0.93. The stock slop is 0.05. Do not
lower these to make a gate green.

## M4 solar and wind

An active building with a negative `energyupkeep` adds that amount as
energy per second. `armsolar` is `-20`, so it makes 20. The energy
production multiplier scales it. The default is 1. An active wind
generator adds the map wind, and never more than its `windgenerator`.
`armwin` caps at 25. Supreme Isthmus sets `minwind` 1 and `maxwind` 19
after mapinfo.lua lowercases the atmosphere keys. The strength used here
is their average, 10, which is the strength at the start of a wind cycle.
The gust walk is not simulated. Wind adds no metal, because the metal cap
on a wind generator is 0. Solar and wind income arrive even if an
extractor cannot pay its upkeep. Neither building adds storage: a unit
that exists at the start is inside the storage reset, and the team cap
stays 1000. Nothing is drawn on the frame.

Scenario `eco_power` keeps the commander at 7600, 4900 with 100 metal and
900 energy. It places `armsolar` at 7280, 4900 and `armwin` at 6960, 4900.
Frame 30 is metal 102 and energy 960. Frame 60 is metal 104 and energy
1000. `gate --suite m4pwr` is the self-check and can exit 0 without a
Recoil trace. `gate --suite m3` stays red. The M4 milestone stays red
until construction and reclaim are gated.

Directories: `native/sim/economy`, `native/map`, `native/defs`,
`native/shell`, `native/tools/gate`.

Do not rewrite: the team tick for a commander with no upkeep, the mex
claim, movement, the commander walk host, the cob integrate, the path
slope test, weapons, or the stock game tree.

Tolerance: solar upkeep is -20, wind cap is 25, and the wind range is 1
to 19. The stock slop is 0.05. The upkeep, cap, and wind-strength slop is
0.0001. Do not lower these to make a gate green.

## M4 nanoframe

The commander builds an `armwin` nanoframe. Build power per frame is
`workertime` times `multiplier_buildpower`, divided by 30. `armcom` is 300
and the multiplier default is 1, so each frame adds 10/1600 of the wind
generator. The metal and energy for that whole step are spent after the
frame's income, or the step does not happen. Health starts at 0.1 and grows
by the finished health times the step. Progress stays below 1, so the frame
does not produce energy and does not add storage. The click snaps to the
16-elmo build grid. Ground leveling and the build-stance animation are not
run: progress starts on the first sim frame. Decay, reclaim, repair,
resurrect, and capture factors are read from `modrules.lua`. A continuous
build never decays. The modrule named `constructionDecayTime` is compared
by the engine as a raw frame count. The nanoframe is not drawn.

Scenario `eco_nano` keeps the commander at 7600, 4900 with 100 metal and
900 energy. The order clicks 7680, 4900, which snaps to 7688, 4904. Frame
30 progress is 0.1875, health is 36.85, metal is 94.5, and energy is
897.1875. Frame 60 progress is 0.375, health is 73.6, metal is 89, and
energy is 894.375. The metal trace prints 88.9999 because 2/30 is not a
binary fraction. `gate --suite m4nano` is the self-check and can exit 0
without a Recoil trace. `gate --suite m3` stays red. The M4 milestone
stays red until reclaim is gated.

Directories: `native/sim/build`, `native/sim/command`, `native/defs`,
`native/shell`, `native/tools/gate`.

Do not rewrite: the team tick for a commander with no build, the mex
claim, solar and wind income, movement, the commander walk host, the cob
integrate, the path slope test, weapons, or the stock game tree.

Tolerance: build time is 1600, metal cost is 40, energy cost is 175,
worker time is 300, and build distance is 145. The stock slop is 0.05.
The progress, health, and cost slop is 0.0001. Do not lower these to make
a gate green.

## M4 lab

A finished `armvp` produces `armfav`. Build power per frame is the lab's
`workertime` times `multiplier_buildpower`, divided by 30. `armvp` is 150
and the multiplier default is 1, so each frame adds 5/1100 of the scout.
The metal and energy for that whole step are spent after the frame's
income, or the step does not happen. Health starts at 0.1. The product
sits on the lab. The yard script, the build stance, the pad offset, and
the walk out of the yard are not run, so production starts on the order
frame. The last partial step sets progress to 1 and health to the finished
maximum. The finished unit is not walked out, and it adds no storage and
no income. The lab and the product are not drawn.

Scenario `build_tank` keeps the commander at 7600, 4900 with 100 metal and
900 energy, and places the lab at 7280, 4900. Frame 30 progress is
0.136364, health is 14.4182, metal is 97.7728, and energy is 879.545.
Frame 210 is still unfinished at progress 0.954545. Frame 240 progress is
1, health is 105, metal is 85.0001, and energy is 769.998. The full metal
cost 31 and energy cost 370 have been spent, and the commander income of
2 and 30 is the only income. `gate --suite m4lab` is the self-check and
can exit 0 without a Recoil trace. `gate --suite m3` stays red. The M4
milestone stays red until reclaim is gated.

Directories: `native/sim/build`, `native/sim/command`, `native/defs`,
`native/shell`, `native/tools/gate`.

Do not rewrite: the builder nanoframe range check, the team tick, the mex
claim, solar and wind income, movement, the commander walk host, the cob
integrate, the path slope test, weapons, or the stock game tree.

Tolerance: build time is 1100, metal cost is 31, energy cost is 370, and
worker time is 150. The stock slop is 0.05. The progress, health, and
cost slop is 0.0001. Do not lower these to make a gate green.

## M4 reclaim

The commander reclaims an `armstump` wreck. Reclaim power per frame is
the commander's reclaim speed divided by 30. `armcom` has no
`reclaimspeed`, so the speed is its `workertime` 300 times
`multiplier_buildpower`. The multiplier default is 1, so each frame adds
10/810 of the wreck. The corpse metal in the unit file is 152. The engine
uses `floor(metalcost * wreck_metal_ratio)`, and the modoption default is
0.6, so the wreck metal is `floor(225 * 0.6)` = 135. Feature energy is 0
and the corpse has no reclaim time, so reclaim time is
`max(1, (135 + 0) * 6)` = 810. Metal returns on each step because reclaim
method is 0. The energy cost is `featureEnergyCostFactor` times the metal
fraction, and that float is charged to both resources. The factor is 0,
so the stock does not pay. The build stance is not hosted, so reclaim
starts on the order frame. The last partial step returns the metal still
on the wreck and marks it gone. The wreck is not a unit and is not drawn.
`unit_reclaim_fix.lua` stays off.

Scenario `reclaim` keeps the commander at 7600, 4900 with 100 metal and
900 energy. The order places the wreck at the click 7680, 4900. Frame 30
left is 0.62963, metal is 152, and energy is 930. The wreck is still open
on frame 81. The remainder step finishes it on frame 82. Frame 90 left is
0, the wreck is gone, metal is 241, and energy is 990. That is the full
wreck metal 135 plus the commander income of 2 and 30. `gate --suite
m4reclaim` is the self-check and can exit 0 without a Recoil trace.
`gate --suite m3` stays red. The M4 milestone stays red until a Recoil
trace matches `eco_t1`, `build_tank`, and `reclaim` at 2%.

Directories: `native/sim/build`, `native/sim/command`, `native/defs`,
`native/shell`, `native/tools/gate`.

Do not rewrite: the builder nanoframe range check, the lab production
path, the team tick, the mex claim, solar and wind income, movement, the
commander walk host, the cob integrate, the path slope test, weapons, or
the stock game tree.

Tolerance: wreck metal is 135, reclaim time is 810, reclaim speed is 300,
and build distance is 145. The stock slop is 0.05. The remaining-fraction
slop is 0.0001. Do not lower these to make a gate green.

## M5 laser

A `cormando` fires weapon 1, `COMMANDO_BLASTER`, a LaserCannon, at an
`armstump`. Reload frames are `max(1, int(reloadtime * 30))` = 15.
Projectile speed is `weaponvelocity / 30` = 31. Range rounds down to a
multiple of that speed: `max(1, floor(300 / 31)) * 31` = 279. Burnblow is
omitted, so it defaults true and the bolt lifetime is
`min(ceil(dist/speed), floor(range/speed) - 1)`. A surface hit deals the
default damage, 100. Splash is not applied. Target health is 1800, so 18
hits kill it. The aim script, QueryWeapon, and the build stance are not
hosted, so the first shot is on sim frame 1 and later shots are 15 frames
apart. The muzzle is the top center of the shooter's collision box. Piece
volumes are not hosted, so the hit test is the target's unit box. The
commander is not tested. There is no line-of-sight check, no wreck, no
bolt draw, and no billboard. The target stays in the unit arrays at health
0. Firing stops once the death frame is set. `cormando` energymake is not
added to the team. Autoheal is not stepped.

Scenario `duel_laser` keeps the commander at 7600, 4900. The shooter is
unit 1, `cormando`, team 0, at 7200, 4900. The target is unit 2,
`armstump`, team 1, at 7400, 4900. The order is `0 1 attack 2`. Neither
extra unit is drawn. Frame 30 health is 1600, hits are 2, shots are 2,
and die is 0. Frame 300 health is 0, hits are 18, shots are 18, die is
262, and ground hits are 0. Damage is 100, speed is 31, range is 279, and
reload is 15. The shooter y is 73.1482, so the muzzle y is 98.1482. The
target y is 53.9294. The ray cleared the ground. Team metal and energy
follow the commander only. `gate --suite m5laser` is the self-check and
can exit 0 without a Recoil trace. `gate --suite m3` stays red. The M4
milestone stays red until a Recoil trace matches `eco_t1`, `build_tank`,
and `reclaim` at 2%. The M5 milestone stays red until the duel corpus is
within 15% time to kill of a Recoil trace. The next slice is the cannon.

Directories: `native/sim/weapon`, `native/sim/projectile`,
`native/sim/command`, `native/shell`, `native/tools/gate`.

Do not rewrite: the reclaim path, the lab production path, the builder
nanoframe range check, the team tick, the mex claim, solar and wind
income, movement, the commander walk host, the cob integrate, the path
slope test, or the stock game tree.

Tolerance: health is 1800, damage is 100, file range is 300, reload is
0.5 seconds, and velocity is 930. The position slop is 1. The health slop
is 0.0001. Do not lower these to make a gate green.

## M5 cannon

An `armstump` fires weapon 1, `ARM_LIGHTCANNON`, a Cannon, at another
`armstump`. Reload frames are `max(1, int(reloadtime * 30))` = 36.
Projectile speed is `weaponvelocity / 30` = 7.5. Range stays the file
range 350. The LaserCannon speed-round is not used. Map gravity is the
lowercase `gravity` tag from mapinfo.lua. Supreme Isthmus is 100, so
per-frame gravity is `-max(0.001, 100) / (30 * 30)` = -0.111111. High
trajectory is omitted, so the low arc is used. Cylinder targeting is
omitted, so the range test is the sphere form with heightMod 0.8:
horizontal distance from the muzzle to the aim point is at most
`sqrt(range^2 - (heightDiff * 0.8)^2)`. Accuracy, spray, burst, energy
per shot, mygravity, and flight time are omitted. Burnblow, ground
bounce, and water bounce are omitted, so they stay false. Burnblow false
means the shell lifetime is `2 * ceil(horizontal / horizontalSpeed)`. A
surface hit deals the full default damage, 97. Splash is not applied.
Target health is 1800, so 19 hits kill it: 18 hits leave 54, and the 19th
overkills. The aim script, QueryWeapon, the build stance, and piece
volumes are not hosted. `usepiececollisionvolumes = 1` is ignored and the
unit box is the volume. The muzzle is the top center of the shooter's
collision box. The aim point is the target's box center, not the feet.
Heading stays 0, so the box is axis-aligned. The commander and the owner
are not tested. There is no line-of-sight check, no wreck, no shell draw,
and no billboard. Water was not a separate test. Scatter, high trajectory,
bounce, and a myGravity override are refused rather than simulated. A
failed ballistic root refuses the shot. The target stays in the unit
arrays at health 0. Firing stops once the death frame is set. `armstump`
has no energymake, so team metal and energy follow the commander only.
Autoheal is not stepped.

The first placement, shooter 7200, 4900 and target 7400, 4900, clipped a
hillside. Every shell hit the ground and health stayed 1800. The pair was
moved onto flat ground. Scenario `duel_cannon` keeps the commander at
7600, 4900. The shooter is unit 1, `armstump`, team 0, at 6800, 4600,
y 49.2977. The target is unit 2, `armstump`, team 1, at 7000, 4600,
y 52.6612. Horizontal separation is 200. The order is `0 1 attack 2`.
Neither extra unit is drawn. Neither has a move order. The muzzle is
6800, 59.2977, 4600. Frame 30 health is 1703, hits are 1, shots are 1,
die is 0, and ground hits are 0. Frame 720 health is 0, hits are 19,
shots are 19, die is 673, and ground hits are 0. Damage is 97, speed is
7.5, range is 350, and reload is 36. Gravity is -0.111111 and heightMod
is 0.8. The arc cleared the ground at this placement. The first shot hits
on frame 25. Shot 19 fires at frame 649 and hits at frame 673.
`gate --suite m5cannon` is the self-check and can exit 0 without a Recoil
trace. It recomputes the float32 loop from the hold-sample unit y plus
the pinned boxes, damage, speed, range, reload, gravity tag, and
heightmod. It does not hardcode 673 or 1703. `gate --suite m3` stays red.
The M4 milestone stays red until a Recoil trace matches `eco_t1`,
`build_tank`, and `reclaim` at 2%. The M5 milestone stays red until the
duel corpus is within 15% time to kill of a Recoil trace. The next slice
is the beam.

Directories: `native/sim/weapon`, `native/sim/projectile`,
`native/sim/command`, `native/shell`, `native/map`, `native/tools/gate`.

Do not rewrite: the laser path, the reclaim path, the lab production
path, the builder nanoframe range check, the team tick, the mex claim,
solar and wind income, movement, the commander walk host, the cob
integrate, the path slope test, or the stock game tree.

Tolerance: health is 1800, damage is 97, file range is 350, reload is
1.2 seconds, velocity is 225, the gravity tag is 100, and heightMod is
0.8. The position slop is 1. The health slop is 0.0001. Do not lower
these to make a gate green. Do not put the pair back at 7200, 4900 and
7400, 4900.

## Oracle traces

The pinned engine on this Mac is the BAR Launcher `spring`, version
2026.07.04. It is the headed client. A one-gadget mutator, BAR Metal
Oracle, runs inside it and echoes commander samples. The game archive is
the installed rapid package `Beyond All Reason test-31578-8247b61`. The
dump keeps the commander and drops the critters. The gadget and the
launch script live in `native/oracle/recoil/`.

`boot` puts the commander at 7600, 57.687881, 4900. The native y is
57.691. `gate --suite m0` exits 0. `gate --suite m2` exits 0.

`move_kbot` orders the commander to 8000, 4900 at frame 90. Recoil holds
the commander in the spawn warp until that frame, so it is still at
7600, 4900 at frame 90. It stops at 7993.4785, 4900.1904 on frame 420
and is still there at frame 600. Native is at 8000, 4900 on frame 450.
The gap is 6.52 elmos, inside the 8 elmo bar. `gate --suite m3` exits 0.
The two walks are not frame-aligned.

`gate --suite m1` stays red. There are no Recoil reference pictures.
The M4 and M5 milestones stay red. Their scenarios were not captured.
The next slice is the beam.

## M5 beam

An `armfav` fires weapon 1, `ARM_LASER`, a BeamLaser, at another `armfav`.
`beamburst` is omitted, so it stays false and `burstrate` 0.2 does not
apply. Salvo size is `max(1, int(beamtime * 30))`. Beamtime 0.18 times 30
is just under 5.4 in float32, so the salvo is 5 ticks, not 6. Salvo delay
is 0, so the first tick is the same frame that stamps reload. Reload
frames are `max(1, int(reloadtime * 30))` = 30, measured from that first
tick. The next salvo starts on that frame and fires immediately. Shots in
a salvo are consecutive frames. `customparams.reaimtime` is 3, but the aim
script is not hosted, so the first shot is frame 1. Damage per tick is
`damage.default * (1/5) * hitIntensity`. Default damage is 35.
`minIntensity` is omitted, so the floor is 0. `targetBorder` is omitted,
so the falloff range stays the file range 180. `hitIntensity` is
`max(0, 1 - rayEntry / (range * 2))`. `rayEntry` is the distance from the
muzzle to the first face of the target box along the direction toward the
box center, not the distance to the center. `heightmod` is omitted.
BeamLaser uses 1, not the shared weapon default 0.2 and not the cannon
0.8, so the range test is a sphere: the 3D distance from the muzzle to the
box center is at most 180. `cylinderTargeting` is omitted. `impactonly` is
1, so there is no splash and the vtol damage 2 is not used.
`energypershot` is 0, so team metal and energy stay on the commander.
Accuracy and spray are omitted. Sweep fire is omitted. A shot that misses
the box counts as a ground hit and deals no unit damage, and it still
consumes the tick. This placement hits, so ground stays 0. Firing stops
when the death frame is set, including the leftover ticks of that salvo.
A continuing salvo into a corpse is not simulated. The target stays in
the unit arrays at health 0. The beam is not drawn. There is no wreck, no
shield, no water clip, and no line-of-sight check. Weapon velocity 800 is
visual. The muzzle is the top center of the shooter box. The aim point is
the target box center. Heading stays 0, so the box is axis-aligned.
`armfav` can move. Neither extra unit has a move order, and neither is
drawn. The unit script is cob and is not hosted.

Scenario `duel_beam` keeps the commander at 7600, 4900. The shooter is
unit 1, `armfav`, team 0, at 6800, 4600, y 49.2977. The target is unit 2,
`armfav`, team 1, at 6920, 4600, y 50.0023. Separation is 120, inside the
range 180. The order is `0 1 attack 2`. The pair is not the hillside pair
at 7200, 4900 and 7400, 4900, and it is not 200 elmos apart. The muzzle is
6800, 58.2977, 4600. Intensity is 0.695272. Frame 30 health is 80.6655,
hits are 5, shots are 5, die is 0, and ground hits are 0. Frame 120 health
is 7.66194 with 20 hits. The fifth salvo starts at frame 121. The second
tick of that salvo kills, so frame 300 health is 0, hits are 22, shots are
22, die is 122, and ground hits are 0. The last three ticks of the killing
salvo are not fired. Health is 105 and damage is 35. `gate --suite m5beam`
is the self-check and exited 0 on engine output. It recomputes the float32
loop from the hold-sample unit y plus the pinned boxes, damage 35,
beamtime 0.18, reload 1, range 180, and salvo `int(beamtime * 30)`. It
does not hardcode 122 or 80.6655. It does not need a Recoil trace. m1
stays red without reference pictures. The M4 milestone stays red until a
Recoil trace matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The M5
milestone stays red until the duel corpus is within 15% time to kill of a
Recoil trace. The next slice is the missile.

Directories: `native/sim/weapon`, `native/sim/command`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the reclaim path, the lab
production path, the builder nanoframe range check, the team tick, the mex
claim, solar and wind income, movement, the commander walk host, the cob
integrate, the path slope test, or the stock game tree.

Tolerance: health is 105, damage is 35, file range is 180, reload is 1
second, and beamtime is 0.18. The position slop is 1. The health slop is
0.0001. Do not lower these to make a gate green. Do not put the pair back
at 7200, 4900 and 7400, 4900.

## M5 missile

A `legbal` fires weapon 1, `LEG_BOT_ROCKET` (`leg_bot_rocket`), a
MissileLauncher, at another `legbal`. `trajectoryheight` is omitted, so
the launch is not tilted up. `tracks`, `turnrate`, `wobble`, `dance`,
`sprayangle`, `accuracy`, and `flighttime` are omitted, so the rocket
holds its launch direction and its fuel is
`ceil(max(distance, range) / maxSpeed)`. `burnblow` and `fixedlauncher`
are omitted, so the rocket is unguided and leaves from the computed
muzzle. `mygravity` is omitted, so the powered shot does not read map
gravity. When `ttl` reaches 0 the rocket is removed. `heightmod` is
omitted. MissileLauncher keeps the weapon default 0.2, so the range test
is horizontal distance against `sqrt(range^2 - (heightDiff * 0.2)^2)`.
`cylinderTargeting` and `targetBorder` are omitted. `burst` is omitted, so
one rocket is fired and reload is stamped on that frame. Reload frames
are `max(1, int(reloadtime * 30))` = 165. `startvelocity` and
`weaponvelocity` are both 230, so the per-frame speed is `230/30` and
stays there. `weaponacceleration` is 120, which is `120/(30*30)` per
frame and does not add speed while the rocket is already at the cap.
`weapontimer` 2 is the starburst uptime and is ignored. `areaofeffect` is
70 and is not applied. A direct hit deals `damage.default` 250.
`energypershot` is omitted and `metalpershot` is 0, so team metal and
energy stay on the commander. `customparams.reaimtime` is 5, but the aim
script is not hosted, so the first shot is frame 1. The same step that
spawns the rocket tests the segment from its position along its velocity
against the target box, then moves it. A hit deals 250, removes the
rocket, and sets the death frame when health reaches 0. A later shot is
not fired after that. A miss whose position is already under the terrain
height counts as a ground hit and deals no damage. This placement hits
the box, so ground stays 0. The target stays in the unit arrays at health
0. The rocket is not drawn. There is no wreck, no shield, no splash, and
no line-of-sight check. The muzzle is the top center of the shooter box,
`origin + offset + (0, halfHeight, 0)`. The aim point is the target box
center, `origin + offset`. The box scales are 26, 36, 20 and the offsets
are 0, -2, -1, so the muzzle sits 16 above the unit origin and 1 elmo
toward -z. Heading stays 0, so the box is axis-aligned. `legbal` can
move. Neither extra unit has a move order, and neither is drawn. The unit
script is cob and is not hosted.

Scenario `duel_missile` keeps the commander at 7600, 4900. The shooter is
unit 1, `legbal`, team 0, at 6800, 4600, y 49.2977. The target is unit 2,
`legbal`, team 1, at 7000, 4600, y 52.6612. Separation is 200, inside the
range 460. The order is `0 1 attack 2`. The pair is not the hillside pair
at 7200, 4900 and 7400, 4900. The muzzle is 6800, 65.2977, 4599. Frame 90
health is 650, hits are 1, shots are 1, die is 0, and ground hits are 0.
Frame 720 health is 0, hits are 4, shots are 4, die is 520, and ground
hits are 0. Health is 900 and damage is 250, so the fourth hit kills.
`gate --suite m5missile` is the self-check and exited 0 on engine output.
It recomputes the float32 rocket loop from the hold-sample unit positions
plus the pinned boxes, damage 250, range 460, reload 5.5, start velocity
230, max velocity 230, acceleration 120, and heightmod 0.2. It does not
hardcode 520 or 650. It does not need a Recoil trace. m1 stays red
without reference pictures. The M4 milestone stays red until a Recoil
trace matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The M5
milestone stays red until the duel corpus is within 15% time to kill of a
Recoil trace. The next slice is lightning.

Directories: `native/sim/weapon`, `native/sim/projectile`,
`native/sim/command`, `native/shell`, `native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
reclaim path, the lab production path, the builder nanoframe range check,
the team tick, the mex claim, solar and wind income, movement, the
commander walk host, the cob integrate, the path slope test, or the stock
game tree.

Tolerance: health is 900, damage is 250, file range is 460, reload is 5.5
seconds, start velocity is 230, weapon velocity is 230, and acceleration
is 120. The position slop is 1. The health slop is 0.0001. Do not lower
these to make a gate green. Do not put the pair back at 7200, 4900 and
7400, 4900.

## M5 lightning

An `armlatnk` fires weapon 1, `LIGHTNING`, a LightningCannon, at another
`armlatnk`. Weapon 2 is an anti-air missile and is not fired. `heightmod`
is omitted. LightningCannon uses 1, so the range test is a sphere: the 3D
distance from the muzzle to the box center is at most the file range.
`impactonly` is 1, so a hit deals `damage.default` and the area 8 is not
applied. `accuracy` and `sprayangle` are omitted, so the bolt has no
spread. `burst` is 10. `burstrate` is 0.03333, and `int(burstrate * 30)`
is 0, so the ten bolts are consecutive frames. Reload frames are
`max(1, int(reloadtime * 30))` = 45, stamped when the burst is accepted,
before the first bolt. The next burst waits until that frame. `energypershot`
is 10 and is spent once for the burst, from the team stock after that
frame's income, not once per bolt. A burst that the stock cannot afford
does not start and does not stamp reload. `metalpershot` is omitted.
`paralyzer` is omitted. `waterweapon` is omitted, so a bolt that reaches
y = 0 before the target box deals no damage and counts as a ground hit.
`customparams` spark fields belong to the chain gadget. That gadget is
off, so a bolt does not fork. `beamttl` and `duration` are visual.
`usepiececollisionvolumes` is 1. Piece volumes are not hosted, so the unit
box is the target. The box scales are 30, 26, 34 and the offsets are
0, 0, 0. The muzzle is the top center of the shooter box. The aim point
is the target box center. Heading stays 0. `customparams.reaimtime` is
absent, and the aim script is not hosted, so the first bolt is frame 1.
Firing stops when the death frame is set, including the leftover bolts of
that burst. The target stays in the unit arrays at health 0. The bolt is
not drawn. There is no wreck, no shield, and no line-of-sight check.
`armlatnk` can move. Neither extra unit has a move order, and neither is
drawn. The unit script is cob and is not hosted.

Scenario `duel_lightning` keeps the commander at 7600, 4900. The shooter
is unit 1, `armlatnk`, team 0, at 6800, 4600, y 49.2977. The target is
unit 2, `armlatnk`, team 1, at 7000, 4600, y 52.6612. Separation is 200,
inside the range 300. The order is `0 1 attack 2`. The pair is not the
hillside pair at 7200, 4900 and 7400, 4900. The muzzle is 6800, 62.2977,
4600. Frame 30 health is 840, hits are 10, shots are 10, die is 0, ground
hits are 0, and team energy is 920. Frame 180 health is 180 with 40 hits.
The fifth burst starts at frame 181. Its ninth bolt kills, so frame 720
health is 0, hits are 49, shots are 49, die is 189, ground hits are 0,
and team energy is 1000. The tenth bolt of the killing burst is not
fired. Health is 1060 and damage is 22. Five bursts spend 50 energy.
Commander income still refills the stock to storage 1000. `gate --suite
m5lightning` is the self-check and exited 0 on engine output. It
recomputes the float32 burst and the energy tick from the hold-sample
unit positions plus the pinned boxes, damage 22, range 300, reload 1.5,
burst 10, burstrate 0.03333, cost 10, opening energy 900, income 30, and
storage 1000. It does not hardcode 189, 840, or 920. It does not need a
Recoil trace. m1 stays red without reference pictures. The M4 milestone
stays red until a Recoil trace matches `eco_t1`, `build_tank`, and
`reclaim` at 2%. The M5 milestone stays red until the duel corpus is
within 15% time to kill of a Recoil trace. The next slice is flame.

Directories: `native/sim/weapon`, `native/sim/command`, `native/shell`,
`native/tools/gate`.

## M5 flame

A `corcrwt4` fires weapon 1, `kmaw`, a Flame, at another `corcrwt4`.
Weapon 2 is an anti-air missile and is not fired. The later weapons are
beam lasers and are not fired. `sprayangle` is omitted and `accuracy` is
0, so the only spread is the fixed `-dir * 0.001` added to the velocity
after each miss. `burst` is 12. `burstrate` is 0.03333, and
`int(float32(burstrate * 30))` is 0, so the twelve particles are
consecutive frames. Reload frames are `max(1, int(float32(reloadtime * 30)))`.
`reloadtime` is 0.39996. That product is 11.9988 in float32, so the reload
is 11 frames, stamped when the burst is accepted. The next burst starts on
the following frame. There is no idle frame, and the duel fires one new
particle every frame until the target dies. `energypershot` and
`metalpershot` are omitted, so the team stock is not spent. `flameGFXTime`
is 1. Lifetime is `ceil(max(distance, range) / (weaponvelocity / 30) *
duration)` in float32. Range is 500 and weapon velocity is 500, so that
lifetime is 31 frames. `noexplode` is false, so the first entry into the
target box deals `damage.default` 40 and removes the particle. Area falloff
is not applied. The growing radius is not the hit test. The segment from
the current position through this frame's speed is tested before the move.
Ground slide, bounce, and water clipping are not hosted. A particle that
expires without entering the box counts as a ground hit. `waterweapon` is
omitted. The box scales are 80, 51, 120 and the offsets are 0, 10, 0. The
muzzle is the top center of the shooter box, including that offset. The aim
point is the target box center, including that offset. Heading stays 0. The
aim script is not hosted, so the first particle is frame 1. Both units can
fly and hover-attack, so each sits at the surface plus cruise altitude 110.
Neither has a move order. Firing stops on the frame after death, and
particles still in flight are dropped. The killing particle is the one that
sets the death frame. The target stays in the unit arrays at health 0. The
particle is not drawn. There is no wreck, no shield, and no line-of-sight
check. The unit script is cob and is not hosted.

Scenario `duel_flame` keeps the commander at 7600, 4900. The shooter is
unit 1, `corcrwt4`, team 0, at 6800, 4600, y 159.298. The target is unit 2,
`corcrwt4`, team 1, at 7000, 4600, y 162.661. Separation is 200, inside the
range 500. The order is `0 1 attack 2`. The pair is not the hillside pair
at 7200, 4900 and 7400, 4900. The muzzle is 6800, 194.798, 4600. Frame 30
health is 21160, hits are 21, shots are 30, die is 0, and ground hits are
0. Frame 180 health is 15160 with 171 hits. The particle fired on frame 1
arrives on frame 10. Health 22000 takes 550 hits of 40. The particle fired
on frame 550 arrives on frame 559 and kills, so frame 720 health is 0,
hits are 550, shots are 559, die is 559, and ground hits are 0. Nine
particles already in flight are not stepped after that frame. `gate
--suite m5flame` is the self-check and exited 0 on engine output. It
recomputes the float32 particle from the hold-sample unit positions plus
the pinned boxes, damage 40, range 500, reload 0.39996, burst 12, burstrate
0.03333, velocity 500, and duration 1. It does not hardcode 559, 21160, or
550. It does not need a Recoil trace. m1 stays red without reference
pictures. The M4 milestone stays red until a Recoil trace matches
`eco_t1`, `build_tank`, and `reclaim` at 2%. The M5 milestone stays red
until the duel corpus is within 15% time to kill of a Recoil trace. The
next slice is torpedo.

Directories: `native/sim/weapon`, `native/shell`, `native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
missile path, the reclaim path, the lab production path, the builder
nanoframe range check, the team tick, the mex claim, solar and wind
income, movement, the commander walk host, the cob integrate, the path
slope test, or the stock game tree.

Tolerance: health is 1060, damage is 22, file range is 300, reload is 1.5
seconds, burst is 10, burstrate is 0.03333, and the burst cost is 10
energy. Opening energy is 900, income is 30 per second, and storage is
1000. The position slop is 1. The health slop is 0.0001. Do not lower
these to make a gate green. Do not put the pair back at 7200, 4900 and
7400, 4900.

## M5 torpedo

A `corsub` fires weapon 1, `TORPEDO`, a TorpedoLauncher, at another
`corsub`. `tracks` is omitted, so the shot does not home and the heading
stays the launch direction. `flighttime` is 3, so fuel is
`int(float32(3 * 30))` = 90. While fuel remains and the shot is at or
below the water plane, each update adds 0.2 to the speed if it is still
under `weaponvelocity`, then moves by that new vector. The collision test
uses the speed from before that add. `startvelocity` is 130 and
`weaponvelocity` is 200, so the per-frame speeds are 130/30 and 200/30.
`weaponacceleration` is not the thrust. After fuel, the shot drags by 0.98
and takes map gravity. It is not deleted on fuel expiry. `burnblow` is
true and `noexplode` is omitted, so the first entry into the target box
deals `damage.default` 342 and removes the shot. Area falloff is not
applied. `waterweapon` is true, so sitting underwater is not a hit. A shot
whose position is below the sea floor counts as a ground hit. Reload is
`max(1, int(float32(2.5 * 30)))` = 75. Burst is omitted, so one shot is
fired per reload. The shot spends no energy. The unit file sets
`waterline` 45 and does not set `floater`, so each sub sits on the sea
floor. `UBOAT4` has no maxslope, so the hull slope is 90. Neither sub has
a move order. The aim script is not hosted. The muzzle is the top center
of the shooter box. The aim point is the target box center. Heading stays
0. The box scales are 35, 22, 50 and the offsets are 0, 0, 0. The torpedo
is not drawn. There is no wreck, no shield, and no line-of-sight check.
The unit script is cob and is not hosted.

Scenario `duel_torpedo` keeps the commander at 7600, 4900. The shooter is
unit 1, `corsub`, team 0, at 1800, 1000, y -86.6523. The target is unit 2,
`corsub`, team 1, at 1800, 1200, y -86.9985. That line is the deep water
the ship scenario already sails. Separation is 200, inside the range 500.
The order is `0 1 attack 2`. The muzzle is 1800, -75.6523, 1000. Frame 30
health is 718, hits are 1, shots are 1, die is 0, and ground hits are 0.
Frame 180 health is 34 with 3 hits. Health 1060 takes 4 hits of 342. The
fourth shot kills on frame 253, so frame 720 health is 0, hits are 4,
shots are 4, die is 253, and ground hits are 0. `gate --suite m5torpedo`
is the self-check and exited 0 on engine output. It recomputes the float32
shot from the hold-sample unit positions plus the pinned boxes, damage
342, range 500, reload 2.5, start velocity 130, max velocity 200, flight
3, and map gravity tag 100. It does not hardcode 253 or 718. It does not
need a Recoil trace. m1 stays red without reference pictures. The M4
milestone stays red until a Recoil trace matches `eco_t1`, `build_tank`,
and `reclaim` at 2%. The M5 milestone stays red until the duel corpus is
within 15% time to kill of a Recoil trace. The next slice is bomb.

Directories: `native/sim/weapon`, `native/shell`, `native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
missile path, the lightning path, the flame path, the reclaim path, the
lab production path, the builder nanoframe range check, the team tick, the
mex claim, solar and wind income, movement, the commander walk host, the
cob integrate, the path slope test, or the stock game tree.

## M5 bomb

A `corshad` fires weapon 1, `CORBOMB`, an AircraftBomb, at an `armfav`.
The bomber has no collision box, so the muzzle is the airplane origin.
The target box scales are 21, 16, 26 and the offset is 0, 1, 0. Burst is
5. `burstrate` 0.26667 becomes `int(float32(0.26667 * 30))` = 8. Reload is
`int(float32(6 * 30))` = 180. Damage is 112 and the file range is 1280.
The drop test is the predicted landing window, not that range sphere.
`dropDist` is `max(1, burst - 1) * delay * speed2D * 0.5`. Once the window
opens, the rest of the burst fires without another range test. Each bomb
inherits the airplane velocity. Map gravity is added after the segment
test. `ttl` is 1000. The first entry into the target box deals
`damage.default` and removes that bomb. Area 168 is not applied. A bomb
below the ground, or below the water plane, counts as a ground hit. The
bomber also has a move order along +z so heading stays 0. A parked
airplane whose speed is still 0 stays put. One that is already moving
keeps flying after the goal clears. The chasing integration is unchanged.
The aim script is not hosted. The bomb is not drawn. There is no wreck, no
shield, and no line-of-sight check.

Scenario `duel_bomb` keeps the commander at 7600, 4900. The shooter is
unit 1, `corshad`, team 0, at 6800, 3600, y 218.047. The target is unit 2,
`armfav`, team 1, at 6800, 4340, y 44.3659. The orders are `0 1 6800 6400`
and `0 1 attack 2`. The first target, at z 4500, sat between the five
landing points and every bomb reached the ground, so the target was moved
to 4340. Frame 150 health is 105, hits are 0, shots are 5, die is 0, and
ground hits are 0. The killing bomb hits on frame 167. One earlier bomb in
the same burst reaches the ground. Frame 720 health is 0, hits are 1,
shots are 5, die is 167, and ground hits are 1. `gate --suite m5bomb` is
the self-check and exited 0 on engine output. It recomputes the airplane
and the bombs from the pinned 16-elmo height strip plus damage 112, range
1280, reload 6, burst 5, burstrate 0.26667, speed 234, acceleration
0.0575, cruise 165, pitch 0.625, and map gravity tag 100. It expects that
one ground hit. It does not hardcode 167. It does not need a Recoil trace.
m1 stays red without reference pictures. The M4 milestone stays red until
a Recoil trace matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The
M5 milestone stays red until the duel corpus is within 15% time to kill
of a Recoil trace. The next slice is dgun.

Directories: `native/sim/weapon`, `native/sim/move`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
missile path, the lightning path, the flame path, the torpedo path, the
reclaim path, the lab production path, the builder nanoframe range check,
the team tick, the mex claim, solar and wind income, the chasing airplane
integration, the commander walk host, the cob integrate, the path slope
test, or the stock game tree.

Tolerance: health is 1060, damage is 342, file range is 500, reload is 2.5
seconds, start velocity is 130, max velocity is 200, and flight time is 3
seconds. Map gravity tag is 100. The position slop is 1. The health slop
is 0.0001. Do not lower these to make a gate green. Do not put the pair
on the 6800, 4600 plateau.

## M5 dgun

A `corjugg` fires weapon 1, `JUGGERNAUT_FIRE`, a DGun, at an `armfav`.
The shooter volume is a CylY with scales 90, 80, 90 and offset 0, 0, 0.
That cylinder is used only as the muzzle box: the muzzle is the unit
origin plus the offset plus half the vertical scale. The piece radius is
not hosted. The target box scales are 21, 16, 26 and the offset is 0, 1, 0.
The aim point is that box center, not the top of the box. Burst is omitted,
so it is 1 and the delay is 0. Reload is `max(1, int(float32(4 * 30)))` =
120. Damage is 1200 and the file range is 590. Speed is `520/30`. The shot
spends no energy. The segment from the current position through this
frame's speed is tested before the move. Map gravity is added after the
move. `noexplode` is true, so a hit deals `damage.default` and does not
remove the shot. Area 65 is not applied. Once the shot has traveled farther
than the weapon range it is removed, and that removal is not a ground hit.
Sparks are not hosted. The duel stops weapon updates on the frame after the
target dies, so a one-shot kill stays at one hit. Neither hull has a move
order. The aim script and the cob turret are not hosted. The fireball is
not drawn. There is no wreck, no shield, and no line-of-sight check.

Scenario `duel_dgun` keeps the commander at 7600, 4900. The shooter is
unit 1, `corjugg`, team 0, at 6800, 4600, y 49.2977. The muzzle is 6800,
89.2977, 4600. The target is unit 2, `armfav`, team 1, at 6900, 4600, y
49.9701. The only order is `0 1 attack 2`. The gap is 100 elmos. The flight
ends before the first trace sample, so frame 30 is already the death
sample. Frame 30 health is 0, hits are 1, shots are 1, die is 6, and
ground hits are 0. Frame 120 repeats those counters. `gate --suite m5dgun`
is the self-check and exited 0 on engine output. It recomputes the
fireball from the traced unit positions plus damage 1200, range 590,
reload 4, velocity 520, and map gravity tag 100. It expects ground hits to
stay 0. It does not hardcode 6. It does not sample the terrain. It does
not need a Recoil trace. m1 stays red without reference pictures. The M4
milestone stays red until a Recoil trace matches `eco_t1`, `build_tank`,
and `reclaim` at 2%. The M5 milestone stays red until the duel corpus is
within 15% time to kill of a Recoil trace. The next slice is LOS.

Directories: `native/sim/weapon`, `native/sim/move`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
missile path, the lightning path, the flame path, the torpedo path, the
bomb path, the parked-airplane early return, the reclaim path, the lab
production path, the builder nanoframe range check, the team tick, the mex
claim, solar and wind income, the chasing airplane integration, the
commander walk host, the cob integrate, the path slope test, or the stock
game tree.

Tolerance: health is 105, damage is 1200, file range is 590, reload is 4
seconds, and velocity is 520. Map gravity tag is 100. The position slop is
1. The health slop is 0.0001. Do not lower these to make a gate green. Do
not move the pair farther apart. Do not give the juggernaut a move order.

## M5 LOS

Ground sight uses the attacker's `sightdistance` and the map's los square.
`losMipLevel` 3 and square size 8 make a square of 64 elmos. The eye is
the top of the collision box, `offset y + scale y / 2`. The ray samples
terrain at each whole square between the two units and not on either
endpoint. A sample above the straight line between the eyes blocks the
target. A pair whose horizontal distance is past sightdistance is not
targetable even when the ray is clear. Aircraft and underwater units are
not governed. A blocked or distant order does not start a weapon. A clear
order inside sight falls through to the weapon it already had. The aim
script is not hosted. Air LOS, radar, and sonar are not hosted.

Scenario `los` keeps the commander at 7600, 4900. Three `armfav` pairs
share sightdistance 635 and an eye lift of 9. The open pair is unit 1 at
6400, 4200, y 42.387, shooting unit 2 at 6560, 4200, y 42.3748. The
distance is 160, the one intermediate sample stays under the eye line, and
the BeamLaser starts. The blocked pair is unit 3 at 7008, 4736, y 55.1424,
shooting unit 4 at 7328, 4736, y 71.8432. The distance is 320, inside
sight, and the ridge between them is above the eye line, so no weapon
starts. The far pair is unit 5 at 6000, 4608, y 21.949, shooting unit 6 at
6000, 5312, y 25.5147. The distance is 704, past 635, so no weapon starts.
The orders refuse the blocked pair and the far pair before the open pair
fires. Frame 30 and frame 60 repeat see 1, see 0, see 0. `gate --suite los`
is the self-check and exited 0 on engine output. It recomputes the three
bits from the pinned height samples, sightdistance 635, mip 3, and the
armfav box. It does not need a Recoil trace. m1 stays red without reference
pictures. The M4 milestone stays red until a Recoil trace matches
`eco_t1`, `build_tank`, and `reclaim` at 2%. The M5 milestone stays red
until the duel corpus is within 15% time to kill of a Recoil trace. The
next slice is wrecks.

Directories: `native/sim/los`, `native/sim/move`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
missile path, the lightning path, the flame path, the torpedo path, the
bomb path, the dgun path, the parked-airplane early return, the reclaim
path, the lab production path, the builder nanoframe range check, the team
tick, the mex claim, solar and wind income, the chasing airplane
integration, the commander walk host, the cob integrate, the path slope
test, or the stock game tree.

Tolerance: sightdistance is 635, los mip is 3, and the map square is 8.
The position slop is 1. The health slop is 0.0001. Do not lower these to
make a gate green. Do not move the blocked pair off the ridge. Do not
shorten the far pair to 635 or less.

## M5 wreck

A unit that dies in a duel leaves one corpse at its position on that
frame. The corpse is not a unit, it is not drawn, and it does not pay the
team. Metal uses the same `floor(metalcost * ratio)` helper as reclaim.
The file feature metal is the fallback, and only when the unit is a
commander, an effigy, or is missing health or either cost. Sparks, the
heap, and the wreck mesh are not hosted.

Scenario `duel_wreck` keeps the commander at 7600, 4900. Unit 1 is
`corjugg` at 6800, 4600, y 49.2977. Unit 2 is `armfav` at 6900, 4600, y
49.9701. The only order is `0 1 attack 2`. The DGun kills the armfav on
frame 6. The corpse is `armfav` id 2 at 6900, 49.9701, 4600. Metal cost is
31, the ratio is 0.6, and the metal is 18. The file metal 15 does not
match. Frame 30 team metal is 102. Frame 120 team metal is 108, health is
0, die is 6, hits are 1, shots are 1, and ground hits are 0. The corpse is
unchanged at frame 120. Unit count stays 3. `gate --suite m5wreck` is the
self-check and exited 0 on engine output. It recomputes the death frame
from the dgun closed form and the metal from the shared floor. It does not
need a Recoil trace. m1 stays red without reference pictures. The M4
milestone stays red until a Recoil trace matches `eco_t1`, `build_tank`,
and `reclaim` at 2%. The M5 milestone stays red until the duel corpus is
within 15% time to kill of a Recoil trace. The next slice is M6.

Directories: `native/sim/build`, `native/sim/move`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the laser path, the cannon path, the beam path, the
missile path, the lightning path, the flame path, the torpedo path, the
bomb path, the dgun path, the LOS path, the parked-airplane early return,
the reclaim path, the lab production path, the builder nanoframe range
check, the team tick, the mex claim, solar and wind income, the chasing
airplane integration, the commander walk host, the cob integrate, the path
slope test, or the stock game tree.

Tolerance: metal cost is 31, the ratio is 0.6, and the formula metal is
18. The file metal is 15 and must not pass. Team metal starts at 100 with
income 2. The team slop is 0.05. The position slop is 1. The health slop
is 0.0001. Do not lower these to make a gate green. Do not pay the corpse
metal to the team. Do not spawn the corpse as a unit. Do not give the
juggernaut a move order.

## M6 spawn

Layer 1 hosts the synced half of `luarules/gadgets/game_initial_spawn.lua`
and `luarules/gadgets/game_team_resources.lua` before the first tick.
`Spring.CreateUnit` binds the commander native already placed and does not
add a second unit. Hide, paralyze, and movectrl are recorded and not
applied, so the commander still walks from frame 1. The warp-in stays
deferred. `SetTeamResource` writes the same start stocks the economy step
already used. A start script that replaces the side commander with `Unit=`
records the ask and leaves that unit in place.

Scenario `boot` keeps one Armada commander at 7600, 57.691, 4900. The
gadget asks for `armcom` team 0 unit 0 at that point, with hide, paralyze,
and movectrl set. boot.txt has no modoption override, so start metal and
energy are the file default 1000 and storage is 1000. Frame 30 still has
one unit, speed 0, and team metal 1000 and energy 1000, because income
that would pass storage is clamped. `gate --suite m6spawn` exited 0.
`gate --suite m2` and `gate --suite m3` were run again on fresh traces and
exited 0, so the spawn and the walk still match the Recoil traces. Weapon
suites were not re-run: CreateUnit bound the existing commander and
SetTeamResource wrote the same start values before tick 1. m1 stays red
without reference pictures. The M4 milestone stays red until a Recoil
trace matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The M5
milestone stays red until the duel corpus is within 15% time to kill of a
Recoil trace. The M6 milestone stays red until `ai_10min` holds a 1v1 for
ten minutes. The next slice is M6 layer 2, the unit-behavior gadgets a
duel regresses without.

Directories: `native/lua`, `native/map`, `native/shell`, `native/tools/gate`.

Do not rewrite: the weapon paths, the LOS path, the wreck path, the team
tick, the commander walk host, or the stock game tree. Do not apply the
commander warp. Do not spawn a second commander.

Tolerance: the commander stays within 1 elmo of 7600, 4900. Start metal
and energy are 1000. Both stores are 1000. Metal income is 2 and energy
income is 30, at 30 Hz. The team slop is 0.05. Do not lower these to make
a gate green.

## M6 behavior

Layer 2 keeps the synced Lua state open and hosts the UnitPreDamaged
chain for six default-on damage gadgets: Paralyze Damage Multiplier,
No Land Damage, unit_combomb_full_damage, Experimental Bots Steps
Damages, D-Gun Behaviour, and Mobile Unit Paralyze Damage Handler. Each
native damage subtraction asks that chain with paralyzer false, weapon
def 0, and projectile -1. A returned damage that differs by more than
0.0001 is recorded and not applied, so a gated shot keeps its number.

Scenario `duel_laser` runs 300 frames. The report has 18 calls, those
six names, an empty rewrite list, and deferred `dgun-projectile`.
`gate --suite m6behavior` exited 0. The same trace was checked with
`gate --suite m5laser` and exited 0: frame 30 health 1600 with 2 hits,
frame 300 health 0, die 262. `boot` and `move_kbot` were run again.
`gate --suite m2` and `gate --suite m3` exited 0. Frame 30 is 21.2512
elmos from the start. Frame 450 is at 8000, 56.5423, 4900. The commander
motion line is still slope 54 water 5000. Hide, paralyze, and movectrl
are still recorded and not applied.

The D-Gun ground-hug, the lightning splash projectiles, shields, and
`unit_script.lua` are not driven. Gfx gadgets, holiday gadgets,
`unit_reclaim_fix.lua`, scavengers, raptors, and AI stay unloaded. The
other M3 movement suites and the other M5 weapon suites were not re-run.
The filter sits in all nine damage sites. The laser duel is the one that
called it. m1 stays red without reference pictures. The M4 milestone
stays red until a Recoil trace matches `eco_t1`, `build_tank`, and
`reclaim` at 2%. The M5 milestone stays red until the duel corpus is
within 15% time to kill of a Recoil trace. The M6 milestone stays red
until `ai_10min` holds a 1v1 for ten minutes. The next slice is M6 layer
3, `ai_simpleai`, then `ai_10min`.

Directories: `native/lua`, `native/sim/weapon`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the closed weapon formulas, the LOS path, the wreck
path, the team tick, the commander walk host, or the stock game tree.
Do not apply a UnitPreDamaged return that differs. Do not drive the
D-Gun projectile.

Tolerance: the behavior report needs at least 1 call. The laser health
slop stays 0.0001. Do not lower these to make a gate green.

## M6 simpleai

Layer 3 loads `luarules/gadgets/ai_simpleai.lua` on every run, after the
spawn gadgets and the six damage gadgets. Teams without `LuaAI=SimpleAI`
leave it disabled, so boot and the duel corpus record no orders.
`simple_ai` is boot.txt plus `LuaAI=SimpleAI` on team 0. At frame 15 the
commander records a cloak toggle and then one extractor build. The build
is `armmex` at 7416, 53.8712, 4888, the closest metal spot more than 128
elmos from 7600, 4900. Frame 30 records the cloak toggle again and does
not queue a second extractor. GiveOrderToUnit is recorded and not
executed, so the commander is still at 7600, 57.691, 4900 and both stocks
are still 1000. `gate --suite m6ai` exited 0. The same binary re-ran the
laser duel, boot, and the commander walk. `gate --suite m5laser`,
`gate --suite m2`, and `gate --suite m3` exited 0. Frame 30 laser health
is 1600 with 2 hits. Frame 300 health is 0 and die is 262. The walk at
frame 30 is 21.2512 elmos from the start and frame 450 is at 8000,
56.5423, 4900. The motion line is still slope 54 water 5000.

The order is not a nanoframe. A second SimpleAI team is not spawned.
`ai_10min` was not run. Scavengers and raptors stay unloaded. m1 stays
red without reference pictures. The M4 milestone stays red until a Recoil
trace matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The M5
milestone stays red until the duel corpus is within 15% time to kill of
a Recoil trace. The M6 milestone stays red until `ai_10min` holds a 1v1
for ten minutes. The next slice is `ai_10min`, not scavengers.

Directories: `native/lua`, `native/defs`, `native/map`, `native/shell`,
`native/tools/gate`.

Do not rewrite: the closed weapon formulas, the LOS path, the wreck
path, the team tick, the commander walk host, or the stock game tree.
Do not execute the recorded build. Do not mark a corpus team SimpleAI.

Tolerance: the order must be `armmex` or `armuwms` within 1 elmo of the
closest open spot. The commander stays within 1 elmo of 7600, 4900.
Metal and energy stay within 0.05 of 1000. Do not lower these to make a
gate green.

## M6 ai_10min

Two Armada SimpleAI teams play Supreme Isthmus. Team 0 starts at 7600,
4900. Team 1 starts at 6400, 4200. They are different ally teams.
GiveOrderToUnit for a build is executed. The builder walks to the spot,
the nanoframe spends after income, and a finished building is added on
that team's resource row. An extractor adds mex income. A solar or a wind
generator adds energy. A lab uses the same power rule, and its products
are simulated and not drawn.

`simple_ai` stays the one-team order check. Frame 15 records the cloak
toggle and then `armmex` at 7416, 53.8712, 4888. Frame 30 is one armcom
at 7598.21, 57.6731, 4900.06 with metal 1000 and energy 1000. The walk
has started and has not reached the site. `gate --suite m6ai` exited 0.

`ai_10min` runs 18000 frames. Both commanders are still in the trace.
Team 0 mex income is 13.8133. Team 1 mex income is 16.1104. The trace has
156 units, including `armvp` and `armhp`. Team metal is 208.179 and
255.306. Winner is none. Wall time was 126.96 seconds, about 7.1 ms per
presented frame on average. A per-frame sim p95 was not recorded.
`gate --suite ai_10min` exited 0. The same binary re-ran the laser duel,
boot, and the commander walk. `gate --suite m5laser`, `gate --suite m2`,
`gate --suite m3`, and `gate --suite m6ai` exited 0, and `ai_10min` was
last. Frame 30 laser health is 1600 with 2 hits. Frame 300 health is 0
and die is 262. The walk at frame 30 is 21.2512 elmos from the start and
frame 450 is at 8000, 56.5423, 4900.

This gate does not compare stocks to Recoil. m1 stays red without
reference pictures. The M4 milestone stays red until a Recoil trace
matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The M5 milestone
stays red until the duel corpus is within 15% time to kill of a Recoil
trace. The M6 milestone stays red until a Recoil trace shows the
10-minute resource totals within 15%. Scavengers and raptors stay
unloaded. The next slice waits on that Recoil trace.

Directories: `native/lua`, `native/sim`, `native/shell`, `native/tools/gate`.

Do not rewrite: the closed weapon formulas, the LOS path, the wreck
path, the team tick, the commander walk host, or the stock game tree.
Do not draw the extra units. Do not mark a corpus team SimpleAI.

Tolerance: the `m6ai` position slop stays 1. `m6a_com_x` is 7598.21.
`m6a_com_z` stays 4900. Frame 30 stocks stay within 0.05 of 1000.
`ai_10min` requires both mex incomes above 0.01, at least 4 units, and
both armcoms at frame 18000. Do not lower these to make a gate green.

## M6 scavengers and raptors

Scavengers and raptors load on every run. A game that is not that mode
returns false before the spawn config. `scav` is one player team and one
ScavengersAI team. `raptor` is the same with RaptorsAI. The player
commander stays at 7600, 57.691, 4900. The mode commander is destroyed on
frame 1. A scavenger sample then placed `scavbeacon_t1_scav` on team 1.
A raptor sample placed `raptor_hive` on team 1. Neither structure is
drawn. The burrow search is seeded from the clock, so the structure
coordinates are not locked.

`gate --suite pve` exited 0 on frame 180. The same binary re-ran the
laser duel, boot, the commander walk, simple AI, and the 10-minute game.
`gate --suite m5laser`, `gate --suite m2`, `gate --suite m3`,
`gate --suite m6ai`, and `gate --suite pve` exited 0. `ai_10min` was
last and exited 0. That re-run had 114 units, mex income 6.9099 and
6.9006, both armcoms, and winner none. The floors stayed above 0.01 mex,
at least 4 units, and both armcoms. Frame 30 of simple AI is still
7598.21, 57.6731, 4900.06. The walk at frame 450 is still 8000, 56.5423,
4900. Laser frame 30 is still health 1600 with 2 hits, and frame 300 is
health 0 with die 262.

This gate does not compare the spawn to Recoil. m1 stays red without
reference pictures. The M4 milestone stays red until a Recoil trace
matches `eco_t1`, `build_tank`, and `reclaim` at 2%. The M5 milestone
stays red until the duel corpus is within 15% time to kill of a Recoil
trace. The M6 milestone stays red until a Recoil trace shows the
10-minute resource totals within 15%.

Directories: `native/lua`, `native/defs`, `native/sim/los`, `native/tools/gate`,
`native/oracle/scenarios`.

Do not rewrite: the closed weapon formulas, the weapon LOS ray, the wreck
path, the team tick, the commander walk host, or the stock game tree.
Do not draw the spawned structures. Do not mark a corpus team Scavengers
or Raptors. Do not apply the spawn gadget's hide, paralyze, or movectrl.

Tolerance: the player commander stays within 1 elmo of 7600, 57.691, 4900.
Frame 180 has 2 living units. The mode structure is on team 1 and inside
the 12288 map. The `ai_10min` floors did not move. Do not lower these to
make a gate green.

## M7 HUD

Plan exit (authoritative, `native/PLAN.md`): checklist complete, HUD
bookmarks under the M7 FLIP bar (0.22).

### Widget layers (done)

Verified by `gate --suite m7widgets`: resource bar/selection/cursors, build
menu+preview, minimap, health/range/tooltip, Retina/default widgets.
Recoil traces: select_commander, build_mex, minimap_move (equiv MOVE),
health_range. Only unit 0 is drawn.

### Player checklist (done)

`check_m7checklist.sh` exited 0. Items:

| Item | Self | Oracle |
|---|---|---|
| left-click select | pass | click_select cmd 10 |
| drag box | pass | self-only (UI geometry) |
| shift-queue | pass | shift_queue MOVE+shift |
| right-click attack | pass | attack cmd 20 |
| build facing | pass | facing 2 |
| control groups 1–9 | pass | self-only (client UI) |
| pause | pass | pause_speed event |
| gamespeed 1–4 | pass | pause_speed event |
| reclaim | pass | reclaim cmd 90 |
| repair | pass | repair cmd 40 |

### Outstanding for official M7

HUD FLIP bookmarks under `m7_flip` 0.22 against headed Recoil reference
shots. `gate --suite m7` stays red until `m7flip/flip.json` passes (checklist
json alone is not enough).

Directories: `native/ui`, `native/lua/gl`, `native/rhi/metal4`,
`native/shell`, `native/tools/gate`, `native/tools/check_m7*.sh`,
`native/oracle/recoil`, `native/oracle/traces`, `native/PLAN.md`.

Do not rewrite: closed weapon formulas, LOS, wreck path, team tick, stock
game tree. Do not draw projectiles, billboards, wrecks, or extra unit
meshes. Do not loosen tolerances.

Tolerance: widget-layer `[m7]` bars unchanged. Official exit adds
`m7_flip = 0.22`. Do not lower these to make a gate green.
