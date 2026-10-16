#include "terrain_renderer.h"
#include "grass.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gl/image_cache.h"
#include "smf_textures.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace bar {

// One command allocator can be reset only after every command buffer that used it has
// finished on the GPU. Three slots let the CPU encode the next frame while the GPU is
// still drawing the earlier ones, and keep a drawable from being reused on the next
// present. A single slot made nextDrawable and the completion fence wait on the display.
constexpr int kInFlight = 3;

struct FrameSlot {
    id<MTL4CommandAllocator> allocator = nil;
    id<MTL4CommandBuffer> command_buffer = nil;
    id<MTLBuffer> uniforms = nil;
    // Six TerrainUniforms, one per reflection cube face.
    id<MTLBuffer> face_uniforms = nil;
    id<MTLBuffer> pieces = nil;
    id<MTLTexture> hud_atlas = nil;
    id<MTLBuffer> hud_color = nil;
    id<MTLBuffer> hud_text_verts = nil;
    id<MTLBuffer> hud_minimap = nil;
    id<MTLBuffer> hud_marker = nil;
    NSUInteger hud_color_count = 0;
    NSUInteger hud_text_count = 0;
    NSUInteger hud_minimap_count = 0;
    NSUInteger hud_marker_count = 0;
    // Draw-order runs over hud_color: image 0 is solid color, else an image_cache id.
    struct HudSeg { NSUInteger start; NSUInteger count; int image; };
    std::vector<HudSeg> hud_segments;
    uint64_t ticket = 0;
    bool pending = false;
};

struct TerrainUniforms {
    float view_proj[16];
    float sun[4];
    float ambient[4];
    float diffuse[4];
    float sky[4];
    float team[4];
    float model_pose[4];
    float marker[4];
    float shadow_vp[16];
    float shadow[4];
    float eye[4];
    float ground_specular[4];
    float splat_scales[4];
    float splat_mults[4];
    float smf_flags[4];
    float map[4];
    float water_base[4];
    float water_min[4];
    float water_absorb[4];
    float unit_ambient[4];
    float unit_diffuse[4];
    float unit_specular[4];
    // CUS: x simFrame, y unitID, z 1 when normalTex is bound, w unused.
    float unit_frame[4];
    // Inverse of view_proj (sky_fs rebuilds the view ray per pixel).
    float sky_inv[16];
    // Water planeColor (xyz), w 1 when the map has a water plane (CubeMapFS.glsl).
    float plane_color[4];
    // CBasicWater: xy texture repeats over the map (repeatX/Y or the default), z 1 with a texture.
    float water_repeat[4];
    // Advanced water (water_fs). x fresnelMin, y fresnelMax, z fresnelPower, w reflectionDistortion.
    float water_fresnel[4];
    // x perlinStartFreq, y perlinLacunarity, z perlinAmplitude, w windSpeed.
    float water_waves[4];
    // x ambientFactor, y diffuseFactor, z specularFactor, w specularPower.
    float water_light[4];
    // specularColor (xyz), w surfaceAlpha.
    float water_specular[4];
    // surfaceColor (xyz), w time in seconds (sim frame / 30).
    float water_surface[4];
    // x shoreWaves, y waveLength, z waveFoamIntensity, w waveFoamDistortion.
    float water_shore[4];
    // x causticsResolution, y causticsStrength, z 1 with a foam texture, w 1 with a reflection cube.
    float water_extra[4];
    // 1 / target size (xy) for screen-space refraction lookups.
    float water_screen[4];
    // Map Grass GL4: x MAPCOLORFACTOR, y MAPCOLORBASE, z ALPHATHRESHOLD, w GRASSBRIGHTNESS.
    float grass_a[4];
    // x WINDSTRENGTH, y WINDSCALE, z WINDSAMPLESCALE, w SHADOWFACTOR.
    float grass_b[4];
    // x FADESTART * distanceMult, y FADEEND * distanceMult, zw unused.
    float grass_c[4];
    // grassuniforms: wind offset x, wind offset z, wind strength, global alpha.
    float grass_u[4];
};

// terrain_fs texture slots. Slot 2 is the shadow map.
enum GroundSlot { kGroundDiffuse = 0, kGroundNormals = 1, kGroundSpecular = 3, kGroundSplatDistr = 4,
                  kGroundBlendNormals = 5, kGroundSplatNormal0 = 6, kGroundSlots = 10 };
// Above the ground slots: atmosphere.skyBox cube and the $reflection cube (model_fs).
enum { kSkyCubeSlot = 10, kReflectionSlot = 11, kWaterSlot = 12, kRefractColorSlot = 13, kRefractDepthSlot = 14,
       kFoamSlot = 15, kWaveNormalSlot = 16, kCausticsSlot = 17, kGrassBladeSlot = 18, kGrassPerlinSlot = 19,
       kGrassHeightSlot = 20, kGrassMapColorSlot = 21, kTextureSlots = 22 };
// Recoil CubeTexSizeReflection default; the oracle config does not set it, and BAR's
// api_pbr_enabler only changes the config after the cubemap exists.
constexpr NSUInteger kReflectionSize = 128;

// Shadow map edge in texels. One map covers the whole visible ground (no cascades yet), so 4096.
constexpr NSUInteger kShadowSize = 4096;

struct TerrainRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTL4CommandQueue> queue = nil;
    FrameSlot slots[kInFlight];
    int slot_index = 0;
    FrameSlot* active = nullptr;
    id<MTLRenderPipelineState> terrain_pipeline = nil;
    id<MTLRenderPipelineState> model_pipeline = nil;
    id<MTLRenderPipelineState> water_pipeline = nil;
    id<MTLRenderPipelineState> ring_pipeline = nil;
    id<MTLRenderPipelineState> hud_color_pipeline = nil;
    id<MTLRenderPipelineState> hud_text_pipeline = nil;
    id<MTLRenderPipelineState> hud_image_pipeline = nil;
    id<MTLRenderPipelineState> shadow_terrain_pipeline = nil;
    id<MTLRenderPipelineState> shadow_model_pipeline = nil;
    id<MTLTexture> shadow_map = nil;
    float ground_shadow_density = 0.8f;
    float unit_shadow_density = 0.8f;
    float height_min = 0.f;
    float height_max = 0.f;
    std::unordered_map<int, id<MTLTexture>> hud_images;
    id<MTLDepthStencilState> terrain_depth = nil;
    id<MTLDepthStencilState> water_depth = nil;
    id<MTL4ArgumentTable> arguments = nil;
    id<MTLBuffer> vertices = nil;
    id<MTLBuffer> indices = nil;
    id<MTLBuffer> water_vertices = nil;
    id<MTLBuffer> water_indices = nil;
    id<MTLBuffer> ring_vertices = nil;
    id<MTLBuffer> ring_indices = nil;
    id<MTLTexture> minimap = nil;
    id<MTLTexture> metal_map = nil;
    id<MTLBuffer> model_vertices = nil;
    id<MTLBuffer> model_indices = nil;
    id<MTLTexture> model_color = nil;
    id<MTLTexture> model_other = nil;
    id<MTLTexture> model_normal = nil;
    id<MTLTexture> brdf_lut = nil;
    id<MTLTexture> sky_cube = nil;
    id<MTLTexture> reflection = nil;
    id<MTLTexture> reflection_depth = nil;
    id<MTLRenderPipelineState> sky_pipeline = nil;
    id<MTLDepthStencilState> sky_depth = nil;
    // CubeMapHandler renders one reflection face per frame; the first frame fills all six.
    int reflection_face = 0;
    bool reflection_primed = false;
    float plane_color[4] = {0, 0, 0, 0};
    id<MTLTexture> water_tex = nil;
    float water_repeat[4] = {0, 0, 0, 0};
    // Advanced water: scene colour/depth copies for refraction, foam, procedural waves/caustics.
    id<MTLTexture> refract_color = nil;
    id<MTLTexture> refract_depth = nil;
    id<MTLTexture> foam_tex = nil;
    id<MTLTexture> wave_tex = nil;
    id<MTLTexture> caustics_tex = nil;
    float water_fresnel[4] = {0, 0, 0, 0};
    float water_waves[4] = {0, 0, 0, 0};
    float water_light[4] = {0, 0, 0, 0};
    float water_specular[4] = {0, 0, 0, 0};
    float water_surface[4] = {0, 0, 0, 0};
    float water_shore[4] = {0, 0, 0, 0};
    float water_extra[4] = {0, 0, 0, 0};
    // Map Grass GL4 port.
    id<MTLRenderPipelineState> grass_pipeline = nil;
    id<MTLDepthStencilState> grass_depth = nil;
    id<MTLBuffer> grass_vertices = nil;
    id<MTLBuffer> grass_indices = nil;
    id<MTLBuffer> grass_instances = nil;
    id<MTLTexture> grass_blade = nil;
    id<MTLTexture> grass_perlin = nil;
    id<MTLTexture> grass_height = nil;
    NSUInteger grass_index_count = 0;
    std::vector<uint32_t> grass_tile_offset;
    std::vector<uint32_t> grass_tile_count;
    int grass_cols = 0;
    int grass_rows = 0;
    float grass_tile_world = 512.f;
    float grass_a[4] = {0, 0, 0, 0};
    float grass_b[4] = {0, 0, 0, 0};
    float grass_c[4] = {0, 0, 0, 0};
    float grass_wind[2] = {4.5f, 20.f};  // grassWindMult, maxWindSpeed
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> depth = nil;
    id<MTLTexture> offscreen = nil;
    id<MTLBuffer> readback = nil;
    id<MTLSharedEvent> fence = nil;
    id<MTLResidencySet> residency = nil;
    NSUInteger index_count = 0;
    NSUInteger water_index_count = 0;
    NSUInteger ring_index_count = 0;
    NSUInteger model_index_count = 0;
    float team_color[3] = {0, 0, 0};
    float model_pose[4] = {0, 0, 0, 0};
    float marker_radius = 0;
    float bank = 0;
    bool metal_hud = false;
    std::vector<float> piece_cpu;
    // Drawable textures stay resident. The GPU fence fires before the display finishes
    // scanning a frame, and removing the texture there shows up as pink tiles.
    std::vector<id<MTLTexture>> live_drawables;
    NSUInteger target_width = 0;
    NSUInteger target_height = 0;
    uint64_t ticket = 0;
    float sky[3] = {0.43f, 0.58f, 0.64f};
    float sun[3] = {0, 1, 0};
    float ambient[3] = {0.35f, 0.35f, 0.35f};
    float diffuse[3] = {1, 1, 1};
    // SMF ground maps by GroundSlot (slot 2 stays nil) and the uniforms that gate them.
    id<MTLTexture> ground[kGroundSlots] = {};
    float ground_specular[4] = {0.1f, 0.1f, 0.1f, 100.f};
    float splat_scales[4] = {0.02f, 0.02f, 0.02f, 0.02f};
    float splat_mults[4] = {1, 1, 1, 1};
    float smf_flags[4] = {0, 0, 0, 0};
    float map[4] = {0, 0, 0, 0};
    float water_base[4] = {0, 0, 0, 0};
    float water_min[4] = {0, 0, 0, 0};
    float water_absorb[4] = {0, 0, 0, 0};
    float unit_ambient[4] = {0.4f, 0.4f, 0.4f, 0};
    float unit_diffuse[4] = {0.7f, 0.7f, 0.7f, 0};
    float unit_specular[4] = {0.7f, 0.7f, 0.7f, 0};
    // Grass CPU side: heightmap copy for Spring.GetGroundHeight(camera) and tile culling.
    std::vector<float> grass_heights;
    int grass_hm_w = 0;
    int grass_hm_h = 0;
    float grass_square = 8.f;

    float ground_height_at(float x, float z) const {
        if (grass_heights.empty()) return 0.f;
        const int ix = std::clamp(static_cast<int>(x / grass_square + 0.5f), 0, grass_hm_w - 1);
        const int iz = std::clamp(static_cast<int>(z / grass_square + 0.5f), 0, grass_hm_h - 1);
        return grass_heights[static_cast<size_t>(iz) * grass_hm_w + ix];
    }

    // DrawWorldPreUnit tile culling: tiles within fadeEnd + tileRadius of the camera whose
    // bounding sphere (tileMidHeight, tileRadius) is in the view frustum, merged into runs.
    void cull_grass(const float* m, const float* eye, std::vector<std::pair<uint32_t, uint32_t>>& runs) const {
        runs.clear();
        const float half = grass_tile_world * 0.5f;
        const float tile_mid = (height_min + height_max) * 0.5f;
        const float slack = (height_max - height_min) * 0.5f + 200.f;
        const float radius = std::sqrt(half * half * 2.f) + slack;
        const float bound = grass_c[1] + radius;
        // Clip planes from the column-major view_proj (Metal depth 0..1).
        float planes[6][4];
        for (int i = 0; i < 4; ++i) {
            const float r0 = m[i * 4 + 0], r1 = m[i * 4 + 1], r2 = m[i * 4 + 2], r3 = m[i * 4 + 3];
            planes[0][i] = r3 + r0;
            planes[1][i] = r3 - r0;
            planes[2][i] = r3 + r1;
            planes[3][i] = r3 - r1;
            planes[4][i] = r2;
            planes[5][i] = r3 - r2;
        }
        const int tx0 = std::max(0, static_cast<int>(std::floor((eye[0] - bound) / grass_tile_world)));
        const int tx1 = std::min(grass_cols - 1, static_cast<int>(std::floor((eye[0] + bound) / grass_tile_world)));
        const int tz0 = std::max(0, static_cast<int>(std::floor((eye[2] - bound) / grass_tile_world)));
        const int tz1 = std::min(grass_rows - 1, static_cast<int>(std::floor((eye[2] + bound) / grass_tile_world)));
        for (int tz = tz0; tz <= tz1; ++tz) {
            const float czw = tz * grass_tile_world + half;
            long start = -1;
            uint32_t count = 0;
            for (int tx = tx0; tx <= tx1; ++tx) {
                const size_t tile = static_cast<size_t>(tz) * grass_cols + tx;
                bool visible = false;
                if (grass_tile_count[tile] > 0) {
                    const float cxw = tx * grass_tile_world + half;
                    const float dx = cxw - eye[0], dz = czw - eye[2];
                    if (dx * dx + dz * dz < bound * bound) {
                        visible = true;
                        for (const auto& p : planes) {
                            const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                            if ((p[0] * cxw + p[1] * tile_mid + p[2] * czw + p[3]) / len < -radius) {
                                visible = false;
                                break;
                            }
                        }
                    }
                }
                if (visible) {
                    if (start < 0) {
                        start = grass_tile_offset[tile];
                        count = grass_tile_count[tile];
                    } else {
                        count += grass_tile_count[tile];
                    }
                } else if (start >= 0) {
                    runs.emplace_back(static_cast<uint32_t>(start), count);
                    start = -1;
                    count = 0;
                }
            }
            if (start >= 0) runs.emplace_back(static_cast<uint32_t>(start), count);
        }
    }
};

