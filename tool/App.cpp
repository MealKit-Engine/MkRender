#include "App.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (first layout)
#include <ImGuizmo.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace mkrender::tool {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kDeg = kPi / 180.0f;
constexpr const char* kDragEntity = "MKR_ENTITY";

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::filesystem::file_time_type fileTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(pathOf(path), ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

Quat axisAngle(Vec3 axis, f32 angle) {
    const f32 s = std::sin(angle * 0.5f);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5f)};
}

// Degrees (pitch x, yaw y, roll z), applied roll, then pitch, then yaw (q = yaw * pitch * roll)
Quat fromEuler(Vec3 deg) {
    return normalizeQuat(mul(mul(axisAngle({0, 1, 0}, deg.y * kDeg), axisAngle({1, 0, 0}, deg.x * kDeg)), axisAngle({0, 0, 1}, deg.z * kDeg)));
}

Vec3 toEuler(Quat q) {
    const f32 sinp = std::clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f);
    const f32 pitch = std::asin(sinp);
    const f32 yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const f32 roll = std::atan2(2.0f * (q.x * q.y + q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    const auto clean = [](f32 v) { return std::abs(v) < 1e-4f ? 0.0f : v; };
    return {clean(pitch / kDeg), clean(yaw / kDeg), clean(roll / kDeg)};
}

// The editor's gizmo matrices (modules/editor Editor.cpp compose/decompose): rows are the scaled axes, then the position
void compose(const Transform& e, f32 out[16]) {
    const Vec3 x = rotate(e.rotation, {1, 0, 0}) * e.scale.x;
    const Vec3 y = rotate(e.rotation, {0, 1, 0}) * e.scale.y;
    const Vec3 z = rotate(e.rotation, {0, 0, 1}) * e.scale.z;
    const f32 m[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, e.position.x, e.position.y, e.position.z, 1};
    std::memcpy(out, m, sizeof(m));
}

void decompose(const f32 m[16], Transform& e) {
    Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    const f32 sx = std::max(length(x), 0.01f), sy = std::max(length(y), 0.01f), sz = std::max(length(z), 0.01f);
    x = x / sx;
    y = y / sy;
    z = z / sz;
    const f32 r00 = x.x, r11 = y.y, r22 = z.z;
    const f32 r21 = y.z, r12 = z.y, r02 = z.x, r20 = x.z, r10 = x.y, r01 = y.x;
    Quat q;
    const f32 trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const f32 s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q = {0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
    } else if (r11 > r22) {
        const f32 s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q = {(r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q = {(r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s};
    }
    e.position = {m[12], m[13], m[14]};
    e.rotation = normalizeQuat(q);
    e.scale = {sx, sy, sz};
}

f32 srgbToLinear(u8 c) { return std::pow(c / 255.0f, 2.2f); }

void colorToFloats(u32 c, f32 out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<f32>((c >> (8 * i)) & 0xFFu) / 255.0f;
}

u32 floatsToColor(const f32 in[4]) {
    u32 c = 0;
    for (int i = 0; i < 4; ++i) c |= static_cast<u32>(std::lround(std::clamp(in[i], 0.0f, 1.0f) * 255.0f)) << (8 * i);
    return c;
}

bool parseFloat(const std::string& text, f32& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtof(text.c_str(), &end);
    return end && *end == '\0';
}

bool parseVec3(const std::string& text, Vec3& v) {
    std::istringstream in(text);
    return static_cast<bool>(in >> v.x >> v.y >> v.z);
}

bool inputString(const char* label, std::string& value) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (!ImGui::InputText(label, buf, sizeof(buf))) return false;
    value = buf;
    return true;
}

// Label on the left, widget filling the rest (names don't get cut off like with ImGui's right-hand labels)
void row(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.0f + ImGui::GetFontSize() * 7.5f);
    ImGui::SetNextItemWidth(-1.0f);
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

const SDL_DialogFileFilter kSceneFilters[] = {{"Scene", "scene"}, {"glTF model", "gltf;glb"}};

} // namespace

// ---------------------------------------------------------------- Setup and files

bool App::init(void* sdlWindow, Renderer& renderer) {
    m_window = sdlWindow;
    m_renderer = &renderer;
    m_assets.init(renderer);
    newScene();
    return true;
}

void App::shutdown() {}

void App::log(const std::string& line) {
    SDL_Log("%s", line.c_str());
    m_log.push_back(line);
    while (m_log.size() > 300) m_log.pop_front();
    m_status = line;
    m_statusTime = m_time;
}

std::string App::title() const {
    std::string name = m_path.empty() ? "untitled.scene" : utf8Of(pathOf(m_path).filename());
    if (dirty()) name += " *";
    if (!m_project.empty()) name += " - " + utf8Of(pathOf(m_project).filename());
    return name + " - MkRender";
}

bool App::dirty() const { return writeScene(m_scene) != m_savedText; }

std::string App::projectSetting(const std::string& key) const {
    if (m_project.empty()) return {};
    std::ifstream in(pathOf(m_project) / "mealkit.project");
    std::string lineText;
    while (std::getline(in, lineText)) {
        if (!lineText.empty() && lineText.back() == '\r') lineText.pop_back();
        if (lineText.rfind(key + " ", 0) == 0) return lineText.substr(key.size() + 1);
    }
    return {};
}

void App::setLightingModel(LightingModel model) {
    m_lighting = model;
    m_renderer->setLightingModel(model);
}

bool App::openProject(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(pathOf(dir) / "mealkit.project", ec)) {
        log("Not a MealKit project (no mealkit.project): " + dir);
        return false;
    }
    m_project = utf8Of(std::filesystem::absolute(pathOf(dir), ec).lexically_normal());
    m_assets.setAssetsDir(utf8Of(pathOf(m_project) / "assets"));
    reloadKinds();
    const std::string model = projectSetting("lightingModel");
    setLightingModel(model == "Toon" ? LightingModel::Toon : model == "Unlit" ? LightingModel::Unlit : LightingModel::Pbr);
    log("Project " + m_project);
    const std::string main = projectSetting("scene");
    if (!main.empty()) return loadScenePath(m_assets.resolve(main));
    const std::vector<std::string> scenes = m_assets.files(".scene");
    if (!scenes.empty()) return loadScenePath(m_assets.resolve(scenes.front()));
    newScene();
    return true;
}

bool App::openFile(const std::string& path) {
    const std::string ext = lower(utf8Of(pathOf(path).extension()));
    // The project is the folder above the nearest "assets" folder
    std::error_code ec;
    for (std::filesystem::path p = std::filesystem::absolute(pathOf(path), ec).parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path()) {
        if (std::filesystem::is_regular_file(p / "mealkit.project", ec)) {
            if (utf8Of(p) != m_project) {
                m_project = utf8Of(p);
                m_assets.setAssetsDir(utf8Of(p / "assets"));
                reloadKinds();
                const std::string model = projectSetting("lightingModel");
                setLightingModel(model == "Toon" ? LightingModel::Toon : model == "Unlit" ? LightingModel::Unlit : LightingModel::Pbr);
            }
            break;
        }
    }
    if (ext == ".scene") return loadScenePath(path);
    if (ext == ".gltf" || ext == ".glb") {
        newScene();
        Entity& e = addEntity(kKindDecoration);
        e.name = utf8Of(pathOf(path).stem());
        e.model = m_assets.relative(utf8Of(std::filesystem::absolute(pathOf(path), ec)));
        e.color = 0xFFFFFFFF;
        e.position = {};
        m_lastSnapshot = writeScene(m_scene);
        m_undo.clear();
        frameSelected();
        return true;
    }
    log("Cannot open " + path + ": not a .scene, .gltf or .glb file");
    return false;
}

bool App::loadScenePath(const std::string& path) {
    Scene scene;
    std::string error;
    std::vector<std::string> warnings;
    if (!loadScene(path, scene, error, &warnings)) {
        log("Cannot open " + path + ": " + error);
        return false;
    }
    m_scene = std::move(scene);
    m_path = path;
    m_savedText = writeScene(m_scene);
    m_lastSnapshot = m_savedText;
    m_undo.clear();
    m_redo.clear();
    m_fileTime = fileTime(path);
    m_externalChange = false;
    m_warnings = warnings;
    m_selected = 0;
    log("Opened " + path + " (" + std::to_string(m_scene.entities.size()) + " objects)");
    for (const std::string& w : warnings) log("  " + w);
    // Look at everything drawn
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Entity& e : m_scene.entities) {
        const Transform w = worldTransform(m_scene, e);
        const Vec3 half = isMarker(e) || e.is(kKindScatter) ? Vec3{} : Vec3{std::abs(w.scale.x), std::abs(w.scale.y), std::abs(w.scale.z)} * 0.5f;
        lo = {std::min(lo.x, w.position.x - half.x), std::min(lo.y, w.position.y - half.y), std::min(lo.z, w.position.z - half.z)};
        hi = {std::max(hi.x, w.position.x + half.x), std::max(hi.y, w.position.y + half.y), std::max(hi.z, w.position.z + half.z)};
    }
    if (m_scene.entities.empty()) lo = hi = {};
    m_pivot = (lo + hi) * 0.5f;
    m_pitch = 0.6f;
    m_distance = std::clamp(length(hi - lo) * 0.5f / std::sin(m_fov * kDeg * 0.5f) * 0.8f, 5.0f, 600.0f);
    return true;
}

