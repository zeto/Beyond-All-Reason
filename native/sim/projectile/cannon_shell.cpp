#include "projectile/cannon_shell.h"

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

bool cannon_shell_hit(const CannonShell& shell, float unit_x, float unit_y, float unit_z, const CannonBox& box) {
    if (!shell.alive) {
        return false;
    }
    const float speed_sq = shell.vx * shell.vx + shell.vy * shell.vy + shell.vz * shell.vz;
    if (!(speed_sq > 0.f)) {
        return false;
    }
    const float speed = std::sqrt(speed_sq);
    const float dx = shell.vx / speed;
    const float dy = shell.vy / speed;
    const float dz = shell.vz / speed;
    const float min_x = unit_x + box.ox - box.hx;
    const float max_x = unit_x + box.ox + box.hx;
    const float min_y = unit_y + box.oy - box.hy;
    const float max_y = unit_y + box.oy + box.hy;
    const float min_z = unit_z + box.oz - box.hz;
    const float max_z = unit_z + box.oz + box.hz;
    float tmin = 0.f;
    float tmax = 1.0e9f;
    if (!slab(shell.x, dx, min_x, max_x, tmin, tmax) || !slab(shell.y, dy, min_y, max_y, tmin, tmax) ||
        !slab(shell.z, dz, min_z, max_z, tmin, tmax) || tmax < 0.f) {
        return false;
    }
    const float enter = tmin < 0.f ? 0.f : tmin;
    // The sweep is this frame's velocity, including the far end, before gravity is added.
    return enter <= speed;
}

void cannon_shell_advance(CannonShell& shell, float gravity) {
    if (!shell.alive) {
        return;
    }
    shell.vy += gravity;
    shell.x += shell.vx;
    shell.y += shell.vy;
    shell.z += shell.vz;
    // A ttl of N is collision-checked N times, then the move that reaches 0 ends the shell.
    if (--shell.ttl == 0) {
        shell.alive = false;
    }
}

}  // namespace bar
