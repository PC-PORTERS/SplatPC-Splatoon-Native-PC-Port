cmake_minimum_required(VERSION 3.18)
project(GambitPort CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared public headers (the logger lives here).
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
set(RBREW_GAME_REVISION "16" CACHE STRING "Title revision of the selected manifest (app_version)")
if(NOT RBREW_GAME_REVISION MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Unsupported title revision")
endif()
add_compile_definitions(RBREW_GAME_REVISION=${RBREW_GAME_REVISION})
# MinGW headers (the Builder's userspace clang) default to Windows 7 and hide Windows 10 APIs such as
# BCryptHash; the MSVC SDK already targets Windows 10.
if(MINGW)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# Windowing & input: require the audited static SDL2 Vulkan-only profile.
find_package(SDL2 CONFIG REQUIRED)
if(NOT TARGET SDL2::SDL2-static OR NOT EXISTS "${SDL2_DIR}/SDL2VulkanOnly.cmake")
    message(FATAL_ERROR "Build the Vulkan dependency profile with tools/build_vulkan_dependencies.py and set SDL2_DIR to its cmake directory")
endif()
set(SDL2_LIBRARIES SDL2::SDL2-static)

# Graphics backend: Vulkan is the only native graphics backend.
set(RBREW_GRAPHICS_BACKEND "Vulkan" CACHE STRING "Native GX2 graphics backend")
set_property(CACHE RBREW_GRAPHICS_BACKEND PROPERTY STRINGS Vulkan)
if(NOT RBREW_GRAPHICS_BACKEND STREQUAL "Vulkan")
    message(FATAL_ERROR "Gambit requires the Vulkan backend. No alternate graphics backend is available.")
endif()
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(glslang CONFIG REQUIRED)
if(NOT EXISTS "${glslang_DIR}/GlslangVulkanOnly.cmake")
    message(FATAL_ERROR "Build the Vulkan dependency profile and set glslang_DIR to the directory of its glslang CMake config (lib/cmake/glslang, lib64/cmake/glslang, ...)")
endif()
option(RBREW_TEXTURE_DIAGNOSTICS "Build the opt-in one-session texture lifecycle journal" OFF)
option(RBREW_GX2_DEBUG_INFO "Emit debug information for the GX2 renderer sources (profiling)" OFF)

# Dear ImGui (docking branch), fetched at configure time.
include(FetchContent)
set(RBREW_IMGUI_GIT_REPOSITORY "https://github.com/ocornut/imgui.git" CACHE STRING "ImGui git repository")
set(RBREW_IMGUI_GIT_TAG "docking" CACHE STRING "ImGui git branch/tag (docking adds window docking)")
FetchContent_Declare(imgui
    GIT_REPOSITORY ${RBREW_IMGUI_GIT_REPOSITORY}
    GIT_TAG ${RBREW_IMGUI_GIT_TAG}
    GIT_SHALLOW TRUE)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    # SDL_GL_* fallbacks: the backend references them, the Vulkan-only SDL profile has no OpenGL.
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui_sdl2_gl_stubs.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC SDL2::SDL2-static Vulkan::Headers volk::volk)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

# miniaudio (public domain / MIT-0)
set(_miniaudio_header "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio.h")
if(NOT EXISTS "${_miniaudio_header}")
    message(STATUS "miniaudio: fetching tag 0.11.25")
    include(FetchContent)
    FetchContent_Declare(
        miniaudio_dl
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG        0.11.25
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  "__none__"   # don't call add_subdirectory
    )
    FetchContent_MakeAvailable(miniaudio_dl)
    get_filename_component(_miniaudio_dir "${_miniaudio_header}" DIRECTORY)
    file(MAKE_DIRECTORY "${_miniaudio_dir}")
    file(COPY_FILE "${miniaudio_dl_SOURCE_DIR}/miniaudio.h" "${_miniaudio_header}")
    message(STATUS "miniaudio: header ready at third_party/miniaudio.h")
else()
    message(STATUS "miniaudio: using cached header (third_party/miniaudio.h)")
endif()
set(HAVE_MINIAUDIO ON)

add_subdirectory(third_party/cemu_shader)

# Manifest
# Every Manifest/*.manifest describes one image. Prefer an explicit
# -DRBREW_MANIFEST, then the manifest that matches -DRBREW_RPX, then the only
# manifest in the tree. A plain variable, not a cache entry: a cached default
# would freeze a wrong path into an existing build tree.
set(RBREW_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Manifest")
set(RBREW_MANIFEST_PATH "")
if(DEFINED RBREW_MANIFEST AND EXISTS "${RBREW_MANIFEST}")
    set(RBREW_MANIFEST_PATH "${RBREW_MANIFEST}")   # explicit -DRBREW_MANIFEST=...
endif()
set(RBREW_PLT_RESOLVER "${CMAKE_CURRENT_SOURCE_DIR}/../tools/plt_resolver.py")
set(RBREW_PLT_OUTPUTS
    gambit_plt_stubs.cpp
    os/gambit_plt_auto.cpp
    os/coreinit/coreinit_addrs.h
    os/gx2/gx2_addrs.h
    os/padscore/padscore_addrs.h
    os/snd_core/snd_core_addrs.h
    os/stubs/stubs_addrs.h
    os/vpad/vpad_addrs.h
    runtime/game_revision.h
    runtime/game_branch_tables.h)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT RBREW_MANIFEST_PATH AND Python3_Interpreter_FOUND AND EXISTS "${RBREW_PLT_RESOLVER}")
    if(DEFINED RBREW_RPX AND EXISTS "${RBREW_RPX}")
        # The image itself selects its manifest, exactly as the recompiler does.
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${RBREW_PLT_RESOLVER}" select
                    --rpx "${RBREW_RPX}" --manifest-dir "${RBREW_MANIFEST_DIR}"
            RESULT_VARIABLE RBREW_SELECT_RESULT
            OUTPUT_VARIABLE RBREW_SELECT_STDOUT
            ERROR_VARIABLE  RBREW_SELECT_STDERR)
        if(NOT RBREW_SELECT_RESULT EQUAL 0)
            message(FATAL_ERROR
                "No manifest describes ${RBREW_RPX}:
${RBREW_SELECT_STDERR}")
        endif()
        string(STRIP "${RBREW_SELECT_STDOUT}" RBREW_MANIFEST_PATH)
    else()
        file(GLOB RBREW_MANIFESTS CONFIGURE_DEPENDS "${RBREW_MANIFEST_DIR}/*.manifest")
        list(LENGTH RBREW_MANIFESTS RBREW_MANIFEST_COUNT)
        if(RBREW_MANIFEST_COUNT EQUAL 1)
            list(GET RBREW_MANIFESTS 0 RBREW_MANIFEST_PATH)
        elseif(RBREW_MANIFEST_COUNT GREATER 1)
            # A tests-only build must not rewrite headers another tree already generated
            # (the default revision's manifest would replace a revision 288 map with the empty one).
            set(RBREW_HEADERS_PRESENT ON)
            foreach(relative IN LISTS RBREW_PLT_OUTPUTS)
                if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relative}")
                    set(RBREW_HEADERS_PRESENT OFF)
                endif()
            endforeach()
            # Without an image, fall back to the manifest of the selected revision
            # (16 by default), so builds that need no dump (CI, tests) still configure.
            set(RBREW_REVISION_MANIFEST "${RBREW_MANIFEST_DIR}/Gambit_v${RBREW_GAME_REVISION}.manifest")
            if(DEFINED RBREW_BUILD_GAME AND NOT RBREW_BUILD_GAME AND RBREW_HEADERS_PRESENT)
                message(STATUS "Tests-only build: keeping the existing generated headers")
            elseif(EXISTS "${RBREW_REVISION_MANIFEST}")
                set(RBREW_MANIFEST_PATH "${RBREW_REVISION_MANIFEST}")
            else()
                message(FATAL_ERROR
                    "Several manifests are available. Pass -DRBREW_RPX=<Gambit.rpx> "
                    "(or -DRBREW_MANIFEST=<file>) so the build uses the right one.")
            endif()
        endif()
    endif()