void App::newScene() {
    m_scene = Scene{};
    m_path.clear();
    m_savedText = writeScene(m_scene);
    m_lastSnapshot = m_savedText;
    m_undo.clear();
    m_redo.clear();
    m_warnings.clear();
    m_selected = 0;
    m_pivot = {};
    m_distance = 12.0f;
}

bool App::save() {
    if (m_path.empty()) {
        saveAs();
        return false;
    }
    std::string error;
    if (!saveScene(m_path, m_scene, error)) {
        log("Cannot save " + m_path + ": " + error);
        return false;
    }
    m_savedText = writeScene(m_scene);
    m_fileTime = fileTime(m_path);
    m_externalChange = false;
    log("Saved " + m_path);
    return true;
}

void App::saveAs() {
    std::string where = m_assets.assetsDir().empty() ? std::string() : utf8Of(pathOf(m_assets.assetsDir()) / "scenes");
    SDL_ShowSaveFileDialog(
        [](void* user, const char* const* files, int) {
            if (files && files[0]) static_cast<App*>(user)->setPending(std::string("save:") + files[0]);
        },
        this, static_cast<SDL_Window*>(m_window), kSceneFilters, 1, where.empty() ? nullptr : where.c_str());
}

void App::reloadFromDisk() {
    if (m_path.empty()) return;
    const u32 keep = m_selected;
    const Vec3 pivot = m_pivot;
    const f32 distance = m_distance;
    if (loadScenePath(m_path)) {
        m_pivot = pivot;
        m_distance = distance;
        if (m_scene.find(keep)) m_selected = keep;
    }
}

std::string App::kindsPath() const { return m_project.empty() ? std::string() : utf8Of(pathOf(m_project) / "build" / "editor" / "kinds.txt"); }

void App::reloadKinds() {
    const std::string path = kindsPath();
    m_kindsTime = path.empty() ? std::filesystem::file_time_type{} : fileTime(path);
    std::string error;
    if (m_kindsTime != std::filesystem::file_time_type{} && loadKinds(path, error)) {
        m_kindsLoaded = true;
        size_t game = 0;
        for (const KindInfo& k : kinds()) game += k.game ? 1 : 0;
        log("Object kinds: " + std::to_string(kinds().size()) + " (" + std::to_string(game) + " from the game) from " + path);
        return;
    }
    if (!error.empty()) log("Object kinds: " + error);
    resetKinds();
    m_kindsLoaded = false;
}

void App::checkExternalChange() {
    if (m_path.empty() || m_time < m_fileCheck) return;
    m_fileCheck = m_time + 1.0;
    const auto t = fileTime(m_path);
    if (t == m_fileTime || t == std::filesystem::file_time_type{}) return;
    if (!dirty()) {
        log("Reloaded (changed on disk)");
        reloadFromDisk();
    } else {
        m_externalChange = true;
    }
}

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
    if (path.empty()) return;
    if (path.rfind("save:", 0) == 0) {
        std::string target = path.substr(5);
        if (lower(utf8Of(pathOf(target).extension())) != ".scene") target += ".scene";
        m_path = target;
        save();
        return;
    }
    guarded(Pending::OpenPath, path); // "project:<folder>" or a file
}

// Runs now when nothing is edited, else drawSaveDialog asks first
void App::guarded(Pending action, const std::string& path) {
    m_pendingAction = action;
    m_pendingActionPath = path;
    if (!dirty()) runPending();
}

void App::runPending() {
    const Pending action = m_pendingAction;
    m_pendingAction = Pending::None;
    switch (action) {
    case Pending::Quit: m_quitConfirmed = true; break;
    case Pending::NewScene: newScene(); break;
    case Pending::OpenPath:
        if (m_pendingActionPath.rfind("project:", 0) == 0) openProject(m_pendingActionPath.substr(8));
        else openFile(m_pendingActionPath);
        break;
    case Pending::None: break;
    }
}

void App::requestQuit() { guarded(Pending::Quit); }

// ---------------------------------------------------------------- Editing

void App::snapshot() {
    const std::string text = writeScene(m_scene);
    if (text == m_lastSnapshot) return;
    m_undo.push_back(m_lastSnapshot);
    while (m_undo.size() > 200) m_undo.pop_front();
    m_redo.clear();
    m_lastSnapshot = text;
}

void App::undo(bool redo) {
    auto& from = redo ? m_redo : m_undo;
    auto& to = redo ? m_undo : m_redo;
    if (from.empty()) return;
    to.push_back(m_lastSnapshot);
    m_lastSnapshot = from.back();
    from.pop_back();
    std::string error;
    Scene scene;
    if (readScene(m_lastSnapshot, scene, error, nullptr)) m_scene = std::move(scene);
    if (!m_scene.find(m_selected)) m_selected = 0;
    m_eulerFor = 0;
}

Entity& App::addEntity(const std::string& kind) {
    const KindInfo* info = findKind(kind);
    Entity& e = m_scene.add(kind, info ? info->label : kind);
    e.position = m_pivot;
    e.fields = defaultFields(kind);
    if (kind == kKindSpawn) e.color = rgba(255, 220, 90);
    if (kind == kKindProbe || kind == kKindReverb) e.scale = {10, 5, 10};
    if (kind == kKindTerrain) e.scale = {64, 8, 64};
    m_selected = e.id;
    return e;
}

void App::duplicateSelected() {
    if (!m_scene.find(m_selected)) return;
    const std::vector<u32> ids = subtree(m_scene, m_selected);
    std::map<u32, u32> remap;
    for (u32 id : ids) remap[id] = m_scene.nextId++;
    std::vector<Entity> copies;
    for (u32 id : ids) {
        Entity e = *m_scene.find(id);
        e.id = remap[id];
        if (id != m_selected) e.parent = remap[e.parent];
        copies.push_back(std::move(e));
    }
    for (Entity& e : copies) m_scene.entities.push_back(std::move(e));
    m_selected = remap[m_selected];
}

