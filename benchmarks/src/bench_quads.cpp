// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quad batching: how long it takes to turn a list of quads into an uploaded buffer and a
// recorded draw call.
//
// This measures the processor-side cost, which is what the engine controls. It does not
// measure how long the graphics processor takes to draw the result: SDL_GPU exposes no
// timestamp queries, so that number is not available and is therefore not claimed. See
// docs/PERFORMANCE.md.
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
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
using atlas::math::Mat4;
using atlas::math::Rect;
using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::renderer::Quad;
using atlas::renderer::QuadBatch;
using atlas::rhi::Device;

struct Harness {
    Platform platform;
    Window window;
    Device device;
    atlas::rhi::TextureHandle texture;
    atlas::rhi::SamplerHandle sampler;

    Harness(Platform p, Window w, Device d, atlas::rhi::TextureHandle t,
            atlas::rhi::SamplerHandle s)
        : platform(std::move(p)), window(std::move(w)), device(std::move(d)), texture(t),
          sampler(s) {}

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;

    ~Harness() {
        device.destroy_sampler(sampler);
        device.destroy_texture(texture);
    }
};

[[nodiscard]] std::optional<Harness> make_harness() {
    auto platform = Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    auto window = platform->create_window(
        {.title = "Atlas benchmark", .width = 1280, .height = 720, .resizable = false});
    if (!window) {
        return std::nullopt;
    }

    // Validation off: it is a development aid whose cost would be measured as if it were
    // the engine's. Immediate presentation asked for, though the compositor may still pace
    // a visible window, which is why the timed section excludes presentation anyway.
    auto device = Device::create(
        {.debug = false, .present_mode = atlas::rhi::PresentMode::Immediate}, *window);
    if (!device) {
        return std::nullopt;
    }

    auto texture = device->create_texture({.width = 1,
                                           .height = 1,
                                           .format = atlas::rhi::TextureFormat::Rgba8Unorm,
                                           .debug_name = "bench white"});
    if (!texture) {
        return std::nullopt;
    }
    const std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255},
                                         std::byte{255}};
    if (!device->upload_texture(*texture, white)) {
        return std::nullopt;
    }

    auto sampler = device->create_sampler({.debug_name = "bench sampler"});
    if (!sampler) {
        return std::nullopt;
    }

    return std::make_optional<Harness>(std::move(*platform), std::move(*window), std::move(*device),
                                       *texture, *sampler);
}

[[nodiscard]] std::vector<Quad> make_quads(std::size_t count) {
    std::vector<Quad> quads;
    quads.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto x = static_cast<float>(i % 1000) * 4.0F;
        const auto y = static_cast<float>(i / 1000) * 4.0F;
        quads.push_back(Quad{.bounds = Rect{.position = {x, y}, .size = {3.0F, 3.0F}}});
    }
    return quads;
}

/// One frame, returning the nanoseconds spent batching and submitting.
///
/// Acquiring a swapchain image waits for the display and presenting hands the frame to a
/// compositor, so both sit outside the timed section. Timing them measured the display's
/// refresh interval instead: every size reported the same 8.3 milliseconds, whatever the
/// engine did. What is timed is what the engine controls. Returns zero when no image was
/// available, which the harness treats as a skipped iteration.
[[nodiscard]] std::uint64_t render_once(Harness& harness, QuadBatch& batch,
                                        const std::vector<Quad>& quads) {
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

    batch.begin(*pass, Mat4::identity());
    batch.set_texture(harness.texture, harness.sampler);
    batch.add(quads);
    (void)batch.end();

    const auto elapsed = std::chrono::steady_clock::now() - start;

    pass->end();
    (void)harness.device.end_frame(std::move(*frame));

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;

    auto harness = make_harness();
    if (!harness) {
        std::printf("quads: skipped, no graphics device available\n");
        return results;
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 65536});
    if (!batch) {
        std::printf("quads: skipped, %s\n", batch.error().to_string().c_str());
        return results;
    }

    // 10k and 100k: the scales docs/PERFORMANCE.md names for renderer submission.
    for (const std::size_t count : {std::size_t{10000}, std::size_t{100000}}) {
        const auto quads = make_quads(count);

        // A dry run first, so a machine that cannot present is skipped rather than
        // recording a meaningless number.
        if (render_once(*harness, *batch, quads) == 0) {
            std::printf("quads: skipped %zu, no swapchain image available\n", count);
            continue;
        }

        auto result = atlas::bench::measure_reported(
            "renderer/quad_batch_submit", std::format("{}k quads", count / 1000), 200, 20,
            [&] { return render_once(*harness, *batch, quads); });
        result.units_per_iteration = count;
        result.unit_name = "quads";
        results.push_back(std::move(result));
    }

    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("quads", run);

}  // namespace
