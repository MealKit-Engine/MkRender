#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// mkrhi: thin graphics API layer of MkRender (no engine headers; the host passes a native window handle and size).
// Only as much as the renderer uses: device/swap chain, frame command lists, static buffers, textures, pipelines, frame upload ring, capture.
// Its shape follows DX12 (command list recording, explicit barriers, bindless indices). One backend per build (CMake MKRHI_BACKEND: src/mkrhi/dx12/,
// src/mkrhi/vulkan/, src/mkrhi/webgpu/), with per-backend .cpp files implementing the same classes without virtual functions. Graphics API headers stay in the backend folders.
//
// Binding: every pipeline uses the same layout (shaders/Bindless.hlsli on the renderer side).
//   push constants 64 bytes (HLSL `[[vk::push_constant]] ConstantBuffer<T> : register(b0)`)
//   buffer array `ByteAddressBuffer g_buffers[] : register(t0, space1)` ([[vk::binding(0, 0)]]) — bufferIndex()
//   texture array `Texture2D g_textures[] : register(t0, space2)` ([[vk::binding(1, 0)]]) — textureIndex()
//   cube array `TextureCube g_cubes[] : register(t0, space3)` ([[vk::binding(4, 0)]]) — textureIndex() of cube textures (same index space)
//   read/write buffer array `RWByteAddressBuffer g_rwBuffers[] : register(u0, space1)` ([[vk::binding(5, 0)]]) — storageBufferIndex() (compute, G1)
//   read/write texture array `RWTexture2D<float4> g_rwTextures[] : register(u0, space2)` ([[vk::binding(6, 0)]]) — storageTextureIndex()
//   static samplers `SamplerState g_linearClamp : register(s0)` ([[vk::binding(2, 0)]]),
//              `SamplerState g_linearWrap : register(s1)` ([[vk::binding(3, 0)]], anisotropy 8, model textures)
namespace mkrhi {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// Log output (set once before Device::init; without it messages go to stderr). Fatal ends the process after the callback returns
enum class LogLevel : u8 { Info, Warn, Error, Fatal };
using LogFn = void (*)(LogLevel level, const char* message, void* user);
void setLog(LogFn fn, void* user = nullptr);

// "dx12", "vulkan" or "webgpu": the backend this library was built with
const char* backendName();

// GPU feature inspection results. Written to the startup log and `--system-info` (min-spec hardware measurement records).
struct GpuInfo {
    std::string api;           // "Direct3D 12", "Vulkan 1.3.280"
    std::string adapter;       // adapter name (UTF-8)
    std::string driverVersion; // driver version "31.0.101.5186" (empty string if unknown)
    u32 vendorId = 0;          // 0x8086 Intel, 0x1002 AMD, 0x10DE NVIDIA
    u32 deviceId = 0;
    u64 dedicatedVideoMemory = 0;
    u64 sharedSystemMemory = 0;
    u32 featureLevel = 0;        // D3D_FEATURE_LEVEL (0xC100 = 12_1). 0 for Vulkan
    u32 shaderModel = 0;         // D3D_SHADER_MODEL (0x66 = 6.6). 0 for Vulkan
    u32 resourceBindingTier = 0; // 1~3. 0 for Vulkan
    u32 meshShaderTier = 0;      // 0 = none (for reference; the engine doesn't use it)
    u32 waveLaneCountMin = 0;
    u32 waveLaneCountMax = 0;
    bool uma = false; // shares memory with the CPU like integrated graphics
    std::string unsupported; // min-spec shortfalls found by the backend (Vulkan version/features). Empty if none
};

// Why it falls short of the min spec. Empty string if it meets it.
std::string gpuMinimumError(const GpuInfo& info);
// Inspects the GPU the device would pick, without a window. false and error if there's no GPU.
bool queryGpu(GpuInfo& out, std::string* error = nullptr);

// Handles: 0 = none
struct Buffer {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};
struct Texture {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};
struct Pipeline {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};
// Offscreen render target (TargetKind decides the format; draw with pipelines whose Target matches). Depth-only targets via createDepthTarget
struct RenderTarget {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
};

// Texture formats. Srgb converts to linear on read (color textures). BC is 4×4 block compression (BC7 = color 16 bytes, BC5 = two channels 16 bytes)
// Rgb9e5: HDR (shared exponent, 4 bytes. Sky/reflection cubemaps). Rgba16Float and R32Float are for storage textures (compute)
enum class Format : u8 { R8Unorm, Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb, Bc5Unorm, Rgb9e5, Rgba16Float, R32Float };

inline bool isBlockCompressed(Format f) { return f == Format::Bc7Unorm || f == Format::Bc7Srgb || f == Format::Bc5Unorm; }
// Bytes of one mip (no gaps between rows, block rows for BC)
inline u64 textureMipBytes(Format f, u32 width, u32 height) {
    if (isBlockCompressed(f)) return static_cast<u64>((width + 3) / 4) * ((height + 3) / 4) * 16;
    return static_cast<u64>(width) * height * (f == Format::R8Unorm ? 1u : f == Format::Rgba16Float ? 8u : 4u);
}

// Texture with contents (uploaded once on load, waits for the GPU). data is textureMipBytes each, in order from mip 0.
// For cubes, six faces (+X, -X, +Y, -Y, +Z, -Z) in order, each from mip 0 (shaders read via g_cubes[textureIndex()])
struct TextureInit {
    u32 width = 0, height = 0;
    u32 mipLevels = 1;
    Format format = Format::Rgba8Unorm;
    const void* data = nullptr;
    bool cube = false;
};
enum class BufferUsage : u8 { Vertex, Index16, Index32 };
// UnorderedAccess: read/write from compute (storage textures only)
enum class TextureState : u8 { ShaderRead, CopyDst, UnorderedAccess };
// Storage buffers: ShaderRead = read through g_buffers[], UnorderedAccess = read/write through g_rwBuffers[]
enum class BufferState : u8 { ShaderRead, UnorderedAccess };

// Vertex attributes (in HLSL input order. DX12 semantic names, Vulkan location = order)
enum class VertexFormat : u8 { Float3, Unorm8x4, Half2, Uint16x2, Uint8x4, Snorm16x4, Snorm16x2 };
struct VertexAttribute {
    const char* semantic = "";
    VertexFormat format = VertexFormat::Float3;
    u32 offset = 0;
};

struct ShaderCode {
    const void* data = nullptr;
    size_t size = 0;
};

// Compute pipeline: one shader with entry point CSMain (SPIR-V) / any entry (DXIL), the same binding layout as graphics pipelines
struct ComputePipelineDesc {
    ShaderCode cs;
};

// Format of the target a pipeline draws to (docs/DECISIONS.md 2026-10-27 "Post-processing")
enum class Target : u8 {
    Scene,   // HDR scene target (TargetKind::Scene): color R11G11B10F (linear) + depth D32 (reversed-Z: GREATER)
    Hdr,     // HDR color only (TargetKind::Hdr, bloom chain): R11G11B10F, no depth
    Overlay, // 8-bit color, no depth: the back buffer and TargetKind::Ldr. No gamma conversion, so shaders write sRGB values (post/UI)
    Shadow,  // depth-only target (createDepthTarget, shadow maps): no color, depth D32 (reversed-Z: GREATER). Pixel shaders only discard (alpha mask)
};

// Kinds of offscreen targets. The color is also a Texture shaders read (renderTargetTexture)
enum class TargetKind : u8 {
    Scene, // scene (Target::Scene): HDR color + depth
    Hdr,   // HDR color only (Target::Hdr)
    Ldr,   // 8-bit color only (Target::Overlay, same format as the back buffer): sRGB values read as is (debug UI/capture)
};

struct PipelineDesc {
    ShaderCode vs, ps;
    std::vector<VertexAttribute> vertexAttributes; // empty = no vertex buffer (SV_VertexID)
    u32 vertexStride = 0;
    Target target = Target::Scene;
    bool cullBack = true;       // clockwise is the front face (DX12 default)
    bool cullFront = false;     // discards front faces (inverted hull of outlines). Takes precedence over cullBack
    bool premultipliedBlend = false;
    // For editor view modes (render::DebugView): draw as lines, additive blend (overlap count = brightness), depth test/write off
    bool wireframe = false; // Vulkan draws filled faces if fillModeNonSolid is unavailable
    bool additiveBlend = false;
    bool depthTest = true; // Scene targets only
    bool depthWrite = true; // doesn't write even when depth testing (translucent particles)
};

// Linux (X11) window for DeviceDesc::window: the host's Display* and Window, without Xlib headers here
struct XlibWindow {
    void* display = nullptr;
    unsigned long window = 0;
};

// Browser (Emscripten, WebGPU backend) window for DeviceDesc::window: the canvas element's CSS selector. The canvas size is the
// host's (DeviceDesc width/height, resize); endFrame waits for the browser's next animation frame instead of presenting
struct WebCanvas {
    const char* selector = "#canvas";
};

struct DeviceDesc {
    void* window = nullptr; // native window handle: HWND on Windows, XlibWindow* on Linux (X11), WebCanvas* in a browser
    u32 width = 0;
    u32 height = 0;
    bool debug = false; // validation layer (DX12 debug layer, Vulkan validation)
    // Called by init right before the device is created (GPU capture tools such as RenderDoc/PIX must be loaded by then). dx12: true on the DX12 backend
    void (*beforeDeviceCreate)(bool dx12) = nullptr;
    std::string pipelineCache; // folder of the compiled pipelines file (rhi/PipelineCache.h, one per backend), read on init and written on shutdown. Empty = off
};

// Captured back buffer (RGBA8, in row order). If source is given, reads that offscreen target (TargetKind::Ldr) instead of the back buffer
// (reflection probe baking, values are the written sRGB as is). The target must have finished through endTarget this frame
struct Capture {
    RenderTarget source;
    u32 width = 0, height = 0;
    std::vector<u8> rgba;
};

// One texel read at the end of the frame (pixel inspector). source = none reads the back buffer, else the color of an offscreen target
// (it must have finished through endTarget this frame). texel is the 4 bytes as stored: R11G11B10F for TargetKind::Scene/Hdr,
// RGBA8 (R first, the backend swizzles a BGRA swap chain) for TargetKind::Ldr and the back buffer. valid is false if the read failed
// (outside the image, or the back buffer holds nothing this frame). Skipped on a frame that also has a Capture
struct PixelRead {
    RenderTarget source;
    u32 x = 0, y = 0;
    bool valid = false;
    u8 texel[4] = {};
};
inline constexpr u32 kMaxPixelReads = 4;

// GPU time (timestamp queries). The meaning of section numbers (0..kMaxGpuTimers-1) is decided by the user (renderer passes).
// Results are from the most recent frame the GPU finished (kFramesInFlight frames late). Sections not used this frame are 0.
inline constexpr u32 kMaxGpuTimers = 8;
struct GpuTimes {
    bool valid = false; // timestamps are supported and at least one frame has finished
    f32 frameMs = 0.0f; // from the start to the end of the command list
    f32 timerMs[kMaxGpuTimers] = {};
};

// GPU memory: the budget the driver gives this process and current usage (DX12 QueryVideoMemoryInfo, Vulkan VK_EXT_memory_budget).
// Integrated graphics (UMA) share system memory, so the budget is based on shared memory. 0 if unknown
struct GpuMemory {
    u64 used = 0;
    u64 budget = 0;
};

class Device;

// One frame's command list (between Device::beginFrame and endFrame).
// Draw targets (Scene/Overlay) are bound with begin*, and copies (copyToTexture) unbind the bound target: call begin* again after copying.
class CommandList {
public:
    // Binds the back buffer (Target::Overlay, no gamma conversion, not cleared. If first in the frame the contents are undefined: post covers them)
    void beginOverlay();
    // Binds an offscreen target. If clearColor is nullptr, color isn't cleared and contents are kept (post passes, bloom additive blend).
    // Depth is always cleared to clearDepth. When done, endTarget returns it to the shader-readable state.
    // To draw to the back buffer after that, call beginOverlay again. Depth-only targets don't use clearColor
    void beginTarget(RenderTarget target, const f32 clearColor[4], f32 clearDepth);
    void endTarget(RenderTarget target);
    void setViewport(f32 x, f32 y, f32 width, f32 height); // back buffer pixels, top-left origin (scissor too)
    // Graphics or compute pipeline (the kind it was created as). A compute pipeline ends the bound target like copies do
    void setPipeline(Pipeline pipeline);
    void pushConstants(const void* data, u32 bytes); // up to 64 bytes, used by the next draw or dispatch of the current pipeline's kind
    void setVertexBuffer(Buffer buffer, u32 stride);
    void setIndexBuffer(Buffer buffer);
    void drawIndexed(u32 indexCount, u32 instanceCount);
    void draw(u32 vertexCount, u32 instanceCount);
    void textureBarrier(Texture texture, TextureState before, TextureState after);
    // Textures (textureIndex values; kNoTexture-like ~0u and pooled ones are skipped) the next draws and dispatches may read that a
    // WebGPU texture pool doesn't hold: render targets, storage textures, other formats and sizes. WebGPU binds at most two 2D
    // textures, one cube and one depth texture of these at a time (browsers allow 16 sampled textures per shader stage); a shader
    // reading another one gets the wrong texture. Stays until the next call. DX12/Vulkan read every texture and ignore it
    void useTextures(const u32* indices, u32 count);
    void useTextures(std::initializer_list<u32> indices) { useTextures(indices.begin(), static_cast<u32>(indices.size())); }
    // Compute (G1). dispatch runs the current compute pipeline with groupsX*Y*Z thread groups (it ends the bound target).
    // bufferBarrier moves a storage buffer between read (g_buffers[]) and read/write (g_rwBuffers[]) use; uavBarrier makes earlier
    // compute writes visible to later reads/writes of the same state (dispatch → dispatch on the same buffer or texture)
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ);
    void bufferBarrier(Buffer buffer, BufferState before, BufferState after);
    void uavBarrier();
    // Copies from offset in the upload ring (Device::allocUpload) to a texture region. rowPitch = Device::copyPitch
    void copyToTexture(Texture texture, u32 x, u32 y, u32 width, u32 height, u64 uploadOffset, u32 rowPitch);
    // GPU time sections (Device::gpuTimes). Once per section per frame, begin then end
    void beginTimer(u32 timer);
    void endTimer(u32 timer);

private:
    friend class Device;
    Device* m_device = nullptr;
};