void App::deleteSelected() {
    if (!m_scene.find(m_selected)) return;
    const std::vector<u32> ids = subtree(m_scene, m_selected);
    std::erase_if(m_scene.entities, [&](const Entity& e) { return std::find(ids.begin(), ids.end(), e.id) != ids.end(); });
    m_selected = 0;
}

void App::reparent(u32 id, u32 parent) {
    Entity* e = m_scene.find(id);
    if (!e || (parent && isDescendant(m_scene, parent, id))) return;
    const Transform world = worldTransform(m_scene, *e);
    e->parent = parent;
    if (parent) e->folder.clear();
    setWorldTransform(m_scene, *e, world);
}

void App::select(u32 id) {
    if (m_scene.find(id)) {
        m_selected = id;
        frameSelected();
    }
}

void App::frameSelected() {
    const Entity* e = m_scene.find(m_selected);
    if (!e) return;
    const Transform t = worldTransform(m_scene, *e);
    Vec3 lo, hi;
    const bool shape = entityBounds(*e, lo, hi);
    const Vec3 center = shape ? (lo + hi) * 0.5f : Vec3{};
    m_pivot = t.position + rotate(t.rotation, {center.x * t.scale.x, center.y * t.scale.y, center.z * t.scale.z});
    const Vec3 size = shape ? hi - lo : Vec3{1, 1, 1};
    const f32 radius = length(Vec3{size.x * t.scale.x, size.y * t.scale.y, size.z * t.scale.z}) * 0.5f;
    m_distance = std::clamp(radius / std::sin(m_fov * kDeg * 0.5f) * 1.2f, 1.0f, 1000.0f);
}

// ---------------------------------------------------------------- View math

Vec3 App::cameraForward() const { return {std::cos(m_pitch) * std::sin(m_yaw), -std::sin(m_pitch), std::cos(m_pitch) * std::cos(m_yaw)}; }
Vec3 App::cameraEye() const { return m_pivot - cameraForward() * m_distance; }
Mat4 App::viewMatrix() const { return lookAtLH(cameraEye(), m_pivot, {0, 1, 0}); }
Mat4 App::projMatrix() const {
    return perspectiveReversedZ(m_fov * kDeg, std::max(1.0f, m_view.width) / std::max(1.0f, m_view.height), 0.05f, 4000.0f);
}

bool App::project(Vec3 p, f32& x, f32& y) const {
    const Mat4 m = viewMatrix() * projMatrix();
    const f32 cx = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const f32 cy = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const f32 cw = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    if (cw <= 1e-4f) return false;
    x = m_uiX + (cx / cw * 0.5f + 0.5f) * m_uiW;
    y = m_uiY + (0.5f - cy / cw * 0.5f) * m_uiH;
    return true;
}

bool App::isMarker(const Entity& e) const {
    const KindInfo* k = findKind(e.kind);
    return k && k->marker;
}

bool App::isArea(const Entity& e) const {
    const KindInfo* k = findKind(e.kind);
    if (!k || (k->game && !m_kindsLoaded)) return e.model.empty(); // a game kind without a model: often a trigger volume, so lines
    if (k->game && !k->marker && !k->area) return e.model.empty();
    return k->area;
}

bool App::entityBounds(const Entity& e, Vec3& lo, Vec3& hi) {
    if (isMarker(e) || e.is(kKindScatter)) return false;
    lo = {-0.5f, -0.5f, -0.5f};
    hi = {0.5f, 0.5f, 0.5f};
    if (!e.model.empty() && !isArea(e) && !e.is(kKindTerrain) && !e.is(kKindSprite)) {
        const ModelEntry& m = m_assets.model(e.model);
        if (m.ok) {
            lo = m.boundsMin;
            hi = m.boundsMax;
        }
    }
    return true;
}

u32 App::pick(f32 mx, f32 my) {
    const f32 nx = (mx - m_uiX) / m_uiW * 2.0f - 1.0f, ny = 1.0f - (my - m_uiY) / m_uiH * 2.0f;
    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross({0, 1, 0}, forward));
    const Vec3 up = cross(forward, right);
    const f32 t = std::tan(m_fov * kDeg * 0.5f);
    const Vec3 dir = normalize(forward + right * (nx * t * m_uiW / m_uiH) + up * (ny * t));
    const Vec3 eye = cameraEye();
    u32 best = 0;
    f32 bestT = 1e30f;
    for (const Entity& e : m_scene.entities) {
        const Transform w = worldTransform(m_scene, e);
        if (isMarker(e) || e.is(kKindScatter)) {
            if (!m_showMarkers) continue;
            f32 sx, sy;
            if (project(w.position, sx, sy) && std::hypot(sx - mx, sy - my) < 12.0f) {
                const f32 d = length(w.position - eye);
                if (d < bestT) bestT = d, best = e.id;
            }
            continue;
        }
        Vec3 lo, hi;
        if (!entityBounds(e, lo, hi)) continue;
        // Ray in the object's space (scale divided out), slab test against its box
        const Quat inv{-w.rotation.x, -w.rotation.y, -w.rotation.z, w.rotation.w};
        const auto toLocal = [&](Vec3 v) {
            const Vec3 r = rotate(inv, v);
            return Vec3{r.x / (w.scale.x != 0 ? w.scale.x : 1), r.y / (w.scale.y != 0 ? w.scale.y : 1), r.z / (w.scale.z != 0 ? w.scale.z : 1)};
        };
        const Vec3 o = toLocal(eye - w.position), d = toLocal(dir);
        f32 t0 = 0.0f, t1 = 1e30f;
        bool hit = true;
        for (int a = 0; a < 3 && hit; ++a) {
            const f32 oa = (&o.x)[a], da = (&d.x)[a], la = (&lo.x)[a], ha = (&hi.x)[a];
            if (std::abs(da) < 1e-8f) {
                hit = oa >= la && oa <= ha;
                continue;
            }
            f32 ta = (la - oa) / da, tb = (ha - oa) / da;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            hit = t0 <= t1;
        }
        if (!hit) continue;
        if (isArea(e) && t0 <= 0.0f) continue; // inside a volume: pick what's in it instead
        // t0 is in world units along dir (the local ray is the same ray, only rescaled per axis)
        const Vec3 hitLocal = o + d * t0;
        const Vec3 hitWorld = w.position + rotate(w.rotation, {hitLocal.x * w.scale.x, hitLocal.y * w.scale.y, hitLocal.z * w.scale.z});
        const f32 dist = length(hitWorld - eye);
        if (dist < bestT) bestT = dist, best = e.id;
    }
    return best;
}

// ---------------------------------------------------------------- Frame

