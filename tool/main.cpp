// MkRender executable: SDL3 window, mkrender drawing into it (DX12 or Vulkan, the backend MkRender was built with),
// Dear ImGui panels drawn by mkrhi's ImGui backend. The actual screen is App (scene editor).
//   MkRender [--project <MealKit project folder>] [--file <.scene|.gltf|.glb>] [file] [--select <id>[,<id>...]]
//            [--lighting pbr|toon|unlit] [--debug-layer] [--frames N --capture <png>] [--bake-probes] [--grading-capture] [--editor-play] [--window <name>] [--render-golden <shots.txt> <folder>] [--version]
//   MkRender --check-terrain <file.png|file.scatter>   decodes and encodes with mkterrain: "TERRAIN same" when the bytes come back
//   MkRender --check-scene <file.scene>   reads and writes the scene in memory: "SCENE same" when the text comes back
//            unchanged, "SCENE equal" when only float text differs (old %.9g files), else the first differing line (exit 1)
#include "App.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

mkrender::tool::App* g_app = nullptr;

void logLine(mkrhi::LogLevel level, const char* message, void*) {
    static const char* names[] = {"info", "warn", "error", "fatal"};
    const std::string line = std::string("[") + names[static_cast<int>(level)] + "] " + message;
    if (g_app) g_app->log(line);
    else SDL_Log("%s", line.c_str());
}

// Segoe UI + Malgun Gothic (Korean names in scenes) + the icon font (engine docs/TOOL_DESIGN.md section 2), all at one size.
// Returns whether the icon font loaded: without it icon buttons show their names (App::iconButton)
bool loadFont(float size) {
    ImGuiIO& io = ImGui::GetIO();
    const char* path = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (!SDL_GetPathInfo(path, nullptr) || !io.Fonts->AddFontFromFileTTF(path, size)) {
        ImFontConfig config;
        config.SizePixels = size;
        io.Fonts->AddFontDefaultVector(&config);
    }
    ImFontConfig merge;
    merge.MergeMode = true;
    const char* korean = "C:\\Windows\\Fonts\\malgun.ttf";
    if (SDL_GetPathInfo(korean, nullptr)) io.Fonts->AddFontFromFileTTF(korean, size, &merge);
    // Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on 10: the editor's code points
    for (const char* icons : {"C:\\Windows\\Fonts\\SegoeIcons.ttf", "C:\\Windows\\Fonts\\segmdl2.ttf"}) {
        ImFontConfig config;
        config.MergeMode = true;
        config.GlyphOffset = ImVec2(0.0f, 3.0f);
        if (SDL_GetPathInfo(icons, nullptr) && io.Fonts->AddFontFromFileTTF(icons, size, &config)) return true;
    }
    return false;
}

