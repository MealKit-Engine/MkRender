// Debug UI (Dear ImGui) WebGPU drawing backend, written here (the vendored ImGui has no WebGPU backend): one pipeline with its own
// WGSL, ImGui's texture requests (ImGuiBackendFlags_RendererHasTextures, the dynamic font atlas) and textures by ImTextureID = index
// + 1 into a list of bind groups. Draws into the overlay pass (after beginOverlay; no gamma conversion, like the other backends).
#include "Wgpu.h"

#include "mkrhi/Log.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace mkrhi {

struct Device::Impl::ImGuiState {
    wgpu::RenderPipeline pipeline;
    wgpu::BindGroupLayout layout;
    wgpu::Sampler sampler;
    std::vector<wgpu::BindGroup> groups; // ImTextureID - 1 (released entries stay null)
    std::vector<std::pair<u32, u64>> deviceTextures; // Texture::id -> ImTextureID (imguiTextureId)
    wgpu::Buffer vertices, indices;
    u64 vertexBytes = 0, indexBytes = 0;
};

namespace {

constexpr char kShader[] = R"(
requires immediate_address_space;
struct Transform { scale : vec2f, translate : vec2f }
var<immediate> transform : Transform;
struct Varyings {
    @builtin(position) position : vec4f,
    @location(0) color : vec4f,
    @location(1) uv : vec2f,
}
@vertex fn vs(@location(0) position : vec2f, @location(1) uv : vec2f, @location(2) color : vec4f) -> Varyings {
    var out : Varyings;
    out.position = vec4f(position * transform.scale + transform.translate, 0.0, 1.0);
    out.color = color;
    out.uv = uv;
    return out;
}
@group(0) @binding(0) var uiSampler : sampler;
@group(0) @binding(1) var uiTexture : texture_2d<f32>;
@fragment fn fs(in : Varyings) -> @location(0) vec4f { return in.color * textureSample(uiTexture, uiSampler, in.uv); }
)";

// A bind group for `view`, returned as its ImTextureID
u64 addGroup(Device::Impl& m, const wgpu::TextureView& view) {
    Device::Impl::ImGuiState& ui = *m.imgui;
    wgpu::BindGroupEntry entries[2];
    entries[0].binding = 0;
    entries[0].sampler = ui.sampler;
    entries[1].binding = 1;
    entries[1].textureView = view;
    wgpu::BindGroupDescriptor desc;
    desc.layout = ui.layout;
    desc.entryCount = 2;
    desc.entries = entries;
    ui.groups.push_back(m.device.CreateBindGroup(&desc));
    return ui.groups.size();
}

// ImGui's own textures (the font atlas): create, update a rectangle, destroy (after it went unused for kFramesInFlight frames)
void updateTexture(Device::Impl& m, ImTextureData* tex) {
    Device::Impl::ImGuiState& ui = *m.imgui;
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        if (tex->Status == ImTextureStatus_WantCreate) {
            IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
            wgpu::TextureDescriptor td;
            td.dimension = wgpu::TextureDimension::e2D;
            td.size = {static_cast<u32>(tex->Width), static_cast<u32>(tex->Height), 1};
            td.format = wgpu::TextureFormat::RGBA8Unorm;
            td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
            auto* texture = new wgpu::Texture(m.device.CreateTexture(&td));
            tex->BackendUserData = texture;
            tex->SetTexID(static_cast<ImTextureID>(addGroup(m, texture->CreateView())));
        }
        const bool all = tex->Status == ImTextureStatus_WantCreate;
        const int x = all ? 0 : tex->UpdateRect.x, y = all ? 0 : tex->UpdateRect.y;
        const int w = all ? tex->Width : tex->UpdateRect.w, h = all ? tex->Height : tex->UpdateRect.h;
        wgpu::TexelCopyTextureInfo dst;
        dst.texture = *static_cast<wgpu::Texture*>(tex->BackendUserData);
        dst.origin = {static_cast<u32>(x), static_cast<u32>(y), 0};
        wgpu::TexelCopyBufferLayout layout;
        layout.bytesPerRow = static_cast<u32>(tex->GetPitch());
        layout.rowsPerImage = static_cast<u32>(h);
        const wgpu::Extent3D size{static_cast<u32>(w), static_cast<u32>(h), 1};
        const size_t bytes = static_cast<size_t>(tex->GetPitch()) * (h - 1) + static_cast<size_t>(w) * tex->BytesPerPixel;
        m.queue.WriteTexture(&dst, tex->GetPixelsAt(x, y), bytes, &layout, &size);
        tex->SetStatus(ImTextureStatus_OK);
    }
    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= static_cast<int>(Device::kFramesInFlight)) {
        if (auto* texture = static_cast<wgpu::Texture*>(tex->BackendUserData)) {
            texture->Destroy();
            delete texture;
        }
        const u64 id = static_cast<u64>(tex->GetTexID());
        if (id && id <= ui.groups.size()) ui.groups[id - 1] = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

// Grows a vertex/index buffer to at least `bytes` (4-byte multiple: queue.WriteBuffer's rule)
void ensureBuffer(Device::Impl& m, wgpu::Buffer& buffer, u64& capacity, u64 bytes, wgpu::BufferUsage usage) {
    if (bytes <= capacity) return;
    capacity = std::max<u64>(bytes + bytes / 2, 64 * 1024);
    capacity = (capacity + 3) & ~u64{3};
    wgpu::BufferDescriptor bd;
    bd.size = capacity;
    bd.usage = usage | wgpu::BufferUsage::CopyDst;
    buffer = m.device.CreateBuffer(&bd);
}

} // namespace

