#include "economy/metal_extract.h"

#include <algorithm>
#include <cmath>

namespace bar {

bool metal_claims_open(MetalClaims& claims, const TerrainMesh& mesh, std::string& error) {
    claims = MetalClaims{};
    const int size_x = mesh.map_x / 2;
    const int size_z = mesh.map_y / 2;
    if (size_x < 1 || size_z < 1 || mesh.metal.size() < static_cast<size_t>(size_x) * static_cast<size_t>(size_z)) {
        error = "metal map is missing";
        return false;
    }
    if (!(mesh.square_size > 0) || !(mesh.extractor_radius > 0.f)) {
        error = "extractor radius is missing";
        return false;
    }
    claims.size_x = size_x;
    claims.size_z = size_z;
    claims.square = static_cast<float>(mesh.square_size * 2);
    claims.scale = mesh.max_metal;
    claims.radius = mesh.extractor_radius;
    claims.taken.assign(static_cast<size_t>(size_x) * static_cast<size_t>(size_z), 0.f);
    return true;
}

MexTrace metal_claim(MetalClaims& claims, const TerrainMesh& mesh, const std::string& def_name, float x, float z,
                     float depth) {
    MexTrace claim;
    claim.def_name = def_name;
    claim.x = x;
    claim.z = z;
    claim.depth = depth;
    claim.scale = claims.scale;
    if (!(depth > 0.f) || !(claims.square > 0.f)) {
        return claim;
    }
    const float radius = claims.radius;
    const float square = claims.square;
    const int x_begin = std::max(0, static_cast<int>((x - radius) / square));
    const int x_end = std::min(claims.size_x - 1, static_cast<int>((x + radius) / square));
    const int z_begin = std::max(0, static_cast<int>((z - radius) / square));
    const int z_end = std::min(claims.size_z - 1, static_cast<int>((z + radius) / square));
    const float reach = radius * radius;
    for (int iz = z_begin; iz <= z_end; ++iz) {
        const float cz = (static_cast<float>(iz) + 0.5f) * square;
        const float dz = cz - z;
        for (int ix = x_begin; ix <= x_end; ++ix) {
            const float cx = (static_cast<float>(ix) + 0.5f) * square;
            const float dx = cx - x;
            if (dx * dx + dz * dz >= reach) {
                continue;
            }
            const size_t at = static_cast<size_t>(iz) * static_cast<size_t>(claims.size_x) + static_cast<size_t>(ix);
            const int byte = mesh.metal[at];
            ++claim.squares;
            claim.raw += byte;
            const float held = claims.taken[at];
            if (depth <= held) {
                continue;
            }
            const float available = depth - held;
            claims.taken[at] = depth;
            claim.rate += available * (static_cast<float>(byte) * claims.scale);
        }
    }
    return claim;
}

}  // namespace bar
