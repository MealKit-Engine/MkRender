// Compute bindings (G1, docs/DECISIONS.md 2026-11-25 "RHI compute"): the read/write arrays next to Bindless.hlsli's read-only ones.
// DX12: UAV tables u0 space1 (buffers) and space2 (2D textures). Vulkan: set 0 binding 5 (storage buffers) and 6 (storage images).
// Indices come from Device::storageBufferIndex / storageTextureIndex through push constants, like every other bindless index.
// Shaders reach g_rwBuffers[] / g_rwTextures[] only through the mkRw* functions below (a backend without arrays replaces them).
#include "Bindless.hlsli"

#if MKRHI_WEBGPU
// WebGPU (group 3, compute only; mkrhi/webgpu/Wgpu.h kStorageBuffers / kStoragePerFormat), sized to WebGPU's default limits:
// 3 storage buffers (bindings 0..2) and 4 storage textures, which WGSL types by format: rgba8unorm (3, 4), rgba16float (5),
// r32float (6). Storage texture index 0, 1 = rgba8unorm, 2 = rgba16float, 3 = r32float. Write-only (WebGPU's baseline; read the
// texture through mkLoad2D after a barrier)
[[vk::binding(0, 3)]] RWByteAddressBuffer g_rwBuffer0;
[[vk::binding(1, 3)]] RWByteAddressBuffer g_rwBuffer1;
[[vk::binding(2, 3)]] RWByteAddressBuffer g_rwBuffer2;
[[vk::binding(3, 3)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_rwRgba8_0;
[[vk::binding(4, 3)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> g_rwRgba8_1;
[[vk::binding(5, 3)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> g_rwRgba16f;
[[vk::binding(6, 3)]] [[vk::image_format("r32f")]] RWTexture2D<float4> g_rwR32f;

uint mkRwLoad(uint index, uint offset)
{
    switch (index) {
    case 0: return g_rwBuffer0.Load(offset);
    case 1: return g_rwBuffer1.Load(offset);
    default: return g_rwBuffer2.Load(offset);
    }
}
void mkRwStore(uint index, uint offset, uint value)
{
    switch (index) {
    case 0: g_rwBuffer0.Store(offset, value); break;
    case 1: g_rwBuffer1.Store(offset, value); break;
    default: g_rwBuffer2.Store(offset, value); break;
    }
}
void mkRwStore2D(uint index, uint2 texel, float4 value)
{
    switch (index) {
    case 0: g_rwRgba8_0[texel] = value; break;
    case 1: g_rwRgba8_1[texel] = value; break;
    case 2: g_rwRgba16f[texel] = value; break;
    default: g_rwR32f[texel] = value; break;
    }
}
#else
[[vk::binding(5, 0)]] RWByteAddressBuffer g_rwBuffers[] : register(u0, space1);
[[vk::binding(6, 0)]] RWTexture2D<float4> g_rwTextures[] : register(u0, space2);

uint mkRwLoad(uint index, uint offset) { return g_rwBuffers[index].Load(offset); }
void mkRwStore(uint index, uint offset, uint value) { g_rwBuffers[index].Store(offset, value); }
void mkRwStore2D(uint index, uint2 texel, float4 value) { g_rwTextures[index][texel] = value; }
#endif
