#pragma once

#include "mkrender/Config.h"
#include "mkrender/Math.h"

#include <string>
#include <vector>

// mkbake: the CPU half of rendering assets, no GPU and no files (the host reads and writes bytes). MealKit's engine and the MkRender
// scene editor both bake with it, so their caches and probe files are the same bytes.
//   - sky HDRI (equirectangular linear rgb) -> sky cube, reflection cube with GGX mips (mip k = roughness k / (mips - 1)), 9 SH ambient coefficients
//   - reflection probe (six rendered faces) -> reflection cube with the same mips, the .mkprobe file
//   - .cube 3D LUT -> resampled grid for Renderer::createLut
//   - texture mips and BC7/BC5 compression (MKBAKE_BC; bc7enc/rgbcx), the .mktex cache file
// Cube faces are +X -X +Y -Y +Z -Z (cubeDirection, DX12/Vulkan rules, left-handed y-up), cube texels RGB9E5, per face from mip 0.
namespace mkrender {

// --- Sky HDRI
struct EnvironmentMap {
    f32 sh[9][3] = {}; // ambient color = sum sh[i] * Y_i(n) (real SH orders 0..2, the Lambert convolution multiplied in)
    u32 skySize = 0;   // sky cube face size (one mip)
    std::vector<u32> sky;
    u32 reflectSize = 0, reflectMips = 0; // reflection cube, per face from mip 0
    std::vector<u32> reflect;
};
void bakeEnvironment(const f32* rgb, u32 width, u32 height, EnvironmentMap& out);
// Cache file "MKEV" (header, SH, sky, reflection). The name is the content hash of the .hdr file with the bake version:
// "<16 hex>.mkenv", so changing the baking (kEnvironmentVersion) never reads old files
std::string environmentCacheName(const std::string& hdrBytes);
std::string environmentFileBytes(const EnvironmentMap& map);
bool readEnvironmentFile(const std::string& bytes, EnvironmentMap& out); // false: not a cache file of this version

// --- Reflection probes (.mkprobe, committed with the game: shipping reads them without baking)
struct ProbeMap {
    u32 size = 0, mips = 0;
    std::vector<u32> reflect;
};
// faces: six faces x faceSize^2 x linear rgb (row order, the camera of cubeFaceForward/cubeFaceUp). Mips down to 4
void bakeProbe(const f32* faces, u32 faceSize, ProbeMap& out);
std::string probeFileBytes(const ProbeMap& probe);
bool readProbeFile(const std::string& bytes, ProbeMap& out, std::string& error);
// The camera that renders face i (90 degree fov, square): lookAtLH(eye, eye + forward, up) gives cubeDirection's layout
Vec3 cubeFaceForward(u32 face);
Vec3 cubeFaceUp(u32 face);

// --- Parts (unit tests, shaders do the same)
u32 packRgb9e5(f32 r, f32 g, f32 b); // negatives become 0, too large values the max
void unpackRgb9e5(u32 v, f32 out[3]);
Vec3 cubeDirection(u32 face, f32 s, f32 t); // face 0..5, s/t = -1..1 (texel center) -> normalized direction
Vec3 evalSh(const f32 (&sh)[9][3], Vec3 n);

// --- Color grading LUT (.cube from Resolve, Photoshop and others): sRGB values resampled to size^3 cells (red fastest), trilinear,
// DOMAIN_MIN/MAX honored. 1D LUTs and bad files return false and a reason
bool parseCubeLut(const std::string& text, u32 size, std::vector<f32>& out, std::string& error);
std::string neutralCubeLut(u32 size); // identity table, a start for grading in an external tool

// --- Textures
// Values are MealKit's asset::TextureUsage (they are part of cache names)
enum class TextureUsage : u8 {
    Color,  // base color, emissive: sRGB BC7
    Linear, // metallic-roughness: BC7
    Normal, // normal map: BC5 (x, y; the shader rebuilds z)
};
struct CompressedTexture {
    u32 width = 0, height = 0, mipLevels = 0;
    mkrhi::Format format = mkrhi::Format::Bc7Unorm;
    std::vector<u8> data; // mkrhi::textureMipBytes each, from mip 0
};
// RGBA8 mip chain down to 1x1 (2x2 box). srgb averages in linear; normal renormalizes
void buildMips(const u8* rgba, u32 width, u32 height, bool srgb, bool normal, std::vector<std::vector<u8>>& mips);
bool hasTextureCompression(); // false when built without MKBAKE_BC
// Mips + BC7 (Color sRGB, Linear) or BC5 (Normal). False without MKBAKE_BC
bool compressTexture(const u8* rgba, u32 width, u32 height, TextureUsage usage, CompressedTexture& out);
// Cache file "MKTX": the name is the hash of the encoded image (PNG/JPEG bytes) with the usage and compression version
std::string textureCacheName(const void* encoded, size_t bytes, TextureUsage usage);
std::string textureFileBytes(const CompressedTexture& texture);
bool readTextureFile(const std::string& bytes, CompressedTexture& out);

// 64-bit FNV-1a (MealKit's Hasher), used for the cache names
u64 fnv1a64(const void* data, size_t bytes, u64 hash = 14695981039346656037ull);

} // namespace mkrender
