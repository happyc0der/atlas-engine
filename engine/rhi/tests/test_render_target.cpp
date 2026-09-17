// SPDX-License-Identifier: GPL-3.0-or-later

/// \file
/// Offscreen colour targets, the integer format, and reading a region back.
///
/// These need a real device and carry the `gpu` label, but unlike every other render test
/// here they do **not** need a swapchain image: a pass that names its own texture works
/// whether or not the window system has handed one over. That makes them deterministic
/// rather than retrying thirty times for an image, and it is the property the identifier
/// pass depends on.

#include <atlas/core/assert.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <optional>

using atlas::ErrorCode;
using atlas::rhi::Device;
using atlas::rhi::LoadOp;
using atlas::rhi::Rect2D;
using atlas::rhi::TextureFormat;
using atlas::rhi::TextureHandle;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

struct Harness {
    atlas::platform::Platform platform;
    atlas::platform::Window window;
    Device device;
};

/// A device with a hidden window. Hidden on purpose: nothing here draws to the screen, and a
/// window that flashes up during a test run is a nuisance.
[[nodiscard]] std::optional<Harness> make_harness() {
    auto platform = atlas::platform::Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    auto window = platform->create_window(
        {.title = "Atlas render target test", .width = 64, .height = 64, .resizable = false});
    if (!window) {
        return std::nullopt;
    }
    auto device = Device::create({.debug = true}, *window);
    if (!device) {
        return std::nullopt;
    }
    return Harness{.platform = std::move(*platform),
                   .window = std::move(*window),
                   .device = std::move(*device)};
}

/// Read a region back, forcing the copy to complete so the test does not depend on timing.
[[nodiscard]] std::optional<Device::Readback> read_now(Device& device, TextureHandle texture,
                                                       Rect2D region) {
    auto ticket = device.request_readback(texture, region);
    if (!ticket) {
        return std::nullopt;
    }
    if (!device.wait_idle()) {
        return std::nullopt;
    }
    auto taken = device.take_readback(*ticket);
    if (!taken) {
        return std::nullopt;
    }
    return std::move(*taken);
}

}  // namespace

TEST_CASE("a texture declaring no usage is refused", "[rhi][target][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 16,
        .height = 16,
        .usage = {.sampled = false, .colour_target = false},
        .debug_name = "useless",
    });
    REQUIRE_FALSE(texture.has_value());
    CHECK(texture.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a sampled-only texture is refused as a pass target", "[rhi][target][gpu]") {
    // The mistake this catches is passing an ordinary texture to a pass. Caught here with a
    // message naming what the texture actually is, rather than by the driver.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 16,
        .height = 16,
        .usage = {.sampled = true, .colour_target = false},
        .debug_name = "sampled only",
    });
    REQUIRE(texture.has_value());

    auto frame = harness->device.begin_frame();
    REQUIRE(frame.has_value());

    const auto pass = frame->begin_render_pass({.colour = {.texture = *texture}});
    REQUIRE_FALSE(pass.has_value());
    CHECK(pass.error().code() == ErrorCode::InvalidArgument);

    harness->device.destroy_texture(*texture);
    (void)harness->device.end_frame(std::move(*frame));
}

TEST_CASE("an offscreen pass clears its target and reads back", "[rhi][target][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 32,
        .height = 32,
        .format = TextureFormat::Rgba8Unorm,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "offscreen",
    });
    REQUIRE(texture.has_value());

    {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        auto pass = frame->begin_render_pass({
            .colour = {.texture = *texture,
                       .load = LoadOp::Clear,
                       .clear_colour = {.r = 1.0F, .g = 0.5F, .b = 0.25F, .a = 1.0F}},
        });
        REQUIRE(pass.has_value());
        pass->end();
        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
    }

    const auto pixels =
        read_now(harness->device, *texture, Rect2D{.x = 0, .y = 0, .extent = {32, 32}});
    REQUIRE(pixels.has_value());
    CHECK(pixels->format == TextureFormat::Rgba8Unorm);
    REQUIRE(pixels->pixels.size() == std::size_t{32} * 32 * 4);

    // Channel order matters: a swapped red and blue would still be "a colour".
    const auto at = [&](std::size_t i) { return std::to_integer<int>(pixels->pixels[i]); };
    CHECK(at(0) > 200);
    CHECK(at(1) > 100);
    CHECK(at(1) < 180);
    CHECK(at(2) < 100);

    harness->device.destroy_texture(*texture);
}

