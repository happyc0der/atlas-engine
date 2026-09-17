// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>
#include <vector>

using atlas::ErrorCode;
using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::rhi::Backend;
using atlas::rhi::BufferHandle;
using atlas::rhi::Device;
using atlas::rhi::ShaderFormat;

// These need a real graphics device and are labelled "gpu" so that a hosted runner, which
// has none, excludes them. They run on the development machine and on any runner with a
// software or hardware device.

namespace {

/// A platform, a hidden window and a device, or nothing if this machine has no device.
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

    auto window = platform->create_window(
        {.title = "Atlas rhi test", .width = 256, .height = 256, .hidden = true});
    if (!window) {
        return std::nullopt;
    }

    auto device = Device::create({.debug = true}, *window);
    if (!device) {
        return std::nullopt;
    }

    return Harness{
        .platform = std::move(*platform),
        .window = std::move(*window),
        .device = std::move(*device),
    };
}

}  // namespace

TEST_CASE("a device reports a real backend and a swapchain format", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    CHECK(harness->device.valid());
    CHECK(harness->device.backend() != Backend::Unknown);
    CHECK_FALSE(harness->device.backend_name().empty());
    CHECK(harness->device.swapchain_format() != atlas::rhi::TextureFormat::Unknown);

    // Whatever the backend, it must accept at least one format Atlas can produce, or
    // nothing could ever be drawn.
    const bool any_format = harness->device.supports_shader_format(ShaderFormat::SpirV) ||
                            harness->device.supports_shader_format(ShaderFormat::Msl) ||
                            harness->device.supports_shader_format(ShaderFormat::Dxil);
    CHECK(any_format);
}

TEST_CASE("a frame can be begun and submitted", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto frame = harness->device.begin_frame();
    REQUIRE(frame.has_value());
    CHECK(frame->valid());

    const auto status = harness->device.end_frame(std::move(*frame));
    CHECK(status.has_value());
}

TEST_CASE("two frames cannot be open at once", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto first = harness->device.begin_frame();
    REQUIRE(first.has_value());

    // Two command buffers fighting over one swapchain image is a bug worth refusing rather
    // than producing whatever the driver happens to do.
    const auto second = harness->device.begin_frame();
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code() == ErrorCode::InvalidArgument);

    REQUIRE(harness->device.end_frame(std::move(*first)).has_value());
}

TEST_CASE("an abandoned frame does not wedge the device", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    {
        const auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        // Dropped without submitting, which is what happens when an error aborts a frame.
    }

    // The device must be usable again afterwards.
    auto next = harness->device.begin_frame();
    REQUIRE(next.has_value());
    CHECK(harness->device.end_frame(std::move(*next)).has_value());
}

TEST_CASE("a hidden window yields frames without a swapchain target, and that is not an error",
          "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto frame = harness->device.begin_frame();
    REQUIRE(frame.has_value());

    if (!frame->has_swapchain_target()) {
        // Beginning a pass must fail clearly rather than drawing into nothing.
        const auto pass = frame->begin_render_pass({});
        REQUIRE_FALSE(pass.has_value());
        CHECK(pass.error().code() == ErrorCode::Unavailable);
    }

    CHECK(harness->device.end_frame(std::move(*frame)).has_value());
}

TEST_CASE("buffers are created, uploaded to, and destroyed", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto buffer = harness->device.create_buffer(
        {.size = 256, .usage = atlas::rhi::BufferUsage::Vertex, .debug_name = "test buffer"});
    REQUIRE(buffer.has_value());
    CHECK(harness->device.resource_counts().buffers == 1);

    const std::vector<std::byte> data(256, std::byte{0x7F});
    CHECK(harness->device.upload_buffer(*buffer, data).has_value());

    harness->device.destroy_buffer(*buffer);
    CHECK(harness->device.resource_counts().buffers == 0);

    // Destroying twice is harmless, which is what makes cleanup code simple to write.
    harness->device.destroy_buffer(*buffer);
    CHECK(harness->device.resource_counts().buffers == 0);
}