FrameParams App::frameParams() const {
    FrameParams p;
    const Environment& env = m_scene.environment;
    p.viewport = m_view;
    p.viewProj = viewMatrix() * projMatrix();
    p.cameraPos = cameraEye();
    p.cameraForward = cameraForward();
    p.sunDirection = env.sunDirection;
    p.sunColor = env.sunColor;
    p.skyColor = env.skyColor;
    p.groundColor = env.groundColor;
    p.fogColor = env.fogColor;
    p.fogDensity = env.fogDensity;
    p.time = static_cast<f32>(m_time);
    PostParams& post = p.post;
    post.toneMapping = env.postValue("toneMapping") != "false";
    post.punchy = env.postValue("punchy") != "false";
    parseFloat(env.postValue("exposure"), post.exposure);
    parseFloat(env.postValue("bloomIntensity"), post.bloomIntensity);
    parseFloat(env.postValue("bloomThreshold"), post.bloomThreshold);
    parseVec3(env.postValue("bloomTint"), post.bloomTint);
    parseFloat(env.postValue("vignette"), post.vignette);
    parseFloat(env.postValue("vignetteSmoothness"), post.vignetteSmoothness);
    parseFloat(env.postValue("grain"), post.grain);
    parseFloat(env.postValue("chromaticAberration"), post.chromaticAberration);
    p.lights = m_lights.data();
    p.lightCount = static_cast<u32>(m_lights.size());
    return p;
}

void App::fill(DrawList& list) {
    m_lights.clear();
    for (const Entity& e : m_scene.entities) {
        const Transform w = worldTransform(m_scene, e);
        if (e.is(kKindPointLight)) {
            u8 c[4] = {255, 214, 160, 255};
            parseColorText(e.field("color"), c);
            f32 intensity = 3.0f, range = 6.0f;
            parseFloat(e.field("intensity"), intensity);
            parseFloat(e.field("range"), range);
            PointLight l;
            l.position = w.position;
            l.range = range;
            l.color = Vec3{srgbToLinear(c[0]), srgbToLinear(c[1]), srgbToLinear(c[2])} * intensity;
            m_lights.push_back(l);
            continue;
        }
        if (isMarker(e) || isArea(e) || e.is(kKindScatter) || e.is(kKindSprite)) continue;
        InstanceData inst;
        inst.color = e.color;
        inst.flags = e.flags & ~kInstanceHighlight;
        if (e.id == m_selected) inst.flags |= kInstanceHighlight;
        if (e.is(kKindTerrain)) {
            std::string error;
            const MeshId mesh = m_assets.terrain(e.id, e.field("heightmap"), w.scale, &error);
            if (!error.empty() && std::find(m_modelProblems.begin(), m_modelProblems.end(), error) == m_modelProblems.end()) {
                m_modelProblems.push_back(error);
                log("Terrain " + e.name + ": " + error);
            }
            inst.position = w.position;
            list.add(mesh, inst);
            continue;
        }
        MeshId mesh = m_assets.box();
        if (!e.model.empty()) {
            const ModelEntry& m = m_assets.model(e.model);
            if (m.ok) mesh = m.mesh;
            else if (std::find(m_modelProblems.begin(), m_modelProblems.end(), e.model) == m_modelProblems.end()) {
                m_modelProblems.push_back(e.model);
                log("Model " + e.model + ": " + m.error + " (drawn as a box)");
            }
        }
        inst.position = w.position;
        inst.rotation = w.rotation;
        inst.scale = w.scale;
        list.add(mesh, inst);
    }
}

void App::saveScreenshot(const std::string& path, bool withUi) {
    m_hideUiThisFrame = !withUi;
    m_renderer->requestCapture(saveCapture, new CaptureJob{this, path});
}

void App::afterFrame() { m_hideUiThisFrame = false; }

void App::frame(f64 dt) {
    m_time += dt;
    m_fps = m_fps <= 0.0f ? 60.0f : m_fps * 0.95f + 0.05f / static_cast<f32>(std::max(dt, 1e-4));
    ImGuizmo::BeginFrame();
    checkExternalChange();
    if (!m_project.empty() && fileTime(kindsPath()) != m_kindsTime) reloadKinds();

    const ImGuiID dockspace = ImGui::GetID("MkRenderDock");
    if (!m_layoutDone) {
        m_layoutDone = true;
        if (!ImGui::DockBuilderGetNode(dockspace) || !ImGui::FindWindowSettingsByID(ImHashStr("Hierarchy"))) layoutDocks(dockspace);
    }
    drawMenu();
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
    const ImGuiIO& io = ImGui::GetIO();
    if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace)) {
        const f32 s = io.DisplayFramebufferScale.x;
        m_uiX = central->Pos.x;
        m_uiY = central->Pos.y;
        m_uiW = std::max(1.0f, central->Size.x);
        m_uiH = std::max(1.0f, central->Size.y);
        m_view = {m_uiX * s, m_uiY * s, m_uiW * s, m_uiH * s};
        m_view.width = std::min(m_view.width, static_cast<f32>(m_renderer->width()) - m_view.x);
        m_view.height = std::min(m_view.height, static_cast<f32>(m_renderer->height()) - m_view.y);
    }
    drawHierarchy();
    drawInspector();
    drawEnvironment();
    drawStats();
    drawOverlay();
    drawGizmo();
    viewportInput();
    drawSaveDialog();
    if (!m_status.empty() && m_time - m_statusTime < 4.0) {
        ImGui::SetNextWindowPos({m_uiX + 10.0f, m_uiY + m_uiH - 10.0f}, 0, {0.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                                              ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(m_status.c_str());
        ImGui::End();
    }
    if (!ImGui::IsAnyItemActive() && !ImGuizmo::IsUsing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) snapshot();
}

void App::layoutDocks(unsigned dockspace) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace, vp->WorkSize);
    ImGuiID rest = dockspace;
    ImGuiID left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.2f, nullptr, &rest);
    const ImGuiID right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.27f, nullptr, &rest);
    const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.3f, nullptr, &left);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Stats", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Environment", right);
    ImGui::DockBuilderFinish(dockspace);
}

// ---------------------------------------------------------------- Menu and dialogs

