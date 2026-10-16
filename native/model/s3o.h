#pragma once

#include "smf.h"

#include "pool.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bar {

// One s3o node. ox/oy/oz is the offset from the parent. Vertices stay in this piece's space.
struct ModelPiece {
    std::string name;
    int parent = -1;
    float ox = 0;
    float oy = 0;
    float oz = 0;
    float bind_x = 0;
    float bind_y = 0;
    float bind_z = 0;
};

struct ModelMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ModelPiece> pieces;
    std::vector<uint8_t> color_bc3;
    std::vector<uint8_t> other_bc3;
    int tex_size = 0;
    // CUS normalTex (unitDef customParams.normaltex, else unittextures/blank_normal.dds), BC3 mip 0.
    std::vector<uint8_t> normal_bc3;
    int normal_size = 0;
};

// Piece-local vertices. pad0 is the piece index. Medals and the crown keep their nodes and drop their
// vertices while the bind origin is below the model, so a script can Hide them without drawing them.
bool load_s3o(const GameContent& content, const std::string& object_name, ModelMesh& mesh, std::string& error);

// Reads a DXT5 tangent-space normal map by its VFS path (e.g. "unittextures/Arm_normal.dds").
bool load_model_normal(const GameContent& content, const std::string& path, ModelMesh& mesh, std::string& error);

}  // namespace bar
