#pragma once

namespace bar {

// One worker. Later milestones add threads. Each call sees a contiguous index range.
inline void run_range(int begin, int end, void (*fn)(int index, void* user), void* user) {
    for (int index = begin; index < end; ++index) {
        fn(index, user);
    }
}

}  // namespace bar