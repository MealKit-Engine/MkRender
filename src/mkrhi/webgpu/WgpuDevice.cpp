#include "Wgpu.h"

#include "mkrhi/Log.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace mkrhi {
namespace {

std::string text(wgpu::StringView s) { return s.data ? std::string(s.data, s.length == WGPU_STRLEN ? std::strlen(s.data) : s.length) : std::string(); }

u32 align(u32 v, u32 a) { return (v + a - 1) / a * a; }

wgpu::TextureFormat toWgpu(Format f) {
    switch (f) {
    case Format::R8Unorm: return wgpu::TextureFormat::R8Unorm;
    case Format::Rgba8Srgb: return wgpu::TextureFormat::RGBA8UnormSrgb;
    case Format::Bc7Unorm: return wgpu::TextureFormat::BC7RGBAUnorm;
    case Format::Bc7Srgb: return wgpu::TextureFormat::BC7RGBAUnormSrgb;
    case Format::Bc5Unorm: return wgpu::TextureFormat::BC5RGUnorm;
    case Format::Rgb9e5: return wgpu::TextureFormat::RGB9E5Ufloat;
    case Format::Rgba16Float: return wgpu::TextureFormat::RGBA16Float;
    case Format::R32Float: return wgpu::TextureFormat::R32Float;
    default: return wgpu::TextureFormat::RGBA8Unorm;
    }
}

wgpu::VertexFormat toWgpu(VertexFormat f) {
    switch (f) {
    case VertexFormat::Float3: return wgpu::VertexFormat::Float32x3;
    case VertexFormat::Half2: return wgpu::VertexFormat::Float16x2;
    case VertexFormat::Uint16x2: return wgpu::VertexFormat::Uint16x2;
    case VertexFormat::Uint8x4: return wgpu::VertexFormat::Uint8x4;
    case VertexFormat::Snorm16x4: return wgpu::VertexFormat::Snorm16x4;
    case VertexFormat::Snorm16x2: return wgpu::VertexFormat::Snorm16x2;
    default: return wgpu::VertexFormat::Unorm8x4;
    }
}

// Index the shaders decode (Bindless.hlsli mkDirectSlot): pool 31 in bits 23..19, the slot in bits 18..11
u32 directIndex(u32 slot) { return (kDirectPool << 19) | (slot << 11); }

wgpu::Instance createInstance() {
    static const wgpu::InstanceFeatureName kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor desc;
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = &kTimedWaitAny;
    return wgpu::CreateInstance(&desc);
}

wgpu::Adapter requestAdapter(const wgpu::Instance& instance, const wgpu::Surface& surface, std::string* error) {
    wgpu::RequestAdapterOptions options;
    options.powerPreference = wgpu::PowerPreference::HighPerformance;
    options.compatibleSurface = surface;
    wgpu::Adapter adapter;
    instance.WaitAny(instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView message) {
                                                 if (status == wgpu::RequestAdapterStatus::Success) adapter = std::move(a);
                                                 else if (error) *error = text(message);
                                             }),
                     UINT64_MAX);
    return adapter;
}

const char* backendText(wgpu::BackendType t) {
    switch (t) {
    case wgpu::BackendType::Vulkan: return "Vulkan";
    case wgpu::BackendType::D3D12: return "D3D12";
    case wgpu::BackendType::Metal: return "Metal";
    case wgpu::BackendType::OpenGL: return "OpenGL";
    case wgpu::BackendType::OpenGLES: return "OpenGL ES";
    default: return "?";
    }
}

// Features and limits the renderer needs (the min spec); what's missing as a sentence
constexpr u32 kSampledTexturesNeeded = kDirectBindings;
std::string missingFeatures(const wgpu::Adapter& adapter) {
    std::string missing;
    auto need = [&](bool ok, const char* what) {
        if (ok) return;
        if (!missing.empty()) missing += ", ";
        missing += what;
    };
    wgpu::Limits limits;
    adapter.GetLimits(&limits);
    need(adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC), "BC texture compression");
    need(adapter.HasFeature(wgpu::FeatureName::RG11B10UfloatRenderable), "RG11B10 render targets");
    need(limits.maxImmediateSize >= Device::kPushConstantBytes, "64 bytes of immediates");
    need(limits.maxSampledTexturesPerShaderStage >= kSampledTexturesNeeded, "24 sampled textures per shader stage");
    return missing;
}

void describeGpu(const wgpu::Adapter& adapter, GpuInfo& info) {
    wgpu::AdapterInfo a;
    adapter.GetInfo(&a);
    info.api = std::string("WebGPU (Dawn, ") + backendText(a.backendType) + ")";
    info.adapter = text(a.device);
    info.driverVersion = text(a.description);
    info.vendorId = a.vendorID;
    info.deviceId = a.deviceID;
    info.uma = a.adapterType == wgpu::AdapterType::IntegratedGPU;
    info.waveLaneCountMin = a.subgroupMinSize;
    info.waveLaneCountMax = a.subgroupMaxSize;
    const std::string missing = missingFeatures(adapter);
    info.unsupported = missing.empty() ? std::string() : info.adapter + " lacks " + missing + " (WebGPU)";
}

} // namespace

// ---------------------------------------------------------------- Impl

