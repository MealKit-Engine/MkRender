// RHI binding layout (mealkit/rhi/Rhi.h, docs/DECISIONS.md 2026-10-19 "RHI binding layout"). Shared by all shaders.
// DX12: root constants b0 + three descriptor tables (space1 buffers, space2 textures, space3 cubes: same heap slots as textures) + static samplers s0/s1
// Vulkan: push constants + set 0 binding 0 (buffer array), 1 (texture array), 2/3 (static samplers), 4 (cube array)
// Structs are read field by field with Load (avoids differences in struct layout rules between the two APIs).

[[vk::binding(0, 0)]] ByteAddressBuffer g_buffers[] : register(t0, space1);
[[vk::binding(1, 0)]] Texture2D g_textures[] : register(t0, space2);
[[vk::binding(2, 0)]] SamplerState g_linearClamp : register(s0);
[[vk::binding(3, 0)]] SamplerState g_linearWrap : register(s1); // model textures (wrap, mips, anisotropy 8)
[[vk::binding(4, 0)]] TextureCube g_cubes[] : register(t0, space3); // sky/reflections (same register space as textures)

float3 loadFloat3(ByteAddressBuffer b, uint offset) { return asfloat(b.Load3(offset)); }
float4 loadFloat4(ByteAddressBuffer b, uint offset) { return asfloat(b.Load4(offset)); }
