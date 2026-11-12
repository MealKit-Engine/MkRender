#include "App.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (first layout)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace mkrender::tool {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr int kGridSize = 5; // material spheres: metallic rows x roughness columns

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

Quat yawQuat(f32 yaw) { return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)}; }

Vec3 hue(f32 h) { // saturated color around the color wheel (point lights)
    const auto c = [&](f32 offset) { return std::clamp(std::abs(std::fmod(h * 6.0f + offset, 6.0f) - 3.0f) - 1.0f, 0.0f, 1.0f); };
    return {c(0.0f), c(4.0f), c(2.0f)};
}

struct CaptureJob {
    App* app;
    std::string path;
};

void saveCapture(const Image& image, void* user) {
    auto* job = static_cast<CaptureJob*>(user);
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(image.width), static_cast<int>(image.height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<u8*>(image.rgba.data()), static_cast<int>(image.width * 4));
    const bool ok = surface && SDL_SavePNG(surface, job->path.c_str());
    SDL_DestroySurface(surface);
    job->app->log(ok ? "Saved " + job->path : "Cannot save " + job->path + ": " + SDL_GetError());
    delete job;
}

bool slider(const char* label, f32* v, f32 lo, f32 hi, const char* format = "%.2f") { return ImGui::SliderFloat(label, v, lo, hi, format); }

} // namespace

bool App::init(void* sdlWindow, Renderer& renderer) {
    m_window = sdlWindow;
    m_renderer = &renderer;
    m_floor = renderer.createMesh(makePlane(1.0f, 0));
    m_shadows = renderer.shadows();
    m_postOptions = renderer.postOptions();
    m_params.fogDensity = 0.0f;
    m_params.post.bloomIntensity = 0.15f;
    showSpheres();
    return true;
}

void App::shutdown() {}

void App::log(const std::string& line) {
    SDL_Log("%s", line.c_str());
    m_log.push_back(line);
    while (m_log.size() > 200) m_log.pop_front();
    m_status = line;
    m_statusTime = m_time;
}

std::string App::title() const { return m_name.empty() ? "MkRender" : m_name + " - MkRender"; }

void App::setPending(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pending = path;
}

void App::openPending() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        path.swap(m_pending);
    }
    if (!path.empty()) openModel(path);
}

void App::setLightingModel(LightingModel model) {
    m_lighting = model;
    m_renderer->setLightingModel(model);
}

void App::upload(ModelData& model, const std::string& name) {
    Renderer& r = *m_renderer;
    r.waitIdle();
    // Textures -> bindless indices, materials -> renderer indices, vertices point at renderer materials
    std::vector<u32> textures;
    for (const TextureImage& image : model.images) {
        mkrhi::TextureInit init;
        const std::vector<u8> mips = buildMips(image, init.mipLevels);
        init.width = image.width;
        init.height = image.height;
        init.format = image.srgb ? mkrhi::Format::Rgba8Srgb : mkrhi::Format::Rgba8Unorm;
        init.data = mips.data();
        textures.push_back(r.createTexture(init));
    }
    const auto tex = [&](u32 i) { return i < textures.size() ? textures[i] : kNoTexture; };
    m_materials.clear();
    for (size_t i = 0; i < model.materials.size(); ++i) {
        MaterialData d = model.materials[i];
        d.baseColorTexture = tex(d.baseColorTexture);
        d.normalTexture = tex(d.normalTexture);
        d.metallicRoughnessTexture = tex(d.metallicRoughnessTexture);
        d.emissiveTexture = tex(d.emissiveTexture);
        Material m;
        m.name = i < model.materialNames.size() ? model.materialNames[i] : "material " + std::to_string(i);
        m.data = d;
        m.index = r.createMaterial(d);
        m_materials.push_back(m);
    }
    for (Vertex& v : model.mesh.vertices) v.material = static_cast<u16>(v.material < m_materials.size() ? m_materials[v.material].index : 0);
    const Vec3 center = (model.boundsMin + model.boundsMax) * 0.5f;
    m_objects.clear();
    m_objects.push_back({r.createMesh(model.mesh), Vec3{-center.x, -model.boundsMin.y, -center.z}});
    m_boundsMin = model.boundsMin - center;
    m_boundsMax = model.boundsMax - center;
    m_boundsMax.y -= m_boundsMin.y;
    m_boundsMin.y = 0.0f;
    m_vertexCount = static_cast<u32>(model.mesh.vertices.size());
    m_triangleCount = static_cast<u32>(model.mesh.indices.size() / 3);
    m_textureCount = static_cast<u32>(model.images.size());
    m_warnings = model.warnings;
    m_name = name;
    m_selected = 0;
    m_camYaw = 0.6f; // three-quarter view from the front (glTF models face +z, i.e. toward a camera on -z after the flip)
    m_camPitch = 0.3f;
    frameCamera();
}