bool imguiInit(Device& device) {
    Device::Impl* m = device.impl();
    m->imgui = new Device::Impl::ImGuiState();
    Device::Impl::ImGuiState& ui = *m->imgui;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "mkrhi_webgpu";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    wgpu::SamplerDescriptor sd;
    sd.magFilter = sd.minFilter = wgpu::FilterMode::Linear;
    sd.addressModeU = sd.addressModeV = sd.addressModeW = wgpu::AddressMode::ClampToEdge;
    ui.sampler = m->device.CreateSampler(&sd);
    wgpu::BindGroupLayoutEntry entries[2];
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].sampler.type = wgpu::SamplerBindingType::Filtering;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    wgpu::BindGroupLayoutDescriptor ld;
    ld.entryCount = 2;
    ld.entries = entries;
    ui.layout = m->device.CreateBindGroupLayout(&ld);
    wgpu::PipelineLayoutDescriptor pld;
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &ui.layout;
    pld.immediateSize = 16; // scale, translate
    const wgpu::PipelineLayout layout = m->device.CreatePipelineLayout(&pld);

    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = wgpu::StringView(kShader, sizeof(kShader) - 1);
    wgpu::ShaderModuleDescriptor md;
    md.nextInChain = &wgsl;
    const wgpu::ShaderModule module = m->device.CreateShaderModule(&md);

    wgpu::VertexAttribute attributes[3];
    attributes[0] = {nullptr, wgpu::VertexFormat::Float32x2, offsetof(ImDrawVert, pos), 0};
    attributes[1] = {nullptr, wgpu::VertexFormat::Float32x2, offsetof(ImDrawVert, uv), 1};
    attributes[2] = {nullptr, wgpu::VertexFormat::Unorm8x4, offsetof(ImDrawVert, col), 2};
    wgpu::VertexBufferLayout vertexLayout;
    vertexLayout.arrayStride = sizeof(ImDrawVert);
    vertexLayout.attributeCount = 3;
    vertexLayout.attributes = attributes;
    wgpu::BlendState blend;
    blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha};
    blend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha};
    wgpu::ColorTargetState color;
    color.format = m->swapFormat;
    color.blend = &blend;
    wgpu::FragmentState fragment;
    fragment.module = module;
    fragment.entryPoint = "fs";
    fragment.targetCount = 1;
    fragment.targets = &color;
    wgpu::RenderPipelineDescriptor pd;
    pd.layout = layout;
    pd.vertex.module = module;
    pd.vertex.entryPoint = "vs";
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vertexLayout;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.fragment = &fragment;
    ui.pipeline = m->device.CreateRenderPipeline(&pd);
    return static_cast<bool>(ui.pipeline);
}

