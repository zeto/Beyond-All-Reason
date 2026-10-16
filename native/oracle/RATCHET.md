# Tolerance ratchet

No tolerance has been loosened.

M0 scores the boot trace on the presence of a commander, and that check is
not numeric.

M1 introduced its own bar with the terrain slice: a 1280x720 bookmark whose
luminance stddev is at least 8, plus a FLIP comparison that cannot pass
while the Recoil reference shots are absent. Those numbers have not moved.

M2 introduced its own bar with the commander slice: spawn xz within 1 elmo
of the boot start script, y inside the mapinfo height range, and a
1280x720 commander shot with luminance stddev at least 8 and at least 80
team-colored pixels. The Recoil boot trace is still required. Those numbers
have not moved.

M3 introduced its own bar with the ground-move slice: at frame 30 the
commander is between 2 and 40 elmos from its start, and by frame 450 it is
within 8 elmos of the scripted waypoint. The Recoil move trace is still
required. Those numbers have not moved.

M3 walk introduced its own bar from the engine's piece samples and
locked-camera shots. Between frames 30 and 150 the left thigh must change
by at least 0.35 radians across a sample gap of at most 20 frames, and the
two thighs must differ by at least 0.35 radians on some sample. Standing
still, frames 60 and 90 must move the left thigh by less than 0.05 radians
while the dish moves by more than 0.5 radians. Shots are 1280x720. The
team-pixel centroid stays within 0.12 of the frame center, and the team
bbox stays within 0.35 of the frame. The idle front view measured 169 team
pixels and a lower-half xor of 0. The walking side view measured 63 to 88
team pixels, and the strongest 15-frame lower-half xor was 71 (the quietest
pair was 52). The visibility minimum is 40, under that side view and above
an empty frame. The leg-pixel bar is 24, between the still idle and the
walk. The cap is 20000. Those numbers have not moved.

M3 ship introduced its own bar from the BOAT3 move class and the unit's
acceleration. The class minimum depth is 8, so every sample's terrain
height must be at most -8. The hull y stays within 1 elmo of the waterline
at 0. At frame 30 the accel formula (0.1004 elmos per frame squared, no
turn) puts the ship about 46.7 elmos from the start; the band is 20 to 80.
By frame 240 it is within 8 elmos of the water waypoint, and from frame 270
it is still there after the land order. The land point stays more than 8
elmos away. Those numbers have not moved.

The locked shot is 1280x720. The hull is dark metal. A full-frame team-color
count on the end shot is the red shore, 1257 pixels, and 2 of those sit on
the ship, so team color is not the visibility check. The check counts pixels
with r+g+b under 140 inside x 0.35 to 0.65 and y 0.35 to 0.75. The end frame
measured 2269, centroid 0.500 and 0.568, box 0.125. The start frame measured
2316, centroid 0.500 and 0.568, box 0.124. Open water in a side window
measured 0. The floor is 800, under that hull and above empty water. The cap
is 20000. The centroid stays within 0.12 of the frame center. The box stays
within 0.35. Those numbers have not moved.

M3 hover introduced its own bar from the HOVER2 slope and the unit's
acceleration. HOVER2 is 33 degrees after the 1.5 slope restore. The craft
turns about 1.07 radians before it rolls, then accelerates at 0.1105 elmos
per frame squared. At frame 30 that is about 8.6 elmos from the start; the
band is 4 to 20. By frame 150 it is within 8 elmos of the water point
5704, 3680. From frame 210, after the cliff order, it is still there and
more than 8 elmos from 7248, 4016. On those water samples the terrain is
at or below -30 and the craft y stays within 1 of the surface. No sample
climbs above 12. The beach lane tops out near 6.6, and the refused cliff
is 237. Those numbers have not moved.