bool Device::Impl::wait(const wgpu::Future& future, const char* what) {
    constexpr int kTries = 5; // 1 s each, logged (like the Vulkan backend's fence waits)
    for (int i = 0; i < kTries; ++i) {
        const wgpu::WaitStatus status = instance.WaitAny(future, 1'000'000'000ull);
        if (status == wgpu::WaitStatus::Success) return true;
        if (status != wgpu::WaitStatus::TimedOut) break;
        MKRHI_WARN("[WebGPU] %s: not done after %d s", what, i + 1);
    }
    return false;
}

bool Device::Impl::configureSurface() {
    if (width == 0 || height == 0) return false;
    wgpu::SurfaceConfiguration config;
    config.device = device;
    config.format = swapFormat;
    config.usage = wgpu::TextureUsage::RenderAttachment | (swapCopySrc ? wgpu::TextureUsage::CopySrc : wgpu::TextureUsage::None);
    config.width = width;
    config.height = height;
    config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    // Vsync off: tearing (Immediate), otherwise Mailbox, like the other backends
    config.presentMode = vsync                ? wgpu::PresentMode::Fifo
                         : immediateSupported ? wgpu::PresentMode::Immediate
                         : mailboxSupported   ? wgpu::PresentMode::Mailbox
                                              : wgpu::PresentMode::Fifo;
    surface.Configure(&config);
    surfaceDirty = false;
    return true;
}

void Device::Impl::endPass() {
    if (!pass) return;
    pass.End();
    pass = nullptr;
    passExclude = 0;
}

void Device::Impl::beginPass(const wgpu::RenderPassDescriptor& desc, u32 exclude, u32 w, u32 h) {
    endPass();
    pass = encoder.BeginRenderPass(&desc);
    passExclude = exclude;
    passWidth = w;
    passHeight = h;
    applied = {};
}

wgpu::BindGroup Device::Impl::group2(u32 exclude) {
    for (const auto& [key, group] : group2Cache)
        if (key == exclude) return group;
    wgpu::BindGroupEntry entries[kDirectBindings];
    for (u32 i = 0; i < kDirectBindings; ++i) {
        const bool cube = i >= kDirectTextures && i < kDirectTextures + kDirectCubes, depth = i >= kDirectTextures + kDirectCubes;
        const u32 id = depth ? depthSlots[i - kDirectTextures - kDirectCubes] : cube ? cubeSlots[i - kDirectTextures] : slots[i];
        entries[i].binding = i;
        entries[i].textureView = id && id != exclude ? textures[id - 1].view : depth ? dummyDepthView : cube ? dummyCubeView : dummy2DView;
    }
    wgpu::BindGroupDescriptor desc;
    desc.layout = layout2;
    desc.entryCount = kDirectBindings;
    desc.entries = entries;
    wgpu::BindGroup group = device.CreateBindGroup(&desc);
    group2Cache.emplace_back(exclude, group);
    return group;
}

bool Device::Impl::flushDraw() {
    if (!pass || !pipeline) return false;
    if (applied.pipeline != pipeline) {
        pass.SetPipeline(pipelines[pipeline - 1]);
        if (!applied.pipeline) {
            pass.SetBindGroup(0, group0);
            pass.SetBindGroup(1, group1);
        }
        applied.pipeline = pipeline;
    }
    const wgpu::BindGroup g2 = group2(passExclude);
    if (applied.group2 != g2.Get()) {
        pass.SetBindGroup(2, g2);
        applied.group2 = g2.Get();
    }
    if (std::memcmp(applied.viewport, viewport, sizeof(viewport)) != 0) {
        // The scissor has to stay inside the attachment (WebGPU validates it); the viewport is the renderer's
        const u32 x = static_cast<u32>(std::clamp(viewport[0], 0.0f, static_cast<f32>(passWidth)));
        const u32 y = static_cast<u32>(std::clamp(viewport[1], 0.0f, static_cast<f32>(passHeight)));
        const u32 w = static_cast<u32>(std::clamp(viewport[0] + viewport[2], 0.0f, static_cast<f32>(passWidth))) - x;
        const u32 h = static_cast<u32>(std::clamp(viewport[1] + viewport[3], 0.0f, static_cast<f32>(passHeight))) - y;
        pass.SetViewport(viewport[0], viewport[1], viewport[2], viewport[3], 0.0f, 1.0f);
        pass.SetScissorRect(x, y, w, h);
        std::memcpy(applied.viewport, viewport, sizeof(viewport));
    }
    if (!applied.push && pushBytes) {
        pass.SetImmediates(0, push, align(pushBytes, 4));
        applied.push = true;
    }
    if (vertexBuffer && applied.vertexBuffer != vertexBuffer) {
        pass.SetVertexBuffer(0, buffers[vertexBuffer - 1].buffer);
        applied.vertexBuffer = vertexBuffer;
    }
    if (indexBuffer && applied.indexBuffer != indexBuffer) {
        const BufferEntry& b = buffers[indexBuffer - 1];
        pass.SetIndexBuffer(b.buffer, b.usage == BufferUsage::Index16 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32);
        applied.indexBuffer = indexBuffer;
    }
    return true;
}

// ---------------------------------------------------------------- Device

Device::~Device() { shutdown(); }

bool Device::init(const DeviceDesc& desc) {
    if (desc.beforeDeviceCreate) desc.beforeDeviceCreate(false);
    m = new Impl();
    m->width = desc.width;
    m->height = desc.height;
    m->cmd.m_device = this;
    auto fail = [&](const std::string& why) {
        m->initError = why;
        MKRHI_ERROR("%s", why.c_str());
        return false;
    };

    m->instance = createInstance();
    if (!m->instance) return fail("WebGPU instance creation failed");
#if defined(__linux__)
    const auto* xlib = static_cast<const XlibWindow*>(desc.window);
    if (!xlib || !xlib->display) return fail("WebGPU needs an X11 window (XlibWindow)");
    wgpu::SurfaceSourceXlibWindow source;
    source.display = xlib->display;
    source.window = xlib->window;
    wgpu::SurfaceDescriptor surfaceDesc;
    surfaceDesc.nextInChain = &source;
    m->surface = m->instance.CreateSurface(&surfaceDesc);
#else
    return fail("WebGPU surfaces aren't implemented on this OS yet");
#endif
    if (!m->surface) return fail("WebGPU surface creation failed");

    std::string adapterError;
    m->adapter = requestAdapter(m->instance, m->surface, &adapterError);
    if (!m->adapter) return fail("No WebGPU adapter found" + (adapterError.empty() ? std::string() : " (" + adapterError + ")"));
    describeGpu(m->adapter, m->gpu);
    const GpuInfo& gpu = m->gpu;
    MKRHI_INFO("GPU: %s | driver %s | %s | %s memory", gpu.adapter.c_str(), gpu.driverVersion.empty() ? "?" : gpu.driverVersion.c_str(),
               gpu.api.c_str(), gpu.uma ? "unified (UMA)" : "dedicated");
    if (!gpu.unsupported.empty()) return fail(gpu.unsupported);

    // Device: only the features and limits the renderer uses (the rest stay at WebGPU's defaults, like in a browser)
    const wgpu::FeatureName features[] = {wgpu::FeatureName::TextureCompressionBC, wgpu::FeatureName::RG11B10UfloatRenderable};
    wgpu::Limits limits;
    limits.maxImmediateSize = kPushConstantBytes;
    limits.maxSampledTexturesPerShaderStage = kSampledTexturesNeeded;
    wgpu::DeviceDescriptor deviceDesc;
    deviceDesc.requiredFeatureCount = sizeof(features) / sizeof(features[0]);
    deviceDesc.requiredFeatures = features;
    deviceDesc.requiredLimits = &limits;
    deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        MKRHI_ERROR("[WebGPU] %s error: %s", type == wgpu::ErrorType::Validation ? "validation" : "device", text(message).c_str());
    });
    deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
                                     [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                                         if (reason != wgpu::DeviceLostReason::Destroyed)
                                             MKRHI_ERROR("[WebGPU] device lost: %s", text(message).c_str());
                                     });
    std::string deviceError;
    m->instance.WaitAny(m->adapter.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
                                                 [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message) {
                                                     if (status == wgpu::RequestDeviceStatus::Success) m->device = std::move(d);
                                                     else deviceError = text(message);
                                                 }),
                        UINT64_MAX);
    if (!m->device) return fail("WebGPU device creation failed: " + deviceError);
    m->queue = m->device.GetQueue();

    // Swap chain: 8-bit UNORM (post/UI write sRGB values)
    wgpu::SurfaceCapabilities caps;
    m->surface.GetCapabilities(m->adapter, &caps);
    for (wgpu::TextureFormat want : {wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureFormat::BGRA8Unorm})
        for (size_t i = 0; i < caps.formatCount && m->swapFormat == wgpu::TextureFormat::Undefined; ++i)
            if (caps.formats[i] == want) m->swapFormat = want;
    if (m->swapFormat == wgpu::TextureFormat::Undefined) return fail("No 8-bit RGBA surface format on this GPU");
    m->swapBgra = m->swapFormat == wgpu::TextureFormat::BGRA8Unorm;
    m->swapCopySrc = (caps.usages & wgpu::TextureUsage::CopySrc) != wgpu::TextureUsage::None;
    for (size_t i = 0; i < caps.presentModeCount; ++i) {
        m->immediateSupported |= caps.presentModes[i] == wgpu::PresentMode::Immediate;
        m->mailboxSupported |= caps.presentModes[i] == wgpu::PresentMode::Mailbox;
    }
    if (!m->configureSurface()) return fail("Surface configuration failed (window has no size)");

    // Samplers (Bindless.hlsli g_linearClamp / g_linearWrap)
    wgpu::SamplerDescriptor sd;
    sd.magFilter = sd.minFilter = wgpu::FilterMode::Linear;
    sd.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    sd.addressModeU = sd.addressModeV = sd.addressModeW = wgpu::AddressMode::ClampToEdge;
    m->linearClamp = m->device.CreateSampler(&sd);
    sd.addressModeU = sd.addressModeV = sd.addressModeW = wgpu::AddressMode::Repeat;
    sd.maxAnisotropy = 8;
    m->linearWrap = m->device.CreateSampler(&sd);
    wgpu::SamplerDescriptor cd; // shadow PCF taps (Bindless.hlsli g_shadowCompare): gathers ignore filtering
    cd.addressModeU = cd.addressModeV = cd.addressModeW = wgpu::AddressMode::ClampToEdge;
    cd.compare = wgpu::CompareFunction::GreaterEqual;
    m->shadowCompare = m->device.CreateSampler(&cd);

    // Layouts: one for every pipeline, like the other backends (a pipeline may use fewer bindings)
    const wgpu::ShaderStage stages = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    wgpu::BindGroupLayoutEntry e0[4];
    e0[0].binding = 0;
    e0[0].visibility = stages;
    e0[0].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    e0[1].binding = 2;
    e0[1].visibility = stages;
    e0[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    e0[2].binding = 3;
    e0[2].visibility = stages;
    e0[2].sampler.type = wgpu::SamplerBindingType::Filtering;
    e0[3].binding = 4;
    e0[3].visibility = stages;
    e0[3].sampler.type = wgpu::SamplerBindingType::Comparison;
    wgpu::BindGroupLayoutDescriptor ld;
    ld.entryCount = 4;
    ld.entries = e0;
    m->layout0 = m->device.CreateBindGroupLayout(&ld);
    ld.entryCount = 0;
    ld.entries = nullptr;
    m->layout1 = m->device.CreateBindGroupLayout(&ld);
    wgpu::BindGroupLayoutEntry e2[kDirectBindings];
    for (u32 i = 0; i < kDirectBindings; ++i) {
        const bool cube = i >= kDirectTextures && i < kDirectTextures + kDirectCubes, depth = i >= kDirectTextures + kDirectCubes;
        e2[i].binding = i;
        e2[i].visibility = stages;
        e2[i].texture.sampleType = depth ? wgpu::TextureSampleType::Depth : wgpu::TextureSampleType::Float;
        e2[i].texture.viewDimension = cube ? wgpu::TextureViewDimension::Cube : wgpu::TextureViewDimension::e2D;
    }
    ld.entryCount = kDirectBindings;
    ld.entries = e2;
    m->layout2 = m->device.CreateBindGroupLayout(&ld);
    const wgpu::BindGroupLayout layouts[3] = {m->layout0, m->layout1, m->layout2};
    wgpu::PipelineLayoutDescriptor pld;
    pld.bindGroupLayoutCount = 3;
    pld.bindGroupLayouts = layouts;
    pld.immediateSize = kPushConstantBytes;
    m->pipelineLayout = m->device.CreatePipelineLayout(&pld);

    // Upload ring (two frame sections) and group 0
    wgpu::BufferDescriptor bd;
    bd.size = kUploadBytesPerFrame * kFramesInFlight;
    bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
    m->ring = m->device.CreateBuffer(&bd);
    m->ringCpu.resize(bd.size);
    wgpu::BindGroupEntry g0[4];
    g0[0].binding = 0;
    g0[0].buffer = m->ring;
    g0[1].binding = 2;
    g0[1].sampler = m->linearClamp;
    g0[2].binding = 3;
    g0[2].sampler = m->linearWrap;
    g0[3].binding = 4;
    g0[3].sampler = m->shadowCompare;
    wgpu::BindGroupDescriptor gd;
    gd.layout = m->layout0;
    gd.entryCount = 4;
    gd.entries = g0;
    m->group0 = m->device.CreateBindGroup(&gd);
    gd.layout = m->layout1;
    gd.entryCount = 0;
    gd.entries = nullptr;
    m->group1 = m->device.CreateBindGroup(&gd);

    // Empty direct slots read these (1×1 zero texture, cube and depth)
    wgpu::TextureDescriptor td;
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {1, 1, 1};
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding;
    m->dummy2D = m->device.CreateTexture(&td);
    m->dummy2DView = m->dummy2D.CreateView();
    td.size = {1, 1, 6};
    m->dummyCube = m->device.CreateTexture(&td);
    wgpu::TextureViewDescriptor vd;
    vd.dimension = wgpu::TextureViewDimension::Cube;
    m->dummyCubeView = m->dummyCube.CreateView(&vd);
    td.size = {1, 1, 1};
    td.format = kDepthFormat;
    m->dummyDepth = m->device.CreateTexture(&td);
    m->dummyDepthView = m->dummyDepth.CreateView();
    return true;
}

