#include "gl/gl_api.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bar {
namespace {

constexpr int kModelview = 0x1700;
constexpr int kTexture = 0x1702;
constexpr int kFill = 0x1B02;
constexpr int kLine = 0x1B01;
constexpr int kFrontAndBack = 0x0408;
constexpr float kPi = 3.14159265f;

struct Mat {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Mat identity_mat() {
    return Mat{};
}

Mat mul(const Mat& a, const Mat& b) {
    Mat r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r.m[col * 4 + row] = a.m[row] * b.m[col * 4] + a.m[4 + row] * b.m[col * 4 + 1] +
                                 a.m[8 + row] * b.m[col * 4 + 2] + a.m[12 + row] * b.m[col * 4 + 3];
        }
    }
    return r;
}

void transform_point(const Mat& mat, float x, float y, float z, float& ox, float& oy) {
    ox = mat.m[0] * x + mat.m[4] * y + mat.m[8] * z + mat.m[12];
    oy = mat.m[1] * x + mat.m[5] * y + mat.m[9] * z + mat.m[13];
}

float scale_y(const Mat& mat) {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    transform_point(mat, 0, 0, 0, x0, y0);
    transform_point(mat, 0, 1, 0, x1, y1);
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    return std::sqrt(dx * dx + dy * dy);
}

enum class Kind : uint8_t { Vertex, TexRect, Push, Pop, Translate, Scale, Rotate, LoadId, Text, Call, Mode };

struct Op {
    Kind kind = Kind::Vertex;
    float f[8] = {};
    int i = 0;
    std::string s;
    std::string s2;
};

struct Geo {
    std::vector<HudVertex> verts;
    std::vector<HudText> texts;
    int width = 0;
    int height = 0;
    bool ndc = false;
};

struct List {
    std::vector<Op> ops;
};

struct Corner {
    float x = 0;
    float y = 0;
    float z = 0;
    float r = 1;
    float g = 1;
    float b = 1;
    float a = 1;
};

struct Recorder {
    std::string root;
    int view_w = 1280;
    int view_h = 720;
    Mat model = identity_mat();
    Mat texture = identity_mat();
    std::vector<Mat> model_stack;
    std::vector<Mat> texture_stack;
    int mode = kModelview;
    float cr = 1;
    float cg = 1;
    float cb = 1;
    float ca = 1;
    int polygon_mode = 0x1B02;  // GL_FILL
    float line_width = 1;
    bool line_stipple = false;
    int bind = 0;
    // True after Texture(":path:") / non-numeric bind we cannot load. Vertex quads
    // (DrawRect glow, TexturedRectRound tiles) must not fall back to solid color —
    // that painted barHeight*7 glow slabs as oversized grey rectangles around the top bar.
    bool texture_missing = false;
    int recording = 0;
    int next_list = 1;
    int next_tex = 1;
    bool scissor_on = false;
    float sx = 0;
    float sy = 0;
    float sw = 0;
    float sh = 0;
    Geo screen;
    std::unordered_map<int, Geo> textures;
    std::unordered_map<int, List> lists;
    std::vector<Corner> batch;
    HudList published;
};

Recorder g;

bool finite_xy(float x, float y) {
    return std::isfinite(x) && std::isfinite(y);
}

Mat* active_matrix() {
    return g.mode == kTexture ? &g.texture : &g.model;
}

std::vector<Mat>* active_stack() {
    return g.mode == kTexture ? &g.texture_stack : &g.model_stack;
}

int g_target = 0;

Geo g_sink;

Geo* target_geo() {
    if (g_target == 0) {
        return &g.screen;
    }
    const auto at = g.textures.find(g_target);
    if (at == g.textures.end()) {
        // Never fall back to the screen: a stale R2T id used to wipe HUD verts via gl.Clear.
        g_sink.verts.clear();
        g_sink.texts.clear();
        return &g_sink;
    }
    return &at->second;
}

void push_quad(const Corner& a, const Corner& b, const Corner& c, const Corner& d) {
    const Corner* corners[4] = {&a, &b, &c, &d};
    HudVertex out[4];
    for (int i = 0; i < 4; ++i) {
        float x = 0;
        float y = 0;
        transform_point(g.model, corners[i]->x, corners[i]->y, corners[i]->z, x, y);
        if (!finite_xy(x, y)) {
            return;
        }
        out[i].x = x;
        out[i].y = y;
        out[i].r = corners[i]->r;
        out[i].g = corners[i]->g;
        out[i].b = corners[i]->b;
        out[i].a = corners[i]->a;
    }
    Geo* geo = target_geo();
    for (const HudVertex& vert : out) {
        geo->verts.push_back(vert);
    }
}

