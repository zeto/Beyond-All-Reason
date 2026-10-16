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
        // max(maxX - minX, maxZ - minZ) of the spot in elmos (resource_spot_finder extents).
        float extent = 0;
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
    // mapinfo lighting, Recoil CMapInfo::ReadLight defaults when a key is absent.
    float sun[3] = {0.f, 1.f, 2.f};
    float ambient[3] = {0.5f, 0.5f, 0.5f};
    float diffuse[3] = {0.5f, 0.5f, 0.5f};
    float specular[3] = {0.1f, 0.1f, 0.1f};
    float specular_exponent = 100.f;
    float unit_ambient[3] = {0.4f, 0.4f, 0.4f};
    float unit_diffuse[3] = {0.7f, 0.7f, 0.7f};
    float unit_specular[3] = {0.7f, 0.7f, 0.7f};
    float sky[3] = {0.43f, 0.58f, 0.64f};
    // Game tree root (BAR gamedata/resources.lua and bitmaps/), set by the shell.
    std::string game_root;
    // atmosphere.skyBox (Recoil loads "maps/" + name). Empty when the map has none.
    std::string sky_box;
    // mapinfo lighting groundShadowDensity / unitShadowDensity (Recoil default 0.8, clamped 0..1).
    float ground_shadow_density = 0.8f;
    float unit_shadow_density = 0.8f;
    std::vector<MapStart> starts;

    // Archive the map came from; terrain textures are read from it lazily.
    std::string archive_path;
    // SMF tile index: one tile per 4x4 heightmap squares (32 px of 1024-per-bigsquare diffuse).
    std::vector<int32_t> tile_index;
    int tile_grid_x = 0;
    int tile_grid_y = 0;
    // SMT files in tile order and how many tiles each holds (mapinfo smtFileName%i overrides names).
    std::vector<std::string> tile_files;
    std::vector<int> tile_file_counts;
    // Recoil SMFShadingTextureFragProg normal per heightmap vertex, (map_x+1)*(map_y+1) of
    // half-float (x, z) pairs, the RG16F normalsTex.
    std::vector<uint16_t> normals_xz;

    // mapinfo resources (already resolved with Recoil's "maps/" fallback).
    std::string specular_tex;
    std::string splat_distr_tex;
    std::string blend_normals_tex;
    std::vector<std::string> splat_normal_tex;
    bool splat_diffuse_alpha = false;
    // mapinfo splats, Recoil defaults 0.02 and 1.
    float splat_scales[4] = {0.02f, 0.02f, 0.02f, 0.02f};
    float splat_mults[4] = {1.f, 1.f, 1.f, 1.f};

    // mapinfo water, Recoil CMapInfo::ReadWater defaults.
    struct Water {
        float repeat_x = 0.f;
        float repeat_y = 0.f;
        float absorb[3] = {0.f, 0.f, 0.f};
        float base_color[3] = {0.f, 0.f, 0.f};
        float min_color[3] = {0.f, 0.f, 0.f};
        float plane_color[3] = {0.f, 0.4f, 0.f};
        bool has_water_plane = false;
        float surface_color[3] = {0.75f, 0.8f, 0.85f};
        float surface_alpha = 0.55f;
        std::string texture;
        // Advanced-water look parameters (mapinfo water table). Defaults for keys the map
        // omits are our own choices, picked to give a neutral look.
        float ambient_factor = 1.0f;
        float diffuse_factor = 1.0f;
        float specular_factor = 1.0f;
        float specular_power = 20.0f;
        float diffuse_color[3] = {1.f, 1.f, 1.f};
        float specular_color[3] = {0.5f, 0.5f, 0.5f};
        float fresnel_min = 0.2f;
        float fresnel_max = 0.8f;
        float fresnel_power = 4.0f;
        float reflection_distortion = 1.0f;
        float perlin_start_freq = 8.0f;
        float perlin_lacunarity = 3.0f;
        float perlin_amplitude = 0.9f;
        float wind_speed = 1.0f;
        bool shore_waves = true;
        float caustics_resolution = 75.0f;
        float caustics_strength = 0.08f;
        float wave_offset_factor = 0.0f;
        float wave_length = 0.15f;
        float wave_foam_distortion = 0.05f;
        float wave_foam_intensity = 0.5f;
    } water;
    // mapinfo custom.grassconfig (luaui/Widgets/map_grass_gl4.lua merges it over its defaults).
    // has_* marks keys the map sets; the widget defaults apply otherwise.
    struct GrassConfig {
        bool present = false;
        std::string grass_dist_tga;
        std::string grass_blade_color_tex;
        bool has_min_size = false, has_max_size = false, has_patch_resolution = false;
        float grass_min_size = 0, grass_max_size = 0, patch_resolution = 0;
        bool has_jitter = false;
        float patch_placement_jitter = 0;
        bool has_blade_scale = false;
        float grass_blade_scale = 0;
        // grassShaderParams keys the map overrides, lowercase name -> value.
        std::vector<std::pair<std::string, float>> shader_params;
    } grass;
};

float terrain_height_at(const TerrainMesh& mesh, float x, float z);

bool terrain_raycast(const TerrainMesh& mesh, const float origin[3], const float direction[3], float& hit_x,
                     float& hit_z);

// Reads mapinfo.lua and the SMF out of a map archive. Height uses mapinfo when it sets both ends.
bool load_terrain(const std::string& archive_path, TerrainMesh& mesh, std::string& error);

}  // namespace bar
