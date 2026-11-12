// Post-processing (docs/DECISIONS.md 2026-10-27 "Post-processing"): the scene is drawn to an HDR target (R11G11B10F, linear),
// only bloom uses a half-res dual filter chain (PSBloomDown halves each step → PSBloomUp comes back with additive blend), and everything else is one uber pass (PSUber)
// writing sRGB values to the back buffer (or Ldr target): FXAA → chromatic aberration → exposure + bloom → AgX tone mapping → color grading LUT → vignette → grain.
// Integrated graphics are bandwidth-bound, so effects don't bounce between targets. One fullscreen triangle (no vertex buffer).
//
// Read coordinates: the scene is in the HDR target at viewport (editor scene view) pixels × dynamic resolution scale (sceneSize.zw maps output pixels to that uv). Shaders read via SV_Position (target pixels),
// and areas outside (not cleared) are clipped with rect so they don't bleed.

#include "Bindless.hlsli"

struct PostPush // 48 bytes (render::Renderer PostPush)
{
    uint ring;        // upload ring (g_buffers index)
    uint constOffset; // PostConstants ring offset (PSUber)
    uint source;      // texture to read (g_textures index): lower step / scene
    uint first;       // PSBloomDown: 1 = first from the scene (only light over the threshold)
    float4 rect;      // valid uv region of the source texture (min xy, max xy)
    float2 texel;     // texel size of the source texture (1 / size)
    float2 scale;     // target pixels → source texture uv (1 / target size, between chain steps of different sizes)
};
[[vk::push_constant]] ConstantBuffer<PostPush> g_push : register(b0);

// PostConstants in the upload ring (same layout as render::Renderer, 96 bytes)
struct PostConstants
{
    float4 exposure;  // exposure scale (2^EV × brightness setting), bloom strength, vignette strength, vignette softness
    float4 effects;   // grain strength, chromatic aberration strength (fraction of screen width), grain time, sharpening (dynamic resolution)
    uint4 textures;   // top of the bloom chain (0xFFFFFFFF = no bloom), LUT (32³ strip, 0xFFFFFFFF = none), flags, 0
    float4 bloom;     // bloom color (rgb, linear multiply), threshold
    float4 region;    // scene region uv (min xy, max xy): vignette/chromatic aberration center
    float4 sceneSize; // scene target size (pixels), scale / size (output pixels → scene uv, scale < 1 with dynamic resolution)
};

static const uint kPostToneMap = 1u;   // when off, linear values are only clamped (reflection probe baking, view modes)
static const uint kPostFxaa = 2u;
static const uint kPostDither = 4u;    // reduce 8-bit banding (when tone mapping)
static const uint kPostSharpen = 8u;   // drawn at reduced dynamic resolution: contrast adaptive sharpening after upscaling
static const uint kPostPunchy = 16u;   // AgX "Punchy" look (the base look is flat and pale)
static const uint kNoTex = 0xFFFFFFFFu;

PostConstants loadPost()
{
    ByteAddressBuffer b = g_buffers[g_push.ring];
    uint o = g_push.constOffset;
    PostConstants c;
    c.exposure = loadFloat4(b, o);
    c.effects = loadFloat4(b, o + 16);
    c.textures = b.Load4(o + 32);
    c.bloom = loadFloat4(b, o + 48);
    c.region = loadFloat4(b, o + 64);
    c.sceneSize = loadFloat4(b, o + 80);
    return c;
}

struct PostVSOut
{
    float4 position : SV_Position;
};

