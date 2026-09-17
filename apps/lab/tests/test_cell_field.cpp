// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/cell_field.hpp>
#include <atlas/lab/generate.hpp>
#include <atlas/lab/snapshot.hpp>

#include "gpu_harness.hpp"
#include <catch2/catch_test_macros.hpp>

using atlas::lab::CellField;
using atlas::lab::MapMode;
using atlas::lab::testing::make_gpu_harness;

TEST_CASE("a map-mode switch reads a different band and rebuilds no geometry", "[lab][gpu]") {
    auto harness = make_gpu_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    const auto lab =
        atlas::lab::generate({.width = 16, .height = 16, .chunk_size = 4, .seed = 3}).value();
    auto field = CellField::create(harness->device, {}).value();
    field.resize(128, 128);
    field.set_layout(lab.layout);
    const auto snapshot = atlas::lab::build_snapshot(lab.world, lab.ids, lab.layout, {});

    const auto* geometry = field.geometry_data();
    const auto size = field.geometry_size();
    REQUIRE(size == 256);

    // Draw in two modes into an offscreen target and read both back. Offscreen, because an
    // offscreen pass works whether or not a swapchain image is available this frame.
    const auto target = harness->device
                            .create_texture({.width = 128,
                                             .height = 128,
                                             .format = harness->device.swapchain_format(),
                                             .usage = {.sampled = false, .colour_target = true},
                                             .debug_name = "mode test target"})
                            .value();
    const auto draw_and_read = [&](MapMode mode) {
        auto frame = harness->device.begin_frame().value();
        {
            auto pass = frame.begin_render_pass({.colour = {.texture = target}}).value();
            const auto stats = field.draw(pass, *snapshot, mode);
            CHECK(stats.visible_chunks == 16);
            CHECK(stats.visible_cells == 256);
            pass.end();
        }
        REQUIRE(harness->device.end_frame(std::move(frame)).has_value());
        const auto ticket =
            harness->device.request_readback(target, {.extent = {.width = 128, .height = 128}})
                .value();
        REQUIRE(harness->device.wait_idle().has_value());
        return harness->device.take_readback(ticket).value().pixels;
    };
    const auto colour_pixels = draw_and_read(MapMode::ColorIndex);
    const auto owner_pixels = draw_and_read(MapMode::OwnerIndex);

    CHECK(field.geometry_data() == geometry);
    CHECK(field.geometry_size() == size);
    CHECK(colour_pixels != owner_pixels);
    harness->device.destroy_texture(target);
}

TEST_CASE("culling keeps only the chunks the camera can see", "[lab][gpu]") {
    auto harness = make_gpu_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    const auto lab =
        atlas::lab::generate({.width = 64, .height = 64, .chunk_size = 8, .seed = 3}).value();
    auto field = CellField::create(harness->device, {}).value();
    field.resize(128, 128);
    field.set_layout(lab.layout);
    std::vector<std::uint32_t> visible;

    field.cull(visible);
    CHECK(visible.size() == 64);  // fitted: everything

    // Zoom in on the top-left corner until only that region is in view.
    field.camera().set_centre({0.0F, 0.0F});
    field.camera().set_zoom(4.0F);  // 128 px / 4 = 32 world units = 4 cells = half a chunk
    field.cull(visible);
    REQUIRE_FALSE(visible.empty());
    CHECK(visible.size() < 64);
    for (const auto chunk : visible) {
        CHECK(field.chunk_bounds(chunk).overlaps(field.camera().visible_bounds()));
    }
    CHECK(visible.front() == 0);
}
