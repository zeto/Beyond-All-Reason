#include "anim/unit_anim.h"

#include "anim/cob.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

namespace bar {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = kPi * 2.f;
constexpr float kSimHz = 30.f;
constexpr int kMaxPieces = 128;

float clamp_rad(float angle) {
    angle = std::fmod(angle, kTwoPi);
    if (angle < 0.f) {
        angle += kTwoPi;
    }
    return angle;
}

float sign_of(float value) {
    if (value < 0.f) {
        return -1.f;
    }
    if (value > 0.f) {
        return 1.f;
    }
    return 0.f;
}

void identity(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 1.f;
    m[5] = 1.f;
    m[10] = 1.f;
    m[15] = 1.f;
}

void mul(float* out, const float* a, const float* b) {
    float r[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r[col * 4 + row] = a[row] * b[col * 4] + a[4 + row] * b[col * 4 + 1] + a[8 + row] * b[col * 4 + 2] +
                               a[12 + row] * b[col * 4 + 3];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void translation(float* m, float x, float y, float z) {
    identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

void rotation_x(float* m, float angle) {
    identity(m);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
}

void rotation_y(float* m, float angle) {
    identity(m);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    m[0] = c;
    m[2] = -s;
    m[8] = s;
    m[10] = c;
}

void rotation_z(float* m, float angle) {
    identity(m);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    m[0] = c;
    m[1] = s;
    m[4] = -s;
    m[5] = c;
}

// Recoil S3DModelPiece::ComposeTransform uses FromEulerYPR: vertex sees Ry, then Rx, then Rz,
// then the piece offset. The parent matrix is applied after that.
void piece_matrix(const float* parent, float ox, float oy, float oz, const float rot[3], bool hidden, float* out) {
    float t[16];
    float rx[16];
    float ry[16];
    float rz[16];
    float spin[16];
    float local[16];
    translation(t, ox, oy, oz);
    rotation_x(rx, rot[0]);
    rotation_y(ry, rot[1]);
    rotation_z(rz, rot[2]);
    mul(spin, rx, ry);
    mul(local, rz, spin);
    mul(spin, t, local);
    if (parent != nullptr) {
        mul(out, parent, spin);
    } else {
        std::memcpy(out, spin, sizeof(spin));
    }
    if (hidden) {
        out[15] = 0.f;
    }
}

bool transform_matches(const float* m, float x, float y, float z, float ex, float ey, float ez) {
    const float rx = m[0] * x + m[4] * y + m[8] * z + m[12];
    const float ry = m[1] * x + m[5] * y + m[9] * z + m[13];
    const float rz = m[2] * x + m[6] * y + m[10] * z + m[14];
    return std::fabs(rx - ex) < 1e-4f && std::fabs(ry - ey) < 1e-4f && std::fabs(rz - ez) < 1e-4f;
}

struct PieceState {
    std::string name;
    int parent = -1;
    float offset[3] = {0, 0, 0};
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
    bool hidden = false;
    bool turning[3] = {false, false, false};
    float turn_dest[3] = {0, 0, 0};
    float turn_speed[3] = {0, 0, 0};
    bool moving[3] = {false, false, false};
    float move_dest[3] = {0, 0, 0};
    float move_speed[3] = {0, 0, 0};
    bool spinning[3] = {false, false, false};
    float spin_speed[3] = {0, 0, 0};
    float spin_dest[3] = {0, 0, 0};
    float spin_accel[3] = {0, 0, 0};
};

struct ScriptThread {
    lua_State* co = nullptr;
    int ref = LUA_NOREF;
    int wake_frame = -1;
    int signal_mask = 0;
    bool dead = false;
};

bool turn_toward(float& cur, float dest, float speed) {
    const float delta = std::fmod(dest - cur + 3.f * kPi, kTwoPi) - kPi;
    if (std::fabs(delta) <= speed) {
        cur = dest;
        return true;
    }
    cur = clamp_rad(cur + speed * sign_of(delta));
    return false;
}

bool move_toward(float& cur, float dest, float speed) {
    const float delta = dest - cur;
    if (std::fabs(delta) <= speed) {
        cur = dest;
        return true;
    }
    cur += speed * sign_of(delta);
    return false;
}

}  // namespace

struct UnitAnim::Impl {
    lua_State* state = nullptr;
    CobProgram* cob = nullptr;
    std::vector<PieceState> pieces;
    std::vector<ScriptThread> threads;
    float speed = 0;
    float def_speed = 0;
    float query_heading = 0;
    float query_ground = 0;
    float query_max_speed = 0;
    bool moving = false;
    bool script_ok = true;
    int lthigh = -1;
    int rthigh = -1;
    int dish = -1;
    int lwheel = -1;
    int rwheel = -1;
    int smoke1 = -1;
    int wheel_spins = 0;

    ~Impl() {
        if (state != nullptr) {
            lua_close(state);
            state = nullptr;
        }
        cob_free(cob);
        cob = nullptr;
    }
};

namespace {

UnitAnim* anim_from(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "bar_anim");
    auto* anim = static_cast<UnitAnim*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return anim;
}

int find_piece(const UnitAnim::Impl& impl, const char* name) {
    if (name == nullptr) {
        return -1;
    }
    std::string lower = name;
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    for (int i = 0; i < static_cast<int>(impl.pieces.size()); ++i) {
        if (impl.pieces[static_cast<size_t>(i)].name == lower) {
            return i;
        }
    }
    return -1;
}

bool piece_axis(lua_State* L, UnitAnim*& anim, int& piece, int& axis) {
    anim = anim_from(L);
    if (anim == nullptr || anim->impl == nullptr) {
        luaL_error(L, "unit script has no commander");
        return false;
    }
    piece = luaL_checkint(L, 1) - 1;
    axis = luaL_checkint(L, 2) - 1;
    if (piece < 0 || piece >= static_cast<int>(anim->impl->pieces.size()) || axis < 0 || axis > 2) {
        luaL_error(L, "bad piece or axis");
        return false;
    }
    return true;
}

void kill_thread(UnitAnim& anim, ScriptThread& thread) {
    thread.dead = true;
    thread.wake_frame = -1;
    if (anim.impl->state != nullptr && thread.ref != LUA_NOREF) {
        luaL_unref(anim.impl->state, LUA_REGISTRYINDEX, thread.ref);
        thread.ref = LUA_NOREF;
    }
}

void resume_thread(UnitAnim& anim, size_t index, int narg) {
    if (index >= anim.impl->threads.size() || anim.impl->threads[index].dead || anim.impl->threads[index].co == nullptr) {
        return;
    }
    // StartThread inside the resume can grow the thread vector, so keep the coroutine pointer
    // and look the slot up again afterwards.
    lua_State* co = anim.impl->threads[index].co;
    const int status = lua_resume(co, narg);
    if (status == 0) {
        kill_thread(anim, anim.impl->threads[index]);
        return;
    }
    if (status == LUA_YIELD) {
        int frames = 1;
        if (lua_gettop(co) >= 1 && lua_isnumber(co, -1)) {
            frames = static_cast<int>(lua_tointeger(co, -1));
        }
        lua_settop(co, 0);
        if (frames < 1) {
            frames = 1;
        }
        anim.impl->threads[index].wake_frame = anim.frame + frames;
        return;
    }
    const char* message = lua_tostring(co, -1);
    std::cerr << "unit script: " << (message != nullptr ? message : "error") << "\n";
    anim.impl->script_ok = false;
    kill_thread(anim, anim.impl->threads[index]);
}

int l_piece(lua_State* L) {
    UnitAnim* anim = anim_from(L);
    const int count = lua_gettop(L);
    if (anim == nullptr || anim->impl == nullptr) {
        return luaL_error(L, "piece() before the model was loaded");
    }
    for (int i = 1; i <= count; ++i) {
        const char* name = luaL_checkstring(L, i);
        const int found = find_piece(*anim->impl, name);
        if (found < 0) {
            return luaL_error(L, "piece not found: %s", name);
        }
        lua_pushinteger(L, found + 1);
    }
    return count;
}

int l_turn(lua_State* L) {
    UnitAnim* anim = nullptr;
    int piece = 0;
    int axis = 0;
    if (!piece_axis(L, anim, piece, axis)) {
        return 0;
    }
    const float dest = static_cast<float>(luaL_checknumber(L, 3));
    const float speed = static_cast<float>(luaL_optnumber(L, 4, 0));
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    node.spinning[axis] = false;
    if (speed == 0.f) {
        node.rot[axis] = clamp_rad(dest);
        node.turning[axis] = false;
        return 0;
    }
    node.turn_dest[axis] = clamp_rad(dest);
    node.turn_speed[axis] = std::fabs(speed);
    node.turning[axis] = true;
    return 0;
}

int l_move(lua_State* L) {
    UnitAnim* anim = nullptr;
    int piece = 0;
    int axis = 0;
    if (!piece_axis(L, anim, piece, axis)) {
        return 0;
    }
    const float dest = static_cast<float>(luaL_checknumber(L, 3));
    const float speed = static_cast<float>(luaL_optnumber(L, 4, 0));
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    const float absolute = node.offset[axis] + dest;
    if (speed == 0.f) {
        node.pos[axis] = absolute;
        node.moving[axis] = false;
        return 0;
    }
    node.move_dest[axis] = absolute;
    node.move_speed[axis] = std::fabs(speed);
    node.moving[axis] = true;
    return 0;
}

int l_spin(lua_State* L) {
    UnitAnim* anim = nullptr;
    int piece = 0;
    int axis = 0;
    if (!piece_axis(L, anim, piece, axis)) {
        return 0;
    }
    const float speed = static_cast<float>(luaL_checknumber(L, 3));
    const float accel = static_cast<float>(luaL_optnumber(L, 4, 0));
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    node.turning[axis] = false;
    node.spinning[axis] = true;
    node.spin_dest[axis] = speed;
    node.spin_accel[axis] = std::fabs(accel);
    if (accel <= 0.f) {
        node.spin_speed[axis] = speed;
    }
    return 0;
}

int set_hidden(lua_State* L, bool hidden) {
    UnitAnim* anim = anim_from(L);
    const int piece = luaL_checkint(L, 1) - 1;
    if (anim == nullptr || anim->impl == nullptr || piece < 0 ||
        piece >= static_cast<int>(anim->impl->pieces.size())) {
        return luaL_error(L, "bad piece");
    }
    anim->impl->pieces[static_cast<size_t>(piece)].hidden = hidden;
    return 0;
}

int l_hide(lua_State* L) { return set_hidden(L, true); }

int l_show(lua_State* L) { return set_hidden(L, false); }

int l_emit(lua_State*) { return 0; }

int l_sleep(lua_State* L) {
    const double ms = luaL_checknumber(L, 1);
    int frames = static_cast<int>(std::floor(ms * 0.03030303));
    if (frames < 1) {
        frames = 1;
    }
    lua_pushinteger(L, frames);
    return lua_yield(L, 1);
}

int l_start_thread(lua_State* L) {
    UnitAnim* anim = anim_from(L);
    if (anim == nullptr || anim->impl == nullptr || anim->impl->state == nullptr) {
        return luaL_error(L, "StartThread has no script");
    }
    if (!lua_isfunction(L, 1)) {
        return luaL_error(L, "StartThread expected a function");
    }
    int mask = 0;
    for (const ScriptThread& thread : anim->impl->threads) {
        if (thread.co == L && !thread.dead) {
            mask = thread.signal_mask;
            break;
        }
    }
    lua_State* co = lua_newthread(anim->impl->state);
    const int ref = luaL_ref(anim->impl->state, LUA_REGISTRYINDEX);
    const int nargs = lua_gettop(L);
    for (int i = 1; i <= nargs; ++i) {
        lua_pushvalue(L, i);
    }
    lua_xmove(L, co, nargs);
    ScriptThread thread;
    thread.co = co;
    thread.ref = ref;
    thread.signal_mask = mask;
    anim->impl->threads.push_back(thread);
    resume_thread(*anim, anim->impl->threads.size() - 1, nargs - 1);
    return 0;
}

int l_signal(lua_State* L) {
    UnitAnim* anim = anim_from(L);
    const int mask = luaL_checkint(L, 1);
    if (anim == nullptr) {
        return 0;
    }
    for (ScriptThread& thread : anim->impl->threads) {
        if (!thread.dead && thread.wake_frame >= 0 && (thread.signal_mask & mask) != 0) {
            kill_thread(*anim, thread);
        }
    }
    return 0;
}

int l_set_mask(lua_State* L) {
    UnitAnim* anim = anim_from(L);
    const int mask = luaL_checkint(L, 1);
    if (anim == nullptr) {
        return 0;
    }
    for (ScriptThread& thread : anim->impl->threads) {
        if (thread.co == L && !thread.dead) {
            thread.signal_mask = mask;
            return 0;
        }
    }
    return 0;
}

int l_cloaked(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

int l_def_id(lua_State* L) {
    lua_pushinteger(L, 1);
    return 1;
}

int l_velocity(lua_State* L) {
    UnitAnim* anim = anim_from(L);
    const float speed = anim != nullptr ? anim->impl->speed : 0.f;
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, speed);
    return 4;
}

void push_c(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

void rebuild(UnitAnim& anim) {
    anim.matrices.assign(static_cast<size_t>(kMaxPieces) * 16, 0.f);
    for (int i = 0; i < kMaxPieces; ++i) {
        identity(anim.matrices.data() + static_cast<size_t>(i) * 16);
    }
    const int count = static_cast<int>(anim.impl->pieces.size());
    for (int i = 0; i < count; ++i) {
        const PieceState& node = anim.impl->pieces[static_cast<size_t>(i)];
        const float* parent = nullptr;
        if (node.parent >= 0 && node.parent < i) {
            parent = anim.matrices.data() + static_cast<size_t>(node.parent) * 16;
        }
        float rot[3] = {node.rot[0], node.rot[1], node.rot[2]};
        piece_matrix(parent, node.pos[0], node.pos[1], node.pos[2], rot, node.hidden,
                     anim.matrices.data() + static_cast<size_t>(i) * 16);
    }
    anim.piece_count = count;
}

void integrate(UnitAnim& anim) {
    const int counted = anim.impl->lwheel;
    for (int index = 0; index < static_cast<int>(anim.impl->pieces.size()); ++index) {
        PieceState& node = anim.impl->pieces[static_cast<size_t>(index)];
        for (int axis = 0; axis < 3; ++axis) {
            if (node.turning[axis]) {
                float cur = clamp_rad(node.rot[axis]);
                if (turn_toward(cur, node.turn_dest[axis], node.turn_speed[axis] / kSimHz)) {
                    node.turning[axis] = false;
                }
                node.rot[axis] = cur;
            } else if (node.spinning[axis]) {
                const float delta = node.spin_dest[axis] - node.spin_speed[axis];
                if (std::fabs(delta) <= node.spin_accel[axis]) {
                    node.spin_speed[axis] = node.spin_dest[axis];
                    if (node.spin_speed[axis] == 0.f) {
                        node.spinning[axis] = false;
                    }
                } else {
                    node.spin_speed[axis] += node.spin_accel[axis] * sign_of(delta);
                }
                if (node.spinning[axis]) {
                    node.rot[axis] = clamp_rad(node.rot[axis] + node.spin_speed[axis] / kSimHz);
                    // Count the integrates that added the left wheel's X spin. The rotation above is unchanged.
                    if (index == counted && axis == 0) {
                        anim.impl->wheel_spins += 1;
                    }
                }
            }
            if (node.moving[axis]) {
                if (move_toward(node.pos[axis], node.move_dest[axis], node.move_speed[axis] / kSimHz)) {
                    node.moving[axis] = false;
                }
            }
        }
    }
}

bool call_script(UnitAnim& anim, const char* name, std::string& error) {
    lua_State* L = anim.impl->state;
    lua_getglobal(L, "script");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        error = "unit script did not define script";
        return false;
    }
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        error = std::string("unit script has no ") + name;
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char* message = lua_tostring(L, -1);
        error = message != nullptr ? message : "unit script call failed";
        lua_pop(L, 1);
        anim.impl->script_ok = false;
        return false;
    }
    return anim.impl->script_ok;
}

int named_piece(const UnitAnim::Impl& impl, const char* name) {
    return find_piece(impl, name);
}

constexpr float kCobToRad = kPi / 32768.f;

bool cob_piece(const UnitAnim& anim, int piece, int axis) {
    return anim.impl != nullptr && piece >= 0 && piece < static_cast<int>(anim.impl->pieces.size()) && axis >= 0 &&
           axis <= 2;
}

void cob_op_spin(void* user, int piece, int axis, int32_t speed, int32_t accel) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, axis)) {
        return;
    }
    float dest = static_cast<float>(static_cast<double>(speed) * static_cast<double>(kPi) / 32768.0);
    if (axis == 2) {
        dest = -dest;
    }
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    node.turning[axis] = false;
    node.spinning[axis] = true;
    node.spin_dest[axis] = dest;
    node.spin_accel[axis] = std::fabs(static_cast<float>(static_cast<double>(accel) * static_cast<double>(kPi) / 32768.0));
    if (accel == 0) {
        node.spin_speed[axis] = dest;
    }
}

