// MealKit forward shading.
// Reads instance data (position/rotation/scale/color) from the upload ring and transforms in the vertex shader.
// The CPU builds no matrices and writes only 48-byte instances. Materials are glTF PBR metallic-roughness (vertex material index →
// material table in the upload ring → bindless textures). Lighting: sun (1~2 shadow cascades, drawn first by VSShadow/PSShadow) + point lights (up to 8 per object chosen by the CPU, no shadows)
// + ambient (SH of the sky HDRI, or a hemisphere gradient without one) + exponential fog.
// With a sky HDRI, VSSky/PSSky draw the skybox before 3D, and reflections read the baked cube (mip = roughness).
// With reflection probes, the vertex shader picks the one probe whose influence box contains the instance position and whose center is closest, and reads that cube instead of the sky reflection.
// Style presets (docs/CODING_MODEL.md "Shader code"): the lighting model is three build-time variants via LIGHTING_MODEL (PSMain),
// material features (outline/wind/rim/dissolve) are chosen in the shader via material table bits. Outline is the VSOutline/PSOutline pass
//   0 PBR: wrapped diffuse (squared half Lambert) + GGX specular   1 Toon: ramp texture diffuse + hard-edged highlight   2 Unlit: color as is

#include "Bindless.hlsli"

#ifndef LIGHTING_MODEL
#define LIGHTING_MODEL 0
#endif

static const uint kMaterialOutline = 1u; // render::kMaterial* (Renderer.h)
static const uint kMaterialWind = 2u;
static const uint kMaterialRim = 4u;
static const uint kMaterialDissolve = 8u;

struct Instance // 48 bytes (render::InstanceData)
{
    float3 position;
    uint color;      // RGBA8 (sRGB), A unused
    float4 rotation; // quaternion
    float3 scale;
    uint flags;      // bit0: checker pattern, bit1: selection highlight, bit2: over budget, bit8~9: LOD (editor view mode),
                     // bit16~23: flash, bit24~31: dissolve progress (render::instanceFlash/instanceDissolve)
};

struct FrameConstants // 672 bytes (Renderer.cpp FrameConstants)
{
    float4x4 viewProj;
    float4 cameraPos;
    float4 sunDir;      // direction toward the sun
    float4 sunColor;
    float4 skyColor;
    float4 groundColor;
    float4 fogColor;    // a = fog density
    float4 screenTint;  // screen flash (rgb linear color, a = strength): flash from the feedback table
    uint4 materials;    // x = material table ring offset, y = count
    float4 wind;        // xy = horizontal direction (world xz), z = strength, w = view time (seconds)
    uint4 style;        // x = toon ramp texture (bindless)
    float4x4 invViewProj; // skybox: screen → world
    uint4 env;          // x = sky cube, y = reflection cube (g_cubes), z = reflection mip count, w = present
    float4 envParams;   // x = intensity, y = cos(rotation), z = sin(rotation)
    float4 sh[9];       // ambient SH (rgb)
    uint4 probes;       // x = reflection probe table ring offset, y = count (32-byte entries: position xyz, cube index | mip count << 24, box half size xyz, 0)
    uint4 lights;       // x = point light table ring offset, y = count (32-byte entries: position xyz, range, color rgb (× intensity), 0)
    float4x4 shadowViewProj[2]; // per cascade world → shadow clip
    float4 shadowForward; // camera forward direction (cascade selection)
    float4 shadowSplits;  // x = cascade 0 end distance, y = last cascade end distance, z/w = world size of a cascade 0/1 texel
    uint4 shadow;         // x = shadow map (bindless), y = cascade count (0 = none), z = size of one cascade
};

static const uint kNoTexture = 0xFFFFFFFFu;
struct Material // 128 bytes (Renderer.cpp MaterialGpu)
{
    float4 baseColor;
    float3 emissive;
    float alphaCutoff;
    float metallic;
    float roughness;
    float normalScale;
    uint features;  // kMaterial* bits
    uint4 textures; // baseColor, normal, metallicRoughness, emissive
    float3 outlineColor;
    float outlineWidth;
    float3 rimColor;
    float rimStrength;
    float3 dissolveColor;
    float dissolveEdge;
    float windStrength;
};

