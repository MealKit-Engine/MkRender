// Compute bindings (G1, docs/DECISIONS.md 2026-11-25 "RHI compute"): the read/write arrays next to Bindless.hlsli's read-only ones.
// DX12: UAV tables u0 space1 (buffers) and space2 (2D textures). Vulkan: set 0 binding 5 (storage buffers) and 6 (storage images).
// Indices come from Device::storageBufferIndex / storageTextureIndex through push constants, like every other bindless index.
#include "Bindless.hlsli"

[[vk::binding(5, 0)]] RWByteAddressBuffer g_rwBuffers[] : register(u0, space1);
[[vk::binding(6, 0)]] RWTexture2D<float4> g_rwTextures[] : register(u0, space2);
