#include "smf_textures.h"

#include "smf.h"
#include "water_textures.h"
#include "../vfs/archive.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace bar {
namespace {

int32_t read_i32(const uint8_t* p) {
    int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

uint32_t read_u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

// Recoil FIND_MAP_TEXTURE: the name as given, else "maps/" + name.
bool read_map_file(const std::string& archive, const std::string& name, std::vector<uint8_t>& out) {
    std::string ignored;
    if (name.empty()) {
        return false;
    }
    if (Archive::read_file(archive, name, out, ignored)) {
        return true;
    }
    return Archive::read_file(archive, "maps/" + name, out, ignored);
}

std::string lower_ext(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

bool load_image(const std::string& archive, const std::string& name, GpuImage& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_map_file(archive, name, bytes)) {
        error = "missing " + name;
        return false;
    }
    const std::string ext = lower_ext(name);
    if (ext == "dds") {
        return decode_dds_blocks(bytes, out, error);
    }
    if (ext == "tga") {
        if (!decode_tga(bytes, out, error)) {
            return false;
        }
        build_rgba_mips(out);
        return true;
    }
    error = "unsupported map texture type " + name;
    return false;
}

// SMT tile: DXT1 32x32 with 16, 8 and 4 px mips, 680 bytes (Recoil SMALL_TILE_SIZE).
constexpr int kTileBytes = 680;
constexpr int kTileMipOffset[4] = {0, 512, 640, 672};

bool load_diffuse(const TerrainMesh& mesh, GpuImage& out, std::string& error) {
    if (mesh.tile_index.empty() || mesh.tile_files.empty()) {
        error = "SMF has no tile index";
        return false;
    }
    std::vector<uint8_t> tiles;
    const size_t slash = mesh.map_file.find_last_of('/');
    const std::string smf_dir = slash == std::string::npos ? "" : mesh.map_file.substr(0, slash + 1);
    for (size_t f = 0; f < mesh.tile_files.size(); ++f) {
        const int count = mesh.tile_file_counts[f];
        std::vector<uint8_t> smt;
        std::string ignored;
        if (!Archive::read_file(mesh.archive_path, smf_dir + mesh.tile_files[f], smt, ignored) &&
            !Archive::read_file(mesh.archive_path, mesh.tile_files[f], smt, ignored)) {
            // Recoil fills a missing file's tiles with 0xaa ("ALL SMALL TILES WILL BE MADE RED").
            tiles.insert(tiles.end(), static_cast<size_t>(count) * kTileBytes, 0xaa);
            continue;
        }
        if (smt.size() < 32 || std::memcmp(smt.data(), "spring tilefile", 15) != 0 || read_i32(smt.data() + 16) != 1 ||
            read_i32(smt.data() + 24) != 32 || read_i32(smt.data() + 28) != 1) {
            error = "SMT " + mesh.tile_files[f] + " does not match the .smt format";
            return false;
        }
        const size_t need = 32 + static_cast<size_t>(count) * kTileBytes;
        if (smt.size() < need) {
            error = "SMT " + mesh.tile_files[f] + " is truncated";
            return false;
        }
        tiles.insert(tiles.end(), smt.begin() + 32, smt.begin() + static_cast<std::ptrdiff_t>(need));
    }
    const int tile_count = static_cast<int>(tiles.size() / kTileBytes);
    out.format = GpuImage::Format::BC1;
    out.width = mesh.tile_grid_x * 32;
    out.height = mesh.tile_grid_y * 32;
    out.levels.assign(4, {});
    for (int level = 0; level < 4; ++level) {
        const int blocks_per_tile = 8 >> level;
        const int row_blocks = mesh.tile_grid_x * blocks_per_tile;
        const int col_blocks = mesh.tile_grid_y * blocks_per_tile;
        std::vector<uint8_t>& dst = out.levels[level];
        dst.assign(static_cast<size_t>(row_blocks) * col_blocks * 8, 0);
        for (int ty = 0; ty < mesh.tile_grid_y; ++ty) {
            for (int tx = 0; tx < mesh.tile_grid_x; ++tx) {
                int tile = mesh.tile_index[static_cast<size_t>(ty) * mesh.tile_grid_x + tx];
                if (tile < 0 || tile >= tile_count) {
                    continue;
                }
                const uint8_t* src = tiles.data() + static_cast<size_t>(tile) * kTileBytes + kTileMipOffset[level];
                for (int by = 0; by < blocks_per_tile; ++by) {
                    uint8_t* row = dst.data() +
                                   ((static_cast<size_t>(ty) * blocks_per_tile + by) * row_blocks +
                                    static_cast<size_t>(tx) * blocks_per_tile) * 8;
                    std::memcpy(row, src + static_cast<size_t>(by) * blocks_per_tile * 8,
                                static_cast<size_t>(blocks_per_tile) * 8);
                }
            }
        }
    }
    return true;
}

}  // namespace

bool decode_dds_blocks(const std::vector<uint8_t>& f, GpuImage& out, std::string& error) {
    if (f.size() < 128 || std::memcmp(f.data(), "DDS ", 4) != 0) {
        error = "not a DDS file";
        return false;
    }
    const int height = static_cast<int>(read_u32(f.data() + 12));
    const int width = static_cast<int>(read_u32(f.data() + 16));
    const uint32_t flags = read_u32(f.data() + 8);
    int mips = (flags & 0x20000u) ? static_cast<int>(read_u32(f.data() + 28)) : 1;
    mips = std::max(mips, 1);
    const uint8_t* fourcc = f.data() + 84;
    int block = 0;
    if (std::memcmp(fourcc, "DXT1", 4) == 0) {
        out.format = GpuImage::Format::BC1;
        block = 8;
    } else if (std::memcmp(fourcc, "DXT3", 4) == 0) {
        out.format = GpuImage::Format::BC2;
        block = 16;
    } else if (std::memcmp(fourcc, "DXT5", 4) == 0) {
        out.format = GpuImage::Format::BC3;
        block = 16;
    } else {
        error = "unsupported DDS format";
        return false;
    }
    out.width = width;
    out.height = height;
    out.levels.clear();
    // DDSCAPS2_CUBEMAP with all six DDSCAPS2_CUBEMAP_* face bits.
    const uint32_t caps2 = read_u32(f.data() + 112);
    out.faces = (caps2 & 0x200u) && (caps2 & 0xfc00u) == 0xfc00u ? 6 : 1;
    size_t at = 128;
    for (int face = 0; face < out.faces; ++face) {
        int w = width;
        int h = height;
        for (int level = 0; level < mips; ++level) {
            const size_t bytes = static_cast<size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * block;
            if (at + bytes > f.size()) {
                error = "DDS level is truncated";
                return false;
            }
            out.levels.emplace_back(f.begin() + static_cast<std::ptrdiff_t>(at),
                                    f.begin() + static_cast<std::ptrdiff_t>(at + bytes));
            at += bytes;
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
    }
    if (out.levels.empty()) {
        error = "DDS base level is truncated";
        return false;
    }
    return true;
}

bool decode_tga(const std::vector<uint8_t>& f, GpuImage& out, std::string& error) {
    if (f.size() < 18) {
        error = "TGA header truncated";
        return false;
    }
    const int id_len = f[0];
    const int cmap_type = f[1];
    const int type = f[2];
    const int cmap_len = f[5] | (f[6] << 8);
    const int cmap_bits = f[7];
    const int width = f[12] | (f[13] << 8);
    const int height = f[14] | (f[15] << 8);
    const int bpp = f[16];
    const bool top_first = (f[17] & 0x20) != 0;
    const bool rle = type == 10 || type == 11;
    const bool gray = type == 3 || type == 11;
    if (cmap_type != 0 || (type != 2 && type != 3 && type != 10 && type != 11) ||
        !(bpp == 8 || bpp == 24 || bpp == 32) || width <= 0 || height <= 0) {
        error = "unsupported TGA";
        return false;
    }
    const int px_bytes = bpp / 8;
    size_t at = 18 + static_cast<size_t>(id_len) + static_cast<size_t>(cmap_len) * ((cmap_bits + 7) / 8);
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<uint8_t> raw(count * px_bytes);
    if (!rle) {
        if (at + raw.size() > f.size()) {
            error = "TGA pixels truncated";
            return false;
        }
        std::memcpy(raw.data(), f.data() + at, raw.size());
    } else {
        size_t px = 0;
        while (px < count) {
            if (at >= f.size()) {
                error = "TGA RLE truncated";
                return false;
            }
            const uint8_t head = f[at++];
            const size_t run = (head & 0x7f) + 1;
            if (head & 0x80) {
                if (at + px_bytes > f.size()) {
                    error = "TGA RLE truncated";
                    return false;
                }
                for (size_t i = 0; i < run && px < count; ++i, ++px) {
                    std::memcpy(&raw[px * px_bytes], f.data() + at, px_bytes);
                }
                at += px_bytes;
            } else {
                const size_t n = std::min(run, count - px);
                if (at + n * px_bytes > f.size()) {
                    error = "TGA RLE truncated";
                    return false;
                }
                std::memcpy(&raw[px * px_bytes], f.data() + at, n * px_bytes);
                at += run * px_bytes;
                px += n;
            }
        }
    }
    out.format = GpuImage::Format::RGBA8;
    out.width = width;
    out.height = height;
    out.levels.assign(1, std::vector<uint8_t>(count * 4));
    std::vector<uint8_t>& dst = out.levels[0];
    for (int y = 0; y < height; ++y) {
        const int src_y = top_first ? y : height - 1 - y;
        for (int x = 0; x < width; ++x) {
            const uint8_t* s = &raw[(static_cast<size_t>(src_y) * width + x) * px_bytes];
            uint8_t* d = &dst[(static_cast<size_t>(y) * width + x) * 4];
            if (gray || px_bytes == 1) {
                d[0] = d[1] = d[2] = s[0];
                d[3] = 255;
            } else {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = px_bytes == 4 ? s[3] : 255;
            }
        }
    }
    return true;
}

void build_rgba_mips(GpuImage& image) {
    if (image.format != GpuImage::Format::RGBA8 || image.levels.empty()) {
        return;
    }
    image.levels.resize(1);
    int w = image.width;
    int h = image.height;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2);
        const int nh = std::max(1, h / 2);
        const std::vector<uint8_t>& src = image.levels.back();
        std::vector<uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(w - 1, x * 2 + dx);
                            const int sy = std::min(h - 1, y * 2 + dy);
                            sum += src[(static_cast<size_t>(sy) * w + sx) * 4 + c];
                        }
                    }
                    dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        image.levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