void cob_op_stop_spin(void* user, int piece, int axis, int32_t decel) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, axis)) {
        return;
    }
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    node.turning[axis] = false;
    if (decel == 0) {
        node.spinning[axis] = false;
        node.spin_speed[axis] = 0.f;
        node.spin_dest[axis] = 0.f;
        node.spin_accel[axis] = 0.f;
        return;
    }
    node.spinning[axis] = true;
    node.spin_dest[axis] = 0.f;
    node.spin_accel[axis] = std::fabs(static_cast<float>(decel) * kCobToRad);
}

void cob_op_turn(void* user, int piece, int axis, int32_t dest_cob, int32_t speed_cob) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, axis)) {
        return;
    }
    float dest = static_cast<float>(dest_cob) * kCobToRad;
    if (axis == 2) {
        dest = -dest;
    }
    const float speed = std::fabs(static_cast<float>(speed_cob) * kCobToRad);
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    node.spinning[axis] = false;
    if (speed == 0.f) {
        node.rot[axis] = clamp_rad(dest);
        node.turning[axis] = false;
        return;
    }
    node.turn_dest[axis] = clamp_rad(dest);
    node.turn_speed[axis] = speed;
    node.turning[axis] = true;
}

void cob_op_move(void* user, int piece, int axis, int32_t dest_cob, int32_t speed_cob) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, axis)) {
        return;
    }
    PieceState& node = anim->impl->pieces[static_cast<size_t>(piece)];
    const float absolute = node.offset[axis] + static_cast<float>(dest_cob) / 65536.f;
    const float speed = std::fabs(static_cast<float>(speed_cob) / 65536.f);
    if (speed == 0.f) {
        node.pos[axis] = absolute;
        node.moving[axis] = false;
        return;
    }
    node.move_dest[axis] = absolute;
    node.move_speed[axis] = speed;
    node.moving[axis] = true;
}

