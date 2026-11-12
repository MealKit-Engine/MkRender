# MkRender

The renderer of the [MealKit](https://github.com/MealKit-Engine/MealKit) engine as engine-free libraries, part of the MealKit tool family. Free, MIT.

> **Beta (v0.x).** APIs may change between versions until v1.0.

| Library | What it is |
|---|---|
| `mkrhi` | Thin graphics layer over DX12 (reference) or Vulkan 1.3: device and swap chain from a native window handle, frame command lists, static buffers, textures, offscreen and depth targets, pipelines with one bindless binding layout, a per-frame upload ring, GPU timestamps, back buffer capture, an on-disk pipeline cache |
| `mkrender` | Forward renderer on `mkrhi` with plain inputs: meshes, materials, 48-byte instances, point lights, camera, sprites, particles, UI quads; PBR/toon/unlit lighting, sun shadows (CSM), sky HDRI and reflection probes, bloom/AgX/LUT post, dynamic resolution, frustum culling in `DrawList`. Shaders (`shaders/`) are compiled to byte arrays at build time |
| `mkterrain` | Heightmap terrain and scatter layers as data (`include/mkrender/Terrain.h`): 16-bit PNG heightmaps, surface height and ray hits, chunk meshes, raise/lower/flatten/smooth brushes, scatter text, painting and erasing; the editing half of MealKit's `asset/Terrain.h` and `asset/Scatter.h` with the same layout and files |
| `mkbake` | CPU baking without GPU or files (`include/mkrender/Bake.h`): sky HDRI (sky cube, reflection cube with GGX mips, SH ambient), reflection probes from six rendered faces (`.mkprobe`), `.cube` LUTs, texture mips and BC7/BC5 (bc7enc/rgbcx), and the cache file bytes and names. The MealKit engine and the scene editor both bake with it, so their caches are the same files |

**MkRender** (`build/bin/MkRender.exe`) is a scene editor built on them, like MkUI for `.ui` screens and MkAudio for sound events: it opens a MealKit project's `.scene` files with a hierarchy, a viewport and an inspector, and saves them for the engine (below).

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

## MkRender scene editor

`tool/` edits a MealKit project's scenes (`assets/scenes/*.scene`) without the engine: an SDL3 window drawn by `mkrender`, Dear ImGui panels docked around the 3D view, ImGuizmo handles.

- **Hierarchy**: every object with its kind, parents and children (drag one onto another to parent it, onto empty space to move it to the top level), scene tree folders, search, add / duplicate / delete
- **Viewport**: the scene drawn with the project's lighting model (`lightingModel` in `mealkit.project`), sun shadows, point lights and post values from the scene; click to select, W/E/R move/rotate/scale handles (Ctrl snaps), F frames the selection; right drag orbits (with WASD/QE to fly), middle drag pans, the wheel zooms. Spawn points, lights and cameras show as icons; probes, reverb zones and game kinds without a model as line boxes (Alt shows names)
- **Inspector**: name, kind (built-in or a game kind's name), folder or parent, parent-relative position / rotation / scale, model (glTF under `assets/`), color, checker, and the object's field lines with widgets picked from their text (numbers, true/false, `#RRGGBB` colors, camera enums); fields can be added and removed
- **Environment**: sun direction and color, sky / ground / fog, sky HDRI file values, post-processing (tone mapping, exposure, bloom, vignette, grain, chromatic aberration, LUT file)
- Undo/redo (Ctrl+Z / Ctrl+Y), save (Ctrl+S), save as, new scene, project scene list; a file changed by the MealKit editor reloads by itself when nothing is edited here, or asks

The file is written the way the engine's scene module writes it (same keys, order, shortest float text, `field` lines), so a scene the engine saved comes back byte for byte, and older formats are upgraded like the engine does. Game kinds' fields are kept as text, so nothing is lost without the game's code. When the project has `build/editor/kinds.txt` (written by the MealKit editor when it loads the game code, or `<game>_headless --write-kinds`; MealKit-Contract "Object kinds"), MkRender uses it for kind names, the [Add] menu, field labels, typed widgets and defaults, and rereads it when it changes.

Editing beyond one object: Shift/Ctrl click and box select, Edit > Arrange (align, distribute), Drop to floor (End), hide (H) and lock, camera bookmarks (Ctrl+1..9 / 1..9) kept in the project's `build/editor/<scene>.view` like the MealKit editor, grid (G) and snap steps (View > Grid and snap), recent scenes, autosave to `build/editor/autosave/`. The Assets window places models, prefabs and PNG sprites by dragging into the view and finds where a file is used; Edit > Save selection as prefab writes `assets/prefabs/<name>.prefab`. Sprites, scatter layers and virtual camera frustums are drawn. Terrain draws its heightmap in chunks. Skinned models play the clip of their `anim` field in a loop (the inspector picks it; View > Play animations), and the Assets window shows pictures of models and PNGs (Pictures box).

Brushes (select a terrain or scatter layer, B or the inspector's Brush box): terrain raise/lower/flatten/smooth and scatter paint/erase (Shift erases) onto terrain and floors, with the radius and strength in the inspector; a stroke is one undo step, and the painted 16-bit PNG and `.scatter` file are written when the scene is saved. A terrain or layer without a file gets a new one (`assets/terrain/<scene>_<id>.png` / `.scatter`).

Rendering parts like the engine: the sky HDRI (baked once into the project's `build/cache/environments/`, the file the engine reads), the color LUT, reflection probes from their `.mkprobe` files, and model textures as BC7/BC5 from the engine's texture cache (`build/cache/textures/`, filled when missing; View > Compress model textures turns it off). [Bake probes] in the Environment window (or the probe's inspector) renders six faces per probe and writes `assets/probes/<scene>_<id>.mkprobe`, the editor's files. [Grading capture] saves the view without the LUT to `art/grading/<scene>.png` with a `neutral.cube` beside it, to grade in an external tool and export into `assets/lut/`.

```
MkRender [--project <folder>] [--file <.scene|.gltf|.glb>] [--select <id>] [--lighting pbr|toon|unlit]
         [--debug-layer] [--frames N --capture out.png] [--bake-probes] [--grading-capture] [--editor-play] [--window <name>] [--version]
MkRender --check-scene <file.scene>   # read + write in memory: same / upgraded / FAIL with the first differing line
MkRender --check-terrain <file.png|file.scatter>   # decode + encode with mkterrain: same bytes, or the same samples from another encoder
MkRender --project <folder> --render-golden <shots.txt> <out folder>   # the MealKit engine's graphics regression shots (engine/tests/golden), 1600 x 900, no UI
```

Playing goes through the project's running MealKit editor (MealKit-Contract "Scene service" on the live connection): MkRender finds it in `<project>/build/live/`, and the Editor menu (F5, Shift+F5) saves the scene and asks the editor to play it in its play view, from this view's camera with Shift (solo; a selected spawn point is used as is), stop, look through the selected virtual camera or preview a cutscene. The editor's [Scenes] window and asset menu open a scene here with "Open in MkRender".

A `.gltf`/`.glb` opens in a new unsaved scene as one decoration. The build writes `mealkit-tool.txt` next to the exe (`opens .scene .gltf .glb`), so the MealKit editor lists MkRender in its [Tools] menu when that folder is in `%LOCALAPPDATA%\MealKit\tools\render\` or given with `--tool-path`; the editor starts tools with `--project <folder> --file <file>`.

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
| `MKTERRAIN_STB_DIR` | stb_image and stb_image_write for `mkterrain` (default `third_party/stb`) |
| `MKBAKE_BC`, `MKBAKE_BC7ENC_DIR` | BC7/BC5 compression in `mkbake` (default on) and the bc7enc folder (default `third_party/bc7enc`) |
| `MKRENDER_BUILD_TOOL` | the scene editor (default: on when built standalone on Windows; it needs `mkrender`, so a DXC) |

`third_party/` holds what the standalone build and the scene editor use: Vulkan-Headers, volk, VMA, SDL3, Dear ImGui (docking), ImGuizmo, cgltf, stb_image, stb_image_write, bc7enc and mkcontract (MealKit-Contract's live client, copied with its `tools/vendor.ps1`) (`mkbake`), each with its license. A host that adds MkRender with `add_subdirectory` uses its own copies through the variables above.

GPU capture tools (RenderDoc, PIX) are loaded by the host from `DeviceDesc::beforeDeviceCreate`, which runs right before the device is made.

## In MealKit

The engine pins a copy in `third_party/mkrender` (`tools\update_mkrender.ps1 -From <checkout>`) and wraps it in the engine modules `rhi` (`mealkit::rhi`, engine log, capture tools) and `render` (`render::Renderer` with the job system, shader hot reload, PNG screenshots; model, texture, sky and sprite loading from engine assets; the texture and environment caches and probe files through `mkbake`). To work on both, configure the engine with `-DMEALKIT_MKRENDER_DIR=<checkout>`.

## License

MIT, see `LICENSE`.