namespace {

// JPEG/PNG/BMP through ImageIO into straight RGBA8, row 0 = first stored row (CBitmap order).
bool decode_imageio_rgba(const std::vector<uint8_t>& bytes, GpuImage& out) {
    CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
    if (data == nullptr) {
        return false;
    }
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (src == nullptr) {
        return false;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (image == nullptr) {
        return false;
    }
    const int width = static_cast<int>(CGImageGetWidth(image));
    const int height = static_cast<int>(CGImageGetHeight(image));
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (ctx == nullptr) {
        CGImageRelease(image);
        return false;
    }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
    CGContextRelease(ctx);
    CGImageRelease(image);
    out.format = GpuImage::Format::RGBA8;
    out.width = width;
    out.height = height;
    out.faces = 1;
    out.levels.assign(1, std::move(rgba));
    return width > 0 && height > 0;
}

// VFS.DirList(dir, pattern, mode, recursive) over the game tree: forward-slash paths
// relative to the game root, recursive when asked. resources.lua lists its bitmaps with it.
int vfs_dir_list(lua_State* state) {
    const char* dir = luaL_checkstring(state, 1);
    const bool recursive = lua_toboolean(state, 4) != 0;
    lua_getfield(state, LUA_REGISTRYINDEX, "bar_game_root");
    const std::string root = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    lua_newtable(state);
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::path(root) / dir;
    if (!std::filesystem::is_directory(base, ec)) {
        return 1;
    }
    std::vector<std::string> files;
    auto add = [&](const std::filesystem::directory_entry& entry) {
        if (entry.is_regular_file(ec)) {
            files.push_back(std::filesystem::relative(entry.path(), root, ec).generic_string());
        }
    };
    if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base, ec)) {
            add(entry);
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            add(entry);
        }
    }
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i < files.size(); ++i) {
        lua_pushstring(state, files[i].c_str());
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// resources.lua graphics.maps.watertex, or Recoil's "ocean.jpg" when the game sets none.
std::string resources_watertex(const std::string& game_root) {
    std::string tex = "ocean.jpg";
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
        return tex;
    }
    luaL_openlibs(state);
    lua_pushstring(state, game_root.c_str());
    lua_setfield(state, LUA_REGISTRYINDEX, "bar_game_root");
    lua_newtable(state);
    lua_pushcfunction(state, vfs_dir_list);
    lua_setfield(state, -2, "DirList");
    lua_setglobal(state, "VFS");
    const std::string path = game_root + "/gamedata/resources.lua";
    if (luaL_dofile(state, path.c_str()) == 0 && lua_istable(state, -1)) {
        lua_getfield(state, -1, "graphics");
        if (lua_istable(state, -1)) {
            lua_getfield(state, -1, "maps");
            if (lua_istable(state, -1)) {
                lua_getfield(state, -1, "watertex");
                if (lua_isstring(state, -1)) {
                    tex = lua_tostring(state, -1);
                }
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    lua_close(state);
    return tex;
}

bool read_disk_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.empty();
}

// MapInfo: FIND_MAP_TEXTURE(texture) (else "maps/" + texture) in the map; with no texture key,
// FIND_MAP_TEXTURE(watertex, "bitmaps/") in the game VFS.
void load_water_texture(const TerrainMesh& mesh, TerrainTextures& out) {
    std::vector<uint8_t> bytes;
    if (!mesh.water.texture.empty()) {
        if (read_map_file(mesh.archive_path, mesh.water.texture, bytes)) {
            out.water_path = mesh.water.texture;
        }
    } else if (!mesh.game_root.empty()) {
        const std::string tex = resources_watertex(mesh.game_root);
        if (read_disk_file(mesh.game_root + "/" + tex, bytes)) {
            out.water_path = tex;
        } else if (read_disk_file(mesh.game_root + "/bitmaps/" + tex, bytes)) {
            out.water_path = "bitmaps/" + tex;
        }
    }
    if (bytes.empty()) {
        return;
    }
    const std::string ext = lower_ext(out.water_path);
    std::string ignored;
    bool ok = false;
    if (ext == "dds") {
        ok = decode_dds_blocks(bytes, out.water, ignored);
    } else if (ext == "tga") {
        ok = decode_tga(bytes, out.water, ignored);
        if (ok) {
            build_rgba_mips(out.water);
        }
    } else {
        ok = decode_imageio_rgba(bytes, out.water);
        if (ok) {
            build_rgba_mips(out.water);
        }
    }
    if (!ok) {
        out.water = GpuImage{};
        out.water_path.clear();
    }
}

}  // namespace

bool load_terrain_textures(const TerrainMesh& mesh, TerrainTextures& out, std::string& error) {
    if (!load_diffuse(mesh, out.diffuse, error)) {
        return false;
    }
    out.normals.format = GpuImage::Format::RG16F;
    out.normals.width = mesh.map_x + 1;
    out.normals.height = mesh.map_y + 1;
    out.normals.levels.assign(1, std::vector<uint8_t>(mesh.normals_xz.size() * 2));
    std::memcpy(out.normals.levels[0].data(), mesh.normals_xz.data(), mesh.normals_xz.size() * 2);

    load_water_texture(mesh, out);
    if (!mesh.game_root.empty()) {
        std::vector<uint8_t> foam;
        std::string foam_error;
        if (read_disk_file(mesh.game_root + "/bitmaps/atmos/waterfoam.tga", foam) &&
            decode_tga(foam, out.water_foam, foam_error)) {
            build_rgba_mips(out.water_foam);
        } else {
            out.water_foam = GpuImage{};
        }
    }
    out.water_normals = generate_wave_normals(256, 1u);
    out.water_caustics = generate_caustics(256, 2u);
    if (!mesh.sky_box.empty()) {
        std::string sky_error;
        if (!load_image(mesh.archive_path, mesh.sky_box, out.sky_box, sky_error) || out.sky_box.faces != 6) {
            out.sky_box = GpuImage{};
        }
    }

    // Optional maps: a missing or unreadable file disables that SMF feature, as when
    // Recoil's texture id stays 0 (specularTex falls back to a white dummy there).
    std::string ignored;
    if (!mesh.specular_tex.empty() && !load_image(mesh.archive_path, mesh.specular_tex, out.specular, ignored)) {
        out.specular.format = GpuImage::Format::RGBA8;
        out.specular.width = out.specular.height = 1;
        out.specular.levels.assign(1, std::vector<uint8_t>{255, 255, 255, 255});
    }
    if (!mesh.blend_normals_tex.empty()) {
        load_image(mesh.archive_path, mesh.blend_normals_tex, out.blend_normals, ignored);
    }
    if (!mesh.splat_distr_tex.empty() && !mesh.splat_normal_tex.empty()) {
        if (!load_image(mesh.archive_path, mesh.splat_distr_tex, out.splat_distr, ignored)) {
            // Recoil fallback distribution: SColor(255, 0, 0, 0).
            out.splat_distr.format = GpuImage::Format::RGBA8;
            out.splat_distr.width = out.splat_distr.height = 1;
            out.splat_distr.levels.assign(1, std::vector<uint8_t>{255, 0, 0, 0});
        }
        out.splat_normal_count = static_cast<int>(std::min<size_t>(4, mesh.splat_normal_tex.size()));
        for (int i = 0; i < out.splat_normal_count; ++i) {
            if (!load_image(mesh.archive_path, mesh.splat_normal_tex[i], out.splat_normals[i], ignored)) {
                // Recoil fallback: one texel (127, 127, 255, 127).
                out.splat_normals[i].format = GpuImage::Format::RGBA8;
                out.splat_normals[i].width = out.splat_normals[i].height = 1;
                out.splat_normals[i].levels.assign(1, std::vector<uint8_t>{127, 127, 255, 127});
            }
        }
    }
    return true;
}

}  // namespace bar