void push_text(const HudText& text) {
    float x = 0;
    float y = 0;
    transform_point(g.model, text.x, text.y, 0, x, y);
    if (!finite_xy(x, y)) {
        return;
    }
    HudText out = text;
    out.x = x;
    out.y = y;
    out.size = text.size * scale_y(g.model);
    if (!(out.size > 0.f) || !std::isfinite(out.size)) {
        return;
    }
    target_geo()->texts.push_back(std::move(out));
}

void flush_batch() {
    for (size_t i = 0; i + 3 < g.batch.size(); i += 4) {
        push_quad(g.batch[i], g.batch[i + 1], g.batch[i + 2], g.batch[i + 3]);
    }
    g.batch.clear();
}

void replay_texture(int id, float x1, float y1, float x2, float y2, float r, float gch, float b, float a) {
    const auto at = g.textures.find(id);
    if (at == g.textures.end()) {
        return;
    }
    const Geo& src = at->second;
    const float left = std::min(x1, x2);
    const float right = std::max(x1, x2);
    const float bottom = std::min(y1, y2);
    const float top = std::max(y1, y2);
    const float width = right - left;
    const float height = top - bottom;
    auto map_x = [&](float ndc) { return (ndc * 0.5f + 0.5f) * width + left; };
    auto map_y = [&](float ndc) { return (ndc * 0.5f + 0.5f) * height + bottom; };
    Geo* geo = target_geo();
    for (size_t i = 0; i + 3 < src.verts.size(); i += 4) {
        Corner corners[4];
        for (int k = 0; k < 4; ++k) {
            const HudVertex& vert = src.verts[i + static_cast<size_t>(k)];
            corners[k].x = map_x(vert.x);
            corners[k].y = map_y(vert.y);
            corners[k].r = vert.r * r;
            corners[k].g = vert.g * gch;
            corners[k].b = vert.b * b;
            corners[k].a = vert.a * a;
        }
        push_quad(corners[0], corners[1], corners[2], corners[3]);
    }
    for (const HudText& text : src.texts) {
        HudText mapped = text;
        mapped.x = map_x(text.x);
        mapped.y = map_y(text.y);
        mapped.size = text.size * height * 0.5f;
        mapped.r *= r;
        mapped.g *= gch;
        mapped.b *= b;
        mapped.a *= a;
        float x = 0;
        float y = 0;
        transform_point(g.model, mapped.x, mapped.y, 0, x, y);
        if (!finite_xy(x, y) || !(mapped.size > 0.f)) {
            continue;
        }
        mapped.x = x;
        mapped.y = y;
        mapped.size *= scale_y(g.model);
        geo->texts.push_back(std::move(mapped));
    }
}

void color_rect(float x1, float y1, float x2, float y2, float r, float gch, float b, float a) {
    Corner corners[4];
    const float xs[4] = {x1, x2, x2, x1};
    const float ys[4] = {y1, y1, y2, y2};
    for (int i = 0; i < 4; ++i) {
        corners[i].x = xs[i];
        corners[i].y = ys[i];
        corners[i].r = r;
        corners[i].g = gch;
        corners[i].b = b;
        corners[i].a = a;
    }
    push_quad(corners[0], corners[1], corners[2], corners[3]);
}

void outline_rect(float x1, float y1, float x2, float y2, float r, float gch, float b, float a, float width) {
    const float left = std::min(x1, x2);
    const float right = std::max(x1, x2);
    const float bottom = std::min(y1, y2);
    const float top = std::max(y1, y2);
    const float t = std::max(1.f, width);
    color_rect(left, bottom, right, bottom + t, r, gch, b, a);
    color_rect(left, top - t, right, top, r, gch, b, a);
    color_rect(left, bottom, left + t, top, r, gch, b, a);
    color_rect(right - t, bottom, right, top, r, gch, b, a);
}


void apply_translate(float x, float y, float z) {
    Mat t = identity_mat();
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    *active_matrix() = mul(*active_matrix(), t);
}

void apply_scale(float x, float y, float z) {
    Mat s = identity_mat();
    s.m[0] = x;
    s.m[5] = y;
    s.m[10] = z;
    *active_matrix() = mul(*active_matrix(), s);
}

void apply_rotate(float deg, float ax, float ay, float az) {
    if (std::fabs(az) < 0.5f && (std::fabs(ax) > 0.5f || std::fabs(ay) > 0.5f)) {
        return;
    }
    const float rad = deg * kPi / 180.f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat r = identity_mat();
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    *active_matrix() = mul(*active_matrix(), r);
}

