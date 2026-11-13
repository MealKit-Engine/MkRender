#include "App.h"
#include "Strings.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (first layout)
#include <ImGuizmo.h>
#include <mkcontract/Client.h>
#include <mkcontract/User.h>

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
constexpr u32 kThumbSize = 96; // asset thumbnails (pixels)
constexpr u32 kSheetSize = 256; // sprite slicer picture (pixels)
constexpr u32 kLookSize = 768;  // Look Dev image (pixels, square)
constexpr const char* kDragEntity = "MKR_ENTITY";
constexpr const char* kDragAsset = "MKR_ASSET";
// Glyphs from engine docs/TOOL_DESIGN.md section 2 (Segoe Fluent Icons / MDL2 Assets)
constexpr const char* kIconAdd = "\xEE\x9C\x90";     // U+E710
constexpr const char* kIconRemove = "\xEE\x9C\xB8";  // U+E738 remove from a list
constexpr const char* kIconRefresh = "\xEE\x9C\xAC"; // U+E72C
constexpr const char* kIconPlay = "\xEE\x9D\xA8";    // U+E768
constexpr const char* kIconCamera = "\xEE\x9C\xA2";  // U+E722 capture
constexpr const char* kIconClear = "\xEE\xA2\x94";   // U+E894
// Panel tabs (engine docs/TOOL_DESIGN.md section 2 "Panels")
constexpr const char* kIconTree = "\xEE\xA3\xBD";        // U+E8FD Hierarchy
constexpr const char* kIconInspector = "\xEE\xA7\xA9";   // U+E9E9
constexpr const char* kIconFolder = "\xEE\xA2\xB7";      // U+E8B7 Assets
constexpr const char* kIconBrightness = "\xEE\x9C\x86";  // U+E706 Environment
constexpr const char* kIconPerformance = "\xEE\xA7\x99"; // U+E9D9 Stats
constexpr const char* kIconLanguage = "\xEF\x8A\xB7";    // U+F2B7 LocaleLanguage
constexpr const char* kIconLookDev = "\xEE\x9F\xB4";     // U+E7F4 Look Dev (the editor's Preview)
// Hierarchy kind icons (module-editor ImGuiLayer.h icon::, the editor's scene tree)
constexpr const char* kIconBox = "\xEE\x9C\xB9";    // U+E739 solid, decoration
constexpr const char* kIconModel = "\xEE\x9E\xB8";  // U+E7B8 an object with a model
constexpr const char* kIconPin = "\xEE\x9C\x87";    // U+E707 spawn and other markers
constexpr const char* kIconArea = "\xEE\xA0\x8A";   // U+E80A volumes
constexpr const char* kIconLight = "\xEE\xAA\x80";  // U+EA80 point light
constexpr const char* kIconProbe = "\xEE\xA4\x89";  // U+E909 reflection probe
constexpr const char* kIconFlag = "\xEE\x9F\x81";   // U+E7C1 other game kinds
constexpr const char* kIconImage = "\xEE\xA4\x9B";  // U+E91B Photo: sprite

// Orthographic projection with reversed z like perspectiveReversedZ (the engine's orthographicReversedZ): near maps to 1, far to 0
Mat4 orthographicReversedZ(f32 halfHeight, f32 aspect, f32 zNear, f32 zFar) {
    Mat4 r;
    r.m[0][0] = 1.0f / (halfHeight * aspect);
    r.m[1][1] = 1.0f / halfHeight;
    r.m[2][2] = -1.0f / (zFar - zNear);
    r.m[3][2] = zFar / (zFar - zNear);
    r.m[3][3] = 1.0f;
    return r;
}

// Checkbox and radio colors (engine docs/TOOL_DESIGN.md 3.1, 3.2): a gray 4 box on a gray 2 panel is about 1.25:1, so an unchecked
// box gets a gray 9 outline (gray 11 hovered) and a checked one the accent fill with a gray 12 mark
const ImVec4 kControlOutline(0x8A / 255.0f, 0x8A / 255.0f, 0x8A / 255.0f, 1.0f);      // gray 9
const ImVec4 kControlOutlineHover(0xAF / 255.0f, 0xAF / 255.0f, 0xAF / 255.0f, 1.0f); // gray 11
const ImVec4 kControlMark(0xEB / 255.0f, 0xEB / 255.0f, 0xEB / 255.0f, 1.0f);         // gray 12
const ImVec4 kAccent(0x3B / 255.0f, 0x63 / 255.0f, 0xFB / 255.0f, 1.0f);
const ImVec4 kAccentHover(0x4B / 255.0f, 0x75 / 255.0f, 0xFF / 255.0f, 1.0f);
const ImVec4 kAccentDown(0x27 / 255.0f, 0x4D / 255.0f, 0xEA / 255.0f, 1.0f);

// Pushes the fill and mark of a checkbox or radio button in the state it is drawn in; pop 4 colors and 1 var after
void pushControlColors(bool on) {
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImGuiStyle& s = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, on ? kAccent : clear);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, on ? kAccentHover : s.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, on ? kAccentDown : s.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kControlMark);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.FrameRounding * 0.5f); // a 4 px box, not the round 8 px of fields
}

void popControlColors() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

// Every checkbox goes through here, never ImGui::Checkbox (engine docs/TOOL_DESIGN.md 3.2)
bool checkbox(const char* label, bool* v) {
    const bool on = *v;
    pushControlColors(on);
    const bool changed = ImGui::Checkbox(label, v);
    const f32 rounding = ImGui::GetStyle().FrameRounding;
    popControlColors();
    if (!on) {
        // The box is the square at the start of the item (ImGui::Checkbox check_bb)
        const ImVec2 p = ImGui::GetItemRectMin();
        const f32 size = ImGui::GetFrameHeight();
        const f32 width = std::max(1.0f, ImGui::GetStyle().FontScaleDpi);
        const f32 inset = width * 0.5f;
        ImGui::GetWindowDrawList()->AddRect({p.x + inset, p.y + inset}, {p.x + size - inset, p.y + size - inset},
                                            ImGui::GetColorU32(ImGui::IsItemHovered() ? kControlOutlineHover : kControlOutline), rounding, 0, width);
    }
    return changed;
}

bool radioButton(const char* label, bool active) {
    pushControlColors(active);
    const bool pressed = ImGui::RadioButton(label, active);
    popControlColors();
    if (!active) {
        // Same circle as ImGui::RadioButton: radius (square - 1) / 2 around the square's center
        const ImVec2 p = ImGui::GetItemRectMin();
        const f32 size = ImGui::GetFrameHeight();
        const f32 width = std::max(1.0f, ImGui::GetStyle().FontScaleDpi);
        const ImVec2 center(std::floor(p.x + size * 0.5f + 0.5f), std::floor(p.y + size * 0.5f + 0.5f));
        ImGui::GetWindowDrawList()->AddCircle(center, (size - 1.0f) * 0.5f - width * 0.5f,
                                              ImGui::GetColorU32(ImGui::IsItemHovered() ? kControlOutlineHover : kControlOutline), 0, width);
    }
    return pressed;
}

Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

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
    const f32 width = ImGui::GetFontSize() * 7.5f - ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushClipRect(at, {at.x + width, at.y + ImGui::GetFrameHeight()}, true);
    ImGui::TextUnformatted(label);
    ImGui::PopClipRect();
    if (ImGui::CalcTextSize(label).x > width) ImGui::SetItemTooltip("%s", label); // cut off: the whole name on hover
    ImGui::SameLine(ImGui::GetFontSize() * 7.5f);
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
    loadPrefs();
    loadLanguage();
    if (!languageOverride.empty()) setLanguage(languageOverride);
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

bool App::dirty() const { return writeScene(m_scene) != m_savedText || m_assets.hasUnsavedEdits(); }

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
    m_view2d = projectSetting("view2d") == "true";
    if (m_view2d) { // face the XY plane around the origin
        m_yaw = m_pitch = 0.0f;
        m_pivot = {0.0f, 0.0f, 0.0f};
        m_distance = 12.0f;
    }
    log("Project " + m_project + (m_view2d ? " (2D)" : ""));
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
    m_assets.discardEdits(); // painted heightmaps/scatter of the scene left behind (the save prompt came first)
    m_brushEdits.clear();
    m_brushStroke = false;
    m_savedText = writeScene(m_scene);
    m_lastSnapshot = m_savedText;
    m_undo.clear();
    m_redo.clear();
    m_fileTime = fileTime(path);
    m_externalChange = false;
    m_warnings = warnings;
    m_selected = 0;
    m_autosaveAt = m_time + m_autosaveMinutes * 60.0;
    loadViewState();
    addRecent(path);
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
    m_assets.discardEdits(); // painted heightmaps/scatter of the scene left behind (the save prompt came first)
    m_brushEdits.clear();
    m_brushStroke = false;
    m_savedText = writeScene(m_scene);
    m_lastSnapshot = m_savedText;
    m_undo.clear();
    m_redo.clear();
    m_warnings.clear();
    m_selected = 0;
    m_hidden.clear();
    m_locked.clear();
    for (Bookmark& b : m_bookmarks) b = {};
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
    if (!m_assets.saveEdits(error)) log("Cannot save painted files: " + error);
    m_savedText = writeScene(m_scene);
    m_fileTime = fileTime(m_path);
    m_externalChange = false;
    m_autosaveAt = m_time + m_autosaveMinutes * 60.0;
    addRecent(m_path);
    saveViewState();
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
    if (!from.back().empty() && from.back()[0] == '\x01') { // a brush stroke: swap the painted file's data
        const size_t index = std::strtoul(from.back().c_str() + 1, nullptr, 10);
        to.push_back(from.back());
        from.pop_back();
        if (index < m_brushEdits.size()) swapBrushEdit(m_brushEdits[index]);
        return;
    }
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
    const std::vector<u32> roots = selectedRoots();
    if (roots.empty()) return;
    std::vector<u32> newRoots;
    for (u32 root : roots) {
        const std::vector<u32> ids = subtree(m_scene, root);
        std::map<u32, u32> remap;
        for (u32 id : ids) remap[id] = m_scene.nextId++;
        std::vector<Entity> copies;
        for (u32 id : ids) {
            Entity e = *m_scene.find(id);
            e.id = remap[id];
            if (id != root) e.parent = remap[e.parent];
            copies.push_back(std::move(e));
        }
        for (Entity& e : copies) m_scene.entities.push_back(std::move(e));
        newRoots.push_back(remap[root]);
    }
    m_selection = newRoots;
    m_selected = newRoots.back();
}

void App::deleteSelected() {
    std::vector<u32> ids;
    for (u32 root : selectedRoots())
        for (u32 id : subtree(m_scene, root)) ids.push_back(id);
    if (ids.empty()) return;
    std::erase_if(m_scene.entities, [&](const Entity& e) { return std::find(ids.begin(), ids.end(), e.id) != ids.end(); });
    m_selected = 0;
    m_selection.clear();
}

bool App::isSelected(u32 id) const { return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end(); }

void App::clickSelect(u32 id, bool toggle) {
    if (!toggle) {
        m_selected = id;
        m_selection.clear();
        if (id) m_selection.push_back(id);
        return;
    }
    if (!id) return;
    if (isSelected(id)) {
        std::erase(m_selection, id);
        m_selected = m_selection.empty() ? 0 : m_selection.back();
    } else {
        m_selection.push_back(id);
        m_selected = id;
    }
}

// Code that sets m_selected alone selects just that object
void App::syncSelection() {
    std::erase_if(m_selection, [&](u32 id) { return !m_scene.find(id); });
    if (!m_scene.find(m_selected)) m_selected = m_selection.empty() ? 0 : m_selection.back();
    if (!m_selected) m_selection.clear();
    else if (!isSelected(m_selected)) m_selection = {m_selected};
}

std::vector<u32> App::selectedRoots() const {
    std::vector<u32> roots;
    for (u32 id : m_selection) {
        bool under = false;
        for (u32 other : m_selection) under |= other != id && isDescendant(m_scene, id, other);
        if (!under && m_scene.find(id)) roots.push_back(id);
    }
    return roots;
}

bool App::worldBox(const Entity& e, Vec3& lo, Vec3& hi) {
    const Transform w = worldTransform(m_scene, e);
    Vec3 blo, bhi;
    if (!entityBounds(e, blo, bhi)) {
        lo = hi = w.position;
        return false;
    }
    const Quat r = e.is(kKindTerrain) ? Quat{} : w.rotation;
    lo = {1e30f, 1e30f, 1e30f};
    hi = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        const Vec3 l{(i & 1) ? bhi.x : blo.x, (i & 2) ? bhi.y : blo.y, (i & 4) ? bhi.z : blo.z};
        const Vec3 p = w.position + rotate(r, {l.x * w.scale.x, l.y * w.scale.y, l.z * w.scale.z});
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return true;
}

namespace {
void moveWorld(const Scene& scene, Entity& e, Vec3 delta) {
    Transform w = worldTransform(scene, e);
    w.position += delta;
    setWorldTransform(scene, e, w);
}
} // namespace

void App::alignSelection(int axis, int mode) {
    const std::vector<u32> roots = selectedRoots();
    Entity* primary = m_scene.find(m_selected);
    if (roots.size() < 2 || !primary) return;
    Vec3 plo, phi;
    worldBox(*primary, plo, phi);
    const auto value = [&](Vec3 lo, Vec3 hi) { return mode == 0 ? (&lo.x)[axis] : mode == 2 ? (&hi.x)[axis] : ((&lo.x)[axis] + (&hi.x)[axis]) * 0.5f; };
    const f32 target = value(plo, phi);
    for (u32 id : roots) {
        Entity* e = m_scene.find(id);
        if (!e || id == m_selected) continue;
        Vec3 lo, hi;
        worldBox(*e, lo, hi);
        Vec3 delta{};
        (&delta.x)[axis] = target - value(lo, hi);
        moveWorld(m_scene, *e, delta);
    }
}

void App::distributeSelection(int axis) {
    std::vector<std::pair<f32, u32>> order;
    for (u32 id : selectedRoots()) {
        Vec3 lo, hi;
        worldBox(*m_scene.find(id), lo, hi);
        order.push_back({((&lo.x)[axis] + (&hi.x)[axis]) * 0.5f, id});
    }
    if (order.size() < 3) return;
    std::sort(order.begin(), order.end());
    const f32 first = order.front().first, step = (order.back().first - first) / static_cast<f32>(order.size() - 1);
    for (size_t i = 1; i + 1 < order.size(); ++i) {
        Vec3 delta{};
        (&delta.x)[axis] = first + step * static_cast<f32>(i) - order[i].first;
        moveWorld(m_scene, *m_scene.find(order[i].second), delta);
    }
}

// Each selected object down onto the highest box top below it (objects that aren't selected, hidden, markers or areas), else y = 0
void App::dropToFloor() {
    const std::vector<u32> roots = selectedRoots();
    for (u32 id : roots) {
        Entity* e = m_scene.find(id);
        if (!e) continue;
        Vec3 lo, hi;
        worldBox(*e, lo, hi);
        f32 floor = 0.0f;
        bool found = false;
        for (const Entity& o : m_scene.entities) {
            if (isSelected(o.id) || m_hidden.count(o.id) || isMarker(o) || isArea(o) || o.is(kKindScatter) || isDescendant(m_scene, o.id, id)) continue;
            Vec3 olo, ohi;
            if (!worldBox(o, olo, ohi)) continue;
            if (ohi.x < lo.x || olo.x > hi.x || ohi.z < lo.z || olo.z > hi.z || ohi.y > lo.y + 0.01f) continue;
            if (!found || ohi.y > floor) floor = ohi.y, found = true;
        }
        if (!found && lo.y < 0.0f) continue; // already below the floor: leave it
        moveWorld(m_scene, *e, {0.0f, floor - lo.y, 0.0f});
    }
}

// ---------------------------------------------------------------- View state, preferences, autosave

std::string App::viewStatePath() const {
    if (m_project.empty() || m_path.empty()) return {};
    return utf8Of(pathOf(m_project) / "build" / "editor" / (utf8Of(pathOf(m_path).stem()) + ".view"));
}

// The MealKit editor's format: "hidden 3 5" / "locked 7" + "camera <1~9> x y z yaw pitch"
void App::loadViewState() {
    m_hidden.clear();
    m_locked.clear();
    for (Bookmark& b : m_bookmarks) b = {};
    const std::string path = viewStatePath();
    if (path.empty()) return;
    std::ifstream in(pathOf(path));
    std::string line;
    while (std::getline(in, line)) {
        int slot = 0;
        Bookmark b;
        if (std::sscanf(line.c_str(), "camera %d %f %f %f %f %f", &slot, &b.position.x, &b.position.y, &b.position.z, &b.yaw, &b.pitch) == 6 &&
            slot >= 1 && slot <= 9) {
            b.set = true;
            m_bookmarks[slot - 1] = b;
            continue;
        }
        std::set<u32>* set = line.rfind("hidden", 0) == 0 ? &m_hidden : line.rfind("locked", 0) == 0 ? &m_locked : nullptr;
        if (!set) continue;
        std::istringstream ids(line.substr(6));
        for (u32 id; ids >> id;)
            if (id) set->insert(id);
    }
}

