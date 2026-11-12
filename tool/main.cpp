// MkRender executable: SDL3 window, mkrender drawing into it (DX12 or Vulkan, the backend MkRender was built with),
// Dear ImGui panels drawn by mkrhi's ImGui backend. The actual screen is App (scene editor).
//   MkRender [--project <MealKit project folder>] [--file <.scene|.gltf|.glb>] [file] [--select <id>]
//            [--lighting pbr|toon|unlit] [--debug-layer] [--frames N --capture <png>] [--version]
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

namespace {

mkrender::tool::App* g_app = nullptr;

void logLine(mkrhi::LogLevel level, const char* message, void*) {
    static const char* names[] = {"info", "warn", "error", "fatal"};
    const std::string line = std::string("[") + names[static_cast<int>(level)] + "] " + message;
    if (g_app) g_app->log(line);
    else SDL_Log("%s", line.c_str());
}

void loadFont(float size) {
    ImGuiIO& io = ImGui::GetIO();
    const char* path = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (SDL_GetPathInfo(path, nullptr) && io.Fonts->AddFontFromFileTTF(path, size)) {
        // Korean names in scenes: Malgun Gothic merged in
        ImFontConfig merge;
        merge.MergeMode = true;
        const char* korean = "C:\\Windows\\Fonts\\malgun.ttf";
        if (SDL_GetPathInfo(korean, nullptr)) io.Fonts->AddFontFromFileTTF(korean, size, &merge);
        return;
    }
    ImFontConfig config;
    config.SizePixels = size;
    io.Fonts->AddFontDefaultVector(&config);
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
    long frames = 0; // --frames N: quit after N frames (--capture saves the last one as PNG, for docs and tests)
    unsigned long selectId = 0;
    bool debugLayer = false;
    int lighting = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        else if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) file = argv[++i];
        else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) selectId = std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--debug-layer") == 0) debugLayer = true;
        else if (std::strcmp(argv[i], "--check-scene") == 0 && i + 1 < argc) return checkScene(argv[++i]);
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
    SDL_Window* window = SDL_CreateWindow("MkRender", static_cast<int>(1500 * scale), static_cast<int>(900 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window) {
        SDL_Log("Cannot create the window: %s", SDL_GetError());
        return 1;
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
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FrameRounding = 3.0f;
    loadFont(16.0f);
    ImGui_ImplSDL3_InitForOther(window);
    mkrhi::imguiInit(renderer.device());

    mkrender::tool::App app;
    g_app = &app;
    app.init(window, renderer);
    app.log(std::string("MkRender ") + MKRENDER_VERSION + ", " + mkrhi::backendName() + ", " + renderer.adapterName());
    if (!project.empty() && file.empty()) app.openProject(project);
    else if (!file.empty()) app.openFile(file);
    if (lighting >= 0) app.setLightingModel(static_cast<mkrender::LightingModel>(lighting));
    if (selectId) app.select(static_cast<mkrender::u32>(selectId));

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
        app.frame(dt);
        ImGui::Render();
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
            renderer.beginOverlay();
            if (app.drawUi()) mkrhi::imguiRender(renderer.device());
            renderer.endFrame();
        }
        app.afterFrame();
    }

    renderer.waitIdle();
    g_app = nullptr;
    app.shutdown();
    mkrhi::imguiShutdown(renderer.device());
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    renderer.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
