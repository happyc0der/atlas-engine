// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The sandbox's demonstration scene.
///
/// A field of coloured quads with a camera over it, which is what M3 exists to show: an
/// orthographic camera, a texture, and thousands of rectangles in one draw call. It is not
/// a map and it is not a game; the quads carry no meaning beyond being many.

#include <atlas/core/result.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
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
    };

    [[nodiscard]] static Result<DemoScene> create(rhi::Device& device, const Config& config);

    ~DemoScene();

    DemoScene(const DemoScene&) = delete;
    DemoScene& operator=(const DemoScene&) = delete;
    DemoScene(DemoScene&& other) noexcept;
    DemoScene& operator=(DemoScene&& other) noexcept;

    /// Apply input to the camera: drag to pan, wheel to zoom.
    void update(const platform::InputState& input, std::span<const platform::Event> events);

    void resize(std::uint32_t pixel_width, std::uint32_t pixel_height);

    /// Draw the visible quads, and return what that cost.
    [[nodiscard]] renderer::BatchStats draw(rhi::RenderPass& pass);

    [[nodiscard]] math::OrthoCamera& camera() noexcept { return m_camera; }

    [[nodiscard]] std::size_t quad_count() const noexcept { return m_quads.size(); }

    /// Quads submitted last frame after culling.
    [[nodiscard]] std::uint32_t visible_last_frame() const noexcept { return m_visible; }

  private:
    DemoScene() = default;
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    rhi::TextureHandle m_texture;
    rhi::SamplerHandle m_sampler;
    renderer::QuadBatch m_batch;

    math::OrthoCamera m_camera;
    std::vector<renderer::Quad> m_quads;
    std::uint32_t m_visible = 0;
    bool m_dragging = false;
};

}  // namespace atlas::sandbox