void App::saveViewState() const {
    const std::string path = viewStatePath();
    if (path.empty()) return;
    std::string text;
    for (auto [name, set] : {std::pair{"hidden", &m_hidden}, std::pair{"locked", &m_locked}}) {
        text += name;
        for (u32 id : *set) text += " " + std::to_string(id);
        text += "\n";
    }
    for (int i = 0; i < 9; ++i) {
        const Bookmark& b = m_bookmarks[i];
        if (!b.set) continue;
        char line[160];
        std::snprintf(line, sizeof(line), "camera %d %.3f %.3f %.3f %.4f %.4f\n", i + 1, b.position.x, b.position.y, b.position.z, b.yaw, b.pitch);
        text += line;
    }
    std::error_code ec;
    std::filesystem::create_directories(pathOf(path).parent_path(), ec);
    std::ofstream(pathOf(path), std::ios::binary) << text;
}

void App::setHidden(const std::vector<u32>& ids, bool hidden) {
    for (u32 id : ids) {
        if (hidden) m_hidden.insert(id);
        else m_hidden.erase(id);
    }
    saveViewState();
}

void App::setLocked(const std::vector<u32>& ids, bool locked) {
    for (u32 id : ids) {
        if (locked) m_locked.insert(id);
        else m_locked.erase(id);
    }
    saveViewState();
}

namespace {
std::string prefsFile() {
    char* dir = SDL_GetPrefPath("MealKit", "MkRender");
    if (!dir) return {};
    std::string path = std::string(dir) + "settings.txt";
    SDL_free(dir);
    return path;
}
} // namespace

void App::loadPrefs() {
    const std::string path = prefsFile();
    if (path.empty()) return;
    std::ifstream in(pathOf(path));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp), value = line.substr(sp + 1);
        f32 v = 0.0f;
        const bool number = parseFloat(value, v);
        if (key == "recent") m_recent.push_back(value);
        else if (key == "grid") m_showGrid = value == "true";
        else if (key == "alwaysSnap") m_alwaysSnap = value == "true";
        else if (key == "markers") m_showMarkers = value != "false";
        else if (key == "compressTextures") m_assets.setCompressTextures(value != "false");
        else if (key == "thumbnails") m_showThumbs = value != "false";
        else if (number && key == "gridStep") m_gridStep = std::max(v, 0.01f);
        else if (number && key == "snapMove") m_snapMove = std::max(v, 0.001f);
        else if (number && key == "snapRotate") m_snapRotate = std::max(v, 0.1f);
        else if (number && key == "snapScale") m_snapScale = std::max(v, 0.001f);
        else if (number && key == "autosaveMinutes") m_autosaveMinutes = std::max(v, 0.0f);
    }
}

void App::savePrefs() const {
    const std::string path = prefsFile();
    if (path.empty()) return;
    std::ofstream out(pathOf(path), std::ios::binary);
    out << "# MkRender preferences\n";
    out << "grid " << (m_showGrid ? "true" : "false") << "\ngridStep " << floatText(m_gridStep) << "\nalwaysSnap " << (m_alwaysSnap ? "true" : "false")
        << "\nmarkers " << (m_showMarkers ? "true" : "false") << "\nsnapMove " << floatText(m_snapMove) << "\nsnapRotate " << floatText(m_snapRotate)
        << "\nsnapScale " << floatText(m_snapScale) << "\nautosaveMinutes " << floatText(m_autosaveMinutes)
        << "\ncompressTextures " << (m_assets.compressTextures() ? "true" : "false") << "\nthumbnails " << (m_showThumbs ? "true" : "false") << "\n";
    for (const std::string& r : m_recent) out << "recent " << r << "\n";
}

void App::addRecent(const std::string& path) {
    std::error_code ec;
    const std::string full = utf8Of(std::filesystem::absolute(pathOf(path), ec).lexically_normal());
    std::erase_if(m_recent, [&](const std::string& r) { return lower(r) == lower(full); });
    m_recent.insert(m_recent.begin(), full);
    if (m_recent.size() > 12) m_recent.resize(12);
    savePrefs();
}

// The unsaved scene every few minutes, next to the editor's files (never over the scene itself)
std::string App::autosavePath() const {
    const std::string name = (m_path.empty() ? std::string("untitled") : utf8Of(pathOf(m_path).stem())) + ".scene";
    if (!m_project.empty()) return utf8Of(pathOf(m_project) / "build" / "editor" / "autosave" / name);
    const std::string prefs = prefsFile();
    return prefs.empty() ? std::string() : utf8Of(pathOf(prefs).parent_path() / "autosave" / name);
}

void App::autosave() {
    if (m_autosaveMinutes <= 0.0f || m_time < m_autosaveAt) return;
    m_autosaveAt = m_time + m_autosaveMinutes * 60.0;
    if (!dirty()) return;
    const std::string path = autosavePath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(pathOf(path).parent_path(), ec);
    std::string error;
    if (saveScene(path, m_scene, error)) m_status = trf("Autosaved to %s", path.c_str()), m_statusTime = m_time;
    else log("Autosave failed: " + error);
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

void App::setShot(Vec3 eye, f32 yawDegrees, f32 pitchDegrees) {
    m_shot = true;
    m_playAnims = false;
    m_selection.clear();
    m_selected = 0;
    m_yaw = yawDegrees * kDeg;
    m_pitch = -pitchDegrees * kDeg; // the editor's pitch looks up when positive
    m_fov = 60.0f;                  // the editor's kFovY
    m_distance = 10.0f;
    m_pivot = eye + cameraForward() * m_distance;
}

Vec3 App::cameraForward() const { return {std::cos(m_pitch) * std::sin(m_yaw), -std::sin(m_pitch), std::cos(m_pitch) * std::cos(m_yaw)}; }
Vec3 App::cameraEye() const { return m_pivot - cameraForward() * m_distance; }
Mat4 App::viewMatrix() const { return lookAtLH(cameraEye(), m_pivot, {0, 1, 0}); }
Mat4 App::projMatrix() const {
    if (m_view2d) return orthographicReversedZ(orthoHalfHeight(), std::max(1.0f, m_view.width) / std::max(1.0f, m_view.height), -4000.0f, 4000.0f);
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
    if (e.is(kKindSprite)) {
        f32 px = 0.5f, py = 0.5f;
        parseFloat(e.field("pivotX"), px);
        parseFloat(e.field("pivotY"), py);
        const Vec3 k = spriteSize(e);
        lo = {-px * k.x, -py * k.y, -0.02f};
        hi = {(1.0f - px) * k.x, (1.0f - py) * k.y, 0.02f};
        return true;
    }
    if (!e.model.empty() && !isArea(e) && !e.is(kKindTerrain) && !e.is(kKindSprite)) {
        const ModelEntry& m = m_assets.model(e.model);
        if (m.ok) {
            lo = m.boundsMin;
            hi = m.boundsMax;
        }
    }
    return true;
}

void App::mouseRay(f32 mx, f32 my, Vec3& eye, Vec3& dir) const {
    const f32 nx = (mx - m_uiX) / m_uiW * 2.0f - 1.0f, ny = 1.0f - (my - m_uiY) / m_uiH * 2.0f;
    const Vec3 forward = cameraForward();
    const Vec3 right = normalize(cross({0, 1, 0}, forward));
    const Vec3 up = cross(forward, right);
    if (m_view2d) { // parallel rays from the view plane
        const f32 h = orthoHalfHeight();
        eye = cameraEye() + right * (nx * h * m_uiW / m_uiH) + up * (ny * h) - forward * 4000.0f;
        dir = forward;
        return;
    }
    const f32 t = std::tan(m_fov * kDeg * 0.5f);
    dir = normalize(forward + right * (nx * t * m_uiW / m_uiH) + up * (ny * t));
    eye = cameraEye();
}

f32 App::rayEntity(const Entity& e, Vec3 eye, Vec3 dir) {
    const Transform w = worldTransform(m_scene, e);
    if (e.is(kKindTerrain)) { // the heights, not the box
    const TerrainView& t = m_assets.terrain(e.id, e.field("heightmap"), w.scale);
    return t.map ? raycastTerrain(*t.map, w.position, w.scale, eye, dir, 1e5f) : -1.0f;
    }
    Vec3 lo, hi;
    if (!entityBounds(e, lo, hi)) return -1.0f;
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
    if (!hit) return -1.0f;
    if (isArea(e) && t0 <= 0.0f) return -1.0f; // inside a volume: pick what's in it instead
    // t0 is in world units along dir (the local ray is the same ray, only rescaled per axis)
    const Vec3 hitLocal = o + d * t0;
    const Vec3 hitWorld = w.position + rotate(w.rotation, {hitLocal.x * w.scale.x, hitLocal.y * w.scale.y, hitLocal.z * w.scale.z});
    return length(hitWorld - eye);
}

bool App::raySurface(Vec3 eye, Vec3 dir, u32 skipId, f32& distance) {
    distance = 1e30f;
    for (const Entity& e : m_scene.entities) {
        if (e.id == skipId || m_hidden.count(e.id) || isMarker(e) || isArea(e) || e.is(kKindScatter) || e.is(kKindSprite)) continue;
        const f32 d = rayEntity(e, eye, dir);
        if (d >= 0.0f && d < distance) distance = d;
    }
    return distance < 1e29f;
}

bool App::groundPoint(f32 mx, f32 my, Vec3& out) {
    Vec3 eye, dir;
    mouseRay(mx, my, eye, dir);
    f32 dist = 0.0f;
    const u32 hit = pick(mx, my, &dist);
    const Entity* e = m_scene.find(hit);
    if (e && !isMarker(*e) && !isArea(*e) && !e->is(kKindScatter)) {
        out = eye + dir * dist;
        return true;
    }
    if (m_view2d && std::fabs(dir.z) > 1e-4f) { // 2D: the z = 0 plane
        out = eye + dir * (-eye.z / dir.z);
        return true;
    }
    if (!m_view2d && dir.y < -1e-4f) {
        out = eye + dir * (-eye.y / dir.y);
        return true;
    }
    out = m_pivot;
    return false;
}

Vec3 App::spriteSize(const Entity& e) {
    f32 ppu = 0.0f;
    parseFloat(e.field("pixelsPerUnit"), ppu);
    if (ppu <= 0.0f) return {1, 1, 1};
    f32 columns = 1, rows = 1, cell = 0;
    parseFloat(e.field("columns"), columns);
    parseFloat(e.field("rows"), rows);
    parseFloat(e.field("cell"), cell);
    const SpriteEntry& s = m_assets.sprite(e.field("image"), static_cast<int>(columns), static_cast<int>(rows), static_cast<int>(cell), 0.5f, 0.5f, false, false);
    if (!s.cellWidth) return {1, 1, 1};
    return {s.cellWidth / ppu, s.cellHeight / ppu, 1};
}

u32 App::pick(f32 mx, f32 my, f32* hitDistance) {
    Vec3 eye, dir;
    mouseRay(mx, my, eye, dir);
    u32 best = 0;
    f32 bestT = 1e30f;
    for (const Entity& e : m_scene.entities) {
        if (m_hidden.count(e.id) || m_locked.count(e.id)) continue;
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
        const f32 dist = rayEntity(e, eye, dir);
        if (dist < 0.0f) continue;
        if (dist < bestT) bestT = dist, best = e.id;
    }
    if (hitDistance) *hitDistance = bestT;
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
    p.time = m_shot ? 0.0f : static_cast<f32>(m_time);
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
    p.environment = m_environmentId;
    p.environmentIntensity = env.hdriIntensity;
    p.environmentRotation = env.hdriRotation * kDeg;
    p.probes = m_probeList.data();
    p.probeCount = static_cast<u32>(std::min<size_t>(m_probeList.size(), kMaxReflectionProbes));
    post.lut = m_lutId;
    return p;
}

void App::reportProblem(const std::string& what) {
    if (std::find(m_modelProblems.begin(), m_modelProblems.end(), what) != m_modelProblems.end()) return;
    m_modelProblems.push_back(what);
    log(what);
}

std::string App::sceneStem() const { return m_path.empty() ? std::string("untitled") : utf8Of(pathOf(m_path).stem()); }

// ---------------------------------------------------------------- Brushes

bool App::brushActive() const {
    const Entity* e = m_scene.find(m_selected);
    return m_brushOn && e && m_selection.size() <= 1 && (e->is(kKindTerrain) || e->is(kKindScatter));
}

void App::swapBrushEdit(BrushEdit& edit) {
    if (edit.terrain) {
        if (Heightmap* map = m_assets.heightmap(edit.path)) {
            std::swap(*map, edit.map);
            m_assets.createHeightmap(edit.path, *map); // same data, rebuilds every chunk
        }
    } else {
        std::swap(m_assets.scatterEdit(edit.path).items, edit.items);
    }
}

void App::drawBrushOutline(const Heightmap* map, Vec3 center, Vec3 size, u32 color) {
    ImVec2 points[48];
    int count = 0;
    for (int i = 0; i < 48; ++i) {
        const f32 a = static_cast<f32>(i) * (6.2831853f / 48.0f);
        Vec3 p = m_brushPoint + Vec3{std::cos(a) * m_brushRadius, 0.0f, std::sin(a) * m_brushRadius};
        f32 y = p.y;
        if (map && terrainHeightAt(*map, center, size, p.x, p.z, &y)) p.y = y;
        f32 x, sy;
        if (project(p, x, sy)) points[count++] = {x, sy};
    }
    if (count > 2) ImGui::GetForegroundDrawList()->AddPolyline(points, count, color, ImDrawFlags_Closed, 2.0f);
}

bool App::brushInput(bool free) {
    m_brushHit = false;
    if (!brushActive()) {
        m_brushStroke = false;
        return false;
    }
    Entity& e = *m_scene.find(m_selected);
    ImGuiIO& io = ImGui::GetIO();
    Vec3 eye, dir;
    mouseRay(io.MousePos.x, io.MousePos.y, eye, dir);
    const Transform w = worldTransform(m_scene, e);
    const bool terrain = e.is(kKindTerrain);
    const std::string path = terrain ? e.field("heightmap") : e.field("file");
    if (path.empty()) return false;
    Heightmap* map = terrain ? m_assets.heightmap(path) : nullptr;
    if (terrain && !map) return false;
    if (free) {
        f32 t = -1.0f;
        if (terrain) t = raycastTerrain(*map, w.position, w.scale, eye, dir, 1e5f);
        else if (!raySurface(eye, dir, e.id, t)) t = -1.0f;
        if (t >= 0.0f) {
            m_brushHit = true;
            m_brushPoint = eye + dir * t;
        }
    }
    if (m_brushStroke && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // stroke done: one undo step
        m_brushStroke = false;
        m_brushEdits.push_back(std::move(m_strokeBefore));
        m_undo.push_back("\x01" + std::to_string(m_brushEdits.size() - 1));
        while (m_undo.size() > 200) m_undo.pop_front();
        m_redo.clear();
    }
    if (!m_brushStroke && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_brushHit && !io.KeyAlt) {
        m_brushStroke = true;
        m_strokeBefore = {};
        m_strokeBefore.terrain = terrain;
        m_strokeBefore.path = path;
        if (terrain) {
            m_strokeBefore.map = *map;
            const f32 bottom = w.position.y - w.scale.y * 0.5f;
            m_flattenTo = static_cast<u16>(std::lround(std::clamp((m_brushPoint.y - bottom) / std::max(w.scale.y, 0.001f), 0.0f, 1.0f) * 65535.0f));
        } else {
            m_strokeBefore.items = m_assets.scatter(path).items;
        }
    }
    const f32 dt = std::min(io.DeltaTime, 0.1f);
    if (m_brushStroke && m_brushHit) {
        if (terrain) {
            const auto brush = static_cast<TerrainBrush>(m_brush);
            // Raise/Lower: strength 1 = a fifth of the box height per second at the center (the editor's numbers)
            const f32 amount = (brush == TerrainBrush::Raise || brush == TerrainBrush::Lower ? 0.2f : 6.0f) * m_brushStrength * dt;
            const SampleRect rect = applyBrush(*map, w.position, w.scale, brush, m_brushPoint, m_brushRadius, amount, m_flattenTo);
            if (!rect.empty) m_assets.heightmapEdited(path, rect);
        } else if (m_scatterErase != io.KeyShift) {
            if (!m_assets.scatter(path).items.empty()) eraseScatter(m_assets.scatterEdit(path).items, m_brushPoint, m_brushRadius);
        } else {
            ScatterPaint p;
            p.center = m_brushPoint;
            p.radius = m_brushRadius;
            parseFloat(e.field("density"), p.density);
            parseFloat(e.field("minScale"), p.minScale);
            parseFloat(e.field("maxScale"), p.maxScale);
            p.randomYaw = e.field("randomYaw") != "false";
            // Strength 1 fills about the whole circle in half a second
            const f32 area = 3.14159265f * m_brushRadius * m_brushRadius * std::max(p.density, 0.0f);
            const u32 maxAdd = static_cast<u32>(std::ceil(area * m_brushStrength * dt * 2.0f));
            const auto random01 = [this] { // editor-only randomness
                m_scatterSeed = m_scatterSeed * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<f32>(m_scatterSeed >> 40) / 16777216.0f;
            };
            const f32 top = m_brushPoint.y + m_brushRadius + 50.0f;
            const auto floorAt = [&](f32 x, f32 z, f32* y) {
                f32 d = 0.0f;
                if (!raySurface({x, top, z}, {0.0f, -1.0f, 0.0f}, e.id, d)) return false;
                *y = top - d;
                return true;
            };
            std::vector<ScatterInstance> items = m_assets.scatter(path).items;
            if (paintScatter(items, p, maxAdd, floorAt, random01)) m_assets.scatterEdit(path).items = std::move(items);
        }
    }
    if (m_brushHit) {
        const u32 color = terrain ? IM_COL32(255, 220, 90, 220) : (m_scatterErase != io.KeyShift ? IM_COL32(255, 110, 90, 220) : IM_COL32(120, 230, 120, 220));
        drawBrushOutline(map, w.position, w.scale, color);
    }
    return m_brushStroke || m_brushHit;
}

void App::drawBrushInspector(Entity& e) {
    const bool terrain = e.is(kKindTerrain);
    ImGui::SeparatorText(terrain ? tr("Terrain brush") : tr("Scatter brush"));
    const std::string key = terrain ? "heightmap" : "file";
    std::string path = e.field(key);
    if (path.empty()) {
        if (m_project.empty()) {
            ImGui::TextDisabled(tr("Open a project to paint (files go to its assets/terrain)."));
            return;
        }
        const std::string made = "terrain/" + sceneStem() + "_" + std::to_string(e.id) + (terrain ? ".png" : ".scatter"); // scene::terrainFileFor
        if (iconButton(kIconAdd, terrain ? tr("New flat heightmap") : tr("New instance file"))) {
            snapshot();
            e.setField(key, made);
            if (terrain) m_assets.createHeightmap(made, flatHeightmap(129, 16384)); // 128 cells, a quarter up so brushes can dig
            else m_assets.scatterEdit(made);
            m_brushOn = true;
        }
        ImGui::SetItemTooltip(tr("%s (written when the scene is saved)"), made.c_str());
        return;
    }
    if (terrain) {
        const Heightmap* map = m_assets.heightmap(path);
        if (!map) {
            ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f}, tr("Cannot read %s"), path.c_str());
            return;
        }
        ImGui::TextDisabled(tr("%u x %u samples, %.2f m cells"), map->size, map->size, worldTransform(m_scene, e).scale.x / static_cast<f32>(map->size - 1));
    } else {
        ImGui::TextDisabled(tr("%zu instances"), m_assets.scatter(path).items.size());
    }
    checkbox(tr("Brush (B)"), &m_brushOn);
    if (!terrain) {
        ImGui::SameLine();
        if (iconButton(kIconClear, tr("Clear instances")) && !m_assets.scatter(path).items.empty()) {
            m_strokeBefore = {};
            m_strokeBefore.terrain = false;
            m_strokeBefore.path = path;
            m_strokeBefore.items = m_assets.scatter(path).items;
            m_assets.scatterEdit(path).items.clear();
            m_brushEdits.push_back(std::move(m_strokeBefore));
            m_undo.push_back("\x01" + std::to_string(m_brushEdits.size() - 1));
            m_redo.clear();
        }
    }
    ImGui::BeginDisabled(!m_brushOn);
    if (terrain) {
        const char* names[] = {tr("Raise"), tr("Lower"), tr("Flatten"), tr("Smooth")}; // per frame: follows a language change
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (radioButton(names[i], m_brush == i)) m_brush = i;
        }
    } else {
        if (radioButton(tr("Paint"), !m_scatterErase)) m_scatterErase = false;
        ImGui::SameLine();
        if (radioButton(tr("Erase"), m_scatterErase)) m_scatterErase = true;
    }
    row(tr("Radius (m)"));
    ImGui::SliderFloat("##bradius", &m_brushRadius, 0.5f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    row(tr("Strength"));
    ImGui::SliderFloat("##bstrength", &m_brushStrength, 0.01f, 1.0f, "%.2f");
    ImGui::TextDisabled(terrain ? tr("Left drag paints; Flatten keeps the height under the stroke start; Ctrl+Z undoes a stroke")
                                : tr("Left drag paints onto terrain and floors (Shift erases); density and sizes are fields below"));
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------- Editor link

namespace {
std::string jsonValue(const std::string& json, const char* key) { // flat JSON: the text after "key": up to , or }
    const std::string k = std::string("\"") + key + "\":";
    size_t at = json.find(k);
    if (at == std::string::npos) return {};
    at += k.size();
    if (at < json.size() && json[at] == '"') {
        const size_t end = json.find('"', at + 1);
        return end == std::string::npos ? std::string() : json.substr(at + 1, end - at - 1);
    }
    const size_t end = json.find_first_of(",}", at);
    return json.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
} // namespace

bool processAlive(u32 pid); // Process.cpp

void App::pollLink() {
    if (m_project.empty() || m_time < m_linkPollAt) return;
    m_linkPollAt = m_time + 1.0;
    const bool was = m_linkFound;
    m_linkFound = false;
    for (const mkcontract::live::Instance& in : mkcontract::live::findInstances(m_project)) {
        if (in.kind != "editor" || m_linkDead.count(in.pid) || !processAlive(in.pid)) continue; // a crashed editor's file stays behind
        const mkcontract::live::HttpResult r = mkcontract::live::request(in, "GET", "scene/state");
        if (r.status != 200) { // gone, or an editor without the scene service: not asked again
            m_linkDead.insert(in.pid);
            continue;
        }
        m_editor = in;
        m_linkFound = true;
        m_linkMode = jsonValue(r.body, "mode");
        m_linkScene = jsonValue(r.body, "scene");
        m_linkGame = jsonValue(r.body, "game") == "true";
        m_linkDirty = jsonValue(r.body, "dirty") == "true";
        break;
    }
    if (m_linkFound != was) log(m_linkFound ? "MealKit editor found (pid " + std::to_string(m_editor.pid) + "): F5 plays the saved scene there" : "MealKit editor link lost");
    if (m_linkFound && linkPlayWhenReady && m_linkGame) {
        linkPlayWhenReady = false;
        linkPlay(false);
    }
}

bool App::linkPost(const char* path, const std::string& body) {
    if (!m_linkFound) {
        log("No MealKit editor is running for this project (open the project in the editor)");
        return false;
    }
    const mkcontract::live::HttpResult r = mkcontract::live::request(m_editor, "POST", path, body, 2000);
    if (r.status == 200) {
        m_linkPollAt = 0.0; // refresh the state soon
        return true;
    }
    const std::string error = jsonValue(r.body, "error");
    log(std::string("Editor: ") + (r.status == 0 ? "no answer" : error.empty() ? "error " + std::to_string(r.status) : error));
    return false;
}

void App::linkPlay(bool fromHere) {
    if (m_path.empty()) {
        log("Save the scene into the project first: the editor plays the saved file");
        return;
    }
    if (dirty() && !save()) return;
    const std::string file = m_assets.relative(m_path);
    std::string body = "file=" + mkcontract::live::urlEncode(file);
    if (fromHere) {
        const Vec3 eye = cameraEye();
        body += "&from=1&x=" + floatText(eye.x) + "&y=" + floatText(eye.y) + "&z=" + floatText(eye.z) + "&yaw=" + floatText(m_yaw) +
                "&spawn=" + std::to_string(m_selected);
    }
    if (linkPost("scene/play", body)) log(std::string("Editor plays ") + file + (fromHere ? " from this camera" : ""));
}

void App::linkStop() {
    if (linkPost("scene/stop", "")) log("Editor: stop");
}

void App::linkLook(u32 cameraId, const std::string& cutscene) {
    if (m_path.empty()) return;
    if (dirty() && !save()) return;
    std::string body = "file=" + mkcontract::live::urlEncode(m_assets.relative(m_path));
    body += cutscene.empty() ? "&camera=" + std::to_string(cameraId) : "&cutscene=" + mkcontract::live::urlEncode(cutscene);
    if (linkPost("scene/look", body)) log(cutscene.empty() ? "Editor looks through camera " + std::to_string(cameraId) : "Editor previews cutscene " + cutscene);
}

void App::drawLinkMenu() {
    if (!ImGui::BeginMenu(tr("Editor"))) return;
    if (m_project.empty()) ImGui::TextDisabled(tr("Open a project: MkRender looks for its running MealKit editor"));
    else if (!m_linkFound) ImGui::TextDisabled(tr("No MealKit editor running for this project"));
    else {
        ImGui::TextDisabled(tr("Editor (pid %u): %s, %s%s"), m_editor.pid, m_linkMode == "play" ? tr("playing") : tr("editing"),
                            m_linkScene.empty() ? tr("no scene") : m_linkScene.c_str(), m_linkDirty ? tr(" (unsaved there)") : "");
        if (!m_linkGame) ImGui::TextDisabled(tr("Its game code isn't loaded yet (build it in the editor)"));
    }
    const bool can = m_linkFound && m_linkGame && m_linkMode != "play";
    if (ImGui::MenuItem(tr("Play in editor"), "F5", false, can)) linkPlay(false);
    if (ImGui::MenuItem(tr("Play from here"), "Shift+F5", false, can)) linkPlay(true);
    ImGui::SetItemTooltip(tr("Solo play from this camera (or from the selected spawn point)"));
    if (ImGui::MenuItem(tr("Stop"), "F5", false, m_linkFound && m_linkMode == "play")) linkStop();
    ImGui::Separator();
    const Entity* e = m_scene.find(m_selected);
    if (ImGui::MenuItem(tr("Look through selected camera"), nullptr, false, m_linkFound && e && e->is(kKindCamera) && m_linkMode != "play")) linkLook(e->id);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::InputTextWithHint("##cutscene", tr("cutscene name"), m_cutsceneName, sizeof(m_cutsceneName));
    ImGui::SameLine();
    if (iconButton(kIconPlay, tr("Preview cutscene")) && m_cutsceneName[0] && m_linkFound) linkLook(0, m_cutsceneName);
    if (ImGui::MenuItem(tr("End preview"), nullptr, false, m_linkFound && m_linkMode != "play")) linkStop();
    ImGui::TextDisabled(tr("The editor plays and previews the saved scene file; MkRender saves first."));
    ImGui::EndMenu();
}

// ---------------------------------------------------------------- Probes and grading

void App::bakeProbes(u32 onlyId) {
    m_probeQueue.clear();
    m_probeFace = 0;
    m_probeCapturePending = false;
    if (m_project.empty()) {
        log("Open a project first: probes are written to its assets/probes");
        return;
    }
    for (const Entity& e : m_scene.entities)
        if (e.is(kKindProbe) && (!onlyId || e.id == onlyId)) m_probeQueue.push_back(e.id);
    if (m_probeQueue.empty()) log("No reflection probes to bake");
    else log("Baking " + std::to_string(m_probeQueue.size()) + " reflection probe(s)");
}

void App::fillScene(DrawList& list, bool highlight) {
    const std::vector<u32> selection = m_selection;
    const u32 selected = m_selected;
    if (!highlight) {
        m_selection.clear();
        m_selected = 0;
    }
    addScene(list);
    m_selection = selection;
    m_selected = selected;
}

void App::finishProbeFace() {
    m_probeCapturePending = false;
    const u32 size = m_probeSize;
    m_probeFaces.resize(static_cast<size_t>(size) * size * 18);
    if (m_probeCapture.width == size && m_probeCapture.height == size) { // sRGB 8-bit -> linear
        f32* dst = &m_probeFaces[static_cast<size_t>(m_probeFace) * size * size * 3];
        for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i)
            for (u32 c = 0; c < 3; ++c) {
                const f32 v = static_cast<f32>(m_probeCapture.rgba[i * 4 + c]) / 255.0f;
                dst[i * 3 + c] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            }
    }
    if (++m_probeFace < 6) return;
    m_probeFace = 0;
    const u32 id = m_probeQueue.front();
    m_probeQueue.erase(m_probeQueue.begin());
    Entity* e = m_scene.find(id);
    if (!e) return;
    ProbeMap probe;
    bakeProbe(m_probeFaces.data(), size, probe);
    const std::string rel = "probes/" + sceneStem() + "_" + std::to_string(id) + ".mkprobe"; // scene::probeFileFor
    const std::string file = m_assets.resolve(rel);
    std::error_code ec;
    std::filesystem::create_directories(pathOf(file).parent_path(), ec);
    std::ofstream out(pathOf(file), std::ios::binary | std::ios::trunc);
    const std::string bytes = probeFileBytes(probe);
    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        log("Cannot write " + file);
        return;
    }
    out.close();
    if (e->field("bake") != rel) e->setField("bake", rel);
    log("Baked reflection probe " + rel + " (" + std::to_string(probe.size) + ", " + std::to_string(probe.mips) + " mips)");
}

