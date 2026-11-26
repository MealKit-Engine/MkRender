// RHI binding layout (mealkit/rhi/Rhi.h, docs/DECISIONS.md 2026-10-19 "RHI binding layout"). Shared by all shaders.
// DX12: root constants b0 + three descriptor tables (space1 buffers, space2 textures, space3 cubes: same heap slots as textures) + static samplers s0/s1
// Vulkan: push constants + set 0 binding 0 (buffer array), 1 (texture array), 2/3 (static samplers), 4 (cube array)
// Structs are read field by field with Load (avoids differences in struct layout rules between the two APIs).
//
// Shaders reach g_buffers[] / g_textures[] / g_cubes[] only through the mk* functions below, so a backend without descriptor
// arrays can replace their bodies. MKRHI_WEBGPU (WebGPU, work in progress): no resource arrays, see the block below.

#if MKRHI_WEBGPU
// WebGPU: SPIR-V for Tint (SPIR-V 1.3, no SPV_EXT_descriptor_indexing). Group 0: upload ring + samplers.
// Group 0 also holds the storage buffers for reading (bindings 5..7, mkBufferLoad: index 1 + slot; 0 = the ring).
// Group 1: texture array pools, one per format and size class (pool = format * 2 + class, class 0 = 512, 1 = 2048; mkrhi/webgpu/Wgpu.h
// kPools), then the Rgb9e5 cube array pools 10 (512) and 11 (2048). Index: pool bits 23..19, layer 18..11, mip bias 10..8 (the pool
// mip the texture's mip 0 sits in), last mip 7..0.
// Group 2: direct textures (pool 31, bits 18..11 = direct id): the few the host named with CommandList::useTextures for these draws,
// two 2D (bindings 0, 1), a cube (2) and a depth texture (3, shadow maps read with a comparison sampler), and their direct ids
// (binding 4, g_directIds: 2D 0, 2D 1, cube, depth). 12 pool + 4 bindings = the 16 sampled textures browsers allow per stage.
[[vk::binding(0, 1)]] Texture2DArray g_pool0;
[[vk::binding(1, 1)]] Texture2DArray g_pool1;
[[vk::binding(2, 1)]] Texture2DArray g_pool2;
[[vk::binding(3, 1)]] Texture2DArray g_pool3;
[[vk::binding(4, 1)]] Texture2DArray g_pool4;
[[vk::binding(5, 1)]] Texture2DArray g_pool5;
[[vk::binding(6, 1)]] Texture2DArray g_pool6;
[[vk::binding(7, 1)]] Texture2DArray g_pool7;
[[vk::binding(8, 1)]] Texture2DArray g_pool8;
[[vk::binding(9, 1)]] Texture2DArray g_pool9;
[[vk::binding(10, 1)]] TextureCubeArray g_cubePool0;
[[vk::binding(11, 1)]] TextureCubeArray g_cubePool1;
[[vk::binding(5, 0)]] ByteAddressBuffer g_storage0;
[[vk::binding(6, 0)]] ByteAddressBuffer g_storage1;
[[vk::binding(7, 0)]] ByteAddressBuffer g_storage2;
[[vk::binding(0, 0)]] ByteAddressBuffer g_ring;
[[vk::binding(2, 0)]] SamplerState g_linearClamp;
[[vk::binding(3, 0)]] SamplerState g_linearWrap;
[[vk::binding(4, 0)]] SamplerComparisonState g_shadowCompare; // GreaterEqual: 1 where the reference is at or in front of the texel
[[vk::binding(0, 2)]] Texture2D g_direct0;
[[vk::binding(1, 2)]] Texture2D g_direct1;
[[vk::binding(2, 2)]] TextureCube g_directCube;
[[vk::binding(3, 2)]] Texture2D g_directDepth; // GatherCmp makes this texture_depth_2d in WGSL
[[vk::binding(4, 2)]] cbuffer MkDirectIds { uint4 g_directIds; };
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
// mkBuffer is for the upload ring (pc.ring: a uniform choice WGSL can't make between buffers): every index is the ring.
// Other buffers (storage buffers through bufferIndex) are read with mkBufferLoad / mkBufferLoad4
ByteAddressBuffer mkBuffer(uint index) { return g_ring; }
uint mkBufferLoad(uint index, uint offset)
{
    switch (index) {
    case 0: return g_ring.Load(offset);
    case 1: return g_storage0.Load(offset);
    case 2: return g_storage1.Load(offset);
    default: return g_storage2.Load(offset);
    }
}
uint4 mkBufferLoad4(uint index, uint offset)
{
    switch (index) {
    case 0: return g_ring.Load4(offset);
    case 1: return g_storage0.Load4(offset);
    case 2: return g_storage1.Load4(offset);
    default: return g_storage2.Load4(offset);
    }
}

