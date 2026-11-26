#pragma once

// WebGPU backend internals (only rhi/webgpu/ files include this). Dawn's C++ API (webgpu_cpp.h), Linux X11 surfaces for now.
// Work in progress: WebGPU has no descriptor arrays, so textures sit in
// "direct slots" (separately bound, picked by the index, shaders/Bindless.hlsli MKRHI_WEBGPU). Texture array pools, compute, GPU timers,
// ImGui and the pipeline cache are not done yet.
#include "mkrhi/Rhi.h"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <utility>
#include <vector>

namespace mkrhi {

constexpr u32 kDirectTextures = 16; // group 2 bindings 0..15 (Bindless.hlsli g_direct*)
constexpr u32 kDirectCubes = 4;     // group 2 bindings 16..19 (g_directCube*)
constexpr u32 kDirectDepths = 4;    // group 2 bindings 20..23 (g_directDepth*: shadow maps, texture_depth_2d)
constexpr u32 kDirectBindings = kDirectTextures + kDirectCubes + kDirectDepths;
constexpr u32 kDirectPool = 31;     // index bits 23..19 for a direct slot (pools 0..30 come later)
constexpr u64 kUploadBytesPerFrame = 8ull << 20;
constexpr wgpu::TextureFormat kHdrFormat = wgpu::TextureFormat::RG11B10Ufloat; // scene/bloom targets (Target::Scene/Hdr, linear)
constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth32Float;

struct Device::Impl {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface;
    wgpu::TextureFormat swapFormat = wgpu::TextureFormat::Undefined; // 8-bit UNORM (Ldr targets use it too)
    bool swapBgra = false;
    bool swapCopySrc = false; // the surface allows CopySrc (back buffer capture)
    bool immediateSupported = false, mailboxSupported = false;
    bool vsync = true;
    bool surfaceDirty = true;

    // Group 0: upload ring + samplers. Group 1: empty (texture array pools later). Group 2: direct slots. 64 bytes of immediates
    wgpu::Sampler linearClamp, linearWrap, shadowCompare;
    wgpu::BindGroupLayout layout0, layout1, layout2;
    wgpu::PipelineLayout pipelineLayout;
    wgpu::BindGroup group0, group1;
    // Direct slot contents (Texture::id, 0 = empty) and the group 2 variants built from them: one per texture left out (the color or
    // depth target bound in the current pass, which a pass can't also read). Rebuilt after a slot changes
    std::array<u32, kDirectTextures> slots{};
    std::array<u32, kDirectCubes> cubeSlots{};
    std::array<u32, kDirectDepths> depthSlots{};
    std::vector<std::pair<u32, wgpu::BindGroup>> group2Cache;
    wgpu::Texture dummy2D, dummyCube, dummyDepth;
    wgpu::TextureView dummy2DView, dummyCubeView, dummyDepthView;

    // Upload ring: written on the CPU, sent with queue.WriteBuffer before the frame's submit (WebGPU has no persistent mapping)
    wgpu::Buffer ring;
    std::vector<u8> ringCpu;
    u64 uploadStart = 0, uploadOffset = 0, uploadLimit = 0;

    struct BufferEntry {
        wgpu::Buffer buffer;
        BufferUsage usage = BufferUsage::Vertex;
    };
    std::vector<BufferEntry> buffers;
    struct TextureEntry {
        wgpu::Texture texture;
        wgpu::TextureView view;
        u32 slot = 0;
        bool cube = false;
        bool depth = false; // depth texture: a depth slot (shadow maps, read only through mkShadowGather)
    };
    std::vector<TextureEntry> textures;
    struct TargetEntry {
        u32 texture = 0; // textures index (Texture::id)
        wgpu::Texture depth;
        wgpu::TextureView depthView;
        u32 width = 0, height = 0;
        bool depthOnly = false;
        bool ldr = false; // TargetKind::Ldr: the swap chain's 8-bit format
    };
    std::vector<TargetEntry> targets;
    std::vector<wgpu::RenderPipeline> pipelines;

    // Frame
    CommandList cmd;
    wgpu::CommandEncoder encoder;
    wgpu::RenderPassEncoder pass;
    wgpu::Texture backTexture;
    wgpu::TextureView backView;
    u32 frameIndex = 0;
    bool inFrame = false;
    bool backDrawn = false;   // the back buffer was bound this frame
    u32 passExclude = 0;      // Texture::id bound as an attachment in the current pass (0 = none)
    u32 passWidth = 0, passHeight = 0;

    // Draw state: WebGPU passes start empty, so the last values set are kept here and applied before each draw
    u32 pipeline = 0, vertexBuffer = 0, indexBuffer = 0;
    f32 viewport[4] = {};
    u8 push[Device::kPushConstantBytes] = {};
    u32 pushBytes = 0;
    struct Applied {
        u32 pipeline = 0, vertexBuffer = 0, indexBuffer = 0;
        WGPUBindGroup group2 = nullptr;
        f32 viewport[4] = {-1, -1, -1, -1};
        bool push = false;
    } applied;

    u32 width = 0, height = 0;
    GpuInfo gpu;
    GpuTimes gpuTimes;
    std::string initError;

    bool configureSurface();
    void endPass();
    void beginPass(const wgpu::RenderPassDescriptor& desc, u32 exclude, u32 w, u32 h);
    wgpu::BindGroup group2(u32 exclude);
    void slotsChanged() { group2Cache.clear(); }
    bool flushDraw();
    bool wait(const wgpu::Future& future, const char* what);
};

} // namespace mkrhi
