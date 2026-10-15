#pragma once

namespace bar {

// An unguided rocket. Speed is elmos per frame and does not arc.
// The aim script is not hosted, so the muzzle is chosen by the weapon, not a piece.
struct MissileRocket {
    bool alive = false;
    int ttl = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
    float speed = 0;
};

// Axis-aligned box. The offset is from the unit position. Extents are half sizes.
struct MissileBox {
    float ox = 0;
    float oy = 0;
    float oz = 0;
    float hx = 0;
    float hy = 0;
    float hz = 0;
};

// True when the segment from the rocket's position along its current velocity enters the box.
bool missile_rocket_hit(const MissileRocket& rocket, float unit_x, float unit_y, float unit_z, const MissileBox& box);

// While fuel remains, speed climbs toward the maximum by one acceleration step, then the rocket moves.
// Reaching ttl 0 removes the rocket. This slice does not drop it under gravity.
void missile_rocket_advance(MissileRocket& rocket, float accel, float max_speed);

}  // namespace bar
