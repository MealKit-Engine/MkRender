#pragma once

#include "Assets.h"
#include "SceneFile.h"

#include "mkrender/DrawList.h"
#include "mkrender/Renderer.h"

#include <mkcontract/Live.h>

#include <cmath>
#include <deque>
#include <map>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
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
    // After the main view's DrawList::submit, before UI: reflection probe faces and the grading capture (offscreen targets)
    void renderOffscreen();
    bool drawUi() const { return !m_hideUiThisFrame; }
    void afterFrame();

    bool openProject(const std::string& dir); // folder with mealkit.project (opens its main scene)
    bool openFile(const std::string& path);   // .scene, or a .gltf/.glb shown in a new unsaved scene
    void setPending(const std::string& path); // from a file dialog thread or a drop
    void openPending();
    void select(u32 id);
    void addToSelection(u32 id) { syncSelection(); if (!isSelected(id)) clickSelect(id, true); }
    void setLightingModel(LightingModel model);
    void setDebugView(DebugView view) { m_debugView = view; } // --view-mode
    std::string title() const;
    void log(const std::string& line);
    void saveScreenshot(const std::string& path, bool withUi);
    void requestQuit();
    bool canQuit() const { return m_quitConfirmed; }
    // Reflection probes (scene reflection_probe objects): six faces each, one per frame, baked with mkbake into
    // assets/probes/<scene>_<id>.mkprobe (the editor's files and names), and the object's "bake" field set. 0 = all
    void bakeProbes(u32 onlyId = 0);
    bool bakingProbes() const { return !m_probeQueue.empty(); }
    // The view without LUT to art/grading/<scene>.png, with art/grading/neutral.cube beside it (grade it, export a .cube to assets/lut)
    void requestGradingCapture() { m_gradeRequested = true; }
    // Editor link (MealKit-Contract "Scene service"): the project's running MealKit editor plays the saved scene in its play view.
    // Play saves first; fromHere plays from this view's camera (solo), with the selected spawn point if one is selected
    void linkPlay(bool fromHere);
    void linkStop();
    void linkLook(u32 cameraId, const std::string& cutscene = {});
    bool linkPlayWhenReady = false;
    std::string focusWindow;
    std::string languageOverride; // --lang: used instead of the suite language, which isn't followed then
    // Graphics regression shots (--render-golden): the whole window from the editor's fly camera (eye, yaw/pitch in degrees, 60 degree fov),
    // no UI, no selection, view time 0 (animation clips at their offset, wind still)
    void setShot(Vec3 eye, f32 yawDegrees, f32 pitchDegrees);
    bool shotMode() const { return m_shot; } // --window <name>: brings that docked window to the front (captures for docs and checks) // --editor-play: play once an editor is found
    bool iconFont = false; // the Segoe icon font merged into the UI font (main.cpp loadFont)
    void* strongFont = nullptr; // ImFont* of Segoe UI Semibold (Body Strong: buttons of empty states), nullptr: the regular font