void cob_op_hide(void* user, int piece, bool hidden) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, 0)) {
        return;
    }
    anim->impl->pieces[static_cast<size_t>(piece)].hidden = hidden;
}

bool cob_op_busy_turn(void* user, int piece, int axis) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, axis)) {
        return false;
    }
    return anim->impl->pieces[static_cast<size_t>(piece)].turning[axis];
}

bool cob_op_busy_move(void* user, int piece, int axis) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || !cob_piece(*anim, piece, axis)) {
        return false;
    }
    return anim->impl->pieces[static_cast<size_t>(piece)].moving[axis];
}

int32_t cob_op_query(void* user, int32_t id) {
    auto* anim = static_cast<UnitAnim*>(user);
    if (anim == nullptr || anim->impl == nullptr) {
        return 0;
    }
    const UnitAnim::Impl& impl = *anim->impl;
    if (id == 75) {
        return static_cast<int32_t>(static_cast<double>(impl.query_max_speed) * 65536.0);
    }
    if (id == 29) {
        return static_cast<int32_t>(static_cast<double>(impl.speed) * 65536.0);
    }
    if (id == 82) {
        return static_cast<int32_t>(static_cast<double>(impl.query_heading) * 32768.0 / static_cast<double>(kPi));
    }
    if (id == 28) {
        return impl.query_ground < 0.f ? 1 : 0;
    }
    if (id == 4) {
        return 100;
    }
    return 0;
}