const std::string& Device::initError() const {
    static const std::string none;
    return m ? m->initError : none;
}

void Device::shutdown() {
    if (!m) return;
    if (m->device) {
        waitIdle();
        m->surface.Unconfigure();
    }
    delete m; // handles release themselves
    m = nullptr;
}

void Device::resize(u32 width, u32 height) {
    if (!m || width == 0 || height == 0 || (width == m->width && height == m->height)) return;
    m->width = width;
    m->height = height;
    m->configureSurface();
}

u32 Device::width() const { return m ? m->width : 0; }
u32 Device::height() const { return m ? m->height : 0; }

const GpuInfo& Device::gpu() const {
    static const GpuInfo empty{};
    return m ? m->gpu : empty;
}

Buffer Device::createBuffer(BufferUsage usage, const void* data, u64 bytes) {
    Impl::BufferEntry entry;
    entry.usage = usage;
    wgpu::BufferDescriptor bd;
    bd.size = (bytes + 3) & ~u64{3};
    bd.usage = (usage == BufferUsage::Vertex ? wgpu::BufferUsage::Vertex : wgpu::BufferUsage::Index) | wgpu::BufferUsage::CopyDst;
    bd.mappedAtCreation = true;
    entry.buffer = m->device.CreateBuffer(&bd);
    std::memcpy(entry.buffer.GetMappedRange(), data, bytes);
    entry.buffer.Unmap();
    m->buffers.push_back(entry);
    return {static_cast<u32>(m->buffers.size())};
}

