#include "projectile/laser_bolt.h"

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

bool laser_bolt_hit(const LaserBolt& bolt, float unit_x, float unit_y, float unit_z, const LaserBox& box,
                    float& travel) {
    if (!bolt.alive || !bolt.check || !(bolt.speed > 0.f)) {
        return false;
    }
    const float min_x = unit_x + box.ox - box.hx;
    const float max_x = unit_x + box.ox + box.hx;
    const float min_y = unit_y + box.oy - box.hy;
    const float max_y = unit_y + box.oy + box.hy;
    const float min_z = unit_z + box.oz - box.hz;
    const float max_z = unit_z + box.oz + box.hz;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!slab(bolt.x, bolt.dx, min_x, max_x, tmin, tmax) || !slab(bolt.y, bolt.dy, min_y, max_y, tmin, tmax) ||
        !slab(bolt.z, bolt.dz, min_z, max_z, tmin, tmax)) {
        return false;
    }
    if (tmax < 0.f) {
        return false;
    }
    const float enter = tmin < 0.f ? 0.f : tmin;
    // The collision test covers this frame's travel, including the far end of the segment.
    if (enter > bolt.speed) {
        return false;
    }
    travel = enter;
    return true;
}

void laser_bolt_advance(LaserBolt& bolt) {
    if (!bolt.alive || !bolt.check) {
        bolt.alive = false;
        return;
    }
    bolt.x += bolt.dx * bolt.speed;
    bolt.y += bolt.dy * bolt.speed;
    bolt.z += bolt.dz * bolt.speed;
    // Pre-decrement: a bolt created with ttl N is tested on the frames where ttl is N..0.
    bolt.check = (--bolt.ttl) >= 0;
    if (!bolt.check) {
        bolt.alive = false;
    }
}

}  // namespace bar