PostVSOut VSMain(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    PostVSOut o;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 sampleSource(float2 uv)
{
    uv = clamp(uv, g_push.rect.xy + g_push.texel * 0.5, g_push.rect.zw - g_push.texel * 0.5);
    return g_textures[NonUniformResourceIndex(g_push.source)].SampleLevel(g_linearClamp, uv, 0).rgb;
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---------------------------------------------------------------- Bloom (dual filter, Marius Bjørge 2015)

float4 PSBloomDown(PostVSOut input) : SV_Target
{
    float2 uv = input.position.xy * g_push.scale; // target pixel center = center of the source texture's 2×2
    float2 h = g_push.texel; // one source texel (four diagonal half texels + center)
    float3 c = sampleSource(uv) * 4.0;
    c += sampleSource(uv + float2(-h.x, -h.y));
    c += sampleSource(uv + float2(h.x, -h.y));
    c += sampleSource(uv + float2(-h.x, h.y));
    c += sampleSource(uv + float2(h.x, h.y));
    c *= 0.125;
    if (g_push.first != 0) { // threshold: soft knee (radius 0.5); weights divided by brightness reduce flicker from sparkling points
        PostConstants post = loadPost();
        float threshold = post.bloom.w;
        float l = luminance(c);
        float soft = clamp(l - threshold + 0.5, 0.0, 1.0);
        float contribution = max(soft * soft * 0.5, l - threshold) / max(l, 1e-4);
        c *= contribution / (1.0 + l * 0.05);
    }
    return float4(c, 1.0);
}

float4 PSBloomUp(PostVSOut input) : SV_Target
{
    float2 uv = input.position.xy * g_push.scale;
    float2 h = g_push.texel; // texel of the smaller side
    float3 c = 0.0;
    c += sampleSource(uv + float2(-h.x * 2.0, 0.0));
    c += sampleSource(uv + float2(h.x * 2.0, 0.0));
    c += sampleSource(uv + float2(0.0, -h.y * 2.0));
    c += sampleSource(uv + float2(0.0, h.y * 2.0));
    c += sampleSource(uv + float2(-h.x, -h.y)) * 2.0;
    c += sampleSource(uv + float2(h.x, -h.y)) * 2.0;
    c += sampleSource(uv + float2(-h.x, h.y)) * 2.0;
    c += sampleSource(uv + float2(h.x, h.y)) * 2.0;
    return float4(c / 12.0, 1.0); // additive blend: stacks on top of the value that came down at this step
}

// ---------------------------------------------------------------- Tone mapping (AgX, same family as the Blender 4 default)
// Benjamin Wrensch's polynomial approximation (MIT, iolite-engine.com/blog_posts/minimal_agx_implementation): input is linear Rec.709

float3 agxContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx(float3 c) // linear → display (close to gamma-encoded sRGB, 0..1)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = mul(max(c, 0.0), inset);
    c = clamp(log2(max(c, 1e-10)), minEv, maxEv);
    c = (c - minEv) / (maxEv - minEv);
    return agxContrast(c);
}

float3 agxPunchy(float3 c) // the "Punchy" look from the same implementation: power 1.35, saturation 1.4 (in AgX display space)
{
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = pow(max(c, 0.0), 1.35);
    return luma + 1.4 * (c - luma);
}

float3 agxEotf(float3 c) // AgX display value → linear Rec.709 (undoes the outset primaries)
{
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    c = mul(c, outset);
    return pow(max(c, 0.0), 2.2);
}

float3 toSrgb(float3 linearColor)
{
    linearColor = saturate(linearColor);
    return select(linearColor <= 0.0031308, linearColor * 12.92, 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055);
}

// 32³ LUT as a 1024×32 strip (x = blue slice × 32 + red, y = green). Blends two blue slices
float3 applyLut(uint lut, float3 c)
{
    c = saturate(c);
    float slice = c.b * 31.0;
    float s0 = floor(slice);
    float s1 = min(s0 + 1.0, 31.0);
    float u = c.r * 31.0 + 0.5;
    float v = (c.g * 31.0 + 0.5) / 32.0;
    Texture2D t = g_textures[NonUniformResourceIndex(lut)];
    float3 a = t.SampleLevel(g_linearClamp, float2((s0 * 32.0 + u) / 1024.0, v), 0).rgb;
    float3 b = t.SampleLevel(g_linearClamp, float2((s1 * 32.0 + u) / 1024.0, v), 0).rgb;
    return lerp(a, b, slice - s0);
}

float hash12(float2 p) // evenly scattered 0..1 per pixel (Dave Hoskins hash without sine, MIT)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// FXAA (Timothy Lottes console variant family, 9 taps): HDR values are divided by brightness (reversible Reinhard) before blending so bright points don't smear
float3 compress(float3 c) { return c / (1.0 + luminance(c)); }
float3 uncompress(float3 c) { return c / max(1.0 - luminance(c), 1e-4); }
float edgeLuma(float3 c) { return sqrt(luminance(compress(c))); }

float3 fxaa(float2 uv)
{
    float2 t = g_push.texel;
    float3 rgbM = sampleSource(uv);
    float lumaNW = edgeLuma(sampleSource(uv + float2(-t.x, -t.y)));
    float lumaNE = edgeLuma(sampleSource(uv + float2(t.x, -t.y)));
    float lumaSW = edgeLuma(sampleSource(uv + float2(-t.x, t.y)));
    float lumaSE = edgeLuma(sampleSource(uv + float2(t.x, t.y)));
    float lumaM = edgeLuma(rgbM);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125)) return rgbM; // not an edge

    float2 dir = float2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)), (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8.0, 8.0) * t;
    float3 a = 0.5 * (compress(sampleSource(uv + dir * (1.0 / 3.0 - 0.5))) + compress(sampleSource(uv + dir * (2.0 / 3.0 - 0.5))));
    float3 b = a * 0.5 + 0.25 * (compress(sampleSource(uv - dir * 0.5)) + compress(sampleSource(uv + dir * 0.5)));
    float lumaB = sqrt(luminance(b));
    return uncompress(lumaB < lumaMin || lumaB > lumaMax ? a : b);
}