CobOps cob_ops(UnitAnim& anim) {
    CobOps ops;
    ops.user = &anim;
    ops.spin = cob_op_spin;
    ops.stop_spin = cob_op_stop_spin;
    ops.turn = cob_op_turn;
    ops.move = cob_op_move;
    ops.hide = cob_op_hide;
    ops.busy_turn = cob_op_busy_turn;
    ops.busy_move = cob_op_busy_move;
    ops.query = cob_op_query;
    return ops;
}

bool cob_script(UnitAnim& anim, const char* name, const int32_t* args, int argc, std::string& error) {
    if (anim.impl == nullptr || anim.impl->cob == nullptr) {
        error = "cob script is not loaded";
        return false;
    }
    const CobOps ops = cob_ops(anim);
    if (!cob_call(*anim.impl->cob, ops, name, args, argc, anim.frame, error)) {
        anim.impl->script_ok = false;
        return false;
    }
    return true;
}

}  // namespace

UnitAnim::UnitAnim() : impl(new Impl) {}

UnitAnim::~UnitAnim() { delete impl; }

bool anim_convention_ok(std::string& error) {
    const float pitch[3] = {kPi * 0.5f, 0.f, 0.f};
    float pitched[16];
    piece_matrix(nullptr, 0, 0, 0, pitch, false, pitched);
    if (!transform_matches(pitched, 0, 1, 0, 0, 0, 1)) {
        error = "a positive piece X rotation should send +y to +z";
        return false;
    }
    const float yaw[3] = {0.f, kPi * 0.5f, 0.f};
    float yawed[16];
    piece_matrix(nullptr, 0, 0, 0, yaw, false, yawed);
    if (!transform_matches(yawed, 0, 0, 1, 1, 0, 0)) {
        error = "a positive piece Y rotation should send +z to +x";
        return false;
    }
    float parent[16];
    const float none[3] = {0, 0, 0};
    piece_matrix(nullptr, 0, 10, 0, none, false, parent);
    float child[16];
    piece_matrix(parent, 0, 5, 0, none, false, child);
    if (!transform_matches(child, 0, 1, 0, 0, 16, 0)) {
        error = "a child piece should add its offset in the parent frame";
        return false;
    }
    return true;
}

