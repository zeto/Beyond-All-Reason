#include "smf.h"

#include "archive.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>

static_assert(sizeof(bar::TerrainVertex) == 48, "terrain vertex is a 48-byte GPU record");

namespace bar {
namespace {

constexpr int kMinimapSize = 1024;
constexpr int kStride = 2;

int read_i32(const uint8_t* bytes) {
    int32_t value = 0;
    std::memcpy(&value, bytes, 4);
    return value;
}

float read_f32(const uint8_t* bytes) {
    float value = 0;
    std::memcpy(&value, bytes, 4);
    return value;
}

uint16_t read_u16(const uint8_t* bytes) {
    uint16_t value = 0;
    std::memcpy(&value, bytes, 2);
    return value;
}

void normalize(float& x, float& y, float& z) {
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length > 1e-8f) {
        x /= length;
        y /= length;
        z /= length;
    }
}

// cross((0, dhz, span_z), (span_x, dhx, 0)) points up when the samples are flat.
void terrain_normal(float span_x, float dhx, float span_z, float dhz, float& nx, float& ny, float& nz) {
    nx = -span_z * dhx;
    ny = span_z * span_x;
    nz = -dhz * span_x;
    normalize(nx, ny, nz);
}

bool normal_points_up(std::string& error) {
    float nx = 0;
    float ny = 0;
    float nz = 0;
    terrain_normal(16.f, 0.f, 16.f, 0.f, nx, ny, nz);
    if (!(ny > 0.99f) || std::fabs(nx) > 0.01f || std::fabs(nz) > 0.01f) {
        error = "flat terrain normal does not point up";
        return false;
    }
    terrain_normal(16.f, 16.f, 16.f, 0.f, nx, ny, nz);
    if (!(nx < -0.2f)) {
        error = "uphill toward +x should tilt the normal toward -x";
        return false;
    }
    return true;
}

bool read_vec3(lua_State* state, float out[3]) {
    if (!lua_istable(state, -1)) {
        return false;
    }
    for (int i = 1; i <= 3; ++i) {
        lua_rawgeti(state, -1, i);
        if (!lua_isnumber(state, -1)) {
            lua_pop(state, 1);
            return false;
        }
        out[i - 1] = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
    }
    return true;
}

struct MapInfo {
    std::string smf_path;
    float min_height = 0;
    float max_height = 0;
    bool has_height = false;
    float sun[3] = {-0.64f, 0.66f, -0.57f};
    float ambient[3] = {0.35f, 0.35f, 0.35f};
    float diffuse[3] = {1.2f, 1.2f, 1.2f};
    float sky[3] = {0.43f, 0.58f, 0.64f};
    float ground_shadow_density = 0.8f;
    float unit_shadow_density = 0.8f;
    // Spring uses 100 when a map omits extractorRadius. Recoil's maxMetal default is 0.02.
    float extractor_radius = 100.f;
    float max_metal = 0.02f;
    // Recoil atmosphere defaults. Clamped so min is inside [0, max].
    float min_wind = 5.f;
    float max_wind = 25.f;
    // Per-frame gravity. The map tag defaults to 130 and is divided by 30 squared.
    float gravity = -130.f / 900.f;
    std::vector<MapStart> starts;
};

bool read_mapinfo(const std::vector<uint8_t>& bytes, MapInfo& info, std::string& error) {
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        error = "luaL_newstate failed";
        return false;
    }
    luaL_openlibs(state);
    const char* stub =
        "VFS = { DirList = function() return {} end, Include = function() return nil end }\n"
        "Spring = { GetMapOptions = function() return {} end, Echo = function() end }\n";
    std::string source = stub;
    source.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (luaL_loadbuffer(state, source.data(), source.size(), "mapinfo.lua") != 0 || lua_pcall(state, 0, 1, 0) != 0) {
        error = lua_tostring(state, -1) != nullptr ? lua_tostring(state, -1) : "mapinfo.lua failed";
        lua_close(state);
        return false;
    }
    if (!lua_istable(state, -1)) {
        error = "mapinfo.lua did not return a table";
        lua_close(state);
        return false;
    }