void App::finishGradingCapture() {
    m_gradeCapturePending = false;
    const std::filesystem::path folder = pathOf(m_project) / "art" / "grading";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string png = utf8Of(folder / (sceneStem() + ".png")), cube = utf8Of(folder / "neutral.cube");
    if (!std::filesystem::exists(pathOf(cube), ec)) {
        std::ofstream out(pathOf(cube), std::ios::binary);
        out << neutralCubeLut(kLutSize);
    }
    SDL_Surface* surface = m_gradeCapture.width ? SDL_CreateSurfaceFrom(static_cast<int>(m_gradeCapture.width), static_cast<int>(m_gradeCapture.height),
                                                                      SDL_PIXELFORMAT_RGBA32, m_gradeCapture.rgba.data(),
                                                                      static_cast<int>(m_gradeCapture.width * 4))
                                                : nullptr;
    const bool ok = surface && SDL_SavePNG(surface, png.c_str());
    SDL_DestroySurface(surface);
    log(ok ? "Grading capture saved: " + png + " (grade it with " + cube + ", export a .cube to assets/lut)" : "Cannot write " + png);
}

void App::renderOffscreen() {
    Renderer& r = *m_renderer;
    if (!m_thumbNext.empty()) {
        Thumb& t = m_thumbs[m_thumbNext];
        if (!t.target) t.target = r.createRenderTarget(kThumbSize, kThumbSize);
        if (t.target) {
            r.drawToTarget(t.target, m_thumbFrame, m_thumbList, m_thumbLighting);
            t.drawn = true;
        }
        m_thumbNext.clear();
    }
    if (m_sheetPending) {
        if (!m_sheetTarget) m_sheetTarget = r.createRenderTarget(kSheetSize, kSheetSize);
        if (m_sheetTarget) r.drawToTarget(m_sheetTarget, m_sheetFrame, m_sheetList, LightingModel::Unlit);
        m_sheetPending = false;
    }
    if (m_lookPending) {
        if (!m_lookTarget) m_lookTarget = r.createRenderTarget(kLookSize, kLookSize);
        if (m_lookTarget) r.drawToTarget(m_lookTarget, m_lookFrame, m_lookList, m_lookDrawLighting);
        m_lookPending = false;
    }
    if (!m_probeQueue.empty() && m_bakeListReady) {
        const Entity* e = m_scene.find(m_probeQueue.front());
        if (!e) {
            m_probeQueue.erase(m_probeQueue.begin());
            m_probeFace = 0;
            return;
        }
        if (!m_probeTarget || m_probeTargetSize != m_probeSize) { // a new size makes a new target (the renderer keeps the old one)
            m_probeTarget = r.createRenderTarget(m_probeSize, m_probeSize);
            m_probeTargetSize = m_probeSize;
        }
        const Vec3 eye = worldTransform(m_scene, *e).position, forward = cubeFaceForward(m_probeFace);
        FrameParams frame = frameParams();
        frame.viewport = {0.0f, 0.0f, static_cast<f32>(m_probeSize), static_cast<f32>(m_probeSize)};
        frame.viewProj = lookAtLH(eye, eye + forward, cubeFaceUp(m_probeFace)) * perspectiveReversedZ(3.14159265f * 0.5f, 1.0f, 0.05f, 2000.0f);
        frame.cameraPos = eye;
        frame.cameraForward = forward;
        frame.probes = nullptr; // other probes' old images don't mix in
        frame.probeCount = 0;
        frame.post.toneMapping = false; // reflections stay linear (tone mapping happens once on the screen that receives them)
        r.drawToTarget(m_probeTarget, frame, m_bakeList, m_lighting);
        r.captureTarget(m_probeTarget, &m_probeCapture);
        m_probeCapturePending = true;
        return; // one target capture per frame
    }
    if (m_gradeRequested && m_bakeListReady && m_time > 0.25) {
        m_gradeRequested = false;
        const u32 w = std::max(64u, static_cast<u32>(m_view.width)), h = std::max(64u, static_cast<u32>(m_view.height));
        if (!m_gradeTarget || m_gradeSize[0] != w || m_gradeSize[1] != h) {
            m_gradeTarget = r.createRenderTarget(w, h);
            m_gradeSize[0] = w;
            m_gradeSize[1] = h;
        }
        FrameParams frame = frameParams();
        frame.viewport = {0.0f, 0.0f, static_cast<f32>(w), static_cast<f32>(h)};
        frame.post.lut = 0; // the image before grading (up to tone mapping)
        r.drawToTarget(m_gradeTarget, frame, m_bakeList, m_lighting);
        r.captureTarget(m_gradeTarget, &m_gradeCapture);
        m_gradeCapturePending = true;
    }
}

