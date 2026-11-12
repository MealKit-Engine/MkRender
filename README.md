# MkRender

The renderer of the [MealKit](https://github.com/MealKit-Engine/MealKit) engine as engine-free libraries, part of the MealKit tool family. Free, MIT.

> **Beta (v0.x).** APIs may change between versions until v1.0. Today it holds `mkrhi`; the renderer itself (`mkrender`) follows.

| Library | What it is |
|---|---|
| `mkrhi` | Thin graphics layer over DX12 (reference) or Vulkan 1.3: device and swap chain from a native window handle, frame command lists, static buffers, textures, offscreen and depth targets, pipelines with one bindless binding layout, a per-frame upload ring, GPU timestamps, back buffer capture, an on-disk pipeline cache |
| `mkrender` | (next) Forward renderer with plain inputs: meshes, materials, lights, camera, sprites, particles, UI quads; shadows, post-processing, dynamic resolution |

The libraries include no engine header. The host sets a log callback and passes plain values:

```cpp
mkrhi::setLog([](mkrhi::LogLevel level, const char* text, void*) { /* your log */ });
mkrhi::Device device;
mkrhi::DeviceDesc desc;
desc.window = hwnd;            // native handle
desc.width = 1280;
desc.height = 720;
desc.pipelineCache = cacheDir; // optional
if (!device.init(desc)) { /* device.initError() */ }
// every frame
if (device.beginFrame()) {
    mkrhi::CommandList& cmd = device.cmd();
    cmd.beginOverlay();
    // setPipeline, pushConstants, draw ...
    device.endFrame(/*vsync*/ true);
}
```

The binding layout is the same for every pipeline: 64 bytes of push constants, `ByteAddressBuffer g_buffers[]`, `Texture2D g_textures[]`, `TextureCube g_cubes[]` and two static samplers (`include/mkrhi/Rhi.h` has the registers). `queryGpu` and `gpuMinimumError` check a GPU against MealKit's minimum spec (Shader Model 6.6 bindless, Intel 11th gen integrated graphics) without a window.

## Building

```
build.bat                     # Windows: DX12, Release, Ninja; runs the tests
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Options for a host project that adds it with `add_subdirectory`:

| Variable | Meaning |
|---|---|
| `MKRHI_BACKEND` | `DX12` (default) or `Vulkan` |
| `MKRHI_VULKAN_INCLUDE_DIRS` | Vulkan headers, volk and VMA folders (required for Vulkan; volk loads `vulkan-1` at run time, no SDK needed) |
| `MKRHI_IMGUI_TARGET` | a Dear ImGui target with the matching backend: builds `mkrhi::imgui*` for debug UI. Only its include folders are used; the host links ImGui |
| `MKRHI_TRACY_TARGET` | a Tracy target: GPU frame zones on DX12 when it defines `TRACY_ENABLE` |
| `MKRENDER_OBJECT_LIBRARIES` | `ON` builds OBJECT libraries (for a host DLL that exports every symbol) |

GPU capture tools (RenderDoc, PIX) are loaded by the host from `DeviceDesc::beforeDeviceCreate`, which runs right before the device is made.

## In MealKit

The engine pins a copy in `third_party/mkrender` (`tools\update_mkrender.ps1 -From <checkout>`) and wraps it in the engine module `rhi` (`mealkit::rhi`, engine log, capture tools). To work on both, configure the engine with `-DMEALKIT_MKRENDER_DIR=<checkout>`.

## License

MIT, see `LICENSE`.