// The MealKit editor's look (module-debug src/mealkit/debug/ImGuiLayer.cpp applyStyle), copied since MkRender doesn't link the engine
// (engine docs/TOOL_DESIGN.md section 3). Keep it the same as the original.
void applyEditorStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    // Spectrum 2 shape (engine docs/TOOL_DESIGN.md 3.1): 8 px fields, 10 px panels and popups.
    // Panels are separated by space and a darker gutter, not by lines
    s.WindowRounding = 10.0f;
    s.ChildRounding = 10.0f;
    s.PopupRounding = 10.0f;
    s.FrameRounding = 8.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.ScrollbarSize = 10.0f;
    s.WindowBorderSize = 0.0f; // layers are told apart by color, not outlines
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBarBorderSize = 0.0f;
    s.TabBarOverlineSize = 0.0f;
    s.DockingSeparatorSize = 4.0f;
    s.DockingNodeHasCloseButton = false; // each tab has its own close button
    s.TabCloseButtonMinWidthSelected = 0.0f; // tabs show it only while hovered
    s.TabCloseButtonMinWidthUnselected = 0.0f;
    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(7.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.CellPadding = ImVec2(6.0f, 3.0f);
    s.IndentSpacing = 16.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None; // no triangle on every dock tab bar: tabs are all visible and can be dragged
    // Tokens of engine docs/TOOL_DESIGN.md 3.1: Spectrum 2 dark gray-25..gray-900 (gray 1..12) and blue. Pick a step by its job, never a color by eye
    const auto rgb = [](unsigned v, float a = 1.0f) {
        return ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, a);
    };
    const auto gray = [&](int step) {
        static constexpr unsigned kGray[13] = {0, 0x111111, 0x1B1B1B, 0x222222, 0x2C2C2C, 0x323232, 0x393939,
                                          0x444444, 0x6D6D6D, 0x8A8A8A, 0x909090, 0xAFAFAF, 0xEBEBEB};
        return rgb(kGray[step]);
    };
    const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
    const ImVec4 focus = rgb(0x5D89FF);  // keyboard focus, hovered splitters
    const ImVec4 accent = rgb(0x3B63FB); // selection, check marks, active grabs
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = gray(12);
    c[ImGuiCol_TextDisabled] = gray(11); // secondary text: paths, dates, hints
    c[ImGuiCol_TextSelectedBg] = rgb(0x3B63FB, 0.45f);
    c[ImGuiCol_WindowBg] = gray(2);
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = gray(3);
    c[ImGuiCol_Border] = gray(6);
    c[ImGuiCol_BorderShadow] = clear;
    c[ImGuiCol_MenuBarBg] = gray(1);
    c[ImGuiCol_TitleBg] = gray(1);
    c[ImGuiCol_TitleBgActive] = gray(1);
    c[ImGuiCol_TitleBgCollapsed] = gray(1);
    // Hover is one neutral step up, pressed and selected one more; the accent never fills a hovered control
    c[ImGuiCol_FrameBg] = gray(4); // fields and buttons share a step, so a combo and its arrow read as one control
    c[ImGuiCol_FrameBgHovered] = gray(5);
    c[ImGuiCol_FrameBgActive] = gray(6);
    c[ImGuiCol_Header] = gray(5);
    c[ImGuiCol_HeaderHovered] = gray(4);
    c[ImGuiCol_HeaderActive] = gray(6);
    c[ImGuiCol_Button] = gray(4);
    c[ImGuiCol_ButtonHovered] = gray(6);
    c[ImGuiCol_ButtonActive] = gray(7);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = gray(9);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Separator] = gray(5);
    c[ImGuiCol_SeparatorHovered] = focus;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = clear;
    c[ImGuiCol_ResizeGripHovered] = focus;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = gray(6);
    c[ImGuiCol_ScrollbarGrabHovered] = gray(7);
    c[ImGuiCol_ScrollbarGrabActive] = gray(8);
    // The selected tab joins its panel; the others sit on the strip
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = gray(4);
    c[ImGuiCol_TabSelected] = gray(2);
    c[ImGuiCol_TabSelectedOverline] = clear;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = gray(2);
    c[ImGuiCol_TabDimmedSelectedOverline] = clear;
    c[ImGuiCol_TableHeaderBg] = clear; // no header band: quiet header text over a line
    c[ImGuiCol_TableBorderStrong] = gray(6);
    c[ImGuiCol_TableBorderLight] = gray(5);
    c[ImGuiCol_TableRowBgAlt] = clear; // no zebra rows
    c[ImGuiCol_DockingPreview] = rgb(0x3B63FB, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = clear;
    c[ImGuiCol_NavCursor] = focus;
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

// Dark title bar in gray 1 #111111 (editor kEditorCaptionColor, docs/TOOL_DESIGN.md 3.1). Ignored where Windows doesn't support it
void applyEditorCaption(SDL_Window* window) {
#if defined(_WIN32)
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x11, 0x11, 0x11);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
#else
    (void)window;
#endif
}

// --check-terrain <file.png|file.scatter>: decode + encode with mkterrain, "TERRAIN same" when the bytes come back unchanged
// (files the engine or the editor wrote), else "TERRAIN differs" (exit 1)
int checkTerrain(const std::string& path) {
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error, again;
    if (path.size() > 8 && path.compare(path.size() - 8, 8, ".scatter") == 0) {
        std::vector<mkrender::ScatterInstance> items;
        if (!mkrender::readScatter(bytes, items, error)) return std::printf("TERRAIN FAIL %s\n", error.c_str()), 1;
        again = mkrender::scatterText(items);
        std::string unix;
        for (char c : bytes)
            if (c != '\r') unix += c;
        std::printf("TERRAIN %s (%zu instances)\n", again == unix ? "same" : "differs", items.size());
        return again == unix ? 0 : 1;
    }
    mkrender::Heightmap map;
    if (!mkrender::decodeHeightmap(reinterpret_cast<const mkrender::u8*>(bytes.data()), bytes.size(), map, error))
        return std::printf("TERRAIN FAIL %s\n", error.c_str()), 1;
    const std::vector<mkrender::u8> png = mkrender::encodeHeightmap(map);
    const bool same = std::string(png.begin(), png.end()) == bytes;
    mkrender::Heightmap back; // another encoder (a script, an image editor): the samples must still come back
    const bool sameSamples = mkrender::decodeHeightmap(png.data(), png.size(), back, error) && back.samples == map.samples;
    std::printf("TERRAIN %s (%u x %u samples)\n", same ? "same" : sameSamples ? "same samples (other encoder)" : "differs", map.size, map.size);
    return sameSamples ? 0 : 1;
}

