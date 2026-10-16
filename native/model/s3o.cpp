#include "s3o.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bar {
namespace {

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

std::string lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string cstr_at(const std::vector<uint8_t>& file, int offset) {
    if (offset < 0 || static_cast<size_t>(offset) >= file.size()) {
        return {};
    }
    const auto end = std::find(file.begin() + offset, file.end(), 0);
    return std::string(file.begin() + offset, end);
}

bool in_file(const std::vector<uint8_t>& file, int offset, size_t bytes) {
    return offset >= 0 && static_cast<size_t>(offset) + bytes <= file.size();
}

bool emit_indices(const std::vector<uint8_t>& file, int table, int count, int primitive, uint32_t base, int verts,
                  std::vector<uint32_t>& indices, std::string& error) {
    if (count <= 0) {
        return true;
    }
    if (!in_file(file, table, static_cast<size_t>(count) * 4)) {
        error = "s3o index table falls outside the file";
        return false;
    }
    auto at = [&](int i) {
        return read_i32(file.data() + table + i * 4);
    };
    auto push = [&](int a, int b, int c) {
        if (a < 0 || b < 0 || c < 0 || a >= verts || b >= verts || c >= verts) {
            return false;
        }
        indices.push_back(base + static_cast<uint32_t>(a));
        indices.push_back(base + static_cast<uint32_t>(b));
        indices.push_back(base + static_cast<uint32_t>(c));
        return true;
    };
    if (primitive == 0) {
        if (count % 3 != 0) {
            error = "s3o triangle list length is not a multiple of 3";
            return false;
        }
        for (int i = 0; i < count; i += 3) {
            if (!push(at(i), at(i + 1), at(i + 2))) {
                error = "s3o index out of range";
                return false;
            }
        }
        return true;
    }
    if (primitive == 1) {
        for (int i = 0; i + 2 < count; ++i) {
            int a = at(i);
            int b = at(i + 1);
            int c = at(i + 2);
            if ((i & 1) != 0) {
                std::swap(b, c);
            }
            if (a == b || b == c || a == c) {
                continue;
            }
            if (!push(a, b, c)) {
                error = "s3o strip index out of range";
                return false;
            }
        }
        return true;
    }
    if (primitive == 2) {
        if (count % 4 != 0) {
            error = "s3o quad list length is not a multiple of 4";
            return false;
        }
        for (int i = 0; i < count; i += 4) {
            if (!push(at(i), at(i + 1), at(i + 2)) || !push(at(i), at(i + 2), at(i + 3))) {
                error = "s3o quad index out of range";
                return false;
            }
        }
        return true;
    }
    error = "s3o primitive type is not triangles, a strip, or quads";
    return false;
}

bool emit_piece(const std::vector<uint8_t>& file, int offset, int parent, float ax, float ay, float az, ModelMesh& mesh,
                std::string& error) {
    if (!in_file(file, offset, 52)) {
        error = "s3o piece falls outside the file";
        return false;
    }
    if (mesh.pieces.size() >= 128) {
        error = "s3o has more than 128 pieces";
        return false;
    }
    const uint8_t* piece = file.data() + offset;
    const int child_count = read_i32(piece + 4);
    const int child_table = read_i32(piece + 8);
    const int vert_count = read_i32(piece + 12);
    const int vert_table = read_i32(piece + 16);
    const int primitive = read_i32(piece + 24);
    const int index_count = read_i32(piece + 28);
    const int index_table = read_i32(piece + 32);
    const float lx = read_f32(piece + 40);
    const float ly = read_f32(piece + 44);
    const float lz = read_f32(piece + 48);
    const float bx = ax + lx;
    const float by = ay + ly;
    const float bz = az + lz;
    const int index = static_cast<int>(mesh.pieces.size());
    ModelPiece node;
    node.name = lower_copy(cstr_at(file, read_i32(piece)));
    node.parent = parent;
    node.ox = lx;
    node.oy = ly;
    node.oz = lz;
    node.bind_x = bx;
    node.bind_y = by;
    node.bind_z = bz;
    mesh.pieces.push_back(node);

    // Pieces parked under the model (medals, crown) keep a node so the script can name them.
    if (vert_count > 0 && by >= -1.f) {
        if (!in_file(file, vert_table, static_cast<size_t>(vert_count) * 32)) {
            error = "s3o vertices fall outside the file";
            return false;
        }
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.reserve(mesh.vertices.size() + static_cast<size_t>(vert_count));
        for (int i = 0; i < vert_count; ++i) {
            const uint8_t* vertex = file.data() + vert_table + i * 32;
            TerrainVertex out{};
            out.px = read_f32(vertex);
            out.py = read_f32(vertex + 4);
            out.pz = read_f32(vertex + 8);
            out.pad0 = static_cast<float>(index);
            out.nx = read_f32(vertex + 12);
            out.ny = read_f32(vertex + 16);
            out.nz = read_f32(vertex + 20);
            const float length = std::sqrt(out.nx * out.nx + out.ny * out.ny + out.nz * out.nz);
            if (length > 1e-6f) {
                out.nx /= length;
                out.ny /= length;
                out.nz /= length;
            } else {
                out.ny = 1;
            }
            out.u = read_f32(vertex + 24);
            out.v = read_f32(vertex + 28);
            mesh.vertices.push_back(out);
        }
        if (!emit_indices(file, index_table, index_count, primitive, base, vert_count, mesh.indices, error)) {
            return false;
        }
    }

    if (child_count > 0) {
        if (!in_file(file, child_table, static_cast<size_t>(child_count) * 4)) {
            error = "s3o child table falls outside the file";
            return false;
        }
        for (int i = 0; i < child_count; ++i) {
            if (!emit_piece(file, read_i32(file.data() + child_table + i * 4), index, bx, by, bz, mesh, error)) {
                return false;
            }
        }
    }
    return true;
}

bool piece_chain_matches(const ModelMesh& mesh, std::string& error) {
    for (int index = 0; index < static_cast<int>(mesh.pieces.size()); ++index) {
        float x = 0;
        float y = 0;
        float z = 0;
        int cursor = index;
        int guard = 0;
        while (cursor >= 0) {
            if (cursor >= static_cast<int>(mesh.pieces.size()) || guard++ > 128) {
                error = "s3o piece parent chain is broken";
                return false;
            }
            x += mesh.pieces[static_cast<size_t>(cursor)].ox;
            y += mesh.pieces[static_cast<size_t>(cursor)].oy;
            z += mesh.pieces[static_cast<size_t>(cursor)].oz;
            cursor = mesh.pieces[static_cast<size_t>(cursor)].parent;
        }
        const ModelPiece& piece = mesh.pieces[static_cast<size_t>(index)];
        if (std::fabs(x - piece.bind_x) > 1e-3f || std::fabs(y - piece.bind_y) > 1e-3f ||
            std::fabs(z - piece.bind_z) > 1e-3f) {
            error = "s3o piece " + piece.name + " does not match its parent chain";
            return false;
        }
    }
    return true;
}

bool extract_bc3(const std::vector<uint8_t>& dds, std::vector<uint8_t>& mip0, int& size, std::string& error) {
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) {
        error = "unit texture is not a DDS file";
        return false;
    }
    const int height = read_i32(dds.data() + 12);
    const int width = read_i32(dds.data() + 16);
    if (width < 4 || height != width || (width % 4) != 0 || std::memcmp(dds.data() + 84, "DXT5", 4) != 0) {
        error = "unit texture is not a square DXT5 image";
        return false;
    }
    const size_t bytes = static_cast<size_t>(width / 4) * static_cast<size_t>(height / 4) * 16;
    if (dds.size() < 128 + bytes) {
        error = "DXT5 mip is truncated";
        return false;
    }
    mip0.assign(dds.begin() + 128, dds.begin() + 128 + static_cast<std::ptrdiff_t>(bytes));
    size = width;
    return true;
}

