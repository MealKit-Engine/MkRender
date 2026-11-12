#pragma once

#include "mkrender/Math.h"

#include <vector>

// CPU mesh and material data the renderer uploads (Renderer::createMesh, createMaterial).
// A host can use its own types instead (mkrender/Config.h, MKRENDER_HOST_MESH).
#ifndef MKRENDER_HOST_MESH
namespace mkrender {

// 36 bytes: texture coordinates are two halfs, material is an index into the renderer's material table
struct Vertex {
    Vec3 position;
    Vec3 normal;
    u32 color = 0xFFFFFFFF; // RGBA8 sRGB, multiplied with the instance color and material base color
    u32 uv = 0;             // half u | half v << 16
    u16 material = 0;       // Renderer::createMaterial index (0 = default white)
    u16 reserved = 0;
};

// Four joints of a skinned vertex: indices u8 x4, weights unorm8 x4 (sum 255)
struct SkinWeights {
    u32 joints = 0;
    u32 weights = 0xFFu; // first joint 1
};

// One slot of the skin palette (48 bytes read by the vertex shader: three rows, p' = (dot(r0, p1), dot(r1, p1), dot(r2, p1)))
struct BoneMatrix {
    f32 rows[3][4];
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;      // with fewer than 65536 vertices, uploaded to the GPU as 16-bit
    std::vector<SkinWeights> skin; // empty for static meshes, one per vertex if present (skinned mesh)
};

// Renderer mesh handle (Renderer::createMesh)
using MeshId = u32;
inline constexpr MeshId kNoMesh = 0xFFFFFFFFu; // not drawn

// Textures are bindless indices (Renderer::createTexture); kNoTexture = none
inline constexpr u32 kNoTexture = 0xFFFFFFFFu;

// Material features: chosen inside the shader by material table bits (one draw call even when features mix in a mesh)
inline constexpr u32 kMaterialOutline = 1u;  // inverted hull pass (the mesh is drawn once more)
inline constexpr u32 kMaterialWind = 2u;     // wind sway, strength in vertex color alpha
inline constexpr u32 kMaterialRim = 4u;      // rim light
inline constexpr u32 kMaterialDissolve = 8u; // disappears by the instance's dissolve progress (instanceDissolve)

// glTF PBR metallic-roughness range plus the style features. Colors are linear (shaders/Forward.hlsl Material, 64 bytes on the GPU)
struct MaterialData {
    f32 baseColor[4] = {1, 1, 1, 1};
    f32 emissive[3] = {0, 0, 0};
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    f32 normalScale = 1.0f;
    f32 alphaCutoff = 0.0f; // > 0 means alpha mask
    u32 baseColorTexture = kNoTexture;
    u32 normalTexture = kNoTexture;
    u32 metallicRoughnessTexture = kNoTexture; // G = roughness, B = metal
    u32 emissiveTexture = kNoTexture;
    u32 features = 0; // kMaterial* bits
    f32 outlineWidth = 0.02f; // meters
    f32 outlineColor[3] = {0, 0, 0};
    f32 windStrength = 0.1f; // meters at vertex alpha 1 and FrameParams::windStrength 1
    f32 rimStrength = 0.5f;
    f32 rimColor[3] = {1, 1, 1};
    f32 dissolveEdge = 0.05f; // glowing width of the disappearing edge (progress units 0..1)
    f32 dissolveColor[3] = {4.0f, 1.6f, 0.4f};
};

} // namespace mkrender
#endif

namespace mkrender {
static_assert(sizeof(Vertex) == 36 && sizeof(SkinWeights) == 8 && sizeof(BoneMatrix) == 48, "mkrender mesh layouts");
} // namespace mkrender