TEST_CASE("a zero-sized buffer is refused", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto buffer = harness->device.create_buffer({.size = 0});
    REQUIRE_FALSE(buffer.has_value());
    CHECK(buffer.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an upload that does not fit is refused rather than truncated", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto buffer = harness->device.create_buffer({.size = 64, .debug_name = "small"});
    REQUIRE(buffer.has_value());

    const std::vector<std::byte> too_much(128, std::byte{0});
    const auto status = harness->device.upload_buffer(*buffer, too_much);

    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::OutOfRange);
    // The message must name the buffer, or the report is useless when there are hundreds.
    CHECK(status.error().message().contains("small"));

    harness->device.destroy_buffer(*buffer);
}

TEST_CASE("uploading through a stale handle is refused", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto buffer = harness->device.create_buffer({.size = 64});
    REQUIRE(buffer.has_value());
    harness->device.destroy_buffer(*buffer);

    const std::vector<std::byte> data(16, std::byte{0});
    const auto status = harness->device.upload_buffer(*buffer, data);

    // The generation counter is what makes this detectable instead of a write into a
    // destroyed resource.
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("creating many buffers and destroying them leaves nothing behind", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    std::vector<BufferHandle> handles;
    for (int i = 0; i < 200; ++i) {
        const auto buffer = harness->device.create_buffer({.size = 128, .debug_name = "churn"});
        REQUIRE(buffer.has_value());
        handles.push_back(*buffer);
    }
    CHECK(harness->device.resource_counts().buffers == 200);

    for (const auto handle : handles) {
        harness->device.destroy_buffer(handle);
    }

    // An empty pool at shutdown is what makes the leak report meaningful.
    CHECK(harness->device.resource_counts().total() == 0);
}

TEST_CASE("a shader in a format the backend rejects is refused with a reason",
          "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    // Find a format this backend does not accept. On Metal that is SPIR-V; on Vulkan it is
    // Metal Shading Language. If a backend somehow accepted all three there is nothing to
    // test here.
    std::optional<ShaderFormat> unsupported;
    for (const auto format : {ShaderFormat::SpirV, ShaderFormat::Msl, ShaderFormat::Dxil}) {
        if (!harness->device.supports_shader_format(format)) {
            unsupported = format;
            break;
        }
    }
    if (!unsupported) {
        SKIP("this backend accepts every shader format Atlas produces");
    }

    const std::vector<std::byte> nonsense(64, std::byte{0});
    const auto shader = harness->device.create_shader({
        .stage = atlas::rhi::ShaderStage::Vertex,
        .format = *unsupported,
        .code = nonsense,
        .entry_point = "main",
        .debug_name = "wrong format",
    });

    REQUIRE_FALSE(shader.has_value());
    CHECK(shader.error().code() == ErrorCode::NotSupported);
}

TEST_CASE("a shader with no code is refused", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto shader = harness->device.create_shader({.debug_name = "empty"});
    REQUIRE_FALSE(shader.has_value());
    CHECK(shader.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a pipeline needs live shaders", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto pipeline = harness->device.create_graphics_pipeline({.debug_name = "no shaders"});
    REQUIRE_FALSE(pipeline.has_value());
    CHECK(pipeline.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a device can be moved without destroying the underlying device twice",
          "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto backend = harness->device.backend();
    Device moved = std::move(harness->device);

    CHECK(moved.valid());
    CHECK(moved.backend() == backend);
    CHECK_FALSE(harness->device.valid());  // NOLINT(bugprone-use-after-move): the point

    // The moved-to device still works, and at scope exit only one of the two destroys
    // anything. A double destroy would surface as a crash here rather than at the mistake.
    auto frame = moved.begin_frame();
    REQUIRE(frame.has_value());
    CHECK(moved.end_frame(std::move(*frame)).has_value());
}

TEST_CASE("waiting for idle succeeds on a live device", "[rhi][device][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }
    CHECK(harness->device.wait_idle().has_value());
}