void Device::destroyBuffer(Buffer buffer) {
    if (!m || !buffer || buffer.id > m->buffers.size() || m->inFrame) return;
    Impl::BufferEntry& b = m->buffers[buffer.id - 1];
    if (b.buffer) b.buffer.Destroy();
    b = {};
}

// ---------------------------------------------------------------- compute resources (not on WebGPU yet)

Buffer Device::createStorageBuffer(u64) { MKRHI_FATAL("Storage buffers aren't implemented on the WebGPU backend yet"); }
u32 Device::bufferIndex(Buffer) const { return 0; }
u32 Device::storageBufferIndex(Buffer) const { return 0; }
bool Device::readBuffer(Buffer, u64, void*, u64) { return false; }
Texture Device::createStorageTexture(u32, u32, Format) { MKRHI_FATAL("Storage textures aren't implemented on the WebGPU backend yet"); }
u32 Device::storageTextureIndex(Texture) const { return 0; }
Pipeline Device::createComputePipeline(const ComputePipelineDesc&) { MKRHI_FATAL("Compute pipelines aren't implemented on the WebGPU backend yet"); }
bool Device::recreatePipeline(Pipeline, const ComputePipelineDesc&) { return false; }

// ---------------------------------------------------------------- textures

namespace {
// Direct slots of the texture's kind (2D, cube or depth)
u32* slotsOf(Device::Impl& m, bool cube, bool depth, u32* count) {
    *count = depth ? kDirectDepths : cube ? kDirectCubes : kDirectTextures;
    return depth ? m.depthSlots.data() : cube ? m.cubeSlots.data() : m.slots.data();
}

// Puts a texture into the next free direct slot of its kind. Fatal when they run out, like the bindless backends' slot exhaustion
u32 allocSlot(Device::Impl& m, u32 textureId, bool cube, bool depth = false) {
    u32 count = 0;
    u32* first = slotsOf(m, cube, depth, &count);
    for (u32 i = 0; i < count; ++i)
        if (!first[i]) {
            first[i] = textureId;
            m.slotsChanged();
            return i;
        }
    MKRHI_FATAL("WebGPU direct %s slots exhausted (%u; texture array pools are not done yet)", depth ? "depth" : cube ? "cube" : "texture",
                count);
}

void freeSlot(Device::Impl& m, const Device::Impl::TextureEntry& t) {
    u32 count = 0;
    slotsOf(m, t.cube, t.depth, &count)[t.slot] = 0;
    m.slotsChanged();
}
} // namespace