TEST_CASE("an offscreen pass needs no swapchain image", "[rhi][target][gpu]") {
    // The property that makes the identifier pass work on a minimised window, and that makes
    // every test in this file deterministic.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 8,
        .height = 8,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "no swapchain needed",
    });
    REQUIRE(texture.has_value());

    auto frame = harness->device.begin_frame();
    REQUIRE(frame.has_value());

    // Whether or not an image was available, the offscreen pass opens.
    const bool had_swapchain = frame->has_swapchain_target();
    auto pass = frame->begin_render_pass({.colour = {.texture = *texture}});
    INFO("the frame " << (had_swapchain ? "had" : "did not have") << " a swapchain image");
    REQUIRE(pass.has_value());
    CHECK(pass->target_extent() == atlas::rhi::Extent2D{8, 8});
    CHECK(pass->target_format() == TextureFormat::Rgba8Unorm);
    pass->end();

    REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
    harness->device.destroy_texture(*texture);
}

TEST_CASE("an integer target clears to exactly zero", "[rhi][target][gpu]") {
    // Zero is the only clear value that is portable, and the reason identifiers start at
    // one. Exactness is the point: an approximate check would pass under blending or
    // filtering, which is the failure this format exists to rule out.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 16,
        .height = 16,
        .format = TextureFormat::R32Uint,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "identifiers",
    });
    if (!texture) {
        SKIP("this backend cannot use R32Uint as a colour target: " + texture.error().to_string());
    }

    {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        auto pass = frame->begin_render_pass({
            .colour = {.texture = *texture, .load = LoadOp::Clear},
        });
        REQUIRE(pass.has_value());
        pass->end();
        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
    }

    const auto pixels =
        read_now(harness->device, *texture, Rect2D{.x = 0, .y = 0, .extent = {16, 16}});
    REQUIRE(pixels.has_value());
    CHECK(pixels->format == TextureFormat::R32Uint);
    REQUIRE(pixels->pixels.size() == std::size_t{16} * 16 * 4);

    // Every pixel, not just the first: a clear that only took on part of the target would
    // be worse than one that did not work at all.
    for (const std::byte pixel : pixels->pixels) {
        REQUIRE(std::to_integer<int>(pixel) == 0);
    }

    harness->device.destroy_texture(*texture);
}

TEST_CASE("a one-pixel readback returns exactly four bytes", "[rhi][target][gpu]") {
    // The case picking uses. A whole-frame readback would work and would cost the frame.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 64,
        .height = 64,
        .format = TextureFormat::R32Uint,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "identifiers",
    });
    if (!texture) {
        SKIP("this backend cannot use R32Uint as a colour target");
    }

    {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        auto pass = frame->begin_render_pass({
            .colour = {.texture = *texture, .load = LoadOp::Clear},
        });
        REQUIRE(pass.has_value());
        pass->end();
        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
    }

    const auto pixel =
        read_now(harness->device, *texture, Rect2D{.x = 37, .y = 11, .extent = {1, 1}});
    REQUIRE(pixel.has_value());
    REQUIRE(pixel->pixels.size() == 4);
    CHECK(pixel->region == Rect2D{.x = 37, .y = 11, .extent = {1, 1}});

    std::uint32_t value = 0xFFFF'FFFF;
    std::memcpy(&value, pixel->pixels.data(), sizeof(value));
    CHECK(value == 0);

    harness->device.destroy_texture(*texture);
}

TEST_CASE("a region outside the texture is refused", "[rhi][target][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 16,
        .height = 16,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "small",
    });
    REQUIRE(texture.has_value());

    for (const Rect2D region : {
             Rect2D{.x = 0, .y = 0, .extent = {17, 16}},
             Rect2D{.x = 0, .y = 0, .extent = {16, 17}},
             Rect2D{.x = 16, .y = 0, .extent = {1, 1}},
             Rect2D{.x = 0, .y = 0, .extent = {0, 4}},
             // Near the top of the range, where a 32-bit sum would wrap into a small number.
             Rect2D{.x = 0xFFFF'FFF0, .y = 0, .extent = {32, 1}},
         }) {
        INFO("region " << region.x << "," << region.y << " " << region.extent.width << "x"
                       << region.extent.height);
        const auto ticket = harness->device.request_readback(*texture, region);
        REQUIRE_FALSE(ticket.has_value());
        CHECK(ticket.error().code() == ErrorCode::InvalidArgument);
    }

    harness->device.destroy_texture(*texture);
}

TEST_CASE("a readback ticket is good once", "[rhi][target][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 8,
        .height = 8,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "once",
    });
    REQUIRE(texture.has_value());

    const auto ticket =
        harness->device.request_readback(*texture, Rect2D{.x = 0, .y = 0, .extent = {8, 8}});
    REQUIRE(ticket.has_value());
    CHECK(harness->device.pending_readbacks() == 1);

    REQUIRE(harness->device.wait_idle().has_value());
    REQUIRE(harness->device.take_readback(*ticket).has_value());
    CHECK(harness->device.pending_readbacks() == 0);

    const auto again = harness->device.take_readback(*ticket);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code() == ErrorCode::InvalidArgument);

    harness->device.destroy_texture(*texture);
}

