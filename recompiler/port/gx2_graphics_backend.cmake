# Vulkan is the only native graphics backend.
set(RBREW_GRAPHICS_BACKEND "Vulkan" CACHE STRING "Native GX2 graphics backend")
set_property(CACHE RBREW_GRAPHICS_BACKEND PROPERTY STRINGS Vulkan)
if(NOT RBREW_GRAPHICS_BACKEND STREQUAL "Vulkan")
    message(FATAL_ERROR "Only the Vulkan backend is supported")
endif()
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(glslang CONFIG REQUIRED)
if(NOT EXISTS "${glslang_DIR}/GlslangVulkanOnly.cmake")
    message(FATAL_ERROR "Build the Vulkan dependency profile and set glslang_DIR to the directory of its glslang CMake config (lib/cmake/glslang, lib64/cmake/glslang, ...)")
endif()
set(RBREW_GX2_DIR "${CMAKE_CURRENT_LIST_DIR}/os/gx2")
option(RBREW_TEXTURE_DIAGNOSTICS "Build the opt-in one-session texture lifecycle journal" OFF)
set(RBREW_GX2_BACKEND_SOURCES
    ${RBREW_GX2_DIR}/gx2_texture_diagnostics.cpp
    ${RBREW_GX2_DIR}/gx2_render_vk.cpp
    ${RBREW_GX2_DIR}/gx2_render_vk_draw.cpp
    ${RBREW_GX2_DIR}/gx2_render_vk_capture.cpp
    ${RBREW_GX2_DIR}/gx2_vk_context.cpp
    ${RBREW_GX2_DIR}/gx2_frame_interp.cpp
    ${RBREW_GX2_DIR}/gx2_weather.cpp
    ${RBREW_GX2_DIR}/gx2_env_light.cpp
    ${RBREW_GX2_DIR}/gx2_texture_pack.cpp
    ${RBREW_GX2_DIR}/vk/gx2_vk.cpp
    ${RBREW_GX2_DIR}/vk/gx2_vk_capture.cpp
    ${RBREW_GX2_DIR}/vk/gx2_vk_shader.cpp
    ${RBREW_GX2_DIR}/vk/gx2_imgui_overlay.cpp
    ${RBREW_GX2_DIR}/vk/gx2_graphics_menu.cpp)
function(rbrew_gx2_backend target scope)
    target_sources(${target} PRIVATE ${RBREW_GX2_BACKEND_SOURCES})
    target_compile_definitions(${target} ${scope} RBREW_GRAPHICS_VULKAN=1)
    if(RBREW_TEXTURE_DIAGNOSTICS)
        target_compile_definitions(${target} PRIVATE RBREW_TEXTURE_DIAGNOSTICS=1)
        if(WIN32)
            find_file(RBREW_TEXTURE_DIAG_LAYER_JSON VkLayer_khronos_validation.json
                PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
                      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin" NO_DEFAULT_PATH)
            if(RBREW_TEXTURE_DIAG_LAYER_JSON)
                get_filename_component(layer_directory "${RBREW_TEXTURE_DIAG_LAYER_JSON}" DIRECTORY)
                target_compile_definitions(${target} PRIVATE RBREW_TEXTURE_DIAG_LAYER_PATH="${layer_directory}")
            endif()
        endif()
    endif()
    target_link_libraries(${target} ${scope} Vulkan::Headers volk::volk glslang::glslang
        glslang::glslang-default-resource-limits glslang::SPIRV)
endfunction()
# Debug information for the renderer sources only, so gambit_render_profile
# can name hot functions and lines. Code generation is unchanged.
option(RBREW_GX2_DEBUG_INFO "Emit debug information for the GX2 renderer sources (profiling)" OFF)
if(RBREW_GX2_DEBUG_INFO AND MSVC)
    set_source_files_properties(${RBREW_GX2_BACKEND_SOURCES} PROPERTIES COMPILE_OPTIONS "/Zi")
endif()