struct PushConstants
{
    uint ring;           // g_buffers[] index of the upload ring
    uint frameOffset;    // FrameConstants offset
    uint instanceOffset; // offset of this draw's Instance array
    uint debugView;      // render::DebugView: 0 none, 1 wireframe, 2 normals, 3 LOD, 4 overdraw, 5 budget, 6 material features
    uint lightOffset;    // ring offset of 8 point light indices per instance (bytes, 0xFF = end)
    uint paletteOffset;  // skinned mesh: ring offset of the joint palette (three rows per joint = 48 bytes)
    uint2 pad;
    float4 boxCenter;    // mesh quantization box: position = boxCenter + snorm * boxExtent (Renderer.cpp GpuVertex)
    float4 boxExtent;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0);

FrameConstants loadFrame()
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint o = pc.frameOffset;
    FrameConstants f;
    // Row-major matrices (a line in CPU memory = a matrix row)
    f.viewProj = float4x4(loadFloat4(b, o), loadFloat4(b, o + 16), loadFloat4(b, o + 32), loadFloat4(b, o + 48));
    f.cameraPos = loadFloat4(b, o + 64);
    f.sunDir = loadFloat4(b, o + 80);
    f.sunColor = loadFloat4(b, o + 96);
    f.skyColor = loadFloat4(b, o + 112);
    f.groundColor = loadFloat4(b, o + 128);
    f.fogColor = loadFloat4(b, o + 144);
    f.screenTint = loadFloat4(b, o + 160);
    f.materials = b.Load4(o + 176);
    f.wind = loadFloat4(b, o + 192);
    f.style = b.Load4(o + 208);
    f.invViewProj = float4x4(loadFloat4(b, o + 224), loadFloat4(b, o + 240), loadFloat4(b, o + 256), loadFloat4(b, o + 272));
    f.env = b.Load4(o + 288);
    f.envParams = loadFloat4(b, o + 304);
    [unroll] for (uint i = 0; i < 9; ++i)
        f.sh[i] = loadFloat4(b, o + 320 + i * 16);
    f.probes = b.Load4(o + 464);
    f.lights = b.Load4(o + 480);
    [unroll] for (uint c = 0; c < 2; ++c)
        f.shadowViewProj[c] = float4x4(loadFloat4(b, o + 496 + c * 64), loadFloat4(b, o + 512 + c * 64), loadFloat4(b, o + 528 + c * 64),
                                       loadFloat4(b, o + 544 + c * 64));
    f.shadowForward = loadFloat4(b, o + 624);
    f.shadowSplits = loadFloat4(b, o + 640);
    f.shadow = b.Load4(o + 656);
    return f;
}

Material loadMaterial(FrameConstants frame, uint index)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint o = frame.materials.x + min(index, frame.materials.y - 1) * 128;
    Material m;
    m.baseColor = loadFloat4(b, o);
    float4 e = loadFloat4(b, o + 16);
    m.emissive = e.xyz;
    m.alphaCutoff = e.w;
    float4 p = loadFloat4(b, o + 32);
    m.metallic = p.x;
    m.roughness = p.y;
    m.normalScale = p.z;
    m.features = asuint(p.w);
    m.textures = b.Load4(o + 48);
    float4 outline = loadFloat4(b, o + 64), rim = loadFloat4(b, o + 80), dissolve = loadFloat4(b, o + 96);
    m.outlineColor = outline.xyz;
    m.outlineWidth = outline.w;
    m.rimColor = rim.xyz;
    m.rimStrength = rim.w;
    m.dissolveColor = dissolve.xyz;
    m.dissolveEdge = dissolve.w;
    m.windStrength = asfloat(b.Load(o + 112));
    return m;
}

Instance loadInstance(uint index)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint o = pc.instanceOffset + index * 48;
    Instance i;
    i.position = loadFloat3(b, o);
    i.color = b.Load(o + 12);
    i.rotation = loadFloat4(b, o + 16);
    i.scale = loadFloat3(b, o + 32);
    i.flags = b.Load(o + 44);
    return i;
}