Texture Device::createTexture(u32 width, u32 height, Format format) {
    Impl::TextureEntry entry;
    wgpu::TextureDescriptor td;
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {width, height, 1};
    td.format = toWgpu(format);
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst; // WebGPU textures start zeroed
    entry.texture = m->device.CreateTexture(&td);
    entry.view = entry.texture.CreateView();
    m->textures.push_back(entry);
    const u32 id = static_cast<u32>(m->textures.size());
    m->textures.back().slot = allocSlot(*m, id, false);
    return {id};
}

Texture Device::createTexture(const TextureInit& init) {
    Impl::TextureEntry entry;
    entry.cube = init.cube;
    const u32 layers = init.cube ? 6u : 1u;
    wgpu::TextureDescriptor td;
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {init.width, init.height, layers};
    td.format = toWgpu(init.format);
    td.mipLevelCount = init.mipLevels;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    entry.texture = m->device.CreateTexture(&td);
    wgpu::TextureViewDescriptor vd;
    vd.dimension = init.cube ? wgpu::TextureViewDimension::Cube : wgpu::TextureViewDimension::e2D;
    entry.view = entry.texture.CreateView(&vd);

    // Data: each face's mips from mip 0, faces in order (TextureInit)
    const u8* src = static_cast<const u8*>(init.data);
    const bool bc = isBlockCompressed(init.format);
    for (u32 layer = 0; layer < layers; ++layer)
        for (u32 mip = 0; mip < init.mipLevels; ++mip) {
            const u32 w = std::max(1u, init.width >> mip), h = std::max(1u, init.height >> mip);
            const u64 bytes = textureMipBytes(init.format, w, h);
            wgpu::TexelCopyTextureInfo dst;
            dst.texture = entry.texture;
            dst.mipLevel = mip;
            dst.origin = {0, 0, layer};
            wgpu::TexelCopyBufferLayout layout;
            const u32 rows = bc ? (h + 3) / 4 : h;
            layout.bytesPerRow = static_cast<u32>(bytes / rows);
            layout.rowsPerImage = rows;
            // BC mips smaller than a block are still copied as one whole block
            const wgpu::Extent3D size{bc ? std::max(w, 4u) : w, bc ? std::max(h, 4u) : h, 1};
            m->queue.WriteTexture(&dst, src, bytes, &layout, &size);
            src += bytes;
        }
    m->textures.push_back(entry);
    const u32 id = static_cast<u32>(m->textures.size());
    m->textures.back().slot = allocSlot(*m, id, init.cube);
    return {id};
}

u32 Device::textureIndex(Texture texture) const { return directIndex(m->textures[texture.id - 1].slot); }

RenderTarget Device::createRenderTarget(u32 width, u32 height, TargetKind kind) {
    Impl::TextureEntry color;
    wgpu::TextureDescriptor td;
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {width, height, 1};
    td.format = kind == TargetKind::Ldr ? m->swapFormat : kHdrFormat;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc;
    color.texture = m->device.CreateTexture(&td);
    color.view = color.texture.CreateView();
    Impl::TargetEntry target;
    target.width = width;
    target.height = height;
    target.ldr = kind == TargetKind::Ldr;
    if (kind == TargetKind::Scene) {
        td.format = kDepthFormat;
        td.usage = wgpu::TextureUsage::RenderAttachment;
        target.depth = m->device.CreateTexture(&td);
        target.depthView = target.depth.CreateView();
    }
    m->textures.push_back(color);
    target.texture = static_cast<u32>(m->textures.size());
    m->textures.back().slot = allocSlot(*m, target.texture, false);
    m->targets.push_back(target);
    return {static_cast<u32>(m->targets.size())};
}

void Device::destroyRenderTarget(RenderTarget target) {
    if (!m || !target || target.id > m->targets.size() || m->inFrame) return;
    Impl::TargetEntry& t = m->targets[target.id - 1];
    if (!t.texture || t.depthOnly) return;
    if (t.depth) t.depth.Destroy();
    Impl::TextureEntry& color = m->textures[t.texture - 1];
    freeSlot(*m, color);
    color.texture.Destroy();
    color = {};
    t = {};
}

RenderTarget Device::createDepthTarget(u32 width, u32 height) {
    Impl::TextureEntry depth;
    depth.depth = true;
    wgpu::TextureDescriptor td;
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {width, height, 1};
    td.format = kDepthFormat;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    depth.texture = m->device.CreateTexture(&td);
    depth.view = depth.texture.CreateView();
    Impl::TargetEntry target;
    target.depthOnly = true;
    target.width = width;
    target.height = height;
    target.depth = depth.texture;
    target.depthView = depth.view;
    m->textures.push_back(depth);
    target.texture = static_cast<u32>(m->textures.size());
    m->textures.back().slot = allocSlot(*m, target.texture, false, true);
    m->targets.push_back(target);
    return {static_cast<u32>(m->targets.size())};
}

Texture Device::renderTargetTexture(RenderTarget target) const { return {target ? m->targets[target.id - 1].texture : 0}; }