    lua_getfield(state, -1, "mapfile");
    if (lua_isstring(state, -1)) {
        info.smf_path = lua_tostring(state, -1);
    }
    lua_pop(state, 1);
    if (info.smf_path.empty()) {
        error = "mapinfo.lua has no mapfile";
        lua_close(state);
        return false;
    }

    lua_getfield(state, -1, "smf");
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "minheight");
        const bool has_min = lua_isnumber(state, -1);
        const float min_height = has_min ? static_cast<float>(lua_tonumber(state, -1)) : 0;
        lua_pop(state, 1);
        lua_getfield(state, -1, "maxheight");
        const bool has_max = lua_isnumber(state, -1);
        const float max_height = has_max ? static_cast<float>(lua_tonumber(state, -1)) : 0;
        lua_pop(state, 1);
        if (has_min && has_max) {
            info.min_height = min_height;
            info.max_height = max_height;
            info.has_height = true;
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, -1, "atmosphere");
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "skycolor");
        read_vec3(state, info.sky);
        lua_pop(state, 1);
        // mapinfo.lua lowercases nested keys too. Recoil's names are minWind and maxWind.
        lua_getfield(state, -1, "minwind");
        if (lua_isnumber(state, -1)) {
            info.min_wind = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
        lua_getfield(state, -1, "maxwind");
        if (lua_isnumber(state, -1)) {
            info.max_wind = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    info.max_wind = std::max(0.f, info.max_wind);
    info.min_wind = std::min(info.max_wind, std::max(0.f, info.min_wind));

    lua_getfield(state, -1, "lighting");
    if (lua_istable(state, -1)) {
        lua_getfield(state, -1, "sundir");
        read_vec3(state, info.sun);
        lua_pop(state, 1);
        lua_getfield(state, -1, "groundambientcolor");
        read_vec3(state, info.ambient);
        lua_pop(state, 1);
        lua_getfield(state, -1, "grounddiffusecolor");
        read_vec3(state, info.diffuse);
        lua_pop(state, 1);
        // Recoil MapInfo: groundShadowDensity / unitShadowDensity, default 0.8, clamped to [0, 1].
        lua_getfield(state, -1, "groundshadowdensity");
        if (lua_isnumber(state, -1)) {
            info.ground_shadow_density = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
        lua_getfield(state, -1, "unitshadowdensity");
        if (lua_isnumber(state, -1)) {
            info.unit_shadow_density = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    lua_getfield(state, -1, "teams");
    if (lua_istable(state, -1)) {
        const int teams_index = lua_gettop(state);
        lua_pushnil(state);
        while (lua_next(state, teams_index) != 0) {
            if (lua_isnumber(state, -2) && lua_istable(state, -1)) {
                const int team = static_cast<int>(lua_tonumber(state, -2));
                lua_getfield(state, -1, "startpos");
                if (lua_istable(state, -1)) {
                    lua_getfield(state, -1, "x");
                    lua_getfield(state, -2, "z");
                    if (lua_isnumber(state, -2) && lua_isnumber(state, -1)) {
                        MapStart start;
                        start.team = team;
                        start.x = static_cast<float>(lua_tonumber(state, -2));
                        start.z = static_cast<float>(lua_tonumber(state, -1));
                        info.starts.push_back(start);
                    }
                    lua_pop(state, 2);
                }
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    // mapinfo.lua lowercases its keys before returning.
    lua_getfield(state, -1, "extractorradius");
    if (lua_isnumber(state, -1)) {
        info.extractor_radius = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);

    lua_getfield(state, -1, "maxmetal");
    if (lua_isnumber(state, -1)) {
        info.max_metal = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);

    // mapinfo.lua lowercases this too. Recoil's default tag is 130, then negated per frame.
    lua_getfield(state, -1, "gravity");
    float gravity_tag = 130.f;
    if (lua_isnumber(state, -1)) {
        gravity_tag = static_cast<float>(lua_tonumber(state, -1));
    }
    lua_pop(state, 1);
    gravity_tag = std::max(0.001f, gravity_tag);
    info.gravity = -gravity_tag / (30.f * 30.f);

    lua_close(state);
    return true;
}

void decode_dxt1(const uint8_t* blocks, int size, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(size) * size * 4, 0);
    const int blocks_per_row = size / 4;
    for (int by = 0; by < blocks_per_row; ++by) {
        for (int bx = 0; bx < blocks_per_row; ++bx) {
            const uint8_t* block = blocks + (static_cast<size_t>(by) * blocks_per_row + bx) * 8;
            const uint16_t c0 = read_u16(block);
            const uint16_t c1 = read_u16(block + 2);
            uint32_t bits = 0;
            std::memcpy(&bits, block + 4, 4);
            const int r0 = (((c0 >> 11) & 31) * 255) / 31;
            const int g0 = (((c0 >> 5) & 63) * 255) / 63;
            const int b0 = ((c0 & 31) * 255) / 31;
            const int r1 = (((c1 >> 11) & 31) * 255) / 31;
            const int g1 = (((c1 >> 5) & 63) * 255) / 63;
            const int b1 = ((c1 & 31) * 255) / 31;
            int colors[4][3] = {{r0, g0, b0}, {r1, g1, b1}, {0, 0, 0}, {0, 0, 0}};
            if (c0 > c1) {
                colors[2][0] = (2 * r0 + r1) / 3;
                colors[2][1] = (2 * g0 + g1) / 3;
                colors[2][2] = (2 * b0 + b1) / 3;
                colors[3][0] = (r0 + 2 * r1) / 3;
                colors[3][1] = (g0 + 2 * g1) / 3;
                colors[3][2] = (b0 + 2 * b1) / 3;
            } else {
                colors[2][0] = (r0 + r1) / 2;
                colors[2][1] = (g0 + g1) / 2;
                colors[2][2] = (b0 + b1) / 2;
            }
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int code = (bits >> (2 * (py * 4 + px))) & 3;
                    const int x = bx * 4 + px;
                    const int y = by * 4 + py;
                    uint8_t* pixel = rgba.data() + (static_cast<size_t>(y) * size + x) * 4;
                    pixel[0] = static_cast<uint8_t>(colors[code][0]);
                    pixel[1] = static_cast<uint8_t>(colors[code][1]);
                    pixel[2] = static_cast<uint8_t>(colors[code][2]);
                    pixel[3] = 255;
                }
            }
        }
    }
}

// Groups metal-map strips the way api_resource_spot_finder GetSpotsMetal does, then
// keeps the spots gui_metalspots would draw. Occupied spots stay cyan until extractors exist.
void build_metal_rings(TerrainMesh& mesh, float extractor_radius) {
    mesh.ring_vertices.clear();
    mesh.ring_indices.clear();
    mesh.metal_spot_count = 0;
    mesh.metal_map = false;
    mesh.extractor_radius = extractor_radius;
    const int step = mesh.square_size * 2;
    const int metal_x = mesh.map_x / 2;
    const int metal_y = mesh.map_y / 2;
    if (step <= 0 || metal_x < 2 || metal_y < 2 ||
        mesh.metal.size() < static_cast<size_t>(metal_x) * static_cast<size_t>(metal_y)) {
        return;
    }
    const int start_x = (step * 3) / 2;
    const int start_z = start_x;
    const int end_x = static_cast<int>(mesh.world_x) - start_x;
    const int end_z = static_cast<int>(mesh.world_z) - start_z;
    if (end_x < start_x || end_z < start_z) {
        return;
    }
    const float max_strip = extractor_radius * 6.f;

    auto metal_at = [&](int world_x, int world_z) {
        const int ix = world_x / step;
        const int iz = world_z / step;
        if (ix < 0 || iz < 0 || ix >= metal_x || iz >= metal_y) {
            return 0;
        }
        return static_cast<int>(mesh.metal[static_cast<size_t>(iz) * metal_x + ix]);
    };

    struct StripGroup {
        std::map<int, int> left;
        std::map<int, int> right;
        int min_z = 0;
        int max_z = 0;
        int worth = 0;
        int parent = -1;
    };
    struct Strip {
        int x1 = 0;
        int x2 = 0;
        int group = 0;
    };
    std::vector<StripGroup> groups;
    std::vector<Strip> strips;

    auto find_group = [&](int id) {
        int root = id;
        while (groups[root].parent >= 0) {
            root = groups[root].parent;
        }
        while (id != root && groups[id].parent != root) {
            const int next = groups[id].parent;
            groups[id].parent = root;
            id = next;
        }
        return root;
    };
    auto absorb = [&](int dst_id, int src_id, int z) {
        if (dst_id == src_id) {
            return;
        }
        StripGroup& dst = groups[dst_id];
        StripGroup& src = groups[src_id];
        for (int iz = src.min_z; iz <= dst.min_z - step; iz += step) {
            const auto left = src.left.find(iz);
            if (left != src.left.end()) {
                dst.left[iz] = left->second;
            }
        }
        for (int iz = src.min_z; iz <= src.max_z; iz += step) {
            const auto right = src.right.find(iz);
            if (right != src.right.end()) {
                dst.right[iz] = right->second;
            }
            if (dst.left.find(iz) == dst.left.end()) {
                const auto left = src.left.find(iz);
                if (left != src.left.end()) {
                    dst.left[iz] = left->second;
                }
            }
        }
        if (src.min_z < dst.min_z) {
            dst.min_z = src.min_z;
        }
        dst.max_z = z;
        dst.worth += src.worth;
        src.parent = dst_id;
    };
    auto do_strip = [&](int x1, int x2, int z, int worth, int above, int working) {
        int assigned = -1;
        for (int i = above; i < working; ++i) {
            const Strip& prev = strips[i];
            if (prev.x1 > x2 + step) {
                break;
            }
            if (prev.x2 + step >= x1) {
                const int match = find_group(prev.group);
                if (assigned >= 0) {
                    if (match != assigned) {
                        absorb(assigned, match, z);
                    }
                } else {
                    assigned = match;
                    StripGroup& group = groups[assigned];
                    if (group.left.find(z) == group.left.end()) {
                        group.left[z] = x1;
                    }
                    group.right[z] = x2;
                    group.max_z = z;
                    group.worth += worth;
                }
            } else {
                above = i + 1;
            }
        }
        if (assigned < 0) {
            StripGroup created;
            created.left[z] = x1;
            created.right[z] = x2;
            created.min_z = z;
            created.max_z = z;
            created.worth = worth;
            groups.push_back(std::move(created));
            assigned = static_cast<int>(groups.size()) - 1;
        }
        strips.push_back(Strip{x1, x2, assigned});
        return above;
    };

    int row_begin = 0;
    int row_end = 0;
    bool first_row = true;
    for (int mz = start_z; mz <= end_z; mz += step) {
        int above = 0;
        int working = 0;
        if (!first_row) {
            above = row_begin;
            working = row_end;
        }
        first_row = false;
        const int this_row = static_cast<int>(strips.size());
        int strip_start = -1;
        int strip_worth = 0;
        for (int mx = start_x; mx <= end_x; mx += step) {
            const int amount = metal_at(mx, mz);
            if (amount > 0) {
                if (strip_start < 0) {
                    strip_start = mx;
                }
                strip_worth += amount;
            } else if (strip_start >= 0) {
                above = do_strip(strip_start, mx - step, mz, strip_worth, above, working);
                strip_start = -1;
                strip_worth = 0;
            }
        }
        if (strip_start >= 0) {
            do_strip(strip_start, end_x, mz, strip_worth, above, working);
        }
        row_begin = this_row;
        row_end = static_cast<int>(strips.size());
    }

    struct Spot {
        float x = 0;
        float z = 0;
        float scale = 1;
    };
    std::vector<Spot> spots;
    bool metal_map = false;
    for (const StripGroup& group : groups) {
        if (group.parent >= 0) {
            continue;
        }
        int min_x = std::numeric_limits<int>::max();
        int max_x = -1;
        for (int iz = group.min_z; iz <= group.max_z; iz += step) {
            const auto left = group.left.find(iz);
            const auto right = group.right.find(iz);
            if (left != group.left.end() && left->second < min_x) {
                min_x = left->second;
            }
            if (right != group.right.end() && right->second > max_x) {
                max_x = right->second;
            }
        }
        if (max_x < 0 || min_x == std::numeric_limits<int>::max()) {
            continue;
        }
        if (static_cast<float>(max_x - min_x) > max_strip ||
            static_cast<float>(group.max_z - group.min_z) > max_strip) {
            metal_map = true;
            break;
        }
        const float width = static_cast<float>(std::abs(max_x - min_x));
        const float depth = static_cast<float>(std::abs(group.max_z - group.min_z));
        const float scale = 0.77f + (width * depth) / 10000.f;
        const float shown = static_cast<float>(std::round(static_cast<double>(group.worth) / 100.0) / 10.0);
        if (!(shown > 0.001f && shown < 15.f) || !(scale < 4.f)) {
            continue;
        }
        spots.push_back(Spot{(min_x + max_x) * 0.5f, (group.min_z + group.max_z) * 0.5f, scale});
    }
    if (metal_map) {
        mesh.metal_map = true;
        return;
    }

    mesh.metal_spots.clear();
    for (const Spot& spot : spots) {
        mesh.metal_spots.push_back(TerrainMesh::MetalSpot{spot.x, spot.z});
    }

    // Visible band of the widget ring: inner 1.8*(12-1)*2*scale, outer 1.98*(12+1)*2*scale.
    constexpr float kInner = 39.6f;
    constexpr float kOuter = 51.48f;
    constexpr int kSegments = 48;
    constexpr float kTurn = 6.2831853f;
    for (const Spot& spot : spots) {
        const float ground = terrain_height_at(mesh, spot.x, spot.z);
        const float y = ground >= 0.f ? ground + 5.f : 2.f;
        const float inner = kInner * spot.scale;
        const float outer = kOuter * spot.scale;
        const uint32_t base = static_cast<uint32_t>(mesh.ring_vertices.size());
        for (int i = 0; i <= kSegments; ++i) {
            const float angle = (static_cast<float>(i) / kSegments) * kTurn;
            const float s = std::sin(angle);
            const float c = std::cos(angle);
            TerrainVertex vertex{};
            vertex.ny = 1.f;
            vertex.px = spot.x + s * inner;
            vertex.py = y;
            vertex.pz = spot.z + c * inner;
            mesh.ring_vertices.push_back(vertex);
            vertex.px = spot.x + s * outer;
            vertex.pz = spot.z + c * outer;
            mesh.ring_vertices.push_back(vertex);
        }
        for (int i = 0; i < kSegments; ++i) {
            const uint32_t i0 = base + static_cast<uint32_t>(i * 2);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + 2;
            const uint32_t i3 = i0 + 3;
            mesh.ring_indices.insert(mesh.ring_indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    mesh.metal_spot_count = static_cast<int>(spots.size());
}

bool build_mesh(const std::vector<uint8_t>& smf, const MapInfo& info, TerrainMesh& mesh, std::string& error) {
    if (!normal_points_up(error)) {
        return false;
    }
    if (smf.size() < 80 || std::memcmp(smf.data(), "spring map file", 15) != 0) {
        error = "not a spring map file";
        return false;
    }
    const int map_x = read_i32(smf.data() + 24);
    const int map_y = read_i32(smf.data() + 28);
    const int square_size = read_i32(smf.data() + 32);
    const float header_min = read_f32(smf.data() + 44);
    const float header_max = read_f32(smf.data() + 48);
    const int height_ptr = read_i32(smf.data() + 52);
    const int minimap_ptr = read_i32(smf.data() + 64);
    const int metal_ptr = read_i32(smf.data() + 68);
    if (map_x < 2 || map_y < 2 || map_x > 8192 || map_y > 8192 || square_size <= 0) {
        error = "SMF dimensions are unusable";
        return false;
    }
    if (map_x % kStride != 0 || map_y % kStride != 0) {
        error = "SMF dimensions are not divisible by the mesh stride";
        return false;
    }

    const size_t height_bytes = static_cast<size_t>(map_x + 1) * static_cast<size_t>(map_y + 1) * 2;
    const size_t metal_bytes = static_cast<size_t>(map_x / 2) * static_cast<size_t>(map_y / 2);
    constexpr size_t kMinimapBytes = static_cast<size_t>(kMinimapSize / 4) * (kMinimapSize / 4) * 8;
    if (height_ptr < 0 || metal_ptr < 0 || minimap_ptr < 0 ||
        static_cast<size_t>(height_ptr) + height_bytes > smf.size() ||
        static_cast<size_t>(metal_ptr) + metal_bytes > smf.size() ||
        static_cast<size_t>(minimap_ptr) + kMinimapBytes > smf.size()) {
        error = "SMF payload pointers fall outside the file";
        return false;
    }

    mesh.map_file = info.smf_path;
    mesh.map_x = map_x;
    mesh.map_y = map_y;
    mesh.square_size = square_size;
    mesh.height_from_mapinfo = info.has_height;
    mesh.min_height = info.has_height ? info.min_height : header_min;
    mesh.max_height = info.has_height ? info.max_height : header_max;
    mesh.world_x = static_cast<float>(map_x * square_size);
    mesh.world_z = static_cast<float>(map_y * square_size);
    std::memcpy(mesh.sun, info.sun, sizeof(mesh.sun));
    std::memcpy(mesh.ambient, info.ambient, sizeof(mesh.ambient));
    std::memcpy(mesh.diffuse, info.diffuse, sizeof(mesh.diffuse));
    std::memcpy(mesh.sky, info.sky, sizeof(mesh.sky));
    mesh.ground_shadow_density = std::clamp(info.ground_shadow_density, 0.f, 1.f);
    mesh.unit_shadow_density = std::clamp(info.unit_shadow_density, 0.f, 1.f);
    mesh.starts = info.starts;
    mesh.mesh_stride = kStride;
    normalize(mesh.sun[0], mesh.sun[1], mesh.sun[2]);

    const float height_span = mesh.max_height - mesh.min_height;
    const uint8_t* height_bytes_ptr = smf.data() + height_ptr;
    auto sample = [&](int sx, int sz) {
        sx = std::clamp(sx, 0, map_x);
        sz = std::clamp(sz, 0, map_y);
        const uint16_t raw = read_u16(height_bytes_ptr + (static_cast<size_t>(sz) * (map_x + 1) + sx) * 2);
        return mesh.min_height + (static_cast<float>(raw) / 65535.f) * height_span;
    };

    const int grid_x = map_x / kStride + 1;
    const int grid_z = map_y / kStride + 1;
    mesh.vertices.resize(static_cast<size_t>(grid_x) * grid_z);
    for (int iz = 0; iz < grid_z; ++iz) {
        for (int ix = 0; ix < grid_x; ++ix) {
            const int sx = ix * kStride;
            const int sz = iz * kStride;
            const int sx0 = std::clamp(sx - kStride, 0, map_x);
            const int sx1 = std::clamp(sx + kStride, 0, map_x);
            const int sz0 = std::clamp(sz - kStride, 0, map_y);
            const int sz1 = std::clamp(sz + kStride, 0, map_y);
            const float span_x = static_cast<float>((sx1 - sx0) * square_size);
            const float span_z = static_cast<float>((sz1 - sz0) * square_size);
            TerrainVertex& vertex = mesh.vertices[static_cast<size_t>(iz) * grid_x + ix];
            vertex.px = static_cast<float>(sx * square_size);
            vertex.py = sample(sx, sz);
            vertex.pz = static_cast<float>(sz * square_size);
            vertex.pad0 = 0;
            terrain_normal(span_x, sample(sx1, sz) - sample(sx0, sz), span_z, sample(sx, sz1) - sample(sx, sz0),
                           vertex.nx, vertex.ny, vertex.nz);
            vertex.pad1 = 0;
            vertex.u = vertex.px / mesh.world_x;
            vertex.v = vertex.pz / mesh.world_z;
            vertex.mu = vertex.u;
            vertex.mv = vertex.v;
        }
    }
    mesh.center_height = sample(map_x / 2, map_y / 2);
    mesh.corner_height = sample(0, 0);

    mesh.indices.reserve(static_cast<size_t>(map_x / kStride) * (map_y / kStride) * 6);
    for (int iz = 0; iz < grid_z - 1; ++iz) {
        for (int ix = 0; ix < grid_x - 1; ++ix) {
            const uint32_t a = static_cast<uint32_t>(iz * grid_x + ix);
            const uint32_t b = a + 1;
            const uint32_t c = static_cast<uint32_t>((iz + 1) * grid_x + ix + 1);
            const uint32_t d = c - 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
        }
    }

    constexpr int kWater = 32;
    mesh.water_vertices.resize(static_cast<size_t>(kWater + 1) * (kWater + 1));
    for (int iz = 0; iz <= kWater; ++iz) {
        for (int ix = 0; ix <= kWater; ++ix) {
            TerrainVertex& vertex = mesh.water_vertices[static_cast<size_t>(iz) * (kWater + 1) + ix];
            vertex.px = mesh.world_x * static_cast<float>(ix) / kWater;
            vertex.py = 0.05f;
            vertex.pz = mesh.world_z * static_cast<float>(iz) / kWater;
            vertex.pad0 = 0;
            vertex.nx = 0;
            vertex.ny = 1;
            vertex.nz = 0;
            vertex.pad1 = 0;
            vertex.u = vertex.px / mesh.world_x;
            vertex.v = vertex.pz / mesh.world_z;
            vertex.mu = vertex.u;
            vertex.mv = vertex.v;
        }
    }
    mesh.water_indices.reserve(static_cast<size_t>(kWater) * kWater * 6);
    for (int iz = 0; iz < kWater; ++iz) {
        for (int ix = 0; ix < kWater; ++ix) {
            const uint32_t a = static_cast<uint32_t>(iz * (kWater + 1) + ix);
            const uint32_t b = a + 1;
            const uint32_t c = static_cast<uint32_t>((iz + 1) * (kWater + 1) + ix + 1);
            const uint32_t d = c - 1;
            mesh.water_indices.insert(mesh.water_indices.end(), {a, b, c, a, c, d});
        }
    }

    decode_dxt1(smf.data() + minimap_ptr, kMinimapSize, mesh.minimap_rgba);
    mesh.minimap_size = kMinimapSize;
    mesh.metal.assign(smf.begin() + metal_ptr, smf.begin() + metal_ptr + static_cast<std::ptrdiff_t>(metal_bytes));
    mesh.metal_size = map_x / 2;
    mesh.max_metal = info.max_metal;
    mesh.min_wind = info.min_wind;
    mesh.max_wind = info.max_wind;
    mesh.gravity = info.gravity;
    build_metal_rings(mesh, info.extractor_radius);
    return true;
}

}  // namespace

float terrain_height_at(const TerrainMesh& mesh, float x, float z) {
    const int stride = std::max(mesh.mesh_stride, 1);
    const float step = static_cast<float>(mesh.square_size * stride);
    const int grid_x = mesh.map_x / stride + 1;
    const int grid_z = mesh.map_y / stride + 1;
    if (grid_x < 2 || grid_z < 2 || mesh.vertices.size() < static_cast<size_t>(grid_x) * grid_z || step <= 0.f) {
        return 0;
    }
    const float fx = std::clamp(x / step, 0.f, static_cast<float>(grid_x - 1));
    const float fz = std::clamp(z / step, 0.f, static_cast<float>(grid_z - 1));
    const int x0 = static_cast<int>(fx);
    const int z0 = static_cast<int>(fz);
    const int x1 = std::min(x0 + 1, grid_x - 1);
    const int z1 = std::min(z0 + 1, grid_z - 1);
    const float tx = fx - static_cast<float>(x0);
    const float tz = fz - static_cast<float>(z0);
    auto height = [&](int ix, int iz) { return mesh.vertices[static_cast<size_t>(iz) * grid_x + ix].py; };
    const float h00 = height(x0, z0);
    const float h10 = height(x1, z0);
    const float h01 = height(x0, z1);
    const float h11 = height(x1, z1);
    const float top = h00 + (h10 - h00) * tx;
    const float bottom = h01 + (h11 - h01) * tx;
    return top + (bottom - top) * tz;
}

bool terrain_raycast(const TerrainMesh& mesh, const float origin[3], const float direction[3], float& hit_x,
                     float& hit_z) {
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (length < 1e-6f) {
        return false;
    }
    const float dx = direction[0] / length;
    const float dy = direction[1] / length;
    const float dz = direction[2] / length;
    auto at = [&](float t, float& x, float& y, float& z) {
        x = origin[0] + dx * t;
        y = origin[1] + dy * t;
        z = origin[2] + dz * t;
    };
    float previous = 4.f;
    bool was_above = false;
    for (int step = 0; step < 4000; ++step) {
        const float t = 4.f + static_cast<float>(step) * 16.f;
        float x = 0;
        float y = 0;
        float z = 0;
        at(t, x, y, z);
        if (x < -64.f || z < -64.f || x > mesh.world_x + 64.f || z > mesh.world_z + 64.f) {
            if (was_above) {
                break;
            }
            continue;
        }
        const float ground = terrain_height_at(mesh, x, z);
        if (y <= ground && was_above) {
            float lo = previous;
            float hi = t;
            for (int refine = 0; refine < 12; ++refine) {
                const float mid = 0.5f * (lo + hi);
                float mx = 0;
                float my = 0;
                float mz = 0;
                at(mid, mx, my, mz);
                if (my <= terrain_height_at(mesh, mx, mz)) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            float hx = 0;
            float hy = 0;
            float hz = 0;
            at(hi, hx, hy, hz);
            hit_x = std::clamp(hx, 0.f, mesh.world_x);
            hit_z = std::clamp(hz, 0.f, mesh.world_z);
            return true;
        }
        was_above = y > ground;
        previous = t;
    }
    return false;
}

bool load_terrain(const std::string& archive_path, TerrainMesh& mesh, std::string& error) {
    std::vector<uint8_t> mapinfo;
    if (!Archive::read_file(archive_path, "mapinfo.lua", mapinfo, error)) {
        return false;
    }
    MapInfo info;
    if (!read_mapinfo(mapinfo, info, error)) {
        return false;
    }
    std::vector<uint8_t> smf;
    if (!Archive::read_file(archive_path, info.smf_path, smf, error)) {
        return false;
    }
    return build_mesh(smf, info, mesh, error);
}

}  // namespace bar
