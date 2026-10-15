#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bar {

// Piece ops use the model piece index. A cob name that is not on the model is not called.
struct CobOps {
    void* user = nullptr;
    void (*spin)(void* user, int piece, int axis, int32_t speed, int32_t accel) = nullptr;
    void (*stop_spin)(void* user, int piece, int axis, int32_t decel) = nullptr;
    void (*turn)(void* user, int piece, int axis, int32_t dest, int32_t speed) = nullptr;
    void (*move)(void* user, int piece, int axis, int32_t dest, int32_t speed) = nullptr;
    void (*hide)(void* user, int piece, bool hidden) = nullptr;
    bool (*busy_turn)(void* user, int piece, int axis) = nullptr;
    bool (*busy_move)(void* user, int piece, int axis) = nullptr;
    int32_t (*query)(void* user, int32_t id) = nullptr;
};

struct CobProgram;

// Version-4 cob. piece_names are the model pieces, already lowercase, in model order.
bool cob_load(CobProgram*& out, const std::string& path, const std::vector<std::string>& piece_names,
              std::string& error);
void cob_free(CobProgram* program);

// Model piece index for a cob piece name, or -1.
int cob_model_piece(const CobProgram& program, const char* name);

// Runs one script until it returns or sleeps, then runs threads it started.
// args are source order. The callee's first local pops args[0].
bool cob_call(CobProgram& program, const CobOps& ops, const char* name, const int32_t* args, int argc, int frame,
              std::string& error);

// Wakes threads whose sleep has elapsed, then runs threads they start.
bool cob_wake(CobProgram& program, const CobOps& ops, int frame, std::string& error);

}  // namespace bar
