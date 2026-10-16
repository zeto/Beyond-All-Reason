#include "grass.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>

namespace bar {
namespace {

std::string lower_ext(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool load_texture(const TerrainMesh& mesh, const std::string& path, GpuImage& out) {
    std::vector<uint8_t> bytes;
    // VFS: the map archive first (map art such as maps/grass-dandelion-4.dds), then the game.
    if (!map_file_bytes(mesh.archive_path, path, bytes) &&
        !disk_file_bytes(mesh.game_root + "/" + path, bytes)) {
        // VFS is case-insensitive; the game tree stores LuaUI as luaui.
        std::string lower = path;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!disk_file_bytes(mesh.game_root + "/" + lower, bytes)) {
            return false;
        }
    }
    std::string ignored;
    const std::string ext = lower_ext(path);
    if (ext == "dds") {
        return decode_dds_blocks(bytes, out, ignored);
    }
    if (ext == "tga") {
        if (!decode_tga(bytes, out, ignored)) return false;
        build_rgba_mips(out);
        return true;
    }
    if (!decode_image_rgba(bytes, out)) return false;
    build_rgba_mips(out);
    return true;
}

bool load_patch(const std::string& game_root, int patch_size, std::vector<float>& soup, std::string& error) {
    lua_State* state = luaL_newstate();
    luaL_openlibs(state);
    const std::string path = game_root + "/luaui/Include/grassPatches.lua";
    if (luaL_dofile(state, path.c_str()) != 0 || !lua_istable(state, -1)) {
        error = "grass: cannot load " + path;
        lua_close(state);
        return false;
    }
    lua_rawgeti(state, -1, patch_size);
    if (!lua_istable(state, -1)) {
        error = "grass: grassPatches has no patch of size " + std::to_string(patch_size);
        lua_close(state);
        return false;
    }
    const int n = static_cast<int>(lua_objlen(state, -1));
    soup.resize(static_cast<size_t>(n));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(state, -1, i);
        soup[static_cast<size_t>(i - 1)] = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
    }
    lua_close(state);
    return true;
}

}  // namespace

