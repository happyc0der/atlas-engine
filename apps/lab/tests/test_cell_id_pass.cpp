// SPDX-License-Identifier: GPL-3.0-or-later
//
// The identifier pass against its analytic inverse. GridLayout::cell_index composed with
// screen_to_world gives the answer with no GPU; the pass must agree at cell edges, at chunk
// boundaries, and outside the grid, where it must say nothing.

#include <atlas/lab/cell_field.hpp>
#include <atlas/lab/cell_id_pass.hpp>
#include <atlas/lab/generate.hpp>

#include "gpu_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using atlas::lab::CellField;
using atlas::lab::CellIdPass;
using atlas::lab::testing::make_gpu_harness;

namespace {

struct Picker {
    atlas::rhi::Device* device;
    CellField* field;
    CellIdPass* pass;
    std::vector<std::uint32_t> chunks;

    [[nodiscard]] std::optional<std::uint32_t> pick(std::uint32_t x, std::uint32_t y) {
        auto frame = device->begin_frame().value();
        field->cull(chunks);
        REQUIRE(pass->render(frame, *field, chunks).has_value());
        REQUIRE(device->end_frame(std::move(frame)).has_value());
        const auto ticket = pass->request_pixel(x, y).value();
        REQUIRE(device->wait_idle().has_value());
        return CellIdPass::decode(device->take_readback(ticket).value());
    }
};

}  // namespace

TEST_CASE("the identifier pass agrees with the analytic inverse", "[lab][gpu][picking]") {
    auto harness = make_gpu_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    // 16x16 cells in chunks of 4, fitted into 128 pixels: about 7.6 pixels per cell, so
    // sub-cell precision is being tested, not just "somewhere in the grid".
    const auto lab =
        atlas::lab::generate({.width = 16, .height = 16, .chunk_size = 4, .seed = 1}).value();
    auto field = CellField::create(harness->device, {}).value();
    field.resize(128, 128);
    field.set_layout(lab.layout);
    auto pass = CellIdPass::create(harness->device, {}).value();
    REQUIRE(pass.resize(128, 128).has_value());
    Picker picker{&harness->device, &field, &pass, {}};

    std::size_t checked = 0;
    std::size_t inside = 0;
    for (std::uint32_t y = 0; y < 128; y += 5) {
        for (std::uint32_t x = 0; x < 128; x += 5) {
            const auto expected =
                field.cell_at_screen({static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F});
            const auto picked = picker.pick(x, y);
            INFO("pixel (" << x << ", " << y << ")");
            CHECK(picked == expected);
            ++checked;
            inside += expected.has_value() ? 1 : 0;
        }
    }
    // The sample must have covered both the grid and the margin around it, or the
    // "outside says nothing" half of the claim was never tested.
    CHECK(inside > 0);
    CHECK(inside < checked);
}

TEST_CASE("the identifier pass crosses chunk boundaries correctly", "[lab][gpu][picking]") {
    auto harness = make_gpu_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    const auto lab =
        atlas::lab::generate({.width = 16, .height = 16, .chunk_size = 4, .seed = 1}).value();
    auto field = CellField::create(harness->device, {}).value();
    field.resize(128, 128);
    field.set_layout(lab.layout);
    // One cell fills 32 pixels: pixel 31 is the last column of cell 0, pixel 32 the first
    // of cell 1, and pixels 127/128 straddle the first chunk boundary.
    field.camera().set_centre({0.0F, 0.0F});
    field.camera().set_zoom(4.0F);
    auto pass = CellIdPass::create(harness->device, {}).value();
    REQUIRE(pass.resize(128, 128).has_value());
    Picker picker{&harness->device, &field, &pass, {}};

    // Centre at world (0,0) puts the grid's corner in the middle of the window: the top-left
    // quadrant is outside the grid, and cells begin at pixel 64.
    CHECK_FALSE(picker.pick(10, 10).has_value());
    CHECK(picker.pick(64, 64) == lab.layout.cell_index(0, 0));
    CHECK(picker.pick(95, 64) == lab.layout.cell_index(0, 0));
    CHECK(picker.pick(96, 64) == lab.layout.cell_index(1, 0));
    CHECK(picker.pick(64, 96) == lab.layout.cell_index(0, 1));
    CHECK(picker.pick(127, 127) == lab.layout.cell_index(1, 1));
    for (const auto& [x, y] : {std::pair{64U, 64U}, std::pair{96U, 64U}, std::pair{127U, 127U}}) {
        CHECK(picker.pick(x, y) ==
              field.cell_at_screen({static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F}));
    }
}