bool App::openModel(const std::string& path) {
    ModelData model;
    std::string error;
    if (!loadGltf(path, model, error)) {
        log("Cannot open " + path + ": " + error);
        return false;
    }
    m_path = path;
    upload(model, utf8Of(pathOf(path).filename()));
    log("Opened " + path + " (" + std::to_string(m_triangleCount) + " triangles, " + std::to_string(m_materials.size()) + " materials)");
    for (const std::string& w : m_warnings) log("  " + w);
    return true;
}

void App::showSpheres() {
    Renderer& r = *m_renderer;
    r.waitIdle();
    m_materials.clear();
    m_objects.clear();
    m_warnings.clear();
    const MeshData sphere = makeSphere(0.4f, 48, 24, 0);
    constexpr f32 kSpacing = 1.0f;
    for (int row = 0; row < kGridSize; ++row)
        for (int col = 0; col < kGridSize; ++col) {
            Material m;
            m.data.baseColor[0] = 0.85f;
            m.data.baseColor[1] = 0.32f;
            m.data.baseColor[2] = 0.18f;
            m.data.metallic = static_cast<f32>(row) / (kGridSize - 1);
            m.data.roughness = std::max(0.05f, static_cast<f32>(col) / (kGridSize - 1));
            char name[64];
            std::snprintf(name, sizeof(name), "metallic %.2f, roughness %.2f", m.data.metallic, m.data.roughness);
            m.name = name;
            m.index = r.createMaterial(m.data);
            m_materials.push_back(m);
            MeshData mesh = sphere;
            for (Vertex& v : mesh.vertices) v.material = static_cast<u16>(m.index);
            const f32 x = (static_cast<f32>(col) - (kGridSize - 1) * 0.5f) * kSpacing;
            const f32 y = 0.45f + static_cast<f32>(kGridSize - 1 - row) * kSpacing;
            m_objects.push_back({r.createMesh(mesh), Vec3{x, y, 0.0f}});
        }
    const f32 half = (kGridSize - 1) * 0.5f * kSpacing + 0.45f;
    m_boundsMin = {-half, 0.0f, -0.45f};
    m_boundsMax = {half, kGridSize * kSpacing, 0.45f};
    m_vertexCount = static_cast<u32>(sphere.vertices.size() * m_objects.size());
    m_triangleCount = static_cast<u32>(sphere.indices.size() / 3 * m_objects.size());
    m_textureCount = 0;
    m_name = "Material spheres";
    m_path.clear();
    m_selected = 0;
    frameCamera();
    m_camYaw = 0.0f;
    m_camPitch = 0.1f;
}

void App::frameCamera() {
    const Vec3 size = m_boundsMax - m_boundsMin;
    const f32 radius = std::max(0.05f, length(size) * 0.5f);
    m_target = (m_boundsMin + m_boundsMax) * 0.5f;
    m_distance = radius / std::sin(m_fov * kPi / 180.0f * 0.5f) * 1.3f;
}

