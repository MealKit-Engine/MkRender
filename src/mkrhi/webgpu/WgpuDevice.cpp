#include "Wgpu.h"

#include "../Files.h"
#include "mkrhi/Log.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
// Browser: the canvas shows a frame when control goes back to the event loop. JSPI suspends the frame loop until the next
// animation frame (the host's loop stays a plain loop). Hidden tabs get no animation frames: a 100 ms timer goes on instead
EM_ASYNC_JS(void, mkrhiWaitAnimationFrame, (), {
    await new Promise((resolve) => {
        requestAnimationFrame(resolve);
        setTimeout(resolve, 100);
    });
});
#endif

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

// Index the shaders decode (Bindless.hlsli mkDirectId): pool 31 in bits 23..19, the direct id in bits 18..11
u32 directIndex(u32 id) { return (kDirectPool << 19) | (id << 11); }

// Storage texture format of a storage slot (kStorageFirst order)
u32 storageFormatOfSlot(u32 slot) {
    u32 f = 0;
    while (f + 1 < kStorageFormats && slot >= kStorageFirst[f + 1]) ++f;
    return f;
}

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
constexpr u32 kSampledTexturesNeeded = kAllPools + kBoundTextures;
constexpr u32 kStorageBuffersNeeded = 1 + 2 * kStorageBuffers; // ring + read slots + write slots (compute)
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
    need(limits.maxSampledTexturesPerShaderStage >= kSampledTexturesNeeded,
         (std::to_string(kSampledTexturesNeeded) + " sampled textures per shader stage").c_str());
    need(limits.maxStorageBuffersPerShaderStage >= kStorageBuffersNeeded,
         (std::to_string(kStorageBuffersNeeded) + " storage buffers per shader stage").c_str());
    need(limits.maxStorageTexturesPerShaderStage >= kStorageTextures,
         (std::to_string(kStorageTextures) + " storage textures per shader stage").c_str());
    return missing;
}

