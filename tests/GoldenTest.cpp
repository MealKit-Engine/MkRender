// Render goldens without the engine (engine ROADMAP "MkRender leftovers"): a hidden window, fixed scenes built from plain meshes,
// sprites, particles and UI quads, each frame compared with tests/golden/<shot>.png like the engine's graphics regression test
// (engine tools/check.ps1 "Graphics regression": a pixel is off when a channel differs by more than 8, at most 0.5% of pixels off).
// DX12 and Vulkan builds compare against the same images.
//   mkrender_golden            compare; on a difference writes <build>/golden/<shot>.png and <shot>-diff.png (red = off)
//   mkrender_golden --update   writes this run's images into tests/golden (after a change on purpose, checked by eye)
// No GPU (CI runners, software adapters are skipped by mkrhi): prints SKIP and passes, like the GPU checks in RenderTests.cpp.
#include "mkrender/DrawList.h"
#include "mkrender/Renderer.h"
#include "mkrhi/Rhi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Static stb copies (PNG only) like mkterrain's, so they don't clash with anything linked
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

using namespace mkrender;

namespace {

constexpr u32 kWidth = 640, kHeight = 360;
constexpr int kFrames = 4;      // per shot: the capture is taken at the end of the last one
constexpr int kThreshold = 8;   // per channel, like check.ps1
constexpr double kMaxBadPercent = 0.5;
constexpr f32 kPi = 3.14159265f;

// ---------------------------------------------------------------- meshes

u32 packUv(f32 u, f32 v) {
    // half floats for 0..1 values (exact enough for the test's uvs)
    const auto half = [](f32 f) -> u32 {
        u32 bits;
        std::memcpy(&bits, &f, 4);
        const u32 sign = (bits >> 16) & 0x8000u;
        const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
        if (f == 0.0f || exponent <= 0) return sign;
        return sign | (static_cast<u32>(exponent) << 10) | ((bits >> 13) & 0x3FFu);
    };
    return half(u) | (half(v) << 16);
}

// Front faces: (b - a) x (c - a) along the normal, the scene editor's rule (tool/Model.cpp fixWinding)
void fixWinding(MeshData& mesh) {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vertex& a = mesh.vertices[mesh.indices[i]];
        const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        if (dot(cross(b.position - a.position, c.position - a.position), a.normal + b.normal + c.normal) < 0.0f)
            std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

MeshData boxMesh(u16 material) {
    MeshData m;
    const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3& n : normals) {
        // two axes across the face
        const Vec3 a = std::abs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 b = cross(n, a);
        const u32 base = static_cast<u32>(m.vertices.size());
        const f32 corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            Vertex v;
            v.position = n * 0.5f + a * (0.5f * c[0]) + b * (0.5f * c[1]);
            v.normal = n;
            v.uv = packUv(c[0] * 0.5f + 0.5f, c[1] * 0.5f + 0.5f);
            v.material = material;
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    fixWinding(m);
    return m;
}

MeshData sphereMesh(u16 material, u32 rings = 24, u32 segments = 48) {
    MeshData m;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = kPi * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = 2.0f * kPi * static_cast<f32>(s) / static_cast<f32>(segments);
            const Vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            Vertex v;
            v.position = n * 0.5f;
            v.normal = n;
            v.uv = packUv(static_cast<f32>(s) / static_cast<f32>(segments), static_cast<f32>(r) / static_cast<f32>(rings));
            v.material = material;
            m.vertices.push_back(v);
        }
    }
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    fixWinding(m);
    return m;
}