FrameParams App::frameParams() const {
    FrameParams p = m_params;
    p.viewport = m_view;
    const f32 w = m_view.width > 0 ? m_view.width : static_cast<f32>(m_renderer->width());
    const f32 h = m_view.height > 0 ? m_view.height : static_cast<f32>(m_renderer->height());
    const Vec3 forward{std::cos(m_camPitch) * std::sin(m_camYaw), -std::sin(m_camPitch), std::cos(m_camPitch) * std::cos(m_camYaw)};
    const Vec3 eye = m_target - forward * m_distance;
    const f32 nearPlane = std::max(0.01f, m_distance * 0.01f);
    p.viewProj = lookAtLH(eye, m_target, {0, 1, 0}) * perspectiveReversedZ(m_fov * kPi / 180.0f, w / std::max(1.0f, h), nearPlane, m_distance * 100.0f + 100.0f);
    p.cameraPos = eye;
    p.cameraForward = forward;
    p.sunDirection = {std::cos(m_sunElevation) * std::sin(m_sunAzimuth), std::sin(m_sunElevation), std::cos(m_sunElevation) * std::cos(m_sunAzimuth)};
    p.sunColor = m_sunTint * m_sunIntensity;
    p.time = static_cast<f32>(m_time);
    p.lights = m_lights.data();
    p.lightCount = static_cast<u32>(m_lights.size());
    return p;
}

void App::fill(DrawList& list) const {
    const Quat rotation = yawQuat(m_yaw);
    u32 flags = 0;
    if (m_dissolve > 0.0f) flags = instanceDissolve(flags, m_dissolve);
    if (m_flash > 0.0f) flags = instanceFlash(flags, m_flash);
    for (const Object& o : m_objects) {
        InstanceData inst;
        inst.position = rotate(rotation, o.offset);
        inst.rotation = rotation;
        inst.flags = flags;
        list.add(o.mesh, inst);
    }
    if (m_showFloor) {
        const Vec3 size = m_boundsMax - m_boundsMin;
        InstanceData floor;
        const f32 half = std::max({size.x, size.z, 1.0f}) * 2.0f;
        floor.scale = {half, 1.0f, half};
        floor.flags = kInstanceChecker;
        floor.color = rgba(150, 150, 150);
        list.add(m_floor, floor);
    }
}

void App::saveScreenshot(const std::string& path, bool withUi) {
    m_hideUiThisFrame = !withUi;
    m_renderer->requestCapture(saveCapture, new CaptureJob{this, path});
}

void App::afterFrame() { m_hideUiThisFrame = false; }

void App::cameraInput() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;
    const ImVec2 mouse = io.MousePos;
    const f32 scale = io.DisplayFramebufferScale.x;
    const bool inside = mouse.x * scale >= m_view.x && mouse.y * scale >= m_view.y && mouse.x * scale < m_view.x + m_view.width &&
                        mouse.y * scale < m_view.y + m_view.height;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyShift) {
        m_camYaw += io.MouseDelta.x * 0.008f;
        m_camPitch = std::clamp(m_camPitch + io.MouseDelta.y * 0.008f, -1.5f, 1.5f);
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
        (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift)) {
        const Vec3 forward{std::cos(m_camPitch) * std::sin(m_camYaw), -std::sin(m_camPitch), std::cos(m_camPitch) * std::cos(m_camYaw)};
        const Vec3 right = normalize(cross({0, 1, 0}, forward));
        const Vec3 up = cross(forward, right);
        const f32 k = m_distance * 0.0015f;
        m_target += right * (-io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
    }
    if (inside && io.MouseWheel != 0.0f) m_distance = std::clamp(m_distance * std::pow(0.88f, io.MouseWheel), 0.02f, 5000.0f);
    if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F)) frameCamera();
}