endif()

# Reconfigure (and therefore re-emit) when the selected manifest changes.
if(EXISTS "${RBREW_MANIFEST_PATH}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RBREW_MANIFEST_PATH}")
endif()

# A missing py3 or manifest is tolerated so an already-generated tree, or a
# manifest-free build, still configures; the generated files are then trusted.
set(RBREW_PLT_GENERATED OFF)
if(Python3_Interpreter_FOUND AND EXISTS "${RBREW_MANIFEST_PATH}" AND EXISTS "${RBREW_PLT_RESOLVER}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${RBREW_PLT_RESOLVER}" emit
                --manifest "${RBREW_MANIFEST_PATH}"
                --out "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE RBREW_PLT_RESULT
        OUTPUT_VARIABLE RBREW_PLT_STDOUT
        ERROR_VARIABLE  RBREW_PLT_STDERR)
    if(NOT RBREW_PLT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Generating PLT addresses from ${RBREW_MANIFEST_PATH} failed:
${RBREW_PLT_STDERR}")
    endif()
    # runtime/game_revision.h (relocation of base addresses, RBREW_GAME_* constants) comes from the same manifest.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_revision_header.py"
                --manifest "${RBREW_MANIFEST_PATH}"
                --out-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        RESULT_VARIABLE RBREW_REVISION_RESULT
        ERROR_VARIABLE  RBREW_REVISION_STDERR)
    if(NOT RBREW_REVISION_RESULT EQUAL 0)
        message(FATAL_ERROR "Generating game_revision.h from ${RBREW_MANIFEST_PATH} failed:
${RBREW_REVISION_STDERR}")
    endif()
    set(RBREW_PLT_GENERATED ON)
    message(STATUS "PLT addresses generated from ${RBREW_MANIFEST_PATH}")
else()
    foreach(relative IN LISTS RBREW_PLT_OUTPUTS)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${relative}")
            message(FATAL_ERROR
                "${relative} is missing and cannot be generated. Install Python 3, then pass "
                "-DRBREW_RPX=<image> or -DRBREW_MANIFEST=<manifest> (searched ${RBREW_MANIFEST_DIR}), "
                "and reconfigure.")
        endif()
    endforeach()
    message(STATUS "PLT addresses: using the existing generated headers (no manifest selected)")
endif()

# The headers live in the source tree and are shared by every build tree (the base
# game and the 288 update use separate trees). Re-emit this tree's manifest before
# each build so an incremental build never compiles the other revision's map;
# unchanged files are not rewritten.
if(RBREW_PLT_GENERATED)
    list(TRANSFORM RBREW_PLT_OUTPUTS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE RBREW_PLT_FILES)
    # BYPRODUCTS: Ninja re-checks the headers after the step instead of trusting
    # the timestamps it read when the build started.
    add_custom_target(rbrew_plt_emit
        BYPRODUCTS ${RBREW_PLT_FILES}
        COMMAND "${Python3_EXECUTABLE}" "${RBREW_PLT_RESOLVER}" emit
                --manifest "${RBREW_MANIFEST_PATH}" --out "${CMAKE_CURRENT_SOURCE_DIR}"
        # Another tree (base game, test build) may have rewritten the shared revision headers since the
        # configure step; game_branch_tables.h drives the multiplayer case blocks, so re-emit it too.
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_revision_header.py"
                --manifest "${RBREW_MANIFEST_PATH}" --out-dir "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
        COMMENT "PLT addresses from ${RBREW_MANIFEST_PATH}"
        VERBATIM)
endif()

# Native online mode (Windows and Linux): SpotPass files, interactive login, saved tokens,
# and friends relay built directly into the game (online/online_boot.h).
set(RBREW_NLOHMANN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
if(NOT EXISTS "${RBREW_NLOHMANN_DIR}/nlohmann/json.hpp")
    # Same header and hash as the recompiler (recompiler/CMakeLists.txt).
    file(DOWNLOAD https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
         "${RBREW_NLOHMANN_DIR}/nlohmann/json.hpp"
         EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
endif()
add_library(gambit_online STATIC
    online/boss_schedule.cpp
    online/byaml.cpp
    online/friends_relay.cpp
    online/hashing.cpp
    online/integrity_proof.cpp
    online/online_boot.cpp
    online/online_login.cpp
    online/py_random.cpp
    online/social_http.cpp
    online/wg_tunnel.cpp
)
target_include_directories(gambit_online PUBLIC ${RBREW_NLOHMANN_DIR})
target_include_directories(gambit_online PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
get_filename_component(RBREW_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(RBREW_APP_VERSION "" CACHE STRING "Client release version injected from release tag")
set(RBREW_BUILD_VERSION "" CACHE STRING "Game build version shown in the window title (defaults to RBREW_APP_VERSION or dev)")
if(RBREW_BUILD_VERSION)
    string(STRIP "${RBREW_BUILD_VERSION}" _rbrew_window_version)
elseif(RBREW_APP_VERSION)
    string(STRIP "${RBREW_APP_VERSION}" _rbrew_window_version)
else()
    set(_rbrew_window_version "dev")
endif()
if(NOT _rbrew_window_version)
    set(_rbrew_window_version "dev")
endif()
if(NOT RBREW_APP_VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
            WORKING_DIRECTORY "${RBREW_REPO_ROOT}"
            RESULT_VARIABLE _rbrew_tag_res
            OUTPUT_VARIABLE _rbrew_tag_out
            ERROR_QUIET)
        if(_rbrew_tag_res EQUAL 0)
            string(STRIP "${_rbrew_tag_out}" RBREW_APP_VERSION)
        endif()
    endif()
endif()
if(RBREW_APP_VERSION MATCHES "([0-9]+(\\.[0-9]+)+)")
    set(RBREW_APP_VERSION "${CMAKE_MATCH_1}")
else()
    set(RBREW_APP_VERSION "")
endif()
set_property(SOURCE online/online_boot.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    RBREW_DEFAULT_INTEGRITY_ROOT="${RBREW_REPO_ROOT}"
    RBREW_APP_VERSION="${RBREW_APP_VERSION}")
set_property(SOURCE os/gx2/gx2_context.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    RBREW_BUILD_VERSION="${_rbrew_window_version}")
if(WIN32)
    target_link_libraries(gambit_online PUBLIC winhttp bcrypt)
else()
    target_link_libraries(gambit_online PUBLIC CURL::libcurl)
endif()

# Userspace WireGuard P2P client (boringtun fetched from GitHub + thin C-ABI shim).
option(RBREW_ENABLE_BORINGTUN "Build and link the boringtun WireGuard P2P client" ON)
find_program(CARGO_EXECUTABLE cargo)
if(RBREW_ENABLE_BORINGTUN AND CARGO_EXECUTABLE)
    set(RBREW_BORINGTUN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boringtun")
    if(NOT EXISTS "${RBREW_BORINGTUN_SRC}/boringtun/Cargo.toml")
        message(STATUS "boringtun: fetching tag boringtun-cli-0.7.1 from GitHub")
        FetchContent_Declare(
            boringtun_dl
            GIT_REPOSITORY https://github.com/cloudflare/boringtun.git
            GIT_TAG        boringtun-cli-0.7.1
            GIT_SHALLOW    TRUE
            SOURCE_DIR     "${RBREW_BORINGTUN_SRC}"
            SOURCE_SUBDIR  "__none__"
        )
        FetchContent_GetProperties(boringtun_dl)
        if(NOT boringtun_dl_POPULATED)
            FetchContent_Populate(boringtun_dl)
        endif()
    else()
        message(STATUS "boringtun: using cached source (third_party/boringtun)")
    endif()
    set(RBREW_BORINGTUN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/online/boringtun_ffi")
    set(RBREW_BORINGTUN_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/boringtun_target")
    set(RBREW_BORINGTUN_CARGO_ARGS build --release --manifest-path "${RBREW_BORINGTUN_DIR}/Cargo.toml")
    if(MINGW)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
            set(_rbrew_rust_target "x86_64-pc-windows-gnullvm")
        else()
            set(_rbrew_rust_target "x86_64-pc-windows-gnu")
        endif()
        find_program(RUSTUP_EXECUTABLE rustup)
        if(RUSTUP_EXECUTABLE)
            execute_process(
                COMMAND "${RUSTUP_EXECUTABLE}" target list --installed
                OUTPUT_VARIABLE _rbrew_rust_targets
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(NOT _rbrew_rust_targets MATCHES "${_rbrew_rust_target}")
                message(STATUS "boringtun: adding Rust target ${_rbrew_rust_target} for MinGW")
                execute_process(COMMAND "${RUSTUP_EXECUTABLE}" target add "${_rbrew_rust_target}")
            endif()
        endif()
        list(APPEND RBREW_BORINGTUN_CARGO_ARGS --target "${_rbrew_rust_target}")
        set(RBREW_BORINGTUN_LIB "${RBREW_BORINGTUN_TARGET_DIR}/${_rbrew_rust_target}/release/libboringtun_client.a")
    elseif(WIN32)
        set(RBREW_BORINGTUN_LIB "${RBREW_BORINGTUN_TARGET_DIR}/release/boringtun_client.lib")
    else()
        set(RBREW_BORINGTUN_LIB "${RBREW_BORINGTUN_TARGET_DIR}/release/libboringtun_client.a")
    endif()
    set(RBREW_BORINGTUN_ENV "CARGO_TARGET_DIR=${RBREW_BORINGTUN_TARGET_DIR}")
    if(UNIX AND NOT APPLE)
        # Prefer system gcc/clang over userspace Zig cc (whose CLI rejects Rust target triples like x86_64-unknown-linux-gnu).
        find_program(RBREW_SYSTEM_CC NAMES gcc clang PATHS /usr/bin /usr/local/bin /bin NO_DEFAULT_PATH)
        find_program(RBREW_SYSTEM_AR NAMES gcc-ar ar PATHS /usr/bin /usr/local/bin /bin NO_DEFAULT_PATH)
        if(RBREW_SYSTEM_CC)
            list(APPEND RBREW_BORINGTUN_ENV "CC=${RBREW_SYSTEM_CC}")
            if(RBREW_SYSTEM_AR)
                list(APPEND RBREW_BORINGTUN_ENV "AR=${RBREW_SYSTEM_AR}")
            endif()
        else()
            find_program(_rbrew_fallback_cc NAMES cc clang gcc)
            if(NOT _rbrew_fallback_cc)
                set(_rbrew_fallback_cc "cc")
            endif()
            set(_rbrew_zig_cc_wrapper "${CMAKE_CURRENT_BINARY_DIR}/boringtun_cc_wrapper.sh")
            file(WRITE "${_rbrew_zig_cc_wrapper}"
"#!/bin/sh
n=$#
i=0
while [ $i -lt $n ]; do
    a=$1
    shift
    case \"$a\" in
        --target=x86_64-unknown-linux-gnu) set -- \"$@\" -target x86_64-linux-gnu ;;
        --target=aarch64-unknown-linux-gnu) set -- \"$@\" -target aarch64-linux-gnu ;;
        --target=*) ;;
        *) set -- \"$@\" \"$a\" ;;
    esac
    i=$((i + 1))
done
exec \"${_rbrew_fallback_cc}\" \"$@\"
")
            file(CHMOD "${_rbrew_zig_cc_wrapper}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
            list(APPEND RBREW_BORINGTUN_ENV "CC=${_rbrew_zig_cc_wrapper}")
        endif()
    elseif(NOT MINGW)
        list(APPEND RBREW_BORINGTUN_ENV --unset=CC --unset=CXX)
    endif()
    add_custom_command(
        OUTPUT "${RBREW_BORINGTUN_LIB}"
        COMMAND "${CMAKE_COMMAND}" -E env ${RBREW_BORINGTUN_ENV}
                "${CARGO_EXECUTABLE}" ${RBREW_BORINGTUN_CARGO_ARGS}
        DEPENDS "${RBREW_BORINGTUN_DIR}/Cargo.toml" "${RBREW_BORINGTUN_DIR}/src/lib.rs"
                "${RBREW_BORINGTUN_SRC}/boringtun/Cargo.toml"
        COMMENT "Building boringtun_client static library"
        VERBATIM
    )
    add_custom_target(boringtun_client_build DEPENDS "${RBREW_BORINGTUN_LIB}")
    add_library(boringtun_client STATIC IMPORTED GLOBAL)
    set_target_properties(boringtun_client PROPERTIES IMPORTED_LOCATION "${RBREW_BORINGTUN_LIB}")
    add_dependencies(boringtun_client boringtun_client_build)
    target_include_directories(gambit_online PRIVATE "${RBREW_BORINGTUN_DIR}/include")
    target_compile_definitions(gambit_online PRIVATE RBREW_HAS_BORINGTUN=1)
    target_link_libraries(gambit_online PUBLIC boringtun_client)
    if(WIN32)
        target_link_libraries(gambit_online PUBLIC ws2_32 userenv ntdll bcrypt synchronization advapi32 dbghelp)
    else()
        target_link_libraries(gambit_online PUBLIC pthread dl m)
    endif()
endif()
if(MSVC)
    target_compile_options(gambit_online PRIVATE /utf-8)
endif()

file(GLOB_RECURSE GAMBIT_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/os/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/loader/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp"
)
list(REMOVE_ITEM GAMBIT_RUNTIME_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/os/gx2/gx2_render_headless.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/rebrewu_differential.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/rebrewu_ppc_hybrid.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/rebrewu_ppc_interpreter.cpp"
)
list(APPEND GAMBIT_RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gambit_plt_stubs.cpp")

add_library(gambit_runtime STATIC ${GAMBIT_RUNTIME_SOURCES})

target_include_directories(gambit_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(gambit_runtime PUBLIC HAVE_SDL2)
# Gameplay builds contain no console/file diagnostics or profiler hooks.
# RBREW_DIAGNOSTIC_RUNTIME keeps them for a separately linked, one-off
# diagnostic copy (capture/trace); never for the delivered game.
option(RBREW_DIAGNOSTIC_RUNTIME "Keep diagnostics in the runtime for a one-off diagnostic copy" OFF)
if(NOT RBREW_DIAGNOSTIC_RUNTIME)
    target_compile_definitions(gambit_runtime PRIVATE RBREW_DISABLE_LOGGING)
    target_compile_definitions(gambit_online PRIVATE RBREW_DISABLE_LOGGING)
    target_compile_definitions(cemu_shader PRIVATE RBREW_DISABLE_LOGGING)
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS RBREW_DISABLE_LOGGING)
endif()
# HYPER_OPTIMIZE: aggressive CPU path for the recompiled game code. The host
# aperture covers the whole 32-bit guest address space, so generated loads and
# stores skip bounds checks and store gating; the compiler gets full inlining
# and LTCG/LTO. OFF restores the checked memory path and the original flags.
option(HYPER_OPTIMIZE "Aggressive CPU optimizations for guest code (zero-check memory, LTCG/LTO)" ON)
if(HYPER_OPTIMIZE)
    message(STATUS "HYPER_OPTIMIZE enabled: zero-check guest memory, aggressive inlining")
endif()
target_compile_definitions(gambit_runtime PUBLIC RBREW_GRAPHICS_VULKAN=1)
if(RBREW_TEXTURE_DIAGNOSTICS)
    target_compile_definitions(gambit_runtime PRIVATE RBREW_TEXTURE_DIAGNOSTICS=1)
    if(WIN32)
        find_file(RBREW_TEXTURE_DIAG_LAYER_JSON VkLayer_khronos_validation.json
            PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
                  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin" NO_DEFAULT_PATH)
        if(RBREW_TEXTURE_DIAG_LAYER_JSON)
            get_filename_component(layer_directory "${RBREW_TEXTURE_DIAG_LAYER_JSON}" DIRECTORY)
            target_compile_definitions(gambit_runtime PRIVATE RBREW_TEXTURE_DIAG_LAYER_PATH="${layer_directory}")
        endif()
    endif()
endif()
if(RBREW_GX2_DEBUG_INFO AND MSVC)
    file(GLOB_RECURSE RBREW_GX2_DEBUG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/os/gx2/*.cpp")
    list(REMOVE_ITEM RBREW_GX2_DEBUG_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/os/gx2/gx2_render_headless.cpp")
    set_source_files_properties(${RBREW_GX2_DEBUG_SOURCES} PROPERTIES COMPILE_OPTIONS "/Zi")
endif()
target_link_libraries(gambit_runtime PUBLIC
    Vulkan::Headers volk::volk glslang::glslang
    glslang::glslang-default-resource-limits glslang::SPIRV)

if(HAVE_MINIAUDIO)
    target_compile_definitions(gambit_runtime PUBLIC HAVE_MINIAUDIO)
    target_include_directories(gambit_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    )
endif()

target_link_libraries(gambit_runtime PUBLIC
    cemu_shader
    gambit_online
    ${SDL2_LIBRARIES}

    CURL::libcurl
    ZLIB::ZLIB
)
# Dear ImGui (docking branch, fetched at configure time): the performance
# overlay backend. PRIVATE: only the GX2 Vulkan layer uses it.
target_link_libraries(gambit_runtime PRIVATE imgui)

if(WIN32)
    target_link_libraries(gambit_runtime PUBLIC ws2_32 winmm bcrypt iphlpapi windowscodecs ole32 psapi)
    if(HAVE_MINIAUDIO)
        target_link_libraries(gambit_runtime PUBLIC ksuser)
    endif()
endif()

if(UNIX AND NOT APPLE AND HAVE_MINIAUDIO)
    find_library(ALSA_LIB asound)
    if(ALSA_LIB)
        target_link_libraries(gambit_runtime PUBLIC ${ALSA_LIB})
    endif()
    find_library(PTHREAD_LIB pthread)
    if(PTHREAD_LIB)
        target_link_libraries(gambit_runtime PUBLIC ${PTHREAD_LIB})
    endif()
endif()

set(GAMBIT_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Gambit" CACHE PATH
    "Directory containing RebrewU-generated Gambit C++")
# Unit tests, and the commands the Python cross-checks run (tools/tests/test_online_native.py).
if(RBREW_BUILD_TESTS)
    add_executable(gambit_online_tests ../tests/test_online_native.cpp)
    target_include_directories(gambit_online_tests PRIVATE ${RBREW_NLOHMANN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(gambit_online_tests PRIVATE gambit_online)
    if(MSVC)
        target_compile_options(gambit_online_tests PRIVATE /utf-8)
    endif()
endif()

set(RBREW_GAME_DEFAULT OFF)
if(EXISTS "${GAMBIT_GENERATED_DIR}/Gambit_data.cpp")
    set(RBREW_GAME_DEFAULT ON)
endif()
option(RBREW_BUILD_GAME "Build a private, locally generated game" ${RBREW_GAME_DEFAULT})
if(RBREW_BUILD_GAME)
file(GLOB GAMBIT_PARTS "${GAMBIT_GENERATED_DIR}/Gambit_part*.cpp")

if(TARGET rbrew_plt_emit)
    add_dependencies(gambit_runtime rbrew_plt_emit)
endif()

add_library(gambit_generated OBJECT
    ${GAMBIT_PARTS}
    ${GAMBIT_GENERATED_DIR}/Gambit_data.cpp
    ${GAMBIT_GENERATED_DIR}/Gambit_register.cpp
)

target_include_directories(gambit_generated PRIVATE
    ${GAMBIT_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(gambit_generated PRIVATE
    RBREW_DISABLE_LOGGING
    HAVE_SDL2
)
if(HYPER_OPTIMIZE)
    target_compile_definitions(gambit_generated PRIVATE RBREW_HYPER_OPTIMIZE=1)
endif()

if(MSVC)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        target_compile_options(gambit_generated PRIVATE /MP)
    endif()
    target_compile_options(gambit_generated PRIVATE "$<$<CONFIG:Debug>:/Od>" /bigobj /GS- /Gy- /EHs-c- /GR-)
    if(HYPER_OPTIMIZE)
        # Full inlining plus whole-program optimization on the generated parts.
        # /arch:AVX2 (Haswell+) brings MOVBE and BMI2 like -march=native above.
        target_compile_options(gambit_generated PRIVATE /Ob2 /Oi /GT /GL /arch:AVX2)
    else()
        target_compile_options(gambit_generated PRIVATE /Ob1)
    endif()
    if(EXISTS "${GAMBIT_GENERATED_DIR}/Gambit.h")
        target_precompile_headers(gambit_generated PRIVATE "${GAMBIT_GENERATED_DIR}/Gambit.h")
    endif()
endif()

add_executable(Gambit
    main.cpp
)

target_include_directories(Gambit PRIVATE
    ${GAMBIT_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(Gambit PRIVATE gambit_generated gambit_runtime)
if(TARGET gambit_online)
    target_link_libraries(Gambit PRIVATE gambit_online)
endif()
set_target_properties(Gambit PROPERTIES OUTPUT_NAME Splatoon)
endif()
if(WIN32)
    enable_language(RC)
    if(TARGET Gambit)
        target_sources(Gambit PRIVATE resources/splatoon.rc)
        target_include_directories(Gambit PRIVATE resources)
        if(MSVC)
            # splatoon.rc already embeds the application manifest as ID 1.
            # An automatically generated manifest would duplicate that resource.
            target_link_options(Gambit PRIVATE /MANIFEST:NO)
        endif()
    endif()
    add_library(splport_host SHARED EXCLUDE_FROM_ALL main.cpp runtime/splport_host.def)
    target_compile_definitions(splport_host PRIVATE RBREW_DISTRIBUTABLE_RUNTIME RBREW_DISABLE_LOGGING)
    target_link_libraries(splport_host PRIVATE gambit_runtime gambit_online)
    if(MSVC)
        target_compile_options(splport_host PRIVATE /utf-8)
    endif()
    add_executable(splport_check_runtime EXCLUDE_FROM_ALL tools/check_prerequisites.cpp)
    target_link_libraries(splport_check_runtime PRIVATE Vulkan::Headers volk::volk)
endif()
add_executable(cache_prepare tools/cache_prepare.cpp)
target_include_directories(cache_prepare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cache_prepare PRIVATE gambit_runtime)
if(MSVC)
    target_compile_options(cache_prepare PRIVATE /utf-8)
endif()
if(TARGET Gambit)
    if(WIN32 AND MSVC)
        set_target_properties(Gambit PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(Gambit PRIVATE /ENTRY:mainCRTStartup)
    endif()
    if(NOT RBREW_DIAGNOSTIC_RUNTIME)
        target_compile_definitions(Gambit PRIVATE RBREW_DISABLE_LOGGING)
    endif()
endif()

set(RBREW_TITLE_METADATA "" CACHE FILEPATH "Owned title meta.xml used for native MCP region profile")
if(RBREW_TITLE_METADATA)
    if(NOT EXISTS "${RBREW_TITLE_METADATA}")
        message(FATAL_ERROR "Configured RBREW_TITLE_METADATA does not exist")
    endif()
    set_source_files_properties(main.cpp PROPERTIES
        COMPILE_DEFINITIONS RBREW_TITLE_METADATA_PATH="${RBREW_TITLE_METADATA}")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(TARGET Gambit)
        target_compile_options(Gambit PRIVATE "$<$<CONFIG:Debug>:-O0>" -fno-strict-aliasing)
    endif()
    target_compile_options(gambit_runtime PRIVATE -O2)
    if(HYPER_OPTIMIZE AND TARGET gambit_generated)
        target_compile_options(gambit_generated PRIVATE -O3)
        # MOVBE fuses each guest load/store + bswap into one instruction and
        # BMI2 replaces shift-by-register sequences; -march=native matches the
        # build machine (do not redistribute such a binary).
        target_compile_options(gambit_generated PRIVATE -march=native)
        # GCC/Clang -flto on the generated corpus OOMs the linker (the parts
        # carry thousands of large functions); MSVC /GL+/LTCG is used there.
    endif()
    if(TARGET Gambit AND CMAKE_BUILD_TYPE STREQUAL "Release")
        # Symbols stay in the local binary: CPU profiling of the recompiled
        # corpus resolves Gambit_fn_* addresses (a release zip can strip them).
    endif()
endif()

if(TARGET Gambit)
    set_target_properties(Gambit PROPERTIES UNITY_BUILD OFF)
endif()

if(MSVC)
    # Keep large generated functions unoptimized in diagnostic builds, but
    # leave Release free to use its configuration's /O2 settings.
    if(TARGET Gambit)
        target_compile_options(Gambit PRIVATE "$<$<CONFIG:Debug>:/Od>" /bigobj /MP4)
        # Guest stores read a constant-initialized static TLS flag
        # (rebrewu_runtime.h); the executable needs no per-access TLS guard.
        target_compile_options(Gambit PRIVATE /Zc:tlsGuards-)
        target_link_options(Gambit PRIVATE "/MAP:$<TARGET_FILE_DIR:Gambit>/Splatoon.map")
        if(HYPER_OPTIMIZE)
            # gambit_generated is compiled with /GL; link the whole game with LTCG.
            target_link_options(Gambit PRIVATE /LTCG)
        endif()
    endif()
    target_compile_options(gambit_runtime PRIVATE /utf-8 /MP)
endif()

# Optimize host-runtime boundaries without recompiling the large generated
# guest sources with /GL. Consumers link the resulting runtime through LTCG.
# Keep precise floating-point semantics and the existing guest build flags.
option(RBREW_RUNTIME_IPO "Enable MSVC Release interprocedural optimization of the host runtime" ON)
if(RBREW_RUNTIME_IPO AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT RBREW_IPO_SUPPORTED OUTPUT RBREW_IPO_ERROR LANGUAGES CXX)
    if(RBREW_IPO_SUPPORTED)
        set_property(TARGET gambit_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET cemu_shader PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        target_link_options(gambit_runtime INTERFACE "$<$<CONFIG:Release>:/LTCG>")
    else()
        message(STATUS "Host runtime IPO unavailable: ${RBREW_IPO_ERROR}")
    endif()
endif()

option(RBREW_ENABLE_COVERAGE "Record original guest execution to RBREW_COVERAGE_FILE" OFF)
if(RBREW_ENABLE_COVERAGE)
    target_compile_definitions(gambit_runtime PUBLIC RBREW_ENABLE_COVERAGE)
endif()

option(RBREW_ENABLE_PPC_FALLBACK "Enable conservative hybrid prototype (runtime opt-in too)" OFF)
if(RBREW_ENABLE_PPC_FALLBACK)
    if(NOT TARGET Gambit)
        message(FATAL_ERROR "Hybrid diagnostics require a private generated game")
    endif()
    if(NOT EXISTS "${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.cpp" OR
       NOT EXISTS "${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.bin")
        message(FATAL_ERROR "Hybrid prototype requires regeneration with --emit-ppc-image")
    endif()
    target_sources(gambit_runtime PRIVATE runtime/rebrewu_ppc_interpreter.cpp
        runtime/rebrewu_ppc_hybrid.cpp)
    target_sources(Gambit PRIVATE "${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.cpp")
    target_compile_definitions(gambit_runtime PUBLIC RBREW_ENABLE_PPC_FALLBACK)
    target_compile_definitions(Gambit PRIVATE GAMBIT_PPC_IMAGE_PATH="${GAMBIT_GENERATED_DIR}/Gambit_ppc_image.bin")
endif()

# Offline validation of the actual silent gameplay runtime; no guest code is linked.
option(RBREW_BUILD_TESTS "Build Nintendo-free native performance regression tests" OFF)
if(RBREW_BUILD_TESTS)
    enable_testing()
    add_executable(gambit_device_tests ../tests/test_vulkan_device.cpp)
    target_include_directories(gambit_device_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(gambit_device_tests PRIVATE Vulkan::Headers volk::volk)
    add_test(NAME vulkan_device_selection COMMAND gambit_device_tests)
    add_executable(gambit_texture_pack_tests ../tests/test_texture_pack.cpp)
    target_link_libraries(gambit_texture_pack_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_texture_pack_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_texture_pack COMMAND gambit_texture_pack_tests)
    add_executable(gambit_nfp_tests ../tests/test_nn_nfp.cpp)
    target_link_libraries(gambit_nfp_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_nfp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_nn_nfp COMMAND gambit_nfp_tests)
    add_executable(gambit_camera_nan_guard_tests ../tests/test_camera_nan_guard.cpp)
    target_include_directories(gambit_camera_nan_guard_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_camera_nan_guard COMMAND gambit_camera_nan_guard_tests)
    add_executable(gambit_player_nan_guard_tests ../tests/test_player_nan_guard.cpp)
    target_include_directories(gambit_player_nan_guard_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_player_nan_guard COMMAND gambit_player_nan_guard_tests)
    add_executable(gambit_quit_confirm_tests ../tests/test_quit_confirmation.cpp)
    target_link_libraries(gambit_quit_confirm_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_quit_confirm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_quit_confirm COMMAND gambit_quit_confirm_tests)
    add_executable(gambit_mouse_camera_tests ../tests/test_mouse_camera.cpp)
    target_include_directories(gambit_mouse_camera_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_mouse_camera COMMAND gambit_mouse_camera_tests)
    add_executable(gambit_non_ieee_mode_tests ../tests/test_non_ieee_mode.cpp)
    target_link_libraries(gambit_non_ieee_mode_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_non_ieee_mode_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_non_ieee_mode COMMAND gambit_non_ieee_mode_tests)
    if(TARGET gambit_online_tests)
        add_test(NAME native_online COMMAND gambit_online_tests)
    endif()
    # Local developer tests/tools below are optional (not in the public tree).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_frame_pacing.cpp")
        add_executable(gambit_frame_pacing_tests ../tests/test_frame_pacing.cpp)
        target_include_directories(gambit_frame_pacing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME native_frame_pacing COMMAND gambit_frame_pacing_tests)
    endif()
    add_executable(gambit_texture_bench ../tests/test_texture_streaming.cpp)
    target_link_libraries(gambit_texture_bench PRIVATE gambit_runtime)
    target_include_directories(gambit_texture_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        target_link_libraries(gambit_texture_bench PRIVATE psapi)
    endif()
    foreach(case layouts uniforms textures edgecases)
        add_test(NAME streaming_${case} COMMAND gambit_texture_bench ${case})
        set_tests_properties(streaming_${case} PROPERTIES TIMEOUT 120)
    endforeach()
    add_executable(gambit_render_bench ../tests/test_render_performance.cpp)
    target_link_libraries(gambit_render_bench PRIVATE gambit_runtime)
    target_include_directories(gambit_render_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        target_link_libraries(gambit_render_bench PRIVATE psapi)
    endif()
    if(MSVC)
        # Same authored workloads with an opt-in main-thread sampler
        # (RBREW_BENCH_SAMPLE=1) and a PDB for function names. Not a test.
        add_executable(gambit_render_profile ../tests/test_render_performance.cpp)
        target_compile_definitions(gambit_render_profile PRIVATE RBREW_BENCH_SAMPLER=1)
        target_link_libraries(gambit_render_profile PRIVATE gambit_runtime psapi dbghelp)
        target_include_directories(gambit_render_profile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_options(gambit_render_profile PRIVATE /DEBUG)
    endif()
    foreach(case fallback push churn contracts lifetime heavy heavy_push overdraw watch geometry packets plaza npc_1 npc_10 npc_25 npc_50 npc_100 npc_fallback_50 guest_hooks idle_lookaround present_capture)
        add_test(NAME render_${case} COMMAND gambit_render_bench ${case})
        set_tests_properties(render_${case} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endforeach()
    # Standalone authored workloads only; never links the generated game.
    add_executable(gambit_shader_bench ../tests/test_shader_performance.cpp)
    target_link_libraries(gambit_shader_bench PRIVATE gambit_runtime)
    target_include_directories(gambit_shader_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        target_link_libraries(gambit_shader_bench PRIVATE psapi)
    endif()
    foreach(case compiler translation scheduling engine gpu gpu_memory)
        add_test(NAME shader_${case} COMMAND gambit_shader_bench ${case})
        set_tests_properties(shader_${case} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endforeach()
    target_compile_definitions(gambit_runtime PRIVATE RBREW_ADVANCED_TESTS=1)
    add_executable(gambit_advanced_tests ../tests/test_advanced_performance.cpp)
    target_link_libraries(gambit_advanced_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_advanced_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    foreach(case paired synchronization pipeline_lookup pipeline_gpu)
        add_test(NAME advanced_${case} COMMAND gambit_advanced_tests ${case})
        set_tests_properties(advanced_${case} PROPERTIES TIMEOUT 60)
    endforeach()
    # Guest-CPU character animation in recompiled form (authored code only).
    add_executable(gambit_guest_bench ../tests/test_guest_animation.cpp)
    target_link_libraries(gambit_guest_bench PRIVATE gambit_runtime)
    target_include_directories(gambit_guest_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(HYPER_OPTIMIZE)
        target_compile_definitions(gambit_guest_bench PRIVATE RBREW_HYPER_OPTIMIZE=1)
    endif()
    if(MSVC)
        target_compile_options(gambit_guest_bench PRIVATE /Zc:tlsGuards-) # as the Gambit target
    endif()
    foreach(case npc_1 npc_10 npc_25 npc_50 npc_100 paired_equivalence psq_equivalence)
        add_test(NAME guest_${case} COMMAND gambit_guest_bench ${case})
        set_tests_properties(guest_${case} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endforeach()
    add_executable(gambit_memory_tests ../tests/test_vulkan_memory.cpp)
    target_link_libraries(gambit_memory_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_memory_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    foreach(case policy allocator transfers)
        add_test(NAME memory_${case} COMMAND gambit_memory_tests ${case})
        set_tests_properties(memory_${case} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endforeach()
    add_test(NAME memory_transfers_serial COMMAND gambit_memory_tests transfers)
    set_tests_properties(memory_transfers_serial PROPERTIES ENVIRONMENT "RBREW_RENDER_SPLIT=0" TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(gambit_resolution_tests ../tests/test_render_resolution.cpp)
    add_executable(gambit_weather_tests ../tests/test_weather.cpp)
    target_link_libraries(gambit_weather_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_weather_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME weather_math COMMAND gambit_weather_tests math)
    add_test(NAME weather_gpu COMMAND gambit_weather_tests gpu)
    set_tests_properties(weather_gpu PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    target_link_libraries(gambit_resolution_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_resolution_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME native_display_resolution COMMAND gambit_resolution_tests)
    set_tests_properties(native_display_resolution PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(gambit_performance_tests ../tests/test_runtime_performance.cpp)
    target_link_libraries(gambit_performance_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_performance_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(gambit_performance_tests PRIVATE /utf-8)
    endif()
    foreach(case timing uploads textures queue queries detile compiler)
        add_test(NAME native_${case} COMMAND gambit_performance_tests ${case})
        set_tests_properties(native_${case} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    endforeach()
    if(WIN32)
        # Small-CPU render-queue model; RBREW_BENCH_AFFINITY=3 for two CPUs.
        add_test(NAME native_contention COMMAND gambit_performance_tests contention)
        set_tests_properties(native_contention PROPERTIES TIMEOUT 60)
    endif()
    foreach(case uploads textures)
        add_test(NAME native_${case}_split COMMAND gambit_performance_tests ${case})
        set_tests_properties(native_${case}_split PROPERTIES ENVIRONMENT "RBREW_RENDER_SPLIT=1" TIMEOUT 30 SKIP_RETURN_CODE 77)
        add_test(NAME native_${case}_serial COMMAND gambit_performance_tests ${case})
        set_tests_properties(native_${case}_serial PROPERTIES ENVIRONMENT "RBREW_RENDER_SPLIT=0" TIMEOUT 30 SKIP_RETURN_CODE 77)
    endforeach()
    add_executable(gambit_efficiency_tests ../tests/test_runtime_efficiency.cpp)
    target_link_libraries(gambit_efficiency_tests PRIVATE gambit_runtime)
    target_include_directories(gambit_efficiency_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(gambit_efficiency_tests PRIVATE /utf-8)
    endif()
    foreach(case detile shader cache_io queue)
        add_test(NAME efficiency_${case} COMMAND gambit_efficiency_tests ${case})
        set_tests_properties(efficiency_${case} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    endforeach()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/splport_analyze.cpp")
        add_executable(splport_analyze tools/splport_analyze.cpp)
        target_link_libraries(splport_analyze PRIVATE gambit_runtime)
        target_include_directories(splport_analyze PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        if(MSVC)
            target_compile_options(splport_analyze PRIVATE /utf-8)
        endif()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_compatibility.cpp")
        add_executable(gambit_compatibility_tests ../tests/test_compatibility.cpp)
        target_link_libraries(gambit_compatibility_tests PRIVATE gambit_runtime)
        target_include_directories(gambit_compatibility_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        if(MSVC)
            target_compile_options(gambit_compatibility_tests PRIVATE /utf-8)
        endif()
        foreach(case hiz aux account network padscore)
            add_test(NAME compatibility_${case} COMMAND gambit_compatibility_tests ${case})
            set_tests_properties(compatibility_${case} PROPERTIES TIMEOUT 30)
        endforeach()
    endif()
endif()

# Online mode: network confinement policy and local identity (header-only, no guest code).
if(RBREW_BUILD_TESTS)
    add_executable(gambit_net_policy_tests ../tests/test_net_policy.cpp)
    target_include_directories(gambit_net_policy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME net_policy COMMAND gambit_net_policy_tests)
    add_executable(gambit_online_identity_tests ../tests/test_online_identity.cpp)
    target_include_directories(gambit_online_identity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
    if(WIN32)
        target_link_libraries(gambit_online_identity_tests PRIVATE bcrypt)
    endif()
    add_test(NAME online_identity COMMAND gambit_online_identity_tests)
    add_test(NAME online_native COMMAND gambit_online_tests)
endif()

# The developer test sources are not part of the public tree.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/gx2_replay.cpp")
    add_executable(gx2_replay_quiet EXCLUDE_FROM_ALL ../tests/gx2_replay.cpp)
    target_include_directories(gx2_replay_quiet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/os/gx2)
    target_link_libraries(gx2_replay_quiet PRIVATE gambit_runtime)
    target_compile_definitions(gx2_replay_quiet PRIVATE RBREW_DISABLE_LOGGING)
endif()
# The cache-wide release /MAP names Gambit.map; give this tool its own map so
# building it never replaces the game's map (the last /MAP option wins).
if(MSVC AND TARGET gx2_replay_quiet)
    target_link_options(gx2_replay_quiet PRIVATE "/MAP:$<TARGET_FILE_DIR:gx2_replay_quiet>/gx2_replay_quiet.map")
endif()