Quat axisAngle(Vec3 axis, f32 angle) {
    const Vec3 a = normalize(axis);
    const f32 s = std::sin(angle * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
}

Mat4 orthographicReversedZ(f32 halfHeight, f32 aspect, f32 zNear, f32 zFar) {
    Mat4 r;
    r.m[0][0] = 1.0f / (halfHeight * aspect);
    r.m[1][1] = 1.0f / halfHeight;
    r.m[2][2] = -1.0f / (zFar - zNear);
    r.m[3][2] = zFar / (zFar - zNear);
    r.m[3][3] = 1.0f;
    return r;
}

// ---------------------------------------------------------------- scene

struct SceneIds {
    MeshId box = kNoMesh, ball = kNoMesh, metalBall = kNoMesh, toonBox = kNoMesh, glowBox = kNoMesh;
    u32 spriteTexture = 0, smallTexture = 0;
};

// 8 x 8 sprite: a ring with a transparent middle and corners, a darker top row (shows flips and point sampling); premultiplied alpha
std::vector<u8> spritePixels(u32 size) {
    std::vector<u8> px(static_cast<size_t>(size) * size * 4);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const f32 cx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size) - 0.5f, cy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size) - 0.5f;
            const f32 r = std::sqrt(cx * cx + cy * cy);
            u8 c[4] = {240, 200, 60, 255};
            if (y == 0) c[0] = 60, c[1] = 90, c[2] = 220;
            if (x == 0) c[0] = 220, c[1] = 60, c[2] = 60;
            const bool inside = r < 0.5f && (r > 0.22f || y == 0 || x == 0);
            const u8 a = inside ? 255 : 0;
            u8* p = &px[(static_cast<size_t>(y) * size + x) * 4];
            for (int i = 0; i < 3; ++i) p[i] = static_cast<u8>(c[i] * a / 255);
            p[3] = a;
        }
    return px;
}

SceneIds makeScene(Renderer& r) {
    SceneIds ids;
    MaterialData rough;
    rough.baseColor[0] = 0.8f, rough.baseColor[1] = 0.8f, rough.baseColor[2] = 0.8f;
    rough.roughness = 0.9f;
    const u16 roughId = static_cast<u16>(r.createMaterial(rough));
    MaterialData metal;
    metal.baseColor[0] = 0.95f, metal.baseColor[1] = 0.75f, metal.baseColor[2] = 0.35f;
    metal.metallic = 1.0f;
    metal.roughness = 0.3f;
    const u16 metalId = static_cast<u16>(r.createMaterial(metal));
    MaterialData toon;
    toon.baseColor[0] = 0.3f, toon.baseColor[1] = 0.6f, toon.baseColor[2] = 0.9f;
    toon.features = kMaterialOutline | kMaterialRim;
    toon.outlineWidth = 0.03f;
    toon.rimStrength = 0.8f;
    const u16 toonId = static_cast<u16>(r.createMaterial(toon));
    MaterialData glow;
    glow.baseColor[0] = 0.2f, glow.baseColor[1] = 0.2f, glow.baseColor[2] = 0.2f;
    glow.emissive[0] = 6.0f, glow.emissive[1] = 2.0f, glow.emissive[2] = 0.5f; // over the bloom threshold
    const u16 glowId = static_cast<u16>(r.createMaterial(glow));
    ids.box = r.createMesh(boxMesh(roughId));
    ids.ball = r.createMesh(sphereMesh(roughId));
    ids.metalBall = r.createMesh(sphereMesh(metalId));
    ids.toonBox = r.createMesh(boxMesh(toonId));
    ids.glowBox = r.createMesh(boxMesh(glowId));
    for (u32 size : {8u, 4u}) {
        const std::vector<u8> px = spritePixels(size);
        mkrhi::TextureInit init;
        init.width = init.height = size;
        init.format = mkrhi::Format::Rgba8Srgb;
        init.data = px.data();
        (size == 8 ? ids.spriteTexture : ids.smallTexture) = r.createTexture(init);
    }
    return ids;
}