void imguiShutdown(Device& device) {
    Device::Impl* m = device.impl();
    if (!m || !m->imgui) return;
    device.waitIdle();
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (auto* texture = static_cast<wgpu::Texture*>(tex->BackendUserData)) {
            texture->Destroy();
            delete texture;
            tex->SetTexID(ImTextureID_Invalid);
            tex->BackendUserData = nullptr;
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    delete m->imgui;
    m->imgui = nullptr;
}

void imguiNewFrame() {}

void imguiRender(Device& device) {
    Device::Impl* m = device.impl();
    ImDrawData* draw = ImGui::GetDrawData();
    if (!m->imgui || !draw || !m->pass) return;
    Device::Impl::ImGuiState& ui = *m->imgui;
    if (draw->Textures)
        for (ImTextureData* tex : *draw->Textures)
            if (tex->Status != ImTextureStatus_OK) updateTexture(*m, tex);
    const f32 fbW = draw->DisplaySize.x * draw->FramebufferScale.x, fbH = draw->DisplaySize.y * draw->FramebufferScale.y;
    if (fbW <= 0 || fbH <= 0 || draw->TotalVtxCount == 0) return;

    // Geometry: every list into one vertex and one index buffer (queue writes land before this frame's commands run)
    const u64 vertexBytes = static_cast<u64>(draw->TotalVtxCount) * sizeof(ImDrawVert);
    const u64 indexBytes = (static_cast<u64>(draw->TotalIdxCount) * sizeof(ImDrawIdx) + 3) & ~u64{3};
    ensureBuffer(*m, ui.vertices, ui.vertexBytes, vertexBytes, wgpu::BufferUsage::Vertex);
    ensureBuffer(*m, ui.indices, ui.indexBytes, indexBytes, wgpu::BufferUsage::Index);
    std::vector<u8> vertexData(vertexBytes), indexData(indexBytes, 0);
    u64 vo = 0, io = 0;
    for (const ImDrawList* list : draw->CmdLists) {
        std::memcpy(vertexData.data() + vo, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(indexData.data() + io, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vo += list->VtxBuffer.Size * sizeof(ImDrawVert);
        io += list->IdxBuffer.Size * sizeof(ImDrawIdx);
    }
    m->queue.WriteBuffer(ui.vertices, 0, vertexData.data(), vertexBytes);
    m->queue.WriteBuffer(ui.indices, 0, indexData.data(), indexBytes);

    const wgpu::RenderPassEncoder& pass = m->pass;
    pass.SetPipeline(ui.pipeline);
    pass.SetVertexBuffer(0, ui.vertices);
    pass.SetIndexBuffer(ui.indices, sizeof(ImDrawIdx) == 2 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32);
    pass.SetViewport(0, 0, fbW, fbH, 0, 1);
    const f32 l = draw->DisplayPos.x, r = l + draw->DisplaySize.x, t = draw->DisplayPos.y, b = t + draw->DisplaySize.y;
    const f32 transform[4] = {2.0f / (r - l), 2.0f / (t - b), (r + l) / (l - r), (t + b) / (b - t)};
    pass.SetImmediates(0, transform, sizeof(transform));

    const ImVec2 clipOff = draw->DisplayPos, clipScale = draw->FramebufferScale;
    const f32 passW = static_cast<f32>(m->passWidth), passH = static_cast<f32>(m->passHeight);
    u32 vertexBase = 0, indexBase = 0;
    for (const ImDrawList* list : draw->CmdLists) {
        for (const ImDrawCmd& c : list->CmdBuffer) {
            if (c.UserCallback) {
                if (c.UserCallback != ImDrawCallback_ResetRenderState) c.UserCallback(list, &c);
                continue;
            }
            const f32 x0 = std::clamp((c.ClipRect.x - clipOff.x) * clipScale.x, 0.0f, passW);
            const f32 y0 = std::clamp((c.ClipRect.y - clipOff.y) * clipScale.y, 0.0f, passH);
            const f32 x1 = std::clamp((c.ClipRect.z - clipOff.x) * clipScale.x, 0.0f, passW);
            const f32 y1 = std::clamp((c.ClipRect.w - clipOff.y) * clipScale.y, 0.0f, passH);
            if (x1 <= x0 || y1 <= y0) continue;
            const u64 id = static_cast<u64>(c.GetTexID());
            if (!id || id > ui.groups.size() || !ui.groups[id - 1]) continue;
            pass.SetBindGroup(0, ui.groups[id - 1]);
            pass.SetScissorRect(static_cast<u32>(x0), static_cast<u32>(y0), static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0));
            pass.DrawIndexed(c.ElemCount, 1, indexBase + c.IdxOffset, static_cast<i32>(vertexBase + c.VtxOffset));
        }
        vertexBase += static_cast<u32>(list->VtxBuffer.Size);
        indexBase += static_cast<u32>(list->IdxBuffer.Size);
    }
    m->applied = {}; // the renderer's next draw sets its pipeline, groups and state again
}

u64 imguiTextureId(Device& device, Texture texture) {
    Device::Impl* m = device.impl();
    if (!texture || !m->imgui || texture.id > m->textures.size()) return 0;
    for (const auto& [id, value] : m->imgui->deviceTextures)
        if (id == texture.id) return value;
    const Device::Impl::TextureEntry& t = m->textures[texture.id - 1];
    if (t.cube || t.depth) return 0; // ImGui shows 2D color textures
    wgpu::TextureView view = t.view;
    if (t.pooled) { // its layer of the pool array, from its first mip (the pool may grow later: this view keeps the old array)
        const u32 pool = t.index >> 19, bias = (t.index >> 8) & 7u;
        wgpu::TextureViewDescriptor vd;
        vd.dimension = wgpu::TextureViewDimension::e2D;
        vd.baseArrayLayer = t.slot;
        vd.arrayLayerCount = 1;
        vd.baseMipLevel = bias;
        vd.mipLevelCount = (t.index & 0xFFu) + 1;
        view = m->pools[pool].texture.CreateView(&vd);
    }
    const u64 value = addGroup(*m, view);
    m->imgui->deviceTextures.emplace_back(texture.id, value);
    return value;
}

} // namespace mkrhi
