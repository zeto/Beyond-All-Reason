#pragma once

#include "smf.h"

#include <string>
#include <vector>

namespace bar {

struct Waypoint {
    float x = 0;
    float z = 0;
};

// Straight line when the slope and water allow it, otherwise an 8-connected grid search.
// min_water_depth is the MoveDef value. A cell is legal when its height is at most -min and at least -max.
// A hover rides the surface: water counts as height 0, and the sea floor's slope is ignored.
bool build_ground_path(const TerrainMesh& terrain, float max_slope_deg, float max_water_depth, float min_water_depth,
                       float x0, float z0, float x1, float z1, std::vector<Waypoint>& path, std::string& error,
                       bool hover = false);

}  // namespace bar