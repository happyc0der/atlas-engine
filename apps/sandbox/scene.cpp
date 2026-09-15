// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene.hpp"

#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <variant>

namespace atlas::sandbox {
namespace {

constexpr log::Category kApp = log::category::kApp;

/// World units per quad, including the gap between them.
constexpr float kCellSize = 12.0F;
constexpr float kCellGap = 2.0F;

/// A small checkerboard, so that texture sampling is visibly doing something. A flat white
/// texture would look identical whether or not the sampler worked.
constexpr std::uint32_t kTextureSize = 8;

[[nodiscard]] std::vector<std::byte> make_checker_texture() {
    std::vector<std::byte> pixels(static_cast<std::size_t>(kTextureSize) * kTextureSize * 4);
    for (std::uint32_t y = 0; y < kTextureSize; ++y) {
        for (std::uint32_t x = 0; x < kTextureSize; ++x) {
            const bool light = ((x / 2) + (y / 2)) % 2 == 0;
            const auto value = static_cast<unsigned char>(light ? 255 : 170);
            const std::size_t index = ((static_cast<std::size_t>(y) * kTextureSize) + x) * 4;
            pixels[index + 0] = std::byte{value};
            pixels[index + 1] = std::byte{value};
            pixels[index + 2] = std::byte{value};
            pixels[index + 3] = std::byte{255};
        }
    }
    return pixels;
}

/// A colour that varies smoothly across the field, so that the grid is legible.
[[nodiscard]] rhi::Colour cell_colour(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                      std::uint32_t height) {
    const float u = static_cast<float>(x) / static_cast<float>(width > 1 ? width - 1 : 1);
    const float v = static_cast<float>(y) / static_cast<float>(height > 1 ? height - 1 : 1);
    return rhi::Colour{
        .r = 0.25F + (0.65F * u),
        .g = 0.30F + (0.55F * v),
        .b = 0.85F - (0.55F * u * v),
        .a = 1.0F,
    };
}

}  // namespace

Result<DemoScene> DemoScene::create(rhi::Device& device, const Config& config) {
    if (config.grid_width == 0 || config.grid_height == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the demonstration grid needs a non-zero size"));
    }

    DemoScene scene;
    scene.m_device = &device;

    auto texture = device.create_texture({
        .width = kTextureSize,
        .height = kTextureSize,
        .format = rhi::TextureFormat::Rgba8Unorm,
        .debug_name = "checker",
    });
    if (!texture) {
        return std::unexpected(std::move(texture).error().context("creating the demo texture"));
    }
    scene.m_texture = *texture;

    const auto pixels = make_checker_texture();
    if (auto status = device.upload_texture(scene.m_texture, pixels); !status) {
        return std::unexpected(std::move(status).error().context("uploading the demo texture"));
    }

    // Nearest filtering: the texture is eight pixels across and meant to be seen as squares,
    // and linear sampling would blur it into a smear at any size.
    auto sampler = device.create_sampler({
        .min_filter = rhi::Filter::Nearest,
        .mag_filter = rhi::Filter::Nearest,
        .debug_name = "checker sampler",
    });
    if (!sampler) {
        return std::unexpected(std::move(sampler).error().context("creating the demo sampler"));
    }
    scene.m_sampler = *sampler;

    auto batch =
        renderer::QuadBatch::create(device, {.capacity = renderer::QuadBatch::kDefaultCapacity,
                                             .shader_directory = config.shader_directory});
    if (!batch) {
        return std::unexpected(std::move(batch).error());
    }
    scene.m_batch = std::move(*batch);

    const std::size_t count = static_cast<std::size_t>(config.grid_width) * config.grid_height;
    scene.m_quads.reserve(count);

    for (std::uint32_t y = 0; y < config.grid_height; ++y) {
        for (std::uint32_t x = 0; x < config.grid_width; ++x) {
            scene.m_quads.push_back(renderer::Quad{
                .bounds = {.position = {static_cast<float>(x) * (kCellSize + kCellGap),
                                        static_cast<float>(y) * (kCellSize + kCellGap)},
                           .size = {kCellSize, kCellSize}},
                .uv = {.position = {0.0F, 0.0F}, .size = {1.0F, 1.0F}},
                .colour = cell_colour(x, y, config.grid_width, config.grid_height),
            });
        }
    }

    // Start looking at the middle of the field, zoomed out enough to see a good deal of it.
    const float span_x = static_cast<float>(config.grid_width) * (kCellSize + kCellGap);
    const float span_y = static_cast<float>(config.grid_height) * (kCellSize + kCellGap);
    scene.m_camera.set_centre({span_x * 0.5F, span_y * 0.5F});
    scene.m_camera.set_zoom(0.5F);

    ATLAS_LOG_INFO(kApp, "demo scene ready: {} quads in a {}x{} grid", count, config.grid_width,
                   config.grid_height);
    return scene;
}

void DemoScene::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    // The batch owns graphics resources too, so it goes before the device's are released.
    m_batch = renderer::QuadBatch{};
    m_device->destroy_sampler(m_sampler);
    m_device->destroy_texture(m_texture);
    m_device = nullptr;
}

