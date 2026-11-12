// Particles (render::Particle, spawned by feedback::Player from effects in the feedback table). After opaque 3D, depth test only, no depth write.
// One instance = a round quad facing the camera (6 vertices via SV_VertexID, no vertex buffer). Output is premultiplied alpha:
// normal particles are (color × alpha, alpha), additive ones (color × alpha, 0), so one pipeline draws both blends.

#include "Bindless.hlsli"

struct Particle // 32 bytes (render::Particle)
{
    float3 position;
    float size;     // diameter (meters)
    uint color;     // sRGB RGBA8, straight alpha
    float rotation; // radians
    uint flags;     // 1 = additive
    float softness; // edge blur 0..1
};

struct PushConstants // same layout as Renderer.cpp ForwardPush
{
    uint ring;
    uint frameOffset;    // FrameConstants (Forward.hlsl): viewProj 0, cameraPos 64, fogColor 160
    uint particleOffset; // Particle array offset
    uint3 pad;
    uint2 pad2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0);

Particle loadParticle(uint index)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint o = pc.particleOffset + index * 32;
    Particle p;
    p.position = loadFloat3(b, o);
    p.size = asfloat(b.Load(o + 12));
    p.color = b.Load(o + 16);
    p.rotation = asfloat(b.Load(o + 20));
    p.flags = b.Load(o + 24);
    p.softness = asfloat(b.Load(o + 28));
    return p;
}

struct VSOutput
{
    float4 position : SV_Position;
    float2 local : LOCAL; // -1..1 (inside the quad)
    nointerpolation float4 color : COLOR; // linear rgb, alpha
    nointerpolation float2 style : STYLE; // x = additive, y = blur
    float fog : FOG;
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint f = pc.frameOffset;
    float4x4 viewProj = float4x4(loadFloat4(b, f), loadFloat4(b, f + 16), loadFloat4(b, f + 32), loadFloat4(b, f + 48));
    float3 cameraPos = loadFloat4(b, f + 64).xyz;
    float fogDensity = loadFloat4(b, f + 160).w;

    Particle p = loadParticle(instanceId);
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(1, 1), float2(-1, 1) };
    float2 c = corners[vertexId];
    float s = sin(p.rotation), co = cos(p.rotation);
    float2 r = float2(c.x * co - c.y * s, c.x * s + c.y * co);

    // Camera-facing plane: right/up axes (relative to the z axis when looking straight up/down)
    float3 toCamera = normalize(cameraPos - p.position);
    float3 up = abs(toCamera.y) > 0.99 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 right = normalize(cross(up, toCamera));
    up = cross(toCamera, right);
    float3 world = p.position + (right * r.x + up * r.y) * (p.size * 0.5);

    VSOutput o;
    o.position = mul(float4(world, 1.0), viewProj);
    o.local = c;
    float3 srgb = float3(p.color & 0xFF, (p.color >> 8) & 0xFF, (p.color >> 16) & 0xFF) / 255.0;
    o.color = float4(srgb * srgb, (p.color >> 24) / 255.0); // sRGB -> linear approximation (same as Forward.hlsl)
    o.style = float2(p.flags & 1, saturate(p.softness));
    o.fog = exp(-fogDensity * length(cameraPos - p.position)); // 1 = no fog
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float d = length(input.local);
    if (d >= 1.0) discard;
    float a = input.color.a * (1.0 - smoothstep(1.0 - max(input.style.y, 0.02), 1.0, d)) * input.fog;
    float3 rgb = input.color.rgb * a;
    return float4(rgb, input.style.x > 0.5 ? 0.0 : a);
}