The locked hover shot is 1280x720. The craft is dark. The end frame
measured 1519 dark pixels in the locked middle, centroid 0.502 and 0.563,
box 0.069. The start frame measured 1159. A side window of open water
measured 0. Team color in that middle was 16 pixels, so team color is not
the check. The floor is 500, under that craft and above empty water. The
cap is 20000. The centroid stays within 0.12 of the frame center. The box
stays within 0.35. Those numbers have not moved.

M3 gunship introduced its own bar from armkam's cruise and acceleration.
The cruise altitude is 70. The gunship turns about 2.64 radians before it
flies, then accelerates at 0.15 elmos per frame squared. At frame 30 that
is about 8.2 elmos from the start; the band is 4 to 20. By frame 180 it is
within 8 elmos of 7600, 4300. Every sample stays within 1 elmo of the
surface plus 70, where the surface is the ground or the sea. Those numbers
have not moved.

The locked gunship shot is 1280x720. The end frame, over the plateau,
measured 2195 dark pixels in the locked middle, centroid 0.500 and 0.562,
box 0.095. Side windows of grass measured 0. Team color in that middle was
26 pixels, so team color is not the check. The floor is 800, under that
craft and above empty grass. The cap is 20000. The centroid stays within
0.12 of the frame center. The box stays within 0.35. Those numbers have
not moved.

M3 airplane introduced its own bar from armfig's raw unit file. The air
rework table is not applied. Speed is 289.2 elmos per second, acceleration
0.1825 elmos per frame squared, rudder 0.00615 radians per frame, aileron
0.0144, and max bank 0.8. The yaw rate is also capped by speed over the
turn radius of 64, which does not bind once the fighter is moving. At frame
30 the straight accel formula puts it about 84.9 elmos from the start; the
band is 70 to 100. By frame 150 it is within 8 elmos of 7600, 6096.21875.
From frame 30 on, speed stays at least 5 elmos per frame, so it does not
stop to arrive or to turn. Every sample stays within 1 elmo of the surface
plus cruise 125. At frame 240, sixty rudder frames after the east order,
the bank is at least 0.7. Those numbers have not moved.

The locked airplane shot is 1280x720. The end frame, banked over open
water, measured 1181 dark pixels in the locked middle, centroid 0.508 and
0.598, box 0.113. A side window of water measured 0. Team color in that
middle was 5 pixels, so team color is not the check. The floor is 500,
under that craft and above empty water. The cap is 20000. The centroid
stays within 0.12 of the frame center. The box stays within 0.35. Those
numbers have not moved.

M3 cob introduced its own bar from the armbull script constants. StartMoving
spins lwheel and rwheel on X. The per-second speed starts at 0, gains 910
cob units each integrate, and stops gaining at 20020. Each integrate adds
that speed times pi/32768/30 to the angle. The gate recomputes that sum
from the engine's spin count and compares the wrapped angle. The slop is
0.02 radians, under one full-speed step of about 0.064. The spin count at
the sample must be at least 40, so StartMoving actually ran. smoke1 stays
hidden. The sample frame is 300. A run on this machine spun 269 times and
both wheels matched the sum within 0.00001. Those numbers have not moved.

The locked tank shot is 1280x720. The end frame, on the grass, measured
5015 dark pixels in the locked middle, centroid 0.505 and 0.569, box 0.100.
A side window of grass measured 0. Team color in that middle was 57 pixels,
so team color is not the check. The floor is 2000, under that hull and
above empty grass. The cap is 20000. The centroid stays within 0.12 of the
frame center. The box stays within 0.35. Those numbers have not moved.

M3 tank class introduced its own bar for HTANK4. The class slope is 27
degrees and the water depth is 20. The unit file's 12 and 15 are not the
path limits. The shelf at 1200, 2352 sits at gy -18.63, deeper than 15 and
shallower than 20. The straight approach from 1040, 2352 is about 22
degrees, so a 12 degree limit has no path onto that cell. Frame 90 is 40
to 90 elmos from the start. Frame 150 is within 8 elmos of the shelf, gy
is between -19.5 and -16, and y matches gy within 0.05. From frame 210
the deeper order at 1200, 2224 has not moved the tank. A run on this
machine was 62.6 elmos out at frame 90 and on the shelf from frame 150
through 300. Those numbers have not moved.

