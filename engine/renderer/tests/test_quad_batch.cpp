// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/math/camera.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

using atlas::math::Mat4;
using atlas::math::Rect;
using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::renderer::Quad;
using atlas::renderer::QuadBatch;
using atlas::rhi::Device;

namespace {

/// A device, a window, and the texture and sampler the batch draws with.
///
/// The texture and sampler are released in the destructor rather than left to the device.
/// The device would clean them up, but it would also report them as leaked, and a leak
/// report that is routinely noisy is a leak report nobody reads.
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

/// A device, plus a one-pixel white texture to draw with.
[[nodiscard]] std::optional<Harness> make_harness() {
    auto platform = Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    auto window = platform->create_window(
        {.title = "Atlas batch test", .width = 128, .height = 128, .resizable = false});
    if (!window) {
        return std::nullopt;
    }
    auto device = Device::create({.debug = true}, *window);
    if (!device) {
        return std::nullopt;
    }

    auto texture = device->create_texture({.width = 1,
                                           .height = 1,
                                           .format = atlas::rhi::TextureFormat::Rgba8Unorm,
                                           .debug_name = "white"});
    if (!texture) {
        return std::nullopt;
    }
    const std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255},
                                         std::byte{255}};
    if (!device->upload_texture(*texture, white)) {
        return std::nullopt;
    }

    auto sampler = device->create_sampler({.debug_name = "test sampler"});
    if (!sampler) {
        return std::nullopt;
    }

    return std::make_optional<Harness>(std::move(*platform), std::move(*window), std::move(*device),
                                       *texture, *sampler);
}

/// Run one frame, batching through `record`, and return the statistics.
[[nodiscard]] std::optional<atlas::renderer::BatchStats>
run_frame(Harness& harness, QuadBatch& batch, const std::function<void(QuadBatch&)>& record) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        auto frame = harness.device.begin_frame();
        if (!frame) {
            return std::nullopt;
        }
        if (!frame->has_swapchain_target()) {
            (void)harness.device.end_frame(std::move(*frame));
            continue;
        }

        auto pass = frame->begin_render_pass({});
        if (!pass) {
            (void)harness.device.end_frame(std::move(*frame));
            return std::nullopt;
        }

        batch.begin(*pass, Mat4::identity());
        batch.set_texture(harness.texture, harness.sampler);
        record(batch);
        const auto stats = batch.end();

        pass->end();
        if (!harness.device.end_frame(std::move(*frame))) {
            return std::nullopt;
        }
        return stats;
    }
    return std::nullopt;
}

[[nodiscard]] Quad quad_at(float x, float y) {
    return Quad{.bounds = Rect{.position = {x, y}, .size = {1.0F, 1.0F}}};
}

}  // namespace

TEST_CASE("a batch is created and reports its capacity", "[renderer][batch][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 256});
    REQUIRE(batch.has_value());
    CHECK(batch->capacity() == 256);
    CHECK(batch->pending() == 0);
}

TEST_CASE("a batch with no capacity is refused", "[renderer][batch][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    const auto batch = QuadBatch::create(harness->device, {.capacity = 0});
    REQUIRE_FALSE(batch.has_value());
    CHECK(batch.error().code() == atlas::ErrorCode::InvalidArgument);
}

TEST_CASE("many quads become one draw call", "[renderer][batch][gpu]") {
    // The entire point of batching. A thousand quads that produced a thousand draws would be
    // a batcher in name only.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 4096});
    REQUIRE(batch.has_value());

    const auto stats = run_frame(*harness, *batch, [](QuadBatch& b) {
        for (int i = 0; i < 1000; ++i) {
            b.add(quad_at(static_cast<float>(i), 0.0F));
        }
    });
    if (!stats) {
        SKIP("no swapchain image was available");
    }

    CHECK(stats->quads == 1000);
    CHECK(stats->draw_calls == 1);
    CHECK(stats->capacity_flushes == 0);
}