struct VSInput
{
    float4 position : POSITION; // snorm in the mesh box (pc.boxCenter/boxExtent)
    float2 normal : NORMAL;     // octahedral snorm
    float4 color : COLOR; // vertex color (sRGB, glTF COLOR_0). Alpha is wind strength
    float2 uv : TEXCOORD;
    uint2 material : MATERIAL; // x = material table index
#if SKINNED
    uint4 joints : JOINTS;   // four joint indices (palette slots)
    float4 weights : WEIGHTS; // sum 1
#endif
    uint instanceId : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : WORLDPOS;
    float3 localPos : LOCALPOS; // dissolve pattern (moves with the object)
    float3 normal : NORMAL;
    float3 albedo : COLOR; // instance color × vertex color (material base color is in the pixel shader)
    float2 uv : TEXCOORD;
    nointerpolation uint flags : FLAGS;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint probe : PROBE; // reflection probe cube index | mip count << 24 (0 = sky reflection)
    nointerpolation uint2 lights : LIGHTS; // 8 point light indices (bytes, 0xFF = end)
};

float3 rotateByQuat(float4 q, float3 v)
{
    float3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// Octahedral normal (Renderer.cpp quantize): unfold the lower half
float3 decodeOctahedral(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

float3 unpackAlbedo(uint c)
{
    float3 srgb = float3(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF) / 255.0;
    return srgb * srgb; // sRGB -> linear approximation (gamma 2.0)
}

// The reflection probe whose influence box contains the instance position with the closest center (one probe per object, no blending). 0 if none
uint nearestProbe(FrameConstants frame, float3 position)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint best = 0;
    float bestDistance = 3.4e38;
    for (uint i = 0; i < frame.probes.y; ++i)
    {
        uint o = frame.probes.x + i * 32;
        float3 d = loadFloat3(b, o) - position;
        float distance = dot(d, d);
        if (all(abs(d) <= loadFloat3(b, o + 16)) && distance < bestDistance)
        {
            bestDistance = distance;
            best = b.Load(o + 12) + 1; // distinguish from 0 (cube index 0 can exist)
        }
    }
    return best;
}

// Vertex transform (shared by the main and outline passes). For outline, inflate along the normal; collapse vertices of materials without an outline
VSOutput transformVertex(VSInput input, bool outline)
{
    Instance inst = loadInstance(input.instanceId);
    FrameConstants frame = loadFrame();
    Material mat = loadMaterial(frame, input.material.x);
    float3 local = pc.boxCenter.xyz + input.position.xyz * pc.boxExtent.xyz, localNormal = decodeOctahedral(input.normal);
#if SKINNED
    // Skin: weighted sum of the four joints' palette matrices (CPU asset::skinPalette, MealKit coordinates). Normals use the same matrix (assumes no non-uniform scale)
    {
        ByteAddressBuffer b = g_buffers[pc.ring];
        float3x4 skin = (float3x4)0;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            uint o = pc.paletteOffset + input.joints[k] * 48;
            skin += float3x4(loadFloat4(b, o), loadFloat4(b, o + 16), loadFloat4(b, o + 32)) * input.weights[k];
        }
        local = mul(skin, float4(local, 1.0));
        localNormal = mul((float3x3)skin, localNormal);
    }
#endif
    float3 world = rotateByQuat(inst.rotation, local * inst.scale) + inst.position;
    float3 normal = rotateByQuat(inst.rotation, localNormal / inst.scale);

    if (mat.features & kMaterialWind) // wind: vertex alpha = strength (paint root 0, tip 1). Phase varies by position so things don't sway in unison
    {
        float phase = dot(world.xz, float2(0.37, 0.23));
        float t = frame.wind.w;
        float sway = sin(t * 1.7 + phase) * 0.7 + sin(t * 3.3 + phase * 1.9) * 0.3;
        world.xz += frame.wind.xy * (sway * input.color.a * mat.windStrength * frame.wind.z);
    }
    if (outline)
        world += normalize(normal) * mat.outlineWidth;

    VSOutput o;
    o.position = mul(float4(world, 1.0), frame.viewProj);
    if (outline && !(mat.features & kMaterialOutline))
        o.position = float4(0.0, 0.0, 0.0, 0.0); // the rasterizer discards zero-area triangles
    o.worldPos = world;
    o.localPos = local;
    o.normal = normal;
    o.albedo = unpackAlbedo(inst.color) * (input.color.rgb * input.color.rgb);
    o.uv = input.uv;
    o.flags = inst.flags;
    o.material = input.material.x;
    o.probe = nearestProbe(frame, inst.position);
    o.lights = frame.lights.y != 0 ? g_buffers[pc.ring].Load2(pc.lightOffset + input.instanceId * 8) : uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    return o;
}

