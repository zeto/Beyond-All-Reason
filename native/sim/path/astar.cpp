#include "path/astar.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>

namespace bar {
namespace {

constexpr float kCell = 32.f;

float deg(float radians) {
    return radians * 180.f / 3.14159265f;
}

bool inside(const TerrainMesh& terrain, float x, float z) {
    return x >= 0.f && z >= 0.f && x <= terrain.world_x && z <= terrain.world_z;
}

bool depth_ok(float height, float max_water, float min_water) {
    // Too deep, or shallower than the move class allows. Land is height > -min for a ship.
    return height >= -max_water && height <= -min_water;
}

float travel_height(float ground, bool hover) {
    // The water surface is flat. A hover does not climb the sea floor.
    if (hover && ground < 0.f) {
        return 0.f;
    }
    return ground;
}

bool grade_ok(float h0, float h1, float horizontal, float max_slope_deg, float max_water, float min_water,
              bool hover) {
    if (!hover && !depth_ok(h1, max_water, min_water)) {
        return false;
    }
    if (horizontal < 1e-3f) {
        return true;
    }
    const float left = travel_height(h0, hover);
    const float right = travel_height(h1, hover);
    return deg(std::atan2(std::fabs(right - left), horizontal)) <= max_slope_deg;
}

bool segment_clear(const TerrainMesh& terrain, float max_slope_deg, float max_water, float min_water, bool hover,
                   float x0, float z0, float x1, float z1) {
    const float dx = x1 - x0;
    const float dz = z1 - z0;
    const float length = std::sqrt(dx * dx + dz * dz);
    const int steps = std::max(1, static_cast<int>(length / 8.f));
    float previous = terrain_height_at(terrain, x0, z0);
    if (!grade_ok(previous, previous, 0.f, max_slope_deg, max_water, min_water, hover)) {
        return false;
    }
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float x = x0 + dx * t;
        const float z = z0 + dz * t;
        if (!inside(terrain, x, z)) {
            return false;
        }
        const float height = terrain_height_at(terrain, x, z);
        if (!grade_ok(previous, height, length / static_cast<float>(steps), max_slope_deg, max_water, min_water,
                      hover)) {
            return false;
        }
        previous = height;
    }
    return true;
}

struct HeapItem {
    float score = 0;
    int cell = 0;
};

struct HeapGreater {
    bool operator()(const HeapItem& a, const HeapItem& b) const { return a.score > b.score; }
};

}  // namespace

bool build_ground_path(const TerrainMesh& terrain, float max_slope_deg, float max_water_depth, float min_water_depth,
                       float x0, float z0, float x1, float z1, std::vector<Waypoint>& path, std::string& error,
                       bool hover) {
    path.clear();
    if (!inside(terrain, x0, z0) || !inside(terrain, x1, z1)) {
        error = "move order is outside the map";
        return false;
    }
    if (segment_clear(terrain, max_slope_deg, max_water_depth, min_water_depth, hover, x0, z0, x1, z1)) {
        path.push_back(Waypoint{x1, z1});
        return true;
    }

    const int n = std::max(1, static_cast<int>(std::floor(std::min(terrain.world_x, terrain.world_z) / kCell)));
    auto cell_of = [&](float x, float z) {
        const int ix = std::clamp(static_cast<int>(x / kCell), 0, n - 1);
        const int iz = std::clamp(static_cast<int>(z / kCell), 0, n - 1);
        return iz * n + ix;
    };
    auto center = [&](int cell, float& x, float& z) {
        const int ix = cell % n;
        const int iz = cell / n;
        x = (static_cast<float>(ix) + 0.5f) * kCell;
        z = (static_cast<float>(iz) + 0.5f) * kCell;
    };
    const int start = cell_of(x0, z0);
    const int goal = cell_of(x1, z1);
    const int count = n * n;
    std::vector<float> best(static_cast<size_t>(count), 1e30f);
    std::vector<int> parent(static_cast<size_t>(count), -1);
    std::vector<char> closed(static_cast<size_t>(count), 0);
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapGreater> open;
    best[static_cast<size_t>(start)] = 0;
    open.push(HeapItem{0, start});

    const int neighbor[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    bool found = false;
    while (!open.empty()) {
        const HeapItem item = open.top();
        open.pop();
        if (closed[static_cast<size_t>(item.cell)]) {
            continue;
        }
        closed[static_cast<size_t>(item.cell)] = 1;
        if (item.cell == goal) {
            found = true;
            break;
        }
        const int ix = item.cell % n;
        const int iz = item.cell / n;
        float cx = 0;
        float cz = 0;
        center(item.cell, cx, cz);
        const float h0 = terrain_height_at(terrain, cx, cz);
        for (const auto& offset : neighbor) {
            const int nx = ix + offset[0];
            const int nz = iz + offset[1];
            if (nx < 0 || nz < 0 || nx >= n || nz >= n) {
                continue;
            }
            const int next = nz * n + nx;
            if (closed[static_cast<size_t>(next)]) {
                continue;
            }
            float px = 0;
            float pz = 0;
            center(next, px, pz);
            const float horizontal = (offset[0] != 0 && offset[1] != 0) ? kCell * 1.41421356f : kCell;
            const float h1 = terrain_height_at(terrain, px, pz);
            if (!grade_ok(h0, h1, horizontal, max_slope_deg, max_water_depth, min_water_depth, hover)) {
                continue;
            }
            if (offset[0] != 0 && offset[1] != 0) {
                const int side_a = iz * n + nx;
                const int side_b = nz * n + ix;
                float ax = 0;
                float az = 0;
                float bx = 0;
                float bz = 0;
                center(side_a, ax, az);
                center(side_b, bx, bz);
                if (!grade_ok(h0, terrain_height_at(terrain, ax, az), kCell, max_slope_deg, max_water_depth,
                              min_water_depth, hover) ||
                    !grade_ok(h0, terrain_height_at(terrain, bx, bz), kCell, max_slope_deg, max_water_depth,
                              min_water_depth, hover)) {
                    continue;
                }
            }
            const float cost = best[static_cast<size_t>(item.cell)] + horizontal;
            if (cost >= best[static_cast<size_t>(next)]) {
                continue;
            }
            best[static_cast<size_t>(next)] = cost;
            parent[static_cast<size_t>(next)] = item.cell;
            const float gx = (static_cast<float>(goal % n) + 0.5f) * kCell;
            const float gz = (static_cast<float>(goal / n) + 0.5f) * kCell;
            const float hx = px - gx;
            const float hz = pz - gz;
            open.push(HeapItem{cost + std::sqrt(hx * hx + hz * hz), next});
        }
    }
    if (!found) {
        error = "no ground path";
        return false;
    }

    std::vector<int> cells;
    for (int cell = goal; cell != -1; cell = parent[static_cast<size_t>(cell)]) {
        cells.push_back(cell);
        if (cell == start) {
            break;
        }
    }
    std::reverse(cells.begin(), cells.end());
    for (size_t i = 1; i < cells.size(); ++i) {
        float x = 0;
        float z = 0;
        center(cells[i], x, z);
        path.push_back(Waypoint{x, z});
    }
    if (path.empty()) {
        path.push_back(Waypoint{x1, z1});
    } else {
        path.back() = Waypoint{x1, z1};
    }
    return true;
}

}  // namespace bar