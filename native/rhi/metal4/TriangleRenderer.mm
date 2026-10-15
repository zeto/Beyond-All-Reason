#include "triangle_renderer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <fstream>
#include <vector>

namespace bar {

struct TriangleRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTL4CommandQueue> queue = nil;
    id<MTL4CommandAllocator> allocator = nil;
    id<MTL4CommandBuffer> command_buffer = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLSharedEvent> fence = nil;
    id<MTLResidencySet> residency = nil;
    id<MTLTexture> offscreen = nil;
    id<MTLBuffer> readback = nil;
    uint64_t ticket = 0;
    NSUInteger offscreen_width = 0;
    NSUInteger offscreen_height = 0;
};

namespace {

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

void encode_triangle(id<MTL4CommandBuffer> command_buffer, id<MTLTexture> target,
                     id<MTLRenderPipelineState> pipeline, MTLClearColor clear) {
    MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clear;

    id<MTL4RenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    MTLViewport viewport{0, 0, static_cast<double>(target.width), static_cast<double>(target.height), 0, 1};
    [encoder setViewport:viewport];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder barrierAfterStages:MTLStageFragment
              beforeQueueStages:MTLStageBlit
              visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder endEncoding];
}

}  // namespace

TriangleRenderer::TriangleRenderer() : impl_(new Impl) {}

TriangleRenderer::~TriangleRenderer() {
    delete impl_;
}

bool TriangleRenderer::init(const std::string& shader_path, std::string& error) {
    std::string source = read_file(shader_path, error);
    if (source.empty()) {
        return false;
    }

    impl_->device = MTLCreateSystemDefaultDevice();
    if (impl_->device == nil) {
        error = "MTLCreateSystemDefaultDevice returned nil";
        return false;
    }

    NSError* ns_error_obj = nil;
    MTL4CompilerDescriptor* compiler_desc = [MTL4CompilerDescriptor new];
    id<MTL4Compiler> compiler = [impl_->device newCompilerWithDescriptor:compiler_desc error:&ns_error_obj];
    if (compiler == nil) {
        error = ns_error(ns_error_obj, "Metal 4 compiler creation failed");
        return false;
    }

    MTL4LibraryDescriptor* library_desc = [MTL4LibraryDescriptor new];
    library_desc.name = @"triangle";
    library_desc.source = [NSString stringWithUTF8String:source.c_str()];
    id<MTLLibrary> library = [compiler newLibraryWithDescriptor:library_desc error:&ns_error_obj];
    if (library == nil) {
        error = ns_error(ns_error_obj, "shader compile failed");
        return false;
    }

    MTL4LibraryFunctionDescriptor* vertex = [MTL4LibraryFunctionDescriptor new];
    vertex.library = library;
    vertex.name = @"triangle_vs";
    MTL4LibraryFunctionDescriptor* fragment = [MTL4LibraryFunctionDescriptor new];
    fragment.library = library;
    fragment.name = @"triangle_fs";

    MTL4RenderPipelineDescriptor* pipeline_desc = [MTL4RenderPipelineDescriptor new];
    pipeline_desc.label = @"triangle";
    pipeline_desc.vertexFunctionDescriptor = vertex;
    pipeline_desc.fragmentFunctionDescriptor = fragment;
    pipeline_desc.rasterSampleCount = 1;
    pipeline_desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    impl_->pipeline = [compiler newRenderPipelineStateWithDescriptor:pipeline_desc
                                                 compilerTaskOptions:nil
                                                               error:&ns_error_obj];
    if (impl_->pipeline == nil) {
        error = ns_error(ns_error_obj, "pipeline creation failed");
        return false;
    }

    impl_->queue = [impl_->device newMTL4CommandQueue];
    if (impl_->queue == nil) {
        error = "newMTL4CommandQueue returned nil";
        return false;
    }
    impl_->allocator = [impl_->device newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new]
                                                                  error:&ns_error_obj];
    if (impl_->allocator == nil) {
        error = ns_error(ns_error_obj, "command allocator creation failed");
        return false;
    }
    impl_->command_buffer = [impl_->device newCommandBuffer];
    if (impl_->command_buffer == nil) {
        error = "newCommandBuffer returned nil";
        return false;
    }
    impl_->fence = [impl_->device newSharedEvent];
    if (impl_->fence == nil) {
        error = "newSharedEvent returned nil";
        return false;
    }

    impl_->residency = [impl_->device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new]
                                                              error:&ns_error_obj];
    if (impl_->residency == nil) {
        error = ns_error(ns_error_obj, "residency set creation failed");
        return false;
    }
    [impl_->residency commit];
    [impl_->queue addResidencySet:impl_->residency];
    return true;
}