void App::frame(f64 dt) {
    m_time += dt;
    m_fps = m_fps <= 0.0f ? 60.0f : m_fps * 0.95f + 0.05f / static_cast<f32>(std::max(dt, 1e-4));
    if (m_turntable) m_yaw = std::fmod(m_yaw + m_turntableSpeed * static_cast<f32>(dt), 2.0f * kPi);

    // Point lights circling the model
    m_lights.clear();
    const Vec3 size = m_boundsMax - m_boundsMin;
    const f32 ring = std::max(0.5f, std::max(size.x, size.z) * 0.6f);
    for (i32 i = 0; i < m_lightCount; ++i) {
        const f32 a = static_cast<f32>(m_time) * 0.5f + 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(m_lightCount);
        PointLight l;
        l.position = {std::cos(a) * ring, std::max(0.3f, size.y * 0.5f), std::sin(a) * ring};
        l.range = m_lightRange;
        l.color = hue(static_cast<f32>(i) / static_cast<f32>(m_lightCount)) * m_lightIntensity;
        m_lights.push_back(l);
    }

    const ImGuiID dockspace = ImGui::GetID("MkRenderDock");
    if (!m_layoutDone) {
        m_layoutDone = true;
        if (!ImGui::DockBuilderGetNode(dockspace)) layoutDocks(dockspace);
    }
    drawMenu();
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    // 3D goes in the central node (framebuffer pixels)
    const ImGuiIO& io = ImGui::GetIO();
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace)) {
        const f32 s = io.DisplayFramebufferScale.x;
        m_view = {central->Pos.x * s, central->Pos.y * s, std::max(1.0f, central->Size.x * s), std::max(1.0f, central->Size.y * s)};
        const f32 maxW = static_cast<f32>(m_renderer->width()), maxH = static_cast<f32>(m_renderer->height());
        m_view.width = std::min(m_view.width, maxW - m_view.x);
        m_view.height = std::min(m_view.height, maxH - m_view.y);
    }
    drawScene();
    drawLighting();
    drawPost();
    drawMaterials();
    drawStats();
    cameraInput();
    if (!m_status.empty() && m_time - m_statusTime < 4.0) {
        ImGui::SetNextWindowPos({m_view.x / io.DisplayFramebufferScale.x + 10.0f, (m_view.y + m_view.height) / io.DisplayFramebufferScale.y - 10.0f}, 0,
                                {0.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                                              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(m_status.c_str());
        ImGui::End();
    }
}

void App::layoutDocks(unsigned dockspace) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, vp->WorkSize);
    ImGuiID rest = dockspace;
    const ImGuiID right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.3f, nullptr, &rest);
    ImGuiID rightTop = right;
    const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(rightTop, ImGuiDir_Down, 0.35f, nullptr, &rightTop);
    ImGui::DockBuilderDockWindow("Scene", rightTop);
    ImGui::DockBuilderDockWindow("Lighting", rightTop);
    ImGui::DockBuilderDockWindow("Post", rightTop);
    ImGui::DockBuilderDockWindow("Materials", rightTop);
    ImGui::DockBuilderDockWindow("Stats", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void App::drawMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open model...", "Ctrl+O")) {
            static const SDL_DialogFileFilter filters[] = {{"glTF", "gltf;glb"}};
            SDL_ShowOpenFileDialog(
                [](void* user, const char* const* files, int) {
                    if (files && files[0]) static_cast<App*>(user)->setPending(files[0]);
                },
                this, static_cast<SDL_Window*>(m_window), filters, 1, nullptr, false);
        }
        if (ImGui::MenuItem("Reload", "F5", false, !m_path.empty())) openModel(m_path);
        if (ImGui::MenuItem("Material spheres")) showSpheres();
        ImGui::Separator();
        if (ImGui::MenuItem("Save screenshot (3D only)")) {
            if (char* pics = SDL_GetPrefPath("MealKit", "MkRender")) {
                char name[64];
                std::snprintf(name, sizeof(name), "screenshot_%llu.png", static_cast<unsigned long long>(SDL_GetTicks()));
                saveScreenshot(std::string(pics) + name, false);
                SDL_free(pics);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame model", "F")) frameCamera();
        if (ImGui::MenuItem("Reset layout")) layoutDocks(ImGui::GetID("MkRenderDock"));
        ImGui::EndMenu();
    }
    ImGui::TextDisabled("   %s  |  %s", mkrhi::backendName(), m_renderer->adapterName());
    ImGui::EndMainMenuBar();
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        static const SDL_DialogFileFilter filters[] = {{"glTF", "gltf;glb"}};
        SDL_ShowOpenFileDialog([](void* user, const char* const* files, int) { if (files && files[0]) static_cast<App*>(user)->setPending(files[0]); },
                               this, static_cast<SDL_Window*>(m_window), filters, 1, nullptr, false);
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F5) && !m_path.empty()) openModel(m_path);
}