void describeGpu(const wgpu::Adapter& adapter, GpuInfo& info) {
    wgpu::AdapterInfo a;
    adapter.GetInfo(&a);
#ifdef __EMSCRIPTEN__
    info.api = "WebGPU (browser)";
#else
    info.api = std::string("WebGPU (Dawn, ") + backendText(a.backendType) + ")";
#endif
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

// Timestamp writes for the pass about to begin: a start query when marks wait for one, an end query always (it becomes passEnd)
const wgpu::PassTimestampWrites* Device::Impl::nextPassTimestamps() {
    TimerFrame& f = timerFrames[frameIndex];
    if (!timestamps || f.queries + 2 > kQueriesPerFrame) return nullptr;
    const u32 base = frameIndex * kQueriesPerFrame;
    passTimestamps = {};
    passTimestamps.querySet = querySet;
    passTimestamps.beginningOfPassWriteIndex = wgpu::kQuerySetIndexUndefined;
    if (f.pendingBegins) {
        for (u32 i = 0; i < kMarks; ++i)
            if (f.pendingBegins >> i & 1) f.marks[i] = f.queries;
        f.pendingBegins = 0;
        passTimestamps.beginningOfPassWriteIndex = base + f.queries++;
    }
#ifdef __EMSCRIPTEN__
    // emdawnwebgpu passes the "undefined" index (0xFFFFFFFF) on to the browser, which rejects it: write an unused start query instead
    else {
        passTimestamps.beginningOfPassWriteIndex = base + f.queries++;
    }
#endif
    f.passEnd = f.queries++;
    passTimestamps.endOfPassWriteIndex = base + f.passEnd;
    return &passTimestamps;
}

// A frame's timestamps once its readback is mapped (nanoseconds) into gpuTimes
void Device::Impl::readTimers(TimerFrame& frame) {
    if (!frame.pending) return;
    frame.pending = false;
    if (!wait(frame.mapped, "GPU timer readback")) return;
    const u64* t = static_cast<const u64*>(frame.readback.GetConstMappedRange(0, kQueriesPerFrame * 8));
    auto ms = [&](u32 begin, u32 end) {
        if (frame.marks[begin] == kNoQuery || frame.marks[end] == kNoQuery) return 0.0f;
        const u64 a = t[frame.marks[begin]], b = t[frame.marks[end]];
        return b > a ? static_cast<f32>(static_cast<f64>(b - a) / 1e6) : 0.0f;
    };
    gpuTimes.frameMs = ms(0, 1);
    for (u32 i = 0; i < kMaxGpuTimers; ++i) gpuTimes.timerMs[i] = ms(2 + 2 * i, 3 + 2 * i);
    gpuTimes.valid = true;
    frame.readback.Unmap();
}

namespace {
constexpr char kCacheMagic[8] = {'M', 'K', 'W', 'G', 'P', 'U', '1', 0}; // then per entry: u32 key size, u32 value size, key, value
}

void Device::Impl::loadCache(const std::string& folder) {
    cache.path = folder + "/pipelines.webgpu";
    std::string data;
    if (!detail::readFile(cache.path, data) || data.size() < sizeof(kCacheMagic) || std::memcmp(data.data(), kCacheMagic, sizeof(kCacheMagic)) != 0)
        return;
    size_t at = sizeof(kCacheMagic);
    while (at + 8 <= data.size()) {
        u32 sizes[2];
        std::memcpy(sizes, data.data() + at, 8);
        at += 8;
        if (static_cast<u64>(sizes[0]) + sizes[1] > data.size() - at) break; // truncated: keep what came before
        cache.entries.emplace(data.substr(at, sizes[0]), data.substr(at + sizes[0], sizes[1]));
        at += static_cast<size_t>(sizes[0]) + sizes[1];
    }
}

void Device::Impl::saveCache() {
    std::lock_guard<std::mutex> guard(cache.lock);
    if (cache.path.empty() || !cache.dirty) return;
    std::string data(kCacheMagic, sizeof(kCacheMagic));
    for (const auto& [key, value] : cache.entries) {
        const u32 sizes[2] = {static_cast<u32>(key.size()), static_cast<u32>(value.size())};
        data.append(reinterpret_cast<const char*>(sizes), 8);
        data += key;
        data += value;
    }
    const size_t slash = cache.path.find_last_of('/');
    detail::createDirectories(cache.path.substr(0, slash));
    if (detail::writeFile(cache.path, data)) cache.dirty = false;
    else MKRHI_WARN("[WebGPU] pipeline cache: can't write %s", cache.path.c_str());
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
    wgpu::RenderPassDescriptor timed = desc;
    timed.timestampWrites = nextPassTimestamps();
    pass = encoder.BeginRenderPass(&timed);
    passExclude = exclude;
    passWidth = w;
    passHeight = h;
    applied = {};
}

// Group 0: ring, samplers and the storage buffers in ShaderRead (the others read as an empty buffer)
void Device::Impl::buildGroup0() {
    wgpu::BindGroupEntry entries[kGroup0Bindings];
    entries[0].binding = 0;
    entries[0].buffer = ring;
    entries[1].binding = 2;
    entries[1].sampler = linearClamp;
    entries[2].binding = 3;
    entries[2].sampler = linearWrap;
    entries[3].binding = 4;
    entries[3].sampler = shadowCompare;
    for (u32 i = 0; i < kStorageBuffers; ++i) {
        const u32 id = storageBufferSlots[i];
        entries[4 + i].binding = 5 + i;
        entries[4 + i].buffer = id && !buffers[id - 1].unordered ? buffers[id - 1].buffer : dummyRead[i];
    }
    wgpu::BindGroupDescriptor desc;
    desc.layout = layout0;
    desc.entryCount = kGroup0Bindings;
    desc.entries = entries;
    group0 = device.CreateBindGroup(&desc);
    group0Dirty = false;
}

void Device::Impl::buildGroup1() {
    wgpu::BindGroupEntry entries[kAllPools];
    for (u32 i = 0; i < kAllPools; ++i) {
        entries[i].binding = i;
        entries[i].textureView = pools[i].view ? pools[i].view : i < kPools ? dummyArrayView : dummyCubeArrayView;
    }
    wgpu::BindGroupDescriptor desc;
    desc.layout = layout1;
    desc.entryCount = kAllPools;
    desc.entries = entries;
    group1 = device.CreateBindGroup(&desc);
}

// Group 2: the textures useTextures named, without `exclude` (the pass's attachment, which a pass can't also read) and, for
// dispatches, without the storage textures in UnorderedAccess (bound for writing in group 3: WebGPU doesn't allow both uses in one
// dispatch). Binding 4 holds their direct ids (g_directIds: 2D 0, 2D 1, cube, depth; ~0u = none). Built once per combination
wgpu::BindGroup Device::Impl::group2(u32 exclude, bool skipUnordered) {
    Group2Key key;
    key.bound = bound;
    key.exclude = exclude;
    key.skipUnordered = skipUnordered;
    for (const Group2Entry& e : group2Cache)
        if (e.key == key) return e.group;
    u32 ids[4] = {~0u, ~0u, ~0u, ~0u};
    wgpu::BindGroupEntry entries[kDirectBindings];
    for (u32 i = 0; i < kBoundTextures; ++i) {
        const bool cube = i == kBound2D, depth = i == kBound2D + kBoundCubes;
        const u32 id = bound[i];
        const bool use = id && id != exclude && !(skipUnordered && textures[id - 1].unordered);
        if (use) ids[i] = textures[id - 1].slot;
        entries[i].binding = i;
        entries[i].textureView = use ? textures[id - 1].view : depth ? dummyDepthView : cube ? dummyCubeView : dummy2DView;
    }
    wgpu::BufferDescriptor bd;
    bd.size = sizeof(ids);
    bd.usage = wgpu::BufferUsage::Uniform;
    bd.mappedAtCreation = true;
    Group2Entry e;
    e.key = key;
    e.ids = device.CreateBuffer(&bd);
    std::memcpy(e.ids.GetMappedRange(), ids, sizeof(ids));
    e.ids.Unmap();
    entries[kBoundTextures].binding = kBoundTextures;
    entries[kBoundTextures].buffer = e.ids;
    wgpu::BindGroupDescriptor desc;
    desc.layout = layout2;
    desc.entryCount = kDirectBindings;
    desc.entries = entries;
    e.group = device.CreateBindGroup(&desc);
    if (group2Cache.size() >= 256) { // groups already set in an open pass stay alive in the pass
        group2Cache.clear();
        applied.group2 = nullptr; // a new group may get a freed one's address
    }
    group2Cache.push_back(e);
    return e.group;
}

// Dispatch group 3, rebuilt after a slot or a storage texture's state changed
void Device::Impl::buildComputeGroups() {
    if (!computeGroupsDirty) return;
    wgpu::BindGroupEntry entries[kStorageBindings];
    for (u32 i = 0; i < kStorageBuffers; ++i) {
        const u32 id = storageBufferSlots[i];
        entries[i].binding = i;
        entries[i].buffer = id && buffers[id - 1].unordered ? buffers[id - 1].buffer : dummyStorage[i];
    }
    for (u32 i = 0; i < kStorageTextures; ++i) {
        const u32 id = storageTextureSlots[i];
        wgpu::BindGroupEntry& e = entries[kStorageBuffers + i];
        e.binding = kStorageBuffers + i;
        e.textureView = id && textures[id - 1].unordered ? textures[id - 1].storageView : dummyStorageViews[i];
    }
    wgpu::BindGroupDescriptor desc;
    desc.layout = layout3;
    desc.entryCount = kStorageBindings;
    desc.entries = entries;
    computeGroup3 = device.CreateBindGroup(&desc);
    computeGroupsDirty = false;
}

bool Device::Impl::flushDraw() {
    if (!pass || !pipeline) return false;
    if (applied.pipeline != pipeline) {
        pass.SetPipeline(pipelines[pipeline - 1]);
        applied.pipeline = pipeline;
    }
    if (group0Dirty) buildGroup0();
    if (applied.group0 != group0.Get()) {
        pass.SetBindGroup(0, group0);
        applied.group0 = group0.Get();
    }
    if (applied.group1 != group1.Get()) { // a pool grew
        pass.SetBindGroup(1, group1);
        applied.group1 = group1.Get();
    }
    const wgpu::BindGroup g2 = group2(passExclude, false);
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
#if defined(__EMSCRIPTEN__)
    const auto* canvas = static_cast<const WebCanvas*>(desc.window);
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector source;
    source.selector = canvas && canvas->selector ? canvas->selector : "#canvas";
    wgpu::SurfaceDescriptor surfaceDesc;
    surfaceDesc.nextInChain = &source;
    m->surface = m->instance.CreateSurface(&surfaceDesc);
#elif defined(__linux__)
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
    // Optional features: Float32Filterable (R32Float storage textures can then sit in a direct slot, whose binding is a filterable
    // float texture), TimestampQuery (GPU timers, at pass boundaries)
    m->float32Filterable = m->adapter.HasFeature(wgpu::FeatureName::Float32Filterable);
    m->timestamps = m->adapter.HasFeature(wgpu::FeatureName::TimestampQuery);
    std::vector<wgpu::FeatureName> features = {wgpu::FeatureName::TextureCompressionBC, wgpu::FeatureName::RG11B10UfloatRenderable};
    if (m->float32Filterable) features.push_back(wgpu::FeatureName::Float32Filterable);
    if (m->timestamps) features.push_back(wgpu::FeatureName::TimestampQuery);
    wgpu::Limits limits;
    limits.maxImmediateSize = kPushConstantBytes;
    limits.maxSampledTexturesPerShaderStage = kSampledTexturesNeeded;
    limits.maxStorageTexturesPerShaderStage = kStorageTextures;
    limits.maxStorageBuffersPerShaderStage = kStorageBuffersNeeded;
    wgpu::DeviceDescriptor deviceDesc;
#ifndef __EMSCRIPTEN__ // browsers keep their own shader cache and quantize timestamps; Dawn's descriptors don't exist there
    // Pipeline cache: Dawn asks for and hands over blobs by key; the isolation key keeps other GPUs/drivers apart
    WGPUDawnCacheDeviceDescriptor cacheDesc = WGPU_DAWN_CACHE_DEVICE_DESCRIPTOR_INIT;
    const std::string isolation = gpu.adapter + "|" + gpu.driverVersion;
    if (!desc.pipelineCache.empty()) {
        m->loadCache(desc.pipelineCache);
        cacheDesc.isolationKey = {isolation.data(), isolation.size()};
        cacheDesc.dawnLoadCacheDataCallbackInfo.callback = [](size_t keySize, const uint8_t* key, size_t valueSize, uint8_t* value,
                                                              void* userdata, void*) -> size_t {
            auto& c = static_cast<Impl*>(userdata)->cache;
            std::lock_guard<std::mutex> guard(c.lock);
            const auto it = c.entries.find(std::string(reinterpret_cast<const char*>(key), keySize));
            if (it == c.entries.end()) return 0;
            if (value && valueSize >= it->second.size()) std::memcpy(value, it->second.data(), it->second.size());
            return value && valueSize < it->second.size() ? 0 : it->second.size(); // nullptr: size query
        };
        cacheDesc.dawnLoadCacheDataCallbackInfo.userdata1 = m;
        cacheDesc.dawnStoreCacheDataCallbackInfo.callback = [](size_t keySize, const uint8_t* key, size_t valueSize, const uint8_t* value,
                                                               void* userdata, void*) {
            auto& c = static_cast<Impl*>(userdata)->cache;
            std::lock_guard<std::mutex> guard(c.lock);
            c.entries[std::string(reinterpret_cast<const char*>(key), keySize)] = std::string(reinterpret_cast<const char*>(value), valueSize);
            c.dirty = true;
        };
        cacheDesc.dawnStoreCacheDataCallbackInfo.userdata1 = m;
    }
    // Native only: timestamps at full precision (Dawn quantizes them by default against timing attacks in browsers)
    static const char* const kDisabledToggles[] = {"timestamp_quantization"};
    wgpu::DawnTogglesDescriptor toggles;
    toggles.disabledToggleCount = 1;
    toggles.disabledToggles = kDisabledToggles;
    if (!desc.pipelineCache.empty()) toggles.nextInChain = reinterpret_cast<const wgpu::ChainedStruct*>(&cacheDesc);
    deviceDesc.nextInChain = &toggles;
#endif
    deviceDesc.requiredFeatureCount = features.size();
    deviceDesc.requiredFeatures = features.data();
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
#ifdef __EMSCRIPTEN__
    m->swapCopySrc = true; // a canvas takes any usage; emdawnwebgpu leaves caps.usages empty
#else
    m->swapCopySrc = (caps.usages & wgpu::TextureUsage::CopySrc) != wgpu::TextureUsage::None;
#endif
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
    const wgpu::ShaderStage stages = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment | wgpu::ShaderStage::Compute;
    wgpu::BindGroupLayoutEntry e0[kGroup0Bindings];
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
    for (u32 i = 4; i < kGroup0Bindings; ++i) { // storage buffers for reading (bindings 5..7)
        e0[i].binding = i + 1;
        e0[i].visibility = stages;
        e0[i].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    }
    wgpu::BindGroupLayoutDescriptor ld;
    ld.entryCount = kGroup0Bindings;
    ld.entries = e0;
    m->layout0 = m->device.CreateBindGroupLayout(&ld);
    wgpu::BindGroupLayoutEntry e1[kAllPools];
    for (u32 i = 0; i < kAllPools; ++i) {
        e1[i].binding = i;
        e1[i].visibility = stages;
        e1[i].texture.sampleType = wgpu::TextureSampleType::Float;
        e1[i].texture.viewDimension = i < kPools ? wgpu::TextureViewDimension::e2DArray : wgpu::TextureViewDimension::CubeArray;
    }
    ld.entryCount = kAllPools;
    ld.entries = e1;
    m->layout1 = m->device.CreateBindGroupLayout(&ld);
    wgpu::BindGroupLayoutEntry e2[kDirectBindings];
    for (u32 i = 0; i < kBoundTextures; ++i) {
        const bool cube = i == kBound2D, depth = i == kBound2D + kBoundCubes;
        e2[i].binding = i;
        e2[i].visibility = stages;
        e2[i].texture.sampleType = depth ? wgpu::TextureSampleType::Depth : wgpu::TextureSampleType::Float;
        e2[i].texture.viewDimension = cube ? wgpu::TextureViewDimension::Cube : wgpu::TextureViewDimension::e2D;
    }
    e2[kBoundTextures].binding = kBoundTextures;
    e2[kBoundTextures].visibility = stages;
    e2[kBoundTextures].buffer.type = wgpu::BufferBindingType::Uniform;
    ld.entryCount = kDirectBindings;
    ld.entries = e2;
    m->layout2 = m->device.CreateBindGroupLayout(&ld);
    const wgpu::BindGroupLayout layouts[3] = {m->layout0, m->layout1, m->layout2};
    wgpu::PipelineLayoutDescriptor pld;
    pld.bindGroupLayoutCount = 3;
    pld.bindGroupLayouts = layouts;
    pld.immediateSize = kPushConstantBytes;
    m->pipelineLayout = m->device.CreatePipelineLayout(&pld);
    // Group 3 (compute only): storage buffers, then storage textures by format (Compute.hlsli)
    static const wgpu::TextureFormat kStorageFormatList[kStorageFormats] = {
        wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureFormat::RGBA16Float, wgpu::TextureFormat::R32Float};
    wgpu::BindGroupLayoutEntry e3[kStorageBindings];
    for (u32 i = 0; i < kStorageBindings; ++i) {
        e3[i].binding = i;
        e3[i].visibility = wgpu::ShaderStage::Compute;
        if (i < kStorageBuffers) {
            e3[i].buffer.type = wgpu::BufferBindingType::Storage;
        } else {
            e3[i].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
            e3[i].storageTexture.format = kStorageFormatList[storageFormatOfSlot(i - kStorageBuffers)];
            e3[i].storageTexture.viewDimension = wgpu::TextureViewDimension::e2D;
        }
    }
    ld.entryCount = kStorageBindings;
    ld.entries = e3;
    m->layout3 = m->device.CreateBindGroupLayout(&ld);
    const wgpu::BindGroupLayout computeLayouts[4] = {m->layout0, m->layout1, m->layout2, m->layout3};
    pld.bindGroupLayoutCount = 4;
    pld.bindGroupLayouts = computeLayouts;
    m->computeLayout = m->device.CreatePipelineLayout(&pld);

    // Upload ring (two frame sections) and group 0
    wgpu::BufferDescriptor bd;
    bd.size = kUploadBytesPerFrame * kFramesInFlight;
    bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
    m->ring = m->device.CreateBuffer(&bd);
    m->ringCpu.resize(bd.size);

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
    td.format = wgpu::TextureFormat::RGBA8Unorm;
    m->dummyArray = m->device.CreateTexture(&td);
    vd.dimension = wgpu::TextureViewDimension::e2DArray;
    m->dummyArrayView = m->dummyArray.CreateView(&vd);
    vd.dimension = wgpu::TextureViewDimension::CubeArray;
    m->dummyCubeArrayView = m->dummyCube.CreateView(&vd);
    m->buildGroup1();
    // Empty storage slots: a 16-byte buffer and a 1×1 texture in the slot's format each
    bd.size = 16;
    bd.usage = wgpu::BufferUsage::Storage;
    for (wgpu::Buffer& b : m->dummyStorage) b = m->device.CreateBuffer(&bd);
    for (wgpu::Buffer& b : m->dummyRead) b = m->device.CreateBuffer(&bd);
    m->buildGroup0();
    td.usage = wgpu::TextureUsage::StorageBinding;
    for (u32 i = 0; i < kStorageTextures; ++i) {
        td.format = kStorageFormatList[storageFormatOfSlot(i)];
        m->dummyStorageViews[i] = m->device.CreateTexture(&td).CreateView();
    }
    if (m->timestamps) {
        wgpu::QuerySetDescriptor qd;
        qd.type = wgpu::QueryType::Timestamp;
        qd.count = Impl::kQueriesPerFrame * kFramesInFlight;
        m->querySet = m->device.CreateQuerySet(&qd);
        bd.size = static_cast<u64>(qd.count) * 8;
        bd.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        m->queryResolve = m->device.CreateBuffer(&bd);
        bd.size = Impl::kQueriesPerFrame * 8;
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst; // whole frame section (copies are 256-byte aligned: 1 KB)
        for (Impl::TimerFrame& f : m->timerFrames) f.readback = m->device.CreateBuffer(&bd);
    }
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
    m->saveCache();
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
    if (b.storage) {
        m->storageBufferSlots[b.storageSlot] = 0;
        m->computeGroupsDirty = m->group0Dirty = true;
    }
    if (b.buffer) b.buffer.Destroy();
    b = {};
}

// ---------------------------------------------------------------- compute resources

// Storage buffers take a slot of group 3 (read/write in UnorderedAccess, Compute.hlsli mkRwLoad/mkRwStore) and of group 0 (read in
// ShaderRead, Bindless.hlsli mkBufferLoad: bufferIndex = 1 + slot). mkBuffer reaches only the upload ring on WebGPU
Buffer Device::createStorageBuffer(u64 bytes) {
    u32 slot = 0;
    while (slot < kStorageBuffers && m->storageBufferSlots[slot]) ++slot;
    if (slot == kStorageBuffers) MKRHI_FATAL("WebGPU storage buffer slots exhausted (%u)", kStorageBuffers);
    Impl::BufferEntry entry;
    entry.storage = true;
    entry.storageSlot = slot;
    entry.bytes = std::max<u64>((bytes + 3) & ~u64{3}, 4);
    wgpu::BufferDescriptor bd;
    bd.size = entry.bytes;
    bd.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst; // WebGPU buffers start zeroed
    entry.buffer = m->device.CreateBuffer(&bd);
    m->buffers.push_back(entry);
    const u32 id = static_cast<u32>(m->buffers.size());
    m->storageBufferSlots[slot] = id;
    m->computeGroupsDirty = m->group0Dirty = true;
    return {id};
}

u32 Device::bufferIndex(Buffer buffer) const {
    if (!buffer || !m->buffers[buffer.id - 1].storage) MKRHI_FATAL("bufferIndex is for storage buffers");
    return 1 + m->buffers[buffer.id - 1].storageSlot;
}
u32 Device::storageBufferIndex(Buffer buffer) const { return buffer ? m->buffers[buffer.id - 1].storageSlot : 0; }

bool Device::readBuffer(Buffer buffer, u64 offset, void* dst, u64 bytes) {
    if (!m || !buffer || buffer.id > m->buffers.size() || m->inFrame) return false;
    const Impl::BufferEntry& b = m->buffers[buffer.id - 1];
    if (!b.storage || offset + bytes > b.bytes || (offset & 3) != 0) return false;
    wgpu::BufferDescriptor bd;
    bd.size = (bytes + 3) & ~u64{3};
    bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    const wgpu::Buffer staging = m->device.CreateBuffer(&bd);
    wgpu::CommandEncoder encoder = m->device.CreateCommandEncoder();
    encoder.CopyBufferToBuffer(b.buffer, offset, staging, 0, bd.size);
    const wgpu::CommandBuffer commands = encoder.Finish();
    m->queue.Submit(1, &commands);
    bool ok = false;
    const bool done = m->wait(staging.MapAsync(wgpu::MapMode::Read, 0, bd.size, wgpu::CallbackMode::WaitAnyOnly,
                                               [&](wgpu::MapAsyncStatus status, wgpu::StringView) { ok = status == wgpu::MapAsyncStatus::Success; }),
                              "readBuffer");
    if (!done || !ok) return false;
    std::memcpy(dst, staging.GetConstMappedRange(0, bd.size), bytes);
    staging.Unmap();
    return true;
}

namespace {
u32 allocSlot(Device::Impl& m, u32 textureId, bool cube, bool depth = false);
int storageFormat(Format f) { return f == Format::Rgba8Unorm ? 0 : f == Format::Rgba16Float ? 1 : f == Format::R32Float ? 2 : -1; }
} // namespace

// Storage textures: written in group 3 while in UnorderedAccess, read through their direct slot (mkLoad2D/mkSample2D) otherwise
Texture Device::createStorageTexture(u32 width, u32 height, Format format) {
    const int fmt = storageFormat(format);
    if (fmt < 0) MKRHI_FATAL("Storage textures are Rgba8Unorm, Rgba16Float or R32Float");
    if (format == Format::R32Float && !m->float32Filterable)
        MKRHI_FATAL("R32Float storage textures need the float32-filterable WebGPU feature (direct slots are filterable textures)");
    u32 slot = kStorageFirst[fmt];
    const u32 end = slot + kStoragePerFormat[fmt];
    while (slot < end && m->storageTextureSlots[slot]) ++slot;
    if (slot == end) MKRHI_FATAL("WebGPU storage texture slots of this format exhausted (%u)", kStoragePerFormat[fmt]);
    Impl::TextureEntry entry;
    entry.storage = true;
    entry.storageSlot = slot;
    wgpu::TextureDescriptor td;
    td.dimension = wgpu::TextureDimension::e2D;
    td.size = {width, height, 1};
    td.format = toWgpu(format);
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst;
    entry.texture = m->device.CreateTexture(&td);
    entry.view = entry.texture.CreateView();
    entry.storageView = entry.view;
    m->textures.push_back(entry);
    const u32 id = static_cast<u32>(m->textures.size());
    m->textures.back().slot = allocSlot(*m, id, false);
    m->storageTextureSlots[slot] = id;
    m->computeGroupsDirty = true;
    return {id};
}

u32 Device::storageTextureIndex(Texture texture) const { return texture ? m->textures[texture.id - 1].storageSlot : 0; }

namespace {
wgpu::ComputePipeline buildComputePipeline(Device::Impl& m, const ComputePipelineDesc& desc);
}

Pipeline Device::createComputePipeline(const ComputePipelineDesc& desc) {
    m->computePipelines.push_back(buildComputePipeline(*m, desc));
    m->pipelines.emplace_back();
    return {static_cast<u32>(m->pipelines.size())};
}

bool Device::recreatePipeline(Pipeline pipeline, const ComputePipelineDesc& desc) {
    if (!m || m->inFrame || !pipeline || pipeline.id > m->computePipelines.size() || !m->computePipelines[pipeline.id - 1]) return false;
    m->computePipelines[pipeline.id - 1] = buildComputePipeline(*m, desc);
    return true;
}

// ---------------------------------------------------------------- textures

namespace {
// Gives a texture the next free direct id. Fatal when they run out, like the bindless backends' slot exhaustion
u32 allocSlot(Device::Impl& m, u32 textureId, bool, bool) {
    for (u32 i = 0; i < kDirectIds; ++i)
        if (!m.directOwner[i]) {
            m.directOwner[i] = textureId;
            return i;
        }
    MKRHI_FATAL("WebGPU direct texture ids exhausted (%u)", kDirectIds);
}

// Pool format slot (kPoolFormats order), or -1 for formats that stay direct
int poolFormat(Format f) {
    switch (f) {
    case Format::Rgba8Unorm: return 0;
    case Format::Rgba8Srgb: return 1;
    case Format::Bc7Unorm: return 2;
    case Format::Bc7Srgb: return 3;
    case Format::Bc5Unorm: return 4;
    default: return -1;
    }
}

u32 log2u(u32 v) {
    u32 n = 0;
    while (v >>= 1) ++n;
    return n;
}

// Pool and mip bias for a texture, false when it stays direct: square power-of-two textures of a pool format (2D: poolFormat, cubes:
// Rgb9e5), up to 2048, that fit a size class within kPoolMaxBias mips (a 512 class takes 4..512, the 2048 class 1024..2048)
bool poolFor(const TextureInit& init, u32* pool, u32* bias) {
    const int format = init.cube ? (init.format == Format::Rgb9e5 ? 0 : -1) : poolFormat(init.format);
    if (format < 0 || init.width != init.height || (init.width & (init.width - 1)) != 0) return false;
    const u32 size = log2u(init.width);
    for (u32 c = 0; c < kPoolClasses; ++c)
        if (size <= kPoolClassLog2[c] && kPoolClassLog2[c] - size <= kPoolMaxBias && init.mipLevels <= size + 1) {
            *pool = init.cube ? kPools + c : static_cast<u32>(format) * kPoolClasses + c;
            *bias = kPoolClassLog2[c] - size;
            return true;
        }
    return false;
}

// Next free layer of a pool: creates it (first texture) or doubles it, copying the used layers on the GPU and rebuilding group 1.
// The old array stays alive while commands recorded before still use it. Indices don't change
u32 allocLayer(Device::Impl& m, u32 poolIndex, Format format) {
    Device::Impl::Pool& pool = m.pools[poolIndex];
    if (pool.used == pool.layers) {
        const bool cube = poolIndex >= kPools;
        const u32 cls = poolIndex % kPoolClasses, faces = cube ? 6u : 1u; // a cube is six array layers
        const u32 layers = pool.layers ? pool.layers * 2 : cube ? kCubePoolFirstLayers[cls] : kPoolFirstLayers[cls];
        if (layers * faces > kPoolMaxLayers) MKRHI_FATAL("WebGPU texture pool %u is full (%u array layers)", poolIndex, kPoolMaxLayers);
        const u32 size = 1u << kPoolClassLog2[cls];
        wgpu::TextureDescriptor td;
        td.dimension = wgpu::TextureDimension::e2D;
        td.size = {size, size, layers * faces};
        td.format = toWgpu(format);
        td.mipLevelCount = kPoolClassLog2[cls] + 1;
        td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::CopySrc;
        wgpu::Texture texture = m.device.CreateTexture(&td);
        if (pool.used) {
            wgpu::CommandEncoder encoder = m.device.CreateCommandEncoder();
            for (u32 mip = 0; mip < td.mipLevelCount; ++mip) {
                wgpu::TexelCopyTextureInfo src, dst;
                src.texture = pool.texture;
                dst.texture = texture;
                src.mipLevel = dst.mipLevel = mip;
                const u32 s = std::max(1u, size >> mip);
                const bool bc = isBlockCompressed(format);
                const wgpu::Extent3D extent{bc ? std::max(s, 4u) : s, bc ? std::max(s, 4u) : s, pool.used * faces};
                encoder.CopyTextureToTexture(&src, &dst, &extent);
            }
            const wgpu::CommandBuffer commands = encoder.Finish();
            m.queue.Submit(1, &commands);
        }
        wgpu::TextureViewDescriptor vd;
        vd.dimension = cube ? wgpu::TextureViewDimension::CubeArray : wgpu::TextureViewDimension::e2DArray;
        pool.texture = texture;
        pool.view = texture.CreateView(&vd);
        pool.layers = layers;
        m.buildGroup1();
    }
    return pool.used++;
}

void freeSlot(Device::Impl& m, const Device::Impl::TextureEntry& t) {
    const u32 id = m.directOwner[t.slot];
    m.directOwner[t.slot] = 0;
    for (u32& b : m.bound)
        if (b == id) b = 0;
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
    // Pooled: the texture's mip k is the pool's mip k + bias in its layer (normalized UVs read the same texels)
    u32 pool = 0, bias = 0;
    entry.pooled = poolFor(init, &pool, &bias);
    if (entry.pooled) {
        entry.slot = allocLayer(*m, pool, init.format);
        entry.texture = m->pools[pool].texture;
        entry.index = (pool << 19) | (entry.slot << 11) | (bias << 8) | (init.mipLevels - 1);
    } else {
        entry.texture = m->device.CreateTexture(&td);
        wgpu::TextureViewDescriptor vd;
        vd.dimension = init.cube ? wgpu::TextureViewDimension::Cube : wgpu::TextureViewDimension::e2D;
        entry.view = entry.texture.CreateView(&vd);
    }

    // Data: each face's mips from mip 0, faces in order (TextureInit)
    const u8* src = static_cast<const u8*>(init.data);
    const bool bc = isBlockCompressed(init.format);
    for (u32 layer = 0; layer < layers; ++layer)
        for (u32 mip = 0; mip < init.mipLevels; ++mip) {
            const u32 w = std::max(1u, init.width >> mip), h = std::max(1u, init.height >> mip);
            const u64 bytes = textureMipBytes(init.format, w, h);
            wgpu::TexelCopyTextureInfo dst;
            dst.texture = entry.texture;
            dst.mipLevel = entry.pooled ? mip + bias : mip;
            dst.origin = {0, 0, entry.pooled ? entry.slot * layers + layer : layer}; // cube pools: six layers per cube
            wgpu::TexelCopyBufferLayout layout;
            const u32 rows = bc ? (h + 3) / 4 : h;
            layout.bytesPerRow = static_cast<u32>(bytes / rows);
            layout.rowsPerImage = rows;
            // BC mips smaller than a block are still copied as one whole block
            const wgpu::Extent3D size{bc ? std::max(w, 4u) : w, bc ? std::max(h, 4u) : h, 1};
            m->queue.WriteTexture(&dst, src, bytes, &layout, &size);
            src += bytes;
        }
    entry.texture = entry.pooled ? wgpu::Texture() : entry.texture; // the pool owns its array (it may be replaced when it grows)
    m->textures.push_back(entry);
    const u32 id = static_cast<u32>(m->textures.size());
    if (!entry.pooled) m->textures.back().slot = allocSlot(*m, id, init.cube);
    return {id};
}

u32 Device::textureIndex(Texture texture) const {
    const Impl::TextureEntry& t = m->textures[texture.id - 1];
    return t.pooled ? t.index : directIndex(t.slot);
}

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
// Fatal with Dawn's message when the validation scope opened before pipeline creation caught an error
void checkPipelineErrors(Device::Impl& m) {
    std::string error;
    m.wait(m.device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                                  [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
                                      if (type != wgpu::ErrorType::NoError) error = text(message);
                                  }),
           "pipeline creation");
    if (!error.empty()) MKRHI_FATAL("WebGPU pipeline creation failed:\n%s", error.c_str());
}

wgpu::ComputePipeline buildComputePipeline(Device::Impl& m, const ComputePipelineDesc& desc) {
    m.device.PushErrorScope(wgpu::ErrorFilter::Validation);
    wgpu::ComputePipelineDescriptor pd;
    pd.layout = m.computeLayout;
    pd.compute.module = shaderModule(m.device, desc.cs);
    pd.compute.entryPoint = "CSMain"; // CMake renames compute entry points (-fspv-entrypoint-name)
    wgpu::ComputePipeline pipeline = m.device.CreateComputePipeline(&pd);
    checkPipelineErrors(m);
    return pipeline;
}

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
    checkPipelineErrors(m);
    return pipeline;
}
} // namespace