void App::fill(DrawList& list) {
    // Offscreen captures read back at the last endFrame
    if (m_probeCapturePending) finishProbeFace();
    if (m_gradeCapturePending) finishGradingCapture();
    // Sky HDRI, LUT, reflection probes
    const Environment& env = m_scene.environment;
    m_environmentId = 0;
    if (!env.hdri.empty()) {
        const GpuEntry& g = m_assets.environment(env.hdri);
        m_environmentId = g.id;
        if (!g.error.empty()) reportProblem("Sky HDRI " + env.hdri + ": " + g.error);
    }
    m_lutId = 0;
    const std::string lut = env.postValue("lut");
    if (!lut.empty()) {
        const GpuEntry& g = m_assets.lut(lut);
        m_lutId = g.id;
        if (!g.error.empty()) reportProblem("LUT " + lut + ": " + g.error);
    }
    m_probeList.clear();
    for (const Entity& e : m_scene.entities) {
        if (!e.is(kKindProbe) || e.field("bake").empty()) continue;
        const GpuEntry& g = m_assets.probe(e.field("bake"));
        if (!g.id) continue; // not baked yet: the sky reflection is used
        const Transform w = worldTransform(m_scene, e);
        m_probeList.push_back({w.position, Vec3{std::fabs(w.scale.x), std::fabs(w.scale.y), std::fabs(w.scale.z)} * 0.5f, g.id});
    }
    addScene(list);
    if (m_showThumbs) prepareThumbnail();
    prepareSheet();
    prepareLookDev(m_lastDt);
    // Probe faces and the grading capture draw the scene without selection highlight
    m_bakeListReady = false;
    const bool grade = m_gradeRequested && m_time > 0.25; // after the first frames, when the dock layout gives the view its size
    if (!m_probeQueue.empty() || grade) {
        if (!m_probeQueue.empty() && m_probeFace == 0) {
            f32 size = 128.0f; // project setting probeSize, a power of two 16..512 (mips halve)
            parseFloat(projectSetting("probeSize"), size);
            m_probeSize = 16;
            while (m_probeSize * 2 <= static_cast<u32>(std::clamp(size, 16.0f, 512.0f))) m_probeSize *= 2;
        }
        m_bakeList.clear();
        fillScene(m_bakeList, false);
        m_bakeListReady = true;
    }
}

// ---------------------------------------------------------------- Thumbnails

namespace {
constexpr size_t kThumbLimit = 512; // targets aren't deleted: a cap for very big projects
long long thumbFileTime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
}
} // namespace

u64 App::thumbnail(const std::string& path) {
    auto it = m_thumbs.find(path);
    if (it == m_thumbs.end()) {
        if (m_thumbs.size() < kThumbLimit) m_thumbs.emplace(path, Thumb{});
        return 0;
    }
    Thumb& t = it->second;
    if (m_time - t.checked >= 1.0) { // redrawn after the file changes
        t.checked = m_time;
        const long long time = thumbFileTime(m_assets.resolve(path));
        if (time != t.fileTime) {
            t.fileTime = time;
            t.drawn = false;
        }
    }
    if (!t.target || !t.drawn) return 0;
    const mkrhi::Texture texture = m_renderer->renderTargetTexture(t.target);
    return texture ? mkrhi::imguiTextureId(m_renderer->device(), texture) : 0;
}

// The editor's AssetThumbnails look: grey studio light, the model standing at the origin seen from the front-right
void App::prepareThumbnail() {
    m_thumbNext.clear();
    for (auto& [path, t] : m_thumbs) {
        if (t.drawn) continue;
        m_thumbList.clear();
        FrameParams& frame = m_thumbFrame;
        frame = FrameParams{};
        frame.viewport = {0.0f, 0.0f, static_cast<f32>(kThumbSize), static_cast<f32>(kThumbSize)};
        frame.sunDirection = {0.45f, 0.75f, -0.5f};
        frame.sunColor = {1.0f, 1.0f, 1.0f};
        frame.skyColor = {0.32f, 0.32f, 0.34f};
        frame.groundColor = {0.16f, 0.16f, 0.16f};
        frame.fogColor = {0.16f, 0.17f, 0.19f};
        frame.fogDensity = 0.0f;
        constexpr f32 kFovY = 30.0f * kDeg;
        const bool image = path.size() > 4 && lower(path.substr(path.size() - 4)) == ".png";
        if (image) { // a 1 m quad seen straight on
            const SpriteEntry& s = m_assets.sprite(path, 1, 1, 0, 0.5f, 0.5f, false, false);
            if (s.mesh != kNoMesh) {
                const f32 aspect = s.cellHeight ? static_cast<f32>(s.cellWidth) / static_cast<f32>(s.cellHeight) : 1.0f;
                InstanceData inst;
                inst.scale = aspect >= 1.0f ? Vec3{1.0f, 1.0f / aspect, 1.0f} : Vec3{aspect, 1.0f, 1.0f};
                m_thumbList.add(s.mesh, inst);
            }
            const f32 distance = 0.5f / std::tan(kFovY * 0.5f) * 1.08f;
            const Vec3 eye{0.0f, 0.0f, -distance};
            frame.viewProj = lookAtLH(eye, {0, 0, 0}, {0, 1, 0}) * perspectiveReversedZ(kFovY, 1.0f, 0.05f, distance * 4.0f);
            frame.cameraPos = eye;
            frame.cameraForward = {0, 0, 1};
            m_thumbLighting = LightingModel::Unlit;
        } else {
            const ModelEntry& m = m_assets.model(path);
            const Vec3 lo = m.ok ? m.boundsMin : Vec3{-0.5f, 0.0f, -0.5f}, hi = m.ok ? m.boundsMax : Vec3{0.5f, 1.0f, 0.5f};
            const Vec3 size = hi - lo;
            const f32 radius = std::max(0.5f * length(size), 0.05f);
            const Vec3 center{0.0f, 0.5f * size.y, 0.0f};
            const f32 distance = radius / std::sin(kFovY * 0.5f) * 1.1f;
            const Vec3 eye = center + normalize(Vec3{0.6f, 0.45f, -1.0f}) * distance;
            frame.viewProj = lookAtLH(eye, center, {0, 1, 0}) * perspectiveReversedZ(kFovY, 1.0f, distance * 0.02f, distance * 4.0f);
            frame.cameraPos = eye;
            frame.cameraForward = normalize(center - eye);
            if (m.ok) {
                InstanceData inst;
                inst.position = {-(lo.x + hi.x) * 0.5f, -lo.y, -(lo.z + hi.z) * 0.5f};
                if (m.skinned()) {
                    restPose(m.skeleton, m_pose);
                    skinPalette(m.skeleton, m_pose, m_palette);
                    m_thumbList.addSkinned(m.mesh, inst, m_palette.data(), static_cast<u32>(m_palette.size()));
                } else {
                    m_thumbList.add(m.mesh, inst);
                }
            }
            m_thumbLighting = LightingModel::Pbr;
        }
        m_thumbNext = path;
        return; // one per frame
    }
}

void App::addScene(DrawList& list) {
    m_lights.clear();
    for (const Entity& e : m_scene.entities) {
        if (m_hidden.count(e.id)) continue;
        const Transform w = worldTransform(m_scene, e);
        if (e.is(kKindSprite)) {
            f32 columns = 1, rows = 1, cell = 0, px = 0.5f, py = 0.5f;
            parseFloat(e.field("columns"), columns);
            parseFloat(e.field("rows"), rows);
            parseFloat(e.field("cell"), cell);
            parseFloat(e.field("pivotX"), px);
            parseFloat(e.field("pivotY"), py);
            if (e.field("image").empty()) continue;
            const SpriteEntry& s = m_assets.sprite(e.field("image"), static_cast<int>(columns), static_cast<int>(rows), static_cast<int>(cell), px, py,
                                                   e.field("flipX") == "true", e.field("flipY") == "true");
            if (s.mesh == kNoMesh) {
                if (std::find(m_modelProblems.begin(), m_modelProblems.end(), s.error) == m_modelProblems.end()) {
                    m_modelProblems.push_back(s.error);
                    log("Sprite " + e.name + ": " + s.error);
                }
                continue;
            }
            const Vec3 k = spriteSize(e);
            InstanceData inst;
            inst.color = e.color;
            inst.flags = isSelected(e.id) ? kInstanceHighlight : 0u;
            inst.position = w.position;
            inst.rotation = w.rotation;
            inst.scale = {w.scale.x * k.x, w.scale.y * k.y, 1.0f};
            list.add(s.mesh, inst);
            continue;
        }
        if (e.is(kKindScatter)) {
            const std::string model = e.field("model"), file = e.field("file");
            if (model.empty() || file.empty()) continue;
            const ModelEntry& m = m_assets.model(model);
            const ScatterEntry& sc = m_assets.scatter(file);
            if (!m.ok || !sc.error.empty()) {
                const std::string problem = !m.ok ? model + ": " + m.error : sc.error;
                if (std::find(m_modelProblems.begin(), m_modelProblems.end(), problem) == m_modelProblems.end()) {
                    m_modelProblems.push_back(problem);
                    log("Scatter " + e.name + ": " + problem);
                }
                continue;
            }
            for (const ScatterItem& item : sc.items) {
                InstanceData inst;
                inst.position = item.position;
                inst.rotation = axisAngle({0, 1, 0}, item.yaw);
                inst.scale = {item.scale, item.scale, item.scale};
                inst.color = 0xFFFFFFFF;
                list.add(m.mesh, inst);
            }
            continue;
        }
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
        if (isSelected(e.id)) inst.flags |= kInstanceHighlight;
        if (e.is(kKindTerrain)) {
            const TerrainView& t = m_assets.terrain(e.id, e.field("heightmap"), w.scale);
            if (!t.error.empty()) reportProblem("Terrain " + e.name + ": " + t.error);
            inst.position = w.position;
            for (MeshId chunk : t.chunks) list.add(chunk, inst);
            continue;
        }
        MeshId mesh = m_assets.box();
        if (!e.model.empty()) {
            const ModelEntry& m = m_assets.model(e.model);
            if (m.ok && m.skinned()) { // the "anim" field: "<clip> [offset seconds]", looping (render::addModel); no clip = rest pose
                const std::string anim = e.field("anim");
                const size_t space = anim.find(' ');
                const AnimationClip* clip = anim.empty() ? nullptr : findClip(m.clips, anim.substr(0, space));
                if (clip) {
                    const f64 offset = space == std::string::npos ? 0.0 : std::atof(anim.c_str() + space + 1);
                    samplePose(m.skeleton, *clip, static_cast<f32>(std::fmod((m_playAnims ? m_time : 0.0) + offset, 4096.0)), true, m_pose);
                } else {
                    restPose(m.skeleton, m_pose);
                }
                skinPalette(m.skeleton, m_pose, m_palette);
                inst.position = w.position;
                inst.rotation = w.rotation;
                inst.scale = w.scale;
                list.addSkinned(m.mesh, inst, m_palette.data(), static_cast<u32>(m_palette.size()));
                continue;
            }
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
    m_lastDt = dt;
    if (m_view2d) m_yaw = m_pitch = 0.0f;
    m_fps = m_fps <= 0.0f ? 60.0f : m_fps * 0.95f + 0.05f / static_cast<f32>(std::max(dt, 1e-4));
    ImGuizmo::BeginFrame();
    syncSelection();
    checkExternalChange();
    if (languageOverride.empty()) followLanguage(); // MealKit Manager or another Mk tool changed the suite language
    autosave();
    if (!m_project.empty() && fileTime(kindsPath()) != m_kindsTime) reloadKinds();
    pollLink();

    const ImGuiID dockspace = ImGui::GetID("MkRenderDock");
    if (!m_layoutDone) {
        m_layoutDone = true;
        const ImGuiWindowSettings* assets = ImGui::FindWindowSettingsByID(ImHashStr("###Assets"));
        const bool assetsDocked = assets && assets->DockId != 0; // layouts saved before the Assets window existed
        if (!ImGui::DockBuilderGetNode(dockspace) || !ImGui::FindWindowSettingsByID(ImHashStr("###Hierarchy")) || !assetsDocked)
            layoutDocks(dockspace);
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
        if (m_shot) m_view = {0.0f, 0.0f, static_cast<f32>(m_renderer->width()), static_cast<f32>(m_renderer->height())};
        m_view.width = std::min(m_view.width, static_cast<f32>(m_renderer->width()) - m_view.x);
        m_view.height = std::min(m_view.height, static_cast<f32>(m_renderer->height()) - m_view.y);
    }
    drawHierarchy();
    drawAssets();
    if (!focusWindow.empty() && m_time > 0.1) {
        ImGui::SetWindowFocus(("###" + focusWindow).c_str());
        focusWindow.clear();
    }
    drawInspector();
    drawEnvironment();
    drawLookDev();
    drawStats();
    drawOverlay();
    drawGizmo();
    drawFieldHandles();
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
    ImGui::DockBuilderDockWindow("###Hierarchy", left);
    ImGui::DockBuilderDockWindow("###Stats", leftBottom);
    ImGui::DockBuilderDockWindow("###Assets", leftBottom);
    ImGui::DockBuilderDockWindow("###Inspector", right);
    ImGui::DockBuilderDockWindow("###Environment", right);
    ImGui::DockBuilderDockWindow("###Look Dev", right);
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
        if (ImGui::BeginMenu(tr("File"))) {
            if (ImGui::MenuItem(tr("New scene"), "Ctrl+N")) guarded(Pending::NewScene);
            if (ImGui::MenuItem(tr("Open project..."))) openProjectDialog();
            if (ImGui::MenuItem(tr("Open scene..."), "Ctrl+O")) openSceneDialog();
            if (ImGui::BeginMenu(tr("Recent scenes"), !m_recent.empty())) {
                for (const std::string& r : m_recent)
                    if (ImGui::MenuItem(r.c_str(), nullptr, r == m_path)) guarded(Pending::OpenPath, r);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(tr("Project scenes"), !m_assets.assetsDir().empty())) {
                for (const std::string& s : m_assets.files(".scene"))
                    if (ImGui::MenuItem(s.c_str(), nullptr, m_assets.resolve(s) == m_path)) guarded(Pending::OpenPath, m_assets.resolve(s));
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Save"), "Ctrl+S")) save();
            if (ImGui::MenuItem(tr("Save as..."))) saveAs();
            if (ImGui::MenuItem(tr("Reload from disk"), nullptr, false, !m_path.empty())) reloadFromDisk();
            {
                std::error_code ec;
                const std::string backup = autosavePath();
                if (ImGui::MenuItem(tr("Recover autosave"), nullptr, false, !backup.empty() && std::filesystem::is_regular_file(pathOf(backup), ec))) {
                    Scene scene;
                    std::string error;
                    if (loadScene(backup, scene, error, nullptr)) {
                        snapshot();
                        m_scene = std::move(scene);
                        log("Recovered " + backup + " (not saved yet)");
                    } else {
                        log("Cannot read " + backup + ": " + error);
                    }
                }
                ImGui::SetItemTooltip(tr("Replace the scene with its last autosave (build/editor/autosave); undo goes back"));
            }
            if (ImGui::MenuItem(tr("Reload models"))) {
                m_assets.reloadAll();
                m_modelProblems.clear();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Save screenshot (3D only)"))) {
                if (char* dir = SDL_GetPrefPath("MealKit", "MkRender")) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "screenshot_%llu.png", static_cast<unsigned long long>(SDL_GetTicks()));
                    saveScreenshot(std::string(dir) + name, false);
                    SDL_free(dir);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Quit"))) requestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Edit"))) {
            if (ImGui::MenuItem(tr("Undo"), "Ctrl+Z", false, !m_undo.empty())) undo(false);
            if (ImGui::MenuItem(tr("Redo"), "Ctrl+Y", false, !m_redo.empty())) undo(true);
            ImGui::Separator();
            if (ImGui::BeginMenu(tr("Add"))) {
                drawAddMenu();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(tr("Duplicate"), "Ctrl+D", false, m_selected != 0)) duplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, m_selected != 0)) deleteSelected();
            if (ImGui::MenuItem(tr("Select all"), "Ctrl+A")) {
                m_selection.clear();
                for (const Entity& e : m_scene.entities)
                    if (!m_hidden.count(e.id) && !m_locked.count(e.id)) m_selection.push_back(e.id);
                m_selected = m_selection.empty() ? 0 : m_selection.back();
            }
            ImGui::Separator();
            if (ImGui::BeginMenu(tr("Arrange"), m_selection.size() >= 2)) {
                static const char* axes[] = {"X", "Y", "Z"};
                for (int a = 0; a < 3; ++a) {
                    if (ImGui::MenuItem(trf("Align min %s", axes[a]).c_str())) alignSelection(a, 0);
                    if (ImGui::MenuItem(trf("Align center %s", axes[a]).c_str())) alignSelection(a, 1);
                    if (ImGui::MenuItem(trf("Align max %s", axes[a]).c_str())) alignSelection(a, 2);
                }
                ImGui::Separator();
                for (int a = 0; a < 3; ++a)
                    if (ImGui::MenuItem(trf("Distribute %s", axes[a]).c_str(), nullptr, false, m_selection.size() >= 3)) distributeSelection(a);
                ImGui::TextDisabled(tr("Aligns to the last selected object"));
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(tr("Drop to floor"), "End", false, m_selected != 0)) dropToFloor();
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Hide"), "H", false, m_selected != 0)) setHidden(m_selection, true);
            if (ImGui::MenuItem(tr("Show all"), "Shift+H", false, !m_hidden.empty())) {
                m_hidden.clear();
                saveViewState();
            }
            if (ImGui::MenuItem(tr("Lock"), nullptr, false, m_selected != 0)) setLocked(m_selection, true);
            if (ImGui::MenuItem(tr("Unlock all"), nullptr, false, !m_locked.empty())) {
                m_locked.clear();
                saveViewState();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Save selection as prefab..."), nullptr, false, m_selected != 0 && !m_assets.assetsDir().empty())) m_prefabPopup = true;
            ImGui::EndMenu();
        }
        drawLinkMenu();
        if (ImGui::BeginMenu(tr("View"))) {
            if (ImGui::MenuItem(tr("Frame selection"), "F", false, m_selected != 0)) frameSelected();
            if (ImGui::MenuItem(tr("Move"), "W", m_gizmo == Gizmo::Translate)) m_gizmo = Gizmo::Translate;
            if (ImGui::MenuItem(tr("Rotate"), "E", m_gizmo == Gizmo::Rotate)) m_gizmo = Gizmo::Rotate;
            if (ImGui::MenuItem(tr("Scale"), "R", m_gizmo == Gizmo::Scale)) m_gizmo = Gizmo::Scale;
            ImGui::MenuItem(tr("Local axes"), nullptr, &m_gizmoLocal);
            if (ImGui::MenuItem(tr("Markers and areas"), nullptr, &m_showMarkers)) savePrefs();
            if (ImGui::MenuItem(tr("Grid"), "G", &m_showGrid)) savePrefs();
            if (ImGui::BeginMenu(tr("Grid and snap"))) {
                bool changed = ImGui::MenuItem(tr("Always snap (Ctrl turns it off)"), nullptr, &m_alwaysSnap);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                changed |= ImGui::DragFloat(tr("Grid step (m)"), &m_gridStep, 0.05f, 0.05f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                changed |= ImGui::DragFloat(tr("Move step (m)"), &m_snapMove, 0.05f, 0.01f, 100.0f, "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                changed |= ImGui::DragFloat(tr("Rotate step (deg)"), &m_snapRotate, 0.5f, 1.0f, 90.0f, "%.1f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                changed |= ImGui::DragFloat(tr("Scale step"), &m_snapScale, 0.01f, 0.01f, 10.0f, "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                changed |= ImGui::DragFloat(tr("Autosave (min, 0 off)"), &m_autosaveMinutes, 0.1f, 0.0f, 60.0f, "%.1f");
                if (changed) savePrefs();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(tr("Camera bookmarks"))) {
                for (int i = 0; i < 9; ++i) {
                    char label[32];
                    std::snprintf(label, sizeof(label), "%s", trf("Go to %d", i + 1).c_str());
                    char key[8];
                    std::snprintf(key, sizeof(key), "%d", i + 1);
                    if (ImGui::MenuItem(label, key, false, m_bookmarks[i].set)) {
                        m_yaw = m_bookmarks[i].yaw;
                        m_pitch = -m_bookmarks[i].pitch;
                        m_pivot = m_bookmarks[i].position + cameraForward() * m_distance;
                    }
                }
                ImGui::TextDisabled("Ctrl+1..9 saves the current view (shared with the editor)");
                ImGui::EndMenu();
            }
            int model = static_cast<int>(m_lighting);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::Combo(tr("Lighting model"), &model, "PBR\0Toon\0Unlit\0")) setLightingModel(static_cast<LightingModel>(model));
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Bake reflection probes"), nullptr, false, !bakingProbes() && !m_project.empty())) bakeProbes();
            if (ImGui::MenuItem(tr("Grading capture"), nullptr, false, !m_project.empty())) requestGradingCapture();
            bool compress = m_assets.compressTextures();
            if (ImGui::MenuItem(tr("Compress model textures (BC7/BC5)"), nullptr, &compress)) {
                m_assets.setCompressTextures(compress);
                m_assets.reloadAll();
                m_modelProblems.clear();
                savePrefs();
            }
            ImGui::SetItemTooltip(tr("Uses the engine's texture cache (build/cache/textures) and fills it when missing. Off: plain RGBA8"));
            ImGui::Separator();
            ImGui::MenuItem(tr("Play animations"), nullptr, &m_playAnims);
            ImGui::SetItemTooltip(tr("Skinned models loop the clip of their anim field (off: the clip's first frame plus its offset)"));
            if (ImGui::MenuItem(tr("Reset layout"))) layoutDocks(ImGui::GetID("MkRenderDock"));
            ImGui::EndMenu();
        }
        if (m_externalChange) {
            ImGui::TextColored({1.0f, 0.75f, 0.3f, 1.0f}, tr("   The file changed on disk."));
            if (ImGui::SmallButton(tr("Reload (drop my edits)"))) reloadFromDisk();
            if (ImGui::SmallButton(tr("Keep mine"))) {
                m_externalChange = false;
                m_fileTime = fileTime(m_path);
            }
        }
        ImGui::TextDisabled("   %s  |  %s", mkrhi::backendName(), m_renderer->adapterName());
        drawLanguageButton();
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
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            m_selection.clear();
            for (const Entity& e : m_scene.entities)
                if (!m_hidden.count(e.id) && !m_locked.count(e.id)) m_selection.push_back(e.id);
            m_selected = m_selection.empty() ? 0 : m_selection.back();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End)) dropToFloor();
        for (int i = 0; i < 9 && !io.KeyAlt && !io.KeyShift && !ImGui::IsMouseDown(ImGuiMouseButton_Right); ++i) {
            if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false)) continue;
            Bookmark& b = m_bookmarks[i];
            if (ctrl) {
                b = {true, cameraEye(), m_yaw, -m_pitch};
                saveViewState();
                log("Camera bookmark " + std::to_string(i + 1) + " saved");
            } else if (b.set) {
                m_yaw = b.yaw;
                m_pitch = std::clamp(-b.pitch, -1.55f, 1.55f);
                m_pivot = b.position + cameraForward() * m_distance;
            }
        }
        if (!ctrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            if (ImGui::IsKeyPressed(ImGuiKey_H)) {
                if (io.KeyShift) {
                    m_hidden.clear();
                    saveViewState();
                } else if (m_selected) {
                    setHidden(m_selection, true);
                    m_selected = 0;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_B)) m_brushOn = !m_brushOn; // terrain/scatter brush of the selected object
            if (ImGui::IsKeyPressed(ImGuiKey_G)) {
                m_showGrid = !m_showGrid;
                savePrefs();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmo = Gizmo::Translate;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmo = Gizmo::Rotate;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmo = Gizmo::Scale;
            if (ImGui::IsKeyPressed(ImGuiKey_F)) frameSelected();
            if (ImGui::IsKeyPressed(ImGuiKey_F5) && m_linkFound) {
                if (m_linkMode == "play") linkStop();
                else linkPlay(io.KeyShift);
            }
        }
    }
}

