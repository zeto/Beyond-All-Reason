#include "gl/image_cache.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace bar {
namespace {

std::mutex g_mutex;
std::vector<std::string> g_roots;
std::unordered_map<std::string, int> g_by_name;  // resolved path -> id (0 = failed)
std::vector<std::shared_ptr<const DecodedImage>> g_images{nullptr};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Spring texture names carry option prefixes like ":n:", ":l:", ":lr128,128:". Strip them.
std::string strip_flags(const std::string& name) {
    std::string out = name;
    while (out.size() > 1 && out[0] == ':') {
        const size_t close = out.find(':', 1);
        if (close == std::string::npos) break;
        out = out.substr(close + 1);
    }
    return out;
}

// Recoil NamedTextures option letters that change pixels: 'i' InvertColors, then
// 'g' MakeGrayScale (luma 0.299/0.587/0.114, times 256/255). Others are sampling hints.
struct PixelFlags {
    bool invert = false;
    bool grey = false;
    bool nearest = false;
    bool linear = false;
};

PixelFlags parse_flags(const std::string& name) {
    PixelFlags f;
    if (name.size() < 2 || name[0] != ':') return f;
    const size_t close = name.find(':', 1);
    if (close == std::string::npos) return f;
    for (size_t i = 1; i < close; ++i) {
        const char c = name[i];
        if (c == 'r' || c == 't') {
            // r<w>,<h> / t<r>,<g>,<b>: skip the numbers
            while (i + 1 < close && (std::isdigit(static_cast<unsigned char>(name[i + 1])) || name[i + 1] == ',' ||
                                     name[i + 1] == '.')) {
                ++i;
            }
        } else if (c == 'i') {
            f.invert = true;
        } else if (c == 'g') {
            f.grey = true;
        } else if (c == 'n') {
            f.nearest = true;
        } else if (c == 'l') {
            f.linear = true;
        }
    }
    return f;
}

// Pixels are premultiplied, so both ops are applied in alpha-scaled form:
// invert (1-c)*a = a - c*a, and luma is linear in c*a (clamped to a).
void apply_pixel_flags(DecodedImage& img, PixelFlags f) {
    if (!f.invert && !f.grey) return;
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        float r = img.rgba[i], g = img.rgba[i + 1], b = img.rgba[i + 2];
        const float a = img.rgba[i + 3];
        if (f.invert) {
            r = a - r;
            g = a - g;
            b = a - b;
        }
        if (f.grey) {
            const float luma = (r * 0.299f + g * 0.587f + b * 0.114f) * (256.f / 255.f);
            r = g = b = std::min(luma, a);
        }
        img.rgba[i] = static_cast<uint8_t>(std::clamp(r, 0.f, 255.f) + 0.5f);
        img.rgba[i + 1] = static_cast<uint8_t>(std::clamp(g, 0.f, 255.f) + 0.5f);
        img.rgba[i + 2] = static_cast<uint8_t>(std::clamp(b, 0.f, 255.f) + 0.5f);
    }
}

// Case-insensitive path walk under root (the stock tree mixes LuaUI/luaui, ARMCK.DDS/armck.dds).
std::string find_ci(const std::string& root, const std::string& rel) {
    namespace fs = std::filesystem;
    fs::path cur(root);
    std::error_code ec;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find_first_of("/\\", start);
        const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? rel.size() + 1 : slash + 1;
        if (part.empty() || part == ".") continue;
        fs::path direct = cur / part;
        if (fs::exists(direct, ec)) { cur = direct; continue; }
        bool found = false;
        const std::string want = lower(part);
        for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
            if (lower(it->path().filename().string()) == want) { cur = it->path(); found = true; break; }
        }
        if (!found) return {};
    }
    return fs::is_regular_file(cur, ec) ? cur.string() : std::string();
}

void premultiply(DecodedImage& img) {
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        const unsigned a = img.rgba[i + 3];
        img.rgba[i] = static_cast<uint8_t>((img.rgba[i] * a + 127) / 255);
        img.rgba[i + 1] = static_cast<uint8_t>((img.rgba[i + 1] * a + 127) / 255);
        img.rgba[i + 2] = static_cast<uint8_t>((img.rgba[i + 2] * a + 127) / 255);
    }
}

void flip_rows(DecodedImage& img) {
    const size_t row = static_cast<size_t>(img.width) * 4;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < img.height / 2; ++y) {
        uint8_t* a = img.rgba.data() + y * row;
        uint8_t* b = img.rgba.data() + (img.height - 1 - y) * row;
        std::memcpy(tmp.data(), a, row);
        std::memcpy(a, b, row);
        std::memcpy(b, tmp.data(), row);
    }
}

// ---- DDS (BC1/BC2/BC3 and uncompressed 8/24/32-bit), top-down as stored ----
void rgb565(uint16_t c, uint8_t* out) {
    out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
}

