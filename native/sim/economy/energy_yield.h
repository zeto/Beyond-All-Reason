#pragma once

#include <string>

namespace bar {

// Recoil starts a wind cycle on a vector whose length is the average of the map
// range. The blend weight on that first update is 0, so the strength is the
// average. This slice holds that strength. It does not walk the later gust.
inline float wind_cycle_strength(float min_wind, float max_wind) {
    return (min_wind + max_wind) * 0.5f;
}

// The generator produces the current wind, and never more than its cap.
inline float capped_wind_energy(float strength, float cap) {
    return strength < cap ? strength : cap;
}

// One active solar or wind. production is energy per second from a negative
// upkeep. rate is energy per second from the wind cap. Either may be 0.
struct GenTrace {
    std::string def_name;
    float x = 0;
    float z = 0;
    float upkeep = 0;
    float production = 0;
    float wind_cap = 0;
    float wind = 0;
    float rate = 0;
};

}  // namespace bar
