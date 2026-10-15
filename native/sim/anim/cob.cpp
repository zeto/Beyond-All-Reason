#include "anim/cob.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace bar {
namespace {

constexpr uint32_t kMove = 0x10001000;
constexpr uint32_t kTurn = 0x10002000;
constexpr uint32_t kSpin = 0x10003000;
constexpr uint32_t kStopSpin = 0x10004000;
constexpr uint32_t kShow = 0x10005000;
constexpr uint32_t kHide = 0x10006000;
constexpr uint32_t kMoveNow = 0x1000B000;
constexpr uint32_t kTurnNow = 0x1000C000;
constexpr uint32_t kEmitSfx = 0x1000F000;
constexpr uint32_t kWaitTurn = 0x10011000;
constexpr uint32_t kWaitMove = 0x10012000;
constexpr uint32_t kSleep = 0x10013000;
constexpr uint32_t kPushConst = 0x10021001;
constexpr uint32_t kPushLocal = 0x10021002;
constexpr uint32_t kPushStatic = 0x10021004;
constexpr uint32_t kCreateLocal = 0x10022000;
constexpr uint32_t kPopLocal = 0x10023002;
constexpr uint32_t kPopStatic = 0x10023004;
constexpr uint32_t kPopStack = 0x10024000;
constexpr uint32_t kAdd = 0x10031000;
constexpr uint32_t kSub = 0x10032000;
constexpr uint32_t kMul = 0x10033000;
constexpr uint32_t kDiv = 0x10034000;
constexpr uint32_t kMod = 0x10034001;
constexpr uint32_t kBitAnd = 0x10035000;
constexpr uint32_t kBitOr = 0x10036000;
constexpr uint32_t kBitXor = 0x10037000;
constexpr uint32_t kBitNot = 0x10038000;
constexpr uint32_t kRand = 0x10041000;
constexpr uint32_t kGetUnit = 0x10042000;
constexpr uint32_t kGet = 0x10043000;
constexpr uint32_t kLess = 0x10051000;
constexpr uint32_t kLessEqual = 0x10052000;
constexpr uint32_t kGreater = 0x10053000;
constexpr uint32_t kGreaterEqual = 0x10054000;
constexpr uint32_t kEqual = 0x10055000;
constexpr uint32_t kNotEqual = 0x10056000;
constexpr uint32_t kLogicAnd = 0x10057000;
constexpr uint32_t kLogicOr = 0x10058000;
constexpr uint32_t kLogicXor = 0x10059000;
constexpr uint32_t kLogicNot = 0x1005A000;
constexpr uint32_t kStart = 0x10061000;
constexpr uint32_t kCall = 0x10062000;
constexpr uint32_t kJump = 0x10064000;
constexpr uint32_t kReturn = 0x10065000;
constexpr uint32_t kJumpIfZero = 0x10066000;
constexpr uint32_t kSignal = 0x10067000;
constexpr uint32_t kSetMask = 0x10068000;
constexpr uint32_t kExplode = 0x10071000;
constexpr uint32_t kPlaySound = 0x10072000;
constexpr uint32_t kSet = 0x10082000;

constexpr int kMaxId = 132;
constexpr int kMinId = 131;
constexpr int kAbsId = 133;
constexpr int kGameFrameId = 134;
constexpr int kSinId = 135;
constexpr int kCosId = 136;
constexpr int kMaxSteps = 200000;

struct CallFrame {
    int pc = 0;
    std::vector<int32_t> stack;
    std::vector<int32_t> locals;
    int args_left = 0;
};

struct Thread {
    int pc = 0;
    std::vector<int32_t> stack;
    std::vector<int32_t> locals;
    int args_left = 0;
    std::vector<CallFrame> calls;
    int wake_frame = -1;
    int mask = 0;
    bool dead = false;
};

int32_t as_i32(uint32_t word) { return static_cast<int32_t>(word); }

int32_t mul_i32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b));
}