namespace {

void retire_slot(id<MTLResidencySet> residency, FrameSlot& slot) {
    if (slot.hud_color != nil) {
        [residency removeAllocation:slot.hud_color];
        slot.hud_color = nil;
    }
    if (slot.hud_text_verts != nil) {
        [residency removeAllocation:slot.hud_text_verts];
        slot.hud_text_verts = nil;
    }
    if (slot.hud_minimap != nil) {
        [residency removeAllocation:slot.hud_minimap];
        slot.hud_minimap = nil;
    }
    if (slot.hud_marker != nil) {
        [residency removeAllocation:slot.hud_marker];
        slot.hud_marker = nil;
    }
    slot.hud_color_count = 0;
    slot.hud_text_count = 0;
    slot.hud_segments.clear();
    slot.hud_minimap_count = 0;
    slot.hud_marker_count = 0;
    if (slot.allocator != nil) {
        [slot.allocator reset];
    }
    slot.pending = false;
}

std::string ns_error(NSError* error, const char* fallback) {
    if (error.localizedDescription != nil) {
        return std::string(error.localizedDescription.UTF8String);
    }
    return fallback;
}

std::string read_file(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "could not read shader " + path;
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_ppm(const char* path, const uint8_t* bgra, NSUInteger width, NSUInteger height,
               NSUInteger bytes_per_row, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = std::string("could not write ") + path;
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * 3);
    for (NSUInteger y = 0; y < height; ++y) {
        const uint8_t* row = bgra + y * bytes_per_row;
        for (NSUInteger x = 0; x < width; ++x) {
            rgb[x * 3 + 0] = row[x * 4 + 2];
            rgb[x * 3 + 1] = row[x * 4 + 1];
            rgb[x * 3 + 2] = row[x * 4 + 0];
        }
        out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    }
    if (!out) {
        error = std::string("failed while writing ") + path;
        return false;
    }
    return true;
}

id<MTLRenderPipelineState> make_pipeline(id<MTL4Compiler> compiler, id<MTLLibrary> library, NSString* vertex_name,
                                         NSString* fragment_name, bool blend, NSError** error, bool premul = false) {
    MTL4LibraryFunctionDescriptor* vertex = [MTL4LibraryFunctionDescriptor new];
    vertex.library = library;
    vertex.name = vertex_name;
    MTL4LibraryFunctionDescriptor* fragment = [MTL4LibraryFunctionDescriptor new];
    fragment.library = library;
    fragment.name = fragment_name;

    MTL4RenderPipelineDescriptor* pipeline_desc = [MTL4RenderPipelineDescriptor new];
    pipeline_desc.label = fragment_name;
    pipeline_desc.vertexFunctionDescriptor = vertex;
    pipeline_desc.fragmentFunctionDescriptor = fragment;
    pipeline_desc.rasterSampleCount = 1;
    pipeline_desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blend) {
        pipeline_desc.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
        pipeline_desc.colorAttachments[0].sourceRGBBlendFactor =
            premul ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
        pipeline_desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipeline_desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pipeline_desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    return [compiler newRenderPipelineStateWithDescriptor:pipeline_desc compilerTaskOptions:nil error:error];
}

constexpr int kPieceSlots = 128;

void fill_piece_identities(std::vector<float>& mats) {
    mats.assign(static_cast<size_t>(kPieceSlots) * 16, 0.f);
    for (int i = 0; i < kPieceSlots; ++i) {
        float* m = mats.data() + static_cast<size_t>(i) * 16;
        m[0] = 1.f;
        m[5] = 1.f;
        m[10] = 1.f;
        m[15] = 1.f;
    }
}

id<MTLBuffer> upload(id<MTLDevice> device, const void* bytes, size_t size) {
    if (size == 0) {
        return nil;
    }
    return [device newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared];
}

struct SceneBindings {
    id<MTL4ArgumentTable> arguments;
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;
    id<MTLBuffer> water_vertices;
    id<MTLBuffer> water_indices;
    id<MTLBuffer> ring_vertices;
    id<MTLBuffer> ring_indices;
    id<MTLBuffer> model_vertices;
    id<MTLBuffer> model_indices;
    id<MTLRenderPipelineState> terrain_pipeline;
    id<MTLRenderPipelineState> model_pipeline;
    id<MTLRenderPipelineState> water_pipeline;
    id<MTLRenderPipelineState> ring_pipeline;
    id<MTLDepthStencilState> terrain_depth;
    id<MTLDepthStencilState> water_depth;
    id<MTLTexture> minimap;
    id<MTLTexture> metal_map;
    id<MTLTexture> model_color;
    id<MTLTexture> model_other;
    id<MTLTexture> model_normal;
    id<MTLTexture> brdf_lut;
    id<MTLTexture> sky_cube;
    id<MTLTexture> reflection;
    id<MTLRenderPipelineState> sky_pipeline;
    id<MTLDepthStencilState> sky_depth;
    id<MTLTexture> water_tex;
    id<MTLTexture> refract_color;
    id<MTLTexture> refract_depth;
    id<MTLTexture> foam_tex;
    id<MTLTexture> wave_tex;
    id<MTLTexture> caustics_tex;
    id<MTLRenderPipelineState> grass_pipeline;
    id<MTLDepthStencilState> grass_depth;
    id<MTLBuffer> grass_vertices;
    id<MTLBuffer> grass_indices;
    id<MTLBuffer> grass_instances;
    id<MTLTexture> grass_blade;
    id<MTLTexture> grass_perlin;
    id<MTLTexture> grass_height;
    NSUInteger grass_index_count;
    // Visible tile runs (first instance, count), DrawWorldPreUnit's cullRuns.
    std::vector<std::pair<uint32_t, uint32_t>> grass_runs;
    // ReadMap::HasVisibleWater (some ground below 0); BasicWater skips the draw otherwise.
    bool water_visible = false;
    id<MTLBuffer> pieces;
    id<MTLTexture> shadow_map;
    const id<MTLTexture>* ground;
    id<MTLRenderPipelineState> shadow_terrain_pipeline;
    id<MTLRenderPipelineState> shadow_model_pipeline;
    NSUInteger index_count;
    NSUInteger water_index_count;
    NSUInteger ring_index_count;
    NSUInteger model_index_count;
    bool metal_hud = false;
};

MTLPixelFormat gpu_format(GpuImage::Format format) {
    switch (format) {
        case GpuImage::Format::RGBA8: return MTLPixelFormatRGBA8Unorm;
        case GpuImage::Format::BC1: return MTLPixelFormatBC1_RGBA;
        case GpuImage::Format::BC2: return MTLPixelFormatBC2_RGBA;
        case GpuImage::Format::BC3: return MTLPixelFormatBC3_RGBA;
        case GpuImage::Format::RG16F: return MTLPixelFormatRG16Float;
        case GpuImage::Format::R32F: return MTLPixelFormatR32Float;
        default: return MTLPixelFormatInvalid;
    }
}

// BAR luarules/gadgets/include/GenBrdfLut.lua (api_pbr_enabler: 512x512, G_OPTION 1, 1024
// samples, RESTRICT_BRANCHING). Texel (x, y) holds BRDF(NoV = (x + .5) / 512, roughness = (y + .5) / 512).
GpuImage generate_brdf_lut() {
    const int size = 512;
    const uint32_t samples = 1024;
    const float pi = 3.14159265358979323846f;
    GpuImage image;
    image.format = GpuImage::Format::RG16F;
    image.width = size;
    image.height = size;
    image.levels.resize(1);
    image.levels[0].resize(static_cast<size_t>(size) * size * 4);
    auto row_job = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float roughness = (y + 0.5f) / size;
            const float r2 = roughness * roughness;
            const float k = r2 / 2.f;
            for (int x = 0; x < size; ++x) {
                const float nov = (x + 0.5f) / size;
                const float vx = std::sqrt(1.f - nov * nov);
                const float vz = nov;
                float a = 0.f;
                float b = 0.f;
                for (uint32_t i = 0; i < samples; ++i) {
                    uint32_t bits = (i << 16u) | (i >> 16u);
                    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
                    const float xi_x = static_cast<float>(i) / samples;
                    const float xi_y = static_cast<float>(bits) * 2.3283064365386963e-10f;
                    const float phi = 2.f * pi * xi_x;
                    const float cos2 = (1.f - xi_y) / (1.f + (r2 * r2 - 1.f) * xi_y);
                    const float cos_t = std::sqrt(cos2);
                    const float sin_t = std::sqrt(std::max(0.f, 1.f - cos2));
                    // N = +z, so tangentX = +x and tangentY = +y.
                    float hx = sin_t * std::cos(phi);
                    float hy = sin_t * std::sin(phi);
                    float hz = cos_t;
                    const float hl = std::sqrt(hx * hx + hy * hy + hz * hz);
                    hx /= hl;
                    hy /= hl;
                    hz /= hl;
                    const float vdh = vx * hx + vz * hz;
                    const float lz = 2.f * vdh * hz - vz;
                    const float dot_nl = std::max(lz, 0.f);
                    const float dot_vh = std::max(vdh, 0.f);
                    const float dot_nv = std::clamp(vz, 0.0000001f, 1.f);
                    const float dot_nh = std::clamp(hz, 0.0000001f, 1.f);
                    const float gl = dot_nl / (dot_nl * (1.f - k) + k);
                    const float gv = dot_nv / (dot_nv * (1.f - k) + k);
                    const float g_vis = (gl * gv * dot_vh) / (dot_nh * dot_nv);
                    const float fc = std::pow(1.f - dot_vh, 5.f);
                    if (dot_nl > 0.f) {
                        a += (1.f - fc) * g_vis;
                        b += fc * g_vis;
                    }
                }
                _Float16* out = reinterpret_cast<_Float16*>(image.levels[0].data()) + (static_cast<size_t>(y) * size + x) * 2;
                out[0] = static_cast<_Float16>(a / samples);
                out[1] = static_cast<_Float16>(b / samples);
            }
        }
    };
    const int threads = 8;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back(row_job, size * t / threads, size * (t + 1) / threads);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    return image;
}

id<MTLTexture> upload_image(id<MTLDevice> device, const GpuImage& image) {
    if (!image.valid()) {
        return nil;
    }
    const bool cube = image.faces == 6;
    const int mips = image.mip_count();
    MTLTextureDescriptor* desc =
        cube ? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:gpu_format(image.format)
                                                                     size:image.width
                                                                mipmapped:NO]
             : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:gpu_format(image.format)
                                                                  width:image.width
                                                                 height:image.height
                                                              mipmapped:NO];
    desc.mipmapLevelCount = mips;
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    if (texture == nil) {
        return nil;
    }
    const bool block = image.format == GpuImage::Format::BC1 || image.format == GpuImage::Format::BC2 ||
                       image.format == GpuImage::Format::BC3;
    const NSUInteger block_bytes = image.format == GpuImage::Format::BC1 ? 8 : 16;
    const NSUInteger pixel_bytes = image.format == GpuImage::Format::RGBA8 ? 4 : 4;  // RG16F is 4 bytes too
    for (int face = 0; face < image.faces; ++face) {
        NSUInteger w = image.width;
        NSUInteger h = image.height;
        for (int level = 0; level < mips; ++level) {
            const NSUInteger row = block ? std::max<NSUInteger>(1, (w + 3) / 4) * block_bytes : w * pixel_bytes;
            const NSUInteger rows = block ? std::max<NSUInteger>(1, (h + 3) / 4) : h;
            [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                       mipmapLevel:level
                             slice:face
                         withBytes:image.levels[static_cast<size_t>(face * mips + level)].data()
                       bytesPerRow:row
                     bytesPerImage:row * rows];
            w = std::max<NSUInteger>(1, w / 2);
            h = std::max<NSUInteger>(1, h / 2);
        }
    }
    return texture;
}

bool model_drawable(const SceneBindings& scene) {
    return scene.model_index_count > 0 && scene.model_vertices != nil && scene.model_indices != nil &&
           scene.model_color != nil && scene.model_other != nil && scene.model_pipeline != nil && scene.pieces != nil;
}

// Sun-space depth of the terrain and unit 0. The colour pass samples it at texture 2.
void encode_shadow(id<MTL4CommandBuffer> command_buffer, const SceneBindings& scene) {
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.depthAttachment.texture = scene.shadow_map;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.0;
    pass.renderTargetWidth = scene.shadow_map.width;
    pass.renderTargetHeight = scene.shadow_map.height;
    id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    MTLViewport viewport{0, 0, static_cast<double>(scene.shadow_map.width),
                         static_cast<double>(scene.shadow_map.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setArgumentTable:scene.arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    [encoder setDepthStencilState:scene.terrain_depth];
    [scene.arguments setAddress:scene.vertices.gpuAddress atIndex:1];
    [encoder setRenderPipelineState:scene.shadow_terrain_pipeline];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:scene.index_count
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:scene.indices.gpuAddress
                  indexBufferLength:scene.indices.length];
    if (model_drawable(scene)) {
        [scene.arguments setAddress:scene.model_vertices.gpuAddress atIndex:1];
        [scene.arguments setAddress:scene.pieces.gpuAddress atIndex:2];
        [encoder setRenderPipelineState:scene.shadow_model_pipeline];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:scene.model_index_count
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:scene.model_indices.gpuAddress
                      indexBufferLength:scene.model_indices.length];
    }
    [encoder barrierAfterStages:MTLStageVertex | MTLStageFragment
              beforeQueueStages:MTLStageFragment
              visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder endEncoding];
}

bool invert4(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) {
        return false;
    }
    det = 1.f / det;
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * det;
    }
    return true;
}