// ---------------------------------------------------------------- pipelines

namespace {
wgpu::ShaderModule shaderModule(const wgpu::Device& device, const ShaderCode& code) {
    // WGSL text with a trailing 0 (cmake/WgslHeader.cmake)
    const char* source = static_cast<const char*>(code.data);
    size_t size = code.size;
    while (size && source[size - 1] == 0) --size;
    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = wgpu::StringView(source, size);
    wgpu::ShaderModuleDescriptor desc;
    desc.nextInChain = &wgsl;
    return device.CreateShaderModule(&desc);
}
} // namespace

namespace {
wgpu::RenderPipeline buildPipeline(Device::Impl& m, const PipelineDesc& desc) {
    m.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    const wgpu::ShaderModule vs = shaderModule(m.device, desc.vs), ps = shaderModule(m.device, desc.ps);

    std::vector<wgpu::VertexAttribute> attributes(desc.vertexAttributes.size());
    for (size_t i = 0; i < desc.vertexAttributes.size(); ++i) {
        attributes[i].format = toWgpu(desc.vertexAttributes[i].format);
        attributes[i].offset = desc.vertexAttributes[i].offset;
        attributes[i].shaderLocation = static_cast<u32>(i);
    }
    wgpu::VertexBufferLayout vertexLayout;
    vertexLayout.stepMode = wgpu::VertexStepMode::Vertex;
    vertexLayout.arrayStride = desc.vertexStride;
    vertexLayout.attributeCount = attributes.size();
    vertexLayout.attributes = attributes.data();

    wgpu::RenderPipelineDescriptor pd;
    pd.layout = m.pipelineLayout;
    pd.vertex.module = vs;
    pd.vertex.entryPoint = "VSMain";
    if (!attributes.empty()) {
        pd.vertex.bufferCount = 1;
        pd.vertex.buffers = &vertexLayout;
    }
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.primitive.frontFace = wgpu::FrontFace::CW; // DX12's front face; WebGPU clip space matches DX12, so no viewport flip
    pd.primitive.cullMode = desc.cullFront ? wgpu::CullMode::Front : desc.cullBack ? wgpu::CullMode::Back : wgpu::CullMode::None;
    // desc.wireframe: WebGPU has no line fill mode, so faces are filled (like Vulkan without fillModeNonSolid)

    const bool scene = desc.target == Target::Scene, shadow = desc.target == Target::Shadow;
    wgpu::DepthStencilState depth;
    if (scene || shadow) {
        const bool test = (scene && desc.depthTest) || shadow;
        depth.format = kDepthFormat;
        depth.depthWriteEnabled = test && (desc.depthWrite || shadow);
        depth.depthCompare = test ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Always; // reversed-Z
        pd.depthStencil = &depth;
    }
    wgpu::BlendState blend;
    if (desc.additiveBlend) {
        blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::One};
        blend.alpha = blend.color;
    } else if (desc.premultipliedBlend) {
        blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha};
        blend.alpha = blend.color;
    }
    wgpu::ColorTargetState color;
    color.format = desc.target == Target::Overlay ? m.swapFormat : kHdrFormat;
    color.blend = desc.additiveBlend || desc.premultipliedBlend ? &blend : nullptr;
    wgpu::FragmentState fragment;
    fragment.module = ps;
    fragment.entryPoint = "PSMain";
    fragment.targetCount = shadow ? 0 : 1;
    fragment.targets = &color;
    pd.fragment = &fragment;
    wgpu::RenderPipeline pipeline = m.device.CreateRenderPipeline(&pd);

    std::string error;
    m.wait(m.device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                                  [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
                                      if (type != wgpu::ErrorType::NoError) error = text(message);
                                  }),
           "pipeline creation");
    if (!error.empty()) MKRHI_FATAL("WebGPU pipeline creation failed:\n%s", error.c_str());
    return pipeline;
}
} // namespace

Pipeline Device::createPipeline(const PipelineDesc& desc) {
    m->pipelines.push_back(buildPipeline(*m, desc));
    return {static_cast<u32>(m->pipelines.size())};
}

bool Device::recreatePipeline(Pipeline pipeline, const PipelineDesc& desc) {
    if (!m || m->inFrame || !pipeline || pipeline.id > m->pipelines.size()) return false;
    m->pipelines[pipeline.id - 1] = buildPipeline(*m, desc);
    return true;
}

// ---------------------------------------------------------------- frame

bool Device::beginFrame() {
    if (!m || m->width == 0 || m->height == 0) return false;
    m->instance.ProcessEvents(); // error and device-lost callbacks
    if (m->surfaceDirty && !m->configureSurface()) return false;
    wgpu::SurfaceTexture st;
    m->surface.GetCurrentTexture(&st);
    if (st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        st.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) {
        m->surfaceDirty = true; // timeout, outdated or lost: drop this frame and configure again
        return false;
    }
    if (st.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) m->surfaceDirty = true;
    m->backTexture = st.texture;
    m->backView = st.texture.CreateView();
    m->encoder = m->device.CreateCommandEncoder();
    m->uploadStart = m->frameIndex * kUploadBytesPerFrame;
    m->uploadOffset = m->uploadStart;
    m->uploadLimit = m->uploadStart + kUploadBytesPerFrame;
    m->backDrawn = false;
    m->pipeline = m->vertexBuffer = m->indexBuffer = 0;
    m->pushBytes = 0;
    m->inFrame = true;
    return true;
}

CommandList& Device::cmd() { return m->cmd; }

u8* Device::allocUpload(u64 bytes, u32 alignment, u64* offset) {
    const u64 aligned = (m->uploadOffset + alignment - 1) & ~static_cast<u64>(alignment - 1);
    if (aligned + bytes > m->uploadLimit) return nullptr;
    m->uploadOffset = aligned + bytes;
    *offset = aligned;
    return m->ringCpu.data() + aligned;
}