bool anim_open(UnitAnim& anim, const ModelMesh& model, const std::string& script_path, float def_speed_per_sec,
               std::string& error) {
    if (!anim_convention_ok(error)) {
        return false;
    }
    if (model.pieces.empty() || model.pieces.size() > static_cast<size_t>(kMaxPieces)) {
        error = "commander piece count is outside 1..128";
        return false;
    }
    anim.impl->pieces.clear();
    anim.impl->pieces.reserve(model.pieces.size());
    for (const ModelPiece& source : model.pieces) {
        PieceState node;
        node.name = source.name;
        node.parent = source.parent;
        node.offset[0] = source.ox;
        node.offset[1] = source.oy;
        node.offset[2] = source.oz;
        node.pos[0] = source.ox;
        node.pos[1] = source.oy;
        node.pos[2] = source.oz;
        anim.impl->pieces.push_back(node);
    }
    anim.impl->lthigh = named_piece(*anim.impl, "lthigh");
    anim.impl->rthigh = named_piece(*anim.impl, "rthigh");
    anim.impl->dish = named_piece(*anim.impl, "dish");
    if (anim.impl->lthigh < 0 || anim.impl->rthigh < 0 || anim.impl->dish < 0) {
        error = "commander model is missing lthigh, rthigh, or dish";
        return false;
    }
    anim.impl->def_speed = def_speed_per_sec;
    rebuild(anim);

    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        error = "lua 5.1 state was not created";
        return false;
    }
    anim.impl->state = L;
    luaL_openlibs(L);
    lua_pushlightuserdata(L, &anim);
    lua_setfield(L, LUA_REGISTRYINDEX, "bar_anim");

    lua_newtable(L);
    lua_setglobal(L, "script");
    lua_pushinteger(L, 1);
    lua_setglobal(L, "unitID");
    lua_pushinteger(L, 1);
    lua_setglobal(L, "x_axis");
    lua_pushinteger(L, 2);
    lua_setglobal(L, "y_axis");
    lua_pushinteger(L, 3);
    lua_setglobal(L, "z_axis");
    lua_pushcfunction(L, l_piece);
    lua_setglobal(L, "piece");
    lua_pushcfunction(L, l_turn);
    lua_setglobal(L, "Turn");
    lua_pushcfunction(L, l_move);
    lua_setglobal(L, "Move");
    lua_pushcfunction(L, l_spin);
    lua_setglobal(L, "Spin");
    lua_pushcfunction(L, l_sleep);
    lua_setglobal(L, "Sleep");
    lua_pushcfunction(L, l_start_thread);
    lua_setglobal(L, "StartThread");
    lua_pushcfunction(L, l_signal);
    lua_setglobal(L, "Signal");
    lua_pushcfunction(L, l_set_mask);
    lua_setglobal(L, "SetSignalMask");
    lua_pushcfunction(L, l_hide);
    lua_setglobal(L, "Hide");
    lua_pushcfunction(L, l_show);
    lua_setglobal(L, "Show");

    lua_newtable(L);
    lua_newtable(L);
    lua_pushnumber(L, def_speed_per_sec);
    lua_setfield(L, -2, "speed");
    lua_rawseti(L, -2, 1);
    lua_setglobal(L, "UnitDefs");

    lua_newtable(L);
    push_c(L, "Turn", l_turn);
    push_c(L, "Move", l_move);
    push_c(L, "Spin", l_spin);
    push_c(L, "Sleep", l_sleep);
    push_c(L, "StartThread", l_start_thread);
    push_c(L, "Signal", l_signal);
    push_c(L, "SetSignalMask", l_set_mask);
    push_c(L, "Hide", l_hide);
    push_c(L, "Show", l_show);
    push_c(L, "EmitSfx", l_emit);
    lua_setglobal(L, "UnitScript");

    lua_newtable(L);
    push_c(L, "GetUnitIsCloaked", l_cloaked);
    push_c(L, "GetUnitDefID", l_def_id);
    push_c(L, "GetUnitVelocity", l_velocity);
    lua_getglobal(L, "UnitScript");
    lua_setfield(L, -2, "UnitScript");
    lua_setglobal(L, "Spring");

    if (luaL_loadfile(L, script_path.c_str()) != 0) {
        const char* message = lua_tostring(L, -1);
        error = message != nullptr ? message : "could not read the unit script";
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char* message = lua_tostring(L, -1);
        error = message != nullptr ? message : "unit script failed while loading";
        return false;
    }
    anim.ready = true;
    return true;
}