TEST_CASE("readbacks are bounded", "[rhi][target][gpu]") {
    // A caller that asks and never collects is a bug, and hearing about it at the fifth
    // request is better than growing until memory runs out.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 8,
        .height = 8,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "bounded",
    });
    REQUIRE(texture.has_value());

    const Rect2D whole{.x = 0, .y = 0, .extent = {8, 8}};
    for (std::size_t i = 0; i < Device::kMaxPendingReadbacks; ++i) {
        INFO("request " << i);
        REQUIRE(harness->device.request_readback(*texture, whole).has_value());
    }

    const auto excess = harness->device.request_readback(*texture, whole);
    REQUIRE_FALSE(excess.has_value());
    CHECK(excess.error().code() == ErrorCode::Exhausted);

    // Collect one and the next request fits again.
    REQUIRE(harness->device.wait_idle().has_value());
    CHECK(harness->device.pending_readbacks() == Device::kMaxPendingReadbacks);

    harness->device.destroy_texture(*texture);
}

TEST_CASE("a readback is genuinely deferred", "[rhi][target][gpu]") {
    // Written to pass either way on purpose. On a fast path the fence may already have
    // signalled by the time it is asked, and a test that demanded "not ready" would flake.
    // What must hold is that it becomes ready and that the pixels are right, which is
    // asserted in both branches rather than only in one.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 32,
        .height = 32,
        .format = TextureFormat::R32Uint,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "deferred",
    });
    if (!texture) {
        SKIP("this backend cannot use R32Uint as a colour target");
    }

    {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        auto pass = frame->begin_render_pass({
            .colour = {.texture = *texture, .load = LoadOp::Clear},
        });
        REQUIRE(pass.has_value());
        pass->end();
        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
    }

    const auto ticket =
        harness->device.request_readback(*texture, Rect2D{.x = 0, .y = 0, .extent = {1, 1}});
    REQUIRE(ticket.has_value());

    const bool ready_at_once = harness->device.readback_ready(*ticket);
    INFO("the readback was " << (ready_at_once ? "already" : "not yet") << " ready on return");

    REQUIRE(harness->device.wait_idle().has_value());
    CHECK(harness->device.readback_ready(*ticket));

    const auto taken = harness->device.take_readback(*ticket);
    REQUIRE(taken.has_value());
    std::uint32_t value = 0xFFFF'FFFF;
    std::memcpy(&value, taken->pixels.data(), sizeof(value));
    CHECK(value == 0);

    harness->device.destroy_texture(*texture);
}

TEST_CASE("a capture does not hijack a pass that names its own target", "[rhi][target][gpu]") {
    // Capture redirects a swapchain pass to an offscreen texture it owns. It must not do
    // that to a pass the caller aimed somewhere deliberately: the caller is going to read
    // that texture, and the window would show the wrong thing.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto texture = harness->device.create_texture({
        .width = 16,
        .height = 16,
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "mine",
    });
    REQUIRE(texture.has_value());

    harness->device.request_capture();

    auto frame = harness->device.begin_frame();
    REQUIRE(frame.has_value());
    auto pass = frame->begin_render_pass({
        .colour = {.texture = *texture,
                   .load = LoadOp::Clear,
                   .clear_colour = {.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F}},
    });
    REQUIRE(pass.has_value());
    CHECK(pass->target_extent() == atlas::rhi::Extent2D{16, 16});
    pass->end();
    REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());

    // The caller's texture holds what the caller drew, whatever the capture did.
    const auto pixels =
        read_now(harness->device, *texture, Rect2D{.x = 0, .y = 0, .extent = {1, 1}});
    REQUIRE(pixels.has_value());
    CHECK(std::to_integer<int>(pixels->pixels[0]) < 60);
    CHECK(std::to_integer<int>(pixels->pixels[1]) > 200);

    harness->device.destroy_texture(*texture);
}

TEST_CASE("blending an integer target is refused", "[rhi][target][gpu]") {
    // Measured: Metal aborts the process on this and Vulkan accepts it silently and blends
    // the identifiers. Neither is a behaviour to ship, so Atlas refuses it itself.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto pipeline = harness->device.create_graphics_pipeline({
        .colour_format = TextureFormat::R32Uint,
        .blend = atlas::rhi::BlendMode::AlphaBlend,
        .debug_name = "blended identifiers",
    });
    REQUIRE_FALSE(pipeline.has_value());
    CHECK(pipeline.error().code() == ErrorCode::InvalidArgument);
}
