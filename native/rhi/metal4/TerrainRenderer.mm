#include "terrain_renderer.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
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
    id<MTLBuffer> pieces = nil;
    id<MTLTexture> hud_atlas = nil;
    id<MTLBuffer> hud_color = nil;
    id<MTLBuffer> hud_text_verts = nil;
    id<MTLBuffer> hud_minimap = nil;
    NSUInteger hud_color_count = 0;
    NSUInteger hud_text_count = 0;
    NSUInteger hud_minimap_count = 0;
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
};

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
    slot.hud_color_count = 0;
    slot.hud_text_count = 0;
    slot.hud_minimap_count = 0;
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
    id<MTLBuffer> pieces;
    NSUInteger index_count;
    NSUInteger water_index_count;
    NSUInteger ring_index_count;
    NSUInteger model_index_count;
    bool metal_hud = false;
};

void encode_scene(id<MTL4CommandBuffer> command_buffer, id<MTLTexture> target, id<MTLTexture> depth,
                  const SceneBindings& scene, MTLClearColor clear) {
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clear;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 1.0;

    id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    MTLViewport viewport{0, 0, static_cast<double>(target.width), static_cast<double>(target.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    [encoder setArgumentTable:scene.arguments atStages:MTLRenderStageVertex | MTLRenderStageFragment];

    // Model draws reuse texture slots 0 and 1, so terrain binds its own maps first.
    [scene.arguments setTexture:scene.minimap.gpuResourceID atIndex:0];
    [scene.arguments setTexture:scene.metal_map.gpuResourceID atIndex:1];
    [scene.arguments setAddress:scene.vertices.gpuAddress atIndex:1];
    [encoder setRenderPipelineState:scene.terrain_pipeline];
    [encoder setDepthStencilState:scene.terrain_depth];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:scene.index_count
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:scene.indices.gpuAddress
                  indexBufferLength:scene.indices.length];

    if (scene.model_index_count > 0 && scene.model_vertices != nil && scene.model_indices != nil &&
        scene.model_color != nil && scene.model_other != nil && scene.model_pipeline != nil && scene.pieces != nil) {
        [scene.arguments setTexture:scene.model_color.gpuResourceID atIndex:0];
        [scene.arguments setTexture:scene.model_other.gpuResourceID atIndex:1];
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

    [scene.arguments setAddress:scene.water_vertices.gpuAddress atIndex:1];
    [encoder setRenderPipelineState:scene.water_pipeline];
    [encoder setDepthStencilState:scene.water_depth];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:scene.water_index_count
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:scene.water_indices.gpuAddress
                  indexBufferLength:scene.water_indices.length];

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

CTLineRef hud_line(CTFontRef font, const std::string& text, float r, float g, float b, float a) {
    CFStringRef chars = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                                 static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
    CGColorRef color = CGColorCreateGenericRGB(r, g, b, a);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* values[] = {font, color};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef run = CFAttributedStringCreate(kCFAllocatorDefault, chars, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(run);
    CFRelease(run);
    CFRelease(attrs);
    CGColorRelease(color);
    CFRelease(chars);
    return line;
}

constexpr int kHudAtlasW = 512;
constexpr int kHudAtlasH = 256;

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
    const float pad = text.outline ? 2.f : 1.f;
    CTFontRef font = hud_font(text.font_path, size);
    if (font == nullptr) {
        return false;
    }
    CTLineRef fill = hud_line(font, text.text, text.r, text.g, text.b, text.a);
    CTLineRef outline = text.outline ? hud_line(font, text.text, 0.f, 0.f, 0.f, text.a) : nullptr;
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
    const int width = static_cast<int>(std::ceil(advance + static_cast<double>(pad) * 2.0));
    const int height = static_cast<int>(std::ceil(static_cast<double>(ascent + descent) + static_cast<double>(pad) * 2.0));
    if (width < 1 || height < 1 || width > 2048 || height > 512) {
        CFRelease(fill);
        if (outline != nullptr) {
            CFRelease(outline);
        }
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 4), 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(pixels.data(), static_cast<size_t>(width), static_cast<size_t>(height),
                                                  8, static_cast<size_t>(width * 4), space,
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
    const CGFloat baseline = pad + descent;
    if (outline != nullptr) {
        const CGFloat offsets[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const CGFloat* offset : offsets) {
            CGContextSetTextPosition(context, pad + offset[0], baseline + offset[1]);
            CTLineDraw(outline, context);
        }
        CFRelease(outline);
    }
    CGContextSetTextPosition(context, pad, baseline);
    CTLineDraw(fill, context);
    CFRelease(fill);
    CGContextRelease(context);

    cached.pixels = std::move(pixels);
    cached.width = width;
    cached.height = height;
    cached.advance = static_cast<float>(advance);
    cached.descent = static_cast<float>(descent);
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
    slot->hud_color_count = 0;
    slot->hud_text_count = 0;
    slot->hud_minimap_count = 0;
}

bool TerrainRenderer::prepare_hud(const HudList* list, std::string& error) {
    release_hud();
    if (list == nullptr || (list->vertices.empty() && list->texts.empty())) {
        return true;
    }
    const float view_w = list->width > 0 ? static_cast<float>(list->width) : 1280.f;
    const float view_h = list->height > 0 ? static_cast<float>(list->height) : 720.f;
    std::vector<HudGpuVertex> color;
    color.reserve(list->vertices.size() / 4 * 6);
    for (size_t i = 0; i + 3 < list->vertices.size(); i += 4) {
        HudGpuVertex corners[4];
        for (int corner = 0; corner < 4; ++corner) {
            const HudVertex& vert = list->vertices[i + static_cast<size_t>(corner)];
            corners[corner] = hud_vert(vert.x, vert.y, 0, 0, vert.r, vert.g, vert.b, vert.a, view_w, view_h);
        }
        append_triangle(color, corners[0], corners[1], corners[2]);
        append_triangle(color, corners[0], corners[2], corners[3]);
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
    if (!color.empty()) {
        slot->hud_color = upload(impl_->device, color.data(), color.size() * sizeof(HudGpuVertex));
        if (slot->hud_color == nil) {
            error = "hud color buffer allocation failed";
            return false;
        }
        slot->hud_color_count = color.size();
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
        (slot->hud_color_count == 0 && slot->hud_text_count == 0 && slot->hud_minimap_count == 0)) {
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
    if (slot->hud_minimap_count > 0 && slot->hud_minimap != nil && impl_->hud_text_pipeline != nil &&
        impl_->minimap != nil) {
        [impl_->arguments setTexture:impl_->minimap.gpuResourceID atIndex:0];
        [impl_->arguments setAddress:slot->hud_minimap.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:impl_->hud_text_pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:slot->hud_minimap_count];
    }
    if (slot->hud_color_count > 0 && slot->hud_color != nil && impl_->hud_color_pipeline != nil) {
        [impl_->arguments setAddress:slot->hud_color.gpuAddress atIndex:1];
        [encoder setRenderPipelineState:impl_->hud_color_pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:slot->hud_color_count];
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
    impl_->model_pipeline = make_pipeline(compiler, library, @"model_vs", @"model_fs", false, &ns_error_obj);
    if (impl_->model_pipeline == nil) {
        error = ns_error(ns_error_obj, "model pipeline creation failed");
        return false;
    }
    impl_->ring_pipeline = make_pipeline(compiler, library, @"terrain_vs", @"ring_fs", true, &ns_error_obj);
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
    table_desc.maxTextureBindCount = 2;
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
        [impl_->residency addAllocation:slot.pieces];
    }
    [impl_->residency addAllocation:impl_->minimap];
    [impl_->residency addAllocation:impl_->metal_map];
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
    std::memcpy(slot.uniforms.contents, &uniforms, sizeof(uniforms));
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
    scene.pieces = slot.pieces;
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

}  // namespace bar