bool anim_open_cob(UnitAnim& anim, const ModelMesh& model, const std::string& script_path, float def_speed_per_sec,
                   std::string& error) {
    if (!anim_convention_ok(error)) {
        return false;
    }
    if (model.pieces.empty() || model.pieces.size() > static_cast<size_t>(kMaxPieces)) {
        error = "model piece count is outside 1..128";
        return false;
    }
    anim.impl->pieces.clear();
    anim.impl->pieces.reserve(model.pieces.size());
    std::vector<std::string> names;
    names.reserve(model.pieces.size());
    for (const ModelPiece& source : model.pieces) {
        PieceState node;
        node.name = source.name;
        node.parent = source.parent;
        node.offset[0] = source.ox;
        node.offset[1] = source.oy;
        node.offset[2] = source.oz;
        node.pos[0] = source.ox;
        node.pos[1] = source.oy;
        node.pos[2] = source.oz;
        anim.impl->pieces.push_back(node);
        names.push_back(source.name);
    }
    CobProgram* program = nullptr;
    if (!cob_load(program, script_path, names, error)) {
        return false;
    }
    const int lwheel = cob_model_piece(*program, "lwheel");
    const int rwheel = cob_model_piece(*program, "rwheel");
    const int base = cob_model_piece(*program, "base");
    const int smoke1 = cob_model_piece(*program, "smoke1");
    if (lwheel < 0 || rwheel < 0 || base < 0 || smoke1 < 0) {
        cob_free(program);
        error = "armbull model is missing lwheel, rwheel, base, or smoke1";
        return false;
    }
    cob_free(anim.impl->cob);
    anim.impl->cob = program;
    anim.impl->state = nullptr;
    anim.impl->lwheel = lwheel;
    anim.impl->rwheel = rwheel;
    anim.impl->smoke1 = smoke1;
    anim.impl->wheel_spins = 0;
    anim.impl->def_speed = def_speed_per_sec;
    anim.impl->query_max_speed = def_speed_per_sec / 30.f;
    rebuild(anim);
    anim.ready = true;
    return true;
}