// Orthographic sun projection fitted to the part of the ground the camera sees.
// Each screen-corner ray is cut at the height band [height_min, height_max] (and at
// kReach elmos), the hits are bounded in sun space, and the depth range is pushed
// back toward the sun so off-screen cliffs still cast into view.
bool fit_shadow(const float view_proj[16], const float sun[3], float height_min, float height_max, float out[16]) {
    constexpr float kReach = 6000.f;
    float inv[16];
    if (!invert4(view_proj, inv)) {
        return false;
    }
    auto unproject = [&](float x, float y, float z, float p[3]) {
        const float v[4] = {x, y, z, 1.f};
        float r[4] = {0, 0, 0, 0};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                r[row] += inv[col * 4 + row] * v[col];
            }
        }
        p[0] = r[0] / r[3];
        p[1] = r[1] / r[3];
        p[2] = r[2] / r[3];
    };
    // Sun basis: f points from the scene toward the sun.
    float f[3] = {sun[0], sun[1], sun[2]};
    float up[3] = {0.f, 1.f, 0.f};
    if (std::fabs(f[1]) > 0.99f) {
        up[1] = 0.f;
        up[2] = 1.f;
    }
    float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    r[0] /= rl;
    r[1] /= rl;
    r[2] /= rl;
    float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    auto add = [&](const float p[3]) {
        const float q[3] = {p[0] * r[0] + p[1] * r[1] + p[2] * r[2], p[0] * u[0] + p[1] * u[1] + p[2] * u[2],
                            p[0] * f[0] + p[1] * f[1] + p[2] * f[2]};
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], q[i]);
            hi[i] = std::max(hi[i], q[i]);
        }
    };
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& c : corners) {
        float n[3];
        float fpt[3];
        unproject(c[0], c[1], 0.f, n);
        unproject(c[0], c[1], 1.f, fpt);
        float d[3] = {fpt[0] - n[0], fpt[1] - n[1], fpt[2] - n[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        d[0] /= dl;
        d[1] /= dl;
        d[2] /= dl;
        for (float h : {height_min, height_max}) {
            float t = kReach;
            if (d[1] < -1e-4f && n[1] > h) {
                t = std::min(kReach, (h - n[1]) / d[1]);
            }
            const float p[3] = {n[0] + d[0] * t, n[1] + d[1] * t, n[2] + d[2] * t};
            add(p);
        }
        add(n);
    }
    // Casters between the sun and the visible ground: extend toward the sun.
    hi[2] += kReach;
    const float sx = 2.f / (hi[0] - lo[0]);
    const float sy = 2.f / (hi[1] - lo[1]);
    const float sz = 1.f / (hi[2] - lo[2]);
    // Column-major: x' = sx*(dot(p,r) - lo0) - 1, y' likewise, z' = (hi2 - dot(p,f)) * sz.
    float m[16] = {};
    m[0] = sx * r[0];
    m[4] = sx * r[1];
    m[8] = sx * r[2];
    m[12] = -sx * lo[0] - 1.f;
    m[1] = sy * u[0];
    m[5] = sy * u[1];
    m[9] = sy * u[2];
    m[13] = -sy * lo[1] - 1.f;
    m[2] = -sz * f[0];
    m[6] = -sz * f[1];
    m[10] = -sz * f[2];
    m[14] = sz * hi[2];
    m[15] = 1.f;
    std::memcpy(out, m, sizeof(m));
    return true;
}

void bind_ground(const SceneBindings& scene) {
    for (int i = 0; i < kGroundSlots; ++i) {
        if (i != 2 && scene.ground[i] != nil) {
            [scene.arguments setTexture:scene.ground[i].gpuResourceID atIndex:i];
        }
    }
    [scene.arguments setTexture:scene.shadow_map.gpuResourceID atIndex:2];
    if (scene.sky_cube != nil) {
        [scene.arguments setTexture:scene.sky_cube.gpuResourceID atIndex:kSkyCubeSlot];
    }
}

void draw_sky(id<MTL4RenderCommandEncoder> encoder, const SceneBindings& scene) {
    if (scene.sky_cube == nil || scene.sky_pipeline == nil) {
        return;
    }
    [encoder setRenderPipelineState:scene.sky_pipeline];
    [encoder setDepthStencilState:scene.sky_depth];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// CubeMapHandler::CreateReflectionFace: clear to fog colour, sky, then the ground
// (DrawPass::TerrainReflection) from the camera position with a 90 degree face camera.
void encode_reflection(id<MTL4CommandBuffer> command_buffer, const SceneBindings& scene, id<MTLTexture> cube,
                       id<MTLTexture> depth, id<MTLBuffer> face_uniforms, id<MTLBuffer> main_uniforms,
                       const bool faces[6], MTLClearColor clear) {
    for (int face = 0; face < 6; ++face) {
        if (!faces[face]) {
            continue;
        }
        MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
        pass.colorAttachments[0].texture = cube;
        pass.colorAttachments[0].slice = face;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = clear;
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 1.0;
        id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
        MTLViewport viewport{0, 0, static_cast<double>(cube.width), static_cast<double>(cube.height), 0, 1};
        [encoder setViewport:viewport];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setArgumentTable:scene.arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [scene.arguments setAddress:face_uniforms.gpuAddress + face * sizeof(TerrainUniforms) atIndex:0];
        bind_ground(scene);
        draw_sky(encoder, scene);
        [scene.arguments setAddress:scene.vertices.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:scene.terrain_pipeline];
        [encoder setDepthStencilState:scene.terrain_depth];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:scene.index_count
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:scene.indices.gpuAddress
                      indexBufferLength:scene.indices.length];
        [encoder barrierAfterStages:MTLStageFragment
                  beforeQueueStages:MTLStageFragment
                  visibilityOptions:MTL4VisibilityOptionDevice];
        [encoder endEncoding];
    }
    [scene.arguments setAddress:main_uniforms.gpuAddress atIndex:0];
}

void encode_scene(id<MTL4CommandBuffer> command_buffer, id<MTLTexture> target, id<MTLTexture> depth,
                  const SceneBindings& scene, MTLClearColor clear) {
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clear;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.0;

    id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    MTLViewport viewport{0, 0, static_cast<double>(target.width), static_cast<double>(target.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    [encoder setArgumentTable:scene.arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];

    // Model draws reuse texture slots 0 and 1, so terrain binds its own maps first.
    bind_ground(scene);
    // CSkyBox::Draw before the ground.
    draw_sky(encoder, scene);
    [scene.arguments setAddress:scene.vertices.gpuAddress atIndex:1];
    [encoder setRenderPipelineState:scene.terrain_pipeline];
    [encoder setDepthStencilState:scene.terrain_depth];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:scene.index_count
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:scene.indices.gpuAddress
                  indexBufferLength:scene.indices.length];

    // widget:DrawWorldPreUnit (Map Grass GL4): after the ground, before units. Depth test
    // LEQUAL with depth writes, back-face culling.
    if (scene.grass_pipeline != nil && !scene.grass_runs.empty()) {
        [scene.arguments setTexture:scene.grass_blade.gpuResourceID atIndex:kGrassBladeSlot];
        [scene.arguments setTexture:scene.grass_perlin.gpuResourceID atIndex:kGrassPerlinSlot];
        [scene.arguments setTexture:scene.grass_height.gpuResourceID atIndex:kGrassHeightSlot];
        [scene.arguments setTexture:scene.minimap.gpuResourceID atIndex:kGrassMapColorSlot];
        [scene.arguments setTexture:scene.shadow_map.gpuResourceID atIndex:2];
        [scene.arguments setAddress:scene.grass_vertices.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:scene.grass_pipeline];
        [encoder setDepthStencilState:scene.grass_depth];
        [encoder setCullMode:MTLCullModeBack];
        [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
        for (const auto& run : scene.grass_runs) {
            [scene.arguments setAddress:scene.grass_instances.gpuAddress + run.first * 16 atIndex:3];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                 indexCount:scene.grass_index_count
                                  indexType:MTLIndexTypeUInt32
                                indexBuffer:scene.grass_indices.gpuAddress
                          indexBufferLength:scene.grass_indices.length
                              instanceCount:run.second];
        }
        [encoder setCullMode:MTLCullModeNone];
        [encoder setFrontFacingWinding:MTLWindingClockwise];
    }

    if (model_drawable(scene)) {
        [scene.arguments setTexture:scene.model_color.gpuResourceID atIndex:0];
        [scene.arguments setTexture:scene.model_other.gpuResourceID atIndex:1];
        // model_fs slot 3 normalTex, slot 4 brdfLUT. Without a normal map the flag skips slot 3.
        id<MTLTexture> normal = scene.model_normal != nil ? scene.model_normal : scene.model_color;
        [scene.arguments setTexture:normal.gpuResourceID atIndex:3];
        [scene.arguments setTexture:scene.brdf_lut.gpuResourceID atIndex:4];
        [scene.arguments setTexture:scene.reflection.gpuResourceID atIndex:kReflectionSlot];
        [scene.arguments setAddress:scene.model_vertices.gpuAddress atIndex:1];
        [scene.arguments setAddress:scene.pieces.gpuAddress atIndex:2];
        [encoder setRenderPipelineState:scene.model_pipeline];
        [encoder setDepthStencilState:scene.terrain_depth];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:scene.model_index_count
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:scene.model_indices.gpuAddress
                      indexBufferLength:scene.model_indices.length];
    }

    // Advanced water: copy the opaque scene (colour and depth) so the water pass can refract the
    // seabed and measure water depth, then draw the surface in a second pass on top.
    const bool water = scene.water_visible && scene.refract_color != nil && scene.refract_depth != nil &&
                       scene.refract_color.width == target.width && scene.refract_color.height == target.height;
    if (water) {
        [encoder barrierAfterStages:MTLStageFragment
                  beforeQueueStages:MTLStageBlit
                  visibilityOptions:MTL4VisibilityOptionDevice];
        [encoder endEncoding];
        id<MTL4ComputeCommandEncoder> copy = [command_buffer computeCommandEncoder];
        [copy copyFromTexture:target toTexture:scene.refract_color];
        [copy copyFromTexture:depth toTexture:scene.refract_depth];
        [copy barrierAfterStages:MTLStageBlit
               beforeQueueStages:MTLStageFragment
               visibilityOptions:MTL4VisibilityOptionDevice];
        [copy endEncoding];

        MTL4RenderPassDescriptor* water_pass = [MTL4RenderPassDescriptor new];
        water_pass.colorAttachments[0].texture = target;
        water_pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        water_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        water_pass.depthAttachment.texture = depth;
        water_pass.depthAttachment.loadAction = MTLLoadActionLoad;
        water_pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        encoder = [command_buffer renderCommandEncoderWithDescriptor:water_pass];
        [encoder setViewport:viewport];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setFrontFacingWinding:MTLWindingClockwise];
        [encoder setArgumentTable:scene.arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];

        if (scene.water_tex != nil) {
            [scene.arguments setTexture:scene.water_tex.gpuResourceID atIndex:kWaterSlot];
        }
        [scene.arguments setTexture:scene.refract_color.gpuResourceID atIndex:kRefractColorSlot];
        [scene.arguments setTexture:scene.refract_depth.gpuResourceID atIndex:kRefractDepthSlot];
        [scene.arguments setTexture:(scene.foam_tex != nil ? scene.foam_tex : scene.wave_tex).gpuResourceID
                            atIndex:kFoamSlot];
        [scene.arguments setTexture:scene.wave_tex.gpuResourceID atIndex:kWaveNormalSlot];
        [scene.arguments setTexture:scene.caustics_tex.gpuResourceID atIndex:kCausticsSlot];
        id<MTLTexture> cube = scene.reflection != nil ? scene.reflection : scene.sky_cube;
        if (cube != nil) {
            [scene.arguments setTexture:cube.gpuResourceID atIndex:kReflectionSlot];
        }
        [scene.arguments setAddress:scene.water_vertices.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:scene.water_pipeline];
        [encoder setDepthStencilState:scene.water_depth];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:scene.water_index_count
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:scene.water_indices.gpuAddress
                      indexBufferLength:scene.water_indices.length];
    }

    // Mex rings sit after the water so spots on the sea stay visible.
    if (scene.metal_hud && scene.ring_index_count > 0 && scene.ring_vertices != nil && scene.ring_indices != nil &&
        scene.ring_pipeline != nil) {
        [scene.arguments setAddress:scene.ring_vertices.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:scene.ring_pipeline];
        [encoder setDepthStencilState:scene.water_depth];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                             indexCount:scene.ring_index_count
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:scene.ring_indices.gpuAddress
                      indexBufferLength:scene.ring_indices.length];
    }

    [encoder barrierAfterStages:MTLStageFragment
              beforeQueueStages:MTLStageBlit
              visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder endEncoding];
}

struct HudGpuVertex {
    float x = 0;
    float y = 0;
    float u = 0;
    float v = 0;
    float r = 1;
    float g = 1;
    float b = 1;
    float a = 1;
};

HudGpuVertex hud_vert(float px, float py, float u, float v, float r, float g, float b, float a, float view_w,
                      float view_h) {
    HudGpuVertex out;
    out.x = (px / view_w) * 2.f - 1.f;
    out.y = (py / view_h) * 2.f - 1.f;
    out.u = u;
    out.v = v;
    out.r = r;
    out.g = g;
    out.b = b;
    out.a = a;
    return out;
}

void append_triangle(std::vector<HudGpuVertex>& verts, const HudGpuVertex& a, const HudGpuVertex& b,
                     const HudGpuVertex& c) {
    verts.push_back(a);
    verts.push_back(b);
    verts.push_back(c);
}

CTFontRef hud_font(const std::string& path, float size) {
    if (!path.empty()) {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                               reinterpret_cast<const UInt8*>(path.data()),
                                                               static_cast<CFIndex>(path.size()), false);
        if (url != nullptr) {
            CGDataProviderRef provider = CGDataProviderCreateWithURL(url);
            CFRelease(url);
            if (provider != nullptr) {
                CGFontRef file_font = CGFontCreateWithDataProvider(provider);
                CGDataProviderRelease(provider);
                if (file_font != nullptr) {
                    CTFontRef font = CTFontCreateWithGraphicsFont(file_font, size, nullptr, nullptr);
                    CGFontRelease(file_font);
                    if (font != nullptr) {
                        return font;
                    }
                }
            }
        }
    }
    return CTFontCreateWithName(CFSTR("Helvetica"), size, nullptr);
}

// Never hand CoreText a NULL CFString (UTF-8 reject of Spring \255 color bytes).
CFStringRef cfstring_lossy(const std::string& text) {
    const UInt8* bytes = reinterpret_cast<const UInt8*>(text.data());
    const CFIndex n = static_cast<CFIndex>(text.size());
    CFStringRef chars =
        CFStringCreateWithBytes(kCFAllocatorDefault, bytes, n, kCFStringEncodingUTF8, false);
    if (chars != nullptr) {
        return chars;
    }
    chars = CFStringCreateWithBytes(kCFAllocatorDefault, bytes, n, kCFStringEncodingISOLatin1, false);
    if (chars != nullptr) {
        return chars;
    }
    std::string safe;
    safe.reserve(text.size());
    for (unsigned char c : text) {
        if (c >= 0x20 && c < 0x80) {
            safe.push_back(static_cast<char>(c));
        } else if (c == '\n' || c == '\t') {
            safe.push_back(static_cast<char>(c));
        } else {
            safe.push_back('?');
        }
    }
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(safe.data()),
                                   static_cast<CFIndex>(safe.size()), kCFStringEncodingUTF8, false);
}

// Spring \255RGB applies a mid-string color; \008 resets to the Print base color.
// Strip codes into plain UTF-8, record (start,len,rgba) runs, then one attributed string.
CTLineRef hud_line(CTFontRef font, const std::string& text, float r, float g, float b, float a,
                 bool apply_color_codes = true) {
    struct ColorRun {
        CFIndex start = 0;
        CFIndex len = 0;
        float r = 1;
        float g = 1;
        float b = 1;
        float a = 1;
    };
    std::string plain;
    plain.reserve(text.size());
    std::vector<ColorRun> runs;
    float cr = r;
    float cg = g;
    float cb = b;
    float ca = a;
    CFIndex run_start = 0;
    auto close_run = [&]() {
        const CFIndex end = static_cast<CFIndex>(plain.size());
        if (end > run_start) {
            ColorRun run;
            run.start = run_start;
            run.len = end - run_start;
            run.r = cr;
            run.g = cg;
            run.b = cb;
            run.a = ca;
            runs.push_back(run);
            run_start = end;
        }
    };
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (apply_color_codes && c == 255 && i + 3 < text.size()) {
            close_run();
            cr = static_cast<unsigned char>(text[i + 1]) / 255.f;
            cg = static_cast<unsigned char>(text[i + 2]) / 255.f;
            cb = static_cast<unsigned char>(text[i + 3]) / 255.f;
            ca = 1.f;
            i += 4;
            continue;
        }
        if (apply_color_codes && c == 8) {
            close_run();
            cr = r;
            cg = g;
            cb = b;
            ca = a;
            ++i;
            continue;
        }
        // When stripping for a solid outline pass, still drop Spring color bytes.
        if (!apply_color_codes && c == 255 && i + 3 < text.size()) {
            i += 4;
            continue;
        }
        if (!apply_color_codes && c == 8) {
            ++i;
            continue;
        }
        plain.push_back(static_cast<char>(c));
        ++i;
    }
    close_run();
    if (plain.empty()) {
        return nullptr;
    }
    CFStringRef chars = cfstring_lossy(plain);
    if (chars == nullptr) {
        return nullptr;
    }
    // DeviceRGB matches the bitmap context; GenericRGB has produced invisible glyphs.
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGFloat base_components[4] = {r, g, b, a};
    CGColorRef base_color =
        space != nullptr ? CGColorCreate(space, base_components) : CGColorCreateGenericRGB(r, g, b, a);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* values[] = {font, base_color};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFMutableAttributedStringRef attr = nullptr;
    if (attrs != nullptr) {
        CFAttributedStringRef base = CFAttributedStringCreate(kCFAllocatorDefault, chars, attrs);
        if (base != nullptr) {
            attr = CFAttributedStringCreateMutableCopy(kCFAllocatorDefault, 0, base);
            CFRelease(base);
        }
        CFRelease(attrs);
    }
    // Runs hold UTF-8 byte offsets; attributed-string ranges are UTF-16 indices.
    // Map each byte offset to its UTF-16 index (identity when cfstring_lossy fell
    // back to a one-byte encoding) and clamp, so multi-byte chat glyphs stay in range.
    const CFIndex utf16_len = CFStringGetLength(chars);
    std::vector<CFIndex> u16(plain.size() + 1, 0);
    if (utf16_len == static_cast<CFIndex>(plain.size())) {
        for (size_t i = 0; i <= plain.size(); ++i) {
            u16[i] = static_cast<CFIndex>(i);
        }
    } else {
        CFIndex k = 0;
        for (size_t i = 0; i < plain.size();) {
            const unsigned char c = static_cast<unsigned char>(plain[i]);
            const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
            for (size_t j = 0; j < n && i + j <= plain.size(); ++j) {
                u16[i + j] = k;
            }
            k += n == 4 ? 2 : 1;
            i += n;
        }
        u16[plain.size()] = k;
    }
    for (ColorRun& run : runs) {
        const CFIndex s = std::min(u16[std::min<size_t>(run.start, plain.size())], utf16_len);
        const CFIndex e = std::min(u16[std::min<size_t>(run.start + run.len, plain.size())], utf16_len);
        run.start = s;
        run.len = e - s;
    }
    CFRelease(chars);
    if (base_color != nullptr) {
        CGColorRelease(base_color);
    }
    if (attr == nullptr) {
        if (space != nullptr) {
            CGColorSpaceRelease(space);
        }
        return nullptr;
    }
    for (const ColorRun& run : runs) {
        if (run.len <= 0) {
            continue;
        }
        CGFloat components[4] = {run.r, run.g, run.b, run.a};
        CGColorRef color =
            space != nullptr ? CGColorCreate(space, components) : CGColorCreateGenericRGB(run.r, run.g, run.b, run.a);
        if (color == nullptr) {
            continue;
        }
        CFAttributedStringSetAttribute(attr, CFRangeMake(run.start, run.len), kCTForegroundColorAttributeName, color);
        CGColorRelease(color);
    }
    if (space != nullptr) {
        CGColorSpaceRelease(space);
    }
    CTLineRef line = CTLineCreateWithAttributedString(attr);
    CFRelease(attr);
    return line;
}

