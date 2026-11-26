// RHI binding layout (mealkit/rhi/Rhi.h, docs/DECISIONS.md 2026-10-19 "RHI binding layout"). Shared by all shaders.
// DX12: root constants b0 + three descriptor tables (space1 buffers, space2 textures, space3 cubes: same heap slots as textures) + static samplers s0/s1
// Vulkan: push constants + set 0 binding 0 (buffer array), 1 (texture array), 2/3 (static samplers), 4 (cube array)
// Structs are read field by field with Load (avoids differences in struct layout rules between the two APIs).
//
// Shaders reach g_buffers[] / g_textures[] / g_cubes[] only through the mk* functions below, so a backend without descriptor
// arrays can replace their bodies. MKRHI_WEBGPU (WebGPU, work in progress): no resource arrays, see the block below.

#if MKRHI_WEBGPU
// WebGPU: SPIR-V for Tint (SPIR-V 1.3, no SPV_EXT_descriptor_indexing). Group 0: upload ring + samplers. Group 2: direct slots,
// separately bound textures picked by the index (pool 31 = direct, bits 18..11 = slot): 16 2D (bindings 0..15), 4 cubes (16..19) and
// 4 depth textures (20..23, shadow maps read with a comparison sampler), mkrhi/webgpu/Wgpu.h kDirectTextures/kDirectCubes/kDirectDepths. Group 1 (texture array pools) is not done yet, so every texture is direct for now.
[[vk::binding(0, 0)]] ByteAddressBuffer g_ring;
[[vk::binding(2, 0)]] SamplerState g_linearClamp;
[[vk::binding(3, 0)]] SamplerState g_linearWrap;
[[vk::binding(4, 0)]] SamplerComparisonState g_shadowCompare; // GreaterEqual: 1 where the reference is at or in front of the texel
[[vk::binding(0, 2)]] Texture2D g_direct0;
[[vk::binding(1, 2)]] Texture2D g_direct1;
[[vk::binding(2, 2)]] Texture2D g_direct2;
[[vk::binding(3, 2)]] Texture2D g_direct3;
[[vk::binding(4, 2)]] Texture2D g_direct4;
[[vk::binding(5, 2)]] Texture2D g_direct5;
[[vk::binding(6, 2)]] Texture2D g_direct6;
[[vk::binding(7, 2)]] Texture2D g_direct7;
[[vk::binding(8, 2)]] Texture2D g_direct8;
[[vk::binding(9, 2)]] Texture2D g_direct9;
[[vk::binding(10, 2)]] Texture2D g_direct10;
[[vk::binding(11, 2)]] Texture2D g_direct11;
[[vk::binding(12, 2)]] Texture2D g_direct12;
[[vk::binding(13, 2)]] Texture2D g_direct13;
[[vk::binding(14, 2)]] Texture2D g_direct14;
[[vk::binding(15, 2)]] Texture2D g_direct15;
[[vk::binding(16, 2)]] TextureCube g_directCube0;
[[vk::binding(17, 2)]] TextureCube g_directCube1;
[[vk::binding(18, 2)]] TextureCube g_directCube2;
[[vk::binding(19, 2)]] TextureCube g_directCube3;
[[vk::binding(20, 2)]] Texture2D g_directDepth0; // GatherCmp makes these texture_depth_2d in WGSL
[[vk::binding(21, 2)]] Texture2D g_directDepth1; // GatherCmp makes these texture_depth_2d in WGSL
[[vk::binding(22, 2)]] Texture2D g_directDepth2; // GatherCmp makes these texture_depth_2d in WGSL
[[vk::binding(23, 2)]] Texture2D g_directDepth3; // GatherCmp makes these texture_depth_2d in WGSL
#else
[[vk::binding(0, 0)]] ByteAddressBuffer g_buffers[] : register(t0, space1);
[[vk::binding(1, 0)]] Texture2D g_textures[] : register(t0, space2);
[[vk::binding(2, 0)]] SamplerState g_linearClamp : register(s0);
[[vk::binding(3, 0)]] SamplerState g_linearWrap : register(s1); // model textures (wrap, mips, anisotropy 8)
[[vk::binding(4, 0)]] TextureCube g_cubes[] : register(t0, space3); // sky/reflections (same register space as textures)
#endif

float3 loadFloat3(ByteAddressBuffer b, uint offset) { return asfloat(b.Load3(offset)); }
float4 loadFloat4(ByteAddressBuffer b, uint offset) { return asfloat(b.Load4(offset)); }

#if MKRHI_WEBGPU
// Graphics shaders only read the upload ring, so every buffer index is the ring
ByteAddressBuffer mkBuffer(uint index) { return g_ring; }