void apply_load_id() {
    *active_matrix() = identity_mat();
}

void apply_push() {
    active_stack()->push_back(*active_matrix());
}

void apply_pop() {
    std::vector<Mat>* stack = active_stack();
    if (stack->empty()) {
        return;
    }
    *active_matrix() = stack->back();
    stack->pop_back();
}

void exec_list(int id);

void play(const Op& op) {
    switch (op.kind) {
        case Kind::Vertex: {
            Corner corner;
            corner.x = op.f[0];
            corner.y = op.f[1];
            corner.z = op.f[2];
            corner.r = op.f[3];
            corner.g = op.f[4];
            corner.b = op.f[5];
            corner.a = op.f[6];
            g.batch.push_back(corner);
            if (g.batch.size() == 4) {
                flush_batch();
            }
            break;
        }
        case Kind::TexRect:
            flush_batch();
            if (op.i > 0) {
                replay_texture(op.i, op.f[0], op.f[1], op.f[2], op.f[3], op.f[4], op.f[5], op.f[6], op.f[7]);
            }
            // i==0: skip (see note_texrect); do not invent a solid rotating square.
            break;
        case Kind::Push:
            flush_batch();
            apply_push();
            break;
        case Kind::Pop:
            flush_batch();
            apply_pop();
            break;
        case Kind::Translate:
            flush_batch();
            apply_translate(op.f[0], op.f[1], op.f[2]);
            break;
        case Kind::Scale:
            flush_batch();
            apply_scale(op.f[0], op.f[1], op.f[2]);
            break;
        case Kind::Rotate:
            flush_batch();
            apply_rotate(op.f[0], op.f[1], op.f[2], op.f[3]);
            break;
        case Kind::LoadId:
            flush_batch();
            apply_load_id();
            break;
        case Kind::Text: {
            flush_batch();
            HudText text;
            text.text = op.s;
            text.font_path = op.s2;
            text.x = op.f[0];
            text.y = op.f[1];
            text.size = op.f[2];
            text.r = op.f[3];
            text.g = op.f[4];
            text.b = op.f[5];
            text.a = op.f[6];
            text.align = op.i;
            text.outline = op.f[7] > 0.5f;
            push_text(text);
            break;
        }
        case Kind::Call:
            flush_batch();
            exec_list(op.i);
            break;
        case Kind::Mode:
            flush_batch();
            g.mode = op.i;
            break;
    }
}

void exec_list(int id) {
    const auto at = g.lists.find(id);
    if (at == g.lists.end()) {
        return;
    }
    // Copy the ops so a list that deletes itself cannot invalidate the walk.
    const std::vector<Op> ops = at->second.ops;
    for (const Op& op : ops) {
        play(op);
    }
    flush_batch();
}

void add_op(Op op) {
    g.lists[g.recording].ops.push_back(std::move(op));
}

void note_vertex(float x, float y, float z) {
    if (g.texture_missing) {
        return;
    }
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Vertex;
        op.f[0] = x;
        op.f[1] = y;
        op.f[2] = z;
        op.f[3] = g.cr;
        op.f[4] = g.cg;
        op.f[5] = g.cb;
        op.f[6] = g.ca;
        add_op(std::move(op));
        return;
    }
    Corner corner;
    corner.x = x;
    corner.y = y;
    corner.z = z;
    corner.r = g.cr;
    corner.g = g.cg;
    corner.b = g.cb;
    corner.a = g.ca;
    g.batch.push_back(corner);
    if (g.batch.size() == 4) {
        flush_batch();
    }
}

void note_texrect(float x1, float y1, float x2, float y2) {
    if (g.texture_missing) {
        return;
    }
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::TexRect;
        op.f[0] = x1;
        op.f[1] = y1;
        op.f[2] = x2;
        op.f[3] = y2;
        op.f[4] = g.cr;
        op.f[5] = g.cg;
        op.f[6] = g.cb;
        op.f[7] = g.ca;
        op.i = g.bind;
        add_op(std::move(op));
        return;
    }
    if (g.bind > 0) {
        replay_texture(g.bind, x1, y1, x2, y2, g.cr, g.cg, g.cb, g.ca);
    }
    // Unbound TexRect used to fill a solid color quad. That turned the wind-blades
    // icon (Texture(":n:...png") is not a numeric bind) into a rotating white square.
}

void note_text(HudText text) {
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Text;
        op.f[0] = text.x;
        op.f[1] = text.y;
        op.f[2] = text.size;
        op.f[3] = text.r;
        op.f[4] = text.g;
        op.f[5] = text.b;
        op.f[6] = text.a;
        op.f[7] = text.outline ? 1.f : 0.f;
        op.i = text.align;
        op.s = std::move(text.text);
        op.s2 = std::move(text.font_path);
        add_op(std::move(op));
        return;
    }
    push_text(text);
}