VSOutput VSMain(VSInput input) { return transformVertex(input, false); }
VSOutput VSShadow(VSInput input) { return transformVertex(input, false); } // frame constant viewProj = shadow cascade
VSOutput VSOutline(VSInput input) { return transformVertex(input, true); }

float4 sampleTexture(uint index, float2 uv, float4 fallback)
{
    return index == kNoTexture ? fallback : g_textures[NonUniformResourceIndex(index)].Sample(g_linearWrap, uv);
}

// Builds tangent space from screen derivatives without tangents (no tangents in vertices, docs/DECISIONS.md 2026-10-21 "Vertex format")
float3 perturbNormal(float3 n, float3 worldPos, float2 uv, float3 mapNormal)
{
    float3 dp1 = ddx(worldPos), dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    // z was flipped for the left-handed system, so b points opposite to glTF
    return normalize(mapNormal.x * t * invmax - mapNormal.y * b * invmax + mapNormal.z * n);
}

// ---- Sky HDRI environment (baked by asset/Environment.cpp)

// World direction → environment direction (y-axis rotation)
float3 envDirection(FrameConstants f, float3 d)
{
    return float3(f.envParams.y * d.x - f.envParams.z * d.z, d.y, f.envParams.z * d.x + f.envParams.y * d.z);
}

// Real spherical harmonics, orders 0~2 (same as asset/Environment.cpp shBasis). Coefficients include the Lambert convolution, so the result is the ambient color
float3 evalSh(FrameConstants f, float3 n)
{
    float3 c = f.sh[0].rgb * 0.282095;
    c += f.sh[1].rgb * (0.488603 * n.y) + f.sh[2].rgb * (0.488603 * n.z) + f.sh[3].rgb * (0.488603 * n.x);
    c += f.sh[4].rgb * (1.092548 * n.x * n.y) + f.sh[5].rgb * (1.092548 * n.y * n.z);
    c += f.sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + f.sh[7].rgb * (1.092548 * n.x * n.z);
    c += f.sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(c, 0.0);
}

// Ambient: SH of the HDRI, or a sky/ground hemisphere gradient without one
float3 ambientLight(FrameConstants f, float3 n)
{
    if (f.env.w != 0)
        return evalSh(f, envDirection(f, n)) * f.envParams.x;
    return lerp(f.groundColor.rgb, f.skyColor.rgb, n.y * 0.5 + 0.5);
}

// Reflection cube (mip = roughness): the reflection probe's cube if any (world direction, scene baked brightness as is), otherwise the sky HDRI
bool hasReflection(FrameConstants f, uint probe) { return probe != 0 || f.env.w != 0; }

float3 reflection(FrameConstants f, float3 r, float roughness, uint probe)
{
    if (probe != 0)
    {
        uint cube = (probe - 1) & 0xFFFFFFu;
        float probeMips = float(max((probe - 1) >> 24, 1u) - 1);
        return g_cubes[NonUniformResourceIndex(cube)].SampleLevel(g_linearClamp, r, roughness * probeMips).rgb;
    }
    float mips = float(max(f.env.z, 1u) - 1);
    return g_cubes[NonUniformResourceIndex(f.env.y)].SampleLevel(g_linearClamp, envDirection(f, r), roughness * mips).rgb * f.envParams.x;
}