uint mkDirectSlot(uint index) { return (index >> 11) & 0xFFu; }
// Runs `body` with `t` bound to the direct slot of `index` (each case is a static choice, which SPIR-V legalization needs)
#define MK_DIRECT_2D(index, body) \
    switch (mkDirectSlot(index)) { \
    case 0: { Texture2D t = g_direct0; body; } \
    case 1: { Texture2D t = g_direct1; body; } \
    case 2: { Texture2D t = g_direct2; body; } \
    case 3: { Texture2D t = g_direct3; body; } \
    case 4: { Texture2D t = g_direct4; body; } \
    case 5: { Texture2D t = g_direct5; body; } \
    case 6: { Texture2D t = g_direct6; body; } \
    case 7: { Texture2D t = g_direct7; body; } \
    case 8: { Texture2D t = g_direct8; body; } \
    case 9: { Texture2D t = g_direct9; body; } \
    case 10: { Texture2D t = g_direct10; body; } \
    case 11: { Texture2D t = g_direct11; body; } \
    case 12: { Texture2D t = g_direct12; body; } \
    case 13: { Texture2D t = g_direct13; body; } \
    case 14: { Texture2D t = g_direct14; body; } \
    default: { Texture2D t = g_direct15; body; } }

// Derivatives are taken before the switch: WGSL allows implicit-derivative sampling only in uniform control flow
float4 mkSample2D(uint index, SamplerState s, float2 uv)
{
    float2 dx = ddx(uv), dy = ddy(uv);
    MK_DIRECT_2D(index, return t.SampleGrad(s, uv, dx, dy));
}
float4 mkSampleLevel2D(uint index, SamplerState s, float2 uv, float lod) { MK_DIRECT_2D(index, return t.SampleLevel(s, uv, lod)); }
float4 mkGatherRed2D(uint index, SamplerState s, float2 uv) { MK_DIRECT_2D(index, return t.GatherRed(s, uv)); }
float4 mkLoad2D(uint index, int2 texel, int mip) { MK_DIRECT_2D(index, return t.Load(int3(texel, mip))); }
uint2 mkTextureSize2D(uint index)
{
    uint w, h;
    MK_DIRECT_2D(index, t.GetDimensions(w, h); return uint2(w, h));
}
// WGSL has no LOD query: the level from the derivatives, as the hardware computes it for isotropic filtering
float mkLod2D(uint index, SamplerState s, float2 uv)
{
    float2 size = float2(mkTextureSize2D(index));
    float2 dx = ddx(uv) * size, dy = ddy(uv) * size;
    return max(0.5 * log2(max(dot(dx, dx), dot(dy, dy))), 0.0);
}
float4 mkSampleCubeLevel(uint index, SamplerState s, float3 dir, float lod)
{
    switch (mkDirectSlot(index)) {
    case 0: return g_directCube0.SampleLevel(s, dir, lod);
    case 1: return g_directCube1.SampleLevel(s, dir, lod);
    case 2: return g_directCube2.SampleLevel(s, dir, lod);
    default: return g_directCube3.SampleLevel(s, dir, lod);
    }
}
// Shadow map PCF taps: per texel of the 2×2 footprint at uv, 1 if receiver >= the stored depth (reversed-Z: lit), 0 if not.
// Gather order (0,1) (1,1) (1,0) (0,0). WebGPU: the comparison happens in the sampler (depth textures can't use filtering samplers)
float4 mkShadowGather(uint index, float2 uv, float receiver)
{
    switch (mkDirectSlot(index)) {
    case 0: return g_directDepth0.GatherCmpRed(g_shadowCompare, uv, receiver);
    case 1: return g_directDepth1.GatherCmpRed(g_shadowCompare, uv, receiver);
    case 2: return g_directDepth2.GatherCmpRed(g_shadowCompare, uv, receiver);
    default: return g_directDepth3.GatherCmpRed(g_shadowCompare, uv, receiver);
    }
}
#else
// DX12/Vulkan: the plain array access (indices may be non-uniform: material textures differ per pixel)
ByteAddressBuffer mkBuffer(uint index) { return g_buffers[index]; }
float4 mkSample2D(uint index, SamplerState s, float2 uv) { return g_textures[NonUniformResourceIndex(index)].Sample(s, uv); }
float4 mkSampleLevel2D(uint index, SamplerState s, float2 uv, float lod) { return g_textures[NonUniformResourceIndex(index)].SampleLevel(s, uv, lod); }
float4 mkGatherRed2D(uint index, SamplerState s, float2 uv) { return g_textures[NonUniformResourceIndex(index)].GatherRed(s, uv); }
float4 mkLoad2D(uint index, int2 texel, int mip) { return g_textures[NonUniformResourceIndex(index)].Load(int3(texel, mip)); }
// Size of mip 0 of the texture itself, in texels
uint2 mkTextureSize2D(uint index)
{
    uint w, h;
    g_textures[NonUniformResourceIndex(index)].GetDimensions(w, h);
    return uint2(w, h);
}
// Mip level the hardware would pick for uv (clamped to the texture's mips)
float mkLod2D(uint index, SamplerState s, float2 uv) { return g_textures[NonUniformResourceIndex(index)].CalculateLevelOfDetail(s, uv); }
float4 mkSampleCubeLevel(uint index, SamplerState s, float3 dir, float lod) { return g_cubes[NonUniformResourceIndex(index)].SampleLevel(s, dir, lod); }
// Shadow map PCF taps: 1 where receiver >= the stored depth (reversed-Z: lit). Gather order (0,1) (1,1) (1,0) (0,0)
float4 mkShadowGather(uint index, float2 uv, float receiver) { return step(mkGatherRed2D(index, g_linearClamp, uv), receiver.xxxx); }
#endif