The locked shelf shot is 1280x720. The end frame, wading, measured 832
dark pixels in the locked middle, centroid 0.515 and 0.527, box 0.052.
A right-hand window of open water measured 0. Team color in that middle
was 21 pixels, so team color is not the check. The floor is 400, under
that hull and above empty water. The cap is 20000. The centroid stays
within 0.12 of the frame center. The box stays within 0.35. Those
numbers have not moved.

M4 team resources introduced its own bar from the commander and the
resource gadget. modoptions.lua starts a team at 1000 metal and 1000
energy with 1000 storage. game_team_resources.lua will not let storage
sit under 1000, and the commander's own 500 is one input to that max,
not a second tank added afterward. This scenario sets the stock to 100
metal and 970 energy so the tick is visible, and leaves storage at the
file default. armcom makes 2 metal and 30 energy per second. Each frame
adds that income divided by 30 and clamps to storage. Frame 30 is one
second: metal 102, energy 1000. Frame 60 stays at energy 1000; without
the clamp it would be 1030. The slop is 0.05, under one frame of either
income. Those numbers have not moved.

M4 mex income introduced its own bar from the metal map and armmex.
mapinfo.lua lowercases keys, so the scale is maxmetal 0.93 and the radius
is extractorradius 90. A square's metal is its byte times 0.93. The mex
depth is extractsmetal 0.001, and the rate is that depth times the sum of
the bytes whose square centers fall inside the radius. The spot at 6024,
5384 sums to 4629 bytes, which is 4.30497 metal per second. The mex at
136, 136 covers 97 squares and sums to 0. Both mexes pay 3 energy, so the
team upkeep is 6. The commander still makes 2 metal and 30 energy. Each
frame adds the makes, subtracts the upkeep when energy can cover it, adds
the ground metal only then, and clamps. Frame 30 is metal 106.305 and
energy 994. Frame 60 is metal 112.61 and energy 1000. The stock slop is
0.05. The depth and scale slop is 0.0001. Those numbers have not moved.

M4 solar and wind introduced its own bar from armsolar, armwin, and the
map atmosphere. armsolar energyupkeep is -20, which is 20 energy per
second while the building is on. armwin windgenerator is 25, a cap.
Supreme Isthmus atmosphere is minwind 1 and maxwind 19. The strength is
the average, 10, and the wind income is the lesser of that and the cap.
The commander still makes 2 metal and 30 energy. Solar and wind add no
metal and no upkeep. Storage stays 1000. The scenario starts at 100 metal
and 900 energy so the energy tick is visible before the cap. Frame 30 is
metal 102 and energy 960. Frame 60 is metal 104 and energy 1000; without
the clamp it would be 1020. The stock slop is 0.05. The upkeep, cap, and
wind-strength slop is 0.0001. Those numbers have not moved.

M4 nanoframe introduced its own bar from armcom's workertime and armwin's
cost. Build power is 300 per second at multiplier 1, so each frame adds
10/1600 of the building. Sixty frames reach progress 0.375 and health
73.6, from a start of 0.1. The click at 7680, 4900 snaps to 7688, 4904.
Metal cost 40 and energy cost 175 are spent after that frame's income.
Frame 30 is metal 94.5 and energy 897.1875. Frame 60 is metal 89 and
energy 894.375. The metal stock prints 88.9999. The stock slop is 0.05.
The progress, health, and cost slop is 0.0001. The frame stays unfinished.
Those numbers have not moved.

