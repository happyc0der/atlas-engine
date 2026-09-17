// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The sandbox's demonstration scene.
///
/// A field of coloured quads with a camera over it, which is what M3 exists to show: an
/// orthographic camera, a texture, and thousands of rectangles in one draw call. It is not
/// a map and it is not a game; the quads carry no meaning beyond being many.

#include <atlas/assets/registry.hpp>
#include <atlas/core/result.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/renderer/texture_cache.hpp>
#include <atlas/rhi/device.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace atlas::sandbox {

class DemoScene {
  public:
    struct Config {
        /// Quads along each axis. The default is a field of ten thousand.
        std::uint32_t grid_width = 100;
        std::uint32_t grid_height = 100;
        std::string_view shader_directory = "assets/cooked/shaders";
        /// The texture to draw the field with, as a virtual path.
        std::string_view texture_path = "textures/tile.png";
    };

    /// The registry must outlive the scene: the scene keeps an identifier and asks for the
    /// texture each frame rather than holding one, so that a reload is picked up.
    [[nodiscard]] static Result<DemoScene> create(rhi::Device& device, assets::Registry& registry,
                                                  const Config& config);

    ~DemoScene();

    DemoScene(const DemoScene&) = delete;
    DemoScene& operator=(const DemoScene&) = delete;
    DemoScene(DemoScene&& other) noexcept;
    DemoScene& operator=(DemoScene&& other) noexcept;

    /// Apply input to the camera: drag to pan, wheel to zoom.
    /// Pan and zoom from the pointer.
    ///
    /// `display_scale` converts the platform's logical pointer coordinates into the pixels
    /// the camera's viewport is measured in. They differ on a high-density display, where
    /// omitting it pans at half speed and anchors a zoom to the wrong point.
    void update(const platform::InputState& input, std::span<const platform::Event> events,
                float display_scale);

    void resize(std::uint32_t pixel_width, std::uint32_t pixel_height);

    /// Create graphics resources for anything the registry has finished decoding.
    ///
    /// Main thread only, once per frame, before drawing.
    std::size_t finalise_assets(assets::Registry& registry);

    /// Draw the visible quads, and return what that cost.
    [[nodiscard]] renderer::BatchStats draw(rhi::RenderPass& pass);

    /// Whether the field is currently drawn with the fallback rather than its real texture.
    [[nodiscard]] bool using_fallback() const noexcept;

    [[nodiscard]] math::OrthoCamera& camera() noexcept { return m_camera; }

    [[nodiscard]] std::size_t quad_count() const noexcept { return m_quads.size(); }

    /// Quads submitted last frame after culling.
    [[nodiscard]] std::uint32_t visible_last_frame() const noexcept { return m_visible; }

  private:
    DemoScene() = default;
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    renderer::TextureCache m_textures;
    renderer::QuadBatch m_batch;
    assets::AssetId m_texture_id;

    math::OrthoCamera m_camera;
    std::vector<renderer::Quad> m_quads;
    std::uint32_t m_visible = 0;
    bool m_dragging = false;
};

}  // namespace atlas::sandbox