int checkScene(const std::string& path) {
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    std::string original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string text;
    for (char c : original)
        if (c != '\r') text += c;
    // Older formats (scene 2..4, extra comment lines) are upgraded when written, like the engine does: only check that
    // what's written reads back to itself
    {
        std::istringstream lines(text);
        std::string l;
        unsigned version = 0;
        int comments = 0;
        while (std::getline(lines, l)) {
            if (l.rfind("@format scene ", 0) == 0) version = static_cast<unsigned>(std::strtoul(l.c_str() + 14, nullptr, 10));
            else if (!l.empty() && l[0] == '#') ++comments;
        }
        if (version != mkrender::tool::kSceneFormatVersion || comments != 1) {
            mkrender::tool::Scene scene, again;
            std::string error;
            if (!mkrender::tool::readScene(text, scene, error, nullptr)) {
                std::printf("SCENE FAIL %s: %s\n", path.c_str(), error.c_str());
                return 1;
            }
            const std::string written = mkrender::tool::writeScene(scene);
            const bool stable = mkrender::tool::readScene(written, again, error, nullptr) && mkrender::tool::writeScene(again) == written;
            std::printf("SCENE %s %s (format %u, %zu objects; written as format %u)\n", stable ? "upgraded" : "FAIL", path.c_str(), version,
                        scene.entities.size(), mkrender::tool::kSceneFormatVersion);
            return stable ? 0 : 1;
        }
    }
    mkrender::tool::Scene scene;
    std::string error;
    std::vector<std::string> warnings;
    if (!mkrender::tool::readScene(text, scene, error, &warnings)) {
        std::printf("SCENE FAIL %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    for (const std::string& w : warnings) std::printf("  warning: %s\n", w.c_str());
    const std::string written = mkrender::tool::writeScene(scene);
    if (written == text) {
        std::printf("SCENE same %s (%zu objects)\n", path.c_str(), scene.entities.size());
        return 0;
    }
    mkrender::tool::Scene again;
    if (mkrender::tool::readScene(written, again, error, nullptr) && mkrender::tool::writeScene(again) == written) {
        // Compare values: every line's words, numbers compared as floats
        std::istringstream a(text), b(written);
        std::string la, lb;
        int lineNo = 0;
        while (true) {
            const bool ga = static_cast<bool>(std::getline(a, la)), gb = static_cast<bool>(std::getline(b, lb));
            ++lineNo;
            if (!ga && !gb) break;
            if (ga != gb) {
                std::printf("SCENE FAIL %s: line counts differ at %d\n", path.c_str(), lineNo);
                return 1;
            }
            if (la == lb) continue;
            std::istringstream wa(la), wb(lb);
            std::string x, y;
            bool same = true;
            while (same) {
                const bool hx = static_cast<bool>(wa >> x), hy = static_cast<bool>(wb >> y);
                if (!hx || !hy) {
                    same = hx == hy;
                    break;
                }
                if (x == y) continue;
                char* ex = nullptr;
                char* ey = nullptr;
                const float fx = std::strtof(x.c_str(), &ex), fy = std::strtof(y.c_str(), &ey);
                same = *ex == '\0' && *ey == '\0' && fx == fy;
            }
            if (!same) {
                std::printf("SCENE FAIL %s:%d\n  file:    %s\n  written: %s\n", path.c_str(), lineNo, la.c_str(), lb.c_str());
                return 1;
            }
        }
        std::printf("SCENE equal %s (%zu objects, float text differs)\n", path.c_str(), scene.entities.size());
        return 0;
    }
    std::printf("SCENE FAIL %s: written text doesn't read back the same\n", path.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string project, file, capture;
    std::string focusWindow; // --window Assets: that window in front
    std::string goldenShots, goldenOut; // --render-golden <shots.txt> <folder>: the engine's graphics regression shots, saved as <name>.png
    bool editorPlay = false; // --editor-play: ask the project's running editor to play the scene once it's found
    bool bakeProbes = false, gradingCapture = false; // --bake-probes, --grading-capture: run once after opening (with --frames for scripts)
    long frames = 0; // --frames N: quit after N frames (--capture saves the last one as PNG, for docs and tests)
    std::string selectIds; // --select 5 or 5,7,9
    bool debugLayer = false;
    int lighting = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        else if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) file = argv[++i];
        else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) selectIds = argv[++i];
        else if (std::strcmp(argv[i], "--debug-layer") == 0) debugLayer = true;
        else if (std::strcmp(argv[i], "--bake-probes") == 0) bakeProbes = true;
        else if (std::strcmp(argv[i], "--grading-capture") == 0) gradingCapture = true;
        else if (std::strcmp(argv[i], "--editor-play") == 0) editorPlay = true;
        else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) focusWindow = argv[++i];
        else if (std::strcmp(argv[i], "--render-golden") == 0 && i + 2 < argc) goldenShots = argv[++i], goldenOut = argv[++i];
        else if (std::strcmp(argv[i], "--check-scene") == 0 && i + 1 < argc) return checkScene(argv[++i]);
        else if (std::strcmp(argv[i], "--check-terrain") == 0 && i + 1 < argc) return checkTerrain(argv[++i]);
        else if (std::strcmp(argv[i], "--lighting") == 0 && i + 1 < argc) {
            const std::string v = argv[++i];
            lighting = v == "toon" ? 1 : v == "unlit" ? 2 : 0;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("MkRender %s (%s)\n", MKRENDER_VERSION, mkrhi::backendName());
            return 0;
        } else if (argv[i][0] != '-') file = argv[i];
    }
    mkrhi::setLog(logLine);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    // Golden shots: 1600 x 900 pixels like the editor's reference images
    const int windowW = goldenShots.empty() ? static_cast<int>(1500 * scale) : 1600;
    const int windowH = goldenShots.empty() ? static_cast<int>(900 * scale) : 900;
    SDL_Window* window = SDL_CreateWindow("MkRender", windowW, windowH,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window) {
        SDL_Log("Cannot create the window: %s", SDL_GetError());
        return 1;
    }
    if (!goldenShots.empty()) { // window units may not be pixels (high pixel density): make the pixel size 1600 x 900
        int px = 0, py = 0;
        SDL_GetWindowSizeInPixels(window, &px, &py);
        if (px > 0 && py > 0 && (px != 1600 || py != 900)) SDL_SetWindowSize(window, 1600 * 1600 / px, 900 * 900 / py);
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);

    std::string prefDir;
    if (char* pref = SDL_GetPrefPath("MealKit", "MkRender")) {
        prefDir = pref;
        SDL_free(pref);
    }
    mkrender::RendererDesc desc;
    desc.window = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    desc.width = static_cast<mkrender::u32>(pw);
    desc.height = static_cast<mkrender::u32>(ph);
    desc.debugLayer = debugLayer;
    desc.pipelineCache = prefDir;
    mkrender::Renderer renderer;
    if (!renderer.init(desc)) {
        const std::string why = std::string("The renderer could not start: ") + renderer.initError();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MkRender", why.c_str(), window);
        return 1;
    }
    applyEditorCaption(window);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    static std::string ini;
    if (!prefDir.empty()) {
        ini = prefDir + "imgui.ini";
        io.IniFilename = ini.c_str();
    }
    applyEditorStyle(scale);
    const bool iconFont = loadFont(16.0f);
    ImGui_ImplSDL3_InitForOther(window);
    mkrhi::imguiInit(renderer.device());

    mkrender::tool::App app;
    g_app = &app;
    app.iconFont = iconFont;
    app.init(window, renderer);
    app.log(std::string("MkRender ") + MKRENDER_VERSION + ", " + mkrhi::backendName() + ", " + renderer.adapterName());
    if (!project.empty() && file.empty()) app.openProject(project);
    else if (!file.empty()) app.openFile(file);
    if (lighting >= 0) app.setLightingModel(static_cast<mkrender::LightingModel>(lighting));
    for (const char* c = selectIds.c_str(); *c;) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(c, &end, 10);
        if (end == c) break;
        if (c == selectIds.c_str()) app.select(static_cast<mkrender::u32>(id));
        else app.addToSelection(static_cast<mkrender::u32>(id));
        c = *end == ',' ? end + 1 : end;
    }
    if (bakeProbes) app.bakeProbes();
    if (gradingCapture) app.requestGradingCapture();
    app.linkPlayWhenReady = editorPlay;
    app.focusWindow = focusWindow;

    // --render-golden: "name scene x y z yaw pitch [pbr|toon|unlit]" lines (MealKit engine/tests/golden/shots.txt), scenes relative to the project's assets
    struct Shot {
        std::string name, scene, lighting;
        mkrender::Vec3 eye;
        float yaw = 0, pitch = 0;
    };
    std::vector<Shot> shots;
    int goldenExit = 0;
    if (!goldenShots.empty()) {
        std::ifstream in(std::filesystem::path(std::u8string(goldenShots.begin(), goldenShots.end())));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '\r') continue;
            std::istringstream ls(line);
            Shot s;
            if (ls >> s.name >> s.scene >> s.eye.x >> s.eye.y >> s.eye.z >> s.yaw >> s.pitch) {
                ls >> s.lighting;
                shots.push_back(s);
            }
        }
        if (shots.empty() || project.empty()) {
            std::printf("GOLDEN no shots in %s (or no --project)\n", goldenShots.c_str());
            return 1;
        }
        std::filesystem::create_directories(std::filesystem::path(std::u8string(goldenOut.begin(), goldenOut.end())));
    }
    constexpr long kGoldenFrames = 6; // models, terrain and HDRIs are uploaded in the first frame
    size_t goldenIndex = 0;
    long goldenFrame = 0;
    const std::string defaultLighting = lighting == 1 ? "toon" : lighting == 2 ? "unlit" : "pbr";

    mkrender::DrawList list;
    Uint64 last = SDL_GetPerformanceCounter();
    std::string shownTitle;
    long frame = 0;
    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) app.requestQuit();
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) app.requestQuit();
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                renderer.resize(static_cast<mkrender::u32>(event.window.data1), static_cast<mkrender::u32>(event.window.data2));
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) app.setPending(event.drop.data);
        }
        if (app.canQuit()) break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        app.openPending();
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(now - last) / static_cast<double>(SDL_GetPerformanceFrequency());
        last = now;

        mkrhi::imguiNewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        std::string goldenPng;
        if (!shots.empty()) { // graphics regression shot: open the scene, fixed camera, no UI
            const Shot& shot = shots[goldenIndex];
            if (goldenFrame == 0) {
                const std::string scene = project + "/assets/" + shot.scene;
                if (!app.openFile(scene)) {
                    std::printf("GOLDEN %s cannot open %s\n", shot.name.c_str(), shot.scene.c_str());
                    goldenExit = 1;
                    break;
                }
                const std::string l = shot.lighting.empty() ? defaultLighting : shot.lighting;
                app.setLightingModel(l == "toon" ? mkrender::LightingModel::Toon : l == "unlit" ? mkrender::LightingModel::Unlit : mkrender::LightingModel::Pbr);
                app.setShot(shot.eye, shot.yaw, shot.pitch);
            }
            if (goldenFrame + 1 == kGoldenFrames) {
                goldenPng = goldenOut + "/" + shot.name + ".png";
                app.saveScreenshot(goldenPng, false);
            }
        }
        app.frame(dt);
        ImGui::Render();
        if (!goldenPng.empty()) {
            const Shot& shot = shots[goldenIndex];
            std::printf("GOLDEN %s %ux%u -> %s\n", shot.name.c_str(), renderer.width(), renderer.height(), goldenPng.c_str());
        }
        if (!shots.empty() && ++goldenFrame == kGoldenFrames) {
            goldenFrame = 0;
            if (++goldenIndex == shots.size()) {
                frames = 1; // this frame is the last one (its capture is saved at endFrame)
                frame = 0;
            }
        }
        const bool lastFrame = frames > 0 && ++frame >= frames;
        if (lastFrame) {
            running = false;
            if (!capture.empty()) app.saveScreenshot(capture, true);
        }

        const std::string title = app.title();
        if (title != shownTitle) {
            SDL_SetWindowTitle(window, title.c_str());
            shownTitle = title;
        }
        list.clear();
        app.fill(list); // before beginFrame: may upload models and terrain
        if (renderer.beginFrame(app.frameParams())) {
            list.submit(renderer);
            app.renderOffscreen();
            renderer.beginOverlay();
            if (app.drawUi()) mkrhi::imguiRender(renderer.device());
            renderer.endFrame();
        }
        app.afterFrame();
    }

    renderer.waitIdle();
    if (!goldenShots.empty()) std::fflush(stdout);
    g_app = nullptr;
    app.shutdown();
    mkrhi::imguiShutdown(renderer.device());
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    renderer.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return goldenExit;
}