std::string hex_op(uint32_t op, int pc) {
    std::ostringstream out;
    out << "unknown cob opcode 0x" << std::hex << op << " at " << std::dec << pc;
    return out.str();
}

int sleep_frames(int32_t ms) {
    int frames = static_cast<int>(std::floor(static_cast<double>(ms) * 0.03030303));
    if (frames < 1) {
        frames = 1;
    }
    return frames;
}

int32_t kilo_trig(int32_t cob, bool cosine) {
    const double radians = static_cast<double>(cob) * 3.141592653589793 / 32768.0;
    const double wave = cosine ? std::cos(radians) : std::sin(radians);
    return static_cast<int32_t>(wave * 1024.0);
}

}  // namespace

struct CobProgram {
    std::vector<uint32_t> code;
    std::vector<int> func_pc;
    std::vector<std::string> func_names;
    std::vector<std::string> piece_names;
    std::vector<int> model_piece;
    std::vector<int32_t> statics;
    std::vector<Thread> threads;
    std::vector<int> born;
    int frame = 0;
    uint32_t rng = 1;
    std::string error;
    const CobOps* ops = nullptr;
};

namespace {

bool pop(Thread& thread, int32_t& out, std::string& error) {
    if (thread.stack.empty()) {
        error = "cob stack underflow";
        return false;
    }
    out = thread.stack.back();
    thread.stack.pop_back();
    return true;
}

void push(Thread& thread, int32_t value) { thread.stack.push_back(value); }

bool pop_two(Thread& thread, int32_t& a, int32_t& b, std::string& error) {
    if (!pop(thread, b, error) || !pop(thread, a, error)) {
        return false;
    }
    return true;
}

bool read_i32(const std::vector<uint8_t>& file, int offset, int32_t& out) {
    if (offset < 0 || static_cast<size_t>(offset) + 4 > file.size()) {
        return false;
    }
    uint32_t word = 0;
    std::memcpy(&word, file.data() + offset, 4);
    out = static_cast<int32_t>(word);
    return true;
}

bool read_name(const std::vector<uint8_t>& file, int offset, std::string& out) {
    if (offset < 0 || static_cast<size_t>(offset) >= file.size()) {
        return false;
    }
    const auto end = std::find(file.begin() + offset, file.end(), 0);
    out.assign(file.begin() + offset, end);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return true;
}

int find_func(const CobProgram& program, const char* name) {
    if (name == nullptr) {
        return -1;
    }
    std::string lower = name;
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    for (int i = 0; i < static_cast<int>(program.func_names.size()); ++i) {
        if (program.func_names[static_cast<size_t>(i)] == lower) {
            return i;
        }
    }
    return -1;
}

int model_of(const CobProgram& program, int cob_piece) {
    if (cob_piece < 0 || cob_piece >= static_cast<int>(program.model_piece.size())) {
        return -2;
    }
    return program.model_piece[static_cast<size_t>(cob_piece)];
}

bool immediate(const CobProgram& program, const Thread& thread, int count, uint32_t* out, std::string& error) {
    if (thread.pc < 0 || thread.pc + count >= static_cast<int>(program.code.size())) {
        error = "cob instruction runs past the code";
        return false;
    }
    for (int i = 0; i < count; ++i) {
        out[i] = program.code[static_cast<size_t>(thread.pc + 1 + i)];
    }
    return true;
}

// Source order is pushed with the last value on top. The callee pops the first argument first.
void stack_args(Thread& thread, const std::vector<int32_t>& popped_top_first) {
    for (int32_t value : popped_top_first) {
        thread.stack.push_back(value);
    }
}

bool spawn(CobProgram& program, int func, const std::vector<int32_t>& popped_top_first, int mask, bool queue) {
    if (func < 0 || func >= static_cast<int>(program.func_pc.size())) {
        program.error = "cob call names a missing script";
        return false;
    }
    Thread thread;
    thread.pc = program.func_pc[static_cast<size_t>(func)];
    thread.args_left = static_cast<int>(popped_top_first.size());
    thread.mask = mask;
    stack_args(thread, popped_top_first);
    program.threads.push_back(std::move(thread));
    if (queue) {
        program.born.push_back(static_cast<int>(program.threads.size()) - 1);
    }
    return true;
}

void kill_masked(CobProgram& program, int self, int32_t mask) {
    for (int i = 0; i < static_cast<int>(program.threads.size()); ++i) {
        Thread& thread = program.threads[static_cast<size_t>(i)];
        if (i == self || thread.dead) {
            continue;
        }
        if ((thread.mask & mask) != 0) {
            thread.dead = true;
            thread.wake_frame = -1;
        }
    }
}

int32_t next_rand(CobProgram& program) {
    uint32_t x = program.rng == 0 ? 1u : program.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0) {
        x = 1;
    }
    program.rng = x;
    return static_cast<int32_t>(x);
}

