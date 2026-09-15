// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene.hpp"

#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>

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

/// A colour that varies smoothly across the field, so the grid is legible.
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

Result<DemoScene> DemoScene::create(rhi::Device& device, assets::Registry& registry,
                                    const Config& config) {
    if (config.grid_width == 0 || config.grid_height == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the demonstration grid needs a non-zero size"));
    }

    DemoScene scene;
    scene.m_device = &device;

    auto textures = renderer::TextureCache::create(device);
    if (!textures) {
        return std::unexpected(std::move(textures).error().context("creating the texture cache"));
    }
    scene.m_textures = std::move(*textures);

    // Requested, not awaited. The identifier comes back immediately and the pixels arrive
    // later; until they do the field draws with the fallback and the frame still happens.
    auto path = assets::VirtualPath::parse(config.texture_path);
    if (!path) {
        return std::unexpected(std::move(path).error().context("the scene texture path"));
    }
    auto texture_id = registry.request(*path, assets::AssetType::Texture);
    if (!texture_id) {
        return std::unexpected(
            std::move(texture_id).error().context("requesting the scene texture"));
    }
    scene.m_texture_id = *texture_id;

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

    ATLAS_LOG_INFO(kApp, "demo scene ready: {} quads in a {}x{} grid, texture '{}'", count,
                   config.grid_width, config.grid_height, config.texture_path);
    return scene;
}

void DemoScene::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    // Both hold graphics resources the device owns, so both go before the device does.
    m_batch = renderer::QuadBatch{};
    m_textures = renderer::TextureCache{};
    m_device = nullptr;
}

DemoScene::~DemoScene() {
    release();
}

DemoScene::DemoScene(DemoScene&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_textures(std::move(other.m_textures)),
      m_batch(std::move(other.m_batch)), m_texture_id(std::exchange(other.m_texture_id, {})),
      m_camera(other.m_camera), m_quads(std::move(other.m_quads)), m_visible(other.m_visible),
      m_dragging(other.m_dragging) {}

DemoScene& DemoScene::operator=(DemoScene&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_textures = std::move(other.m_textures);
        m_batch = std::move(other.m_batch);
        m_texture_id = std::exchange(other.m_texture_id, {});
        m_camera = other.m_camera;
        m_quads = std::move(other.m_quads);
        m_visible = other.m_visible;
        m_dragging = other.m_dragging;
    }
    return *this;
}

std::size_t DemoScene::finalise_assets(assets::Registry& registry) {
    return m_textures.finalise_pending(registry);
}

bool DemoScene::using_fallback() const noexcept {
    return m_textures.texture_for(m_texture_id) == m_textures.fallback();
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
        // module does not depend on math and should not. Converting here is the cost of that
        // boundary, and it is one line.
        const auto pointer = input.mouse_position();
        m_camera.zoom_about(factor, math::Vec2{pointer.x, pointer.y});
    }
}

renderer::BatchStats DemoScene::draw(rhi::RenderPass& pass) {
    ATLAS_ZONE_NAMED("scene draw");

    m_batch.begin(pass, m_camera.view_projection());

    // Asked for every frame rather than held: a reload replaces the graphics texture, and a
    // cached handle would go on drawing the old one.
    m_batch.set_texture(m_textures.texture_for(m_texture_id), m_textures.sampler());

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