float lua_num(lua_State* state, int index, float fallback) {
    if (lua_isnumber(state, index) == 0) {
        return fallback;
    }
    return static_cast<float>(lua_tonumber(state, index));
}

// Spring inline colors: \255 r g b (byte 255 + 3 channels). \008 resets.
// Strip for GetTextWidth / empty checks. Print keeps codes; TerrainRenderer applies runs.
std::string visible_text(const char* raw, size_t length, float& r, float& gch, float& b, float& a) {
    std::string out;
    out.reserve(length);
    size_t i = 0;
    while (i < length) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 255 && i + 3 < length) {
            r = static_cast<unsigned char>(raw[i + 1]) / 255.f;
            gch = static_cast<unsigned char>(raw[i + 2]) / 255.f;
            b = static_cast<unsigned char>(raw[i + 3]) / 255.f;
            a = 1.f;
            i += 4;
            continue;
        }
        if (c == 8) {
            r = 1.f;
            gch = 1.f;
            b = 1.f;
            a = 1.f;
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
    return out;
}

std::string font_file(lua_State* state, int self) {
    lua_getfield(state, self, "path");
    const char* path = lua_tostring(state, -1);
    std::string full;
    if (path != nullptr && path[0] != '\0') {
        full = g.root.empty() ? path : g.root + "/" + path;
    }
    lua_pop(state, 1);
    return full;
}

float field_num(lua_State* state, int self, const char* key, float fallback) {
    lua_getfield(state, self, key);
    const float value = lua_isnumber(state, -1) != 0 ? static_cast<float>(lua_tonumber(state, -1)) : fallback;
    lua_pop(state, 1);
    return value;
}

float table_channel(lua_State* state, int table, int field, float fallback) {
    lua_rawgeti(state, table, field);
    const float value = lua_num(state, -1, fallback);
    lua_pop(state, 1);
    return value;
}

int gl_color(lua_State* state) {
    // FlowUI passes gl.Color({r, g, b, a}). A missing channel keeps the numeric default.
    if (lua_istable(state, 1) != 0) {
        g.cr = table_channel(state, 1, 1, 1);
        g.cg = table_channel(state, 1, 2, 1);
        g.cb = table_channel(state, 1, 3, 1);
        g.ca = table_channel(state, 1, 4, 1);
        return 0;
    }
    g.cr = lua_num(state, 1, 1);
    g.cg = lua_num(state, 2, 1);
    g.cb = lua_num(state, 3, 1);
    g.ca = lua_num(state, 4, 1);
    return 0;
}

int gl_texture(lua_State* state) {
    if (lua_isnumber(state, 1) != 0) {
        g.bind = static_cast<int>(lua_tonumber(state, 1));
        g.texture_missing = false;
        return 0;
    }
    // Texture(false) / nil: solid draws OK. String path: we have no image loader, so
    // mark missing and skip subsequent Vertex batches (see note_vertex).
    g.bind = 0;
    if (lua_isboolean(state, 1) != 0 || lua_isnil(state, 1) != 0 || lua_gettop(state) < 1) {
        g.texture_missing = false;
    } else {
        g.texture_missing = true;
    }
    return 0;
}

int gl_texrect(lua_State* state) {
    note_texrect(lua_num(state, 1, 0), lua_num(state, 2, 0), lua_num(state, 3, 0), lua_num(state, 4, 0));
    return 0;
}

int gl_vertex(lua_State* state) {
    note_vertex(lua_num(state, 1, 0), lua_num(state, 2, 0), lua_num(state, 3, 0));
    return 0;
}

int gl_texcoord(lua_State* state) {
    (void)state;
    return 0;
}

int gl_begin_end(lua_State* state) {
    if (lua_isfunction(state, 2) == 0) {
        return luaL_error(state, "gl.BeginEnd expected a function");
    }
    const int top = lua_gettop(state);
    lua_pushvalue(state, 2);
    for (int i = 3; i <= top; ++i) {
        lua_pushvalue(state, i);
    }
    if (lua_pcall(state, top - 2, 0, 0) != 0) {
        return lua_error(state);
    }
    flush_batch();
    return 0;
}

