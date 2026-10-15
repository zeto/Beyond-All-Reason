#pragma once

namespace bar {

// A cannon shell. Velocity is elmos per frame and changes under gravity.
// The aim script is not hosted, so the muzzle is chosen by the weapon, not a piece.
struct CannonShell {
    bool alive = false;
    int ttl = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    float vx = 0;
    float vy = 0;
    float vz = 0;
};

// Axis-aligned box. The offset is from the unit position. Extents are half sizes.
struct CannonBox {
    float ox = 0;
    float oy = 0;
    float oz = 0;
    float hx = 0;
    float hy = 0;
    float hz = 0;
};

// True when the segment from the shell's position along its current velocity enters the box.
bool cannon_shell_hit(const CannonShell& shell, float unit_x, float unit_y, float unit_z, const CannonBox& box);

// Gravity is applied, then the shell moves. A shell whose ttl reaches 0 stops.
void cannon_shell_advance(CannonShell& shell, float gravity);

}  // namespace bar