TEST_CASE("exceeding the capacity splits into more draws", "[renderer][batch][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 100});
    REQUIRE(batch.has_value());

    const auto stats = run_frame(*harness, *batch, [](QuadBatch& b) {
        for (int i = 0; i < 250; ++i) {
            b.add(quad_at(static_cast<float>(i), 0.0F));
        }
    });
    if (!stats) {
        SKIP("no swapchain image was available");
    }

    // 250 quads into batches of 100: two full flushes plus the remainder at end.
    CHECK(stats->quads == 250);
    CHECK(stats->draw_calls == 3);
    CHECK(stats->capacity_flushes == 2);
}

TEST_CASE("nothing queued means nothing drawn", "[renderer][batch][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 64});
    REQUIRE(batch.has_value());

    const auto stats = run_frame(*harness, *batch, [](QuadBatch&) {});
    if (!stats) {
        SKIP("no swapchain image was available");
    }

    CHECK(stats->quads == 0);
    CHECK(stats->draw_calls == 0);
}

TEST_CASE("bulk submission matches one-at-a-time", "[renderer][batch][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 512});
    REQUIRE(batch.has_value());

    std::vector<Quad> quads;
    quads.reserve(300);
    for (int i = 0; i < 300; ++i) {
        quads.push_back(quad_at(static_cast<float>(i), 1.0F));
    }

    const auto stats = run_frame(*harness, *batch, [&quads](QuadBatch& b) { b.add(quads); });
    if (!stats) {
        SKIP("no swapchain image was available");
    }

    CHECK(stats->quads == 300);
    CHECK(stats->draw_calls == 1);
}

TEST_CASE("a steady state does not grow the instance buffer", "[renderer][batch][gpu]") {
    // The claim behind "no unbounded per-frame allocation after warm-up": the vector is
    // cleared rather than freed, so its capacity survives and later frames reuse it.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 1024});
    REQUIRE(batch.has_value());

    for (int frame = 0; frame < 3; ++frame) {
        const auto stats = run_frame(*harness, *batch, [](QuadBatch& b) {
            for (int i = 0; i < 500; ++i) {
                b.add(quad_at(static_cast<float>(i), 0.0F));
            }
        });
        if (!stats) {
            SKIP("no swapchain image was available");
        }
        INFO("frame " << frame);
        CHECK(stats->quads == 500);
        CHECK(stats->draw_calls == 1);
    }

    // Nothing is left queued between frames.
    CHECK(batch->pending() == 0);
}

