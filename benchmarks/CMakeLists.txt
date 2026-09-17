# SPDX-License-Identifier: GPL-3.0-or-later
#
# Benchmarks are off by default: they take far longer than the tests and nothing depends on
# them. Enable with -DATLAS_BUILD_BENCHMARKS=ON.
if(NOT ATLAS_BUILD_BENCHMARKS)
    return()
endif()

add_executable(atlas_bench
    src/main.cpp
    src/harness.cpp
    src/bench_quads.cpp
    src/bench_math.cpp
    src/bench_allocations.cpp
    src/bench_assets.cpp
    src/bench_hash.cpp
)

target_link_libraries(atlas_bench PRIVATE atlas::renderer atlas::platform atlas::simulation atlas::assets)

# The simulation benchmark drives the lab's world. Applications and benchmarks are gated
# separately, so it is only built when the lab's simulation library exists.
if(TARGET atlas_lab_sim)
    target_sources(atlas_bench PRIVATE src/bench_simulation.cpp)
    target_link_libraries(atlas_bench PRIVATE atlas::lab_sim)
endif()

# The cell field benchmark needs the lab's renderer, which needs a device.
if(TARGET atlas_lab_view)
    target_sources(atlas_bench PRIVATE src/bench_cell_field.cpp)
    target_link_libraries(atlas_bench PRIVATE atlas::lab_view)
endif()

# The asset benchmark encodes its own test image with the header-only stb writer. SYSTEM,
# for the same reason the assets module includes the decoder that way.
find_path(STB_INCLUDE_DIR NAMES stb_image_write.h REQUIRED)
target_include_directories(atlas_bench SYSTEM PRIVATE ${STB_INCLUDE_DIR})
target_include_directories(atlas_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

set_target_properties(atlas_bench PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "benchmarks")

atlas_set_warnings(atlas_bench)
atlas_set_common_compile_options(atlas_bench)
atlas_apply_sanitizers(atlas_bench)
atlas_link_tracy(atlas_bench)
