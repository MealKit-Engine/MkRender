#pragma once

#include "Assets.h"
#include "SceneFile.h"

#include "mkrender/DrawList.h"
#include "mkrender/Renderer.h"

#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// MkRender: scene editor for a MealKit project's .scene files, like MkUI for .ui and MkAudio for sounds.events.
// Hierarchy (objects, parents, folders), a viewport drawn by mkrender (picking, ImGuizmo transforms, markers), an inspector
// (transform, model, color, fields of any kind as text) and the scene environment (sun, sky, fog, post). Saves the file the
// engine's scene module reads; game kinds' fields are kept as text even though their code isn't here.
namespace mkrender::tool {

class App {
public:
    bool init(void* sdlWindow, Renderer& renderer);
    void shutdown();
    void frame(f64 dt); // between ImGui::NewFrame and ImGui::Render
    FrameParams frameParams() const;
    void fill(DrawList& list);
    bool drawUi() const { return !m_hideUiThisFrame; }
    void afterFrame();

    bool openProject(const std::string& dir); // folder with mealkit.project (opens its main scene)
    bool openFile(const std::string& path);   // .scene, or a .gltf/.glb shown in a new unsaved scene
    void setPending(const std::string& path); // from a file dialog thread or a drop
    void openPending();
    void select(u32 id);
    void setLightingModel(LightingModel model);
    std::string title() const;
    void log(const std::string& line);
    void saveScreenshot(const std::string& path, bool withUi);
    void requestQuit();
    bool canQuit() const { return m_quitConfirmed; }

private:
    enum class Pending { None, Quit, OpenPath, NewScene };
    enum class Gizmo { Translate, Rotate, Scale };

    // Files
    bool loadScenePath(const std::string& path);
    bool save();
    void saveAs();
    void newScene();
    void reloadFromDisk();
    void checkExternalChange();
    bool dirty() const;
    void guarded(Pending action, const std::string& path = {}); // asks to save first when edited
    void runPending();
    std::string projectSetting(const std::string& key) const;

    // Editing
    void snapshot();
    void undo(bool redo);
    Entity& addEntity(const std::string& kind);
    void duplicateSelected();
    void deleteSelected();
    void reparent(u32 id, u32 parent);
    void frameSelected();

    // Screen
    void layoutDocks(unsigned dockspace);
    void drawMenu();
    void drawHierarchy();
    void drawTreeNode(const Entity& e);
    void drawInspector();
    void drawFields(Entity& e);
    void drawEnvironment();
    void drawStats();
    void drawOverlay();
    void drawGizmo();
    void viewportInput();
    void drawSaveDialog();
    void drawAddMenu();

    // View
    Vec3 cameraForward() const;
    Vec3 cameraEye() const;
    Mat4 viewMatrix() const;
    Mat4 projMatrix() const;
    bool project(Vec3 world, f32& x, f32& y) const; // ImGui coordinates
    u32 pick(f32 mouseX, f32 mouseY);               // ImGui coordinates, 0 if nothing
    bool entityBounds(const Entity& e, Vec3& lo, Vec3& hi); // model-space box drawn for e
    bool isMarker(const Entity& e) const;
    bool isArea(const Entity& e) const;

    void* m_window = nullptr;
    Renderer* m_renderer = nullptr;
    Assets m_assets;

    // Project and scene
    std::string m_project; // folder with mealkit.project (may be empty)
    std::string m_path;    // .scene file (empty: unsaved new scene)
    Scene m_scene;
    std::string m_savedText;
    std::filesystem::file_time_type m_fileTime{};
    f64 m_fileCheck = 0.0;
    bool m_externalChange = false;
    std::vector<std::string> m_warnings;
    std::deque<std::string> m_undo, m_redo;
    std::string m_lastSnapshot;

    // Selection and inspector
    u32 m_selected = 0;
    u32 m_eulerFor = 0;
    Vec3 m_euler; // degrees, kept while the rotation is edited (no jumps from quaternion round trips)
    char m_filter[64] = {};
    char m_newField[64] = {};
    u32 m_renaming = 0;
    char m_rename[128] = {};

    // Camera (orbit around a pivot)
    Vec3 m_pivot;
    f32 m_yaw = 0.7f, m_pitch = 0.5f, m_distance = 30.0f, m_fov = 50.0f;
    Viewport m_view;      // framebuffer pixels
    f32 m_uiX = 0, m_uiY = 0, m_uiW = 0, m_uiH = 0; // the same in ImGui coordinates
    Gizmo m_gizmo = Gizmo::Translate;
    bool m_gizmoLocal = false;
    bool m_gizmoUsing = false;
    bool m_showMarkers = true;
    bool m_clickCandidate = false;
    f32 m_clickX = 0, m_clickY = 0;

    LightingModel m_lighting = LightingModel::Pbr;
    std::vector<PointLight> m_lights;
    std::vector<std::string> m_modelProblems; // model files that failed (logged once)

    // Status
    std::deque<std::string> m_log;
    std::string m_status;
    f64 m_statusTime = 0.0, m_time = 0.0;
    f32 m_fps = 0.0f;
    bool m_layoutDone = false;
    bool m_hideUiThisFrame = false;
    Pending m_pendingAction = Pending::None;
    std::string m_pendingActionPath;
    bool m_quitConfirmed = false;
    std::string m_pending;
    std::mutex m_pendingMutex;
};

} // namespace mkrender::tool