DemoScene::~DemoScene() {
    release();
}

DemoScene::DemoScene(DemoScene&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)),
      m_texture(std::exchange(other.m_texture, {})), m_sampler(std::exchange(other.m_sampler, {})),
      m_batch(std::move(other.m_batch)), m_camera(other.m_camera),
      m_quads(std::move(other.m_quads)), m_visible(other.m_visible), m_dragging(other.m_dragging) {}

DemoScene& DemoScene::operator=(DemoScene&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_texture = std::exchange(other.m_texture, {});
        m_sampler = std::exchange(other.m_sampler, {});
        m_batch = std::move(other.m_batch);
        m_camera = other.m_camera;
        m_quads = std::move(other.m_quads);
        m_visible = other.m_visible;
        m_dragging = other.m_dragging;
    }
    return *this;
}

void DemoScene::resize(std::uint32_t pixel_width, std::uint32_t pixel_height) {
    m_camera.set_viewport(static_cast<float>(pixel_width), static_cast<float>(pixel_height));
}

void DemoScene::update(const platform::InputState& input, std::span<const platform::Event> events) {
    ATLAS_ZONE_NAMED("scene update");

    if (input.was_pressed(platform::MouseButton::Left)) {
        m_dragging = true;
    }
    if (input.was_released(platform::MouseButton::Left)) {
        m_dragging = false;
    }

    // Dragging moves the world with the pointer, so the delta is divided by the zoom and
    // subtracted: the camera goes the other way from the content.
    if (m_dragging) {
        const float zoom = m_camera.zoom();
        m_camera.pan({-input.mouse_delta_x() / zoom, -input.mouse_delta_y() / zoom});
    }

    for (const auto& event : events) {
        const auto* wheel = std::get_if<platform::MouseWheel>(&event);
        if (wheel == nullptr || wheel->delta_y == 0.0F) {
            continue;
        }

        // Multiplicative, so each notch changes the view by the same proportion however far
        // in or out it already is.
        const float factor = std::pow(1.15F, wheel->delta_y);

        // The platform reports a pointer position in its own type, because the platform
        // module does not depend on math and should not. Converting here is the cost of
        // that boundary, and it is one line.
        const auto pointer = input.mouse_position();
        m_camera.zoom_about(factor, math::Vec2{pointer.x, pointer.y});
    }
}

renderer::BatchStats DemoScene::draw(rhi::RenderPass& pass) {
    ATLAS_ZONE_NAMED("scene draw");

    m_batch.begin(pass, m_camera.view_projection());
    m_batch.set_texture(m_texture, m_sampler);

    // Cull against the visible rectangle before submitting. Without this, panning far away
    // would still upload and draw every quad in the field, and the cost of a scene would be
    // its size rather than what is on screen.
    const math::Rect visible = m_camera.visible_bounds();
    m_visible = 0;
    for (const auto& quad : m_quads) {
        if (quad.bounds.overlaps(visible)) {
            m_batch.add(quad);
            ++m_visible;
        }
    }

    return m_batch.end();
}

}  // namespace atlas::sandbox
