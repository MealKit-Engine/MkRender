# Libraries of the MkRender scene editor (tool/), included by the top CMakeLists.txt before mkrhi so mkrhi builds its ImGui backend
# (MKRHI_IMGUI_TARGET). SDL3 static without tests/examples, Dear ImGui (docking) with the SDL3 platform backend and the
# drawing backend of MKRHI_BACKEND.

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

set(imgui_dir ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(imgui_sources ${imgui_dir}/imgui.cpp ${imgui_dir}/imgui_draw.cpp ${imgui_dir}/imgui_tables.cpp ${imgui_dir}/imgui_widgets.cpp
    ${imgui_dir}/backends/imgui_impl_sdl3.cpp)
if(MKRHI_BACKEND STREQUAL "Vulkan")
    list(APPEND imgui_sources ${imgui_dir}/backends/imgui_impl_vulkan.cpp)
else()
    list(APPEND imgui_sources ${imgui_dir}/backends/imgui_impl_dx12.cpp)
endif()
add_library(mkrender_imgui STATIC ${imgui_sources})
target_include_directories(mkrender_imgui PUBLIC ${imgui_dir} ${imgui_dir}/backends)
target_link_libraries(mkrender_imgui PUBLIC SDL3::SDL3-static)
if(MKRHI_BACKEND STREQUAL "Vulkan")
    # volk supplies the Vulkan functions (no loader import library)
    target_include_directories(mkrender_imgui SYSTEM PRIVATE ${MKRHI_VULKAN_INCLUDE_DIRS})
    target_compile_definitions(mkrender_imgui PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
else()
    target_link_libraries(mkrender_imgui PUBLIC d3d12 dxgi)
endif()
if(MSVC)
    target_compile_options(mkrender_imgui PRIVATE /W0 /utf-8)
endif()
set(MKRHI_IMGUI_TARGET mkrender_imgui)