void color_block(const uint8_t* b, uint8_t px[16][4], bool bc1) {
    const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t pal[4][4] = {};
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (!bc1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    const uint32_t idx = static_cast<uint32_t>(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
    for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

bool decode_dds(const std::vector<uint8_t>& f, DecodedImage& img) {
    if (f.size() < 128 || std::memcmp(f.data(), "DDS ", 4) != 0) return false;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, f.data() + o, 4); return v; };
    const int h = static_cast<int>(u32(12)), w = static_cast<int>(u32(16));
    const uint32_t pf_flags = u32(80);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    const uint8_t* data = f.data() + 128;
    const size_t avail = f.size() - 128;
    if (pf_flags & 0x4) {
        const char* four = reinterpret_cast<const char*>(f.data() + 84);
        int kind = 0;
        if (std::memcmp(four, "DXT1", 4) == 0) kind = 1;
        else if (std::memcmp(four, "DXT3", 4) == 0) kind = 3;
        else if (std::memcmp(four, "DXT5", 4) == 0) kind = 5;
        else return false;
        const size_t block = kind == 1 ? 8 : 16;
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (avail < block * bw * bh) return false;
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* b = data + block * (by * bw + bx);
                uint8_t px[16][4];
                color_block(kind == 1 ? b : b + 8, px, kind == 1);
                if (kind == 3) {
                    for (int i = 0; i < 16; ++i) {
                        const uint8_t nib = (b[i / 2] >> ((i & 1) * 4)) & 15;
                        px[i][3] = static_cast<uint8_t>(nib * 17);
                    }
                } else if (kind == 5) {
                    const uint8_t a0 = b[0], a1 = b[1];
                    uint8_t ap[8] = {a0, a1};
                    if (a0 > a1) for (int k = 1; k < 7; ++k) ap[k + 1] = static_cast<uint8_t>(((7 - k) * a0 + k * a1) / 7);
                    else {
                        for (int k = 1; k < 5; ++k) ap[k + 1] = static_cast<uint8_t>(((5 - k) * a0 + k * a1) / 5);
                        ap[6] = 0; ap[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
                    for (int i = 0; i < 16; ++i) px[i][3] = ap[(bits >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; ++i) {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) std::memcpy(&img.rgba[(static_cast<size_t>(y) * w + x) * 4], px[i], 4);
                }
            }
        }
        return true;
    }
    const uint32_t bits = u32(88);
    const uint32_t rm = u32(92), gm = u32(96), bm = u32(100), am = u32(104);
    const size_t bpp = bits / 8;
    if (bpp == 0 || bpp > 4 || avail < bpp * w * h) return false;
    auto chan = [](uint32_t v, uint32_t mask) -> uint8_t {
        if (mask == 0) return 0;
        int shift = 0;
        while (((mask >> shift) & 1) == 0) ++shift;
        const uint32_t max = mask >> shift;
        return static_cast<uint8_t>(((v & mask) >> shift) * 255 / max);
    };
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        uint32_t v = 0;
        std::memcpy(&v, data + i * bpp, bpp);
        uint8_t* o = &img.rgba[i * 4];
        if (bpp == 1 && rm == 0 && gm == 0 && bm == 0) {  // alpha-only or luminance
            o[0] = o[1] = o[2] = (pf_flags & 0x2) ? 255 : static_cast<uint8_t>(v);
            o[3] = (pf_flags & 0x2) ? static_cast<uint8_t>(v) : 255;
            continue;
        }
        o[0] = chan(v, rm);
        o[1] = gm ? chan(v, gm) : o[0];
        o[2] = bm ? chan(v, bm) : o[0];
        o[3] = (am && (pf_flags & 0x1)) ? chan(v, am) : 255;
    }
    return true;
}

bool decode_imageio(const std::vector<uint8_t>& f, DecodedImage& img) {
    CFDataRef data = CFDataCreate(nullptr, f.data(), static_cast<CFIndex>(f.size()));
    if (data == nullptr) return false;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (src == nullptr) return false;
    CGImageRef cg = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (cg == nullptr) return false;
    img.width = static_cast<int>(CGImageGetWidth(cg));
    img.height = static_cast<int>(CGImageGetHeight(cg));
    img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    // Premultiplied RGBA, row 0 = top of the image (CG bitmap memory order).
    CGContextRef ctx = CGBitmapContextCreate(img.rgba.data(), img.width, img.height, 8, img.width * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (ctx == nullptr) { CGImageRelease(cg); return false; }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, img.width, img.height), cg);
    CGContextRelease(ctx);
    CGImageRelease(cg);
    return img.width > 0 && img.height > 0;
}

}  // namespace

void image_set_roots(const std::vector<std::string>& roots) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_roots = roots;
}

int image_acquire(const std::string& spring_name) {
    const std::string rel = strip_flags(spring_name);
    if (rel.empty() || rel[0] == '#' || rel[0] == '%' || rel[0] == '$' || rel[0] == '!') return 0;
    const PixelFlags flags = parse_flags(spring_name);
    const std::string key = rel + (flags.invert ? "|i" : "") + (flags.grey ? "|g" : "") +
                            ((flags.nearest || flags.linear) ? "|nomip" : "");
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto hit = g_by_name.find(key);
        if (hit != g_by_name.end()) return hit->second;
        roots = g_roots;
    }
    std::string path;
    for (const std::string& root : roots) {
        path = find_ci(root, rel);
        if (!path.empty()) break;
    }
    int id = 0;
    if (!path.empty()) {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto img = std::make_shared<DecodedImage>();
        const std::string ext = lower(std::filesystem::path(path).extension().string());
        bool ok = false;
        if (ext == ".dds") {
            ok = decode_dds(bytes, *img);
            if (ok) {
                premultiply(*img);
                // Recoil Bitmap::Load flips DDS rows except under unitpics/ (kept as-is).
                if (lower(rel).find("unitpics") == std::string::npos) flip_rows(*img);
            }
        } else {
            ok = decode_imageio(bytes, *img);
        }
        if (ok) {
            apply_pixel_flags(*img, flags);
            img->mipmaps = !(flags.nearest || flags.linear);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_images.push_back(img);
            id = static_cast<int>(g_images.size()) - 1;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_by_name[key] = id;
    return id;
}

std::shared_ptr<const DecodedImage> image_get(int id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (id <= 0 || id >= static_cast<int>(g_images.size())) return nullptr;
    return g_images[static_cast<size_t>(id)];
}

}  // namespace bar