M4 lab introduced its own bar from armvp's workertime and armfav's cost.
Build power is 150 per second at multiplier 1, so each frame adds 5/1100
of the scout. The product stays on the lab at 7280, 4900. The yard script,
the build stance, the pad offset, and the walk out are not run. Frame 30
is progress 0.136364, health 14.4182, metal 97.7728, and energy 879.545.
Frame 210 is still open at progress 0.954545. The remainder step finishes
the unit. Frame 240 is progress 1, health 105, metal 85.0001, and energy
769.998, which is the full cost plus the commander's income. The stock
slop is 0.05. The progress, health, and cost slop is 0.0001. Those numbers
have not moved.

M4 reclaim introduced its own bar from armcom's reclaim speed and the
armstump corpse. The file metal 152 is replaced by floor(225 * 0.6) = 135.
Feature energy is 0, so reclaim time is (135 + 0) * 6 = 810. Reclaim power
is 300 per second at multiplier 1, so each frame adds 10/810 of the wreck.
The build stance is not hosted, so reclaim starts on the order frame.
Method 0 returns metal gradually. The energy cost factor is 0. Frame 30
left is 0.62963, metal is 152, and energy is 930. The wreck is still open
on frame 81. The remainder step finishes it on frame 82. Frame 90 left is
0, the wreck is gone, metal is 241, and energy is 990, which is the full
wreck metal plus the commander's income. The stock slop is 0.05. The
remaining-fraction slop is 0.0001. Those numbers have not moved.

M5 laser introduced its own bar from cormando's LaserCannon and the
armstump box. Reload is 0.5 seconds, so the weapon fires every 15 frames.
Speed is 930/30 = 31. File range 300 rounds to 279. Burnblow defaults
true. Damage is 100 and health is 1800, so the eighteenth hit kills. The
aim script and the piece volumes are not hosted. The muzzle is the top of
the shooter box. Frame 30 health is 1600, hits are 2, shots are 2, and
die is 0. Frame 300 health is 0, hits are 18, shots are 18, die is 262,
and ground hits are 0. Damage is 100, speed is 31, range is 279, and
reload is 15. The shooter y is 73.1482 and the muzzle y is 98.1482. The
target y is 53.9294. The health slop is 0.0001. The position slop is 1.
Those numbers have not moved.

M5 cannon introduced its own bar from armstump's Cannon and the two unit
boxes. Reload is 1.2 seconds, so the weapon fires every 36 frames. Speed
is 225/30 = 7.5. Range stays the file range 350. Map gravity tag 100
becomes -0.111111 per frame. HeightMod is 0.8. Damage is 97 and health is
1800, so the nineteenth hit kills. The aim script and the piece volumes
are not hosted. The muzzle is the top of the shooter box. The aim point
is the target box center. Frame 30 health is 1703, hits are 1, shots are
1, and die is 0. Frame 720 health is 0, hits are 19, shots are 19, die is
673, and ground hits are 0. Damage is 97, speed is 7.5, range is 350, and
reload is 36. The shooter y is 49.2977 and the muzzle y is 59.2977. The
target y is 52.6612. The health slop is 0.0001. The position slop is 1.
Those numbers have not moved.

The boot and move_kbot Recoil traces were captured from the BAR Launcher
spring 2026.07.04. Boot y is 57.687881. The move ends at 7993.4785,
4900.1904. The 1 elmo spawn bar and the 8 elmo waypoint bar were already
in force. Those numbers have not moved.

M5 beam introduced its own bar from armfav's BeamLaser and the two unit
boxes. Beamtime is 0.18 seconds. `int(beamtime * 30)` is 5, so each salvo
is 5 ticks and each tick deals 35/5 times the hit intensity. Reload is 1
second, so the next salvo starts 30 frames after the first tick. Heightmod
is 1. Damage is 35 and health is 105. The aim script and the piece volumes
are not hosted. The muzzle is the top of the shooter box. The aim point
is the target box center. Intensity uses the ray entry, not the center.
Frame 30 health is 80.6655, hits are 5, shots are 5, and die is 0. Frame
300 health is 0, hits are 22, shots are 22, die is 122, and ground hits
are 0. Intensity is 0.695272. The shooter y is 49.2977 and the muzzle y
is 58.2977. The target y is 50.0023. The health slop is 0.0001. The
position slop is 1. Those numbers have not moved.