// Contrast adaptive sharpening (AMD FidelityFX CAS / FSR 1 RCAS family, 5 taps): weaker where neighbor contrast is high, preventing edge rings.
// Undoes blur from bilinear upscaling. HDR is blended in compress space
float3 sharpen(float2 uv, float3 c, float amount)
{
    float2 t = g_push.texel;
    float3 m = compress(c);
    float3 n = compress(sampleSource(uv + float2(0.0, -t.y)));
    float3 s = compress(sampleSource(uv + float2(0.0, t.y)));
    float3 e = compress(sampleSource(uv + float2(t.x, 0.0)));
    float3 w = compress(sampleSource(uv + float2(-t.x, 0.0)));
    float3 mn = min(m, min(min(n, s), min(e, w)));
    float3 mx = max(m, max(max(n, s), max(e, w)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float3 wgt = -amp * lerp(0.125, 0.2, amount);
    float3 r = (m + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt);
    return uncompress(saturate(r));
}

float4 PSUber(PostVSOut input) : SV_Target
{
    PostConstants post = loadPost();
    uint flags = post.textures.z;
    float2 uv = input.position.xy * post.sceneSize.zw;
    float2 regionSize = post.region.zw - post.region.xy;
    float2 centered = (uv - post.region.xy) / max(regionSize, 1e-6) - 0.5; // region center 0, edges ±0.5

    float3 color = (flags & kPostFxaa) ? fxaa(uv) : sampleSource(uv);
    if (flags & kPostSharpen) color = sharpen(uv, color, post.effects.w);
    if (post.effects.y > 0.0) { // chromatic aberration: spread red/blue toward the edges (center unchanged)
        float2 offset = centered * dot(centered, centered) * post.effects.y * regionSize;
        color.r = sampleSource(uv + offset).r;
        color.b = sampleSource(uv - offset).b;
    }
    if (!(flags & kPostToneMap)) return float4(toSrgb(color), 1.0);

    if (post.textures.x != kNoTex) // top of the bloom chain (half res, same uv)
        color += g_textures[NonUniformResourceIndex(post.textures.x)].SampleLevel(g_linearClamp, uv, 0).rgb * post.bloom.rgb * post.exposure.y;
    color *= post.exposure.x;

    float3 mapped = agx(color);
    if (flags & kPostPunchy) mapped = agxPunchy(mapped);
    float3 display = toSrgb(agxEotf(mapped)); // 0..1 sRGB values (same space as LUTs and external grading tools)
    if (post.textures.y != kNoTex) display = applyLut(post.textures.y, display); // project LUT + color blindness correction (one combined by the CPU)
    if (post.exposure.z > 0.0) { // vignette: darken region edges (softness = start radius)
        float r = length(centered * float2(regionSize.x / max(regionSize.y, 1e-6), 1.0)) * 1.41421356;
        display *= 1.0 - post.exposure.z * smoothstep(post.exposure.w, 1.0 + post.exposure.w * 0.5, r);
    }
    float noise = hash12(input.position.xy + post.effects.z * 61.0) - 0.5;
    if (post.effects.x > 0.0) display += noise * post.effects.x * (1.0 - display) * 0.5; // grain: more visible in dark areas
    if (flags & kPostDither) display += noise / 255.0;
    return float4(saturate(display), 1.0);
}