constexpr int kHudAtlasW = 1024;
constexpr int kHudAtlasH = 512;

struct GlyphImage {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    float left = 0;
    float bottom = 0;
    float right = 0;
    float top = 0;
};

struct CachedGlyph {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    float advance = 0;
    float descent = 0;
};

std::string glyph_cache_key(const HudText& text) {
    char head[96];
    std::snprintf(head, sizeof(head), "%.4f %.4f %.4f %.4f %.4f %d ", text.size, text.r, text.g, text.b, text.a,
                  text.outline ? 1 : 0);
    std::string key(head);
    key += text.font_path;
    key.push_back('\n');
    key += text.text;
    return key;
}

bool raster_bitmap(const HudText& text, CachedGlyph& cached) {
    const float size = text.size < 4.f ? 4.f : (text.size > 256.f ? 256.f : text.size);
    // Small HUD labels (income / wind / counters) blob when outlined at 1x; raster at 2x then box-filter.
    const int ss = size < 14.f ? 3 : (size < 20.f ? 2 : 1);
    const float raster_size = size * static_cast<float>(ss);
    const float pad = (text.outline ? 2.f : 1.f) * static_cast<float>(ss);
    CTFontRef font = hud_font(text.font_path, raster_size);
    if (font == nullptr) {
        return false;
    }
    // Fill keeps Spring \255RGB runs. Outline must be solid black on stripped plain text —
    // otherwise mid-string colors tint the 8-offset outline the same as the fill (fat blobs).
    CTLineRef fill = hud_line(font, text.text, text.r, text.g, text.b, text.a, true);
    CTLineRef outline =
        text.outline ? hud_line(font, text.text, 0.f, 0.f, 0.f, text.a, false) : nullptr;
    CFRelease(font);
    if (fill == nullptr) {
        if (outline != nullptr) {
            CFRelease(outline);
        }
        return false;
    }
    CGFloat ascent = 0;
    CGFloat descent = 0;
    CGFloat leading = 0;
    const double advance = CTLineGetTypographicBounds(fill, &ascent, &descent, &leading);
    const int hi_w = static_cast<int>(std::ceil(advance + static_cast<double>(pad) * 2.0));
    const int hi_h = static_cast<int>(std::ceil(static_cast<double>(ascent + descent) + static_cast<double>(pad) * 2.0));
    if (hi_w < 1 || hi_h < 1 || hi_w > 4096 || hi_h > 1024) {
        CFRelease(fill);
        if (outline != nullptr) {
            CFRelease(outline);
        }
        return false;
    }
    std::vector<uint8_t> hi(static_cast<size_t>(hi_w * hi_h * 4), 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(hi.data(), static_cast<size_t>(hi_w), static_cast<size_t>(hi_h), 8,
                                                  static_cast<size_t>(hi_w * 4), space,
                                                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    if (context == nullptr) {
        CFRelease(fill);
        if (outline != nullptr) {
            CFRelease(outline);
        }
        return false;
    }
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    CGContextSetShouldAntialias(context, true);
    CGContextSetAllowsAntialiasing(context, true);
    const CGFloat baseline = pad + descent;
    if (outline != nullptr) {
        // Cardinal offsets only when supersampling: diagonal 8-tap fills tiny glyphs into blobs.
        const CGFloat cardinal[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        const CGFloat full[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        const CGFloat (*offsets)[2] = ss > 1 ? cardinal : full;
        const int n_off = ss > 1 ? 4 : 8;
        for (int i = 0; i < n_off; ++i) {
            CGContextSetTextPosition(context, pad + offsets[i][0], baseline + offsets[i][1]);
            CTLineDraw(outline, context);
        }
        CFRelease(outline);
    }
    CGContextSetTextPosition(context, pad, baseline);
    CTLineDraw(fill, context);
    CFRelease(fill);
    CGContextRelease(context);

    const int width = (hi_w + ss - 1) / ss;
    const int height = (hi_h + ss - 1) / ss;
    std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 4), 0);
    if (ss == 1) {
        pixels = std::move(hi);
    } else {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int sum[4] = {0, 0, 0, 0};
                int count = 0;
                for (int dy = 0; dy < ss; ++dy) {
                    const int sy = y * ss + dy;
                    if (sy >= hi_h) {
                        continue;
                    }
                    for (int dx = 0; dx < ss; ++dx) {
                        const int sx = x * ss + dx;
                        if (sx >= hi_w) {
                            continue;
                        }
                        const size_t o = static_cast<size_t>((sy * hi_w + sx) * 4);
                        sum[0] += hi[o];
                        sum[1] += hi[o + 1];
                        sum[2] += hi[o + 2];
                        sum[3] += hi[o + 3];
                        ++count;
                    }
                }
                const size_t d = static_cast<size_t>((y * width + x) * 4);
                if (count > 0) {
                    pixels[d] = static_cast<uint8_t>(sum[0] / count);
                    pixels[d + 1] = static_cast<uint8_t>(sum[1] / count);
                    pixels[d + 2] = static_cast<uint8_t>(sum[2] / count);
                    pixels[d + 3] = static_cast<uint8_t>(sum[3] / count);
                }
            }
        }
    }

    cached.pixels = std::move(pixels);
    cached.width = width;
    cached.height = height;
    cached.advance = static_cast<float>(advance / static_cast<double>(ss));
    cached.descent = static_cast<float>(descent / static_cast<float>(ss));
    return true;
}

bool raster_glyph(const HudText& text, GlyphImage& image) {
    static std::unordered_map<std::string, CachedGlyph> cache;
    const std::string key = glyph_cache_key(text);
    const auto found = cache.find(key);
    if (found == cache.end()) {
        if (cache.size() > 256) {
            cache.clear();
        }
        CachedGlyph cached;
        if (!raster_bitmap(text, cached)) {
            return false;
        }
        cache.emplace(key, std::move(cached));
    }
    const CachedGlyph& cached = cache.find(key)->second;
    const float pad = text.outline ? 2.f : 1.f;
    float anchor = text.x;
    if (text.align == 1) {
        anchor -= cached.advance * 0.5f;
    } else if (text.align == 2) {
        anchor -= cached.advance;
    }
    image.pixels = cached.pixels;
    image.width = cached.width;
    image.height = cached.height;
    image.left = anchor - pad;
    image.bottom = text.y - cached.descent - pad;
    image.right = image.left + static_cast<float>(cached.width);
    image.top = image.bottom + static_cast<float>(cached.height);
    return true;
}

}  // namespace

void TerrainRenderer::release_hud() {
    FrameSlot* slot = impl_ == nullptr ? nullptr : impl_->active;
    if (slot == nullptr || impl_->residency == nil) {
        return;
    }
    if (slot->hud_color != nil) {
        [impl_->residency removeAllocation:slot->hud_color];
        slot->hud_color = nil;
    }
    if (slot->hud_text_verts != nil) {
        [impl_->residency removeAllocation:slot->hud_text_verts];
        slot->hud_text_verts = nil;
    }
    if (slot->hud_minimap != nil) {
        [impl_->residency removeAllocation:slot->hud_minimap];
        slot->hud_minimap = nil;
    }
    if (slot->hud_marker != nil) {
        [impl_->residency removeAllocation:slot->hud_marker];
        slot->hud_marker = nil;
    }
    slot->hud_color_count = 0;
    slot->hud_text_count = 0;
    slot->hud_minimap_count = 0;
    slot->hud_marker_count = 0;
    slot->hud_segments.clear();
}

bool TerrainRenderer::prepare_hud(const HudList* list, std::string& error) {
    release_hud();
    if (list == nullptr ||
        (list->vertices.empty() && list->texts.empty() && !list->minimap_active && !list->minimap_marker)) {
        return true;
    }
    const float view_w = list->width > 0 ? static_cast<float>(list->width) : 1280.f;
    const float view_h = list->height > 0 ? static_cast<float>(list->height) : 720.f;
    std::vector<HudGpuVertex> color;
    color.reserve(list->vertices.size() / 4 * 6);
    std::vector<FrameSlot::HudSeg> segments;
    bool new_images = false;
    for (size_t i = 0; i + 3 < list->vertices.size(); i += 4) {
        int image = list->vertices[i].image;
        if (image > 0 && impl_->hud_images.find(image) == impl_->hud_images.end()) {
            id<MTLTexture> tex = nil;
            const auto decoded = image_get(image);
            if (decoded && decoded->width > 0 && decoded->height > 0) {
                const bool mips = decoded->mipmaps;
                MTLTextureDescriptor* desc =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                       width:decoded->width
                                                                      height:decoded->height
                                                                   mipmapped:mips ? YES : NO];
                desc.usage = MTLTextureUsageShaderRead;
                desc.storageMode = MTLStorageModeShared;
                tex = [impl_->device newTextureWithDescriptor:desc];
                if (tex != nil) {
                    // Level 0, then a 2x2 box-filtered chain like glGenerateMipmap.
                    int w = decoded->width;
                    int h = decoded->height;
                    std::vector<uint8_t> level = decoded->rgba;
                    for (NSUInteger lv = 0; lv < tex.mipmapLevelCount; ++lv) {
                        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                               mipmapLevel:lv
                                 withBytes:level.data()
                               bytesPerRow:static_cast<NSUInteger>(w) * 4];
                        if (w == 1 && h == 1) {
                            break;
                        }
                        const int nw = std::max(1, w / 2);
                        const int nh = std::max(1, h / 2);
                        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
                        for (int y = 0; y < nh; ++y) {
                            for (int x = 0; x < nw; ++x) {
                                const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                                const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                                for (int ch = 0; ch < 4; ++ch) {
                                    const int sum = level[(static_cast<size_t>(y0) * w + x0) * 4 + ch] +
                                                    level[(static_cast<size_t>(y0) * w + x1) * 4 + ch] +
                                                    level[(static_cast<size_t>(y1) * w + x0) * 4 + ch] +
                                                    level[(static_cast<size_t>(y1) * w + x1) * 4 + ch];
                                    next[(static_cast<size_t>(y) * nw + x) * 4 + ch] = static_cast<uint8_t>((sum + 2) / 4);
                                }
                            }
                        }
                        level.swap(next);
                        w = nw;
                        h = nh;
                    }
                    [impl_->residency addAllocation:tex];
                    new_images = true;
                }
            }
            impl_->hud_images[image] = tex;
        }
        if (image > 0 && impl_->hud_images[image] == nil) {
            continue;  // undecodable: drop, never a solid stand-in
        }
        HudGpuVertex corners[4];
        for (int corner = 0; corner < 4; ++corner) {
            const HudVertex& vert = list->vertices[i + static_cast<size_t>(corner)];
            corners[corner] = hud_vert(vert.x, vert.y, vert.u, vert.v, vert.r, vert.g, vert.b, vert.a, view_w, view_h);
        }
        if (segments.empty() || segments.back().image != image) {
            segments.push_back({static_cast<NSUInteger>(color.size()), 0, image});
        }
        append_triangle(color, corners[0], corners[1], corners[2]);
        append_triangle(color, corners[0], corners[2], corners[3]);
        segments.back().count += 6;
    }
    if (new_images) {
        [impl_->residency commit];
    }
    std::vector<HudGpuVertex> text_verts;
    text_verts.reserve(list->texts.size() * 6);
    std::vector<GlyphImage> glyphs;
    glyphs.reserve(list->texts.size());
    for (const HudText& text : list->texts) {
        if (text.text.empty()) {
            continue;
        }
        GlyphImage glyph;
        if (!raster_glyph(text, glyph)) {
            continue;
        }
        glyphs.push_back(std::move(glyph));
    }
    FrameSlot* slot = impl_->active;
    if (slot == nullptr) {
        error = "hud frame slot is missing";
        return false;
    }
    if (!glyphs.empty() && slot->hud_atlas != nil) {
        std::vector<uint8_t> atlas(static_cast<size_t>(kHudAtlasW * kHudAtlasH * 4), 0);
        int pen_x = 1;
        int pen_y = 1;
        int row_h = 0;
        for (const GlyphImage& glyph : glyphs) {
            if (pen_x + glyph.width + 1 > kHudAtlasW) {
                pen_x = 1;
                pen_y += row_h + 1;
                row_h = 0;
            }
            if (pen_y + glyph.height + 1 > kHudAtlasH) {
                continue;
            }
            for (int row = 0; row < glyph.height; ++row) {
                std::memcpy(atlas.data() + static_cast<size_t>(((pen_y + row) * kHudAtlasW + pen_x) * 4),
                            glyph.pixels.data() + static_cast<size_t>(row * glyph.width * 4),
                            static_cast<size_t>(glyph.width * 4));
            }
            const float u0 = static_cast<float>(pen_x) / static_cast<float>(kHudAtlasW);
            const float v0 = static_cast<float>(pen_y) / static_cast<float>(kHudAtlasH);
            const float u1 = static_cast<float>(pen_x + glyph.width) / static_cast<float>(kHudAtlasW);
            const float v1 = static_cast<float>(pen_y + glyph.height) / static_cast<float>(kHudAtlasH);
            const HudGpuVertex corners[4] = {
                hud_vert(glyph.left, glyph.bottom, u0, v1, 1, 1, 1, 1, view_w, view_h),
                hud_vert(glyph.right, glyph.bottom, u1, v1, 1, 1, 1, 1, view_w, view_h),
                hud_vert(glyph.right, glyph.top, u1, v0, 1, 1, 1, 1, view_w, view_h),
                hud_vert(glyph.left, glyph.top, u0, v0, 1, 1, 1, 1, view_w, view_h),
            };
            append_triangle(text_verts, corners[0], corners[1], corners[2]);
            append_triangle(text_verts, corners[0], corners[2], corners[3]);
            pen_x += glyph.width + 1;
            if (glyph.height > row_h) {
                row_h = glyph.height;
            }
        }
        [slot->hud_atlas replaceRegion:MTLRegionMake2D(0, 0, kHudAtlasW, kHudAtlasH)
                            mipmapLevel:0
                              withBytes:atlas.data()
                            bytesPerRow:kHudAtlasW * 4];
    }
    if (list->minimap_active && impl_->minimap != nil) {
        std::vector<HudGpuVertex> mm;
        const HudGpuVertex corners[4] = {
            hud_vert(list->minimap_left, list->minimap_bottom, 0, 1, 1, 1, 1, 1, view_w, view_h),
            hud_vert(list->minimap_right, list->minimap_bottom, 1, 1, 1, 1, 1, 1, view_w, view_h),
            hud_vert(list->minimap_right, list->minimap_top, 1, 0, 1, 1, 1, 1, view_w, view_h),
            hud_vert(list->minimap_left, list->minimap_top, 0, 0, 1, 1, 1, 1, view_w, view_h),
        };
        append_triangle(mm, corners[0], corners[1], corners[2]);
        append_triangle(mm, corners[0], corners[2], corners[3]);
        slot->hud_minimap = upload(impl_->device, mm.data(), mm.size() * sizeof(HudGpuVertex));
        if (slot->hud_minimap == nil) {
            error = "hud minimap buffer allocation failed";
            return false;
        }
        slot->hud_minimap_count = mm.size();
        [impl_->residency addAllocation:slot->hud_minimap];
    }
    if (list->minimap_marker) {
        const float hx = list->minimap_marker_half;
        const float mx = list->minimap_marker_x;
        const float my = list->minimap_marker_y;
        std::vector<HudGpuVertex> mk;
        const HudGpuVertex corners[4] = {
            hud_vert(mx - hx, my - hx, 0, 0, 0.2f, 0.9f, 0.3f, 1.f, view_w, view_h),
            hud_vert(mx + hx, my - hx, 0, 0, 0.2f, 0.9f, 0.3f, 1.f, view_w, view_h),
            hud_vert(mx + hx, my + hx, 0, 0, 0.2f, 0.9f, 0.3f, 1.f, view_w, view_h),
            hud_vert(mx - hx, my + hx, 0, 0, 0.2f, 0.9f, 0.3f, 1.f, view_w, view_h),
        };
        append_triangle(mk, corners[0], corners[1], corners[2]);
        append_triangle(mk, corners[0], corners[2], corners[3]);
        slot->hud_marker = upload(impl_->device, mk.data(), mk.size() * sizeof(HudGpuVertex));
        if (slot->hud_marker == nil) {
            error = "hud marker buffer allocation failed";
            return false;
        }
        slot->hud_marker_count = mk.size();
        [impl_->residency addAllocation:slot->hud_marker];
    }
    if (!color.empty()) {
        slot->hud_color = upload(impl_->device, color.data(), color.size() * sizeof(HudGpuVertex));
        if (slot->hud_color == nil) {
            error = "hud color buffer allocation failed";
            return false;
        }
        slot->hud_color_count = color.size();
        slot->hud_segments = std::move(segments);
        [impl_->residency addAllocation:slot->hud_color];
    }
    if (!text_verts.empty()) {
        slot->hud_text_verts = upload(impl_->device, text_verts.data(), text_verts.size() * sizeof(HudGpuVertex));
        if (slot->hud_text_verts == nil) {
            error = "hud text buffer allocation failed";
            return false;
        }
        slot->hud_text_count = text_verts.size();
        [impl_->residency addAllocation:slot->hud_text_verts];
    }
    return true;
}