void App::drawSaveDialog() {
    if (m_pendingAction != Pending::None && !ImGui::IsPopupOpen("###unsaved") && dirty()) ImGui::OpenPopup("###unsaved");
    if (!ImGui::BeginPopupModal((std::string(tr("Unsaved changes")) + "###unsaved").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text(tr("%s has unsaved changes."), m_path.empty() ? tr("The new scene") : utf8Of(pathOf(m_path).filename()).c_str());
    if (ImGui::Button(tr("Save"))) {
        if (save()) runPending();
        else m_pendingAction = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Don't save"))) {
        runPending();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
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
    ImGui::TextDisabled(tr("Game"));
    for (const KindInfo& k : kinds())
        if (k.game && ImGui::MenuItem(k.label.c_str())) addEntity(k.name);
}

// ---------------------------------------------------------------- Hierarchy

void App::drawTreeNode(const Entity& e) {
    bool hasChildren = false;
    for (const Entity& c : m_scene.entities) hasChildren |= c.parent == e.id;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isSelected(e.id)) flags |= ImGuiTreeNodeFlags_Selected;
    // Keep the selection visible: open the ancestors of the selected object
    if (hasChildren && m_selected && m_selected != e.id && isDescendant(m_scene, m_selected, e.id)) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    ImGui::PushID(static_cast<int>(e.id));
    // Room for the kind icon before the name, drawn in the object's color (the editor's scene tree)
    const char* glyph = iconFont ? entityIcon(e) : nullptr;
    const bool open = ImGui::TreeNodeEx("##node", flags, glyph ? "      %s" : "%s", e.name.empty() ? "(unnamed)" : e.name.c_str());
    if (glyph) {
        f32 c[4];
        colorToFloats(e.color, c);
        if (c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f < 0.25f) c[0] = c[1] = c[2] = 0.8f; // dark objects: a readable icon
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText({rowMin.x + ImGui::GetTreeNodeToLabelSpacing(), rowMin.y}, ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 1.0f}), glyph);
    }
    const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) clickSelect(e.id, ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_selected = e.id;
        frameSelected();
    }
    // "!" after the name: a problem (red) or a warning (orange), the text on hover of the row or the mark
    if (const auto badge = m_badges.find(e.id); badge != m_badges.end()) {
        if (rowHovered) ImGui::SetTooltip("%s", badge->second.text.c_str());
        ImGui::SameLine();
        ImGui::TextColored(badge->second.error ? ImVec4(1.0f, 0.42f, 0.38f, 1.0f) : ImVec4(1.0f, 0.68f, 0.25f, 1.0f), "!");
        ImGui::SetItemTooltip("%s", badge->second.text.c_str());
    }
    ImGui::SameLine();
    const KindInfo* kindInfo = findKind(e.kind);
    ImGui::TextDisabled("%s%s%s", kindInfo && kindInfo->game ? kindInfo->label.c_str() : e.kind.c_str(), m_hidden.count(e.id) ? tr("  (hidden)") : "",
                        m_locked.count(e.id) ? tr("  (locked)") : "");
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
        if (!isSelected(e.id)) clickSelect(e.id, false);
        const bool hidden = m_hidden.count(e.id) != 0, locked = m_locked.count(e.id) != 0;
        if (ImGui::MenuItem(hidden ? tr("Show") : tr("Hide"), "H")) setHidden(m_selection, !hidden);
        if (ImGui::MenuItem(locked ? tr("Unlock") : tr("Lock (not picked in the view)"))) setLocked(m_selection, !locked);
        ImGui::Separator();
        if (ImGui::MenuItem(tr("Duplicate"), "Ctrl+D")) duplicateSelected();
        if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
        if (e.parent && ImGui::MenuItem(tr("Move to top level"))) reparent(e.id, 0);
        if (ImGui::MenuItem(tr("Frame"), "F")) frameSelected();
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

bool App::iconButton(const char* glyph, const char* name, const char* detail) {
    const std::string label = std::string(iconFont ? glyph : name) + "##" + name;
    // Flat like the editor's icon buttons: clear until hovered
    if (iconFont) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    }
    const bool pressed = iconFont ? ImGui::Button(label.c_str(), {ImGui::GetFrameHeight(), 0}) : ImGui::Button(label.c_str());
    if (iconFont) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (detail) ImGui::SetTooltip("%s\n%s", name, detail);
        else if (iconFont) ImGui::SetTooltip("%s", name);
    }
    return pressed;
}

void App::drawLanguageButton() {
    // The name in English too while another language is on, so someone who can't read it still knows the button
    const std::string name = std::strcmp(language(), "en") == 0 ? std::string("Language") : std::string(tr("Language")) + " (Language)";
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()); // the right end, inside the padding
    if (iconButton(kIconLanguage, name.c_str())) ImGui::OpenPopup("##language");
    if (ImGui::BeginPopup("##language")) {
        for (const Language& l : kLanguages) {
            if (ImGui::MenuItem(l.name, nullptr, std::strcmp(language(), l.code) == 0)) {
                languageOverride.clear();
                if (!saveLanguage(l.code)) {
                    setLanguage(l.code);
                    log("Cannot save the language to " + mkcontract::userPath(mkcontract::kEditorSettings));
                }
            }
        }
        ImGui::EndPopup();
    }
}

std::string App::panelTitle(const char* glyph, const char* name) const {
    return (iconFont ? std::string(glyph) + "  " : std::string()) + tr(name) + "###" + name;
}

