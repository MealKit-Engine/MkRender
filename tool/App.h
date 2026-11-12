#pragma once

#include "Model.h"

#include "mkrender/DrawList.h"
#include "mkrender/Renderer.h"

#include <deque>
#include <mutex>
#include <string>
#include <vector>

// MkRender viewer: shows a glTF model or a material sphere grid with mkrender, and edits lighting, post-processing and
// materials live (look development without the engine). Panels are Dear ImGui windows docked around the 3D view.
namespace mkrender::tool {

class App {
public:
    bool init(void* sdlWindow, Renderer& renderer); // after Renderer::init and ImGui backends
    void shutdown();
    void frame(f64 dt); // between ImGui::NewFrame and ImGui::Render: panels and camera input
    // After frame(): this frame's camera/lights/post and what to draw
    FrameParams frameParams() const;
    void fill(DrawList& list) const;
    bool drawUi() const { return !m_hideUiThisFrame; } // false for a 3D-only screenshot frame
    void afterFrame(); // after Renderer::endFrame

    bool openModel(const std::string& path); // .gltf/.glb (command line, drop, File menu)
    void openPending();                      // a path the file dialog passed from its thread
    void setPending(const std::string& path);
    void showSpheres();
    void setLightingModel(LightingModel model);
    std::string title() const;
    void log(const std::string& line);
    void saveScreenshot(const std::string& path, bool withUi);
    bool quit = false;

private:
    struct Material {
        std::string name;
        MaterialData data;
        u32 index = 0; // renderer material index
    };
    struct Object {
        MeshId mesh = kNoMesh;
        Vec3 offset; // model space position before the turntable rotation
    };

    void upload(ModelData& model, const std::string& name);
    void frameCamera();
    void layoutDocks(unsigned dockspace);
    void drawMenu();
    void drawScene();
    void drawLighting();
    void drawPost();
    void drawMaterials();
    void drawStats();
    void cameraInput();

    void* m_window = nullptr;
    Renderer* m_renderer = nullptr;

    // Scene
    std::string m_name; // model file name or "Material spheres"
    std::string m_path;
    std::vector<Object> m_objects;
    std::vector<Material> m_materials;
    std::vector<std::string> m_warnings;
    Vec3 m_boundsMin, m_boundsMax;
    u32 m_vertexCount = 0, m_triangleCount = 0, m_textureCount = 0;
    MeshId m_floor = kNoMesh;
    bool m_showFloor = true;
    bool m_turntable = false;
    f32 m_turntableSpeed = 0.4f; // radians per second
    f32 m_yaw = 0.0f;           // model rotation
    f32 m_dissolve = 0.0f, m_flash = 0.0f;
    i32 m_selected = 0;

    // Camera (orbit)
    Vec3 m_target;
    f32 m_camYaw = 0.6f, m_camPitch = 0.35f, m_distance = 6.0f, m_fov = 45.0f;
    Viewport m_view;

    // Lighting and post (FrameParams values the panels edit)
    FrameParams m_params;
    f32 m_sunAzimuth = 0.9f, m_sunElevation = 0.85f, m_sunIntensity = 1.0f;
    Vec3 m_sunTint{1.0f, 0.95f, 0.85f};
    i32 m_lightCount = 0;
    f32 m_lightRange = 4.0f, m_lightIntensity = 3.0f;
    std::vector<PointLight> m_lights;
    LightingModel m_lighting = LightingModel::Pbr;
    ShadowSettings m_shadows;
    PostOptions m_postOptions;
    u32 m_lightsPerObject = 4;
    f64 m_time = 0.0;

    // Status
    std::deque<std::string> m_log;
    std::string m_status;
    f64 m_statusTime = 0.0;
    f32 m_fps = 0.0f;
    bool m_layoutDone = false;
    bool m_hideUiThisFrame = false;
    std::string m_screenshot; // saved at the next endFrame
    std::string m_pending;
    std::mutex m_pendingMutex;
};

} // namespace mkrender::tool