bool anim_is_cob(const UnitAnim& anim) { return anim.impl != nullptr && anim.impl->cob != nullptr; }

bool anim_bind_pose(UnitAnim& anim, const ModelMesh& model, std::string& error) {
    if (!anim_convention_ok(error)) {
        return false;
    }
    if (model.pieces.empty() || model.pieces.size() > static_cast<size_t>(kMaxPieces)) {
        error = "model piece count is outside 1..128";
        return false;
    }
    anim.impl->pieces.clear();
    anim.impl->pieces.reserve(model.pieces.size());
    for (const ModelPiece& source : model.pieces) {
        PieceState node;
        node.name = source.name;
        node.parent = source.parent;
        node.offset[0] = source.ox;
        node.offset[1] = source.oy;
        node.offset[2] = source.oz;
        node.pos[0] = source.ox;
        node.pos[1] = source.oy;
        node.pos[2] = source.oz;
        anim.impl->pieces.push_back(node);
    }
    anim.impl->state = nullptr;
    rebuild(anim);
    anim.ready = true;
    return true;
}

bool anim_start(UnitAnim& anim, std::string& error) {
    if (!anim.ready) {
        error = "unit script is not loaded";
        return false;
    }
    if (anim.impl->cob != nullptr) {
        if (!cob_script(anim, "Create", nullptr, 0, error)) {
            return false;
        }
        rebuild(anim);
        return true;
    }
    // Bind pose has no script.
    if (anim.impl->state == nullptr) {
        return true;
    }
    if (!call_script(anim, "Create", error)) {
        return false;
    }
    rebuild(anim);
    return true;
}

void anim_set_speed(UnitAnim& anim, float elmos_per_frame) {
    if (anim.impl != nullptr) {
        anim.impl->speed = elmos_per_frame;
    }
}