void App::drawHierarchy() {
    if (ImGui::Begin(panelTitle(kIconTree, "Hierarchy").c_str())) {
        updateBadges();
        if (iconButton(kIconAdd, tr("Add object"))) ImGui::OpenPopup("add");
        if (ImGui::BeginPopup("add")) {
            drawAddMenu();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", tr("Search"), m_filter, sizeof(m_filter));
        ImGui::Separator();
        if (ImGui::BeginChild("tree")) {
            const std::string filter = lower(m_filter);
            if (!filter.empty()) {
                for (const Entity& e : m_scene.entities) {
                    if (lower(e.name).find(filter) == std::string::npos && lower(e.kind).find(filter) == std::string::npos) continue;
                    ImGui::PushID(static_cast<int>(e.id));
                    if (ImGui::Selectable(e.name.c_str(), isSelected(e.id))) clickSelect(e.id, ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl);
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
        if (kf) ImGui::SetItemTooltip("%s\n%s (%s)%s", kf->label.c_str(), key.c_str(), kf->type.c_str(), kf->view ? tr(", view only") : "");
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
            if (checkbox("##v", &on)) value = on ? "true" : "false";
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
            if (checkbox("##v", &on)) value = on ? "true" : "false";
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
        if (iconButton(kIconRemove, tr("Remove this field line"))) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) e.fields.erase(e.fields.begin() + remove);
    if (const KindInfo* k = findKind(e.kind)) {
        size_t missing = 0;
        for (const KindField& f : k->fields) missing += e.field(f.path).empty() && std::none_of(e.fields.begin(), e.fields.end(), [&](const auto& l) { return l.first == f.path; });
        if (missing && ImGui::Button(trf("Add %d missing fields with defaults", static_cast<int>(missing)).c_str()))
            for (const KindField& f : k->fields)
                if (std::none_of(e.fields.begin(), e.fields.end(), [&](const auto& l) { return l.first == f.path; })) e.fields.push_back({f.path, f.value});
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tr("Add field")).x - ImGui::GetStyle().FramePadding.x * 4);
    ImGui::InputTextWithHint("##newfield", tr("field name"), m_newField, sizeof(m_newField));
    ImGui::SameLine();
    if (ImGui::Button(tr("Add field")) && m_newField[0] && !std::strchr(m_newField, ' ')) {
        e.setField(m_newField, "");
        m_newField[0] = '\0';
    }
}

void App::drawInspector() {
    if (ImGui::Begin(panelTitle(kIconInspector, "Inspector").c_str())) {
        Entity* found = m_scene.find(m_selected);
        if (!found) {
            ImGui::TextDisabled(tr("Select an object in the hierarchy or the viewport."));
            ImGui::TextDisabled(tr("%zu objects in the scene"), m_scene.entities.size());
        } else {
            Entity& e = *found;
            if (m_selection.size() > 1) ImGui::TextColored({1.0f, 0.8f, 0.4f, 1.0f}, tr("%zu objects selected (editing the last one; the gizmo moves all)"), m_selection.size());
            ImGui::TextDisabled(tr("id %u"), e.id);
            row(tr("Name"));
            inputString("##name", e.name);
            row(tr("Kind"));
            const KindInfo* current = findKind(e.kind);
            const std::string kindText = current && current->label != e.kind ? current->label + "  (" + e.kind + ")" : e.kind;
            if (ImGui::BeginCombo("##kind", kindText.c_str())) {
                for (const KindInfo& k : kinds())
                    if (ImGui::Selectable(k.label.c_str(), e.kind == k.name) && e.kind != k.name) {
                        e.kind = k.name;
                        for (const auto& [key, value] : defaultFields(k.name))
                            if (e.field(key).empty()) e.setField(key, value);
                    }
                ImGui::Separator();
                ImGui::TextDisabled(tr("Game kind (typed name):"));
                static char custom[64];
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                if (ImGui::InputText("##custom", custom, sizeof(custom), ImGuiInputTextFlags_EnterReturnsTrue) && std::isalpha(static_cast<unsigned char>(custom[0]))) {
                    e.kind = custom;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndCombo();
            }
            if (!findKind(e.kind))
                ImGui::TextDisabled(m_kindsLoaded ? tr("Unknown kind: not in the project's object kinds; fields are kept as text.")
                                                  : tr("Game kind: fields are kept as text (no build/editor/kinds.txt yet: open the project in the editor once, or run <game>_headless --write-kinds)."));
            if (e.parent) {
                const Entity* parent = m_scene.find(e.parent);
                row(tr("Parent"));
                ImGui::TextUnformatted(parent ? parent->name.c_str() : "?");
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Unparent"))) reparent(e.id, 0);
            } else {
                row(tr("Folder"));
                inputString("##folder", e.folder);
            }

            ImGui::SeparatorText(tr("Transform (parent-relative)"));
            row(tr("Position"));
            ImGui::DragFloat3("##pos", &e.position.x, 0.05f, 0.0f, 0.0f, "%.3f");
            if (m_eulerFor != e.id) m_euler = toEuler(e.rotation);
            row(tr("Rotation"));
            if (ImGui::DragFloat3("##rot", &m_euler.x, 0.5f, 0.0f, 0.0f, "%.1f")) e.rotation = fromEuler(m_euler);
            m_eulerFor = ImGui::IsItemActive() ? e.id : 0;
            row(tr("Scale"));
            ImGui::DragFloat3("##scale", &e.scale.x, 0.02f, 0.0f, 0.0f, "%.3f");

            ImGui::SeparatorText(tr("Look"));
            const KindInfo* info = findKind(e.kind);
            const bool modelKind = !info || (!info->marker && !info->area && !e.is(kKindTerrain) && !e.is(kKindScatter) && !e.is(kKindSprite));
            if (modelKind) {
                row(tr("Model"));
                if (ImGui::BeginCombo("##model", e.model.empty() ? tr("(box)") : e.model.c_str(), ImGuiComboFlags_HeightLarge)) {
                    static char search[64];
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputTextWithHint("##search", tr("Search"), search, sizeof(search));
                    if (ImGui::Selectable(tr("(box)"), e.model.empty())) e.model.clear();
                    const std::string s = lower(search);
                    for (const std::string& m : m_assets.modelFiles())
                        if ((s.empty() || lower(m).find(s) != std::string::npos) && ImGui::Selectable(m.c_str(), m == e.model)) e.model = m;
                    ImGui::EndCombo();
                }
                if (!e.model.empty()) {
                    const ModelEntry& m = m_assets.model(e.model);
                    if (m.ok) ImGui::TextDisabled(tr("%u triangles, %u materials, %u textures"), m.triangles, m.materials, m.textures);
                    else ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "%s", m.error.c_str());
                    for (const std::string& w : m.warnings) ImGui::TextDisabled("%s", w.c_str());
                }
            }
            f32 c[4];
            colorToFloats(e.color, c);
            row(tr("Color"));
            if (ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_AlphaPreviewHalf)) e.color = floatsToColor(c);
            bool checker = (e.flags & kInstanceChecker) != 0;
            row(tr("Checker"));
            if (checkbox("##checker", &checker)) e.flags = checker ? e.flags | kInstanceChecker : e.flags & ~kInstanceChecker;
            if (e.is(kKindScatter) || e.is(kKindTerrain)) drawBrushInspector(e);
            if (e.is(kKindSprite)) drawSpriteInspector(e);
            if (!e.model.empty()) {
                const ModelEntry& m = m_assets.model(e.model);
                if (m.ok && m.skinned()) {
                    std::string anim = e.field("anim");
                    const std::string clipName = anim.substr(0, anim.find(' '));
                    row(tr("Clip"));
                    if (ImGui::BeginCombo("##clip", clipName.empty() ? tr("(rest pose)") : clipName.c_str())) {
                        if (ImGui::Selectable(tr("(rest pose)"), clipName.empty())) e.setField("anim", "");
                        for (const AnimationClip& c : m.clips)
                            if (ImGui::Selectable(c.name.c_str(), c.name == clipName))
                                e.setField("anim", c.name + (anim.find(' ') != std::string::npos ? anim.substr(anim.find(' ')) : std::string()));
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip(tr("The anim field (\"<clip> [offset seconds]\"), played in a loop like in the game view"));
                }
            }
            if (e.is(kKindTerrain)) ImGui::TextDisabled(tr("The heightmap fills the object's box (scale)."));
            if (e.is(kKindProbe)) {
                ImGui::BeginDisabled(bakingProbes() || m_project.empty());
                if (ImGui::Button(tr("Bake this probe"))) bakeProbes(e.id);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled(e.field("bake").empty() ? tr("not baked") : tr("box = influence area"));
            }

            ImGui::SeparatorText(tr("Fields"));
            drawFields(e);
        }
    }
    ImGui::End();
}

void App::drawEnvironment() {
    if (ImGui::Begin(panelTitle(kIconBrightness, "Environment").c_str())) {
        Environment& env = m_scene.environment;
        ImGui::SeparatorText(tr("Sun"));
        const Vec3 d = normalize(env.sunDirection);
        f32 azimuth = std::atan2(d.x, d.z) / kDeg, elevation = std::asin(std::clamp(d.y, -1.0f, 1.0f)) / kDeg;
        row(tr("Azimuth"));
        bool sun = ImGui::SliderFloat("##az", &azimuth, -180.0f, 180.0f, tr("%.1f deg"));
        row(tr("Elevation"));
        sun |= ImGui::SliderFloat("##el", &elevation, -10.0f, 90.0f, tr("%.1f deg"));
        if (sun) env.sunDirection = {std::cos(elevation * kDeg) * std::sin(azimuth * kDeg), std::sin(elevation * kDeg), std::cos(elevation * kDeg) * std::cos(azimuth * kDeg)};
        row(tr("Sun color"));
        ImGui::ColorEdit3("##suncolor", &env.sunColor.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        ImGui::SeparatorText(tr("Sky and fog (linear)"));
        row(tr("Sky"));
        ImGui::ColorEdit3("##sky", &env.skyColor.x, ImGuiColorEditFlags_Float);
        row(tr("Ground"));
        ImGui::ColorEdit3("##ground", &env.groundColor.x, ImGuiColorEditFlags_Float);
        row(tr("Fog color"));
        ImGui::ColorEdit3("##fog", &env.fogColor.x, ImGuiColorEditFlags_Float);
        row(tr("Fog density"));
        ImGui::DragFloat("##fogd", &env.fogDensity, 0.0002f, 0.0f, 1.0f, "%.4f");
        // A file picker over the project's files of one kind (assets/textures/**.hdr, assets/lut/**.cube) plus free text
        const auto filePicker = [&](const char* id, std::string& value, const char* extension, const char* folder) {
            bool changed = false;
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            changed |= inputString(id, value);
            ImGui::SameLine();
            if (ImGui::BeginCombo((std::string(id) + "pick").c_str(), nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
                if (ImGui::Selectable(tr("(none)"), value.empty())) value.clear(), changed = true;
                for (const std::string& f : m_assets.files(extension))
                    if (f.rfind(folder, 0) == 0 && ImGui::Selectable(f.c_str(), f == value)) value = f, changed = true;
                ImGui::EndCombo();
            }
            return changed;
        };
        ImGui::SeparatorText(tr("Sky HDRI"));
        row(tr("HDRI file"));
        filePicker("##hdri", env.hdri, ".hdr", "textures/");
        if (!env.hdri.empty()) {
            row(tr("Intensity"));
            ImGui::DragFloat("##hdrii", &env.hdriIntensity, 0.01f, 0.0f, 16.0f);
            row(tr("Rotation"));
            ImGui::DragFloat("##hdrir", &env.hdriRotation, 0.5f, -360.0f, 360.0f, tr("%.1f deg"));
            if (!m_environmentId) ImGui::TextDisabled(tr("Not loaded (see the log): the sky/ground colors are shown."));
            else ImGui::TextDisabled(tr("Baked once into build/cache/environments (the engine reads the same file)."));
        }
        ImGui::SeparatorText(tr("Reflection probes"));
        u32 probes = 0, baked = 0;
        for (const Entity& e : m_scene.entities)
            if (e.is(kKindProbe)) ++probes, baked += e.field("bake").empty() ? 0u : 1u;
        ImGui::Text(tr("%u in the scene, %u baked, %u drawn"), probes, baked, static_cast<u32>(m_probeList.size()));
        ImGui::BeginDisabled(probes == 0 || bakingProbes() || m_project.empty());
        if (ImGui::Button(tr("Bake probes"))) bakeProbes();
        ImGui::EndDisabled();
        if (bakingProbes()) {
            ImGui::SameLine();
            ImGui::TextDisabled(tr("%u left"), static_cast<u32>(m_probeQueue.size()));
        }
        ImGui::SeparatorText(tr("Post-processing"));
        const auto boolRow = [&](const char* key, const char* label) {
            bool on = env.postValue(key) != "false";
            row(label);
            if (checkbox((std::string("##") + key).c_str(), &on)) env.setPost(key, on ? "true" : "false");
        };
        const auto floatRow = [&](const char* key, const char* label, f32 lo, f32 hi) {
            f32 v = 0.0f;
            parseFloat(env.postValue(key), v);
            row(label);
            if (ImGui::SliderFloat((std::string("##") + key).c_str(), &v, lo, hi, "%.3f")) env.setPost(key, floatText(v));
        };
        boolRow("toneMapping", tr("Tone mapping"));
        boolRow("punchy", tr("Punchy look"));
        floatRow("exposure", tr("Exposure"), -4.0f, 4.0f);
        floatRow("bloomIntensity", tr("Bloom"), 0.0f, 1.0f);
        floatRow("bloomThreshold", tr("Bloom threshold"), 0.0f, 8.0f);
        Vec3 tint{1, 1, 1};
        parseVec3(env.postValue("bloomTint"), tint);
        row(tr("Bloom tint"));
        if (ImGui::ColorEdit3("##tint", &tint.x, ImGuiColorEditFlags_Float)) env.setPost("bloomTint", floatsText(&tint.x, 3));
        floatRow("vignette", tr("Vignette"), 0.0f, 1.0f);
        floatRow("vignetteSmoothness", tr("Vignette start"), 0.0f, 1.0f);
        floatRow("grain", tr("Film grain"), 0.0f, 1.0f);
        floatRow("chromaticAberration", tr("Chromatic aberr."), 0.0f, 0.03f);
        std::string lut = env.postValue("lut");
        row(tr("Color LUT"));
        if (filePicker("##lut", lut, ".cube", "lut/")) env.setPost("lut", lut);
        ImGui::BeginDisabled(m_project.empty());
        if (iconButton(kIconCamera, tr("Grading capture"), tr("The view without LUT to art/grading, to grade in an image editor"))) requestGradingCapture();
        ImGui::EndDisabled();
        // One literal: the translation table and its test match whole strings
        ImGui::SetItemTooltip("%s", tr("Saves the view without the LUT to art/grading/<scene>.png with a neutral.cube beside it:\ngrade both in Resolve or Photoshop, export a .cube into assets/lut and pick it here."));
    }
    ImGui::End();
}

void App::drawStats() {
    if (ImGui::Begin(panelTitle(kIconPerformance, "Stats").c_str())) {
        const RenderStats& s = m_renderer->stats();
        ImGui::Text(tr("%.0f fps, %u draw calls, %u instances"), m_fps, s.drawCalls, s.instances);
        if (s.gpuValid) ImGui::Text(tr("GPU %.2f ms (opaque %.2f, shadow %.2f, post %.2f)"), s.gpuFrameMs, s.gpuPassMs[0], s.gpuPassMs[1], s.gpuPassMs[2]);
        bool vsync = m_renderer->vsync();
        if (checkbox(tr("VSync"), &vsync)) m_renderer->setVsync(vsync);
        ImGui::SeparatorText(tr("Log"));
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
    if (m_showGrid) { // lines on y = 0 around the pivot, every grid step, brighter every 10
        const f32 step = std::max(m_gridStep, 0.01f) * (m_distance > 400.0f * m_gridStep ? 10.0f : 1.0f);
        const int half = 40;
        // 3D: the y = 0 floor; 2D: the z = 0 plane the game plays on (second coordinate is y)
        const f32 cx = std::round(m_pivot.x / step) * step, cz = std::round((m_view2d ? m_pivot.y : m_pivot.z) / step) * step;
        const auto at = [&](f32 u, f32 v) { return m_view2d ? Vec3{u, v, 0.0f} : Vec3{u, 0.0f, v}; };
        for (int i = -half; i <= half; ++i) {
            const f32 o = static_cast<f32>(i) * step;
            for (int axis = 0; axis < 2; ++axis) {
                const bool major = std::llround(((axis ? cx : cz) + o) / step) % 10 == 0;
                const ImU32 c = major ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 22);
                const int segments = 8;
                for (int s = 0; s < segments; ++s) {
                    const f32 t0 = -half * step + 2.0f * half * step * s / segments, t1 = -half * step + 2.0f * half * step * (s + 1) / segments;
                    const Vec3 a = axis ? at(cx + o, cz + t0) : at(cx + t0, cz + o);
                    const Vec3 b = axis ? at(cx + o, cz + t1) : at(cx + t1, cz + o);
                    f32 ax, ay, bx, by;
                    if (project(a, ax, ay) && project(b, bx, by)) dl->AddLine({ax, ay}, {bx, by}, c, 1.0f);
                }
            }
        }
    }
    for (const Entity& e : m_scene.entities) {
        if (m_hidden.count(e.id)) continue;
        const Transform w = worldTransform(m_scene, e);
        const bool selected = isSelected(e.id);
        if (e.is(kKindCamera) && (m_showMarkers || selected)) { // frustum: looks along +z of the object
            f32 fov = 0.0f;
            parseFloat(e.field("fov"), fov);
            if (fov <= 0.0f) fov = 50.0f;
            const f32 len = 2.0f, hy = std::tan(fov * kDeg * 0.5f) * len, hx = hy * 16.0f / 9.0f;
            const Vec3 corners[4] = {{-hx, -hy, len}, {hx, -hy, len}, {hx, hy, len}, {-hx, hy, len}};
            ImVec2 sc[4];
            bool ok[4];
            f32 ox, oy;
            const bool origin = project(w.position, ox, oy);
            const ImU32 c = selected ? selectedColor : IM_COL32(150, 220, 150, 170);
            for (int i = 0; i < 4; ++i) ok[i] = project(w.position + rotate(w.rotation, corners[i]), sc[i].x, sc[i].y);
            for (int i = 0; i < 4; ++i) {
                if (ok[i] && origin) dl->AddLine({ox, oy}, sc[i], c, 1.0f);
                if (ok[i] && ok[(i + 1) % 4]) dl->AddLine(sc[i], sc[(i + 1) % 4], c, 1.0f);
            }
        }
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
        if (isMarker(e) || e.is(kKindScatter) || (e.is(kKindSprite) && e.field("image").empty())) {
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
                if (selected && !findKindField(e.kind, "range")) { // range circle on the ground plane (with a kinds file: the range handle)
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
    if (m_boxSelecting) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 lo(std::min(m_clickX, mouse.x), std::min(m_clickY, mouse.y)), hi(std::max(m_clickX, mouse.x), std::max(m_clickY, mouse.y));
        dl->AddRectFilled(lo, hi, IM_COL32(90, 150, 255, 40));
        dl->AddRect(lo, hi, IM_COL32(90, 150, 255, 200));
    }
    dl->PopClipRect();
}

void App::drawGizmo() {
    m_gizmoUsing = false;
    Entity* e = m_scene.find(m_selected);
    if (!e || m_uiW < 2.0f || e->is(kKindScatter) || brushActive()) return;
    ImGuizmo::SetOrthographic(m_view2d);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(m_uiX, m_uiY, m_uiW, m_uiH);
    ImGuizmo::SetGizmoSizeClipSpace(0.13f);
    f32 matrix[16];
    const Transform before = worldTransform(m_scene, *e);
    compose(before, matrix);
    ImGuizmo::OPERATION op = m_gizmo == Gizmo::Translate ? ImGuizmo::TRANSLATE : m_gizmo == Gizmo::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
    if (isMarker(*e) && op == ImGuizmo::SCALE) op = ImGuizmo::TRANSLATE;
    if (m_view2d && op == ImGuizmo::ROTATE) op = ImGuizmo::ROTATE_Z; // 2D: turning on the plane only
    if (m_view2d && op == ImGuizmo::TRANSLATE) op = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
    const ImGuizmo::MODE mode = (m_gizmoLocal || op == ImGuizmo::SCALE) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const f32 step = op == ImGuizmo::TRANSLATE ? m_snapMove : op == ImGuizmo::ROTATE ? m_snapRotate : m_snapScale;
    const f32 snap[3] = {step, step, step};
    const Mat4 view = viewMatrix(), proj = projMatrix();
    const bool snapping = ImGui::GetIO().KeyCtrl != m_alwaysSnap;
    if (ImGuizmo::Manipulate(&view.m[0][0], &proj.m[0][0], op, mode, matrix, nullptr, snapping ? snap : nullptr)) {
        Transform world;
        decompose(matrix, world);
        setWorldTransform(m_scene, *e, world);
        m_eulerFor = 0;
        // The other selected objects follow the primary one
        const Quat dq = normalizeQuat(mul(world.rotation, conjugate(before.rotation)));
        const Vec3 ratio{world.scale.x / (before.scale.x != 0 ? before.scale.x : 1), world.scale.y / (before.scale.y != 0 ? before.scale.y : 1),
                         world.scale.z / (before.scale.z != 0 ? before.scale.z : 1)};
        for (u32 id : selectedRoots()) {
            Entity* other = m_scene.find(id);
            if (!other || id == m_selected || isDescendant(m_scene, id, m_selected) || m_locked.count(id)) continue;
            Transform w = worldTransform(m_scene, *other);
            if (op == ImGuizmo::TRANSLATE) {
                w.position += world.position - before.position;
            } else if (op == ImGuizmo::ROTATE) {
                w.position = world.position + rotate(dq, w.position - before.position);
                w.rotation = normalizeQuat(mul(dq, w.rotation));
            } else {
                Vec3 local = rotate(conjugate(before.rotation), w.position - before.position);
                local = {local.x * ratio.x, local.y * ratio.y, local.z * ratio.z};
                w.position = before.position + rotate(before.rotation, local);
                w.scale = {w.scale.x * ratio.x, w.scale.y * ratio.y, w.scale.z * ratio.z};
            }
            setWorldTransform(m_scene, *other, w);
        }
    }
    m_gizmoUsing = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
}

void App::viewportInput() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= m_uiX && mouse.y >= m_uiY && mouse.x < m_uiX + m_uiW && mouse.y < m_uiY + m_uiH;
    const bool free = inside && !io.WantCaptureMouse;
    const bool brushing = brushInput(free) || m_brushStroke;
    if (free && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_gizmoUsing && !m_handleUsing && !brushing && !(io.KeyAlt && brushActive())) {
        m_clickCandidate = true;
        m_clickX = mouse.x;
        m_clickY = mouse.y;
    }
    if (m_clickCandidate && !io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::hypot(mouse.x - m_clickX, mouse.y - m_clickY) >= 5.0f)
        m_boxSelecting = true;
    if (m_clickCandidate && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_clickCandidate = false;
        const bool toggle = io.KeyShift || io.KeyCtrl;
        if (m_boxSelecting) {
            m_boxSelecting = false;
            const f32 x0 = std::min(m_clickX, mouse.x), x1 = std::max(m_clickX, mouse.x), y0 = std::min(m_clickY, mouse.y), y1 = std::max(m_clickY, mouse.y);
            if (!toggle) clickSelect(0, false);
            for (const Entity& e : m_scene.entities) {
                if (m_hidden.count(e.id) || m_locked.count(e.id)) continue;
                f32 x, y;
                if (project(worldTransform(m_scene, e).position, x, y) && x >= x0 && x <= x1 && y >= y0 && y <= y1 && !isSelected(e.id)) clickSelect(e.id, true);
            }
        } else if (std::hypot(mouse.x - m_clickX, mouse.y - m_clickY) < 5.0f && !io.KeyAlt) {
            clickSelect(pick(mouse.x, mouse.y), toggle);
        }
    }
    // Assets dragged from the Assets window and released over the view
    if (const ImGuiPayload* payload = ImGui::GetDragDropPayload(); payload && payload->IsDataType(kDragAsset) && inside &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        Vec3 at;
        groundPoint(mouse.x, mouse.y, at);
        f32 x, y;
        if (project(at, x, y)) ImGui::GetForegroundDrawList()->AddCircle({x, y}, 8.0f, IM_COL32(90, 200, 255, 230), 0, 2.0f);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            placeAsset(std::string(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize)), at);
    }
    const bool orbit = ImGui::IsMouseDown(ImGuiMouseButton_Right) || (io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left));
    const bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    static bool dragging = false; // started inside the viewport
    if ((orbit || pan) && free && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                                   ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
        dragging = true;
    if (!orbit && !pan) dragging = false;
    if (dragging && orbit && m_view2d) { // 2D: no turning, a right drag pans
        const f32 k = orthoHalfHeight() * 2.0f / std::max(1.0f, m_uiH);
        m_pivot += Vec3{-io.MouseDelta.x * k, io.MouseDelta.y * k, 0.0f};
    } else if (dragging && orbit) {
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
        const Vec3 forward = m_view2d ? Vec3{0, 1, 0} : cameraForward(); // 2D: W/S move up and down on the plane
        const Vec3 right = m_view2d ? Vec3{1, 0, 0} : normalize(cross({0, 1, 0}, forward));
        if (ImGui::IsKeyDown(ImGuiKey_W)) m_pivot += forward * speed;
        if (ImGui::IsKeyDown(ImGuiKey_S)) m_pivot -= forward * speed;
        if (ImGui::IsKeyDown(ImGuiKey_D)) m_pivot += right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_A)) m_pivot -= right * speed;
        if (ImGui::IsKeyDown(ImGuiKey_E)) m_pivot.y += speed;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) m_pivot.y -= speed;
    }
}

// ---------------------------------------------------------------- Sprites

namespace {
int fieldInt(const Entity& e, const char* key, int fallback) {
    f32 v = static_cast<f32>(fallback);
    parseFloat(e.field(key), v);
    return static_cast<int>(v);
}
struct PivotPreset {
    const char* name;
    f32 x, y;
};
// 3 x 3 like the image: top row first (pivotY 1 = top, 0 = bottom: the quad's y runs from -pivotY to 1 - pivotY)
constexpr PivotPreset kPivots[9] = {{trn("Top left"), 0.0f, 1.0f},    {trn("Top"), 0.5f, 1.0f},    {trn("Top right"), 1.0f, 1.0f},
                                    {trn("Left"), 0.0f, 0.5f},        {trn("Center"), 0.5f, 0.5f}, {trn("Right"), 1.0f, 0.5f},
                                    {trn("Bottom left"), 0.0f, 0.0f}, {trn("Bottom"), 0.5f, 0.0f}, {trn("Bottom right"), 1.0f, 0.0f}};
} // namespace

void App::drawSpriteInspector(Entity& e) {
    ImGui::SeparatorText(tr("Sprite"));
    const std::string image = e.field("image");
    row(tr("Image"));
    if (ImGui::BeginCombo("##image", image.empty() ? tr("(none)") : image.c_str(), ImGuiComboFlags_HeightLarge)) {
        static char search[64];
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", tr("Search"), search, sizeof(search));
        const std::string s = lower(search);
        const f32 side = ImGui::GetTextLineHeight() * 1.6f;
        for (const std::string& f : m_assets.files(".png")) {
            if (!s.empty() && lower(f).find(s) == std::string::npos) continue;
            ImGui::PushID(f.c_str());
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##pick", f == image, 0, {0.0f, side})) e.setField("image", f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (const u64 picture = m_showThumbs ? thumbnail(f) : 0)
                draw->AddImage(ImTextureRef(static_cast<ImTextureID>(picture)), at, {at.x + side, at.y + side});
            draw->AddText({at.x + side + ImGui::GetStyle().ItemSpacing.x, at.y + (side - ImGui::GetTextLineHeight()) * 0.5f},
                          ImGui::GetColorU32(ImGuiCol_Text), f.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (image.empty()) {
        ImGui::TextDisabled(tr("Pick a PNG here, or drag one from Assets into the view."));
        return;
    }
    const SpriteEntry& whole = m_assets.sprite(image, 1, 1, 0, 0.5f, 0.5f, false, false);
    if (!whole.error.empty()) {
        ImGui::TextColored({1.0f, 0.5f, 0.4f, 1.0f}, "%s", whole.error.c_str());
        return;
    }
    int columns = std::max(1, fieldInt(e, "columns", 1)), rows = std::max(1, fieldInt(e, "rows", 1));
    int cell = std::clamp(fieldInt(e, "cell", 0), 0, columns * rows - 1);
    row(tr("Columns, rows"));
    int grid[2] = {columns, rows};
    if (ImGui::DragInt2("##grid", grid, 0.05f, 1, 256)) {
        columns = std::max(1, grid[0]);
        rows = std::max(1, grid[1]);
        e.setField("columns", std::to_string(columns));
        e.setField("rows", std::to_string(rows));
        cell = std::min(cell, columns * rows - 1);
        e.setField("cell", std::to_string(cell));
    }
    ImGui::TextDisabled(tr("%u x %u pixels, one cell %u x %u"), whole.cellWidth, whole.cellHeight, whole.cellWidth / columns, whole.cellHeight / rows);

    // Slicer: the whole image with the cell grid; click a cell to show it
    m_sheetWanted = image;
    const f32 side = std::min(ImGui::GetContentRegionAvail().x, static_cast<f32>(kSheetSize) * 1.5f);
    const f32 aspect = whole.cellHeight ? static_cast<f32>(whole.cellWidth) / static_cast<f32>(whole.cellHeight) : 1.0f;
    const f32 w = aspect >= 1.0f ? side : side * aspect, h = aspect >= 1.0f ? side / aspect : side;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sheet", {std::max(side, 1.0f), std::max(h, 1.0f)});
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 lo{at.x + (side - w) * 0.5f, at.y}, hi{lo.x + w, lo.y + h};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(ImGuiCol_FrameBg));
    if (m_sheetTarget && m_sheetDrawn == image) {
        // The picture fits the image into a square (prepareSheet): the image part of it
        const f32 du = aspect >= 1.0f ? 0.0f : (1.0f - aspect) * 0.5f, dv = aspect >= 1.0f ? (1.0f - 1.0f / aspect) * 0.5f : 0.0f;
        if (const mkrhi::Texture texture = m_renderer->renderTargetTexture(m_sheetTarget))
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(mkrhi::imguiTextureId(m_renderer->device(), texture))), lo, hi, {du, dv}, {1.0f - du, 1.0f - dv});
    }
    const f32 cw = w / static_cast<f32>(columns), ch = h / static_cast<f32>(rows);
    const ImU32 lineColor = IM_COL32(255, 255, 255, 70);
    if (cw >= 3.0f)
        for (int c = 1; c < columns; ++c) dl->AddLine({lo.x + cw * c, lo.y}, {lo.x + cw * c, hi.y}, lineColor);
    if (ch >= 3.0f)
        for (int r = 1; r < rows; ++r) dl->AddLine({lo.x, lo.y + ch * r}, {hi.x, lo.y + ch * r}, lineColor);
    const auto cellRect = [&](int index, ImVec2& a, ImVec2& b) {
        a = {lo.x + cw * static_cast<f32>(index % columns), lo.y + ch * static_cast<f32>(index / columns)};
        b = {a.x + cw, a.y + ch};
    };
    ImVec2 a, b;
    cellRect(cell, a, b);
    dl->AddRect(a, b, IM_COL32(255, 200, 60, 255), 0.0f, 0, 2.0f);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (hovered && mouse.x >= lo.x && mouse.x < hi.x && mouse.y >= lo.y && mouse.y < hi.y) {
        const int c = std::clamp(static_cast<int>((mouse.x - lo.x) / cw), 0, columns - 1), r = std::clamp(static_cast<int>((mouse.y - lo.y) / ch), 0, rows - 1);
        const int index = r * columns + c;
        cellRect(index, a, b);
        dl->AddRect(a, b, IM_COL32(90, 200, 255, 230), 0.0f, 0, 1.5f);
        ImGui::SetTooltip(tr("Cell %d (column %d, row %d)"), index, c + 1, r + 1);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) e.setField("cell", std::to_string(index));
    }

    // Pivot: 3 x 3 buttons placed like the point they set on the image
    f32 px = 0.5f, py = 0.5f;
    parseFloat(e.field("pivotX"), px);
    parseFloat(e.field("pivotY"), py);
    row(tr("Pivot"));
    const f32 button = ImGui::GetFrameHeight() * 0.8f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 9; ++i) {
        const PivotPreset& p = kPivots[i];
        ImGui::SetCursorScreenPos({start.x + (button + 2.0f) * static_cast<f32>(i % 3), start.y + (button + 2.0f) * static_cast<f32>(i / 3)});
        ImGui::PushID(i);
        const bool active = std::fabs(px - p.x) < 1e-3f && std::fabs(py - p.y) < 1e-3f;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        if (ImGui::Button("##pivot", {button, button})) {
            e.setField("pivotX", floatText(p.x));
            e.setField("pivotY", floatText(p.y));
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tr(p.name));
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos({start.x, start.y + (button + 2.0f) * 3.0f});
    ImGui::Dummy({0.0f, 0.0f});
}

void App::prepareSheet() {
    m_sheetPending = false;
    const std::string wanted = std::move(m_sheetWanted);
    m_sheetWanted.clear();
    if (wanted.empty() || wanted == m_sheetDrawn) return;
    const SpriteEntry& s = m_assets.sprite(wanted, 1, 1, 0, 0.5f, 0.5f, false, false);
    m_sheetList.clear();
    if (s.mesh != kNoMesh) { // the 1 m quad scaled to the image's shape, fitted into the square
        const f32 aspect = s.cellHeight ? static_cast<f32>(s.cellWidth) / static_cast<f32>(s.cellHeight) : 1.0f;
        InstanceData inst;
        inst.scale = aspect >= 1.0f ? Vec3{1.0f, 1.0f / aspect, 1.0f} : Vec3{aspect, 1.0f, 1.0f};
        m_sheetList.add(s.mesh, inst);
    }
    FrameParams& frame = m_sheetFrame;
    frame = FrameParams{};
    frame.viewport = {0.0f, 0.0f, static_cast<f32>(kSheetSize), static_cast<f32>(kSheetSize)};
    const Vec3 eye{0.0f, 0.0f, -2.0f};
    frame.viewProj = lookAtLH(eye, {0, 0, 0}, {0, 1, 0}) * orthographicReversedZ(0.5f, 1.0f, 0.01f, 10.0f);
    frame.cameraPos = eye;
    frame.cameraForward = {0, 0, 1};
    frame.skyColor = frame.groundColor = frame.fogColor = {0.12f, 0.12f, 0.13f};
    frame.fogDensity = 0.0f;
    frame.post.toneMapping = false; // the image's own colors
    frame.post.bloomIntensity = 0.0f;
    m_sheetDrawn = wanted;
    m_sheetPending = true;
}

// ---------------------------------------------------------------- Field handles

void App::drawFieldHandles() {
    m_handleUsing = false;
    Entity* e = m_scene.find(m_selected);
    const KindInfo* kind = e ? findKind(e->kind) : nullptr;
    if (!e || !kind || m_uiW < 2.0f || m_hidden.count(e->id) || brushActive()) {
        m_radiusDrag.clear();
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect({m_uiX, m_uiY}, {m_uiX + m_uiW, m_uiY + m_uiH}, true);
    const Vec3 c = worldTransform(m_scene, *e).position;
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= m_uiX && mouse.y >= m_uiY && mouse.x < m_uiX + m_uiW && mouse.y < m_uiY + m_uiH;
    for (const KindField& f : kind->fields) {
        if (f.hint.find("gizmo=radius") == std::string::npos) continue;
        f32 r = 0.0f;
        if (!parseFloat(e->field(f.path), r)) parseFloat(f.value, r);
        r = std::max(r, 0.0f);
        const bool dragging = m_radiusDrag == f.path && m_radiusDragId == e->id;
        const auto ring = [&](bool upright, ImU32 color) { // upright: the x/y plane, else the ground plane
            ImVec2 prev{};
            bool havePrev = false;
            for (int i = 0; i <= 64; ++i) {
                const f32 t = static_cast<f32>(i) / 64.0f * 2.0f * kPi;
                const Vec3 p = upright ? c + Vec3{std::cos(t) * r, std::sin(t) * r, 0.0f} : c + Vec3{std::cos(t) * r, 0.0f, std::sin(t) * r};
                f32 x, y;
                const bool ok = project(p, x, y);
                if (ok && havePrev) dl->AddLine(prev, {x, y}, color, dragging ? 2.0f : 1.5f);
                prev = {x, y};
                havePrev = ok;
            }
        };
        f32 hx, hy;
        if (!project(c + Vec3{r, 0.0f, 0.0f}, hx, hy)) continue;
        const bool hot = dragging || (inside && !io.WantCaptureMouse && !m_gizmoUsing && m_radiusDrag.empty() && std::hypot(mouse.x - hx, mouse.y - hy) < 8.0f);
        const ImU32 color = dragging || hot ? IM_COL32(255, 200, 60, 255) : IM_COL32(120, 220, 255, 210);
        if (!m_view2d) ring(false, color);
        ring(true, m_view2d ? color : (color & 0x00FFFFFFu) | (90u << 24));
        const f32 size = hot ? 6.5f : 5.0f;
        dl->AddCircleFilled({hx, hy}, size, color);
        dl->AddCircle({hx, hy}, size, IM_COL32(20, 20, 24, 255), 0, 1.5f);
        if (hot) {
            m_handleUsing = true;
            const std::string label = (f.label.empty() ? f.path : f.label) + "  " + floatText(std::round(r * 100.0f) / 100.0f);
            dl->AddText({hx + 10.0f, hy - ImGui::GetTextLineHeight() * 0.5f}, IM_COL32(235, 235, 235, 240), label.c_str());
        }
        if (hot && !dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_radiusDrag = f.path;
            m_radiusDragId = e->id;
        }
        if (!dragging) continue;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_radiusDrag.clear();
            continue;
        }
        // The distance from the center on the plane the ring lies on (2D: the x/y plane, 3D: the ground plane through the object)
        Vec3 eye, dir;
        mouseRay(mouse.x, mouse.y, eye, dir);
        f32 next = r;
        if (m_view2d && std::fabs(dir.z) > 1e-4f) {
            const Vec3 p = eye + dir * ((c.z - eye.z) / dir.z);
            next = std::hypot(p.x - c.x, p.y - c.y);
        } else if (!m_view2d && std::fabs(dir.y) > 1e-3f && (c.y - eye.y) / dir.y > 0.0f) {
            const Vec3 p = eye + dir * ((c.y - eye.y) / dir.y);
            next = std::hypot(p.x - c.x, p.z - c.z);
        }
        if (io.KeyCtrl != m_alwaysSnap && m_snapMove > 0.0f) next = std::round(next / m_snapMove) * m_snapMove;
        e->setField(f.path, floatText(std::round(std::max(next, 0.0f) * 1000.0f) / 1000.0f));
    }
    dl->PopClipRect();
}

// ---------------------------------------------------------------- Hierarchy icons and problems

const char* App::entityIcon(const Entity& e) const {
    if (e.is(kKindPointLight)) return kIconLight;
    if (e.is(kKindCamera)) return kIconCamera;
    if (e.is(kKindProbe)) return kIconProbe;
    if (e.is(kKindSprite)) return kIconImage;
    if (isArea(e)) return kIconArea;
    if (isMarker(e)) return kIconPin;
    if (!e.model.empty()) return kIconModel;
    if (e.is(kKindSolid) || e.is(kKindDecoration) || e.is(kKindTerrain) || e.is(kKindScatter)) return kIconBox;
    return kIconFlag;
}

// What MkRender can see without the game: model and image files that don't load, sprites without an image, kinds the project
// doesn't have, probes not baked. Field values (rows, required fields) are checked by the editor, which has the game's types
void App::updateBadges() {
    if (m_badgesAt >= 0.0 && m_time - m_badgesAt < 0.5) return;
    m_badgesAt = m_time;
    m_badges.clear();
    for (const Entity& e : m_scene.entities) {
        const auto add = [&](bool error, std::string text) {
            Badge& b = m_badges[e.id];
            if (b.text.empty() || (error && !b.error)) b = {error, std::move(text)};
        };
        if (!e.model.empty()) {
            const ModelEntry& m = m_assets.model(e.model);
            if (!m.ok) add(true, trf("Model %s: %s", e.model.c_str(), m.error.c_str()));
            else if (!m.warnings.empty()) add(false, e.model + ": " + m.warnings.front());
        }
        if (e.is(kKindSprite)) {
            const std::string image = e.field("image");
            if (image.empty()) {
                add(false, tr("No image"));
            } else {
                const SpriteEntry& s = m_assets.sprite(image, 1, 1, 0, 0.5f, 0.5f, false, false);
                if (!s.error.empty()) add(true, trf("Image %s: %s", image.c_str(), s.error.c_str()));
            }
        }
        if (m_kindsLoaded && !findKind(e.kind)) add(false, trf("Unknown kind %s: not in the project's object kinds", e.kind.c_str()));
        if (e.is(kKindProbe) && e.field("bake").empty()) add(false, tr("Reflection probe not baked"));
    }
}

// ---------------------------------------------------------------- Look Dev

namespace {
constexpr f32 kLookFovY = 35.0f * kDeg;
// The editor's LookdevPanel presets: sun direction and color, sky, ground, background. HDRIs from the project come after them
struct LookPreset {
    const char* name;
    Vec3 sunDirection, sunColor, sky, ground, background;
};
constexpr LookPreset kLookPresets[] = {
    {trn("Neutral studio"), {0.45f, 0.75f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.32f, 0.32f, 0.34f}, {0.16f, 0.16f, 0.16f}, {0.20f, 0.20f, 0.22f}},
    {trn("Daylight"), {0.4f, 0.8f, 0.3f}, {1.0f, 0.95f, 0.85f}, {0.18f, 0.36f, 0.85f}, {0.20f, 0.17f, 0.14f}, {0.55f, 0.70f, 0.95f}},
    {trn("Sunset"), {-0.7f, 0.25f, -0.4f}, {1.0f, 0.55f, 0.30f}, {0.22f, 0.18f, 0.32f}, {0.10f, 0.07f, 0.05f}, {0.32f, 0.22f, 0.28f}},
    {trn("Night"), {0.2f, 0.6f, -0.6f}, {0.35f, 0.45f, 0.75f}, {0.06f, 0.08f, 0.14f}, {0.02f, 0.02f, 0.03f}, {0.03f, 0.04f, 0.07f}},
};
constexpr u32 kLookPresetCount = static_cast<u32>(sizeof(kLookPresets) / sizeof(kLookPresets[0]));
} // namespace

void App::drawLookDev() {
    m_lookVisible = false;
    // First run with a saved layout from before this window: a tab beside the inspector
    if (!ImGui::FindWindowSettingsByID(ImHashStr("###Look Dev")))
        if (ImGuiWindow* inspector = ImGui::FindWindowByID(ImHashStr("###Inspector")); inspector && inspector->DockId)
            ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
    if (ImGui::Begin(panelTitle(kIconLookDev, "Look Dev").c_str())) {
        m_lookVisible = true;
        const bool hdri = m_lookPreset >= kLookPresetCount && m_lookPreset - kLookPresetCount < m_lookHdris.size();
        row(tr("Light"));
        if (ImGui::BeginCombo("##light", hdri ? m_lookHdris[m_lookPreset - kLookPresetCount].c_str() : tr(kLookPresets[std::min(m_lookPreset, kLookPresetCount - 1)].name))) {
            if (ImGui::IsWindowAppearing()) m_lookHdris = m_assets.files(".hdr"); // newly added HDRIs
            for (u32 i = 0; i < kLookPresetCount; ++i)
                if (ImGui::Selectable(tr(kLookPresets[i].name), m_lookPreset == i)) m_lookPreset = i;
            if (!m_lookHdris.empty()) ImGui::Separator();
            for (u32 i = 0; i < m_lookHdris.size(); ++i)
                if (ImGui::Selectable(m_lookHdris[i].c_str(), m_lookPreset == kLookPresetCount + i)) m_lookPreset = kLookPresetCount + i;
            ImGui::EndCombo();
        }
        const char* lightingNames[] = {tr("Scene setting"), "Pbr", "Toon", "Unlit"};
        int lighting = m_lookLighting + 1;
        row(tr("Lighting model"));
        if (ImGui::Combo("##lighting", &lighting, lightingNames, 4)) m_lookLighting = lighting - 1;
        row(tr("Turn"));
        checkbox("##turn", &m_lookSpin);
        row(tr("Zoom"));
        ImGui::SliderFloat("##zoom", &m_lookZoom, 0.5f, 3.0f, "%.2f");
        row(tr("Flash"));
        ImGui::SliderFloat("##flash", &m_lookFlash, 0.0f, 1.0f, "%.2f");
        row(tr("Dissolve"));
        ImGui::SliderFloat("##dissolve", &m_lookDissolve, 0.0f, 1.0f, "%.2f");

        std::string model = m_lookModel;
        if (model.empty())
            if (const Entity* e = m_scene.find(m_selected)) model = e->model;
        if (model.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", tr("Click a model in Assets > Models, or select an object with a model."));
            ImGui::PopStyleColor();
        }
        else if (!m_lookFound) ImGui::TextDisabled("%s", trf("Cannot load %s", model.c_str()).c_str());
        else ImGui::TextDisabled("%s", model.c_str());

        // A square fitting the space left; dragging turns the stand and tilts the camera, the wheel zooms
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 side = std::max(64.0f, std::min(avail.x, avail.y));
        const mkrhi::Texture texture = m_lookTarget ? m_renderer->renderTargetTexture(m_lookTarget) : mkrhi::Texture{};
        if (texture) ImGui::Image(ImTextureRef(static_cast<ImTextureID>(mkrhi::imguiTextureId(m_renderer->device(), texture))), {side, side});
        else ImGui::Dummy({side, side});
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                m_lookYaw -= d.x * 0.01f;
                m_lookPitch = std::clamp(m_lookPitch + d.y * 0.01f, -0.3f, 1.4f);
                m_lookSpin = false;
            }
            const f32 wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) m_lookZoom = std::clamp(m_lookZoom * (wheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.5f, 3.0f);
        }
    }
    ImGui::End();
}

