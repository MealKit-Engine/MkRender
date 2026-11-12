# MkRender

The renderer of the [MealKit](https://github.com/MealKit-Engine/MealKit) engine as engine-free libraries, part of the MealKit tool family. Free, MIT.

> **Beta (v0.x).** APIs may change between versions until v1.0.

| Library | What it is |
|---|---|
| `mkrhi` | Thin graphics layer over DX12 (reference) or Vulkan 1.3: device and swap chain from a native window handle, frame command lists, static buffers, textures, offscreen and depth targets, pipelines with one bindless binding layout, a per-frame upload ring, GPU timestamps, back buffer capture, an on-disk pipeline cache |
| `mkrender` | Forward renderer on `mkrhi` with plain inputs: meshes, materials, 48-byte instances, point lights, camera, sprites, particles, UI quads; PBR/toon/unlit lighting, sun shadows (CSM), sky HDRI and reflection probes, bloom/AgX/LUT post, dynamic resolution, frustum culling in `DrawList`. Shaders (`shaders/`) are compiled to byte arrays at build time |

**MkRender** (`build/bin/MkRender.exe`) is a viewer built on them: open a glTF model or a material sphere grid and change lighting, shadows, post-processing and materials live (below).

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

## Drawing a frame

```cpp
mkrender::Renderer renderer;
mkrender::RendererDesc desc;
desc.window = hwnd;
desc.width = 1280;
desc.height = 720;
desc.parallelFor = myParallelFor; // optional: culling on your worker threads
renderer.init(desc);
mkrender::MeshId box = renderer.createMesh(meshData); // Vertex (36 bytes), u32 indices
mkrender::DrawList list;
// every frame
mkrender::FrameParams frame;
frame.viewProj = view * projection; // row vectors, reversed-Z
frame.cameraPos = eye;
if (renderer.beginFrame(frame)) {
    list.clear();
    list.add(box, {position, mkrender::rgba(255, 128, 0), rotation, scale});
    list.submit(renderer);  // shadows, culling, front-to-back, sprites, particles
    renderer.beginOverlay();
    renderer.drawUi(quads, count); // optional 2D pass
    renderer.endFrame();
}
```

### Your own types

A host that already has a `Vec3`/`Quat`/`Mat4`, a vertex and material struct or an image struct with the same members and layout can use them directly, like Dear ImGui's `imconfig.h`: set `MKRENDER_USER_CONFIG` to a header that defines `MKRENDER_HOST_MATH`, `MKRENDER_HOST_MESH` and/or `MKRENDER_HOST_IMAGE` and brings its types into `namespace mkrender` with using-declarations (`include/mkrender/Config.h` lists what each one needs). The static_asserts in the headers check the layouts. MealKit does this for its math and mesh types.

### Hooks

| `RendererDesc` | Meaning |
|---|---|
| `parallelFor` | runs culling ranges on the host's workers (serial without it) |
| `shaderReload` | called once per pipeline with its `shaders/` source and entry points; the host recompiles changed files and calls the given function outside a frame to swap the pipeline |
| `beforeDeviceCreate` | loads GPU capture tools before the device exists |
| `debugViews` | builds the wireframe/overdraw pipelines for `DebugView` |

`requestCapture(fn, user)` hands the back buffer to `fn` at the next `endFrame` (the host writes the file); `captureTarget` reads an offscreen target.

## MkRender viewer

`tool/` is a small look-development app: an SDL3 window drawn by `mkrender`, with Dear ImGui panels docked beside the 3D view.

- **Scene**: open `.gltf`/`.glb` (File menu, Ctrl+O, or drop a file on the window; F5 reloads), or the 5x5 material sphere grid (metallic x roughness); floor, turntable, field of view, dissolve and flash instance effects
- **Lighting**: PBR / toon / unlit, sun direction, color and intensity, sky/ground/fog colors, sun shadows (cascades, map size, distance), orbiting point lights, wind
- **Post**: AgX tone mapping, exposure, bloom, vignette, grain, chromatic aberration, screen tint, and the player options (FXAA, brightness, color blindness)
- **Materials**: base color, metallic, roughness, emissive, normal scale, alpha cutoff, and the style features (outline, rim, wind, dissolve), applied as you drag (`Renderer::updateMaterial`)
- **Stats**: fps, draw calls, culled instances, GPU time per pass, GPU memory, view modes (wireframe, normals, overdraw, features), vsync, log

Left drag orbits, right/middle drag or Shift+left drag pans, the wheel zooms, F frames the model. File > Save screenshot writes the 3D view without panels to the settings folder (`%APPDATA%\MealKit\MkRender`).

```
MkRender [model.glb] [--lighting pbr|toon|unlit] [--debug-layer] [--frames N --capture out.png] [--version]
```

Models are read like MealKit reads them (glTF metallic-roughness, base color/normal/metallic-roughness/emissive textures, alpha mask, z flipped to left-handed); skins are shown in the bind pose. The build writes `mealkit-tool.txt` next to the exe, so the MealKit editor lists it in its [Tools] menu when that folder is in `%LOCALAPPDATA%\MealKit\tools\render\` or given with `--tool-path`.

## Building

```
build.bat                     # Windows: DX12, Release, Ninja; runs the tests (mkrender needs a DXC with SPIR-V support); add Vulkan for the Vulkan backend into build-vulkan\
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

Options for a host project that adds it with `add_subdirectory`:

| Variable | Meaning |
|---|---|
| `MKRHI_BACKEND` | `DX12` (default) or `Vulkan` |
| `MKRHI_VULKAN_INCLUDE_DIRS` | Vulkan headers, volk and VMA folders (default: the copies in `third_party/`; volk loads `vulkan-1` at run time, no SDK needed) |
| `MKRHI_IMGUI_TARGET` | a Dear ImGui target with the matching backend: builds `mkrhi::imgui*` for debug UI. Only its include folders are used; the host links ImGui |
| `MKRHI_TRACY_TARGET` | a Tracy target: GPU frame zones on DX12 when it defines `TRACY_ENABLE` |
| `MKRENDER_OBJECT_LIBRARIES` | `ON` builds OBJECT libraries (for a host DLL that exports every symbol) |
| `MKRENDER_DXC` | `dxc` with SPIR-V support (default: on `PATH` or MealKit's download cache). Without it only `mkrhi` is built |
| `MKRENDER_USER_CONFIG`, `MKRENDER_USER_CONFIG_INCLUDE_DIRS` | your own types (above) and the folders that header needs |
| `MKRENDER_BUILD_TOOL` | the viewer (default: on when built standalone on Windows; it needs `mkrender`, so a DXC) |

`third_party/` holds what the standalone build and the viewer use: Vulkan-Headers, volk, VMA, SDL3, Dear ImGui (docking), cgltf and stb_image, each with its license. A host that adds MkRender with `add_subdirectory` uses its own copies through the variables above.

GPU capture tools (RenderDoc, PIX) are loaded by the host from `DeviceDesc::beforeDeviceCreate`, which runs right before the device is made.

## In MealKit

The engine pins a copy in `third_party/mkrender` (`tools\update_mkrender.ps1 -From <checkout>`) and wraps it in the engine modules `rhi` (`mealkit::rhi`, engine log, capture tools) and `render` (`render::Renderer` with the job system, shader hot reload, PNG screenshots; model, texture, sky and sprite loading from engine assets). To work on both, configure the engine with `-DMEALKIT_MKRENDER_DIR=<checkout>`.

## License

MIT, see `LICENSE`.