M5 missile introduced its own bar from legbal's MissileLauncher and the two
unit boxes. Start velocity and weapon velocity are 230, so the per-frame
speed is 230/30. Acceleration is 120/(30*30) and does not increase a
rocket that already starts at the cap. Reload is 5.5 seconds, so the next
shot is 165 frames later. Heightmod is 0.2. Damage is 250 and health is
900. Flight time is omitted, so ttl is ceil(max(distance, range) /
maxSpeed). The aim script and the piece volumes are not hosted. The muzzle
is the top of the shooter box. The aim point is the target box center.
Splash is not applied. A rocket whose fuel expires is removed. Frame 90
health is 650, hits are 1, shots are 1, and die is 0. Frame 720 health is
0, hits are 4, shots are 4, die is 520, and ground hits are 0. The shooter
y is 49.2977 and the muzzle y is 65.2977. The target y is 52.6612. The
health slop is 0.0001. The position slop is 1. Those numbers have not moved.

M5 lightning introduced its own bar from armlatnk's LightningCannon and the
two unit boxes. Damage is 22 and health is 1060. Range is 300. Heightmod
is 1. Burst is 10. Burstrate 0.03333 becomes int(burstrate * 30) = 0, so
the bolts are consecutive frames. Reload is 1.5 seconds, 45 frames, stamped
when the burst starts. Energy per shot is 10 and is spent once per burst.
Opening energy is 900, income is 30 per second, and storage is 1000. The
aim script and the piece volumes are not hosted. The muzzle is the top of
the shooter box. The aim point is the target box center. Chain sparks are
not applied. Frame 30 health is 840, hits are 10, shots are 10, die is 0,
ground hits are 0, and energy is 920. Frame 720 health is 0, hits are 49,
shots are 49, die is 189, ground hits are 0, and energy is 1000. The
shooter y is 49.2977 and the muzzle y is 62.2977. The target y is 52.6612.
The health slop is 0.0001. The position slop is 1. Those numbers have not moved.

M5 flame introduced its own bar from corcrwt4's Flame and the two unit
boxes. Damage is 40 and health is 22000. Range is 500. Weapon velocity is
500, so the per-frame speed is 500/30. Duration is 1, and the float32
lifetime ceil(max(distance, range) / speed * duration) is 31. Burst is 12.
Burstrate 0.03333 becomes int(burstrate * 30) = 0, so the particles are
consecutive frames. Reload is 0.39996 seconds, 11 frames, stamped when the
burst starts. The shot spends no energy. The aim script and the piece
volumes are not hosted. The muzzle is the top of the shooter box. The aim
point is the target box center. The box offsets are 0, 10, 0 and the scales
are 80, 51, 120. Area falloff is not applied. A particle that expires is
removed. Frame 30 health is 21160, hits are 21, shots are 30, die is 0, and
ground hits are 0. Frame 720 health is 0, hits are 550, shots are 559, die
is 559, and ground hits are 0. The shooter y is 159.298 and the muzzle y is
194.798. The target y is 162.661. The health slop is 0.0001. The position
slop is 1. Those numbers have not moved.

M5 torpedo introduced its own bar from corsub's TorpedoLauncher and the two
unit boxes. Damage is 342 and health is 1060. Range is 500. Start velocity
is 130 and weapon velocity is 200, so the per-frame speeds are 130/30 and
200/30. Flight time is 3 seconds, 90 frames. A powered underwater update
adds 0.2 when the speed is still under the maximum, then moves. Reload is
2.5 seconds, 75 frames. The shot spends no energy. The aim script and the
piece volumes are not hosted. The muzzle is the top of the shooter box. The
aim point is the target box center. The subs are not floaters, so each sits
on the sea floor. Tracking, homing, and area falloff are not applied. Frame
30 health is 718, hits are 1, shots are 1, die is 0, and ground hits are 0.
Frame 720 health is 0, hits are 4, shots are 4, die is 253, and ground hits
are 0. The shooter y is -86.6523 and the muzzle y is -75.6523. The target y
is -86.9985. The health slop is 0.0001. The position slop is 1. Those
numbers have not moved.

