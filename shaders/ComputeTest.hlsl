// Shaders of tests/ComputeTest.cpp: fill a storage buffer and a storage texture, read-modify-write the buffer after a UAV barrier,
// then read the texture through g_textures[] (ShaderRead state) into the buffer. Checks bindings, barriers and state changes on both backends.
#include "Compute.hlsli"

struct ComputePush
{
    uint buffer;     // g_rwBuffers index
    uint texture;    // g_rwTextures index (CSFill)
    uint textureSrv; // g_textures index (CSCopy)
    uint count;      // number of uints the buffer part [0, count) holds; CSCopy writes [count, count + 64)
};
[[vk::push_constant]] ConstantBuffer<ComputePush> g_push : register(b0);

[numthreads(64, 1, 1)]
void CSFill(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_push.count)
        g_rwBuffers[g_push.buffer].Store(id.x * 4, id.x * 3 + 1);
    if (id.x < 64)
        g_rwTextures[g_push.texture][uint2(id.x % 8, id.x / 8)] = float4((id.x % 8) * 32 / 255.0, (id.x / 8) * 32 / 255.0, 0.0, 1.0);
}

[numthreads(64, 1, 1)]
void CSAdd(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_push.count)
        g_rwBuffers[g_push.buffer].Store(id.x * 4, g_rwBuffers[g_push.buffer].Load(id.x * 4) + 1000);
}

[numthreads(64, 1, 1)]
void CSCopy(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 64)
        return;
    const float4 c = g_textures[g_push.textureSrv].Load(int3(id.x % 8, id.x / 8, 0));
    const uint4 b = uint4(round(c * 255.0));
    g_rwBuffers[g_push.buffer].Store((g_push.count + id.x) * 4, b.x | (b.y << 8) | (b.z << 16) | (b.w << 24));
}
