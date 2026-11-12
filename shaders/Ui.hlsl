// MealKit 2D UI (mealkit/ui/). After the 3D scene, at native back buffer resolution, no depth.
// One instance = one rectangle on screen (6 vertices via SV_VertexID, no vertex buffer). Shapes are drawn with SDFs in the pixel shader:
// per-corner rounded rectangles, solid/linear/radial gradients, inner borders, blurred shadows, rotation, images (RGBA atlas), text (SDF atlas).
// Colors are sRGB 8-bit as is (overlay view without gamma conversion), output is premultiplied alpha.

#include "Bindless.hlsli"

struct UiQuad // 112 bytes (render::UiQuad)
{
    float4 rect;     // x, y, w, h (pixels, box before rotation)
    float4 clip;     // x0, y0, x1, y1 (pixels, nothing is drawn outside)
    float4 radii;    // top left, top right, bottom right, bottom left (pixels)
    float4 uv;       // atlas u0, v0, u1, v1
    float4 gradient; // linear: start xy, end xy / radial: center xy, radius xy (box 0..1 coordinates)
    uint color0;     // fill (gradient start)
    uint color1;     // gradient end
    uint strokeColor;
    uint flags;      // 0-3 kind (0 shape, 1 image, 2 text, 3 shadow), 4-5 fill (0 solid, 1 linear, 2 radial)
    float strokeWidth;
    float rotation;  // radians (around the box center)
    float softness;  // shadow blur (pixels)
    float sdfScale;  // text: screen pixels per atlas value unit
};

struct PushConstants
{
    float2 screenSize;
    uint ring;       // g_buffers[] index of the upload ring
    uint quadOffset; // UiQuad array offset
    uint glyphAtlas; // g_textures[] index: glyph SDF (R8)
    uint imageAtlas; // g_textures[] index: images (RGBA8)
    uint2 pad;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc : register(b0);

UiQuad loadQuad(uint index)
{
    ByteAddressBuffer b = g_buffers[pc.ring];
    uint o = pc.quadOffset + index * 112;
    UiQuad q;
    q.rect = loadFloat4(b, o);
    q.clip = loadFloat4(b, o + 16);
    q.radii = loadFloat4(b, o + 32);
    q.uv = loadFloat4(b, o + 48);
    q.gradient = loadFloat4(b, o + 64);
    q.color0 = b.Load(o + 80);
    q.color1 = b.Load(o + 84);
    q.strokeColor = b.Load(o + 88);
    q.flags = b.Load(o + 92);
    q.strokeWidth = asfloat(b.Load(o + 96));
    q.rotation = asfloat(b.Load(o + 100));
    q.softness = asfloat(b.Load(o + 104));
    q.sdfScale = asfloat(b.Load(o + 108));
    return q;
}

struct VSOutput
{
    float4 position : SV_Position;
    float2 local : LOCAL; // pixels relative to box center (before rotation)
    nointerpolation uint quad : QUAD;
};

float4 unpack(uint c)
{
    return float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    UiQuad q = loadQuad(instanceId);
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    float2 c = corners[vertexId];
    float margin = q.softness * 2.0 + 1.0; // half pixel outside the border + shadow blur
    float2 half = q.rect.zw * 0.5;
    float2 local = lerp(-half - margin, half + margin, c);
    float s = sin(q.rotation), co = cos(q.rotation);
    float2 rotated = float2(local.x * co - local.y * s, local.x * s + local.y * co);
    float2 pixel = q.rect.xy + half + rotated;
    VSOutput o;
    o.position = float4(pixel.x / pc.screenSize.x * 2.0 - 1.0, 1.0 - pixel.y / pc.screenSize.y * 2.0, 0.0, 1.0);
    o.local = local;
    o.quad = instanceId;
    return o;
}

// Signed distance of a rounded rectangle with per-corner radii (negative inside)
float roundBox(float2 p, float2 half, float4 r)
{
    float radius = p.x > 0.0 ? (p.y > 0.0 ? r.z : r.y) : (p.y > 0.0 ? r.w : r.x);
    radius = min(radius, min(half.x, half.y));
    float2 d = abs(p) - half + radius;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
}

float4 PSMain(VSOutput input) : SV_Target
{
    UiQuad q = loadQuad(input.quad);
    float2 screen = input.position.xy;
    if (screen.x < q.clip.x || screen.y < q.clip.y || screen.x >= q.clip.z || screen.y >= q.clip.w) discard;

    uint kind = q.flags & 0xF;
    float2 half = q.rect.zw * 0.5;
    float2 box01 = (input.local + half) / max(q.rect.zw, 1e-3);
    float d = roundBox(input.local, half, q.radii);
    float coverage = saturate(0.5 - d);

    if (kind == 3) // shadow
    {
        float blur = max(q.softness, 0.5);
        float a = 1.0 - smoothstep(-blur, blur, d);
        float4 c = unpack(q.color0);
        return float4(c.rgb, 1.0) * (c.a * a);
    }
    if (kind == 2) // text (SDF, 0.5 = edge)
    {
        float2 uv = lerp(q.uv.xy, q.uv.zw, box01);
        float s = g_textures[pc.glyphAtlas].SampleLevel(g_linearClamp, uv, 0).r;
        float a = saturate((s - 0.5) * q.sdfScale + 0.5);
        float4 c = unpack(q.color0);
        return float4(c.rgb, 1.0) * (c.a * a);
    }

    float4 fill;
    if (kind == 1) // image (multiplied by color)
    {
        float2 uv = lerp(q.uv.xy, q.uv.zw, box01);
        fill = g_textures[pc.imageAtlas].SampleLevel(g_linearClamp, uv, 0) * unpack(q.color0);
        float4 bright = unpack(q.color1); // brightness: toward white or black by its alpha
        fill.rgb = lerp(fill.rgb, bright.rgb, bright.a);
    }
    else
    {
        uint paint = (q.flags >> 4) & 0x3;
        float4 c0 = unpack(q.color0);
        fill = c0;
        if (paint == 1)
        {
            float2 a = q.gradient.xy, b = q.gradient.zw;
            float2 ab = b - a;
            float t = saturate(dot(box01 - a, ab) / max(dot(ab, ab), 1e-6));
            fill = lerp(c0, unpack(q.color1), t);
        }
        else if (paint == 2)
        {
            float t = saturate(length((box01 - q.gradient.xy) / max(q.gradient.zw, 1e-4)));
            fill = lerp(c0, unpack(q.color1), t);
        }
    }

    // Inner border: from the outer edge to strokeWidth inward
    float inner = q.strokeWidth > 0.0 ? saturate(0.5 - (d + q.strokeWidth)) : coverage;
    float strokeCoverage = coverage - inner;
    float4 stroke = unpack(q.strokeColor);
    float3 rgb = fill.rgb * fill.a * inner + stroke.rgb * stroke.a * strokeCoverage;
    float alpha = fill.a * inner + stroke.a * strokeCoverage;
    return float4(rgb, alpha);
}