int32_t query_get(CobProgram& program, int32_t id, const int32_t args[4]) {
    if (id == kMaxId) {
        return args[0] > args[1] ? args[0] : args[1];
    }
    if (id == kMinId) {
        return args[0] < args[1] ? args[0] : args[1];
    }
    if (id == kAbsId) {
        return args[0] < 0 ? -args[0] : args[0];
    }
    if (id == kGameFrameId) {
        return program.frame;
    }
    if (id == kSinId) {
        return kilo_trig(args[0], false);
    }
    if (id == kCosId) {
        return kilo_trig(args[0], true);
    }
    if (program.ops != nullptr && program.ops->query != nullptr) {
        return program.ops->query(program.ops->user, id);
    }
    return 0;
}

bool exec(CobProgram& program, int index) {
    int steps = 0;
    while (steps++ < kMaxSteps) {
        if (!program.error.empty()) {
            return false;
        }
        if (index < 0 || index >= static_cast<int>(program.threads.size())) {
            program.error = "cob thread is missing";
            return false;
        }
        Thread& thread = program.threads[static_cast<size_t>(index)];
        if (thread.dead) {
            return true;
        }
        if (thread.pc < 0 || thread.pc >= static_cast<int>(program.code.size())) {
            program.error = "cob pc left the script";
            return false;
        }
        const uint32_t op = program.code[static_cast<size_t>(thread.pc)];
        const CobOps* ops = program.ops;
        uint32_t word[2] = {};
        switch (op) {
        case kCreateLocal: {
            int32_t value = 0;
            if (thread.args_left > 0) {
                if (!pop(thread, value, program.error)) {
                    return false;
                }
                thread.args_left -= 1;
            }
            thread.locals.push_back(value);
            thread.pc += 1;
            break;
        }
        case kPushConst:
            if (!immediate(program, thread, 1, word, program.error)) {
                return false;
            }
            push(thread, as_i32(word[0]));
            thread.pc += 2;
            break;
        case kPushLocal:
        case kPopLocal: {
            if (!immediate(program, thread, 1, word, program.error)) {
                return false;
            }
            const int slot = as_i32(word[0]);
            if (slot < 0 || slot >= static_cast<int>(thread.locals.size())) {
                program.error = "cob local is missing";
                return false;
            }
            if (op == kPushLocal) {
                push(thread, thread.locals[static_cast<size_t>(slot)]);
            } else {
                int32_t value = 0;
                if (!pop(thread, value, program.error)) {
                    return false;
                }
                thread.locals[static_cast<size_t>(slot)] = value;
            }
            thread.pc += 2;
            break;
        }
        case kPushStatic:
        case kPopStatic: {
            if (!immediate(program, thread, 1, word, program.error)) {
                return false;
            }
            const int slot = as_i32(word[0]);
            if (slot < 0 || slot >= static_cast<int>(program.statics.size())) {
                program.error = "cob static is missing";
                return false;
            }
            if (op == kPushStatic) {
                push(thread, program.statics[static_cast<size_t>(slot)]);
            } else {
                int32_t value = 0;
                if (!pop(thread, value, program.error)) {
                    return false;
                }
                program.statics[static_cast<size_t>(slot)] = value;
            }
            thread.pc += 2;
            break;
        }
        case kPopStack: {
            int32_t ignored = 0;
            if (!pop(thread, ignored, program.error)) {
                return false;
            }
            thread.pc += 1;
            break;
        }
        case kAdd:
        case kSub:
        case kMul:
        case kDiv:
        case kMod:
        case kBitAnd:
        case kBitOr:
        case kBitXor:
        case kLess:
        case kLessEqual:
        case kGreater:
        case kGreaterEqual:
        case kEqual:
        case kNotEqual:
        case kLogicAnd:
        case kLogicOr:
        case kLogicXor: {
            int32_t a = 0;
            int32_t b = 0;
            if (!pop_two(thread, a, b, program.error)) {
                return false;
            }
            int32_t value = 0;
            if (op == kAdd) {
                value = static_cast<int32_t>(static_cast<int64_t>(a) + b);
            } else if (op == kSub) {
                value = static_cast<int32_t>(static_cast<int64_t>(a) - b);
            } else if (op == kMul) {
                value = mul_i32(a, b);
            } else if (op == kDiv) {
                value = b == 0 ? 0 : a / b;
            } else if (op == kMod) {
                value = b == 0 ? 0 : a % b;
            } else if (op == kBitAnd) {
                value = a & b;
            } else if (op == kBitOr) {
                value = a | b;
            } else if (op == kBitXor) {
                value = a ^ b;
            } else if (op == kLess) {
                value = a < b ? 1 : 0;
            } else if (op == kLessEqual) {
                value = a <= b ? 1 : 0;
            } else if (op == kGreater) {
                value = a > b ? 1 : 0;
            } else if (op == kGreaterEqual) {
                value = a >= b ? 1 : 0;
            } else if (op == kEqual) {
                value = a == b ? 1 : 0;
            } else if (op == kNotEqual) {
                value = a != b ? 1 : 0;
            } else if (op == kLogicAnd) {
                value = (a != 0 && b != 0) ? 1 : 0;
            } else if (op == kLogicOr) {
                value = (a != 0 || b != 0) ? 1 : 0;
            } else {
                value = ((a != 0) != (b != 0)) ? 1 : 0;
            }
            push(thread, value);
            thread.pc += 1;
            break;
        }
        case kBitNot:
        case kLogicNot: {
            int32_t a = 0;
            if (!pop(thread, a, program.error)) {
                return false;
            }
            push(thread, op == kBitNot ? ~a : (a == 0 ? 1 : 0));
            thread.pc += 1;
            break;
        }
        case kRand: {
            int32_t high = 0;
            int32_t low = 0;
            if (!pop_two(thread, low, high, program.error)) {
                return false;
            }
            if (low > high) {
                const int32_t swap = low;
                low = high;
                high = swap;
            }
            const uint32_t span = static_cast<uint32_t>(high - low) + 1u;
            const int32_t pick = span == 0 ? low : low + static_cast<int32_t>(static_cast<uint32_t>(next_rand(program)) % span);
            push(thread, pick);
            thread.pc += 1;
            break;
        }
        case kGetUnit: {
            int32_t id = 0;
            if (!pop(thread, id, program.error)) {
                return false;
            }
            const int32_t args[4] = {};
            push(thread, query_get(program, id, args));
            thread.pc += 1;
            break;
        }
        case kGet: {
            int32_t args[4] = {};
            int32_t id = 0;
            if (!pop(thread, args[3], program.error) || !pop(thread, args[2], program.error) ||
                !pop(thread, args[1], program.error) || !pop(thread, args[0], program.error) ||
                !pop(thread, id, program.error)) {
                return false;
            }
            push(thread, query_get(program, id, args));
            thread.pc += 1;
            break;
        }
        case kJump:
        case kJumpIfZero: {
            if (!immediate(program, thread, 1, word, program.error)) {
                return false;
            }
            const int dest = as_i32(word[0]);
            if (dest < 0 || dest >= static_cast<int>(program.code.size())) {
                program.error = "cob jump leaves the script";
                return false;
            }
            if (op == kJump) {
                thread.pc = dest;
                break;
            }
            int32_t value = 0;
            if (!pop(thread, value, program.error)) {
                return false;
            }
            thread.pc = value == 0 ? dest : thread.pc + 2;
            break;
        }
        case kReturn: {
            int32_t value = 0;
            if (!thread.stack.empty()) {
                value = thread.stack.back();
                thread.stack.pop_back();
            }
            if (!thread.calls.empty()) {
                CallFrame frame = std::move(thread.calls.back());
                thread.calls.pop_back();
                thread.pc = frame.pc;
                thread.stack = std::move(frame.stack);
                thread.locals = std::move(frame.locals);
                thread.args_left = frame.args_left;
                push(thread, value);
                break;
            }
            thread.dead = true;
            thread.wake_frame = -1;
            return true;
        }
        case kSleep: {
            int32_t ms = 0;
            if (!pop(thread, ms, program.error)) {
                return false;
            }
            thread.wake_frame = program.frame + sleep_frames(ms);
            thread.pc += 1;
            return true;
        }
        case kSignal: {
            int32_t mask = 0;
            if (!pop(thread, mask, program.error)) {
                return false;
            }
            kill_masked(program, index, mask);
            thread.pc += 1;
            break;
        }
        case kSetMask: {
            int32_t mask = 0;
            if (!pop(thread, mask, program.error)) {
                return false;
            }
            thread.mask = mask;
            thread.pc += 1;
            break;
        }
        case kStart:
        case kCall: {
            if (!immediate(program, thread, 2, word, program.error)) {
                return false;
            }
            const int func = as_i32(word[0]);
            const int argc = as_i32(word[1]);
            if (argc < 0) {
                program.error = "cob call has a negative argument count";
                return false;
            }
            std::vector<int32_t> args;
            args.reserve(static_cast<size_t>(argc));
            for (int i = 0; i < argc; ++i) {
                int32_t value = 0;
                if (!pop(thread, value, program.error)) {
                    return false;
                }
                args.push_back(value);
            }
            if (op == kStart) {
                const int mask = thread.mask;
                thread.pc += 3;
                if (!spawn(program, func, args, mask, true)) {
                    return false;
                }
                break;
            }
            CallFrame frame;
            frame.pc = thread.pc + 3;
            frame.stack = std::move(thread.stack);
            frame.locals = std::move(thread.locals);
            frame.args_left = thread.args_left;
            thread.calls.push_back(std::move(frame));
            thread.stack.clear();
            thread.locals.clear();
            thread.args_left = argc;
            stack_args(thread, args);
            if (func < 0 || func >= static_cast<int>(program.func_pc.size())) {
                program.error = "cob call names a missing script";
                return false;
            }
            thread.pc = program.func_pc[static_cast<size_t>(func)];
            break;
        }
        case kSpin:
        case kStopSpin:
        case kTurn:
        case kMove:
        case kTurnNow:
        case kMoveNow:
        case kWaitTurn:
        case kWaitMove: {
            if (!immediate(program, thread, 2, word, program.error)) {
                return false;
            }
            const int piece = model_of(program, as_i32(word[0]));
            const int axis = as_i32(word[1]);
            if (piece == -2 || axis < 0 || axis > 2) {
                program.error = "cob piece or axis is missing";
                return false;
            }
            if (op == kWaitTurn || op == kWaitMove) {
                const bool busy = piece >= 0 && ops != nullptr &&
                                  ((op == kWaitTurn && ops->busy_turn != nullptr && ops->busy_turn(ops->user, piece, axis)) ||
                                   (op == kWaitMove && ops->busy_move != nullptr && ops->busy_move(ops->user, piece, axis)));
                if (busy) {
                    thread.wake_frame = program.frame + 1;
                    return true;
                }
                thread.pc += 3;
                break;
            }
            int32_t first = 0;
            int32_t second = 0;
            const bool one = op == kStopSpin;
            if (one) {
                if (!pop(thread, first, program.error)) {
                    return false;
                }
            } else if (!pop_two(thread, first, second, program.error)) {
                // pop_two pops top into second, then the next into first. Source order wants the
                // first pushed value in `first` only when it was the deeper one. Spin, turn, and
                // move pop the first source value off the top, so swap back.
                return false;
            }
            if (!one) {
                // Top of stack was the first source value. pop_two stored it in `second`.
                const int32_t top = second;
                second = first;
                first = top;
            }
            if (piece >= 0 && ops != nullptr) {
                if (op == kSpin && ops->spin != nullptr) {
                    ops->spin(ops->user, piece, axis, first, second);
                } else if (op == kStopSpin && ops->stop_spin != nullptr) {
                    ops->stop_spin(ops->user, piece, axis, first);
                } else if ((op == kTurn || op == kTurnNow) && ops->turn != nullptr) {
                    const int32_t speed = op == kTurnNow ? 0 : second;
                    ops->turn(ops->user, piece, axis, first, speed);
                } else if ((op == kMove || op == kMoveNow) && ops->move != nullptr) {
                    const int32_t speed = op == kMoveNow ? 0 : second;
                    ops->move(ops->user, piece, axis, first, speed);
                }
            }
            thread.pc += 3;
            break;
        }
        case kShow:
        case kHide:
        case kEmitSfx:
        case kExplode: {
            if (!immediate(program, thread, 1, word, program.error)) {
                return false;
            }
            const int piece = model_of(program, as_i32(word[0]));
            if (piece == -2) {
                program.error = "cob piece is missing";
                return false;
            }
            if (op == kEmitSfx || op == kExplode) {
                int32_t ignored = 0;
                if (!pop(thread, ignored, program.error)) {
                    return false;
                }
            } else if (piece >= 0 && ops != nullptr && ops->hide != nullptr) {
                ops->hide(ops->user, piece, op == kHide);
            }
            thread.pc += 2;
            break;
        }
        case kPlaySound:
        case kSet: {
            int32_t ignored = 0;
            const int count = op == kSet ? 2 : 1;
            for (int i = 0; i < count; ++i) {
                if (!pop(thread, ignored, program.error)) {
                    return false;
                }
            }
            thread.pc += 1;
            break;
        }
        default:
            program.error = hex_op(op, thread.pc);
            return false;
        }
    }
    program.error = "cob script ran too long";
    return false;
}

