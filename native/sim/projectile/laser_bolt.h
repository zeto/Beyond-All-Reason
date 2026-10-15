#pragma once

namespace bar {

// A LaserCannon bolt. The direction is unit length. speed is elmos per frame.
// The aim script is not hosted, so the muzzle is chosen by the weapon, not a piece.
struct LaserBolt {
    bool alive = false;
    bool check = false;
    int owner = -1;
    int ttl = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    float dx = 0;
    float dy = 0;
    float dz = 0;
    float speed = 0;
};

// Axis-aligned box. The offset is from the unit position. Extents are half sizes.
struct LaserBox {
    float ox = 0;
    float oy = 0;
    float oz = 0;
    float hx = 0;
    float hy = 0;
    float hz = 0;
};

// True when the segment from the bolt's position along one frame of travel enters the box.
// travel is the distance in elmos from the current position to the entry point.
bool laser_bolt_hit(const LaserBolt& bolt, float unit_x, float unit_y, float unit_z, const LaserBox& box,
                    float& travel);

// Move one frame and consume one collision check. A bolt that has used its ttl stops checking.
void laser_bolt_advance(LaserBolt& bolt);

}  // namespace bar