void App::prepareLookDev(f64 dt) {
    m_lookPending = false;
    if (!m_lookVisible || m_shot) return;
    if (m_lookSpin) m_lookYaw += static_cast<f32>(dt) * 0.5f;
    std::string path = m_lookModel;
    if (path.empty())
        if (const Entity* e = m_scene.find(m_selected)) path = e->model;
    const ModelEntry* model = path.empty() ? nullptr : &m_assets.model(path);
    m_lookFound = model && model->ok;
    const bool hdri = m_lookPreset >= kLookPresetCount && m_lookPreset - kLookPresetCount < m_lookHdris.size();
    const LookPreset& light = kLookPresets[hdri ? 0 : std::min(m_lookPreset, kLookPresetCount - 1)];

    // The camera distance fits the model's box, centered on the stand
    Vec3 lo{-0.5f, 0.0f, -0.5f}, hi{0.5f, 1.0f, 0.5f};
    if (m_lookFound) lo = model->boundsMin, hi = model->boundsMax;
    const Vec3 size = hi - lo;
    const f32 radius = std::max(0.5f * length(size), 0.05f);
    const Vec3 center{0.0f, 0.5f * size.y, 0.0f};
    const f32 distance = radius / std::sin(kLookFovY * 0.5f) * 1.3f / m_lookZoom;
    const Vec3 eye = center + Vec3{0.0f, std::sin(m_lookPitch), -std::cos(m_lookPitch)} * distance;

    FrameParams& frame = m_lookFrame;
    frame = FrameParams{};
    frame.viewport = {0.0f, 0.0f, static_cast<f32>(kLookSize), static_cast<f32>(kLookSize)};
    frame.viewProj = lookAtLH(eye, center, {0, 1, 0}) * perspectiveReversedZ(kLookFovY, 1.0f, distance * 0.02f, distance * 4.0f);
    frame.cameraPos = eye;
    frame.cameraForward = normalize(center - eye);
    frame.sunDirection = light.sunDirection;
    frame.sunColor = light.sunColor;
    frame.skyColor = light.sky;
    frame.groundColor = light.ground;
    frame.fogColor = light.background;
    frame.fogDensity = 0.0f;
    if (hdri) frame.environment = m_assets.environment(m_lookHdris[m_lookPreset - kLookPresetCount]).id;
    frame.time = static_cast<f32>(std::fmod(m_time, 4096.0)); // wind sway

    m_lookList.clear();
    const Quat spin = fromEuler({0.0f, m_lookYaw / kDeg, 0.0f});
    const f32 plinth = std::max(size.x, size.z) * 0.6f + radius * 0.15f; // the stand: a flat box under the model's feet, turning with it
    InstanceData stand;
    stand.position = {0.0f, -radius * 0.05f, 0.0f};
    stand.color = rgba(150, 154, 166);
    stand.rotation = spin;
    stand.scale = {plinth * 2.0f, radius * 0.1f, plinth * 2.0f};
    m_lookList.add(m_assets.box(), stand);
    if (m_lookFound) {
        const Vec3 base{-(lo.x + hi.x) * 0.5f, -lo.y, -(lo.z + hi.z) * 0.5f}; // the box's bottom center onto the stand
        InstanceData inst;
        inst.position = rotate(spin, base);
        inst.rotation = spin;
        inst.flags = instanceDissolve(instanceFlash(0, m_lookFlash), m_lookDissolve);
        if (model->skinned()) {
            restPose(model->skeleton, m_pose);
            skinPalette(model->skeleton, m_pose, m_palette);
            m_lookList.addSkinned(model->mesh, inst, m_palette.data(), static_cast<u32>(m_palette.size()));
        } else {
            m_lookList.add(model->mesh, inst);
        }
    }
    m_lookDrawLighting = m_lookLighting < 0 ? m_lighting : static_cast<LightingModel>(std::min<i32>(m_lookLighting, static_cast<i32>(kLightingModelCount) - 1));
    m_lookPending = true;
}

