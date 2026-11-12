# MkRender: MealKit's renderer as engine-free libraries (docs/ENGINE_MODULES.md "Middleware" in the MealKit repo).
#   mkrhi     thin graphics API layer (DX12 or Vulkan), takes a native window handle and size
# No engine headers; the host passes a log callback (mkrhi::setLog) and, optionally, Dear ImGui and Tracy targets.
# Built standalone it includes tests; added by another project with add_subdirectory, only the libraries.
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
# Values the including side can set:
#   MKRHI_BACKEND              DX12 (default) or Vulkan
#   MKRHI_VULKAN_INCLUDE_DIRS  Vulkan headers, volk and VMA folders (required for Vulkan; the loader is loaded at run time by volk)
#   MKRHI_IMGUI_TARGET         Dear ImGui target with the backend for MKRHI_BACKEND: builds mkrhi::imgui* (debug UI drawing).
#                              Only its include folders and definitions are used; the host links ImGui itself
#   MKRHI_TRACY_TARGET         Tracy target (defines TRACY_ENABLE when profiling): GPU frame zones on DX12. Used the same way
#   MKRENDER_OBJECT_LIBRARIES  ON makes OBJECT libraries instead of STATIC (a host DLL that exports every symbol lists their objects)
cmake_minimum_required(VERSION 3.25)
project(mkrender VERSION 0.1.0 LANGUAGES CXX)

set(MKRHI_BACKEND "DX12" CACHE STRING "mkrhi graphics backend (DX12 or Vulkan)")
set_property(CACHE MKRHI_BACKEND PROPERTY STRINGS DX12 Vulkan)
option(MKRENDER_OBJECT_LIBRARIES "Build OBJECT libraries instead of STATIC" OFF)
option(MKRENDER_BUILD_TESTS "Build MkRender tests" ${PROJECT_IS_TOP_LEVEL})
if(MKRENDER_OBJECT_LIBRARIES)
    set(mkrender_lib_type OBJECT)
else()
    set(mkrender_lib_type STATIC)
endif()

# --- mkrhi ---
set(mkrhi_sources
    include/mkrhi/Rhi.h
    src/mkrhi/Log.h
    src/mkrhi/Log.cpp
    src/mkrhi/Files.h
    src/mkrhi/Files.cpp
    src/mkrhi/PipelineCache.h)
if(MKRHI_BACKEND STREQUAL "Vulkan")
    if(NOT MKRHI_VULKAN_INCLUDE_DIRS)
        message(FATAL_ERROR "MKRHI_BACKEND Vulkan needs MKRHI_VULKAN_INCLUDE_DIRS (Vulkan headers, volk, VMA)")
    endif()
    list(APPEND mkrhi_sources src/mkrhi/vulkan/Vk.h src/mkrhi/vulkan/VkDevice.cpp src/mkrhi/vulkan/VkLibs.cpp)
    set(mkrhi_imgui_source src/mkrhi/vulkan/VkImGui.cpp)
elseif(MKRHI_BACKEND STREQUAL "DX12")
    list(APPEND mkrhi_sources src/mkrhi/dx12/Dx12.h src/mkrhi/dx12/Dx12Device.cpp)
    set(mkrhi_imgui_source src/mkrhi/dx12/Dx12ImGui.cpp)
else()
    message(FATAL_ERROR "MKRHI_BACKEND must be DX12 or Vulkan (got ${MKRHI_BACKEND})")
endif()
if(MKRHI_IMGUI_TARGET)
    list(APPEND mkrhi_sources ${mkrhi_imgui_source})
endif()

add_library(mkrhi ${mkrender_lib_type} ${mkrhi_sources})
target_include_directories(mkrhi PUBLIC include PRIVATE src)
target_compile_features(mkrhi PUBLIC cxx_std_20)
target_compile_definitions(mkrhi PUBLIC MKRENDER_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(mkrhi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
if(MKRHI_BACKEND STREQUAL "Vulkan")
    target_include_directories(mkrhi SYSTEM PRIVATE ${MKRHI_VULKAN_INCLUDE_DIRS})
    target_compile_definitions(mkrhi PRIVATE VK_NO_PROTOTYPES $<$<BOOL:${WIN32}>:VK_USE_PLATFORM_WIN32_KHR>)
else()
    target_link_libraries(mkrhi PUBLIC d3d12 dxgi dxguid)
endif()
# Usage requirements only (not linked): an ImGui or Tracy OBJECT library linked here would put its objects in mkrhi a second time
foreach(t ${MKRHI_IMGUI_TARGET} ${MKRHI_TRACY_TARGET})
    target_include_directories(mkrhi SYSTEM PRIVATE $<TARGET_PROPERTY:${t},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(mkrhi PRIVATE $<TARGET_PROPERTY:${t},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()
if(MKRHI_IMGUI_TARGET)
    if(MKRHI_BACKEND STREQUAL "Vulkan")
        target_compile_definitions(mkrhi PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
    endif()
endif()
if(MSVC)
    target_compile_options(mkrhi PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    set_source_files_properties(src/mkrhi/vulkan/VkLibs.cpp PROPERTIES COMPILE_OPTIONS "/W0") # volk + VMA implementation
else()
    target_compile_options(mkrhi PRIVATE -Wall -Wextra)
endif()

if(MKRENDER_BUILD_TESTS)
    enable_testing()
    add_executable(mkrender_tests tests/RenderTests.cpp)
    target_link_libraries(mkrender_tests PRIVATE mkrhi)
    if(MSVC)
        target_compile_options(mkrender_tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(mkrender_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mkrender_tests COMMAND mkrender_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