bool TriangleRenderer::draw(const FrameRequest& request, std::string& error) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)request.metal_layer;
    if (layer == nil) {
        error = "missing CAMetalLayer";
        return false;
    }
    layer.device = impl_->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) {
        error = "CAMetalLayer nextDrawable returned nil";
        return false;
    }

    const NSUInteger width = drawable.texture.width;
    const NSUInteger height = drawable.texture.height;
    const bool shot = request.screenshot_ppm != nullptr && request.screenshot_ppm[0] != '\0';
    const MTLClearColor clear = MTLClearColorMake(0.08, 0.09, 0.12, 1.0);

    [impl_->residency addAllocation:drawable.texture];

    if (shot && (impl_->offscreen_width != width || impl_->offscreen_height != height)) {
        if (impl_->offscreen != nil) {
            [impl_->residency removeAllocation:impl_->offscreen];
        }
        if (impl_->readback != nil) {
            [impl_->residency removeAllocation:impl_->readback];
        }
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget;
        desc.storageMode = MTLStorageModePrivate;
        impl_->offscreen = [impl_->device newTextureWithDescriptor:desc];
        const NSUInteger bytes_per_row = (width * 4 + 255u) & ~255u;
        impl_->readback = [impl_->device newBufferWithLength:bytes_per_row * height
                                                     options:MTLResourceStorageModeShared];
        [impl_->residency addAllocation:impl_->offscreen];
        [impl_->residency addAllocation:impl_->readback];
        impl_->offscreen_width = width;
        impl_->offscreen_height = height;
    }
    [impl_->residency commit];

    [impl_->queue waitForDrawable:drawable];
    [impl_->command_buffer beginCommandBufferWithAllocator:impl_->allocator];
    [impl_->command_buffer useResidencySet:impl_->residency];
    encode_triangle(impl_->command_buffer, drawable.texture, impl_->pipeline, clear);
    if (shot) {
        encode_triangle(impl_->command_buffer, impl_->offscreen, impl_->pipeline, clear);
        id<MTL4ComputeCommandEncoder> copy = [impl_->command_buffer computeCommandEncoder];
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
    [impl_->command_buffer endCommandBuffer];

    id<MTL4CommandBuffer> buffers[] = {impl_->command_buffer};
    [impl_->queue commit:buffers count:1];
    [impl_->queue signalDrawable:drawable];
    [drawable present];

    impl_->ticket += 1;
    [impl_->queue signalEvent:impl_->fence value:impl_->ticket];
    [impl_->fence waitUntilSignaledValue:impl_->ticket timeoutMS:2000];
    [impl_->residency removeAllocation:drawable.texture];
    [impl_->residency commit];
    [impl_->allocator reset];

    if (shot) {
        const NSUInteger bytes_per_row = (width * 4 + 255u) & ~255u;
        const uint8_t* pixels = static_cast<const uint8_t*>(impl_->readback.contents);
        if (!write_ppm(request.screenshot_ppm, pixels, width, height, bytes_per_row, error)) {
            return false;
        }
    }
    return true;
}

}  // namespace bar