void TerrainRenderer::encode_hud(void* command_buffer, void* target) {
    FrameSlot* slot = impl_->active;
    if (slot == nullptr ||
        (slot->hud_color_count == 0 && slot->hud_text_count == 0 && slot->hud_minimap_count == 0 &&
         slot->hud_marker_count == 0)) {
        return;
    }
    id<MTL4CommandBuffer> buffer = (__bridge id<MTL4CommandBuffer>)command_buffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)target;
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    MTLViewport viewport{0, 0, static_cast<double>(color.width), static_cast<double>(color.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setArgumentTable:impl_->arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];
    // Color HUD first (selection, bars, FlowUI chrome), then the map texture on top so
    // minimap chrome strips cannot sit above the terrain thumb.
    if (slot->hud_color_count > 0 && slot->hud_color != nil && impl_->hud_color_pipeline != nil) {
        [impl_->arguments setAddress:slot->hud_color.gpuAddress atIndex:1];
        for (const FrameSlot::HudSeg& seg : slot->hud_segments) {
            if (seg.image == 0) {
                [encoder setRenderPipelineState:impl_->hud_color_pipeline];
            } else {
                const auto at = impl_->hud_images.find(seg.image);
                if (at == impl_->hud_images.end() || at->second == nil || impl_->hud_image_pipeline == nil) continue;
                [impl_->arguments setTexture:at->second.gpuResourceID atIndex:0];
                [encoder setRenderPipelineState:impl_->hud_image_pipeline];
            }
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:seg.start vertexCount:seg.count];
        }
    }
    if (slot->hud_minimap_count > 0 && slot->hud_minimap != nil && impl_->hud_text_pipeline != nil &&
        impl_->minimap != nil) {
        [impl_->arguments setTexture:impl_->minimap.gpuResourceID atIndex:0];
        [impl_->arguments setAddress:slot->hud_minimap.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:impl_->hud_text_pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:slot->hud_minimap_count];
    }
    // Unit marker above the map texture (and above any leftover FlowUI chrome).
    if (slot->hud_marker_count > 0 && slot->hud_marker != nil && impl_->hud_color_pipeline != nil) {
        [impl_->arguments setAddress:slot->hud_marker.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:impl_->hud_color_pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:slot->hud_marker_count];
    }
    if (slot->hud_text_count > 0 && slot->hud_text_verts != nil && impl_->hud_text_pipeline != nil &&
        slot->hud_atlas != nil) {
        [encoder setArgumentTable:impl_->arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [impl_->arguments setTexture:slot->hud_atlas.gpuResourceID atIndex:0];
        [impl_->arguments setAddress:slot->hud_text_verts.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:impl_->hud_text_pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:slot->hud_text_count];
    }

    // The screenshot copy is a blit. It must wait until this pass has stored the bar.
    [encoder barrierAfterStages:MTLStageFragment
            beforeQueueStages:MTLStageBlit
            visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder endEncoding];
}

TerrainRenderer::TerrainRenderer() : impl_(new Impl) {}

TerrainRenderer::~TerrainRenderer() {
    std::string error;
    finish_gpu(error);
    delete impl_;
}

bool TerrainRenderer::finish_gpu(std::string& error) {
    if (impl_ == nullptr || impl_->fence == nil) {
        return true;
    }
    for (FrameSlot& slot : impl_->slots) {
        if (!slot.pending) {
            continue;
        }
        if (slot.ticket != 0 && ![impl_->fence waitUntilSignaledValue:slot.ticket timeoutMS:5000]) {
            error = "gpu frame timed out";
            return false;
        }
        retire_slot(impl_->residency, slot);
    }
    if (impl_->residency != nil) {
        [impl_->residency commit];
    }
    return true;
}

