// Sprites (render::Sprite, DrawList::addSprite): textured quads in the world, after opaque 3D and before particles, depth
// tested without writes, already sorted back to front. One instance = one quad (6 vertices via SV_VertexID, no vertex
// buffer). Textures hold premultiplied alpha in sRGB (render/Sprites.h), so the output is premultiplied too.

#include "Bindless.hlsli"

struct Sprite // 80 bytes (render::Sprite)
{
    float3 position;
    uint texture;
    float4 rotation; // quaternion x y z w
    float2 size;
    float2 pivot;
    float4 uv; // u0 v0 u1 v1
    uint color;
    uint flags; // 1 flip x, 2 flip y, 4 point sampling
    int layer;
    float pad;
};

struct PushConstants // same layout as Renderer.cpp ForwardPush
{
    uint ring;
    uint frameOffset;  // FrameConstants (Forward.hlsl): viewProj 0
    uint spriteOffset; // Sprite array offset
    uint3 pad;
    uint2 pad2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0);

Sprite loadSprite(uint index)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint o = pc.spriteOffset + index * 80;
    Sprite s;
    s.position = loadFloat3(b, o);
    s.texture = b.Load(o + 12);
    s.rotation = loadFloat4(b, o + 16);
    s.size = asfloat(b.Load2(o + 32));
    s.pivot = asfloat(b.Load2(o + 40));
    s.uv = loadFloat4(b, o + 48);
    s.color = b.Load(o + 64);
    s.flags = b.Load(o + 68);
    s.layer = asint(b.Load(o + 72));
    s.pad = 0;
    return s;
}

float3 rotateByQuat(float4 q, float3 v)
{
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float4 color : COLOR; // linear rgb, alpha
    nointerpolation uint2 texFlags : TEXFLAGS; // x = texture, y = flags
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint f = pc.frameOffset;
    float4x4 viewProj = float4x4(loadFloat4(b, f), loadFloat4(b, f + 16), loadFloat4(b, f + 32), loadFloat4(b, f + 48));

    Sprite s = loadSprite(instanceId);
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    float2 c = corners[vertexId];
    float3 local = float3((c - s.pivot) * s.size, 0.0);
    float3 world = s.position + rotateByQuat(s.rotation, local);

    float2 t = float2((s.flags & 1) ? 1.0 - c.x : c.x, (s.flags & 2) ? c.y : 1.0 - c.y); // image v runs down
    VSOutput o;
    o.position = mul(float4(world, 1.0), viewProj);
    o.uv = lerp(s.uv.xy, s.uv.zw, t);
    float3 srgb = float3(s.color & 0xFF, (s.color >> 8) & 0xFF, (s.color >> 16) & 0xFF) / 255.0;
    o.color = float4(srgb * srgb, (s.color >> 24) / 255.0); // sRGB -> linear approximation (same as Forward.hlsl)
    o.texFlags = uint2(s.texture, s.flags);
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    Texture2D tex = g_textures[NonUniformResourceIndex(input.texFlags.x)];
    float4 texel;
    if (input.texFlags.y & 4)
    {
        uint w, h;
        tex.GetDimensions(w, h);
        int2 p = clamp(int2(floor(input.uv * float2(w, h))), int2(0, 0), int2(w, h) - 1);
        texel = tex.Load(int3(p, 0));
    }
    else
        texel = tex.Sample(g_linearClamp, input.uv);
    float a = texel.a * input.color.a;
    if (a <= 0.002) discard;
    return float4(texel.rgb * input.color.rgb * input.color.a, a);
}