bool read_texture(const GameContent& content, const std::string& name, std::vector<uint8_t>& mip0, int& size,
                  std::string& error) {
    const std::string path = "unittextures/" + lower_copy(name);
    std::vector<uint8_t> dds;
    if (!content.read(path, dds, error)) {
        return false;
    }
    return extract_bc3(dds, mip0, size, error);
}

}  // namespace

bool load_s3o(const GameContent& content, const std::string& object_name, ModelMesh& mesh, std::string& error) {
    const std::string lower = lower_copy(object_name);
    const auto slash = lower.find_last_of('/');
    const std::string file_name = slash == std::string::npos ? lower : lower.substr(slash + 1);
    const std::string candidates[] = {
        "objects3d/" + lower,
        "objects3d/units/" + file_name,
        "objects3d/" + file_name,
        lower,
    };
    std::vector<uint8_t> file;
    bool found = false;
    for (const std::string& candidate : candidates) {
        std::string read_error;
        if (content.read(candidate, file, read_error)) {
            found = true;
            break;
        }
        error = read_error;
    }
    if (!found) {
        return false;
    }
    if (file.size() < 68 || std::memcmp(file.data(), "Spring unit", 11) != 0) {
        error = "not an s3o: " + object_name;
        return false;
    }
    const int root = read_i32(file.data() + 36);
    mesh = ModelMesh{};
    if (!emit_piece(file, root, -1, 0, 0, 0, mesh, error) || !piece_chain_matches(mesh, error)) {
        return false;
    }
    if (mesh.vertices.size() < 100 || mesh.indices.size() < 100 || mesh.indices.size() % 3 != 0) {
        error = "s3o mesh is empty or not triangulated";
        return false;
    }
    float min_y = 0;
    float max_y = 0;
    bool have_y = false;
    for (const TerrainVertex& vertex : mesh.vertices) {
        const int piece = static_cast<int>(vertex.pad0 + 0.5f);
        if (piece < 0 || piece >= static_cast<int>(mesh.pieces.size())) {
            error = "s3o vertex names a missing piece";
            return false;
        }
        const float y = mesh.pieces[static_cast<size_t>(piece)].bind_y + vertex.py;
        if (!have_y) {
            min_y = max_y = y;
            have_y = true;
        } else {
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    // Commander meshes sit in a tighter band. Ships extend below the waterline, so the check
    // only rejects an empty or exploded bind pose. The commander still passes this range.
    const float span = max_y - min_y;
    if (!have_y || !(min_y > -80.f) || !(span > 8.f) || !(span < 400.f) || !(max_y < 200.f)) {
        error = "s3o bind pose height is not a unit mesh";
        return false;
    }

    const std::string tex1 = cstr_at(file, read_i32(file.data() + 44));
    const std::string tex2 = cstr_at(file, read_i32(file.data() + 48));
    int color_size = 0;
    int other_size = 0;
    if (!read_texture(content, tex1, mesh.color_bc3, color_size, error) ||
        !read_texture(content, tex2, mesh.other_bc3, other_size, error)) {
        return false;
    }
    if (color_size != other_size) {
        error = "commander textures differ in size";
        return false;
    }
    mesh.tex_size = color_size;
    return true;
}

bool load_model_normal(const GameContent& content, const std::string& path, ModelMesh& mesh, std::string& error) {
    std::vector<uint8_t> dds;
    if (!content.read(lower_copy(path), dds, error) && !content.read(path, dds, error)) {
        return false;
    }
    return extract_bc3(dds, mesh.normal_bc3, mesh.normal_size, error);
}

}  // namespace bar
