// Shaders of tests/ComputeTest.cpp: fill a storage buffer and a storage texture, read-modify-write the buffer after a UAV barrier,
// then read the texture through mkLoad2D and the buffer through mkBufferLoad (ShaderRead state) into the buffer. Checks bindings, barriers and state changes on both backends.
#include "Compute.hlsli"

struct ComputePush
{
    uint buffer;     // g_rwBuffers index
    uint texture;    // g_rwTextures index (CSFill)
    uint textureSrv; // g_textures index (CSCopy)
    uint count;      // number of uints the buffer part [0, count) holds; CSCopy writes [count, count + 64)
    uint bufferSrv;  // g_buffers index of the same buffer (CSRead, after it moved to ShaderRead)
    uint buffer2;    // g_rwBuffers index of the second buffer (CSRead writes [0, count) there, CSSample the 8 + 12 after that)
    uint mipTexture; // g_textures index of a 64×64 texture with 7 mips, one solid color each (CSSample)
    uint cube;       // g_cubes index of a 16×16 cube with 2 mips, one solid color per face and mip (CSSample)
};
[[vk::push_constant]] ConstantBuffer<ComputePush> g_push : register(b0);

[numthreads(64, 1, 1)]
void CSFill(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_push.count)
        mkRwStore(g_push.buffer, id.x * 4, id.x * 3 + 1);
    if (id.x < 64)
        mkRwStore2D(g_push.texture, uint2(id.x % 8, id.x / 8), float4((id.x % 8) * 32 / 255.0, (id.x / 8) * 32 / 255.0, 0.0, 1.0));
}

[numthreads(64, 1, 1)]
void CSAdd(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_push.count)
        mkRwStore(g_push.buffer, id.x * 4, mkRwLoad(g_push.buffer, id.x * 4) + 1000);
}

[numthreads(64, 1, 1)]
void CSCopy(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 64)
        return;
    const float4 c = mkLoad2D(g_push.textureSrv, int2(id.x % 8, id.x / 8), 0);
    const uint4 b = uint4(round(c * 255.0));
    mkRwStore(g_push.buffer, (g_push.count + id.x) * 4, b.x | (b.y << 8) | (b.z << 16) | (b.w << 24));
}

// Reads the first buffer through g_buffers[] (ShaderRead) into the second one, doubled
[numthreads(64, 1, 1)]
void CSRead(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_push.count)
        mkRwStore(g_push.buffer2, id.x * 4, mkBufferLoad(g_push.bufferSrv, id.x * 4) * 2);
}

uint packUnorm(float4 c)
{
    const uint4 b = uint4(round(saturate(c) * 255.0));
    return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}

// Sampling through the helpers: the 2D texture at lods 0..7 (7 = past its last mip), the cube's six faces at lods 0 and 1
// (WebGPU: both sit in pools at a mip bias, Bindless.hlsli)
[numthreads(32, 1, 1)]
void CSSample(uint3 id : SV_DispatchThreadID)
{
    const uint base = (g_push.count + id.x) * 4;
    if (id.x < 8) {
        mkRwStore(g_push.buffer2, base, packUnorm(mkSampleLevel2D(g_push.mipTexture, g_linearClamp, float2(0.5, 0.5), float(id.x))));
    } else if (id.x < 20) {
        const uint face = (id.x - 8) % 6, mip = (id.x - 8) / 6;
        const float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
        const float3 c = mkSampleCubeLevel(g_push.cube, g_linearClamp, dirs[face], float(mip)).rgb;
        mkRwStore(g_push.buffer2, base, packUnorm(float4(c * 0.5, 1.0))); // cube values go up to 2
    }
}
