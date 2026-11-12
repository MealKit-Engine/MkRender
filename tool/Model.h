#pragma once

#include "mkrender/Mesh.h"

#include <string>
#include <vector>

// CPU side of what the viewer shows: a glTF file or built-in shapes, before upload (App::upload)
namespace mkrender::tool {

// Decoded texture (RGBA8). srgb: base color / emissive, linear: normal and metallic-roughness maps
struct TextureImage {
    u32 width = 0, height = 0;
    bool srgb = true;
    std::vector<u8> rgba;
};

struct ModelData {
    MeshData mesh;                        // vertex material = index into materials (changed to renderer indices on upload)
    std::vector<MaterialData> materials;  // texture fields = index into images until upload
    std::vector<std::string> materialNames;
    std::vector<TextureImage> images;
    Vec3 boundsMin, boundsMax;
    std::vector<std::string> warnings;    // ignored glTF features, unreadable images
};

// Static meshes of the default scene (or first) of a .gltf/.glb, flattened with node transforms, z flipped to left-handed.
// Skins and animations are drawn in the bind pose
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
