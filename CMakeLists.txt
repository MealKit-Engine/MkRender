# MkRender: MealKit's renderer as engine-free libraries (docs/ENGINE_MODULES.md "Middleware" in the MealKit repo).
#   mkrhi     thin graphics API layer (DX12 or Vulkan), takes a native window handle and size
#   mkrender  forward renderer on mkrhi with plain inputs, its shaders compiled by DXC at build time (MKRENDER_DXC)
# No engine headers; the host passes a log callback (mkrhi::setLog) and, optionally, Dear ImGui and Tracy targets.
# Built standalone it includes tests and the MkRender scene editor (tool/, Windows); added by another project with add_subdirectory, only the libraries.
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
# Values the including side can set:
#   MKRHI_BACKEND              DX12 (default) or Vulkan
#   MKRHI_VULKAN_INCLUDE_DIRS  Vulkan headers, volk and VMA folders (default: this repo's third_party/; the loader is loaded at run time by volk)
#   MKRHI_IMGUI_TARGET         Dear ImGui target with the backend for MKRHI_BACKEND: builds mkrhi::imgui* (debug UI drawing).
#                              Only its include folders and definitions are used; the host links ImGui itself
#   MKRHI_TRACY_TARGET         Tracy target (defines TRACY_ENABLE when profiling): GPU frame zones on DX12. Used the same way
#   MKRENDER_OBJECT_LIBRARIES  ON makes OBJECT libraries instead of STATIC (a host DLL that exports every symbol lists their objects)
#   MKRENDER_DXC               dxc with SPIR-V support (default: on PATH or MealKit's download cache; without it only mkrhi is built)
#   MKRENDER_USER_CONFIG       header with the host's own math/mesh/image types (include/mkrender/Config.h), and
#   MKRENDER_USER_CONFIG_INCLUDE_DIRS  the folders it needs
cmake_minimum_required(VERSION 3.25)
project(mkrender VERSION 0.9.0 LANGUAGES C CXX)

set(MKRHI_BACKEND "DX12" CACHE STRING "mkrhi graphics backend (DX12 or Vulkan)")
set_property(CACHE MKRHI_BACKEND PROPERTY STRINGS DX12 Vulkan)
option(MKRENDER_OBJECT_LIBRARIES "Build OBJECT libraries instead of STATIC" OFF)
option(MKRENDER_BUILD_TESTS "Build MkRender tests" ${PROJECT_IS_TOP_LEVEL})
if(WIN32 AND PROJECT_IS_TOP_LEVEL)
    set(mkrender_tool_default ON)
else()
    set(mkrender_tool_default OFF)
endif()
option(MKRENDER_BUILD_TOOL "Build the MkRender scene editor (SDL3 + Dear ImGui, needs MKRENDER_DXC)" ${mkrender_tool_default})
if(MKRENDER_OBJECT_LIBRARIES)
    set(mkrender_lib_type OBJECT)
else()
    set(mkrender_lib_type STATIC)
endif()

if(MKRHI_BACKEND STREQUAL "Vulkan" AND NOT MKRHI_VULKAN_INCLUDE_DIRS)
    set(MKRHI_VULKAN_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vma)
endif()

# The scene editor's SDL3 and ImGui targets come first: mkrhi builds its ImGui backend against them (MKRHI_IMGUI_TARGET)
if(MKRENDER_BUILD_TOOL AND NOT MKRHI_IMGUI_TARGET)
    include(tool/Libs.cmake)
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

# --- mkbake ---
# CPU baking without GPU or files (include/mkrender/Bake.h): sky HDRIs, reflection probes, .cube LUTs, texture mips and BC7/BC5.
# MKBAKE_BC (default ON) compiles the BC encoders from MKBAKE_BC7ENC_DIR (bc7enc.cpp, rgbcx.cpp; default third_party/bc7enc);
# a host that ships only already compressed textures can turn it off.
option(MKBAKE_BC "Build BC7/BC5 texture compression into mkbake" ON)
if(NOT MKBAKE_BC7ENC_DIR)
    set(MKBAKE_BC7ENC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bc7enc)
