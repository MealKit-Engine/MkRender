// MkRender tests without an engine. GPU checks only report when there's no GPU (CI machines without one still pass).
#include "mkrender/Bake.h"
#include "mkrhi/Rhi.h"
#if MKRENDER_TESTS_RENDERER
#include "mkrender/DrawList.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void testFormats() {
    using mkrhi::Format;
    check(mkrhi::textureMipBytes(Format::Rgba8Unorm, 4, 2) == 32, "rgba8 mip bytes");
    check(mkrhi::textureMipBytes(Format::R8Unorm, 3, 3) == 9, "r8 mip bytes");
    check(mkrhi::textureMipBytes(Format::Bc7Srgb, 5, 4) == 32, "bc7 rounds up to 4x4 blocks");
    check(mkrhi::textureMipBytes(Format::Bc5Unorm, 1, 1) == 16, "bc5 one block");
    check(mkrhi::isBlockCompressed(Format::Bc7Unorm) && !mkrhi::isBlockCompressed(Format::Rgb9e5), "block compressed formats");
}

std::string g_lastLog;
void captureLog(mkrhi::LogLevel, const char* message, void* user) {
    g_lastLog = message;
    ++*static_cast<int*>(user);
}

void testGpu() {
    int logs = 0;
    mkrhi::setLog(&captureLog, &logs);
    const char* backend = mkrhi::backendName();
    check(std::strcmp(backend, "dx12") == 0 || std::strcmp(backend, "vulkan") == 0, "backend name");
    // DX12 judges the feature fields; Vulkan records what's missing in GpuInfo::unsupported while querying
    mkrhi::GpuInfo none;
    if (std::strcmp(backend, "dx12") == 0) check(!mkrhi::gpuMinimumError(none).empty(), "an empty GpuInfo is below the minimum spec");
    none.unsupported = "missing feature";
    check(!mkrhi::gpuMinimumError(none).empty(), "a GpuInfo with a missing feature is below the minimum spec");
    mkrhi::GpuInfo gpu;
    std::string error;
    if (mkrhi::queryGpu(gpu, &error)) {
        check(!gpu.adapter.empty() && !gpu.api.empty(), "queryGpu fills adapter and api");
        const std::string why = mkrhi::gpuMinimumError(gpu);
        std::printf("GPU (%s): %s, %s%s%s\n", backend, gpu.adapter.c_str(), gpu.api.c_str(), why.empty() ? "" : " - below minimum: ", why.c_str());
    } else {
        std::printf("GPU (%s): none (%s)\n", backend, error.c_str());
    }
    mkrhi::setLog(nullptr);
}

