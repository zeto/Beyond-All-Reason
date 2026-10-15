#include "projectile/missile_rocket.h"

#include <algorithm>
#include <cmath>

namespace bar {
namespace {

bool slab(float origin, float direction, float min_bound, float max_bound, float& tmin, float& tmax) {
    if (std::fabs(direction) < 1.0e-8f) {
        return origin >= min_bound && origin <= max_bound;
    }
    float enter = (min_bound - origin) / direction;
    float leave = (max_bound - origin) / direction;
    if (enter > leave) {
        std::swap(enter, leave);
    }
    tmin = std::max(tmin, enter);
    tmax = std::min(tmax, leave);
    return tmin <= tmax;
}

}  // namespace

bool missile_rocket_hit(const MissileRocket& rocket, float unit_x, float unit_y, float unit_z, const MissileBox& box) {
    if (!rocket.alive) {
        return false;
    }
    const float speed_sq = rocket.vx * rocket.vx + rocket.vy * rocket.vy + rocket.vz * rocket.vz;
    if (!(speed_sq > 0.f)) {
        return false;
    }
    const float speed = std::sqrt(speed_sq);
    const float dx = rocket.vx / speed;
    const float dy = rocket.vy / speed;
    const float dz = rocket.vz / speed;
    const float min_x = unit_x + box.ox - box.hx;
    const float max_x = unit_x + box.ox + box.hx;
    const float min_y = unit_y + box.oy - box.hy;
    const float max_y = unit_y + box.oy + box.hy;
    const float min_z = unit_z + box.oz - box.hz;
    const float max_z = unit_z + box.oz + box.hz;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!slab(rocket.x, dx, min_x, max_x, tmin, tmax) || !slab(rocket.y, dy, min_y, max_y, tmin, tmax) ||
        !slab(rocket.z, dz, min_z, max_z, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    const float enter = tmin < 0.f ? 0.f : tmin;
    return enter <= speed;
}

void missile_rocket_advance(MissileRocket& rocket, float accel, float max_speed) {
    if (!rocket.alive) {
        return;
    }
    // Fuel is checked before the move. A rocket that still has a frame left may speed up, then travels.
    if (rocket.ttl - 1 > 0 && rocket.speed < max_speed) {
        const float faster = rocket.speed + accel;
        if (rocket.speed > 0.f) {
            const float scale = faster / rocket.speed;
            rocket.vx *= scale;
            rocket.vy *= scale;
            rocket.vz *= scale;
        }
        rocket.speed = faster;
    }
    rocket.x += rocket.vx;
    rocket.y += rocket.vy;
    rocket.z += rocket.vz;
    if (--rocket.ttl == 0) {
        rocket.alive = false;
    }
}

}  // namespace bar