endif()
set(mkbake_sources include/mkrender/Bake.h src/mkbake/Bake.cpp src/mkbake/Texture.cpp)
if(MKBAKE_BC)
    list(APPEND mkbake_sources ${MKBAKE_BC7ENC_DIR}/bc7enc.cpp ${MKBAKE_BC7ENC_DIR}/rgbcx.cpp)
endif()
add_library(mkbake ${mkrender_lib_type} ${mkbake_sources})
target_include_directories(mkbake PUBLIC include)
target_link_libraries(mkbake PUBLIC mkrhi)
target_compile_definitions(mkbake PRIVATE MKBAKE_BC=$<BOOL:${MKBAKE_BC}> $<$<BOOL:${WIN32}>:_CRT_SECURE_NO_WARNINGS>)
if(MKBAKE_BC)
    target_include_directories(mkbake SYSTEM PRIVATE ${MKBAKE_BC7ENC_DIR})
endif()
if(MKRENDER_USER_CONFIG)
    target_compile_definitions(mkbake PUBLIC MKRENDER_USER_CONFIG="${MKRENDER_USER_CONFIG}")
    target_include_directories(mkbake PUBLIC ${MKRENDER_USER_CONFIG_INCLUDE_DIRS})
endif()
if(MSVC)
    target_compile_options(mkbake PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(MKBAKE_BC)
        set_source_files_properties(${MKBAKE_BC7ENC_DIR}/bc7enc.cpp ${MKBAKE_BC7ENC_DIR}/rgbcx.cpp PROPERTIES COMPILE_OPTIONS "/W0")
    endif()
else()
    target_compile_options(mkbake PRIVATE -Wall -Wextra)
endif()

# --- mkterrain ---
# Heightmap terrain and scatter layers as data (include/mkrender/Terrain.h): PNG heightmaps, surface height and ray hits, chunk meshes,
# brushes, scatter text and painting. Its own static stb_image/stb_image_write from MKTERRAIN_STB_DIR (default third_party/stb).
if(NOT MKTERRAIN_STB_DIR)
    set(MKTERRAIN_STB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
endif()
add_library(mkterrain ${mkrender_lib_type} include/mkrender/Terrain.h src/mkterrain/Terrain.cpp)
target_include_directories(mkterrain PUBLIC include)
target_include_directories(mkterrain SYSTEM PRIVATE ${MKTERRAIN_STB_DIR})
target_link_libraries(mkterrain PUBLIC mkrhi)
if(WIN32)
    target_compile_definitions(mkterrain PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(MKRENDER_USER_CONFIG)
    target_compile_definitions(mkterrain PUBLIC MKRENDER_USER_CONFIG="${MKRENDER_USER_CONFIG}")
    target_include_directories(mkterrain PUBLIC ${MKRENDER_USER_CONFIG_INCLUDE_DIRS})
endif()
if(MSVC)
    target_compile_options(mkterrain PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(mkterrain PRIVATE -Wall -Wextra)
endif()

# --- mkrender ---
# Shaders: DXC turns shaders/*.hlsl into byte-array headers at build time, both DXIL and SPIR-V (a failure on either side fails the build).
# This backend's output goes to <build>/shaders/<name>.h (included by the renderer), the other to shaders/other/ (checked only).
# MKRENDER_DXC: a DXC with SPIR-V support (the Windows SDK one has none). Default: dxc on PATH or MealKit's download cache.
if(NOT MKRENDER_DXC)
    find_program(MKRENDER_DXC NAMES dxc HINTS "$ENV{LOCALAPPDATA}/MealKit/tools/dxc-v1.9.2609/bin/x64")
endif()
if(MKRENDER_DXC)
    set(MKRENDER_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${MKRENDER_SHADER_OUT} ${MKRENDER_SHADER_OUT}/other)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(MKRENDER_DXIL_FLAGS -Zi -Qembed_debug -Od)
        set(MKRENDER_SPIRV_FLAGS -Od)
    else()
        set(MKRENDER_DXIL_FLAGS -O3 -Qstrip_debug -Qstrip_reflect)
        set(MKRENDER_SPIRV_FLAGS -O3)
    endif()
    list(APPEND MKRENDER_SPIRV_FLAGS -spirv -fspv-target-env=vulkan1.3)
    # The host's shader hot reload compiles this backend's format with the same options (MKRENDER_HOT_FLAGS, MKRENDER_SHADER_DIR)
    if(MKRHI_BACKEND STREQUAL "Vulkan")
        list(JOIN MKRENDER_SPIRV_FLAGS " " hot_flags)
    else()
        list(JOIN MKRENDER_DXIL_FLAGS " " hot_flags)
    endif()
    if(NOT PROJECT_IS_TOP_LEVEL)
        set(MKRENDER_HOT_FLAGS "${hot_flags}" PARENT_SCOPE)
        set(MKRENDER_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders PARENT_SCOPE)
    endif()
    set(mkrender_shader_headers "")
    # Remaining arguments are preprocessor defines (LIGHTING_MODEL=1 -> -D LIGHTING_MODEL=1)
    function(mkrender_shader source entry profile name)
        set(defines "")
        foreach(d ${ARGN})
            list(APPEND defines -D ${d})
        endforeach()
        # The Vulkan backend builds pipelines with entry point names VSMain/PSMain: other entry points are renamed in SPIR-V
        if(profile MATCHES "^vs_")
            set(spirv_entry -fspv-entrypoint-name=VSMain)
        else()
            set(spirv_entry -fspv-entrypoint-name=PSMain)
        endif()
        if(MKRHI_BACKEND STREQUAL "Vulkan")
            set(dxil ${MKRENDER_SHADER_OUT}/other/${name}.h)
            set(spirv ${MKRENDER_SHADER_OUT}/${name}.h)
        else()
            set(dxil ${MKRENDER_SHADER_OUT}/${name}.h)
            set(spirv ${MKRENDER_SHADER_OUT}/other/${name}.h)
        endif()
        set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source})
        set(deps ${src} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/Bindless.hlsli)
        add_custom_command(OUTPUT ${dxil}
            COMMAND ${MKRENDER_DXC} -nologo -T ${profile} -E ${entry} -Vn ${name} -Fh ${dxil} ${MKRENDER_DXIL_FLAGS} ${defines} ${src}
            DEPENDS ${deps} COMMENT "DXC ${source} (${entry}, DXIL)" VERBATIM)
        add_custom_command(OUTPUT ${spirv}
            COMMAND ${MKRENDER_DXC} -nologo -T ${profile} -E ${entry} -Vn ${name} -Fh ${spirv} ${MKRENDER_SPIRV_FLAGS} ${spirv_entry} ${defines} ${src}
            DEPENDS ${deps} COMMENT "DXC ${source} (${entry}, SPIR-V)" VERBATIM)
        set(mkrender_shader_headers ${mkrender_shader_headers} ${dxil} ${spirv} PARENT_SCOPE)
    endfunction()

    # SM 6.6 (minimum spec). One pixel shader per lighting model (LightingModel) + outline, shadow, sky, particle, sprite, post and UI passes
    mkrender_shader(Forward.hlsl VSMain vs_6_6 g_ForwardVS)
    mkrender_shader(Forward.hlsl PSMain ps_6_6 g_ForwardPbrPS LIGHTING_MODEL=0)
    mkrender_shader(Forward.hlsl PSMain ps_6_6 g_ForwardToonPS LIGHTING_MODEL=1)
    mkrender_shader(Forward.hlsl PSMain ps_6_6 g_ForwardUnlitPS LIGHTING_MODEL=2)
    mkrender_shader(Forward.hlsl VSOutline vs_6_6 g_OutlineVS)
    mkrender_shader(Forward.hlsl PSOutline ps_6_6 g_OutlinePS)
    mkrender_shader(Forward.hlsl VSShadow vs_6_6 g_ShadowVS)
    mkrender_shader(Forward.hlsl VSMain vs_6_6 g_ForwardSkinVS SKINNED=1)
    mkrender_shader(Forward.hlsl VSOutline vs_6_6 g_OutlineSkinVS SKINNED=1)
    mkrender_shader(Forward.hlsl VSShadow vs_6_6 g_ShadowSkinVS SKINNED=1)
    mkrender_shader(Forward.hlsl PSShadow ps_6_6 g_ShadowPS)
    mkrender_shader(Forward.hlsl VSSky vs_6_6 g_SkyVS)
    mkrender_shader(Forward.hlsl PSSky ps_6_6 g_SkyPS)
    mkrender_shader(Particle.hlsl VSMain vs_6_6 g_ParticleVS)
    mkrender_shader(Particle.hlsl PSMain ps_6_6 g_ParticlePS)
    mkrender_shader(Sprite.hlsl VSMain vs_6_6 g_SpriteVS)
    mkrender_shader(Sprite.hlsl PSMain ps_6_6 g_SpritePS)
    mkrender_shader(Post.hlsl VSMain vs_6_6 g_PostVS)
    mkrender_shader(Post.hlsl PSBloomDown ps_6_6 g_BloomDownPS)
    mkrender_shader(Post.hlsl PSBloomUp ps_6_6 g_BloomUpPS)
    mkrender_shader(Post.hlsl PSUber ps_6_6 g_UberPS)
    mkrender_shader(Ui.hlsl VSMain vs_6_6 g_UiVS)
    mkrender_shader(Ui.hlsl PSMain ps_6_6 g_UiPS)

    # MKRENDER_USER_CONFIG / MKRENDER_USER_CONFIG_INCLUDE_DIRS: the host's types (include/mkrender/Config.h)
    add_library(mkrender ${mkrender_lib_type}
        include/mkrender/Config.h
        include/mkrender/Math.h
        include/mkrender/Mesh.h
        include/mkrender/Image.h
        include/mkrender/Culling.h
        include/mkrender/Renderer.h
        include/mkrender/DrawList.h
        src/mkrender/Renderer.cpp
        ${mkrender_shader_headers})
    target_include_directories(mkrender PUBLIC include PRIVATE src ${MKRENDER_SHADER_OUT})
    target_link_libraries(mkrender PUBLIC mkrhi)
    if(MKRENDER_USER_CONFIG)
        target_compile_definitions(mkrender PUBLIC MKRENDER_USER_CONFIG="${MKRENDER_USER_CONFIG}")
        target_include_directories(mkrender PUBLIC ${MKRENDER_USER_CONFIG_INCLUDE_DIRS})
    endif()
    if(WIN32)
        target_compile_definitions(mkrender PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(mkrender PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(mkrender PRIVATE -Wall -Wextra)
    endif()
else()
    message(STATUS "mkrender: no DXC with SPIR-V support found (set MKRENDER_DXC); only mkrhi is built")
endif()

if(MKRENDER_BUILD_TESTS)
    enable_testing()
    add_executable(mkrender_tests tests/RenderTests.cpp)
    target_link_libraries(mkrender_tests PRIVATE mkrhi mkbake mkterrain)
    if(TARGET mkrender)
        target_link_libraries(mkrender_tests PRIVATE mkrender)
        target_compile_definitions(mkrender_tests PRIVATE MKRENDER_TESTS_RENDERER=1)
    endif()
    if(MSVC)
        target_compile_options(mkrender_tests PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(mkrender_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mkrender_tests COMMAND mkrender_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(MKRENDER_BUILD_TOOL AND TARGET mkrender)
    add_subdirectory(tool)
endif()
