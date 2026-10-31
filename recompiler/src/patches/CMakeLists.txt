add_library(rebrewu_patches STATIC
    manifest_patches.cpp
)

target_include_directories(rebrewu_patches
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rebrewu_patches
    PUBLIC
        rebrewu_core
        rebrewu_ppc
)
