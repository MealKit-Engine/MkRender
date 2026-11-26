// RHI compute without the renderer (G1): a hidden window, a storage buffer and a storage texture, three compute passes with barriers,
// then the buffer is read back and compared. DX12 and Vulkan run the same HLSL. No GPU: prints SKIP and passes, like mkrender_golden.
#include "mkrhi/Rhi.h"

#include "g_ComputeAddCS.h"
#include "g_ComputeCopyCS.h"
#include "g_ComputeFillCS.h"
#include "g_ComputeReadCS.h"
#include "g_ComputeSampleCS.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#else
#include <X11/Xlib.h>
#endif

using namespace mkrhi;

namespace {

constexpr u32 kWidth = 320, kHeight = 180;
constexpr u32 kCount = 100;     // uints written by CSFill / CSAdd
constexpr u32 kTexels = 64;     // 8x8 texture, copied after the first kCount uints

struct Push {
    u32 buffer, texture, textureSrv, count, bufferSrv, buffer2, mipTexture, cube;
};
constexpr u32 kSamples = 8 + 12; // CSSample: 2D lods 0..7, cube faces at lods 0 and 1

// Mip k of the 2D texture (64×64, 7 mips) and face f / mip m of the cube (16×16, 2 mips): solid colors
u32 mipColor(u32 k) { return (k * 30) | ((255 - k * 30) << 8) | ((k * 10) << 16) | 255u << 24; }
void cubeColor(u32 face, u32 mip, f32 rgb[3]) {
    rgb[0] = 0.25f * static_cast<f32>(face + 1);
    rgb[1] = 0.5f * static_cast<f32>(mip);
    rgb[2] = 1.0f;
}
// Shared-exponent RGB9E5 (exact for these colors)
u32 packRgb9e5(const f32 rgb[3]) {
    const f32 maxc = std::fmax(rgb[0], std::fmax(rgb[1], rgb[2]));
    int e = maxc > 0.0f ? std::max(-16, static_cast<int>(std::floor(std::log2(maxc)))) + 16 : 0;
    f32 scale = std::exp2(static_cast<f32>(e - 24));
    if (std::lround(maxc / scale) == 512) {
        scale *= 2.0f;
        ++e;
    }
    u32 v = static_cast<u32>(e) << 27;
    for (int i = 0; i < 3; ++i) v |= static_cast<u32>(std::lround(rgb[i] / scale)) << (9 * i);
    return v;
}
u32 unorm(f32 v) { return static_cast<u32>(std::lround(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f)); }

#ifdef _WIN32
struct NativeWindow {
    HWND hwnd = nullptr;
    explicit operator bool() const { return hwnd != nullptr; }
    void* handle() { return hwnd; }
};
NativeWindow makeHiddenWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MkRenderCompute";
    RegisterClassExW(&wc);
    RECT rect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    NativeWindow w;
    w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"mkrender_compute", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    return w;
}
void destroyWindow(NativeWindow& w) { if (w.hwnd) DestroyWindow(w.hwnd); }
#elif defined(__EMSCRIPTEN__)
// Browser: the page's canvas (mkrender_compute.html)
struct NativeWindow {
    WebCanvas native;
    explicit operator bool() const { return true; }
    void* handle() { return &native; }
};
NativeWindow makeHiddenWindow() {
    emscripten_set_canvas_element_size("#canvas", kWidth, kHeight);
    return {};
}
void destroyWindow(NativeWindow&) {}
#else
struct NativeWindow {
    Display* display = nullptr;
    Window window = 0;
    XlibWindow native;
    explicit operator bool() const { return window != 0; }
    void* handle() { return &native; }
};
NativeWindow makeHiddenWindow() {
    NativeWindow w;
    w.display = XOpenDisplay(nullptr);
    if (!w.display) return w;
    w.window = XCreateSimpleWindow(w.display, RootWindow(w.display, DefaultScreen(w.display)), 0, 0, kWidth, kHeight, 0, 0, 0);
    XFlush(w.display);
    w.native.display = w.display;
    w.native.window = w.window;
    return w;
}
void destroyWindow(NativeWindow& w) {
    if (!w.display) return;
    if (w.window) XDestroyWindow(w.display, w.window);
    XCloseDisplay(w.display);
}
#endif

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

} // namespace