bool drain(CobProgram& program) {
    size_t cursor = 0;
    while (cursor < program.born.size()) {
        const int index = program.born[cursor];
        ++cursor;
        if (index < 0 || index >= static_cast<int>(program.threads.size())) {
            program.error = "cob thread is missing";
            return false;
        }
        if (program.threads[static_cast<size_t>(index)].dead) {
            continue;
        }
        if (!exec(program, index)) {
            return false;
        }
    }
    program.born.clear();
    return program.error.empty();
}

}  // namespace

bool cob_load(CobProgram*& out, const std::string& path, const std::vector<std::string>& piece_names,
              std::string& error) {
    out = nullptr;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not read cob " + path;
        return false;
    }
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (file.size() < 44) {
        error = "cob header is short: " + path;
        return false;
    }
    int32_t header[11] = {};
    for (int i = 0; i < 11; ++i) {
        if (!read_i32(file, i * 4, header[i])) {
            error = "cob header is short: " + path;
            return false;
        }
    }
    if (header[0] != 4) {
        error = "cob version is not 4: " + path;
        return false;
    }
    const int nscripts = header[1];
    const int npieces = header[2];
    const int ncode = header[3];
    const int nstatic = header[4];
    if (nscripts < 1 || npieces < 1 || ncode < 1 || nstatic < 0) {
        error = "cob counts are empty: " + path;
        return false;
    }
    auto* program = new CobProgram();
    program->code.resize(static_cast<size_t>(ncode));
    for (int i = 0; i < ncode; ++i) {
        int32_t word = 0;
        if (!read_i32(file, header[9] + i * 4, word)) {
            delete program;
            error = "cob code is short: " + path;
            return false;
        }
        program->code[static_cast<size_t>(i)] = static_cast<uint32_t>(word);
    }
    program->func_pc.resize(static_cast<size_t>(nscripts));
    program->func_names.resize(static_cast<size_t>(nscripts));
    for (int i = 0; i < nscripts; ++i) {
        int32_t start = 0;
        int32_t name_at = 0;
        if (!read_i32(file, header[6] + i * 4, start) || !read_i32(file, header[7] + i * 4, name_at) ||
            start < 0 || start >= ncode || !read_name(file, name_at, program->func_names[static_cast<size_t>(i)])) {
            delete program;
            error = "cob script table is short: " + path;
            return false;
        }
        program->func_pc[static_cast<size_t>(i)] = start;
    }
    program->piece_names.resize(static_cast<size_t>(npieces));
    program->model_piece.assign(static_cast<size_t>(npieces), -1);
    for (int i = 0; i < npieces; ++i) {
        int32_t name_at = 0;
        if (!read_i32(file, header[8] + i * 4, name_at) ||
            !read_name(file, name_at, program->piece_names[static_cast<size_t>(i)])) {
            delete program;
            error = "cob piece table is short: " + path;
            return false;
        }
        const std::string& name = program->piece_names[static_cast<size_t>(i)];
        for (int piece = 0; piece < static_cast<int>(piece_names.size()); ++piece) {
            if (piece_names[static_cast<size_t>(piece)] == name) {
                program->model_piece[static_cast<size_t>(i)] = piece;
                break;
            }
        }
    }
    program->statics.assign(static_cast<size_t>(nstatic), 0);
    program->threads.reserve(16);
    out = program;
    return true;
}