void fill3d(DrawList& list, const SceneIds& ids) {
    // floor (checker), boxes, a rough and a metal ball, a toon box with outline and rim, a glowing box for bloom
    list.add(ids.box, {{0, -0.25f, 0}, rgba(200, 200, 205), {}, {12, 0.5f, 12}, kInstanceChecker});
    list.add(ids.box, {{-1.6f, 0.5f, 0.4f}, rgba(220, 90, 70), axisAngle({0, 1, 0}, 0.5f), {1, 1, 1}});
    list.add(ids.box, {{-0.4f, 0.35f, 1.8f}, rgba(90, 200, 120), axisAngle({0, 1, 0}, -0.3f), {0.7f, 0.7f, 0.7f}});
    list.add(ids.ball, {{0.3f, 0.6f, -0.2f}, rgba(240, 240, 240), {}, {1.2f, 1.2f, 1.2f}});
    list.add(ids.metalBall, {{1.6f, 0.45f, 0.6f}, rgba(255, 255, 255), {}, {0.9f, 0.9f, 0.9f}});
    list.add(ids.toonBox, {{1.2f, 0.4f, -1.4f}, rgba(255, 255, 255), axisAngle({0, 1, 0}, 0.8f), {0.8f, 0.8f, 0.8f}});
    list.add(ids.glowBox, {{-1.4f, 0.2f, -1.3f}, rgba(255, 255, 255), {}, {0.4f, 0.4f, 0.4f}});
    // particles: normal and additive, soft and hard
    std::vector<Particle> particles;
    for (int i = 0; i < 12; ++i) {
        Particle p;
        const f32 a = static_cast<f32>(i) / 12.0f * 2.0f * kPi;
        p.position = {-1.4f + std::cos(a) * 0.6f, 0.9f + 0.1f * std::sin(a * 3.0f), -1.3f + std::sin(a) * 0.6f};
        p.size = 0.18f;
        p.color = i % 2 ? rgba(255, 180, 80, 255) : rgba(120, 200, 255, 200);
        p.flags = i % 2 ? kParticleAdditive : 0u;
        p.softness = i % 3 == 0 ? 0.0f : 0.6f;
        particles.push_back(p);
    }
    list.addParticles(particles.data(), particles.size());
}

void fillSprites(DrawList& list, const SceneIds& ids) {
    // a box behind the sprites (sprites are depth tested), then overlapping sprites: layers, flips, point vs linear, tint alpha, sheet cell
    list.add(ids.box, {{0, 0, 2}, rgba(60, 70, 90), {}, {8, 4.5f, 0.2f}});
    const auto sprite = [&](Vec3 at, f32 size, u32 flags, i32 layer, u32 color, u32 texture) {
        Sprite s;
        s.position = at;
        s.width = s.height = size;
        s.flags = flags;
        s.layer = layer;
        s.color = color;
        s.texture = texture;
        list.addSprite(s);
    };
    sprite({-2.6f, 0.8f, 0}, 1.4f, kSpritePoint, 0, rgba(255, 255, 255), ids.spriteTexture);
    sprite({-1.0f, 0.8f, 0}, 1.4f, 0, 0, rgba(255, 255, 255), ids.spriteTexture);                               // linear filtering
    sprite({0.6f, 0.8f, 0}, 1.4f, kSpritePoint | kSpriteFlipX, 0, rgba(255, 255, 255), ids.spriteTexture);
    sprite({2.2f, 0.8f, 0}, 1.4f, kSpritePoint | kSpriteFlipY, 0, rgba(255, 255, 255), ids.spriteTexture);
    // overlap: the higher layer draws over the lower one even when it is farther away; a half transparent tint on top
    sprite({-1.4f, -0.9f, 0.5f}, 1.6f, kSpritePoint, 2, rgba(255, 255, 255), ids.spriteTexture);
    sprite({-0.8f, -1.1f, -0.5f}, 1.6f, kSpritePoint, 1, rgba(120, 255, 160), ids.spriteTexture);
    sprite({0.0f, -1.0f, 0}, 1.6f, kSpritePoint, 3, rgba(255, 120, 200, 128), ids.spriteTexture);
    // a sheet cell: the top left quarter of the small texture, stretched
    Sprite cell;
    cell.position = {1.9f, -1.0f, 0};
    cell.width = cell.height = 1.4f;
    cell.flags = kSpritePoint;
    cell.u1 = cell.v1 = 0.5f;
    cell.texture = ids.smallTexture;
    list.addSprite(cell);
}

std::vector<UiQuad> uiQuads() {
    // a rounded panel with a stroke and a vertical gradient, and a shadow under it (UI pass over everything)
    std::vector<UiQuad> quads(2);
    UiQuad& shadow = quads[0];
    shadow.rect[0] = 24, shadow.rect[1] = 28, shadow.rect[2] = 180, shadow.rect[3] = 64;
    for (f32& r : shadow.radii) r = 14;
    shadow.color0 = rgba(0, 0, 0, 140);
    shadow.flags = kUiShadow;
    shadow.softness = 10;
    UiQuad& panel = quads[1];
    panel.rect[0] = 20, panel.rect[1] = 20, panel.rect[2] = 180, panel.rect[3] = 64;
    for (f32& r : panel.radii) r = 14;
    panel.color0 = rgba(40, 44, 60, 230);
    panel.color1 = rgba(70, 90, 160, 230);
    panel.gradient[0] = 0, panel.gradient[1] = 0, panel.gradient[2] = 0, panel.gradient[3] = 1;
    panel.flags = kUiShape | kUiLinear;
    panel.strokeColor = rgba(255, 255, 255, 200);
    panel.strokeWidth = 2;
    return quads;
}

