// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "smf_textures.h"

namespace bar {

// Procedurally generated water art (our own; no engine textures are used).
// Tileable wave normal map: RGB = normal * 0.5 + 0.5 (tangent space, z up), A = height.
GpuImage generate_wave_normals(int size, unsigned seed);
// Tileable caustics: bright cell-edge network in RGB, A = 1.
GpuImage generate_caustics(int size, unsigned seed);

}  // namespace bar
