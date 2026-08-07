cmake_minimum_required(VERSION 3.20)
project(NukeScript C CXX)   # C too — the vendored lua sources are C

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")   # VCPKG_ROOT env var, not a hardcoded path

# nukegen (modular reflection): scan THIS module's sources for NUKE_CLASS + [[nuke::prop/func]] and
# generate an in-TU .inc that registers the module's components into the engine registry — same
# mechanism as the core, so no hand-written MakeField. NukeScript.cpp #includes the .inc + calls the
# generated NukeReflectInit_NukeScript() from OnLoad. Regenerated whenever the source changes.
set(NUKE_GEN "${NUKE_ROOT}/NukeUtils/bin/NukeGen${NUKE_HOSTTOOL_SUFFIX}")   # built FIRST by the superbuild
set(NUKESCRIPT_GEN "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeScript.gen.inc")
add_custom_command(
    OUTPUT  "${NUKESCRIPT_GEN}"
    COMMAND "${NUKE_GEN}"
            --include NukeScript/src --out NukeScript/src/NukeScript.gen.inc
            --init NukeReflectInit_NukeScript --scan-cpp --no-includes
            --sdk "${NUKE_ROOT}/NukeUtils/sdk/nukesdk/NukeScript.sdk.h" --doc "${NUKE_ROOT}/NukeUtils/sdkdocs/NukeScript.md"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeScript.cpp" "${NUKE_GEN}"
    WORKING_DIRECTORY "${NUKE_ROOT}"
    COMMENT "nukegen: NukeScript modular reflection"
    VERBATIM)
add_custom_target(NukeScriptGen DEPENDS "${NUKESCRIPT_GEN}")

# Lua is VENDORED STATICALLY (its C sources compiled in) — like imgui in NukeGUI. No lua.dll; a clean
# build needs nothing extra. NOT defining LUA_BUILD_AS_DLL keeps lua's symbols internal to NukeScript.dll.
file(GLOB NUKESCRIPT_LUA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/deps/lua/*.c")

add_library(NukeScript SHARED
    src/NukeScript.cpp
    ${NUKESCRIPT_LUA_SRC}
)
add_dependencies(NukeScript NukeScriptGen)   # generate the reflection .inc before compiling

# Consumer of the engine DLL: NUKEENGINE_API symbols are dllimport, link NukeEngine.lib.
target_compile_definitions(NukeScript PRIVATE
    _USE_MATH_DEFINES
    GLM_ENABLE_EXPERIMENTAL
)
if(WIN32)
    target_compile_definitions(NukeScript PRIVATE
        WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS
        BOOST_ALL_DYN_LINK
    )
else()
    # Deuterium: engine deps (boost/glm/nlohmann includes) ride in on the NukeEngine target.
    # Lua needs its platform told explicitly (else its liolib #elif trips on the undefined
    # WINAPI_FAMILY_PARTITION function-macro).
    target_compile_definitions(NukeScript PRIVATE
        $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>
        $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX>)
    # Vendored lua/LuaBridge stay internal to the module (the "no lua.dll" intent): hidden
    # visibility prevents cross-image weak-symbol coalescing; `plugin` + ABI stamps carry
    # explicit default-visibility attributes and stay exported.
    target_compile_options(NukeScript PRIVATE
        -fvisibility=hidden -fvisibility-inlines-hidden
        -Wno-c++11-narrowing -Wno-unknown-attributes)
    set_target_properties(NukeScript PROPERTIES
        PREFIX ""                                    # loader matches modules by file stem
        BUILD_RPATH "@loader_path/..;$ORIGIN/..")    # libNukeEngine lives in the run dir above modules/
endif()

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
    ${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>   # NukeEngine.lib (import lib)
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib        # lua + boost
)

target_link_libraries(NukeScript PRIVATE
    NukeEngine
)

# Deploy the plugin DLL into the editor's modules/ folder.
set(MODULES_DIR "${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>/modules")
add_custom_command(TARGET NukeScript POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukeScript>" "${MODULES_DIR}/"
    COMMENT "Deploying NukeScript.dll to modules/"
)

# (lua is vendored statically now — no lua.dll to deploy.)

# Deploy the demo Lua scripts INTO the project content (contentRoot = "project/content"), so they
# resolve via AppInstance::ResolveContent — NOT dumped in the build root.
set(SCRIPTS_DIR "${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>/project/content/scripts")
add_custom_command(TARGET NukeScript POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SCRIPTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${SCRIPTS_DIR}"
    COMMENT "Deploying demo Lua scripts to project/content/scripts/"
)
