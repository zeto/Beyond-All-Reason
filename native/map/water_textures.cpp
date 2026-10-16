// SPDX-License-Identifier: GPL-2.0-or-later
// Procedural water textures: periodic gradient noise for wave heights, periodic
// Worley (F2 - F1) noise for caustics. Both tile exactly over the texture edge.
#include "water_textures.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bar {
namespace {

uint32_t hash3(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ s * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

// Gradient noise on a lattice of `period` cells, wrapping so it tiles.
float periodic_noise(float x, float y, int period, uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - x0;
    const float fy = y - y0;
    auto grad = [&](int ix, int iy, float dx, float dy) {
        const uint32_t h = hash3(static_cast<uint32_t>(((ix % period) + period) % period),
                                 static_cast<uint32_t>(((iy % period) + period) % period), seed);
        const float a = (h & 0xffffu) * (6.2831853f / 65536.f);
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const float n00 = grad(x0, y0, fx, fy);
    const float n10 = grad(x0 + 1, y0, fx - 1.f, fy);
    const float n01 = grad(x0, y0 + 1, fx, fy - 1.f);
    const float n11 = grad(x0 + 1, y0 + 1, fx - 1.f, fy - 1.f);
    const float u = fade(fx);
    const float v = fade(fy);
    return (n00 * (1 - u) + n10 * u) * (1 - v) + (n01 * (1 - u) + n11 * u) * v;
}

void finish_rgba(GpuImage& img, int size) {
    img.format = GpuImage::Format::RGBA8;
    img.width = size;
    img.height = size;
    img.faces = 1;
    build_rgba_mips(img);
}

}  // namespace

GpuImage generate_wave_normals(int size, unsigned seed) {
    std::vector<float> h(static_cast<size_t>(size) * size, 0.f);
    // fBm: base period 4 cells, doubling, amplitude halving. Integer periods keep it tileable.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float sum = 0.f;
            float amp = 1.f;
            int period = 4;
            for (int o = 0; o < 5; ++o) {
                const float sx = static_cast<float>(x) / size * period;
                const float sy = static_cast<float>(y) / size * period;
                sum += amp * periodic_noise(sx, sy, period, seed + o * 7919u);
                amp *= 0.5f;
                period *= 2;
            }
            h[static_cast<size_t>(y) * size + x] = sum;
        }
    }
    float lo = 1e9f, hi = -1e9f;
    for (float v : h) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float span = std::max(1e-6f, hi - lo);
    GpuImage img;
    img.levels.assign(1, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4));
    auto at = [&](int x, int y) { return h[static_cast<size_t>((y + size) % size) * size + (x + size) % size]; };
    // Slope scale: one full height span over ~1/8 texture gives moderate wave steepness.
    const float k = 8.f / span;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (at(x + 1, y) - at(x - 1, y)) * 0.5f * k;
            const float dy = (at(x, y + 1) - at(x, y - 1)) * 0.5f * k;
            float n[3] = {-dx, -dy, 1.f};
            const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            uint8_t* p = &img.levels[0][(static_cast<size_t>(y) * size + x) * 4];
            for (int c = 0; c < 3; ++c) {
                p[c] = static_cast<uint8_t>(std::clamp((n[c] / l * 0.5f + 0.5f) * 255.f + 0.5f, 0.f, 255.f));
            }
            p[3] = static_cast<uint8_t>(std::clamp((at(x, y) - lo) / span * 255.f + 0.5f, 0.f, 255.f));
        }
    }
    finish_rgba(img, size);
    return img;
}

GpuImage generate_caustics(int size, unsigned seed) {
    constexpr int kCells = 8;
    float px[kCells][kCells];
    float py[kCells][kCells];
    for (int j = 0; j < kCells; ++j) {
        for (int i = 0; i < kCells; ++i) {
            const uint32_t hsh = hash3(i, j, seed);
            px[j][i] = (hsh & 0xffffu) / 65535.f;
            py[j][i] = (hsh >> 16) / 65535.f;
        }
    }
    GpuImage img;
    img.levels.assign(1, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) / size * kCells;
            const float fy = static_cast<float>(y) / size * kCells;
            const int cx = static_cast<int>(fx);
            const int cy = static_cast<int>(fy);
            float f1 = 1e9f, f2 = 1e9f;
            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    const int i = cx + di;
                    const int j = cy + dj;
                    const int wi = (i % kCells + kCells) % kCells;
                    const int wj = (j % kCells + kCells) % kCells;
                    const float ddx = i + px[wj][wi] - fx;
                    const float ddy = j + py[wj][wi] - fy;
                    const float d = std::sqrt(ddx * ddx + ddy * ddy);
                    if (d < f1) {
                        f2 = f1;
                        f1 = d;
                    } else if (d < f2) {
                        f2 = d;
                    }
                }
            }
            // Thin bright filaments along cell borders.
            const float edge = std::pow(std::clamp(1.f - (f2 - f1) * 3.f, 0.f, 1.f), 4.f);
            const uint8_t v = static_cast<uint8_t>(edge * 255.f + 0.5f);
            uint8_t* p = &img.levels[0][(static_cast<size_t>(y) * size + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    finish_rgba(img, size);
    return img;
}

}  // namespace bar