// Point lights (no shadows): distance attenuation is inverse square times a window function (0 at range). Diffuse is Lambert, PBR also GGX specular
// diffuseColor = albedo × (1 − metallic). Not called by the unlit model
float3 pointLights(FrameConstants f, uint2 lights, float3 worldPos, float3 n, float3 v, float3 diffuseColor, float3 f0, float roughness)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    float3 color = 0.0;
    [loop] for (uint i = 0; i < 8; ++i)
    {
        uint index = ((i < 4 ? lights.x : lights.y) >> ((i & 3u) * 8)) & 0xFFu;
        if (index == 0xFFu || index >= f.lights.y)
            break;
        uint o = f.lights.x + index * 32;
        float4 pr = loadFloat4(b, o);
        float3 radiance = loadFloat3(b, o + 16);
        float3 toLight = pr.xyz - worldPos;
        float d2 = dot(toLight, toLight);
        float window = saturate(1.0 - (d2 * d2) / (pr.w * pr.w * pr.w * pr.w));
        float atten = window * window / (d2 + 1.0);
        float3 l = toLight * rsqrt(max(d2, 1e-8));
        float ndl = saturate(dot(n, l));
#if LIGHTING_MODEL == 1
        // Toon: diffuse via the ramp (same color steps as the sun), no highlight
        float3 ramp = g_textures[NonUniformResourceIndex(f.style.x)].SampleLevel(g_linearClamp, float2(saturate(dot(n, l) * 0.5 + 0.5), 0.5), 0).rgb;
        color += diffuseColor * ramp * ramp * radiance * atten;
#else
        float3 h = normalize(l + v);
        float ndh = saturate(dot(n, h)), ndv = max(dot(n, v), 1e-4), vdh = saturate(dot(v, h));
        float a = roughness * roughness, a2 = a * a;
        float d = a2 / (3.14159265 * pow(ndh * ndh * (a2 - 1.0) + 1.0, 2.0));
        float k = a * 0.5;
        float vis = 1.0 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k) * 4.0);
        float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
        color += (diffuseColor + d * vis * fresnel) * radiance * (atten * ndl);
#endif
    }
    return color;
}

// Sun shadow (0 = shade, 1 = lit). Cascade chosen by distance along camera forward; pushed 1.5 texels along the normal to prevent self-shadow acne.
// Filtered read: four Gathers (4×4 texels) compared with bilinear weights and averaged (reversed-Z: closer to the light = larger depth)
float sunShadow(FrameConstants f, float3 worldPos, float3 n)
{
    if (f.shadow.y == 0)
        return 1.0;
    float viewDepth = dot(worldPos - f.cameraPos.xyz, f.shadowForward.xyz);
    if (viewDepth >= f.shadowSplits.y)
        return 1.0;
    uint c = (viewDepth < f.shadowSplits.x || f.shadow.y == 1) ? 0 : 1;
    float texelWorld = c == 0 ? f.shadowSplits.z : f.shadowSplits.w;
    float3 p = worldPos + n * (texelWorld * 1.5);
    float4 clip = mul(float4(p, 1.0), f.shadowViewProj[c]);
    float2 uv = clip.xy * float2(0.5, -0.5) + 0.5;
    float size = float(f.shadow.z);
    if (any(uv < 2.0 / size) || any(uv > 1.0 - 2.0 / size))
        return 1.0;
    float2 dims = float2(size * float(f.shadow.y), size);
    float2 texel = (float2(uv.x + float(c), uv.y) * float2(size, size)) - 0.5; // atlas pixels (relative to texel center)
    float receiver = clip.z + 0.0002;
    Texture2D map = g_textures[NonUniformResourceIndex(f.shadow.x)];
    float lit = 0.0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        float2 offset = float2((i & 1u) ? 1.0 : -1.0, (i & 2u) ? 1.0 : -1.0);
        float2 t = texel + offset;
        float2 base = floor(t), w = t - base;
        float4 d = map.GatherRed(g_linearClamp, (base + 1.0) / dims); // (0,1) (1,1) (1,0) (0,0)
        float4 s = step(d, receiver.xxxx);
        lit += lerp(lerp(s.w, s.z, w.x), lerp(s.x, s.y, w.x), w.y);
    }
    lit *= 0.25;
    float fade = saturate((f.shadowSplits.y - viewDepth) / (f.shadowSplits.y * 0.1)); // fades over the last 10%
    return lerp(1.0, lit, fade);
}

