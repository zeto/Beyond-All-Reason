// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bar {

struct UnitSim;
struct TerrainMesh;

// Ground LOS and radar coverage of the local team, one cell per sensor square.
// modrules sensors.los: losMipLevel sets the sight square, radarMipLevel the radar square
// (square_size << mip elmos). A cell is covered when its centre lies inside the unit's
// sightdistance / radardistance and the straight line from the emitter (ground + emit
// height) to the cell's ground point clears the terrain sampled at every square along it.
struct LosMap {
    int width = 0;
    int height = 0;
    float square = 0;       // elmos per LOS cell
    int radar_width = 0;
    int radar_height = 0;
    float radar_square = 0;  // elmos per radar cell
    std::vector<uint8_t> los;    // 1 = in LOS
    std::vector<uint8_t> radar;  // 1 = in radar
    uint32_t version = 0;        // bumps whenever coverage changes
};

// Loads the sensor squares. False when modrules have no usable mip levels.
bool los_map_init(const std::string& game_root, const TerrainMesh& terrain, LosMap& map, std::string& error);

// Recomputes coverage from every unit of team `team`. Bumps version when anything changed.
bool los_map_update(const std::string& game_root, const UnitSim& sim, const TerrainMesh& terrain, int team,
                    LosMap& map, std::string& error);

// Nearest-cell lookups in world coordinates.
bool los_map_in_los(const LosMap& map, float x, float z);
bool los_map_in_radar(const LosMap& map, float x, float z);

}  // namespace bar