int gl_create_list(lua_State* state) {
    if (lua_isfunction(state, 1) == 0) {
        return luaL_error(state, "gl.CreateList expected a function");
    }
    const int id = g.next_list++;
    g.lists[id] = List{};
    const int previous = g.recording;
    g.recording = id;
    const int top = lua_gettop(state);
    lua_pushvalue(state, 1);
    for (int i = 2; i <= top; ++i) {
        lua_pushvalue(state, i);
    }
    const int rc = lua_pcall(state, top - 1, 0, 0);
    g.recording = previous;
    g.batch.clear();
    // CreateList may end after Texture(":path:") without Texture(false); do not leak
    // texture_missing into later lists (would drop solid RectRound bar fills).
    g.texture_missing = false;
    if (rc != 0) {
        return lua_error(state);
    }
    lua_pushinteger(state, id);
    return 1;
}

int gl_call_list(lua_State* state) {
    if (lua_isnumber(state, 1) == 0) {
        return 0;
    }
    const int id = static_cast<int>(lua_tonumber(state, 1));
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Call;
        op.i = id;
        add_op(std::move(op));
        return 0;
    }
    exec_list(id);
    return 0;
}

int gl_delete_list(lua_State* state) {
    if (lua_isnumber(state, 1) != 0) {
        g.lists.erase(static_cast<int>(lua_tonumber(state, 1)));
    }
    return 0;
}

int gl_matrix_op(lua_State* state) {
    (void)state;
    return 0;
}

int gl_push(lua_State* state) {
    (void)state;
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Push;
        add_op(std::move(op));
        return 0;
    }
    apply_push();
    return 0;
}

int gl_pop(lua_State* state) {
    (void)state;
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Pop;
        add_op(std::move(op));
        return 0;
    }
    apply_pop();
    return 0;
}

int gl_translate(lua_State* state) {
    const float x = lua_num(state, 1, 0);
    const float y = lua_num(state, 2, 0);
    const float z = lua_num(state, 3, 0);
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Translate;
        op.f[0] = x;
        op.f[1] = y;
        op.f[2] = z;
        add_op(std::move(op));
        return 0;
    }
    apply_translate(x, y, z);
    return 0;
}

int gl_scale(lua_State* state) {
    const float x = lua_num(state, 1, 1);
    const float y = lua_num(state, 2, 1);
    const float z = lua_num(state, 3, 1);
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Scale;
        op.f[0] = x;
        op.f[1] = y;
        op.f[2] = z;
        add_op(std::move(op));
        return 0;
    }
    apply_scale(x, y, z);
    return 0;
}

int gl_rotate(lua_State* state) {
    const float deg = lua_num(state, 1, 0);
    const float ax = lua_num(state, 2, 0);
    const float ay = lua_num(state, 3, 0);
    const float az = lua_num(state, 4, 1);
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Rotate;
        op.f[0] = deg;
        op.f[1] = ax;
        op.f[2] = ay;
        op.f[3] = az;
        add_op(std::move(op));
        return 0;
    }
    apply_rotate(deg, ax, ay, az);
    return 0;
}

int gl_load_id(lua_State* state) {
    (void)state;
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::LoadId;
        add_op(std::move(op));
        return 0;
    }
    apply_load_id();
    return 0;
}

int gl_matrix_mode(lua_State* state) {
    const int mode = lua_isnumber(state, 1) != 0 ? static_cast<int>(lua_tonumber(state, 1)) : kModelview;
    if (g.recording != 0) {
        Op op;
        op.kind = Kind::Mode;
        op.i = mode;
        add_op(std::move(op));
        return 0;
    }
    g.mode = mode;
    return 0;
}

int gl_view_sizes(lua_State* state) {
    lua_pushinteger(state, g.view_w);
    lua_pushinteger(state, g.view_h);
    return 2;
}

int gl_create_texture(lua_State* state) {
    const int width = std::max(1, static_cast<int>(lua_num(state, 1, 1)));
    const int height = std::max(1, static_cast<int>(lua_num(state, 2, 1)));
    const int id = g.next_tex++;
    Geo geo;
    geo.width = width;
    geo.height = height;
    geo.ndc = true;
    g.textures[id] = std::move(geo);
    lua_pushinteger(state, id);
    return 1;
}

int gl_delete_texture(lua_State* state) {
    if (lua_isnumber(state, 1) != 0) {
        const int id = static_cast<int>(lua_tonumber(state, 1));
        g.textures.erase(id);
        if (g.bind == id) {
            g.bind = 0;
        }
        if (g_target == id) {
            g_target = 0;
        }
    }
    return 0;
}