void cob_free(CobProgram* program) { delete program; }

int cob_model_piece(const CobProgram& program, const char* name) {
    if (name == nullptr) {
        return -1;
    }
    std::string lower = name;
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    for (int i = 0; i < static_cast<int>(program.piece_names.size()); ++i) {
        if (program.piece_names[static_cast<size_t>(i)] == lower) {
            return program.model_piece[static_cast<size_t>(i)];
        }
    }
    return -1;
}

bool cob_call(CobProgram& program, const CobOps& ops, const char* name, const int32_t* args, int argc, int frame,
              std::string& error) {
    program.ops = &ops;
    program.frame = frame;
    program.error.clear();
    program.born.clear();
    const int func = find_func(program, name);
    if (func < 0) {
        error = std::string("cob script has no ") + (name != nullptr ? name : "");
        program.ops = nullptr;
        return false;
    }
    if (argc < 0) {
        argc = 0;
    }
    std::vector<int32_t> popped;
    for (int i = argc - 1; i >= 0; --i) {
        popped.push_back(args != nullptr ? args[i] : 0);
    }
    const int index = static_cast<int>(program.threads.size());
    if (!spawn(program, func, popped, 0, false)) {
        error = program.error;
        program.ops = nullptr;
        return false;
    }
    if (!exec(program, index) || !drain(program)) {
        error = program.error.empty() ? "cob script failed" : program.error;
        program.ops = nullptr;
        return false;
    }
    program.ops = nullptr;
    return true;
}

bool cob_wake(CobProgram& program, const CobOps& ops, int frame, std::string& error) {
    program.ops = &ops;
    program.frame = frame;
    program.error.clear();
    program.born.clear();
    const int count = static_cast<int>(program.threads.size());
    for (int i = 0; i < count; ++i) {
        Thread& thread = program.threads[static_cast<size_t>(i)];
        if (thread.dead || thread.wake_frame < 0 || thread.wake_frame > frame) {
            continue;
        }
        thread.wake_frame = -1;
        if (!exec(program, i)) {
            error = program.error.empty() ? "cob script failed" : program.error;
            program.ops = nullptr;
            return false;
        }
    }
    if (!drain(program)) {
        error = program.error.empty() ? "cob script failed" : program.error;
        program.ops = nullptr;
        return false;
    }
    program.ops = nullptr;
    return true;
}

}  // namespace bar