bool TerrainRenderer::init(const std::string& shader_path, const TerrainMesh& mesh, std::string& error) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.water_vertices.empty() || mesh.minimap_rgba.empty() ||
        mesh.metal.empty()) {
        error = "terrain mesh is empty";
        return false;
    }
    const std::string source = read_file(shader_path, error);
    if (source.empty()) {
        return false;
    }

    impl_->device = MTLCreateSystemDefaultDevice();
    if (impl_->device == nil) {
        error = "MTLCreateSystemDefaultDevice returned nil";
        return false;
    }
    NSError* ns_error_obj = nil;
    id<MTL4Compiler> compiler =
        [impl_->device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&ns_error_obj];
    if (compiler == nil) {
        error = ns_error(ns_error_obj, "Metal 4 compiler creation failed");
        return false;
    }
    MTL4LibraryDescriptor* library_desc = [MTL4LibraryDescriptor new];
    library_desc.name = @"terrain";
    library_desc.source = [NSString stringWithUTF8String:source.c_str()];
    id<MTLLibrary> library = [compiler newLibraryWithDescriptor:library_desc error:&ns_error_obj];
    if (library == nil) {
        error = ns_error(ns_error_obj, "terrain shader compile failed");
        return false;
    }
    impl_->terrain_pipeline = make_pipeline(compiler, library, @"terrain_vs", @"terrain_fs", false, &ns_error_obj);
    if (impl_->terrain_pipeline == nil) {
        error = ns_error(ns_error_obj, "terrain pipeline creation failed");
        return false;
    }
    impl_->water_pipeline = make_pipeline(compiler, library, @"terrain_vs", @"water_fs", true, &ns_error_obj);
    if (impl_->water_pipeline == nil) {
        error = ns_error(ns_error_obj, "water pipeline creation failed");
        return false;
    }
    impl_->grass_pipeline = make_pipeline(compiler, library, @"grass_vs", @"grass_fs", true, &ns_error_obj);
    if (impl_->grass_pipeline == nil) {
        error = ns_error(ns_error_obj, "grass pipeline creation failed");
        return false;
    }
    {
        MTLDepthStencilDescriptor* grass_depth = [MTLDepthStencilDescriptor new];
        grass_depth.depthCompareFunction = MTLCompareFunctionLessEqual;
        grass_depth.depthWriteEnabled = YES;
        impl_->grass_depth = [impl_->device newDepthStencilStateWithDescriptor:grass_depth];
    }
    impl_->model_pipeline = make_pipeline(compiler, library, @"model_vs", @"model_fs", false, &ns_error_obj);
    if (impl_->model_pipeline == nil) {
        error = ns_error(ns_error_obj, "model pipeline creation failed");
        return false;
    }
    {
        // Depth-only sun passes: no fragment stage, no colour attachment.
        for (int which = 0; which < 2; ++which) {
            MTL4LibraryFunctionDescriptor* vertex = [MTL4LibraryFunctionDescriptor new];
            vertex.library = library;
            vertex.name = which == 0 ? @"shadow_terrain_vs" : @"shadow_model_vs";
            MTL4RenderPipelineDescriptor* desc = [MTL4RenderPipelineDescriptor new];
            desc.label = vertex.name;
            desc.vertexFunctionDescriptor = vertex;
            desc.rasterSampleCount = 1;
            desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
            desc.colorAttachments[0].pixelFormat = MTLPixelFormatInvalid;
            id<MTLRenderPipelineState> state =
                [compiler newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil error:&ns_error_obj];
            if (state == nil) {
                error = ns_error(ns_error_obj, "shadow pipeline creation failed");
                return false;
            }
            (which == 0 ? impl_->shadow_terrain_pipeline : impl_->shadow_model_pipeline) = state;
        }
        MTLTextureDescriptor* shadow_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:kShadowSize
                                                              height:kShadowSize
                                                           mipmapped:NO];
        shadow_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        shadow_desc.storageMode = MTLStorageModePrivate;
        impl_->shadow_map = [impl_->device newTextureWithDescriptor:shadow_desc];
        if (impl_->shadow_map == nil) {
            error = "shadow map allocation failed";
            return false;
        }
        [impl_->residency addAllocation:impl_->shadow_map];
    }
    impl_->ring_pipeline = make_pipeline(compiler, library, @"terrain_vs", @"ring_fs", true, &ns_error_obj);
    impl_->sky_pipeline = make_pipeline(compiler, library, @"sky_vs", @"sky_fs", false, &ns_error_obj);
    if (impl_->sky_pipeline == nil) {
        error = ns_error(ns_error_obj, "sky pipeline creation failed");
        return false;
    }
    if (impl_->ring_pipeline == nil) {
        error = ns_error(ns_error_obj, "metal spot ring pipeline creation failed");
        return false;
    }
    impl_->hud_color_pipeline = make_pipeline(compiler, library, @"hud_vs", @"hud_color_fs", true, &ns_error_obj);
    if (impl_->hud_color_pipeline == nil) {
        error = ns_error(ns_error_obj, "hud color pipeline creation failed");
        return false;
    }
    impl_->hud_text_pipeline = make_pipeline(compiler, library, @"hud_vs", @"hud_text_fs", true, &ns_error_obj, true);
    impl_->hud_image_pipeline = make_pipeline(compiler, library, @"hud_vs", @"hud_image_fs", true, &ns_error_obj, true);
    if (impl_->hud_image_pipeline == nil) {
        error = ns_error(ns_error_obj, "hud image pipeline creation failed");
        return false;
    }
    if (impl_->hud_text_pipeline == nil) {
        error = ns_error(ns_error_obj, "hud text pipeline creation failed");
        return false;
    }

    MTLDepthStencilDescriptor* terrain_depth = [MTLDepthStencilDescriptor new];
    terrain_depth.depthCompareFunction = MTLCompareFunctionLess;
    terrain_depth.depthWriteEnabled = YES;
    impl_->terrain_depth = [impl_->device newDepthStencilStateWithDescriptor:terrain_depth];
    MTLDepthStencilDescriptor* water_depth = [MTLDepthStencilDescriptor new];
    water_depth.depthCompareFunction = MTLCompareFunctionLess;
    water_depth.depthWriteEnabled = NO;
    impl_->water_depth = [impl_->device newDepthStencilStateWithDescriptor:water_depth];
    // CubeMapVS writes depth 1 (xyww) under GL_LEQUAL with no depth writes: always behind.
    MTLDepthStencilDescriptor* sky_depth = [MTLDepthStencilDescriptor new];
    sky_depth.depthCompareFunction = MTLCompareFunctionLessEqual;
    sky_depth.depthWriteEnabled = NO;
    impl_->sky_depth = [impl_->device newDepthStencilStateWithDescriptor:sky_depth];
    if (impl_->terrain_depth == nil || impl_->water_depth == nil) {
        error = "depth state creation failed";
        return false;
    }

    impl_->vertices = upload(impl_->device, mesh.vertices.data(), mesh.vertices.size() * sizeof(TerrainVertex));
    impl_->indices = upload(impl_->device, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
    impl_->water_vertices =
        upload(impl_->device, mesh.water_vertices.data(), mesh.water_vertices.size() * sizeof(TerrainVertex));
    impl_->water_indices =
        upload(impl_->device, mesh.water_indices.data(), mesh.water_indices.size() * sizeof(uint32_t));
    fill_piece_identities(impl_->piece_cpu);
    bool slot_buffers = true;
    for (FrameSlot& slot : impl_->slots) {
        slot.uniforms = [impl_->device newBufferWithLength:sizeof(TerrainUniforms) options:MTLResourceStorageModeShared];
        slot.face_uniforms =
            [impl_->device newBufferWithLength:sizeof(TerrainUniforms) * 6 options:MTLResourceStorageModeShared];
        if (slot.face_uniforms == nil) {
            slot_buffers = false;
        }
        slot.pieces = upload(impl_->device, impl_->piece_cpu.data(), impl_->piece_cpu.size() * sizeof(float));
        if (slot.uniforms == nil || slot.pieces == nil) {
            slot_buffers = false;
        }
    }
    impl_->ring_vertices =
        upload(impl_->device, mesh.ring_vertices.data(), mesh.ring_vertices.size() * sizeof(TerrainVertex));
    impl_->ring_indices =
        upload(impl_->device, mesh.ring_indices.data(), mesh.ring_indices.size() * sizeof(uint32_t));
    impl_->index_count = mesh.indices.size();
    impl_->water_index_count = mesh.water_indices.size();
    impl_->ring_index_count = mesh.ring_indices.size();
    if (!mesh.ring_indices.empty() && (impl_->ring_vertices == nil || impl_->ring_indices == nil)) {
        error = "metal spot ring buffer allocation failed";
        return false;
    }
    if (impl_->vertices == nil || impl_->indices == nil || impl_->water_vertices == nil || impl_->water_indices == nil ||
        !slot_buffers) {
        error = "terrain buffer allocation failed";
        return false;
    }

    MTLTextureDescriptor* minimap_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:static_cast<NSUInteger>(mesh.minimap_size)
                                                          height:static_cast<NSUInteger>(mesh.minimap_size)
                                                       mipmapped:NO];
    minimap_desc.usage = MTLTextureUsageShaderRead;
    minimap_desc.storageMode = MTLStorageModeShared;
    impl_->minimap = [impl_->device newTextureWithDescriptor:minimap_desc];
    const MTLRegion minimap_region = MTLRegionMake2D(0, 0, mesh.minimap_size, mesh.minimap_size);
    [impl_->minimap replaceRegion:minimap_region
                      mipmapLevel:0
                        withBytes:mesh.minimap_rgba.data()
                      bytesPerRow:static_cast<NSUInteger>(mesh.minimap_size) * 4];

    MTLTextureDescriptor* metal_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:static_cast<NSUInteger>(mesh.metal_size)
                                                          height:static_cast<NSUInteger>(mesh.metal_size)
                                                       mipmapped:NO];
    metal_desc.usage = MTLTextureUsageShaderRead;
    metal_desc.storageMode = MTLStorageModeShared;
    impl_->metal_map = [impl_->device newTextureWithDescriptor:metal_desc];
    const MTLRegion metal_region = MTLRegionMake2D(0, 0, mesh.metal_size, mesh.metal_size);
    [impl_->metal_map replaceRegion:metal_region
                        mipmapLevel:0
                          withBytes:mesh.metal.data()
                        bytesPerRow:static_cast<NSUInteger>(mesh.metal_size)];
    if (impl_->minimap == nil || impl_->metal_map == nil) {
        error = "terrain texture allocation failed";
        return false;
    }

    {
        TerrainTextures textures;
        if (!load_terrain_textures(mesh, textures, error)) {
            return false;
        }
        impl_->ground[kGroundDiffuse] = upload_image(impl_->device, textures.diffuse);
        impl_->ground[kGroundNormals] = upload_image(impl_->device, textures.normals);
        if (impl_->ground[kGroundDiffuse] == nil || impl_->ground[kGroundNormals] == nil) {
            error = "SMF diffuse or normals texture upload failed";
            return false;
        }
        impl_->ground[kGroundSpecular] = upload_image(impl_->device, textures.specular);
        impl_->ground[kGroundBlendNormals] = upload_image(impl_->device, textures.blend_normals);
        impl_->sky_cube = upload_image(impl_->device, textures.sky_box);
        impl_->water_tex = upload_image(impl_->device, textures.water);
        if (impl_->water_tex != nil) {
            // CBasicWater::GenWaterQuadsRB repeat; the ocean.jpg divide applies to that exact path only.
            float repeat_x = 65536.f / static_cast<float>(mesh.map_x);
            float repeat_y = 65536.f / static_cast<float>(mesh.map_y) * textures.water.width / textures.water.height;
            if (textures.water_path == "bitmaps/ocean.jpg") {
                repeat_x /= 4.f;
                repeat_y /= 4.f;
            }
            impl_->water_repeat[0] = mesh.water.repeat_x != 0.f ? mesh.water.repeat_x : repeat_x;
            impl_->water_repeat[1] = mesh.water.repeat_y != 0.f ? mesh.water.repeat_y : repeat_y;
            impl_->water_repeat[2] = 1.f;
        }
        impl_->foam_tex = upload_image(impl_->device, textures.water_foam);
        impl_->wave_tex = upload_image(impl_->device, textures.water_normals);
        impl_->caustics_tex = upload_image(impl_->device, textures.water_caustics);
        if (impl_->wave_tex == nil || impl_->caustics_tex == nil) {
            error = "procedural water texture upload failed";
            return false;
        }
        std::fprintf(stderr, "water foam: %s\n", impl_->foam_tex != nil ? "bitmaps/atmos/waterfoam.tga" : "none");
        std::fprintf(stderr, "water texture: %s %dx%d repeat %.3f %.3f\n", textures.water_path.c_str(),
                     textures.water.width, textures.water.height, impl_->water_repeat[0], impl_->water_repeat[1]);
        const bool splat = textures.splat_normal_count == 4 && textures.splat_distr.valid();
        if (splat) {
            impl_->ground[kGroundSplatDistr] = upload_image(impl_->device, textures.splat_distr);
            for (int i = 0; i < 4; ++i) {
                impl_->ground[kGroundSplatNormal0 + i] = upload_image(impl_->device, textures.splat_normals[i]);
            }
        }
        // Unused slots still need a bound texture; the flags keep the shader off them.
        for (int i = 0; i < kGroundSlots; ++i) {
            if (i != 2 && impl_->ground[i] == nil) {
                impl_->ground[i] = impl_->ground[kGroundNormals];
            }
        }
        // HaveSplatNormalTexture needs every splatDetailNormalTex the shader samples (4 here).
        impl_->smf_flags[0] = textures.specular.valid() ? 1.f : 0.f;
        impl_->smf_flags[1] = splat ? 1.f : 0.f;
        impl_->smf_flags[2] = textures.blend_normals.valid() ? 1.f : 0.f;
        impl_->smf_flags[3] = mesh.splat_diffuse_alpha ? 1.f : 0.f;
        std::fprintf(stderr, "smf textures: diffuse %dx%d specular %d splat %d blend %d alpha %d\n",
                     textures.diffuse.width, textures.diffuse.height, (int)impl_->smf_flags[0],
                     (int)impl_->smf_flags[1], (int)impl_->smf_flags[2], (int)impl_->smf_flags[3]);
    }
    {
        MTLTextureDescriptor* cube_desc =
            [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                  size:kReflectionSize
                                                             mipmapped:NO];
        cube_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        cube_desc.storageMode = MTLStorageModePrivate;
        impl_->reflection = [impl_->device newTextureWithDescriptor:cube_desc];
        MTLTextureDescriptor* depth_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:kReflectionSize
                                                              height:kReflectionSize
                                                           mipmapped:NO];
        depth_desc.usage = MTLTextureUsageRenderTarget;
        depth_desc.storageMode = MTLStorageModePrivate;
        impl_->reflection_depth = [impl_->device newTextureWithDescriptor:depth_desc];
        if (impl_->reflection == nil || impl_->reflection_depth == nil) {
            error = "reflection cubemap allocation failed";
            return false;
        }
    }
    std::memcpy(impl_->plane_color, mesh.water.plane_color, sizeof(float) * 3);
    impl_->plane_color[3] = mesh.water.has_water_plane ? 1.f : 0.f;
    impl_->brdf_lut = upload_image(impl_->device, generate_brdf_lut());
    if (impl_->brdf_lut == nil) {
        error = "BRDF LUT upload failed";
        return false;
    }
    std::memcpy(impl_->ground_specular, mesh.specular, sizeof(float) * 3);
    impl_->ground_specular[3] = mesh.specular_exponent;
    std::memcpy(impl_->splat_scales, mesh.splat_scales, sizeof(impl_->splat_scales));
    std::memcpy(impl_->splat_mults, mesh.splat_mults, sizeof(impl_->splat_mults));
    impl_->map[0] = mesh.world_x;
    impl_->map[1] = mesh.world_z;
    // SMF_WATER_ABSORPTION = HasVisibleWater: some ground below the water plane.
    impl_->map[2] = mesh.min_height < 0.f ? 1.f : 0.f;
    std::memcpy(impl_->water_base, mesh.water.base_color, sizeof(float) * 3);
    std::memcpy(impl_->water_min, mesh.water.min_color, sizeof(float) * 3);
    std::memcpy(impl_->water_absorb, mesh.water.absorb, sizeof(float) * 3);
    {
        const auto& w = mesh.water;
        const float fresnel[4] = {w.fresnel_min, w.fresnel_max, w.fresnel_power, w.reflection_distortion};
        const float waves[4] = {w.perlin_start_freq, w.perlin_lacunarity, w.perlin_amplitude, w.wind_speed};
        const float light[4] = {w.ambient_factor, w.diffuse_factor, w.specular_factor, w.specular_power};
        const float spec[4] = {w.specular_color[0], w.specular_color[1], w.specular_color[2], w.surface_alpha};
        const float surf[4] = {w.surface_color[0], w.surface_color[1], w.surface_color[2], 0.f};
        const float shore[4] = {w.shore_waves ? 1.f : 0.f, w.wave_length, w.wave_foam_intensity,
                                w.wave_foam_distortion};
        std::memcpy(impl_->water_fresnel, fresnel, sizeof(fresnel));
        std::memcpy(impl_->water_waves, waves, sizeof(waves));
        std::memcpy(impl_->water_light, light, sizeof(light));
        std::memcpy(impl_->water_specular, spec, sizeof(spec));
        std::memcpy(impl_->water_surface, surf, sizeof(surf));
        std::memcpy(impl_->water_shore, shore, sizeof(shore));
        impl_->water_extra[0] = w.caustics_resolution;
        impl_->water_extra[1] = w.caustics_strength;
    }
    std::memcpy(impl_->unit_ambient, mesh.unit_ambient, sizeof(float) * 3);
    std::memcpy(impl_->unit_diffuse, mesh.unit_diffuse, sizeof(float) * 3);
    std::memcpy(impl_->unit_specular, mesh.unit_specular, sizeof(float) * 3);

    MTLSamplerDescriptor* sampler_desc = [MTLSamplerDescriptor new];
    sampler_desc.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->sampler = [impl_->device newSamplerStateWithDescriptor:sampler_desc];
    if (impl_->sampler == nil) {
        error = "sampler creation failed";
        return false;
    }

    MTL4ArgumentTableDescriptor* table_desc = [MTL4ArgumentTableDescriptor new];
    table_desc.maxBufferBindCount = 4;
    table_desc.maxTextureBindCount = kTextureSlots;  // terrain_fs: 0..9, slot 2 the shadow map
    table_desc.maxSamplerStateBindCount = 1;
    table_desc.initializeBindings = YES;
    impl_->arguments = [impl_->device newArgumentTableWithDescriptor:table_desc error:&ns_error_obj];
    if (impl_->arguments == nil) {
        error = ns_error(ns_error_obj, "argument table creation failed");
        return false;
    }
    [impl_->arguments setAddress:impl_->slots[0].uniforms.gpuAddress atIndex:0];
    [impl_->arguments setAddress:impl_->vertices.gpuAddress atIndex:1];
    [impl_->arguments setAddress:impl_->slots[0].pieces.gpuAddress atIndex:2];
    [impl_->arguments setTexture:impl_->minimap.gpuResourceID atIndex:0];
    [impl_->arguments setTexture:impl_->metal_map.gpuResourceID atIndex:1];
    [impl_->arguments setSamplerState:impl_->sampler.gpuResourceID atIndex:0];

    impl_->queue = [impl_->device newMTL4CommandQueue];
    impl_->fence = [impl_->device newSharedEvent];
    impl_->residency = [impl_->device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new]
                                                              error:&ns_error_obj];
    bool slot_commands = impl_->queue != nil && impl_->fence != nil && impl_->residency != nil;
    for (FrameSlot& slot : impl_->slots) {
        slot.allocator = [impl_->device newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new]
                                                                     error:&ns_error_obj];
        slot.command_buffer = [impl_->device newCommandBuffer];
        if (slot.allocator == nil || slot.command_buffer == nil) {
            slot_commands = false;
        }
    }
    if (!slot_commands) {
        error = ns_error(ns_error_obj, "Metal 4 command objects were not created");
        return false;
    }
    [impl_->residency addAllocation:impl_->vertices];
    [impl_->residency addAllocation:impl_->indices];
    [impl_->residency addAllocation:impl_->water_vertices];
    [impl_->residency addAllocation:impl_->water_indices];
    if (impl_->ring_vertices != nil) {
        [impl_->residency addAllocation:impl_->ring_vertices];
    }
    if (impl_->ring_indices != nil) {
        [impl_->residency addAllocation:impl_->ring_indices];
    }
    for (FrameSlot& slot : impl_->slots) {
        [impl_->residency addAllocation:slot.uniforms];
        [impl_->residency addAllocation:slot.face_uniforms];
        [impl_->residency addAllocation:slot.pieces];
    }
    [impl_->residency addAllocation:impl_->minimap];
    [impl_->residency addAllocation:impl_->metal_map];
    [impl_->residency addAllocation:impl_->brdf_lut];
    [impl_->residency addAllocation:impl_->reflection];
    [impl_->residency addAllocation:impl_->reflection_depth];
    if (impl_->sky_cube != nil) {
        [impl_->residency addAllocation:impl_->sky_cube];
    }
    for (id<MTLTexture> extra : {impl_->foam_tex, impl_->wave_tex, impl_->caustics_tex}) {
        if (extra != nil) {
            [impl_->residency addAllocation:extra];
        }
    }
    if (impl_->water_tex != nil) {
        [impl_->residency addAllocation:impl_->water_tex];
    }
    for (int i = 0; i < kGroundSlots; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j) {
            seen = seen || impl_->ground[j] == impl_->ground[i];
        }
        if (i != 2 && impl_->ground[i] != nil && !seen) {
            [impl_->residency addAllocation:impl_->ground[i]];
        }
    }
    MTLTextureDescriptor* atlas_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:kHudAtlasW
                                                          height:kHudAtlasH
                                                       mipmapped:NO];
    atlas_desc.usage = MTLTextureUsageShaderRead;
    atlas_desc.storageMode = MTLStorageModeShared;
    std::vector<uint8_t> clear_atlas(static_cast<size_t>(kHudAtlasW * kHudAtlasH * 4), 0);
    for (FrameSlot& slot : impl_->slots) {
        slot.hud_atlas = [impl_->device newTextureWithDescriptor:atlas_desc];
        if (slot.hud_atlas == nil) {
            error = "hud atlas allocation failed";
            return false;
        }
        [slot.hud_atlas replaceRegion:MTLRegionMake2D(0, 0, kHudAtlasW, kHudAtlasH)
                          mipmapLevel:0
                            withBytes:clear_atlas.data()
                          bytesPerRow:kHudAtlasW * 4];
        [impl_->residency addAllocation:slot.hud_atlas];
    }
    [impl_->residency commit];
    [impl_->queue addResidencySet:impl_->residency];

    std::memcpy(impl_->sky, mesh.sky, sizeof(impl_->sky));
    std::memcpy(impl_->sun, mesh.sun, sizeof(impl_->sun));
    std::memcpy(impl_->ambient, mesh.ambient, sizeof(impl_->ambient));
    std::memcpy(impl_->diffuse, mesh.diffuse, sizeof(impl_->diffuse));
    impl_->ground_shadow_density = mesh.ground_shadow_density;
    impl_->unit_shadow_density = mesh.unit_shadow_density;
    impl_->height_min = mesh.min_height;
    impl_->height_max = mesh.max_height;
    return true;
}

