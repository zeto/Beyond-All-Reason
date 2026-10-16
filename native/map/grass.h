// Native port of BAR's luaui/Widgets/map_grass_gl4.lua (Map Grass GL4) data setup.
// The widget's shaders (MIT, Beherith) are ported to shaders/terrain.metal grass_vs/grass_fs.
#pragma once

#include "smf.h"
#include "smf_textures.h"

#include <string>
#include <vector>

namespace bar {

struct GrassData {
    bool valid = false;
    // grassConfig after the mapinfo custom.grassconfig merge.
    float patch_resolution = 32.f;
    float patch_placement_jitter = 0.66f;
    int patch_size = 4;
    float grass_blade_scale = 0.5f;
    float grass_min_size = 0.55f;
    float grass_max_size = 1.5f;
    float grass_wind_mult = 4.5f;
    float max_wind_speed = 20.f;
    // grassShaderParams.
    float map_color_factor = 0.6f;
    float map_color_base = 1.0f;
    float alpha_threshold = 0.01f;
    float wind_strength = 0.06f;
    float wind_scale = 0.33f;
    float wind_sample_scale = 0.0007f;
    float fade_start = 5000.f;
    float fade_end = 8000.f;
    float shadow_factor = 0.25f;
    float grass_brightness = 1.0f;
    float unit_bend_strength = 5.5f;
    float unit_bend_falloff = 0.8f;
    float unit_bend_shrink = 0.55f;
    // local distanceMult = 0.45 in the widget.
    float distance_mult = 0.45f;

    // Patch mesh (luaui/Include/grassPatches.lua, bladeScale baked in like makeGrassPatchVBO):
    // per vertex px, py, pz, u, nx, ny, nz, v (the widget's COMPACTVBO layout).
    std::vector<float> patch_vertices;
    std::vector<uint32_t> patch_indices;
    // Instances x, rotation, z, size in buildGrassTiles order (512-elmo tiles).
    std::vector<float> instances;
    int grid_cols = 0;  // tiles
    int grid_rows = 0;
    float tile_world = 512.f;
    std::vector<uint32_t> tile_offset;
    std::vector<uint32_t> tile_count;

    GpuImage blade_tex;    // grassBladeColorTex
    GpuImage perlin_tex;   // grassWindPerturbTex
    GpuImage heightmap;    // $heightmap, R32F at (map_x + 1) x (map_y + 1)
    std::string blade_path;
};

bool load_grass(const TerrainMesh& mesh, GrassData& out, std::string& error);

}  // namespace bar