struct Shot {
    const char* name;
    LightingModel lighting;
    bool sprites; // the 2D front view with sprites instead of the 3D scene
};
constexpr Shot kShots[] = {
    {"lit_pbr", LightingModel::Pbr, false},
    {"lit_toon", LightingModel::Toon, false},
    {"lit_unlit", LightingModel::Unlit, false},
    {"sprites", LightingModel::Unlit, true},
};

FrameParams frameFor(const Shot& shot, std::vector<PointLight>& lights) {
    FrameParams f;
    const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);
    if (shot.sprites) {
        // the 2D camera: looks along +z, orthographic, 2.5 m half height
        const Vec3 eye{0, 0, -10};
        f.viewProj = lookAtLH(eye, {0, 0, 0}, {0, 1, 0}) * orthographicReversedZ(2.5f, aspect, 0.1f, 100.0f);
        f.cameraPos = eye;
        f.cameraForward = {0, 0, 1};
        f.post.bloomIntensity = 0.0f;
    } else {
        const Vec3 eye{3.8f, 3.2f, -5.2f}, target{0, 0.4f, 0};
        f.viewProj = lookAtLH(eye, target, {0, 1, 0}) * perspectiveReversedZ(50.0f * kPi / 180.0f, aspect, 0.1f, 200.0f);
        f.cameraPos = eye;
        f.cameraForward = normalize(target - eye);
        lights = {{{0.2f, 1.4f, -1.6f}, 4.0f, {3.0f, 1.6f, 0.6f}}, {{-2.2f, 1.0f, 1.6f}, 3.5f, {0.4f, 0.8f, 2.4f}}};
        f.lights = lights.data();
        f.lightCount = static_cast<u32>(lights.size());
    }
    f.time = 0.0f; // wind and anything time based stay still
    return f;
}

// ---------------------------------------------------------------- images

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writePng(const std::filesystem::path& path, const Image& image) {
    std::string bytes;
    stbi_write_png_to_func([](void* user, void* data, int size) { static_cast<std::string*>(user)->append(static_cast<const char*>(data), static_cast<size_t>(size)); },
                           &bytes, static_cast<int>(image.width), static_cast<int>(image.height), 4, image.rgba.data(), static_cast<int>(image.width * 4));
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return !bytes.empty() && static_cast<bool>(out);
}

bool readPng(const std::filesystem::path& path, Image& image) {
    std::string bytes;
    if (!readFile(path, bytes)) return false;
    int w = 0, h = 0, n = 0;
    u8* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    image.width = static_cast<u32>(w);
    image.height = static_cast<u32>(h);
    image.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

// Fraction of pixels (percent) with a color channel more than kThreshold apart; diff gets them in red over a dimmed reference
double comparePixels(const Image& golden, const Image& actual, Image& diff) {
    diff.width = golden.width;
    diff.height = golden.height;
    diff.rgba.assign(golden.rgba.size(), 255);
    size_t bad = 0;
    const size_t pixels = static_cast<size_t>(golden.width) * golden.height;
    for (size_t i = 0; i < pixels; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(golden.rgba[i * 4 + c] - actual.rgba[i * 4 + c]));
        u8* d = &diff.rgba[i * 4];
        if (worst > kThreshold) {
            ++bad;
            d[0] = 255, d[1] = 0, d[2] = 0;
        } else {
            for (int c = 0; c < 3; ++c) d[c] = static_cast<u8>(golden.rgba[i * 4 + c] / 4);
        }
    }
    return pixels ? 100.0 * static_cast<double>(bad) / static_cast<double>(pixels) : 100.0;
}

