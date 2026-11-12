#include "mkrender/Bake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if MKBAKE_BC
#include "bc7enc.h"
#include "rgbcx.h"
#endif

// Moved from MealKit's modules/render/TextureCache.cpp: same mips, same blocks, same cache bytes and names.
namespace mkrender {
namespace {

// Cache file: "MKTX", format version, platform format family (0 = BC; ASTC for mobile later), mkrhi::Format, size, mip count, data
constexpr u32 kMagic = 0x58544B4D; // "MKTX"
constexpr u32 kVersion = 1;        // bump when the compression changes (it's in the cache name, so old files aren't used)
constexpr u32 kFamilyBc = 0;

struct Header {
    u32 magic, version, family, format, width, height, mipLevels, reserved;
};

f32 toLinear(u8 c) {
    const f32 v = c / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
u8 toSrgb(f32 v) {
    v = std::clamp(v, 0.0f, 1.0f);
    const f32 s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(s * 255.0f + 0.5f);
}

#if MKBAKE_BC
// Per 4x4 block, edges repeat the last pixel
void compress(const std::vector<u8>& rgba, u32 w, u32 h, TextureUsage usage, std::vector<u8>& out) {
    static const bool ready = [] {
        bc7enc_compress_block_init();
        rgbcx::init();
        return true;
    }();
    (void)ready;
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (usage != TextureUsage::Color) bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level = 1; // between quality and speed (once at load)
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t start = out.size();
    out.resize(start + static_cast<size_t>(bw) * bh * 16);
    u8 block[16 * 4];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            for (u32 y = 0; y < 4; ++y)
                for (u32 x = 0; x < 4; ++x) {
                    const u32 sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                    std::memcpy(block + (y * 4 + x) * 4, &rgba[(static_cast<size_t>(sy) * w + sx) * 4], 4);
                }
            u8* dst = out.data() + start + (static_cast<size_t>(by) * bw + bx) * 16;
            if (usage == TextureUsage::Normal) rgbcx::encode_bc5_hq(dst, block, 0, 1, 4);
            else bc7enc_compress_block(dst, block, &params);
        }
    }
}
#endif

} // namespace

void buildMips(const u8* rgba, u32 width, u32 height, bool srgb, bool normal, std::vector<std::vector<u8>>& mips) {
    mips.clear();
    mips.emplace_back(rgba, rgba + static_cast<size_t>(width) * height * 4);
    u32 w = width, h = height;
    while (w > 1 || h > 1) {
        const u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const std::vector<u8>& src = mips.back();
        std::vector<u8> dst(static_cast<size_t>(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x) {
                f32 sum[4] = {};
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                        const u8* p = &src[(static_cast<size_t>(sy) * w + sx) * 4];
                        for (int c = 0; c < 4; ++c) sum[c] += (srgb && c < 3) ? toLinear(p[c]) : p[c] / 255.0f;
                    }
                u8* q = &dst[(static_cast<size_t>(y) * nw + x) * 4];
                if (normal) { // normals are averaged and renormalized to length 1
                    f32 n[3];
                    for (int c = 0; c < 3; ++c) n[c] = sum[c] / 2.0f - 1.0f; // (sum/4)*2-1
                    const f32 len = std::max(1e-6f, std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));
                    for (int c = 0; c < 3; ++c) q[c] = static_cast<u8>(std::clamp(n[c] / len * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    q[3] = static_cast<u8>(sum[3] / 4.0f * 255.0f + 0.5f);
                    continue;
                }
                for (int c = 0; c < 4; ++c)
                    q[c] = (srgb && c < 3) ? toSrgb(sum[c] / 4.0f) : static_cast<u8>(sum[c] / 4.0f * 255.0f + 0.5f);
            }
        mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

bool hasTextureCompression() { return MKBAKE_BC != 0; }

bool compressTexture(const u8* rgba, u32 width, u32 height, TextureUsage usage, CompressedTexture& out) {
#if MKBAKE_BC
    std::vector<std::vector<u8>> mips;
    buildMips(rgba, width, height, usage == TextureUsage::Color, usage == TextureUsage::Normal, mips);
    out.width = width;
    out.height = height;
    out.mipLevels = static_cast<u32>(mips.size());
    out.format = usage == TextureUsage::Normal  ? mkrhi::Format::Bc5Unorm
                 : usage == TextureUsage::Color ? mkrhi::Format::Bc7Srgb
                                                : mkrhi::Format::Bc7Unorm;
    out.data.clear();
    for (u32 mip = 0; mip < out.mipLevels; ++mip)
        compress(mips[mip], std::max(1u, width >> mip), std::max(1u, height >> mip), usage, out.data);
    return true;
#else
    (void)rgba, (void)width, (void)height, (void)usage, (void)out;
    return false;
#endif
}

std::string textureCacheName(const void* encoded, size_t bytes, TextureUsage usage) {
    u64 h = fnv1a64(encoded, bytes);
    const u64 parts[2] = {static_cast<u64>(usage), kVersion}; // MealKit Hasher::add(u64) twice
    h = fnv1a64(parts, sizeof(parts), h);
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.mktex", static_cast<unsigned long long>(h));
    return name;
}

std::string textureFileBytes(const CompressedTexture& t) {
    const Header hd{kMagic, kVersion, kFamilyBc, static_cast<u32>(t.format), t.width, t.height, t.mipLevels, 0};
    std::string file(reinterpret_cast<const char*>(&hd), sizeof(hd));
    file.append(reinterpret_cast<const char*>(t.data.data()), t.data.size());
    return file;
}

bool readTextureFile(const std::string& file, CompressedTexture& out) {
    if (file.size() < sizeof(Header)) return false;
    Header hd;
    std::memcpy(&hd, file.data(), sizeof(hd));
    if (hd.magic != kMagic || hd.version != kVersion || hd.family != kFamilyBc || hd.mipLevels == 0 || hd.mipLevels > 16) return false;
    const mkrhi::Format format = static_cast<mkrhi::Format>(hd.format);
    if (!mkrhi::isBlockCompressed(format)) return false;
    u64 expect = 0;
    for (u32 mip = 0; mip < hd.mipLevels; ++mip)
        expect += mkrhi::textureMipBytes(format, std::max(1u, hd.width >> mip), std::max(1u, hd.height >> mip));
    if (file.size() != sizeof(Header) + expect) return false;
    out.width = hd.width;
    out.height = hd.height;
    out.mipLevels = hd.mipLevels;
    out.format = format;
    out.data.assign(file.begin() + sizeof(Header), file.end());
    return true;
}

} // namespace mkrender