Pipeline Device::createPipeline(const PipelineDesc& desc) {
    m->pipelines.push_back(buildPipeline(*m, desc));
    m->computePipelines.emplace_back();
    return {static_cast<u32>(m->pipelines.size())};
}

bool Device::recreatePipeline(Pipeline pipeline, const PipelineDesc& desc) {
    if (!m || m->inFrame || !pipeline || pipeline.id > m->pipelines.size() || !m->pipelines[pipeline.id - 1]) return false;
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
    m->readTimers(m->timerFrames[m->frameIndex]); // this frame index's previous timestamps (kFramesInFlight frames ago)
    Impl::TimerFrame& timers = m->timerFrames[m->frameIndex];
    timers.queries = 0;
    timers.marks.fill(Impl::kNoQuery);
    timers.pendingBegins = 1; // frame start: the first pass
    timers.passEnd = Impl::kNoQuery;
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

    Impl::TimerFrame& timers = m->timerFrames[m->frameIndex];
    if (m->timestamps && timers.queries) {
        timers.marks[1] = timers.passEnd; // frame end: the end of the last pass
        const u32 first = m->frameIndex * Impl::kQueriesPerFrame;
        m->encoder.ResolveQuerySet(m->querySet, first, timers.queries, m->queryResolve, static_cast<u64>(first) * 8);
        m->encoder.CopyBufferToBuffer(m->queryResolve, static_cast<u64>(first) * 8, timers.readback, 0, Impl::kQueriesPerFrame * 8);
    }
    const wgpu::CommandBuffer commands = m->encoder.Finish();
    m->encoder = nullptr;
    m->queue.Submit(1, &commands);
    if (m->timestamps && timers.queries) {
        timers.mapped = timers.readback.MapAsync(wgpu::MapMode::Read, 0, Impl::kQueriesPerFrame * 8, wgpu::CallbackMode::WaitAnyOnly,
                                                 [](wgpu::MapAsyncStatus, wgpu::StringView) {});
        timers.pending = true;
    }
#ifdef __EMSCRIPTEN__
    mkrhiWaitAnimationFrame();
#else
    m->surface.Present();
#endif
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

const GpuTimes& Device::gpuTimes() const { return m->gpuTimes; }
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

// WebGPU orders and synchronizes resource use itself. The state is kept only for storage textures: in UnorderedAccess they are bound
// for writing in dispatches (and left out of group 2 there)
// Group 2's textures for the next draws and dispatches (Wgpu.h kBound2D etc.): direct ones by kind, in order; extras are dropped
void CommandList::useTextures(const u32* indices, u32 count) {
    Device::Impl* m = m_device->impl();
    std::array<u32, kBoundTextures> next{};
    u32 used2D = 0;
    bool dropped = false;
    for (u32 i = 0; i < count; ++i) {
        const u32 index = indices[i];
        if (index == ~0u || ((index >> 19) & 31u) != kDirectPool) continue;
        const u32 id = m->directOwner[(index >> 11) & 0xFFu];
        if (!id) continue;
        if (std::find(next.begin(), next.end(), id) != next.end()) continue;
        const Device::Impl::TextureEntry& t = m->textures[id - 1];
        u32* slot = t.depth ? &next[kBound2D + kBoundCubes] : t.cube ? &next[kBound2D] : used2D < kBound2D ? &next[used2D++] : nullptr;
        if (!slot || *slot) {
            dropped = true;
            continue;
        }
        *slot = id;
    }
    static bool warned = false;
    if (dropped && !warned) {
        warned = true;
        MKRHI_WARN("[WebGPU] useTextures: more direct textures than group 2 binds (%u 2D, %u cube, %u depth); the rest read wrong",
                   kBound2D, kBoundCubes, kBoundDepths);
    }
    m->bound = next;
}

void CommandList::textureBarrier(Texture texture, TextureState, TextureState after) {
    Device::Impl* m = m_device->impl();
    m->endPass(); // copies run outside passes
    if (!texture || texture.id > m->textures.size()) return;
    Device::Impl::TextureEntry& t = m->textures[texture.id - 1];
    const bool unordered = after == TextureState::UnorderedAccess;
    if (t.storage && t.unordered != unordered) {
        t.unordered = unordered;
        m->computeGroupsDirty = true;
    }
}

void CommandList::dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) {
    Device::Impl* m = m_device->impl();
    if (!m->inFrame || !m->pipeline || m->pipeline > m->computePipelines.size() || !m->computePipelines[m->pipeline - 1]) return;
    m->endPass();
    m->buildComputeGroups();
    if (m->group0Dirty) m->buildGroup0();
    wgpu::ComputePassDescriptor cpd;
    cpd.timestampWrites = m->nextPassTimestamps();
    wgpu::ComputePassEncoder pass = m->encoder.BeginComputePass(&cpd);
    pass.SetPipeline(m->computePipelines[m->pipeline - 1]);
    pass.SetBindGroup(0, m->group0);
    pass.SetBindGroup(1, m->group1);
    pass.SetBindGroup(2, m->group2(0, true));
    pass.SetBindGroup(3, m->computeGroup3);
    if (m->pushBytes) pass.SetImmediates(0, m->push, align(m->pushBytes, 4));
    pass.DispatchWorkgroups(groupsX, groupsY, groupsZ);
    pass.End();
}
// The state picks where a storage buffer is bound: group 0 for reading (ShaderRead) or group 3 for writing (UnorderedAccess)
void CommandList::bufferBarrier(Buffer buffer, BufferState, BufferState after) {
    Device::Impl* m = m_device->impl();
    m->endPass();
    if (!buffer || buffer.id > m->buffers.size()) return;
    Device::Impl::BufferEntry& b = m->buffers[buffer.id - 1];
    const bool unordered = after == BufferState::UnorderedAccess;
    if (b.storage && b.unordered != unordered) {
        b.unordered = unordered;
        m->group0Dirty = m->computeGroupsDirty = true;
    }
}
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

// A section runs from the start of the next pass to the end of the current or last one (WebGPU times passes only)
void CommandList::beginTimer(u32 timer) {
    Device::Impl* m = m_device->impl();
    if (timer >= kMaxGpuTimers || !m->inFrame || !m->timestamps) return;
    m->timerFrames[m->frameIndex].pendingBegins |= 1u << (2 + 2 * timer);
}

void CommandList::endTimer(u32 timer) {
    Device::Impl* m = m_device->impl();
    if (timer >= kMaxGpuTimers || !m->inFrame || !m->timestamps) return;
    Device::Impl::TimerFrame& f = m->timerFrames[m->frameIndex];
    if (f.pendingBegins >> (2 + 2 * timer) & 1) { // no pass ran in the section
        f.pendingBegins &= ~(1u << (2 + 2 * timer));
        return;
    }
    if (f.marks[2 + 2 * timer] != Device::Impl::kNoQuery) f.marks[3 + 2 * timer] = f.passEnd;
}

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