int main() {
    setLog([](LogLevel level, const char* text, void*) {
        if (level != LogLevel::Info) std::printf("[rhi] %s\n", text);
    });
    NativeWindow window = makeHiddenWindow();
    Device device;
    DeviceDesc desc;
    desc.window = window ? window.handle() : nullptr;
    desc.width = kWidth;
    desc.height = kHeight;
    if (!window || !device.init(desc)) {
        std::printf("SKIP mkrender_compute (%s): no GPU device (%s)\n", backendName(), window ? device.initError().c_str() : "no window");
        return 0;
    }
    std::printf("mkrender_compute: %s, %s\n", backendName(), device.gpu().adapter.c_str());

    const Buffer buffer = device.createStorageBuffer((kCount + kTexels) * 4);
    const Texture texture = device.createStorageTexture(8, 8, Format::Rgba8Unorm);
    const Pipeline fill = device.createComputePipeline({{g_ComputeFillCS, sizeof(g_ComputeFillCS)}});
    const Pipeline add = device.createComputePipeline({{g_ComputeAddCS, sizeof(g_ComputeAddCS)}});
    const Pipeline copy = device.createComputePipeline({{g_ComputeCopyCS, sizeof(g_ComputeCopyCS)}});
    const Buffer buffer2 = device.createStorageBuffer((kCount + kSamples) * 4);
    const Pipeline read = device.createComputePipeline({{g_ComputeReadCS, sizeof(g_ComputeReadCS)}});
    const Pipeline sample = device.createComputePipeline({{g_ComputeSampleCS, sizeof(g_ComputeSampleCS)}});
    std::vector<u32> mipData;
    for (u32 k = 0; k < 7; ++k) mipData.insert(mipData.end(), static_cast<size_t>(64 >> k) * (64 >> k), mipColor(k));
    TextureInit mipInit;
    mipInit.width = mipInit.height = 64;
    mipInit.mipLevels = 7;
    mipInit.format = Format::Rgba8Unorm;
    mipInit.data = mipData.data();
    const Texture mipTexture = device.createTexture(mipInit);
    std::vector<u32> cubeData;
    for (u32 face = 0; face < 6; ++face)
        for (u32 mip = 0; mip < 2; ++mip) {
            f32 rgb[3];
            cubeColor(face, mip, rgb);
            cubeData.insert(cubeData.end(), static_cast<size_t>(16 >> mip) * (16 >> mip), packRgb9e5(rgb));
        }
    TextureInit cubeInit;
    cubeInit.width = cubeInit.height = 16;
    cubeInit.mipLevels = 2;
    cubeInit.format = Format::Rgb9e5;
    cubeInit.data = cubeData.data();
    cubeInit.cube = true;
    const Texture cube = device.createTexture(cubeInit);
    check(buffer && texture && fill && add && copy && buffer2 && read && sample && mipTexture && cube, "resources and pipelines are created");

    const Push push{device.storageBufferIndex(buffer), device.storageTextureIndex(texture), device.textureIndex(texture), kCount,
                    device.bufferIndex(buffer), device.storageBufferIndex(buffer2), device.textureIndex(mipTexture),
                    device.textureIndex(cube)};
    bool drawn = false;
    for (int attempt = 0; attempt < 30 && !drawn; ++attempt) drawn = device.beginFrame();
    check(drawn, "a frame begins");
    if (drawn) {
        CommandList& cmd = device.cmd();
        cmd.bufferBarrier(buffer, BufferState::ShaderRead, BufferState::UnorderedAccess);
        cmd.textureBarrier(texture, TextureState::ShaderRead, TextureState::UnorderedAccess);
        cmd.setPipeline(fill);
        cmd.pushConstants(&push, sizeof(push));
        cmd.dispatch((kCount + 63) / 64, 1, 1);
        cmd.uavBarrier();
        cmd.setPipeline(add); // read-modify-write on what fill wrote
        cmd.pushConstants(&push, sizeof(push));
        cmd.dispatch((kCount + 63) / 64, 1, 1);
        cmd.uavBarrier();
        cmd.textureBarrier(texture, TextureState::UnorderedAccess, TextureState::ShaderRead);
        cmd.useTextures({device.textureIndex(texture), device.textureIndex(mipTexture), device.textureIndex(cube)}); // WebGPU group 2
        cmd.setPipeline(copy); // reads the texture through g_textures[]
        cmd.pushConstants(&push, sizeof(push));
        cmd.dispatch(1, 1, 1);
        cmd.bufferBarrier(buffer, BufferState::UnorderedAccess, BufferState::ShaderRead);
        cmd.bufferBarrier(buffer2, BufferState::ShaderRead, BufferState::UnorderedAccess);
        cmd.setPipeline(read); // reads the first buffer through g_buffers[]
        cmd.pushConstants(&push, sizeof(push));
        cmd.dispatch((kCount + 63) / 64, 1, 1);
        cmd.uavBarrier();
        cmd.setPipeline(sample); // texture and cube sampling, after the first kCount values
        cmd.pushConstants(&push, sizeof(push));
        cmd.dispatch(1, 1, 1);
        cmd.bufferBarrier(buffer2, BufferState::UnorderedAccess, BufferState::ShaderRead);
        device.endFrame(false);
    }
    device.waitIdle();

    // GPU timers: a dispatch inside timer 0 over a few frames, read kFramesInFlight frames late (skipped without timestamp support)
    for (u32 frame = 0; frame < Device::kFramesInFlight + 2; ++frame) {
        if (!device.beginFrame()) continue;
        CommandList& cmd = device.cmd();
        cmd.beginTimer(0);
        cmd.setPipeline(sample);
        cmd.pushConstants(&push, sizeof(push));
        cmd.bufferBarrier(buffer2, BufferState::ShaderRead, BufferState::UnorderedAccess);
        cmd.dispatch(1, 1, 1);
        cmd.bufferBarrier(buffer2, BufferState::UnorderedAccess, BufferState::ShaderRead);
        cmd.endTimer(0);
        device.endFrame(false);
    }
    device.waitIdle();
    const GpuTimes& times = device.gpuTimes();
    if (times.valid) {
        // A section can measure 0 where timestamps are coarse (browsers quantize them)
        check(times.frameMs > 0.0f && times.timerMs[0] >= 0.0f && times.timerMs[0] <= times.frameMs, "GPU timers: frame and section times");
        std::printf("  GPU frame %.4f ms, timer 0 %.4f ms\n", times.frameMs, times.timerMs[0]);
    } else {
        std::printf("  GPU timers not available\n");
    }

    std::vector<u32> out(kCount + kTexels, 0xDEADBEEFu);
    check(device.readBuffer(buffer, 0, out.data(), out.size() * 4), "readBuffer");
    u32 badValues = 0, badTexels = 0;
    for (u32 i = 0; i < kCount; ++i) badValues += out[i] != i * 3 + 1 + 1000;
    for (u32 i = 0; i < kTexels; ++i) {
        const u32 expect = (i % 8) * 32 | ((i / 8) * 32) << 8 | 255u << 24;
        badTexels += out[kCount + i] != expect;
    }
    check(badValues == 0, "buffer: fill then add, ordered by uavBarrier");
    check(badTexels == 0, "texture: written as UAV, read through g_textures[] after the state change");
    std::vector<u32> out2(kCount + kSamples, 0xDEADBEEFu);
    check(device.readBuffer(buffer2, 0, out2.data(), out2.size() * 4), "readBuffer (second buffer)");
    u32 badReads = 0;
    for (u32 i = 0; i < kCount; ++i) badReads += out2[i] != (i * 3 + 1 + 1000) * 2;
    check(badReads == 0, "buffer: read through g_buffers[] in ShaderRead");
    u32 badMips = 0, badFaces = 0;
    for (u32 lod = 0; lod < 8; ++lod) badMips += out2[kCount + lod] != mipColor(std::min(lod, 6u));
    for (u32 i = 0; i < 12; ++i) {
        f32 rgb[3];
        cubeColor(i % 6, i / 6, rgb);
        const u32 expect = unorm(rgb[0] * 0.5f) | unorm(rgb[1] * 0.5f) << 8 | unorm(rgb[2] * 0.5f) << 16 | 255u << 24;
        badFaces += out2[kCount + 8 + i] != expect;
    }
    check(badMips == 0, "2D texture: each mip level, past the last mip clamped");
    check(badFaces == 0, "cube: each face and mip");
    if (badMips || badFaces)
        std::printf("  bad mips %u / 8, bad cube samples %u / 12 (lod 0: %08x, lod 7: %08x, cube +X mip 1: %08x)\n", badMips, badFaces,
                    out2[kCount], out2[kCount + 7], out2[kCount + 14]);
    if (badValues || badTexels) std::printf("  bad values %u / %u, bad texels %u / %u (first: %08x %08x)\n", badValues, kCount, badTexels, kTexels, out[0], out[kCount]);

    // Hot reload of a compute pipeline keeps its handle
    check(device.recreatePipeline(fill, ComputePipelineDesc{{g_ComputeFillCS, sizeof(g_ComputeFillCS)}}), "recreatePipeline (compute)");

    device.destroyBuffer(buffer);
    device.destroyBuffer(buffer2);
    device.shutdown();
    destroyWindow(window);
    std::printf("%s: mkrender_compute %d failures\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
