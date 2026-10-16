// SPDX-License-Identifier: GPL-2.0-or-later
#include "los/los_map.h"

#include "move/unit_sim.h"
#include "smf.h"
#include "unitdef.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <map>

namespace bar {
namespace {

bool read_mips(const std::string& game_root, int& los_mip, int& radar_mip) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        return false;
    }
    luaL_openlibs(state);
    const char* stub =
        "VFS = { Include = function() return nil end, DirList = function() return {} end }\n"
        "Script = { IsEngineMinVersion = function() return true end }\n"
        "Spring = { GetModOptions = function() return {} end, Echo = function() end }\n";
    const std::string path = game_root + "/gamedata/modrules.lua";
    bool ok = luaL_dostring(state, stub) == 0 && luaL_loadfile(state, path.c_str()) == 0 &&
              lua_pcall(state, 0, 1, 0) == 0 && lua_istable(state, -1);
    if (ok) {
        lua_getfield(state, -1, "sensors");
        ok = lua_istable(state, -1);
    }
    if (ok) {
        lua_getfield(state, -1, "los");
        ok = lua_istable(state, -1);
    }
    if (ok) {
        lua_getfield(state, -1, "losMipLevel");
        lua_getfield(state, -2, "radarMipLevel");
        ok = lua_isnumber(state, -2) && lua_isnumber(state, -1);
        if (ok) {
            los_mip = static_cast<int>(lua_tonumber(state, -2));
            radar_mip = static_cast<int>(lua_tonumber(state, -1));
        }
    }
    lua_close(state);
    return ok && los_mip >= 0 && los_mip <= 6 && radar_mip >= 0 && radar_mip <= 6;
}

// The emitter at (ex, ey, ez) sees ground point (tx, tz) when no square sample along the ray
// rises above the straight line between them.
bool ray_clear(const TerrainMesh& terrain, float ex, float ey, float ez, float tx, float tz, float step) {
    const float ty = terrain_height_at(terrain, tx, tz);
    const float dx = tx - ex;
    const float dz = tz - ez;
    const float dist = std::sqrt(dx * dx + dz * dz);
    const int steps = static_cast<int>(std::floor(dist / step));
    for (int i = 1; i < steps; ++i) {
        const float t = static_cast<float>(i) * step / dist;
        const float line = ey + (ty - ey) * t;
        if (terrain_height_at(terrain, ex + dx * t, ez + dz * t) > line) {
            return false;
        }
    }
    return true;
}

void cover(const TerrainMesh& terrain, std::vector<uint8_t>& cells, int width, int height, float square, float ux,
           float uz, float emit, float range) {
    if (!(range > 0.f)) {
        return;
    }
    const float ey = std::max(0.f, terrain_height_at(terrain, ux, uz)) + emit;
    const int x0 = std::max(0, static_cast<int>(std::floor((ux - range) / square)));
    const int x1 = std::min(width - 1, static_cast<int>(std::floor((ux + range) / square)));
    const int z0 = std::max(0, static_cast<int>(std::floor((uz - range) / square)));
    const int z1 = std::min(height - 1, static_cast<int>(std::floor((uz + range) / square)));
    for (int cz = z0; cz <= z1; ++cz) {
        for (int cx = x0; cx <= x1; ++cx) {
            const float tx = (static_cast<float>(cx) + 0.5f) * square;
            const float tz = (static_cast<float>(cz) + 0.5f) * square;
            const float dx = tx - ux;
            const float dz = tz - uz;
            if (dx * dx + dz * dz > range * range) {
                continue;
            }
            uint8_t& cell = cells[static_cast<size_t>(cz) * width + cx];
            if (cell == 0 && ray_clear(terrain, ux, ey, uz, tx, tz, square)) {
                cell = 1;
            }
        }
    }
}

bool lookup(const std::vector<uint8_t>& cells, int width, int height, float square, float x, float z) {
    if (cells.empty() || !(square > 0.f)) {
        return false;
    }
    const int cx = static_cast<int>(std::floor(x / square));
    const int cz = static_cast<int>(std::floor(z / square));
    if (cx < 0 || cz < 0 || cx >= width || cz >= height) {
        return false;
    }
    return cells[static_cast<size_t>(cz) * width + cx] != 0;
}

}  // namespace

bool los_map_init(const std::string& game_root, const TerrainMesh& terrain, LosMap& map, std::string& error) {
    int los_mip = 0;
    int radar_mip = 0;
    if (!read_mips(game_root, los_mip, radar_mip) || terrain.square_size <= 0) {
        error = "modrules sensors.los has no usable losMipLevel / radarMipLevel";
        return false;
    }
    const float world_x = terrain.world_x;
    const float world_z = terrain.world_z;
    map = LosMap{};
    map.square = static_cast<float>(terrain.square_size << los_mip);
    map.radar_square = static_cast<float>(terrain.square_size << radar_mip);
    map.width = std::max(1, static_cast<int>(std::ceil(world_x / map.square)));
    map.height = std::max(1, static_cast<int>(std::ceil(world_z / map.square)));
    map.radar_width = std::max(1, static_cast<int>(std::ceil(world_x / map.radar_square)));
    map.radar_height = std::max(1, static_cast<int>(std::ceil(world_z / map.radar_square)));
    map.los.assign(static_cast<size_t>(map.width) * map.height, 0);
    map.radar.assign(static_cast<size_t>(map.radar_width) * map.radar_height, 0);
    return true;
}

bool los_map_update(const std::string& game_root, const UnitSim& sim, const TerrainMesh& terrain, int team,
                    LosMap& map, std::string& error) {
    static std::map<std::string, UnitSensors> cache;
    std::vector<uint8_t> los(map.los.size(), 0);
    std::vector<uint8_t> radar(map.radar.size(), 0);
    for (size_t i = 0; i < sim.x.size(); ++i) {
        if (i < sim.team.size() && sim.team[i] != team) {
            continue;
        }
        const std::string& def = sim.def_name[i];
        auto found = cache.find(def);
        if (found == cache.end()) {
            UnitSensors sensors;
            if (!load_unit_sensors(game_root, def, sensors, error)) {
                return false;
            }
            found = cache.emplace(def, sensors).first;
        }
        const UnitSensors& s = found->second;
        cover(terrain, los, map.width, map.height, map.square, sim.x[i], sim.z[i], s.sight_emit, s.sight);
        cover(terrain, radar, map.radar_width, map.radar_height, map.radar_square, sim.x[i], sim.z[i], s.radar_emit,
              s.radar);
    }
    if (los != map.los || radar != map.radar) {
        map.los.swap(los);
        map.radar.swap(radar);
        ++map.version;
    }
    return true;
}

bool los_map_in_los(const LosMap& map, float x, float z) {
    return lookup(map.los, map.width, map.height, map.square, x, z);
}

bool los_map_in_radar(const LosMap& map, float x, float z) {
    return lookup(map.radar, map.radar_width, map.radar_height, map.radar_square, x, z);
}

}  // namespace bar
