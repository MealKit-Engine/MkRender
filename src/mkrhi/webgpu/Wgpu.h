#pragma once

// WebGPU backend internals (only rhi/webgpu/ files include this). Dawn's C++ API (webgpu_cpp.h): native Dawn on Linux X11, the
// browser's WebGPU through Emscripten's emdawnwebgpu port (__EMSCRIPTEN__, a canvas). WebGPU has no descriptor arrays and browsers
// allow 16 sampled textures per shader stage: asset textures go into texture array pools (group 1: one texture_2d_array per format
// and size class, the index names pool + layer + mip bias), the rest are "direct" (group 2: the few CommandList::useTextures names,
// picked by the index's direct id). shaders/Bindless.hlsli MKRHI_WEBGPU decodes both. Compute adds group 3 (storage buffers and
// textures, shaders/Compute.hlsli). Debug UI: WgpuImGui.cpp.
#include "mkrhi/Rhi.h"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mkrhi {

// Direct textures (render targets, storage textures, formats and sizes the pools don't take) get a direct id (index bits 18..11,
// one id space for 2D, cube and depth). Browsers allow 16 sampled textures per shader stage by default, so a draw binds only a few
// of them: CommandList::useTextures names the ones the next draws read, which group 2 binds (2 2D, 1 cube, 1 depth texture) with
// their ids in a small uniform buffer (Bindless.hlsli g_directIds). 12 pools + 4 = 16
constexpr u32 kDirectIds = 256;
constexpr u32 kBound2D = 2;         // group 2 bindings 0, 1 (g_direct0/1)
constexpr u32 kBoundCubes = 1;      // binding 2 (g_directCube)
constexpr u32 kBoundDepths = 1;     // binding 3 (g_directDepth: shadow maps, texture_depth_2d)
constexpr u32 kBoundTextures = kBound2D + kBoundCubes + kBoundDepths;
constexpr u32 kDirectBindings = kBoundTextures + 1; // + binding 4: the bound ids (uniform buffer)
constexpr u32 kDirectPool = 31;     // index bits 23..19 for a direct slot (pools are 0..kPools-1)
// Texture array pools: pool = format * kPoolClasses + size class (Bindless.hlsli g_pool*). Formats in poolFormat order
constexpr u32 kPoolFormats = 5;     // Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb, Bc5Unorm
constexpr u32 kPoolClasses = 2;
constexpr u32 kPools = kPoolFormats * kPoolClasses;
constexpr u32 kPoolClassLog2[kPoolClasses] = {9, 11}; // 512 and 2048 (Bindless.hlsli mkPoolSize)
constexpr u32 kPoolFirstLayers[kPoolClasses] = {16, 4}; // grows ×2 when full (a 2048 BC7 layer with mips is ~5.6 MB)
constexpr u32 kPoolMaxLayers = 256;     // index bits 18..11
constexpr u32 kPoolMaxBias = 7;         // index bits 10..8
// Cube pools (Rgb9e5 texture_cube_array, the same two size classes) come after the 2D pools: pool kPools + class
constexpr u32 kCubePools = kPoolClasses;
constexpr u32 kAllPools = kPools + kCubePools;
constexpr u32 kCubePoolFirstLayers[kPoolClasses] = {4, 1}; // cubes (a 2048 Rgb9e5 cube with mips is ~134 MB)
// Compute group 3 (Compute.hlsli): storage buffers at bindings 0..2, then storage textures by format (rgba8unorm ×2, rgba16float,
// r32float: WGSL types storage textures by format). Storage texture index = kStorageFirst[format] + slot. Sized to WebGPU's
// default limits: 4 storage textures and 8 storage buffers per stage (ring + 3 for reading + 3 for writing)
constexpr u32 kStorageBuffers = 3;
constexpr u32 kStorageFormats = 3;
constexpr u32 kStoragePerFormat[kStorageFormats] = {2, 1, 1};
constexpr u32 kStorageFirst[kStorageFormats] = {0, 2, 3};
constexpr u32 kStorageTextures = 4;
constexpr u32 kStorageBindings = kStorageBuffers + kStorageTextures;
// Group 0 also holds the storage buffers for reading (bindings 5..7, Bindless.hlsli mkBufferLoad): bufferIndex = 1 + slot, 0 = ring
constexpr u32 kGroup0Bindings = 4 + kStorageBuffers;
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

    // Group 0: upload ring + samplers. Group 1: texture array pools. Group 2: bound direct textures. 64 bytes of immediates
    wgpu::Sampler linearClamp, linearWrap, shadowCompare;
    wgpu::BindGroupLayout layout0, layout1, layout2, layout3;
    wgpu::PipelineLayout pipelineLayout, computeLayout; // compute: groups 0..2 like graphics + group 3
    bool float32Filterable = false; // R32Float storage textures can also be read through a direct slot
    wgpu::BindGroup group0, group1;
    // Direct ids (Texture::id per id, 0 = free), the textures useTextures named (Texture::id per group 2 binding, 0 = none) and the
    // group 2 variants built so far (with their id buffers), cleared when a direct texture goes away
    std::array<u32, kDirectIds> directOwner{};
    std::array<u32, kBoundTextures> bound{};
    struct Group2Key {
        std::array<u32, kBoundTextures> bound{};
        u32 exclude = 0;
        bool skipUnordered = false;
        bool operator==(const Group2Key&) const = default;
    };
    struct Group2Entry {
        Group2Key key;
        wgpu::BindGroup group;
        wgpu::Buffer ids;
    };
    std::vector<Group2Entry> group2Cache;
    wgpu::Texture dummy2D, dummyCube, dummyDepth, dummyArray;
    wgpu::TextureView dummy2DView, dummyCubeView, dummyDepthView, dummyArrayView;

    // Pools are created with their first texture (an empty pool binds dummyArray). Layers aren't freed (textures live until shutdown)
    struct Pool {
        wgpu::Texture texture;
        wgpu::TextureView view;
        u32 layers = 0, used = 0;
    };
    std::array<Pool, kAllPools> pools;
    wgpu::TextureView dummyCubeArrayView;

    // Upload ring: written on the CPU, sent with queue.WriteBuffer before the frame's submit (WebGPU has no persistent mapping)
    wgpu::Buffer ring;
    std::vector<u8> ringCpu;
    u64 uploadStart = 0, uploadOffset = 0, uploadLimit = 0;

    struct BufferEntry {
        wgpu::Buffer buffer;
        BufferUsage usage = BufferUsage::Vertex;
        bool storage = false;
        u32 storageSlot = 0;
        u64 bytes = 0;
        bool unordered = false; // BufferState::UnorderedAccess: bound for writing in group 3, otherwise for reading in group 0
    };
    std::vector<BufferEntry> buffers;
    struct TextureEntry {
        wgpu::Texture texture;
        wgpu::TextureView view, storageView;
        u32 slot = 0;      // direct id, or the layer of a pooled texture
        u32 index = 0;     // textureIndex() (pooled textures: pool, layer, mip bias, last mip)
        bool pooled = false;
        bool cube = false;
        bool depth = false; // depth texture: the depth binding (shadow maps, read only through mkShadowGather)
        bool storage = false; // storage texture (compute): also in storageTextureSlots
        u32 storageSlot = 0;  // storage texture index (kStorageFirst[format] + slot)
        bool unordered = false; // in TextureState::UnorderedAccess: bound for writing in group 3, left out of group 2 in dispatches
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
    std::vector<wgpu::RenderPipeline> pipelines;         // per Pipeline handle: one of the two is set
    std::vector<wgpu::ComputePipeline> computePipelines;
    // Storage slots (Buffer::id / Texture::id, 0 = empty) and empty-slot stand-ins; dispatch bind groups rebuilt after a change
    std::array<u32, kStorageBuffers> storageBufferSlots{};
    std::array<u32, kStorageTextures> storageTextureSlots{};
    std::array<wgpu::Buffer, kStorageBuffers> dummyStorage; // one per slot: writable bindings of one buffer may not alias
    std::array<wgpu::Buffer, kStorageBuffers> dummyRead;    // group 0 read slots (separate: a buffer can't be read and written in one dispatch)
    std::array<wgpu::TextureView, kStorageTextures> dummyStorageViews; // one per slot, in the slot's format (no aliasing either)
    wgpu::BindGroup computeGroup3;
    bool computeGroupsDirty = true;

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
        WGPUBindGroup group0 = nullptr, group1 = nullptr, group2 = nullptr;
        f32 viewport[4] = {-1, -1, -1, -1};
        bool push = false;
    } applied;

    // Pipeline cache (DeviceDesc::pipelineCache): Dawn's key/value blobs (compiled shaders and driver pipeline data), kept in memory,
    // read from <folder>/pipelines.webgpu on init and written on shutdown when something was added. Dawn may call from its own threads
    struct PipelineCache {
        std::mutex lock;
        std::unordered_map<std::string, std::string> entries;
        std::string path;
        bool dirty = false;
    } cache;
    void loadCache(const std::string& folder);
    void saveCache();

    // Debug UI drawing (WgpuImGui.cpp, MKRHI_IMGUI_TARGET builds): created by imguiInit
    struct ImGuiState;
    ImGuiState* imgui = nullptr;

    u32 width = 0, height = 0;
    GpuInfo gpu;
    GpuTimes gpuTimes;
    // GPU timers (timestamp queries, optional feature). WebGPU writes timestamps only at the start and end of passes
    // (timestampWrites), so marks attach to passes: the frame start and timer begins to the start of the next pass, timer ends and the
    // frame end to the end of the current or last pass. Marks name "physical" queries of this frame (up to kQueriesPerFrame), resolved
    // and copied to a readback buffer at endFrame and read when that frame index comes around again (kFramesInFlight late)
    static constexpr u32 kQueriesPerFrame = 128;
    static constexpr u32 kMarks = 2 + 2 * kMaxGpuTimers; // frame start, frame end, then begin/end per timer
    static constexpr u32 kNoQuery = ~0u;
    bool timestamps = false;
    wgpu::QuerySet querySet;
    wgpu::Buffer queryResolve;
    struct TimerFrame {
        wgpu::Buffer readback;
        wgpu::Future mapped{};
        bool pending = false;
        u32 queries = 0;                     // physical queries used
        std::array<u32, kMarks> marks{};     // physical query of each mark (kNoQuery = not written)
        u32 pendingBegins = 0;               // marks waiting for the next pass start (bits)
        u32 passEnd = kNoQuery;              // physical query at the end of the current or last pass
    };
    std::array<TimerFrame, kFramesInFlight> timerFrames;
    wgpu::PassTimestampWrites passTimestamps; // filled by nextPassTimestamps
    const wgpu::PassTimestampWrites* nextPassTimestamps(); // for the pass about to begin (nullptr: none)
    void readTimers(TimerFrame& frame);
    std::string initError;

    bool configureSurface();
    void endPass();
    void beginPass(const wgpu::RenderPassDescriptor& desc, u32 exclude, u32 w, u32 h);
    wgpu::BindGroup group2(u32 exclude, bool skipUnordered);
    void buildComputeGroups();
    void slotsChanged() {
        group2Cache.clear(); // also when it grows past a few hundred combinations (Device::Impl::group2)
        applied.group2 = nullptr;
        computeGroupsDirty = true;
    }
    void buildGroup0();
    void buildGroup1();
    bool group0Dirty = true; // storage buffer slots or states changed
    bool flushDraw();
    bool wait(const wgpu::Future& future, const char* what);
};

} // namespace mkrhi
