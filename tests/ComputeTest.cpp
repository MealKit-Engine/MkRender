// RHI compute without the renderer (G1): a hidden window, a storage buffer and a storage texture, three compute passes with barriers,
// then the buffer is read back and compared. DX12 and Vulkan run the same HLSL. No GPU: prints SKIP and passes, like mkrender_golden.
#include "mkrhi/Rhi.h"

#include "g_ComputeAddCS.h"
#include "g_ComputeCopyCS.h"
#include "g_ComputeFillCS.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <X11/Xlib.h>
#endif

using namespace mkrhi;

namespace {

constexpr u32 kWidth = 320, kHeight = 180;
constexpr u32 kCount = 100;     // uints written by CSFill / CSAdd
constexpr u32 kTexels = 64;     // 8x8 texture, copied after the first kCount uints

struct Push {
    u32 buffer, texture, textureSrv, count;
};

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
    check(buffer && texture && fill && add && copy, "resources and pipelines are created");

    const Push push{device.storageBufferIndex(buffer), device.storageTextureIndex(texture), device.textureIndex(texture), kCount};
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
        cmd.setPipeline(copy); // reads the texture through g_textures[]
        cmd.pushConstants(&push, sizeof(push));
        cmd.dispatch(1, 1, 1);
        cmd.bufferBarrier(buffer, BufferState::UnorderedAccess, BufferState::ShaderRead);
        device.endFrame(false);
    }
    device.waitIdle();

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
    if (badValues || badTexels) std::printf("  bad values %u / %u, bad texels %u / %u (first: %08x %08x)\n", badValues, kCount, badTexels, kTexels, out[0], out[kCount]);

    // Hot reload of a compute pipeline keeps its handle
    check(device.recreatePipeline(fill, ComputePipelineDesc{{g_ComputeFillCS, sizeof(g_ComputeFillCS)}}), "recreatePipeline (compute)");

    device.destroyBuffer(buffer);
    device.shutdown();
    destroyWindow(window);
    std::printf("%s: mkrender_compute %d failures\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