M5 bomb introduced its own bar from corshad's AircraftBomb and the armfav
box. Damage is 112 and health is 105. Range is 1280. Burst is 5. Burstrate
0.26667 becomes int(float32(burstrate * 30)) = 8. Reload is 6 seconds, 180
frames. Speed is 234, so the per-frame maximum is 234/30. Acceleration is
0.0575 per frame. Cruise is 165 and max pitch is 0.625. Map gravity tag
100 is -0.111111 per frame. The shot spends no energy. ttl is 1000. The
aim script is not hosted. The bomber has no collision box, so the muzzle
is the airplane origin. The aim point is the target box center. The box
offset is 0, 1, 0 and the scales are 21, 16, 26. Area falloff is not
applied. One earlier bomb in the burst reaches the ground. Frame 150
health is 105, hits are 0, shots are 5, die is 0, and ground hits are 0.
Frame 720 health is 0, hits are 1, shots are 5, die is 167, and ground
hits are 1. The shooter y is 218.047. The target y is 44.3659. The health
slop is 0.0001. The position slop is 1. Those numbers have not moved.

M5 dgun introduced its own bar from corjugg's DGun and the armfav box.
Damage is 1200 and health is 105. Range is 590. Reload is 4 seconds, 120
frames. Velocity is 520, so the per-frame speed is 520/30. Burst is
omitted, so it is 1 and the delay is 0. Map gravity tag 100 is -0.111111
per frame and is added after the move. The shot spends no energy. The aim
script and the piece radius are not hosted. The shooter volume is a CylY
used only as the muzzle box. The muzzle is the unit origin plus half the
vertical scale. The aim point is the target box center. The box offset is
0, 1, 0 and the scales are 21, 16, 26. Area falloff and sparks are not
applied. A hit does not remove the shot. Frame 30 health is 0, hits are 1,
shots are 1, die is 6, and ground hits are 0. Frame 120 repeats those
counters. The shooter y is 49.2977 and the muzzle y is 89.2977. The target
y is 49.9701. The health slop is 0.0001. The position slop is 1. Those
numbers have not moved.

M5 LOS introduced its own bar from armfav's sightdistance and the map los
grid. Sightdistance is 635. losMipLevel is 3 and the map square is 8, so
the los square is 64. The eye lift is 9, the top of the box whose offset
is 0, 1, 0 and whose scales are 21, 16, 26. The open pair is 160 elmos
apart and may fire. The blocked pair is 320 elmos apart and the ridge
keeps it from firing. The far pair is 704 elmos apart and may not fire.
Frame 30 and frame 60 keep see 1, see 0, and see 0. The open shooter y is
42.387 and the open target y is 42.3748. The blocked shooter y is 55.1424
and the blocked target y is 71.8432. The far shooter y is 21.949 and the
far target y is 25.5147. The health slop is 0.0001. The position slop is
1. Those numbers have not moved.

M5 wreck introduced its own bar from armfav's metal cost and the wreck
ratio. Metal cost is 31. The ratio is 0.6. floor(31 * 0.6) is 18. The file
feature metal is 15 and must not match. The corpse is born on the dgun
death frame, which the closed form puts at 6. The corpse stays at 6900,
49.9701, 4600. Frame 30 team metal is 102. Frame 120 team metal is 108.
The health slop is 0.0001. The position slop is 1. The team slop is 0.05.
Those numbers have not moved.

