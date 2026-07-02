cmake_minimum_required(VERSION 3.20)
project(NukeScript C CXX)   # C too — the vendored lua sources are C

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")   # VCPKG_ROOT env var, not a hardcoded path

# Lua is VENDORED STATICALLY (its C sources compiled in) — like imgui in NukeGUI. No lua.dll; a clean
# build needs nothing extra. NOT defining LUA_BUILD_AS_DLL keeps lua's symbols internal to NukeScript.dll.
file(GLOB NUKESCRIPT_LUA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/deps/lua/*.c")

add_library(NukeScript SHARED
    src/NukeScript.cpp
    ${NUKESCRIPT_LUA_SRC}
)

# Consumer of the engine DLL: NUKEENGINE_API symbols are dllimport, link NukeEngine.lib.
target_compile_definitions(NukeScript PRIVATE
    WIN32 _WINDOWS NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    GLM_ENABLE_EXPERIMENTAL
    BOOST_ALL_DYN_LINK
)

target_include_directories(NukeScript PRIVATE
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/include
    ${NUKE_ROOT}/NukeEngine/deps
    ${NUKE_ROOT}/NukeEngine/deps/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/lua                 # vendored lua headers (lua.hpp) — use these, not vcpkg's
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/LuaBridge3/Source   # LuaBridge3 (header-only)
    ${VCPKG_INSTALLED}/include                       # boost + nlohmann
)

target_link_directories(NukeScript PRIVATE
    ${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>   # NukeEngine.lib (import lib)
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib        # lua + boost
)

target_link_libraries(NukeScript PRIVATE
    NukeEngine
)

# Deploy the plugin DLL into the editor's modules/ folder.
set(MODULES_DIR "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/modules")
add_custom_command(TARGET NukeScript POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukeScript>" "${MODULES_DIR}/"
    COMMENT "Deploying NukeScript.dll to modules/"
)

# (lua is vendored statically now — no lua.dll to deploy.)

# Deploy the demo Lua scripts INTO the project content (contentRoot = "project/content"), so they
# resolve via AppInstance::ResolveContent — NOT dumped in the build root.
set(SCRIPTS_DIR "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/project/content/scripts")
add_custom_command(TARGET NukeScript POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SCRIPTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${SCRIPTS_DIR}"
    COMMENT "Deploying demo Lua scripts to project/content/scripts/"
)
