#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bar {

// Decoded image: RGBA8, premultiplied alpha, row 0 is the row GL texcoord t=0 samples.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    // Recoil builds a mip chain for named textures unless ':n:' or ':l:' asks for
    // plain nearest/linear sampling (NamedTextures needMipMaps).
    bool mipmaps = true;
};

// Search roots for art: the stock game tree, then the BAR content dir. Read-only.
void image_set_roots(const std::vector<std::string>& roots);

// Resolves a Spring texture name (":n:LuaUI/Images/x.png", "unitpics/armcom.dds") to a
// stable id > 0 and decodes it once. Returns 0 when the file is missing or undecodable.
int image_acquire(const std::string& spring_name);

// Decoded pixels for an id from image_acquire. Thread-safe; null for unknown ids.
std::shared_ptr<const DecodedImage> image_get(int id);

}  // namespace bar