uint mkPool(uint index) { return (index >> 19) & 31u; }
bool mkIsDirect(uint index) { return mkPool(index) == 31u; }
uint mkDirectId(uint index) { return (index >> 11) & 0xFFu; }
float mkPoolLayer(uint index) { return float((index >> 11) & 0xFFu); }
uint mkMipBias(uint index) { return (index >> 8) & 7u; }
uint mkLastMip(uint index) { return index & 0xFFu; }
// Size of the pooled texture itself (its mip 0 = the pool's mip bias)
uint2 mkPoolSize(uint index) { return ((mkPool(index) & 1u) ? 2048u : 512u).xx >> mkMipBias(index); }
// Runs `body` with `t` bound to the pool array of `index`
#define MK_POOL_2D(index, body) \
    switch (mkPool(index)) { \
    case 0: { Texture2DArray t = g_pool0; body; } \
    case 1: { Texture2DArray t = g_pool1; body; } \
    case 2: { Texture2DArray t = g_pool2; body; } \
    case 3: { Texture2DArray t = g_pool3; body; } \
    case 4: { Texture2DArray t = g_pool4; body; } \
    case 5: { Texture2DArray t = g_pool5; body; } \
    case 6: { Texture2DArray t = g_pool6; body; } \
    case 7: { Texture2DArray t = g_pool7; body; } \
    case 8: { Texture2DArray t = g_pool8; body; } \
    default: { Texture2DArray t = g_pool9; body; } }
// Runs `body` with `t` bound to the direct 2D texture of `index` (each branch is a static choice, which SPIR-V legalization needs).
// Not one of the two bound (useTextures): reads g_direct1
#define MK_DIRECT_2D(index, body) \
    if (mkDirectId(index) == g_directIds.x) { Texture2D t = g_direct0; body; } \
    else { Texture2D t = g_direct1; body; }