int gl_render_to_texture(lua_State* state) {
    if (lua_isnumber(state, 1) == 0 || lua_isfunction(state, 2) == 0) {
        return 0;
    }
    const int id = static_cast<int>(lua_tonumber(state, 1));
    if (g.textures.find(id) == g.textures.end()) {
        return 0;
    }
    const int previous = g_target;
    g_target = id;
    lua_pushvalue(state, 2);
    const int rc = lua_pcall(state, 0, 0, 0);
    g_target = previous;
    flush_batch();
    if (rc != 0) {
        return lua_error(state);
    }
    return 0;
}

void erase_scissor_region(Geo& geo) {
    if (geo.width <= 0 || geo.height <= 0) {
        return;
    }
    const float x0 = g.sx;
    const float y0 = g.sy;
    const float x1 = g.sx + g.sw;
    const float y1 = g.sy + g.sh;
    std::vector<HudText> kept_text;
    kept_text.reserve(geo.texts.size());
    for (const HudText& text : geo.texts) {
        const float px = (text.x * 0.5f + 0.5f) * static_cast<float>(geo.width);
        const float py = (text.y * 0.5f + 0.5f) * static_cast<float>(geo.height);
        const bool inside = px >= x0 && py >= y0 && px <= x1 && py <= y1;
        if (!inside) {
            kept_text.push_back(text);
        }
    }
    geo.texts.swap(kept_text);
    // Partial R2T updates used to only drop text, so bar/glow verts accumulated every
    // frame and the top bar grew into an oversized grey slab.
    std::vector<HudVertex> kept_verts;
    kept_verts.reserve(geo.verts.size());
    for (size_t i = 0; i + 3 < geo.verts.size(); i += 4) {
        const HudVertex& a = geo.verts[i];
        const HudVertex& c = geo.verts[i + 2];
        const float cx = 0.5f * (a.x + c.x);
        const float cy = 0.5f * (a.y + c.y);
        const float px = (cx * 0.5f + 0.5f) * static_cast<float>(geo.width);
        const float py = (cy * 0.5f + 0.5f) * static_cast<float>(geo.height);
        const bool inside = px >= x0 && py >= y0 && px <= x1 && py <= y1;
        if (inside) {
            continue;
        }
        kept_verts.push_back(geo.verts[i]);
        kept_verts.push_back(geo.verts[i + 1]);
        kept_verts.push_back(geo.verts[i + 2]);
        kept_verts.push_back(geo.verts[i + 3]);
    }
    geo.verts.swap(kept_verts);
}

int gl_scissor(lua_State* state) {
    if (lua_isboolean(state, 1) != 0 && lua_toboolean(state, 1) == 0) {
        g.scissor_on = false;
        return 0;
    }
    if (lua_isnumber(state, 1) == 0) {
        g.scissor_on = false;
        return 0;
    }
    g.scissor_on = true;
    g.sx = lua_num(state, 1, 0);
    g.sy = lua_num(state, 2, 0);
    g.sw = lua_num(state, 3, 0);
    g.sh = lua_num(state, 4, 0);
    return 0;
}

int gl_clear(lua_State* state) {
    (void)state;
    Geo* geo = target_geo();
    if (g.scissor_on) {
        erase_scissor_region(*geo);
        return 0;
    }
    geo->verts.clear();
    geo->texts.clear();
    return 0;
}

int gl_load_font(lua_State* state) {
    const char* path = lua_tostring(state, 1);
    lua_newtable(state);
    if (path != nullptr) {
        lua_pushstring(state, path);
        lua_setfield(state, -2, "path");
    }
    lua_pushnumber(state, lua_num(state, 2, 16));
    lua_setfield(state, -2, "px");
    luaL_getmetatable(state, "bar.font");
    lua_setmetatable(state, -2);
    return 1;
}

int gl_delete_font(lua_State* state) {
    (void)state;
    return 0;
}

int gl_fallback_font(lua_State* state) {
    (void)state;
    return 0;
}

int font_begin(lua_State* state) {
    (void)state;
    return 0;
}

int font_end(lua_State* state) {
    (void)state;
    return 0;
}

int font_set_color(lua_State* state) {
    lua_pushnumber(state, lua_num(state, 2, 1));
    lua_setfield(state, 1, "tr");
    lua_pushnumber(state, lua_num(state, 3, 1));
    lua_setfield(state, 1, "tg");
    lua_pushnumber(state, lua_num(state, 4, 1));
    lua_setfield(state, 1, "tb");
    lua_pushnumber(state, lua_num(state, 5, 1));
    lua_setfield(state, 1, "ta");
    return 0;
}

int font_set_outline(lua_State* state) {
    lua_pushnumber(state, lua_num(state, 2, 0));
    lua_setfield(state, 1, "or");
    lua_pushnumber(state, lua_num(state, 3, 0));
    lua_setfield(state, 1, "og");
    lua_pushnumber(state, lua_num(state, 4, 0));
    lua_setfield(state, 1, "ob");
    lua_pushnumber(state, lua_num(state, 5, 1));
    lua_setfield(state, 1, "oa");
    return 0;
}

