#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bar {

struct TerrainVertex {
    float px, py, pz, pad0;
    float nx, ny, nz, pad1;
    float u, v, mu, mv;
};

struct MapStart {
    int team = 0;
    float x = 0;
    float z = 0;
};

struct TerrainMesh {
    std::string map_file;
    int map_x = 0;
    int map_y = 0;
    int square_size = 8;
    int mesh_stride = 2;
    float min_height = 0;
    float max_height = 0;
    bool height_from_mapinfo = false;
    float world_x = 0;
    float world_z = 0;
    float center_height = 0;
    float corner_height = 0;
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<TerrainVertex> water_vertices;
    std::vector<uint32_t> water_indices;
    std::vector<uint8_t> minimap_rgba;
    int minimap_size = 0;
    std::vector<uint8_t> metal;
    int metal_size = 0;
    // Mex spots the map finder keeps. Empty on a metal map.
    struct MetalSpot {
        float x = 0;
        float z = 0;
    };
    std::vector<MetalSpot> metal_spots;
    // Cyan mex rings. Empty on a metal map, where the HUD draws none.
    std::vector<TerrainVertex> ring_vertices;
    std::vector<uint32_t> ring_indices;
    int metal_spot_count = 0;
    bool metal_map = false;
    float extractor_radius = 100.f;
    // A metal-map byte times this is the metal on that square. Mapinfo default is 0.02.
    float max_metal = 0.02f;
    // Recoil's defaults when atmosphere omits the range. min is never above max.
    float min_wind = 5.f;
    float max_wind = 25.f;
    // Map gravity in elmos per frame squared. Negative is down. The tag default is 130.
    float gravity = -130.f / 900.f;
    float sun[3] = {-0.64f, 0.66f, -0.57f};
    float ambient[3] = {0.35f, 0.35f, 0.35f};
    float diffuse[3] = {1.2f, 1.2f, 1.2f};
    float sky[3] = {0.43f, 0.58f, 0.64f};
    std::vector<MapStart> starts;
};

float terrain_height_at(const TerrainMesh& mesh, float x, float z);

bool terrain_raycast(const TerrainMesh& mesh, const float origin[3], const float direction[3], float& hit_x,
                     float& hit_z);

// Reads mapinfo.lua and the SMF out of a map archive. Height uses mapinfo when it sets both ends.
bool load_terrain(const std::string& archive_path, TerrainMesh& mesh, std::string& error);

}  // namespace bar