// Specular integral over roughness and view angle (Karis's approximation, no lookup texture)
float3 envBrdf(float3 f0, float roughness, float ndv)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// Value noise (integer hash, so the same pattern on every GPU). Used by dissolve
float hash3(uint3 p)
{
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ p.z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) / 16777215.0;
}

float valueNoise(float3 p)
{
    float3 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    uint3 c = uint3(int3(i) + 0x4000);
    float x00 = lerp(hash3(c), hash3(c + uint3(1, 0, 0)), f.x);
    float x10 = lerp(hash3(c + uint3(0, 1, 0)), hash3(c + uint3(1, 1, 0)), f.x);
    float x01 = lerp(hash3(c + uint3(0, 0, 1)), hash3(c + uint3(1, 0, 1)), f.x);
    float x11 = lerp(hash3(c + uint3(0, 1, 1)), hash3(c + uint3(1, 1, 1)), f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

// Dissolve: discard pixels whose noise is below progress; within the remaining edge width return 0..1 (emission strength). Progress 1 removes everything
float dissolveEdge(Material mat, VSOutput input)
{
    float progress = float((input.flags >> 24) & 0xFFu) / 255.0;
    if (!(mat.features & kMaterialDissolve) || progress <= 0.0)
        return 0.0;
    float n = valueNoise(input.localPos * 6.0) * 0.7 + valueNoise(input.localPos * 17.0) * 0.3;
    float cut = progress * (1.0 + mat.dissolveEdge);
    if (n < cut)
        discard;
    return 1.0 - saturate((n - cut) / max(mat.dissolveEdge, 1e-4));
}

// Instance flash + fog + screen flash (shared by main and outline passes)
float4 finishColor(FrameConstants frame, VSOutput input, float3 color)
{
    float flash = float((input.flags >> 16) & 0xFFu) / 255.0;
    color = lerp(color, float3(1.0, 1.0, 1.0), flash);
    float dist = distance(input.worldPos, frame.cameraPos.xyz);
    float fog = exp(-dist * frame.fogColor.a);
    return float4(lerp(lerp(frame.fogColor.rgb, color, fog), frame.screenTint.rgb, frame.screenTint.a), 1.0);
}

float4 PSMain(VSOutput input) : SV_Target
{
    FrameConstants frame = loadFrame();
    Material mat = loadMaterial(frame, input.material);
    float4 base = mat.baseColor * sampleTexture(mat.textures.x, input.uv, float4(1, 1, 1, 1));
    if (mat.alphaCutoff > 0.0 && base.a < mat.alphaCutoff)
        discard;
    float edge = dissolveEdge(mat, input);
    float3 n = normalize(input.normal);
    if (mat.textures.y != kNoTexture)
    {
        float2 xy = (sampleTexture(mat.textures.y, input.uv, float4(0.5, 0.5, 1, 1)).xy * 2.0 - 1.0) * mat.normalScale;
        float3 mapNormal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
        n = perturbNormal(n, input.worldPos, input.uv, mapNormal);
    }
    float4 mr = sampleTexture(mat.textures.z, input.uv, float4(1, 1, 1, 1));
    float roughness = clamp(mat.roughness * mr.g, 0.04, 1.0);
    float metallic = saturate(mat.metallic * mr.b);
    float3 emissive = mat.emissive * sampleTexture(mat.textures.w, input.uv, float4(1, 1, 1, 1)).rgb;
    float3 albedo = input.albedo * base.rgb;

    if (input.flags & 1u)
    {
        float2 cell = floor(input.worldPos.xz * 0.25);
        float checker = fmod(abs(cell.x + cell.y), 2.0);
        albedo *= lerp(1.0, 0.85, checker);
    }

    float3 l = frame.sunDir.xyz;
    float halfLambert = dot(n, l) * 0.5 + 0.5;

    // Editor view modes (no fog/flash)
    if (pc.debugView == 2)
        return float4(n * 0.5 + 0.5, 1.0);
    if (pc.debugView == 4) // additive blend (linear color): one layer is dark brown, over ten layers bright orange
        return float4(0.02, 0.008, 0.002, 1.0);
    if (pc.debugView == 3 || pc.debugView == 5)
    {
        uint lod = (input.flags >> 8) & 3u;
        float3 lodColor = lod == 0 ? float3(0.2, 0.8, 0.25) : (lod == 1 ? float3(0.95, 0.8, 0.15) : float3(0.95, 0.25, 0.2));
        float3 tint = pc.debugView == 3 ? lodColor : ((input.flags & 4u) ? float3(0.95, 0.2, 0.15) : float3(0.6, 0.6, 0.6));
        return float4(tint * (0.35 + 0.65 * halfLambert), 1.0);
    }
    if (pc.debugView == 6) // material features: outline red, wind green, rim blue, dissolve yellow (summed when combined)
    {
        uint f = mat.features;
        float3 tint = float3((f & kMaterialOutline) ? 0.9 : 0.0, (f & kMaterialWind) ? 0.8 : 0.0, (f & kMaterialRim) ? 0.9 : 0.0);
        if (f & kMaterialDissolve)
            tint += float3(0.8, 0.7, 0.0);
        if (f == 0)
            tint = float3(0.45, 0.45, 0.45);
        return float4(saturate(tint) * (0.35 + 0.65 * halfLambert), 1.0);
    }

    float3 ambient = ambientLight(frame, n);
    float3 v = normalize(frame.cameraPos.xyz - input.worldPos);
    float ndv = max(dot(n, v), 1e-4);
#if LIGHTING_MODEL != 2
    // The side facing away from the sun is already dark: removing shadows near the terminator hides shadow map stairs on round surfaces
    float3 geometric = normalize(input.normal);
    float shadow = lerp(1.0, sunShadow(frame, input.worldPos, geometric), saturate(dot(geometric, l) * 4.0));
#endif

#if LIGHTING_MODEL == 0
    // GGX specular (one sun) + metals put their color into specular with no diffuse. Environment reflection is in the sky HDRI term
    float3 h = normalize(l + v);
    float ndl = saturate(dot(n, l)), ndh = saturate(dot(n, h)), vdh = saturate(dot(v, h));
    float a = roughness * roughness, a2 = a * a;
    float d = a2 / (3.14159265 * pow(ndh * ndh * (a2 - 1.0) + 1.0, 2.0));
    float k = a * 0.5;
    float vis = 1.0 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k) * 4.0);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
    float3 specular = d * vis * fresnel * ndl;
    float3 diffuse = albedo * (1.0 - metallic);
    float3 ambientSpecular = hasReflection(frame, input.probe) ? reflection(frame, reflect(-v, n), roughness, input.probe) * envBrdf(f0, roughness, ndv)
                                              : f0 * ambient * (1.0 - roughness * 0.7); // old approximation without an HDRI
    float3 color = diffuse * (frame.sunColor.rgb * (halfLambert * halfLambert * shadow) + ambient) + specular * frame.sunColor.rgb * shadow +
                   ambientSpecular + emissive + pointLights(frame, input.lights, input.worldPos, n, v, diffuse, f0, roughness);
#elif LIGHTING_MODEL == 1
    // Toon: diffuse via ramp texture (half Lambert → color steps), highlight is a single hard-edged spot. Metals tint highlights/environment
    // Shade goes to the dark side of the ramp (half of half Lambert)
    float3 ramp = g_textures[NonUniformResourceIndex(frame.style.x)].SampleLevel(g_linearClamp, float2(saturate(halfLambert * lerp(0.5, 1.0, shadow)), 0.5), 0).rgb;
    ramp *= ramp; // sRGB image → linear approximation
    float3 h = normalize(l + v);
    float shininess = 2.0 / max(roughness * roughness * roughness * roughness, 1e-3) - 2.0;
    float highlight = smoothstep(0.45, 0.55, pow(saturate(dot(n, h)), shininess)) * step(0.0, dot(n, l)) * (1.0 - roughness * 0.8) * step(0.5, shadow);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse = albedo * (1.0 - metallic);
    float3 envSpecular = hasReflection(frame, input.probe) ? reflection(frame, reflect(-v, n), roughness, input.probe) * envBrdf(f0, roughness, ndv)
                                          : f0 * ambient * (1.0 - roughness * 0.7);
    float3 color = diffuse * (frame.sunColor.rgb * ramp + ambient) + highlight * f0 * frame.sunColor.rgb * 4.0 + envSpecular + emissive +
                   pointLights(frame, input.lights, input.worldPos, n, v, diffuse, f0, roughness);
#else
    // Unlit: color as is (still gets rim/fog/flash)
    float3 color = albedo + emissive;
#endif

    if (mat.features & kMaterialRim) // rim: edges nearly perpendicular to the view direction
        color += mat.rimColor * (mat.rimStrength * pow(1.0 - ndv, 3.0));
    color += mat.dissolveColor * edge;

    if (input.flags & 2u) // editor selection highlight
        color = lerp(color, float3(1.0, 0.45, 0.05), 0.45) + float3(0.08, 0.03, 0.0);

    return finishColor(frame, input, color);
}