int font_width(lua_State* state) {
    size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    float r = 1;
    float gch = 1;
    float b = 1;
    float a = 1;
    const std::string visible = raw != nullptr ? visible_text(raw, length, r, gch, b, a) : std::string();
    lua_pushnumber(state, 0.5 * static_cast<double>(visible.size()));
    return 1;
}

int font_height(lua_State* state) {
    (void)state;
    // Approximate em-height as a fraction of the font's pixel size.
    const float px = field_num(state, 1, "px", 16);
    lua_pushnumber(state, 0.8);
    lua_pushnumber(state, 0.2);
    (void)px;
    return 2;
}

int font_print(lua_State* state) {
    size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    float r = field_num(state, 1, "tr", 1);
    float gch = field_num(state, 1, "tg", 1);
    float b = field_num(state, 1, "tb", 1);
    float a = field_num(state, 1, "ta", 1);
    // Keep \255RGB / \008 in the payload so raster can tint mid-string runs.
    // Strip only to decide emptiness and for width; base color stays SetTextColor.
    float ignore_r = r;
    float ignore_g = gch;
    float ignore_b = b;
    float ignore_a = a;
    const std::string plain =
        raw != nullptr ? visible_text(raw, length, ignore_r, ignore_g, ignore_b, ignore_a) : std::string();
    if (plain.empty()) {
        return 0;
    }
    HudText text;
    text.text = raw != nullptr ? std::string(raw, length) : std::string();
    text.x = lua_num(state, 3, 0);
    text.y = lua_num(state, 4, 0);
    text.size = lua_num(state, 5, field_num(state, 1, "px", 16));
    text.r = r;
    text.g = gch;
    text.b = b;
    text.a = a;
    text.font_path = font_file(state, 1);
    const char* options = lua_tostring(state, 6);
    const std::string flags = options != nullptr ? options : "";
    if (flags.find('c') != std::string::npos) {
        text.align = 1;
    } else if (flags.find('r') != std::string::npos) {
        text.align = 2;
    }
    text.outline = flags.find('o') != std::string::npos;
    note_text(std::move(text));
    return 0;
}


int gl_rect(lua_State* state) {
    const float x1 = lua_num(state, 1, 0);
    const float y1 = lua_num(state, 2, 0);
    const float x2 = lua_num(state, 3, 0);
    const float y2 = lua_num(state, 4, 0);
    if (g.polygon_mode == kLine) {
        outline_rect(x1, y1, x2, y2, g.cr, g.cg, g.cb, g.ca, g.line_width);
    } else {
        color_rect(x1, y1, x2, y2, g.cr, g.cg, g.cb, g.ca);
    }
    return 0;
}

int gl_polygon_mode(lua_State* state) {
    (void)lua_num(state, 1, static_cast<float>(kFrontAndBack));
    g.polygon_mode = static_cast<int>(lua_num(state, 2, static_cast<float>(kFill)));
    return 0;
}

int gl_line_width(lua_State* state) {
    g.line_width = lua_num(state, 1, 1);
    return 0;
}

int gl_line_stipple(lua_State* state) {
    if (lua_isboolean(state, 1) != 0) {
        g.line_stipple = lua_toboolean(state, 1) != 0;
    } else if (lua_isnoneornil(state, 1)) {
        g.line_stipple = false;
    } else {
        g.line_stipple = true;
    }
    return 0;
}

void set_gl(lua_State* state, const char* name, lua_CFunction fn) {
    lua_pushcfunction(state, fn);
    lua_setfield(state, -2, name);
}

}  // namespace