uint2 mkTextureSize2D(uint index)
{
    if (!mkIsDirect(index)) return mkPoolSize(index);
    uint w, h;
    MK_DIRECT_2D(index, t.GetDimensions(w, h); return uint2(w, h));
}
// Level of detail from the derivatives in the texture's own texels (isotropic, unclamped)
float mkLodFromGrad(uint index, float2 dx, float2 dy)
{
    float2 size = float2(mkTextureSize2D(index));
    dx *= size;
    dy *= size;
    return 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));
}
// Derivatives are taken before the switch: WGSL allows implicit-derivative sampling only in uniform control flow.
// Pooled: the pool mips below the bias are empty and those past the texture's last mip belong to nothing, so magnification reads
// the bias mip and minification past the last mip reads that last mip (what the hardware does for a texture of its own). In between
// SampleGrad picks the mip (bias included: the pool's texels are 2^bias times smaller in UV), anisotropy intact
float4 mkSample2D(uint index, SamplerState s, float2 uv)
{
    float2 dx = ddx(uv), dy = ddy(uv);
    if (mkIsDirect(index)) { MK_DIRECT_2D(index, return t.SampleGrad(s, uv, dx, dy)); }
    float lod = mkLodFromGrad(index, dx, dy);
    float3 c = float3(uv, mkPoolLayer(index));
    float bias = float(mkMipBias(index)), last = float(mkLastMip(index));
    if (lod <= 0.0) { MK_POOL_2D(index, return t.SampleLevel(s, c, bias)); }
    if (lod >= last) { MK_POOL_2D(index, return t.SampleLevel(s, c, bias + last)); }
    MK_POOL_2D(index, return t.SampleGrad(s, c, dx, dy));
}
float4 mkSampleLevel2D(uint index, SamplerState s, float2 uv, float lod)
{
    if (mkIsDirect(index)) { MK_DIRECT_2D(index, return t.SampleLevel(s, uv, lod)); }
    float level = float(mkMipBias(index)) + clamp(lod, 0.0, float(mkLastMip(index)));
    MK_POOL_2D(index, return t.SampleLevel(s, float3(uv, mkPoolLayer(index)), level));
}
float4 mkLoad2D(uint index, int2 texel, int mip)
{
    if (mkIsDirect(index)) { MK_DIRECT_2D(index, return t.Load(int3(texel, mip))); }
    MK_POOL_2D(index, return t.Load(int4(texel, int(mkPoolLayer(index)), mip + int(mkMipBias(index)))));
}
// Pooled: from four loads of the texture's mip 0 (a gather reads the array's mip 0), edges clamped
float4 mkGatherRed2D(uint index, SamplerState s, float2 uv)
{
    if (mkIsDirect(index)) { MK_DIRECT_2D(index, return t.GatherRed(s, uv)); }
    int2 size = int2(mkPoolSize(index));
    int2 p = int2(floor(uv * float2(size) - 0.5));
    int2 a = clamp(p, 0, size - 1), b = clamp(p + 1, 0, size - 1);
    return float4(mkLoad2D(index, int2(a.x, b.y), 0).r, mkLoad2D(index, b, 0).r, mkLoad2D(index, int2(b.x, a.y), 0).r, mkLoad2D(index, a, 0).r);
}
// WGSL has no LOD query: the level from the derivatives, as the hardware computes it for isotropic filtering (pooled: clamped to
// the texture's mips like CalculateLevelOfDetail)
float mkLod2D(uint index, SamplerState s, float2 uv)
{
    float lod = max(mkLodFromGrad(index, ddx(uv), ddy(uv)), 0.0);
    return mkIsDirect(index) ? lod : min(lod, float(mkLastMip(index)));
}
float4 mkSampleCubeLevel(uint index, SamplerState s, float3 dir, float lod)
{
    if (!mkIsDirect(index)) {
        float4 c = float4(dir, mkPoolLayer(index));
        float level = float(mkMipBias(index)) + clamp(lod, 0.0, float(mkLastMip(index)));
        if (mkPool(index) == 10u) return g_cubePool0.SampleLevel(s, c, level);
        return g_cubePool1.SampleLevel(s, c, level);
    }
    return g_directCube.SampleLevel(s, dir, lod);
}
// Shadow map PCF taps: per texel of the 2×2 footprint at uv, 1 if receiver >= the stored depth (reversed-Z: lit), 0 if not.
// Gather order (0,1) (1,1) (1,0) (0,0). WebGPU: the comparison happens in the sampler (depth textures can't use filtering samplers)
float4 mkShadowGather(uint index, float2 uv, float receiver)
{
    return g_directDepth.GatherCmpRed(g_shadowCompare, uv, receiver);
}
#else
// DX12/Vulkan: the plain array access (indices may be non-uniform: material textures differ per pixel)
ByteAddressBuffer mkBuffer(uint index) { return g_buffers[index]; }
uint mkBufferLoad(uint index, uint offset) { return g_buffers[index].Load(offset); }
uint4 mkBufferLoad4(uint index, uint offset) { return g_buffers[index].Load4(offset); }
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