TEST_CASE("a quad lands where the camera says it should", "[renderer][batch][gpu]") {
    // The test that pins down the matrix layout. A shader reads a uniform matrix
    // column-major while Atlas stores row-major, and uploading the wrong one drops the
    // translation and leaves a varying w, which warps the scene into a wedge. That renders
    // something, so only checking where the pixels landed catches it.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 16});
    REQUIRE(batch.has_value());

    // A camera looking at world (1000, 500), and a quad covering exactly the left half of
    // what it can see. The offset centre is the point: with a dropped translation the quad
    // would be drawn relative to the origin instead and miss entirely.
    atlas::math::OrthoCamera camera;
    camera.set_viewport(128.0F, 128.0F);
    camera.set_centre({1000.0F, 500.0F});
    camera.set_zoom(1.0F);

    const auto visible = camera.visible_bounds();
    const Quad left_half{
        .bounds = {.position = {visible.left(), visible.top()},
                   .size = {visible.size.x * 0.5F, visible.size.y}},
        .uv = {.position = {0.0F, 0.0F}, .size = {1.0F, 1.0F}},
        .colour = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
    };

    std::optional<Device::Capture> capture;
    for (int attempt = 0; attempt < 30 && !capture; ++attempt) {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        if (!frame->has_swapchain_target()) {
            (void)harness->device.end_frame(std::move(*frame));
            continue;
        }

        camera.set_viewport(static_cast<float>(frame->swapchain_extent().width),
                            static_cast<float>(frame->swapchain_extent().height));

        harness->device.request_capture();
        auto pass = frame->begin_render_pass(
            {.colour = {.load = atlas::rhi::LoadOp::Clear,
                        .clear_colour = {.r = 0.0F, .g = 0.0F, .b = 1.0F, .a = 1.0F}}});
        REQUIRE(pass.has_value());

        // Recomputed after the viewport is known, so the quad really is the left half.
        const auto bounds = camera.visible_bounds();
        const Quad sized{.bounds = {.position = {bounds.left(), bounds.top()},
                                    .size = {bounds.size.x * 0.5F, bounds.size.y}},
                         .uv = left_half.uv,
                         .colour = left_half.colour};

        batch->begin(*pass, camera.view_projection());
        batch->set_texture(harness->texture, harness->sampler);
        batch->add(sized);
        (void)batch->end();
        pass->end();

        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
        capture = harness->device.take_capture();
    }
    if (!capture) {
        SKIP("no swapchain image was available for capture");
    }

    const bool bgra = capture->format == atlas::rhi::TextureFormat::Bgra8Unorm ||
                      capture->format == atlas::rhi::TextureFormat::Bgra8UnormSrgb;
    const auto channel = [&](std::uint32_t x, std::uint32_t y, std::size_t offset) {
        const std::size_t index = ((static_cast<std::size_t>(y) * capture->extent.width) + x) * 4;
        return static_cast<int>(static_cast<unsigned char>(capture->pixels[index + offset]));
    };
    const auto red = [&](std::uint32_t x, std::uint32_t y) { return channel(x, y, bgra ? 2 : 0); };
    const auto blue = [&](std::uint32_t x, std::uint32_t y) { return channel(x, y, bgra ? 0 : 2); };

    const std::uint32_t mid_y = capture->extent.height / 2;
    const std::uint32_t quarter_x = capture->extent.width / 4;
    const std::uint32_t three_quarter_x = (capture->extent.width * 3) / 4;

    // The left quarter is inside the quad, so red on a white texture.
    INFO("left sample r=" << red(quarter_x, mid_y) << " b=" << blue(quarter_x, mid_y));
    CHECK(red(quarter_x, mid_y) > 150);
    CHECK(blue(quarter_x, mid_y) < 100);

    // The right quarter is outside it, so the blue clear colour shows through.
    INFO("right sample r=" << red(three_quarter_x, mid_y) << " b=" << blue(three_quarter_x, mid_y));
    CHECK(blue(three_quarter_x, mid_y) > 150);
    CHECK(red(three_quarter_x, mid_y) < 100);
}

TEST_CASE("statistics reset each frame", "[renderer][batch][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    auto batch = QuadBatch::create(harness->device, {.capacity = 128});
    REQUIRE(batch.has_value());

    const auto first =
        run_frame(*harness, *batch, [](QuadBatch& b) { b.add(quad_at(0.0F, 0.0F)); });
    const auto second = run_frame(*harness, *batch, [](QuadBatch&) {});
    if (!first || !second) {
        SKIP("no swapchain image was available");
    }

    CHECK(first->quads == 1);
    CHECK(second->quads == 0);
}