class Device {
public:
    static constexpr u32 kFramesInFlight = 2;
    static constexpr u32 kPushConstantBytes = 64;

    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool init(const DeviceDesc& desc);
    const std::string& initError() const;
    void shutdown();
    void resize(u32 width, u32 height);
    u32 width() const;
    u32 height() const;
    const GpuInfo& gpu() const;

    // Static buffer: created in GPU memory with data copied in (at load time, waits for the GPU)
    Buffer createBuffer(BufferUsage usage, const void* data, u64 bytes);
    // Waits for the GPU and deletes (outside a frame: terrain chunks rebuilt by editor brushes). Handle numbers aren't reused
    void destroyBuffer(Buffer buffer);
    // Storage buffer for compute (G1): zero-filled, 4-byte aligned size, initial state ShaderRead. bufferIndex = g_buffers[] slot (read),
    // storageBufferIndex = g_rwBuffers[] slot (RWByteAddressBuffer). Freed with destroyBuffer
    Buffer createStorageBuffer(u64 bytes);
    u32 bufferIndex(Buffer buffer) const;
    u32 storageBufferIndex(Buffer buffer) const;
    // Copies bytes from offset to dst. Outside a frame, waits for the GPU; the buffer must be in the ShaderRead state
    bool readBuffer(Buffer buffer, u64 offset, void* dst, u64 bytes);
    // Texture compute can write (G1): zero contents, initial state ShaderRead. textureIndex = g_textures[] slot (read when in ShaderRead),
    // storageTextureIndex = g_rwTextures[] slot (RWTexture2D<float4>, read/write when in UnorderedAccess). Formats Rgba8Unorm, Rgba16Float, R32Float
    Texture createStorageTexture(u32 width, u32 height, Format format);
    u32 storageTextureIndex(Texture texture) const;
    // Texture read by shaders. Initial state ShaderRead (contents 0)
    Texture createTexture(u32 width, u32 height, Format format);
    // Static texture with mips (model texture cache). State ShaderRead
    Texture createTexture(const TextureInit& init);
    u32 textureIndex(Texture texture) const; // g_textures[] index
    // Offscreen target (once at creation, waits for the GPU). Targets that follow the window size are recreated after destroyRenderTarget
    RenderTarget createRenderTarget(u32 width, u32 height, TargetKind kind);
    // Waits for the GPU and deletes (outside a frame. The renderer's window-size targets). Handle numbers aren't reused
    void destroyRenderTarget(RenderTarget target);
    // Depth-only offscreen target (shadow maps, Target::Shadow pipelines). renderTargetTexture reads that depth as R32 float
    RenderTarget createDepthTarget(u32 width, u32 height);
    Texture renderTargetTexture(RenderTarget target) const; // color (depth for depth-only targets, initial state ShaderRead)
    Pipeline createPipeline(const PipelineDesc& desc);
    Pipeline createComputePipeline(const ComputePipelineDesc& desc);
    // Replaces the pipeline of the same handle with a new desc (shader hot reload). Waits for the GPU, so call only outside a frame
    bool recreatePipeline(Pipeline pipeline, const PipelineDesc& desc);
    bool recreatePipeline(Pipeline pipeline, const ComputePipelineDesc& desc);

