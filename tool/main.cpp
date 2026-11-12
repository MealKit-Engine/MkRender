// MkRender executable: SDL3 window, mkrender drawing into it (DX12 or Vulkan, the backend MkRender was built with),
// Dear ImGui panels drawn by mkrhi's ImGui backend. The actual screen is App.
//   MkRender [model.gltf|.glb] [--lighting pbr|toon|unlit] [--debug-layer] [--frames N --capture <png>] [--version]
#include "App.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    if (SDL_GetPathInfo(path, nullptr) && io.Fonts->AddFontFromFileTTF(path, size)) return;
    ImFontConfig config;
    config.SizePixels = size;
    io.Fonts->AddFontDefaultVector(&config);
}

} // namespace

int main(int argc, char** argv) {
    std::string model, capture;
    long frames = 0; // --frames N: quit after N frames (--capture saves the last one as PNG, for docs and tests)
    bool debugLayer = false;
    int lighting = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        else if (std::strcmp(argv[i], "--debug-layer") == 0) debugLayer = true;
        else if (std::strcmp(argv[i], "--lighting") == 0 && i + 1 < argc) {
            const std::string v = argv[++i];
            lighting = v == "toon" ? 1 : v == "unlit" ? 2 : 0;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("MkRender %s (%s)\n", MKRENDER_VERSION, mkrhi::backendName());
            return 0;
        } else if (argv[i][0] != '-') model = argv[i];
    }
    mkrhi::setLog(logLine);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window* window = SDL_CreateWindow("MkRender", static_cast<int>(1400 * scale), static_cast<int>(860 * scale),
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
    desc.debugViews = true;
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
    if (lighting >= 0) app.setLightingModel(static_cast<mkrender::LightingModel>(lighting));
    if (!model.empty()) app.openModel(model);

    mkrender::DrawList list;
    Uint64 last = SDL_GetPerformanceCounter();
    std::string shownTitle;
    long frame = 0;
    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) running = false;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                renderer.resize(static_cast<mkrender::u32>(event.window.data1), static_cast<mkrender::u32>(event.window.data2));
            if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) app.setPending(event.drop.data);
        }
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
        if (app.quit) running = false;
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
        if (renderer.beginFrame(app.frameParams())) {
            list.clear();
            app.fill(list);
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