TEST_CASE("two flushes in one frame both survive to the picture", "[renderer][batch][gpu]") {
    // The test that gates streaming uploads.
    //
    // Streaming replaces the instance buffer's contents without waiting for the graphics
    // processor. When a frame flushes twice, the second write happens before the first
    // draw has executed, so the whole approach rests on the library rotating to storage
    // nothing in flight is reading. If it does not, the first draw renders the second
    // flush's data.
    //
    // The existing capacity test would not notice: it checks the batch statistics, and the
    // counts are identical either way. Only the pixels tell the truth, so this draws two
    // groups far apart in different colours, with a capacity that forces a flush between
    // them, and requires both to be there.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    // Offscreen, so this needs no swapchain image and cannot flake on the window system.
    constexpr std::uint32_t kSize = 128;
    const auto target = harness->device.create_texture({
        .width = kSize,
        .height = kSize,
        // The swapchain's format, because the batch's pipeline was built for that one and a
        // pipeline must match its target. Asking the device rather than naming a format is
        // what makes this test work on any backend.
        .format = harness->device.swapchain_format(),
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "two flushes",
    });
    REQUIRE(target.has_value());

    // A capacity of one forces a flush between the two quads.
    auto batch = QuadBatch::create(harness->device, {.capacity = 1});
    REQUIRE(batch.has_value());

    atlas::math::OrthoCamera camera;
    camera.set_viewport(static_cast<float>(kSize), static_cast<float>(kSize));
    camera.set_centre({0.0F, 0.0F});
    camera.set_zoom(1.0F);

    const auto visible = camera.visible_bounds();
    const float half = visible.size.x * 0.5F;

    // Red on the left, green on the right. Different colours so a swap is visible, and far
    // apart so neither can be mistaken for the other.
    const Quad left{
        .bounds = {.position = {visible.left(), visible.top()}, .size = {half, visible.size.y}},
        .uv = {.position = {0.0F, 0.0F}, .size = {1.0F, 1.0F}},
        .colour = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
    };
    const Quad right{
        .bounds = {.position = {visible.left() + half, visible.top()},
                   .size = {half, visible.size.y}},
        .uv = {.position = {0.0F, 0.0F}, .size = {1.0F, 1.0F}},
        .colour = {.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F},
    };

    {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        auto pass = frame->begin_render_pass({
            .colour = {.texture = *target,
                       .load = atlas::rhi::LoadOp::Clear,
                       .clear_colour = {.r = 0.0F, .g = 0.0F, .b = 1.0F, .a = 1.0F}},
        });
        REQUIRE(pass.has_value());

        batch->begin(*pass, camera.view_projection());
        batch->set_texture(harness->texture, harness->sampler);
        batch->add(left);
        batch->add(right);  // capacity 1, so adding this flushes the first
        const auto stats = batch->end();
        pass->end();

        CHECK(stats.quads == 2);
        CHECK(stats.capacity_flushes >= 1);

        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());
    }

    auto ticket = harness->device.request_readback(
        *target, atlas::rhi::Rect2D{.x = 0, .y = 0, .extent = {kSize, kSize}});
    REQUIRE(ticket.has_value());
    REQUIRE(harness->device.wait_idle().has_value());
    const auto pixels = harness->device.take_readback(*ticket);
    REQUIRE(pixels.has_value());

    // The channel order depends on the format, which is the swapchain's here.
    const bool bgra = pixels->format == atlas::rhi::TextureFormat::Bgra8Unorm ||
                      pixels->format == atlas::rhi::TextureFormat::Bgra8UnormSrgb;
    const auto channel = [&](std::uint32_t x, std::uint32_t y, std::size_t offset) {
        const std::size_t index = ((static_cast<std::size_t>(y) * kSize) + x) * 4;
        return std::to_integer<int>(pixels->pixels[index + offset]);
    };
    const auto red = [&](std::uint32_t x, std::uint32_t y) { return channel(x, y, bgra ? 2 : 0); };
    const auto green = [&](std::uint32_t x, std::uint32_t y) { return channel(x, y, 1); };

    const std::uint32_t mid_y = kSize / 2;
    const std::uint32_t left_x = kSize / 4;
    const std::uint32_t right_x = (kSize * 3) / 4;

    INFO("left  red=" << red(left_x, mid_y) << " green=" << green(left_x, mid_y));
    INFO("right red=" << red(right_x, mid_y) << " green=" << green(right_x, mid_y));

    // Both groups present, each its own colour. If the second flush had overwritten the
    // first before it drew, the left half would be the clear colour instead.
    CHECK(red(left_x, mid_y) > 200);
    CHECK(green(left_x, mid_y) < 80);
    CHECK(red(right_x, mid_y) < 80);
    CHECK(green(right_x, mid_y) > 200);

    // The batch holds graphics resources the device owns, so it goes before the texture.
    *batch = QuadBatch{};
    harness->device.destroy_texture(*target);
}

