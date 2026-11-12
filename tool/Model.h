#pragma once

#include "Anim.h"

#include "mkrender/Mesh.h"

#include <string>
#include <vector>

// CPU side of a glTF model before upload (Assets::model), and built-in shapes
namespace mkrender::tool {

// Decoded texture (RGBA8). srgb: base color / emissive, linear: normal and metallic-roughness maps
struct TextureImage {
    u32 width = 0, height = 0;
    bool srgb = true;
    u8 usage = 0;         // mkrender::TextureUsage (Color, Linear, Normal): picks BC7 sRGB / BC7 / BC5
    std::string encoded;  // PNG/JPEG bytes as in the glTF (the texture cache name is their hash, like the engine's)
    std::vector<u8> rgba;
};

struct ModelData {
    MeshData mesh;                        // vertex material = index into materials (changed to renderer indices on upload)
    std::vector<MaterialData> materials;  // texture fields = index into images until upload
    std::vector<std::string> materialNames;
    std::vector<TextureImage> images;
    Vec3 boundsMin, boundsMax;
    std::vector<std::string> warnings;    // ignored glTF features, unreadable images
    Skeleton skeleton;                    // first skin (empty: static model); mesh.skin has one entry per vertex then
    std::vector<AnimationClip> clips;     // its animations baked at 30 Hz
};

// Static meshes of the default scene (or first) of a .gltf/.glb, flattened with node transforms, z flipped to left-handed.
// The first skin and its animations are read like the engine's asset/ModelImport.cpp (other skins are drawn in the bind pose)
bool loadGltf(const std::string& path, ModelData& out, std::string& error);

u32 packHalf2(f32 u, f32 v);
void computeBounds(ModelData& model);
// UV sphere / box / floor with one material
MeshData makeSphere(f32 radius, u32 segments, u32 rings, u16 material);
MeshData makeBox(Vec3 half, u16 material);
MeshData makePlane(f32 half, u16 material);

// Full mip chain (box filter, sRGB averaged in linear) back to back from mip 0: mkrhi::TextureInit::data
std::vector<u8> buildMips(const TextureImage& image, u32& mipLevels);

} // namespace mkrender::tool