// Outline (inverted hull): discard front faces, draw only inflated back faces in a solid color. Alpha mask/dissolve cut the same as the main pass
float4 PSOutline(VSOutput input) : SV_Target
{
    FrameConstants frame = loadFrame();
    Material mat = loadMaterial(frame, input.material);
    float4 base = mat.baseColor * sampleTexture(mat.textures.x, input.uv, float4(1, 1, 1, 1));
    if (mat.alphaCutoff > 0.0 && base.a < mat.alphaCutoff)
        discard;
    float edge = dissolveEdge(mat, input);
    return finishColor(frame, input, mat.outlineColor + mat.dissolveColor * edge);
}

// Shadow pass (depth only): alpha mask and dissolve punch holes the same as the main pass
void PSShadow(VSOutput input)
{
    FrameConstants frame = loadFrame();
    Material mat = loadMaterial(frame, input.material);
    float4 base = mat.baseColor * sampleTexture(mat.textures.x, input.uv, float4(1, 1, 1, 1));
    if (mat.alphaCutoff > 0.0 && base.a < mat.alphaCutoff)
        discard;
    dissolveEdge(mat, input);
}

// Skybox: one fullscreen triangle (no vertex buffer). Drawn before 3D without depth; 3D covers it. Doesn't get fog
// Without a sky HDRI it's a gradient of the scene's fog and sky colors
struct SkyOutput
{
    float4 position : SV_Position;
    float2 clip : CLIP;
};

SkyOutput VSSky(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    SkyOutput o;
    o.clip = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.position = float4(o.clip, 0.0, 1.0);
    return o;
}

float4 PSSky(SkyOutput input) : SV_Target
{
    FrameConstants frame = loadFrame();
    float4 world = mul(float4(input.clip, 1.0, 1.0), frame.invViewProj); // near plane (reversed-Z)
    float3 d = normalize(world.xyz / world.w - frame.cameraPos.xyz);
    float3 color;
    if (frame.env.w != 0)
        color = g_cubes[NonUniformResourceIndex(frame.env.x)].SampleLevel(g_linearClamp, envDirection(frame, d), 0).rgb * frame.envParams.x;
    else // no HDRI: fog color at the horizon (where fogged 3D meets it) up to the sky color overhead
        color = lerp(frame.fogColor.rgb, frame.skyColor.rgb, sqrt(saturate(d.y)));
    return float4(lerp(color, frame.screenTint.rgb, frame.screenTint.a), 1.0);
}