void gl_install(lua_State* state) {
    luaL_newmetatable(state, "bar.font");
    lua_newtable(state);
    lua_pushcfunction(state, font_begin);
    lua_setfield(state, -2, "Begin");
    lua_pushcfunction(state, font_end);
    lua_setfield(state, -2, "End");
    lua_pushcfunction(state, font_print);
    lua_setfield(state, -2, "Print");
    lua_pushcfunction(state, font_set_color);
    lua_setfield(state, -2, "SetTextColor");
    lua_pushcfunction(state, font_set_outline);
    lua_setfield(state, -2, "SetOutlineColor");
    lua_pushcfunction(state, font_width);
    lua_setfield(state, -2, "GetTextWidth");
    lua_pushcfunction(state, font_height);
    lua_setfield(state, -2, "GetTextHeight");
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    lua_newtable(state);
    set_gl(state, "Color", gl_color);
    set_gl(state, "Texture", gl_texture);
    set_gl(state, "TexRect", gl_texrect);
    set_gl(state, "Rect", gl_rect);
    set_gl(state, "PolygonMode", gl_polygon_mode);
    set_gl(state, "LineWidth", gl_line_width);
    set_gl(state, "LineStipple", gl_line_stipple);
    set_gl(state, "Vertex", gl_vertex);
    set_gl(state, "TexCoord", gl_texcoord);
    set_gl(state, "BeginEnd", gl_begin_end);
    set_gl(state, "CreateList", gl_create_list);
    set_gl(state, "CallList", gl_call_list);
    set_gl(state, "DeleteList", gl_delete_list);
    set_gl(state, "Blending", gl_matrix_op);
    set_gl(state, "BlendFuncSeparate", gl_matrix_op);
    set_gl(state, "PushMatrix", gl_push);
    set_gl(state, "PopMatrix", gl_pop);
    set_gl(state, "Translate", gl_translate);
    set_gl(state, "Scale", gl_scale);
    set_gl(state, "Rotate", gl_rotate);
    set_gl(state, "LoadIdentity", gl_load_id);
    set_gl(state, "MatrixMode", gl_matrix_mode);
    set_gl(state, "GetViewSizes", gl_view_sizes);
    set_gl(state, "CreateTexture", gl_create_texture);
    set_gl(state, "DeleteTexture", gl_delete_texture);
    set_gl(state, "RenderToTexture", gl_render_to_texture);
    set_gl(state, "Scissor", gl_scissor);
    set_gl(state, "Clear", gl_clear);
    set_gl(state, "LoadFont", gl_load_font);
    set_gl(state, "DeleteFont", gl_delete_font);
    set_gl(state, "AddFallbackFont", gl_fallback_font);
    lua_setglobal(state, "gl");

    lua_newtable(state);
    lua_pushinteger(state, 7);
    lua_setfield(state, -2, "QUADS");
    lua_pushinteger(state, 1);
    lua_setfield(state, -2, "ONE");
    lua_pushinteger(state, 0x0302);
    lua_setfield(state, -2, "SRC_ALPHA");
    lua_pushinteger(state, 0x0303);
    lua_setfield(state, -2, "ONE_MINUS_SRC_ALPHA");
    lua_pushinteger(state, 0x0303);
    lua_setfield(state, -2, "ONE_MINUS_DST_ALPHA");
    lua_pushinteger(state, 0x0DE1);
    lua_setfield(state, -2, "TEXTURE_2D");
    lua_pushinteger(state, 0x1906);
    lua_setfield(state, -2, "ALPHA");
    lua_pushinteger(state, 0x4000);
    lua_setfield(state, -2, "COLOR_BUFFER_BIT");
    lua_pushinteger(state, kModelview);
    lua_setfield(state, -2, "MODELVIEW");
    lua_pushinteger(state, kTexture);
    lua_setfield(state, -2, "TEXTURE");
    lua_pushinteger(state, kLine);
    lua_setfield(state, -2, "LINE");
    lua_pushinteger(state, kFill);
    lua_setfield(state, -2, "FILL");
    lua_pushinteger(state, kFrontAndBack);
    lua_setfield(state, -2, "FRONT_AND_BACK");
    lua_setglobal(state, "GL");
}

void gl_set_root(const std::string& game_root) {
    g.root = game_root;
}

void gl_set_view(int width, int height) {
    if (width > 0) {
        g.view_w = width;
    }
    if (height > 0) {
        g.view_h = height;
    }
}

int gl_view_width() {
    return g.view_w;
}

int gl_view_height() {
    return g.view_h;
}

void gl_begin_draw() {
    g.polygon_mode = kFill;
    g.line_width = 1.f;
    g.line_stipple = false;
    g.model = identity_mat();
    g.texture = identity_mat();
    g.model_stack.clear();
    g.texture_stack.clear();
    g.mode = kModelview;
    g.cr = 1;
    g.cg = 1;
    g.cb = 1;
    g.ca = 1;
    g.bind = 0;
    g.texture_missing = false;
    g.recording = 0;
    g.scissor_on = false;
    g.batch.clear();
    g.screen.verts.clear();
    g.screen.texts.clear();
    g.screen.ndc = false;
    g_target = 0;
}

const HudList& gl_publish() {
    flush_batch();
    g.published.vertices = g.screen.verts;
    g.published.texts = g.screen.texts;
    g.published.width = g.view_w;
    g.published.height = g.view_h;
    return g.published;
}

}  // namespace bar