u32 Device::uploadBufferIndex() const { return 0; } // Bindless.hlsli mkBuffer: every index is the ring
u32 Device::copyPitch(u32 width, u32 bytesPerPixel) const { return align(width * bytesPerPixel, 256); } // bytesPerRow rule
u32 Device::copyAlignment() const { return 16; }

void Device::endFrame(bool vsync, Capture* capture, PixelRead* reads, u32 readCount) {
    if (!m || !m->inFrame) return;
    m->endPass();
    // The ring's bytes for this frame go ahead of the commands that read them (queue order)
    const u64 used = (m->uploadOffset - m->uploadStart + 3) & ~u64{3};
    if (used) m->queue.WriteBuffer(m->ring, m->uploadStart, m->ringCpu.data() + m->uploadStart, used);

    const Impl::TargetEntry* source = capture && capture->source ? &m->targets[capture->source.id - 1] : nullptr;
    const u32 captureW = source ? source->width : m->width, captureH = source ? source->height : m->height;
    const u32 capturePitch = align(captureW * 4, 256);
    wgpu::Buffer readback;
    if (capture && (source || m->swapCopySrc)) {
        wgpu::BufferDescriptor bd;
        bd.size = static_cast<u64>(capturePitch) * captureH;
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        readback = m->device.CreateBuffer(&bd);
        wgpu::TexelCopyTextureInfo src;
        src.texture = source ? m->textures[source->texture - 1].texture : m->backTexture;
        wgpu::TexelCopyBufferInfo dst;
        dst.buffer = readback;
        dst.layout.bytesPerRow = capturePitch;
        dst.layout.rowsPerImage = captureH;
        const wgpu::Extent3D size{captureW, captureH, 1};
        m->encoder.CopyTextureToBuffer(&src, &dst, &size);
    }
    // Pixel inspector: one texel per 256-byte slot (a capture frame skips them)
    constexpr u32 kPixelSlot = 256;
    wgpu::Buffer pixels;
    if (capture || readCount > kMaxPixelReads) readCount = 0;
    if (readCount && reads) {
        wgpu::BufferDescriptor bd;
        bd.size = static_cast<u64>(kPixelSlot) * readCount;
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        pixels = m->device.CreateBuffer(&bd);
        for (u32 i = 0; i < readCount; ++i) {
            PixelRead& read = reads[i];
            read.valid = false;
            const bool fromBack = !read.source;
            if (fromBack && (!m->backDrawn || !m->swapCopySrc)) continue;
            const Impl::TargetEntry* t = fromBack ? nullptr : &m->targets[read.source.id - 1];
            const u32 w = t ? t->width : m->width, h = t ? t->height : m->height;
            if (read.x >= w || read.y >= h) continue;
            wgpu::TexelCopyTextureInfo src;
            src.texture = t ? m->textures[t->texture - 1].texture : m->backTexture;
            src.origin = {read.x, read.y, 0};
            wgpu::TexelCopyBufferInfo dst;
            dst.buffer = pixels;
            dst.layout.offset = static_cast<u64>(i) * kPixelSlot;
            dst.layout.bytesPerRow = kPixelSlot;
            const wgpu::Extent3D size{1, 1, 1};
            m->encoder.CopyTextureToBuffer(&src, &dst, &size);
            read.valid = true; // confirmed after the GPU finishes
        }
    }

    const wgpu::CommandBuffer commands = m->encoder.Finish();
    m->encoder = nullptr;
    m->queue.Submit(1, &commands);
    m->surface.Present();
    m->backView = nullptr;
    m->backTexture = nullptr;
    if (vsync != m->vsync) { // the present mode only changes by configuring again
        m->vsync = vsync;
        m->surfaceDirty = true;
    }

    auto map = [&](const wgpu::Buffer& buffer, const char* what) {
        bool ok = false;
        const bool done = m->wait(buffer.MapAsync(wgpu::MapMode::Read, 0, wgpu::kWholeMapSize, wgpu::CallbackMode::WaitAnyOnly,
                                                  [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                                      ok = status == wgpu::MapAsyncStatus::Success;
                                                  }),
                                  what);
        return done && ok;
    };
    if (pixels) {
        const bool done = map(pixels, "pixel readback");
        const u8* src = done ? static_cast<const u8*>(pixels.GetConstMappedRange()) : nullptr;
        for (u32 i = 0; i < readCount; ++i) {
            if (!reads[i].valid) continue;
            if (!src) {
                reads[i].valid = false;
                continue;
            }
            std::memcpy(reads[i].texel, src + static_cast<size_t>(i) * kPixelSlot, 4);
            // RGBA8 with R first for the back buffer and Ldr targets (both in the swap chain's format); HDR targets as stored
            const bool eightBit = !reads[i].source || m->targets[reads[i].source.id - 1].ldr;
            if (m->swapBgra && eightBit) std::swap(reads[i].texel[0], reads[i].texel[2]);
        }
    }
    if (capture) {
        capture->width = capture->height = 0;
        capture->rgba.clear();
        if (readback && map(readback, "capture readback")) {
            const u8* src = static_cast<const u8*>(readback.GetConstMappedRange());
            // Ldr targets use the swap chain's format, so both sources swizzle the same way
            const bool bgra = m->swapBgra;
            capture->width = captureW;
            capture->height = captureH;
            capture->rgba.resize(static_cast<size_t>(captureW) * captureH * 4);
            for (u32 y = 0; y < captureH; ++y)
                for (u32 x = 0; x < captureW; ++x) {
                    const u8* p = src + static_cast<size_t>(y) * capturePitch + x * 4;
                    u8* d = capture->rgba.data() + (static_cast<size_t>(y) * captureW + x) * 4;
                    d[0] = p[bgra ? 2 : 0];
                    d[1] = p[1];
                    d[2] = p[bgra ? 0 : 2];
                    d[3] = 255;
                }
        }
    }
    m->frameIndex = (m->frameIndex + 1) % kFramesInFlight;
    m->inFrame = false;
}