TEST_CASE("a rotated quad covers what a rotation would cover", "[renderer][batch][gpu]") {
    // The existing pixel tests cannot see a rotation. Both sample two points on one scanline,
    // which a correctly rotated quad and an axis-aligned one at the same centre both cover, so
    // a shader that ignored the angle entirely would pass them.
    //
    // So this uses an oracle instead of a picture: a tall, narrow quad turned a quarter turn
    // about its centre becomes a short, wide one. The pixel to its side is then covered and the
    // pixel above it is not, and without the turn it is exactly the other way round. Both halves
    // are asserted, because only checking the covered one would pass for a quad that had simply
    // grown.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available on this machine");
    }

    constexpr std::uint32_t kSize = 128;
    const auto target = harness->device.create_texture({
        .width = kSize,
        .height = kSize,
        .format = harness->device.swapchain_format(),
        .usage = {.sampled = false, .colour_target = true},
        .debug_name = "rotated quad",
    });
    REQUIRE(target.has_value());

    auto batch = QuadBatch::create(harness->device, {});
    REQUIRE(batch.has_value());

    atlas::math::OrthoCamera camera;
    camera.set_viewport(static_cast<float>(kSize), static_cast<float>(kSize));
    camera.set_centre({0.0F, 0.0F});
    camera.set_zoom(1.0F);

    const auto visible = camera.visible_bounds();
    // A quarter of the view tall and a sixteenth wide, centred. Narrow enough that the sideways
    // pixel is outside it unturned, and tall enough that the upward one is inside.
    const float width = visible.size.x / 16.0F;
    const float height = visible.size.y / 4.0F;

    const auto draw = [&](float rotation) {
        auto frame = harness->device.begin_frame();
        REQUIRE(frame.has_value());
        auto pass = frame->begin_render_pass({
            .colour = {.texture = *target,
                       .load = atlas::rhi::LoadOp::Clear,
                       .clear_colour = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}},
        });
        REQUIRE(pass.has_value());

        batch->begin(*pass, camera.view_projection());
        batch->set_texture(harness->texture, harness->sampler);
        batch->add(Quad{
            .bounds = {.position = {-width * 0.5F, -height * 0.5F}, .size = {width, height}},
            .uv = {.position = {0.0F, 0.0F}, .size = {1.0F, 1.0F}},
            .colour = {.r = 1.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
            .rotation = rotation,
        });
        (void)batch->end();
        pass->end();
        REQUIRE(harness->device.end_frame(std::move(*frame)).has_value());

        auto ticket = harness->device.request_readback(
            *target, atlas::rhi::Rect2D{.x = 0, .y = 0, .extent = {kSize, kSize}});
        REQUIRE(ticket.has_value());
        REQUIRE(harness->device.wait_idle().has_value());
        auto pixels = harness->device.take_readback(*ticket);
        REQUIRE(pixels.has_value());
        return std::move(*pixels);
    };

    const auto red_at = [&](const auto& pixels, std::uint32_t x, std::uint32_t y) {
        const bool bgra = pixels.format == atlas::rhi::TextureFormat::Bgra8Unorm ||
                          pixels.format == atlas::rhi::TextureFormat::Bgra8UnormSrgb;
        const std::size_t index = ((static_cast<std::size_t>(y) * kSize) + x) * 4;
        return std::to_integer<int>(pixels.pixels[index + (bgra ? 2 : 0)]);
    };

    // The sample distance has to sit between the quad's two half-extents, or neither reading
    // discriminates. At this zoom a world unit is a pixel: the quad is eight wide and
    // thirty-two tall, so its half-extents are four and sixteen, and ten is the only order of
    // magnitude that is inside one and outside the other. A first attempt sampled at
    // twenty-five, which is outside both, and every reading came back as the clear colour.
    constexpr std::uint32_t kOffset = 10;
    static_assert(kOffset > (kSize / 16) / 2, "the sample must fall outside the narrow extent");
    static_assert(kOffset < (kSize / 4) / 2, "and inside the tall one");

    const std::uint32_t centre = kSize / 2;
    const std::uint32_t beside = centre + kOffset;
    const std::uint32_t above = centre - kOffset;

    const auto upright = draw(0.0F);
    INFO("upright beside=" << red_at(upright, beside, centre)
                           << " above=" << red_at(upright, centre, above));
    // Tall and narrow: the pixel above the centre is inside it, the one beside is not.
    CHECK(red_at(upright, centre, above) > 200);
    CHECK(red_at(upright, beside, centre) < 60);

    const auto turned = draw(std::numbers::pi_v<float> / 2.0F);
    INFO("turned beside=" << red_at(turned, beside, centre)
                          << " above=" << red_at(turned, centre, above));
    // A quarter turn swaps them. Both halves matter: a quad that had merely grown would cover
    // the sideways pixel too, and only the second assertion tells the two apart.
    CHECK(red_at(turned, beside, centre) > 200);
    CHECK(red_at(turned, centre, above) < 60);

    *batch = QuadBatch{};
    harness->device.destroy_texture(*target);
}
