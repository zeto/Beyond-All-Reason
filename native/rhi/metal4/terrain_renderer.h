#pragma once

#include "gl/draw_list.h"
#include "s3o.h"
#include "smf.h"

#include <string>

namespace bar {

struct TerrainFrame {
    void* metal_layer = nullptr;
    const float* view_proj = nullptr;
    const char* screenshot_ppm = nullptr;
    // Widget draw list from the sim tick. Null skips the HUD pass.
    const HudList* hud = nullptr;
};

// Metal 4 types stay inside the .mm file.
class TerrainRenderer {
public:
    TerrainRenderer();
    ~TerrainRenderer();

    TerrainRenderer(const TerrainRenderer&) = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;

    bool init(const std::string& shader_path, const TerrainMesh& mesh, std::string& error);
    // Uploads one baked model. An empty mesh is rejected. Team color tints tex1 alpha.
    bool set_model(const ModelMesh& model, float color_r, float color_g, float color_b, std::string& error);
    void set_pose(float x, float y, float z, float heading, float bank = 0);
    // Column-major piece matrices, 16 floats each. A 0 in element 15 hides that piece.
    void set_pieces(const float* column_major, int count);
    // World-space radius of the team-colored ground ring. 0 hides it.
    void set_marker(float radius);
    // Cyan mex rings. On for an interactive window. Off for screenshot checks.
    void set_metal_hud(bool enabled);
    void attach(void* metal_layer);
    bool draw(const TerrainFrame& frame, std::string& error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    // Waits until every submitted frame has finished. Draw calls this before resizing
    // targets. The destructor calls it so allocators are not freed while the GPU reads them.
    bool finish_gpu(std::string& error);
    void release_hud();
    bool prepare_hud(const HudList* list, std::string& error);
    void encode_hud(void* command_buffer, void* target);
};

// Renders HudText samples with Spring \255RGB codes and invalid UTF-8.
// Returns false only if rasterization fails (must not SIGSEGV).
bool check_hud_text(std::string& error);

}  // namespace bar
