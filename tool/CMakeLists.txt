# MkRender (exe MkRender): scene editor for MealKit .scene files. SDL3 window + mkrender + Dear ImGui (docking) + ImGuizmo.
# Build output goes to <build folder>/bin/ with the executable and the manifest mealkit-tool.txt together: put that folder in
# %LOCALAPPDATA%\MealKit\tools\render\, or open the MealKit editor with --tool-path <build folder>/bin and it appears in the [Tools] menu.
# Libraries (SDL3, ImGui) come from Libs.cmake, included before mkrhi.

set(imguizmo ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imguizmo/ImGuizmo.cpp)
add_executable(MkRender WIN32 main.cpp App.h App.cpp Assets.h Assets.cpp Model.h Model.cpp SceneFile.h SceneFile.cpp ${imguizmo})
target_include_directories(MkRender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imguizmo)
target_link_libraries(MkRender PRIVATE mkrender mkbake mkrender_imgui SDL3::SDL3-static)
target_include_directories(MkRender SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cgltf ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)
if(MSVC)
    target_compile_options(MkRender PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(MkRender PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_source_files_properties(${imguizmo} PROPERTIES COMPILE_OPTIONS "/W0")
else()
    target_compile_options(MkRender PRIVATE -Wall -Wextra)
endif()
if(MKRHI_BACKEND STREQUAL "Vulkan")
    target_include_directories(MkRender SYSTEM PRIVATE ${MKRHI_VULKAN_INCLUDE_DIRS})
endif()
set_target_properties(MkRender PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} upper)
    set_target_properties(MkRender PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${upper} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Manifest (version from project(mkrender VERSION ...)). exe is written without an extension, .exe is appended on Windows
set(MKRENDER_VERSION ${PROJECT_VERSION})
configure_file(mealkit-tool.txt ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt @ONLY)
if(WIN32)
    file(READ ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt manifest)
    string(REPLACE "exe MkRender\n" "exe MkRender.exe\n" manifest "${manifest}")
    file(WRITE ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt "${manifest}")
endif()

install(TARGETS MkRender RUNTIME DESTINATION .)
install(FILES ${CMAKE_BINARY_DIR}/bin/mealkit-tool.txt DESTINATION .)