bool TerrainRenderer::set_grass(const GrassData& grass, std::string& error) {
    if (impl_->device == nil || impl_->residency == nil || !grass.valid) {
        error = "grass: renderer not initialized or no grass data";
        return false;
    }
    impl_->grass_vertices =
        upload(impl_->device, grass.patch_vertices.data(), grass.patch_vertices.size() * sizeof(float));
    impl_->grass_indices =
        upload(impl_->device, grass.patch_indices.data(), grass.patch_indices.size() * sizeof(uint32_t));
    impl_->grass_instances = upload(impl_->device, grass.instances.data(), grass.instances.size() * sizeof(float));
    impl_->grass_blade = upload_image(impl_->device, grass.blade_tex);
    impl_->grass_perlin = upload_image(impl_->device, grass.perlin_tex);
    impl_->grass_height = upload_image(impl_->device, grass.heightmap);
    if (impl_->grass_vertices == nil || impl_->grass_indices == nil || impl_->grass_instances == nil ||
        impl_->grass_blade == nil || impl_->grass_perlin == nil || impl_->grass_height == nil) {
        error = "grass: GPU upload failed";
        impl_->grass_vertices = nil;
        return false;
    }
    for (id<MTLAllocation> a : std::initializer_list<id<MTLAllocation>>{
             impl_->grass_vertices, impl_->grass_indices, impl_->grass_instances, impl_->grass_blade,
             impl_->grass_perlin, impl_->grass_height}) {
        [impl_->residency addAllocation:a];
    }
    [impl_->residency commit];
    impl_->grass_index_count = grass.patch_indices.size();
    impl_->grass_tile_offset = grass.tile_offset;
    impl_->grass_tile_count = grass.tile_count;
    impl_->grass_cols = grass.grid_cols;
    impl_->grass_rows = grass.grid_rows;
    impl_->grass_tile_world = grass.tile_world;
    const float a4[4] = {grass.map_color_factor, grass.map_color_base, grass.alpha_threshold, grass.grass_brightness};
    const float b4[4] = {grass.wind_strength, grass.wind_scale, grass.wind_sample_scale, grass.shadow_factor};
    const float c4[4] = {grass.fade_start * grass.distance_mult, grass.fade_end * grass.distance_mult, 0.f, 0.f};
    std::memcpy(impl_->grass_a, a4, sizeof(a4));
    std::memcpy(impl_->grass_b, b4, sizeof(b4));
    std::memcpy(impl_->grass_c, c4, sizeof(c4));
    impl_->grass_wind[0] = grass.grass_wind_mult;
    impl_->grass_wind[1] = grass.max_wind_speed;
    impl_->grass_hm_w = grass.heightmap.width;
    impl_->grass_hm_h = grass.heightmap.height;
    impl_->grass_heights.resize(static_cast<size_t>(grass.heightmap.width) * grass.heightmap.height);
    std::memcpy(impl_->grass_heights.data(), grass.heightmap.levels[0].data(), impl_->grass_heights.size() * 4);
    return true;
}

bool TerrainRenderer::set_model(const ModelMesh& model, float color_r, float color_g, float color_b,
                                std::string& error) {
    if (impl_->device == nil || impl_->residency == nil) {
        error = "terrain renderer is not initialized";
        return false;
    }
    if (model.vertices.empty() || model.indices.empty()) {
        error = "commander model is empty";
        return false;
    }
    if (model.tex_size < 4 || (model.tex_size % 4) != 0) {
        error = "commander texture size is not a BC block multiple";
        return false;
    }
    const size_t bc3_bytes = static_cast<size_t>(model.tex_size / 4) * static_cast<size_t>(model.tex_size / 4) * 16;
    if (model.color_bc3.size() != bc3_bytes || model.other_bc3.size() != bc3_bytes) {
        error = "commander BC3 mip is the wrong size";
        return false;
    }

    auto drop = [&](id<MTLAllocation> resource) {
        if (resource != nil) {
            [impl_->residency removeAllocation:resource];
        }
    };
    drop(impl_->model_vertices);
    drop(impl_->model_indices);
    drop(impl_->model_color);
    drop(impl_->model_other);
    drop(impl_->model_normal);
    impl_->model_normal = nil;

    impl_->model_vertices = upload(impl_->device, model.vertices.data(), model.vertices.size() * sizeof(TerrainVertex));
    impl_->model_indices = upload(impl_->device, model.indices.data(), model.indices.size() * sizeof(uint32_t));
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC3_RGBA
                                                           width:static_cast<NSUInteger>(model.tex_size)
                                                          height:static_cast<NSUInteger>(model.tex_size)
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    impl_->model_color = [impl_->device newTextureWithDescriptor:desc];
    impl_->model_other = [impl_->device newTextureWithDescriptor:desc];
    if (impl_->model_vertices == nil || impl_->model_indices == nil || impl_->model_color == nil ||
        impl_->model_other == nil) {
        error = "commander buffer or BC3 texture allocation failed";
        return false;
    }
    const NSUInteger row_bytes = static_cast<NSUInteger>(model.tex_size / 4) * 16;
    const MTLRegion region = MTLRegionMake2D(0, 0, static_cast<NSUInteger>(model.tex_size),
                                              static_cast<NSUInteger>(model.tex_size));
    [impl_->model_color replaceRegion:region mipmapLevel:0 withBytes:model.color_bc3.data() bytesPerRow:row_bytes];
    [impl_->model_other replaceRegion:region mipmapLevel:0 withBytes:model.other_bc3.data() bytesPerRow:row_bytes];

    [impl_->residency addAllocation:impl_->model_vertices];
    [impl_->residency addAllocation:impl_->model_indices];
    [impl_->residency addAllocation:impl_->model_color];
    [impl_->residency addAllocation:impl_->model_other];
    if (model.normal_size >= 4 && model.normal_bc3.size() ==
            static_cast<size_t>(model.normal_size / 4) * static_cast<size_t>(model.normal_size / 4) * 16) {
        GpuImage normal;
        normal.format = GpuImage::Format::BC3;
        normal.width = model.normal_size;
        normal.height = model.normal_size;
        normal.levels.push_back(model.normal_bc3);
        impl_->model_normal = upload_image(impl_->device, normal);
        if (impl_->model_normal != nil) {
            [impl_->residency addAllocation:impl_->model_normal];
        }
    }
    [impl_->residency commit];
    impl_->model_index_count = model.indices.size();
    impl_->team_color[0] = color_r;
    impl_->team_color[1] = color_g;
    impl_->team_color[2] = color_b;
    return true;
}

void TerrainRenderer::set_pieces(const float* column_major, int count) {
    fill_piece_identities(impl_->piece_cpu);
    if (column_major != nullptr && count > 0) {
        const int slots = count > kPieceSlots ? kPieceSlots : count;
        std::memcpy(impl_->piece_cpu.data(), column_major, static_cast<size_t>(slots) * 16 * sizeof(float));
    }
}

void TerrainRenderer::set_pose(float x, float y, float z, float heading, float bank) {
    impl_->model_pose[0] = x;
    impl_->model_pose[1] = y;
    impl_->model_pose[2] = z;
    impl_->model_pose[3] = heading;
    impl_->bank = bank;
}

void TerrainRenderer::set_marker(float radius) {
    impl_->marker_radius = radius > 0.f ? radius : 0.f;
}

void TerrainRenderer::set_metal_hud(bool enabled) {
    impl_->metal_hud = enabled;
}

void TerrainRenderer::attach(void* metal_layer) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer;
    if (layer == nil) {
        return;
    }
    layer.device = impl_->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The water pass copies the drawable's opaque scene for refraction.
    layer.framebufferOnly = NO;
    // Three drawables match the in-flight slots. The default is already 3; set it so a
    // later OS default cannot collapse the pool back to one.
    layer.maximumDrawableCount = kInFlight;
}