#ifdef _WIN32
HWND makeHiddenWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MkRenderGolden";
    RegisterClassExW(&wc);
    // the client area is the back buffer size (a Vulkan swap chain takes the surface's current extent)
    RECT rect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, wc.lpszClassName, L"mkrender_golden", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                           rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr); // never shown
}
#endif

} // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
#ifndef _WIN32
    (void)update;
    std::printf("SKIP mkrender_golden: Windows only for now\n");
    return 0;
#else
    mkrhi::setLog([](mkrhi::LogLevel level, const char* text, void*) {
        if (level != mkrhi::LogLevel::Info) std::printf("[rhi] %s\n", text);
    });
    HWND window = makeHiddenWindow();
    Renderer renderer;
    RendererDesc desc;
    desc.window = window;
    desc.width = kWidth;
    desc.height = kHeight;
    desc.vsync = false;
    if (!window || !renderer.init(desc)) {
        std::printf("SKIP mkrender_golden (%s): no GPU device (%s)\n", mkrhi::backendName(), window ? renderer.initError() : "no window");
        return 0;
    }
    std::printf("mkrender_golden: %s, %s, %ux%u\n", mkrhi::backendName(), renderer.adapterName(), kWidth, kHeight);
    const SceneIds ids = makeScene(renderer);
    const std::vector<UiQuad> ui = uiQuads();
    const std::filesystem::path goldenDir = MKRENDER_GOLDEN_DIR, outDir = std::filesystem::current_path() / "golden";

    struct Captured {
        Image image;
        bool done = false;
    } captured;
    int failures = 0;
    DrawList list;
    std::vector<PointLight> lights;
    for (const Shot& shot : kShots) {
        renderer.setLightingModel(shot.lighting);
        captured = {};
        for (int frame = 0; frame < kFrames; ++frame) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            list.clear();
            if (shot.sprites) fillSprites(list, ids);
            else fill3d(list, ids);
            if (!renderer.beginFrame(frameFor(shot, lights))) continue;
            list.submit(renderer);
            renderer.beginOverlay();
            renderer.drawUi(ui.data(), static_cast<u32>(ui.size()));
            if (frame + 1 == kFrames)
                renderer.requestCapture([](const Image& image, void* user) {
                    auto* c = static_cast<Captured*>(user);
                    c->image = image;
                    c->done = true;
                }, &captured);
            renderer.endFrame();
        }
        renderer.waitIdle();
        if (!captured.done || captured.image.width != kWidth || captured.image.height != kHeight) {
            std::printf("FAIL %s: no capture\n", shot.name);
            ++failures;
            continue;
        }
        const std::filesystem::path golden = goldenDir / (std::string(shot.name) + ".png");
        if (update) {
            const bool ok = writePng(golden, captured.image);
            std::printf("%s %s -> %s\n", ok ? "UPDATED" : "FAIL", shot.name, golden.string().c_str());
            failures += ok ? 0 : 1;
            continue;
        }
        Image reference, diff;
        if (!readPng(golden, reference)) {
            std::printf("FAIL %s: no reference image %s (run mkrender_golden --update)\n", shot.name, golden.string().c_str());
            writePng(outDir / (std::string(shot.name) + ".png"), captured.image);
            ++failures;
            continue;
        }
        if (reference.width != kWidth || reference.height != kHeight) {
            std::printf("FAIL %s: reference is %ux%u, expected %ux%u\n", shot.name, reference.width, reference.height, kWidth, kHeight);
            ++failures;
            continue;
        }
        const double badPercent = comparePixels(reference, captured.image, diff);
        const bool ok = badPercent <= kMaxBadPercent;
        std::printf("%s %s: %.3f%% of pixels off (limit %.1f%%)\n", ok ? "OK  " : "FAIL", shot.name, badPercent, kMaxBadPercent);
        if (!ok) {
            ++failures;
            writePng(outDir / (std::string(shot.name) + ".png"), captured.image);
            writePng(outDir / (std::string(shot.name) + "-diff.png"), diff);
        }
    }
    renderer.shutdown();
    DestroyWindow(window);
    std::printf("%s: %d shots, %d failures%s\n", failures ? "FAIL" : "OK", static_cast<int>(sizeof(kShots) / sizeof(kShots[0])), failures,
                failures && !update ? (" (images in " + outDir.string() + ")").c_str() : "");
    return failures ? 1 : 0;
#endif
}
