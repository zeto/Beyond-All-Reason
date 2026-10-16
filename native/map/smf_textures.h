#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bar {

struct TerrainMesh;

// CPU-side texture ready for upload. levels[0] is the base mip.
struct GpuImage {
    enum class Format { None, RGBA8, BC1, BC2, BC3, RG16F, R32F };
    Format format = Format::None;
    int width = 0;
    int height = 0;
    // Cubemaps keep faces +X, -X, +Y, -Y, +Z, -Z: levels[face * mip_count + level].
    std::vector<std::vector<uint8_t>> levels;
    int faces = 1;
    int mip_count() const { return faces > 0 ? static_cast<int>(levels.size()) / faces : 0; }
    bool valid() const { return format != Format::None && !levels.empty(); }
};

// The SMF ground inputs Recoil's SMFFragProg samples, read from the map archive.
// DDS files keep their block data as stored; Recoil loads them through nv_dds with a
// vertical flip, so the shader samples those at (u, 1 - v). TGA, PNG and SMT tiles
// are stored with row 0 at v = 0.
struct TerrainTextures {
    GpuImage diffuse;        // SMT tiles, BC1, 4 levels (Recoil persistent square textures)
    GpuImage normals;        // RG16F heightmap normals (x, z), (map_x+1) x (map_y+1)
    GpuImage specular;       // resources.specularTex
    GpuImage splat_distr;    // resources.splatDistrTex
    GpuImage blend_normals;  // resources.detailNormalTex
    GpuImage splat_normals[4];
    int splat_normal_count = 0;
    // atmosphere.skyBox cubemap, faces as stored (nv_dds's per-face flip, the +Y/-Y swap and
    // the SkyBox shader's uvFlip (1, -1, 1) cancel, so stored faces sample at the true direction).
    GpuImage sky_box;
    // CBasicWater texture: water.texture from mapinfo, else resources.lua graphics.maps.watertex
    // under bitmaps/ (Recoil default "ocean.jpg"). RGBA8 with a box-filtered mip chain.
    GpuImage water;
    std::string water_path;
    // Advanced water: bitmaps/atmos/waterfoam.tga from the game install (loaded in place),
    // and procedurally generated wave normals and caustics.
    GpuImage water_foam;
    GpuImage water_normals;
    GpuImage water_caustics;
};

bool load_terrain_textures(const TerrainMesh& mesh, TerrainTextures& out, std::string& error);

// Decoders shared with other map art (water). Rows are stored top-first.
bool decode_tga(const std::vector<uint8_t>& bytes, GpuImage& out, std::string& error);
bool decode_dds_blocks(const std::vector<uint8_t>& bytes, GpuImage& out, std::string& error);
// Box-filter mip chain for an RGBA8 image (glGenerateMipmap equivalent).
void build_rgba_mips(GpuImage& image);
// ImageIO decode (JPEG/PNG) to RGBA8, rows top-first.
bool decode_image_rgba(const std::vector<uint8_t>& bytes, GpuImage& out);
// FIND_MAP_TEXTURE in the map archive, and a plain file read.
bool map_file_bytes(const std::string& archive, const std::string& name, std::vector<uint8_t>& out);
bool disk_file_bytes(const std::string& path, std::vector<uint8_t>& out);

}  // namespace bar