void App::drawMenu() {
    const ImGuiIO& io = ImGui::GetIO();
    const auto openSceneDialog = [&] {
        const std::string where = m_assets.assetsDir().empty() ? std::string() : utf8Of(pathOf(m_assets.assetsDir()) / "scenes");
        SDL_ShowOpenFileDialog([](void* user, const char* const* files, int) { if (files && files[0]) static_cast<App*>(user)->setPending(files[0]); },
                               this, static_cast<SDL_Window*>(m_window), kSceneFilters, 2, where.empty() ? nullptr : where.c_str(), false);
    };
    const auto openProjectDialog = [&] {
        SDL_ShowOpenFolderDialog([](void* user, const char* const* files, int) {
            if (files && files[0]) static_cast<App*>(user)->setPending(std::string("project:") + files[0]);
        }, this, static_cast<SDL_Window*>(m_window), nullptr, false);
    };
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New scene", "Ctrl+N")) guarded(Pending::NewScene);
            if (ImGui::MenuItem("Open project...")) openProjectDialog();
            if (ImGui::MenuItem("Open scene...", "Ctrl+O")) openSceneDialog();
            if (ImGui::BeginMenu("Project scenes", !m_assets.assetsDir().empty())) {
                for (const std::string& s : m_assets.files(".scene"))
                    if (ImGui::MenuItem(s.c_str(), nullptr, m_assets.resolve(s) == m_path)) guarded(Pending::OpenPath, m_assets.resolve(s));
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) save();
            if (ImGui::MenuItem("Save as...")) saveAs();
            if (ImGui::MenuItem("Reload from disk", nullptr, false, !m_path.empty())) reloadFromDisk();
            if (ImGui::MenuItem("Reload models")) {
                m_assets.reloadAll();
                m_modelProblems.clear();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save screenshot (3D only)")) {
                if (char* dir = SDL_GetPrefPath("MealKit", "MkRender")) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "screenshot_%llu.png", static_cast<unsigned long long>(SDL_GetTicks()));
                    saveScreenshot(std::string(dir) + name, false);
                    SDL_free(dir);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) requestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo.empty())) undo(false);
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo.empty())) undo(true);
            ImGui::Separator();
            if (ImGui::BeginMenu("Add")) {
                drawAddMenu();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_selected != 0)) duplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, m_selected != 0)) deleteSelected();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Frame selection", "F", false, m_selected != 0)) frameSelected();
            if (ImGui::MenuItem("Move", "W", m_gizmo == Gizmo::Translate)) m_gizmo = Gizmo::Translate;
            if (ImGui::MenuItem("Rotate", "E", m_gizmo == Gizmo::Rotate)) m_gizmo = Gizmo::Rotate;
            if (ImGui::MenuItem("Scale", "R", m_gizmo == Gizmo::Scale)) m_gizmo = Gizmo::Scale;
            ImGui::MenuItem("Local axes", nullptr, &m_gizmoLocal);
            ImGui::MenuItem("Markers and areas", nullptr, &m_showMarkers);
            int model = static_cast<int>(m_lighting);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::Combo("Lighting model", &model, "PBR\0Toon\0Unlit\0")) setLightingModel(static_cast<LightingModel>(model));
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) layoutDocks(ImGui::GetID("MkRenderDock"));
            ImGui::EndMenu();
        }
        if (m_externalChange) {
            ImGui::TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "   The file changed on disk.");
            if (ImGui::SmallButton("Reload (drop my edits)")) reloadFromDisk();
            if (ImGui::SmallButton("Keep mine")) {
                m_externalChange = false;
                m_fileTime = fileTime(m_path);
            }
        }
        ImGui::TextDisabled("   %s  |  %s", mkrhi::backendName(), m_renderer->adapterName());
        ImGui::EndMainMenuBar();
    }

    if (!io.WantTextInput) {
        const bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) save();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) openSceneDialog();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N)) guarded(Pending::NewScene);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undo(io.KeyShift);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) undo(true);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) duplicateSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelected();
        if (!ctrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmo = Gizmo::Translate;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmo = Gizmo::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmo = Gizmo::Scale;
            if (ImGui::IsKeyPressed(ImGuiKey_F)) frameSelected();
        }
    }
}