M6 spawn introduced its own bar from the boot start script and the two
layer-1 gadgets. The commander stays at 7600, 57.691, 4900. CreateUnit
binds sim index 0. Hide, paralyze, and movectrl are 1 and are not applied.
boot.txt does not override start metal or energy, so both stocks are the
modoption default 1000 and both stores are 1000. Frame 30 metal is 1000
and energy is 1000, because income that would pass storage is clamped.
The position slop is 1. The team slop is 0.05. Those numbers have not moved.

M6 spawn introduced its own bar from the boot start script and the two
layer-1 gadgets. The commander stays at 7600, 57.691, 4900. CreateUnit
binds sim index 0. Hide, paralyze, and movectrl are 1 and are not applied.
boot.txt does not override start metal or energy, so both stocks are the
modoption default 1000 and both stores are 1000. Frame 30 metal is 1000
and energy is 1000, because income that would pass storage is clamped.
The position slop is 1. The team slop is 0.05. Those numbers have not moved.

M6 behavior introduced its own bar from the laser duel and the six
UnitPreDamaged gadgets. The laser still deals 100. Frame 30 health is
1600 with 2 hits. Frame 300 health is 0 and die is 262. The chain was
called 18 times and recorded no rewrite. The commander walk still leaves
the start by 21.2512 elmos at frame 30 and arrives at 8000, 56.5423,
4900 on frame 450. The health slop is 0.0001. The position slop is 1.
Those numbers have not moved.

M6 simpleai introduced its own bar from the SimpleAI commander and the
map's metal spots. The recorded extractor is armmex at 7416, 4888, the
closest spot more than 128 elmos from 7600, 4900. The order is not
executed. Frame 30 still has one armcom at 7600, 57.691, 4900 with metal
1000 and energy 1000. The laser still shows health 1600 and 2 hits at
frame 30, and health 0 with die 262 at frame 300. The walk still leaves
the start by 21.2512 elmos at frame 30 and arrives at 8000, 56.5423,
4900 on frame 450. The position slop is 1. The team slop is 0.05.
Those numbers have not moved.

M6 ai_10min kept the simpleai position slop at 1 and the team slop at
0.05. The one-team commander now walks, so frame 30 is 7598.21, 4900.06.
`m6a_com_x` is that measured x. `m6a_com_z` stays 4900, and 4900.06 is
inside the same slop. Frame 30 metal and energy stay within 0.05 of 1000.
The 10-minute bar is both armcoms present, at least 4 units, and both
teams' mex income above 0.01. Frame 18000 had 156 units, mex income
13.8133 and 16.1104, and metal 208.179 and 255.306. Those floors stayed
where they were set. No Recoil resource trace was added. The M4 2% bar
and the M5 15% time-to-kill bar have not moved.

M6 scavengers and raptors kept every earlier floor. Frame 180 has 2
living units. The player commander stays within 1 elmo of 7600, 57.691,
4900. The mode structure is on team 1 and inside the 12288 map. Its
coordinates are not locked. A later 10-minute run had 114 units and mex
income 6.9099 and 6.9006, still above the same 0.01 floor, with both
armcoms present. No floor moved. No Recoil resource trace was added.
The M4 2% bar and the M5 15% time-to-kill bar have not moved.

M7 introduced its own bar from the HUD self-checks. End-frame top-bar
text coverage stays at least 40 lit non-chrome pixels. Wind and counter
pods need at least 20 and 8 lit glyph pixels. Income red/green glyphs
need at least 20 colored pixels and an edge count of at least half that
(crisp outline, not a same-color blob). Selection keeps sel [1], cursor
cursormove, cmd 10. Build keeps armmex at 7416, 4888 with a live nano
id. Minimap click stays within 250 elmos of 8000, 4900. HUD hover stays
armcom at half health with range 300; the orange ring count stays between
800 and 30000 with median radius above 100. Retina drawable stays at
least 2800x1700; topbar grey rows stay within 10% of height; panel
stability mean_diff stays under 25. Magenta tiles stay 0. The ai_10min
floors did not move. Those numbers have not moved.