void Device::waitIdle() {
    if (!m || !m->queue) return;
    m->wait(m->queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {}),
            "waitIdle");
}

const GpuTimes& Device::gpuTimes() const { return m->gpuTimes; } // no timestamp queries yet: always invalid
GpuMemory Device::gpuMemory() const { return {}; }              // WebGPU doesn't report memory

// ---------------------------------------------------------------- CommandList

void CommandList::beginOverlay() {
    Device::Impl* m = m_device->impl();
    wgpu::RenderPassColorAttachment color;
    color.view = m->backView;
    color.loadOp = wgpu::LoadOp::Load;
    color.storeOp = wgpu::StoreOp::Store;
    wgpu::RenderPassDescriptor rp;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;
    m->beginPass(rp, 0, m->width, m->height);
    m->backDrawn = true;
}

void CommandList::beginTarget(RenderTarget target, const f32 clearColor[4], f32 clearDepth) {
    Device::Impl* m = m_device->impl();
    const Device::Impl::TargetEntry& t = m->targets[target.id - 1];
    wgpu::RenderPassDepthStencilAttachment depth;
    depth.depthLoadOp = wgpu::LoadOp::Clear;
    depth.depthClearValue = clearDepth;
    wgpu::RenderPassDescriptor rp;
    if (t.depthOnly) { // shadow maps: depth is stored since shaders read it next
        depth.view = t.depthView;
        depth.depthStoreOp = wgpu::StoreOp::Store;
        rp.depthStencilAttachment = &depth;
        m->beginPass(rp, t.texture, t.width, t.height);
        return;
    }
    wgpu::RenderPassColorAttachment color;
    color.view = m->textures[t.texture - 1].view;
    color.loadOp = clearColor ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load; // if not cleared, contents are kept
    color.storeOp = wgpu::StoreOp::Store;
    if (clearColor) color.clearValue = {clearColor[0], clearColor[1], clearColor[2], clearColor[3]};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;
    if (t.depth) {
        depth.view = t.depthView;
        depth.depthStoreOp = wgpu::StoreOp::Discard;
        rp.depthStencilAttachment = &depth;
    }
    m->beginPass(rp, t.texture, t.width, t.height);
}

void CommandList::endTarget(RenderTarget) { m_device->impl()->endPass(); } // WebGPU tracks texture usage itself

void CommandList::setViewport(f32 x, f32 y, f32 width, f32 height) {
    f32* v = m_device->impl()->viewport;
    v[0] = x;
    v[1] = y;
    v[2] = width;
    v[3] = height;
}

void CommandList::setPipeline(Pipeline pipeline) { m_device->impl()->pipeline = pipeline.id; }

void CommandList::pushConstants(const void* data, u32 bytes) {
    Device::Impl* m = m_device->impl();
    bytes = std::min(bytes, Device::kPushConstantBytes);
    std::memcpy(m->push, data, bytes);
    m->pushBytes = std::max(m->pushBytes, bytes);
    m->applied.push = false;
}

void CommandList::setVertexBuffer(Buffer buffer, u32) { m_device->impl()->vertexBuffer = buffer.id; }
void CommandList::setIndexBuffer(Buffer buffer) { m_device->impl()->indexBuffer = buffer.id; }

void CommandList::drawIndexed(u32 indexCount, u32 instanceCount) {
    Device::Impl* m = m_device->impl();
    if (m->flushDraw()) m->pass.DrawIndexed(indexCount, instanceCount);
}

void CommandList::draw(u32 vertexCount, u32 instanceCount) {
    Device::Impl* m = m_device->impl();
    if (m->flushDraw()) m->pass.Draw(vertexCount, instanceCount);
}

void CommandList::textureBarrier(Texture, TextureState, TextureState) { m_device->impl()->endPass(); } // copies run outside passes

void CommandList::dispatch(u32, u32, u32) { MKRHI_FATAL("Compute isn't implemented on the WebGPU backend yet"); }
void CommandList::bufferBarrier(Buffer, BufferState, BufferState) { m_device->impl()->endPass(); }
void CommandList::uavBarrier() { m_device->impl()->endPass(); }

void CommandList::copyToTexture(Texture texture, u32 x, u32 y, u32 width, u32 height, u64 uploadOffset, u32 rowPitch) {
    Device::Impl* m = m_device->impl();
    m->endPass();
    wgpu::TexelCopyBufferInfo src;
    src.buffer = m->ring;
    src.layout.offset = uploadOffset;
    src.layout.bytesPerRow = rowPitch;
    src.layout.rowsPerImage = height;
    wgpu::TexelCopyTextureInfo dst;
    dst.texture = m->textures[texture.id - 1].texture;
    dst.origin = {x, y, 0};
    const wgpu::Extent3D size{width, height, 1};
    m->encoder.CopyBufferToTexture(&src, &dst, &size);
}

void CommandList::beginTimer(u32) {}
void CommandList::endTimer(u32) {}

// ---------------------------------------------------------------- GPU inspection

std::string gpuMinimumError(const GpuInfo& info) { return info.unsupported; }

bool queryGpu(GpuInfo& out, std::string* error) {
    const wgpu::Instance instance = createInstance();
    const wgpu::Adapter adapter = instance ? requestAdapter(instance, nullptr, error) : wgpu::Adapter();
    if (!adapter) {
        if (error && error->empty()) *error = "No WebGPU adapter found";
        return false;
    }
    describeGpu(adapter, out);
    return true;
}

const char* backendName() { return "webgpu"; }

} // namespace mkrhi