void App::drawSaveDialog() {
    if (m_pendingAction != Pending::None && !ImGui::IsPopupOpen("Unsaved changes") && dirty()) ImGui::OpenPopup("Unsaved changes");
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%s has unsaved changes.", m_path.empty() ? "The new scene" : utf8Of(pathOf(m_path).filename()).c_str());
    if (ImGui::Button("Save")) {
        if (save()) runPending();
        else m_pendingAction = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Don't save")) {
        runPending();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_pendingAction = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawAddMenu() {
    bool anyGame = false;
    for (const KindInfo& k : kinds()) {
        if (k.game) {
            anyGame = true;
            continue;
        }
        if (ImGui::MenuItem(k.label.c_str())) addEntity(k.name);
    }
    if (!anyGame) return;
    ImGui::Separator();
    ImGui::TextDisabled("Game");
    for (const KindInfo& k : kinds())
        if (k.game && ImGui::MenuItem(k.label.c_str())) addEntity(k.name);
}

// ---------------------------------------------------------------- Hierarchy

void App::drawTreeNode(const Entity& e) {
    bool hasChildren = false;
    for (const Entity& c : m_scene.entities) hasChildren |= c.parent == e.id;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (e.id == m_selected) flags |= ImGuiTreeNodeFlags_Selected;
    // Keep the selection visible: open the ancestors of the selected object
    if (hasChildren && m_selected && m_selected != e.id && isDescendant(m_scene, m_selected, e.id)) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    ImGui::PushID(static_cast<int>(e.id));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", e.name.empty() ? "(unnamed)" : e.name.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_selected = e.id;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_selected = e.id;
        frameSelected();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", e.kind.c_str());
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kDragEntity, &e.id, sizeof(u32));
        ImGui::Text("%s", e.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragEntity)) reparent(*static_cast<const u32*>(p->Data), e.id);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("ctx")) {
        m_selected = e.id;
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelected();
        if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
        if (e.parent && ImGui::MenuItem("Move to top level")) reparent(e.id, 0);
        if (ImGui::MenuItem("Frame", "F")) frameSelected();
        ImGui::EndPopup();
    }
    const u32 id = e.id; // e may move when the list changes below
    if (open && hasChildren) {
        for (size_t i = 0; i < m_scene.entities.size(); ++i)
            if (m_scene.entities[i].parent == id) drawTreeNode(m_scene.entities[i]);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void App::drawHierarchy() {
    if (ImGui::Begin("Hierarchy")) {
        if (ImGui::Button("+ Add")) ImGui::OpenPopup("add");
        if (ImGui::BeginPopup("add")) {
            drawAddMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "Search", m_filter, sizeof(m_filter));
        ImGui::Separator();
        if (ImGui::BeginChild("tree")) {
            const std::string filter = lower(m_filter);
            if (!filter.empty()) {
                for (const Entity& e : m_scene.entities) {
                    if (lower(e.name).find(filter) == std::string::npos && lower(e.kind).find(filter) == std::string::npos) continue;
                    ImGui::PushID(static_cast<int>(e.id));
                    if (ImGui::Selectable(e.name.c_str(), e.id == m_selected)) m_selected = e.id;
                    ImGui::PopID();
                }
            } else {
                // Folders (top-level objects only), then the objects without one, in file order
                std::set<std::string> folders;
                for (const Entity& e : m_scene.entities)
                    if (!e.parent && !e.folder.empty()) folders.insert(e.folder);
                for (const std::string& folder : folders) {
                    const bool open = ImGui::TreeNodeEx(folder.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "[%s]", folder.c_str());
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragEntity)) {
                            const u32 id = *static_cast<const u32*>(p->Data);
                            reparent(id, 0);
                            if (Entity* moved = m_scene.find(id)) moved->folder = folder;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (open) {
                        for (size_t i = 0; i < m_scene.entities.size(); ++i)
                            if (!m_scene.entities[i].parent && m_scene.entities[i].folder == folder) drawTreeNode(m_scene.entities[i]);
                        ImGui::TreePop();
                    }
                }
                for (size_t i = 0; i < m_scene.entities.size(); ++i)
                    if (!m_scene.entities[i].parent && m_scene.entities[i].folder.empty()) drawTreeNode(m_scene.entities[i]);
            }
            // Empty space: drop to move to the top level, click to clear the selection
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##empty", {std::max(1.0f, avail.x), std::max(ImGui::GetFrameHeight(), avail.y)});
            if (ImGui::IsItemClicked()) m_selected = 0;
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragEntity)) {
                    const u32 id = *static_cast<const u32*>(p->Data);
                    reparent(id, 0);
                    if (Entity* moved = m_scene.find(id)) moved->folder.clear();
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- Inspector

void App::drawFields(Entity& e) {
    // Typed widgets from the value text: numbers, true/false, #RRGGBB colors, enum names of known kinds, else text
    static const char* bodies[] = {"ThirdPerson", "FirstPerson", "TopDown", "Orbit", "Rail", "Fixed", "Free"};
    static const char* eases[] = {"EaseInOut", "Linear", "Cut"};
    int remove = -1;
    for (size_t i = 0; i < e.fields.size(); ++i) {
        auto& [key, value] = e.fields[i];
        ImGui::PushID(static_cast<int>(i));
        const KindField* kf = findKindField(e.kind, key);
        row(kf && !kf->label.empty() ? kf->label.c_str() : key.c_str());
        if (kf) ImGui::SetItemTooltip("%s (%s)%s", key.c_str(), kf->type.c_str(), kf->view ? ", view only" : "");
        ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        f32 number = 0.0f;
        u8 color[4];
        const auto enumCombo = [&](const char* const* names, int count) {
            if (ImGui::BeginCombo("##v", value.c_str())) {
                for (int k = 0; k < count; ++k)
                    if (ImGui::Selectable(names[k], value == names[k])) value = names[k];
                ImGui::EndCombo();
            }
        };
        std::vector<const char*> enumNames;
        if (kf)
            for (const std::string& n : kf->enumNames) enumNames.push_back(n.c_str());
        f32 v3[3];
        if (kf && kf->readOnly) {
            ImGui::TextDisabled("%s", value.c_str());
        } else if (kf && kf->type == "enum" && !enumNames.empty()) {
            enumCombo(enumNames.data(), static_cast<int>(enumNames.size()));
        } else if (kf && kf->type == "bool") {
            bool on = value == "true";
            if (ImGui::Checkbox("##v", &on)) value = on ? "true" : "false";
        } else if (kf && kf->type == "color") {
            if (!parseColorText(value, color)) color[0] = color[1] = color[2] = color[3] = 255;
            f32 c[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
            if (ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_AlphaPreviewHalf)) {
                for (int k = 0; k < 4; ++k) color[k] = static_cast<u8>(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
                value = colorText(color);
            }
        } else if (kf && (kf->type == "int" || kf->type == "uint")) {
            parseFloat(value, number);
            int n = static_cast<int>(number);
            if (ImGui::DragInt("##v", &n, 0.1f, kf->type == "uint" ? 0 : INT_MIN, INT_MAX)) value = std::to_string(n);
        } else if (kf && (kf->type == "float" || kf->type == "seconds")) {
            parseFloat(value, number);
            if (ImGui::DragFloat("##v", &number, std::max(0.01f, std::abs(number) * 0.01f), kf->type == "seconds" ? 0.0f : 0.0f, 0.0f,
                                 kf->type == "seconds" ? "%.3f s" : "%.3f"))
                value = floatText(kf->type == "seconds" ? std::max(0.0f, number) : number);
        } else if (kf && kf->type == "vec3" &&
                   std::sscanf(value.c_str(), "%f %f %f", &v3[0], &v3[1], &v3[2]) == 3) {
            if (ImGui::DragFloat3("##v", v3, 0.05f)) value = floatsText(v3, 3);
        } else if (kf && (kf->type == "string" || kf->type == "row" || kf->type == "text")) {
            inputString("##v", value);
        } else if (e.is(kKindCamera) && key == "body") enumCombo(bodies, 7);
        else if (e.is(kKindCamera) && key == "ease") enumCombo(eases, 3);
        else if (value == "true" || value == "false") {
            bool on = value == "true";
            if (ImGui::Checkbox("##v", &on)) value = on ? "true" : "false";
        } else if (parseColorText(value, color)) {
            f32 c[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
            if (ImGui::ColorEdit4("##v", c, ImGuiColorEditFlags_AlphaPreviewHalf)) {
                for (int k = 0; k < 4; ++k) color[k] = static_cast<u8>(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
                value = colorText(color);
            }
        } else if (parseFloat(value, number) && value.find_first_of(".eE") == std::string::npos) {
            int n = static_cast<int>(number);
            if (ImGui::DragInt("##v", &n, 0.1f)) value = std::to_string(n);
        } else if (parseFloat(value, number)) {
            if (ImGui::DragFloat("##v", &number, std::max(0.01f, std::abs(number) * 0.01f), 0.0f, 0.0f, "%.3f")) value = floatText(number);
        } else {
            inputString("##v", value);
        }
        ImGui::SameLine();
        if (ImGui::Button("x", {ImGui::GetFrameHeight(), 0})) remove = static_cast<int>(i);
        ImGui::SetItemTooltip("Remove this field line");
        ImGui::PopID();
    }
    if (remove >= 0) e.fields.erase(e.fields.begin() + remove);
    if (const KindInfo* k = findKind(e.kind)) {
        size_t missing = 0;
        for (const KindField& f : k->fields) missing += e.field(f.path).empty() && std::none_of(e.fields.begin(), e.fields.end(), [&](const auto& l) { return l.first == f.path; });
        if (missing && ImGui::Button(("Add " + std::to_string(missing) + " missing fields with defaults").c_str()))
            for (const KindField& f : k->fields)
                if (std::none_of(e.fields.begin(), e.fields.end(), [&](const auto& l) { return l.first == f.path; })) e.fields.push_back({f.path, f.value});
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Add field").x - ImGui::GetStyle().FramePadding.x * 4);
    ImGui::InputTextWithHint("##newfield", "field name", m_newField, sizeof(m_newField));
    ImGui::SameLine();
    if (ImGui::Button("Add field") && m_newField[0] && !std::strchr(m_newField, ' ')) {
        e.setField(m_newField, "");
        m_newField[0] = '\0';
    }
}

void App::drawInspector() {
    if (ImGui::Begin("Inspector")) {
        Entity* found = m_scene.find(m_selected);
        if (!found) {
            ImGui::TextDisabled("Select an object in the hierarchy or the viewport.");
            ImGui::TextDisabled("%zu objects in the scene", m_scene.entities.size());
        } else {
            Entity& e = *found;
            ImGui::TextDisabled("id %u", e.id);
            row("Name");
            inputString("##name", e.name);
            row("Kind");
            if (ImGui::BeginCombo("##kind", e.kind.c_str())) {
                for (const KindInfo& k : kinds())
                    if (ImGui::Selectable(k.label.c_str(), e.kind == k.name) && e.kind != k.name) {
                        e.kind = k.name;
                        for (const auto& [key, value] : defaultFields(k.name))
                            if (e.field(key).empty()) e.setField(key, value);
                    }
                ImGui::Separator();
                ImGui::TextDisabled("Game kind (typed name):");
                static char custom[64];
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                if (ImGui::InputText("##custom", custom, sizeof(custom), ImGuiInputTextFlags_EnterReturnsTrue) && std::isalpha(static_cast<unsigned char>(custom[0]))) {
                    e.kind = custom;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndCombo();
            }
            if (!findKind(e.kind))
                ImGui::TextDisabled(m_kindsLoaded ? "Unknown kind: not in the project's object kinds; fields are kept as text."
                                                  : "Game kind: fields are kept as text (no build/editor/kinds.txt yet: open the project in the editor once, or run <game>_headless --write-kinds).");
            if (e.parent) {
                const Entity* parent = m_scene.find(e.parent);
                row("Parent");
                ImGui::TextUnformatted(parent ? parent->name.c_str() : "?");
                ImGui::SameLine();
                if (ImGui::SmallButton("Unparent")) reparent(e.id, 0);
            } else {
                row("Folder");
                inputString("##folder", e.folder);
            }

            ImGui::SeparatorText("Transform (parent-relative)");
            row("Position");
            ImGui::DragFloat3("##pos", &e.position.x, 0.05f, 0.0f, 0.0f, "%.3f");
            if (m_eulerFor != e.id) m_euler = toEuler(e.rotation);
            row("Rotation");
            if (ImGui::DragFloat3("##rot", &m_euler.x, 0.5f, 0.0f, 0.0f, "%.1f")) e.rotation = fromEuler(m_euler);
            m_eulerFor = ImGui::IsItemActive() ? e.id : 0;
            row("Scale");
            ImGui::DragFloat3("##scale", &e.scale.x, 0.02f, 0.0f, 0.0f, "%.3f");

            ImGui::SeparatorText("Look");
            const KindInfo* info = findKind(e.kind);
            const bool modelKind = !info || (!info->marker && !info->area && !e.is(kKindTerrain) && !e.is(kKindScatter) && !e.is(kKindSprite));
            if (modelKind) {
                row("Model");
                if (ImGui::BeginCombo("##model", e.model.empty() ? "(box)" : e.model.c_str(), ImGuiComboFlags_HeightLarge)) {
                    static char search[64];
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputTextWithHint("##search", "Search", search, sizeof(search));
                    if (ImGui::Selectable("(box)", e.model.empty())) e.model.clear();
                    const std::string s = lower(search);
                    for (const std::string& m : m_assets.modelFiles())
                        if ((s.empty() || lower(m).find(s) != std::string::npos) && ImGui::Selectable(m.c_str(), m == e.model)) e.model = m;
                    ImGui::EndCombo();
                }
                if (!e.model.empty()) {
                    const ModelEntry& m = m_assets.model(e.model);
                    if (m.ok) ImGui::TextDisabled("%u triangles, %u materials, %u textures", m.triangles, m.materials, m.textures);
                    else ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "%s", m.error.c_str());
                    for (const std::string& w : m.warnings) ImGui::TextDisabled("%s", w.c_str());
                }
            }
            f32 c[4];
            colorToFloats(e.color, c);
            row("Color");
            if (ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_AlphaPreviewHalf)) e.color = floatsToColor(c);
            bool checker = (e.flags & kInstanceChecker) != 0;
            row("Checker");
            if (ImGui::Checkbox("##checker", &checker)) e.flags = checker ? e.flags | kInstanceChecker : e.flags & ~kInstanceChecker;
            if (e.is(kKindScatter) || e.is(kKindSprite) || e.is(kKindCamera))
                ImGui::TextDisabled("Not drawn in MkRender yet (shown as a marker); the fields are saved as they are.");
            if (e.is(kKindTerrain)) ImGui::TextDisabled("The heightmap fills the object's box (scale). Brushes are in the MealKit editor.");

            ImGui::SeparatorText("Fields");
            drawFields(e);
        }
    }
    ImGui::End();
}

void App::drawEnvironment() {
    if (ImGui::Begin("Environment")) {
        Environment& env = m_scene.environment;
        ImGui::SeparatorText("Sun");
        const Vec3 d = normalize(env.sunDirection);
        f32 azimuth = std::atan2(d.x, d.z) / kDeg, elevation = std::asin(std::clamp(d.y, -1.0f, 1.0f)) / kDeg;
        row("Azimuth");
        bool sun = ImGui::SliderFloat("##az", &azimuth, -180.0f, 180.0f, "%.1f deg");
        row("Elevation");
        sun |= ImGui::SliderFloat("##el", &elevation, -10.0f, 90.0f, "%.1f deg");
        if (sun) env.sunDirection = {std::cos(elevation * kDeg) * std::sin(azimuth * kDeg), std::sin(elevation * kDeg), std::cos(elevation * kDeg) * std::cos(azimuth * kDeg)};
        row("Sun color");
        ImGui::ColorEdit3("##suncolor", &env.sunColor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        ImGui::SeparatorText("Sky and fog (linear)");
        row("Sky");
        ImGui::ColorEdit3("##sky", &env.skyColor.x, ImGuiColorEditFlags_Float);
        row("Ground");
        ImGui::ColorEdit3("##ground", &env.groundColor.x, ImGuiColorEditFlags_Float);
        row("Fog color");
        ImGui::ColorEdit3("##fog", &env.fogColor.x, ImGuiColorEditFlags_Float);
        row("Fog density");
        ImGui::DragFloat("##fogd", &env.fogDensity, 0.0002f, 0.0f, 1.0f, "%.4f");
        ImGui::SeparatorText("Sky HDRI");
        row("HDRI file");
        inputString("##hdri", env.hdri);
        if (!env.hdri.empty()) {
            row("Intensity");
            ImGui::DragFloat("##hdrii", &env.hdriIntensity, 0.01f, 0.0f, 16.0f);
            row("Rotation");
            ImGui::DragFloat("##hdrir", &env.hdriRotation, 0.5f, -360.0f, 360.0f, "%.1f deg");
            ImGui::TextDisabled("MkRender doesn't bake HDRIs yet: the sky/ground colors are shown instead.");
        }
        ImGui::SeparatorText("Post-processing");
        const auto boolRow = [&](const char* key, const char* label) {
            bool on = env.postValue(key) != "false";
            row(label);
            if (ImGui::Checkbox((std::string("##") + key).c_str(), &on)) env.setPost(key, on ? "true" : "false");
        };
        const auto floatRow = [&](const char* key, const char* label, f32 lo, f32 hi) {
            f32 v = 0.0f;
            parseFloat(env.postValue(key), v);
            row(label);
            if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, "%.3f")) env.setPost(key, floatText(v));
        };
        boolRow("toneMapping", "Tone mapping");
        boolRow("punchy", "Punchy look");
        floatRow("exposure", "Exposure", -4.0f, 4.0f);
        floatRow("bloomIntensity", "Bloom", 0.0f, 1.0f);
        floatRow("bloomThreshold", "Bloom threshold", 0.0f, 8.0f);
        Vec3 tint{1, 1, 1};
        parseVec3(env.postValue("bloomTint"), tint);
        row("Bloom tint");
        if (ImGui::ColorEdit3("##tint", &tint.x, ImGuiColorEditFlags_Float)) env.setPost("bloomTint", floatsText(&tint.x, 3));
        floatRow("vignette", "Vignette", 0.0f, 1.0f);
        floatRow("vignetteSmoothness", "Vignette start", 0.0f, 1.0f);
        floatRow("grain", "Film grain", 0.0f, 1.0f);
        floatRow("chromaticAberration", "Chromatic aberr.", 0.0f, 0.03f);
        std::string lut = env.postValue("lut");
        row("Color LUT");
        if (inputString("##lut", lut)) env.setPost("lut", lut);
        if (!lut.empty()) ImGui::TextDisabled("LUTs aren't applied in MkRender yet.");
    }
    ImGui::End();
}

void App::drawStats() {
    if (ImGui::Begin("Stats")) {
        const RenderStats& s = m_renderer->stats();
        ImGui::Text("%.0f fps, %u draw calls, %u instances", m_fps, s.drawCalls, s.instances);
        if (s.gpuValid) ImGui::Text("GPU %.2f ms (opaque %.2f, shadow %.2f, post %.2f)", s.gpuFrameMs, s.gpuPassMs[0], s.gpuPassMs[1], s.gpuPassMs[2]);
        bool vsync = m_renderer->vsync();
        if (ImGui::Checkbox("VSync", &vsync)) m_renderer->setVsync(vsync);
        ImGui::SeparatorText("Log");
        if (ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_Borders)) {
            for (const std::string& w : m_warnings) ImGui::TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "%s", w.c_str());
            for (const std::string& line : m_log) ImGui::TextUnformatted(line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- Viewport

void App::drawOverlay() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect({m_uiX, m_uiY}, {m_uiX + m_uiW, m_uiY + m_uiH}, true);
    const auto boxLines = [&](const Transform& w, Vec3 lo, Vec3 hi, ImU32 color, f32 thickness) {
        Vec3 corners[8];
        for (int i = 0; i < 8; ++i) {
            const Vec3 l{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
            corners[i] = w.position + rotate(w.rotation, {l.x * w.scale.x, l.y * w.scale.y, l.z * w.scale.z});
        }
        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& edge : edges) {
            f32 ax, ay, bx, by;
            if (project(corners[edge[0]], ax, ay) && project(corners[edge[1]], bx, by)) dl->AddLine({ax, ay}, {bx, by}, color, thickness);
        }
    };
    const ImU32 selectedColor = IM_COL32(255, 200, 60, 255);
    for (const Entity& e : m_scene.entities) {
        const Transform w = worldTransform(m_scene, e);
        const bool selected = e.id == m_selected;
        if (isArea(e)) {
            const KindInfo* kindInfo = findKind(e.kind);
            const bool game = !kindInfo || kindInfo->game;
            if (m_showMarkers || selected)
                boxLines(w, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f},
                         selected ? selectedColor : game ? IM_COL32(255, 150, 220, 150) : IM_COL32(120, 200, 255, 160), selected ? 2.0f : 1.0f);
            f32 x, y;
            if ((selected || ImGui::GetIO().KeyAlt) && project(w.position, x, y)) dl->AddText({x, y}, IM_COL32(230, 230, 230, 230), e.name.c_str());
            continue;
        }
        if (isMarker(e) || e.is(kKindScatter) || e.is(kKindSprite)) {
            if (!m_showMarkers && !selected) continue;
            f32 x, y;
            if (!project(w.position, x, y)) continue;
            ImU32 color = IM_COL32(200, 200, 200, 230);
            const char* glyph = "?";
            if (e.is(kKindSpawn)) color = IM_COL32(255, 220, 90, 240), glyph = "S";
            else if (e.is(kKindPointLight)) {
                u8 c[4] = {255, 214, 160, 255};
                parseColorText(e.field("color"), c);
                color = IM_COL32(c[0], c[1], c[2], 240);
                glyph = "L";
                if (selected) { // range circle on the ground plane of the light
                    f32 range = 6.0f;
                    parseFloat(e.field("range"), range);
                    ImVec2 prev{};
                    bool havePrev = false;
                    for (int i = 0; i <= 48; ++i) {
                        const f32 a = static_cast<f32>(i) / 48.0f * 2.0f * kPi;
                        f32 px, py;
                        const bool ok = project(w.position + Vec3{std::cos(a) * range, 0.0f, std::sin(a) * range}, px, py);
                        if (ok && havePrev) dl->AddLine(prev, {px, py}, color, 1.5f);
                        prev = {px, py};
                        havePrev = ok;
                    }
                }
            } else if (e.is(kKindCamera)) color = IM_COL32(150, 220, 150, 240), glyph = "C";
            else if (e.is(kKindScatter)) color = IM_COL32(120, 200, 120, 200), glyph = "#";
            else if (e.is(kKindSprite)) color = IM_COL32(220, 160, 240, 220), glyph = "I";
            const f32 r = 9.0f;
            dl->AddCircleFilled({x, y}, r, IM_COL32(20, 20, 24, 200));
            dl->AddCircle({x, y}, r, selected ? selectedColor : color, 0, selected ? 2.5f : 1.5f);
            const ImVec2 size = ImGui::CalcTextSize(glyph);
            dl->AddText({x - size.x * 0.5f, y - size.y * 0.5f}, color, glyph);
            if (selected || ImGui::GetIO().KeyAlt) dl->AddText({x + r + 3.0f, y - size.y * 0.5f}, IM_COL32(230, 230, 230, 230), e.name.c_str());
            continue;
        }
        if (selected) {
            Vec3 lo, hi;
            if (entityBounds(e, lo, hi)) {
                if (e.is(kKindTerrain)) boxLines({w.position, {}, w.scale}, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, selectedColor, 1.5f);
                else boxLines(w, lo, hi, selectedColor, 1.5f);
            }
        }
    }
    dl->PopClipRect();
}

void App::drawGizmo() {
    m_gizmoUsing = false;
    Entity* e = m_scene.find(m_selected);
    if (!e || m_uiW < 2.0f || e->is(kKindScatter)) return;
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(m_uiX, m_uiY, m_uiW, m_uiH);
    ImGuizmo::SetGizmoSizeClipSpace(0.13f);
    f32 matrix[16];
    compose(worldTransform(m_scene, *e), matrix);
    ImGuizmo::OPERATION op = m_gizmo == Gizmo::Translate ? ImGuizmo::TRANSLATE : m_gizmo == Gizmo::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    if (isMarker(*e) && op == ImGuizmo::SCALE) op = ImGuizmo::TRANSLATE;
    const ImGuizmo::MODE mode = (m_gizmoLocal || op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const f32 step = op == ImGuizmo::TRANSLATE ? 0.5f : op == ImGuizmo::ROTATE ? 15.0f : 0.1f;
    const f32 snap[3] = {step, step, step};
    const Mat4 view = viewMatrix(), proj = projMatrix();
    if (ImGuizmo::Manipulate(&view.m[0][0], &proj.m[0][0], op, mode, matrix, nullptr, ImGui::GetIO().KeyCtrl ? snap : nullptr)) {
        Transform world;
        decompose(matrix, world);
        setWorldTransform(m_scene, *e, world);
        m_eulerFor = 0;
    }
    m_gizmoUsing = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
}

void App::viewportInput() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= m_uiX && mouse.y >= m_uiY && mouse.x < m_uiX + m_uiW && mouse.y < m_uiY + m_uiH;
    const bool free = inside && !io.WantCaptureMouse;
    if (free && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_gizmoUsing) {
        m_clickCandidate = true;
        m_clickX = mouse.x;
        m_clickY = mouse.y;
    }
    if (m_clickCandidate && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_clickCandidate = false;
        if (std::hypot(mouse.x - m_clickX, mouse.y - m_clickY) < 5.0f && !io.KeyAlt) m_selected = pick(mouse.x, mouse.y);
    }
    const bool orbit = ImGui::IsMouseDown(ImGuiMouseButton_Right) || (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left));
    const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    static bool dragging = false; // started inside the viewport
    if ((orbit || pan) && free && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                                   ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
        dragging = true;
    if (!orbit && !pan) dragging = false;
    if (dragging && orbit) {
        m_yaw += io.MouseDelta.x * 0.006f;
        m_pitch = std::clamp(m_pitch + io.MouseDelta.y * 0.006f, -1.55f, 1.55f);
    } else if (dragging && pan) {
        const Vec3 forward = cameraForward();
        const Vec3 right = normalize(cross({0, 1, 0}, forward));
        const Vec3 up = cross(forward, right);
        const f32 k = m_distance * std::tan(m_fov * kDeg * 0.5f) * 2.0f / std::max(1.0f, m_uiH);
        m_pivot += right * (-io.MouseDelta.x * k) + up * (io.MouseDelta.y * k);
    }
    if (free && io.MouseWheel != 0.0f) m_distance = std::clamp(m_distance * std::pow(0.87f, io.MouseWheel), 0.2f, 4000.0f);
    // Right drag + WASD/QE flies the pivot (big scenes)
    if (dragging && orbit && !io.WantTextInput) {
        const f32 speed = m_distance * 0.8f * io.DeltaTime * (io.KeyShift ? 3.0f : 1.0f);
        const Vec3 forward = cameraForward();
        const Vec3 right = normalize(cross({0, 1, 0}, forward));
        if (ImGui::IsKeyDown(ImGuiKey_W)) m_pivot += forward * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) m_pivot -= forward * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) m_pivot += right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) m_pivot -= right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) m_pivot.y += speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) m_pivot.y -= speed;
    }
}

} // namespace mkrender::tool