bool load_grass(const TerrainMesh& mesh, GrassData& g, std::string& error) {
    const TerrainMesh::GrassConfig& cfg = mesh.grass;
    std::string blade = "LuaUI/Images/luagrass/grass_field_medit_flowering.dds.cached.dds";
    std::string dist_tga;
    if (cfg.present) {
        if (!cfg.grass_blade_color_tex.empty()) blade = cfg.grass_blade_color_tex;
        dist_tga = cfg.grass_dist_tga;
        if (cfg.has_min_size) g.grass_min_size = cfg.grass_min_size;
        if (cfg.has_max_size) g.grass_max_size = cfg.grass_max_size;
        if (cfg.has_patch_resolution) g.patch_resolution = cfg.patch_resolution;
        if (cfg.has_jitter) g.patch_placement_jitter = cfg.patch_placement_jitter;
        if (cfg.has_blade_scale) g.grass_blade_scale = cfg.grass_blade_scale;
        const std::map<std::string, float*> keys = {
            {"mapcolorfactor", &g.map_color_factor}, {"mapcolorbase", &g.map_color_base},
            {"alphathreshold", &g.alpha_threshold}, {"windstrength", &g.wind_strength},
            {"windscale", &g.wind_scale}, {"windsamplescale", &g.wind_sample_scale},
            {"fadestart", &g.fade_start}, {"fadeend", &g.fade_end}, {"shadowfactor", &g.shadow_factor},
            {"grassbrightness", &g.grass_brightness}, {"unitbendstrength", &g.unit_bend_strength},
            {"unitbendfalloff", &g.unit_bend_falloff}, {"unitbendshrink", &g.unit_bend_shrink}};
        for (const auto& [k, v] : cfg.shader_params) {
            const auto it = keys.find(k);
            if (it != keys.end()) *it->second = v;
        }
    }
    // Only a grassDistTGA distribution is ported; the SMF grass-map fallback is not.
    if (dist_tga.empty()) {
        error = "grass: map has no grassDistTGA (SMF grass map path not ported)";
        return false;
    }

    // --- patch mesh (makeGrassPatchVBO) ---
    std::vector<float> soup;
    if (!load_patch(mesh.game_root, g.patch_size, soup, error)) return false;
    constexpr int kSrc = 17;
    int verts = g.patch_size == 1 ? 36 : 144;
    if (static_cast<int>(soup.size()) < verts * kSrc) {
        error = "grass: patch data too short";
        return false;
    }
    // The widget uses math.random for the copy jitter; a fixed seed keeps runs repeatable.
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    const float scale = g.grass_blade_scale;
    if (scale != 1.f) {
        float min_y = 1e30f;
        for (int v = 0; v < verts; ++v) min_y = std::min(min_y, soup[static_cast<size_t>(v * kSrc + 1)]);
        const int copies = std::max(1, std::min(16, static_cast<int>(std::floor(1.f / (scale * scale) + 0.5f))));
        const int grid_n = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(copies))));
        const float spacing = g.patch_resolution / grid_n;
        struct Cell { int cx, cz, key; };
        std::vector<Cell> cells;
        for (int cz = 0; cz < grid_n; ++cz)
            for (int cx = 0; cx < grid_n; ++cx)
                cells.push_back({cx, cz, ((cx + cz) % grid_n) * grid_n * grid_n + cz * grid_n + cx});
        std::sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) { return a.key < b.key; });
        std::vector<float> scaled;
        for (int c = 0; c < copies; ++c) {
            float ox = 0, oz = 0, cosr = 1, sinr = 0;
            if (copies > 1) {
                const Cell& cell = cells[static_cast<size_t>(c)];
                ox = (cell.cx - (grid_n - 1) * 0.5f) * spacing + (uni(rng) - 0.5f) * spacing * 0.5f;
                oz = (cell.cz - (grid_n - 1) * 0.5f) * spacing + (uni(rng) - 0.5f) * spacing * 0.5f;
                const float rot = uni(rng) * 6.283f;
                cosr = std::cos(rot);
                sinr = std::sin(rot);
            }
            for (int v = 0; v < verts; ++v) {
                const float* src = &soup[static_cast<size_t>(v * kSrc)];
                const float px = src[0] * scale;
                const float pz = src[2] * scale;
                scaled.push_back(cosr * px + sinr * pz + ox);
                scaled.push_back(min_y + (src[1] - min_y) * scale);
                scaled.push_back(cosr * pz - sinr * px + oz);
                for (int f = 3; f < kSrc; ++f) scaled.push_back(src[f]);
            }
        }
        soup.swap(scaled);
        verts *= copies;
    }
    // COMPACTVBO layout + bit-identical vertex merge.
    std::map<std::vector<float>, uint32_t> unique;
    for (int v = 0; v < verts; ++v) {
        const float* s = &soup[static_cast<size_t>(v * kSrc)];
        std::vector<float> key = {s[0], s[1], s[2], s[12], s[3], s[4], s[5], s[13]};
        auto it = unique.find(key);
        uint32_t idx;
        if (it == unique.end()) {
            idx = static_cast<uint32_t>(unique.size());
            unique.emplace(key, idx);
            g.patch_vertices.insert(g.patch_vertices.end(), key.begin(), key.end());
        } else {
            idx = it->second;
        }
        g.patch_indices.push_back(idx);
    }

    // --- distribution (LoadGrassTGA + buildGrassTiles) ---
    std::vector<uint8_t> tga;
    if (!map_file_bytes(mesh.archive_path, dist_tga, tga) || tga.size() < 18) {
        error = "grass: cannot read " + dist_tga;
        return false;
    }
    const int id_len = tga[0];
    const int type = tga[2];
    const int w = tga[12] | (tga[13] << 8);
    const int h = tga[14] | (tga[15] << 8);
    const int bpp = tga[16];
    if (type != 3 || bpp != 8 || tga.size() < static_cast<size_t>(18 + id_len + w * h)) {
        error = "grass: grassDistTGA must be an 8-bit uncompressed greyscale TGA";
        return false;
    }
    // image_tga.lua returns rows in file order (bottom-left origin); the widget indexes
    // texture[gz + 1][gx + 1], so file row gz is grass row gz.
    const uint8_t* px = tga.data() + 18 + id_len;
    auto patch_mult = [&](uint8_t b) {
        return b == 0 ? 0.f : g.grass_min_size + (g.grass_max_size - g.grass_min_size) * (b / 254.f);
    };
    constexpr float kGridSize = 512.f;
    const int per_tile = std::max(1, static_cast<int>(std::floor(kGridSize / g.patch_resolution)));
    g.tile_world = per_tile * g.patch_resolution;
    g.grid_cols = (w + per_tile - 1) / per_tile;
    g.grid_rows = (h + per_tile - 1) / per_tile;
    const float jitter = g.patch_placement_jitter;
    for (int tz = 0; tz < g.grid_rows; ++tz) {
        const int gz0 = tz * per_tile;
        const int tile_h = std::min(per_tile, h - gz0);
        for (int tx = 0; tx < g.grid_cols; ++tx) {
            const int gx0 = tx * per_tile;
            const int tile_w = std::min(per_tile, w - gx0);
            const uint32_t offset = static_cast<uint32_t>(g.instances.size() / 4);
            g.tile_offset.push_back(offset);
            for (int lz = 0; lz < tile_h; ++lz) {
                const int gz = gz0 + lz;
                const float czw = (gz + 0.5f) * g.patch_resolution;
                for (int lx = 0; lx < tile_w; ++lx) {
                    const int gx = gx0 + lx;
                    const float cxw = (gx + 0.5f) * g.patch_resolution;
                    g.instances.push_back(cxw + (uni(rng) - 0.5f) * g.patch_resolution * jitter);
                    g.instances.push_back(uni(rng) * 6.28f);
                    g.instances.push_back(czw + (uni(rng) - 0.5f) * g.patch_resolution * jitter);
                    g.instances.push_back(patch_mult(px[static_cast<size_t>(gz) * w + gx]));
                }
            }
            g.tile_count.push_back(static_cast<uint32_t>(g.instances.size() / 4) - offset);
        }
    }

    // clearMetalspotGrass: adjustGrass(spot, max(extentZ, extentX) * 1.2, 1) for each spot.
    auto instance_at = [&](float wx, float wz) -> long {
        const int gx = static_cast<int>(std::floor(wx / g.patch_resolution));
        const int gz = static_cast<int>(std::floor(wz / g.patch_resolution));
        if (gx < 0 || gx >= w || gz < 0 || gz >= h) return -1;
        const int tx = gx / per_tile, tz = gz / per_tile;
        const int tile = tz * g.grid_cols + tx;
        const int tile_w = std::min(per_tile, w - tx * per_tile);
        return static_cast<long>(g.tile_offset[static_cast<size_t>(tile)]) + (gz - tz * per_tile) * tile_w +
               (gx - tx * per_tile);
    };
    for (const TerrainMesh::MetalSpot& spot : mesh.metal_spots) {
        const float radius = spot.extent * 1.2f;
        if (radius <= 0.f) continue;
        const float sx = std::floor(spot.x), sz = std::floor(spot.z);
        for (float x = sx - radius; x <= sx + radius; x += g.patch_resolution) {
            for (float z = sz - radius; z <= sz + radius; z += g.patch_resolution) {
                if ((x - sx) * (x - sx) + (z - sz) * (z - sz) >= radius * radius) continue;
                // WG.grassgl4 adjustGrass sizeMod, then updateGrassInstanceVBO(x, z, 1, 1 - sizeMod).
                float size_mod = 1.f - (std::fabs((x - sx) / radius) + std::fabs((z - sz) / radius)) / 2.f;
                size_mod = size_mod * 2.f - std::min(0.66f, radius / 100.f);
                const long e = instance_at(x, z);
                if (e < 0) continue;
                float& size = g.instances[static_cast<size_t>(e) * 4 + 3];
                if (size <= 0.f) continue;
                const float mod = 1.f - size_mod;
                float s = std::min(g.grass_max_size, size * mod);
                if (mod < 1.f) {
                    if (s < g.grass_min_size) s = 0.f;
                } else {
                    s = std::max(g.grass_min_size, s);
                }
                size = s;
            }
        }
    }

    if (!load_texture(mesh, blade, g.blade_tex)) {
        error = "grass: cannot load blade texture " + blade;
        return false;
    }
    g.blade_path = blade;
    if (!load_texture(mesh, "bitmaps/GPL/perlin_noise.jpg", g.perlin_tex)) {
        error = "grass: cannot load bitmaps/GPL/perlin_noise.jpg";
        return false;
    }
    g.heightmap.format = GpuImage::Format::R32F;
    g.heightmap.width = mesh.map_x + 1;
    g.heightmap.height = mesh.map_y + 1;
    std::vector<uint8_t> hm(static_cast<size_t>(g.heightmap.width) * g.heightmap.height * 4);
    for (int z = 0; z <= mesh.map_y; ++z) {
        for (int x = 0; x <= mesh.map_x; ++x) {
            const float y = terrain_height_at(mesh, static_cast<float>(x * mesh.square_size),
                                              static_cast<float>(z * mesh.square_size));
            std::memcpy(&hm[(static_cast<size_t>(z) * g.heightmap.width + x) * 4], &y, 4);
        }
    }
    g.heightmap.levels.assign(1, std::move(hm));
    g.valid = true;
    std::fprintf(stderr, "grass: %zu patches, %zu patch verts / %zu indices, blade %s\n", g.instances.size() / 4,
                 g.patch_vertices.size() / 8, g.patch_indices.size(), blade.c_str());
    return true;
}

}  // namespace bar