void App::drawScene() {
    if (ImGui::Begin("Scene")) {
        ImGui::TextUnformatted(m_name.c_str());
        if (!m_path.empty()) ImGui::TextDisabled("%s", m_path.c_str());
        ImGui::Text("%u vertices, %u triangles, %zu materials, %u textures", m_vertexCount, m_triangleCount, m_materials.size(), m_textureCount);
        const Vec3 size = m_boundsMax - m_boundsMin;
        ImGui::Text("Size %.2f x %.2f x %.2f m", size.x, size.y, size.z);
        if (ImGui::Button("Open model...")) {
            static const SDL_DialogFileFilter filters[] = {{"glTF", "gltf;glb"}};
            SDL_ShowOpenFileDialog([](void* user, const char* const* files, int) { if (files && files[0]) static_cast<App*>(user)->setPending(files[0]); },
                                   this, static_cast<SDL_Window*>(m_window), filters, 1, nullptr, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Material spheres")) showSpheres();
        ImGui::TextDisabled("Or drop a .gltf/.glb file on the window");
        ImGui::SeparatorText("View");
        ImGui::Checkbox("Floor", &m_showFloor);
        ImGui::Checkbox("Turntable", &m_turntable);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        slider("##speed", &m_turntableSpeed, -2.0f, 2.0f, "%.2f rad/s");
        f32 yawDeg = m_yaw * 180.0f / kPi;
        if (ImGui::SliderFloat("Model rotation", &yawDeg, 0.0f, 360.0f, "%.0f deg")) m_yaw = yawDeg * kPi / 180.0f;
        slider("Field of view", &m_fov, 10.0f, 100.0f, "%.0f deg");
        if (ImGui::Button("Frame (F)")) frameCamera();
        ImGui::TextDisabled("Left drag: orbit, right/middle or Shift+left: pan, wheel: zoom");
        ImGui::SeparatorText("Instance effects");
        slider("Dissolve", &m_dissolve, 0.0f, 1.0f);
        ImGui::SetItemTooltip("Only materials with the Dissolve feature");
        slider("Flash", &m_flash, 0.0f, 1.0f);
        if (!m_warnings.empty()) {
            ImGui::SeparatorText("Warnings");
            for (const std::string& w : m_warnings) ImGui::BulletText("%s", w.c_str());
        }
    }
    ImGui::End();
}

void App::drawLighting() {
    if (ImGui::Begin("Lighting")) {
        int model = static_cast<int>(m_lighting);
        if (ImGui::Combo("Lighting model", &model, "PBR\0Toon\0Unlit\0")) setLightingModel(static_cast<LightingModel>(model));
        ImGui::SeparatorText("Sun");
        f32 az = m_sunAzimuth * 180.0f / kPi, el = m_sunElevation * 180.0f / kPi;
        if (ImGui::SliderFloat("Azimuth", &az, -180.0f, 180.0f, "%.0f deg")) m_sunAzimuth = az * kPi / 180.0f;
        if (ImGui::SliderFloat("Elevation", &el, -10.0f, 90.0f, "%.0f deg")) m_sunElevation = el * kPi / 180.0f;
        ImGui::ColorEdit3("Sun color", &m_sunTint.x);
        slider("Sun intensity", &m_sunIntensity, 0.0f, 8.0f);
        ImGui::SeparatorText("Sky");
        ImGui::ColorEdit3("Sky", &m_params.skyColor.x, ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Ground", &m_params.groundColor.x, ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Fog / background", &m_params.fogColor.x, ImGuiColorEditFlags_Float);
        slider("Fog density", &m_params.fogDensity, 0.0f, 0.1f, "%.4f");
        ImGui::SeparatorText("Shadows");
        bool changed = ImGui::Checkbox("Sun shadows", &m_shadows.enabled);
        int cascades = static_cast<int>(m_shadows.cascades);
        if (ImGui::SliderInt("Cascades", &cascades, 1, static_cast<int>(kMaxShadowCascades))) {
            m_shadows.cascades = static_cast<u32>(cascades);
            changed = true;
        }
        static const u32 sizes[] = {512, 1024, 2048, 4096};
        int sizeIndex = 0;
        for (int i = 0; i < 4; ++i)
            if (sizes[i] == m_shadows.size) sizeIndex = i;
        if (ImGui::Combo("Map size", &sizeIndex, "512\0" "1024\0" "2048\0" "4096\0")) {
            m_shadows.size = sizes[sizeIndex];
            changed = true;
        }
        changed |= slider("Distance", &m_shadows.distance, 2.0f, 200.0f, "%.0f m");
        if (changed) m_renderer->setShadows(m_shadows);
        ImGui::SeparatorText("Point lights");
        ImGui::SliderInt("Count", &m_lightCount, 0, 16);
        slider("Range", &m_lightRange, 0.5f, 30.0f, "%.1f m");
        slider("Intensity", &m_lightIntensity, 0.0f, 20.0f);
        int perObject = static_cast<int>(m_lightsPerObject);
        if (ImGui::SliderInt("Lights per object", &perObject, 1, static_cast<int>(kMaxLightsPerObject))) {
            m_lightsPerObject = static_cast<u32>(perObject);
            m_renderer->setLightsPerObject(m_lightsPerObject);
        }
        ImGui::SeparatorText("Wind");
        slider("Wind strength", &m_params.windStrength, 0.0f, 4.0f);
    }
    ImGui::End();
}

void App::drawPost() {
    if (ImGui::Begin("Post")) {
        PostParams& p = m_params.post;
        ImGui::Checkbox("Tone mapping (AgX)", &p.toneMapping);
        ImGui::Checkbox("Punchy look", &p.punchy);
        slider("Exposure", &p.exposure, -4.0f, 4.0f, "%.2f EV");
        ImGui::SeparatorText("Bloom");
        slider("Intensity##bloom", &p.bloomIntensity, 0.0f, 1.0f);
        slider("Threshold", &p.bloomThreshold, 0.0f, 8.0f);
        ImGui::ColorEdit3("Tint", &p.bloomTint.x, ImGuiColorEditFlags_Float);
        ImGui::SeparatorText("Lens");
        slider("Vignette", &p.vignette, 0.0f, 1.0f);
        slider("Vignette smoothness", &p.vignetteSmoothness, 0.0f, 1.0f);
        slider("Film grain", &p.grain, 0.0f, 1.0f);
        slider("Chromatic aberration", &p.chromaticAberration, 0.0f, 0.03f, "%.3f");
        ImGui::SeparatorText("Screen tint");
        ImGui::ColorEdit3("Tint color", &m_params.tintColor.x, ImGuiColorEditFlags_Float);
        slider("Tint amount", &m_params.tintAmount, 0.0f, 1.0f);
        ImGui::SeparatorText("Player options");
        bool changed = ImGui::Checkbox("Bloom", &m_postOptions.bloom);
        changed |= ImGui::Checkbox("Anti-aliasing (FXAA)", &m_postOptions.antiAliasing);
        changed |= ImGui::Checkbox("Film grain##opt", &m_postOptions.filmGrain);
        changed |= ImGui::Checkbox("Chromatic aberration##opt", &m_postOptions.chromaticAberration);
        changed |= slider("Brightness", &m_postOptions.brightness, 0.5f, 2.0f);
        int cb = static_cast<int>(m_postOptions.colorBlind);
        if (ImGui::Combo("Color blindness", &cb, "Off\0Protanopia\0Deuteranopia\0Tritanopia\0")) {
            m_postOptions.colorBlind = static_cast<ColorBlindMode>(cb);
            changed = true;
        }
        if (changed) m_renderer->setPostOptions(m_postOptions);
    }
    ImGui::End();
}

void App::drawMaterials() {
    if (ImGui::Begin("Materials")) {
        if (m_materials.empty()) ImGui::TextDisabled("No materials");
        if (ImGui::BeginListBox("##materials", {-1.0f, ImGui::GetTextLineHeightWithSpacing() * 6.0f})) {
            for (i32 i = 0; i < static_cast<i32>(m_materials.size()); ++i)
                if (ImGui::Selectable(m_materials[static_cast<size_t>(i)].name.c_str(), i == m_selected)) m_selected = i;
            ImGui::EndListBox();
        }
        if (m_selected >= 0 && m_selected < static_cast<i32>(m_materials.size())) {
            Material& m = m_materials[static_cast<size_t>(m_selected)];
            MaterialData& d = m.data;
            bool changed = false;
            ImGui::SeparatorText("Surface (linear)");
            changed |= ImGui::ColorEdit4("Base color", d.baseColor, ImGuiColorEditFlags_Float);
            changed |= slider("Metallic", &d.metallic, 0.0f, 1.0f);
            changed |= slider("Roughness", &d.roughness, 0.0f, 1.0f);
            changed |= ImGui::ColorEdit3("Emissive", d.emissive, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            changed |= slider("Normal scale", &d.normalScale, 0.0f, 2.0f);
            changed |= slider("Alpha cutoff", &d.alphaCutoff, 0.0f, 1.0f);
            ImGui::TextDisabled("Textures: base %s, normal %s, metal-rough %s, emissive %s", d.baseColorTexture != kNoTexture ? "yes" : "-",
                                d.normalTexture != kNoTexture ? "yes" : "-", d.metallicRoughnessTexture != kNoTexture ? "yes" : "-",
                                d.emissiveTexture != kNoTexture ? "yes" : "-");
            ImGui::SeparatorText("Style features");
            const auto feature = [&](const char* label, u32 bit) {
                bool on = (d.features & bit) != 0;
                if (ImGui::Checkbox(label, &on)) {
                    d.features = on ? d.features | bit : d.features & ~bit;
                    changed = true;
                }
                return on;
            };
            if (feature("Outline", kMaterialOutline)) {
                changed |= slider("Outline width", &d.outlineWidth, 0.0f, 0.1f, "%.3f m");
                changed |= ImGui::ColorEdit3("Outline color", d.outlineColor, ImGuiColorEditFlags_Float);
            }
            if (feature("Rim light", kMaterialRim)) {
                changed |= slider("Rim strength", &d.rimStrength, 0.0f, 2.0f);
                changed |= ImGui::ColorEdit3("Rim color", d.rimColor, ImGuiColorEditFlags_Float);
            }
            if (feature("Wind", kMaterialWind)) {
                changed |= slider("Wind sway", &d.windStrength, 0.0f, 1.0f, "%.2f m");
                ImGui::TextDisabled("Strength per vertex comes from vertex color alpha");
            }
            if (feature("Dissolve", kMaterialDissolve)) {
                changed |= slider("Edge width", &d.dissolveEdge, 0.0f, 0.3f);
                changed |= ImGui::ColorEdit3("Edge color", d.dissolveColor, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                ImGui::TextDisabled("Progress: Scene > Dissolve");
            }
            if (changed) m_renderer->updateMaterial(m.index, d);
        }
    }
    ImGui::End();
}

void App::drawStats() {
    if (ImGui::Begin("Stats")) {
        const RenderStats& s = m_renderer->stats();
        ImGui::Text("%.0f fps, %u draw calls, %u instances, %u culled", m_fps, s.drawCalls, s.instances, s.culled);
        if (s.gpuValid) {
            ImGui::Text("GPU %.2f ms:", s.gpuFrameMs);
            for (u32 i = 0; i < kGpuPassCount; ++i) {
                ImGui::SameLine();
                ImGui::Text("%s %.2f", gpuPassName(static_cast<GpuPass>(i)), s.gpuPassMs[i]);
            }
        } else {
            ImGui::TextDisabled("GPU time unknown");
        }
        const mkrhi::GpuMemory mem = m_renderer->gpuMemory();
        if (mem.budget) ImGui::Text("GPU memory %.0f / %.0f MB", mem.used / 1048576.0, mem.budget / 1048576.0);
        int view = static_cast<int>(m_params.debugView);
        static const char* names[] = {"Lit", "Wireframe", "Normals", "LOD", "Overdraw", "Budget", "Features"};
        if (ImGui::BeginCombo("View mode", names[view])) {
            for (int i = 0; i < static_cast<int>(DebugView::Count); ++i) {
                const bool ok = m_renderer->supportsDebugView(static_cast<DebugView>(i));
                if (ImGui::Selectable(names[i], i == view, ok ? 0 : ImGuiSelectableFlags_Disabled)) m_params.debugView = static_cast<DebugView>(i);
            }
            ImGui::EndCombo();
        }
        bool vsync = m_renderer->vsync();
        if (ImGui::Checkbox("VSync", &vsync)) m_renderer->setVsync(vsync);
        ImGui::SeparatorText("Log");
        if (ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_Borders)) {
            for (const std::string& line : m_log) ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace mkrender::tool