void anim_set_query(UnitAnim& anim, float heading, float ground_y) {
    if (anim.impl == nullptr) {
        return;
    }
    anim.impl->query_heading = heading;
    anim.impl->query_ground = ground_y;
}

void anim_set_moving(UnitAnim& anim, bool moving) {
    if (anim.impl == nullptr || !anim.ready) {
        return;
    }
    if (anim.impl->cob != nullptr) {
        if (moving == anim.impl->moving) {
            return;
        }
        anim.impl->moving = moving;
        std::string error;
        const int32_t reversing = 0;
        const bool called = moving ? cob_script(anim, "StartMoving", &reversing, 1, error)
                                   : cob_script(anim, "StopMoving", nullptr, 0, error);
        if (!called) {
            std::cerr << "unit script: " << error << "\n";
        }
        return;
    }
    if (anim.impl->state == nullptr || moving == anim.impl->moving) {
        return;
    }
    anim.impl->moving = moving;
    std::string error;
    if (!call_script(anim, moving ? "StartMoving" : "StopMoving", error)) {
        std::cerr << "unit script: " << error << "\n";
    }
}

void anim_step(UnitAnim& anim) {
    if (anim.impl == nullptr || !anim.ready) {
        return;
    }
    if (anim.impl->cob != nullptr) {
        anim.frame += 1;
        std::string error;
        const CobOps ops = cob_ops(anim);
        if (!cob_wake(*anim.impl->cob, ops, anim.frame, error)) {
            std::cerr << "unit script: " << error << "\n";
            anim.impl->script_ok = false;
        }
        integrate(anim);
        rebuild(anim);
        return;
    }
    if (anim.impl->state == nullptr) {
        return;
    }
    anim.frame += 1;
    const size_t threads = anim.impl->threads.size();
    for (size_t i = 0; i < threads; ++i) {
        ScriptThread& thread = anim.impl->threads[i];
        if (!thread.dead && thread.wake_frame >= 0 && thread.wake_frame <= anim.frame) {
            thread.wake_frame = -1;
            resume_thread(anim, i, 0);
        }
    }
    integrate(anim);
    rebuild(anim);
}

bool anim_write_sample(const UnitAnim& anim, const std::string& path, int frame, bool append, std::string& error) {
    if (anim.impl == nullptr || anim.impl->lthigh < 0 || anim.impl->rthigh < 0 || anim.impl->dish < 0) {
        error = "piece sample is missing a thigh or the dish";
        return false;
    }
    std::ofstream out(path, append ? std::ios::app : std::ios::trunc);
    if (!out) {
        error = "could not write piece sample " + path;
        return false;
    }
    const PieceState& left = anim.impl->pieces[static_cast<size_t>(anim.impl->lthigh)];
    const PieceState& right = anim.impl->pieces[static_cast<size_t>(anim.impl->rthigh)];
    const PieceState& dish = anim.impl->pieces[static_cast<size_t>(anim.impl->dish)];
    out << "{\"f\":" << frame << ",\"lthigh_x\":" << left.rot[0] << ",\"lthigh_y\":" << left.rot[1]
        << ",\"lthigh_z\":" << left.rot[2] << ",\"rthigh_x\":" << right.rot[0] << ",\"rthigh_y\":" << right.rot[1]
        << ",\"rthigh_z\":" << right.rot[2] << ",\"dish_y\":" << dish.rot[1] << "}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool anim_write_cob_sample(const UnitAnim& anim, const std::string& path, int frame, bool append, std::string& error) {
    if (anim.impl == nullptr || anim.impl->lwheel < 0 || anim.impl->rwheel < 0 || anim.impl->smoke1 < 0) {
        error = "cob sample is missing a wheel or smoke1";
        return false;
    }
    std::ofstream out(path, append ? std::ios::app : std::ios::trunc);
    if (!out) {
        error = "could not write cob sample " + path;
        return false;
    }
    const PieceState& left = anim.impl->pieces[static_cast<size_t>(anim.impl->lwheel)];
    const PieceState& right = anim.impl->pieces[static_cast<size_t>(anim.impl->rwheel)];
    const PieceState& smoke = anim.impl->pieces[static_cast<size_t>(anim.impl->smoke1)];
    out << std::setprecision(9);
    out << "{\"f\":" << frame << ",\"lwheel_x\":" << left.rot[0] << ",\"rwheel_x\":" << right.rot[0]
        << ",\"spins\":" << anim.impl->wheel_spins << ",\"smoke1_hidden\":" << (smoke.hidden ? 1 : 0) << "}\n";
    if (!out) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

}  // namespace bar