bool TerrainRenderer::draw(const TerrainFrame& frame, std::string& error) {
    if (frame.view_proj == nullptr) {
        error = "terrain draw is missing a view matrix";
        return false;
    }
    CAMetalLayer* layer = (__bridge CAMetalLayer*)frame.metal_layer;
    if (layer == nil) {
        error = "missing CAMetalLayer";
        return false;
    }
    layer.device = impl_->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // The water pass copies the drawable's opaque scene for refraction.
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = kInFlight;

    const NSUInteger width = static_cast<NSUInteger>(layer.drawableSize.width);
    const NSUInteger height = static_cast<NSUInteger>(layer.drawableSize.height);
    if (width < 1 || height < 1) {
        error = "drawable size is empty";
        return false;
    }
    const bool shot = frame.screenshot_ppm != nullptr && frame.screenshot_ppm[0] != '\0';
    if (impl_->target_width != width || impl_->target_height != height) {
        // In-flight frames still name the old depth texture. Drain them before replacing it.
        if (!finish_gpu(error)) {
            return false;
        }
    }
    FrameSlot& slot = impl_->slots[impl_->slot_index];
    if (slot.pending) {
        if (slot.ticket != 0 && ![impl_->fence waitUntilSignaledValue:slot.ticket timeoutMS:5000]) {
            error = "gpu frame timed out";
            return false;
        }
        retire_slot(impl_->residency, slot);
        // The drawable stays checked out until this removal is committed. Doing it
        // after nextDrawable left the pool empty and blocked the present thread.
        [impl_->residency commit];
    }
    impl_->active = &slot;

    TerrainUniforms uniforms{};
    std::memcpy(uniforms.view_proj, frame.view_proj, sizeof(uniforms.view_proj));
    uniforms.sun[0] = impl_->sun[0];
    uniforms.sun[1] = impl_->sun[1];
    uniforms.sun[2] = impl_->sun[2];
    uniforms.ambient[0] = impl_->ambient[0];
    uniforms.ambient[1] = impl_->ambient[1];
    uniforms.ambient[2] = impl_->ambient[2];
    uniforms.diffuse[0] = impl_->diffuse[0];
    uniforms.diffuse[1] = impl_->diffuse[1];
    uniforms.diffuse[2] = impl_->diffuse[2];
    uniforms.sky[0] = impl_->sky[0];
    uniforms.sky[1] = impl_->sky[1];
    uniforms.sky[2] = impl_->sky[2];
    uniforms.team[0] = impl_->team_color[0];
    uniforms.team[1] = impl_->team_color[1];
    uniforms.team[2] = impl_->team_color[2];
    uniforms.team[3] = 1;
    uniforms.model_pose[0] = impl_->model_pose[0];
    uniforms.model_pose[1] = impl_->model_pose[1];
    uniforms.model_pose[2] = impl_->model_pose[2];
    uniforms.model_pose[3] = impl_->model_pose[3];
    uniforms.marker[0] = impl_->marker_radius;
    uniforms.marker[1] = impl_->bank;
    const bool shadows = impl_->shadow_map != nil &&
                         fit_shadow(frame.view_proj, impl_->sun, impl_->height_min, impl_->height_max, uniforms.shadow_vp);
    uniforms.shadow[0] = impl_->ground_shadow_density;
    uniforms.shadow[1] = impl_->unit_shadow_density;
    uniforms.shadow[2] = shadows ? 1.f : 0.f;
    if (frame.eye != nullptr) {
        uniforms.eye[0] = frame.eye[0];
        uniforms.eye[1] = frame.eye[1];
        uniforms.eye[2] = frame.eye[2];
    }
    std::memcpy(uniforms.ground_specular, impl_->ground_specular, sizeof(uniforms.ground_specular));
    std::memcpy(uniforms.splat_scales, impl_->splat_scales, sizeof(uniforms.splat_scales));
    std::memcpy(uniforms.splat_mults, impl_->splat_mults, sizeof(uniforms.splat_mults));
    std::memcpy(uniforms.smf_flags, impl_->smf_flags, sizeof(uniforms.smf_flags));
    std::memcpy(uniforms.map, impl_->map, sizeof(uniforms.map));
    std::memcpy(uniforms.water_base, impl_->water_base, sizeof(uniforms.water_base));
    std::memcpy(uniforms.water_min, impl_->water_min, sizeof(uniforms.water_min));
    std::memcpy(uniforms.water_absorb, impl_->water_absorb, sizeof(uniforms.water_absorb));
    std::memcpy(uniforms.unit_ambient, impl_->unit_ambient, sizeof(uniforms.unit_ambient));
    std::memcpy(uniforms.unit_diffuse, impl_->unit_diffuse, sizeof(uniforms.unit_diffuse));
    std::memcpy(uniforms.unit_specular, impl_->unit_specular, sizeof(uniforms.unit_specular));
    // Only unit 0 is drawn; its sim id is 0.
    uniforms.unit_frame[0] = static_cast<float>(frame.sim_frame);
    uniforms.unit_frame[1] = 0.f;
    uniforms.unit_frame[2] = impl_->model_normal != nil ? 1.f : 0.f;
    std::memcpy(uniforms.plane_color, impl_->plane_color, sizeof(uniforms.plane_color));
    std::memcpy(uniforms.water_repeat, impl_->water_repeat, sizeof(uniforms.water_repeat));
    std::memcpy(uniforms.water_fresnel, impl_->water_fresnel, sizeof(uniforms.water_fresnel));
    std::memcpy(uniforms.water_waves, impl_->water_waves, sizeof(uniforms.water_waves));
    std::memcpy(uniforms.water_light, impl_->water_light, sizeof(uniforms.water_light));
    std::memcpy(uniforms.water_specular, impl_->water_specular, sizeof(uniforms.water_specular));
    std::memcpy(uniforms.water_surface, impl_->water_surface, sizeof(uniforms.water_surface));
    // Waves follow sim time so scripted runs are deterministic.
    uniforms.water_surface[3] = static_cast<float>(frame.sim_frame) / 30.f;
    std::memcpy(uniforms.water_shore, impl_->water_shore, sizeof(uniforms.water_shore));
    std::memcpy(uniforms.water_extra, impl_->water_extra, sizeof(uniforms.water_extra));
    uniforms.water_extra[2] = impl_->foam_tex != nil ? 1.f : 0.f;
    uniforms.water_extra[3] = impl_->reflection != nil ? 1.f : 0.f;
    std::memcpy(uniforms.grass_a, impl_->grass_a, sizeof(uniforms.grass_a));
    std::memcpy(uniforms.grass_b, impl_->grass_b, sizeof(uniforms.grass_b));
    std::memcpy(uniforms.grass_c, impl_->grass_c, sizeof(uniforms.grass_c));
    {
        // getWindSpeed: bar-metal's Spring.GetWind direction is (0, 0), so the wind offsets stay 0
        // and windStrength = min(maxWindSpeed, max(4, |x| + |z|)) = 4.
        const float wind_x = 0.f, wind_z = 0.f;
        uniforms.grass_u[0] = 0.f;
        uniforms.grass_u[1] = 0.f;
        uniforms.grass_u[2] = std::min(impl_->grass_wind[1], std::max(4.f, std::fabs(wind_x) + std::fabs(wind_z)));
        // globalgrassfade from camera height over the ground (the widget smooths it over wall
        // time; bar-metal applies the target value).
        float fade = 1.f;
        if (frame.eye != nullptr && impl_->grass_c[1] > impl_->grass_c[0]) {
            const float cam_height = frame.eye[1] - std::max(0.f, impl_->ground_height_at(frame.eye[0], frame.eye[2]));
            fade = std::clamp((impl_->grass_c[1] - cam_height) / (impl_->grass_c[1] - impl_->grass_c[0]), 0.f, 1.f);
        }
        uniforms.grass_u[3] = fade;
    }
    uniforms.water_screen[0] = 1.f / static_cast<float>(width);
    uniforms.water_screen[1] = 1.f / static_cast<float>(height);
    if (!invert4(uniforms.view_proj, uniforms.sky_inv)) {
        std::memset(uniforms.sky_inv, 0, sizeof(uniforms.sky_inv));
    }
    std::memcpy(slot.uniforms.contents, &uniforms, sizeof(uniforms));
    // Reflection faces: Metal cube face (right, up, forward) per the cube-map face table
    // (s along right, t down the face). Projection: 90 degree square, Metal depth 0..1.
    bool reflection_faces[6] = {false, false, false, false, false, false};
    if (frame.eye != nullptr) {
        static const float kFaces[6][3][3] = {
            {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},   // +X
            {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},   // -X
            {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},   // +Y
            {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},   // -Y
            {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},    // +Z
            {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},  // -Z
        };
        const float near_plane = 1.f;
        const float far_plane = 2.f * std::sqrt(impl_->map[0] * impl_->map[0] + impl_->map[1] * impl_->map[1]) + 8192.f;
        const float a = far_plane / (far_plane - near_plane);
        const float b = -far_plane * near_plane / (far_plane - near_plane);
        auto* faces = static_cast<TerrainUniforms*>(slot.face_uniforms.contents);
        for (int face = 0; face < 6; ++face) {
            const float* r = kFaces[face][0];
            const float* u = kFaces[face][1];
            const float* f = kFaces[face][2];
            const float* e = frame.eye;
            const float re = r[0] * e[0] + r[1] * e[1] + r[2] * e[2];
            const float ue = u[0] * e[0] + u[1] * e[1] + u[2] * e[2];
            const float fe = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
            const float rows[4][4] = {
                {r[0], r[1], r[2], -re},
                {u[0], u[1], u[2], -ue},
                {a * f[0], a * f[1], a * f[2], -a * fe + b},
                {f[0], f[1], f[2], -fe},
            };
            TerrainUniforms face_u = uniforms;
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    face_u.view_proj[col * 4 + row] = rows[row][col];
                }
            }
            if (!invert4(face_u.view_proj, face_u.sky_inv)) {
                std::memset(face_u.sky_inv, 0, sizeof(face_u.sky_inv));
            }
            faces[face] = face_u;
        }
        if (!impl_->reflection_primed) {
            for (bool& face : reflection_faces) {
                face = true;
            }
            impl_->reflection_primed = true;
        } else {
            reflection_faces[impl_->reflection_face] = true;
            impl_->reflection_face = (impl_->reflection_face + 1) % 6;
        }
    }
    if (slot.pieces != nil && impl_->piece_cpu.size() * sizeof(float) == slot.pieces.length) {
        std::memcpy(slot.pieces.contents, impl_->piece_cpu.data(), slot.pieces.length);
    }

    if (impl_->target_width != width || impl_->target_height != height) {
        if (impl_->depth != nil) {
            [impl_->residency removeAllocation:impl_->depth];
        }
        if (impl_->offscreen != nil) {
            [impl_->residency removeAllocation:impl_->offscreen];
            impl_->offscreen = nil;
        }
        if (impl_->readback != nil) {
            [impl_->residency removeAllocation:impl_->readback];
            impl_->readback = nil;
        }
        MTLTextureDescriptor* depth_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        depth_desc.usage = MTLTextureUsageRenderTarget;
        depth_desc.storageMode = MTLStorageModePrivate;
        impl_->depth = [impl_->device newTextureWithDescriptor:depth_desc];
        if (impl_->depth == nil) {
            error = "depth texture allocation failed";
            return false;
        }
        [impl_->residency addAllocation:impl_->depth];
        if (impl_->refract_color != nil) {
            [impl_->residency removeAllocation:impl_->refract_color];
            [impl_->residency removeAllocation:impl_->refract_depth];
        }
        MTLTextureDescriptor* refract_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        refract_desc.usage = MTLTextureUsageShaderRead;
        refract_desc.storageMode = MTLStorageModePrivate;
        impl_->refract_color = [impl_->device newTextureWithDescriptor:refract_desc];
        MTLTextureDescriptor* refract_depth_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        refract_depth_desc.usage = MTLTextureUsageShaderRead;
        refract_depth_desc.storageMode = MTLStorageModePrivate;
        impl_->refract_depth = [impl_->device newTextureWithDescriptor:refract_depth_desc];
        if (impl_->refract_color == nil || impl_->refract_depth == nil) {
            error = "water refraction texture allocation failed";
            return false;
        }
        [impl_->residency addAllocation:impl_->refract_color];
        [impl_->residency addAllocation:impl_->refract_depth];
        impl_->target_width = width;
        impl_->target_height = height;
    }
    if (shot && impl_->offscreen == nil) {
        MTLTextureDescriptor* color_desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        color_desc.usage = MTLTextureUsageRenderTarget;
        color_desc.storageMode = MTLStorageModePrivate;
        impl_->offscreen = [impl_->device newTextureWithDescriptor:color_desc];
        const NSUInteger bytes_per_row = (width * 4 + 255u) & ~255u;
        impl_->readback = [impl_->device newBufferWithLength:bytes_per_row * height
                                                     options:MTLResourceStorageModeShared];
        if (impl_->offscreen == nil || impl_->readback == nil) {
            error = "screenshot target allocation failed";
            return false;
        }
        [impl_->residency addAllocation:impl_->offscreen];
        [impl_->residency addAllocation:impl_->readback];
    }

    if (!prepare_hud(frame.hud, error)) {
        return false;
    }
    // Acquire the drawable after the CPU prep so Core Text does not hold a pooled texture.
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) {
        error = "CAMetalLayer nextDrawable returned nil";
        return false;
    }
    if (drawable.texture.width != width || drawable.texture.height != height) {
        error = "drawable size changed during the frame";
        return false;
    }
    bool drawable_known = false;
    for (id<MTLTexture> kept : impl_->live_drawables) {
        if (kept == drawable.texture) {
            drawable_known = true;
            break;
        }
    }
    if (!drawable_known) {
        [impl_->residency addAllocation:drawable.texture];
        impl_->live_drawables.push_back(drawable.texture);
    }
    [impl_->residency commit];

    const MTLClearColor clear = MTLClearColorMake(impl_->sky[0], impl_->sky[1], impl_->sky[2], 1);
    SceneBindings scene;
    scene.arguments = impl_->arguments;
    scene.vertices = impl_->vertices;
    scene.indices = impl_->indices;
    scene.water_vertices = impl_->water_vertices;
    scene.water_indices = impl_->water_indices;
    scene.ring_vertices = impl_->ring_vertices;
    scene.ring_indices = impl_->ring_indices;
    scene.model_vertices = impl_->model_vertices;
    scene.model_indices = impl_->model_indices;
    scene.terrain_pipeline = impl_->terrain_pipeline;
    scene.model_pipeline = impl_->model_pipeline;
    scene.water_pipeline = impl_->water_pipeline;
    scene.ring_pipeline = impl_->ring_pipeline;
    scene.terrain_depth = impl_->terrain_depth;
    scene.water_depth = impl_->water_depth;
    scene.minimap = impl_->minimap;
    scene.metal_map = impl_->metal_map;
    scene.model_color = impl_->model_color;
    scene.model_other = impl_->model_other;
    scene.model_normal = impl_->model_normal;
    scene.brdf_lut = impl_->brdf_lut;
    scene.sky_cube = impl_->sky_cube;
    scene.reflection = impl_->reflection;
    scene.sky_pipeline = impl_->sky_pipeline;
    scene.sky_depth = impl_->sky_depth;
    scene.water_tex = impl_->water_tex;
    scene.refract_color = impl_->refract_color;
    scene.refract_depth = impl_->refract_depth;
    scene.foam_tex = impl_->foam_tex;
    scene.wave_tex = impl_->wave_tex;
    scene.caustics_tex = impl_->caustics_tex;
    scene.grass_pipeline = impl_->grass_vertices != nil ? impl_->grass_pipeline : nil;
    scene.grass_depth = impl_->grass_depth;
    scene.grass_vertices = impl_->grass_vertices;
    scene.grass_indices = impl_->grass_indices;
    scene.grass_instances = impl_->grass_instances;
    scene.grass_blade = impl_->grass_blade;
    scene.grass_perlin = impl_->grass_perlin;
    scene.grass_height = impl_->grass_height;
    scene.grass_index_count = impl_->grass_index_count;
    if (impl_->grass_vertices != nil && frame.eye != nullptr && frame.view_proj != nullptr) {
        impl_->cull_grass(frame.view_proj, frame.eye, scene.grass_runs);
    }
    scene.water_visible = impl_->map[2] > 0.5f;
    scene.pieces = slot.pieces;
    scene.shadow_map = impl_->shadow_map;
    scene.ground = impl_->ground;
    scene.shadow_terrain_pipeline = impl_->shadow_terrain_pipeline;
    scene.shadow_model_pipeline = impl_->shadow_model_pipeline;
    scene.index_count = impl_->index_count;
    scene.water_index_count = impl_->water_index_count;
    scene.ring_index_count = impl_->ring_index_count;
    scene.model_index_count = impl_->model_index_count;
    scene.metal_hud = impl_->metal_hud;
    [impl_->arguments setAddress:slot.uniforms.gpuAddress atIndex:0];
    // nextDrawable already waits until the display has released this texture. A queue
    // wait here stalls later presents behind the scanout and starves the drawable pool.
    [slot.command_buffer beginCommandBufferWithAllocator:slot.allocator];
    [slot.command_buffer useResidencySet:impl_->residency];
    encode_shadow(slot.command_buffer, scene);
    encode_reflection(slot.command_buffer, scene, impl_->reflection, impl_->reflection_depth, slot.face_uniforms,
                      slot.uniforms, reflection_faces, clear);
    encode_scene(slot.command_buffer, drawable.texture, impl_->depth, scene, clear);
    encode_hud((__bridge void*)slot.command_buffer, (__bridge void*)drawable.texture);
    if (shot) {
        encode_scene(slot.command_buffer, impl_->offscreen, impl_->depth, scene, clear);
        encode_hud((__bridge void*)slot.command_buffer, (__bridge void*)impl_->offscreen);
        id<MTL4ComputeCommandEncoder> copy = [slot.command_buffer computeCommandEncoder];
        const NSUInteger bytes_per_row = (width * 4 + 255u) & ~255u;
        [copy copyFromTexture:impl_->offscreen
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:impl_->readback
            destinationOffset:0
       destinationBytesPerRow:bytes_per_row
     destinationBytesPerImage:0];
        [copy endEncoding];
    }
    [slot.command_buffer endCommandBuffer];

    id<MTL4CommandBuffer> buffers[] = {slot.command_buffer};
    [impl_->queue commit:buffers count:1];
    [impl_->queue signalDrawable:drawable];
    [drawable present];

    impl_->ticket += 1;
    [impl_->queue signalEvent:impl_->fence value:impl_->ticket];
    // Release the drawable object before the next nextDrawable. The pool is only three
    // deep; keeping the object alive holds the texture out of that pool. The texture
    // itself stays in the residency set for the rest of the process.
    slot.ticket = impl_->ticket;
    slot.pending = true;
    impl_->slot_index = (impl_->slot_index + 1) % kInFlight;

    if (shot) {
        if (![impl_->fence waitUntilSignaledValue:slot.ticket timeoutMS:5000]) {
            error = "gpu frame timed out";
            return false;
        }
    }

    if (shot) {
        const NSUInteger bytes_per_row = (width * 4 + 255u) & ~255u;
        const uint8_t* pixels = static_cast<const uint8_t*>(impl_->readback.contents);
        if (!write_ppm(frame.screenshot_ppm, pixels, width, height, bytes_per_row, error)) {
            return false;
        }
    }
    return true;
}

bool check_hud_text(std::string& error) {
    // Samples that used to SIGSEGV: Spring \255RGB mid-string, and non-UTF8 bytes.
    const unsigned char colored[] = {
        'H', 'P', ':', ' ', 255, 255, 100, 100, '5', '0', '0', 8, ' ', 'o', 'k', 0,
    };
    const unsigned char junk[] = {0xff, 0xfe, 'x', 0x80, 'y', 0};
    struct Case {
        const char* label;
        std::string text;
        float r, g, b;
    };
    const Case cases[] = {
        {"leading-color", std::string("\xff\xff\xff\x00Metal", 9), 1.f, 1.f, 1.f},
        {"mid-color", std::string(reinterpret_cast<const char*>(colored)), 1.f, 1.f, 1.f},
        {"invalid-utf8", std::string(reinterpret_cast<const char*>(junk)), 1.f, 0.8f, 0.2f},
        {"empty-ok", std::string("plain"), 0.2f, 0.9f, 0.3f},
    };
    for (const Case& c : cases) {
        HudText text;
        text.text = c.text;
        text.x = 10.f;
        text.y = 10.f;
        text.size = 18.f;
        text.r = c.r;
        text.g = c.g;
        text.b = c.b;
        text.a = 1.f;
        text.outline = true;
        GlyphImage image;
        if (!raster_glyph(text, image)) {
            error = std::string("hud text raster failed: ") + c.label;
            return false;
        }
        if (image.width < 1 || image.height < 1 || image.pixels.empty()) {
            error = std::string("hud text empty bitmap: ") + c.label;
            return false;
        }
        // Colored / plain rasters must leave visible premul coverage (regression: invisible glyphs).
        size_t lit = 0;
        for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
            if (image.pixels[i + 3] > 8) {
                ++lit;
            }
        }
        if (lit < 4) {
            error = std::string("hud text invisible bitmap: ") + c.label;
            return false;
        }
    }
    // Small outlined income-style glyphs must stay crisp: solid black outline + colored fill,
    // not same-color blobs from mid-string runs tinting the outline pass.
    {
        HudText income;
        const unsigned char raw[] = {255, 120, 235, 120, '+', '2'};
        income.text.assign(reinterpret_cast<const char*>(raw), sizeof(raw));
        income.x = 0;
        income.y = 0;
        income.size = 9.f;
        income.r = 1.f;
        income.g = 1.f;
        income.b = 1.f;
        income.a = 1.f;
        income.outline = true;
        GlyphImage image;
        if (!raster_glyph(income, image)) {
            error = "hud text income raster failed";
            return false;
        }
        size_t lit = 0;
        size_t greenish = 0;
        size_t dark = 0;
        for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
            const uint8_t b = image.pixels[i];
            const uint8_t gch = image.pixels[i + 1];
            const uint8_t r = image.pixels[i + 2];
            const uint8_t a = image.pixels[i + 3];
            if (a < 20) {
                continue;
            }
            ++lit;
            if (gch > r + 20 && gch > b + 20) {
                ++greenish;
            }
            if (r < 40 && gch < 40 && b < 40) {
                ++dark;
            }
        }
        if (lit < 8) {
            error = "hud text income invisible";
            return false;
        }
        if (dark * 4 < lit) {
            error = "hud text income outline missing (blob)";
            return false;
        }
        if (greenish < 2) {
            error = "hud text income fill not green";
            return false;
        }
    }
    return true;
}

float hud_measure_width(const std::string& text, const std::string& font_path, float size) {
    if (text.empty()) {
        return 0.f;
    }
    const float px = size < 4.f ? 4.f : (size > 256.f ? 256.f : size);
    CTFontRef font = hud_font(font_path, px);
    if (font == nullptr) {
        return 0.f;
    }
    // Advance is independent of color; white base is fine.
    CTLineRef line = hud_line(font, text, 1.f, 1.f, 1.f, 1.f);
    CFRelease(font);
    if (line == nullptr) {
        return 0.f;
    }
    CGFloat ascent = 0;
    CGFloat descent = 0;
    CGFloat leading = 0;
    const double advance = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CFRelease(line);
    return static_cast<float>(advance);
}


float hud_line_height(const std::string& font_path, float size) {
    const float px = size < 4.f ? 4.f : (size > 256.f ? 256.f : size);
    CTFontRef font = hud_font(font_path, px);
    if (font == nullptr) {
        return size;
    }
    const float h = static_cast<float>(CTFontGetAscent(font) + CTFontGetDescent(font) + CTFontGetLeading(font));
    CFRelease(font);
    return h * (size / px);
}

}  // namespace bar