    // Frame: if false, this frame isn't drawn (minimized, swap chain recreated, etc.)
    bool beginFrame();
    CommandList& cmd();
    // Carves from this frame's upload ring (CPU write, GPU read). nullptr if out of space. offset is the byte position in the ring buffer
    u8* allocUpload(u64 bytes, u32 alignment, u64* offset);
    u32 uploadBufferIndex() const; // g_buffers[] index of the upload ring
    u32 copyPitch(u32 width, u32 bytesPerPixel) const; // copyToTexture row pitch
    u32 copyAlignment() const;                         // copyToTexture start offset alignment
    // Submits and presents. If capture is given, waits for the GPU and reads the back buffer
    void endFrame(bool vsync, Capture* capture = nullptr, PixelRead* reads = nullptr, u32 readCount = 0);
    void waitIdle();

    const GpuTimes& gpuTimes() const;
    GpuMemory gpuMemory() const;

    struct Impl;
    Impl* impl() const { return m; } // backend files (src/mkrhi/<api>/) only

private:
    Impl* m = nullptr;
};

// Debug UI (Dear ImGui) drawing backend. Only when built with CMake MKRHI_IMGUI_TARGET (src/mkrhi/<api>/*ImGui.cpp).
bool imguiInit(Device& device);
void imguiShutdown(Device& device);
void imguiNewFrame();
void imguiRender(Device& device); // draws ImGui::GetDrawData() into the current command list (after beginOverlay)
// ImTextureID value for showing a device texture with ImGui::Image (DX12: GPU descriptor, Vulkan: debug UI descriptor set, created on first call)
u64 imguiTextureId(Device& device, Texture texture);

} // namespace mkrhi