// ---------------------------------------------------------------- Assets window

void App::placeAsset(const std::string& path, Vec3 at) {
    const std::string ext = lower(utf8Of(pathOf(path).extension()));
    if (ext == ".scene") {
        guarded(Pending::OpenPath, m_assets.resolve(path));
        return;
    }
    if (ext == ".prefab") {
        Scene prefab;
        std::string error;
        if (!loadPrefab(m_assets.resolve(path), prefab, error)) {
            log("Cannot place " + path + ": " + error);
            return;
        }
        const std::vector<u32> roots = placePrefab(m_scene, prefab, {at, {}, {1, 1, 1}});
        m_selection = roots;
        m_selected = roots.empty() ? 0 : roots.back();
        log("Placed prefab " + path + " (" + std::to_string(prefab.entities.size()) + " objects)");
        return;
    }
    const std::string name = utf8Of(pathOf(path).stem());
    if (ext == ".png") {
        Entity& e = addEntity(kKindSprite);
        e.name = name;
        e.setField("image", path);
        e.color = 0xFFFFFFFF;
        e.position = at;
        return;
    }
    if (ext == ".glb" || ext == ".gltf") {
        Entity& e = addEntity(kKindDecoration);
        e.name = name;
        e.model = path;
        e.color = 0xFFFFFFFF;
        e.position = at;
        const ModelEntry& m = m_assets.model(path);
        if (m.ok) e.position.y -= m.boundsMin.y; // stand on the point
    }
}

void App::findUses(const std::string& path) {
    m_selection.clear();
    for (const Entity& e : m_scene.entities) {
        bool used = e.model == path;
        for (const auto& [key, value] : e.fields) used |= value == path;
        if (used) m_selection.push_back(e.id);
    }
    m_selected = m_selection.empty() ? 0 : m_selection.back();
    if (m_scene.environment.hdri == path || m_scene.environment.postValue("lut") == path) log(path + ": used by the scene environment");
    log(path + ": " + std::to_string(m_selection.size()) + " objects in this scene use it (selected)");
}

void App::savePrefab(const std::string& name) {
    const std::string text = writePrefab(m_scene, selectedRoots());
    if (text.empty() || m_assets.assetsDir().empty()) return;
    const std::filesystem::path path = pathOf(m_assets.assetsDir()) / "prefabs" / (name + ".prefab");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    log(out ? "Saved prefab " + utf8Of(path) : "Cannot write " + utf8Of(path));
}

void App::drawAssets() {
    if (m_prefabPopup) {
        ImGui::OpenPopup("###saveprefab");
        m_prefabPopup = false;
        if (!m_prefabName[0]) {
            const Entity* e = m_scene.find(m_selected);
            std::snprintf(m_prefabName, sizeof(m_prefabName), "%s", e ? e->name.c_str() : "prefab");
        }
    }
    if (ImGui::BeginPopupModal((std::string(tr("Save prefab")) + "###saveprefab").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(tr("Saves the %zu selected objects (with their children) to assets/prefabs/<name>.prefab"), selectedRoots().size());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText(tr("Name"), m_prefabName, sizeof(m_prefabName), ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = m_prefabName[0] && !std::strpbrk(m_prefabName, "\\/:*?\"<>|");
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(tr("Save")) || (enter && valid)) {
            savePrefab(m_prefabName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::Begin(panelTitle(kIconFolder, "Assets").c_str())) {
        if (m_assets.assetsDir().empty()) {
            ImGui::TextDisabled(tr("Open a project to see its assets."));
        } else {
            if (iconButton(kIconRefresh, tr("Rescan"))) m_assets.rescan();
            ImGui::SameLine();
            if (checkbox(tr("Pictures"), &m_showThumbs)) savePrefs();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##assetfilter", tr("Search assets"), m_assetFilter, sizeof(m_assetFilter));
            const std::string filter = lower(m_assetFilter);
            if (ImGui::BeginTabBar("assetTabs")) {
                const auto list = [&](const char* tab, const char* ext, const char* hint) {
                    if (!ImGui::BeginTabItem(tab)) return;
                    // Scanned when the tab is shown, at most every 2 seconds
                    static std::map<std::string, std::pair<f64, std::vector<std::string>>> cache;
                    auto& [scanned, files] = cache[ext];
                    if (scanned == 0.0 || m_time - scanned > 2.0) {
                        scanned = m_time;
                        files = std::string(ext) == ".glb" ? m_assets.modelFiles() : m_assets.files(ext);
                    }
                    ImGui::TextDisabled("%s", hint);
                    const bool models = std::string(ext) == ".glb";
                    const bool grid = m_showThumbs &&(std::string(ext) == ".glb" || std::string(ext) == ".png");
                    const f32 cell = ImGui::GetFontSize() * 5.0f, textHeight = ImGui::GetTextLineHeight();
                    if (ImGui::BeginChild("files")) {
                        for (const std::string& f : files) {
                            if (!filter.empty() && lower(f).find(filter) == std::string::npos) continue;
                            ImGui::PushID(f.c_str());
                            if (grid) { // picture, then the file name cut to the cell
                                const ImVec2 at = ImGui::GetCursorScreenPos();
                                ImGui::Selectable("##cell", models && f == m_lookModel, ImGuiSelectableFlags_AllowDoubleClick, {cell, cell + textHeight});
                                ImDrawList* draw = ImGui::GetWindowDrawList();
                                if (const u64 picture = thumbnail(f))
                                    draw->AddImage(ImTextureRef(static_cast<ImTextureID>(picture)), at, {at.x + cell, at.y + cell});
                                else
                                    draw->AddRectFilled(at, {at.x + cell, at.y + cell}, ImGui::GetColorU32(ImGuiCol_FrameBg));
                                const std::string name = utf8Of(pathOf(f).filename());
                                draw->PushClipRect({at.x, at.y + cell}, {at.x + cell, at.y + cell + textHeight}, true);
                                draw->AddText({at.x, at.y + cell}, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
                                draw->PopClipRect();
                                ImGui::SetItemTooltip("%s", f.c_str());
                            } else {
                                ImGui::Selectable(f.c_str(), models && f == m_lookModel, ImGuiSelectableFlags_AllowDoubleClick);
                            }
                            if (models && ImGui::IsItemClicked()) m_lookModel = f; // Look Dev shows it
                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) placeAsset(f, m_pivot);
                            if (ImGui::BeginDragDropSource()) {
                                ImGui::SetDragDropPayload(kDragAsset, f.data(), f.size());
                                ImGui::Text(tr("Place %s"), f.c_str());
                                ImGui::EndDragDropSource();
                            }
                            if (ImGui::BeginPopupContextItem("assetctx")) {
                                if (ImGui::MenuItem(tr("Place at the view center"))) placeAsset(f, m_pivot);
                                if (ImGui::MenuItem(tr("Find uses in this scene"))) findUses(f);
                                if (ImGui::MenuItem(tr("Copy path"))) ImGui::SetClipboardText(f.c_str());
                                ImGui::EndPopup();
                            }
                            ImGui::PopID();
                            if (grid && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + cell < ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x)
                                ImGui::SameLine();
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                };
                list(tr("Models"), ".glb", tr("Drag into the view to place, double-click places at the view center"));
                list(tr("Prefabs"), ".prefab", tr("Placed as plain copies (Edit > Save selection as prefab makes one)"));
                list(tr("Images"), ".png", tr("Placed as sprites"));
                list(tr("Scenes"), ".scene", tr("Double-click opens"));
                ImGui::EndTabBar();
            }
        }
    }
    ImGui::End();
}

} // namespace mkrender::tool