private:
    // Flat square icon button with its name in the tooltip (engine docs/TOOL_DESIGN.md section 1); the name as the label without the icon font
    bool iconButton(const char* glyph, const char* name, const char* detail = nullptr);
    // Panel tab title "glyph  Name###Name" (engine docs/TOOL_DESIGN.md section 1 "Panel tabs"): the id after ### stays the same with or
    // without the icon font, so the saved dock layout and --window <Name> keep working
    std::string panelTitle(const char* glyph, const char* name) const;
    // Icon + label button of an empty state (TOOL_DESIGN section 1 "occasional screens"); primary = the one accent-filled button
    bool actionButton(const char* glyph, const char* label, bool primary = false);
    // Menu item "glyph  Name" (TOOL_DESIGN section 1: menu items show an icon and the name); the name alone without the icon font
    bool menuItem(const char* glyph, const char* name, const char* shortcut = nullptr, bool selected = false, bool enabled = true);
    bool beginIconMenu(const char* glyph, const char* name, bool enabled = true); // submenu "glyph  Name"
    // An empty state in the middle of the space left in the current window (TOOL_DESIGN section 4): one sentence in Body Large,
    // then the row of buttons drawn by buttons()
    void emptyState(const char* sentence, const std::function<void()>& buttons);
    enum class Pending { None, Quit, OpenPath, NewScene };
    enum class Gizmo { Translate, Rotate, Scale };

    // Files
    bool loadScenePath(const std::string& path);
    bool save();
    void saveAs();
    void newScene();
    void reloadFromDisk();
    void checkExternalChange();
    std::string kindsPath() const;
    void reloadKinds(); // build/editor/kinds.txt of the project (MealKit-Contract "Object kinds"), else the built-in kinds
    bool dirty() const;
    void guarded(Pending action, const std::string& path = {}); // asks to save first when edited
    void runPending();
    std::string projectSetting(const std::string& key) const;

    // Editing
    void snapshot();
    void undo(bool redo);
    Entity& addEntity(const std::string& kind, const Vec3* at = nullptr);
    void duplicateSelected();
    void deleteSelected();
    // Selection: m_selected is the primary object (inspector, gizmo), m_selection all selected ones including it
    bool isSelected(u32 id) const;
    void clickSelect(u32 id, bool toggle); // toggle: Shift adds/removes
    void syncSelection();
    std::vector<u32> selectedRoots() const; // selected objects whose ancestors aren't selected
    bool worldBox(const Entity& e, Vec3& lo, Vec3& hi); // world AABB (a point for markers)
    void alignSelection(int axis, int mode);  // mode 0 min, 1 center, 2 max (to the primary object)
    void distributeSelection(int axis);
    void dropToFloor();
    // Hidden/locked objects and camera bookmarks: build/editor/<scene>.view, shared with the MealKit editor
    std::string viewStatePath() const;
    void loadViewState();
    void saveViewState() const;
    void setHidden(const std::vector<u32>& ids, bool hidden);
    void setLocked(const std::vector<u32>& ids, bool locked);
    // Preferences (grid, snap, autosave, recent scenes) in the user's MkRender folder
    void loadPrefs();
    void savePrefs() const;
    void addRecent(const std::string& path);
    std::string autosavePath() const;
    void autosave();
    // Assets
    void drawAssets();
    void placeAsset(const std::string& path, Vec3 at);
    void findUses(const std::string& path);
    void savePrefab(const std::string& name);
    bool groundPoint(f32 mouseX, f32 mouseY, Vec3& out); // under the mouse: an object's surface or the y = 0 floor
    Vec3 spriteSize(const Entity& e); // multiplier of the object's x/y scale (pixelsPerUnit), 1 1 otherwise
    void reparent(u32 id, u32 parent);
    void frameSelected();

    // Screen
    void layoutDocks(unsigned dockspace);
    void drawMenu();
    // Language icon at the right end of the menu bar: the languages by their own names (engine docs/TOOL_DESIGN.md section 4 "Language")
    void drawLanguageButton();
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
    void drawAddMenu(const Vec3* at = nullptr); // at: where the new object goes (the view centre without it)
    void openSceneDialog();
    void openProjectDialog();
    void drawEmptyView(); // the viewport's empty state: no objects in the scene (first screen: no scene file either)
    void drawViewportMenu(); // right-click menu of the view (a right click without dragging)
    void environmentMenuItems();

    // View
    Vec3 cameraForward() const;
    Vec3 cameraEye() const;
    Mat4 viewMatrix() const;
    Mat4 projMatrix() const;
    bool project(Vec3 world, f32& x, f32& y) const; // ImGui coordinates
    u32 pick(f32 mouseX, f32 mouseY, f32* hitDistance = nullptr); // ImGui coordinates, 0 if nothing
    void mouseRay(f32 mouseX, f32 mouseY, Vec3& eye, Vec3& dir) const;
    bool entityBounds(const Entity& e, Vec3& lo, Vec3& hi); // model-space box drawn for e
    bool isMarker(const Entity& e) const;
    bool isArea(const Entity& e) const;
    f32 rayEntity(const Entity& e, Vec3 eye, Vec3 dir); // distance to e's surface (box, or terrain heights), -1 if missed
    bool raySurface(Vec3 eye, Vec3 dir, u32 skipId, f32& distance); // nearest solid surface (not markers, areas, scatter, sprites)

    // Brushes (a selected terrain or scatter layer with the brush on): terrain raise/lower/flatten/smooth into its 16-bit PNG,
    // scatter paint/erase into its .scatter file; both saved with the scene, a stroke is one undo step
    bool brushActive() const;
    bool brushInput(bool free); // true: the mouse belongs to the brush this frame
    void drawBrushInspector(Entity& e);
    void drawBrushOutline(const Heightmap* map, Vec3 center, Vec3 size, u32 color);

    // Sprites (inspector): image picker, a sheet slicer over a picture of the whole image (click a cell), pivot presets
    void drawSpriteInspector(Entity& e);
    void prepareSheet(); // fill(): the picture of m_sheetWanted when it isn't drawn yet
    mkrhi::RenderTarget m_sheetTarget{};
    std::string m_sheetWanted, m_sheetDrawn; // image the slicer shows this frame / image in m_sheetTarget
    bool m_sheetPending = false;
    DrawList m_sheetList;
    FrameParams m_sheetFrame;

    // 2D projects (mealkit.project view2d, like the MealKit editor): orthographic, looking along +z at the XY plane, no turning,
    // the grid on z = 0 and the rotate gizmo about z only
    bool m_view2d = false;
    f32 orthoHalfHeight() const { return m_distance * std::tan(m_fov * 0.5f * 3.14159265f / 180.0f); }

    // Field handles: each "gizmo=radius" field of the selected object (kinds file hint) as rings around it with a handle to drag
    void drawFieldHandles();
    bool m_handleUsing = false; // the mouse belongs to a handle this frame (no click select)
    std::string m_radiusDrag;   // field path being dragged
    u32 m_radiusDragId = 0;

    // Hierarchy: the kind icon before each name (the editor's scene tree icons) and a "!" badge for problems found in the scene
    const char* entityIcon(const Entity& e) const;
    struct Badge {
        bool error = false; // red; else orange (a warning)
        std::string text;
    };
    std::map<u32, Badge> m_badges;
    f64 m_badgesAt = -1.0;
    void updateBadges(); // twice a second

    // Look Dev: one model under fixed lighting on a turning stand, apart from the scene lighting (the editor's [Look Dev] page).
    // The model is the one clicked in Assets > Models, else the selected object's
    void drawLookDev();
    void prepareLookDev(f64 dt); // fill(): only while the window is visible
    std::string m_lookModel;
    mkrhi::RenderTarget m_lookTarget{};
    DrawList m_lookList;
    FrameParams m_lookFrame;
    LightingModel m_lookDrawLighting = LightingModel::Pbr;
    bool m_lookVisible = false, m_lookPending = false, m_lookFound = false;
    u32 m_lookPreset = 0;    // color preset, beyond them an .hdr from m_lookHdris
    i32 m_lookLighting = -1; // -1: the scene's lighting model
    f32 m_lookYaw = 0.6f, m_lookPitch = 0.25f, m_lookZoom = 1.0f, m_lookFlash = 0.0f, m_lookDissolve = 0.0f;
    bool m_lookSpin = true;
    std::vector<std::string> m_lookHdris;
    f64 m_lastDt = 0.0;

    void* m_window = nullptr;
    Renderer* m_renderer = nullptr;
    Assets m_assets;

    // Project and scene
    std::string m_project; // folder with mealkit.project (may be empty)
    std::string m_path;    // .scene file (empty: unsaved new scene)
    Scene m_scene;
    std::string m_savedText;
    std::filesystem::file_time_type m_fileTime{};
    std::filesystem::file_time_type m_kindsTime{};
    bool m_kindsLoaded = false;
    f64 m_fileCheck = 0.0;
    bool m_externalChange = false;
    std::vector<std::string> m_warnings;
    std::deque<std::string> m_undo, m_redo;
    std::string m_lastSnapshot;

    // Selection and inspector
    u32 m_selected = 0;
    std::vector<u32> m_selection;
    std::set<u32> m_hidden, m_locked;
    struct Bookmark {
        bool set = false;
        Vec3 position; // the editor's fly camera: position, yaw, pitch (pitch up positive)
        f32 yaw = 0.0f, pitch = 0.0f;
    };
    Bookmark m_bookmarks[9];
    bool m_boxSelecting = false;
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
    bool m_showGrid = true;
    bool m_alwaysSnap = false;
    f32 m_gridStep = 1.0f, m_snapMove = 0.5f, m_snapRotate = 15.0f, m_snapScale = 0.1f;
    f32 m_autosaveMinutes = 2.0f; // 0 = off
    f64 m_autosaveAt = 0.0;
    std::vector<std::string> m_recent;
    char m_assetFilter[64] = {};
    char m_prefabName[64] = {};
    bool m_prefabPopup = false;
    bool m_clickCandidate = false;
    bool m_rightCandidate = false; // right button went down in the view: a menu when released without orbiting or flying
    f32 m_rightX = 0, m_rightY = 0;
    Vec3 m_menuPoint{}; // ground point under the right click (Add here)
    f32 m_clickX = 0, m_clickY = 0;

    LightingModel m_lighting = LightingModel::Pbr;
    // View modes (the engine editor's: lit, wireframe, normals, LOD, overdraw, over budget, material features). The scene view only;
    // thumbnails, look dev, probe bakes and the grading capture always draw lit. A pill at the viewport's top left picks one
    DebugView m_debugView = DebugView::None;
    void drawViewMode();
    bool viewModeItems(); // the seven choices, as menu items (View menu and the pill); true when one was picked
    // Model budgets for DebugView::Budget: the engine's asset/Budget.h defaults, overridden by mealkit.project budget<Kind><Item> lines
    struct ModelLimits {
        u32 triangles = 0, textureSize = 0, bones = 0, materials = 0;
    };
    ModelLimits m_budgets[3] = {{15000, 2048, 64, 4}, {3000, 1024, 0, 2}, {20000, 2048, 0, 4}}; // characters, props, environment
    void loadBudgets();
    bool overBudget(const std::string& modelPath, const ModelEntry& model) const;
    std::vector<PointLight> m_lights;
    bool m_playAnims = true;
    bool m_shot = false;
    // Asset thumbnails (Assets window): models and PNGs drawn into small offscreen targets, one per frame, redrawn when the file changes
    struct Thumb {
        mkrhi::RenderTarget target{};
        bool drawn = false;
        long long fileTime = 0;
        f64 checked = -10.0;
    };
    std::map<std::string, Thumb> m_thumbs;
    std::string m_thumbNext; // set by fill(), drawn by renderOffscreen()
    DrawList m_thumbList;
    FrameParams m_thumbFrame;
    LightingModel m_thumbLighting = LightingModel::Pbr;
    bool m_showThumbs = true;
    u64 thumbnail(const std::string& path); // ImGui texture id, 0 while not drawn yet
    void prepareThumbnail();
    std::vector<JointPose> m_pose; // skinned model drawing scratch
    std::vector<BoneMatrix> m_palette;
    // Sky HDRI, LUT and probes of this frame (fill() resolves them, frameParams() passes them)
    u32 m_environmentId = 0, m_lutId = 0;
    std::vector<ReflectionProbe> m_probeList;
    void reportProblem(const std::string& what); // logged once
    std::string sceneStem() const;
    void addScene(DrawList& list); // objects, point lights
    void fillScene(DrawList& list, bool highlight);
    // Probe baking and grading capture
    std::vector<u32> m_probeQueue;
    u32 m_probeFace = 0, m_probeSize = 0;
    mkrhi::RenderTarget m_probeTarget{};
    u32 m_probeTargetSize = 0;
    bool m_probeCapturePending = false;
    Image m_probeCapture;
    std::vector<f32> m_probeFaces;
    DrawList m_bakeList;
    bool m_bakeListReady = false;
    bool m_gradeRequested = false, m_gradeCapturePending = false;
    mkrhi::RenderTarget m_gradeTarget{};
    u32 m_gradeSize[2] = {};
    Image m_gradeCapture;
    void finishProbeFace();
    void finishGradingCapture();
    // Editor link
    void pollLink();
    bool linkPost(const char* path, const std::string& body);
    void drawLinkMenu();
    bool m_linkFound = false;
    mkcontract::live::Instance m_editor;
    std::string m_linkMode, m_linkScene; // editor's state: "edit"/"play", its open scene
    bool m_linkGame = false, m_linkDirty = false;
    f64 m_linkPollAt = 0.0;
    std::set<u32> m_linkDead; // editor pids that didn't answer
    char m_cutsceneName[64] = {};
    std::vector<std::string> m_modelProblems; // model files that failed (logged once)

    bool m_brushOn = false;
    int m_brush = 0; // TerrainBrush
    f32 m_brushRadius = 4.0f, m_brushStrength = 0.5f;
    bool m_scatterErase = false;
    bool m_brushStroke = false, m_brushHit = false;
    Vec3 m_brushPoint;
    u16 m_flattenTo = 0;
    u64 m_scatterSeed = 0x9E3779B97F4A7C15ull;
    struct BrushEdit { // the other state of a stroke (undo/redo swaps it with the current one)
        bool terrain = true;
        std::string path;
        Heightmap map;
        std::vector<ScatterInstance> items;
    };
    std::vector<BrushEdit> m_brushEdits; // undo entries "\x01<index>" point here
    BrushEdit m_strokeBefore;
    void swapBrushEdit(BrushEdit& edit);

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
