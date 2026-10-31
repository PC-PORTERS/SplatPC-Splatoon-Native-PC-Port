add_executable(rebrewu_manifest_patches_tests test_manifest_patches.cpp)

target_include_directories(rebrewu_manifest_patches_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(rebrewu_manifest_patches_tests
    PRIVATE
        rebrewu_patches
        rebrewu_ppc
        rebrewu_core
)

add_test(NAME rebrewu_manifest_patches COMMAND rebrewu_manifest_patches_tests)
