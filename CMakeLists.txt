cmake_minimum_required(VERSION 3.21)

set(VCPKG_TARGET_TRIPLET "x64-windows-static")

set(PLUGIN_NAME "XPMF")
set(PLUGIN_DESCRIPTION "Extended Projected Materials Framework")
set(PLUGIN_VERSION "0.5.0")
project(${PLUGIN_NAME} VERSION ${PLUGIN_VERSION} DESCRIPTION "${PLUGIN_DESCRIPTION}" LANGUAGES CXX)

if(MSVC)
    # Match vcpkg's x64-windows-static triplet (/MT, /MTd) across all targets.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

add_compile_definitions(PLUGIN_NAME="${PLUGIN_NAME}")
add_compile_definitions(PLUGIN_DESCRIPTION="${PLUGIN_DESCRIPTION}")
add_compile_definitions(PLUGIN_VERSION="${PLUGIN_VERSION}")
add_compile_definitions(PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR})
add_compile_definitions(PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR})
add_compile_definitions(PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# CommonLibSSE-NG resolves game addresses and struct layouts at runtime, so one DLL serves
# every supported runtime. SE and AE are both enabled - AE here covers the 1.7.x releases
# (1.7.99 and 1.7.104, which need Address Library format 5 and SKSE 2.3.x) as well as the
# older 1.6.x ones.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
add_subdirectory("extern/CommonLibSSE-NG" commonlibsse)

# DirectXTex reads DDS files: to average a texture for a fallback color, and to re-upload a PBR
# base color in its non-sRGB format
find_package(directxtex CONFIG REQUIRED)

# The configuration is a list of profiles, each holding lists: JSON
find_package(nlohmann_json CONFIG REQUIRED)

set(headers
    "include/ConfigLoader.hpp"
    "include/EditorIdLookup.hpp"
    "include/MaterialClassifier.hpp"
    "include/MaterialMatcher.hpp"
    "include/Offsets.hpp"
    "include/PbrMaterialObjects.hpp"
    "include/PCH.h"
    "include/ProjectedGeometry.hpp"
    "include/ProjectedTextures.hpp"
    "include/ProjectedVertexData.hpp"
    "include/SeasonsOfSkyrim.hpp"
    "include/ShelterMap.hpp"
    "include/Text.hpp"
    "include/TextureColor.hpp"
    "include/VertexLayout.hpp"
)

set(sources
    "src/ConfigLoader.cpp"
    "src/EditorIdLookup.cpp"
    "src/main.cpp"
    "src/MaterialClassifier.cpp"
    "src/MaterialMatcher.cpp"
    "src/PbrMaterialObjects.cpp"
    "src/ProjectedGeometry.cpp"
    "src/ProjectedTextures.cpp"
    "src/ProjectedVertexData.cpp"
    "src/SeasonsOfSkyrim.cpp"
    "src/ShelterMap.cpp"
    "src/TextureColor.cpp"
)

add_library(${PROJECT_NAME} SHARED ${headers} ${sources})

add_compile_definitions(NOMINMAX)
add_compile_definitions(WIN32_LEAN_AND_MEAN)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE "include/PCH.h")

target_include_directories(${PROJECT_NAME} PRIVATE "include")

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE Microsoft::DirectXTex nlohmann_json::nlohmann_json)

if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")

    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()