// mkbake: values the engine's tests check too, plus the file round trips the engine and the scene editor share
void testBake() {
    using namespace mkrender;
    f32 back[3];
    unpackRgb9e5(packRgb9e5(1.5f, 0.75f, 0.0f), back);
    check(std::abs(back[0] - 1.5f) < 0.01f && std::abs(back[1] - 0.75f) < 0.01f && back[2] == 0.0f, "rgb9e5 round trip");
    const Vec3 px = cubeDirection(0, 0, 0), nz = cubeDirection(5, 0, 0);
    check(px.x > 0.99f && nz.z < -0.99f, "cube face directions");
    for (u32 face = 0; face < 6; ++face) {
        const Vec3 f = cubeFaceForward(face), d = cubeDirection(face, 0, 0);
        check(dot(f, d) > 0.99f, "cube face camera looks at the face center");
    }
    // Flat grey sky: ambient equals the radiance in every direction
    std::vector<f32> flat(64 * 32 * 3, 0.5f);
    EnvironmentMap env;
    bakeEnvironment(flat.data(), 64, 32, env);
    const Vec3 up = evalSh(env.sh, {0, 1, 0}), side = evalSh(env.sh, {1, 0, 0});
    check(std::abs(up.x - 0.5f) < 0.02f && std::abs(side.y - 0.5f) < 0.02f, "flat sky ambient");
    EnvironmentMap read;
    check(readEnvironmentFile(environmentFileBytes(env), read) && read.reflect == env.reflect && read.sky == env.sky, "environment file round trip");
    check(!readEnvironmentFile("nope", read), "environment file rejects other bytes");
    check(environmentCacheName("abc") == environmentCacheName("abc") && environmentCacheName("abc") != environmentCacheName("abd") &&
              environmentCacheName("abc").size() == 22,
          "environment cache names");
    // Probe: six flat faces, file round trip
    const u32 size = 16;
    std::vector<f32> faces(static_cast<size_t>(size) * size * 18, 0.25f);
    ProbeMap probe, probeRead;
    bakeProbe(faces.data(), size, probe);
    std::string error;
    check(probe.size == 16 && probe.mips == 3 && readProbeFile(probeFileBytes(probe), probeRead, error) && probeRead.reflect == probe.reflect,
          "probe bake and file round trip");
    // LUT: the neutral table reads back as identity
    std::vector<f32> lut;
    check(parseCubeLut(neutralCubeLut(8), 32, lut, error) && lut.size() == 32 * 32 * 32 * 3, "neutral LUT parses");
    check(std::abs(lut[(5 * 32 * 32 + 9 * 32 + 20) * 3 + 0] - 20.0f / 31.0f) < 1e-4f, "neutral LUT is identity");
    check(!parseCubeLut("LUT_1D_SIZE 4\n0 0 0\n", 32, lut, error), "1D LUTs are refused");
    // Textures: mips, compression, cache file
    std::vector<u8> rgba(8 * 4 * 4, 200);
    std::vector<std::vector<u8>> mips;
    buildMips(rgba.data(), 8, 4, true, false, mips);
    check(mips.size() == 4 && mips.back().size() == 4, "mips down to 1x1");
    CompressedTexture tex, texRead;
    if (compressTexture(rgba.data(), 8, 4, TextureUsage::Color, tex)) {
        check(tex.format == mkrhi::Format::Bc7Srgb && tex.mipLevels == 4 && tex.data.size() == 32 + 16 * 3, "BC7 sizes");
        check(readTextureFile(textureFileBytes(tex), texRead) && texRead.data == tex.data, "texture file round trip");
    } else {
        check(!hasTextureCompression(), "compressTexture fails only without MKBAKE_BC");
    }
    check(textureCacheName("x", 1, TextureUsage::Color) != textureCacheName("x", 1, TextureUsage::Normal), "texture cache names include the usage");
}

#if MKRENDER_TESTS_RENDERER
void testCulling() {
    using namespace mkrender;
    const Vec3 pts[] = {{-1, 0, 0}, {1, 0, 0}, {0, 2, 0}};
    const BoundingSphere b = boundingSphere(pts, 3);
    check(b.center.y == 1.0f && std::abs(b.radius - std::sqrt(2.0f)) < 1e-5f, "bounding sphere from the vertex box");
    // Camera at the origin looking down +z, 90 degrees, 0.1..100
    const Mat4 vp = lookAtLH({0, 0, 0}, {0, 0, 1}, {0, 1, 0}) * perspectiveReversedZ(1.5707963f, 1.0f, 0.1f, 100.0f);
    const Frustum f = Frustum::fromViewProj(vp);
    check(f.sphereVisible({0, 0, 10}, 0.5f) && !f.sphereVisible({0, 0, -10}, 0.5f) && !f.sphereVisible({50, 0, 10}, 0.5f), "frustum planes");
    // cullInstances without a device: the renderer's parallel-for runs serially, the result keeps bucket order
    Renderer renderer;
    std::vector<std::vector<InstanceData>> buckets(2);
    InstanceData in, out;
    in.position = {0, 0, 10};
    out.position = {0, 0, -10};
    buckets[0] = {in, out, in};
    buckets[1] = {out};
    CullScratch scratch;
    cullInstances(renderer, buckets, 2, f, [](size_t) { return BoundingSphere{{0, 0, 0}, 0.5f}; }, scratch);
    check(scratch.visible[0].size() == 2 && scratch.visible[1].empty(), "cullInstances keeps the instances inside");
    static_assert(sizeof(InstanceData) == 48 && sizeof(Particle) == 32 && sizeof(Sprite) == 80 && sizeof(UiQuad) == 112);
}
#endif

} // namespace

int main() {
    testFormats();
    testGpu();
    testBake();
#if MKRENDER_TESTS_RENDERER
    testCulling();
#endif
    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
