// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the Strategy Lab's cell field costs the processor to submit.
//
// This exists because the number it replaces was wrong. M7 reported "drawing a million cells
// costs 8.3 ms" from the lab's own frame timer, and 8.3 ms is 120 Hz: the same figure came back
// for four thousand cells and for a million, because the frame was waiting for the display
// rather than for the engine. bench_quads carries a comment about exactly that trap, and the
// lab walked into it anyway.
//
// So, as in bench_quads: acquiring a swapchain image and presenting sit outside the timed
// section, and what is timed is what the engine controls — culling, filling instances from the
// snapshot, and recording the draws.

#include <atlas/lab/cell_field.hpp>
#include <atlas/lab/generate.hpp>
#include <atlas/lab/snapshot.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include "harness.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace {

using atlas::bench::Result;
using atlas::lab::CellField;
using atlas::lab::MapMode;
using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::rhi::Device;

struct Harness {
    Platform platform;
    Window window;
    Device device;

    Harness(Platform p, Window w, Device d)
        : platform(std::move(p)), window(std::move(w)), device(std::move(d)) {}

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;
    ~Harness() = default;
};

[[nodiscard]] std::optional<Harness> make_harness() {
    auto platform = Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    auto window = platform->create_window(
        {.title = "Atlas cell field benchmark", .width = 1280, .height = 720, .resizable = false});
    if (!window) {
        return std::nullopt;
    }
    // Immediate, so that presenting does not wait for the display. The timed section excludes
    // presenting in any case; this keeps the untimed part from dominating the wall clock.
    auto device = Device::create(
        {.debug = false, .present_mode = atlas::rhi::PresentMode::Immediate}, *window);
    if (!device) {
        return std::nullopt;
    }
    return std::make_optional<Harness>(std::move(*platform), std::move(*window),
                                       std::move(*device));
}

/// One frame, returning the nanoseconds spent culling, filling and recording. Zero when no
/// swapchain image was available, which the harness treats as a skipped iteration.
[[nodiscard]] std::uint64_t render_once(Harness& harness, CellField& field,
                                        const atlas::lab::CellSnapshot& snapshot, MapMode mode) {
    auto frame = harness.device.begin_frame();
    if (!frame || !frame->has_swapchain_target()) {
        if (frame) {
            (void)harness.device.end_frame(std::move(*frame));
        }
        return 0;
    }
    auto pass = frame->begin_render_pass({});
    if (!pass) {
        (void)harness.device.end_frame(std::move(*frame));
        return 0;
    }

    const auto start = std::chrono::steady_clock::now();
    (void)field.draw(*pass, snapshot, mode);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    pass->end();
    (void)harness.device.end_frame(std::move(*frame));
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

std::vector<Result> run() {
    std::vector<Result> results;
    auto harness = make_harness();
    if (!harness) {
        std::printf("cell field: skipped, no graphics device available\n");
        return results;
    }

    // The sizes docs/PERFORMANCE.md names, and the shapes the lab actually runs.
    for (const auto [side, chunk] :
         {std::pair{100U, 10U}, std::pair{320U, 32U}, std::pair{1024U, 32U}}) {
        auto lab =
            atlas::lab::generate({.width = side, .height = side, .chunk_size = chunk, .seed = 5});
        if (!lab) {
            std::printf("cell field: skipped %ux%u, %s\n", side, side,
                        lab.error().to_string().c_str());
            continue;
        }
        const auto snapshot = atlas::lab::build_snapshot(lab->world, lab->ids, lab->layout, {});

        auto field = CellField::create(harness->device, {});
        if (!field) {
            std::printf("cell field: skipped, %s\n", field.error().to_string().c_str());
            return results;
        }
        field->resize(1280, 720);
        field->set_layout(lab->layout);  // also fits the camera, so every chunk is visible

        if (render_once(*harness, *field, *snapshot, MapMode::ColorIndex) == 0) {
            std::printf("cell field: skipped %ux%u, no swapchain image available\n", side, side);
            continue;
        }
        const std::uint64_t cells = lab->layout.cell_count();
        auto result = atlas::bench::measure_reported(
            "lab/cell_field_submit", std::format("cells={}", cells), 60, 10,
            [&] { return render_once(*harness, *field, *snapshot, MapMode::ColorIndex); });
        result.units_per_iteration = cells;
        result.unit_name = "cells";
        results.push_back(std::move(result));
    }
    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("cell_field", run);

}  // namespace
