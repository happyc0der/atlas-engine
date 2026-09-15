// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::rhi::Colour;
using atlas::rhi::Device;
using atlas::rhi::LoadOp;
using atlas::rhi::TextureFormat;

// Rendering tests that read the result back and check the pixels.
//
// Looking at a screenshot proves a triangle appeared once. Checking the pixels proves it
// still appears, on every run, without anyone having to look. The readback route these use
// is the same one that carries integer-ID picking in M7.

namespace {

struct Harness {
    Platform platform;
    Window window;
    Device device;
};

[[nodiscard]] std::optional<Harness> make_harness() {
    auto platform = Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    // Shown rather than hidden: a hidden window has no swapchain image to draw into, and
    // these tests need one. Small, so it appears and disappears without being a nuisance.
    auto window = platform->create_window(
        {.title = "Atlas render test", .width = 64, .height = 64, .resizable = false});
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

struct Pixel {
    int r = 0;
    int g = 0;
    int b = 0;
};

/// Read one pixel, reordering the channels when the swapchain is blue-green-red-alpha.
[[nodiscard]] Pixel pixel_at(const Device::Capture& capture, std::uint32_t x, std::uint32_t y) {
    const bool bgra = capture.format == TextureFormat::Bgra8Unorm ||
                      capture.format == TextureFormat::Bgra8UnormSrgb;
    const std::size_t index = ((static_cast<std::size_t>(y) * capture.extent.width) + x) * 4;

    const auto at = [&](std::size_t offset) {
        return static_cast<int>(static_cast<unsigned char>(capture.pixels[index + offset]));
    };
    return Pixel{.r = at(bgra ? 2 : 0), .g = at(1), .b = at(bgra ? 0 : 2)};
}

/// Render one frame through `record` and return the pixels.
[[nodiscard]] std::optional<Device::Capture>
render_and_capture(Harness& harness, const Colour& clear,
                   const std::function<void(atlas::rhi::RenderPass&)>& record) {
    // The first frames after a window appears may have no swapchain image yet, so this
    // tries a few times rather than failing on a timing detail of the window system.
    for (int attempt = 0; attempt < 30; ++attempt) {
        auto frame = harness.device.begin_frame();
        if (!frame) {
            return std::nullopt;
        }
        if (!frame->has_swapchain_target()) {
            (void)harness.device.end_frame(std::move(*frame));
            continue;
        }

        harness.device.request_capture();
        auto pass =
            frame->begin_render_pass({.colour = {.load = LoadOp::Clear, .clear_colour = clear}});
        if (!pass) {
            (void)harness.device.end_frame(std::move(*frame));
            return std::nullopt;
        }
        if (record) {
            record(*pass);
        }
        pass->end();

        if (!harness.device.end_frame(std::move(*frame))) {
            return std::nullopt;
        }
        return harness.device.take_capture();
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("a cleared frame reads back as the clear colour", "[rhi][render][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    // A colour whose channels are all different, so a channel ordering mistake shows up
    // rather than cancelling out.
    const Colour clear{.r = 1.0F, .g = 0.5F, .b = 0.25F, .a = 1.0F};
    const auto capture = render_and_capture(*harness, clear, nullptr);
    if (!capture) {
        SKIP("no swapchain image was available for capture");
    }

    REQUIRE(capture->extent.width > 0);
    REQUIRE(capture->pixels.size() ==
            static_cast<std::size_t>(capture->extent.width) * capture->extent.height * 4);

    const auto centre = pixel_at(*capture, capture->extent.width / 2, capture->extent.height / 2);

    // Generous bounds: the swapchain may be an sRGB format, which changes the stored values.
    // What must hold is the ordering, which is what catches a swapped red and blue.
    INFO("centre was r=" << centre.r << " g=" << centre.g << " b=" << centre.b);
    CHECK(centre.r > centre.g);
    CHECK(centre.g > centre.b);
    CHECK(centre.r > 200);
}

TEST_CASE("the clear colour actually changes what is drawn", "[rhi][render][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto dark = render_and_capture(*harness, {.r = 0.0F, .g = 0.0F, .b = 0.0F}, nullptr);
    const auto light = render_and_capture(*harness, {.r = 1.0F, .g = 1.0F, .b = 1.0F}, nullptr);
    if (!dark || !light) {
        SKIP("no swapchain image was available for capture");
    }

    const auto dark_px = pixel_at(*dark, 1, 1);
    const auto light_px = pixel_at(*light, 1, 1);

    // Without this, a capture that returned a stale or zeroed buffer would pass the test
    // above by accident.
    CHECK(dark_px.r < 40);
    CHECK(light_px.r > 200);
}

TEST_CASE("a capture is taken once and then gone", "[rhi][render][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto first = render_and_capture(*harness, {.r = 0.2F, .g = 0.4F, .b = 0.6F}, nullptr);
    if (!first) {
        SKIP("no swapchain image was available for capture");
    }

    // Taking clears it, so a caller cannot accidentally read the same frame twice and
    // believe it is looking at a newer one.
    CHECK_FALSE(harness->device.take_capture().has_value());
}

TEST_CASE("a frame with no capture requested produces none", "[rhi][render][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto frame = harness->device.begin_frame();
    REQUIRE(frame.has_value());
    REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());

    CHECK_FALSE(harness->device.take_capture().has_value());
}
