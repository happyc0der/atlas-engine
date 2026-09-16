// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The sandbox's scene-graph demonstration.
///
/// What M5 exists to show, end to end in one run: a small hierarchy is built in code, saved
/// to text, loaded back, and drawn from the loaded copy. The original is kept only so the two
/// can be compared; the pictures on screen come from the file.
///
/// It is a handful of squares around a moving parent. It is not a map and not a game: the
/// entities carry no meaning beyond being arranged in a tree.

#include <atlas/assets/registry.hpp>
#include <atlas/core/result.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/renderer/texture_cache.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/scene/scene.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace atlas::sandbox {

class SceneDemo {
  public:
    struct Config {
        std::string_view shader_directory = "assets/cooked/shaders";
        std::string_view texture_path = "textures/tile.png";
        /// Where the scene is written and read back from.
        std::filesystem::path save_path;
    };

    /// Build the scene, save it, load it back, and keep the loaded copy to draw.
    ///
    /// Fails if the round trip does not reproduce the file byte for byte. That check is the
    /// point of the exercise, so a run that cannot make it pass should not proceed as though
    /// it had.
    [[nodiscard]] static Result<SceneDemo> create(rhi::Device& device, assets::Registry& registry,
                                                  const Config& config);

    ~SceneDemo();

    SceneDemo(const SceneDemo&) = delete;
    SceneDemo& operator=(const SceneDemo&) = delete;
    SceneDemo(SceneDemo&& other) noexcept;
    SceneDemo& operator=(SceneDemo&& other) noexcept;

    void update(const platform::InputState& input, std::span<const platform::Event> events);

    /// Advance the animation by one simulation tick and recompose the world transforms.
    ///
    /// Driven by ticks rather than by frames so that what is drawn depends on simulated time
    /// and not on how fast the machine happens to be.
    void tick(std::uint64_t tick_index, std::uint64_t ticks_per_second);

    void resize(std::uint32_t pixel_width, std::uint32_t pixel_height);

    std::size_t finalise_assets(assets::Registry& registry);

    [[nodiscard]] renderer::BatchStats draw(rhi::RenderPass& pass);

    /// The loaded scene, for inspection. Const: the overlay may look, not touch.
    [[nodiscard]] const scene::Scene& scene() const noexcept { return m_scene; }

    [[nodiscard]] math::OrthoCamera& camera() noexcept { return m_camera; }

    [[nodiscard]] const std::filesystem::path& save_path() const noexcept { return m_save_path; }

    /// Bytes the saved scene occupies, for the overlay.
    [[nodiscard]] std::size_t saved_size() const noexcept { return m_saved_bytes; }

  private:
    SceneDemo() = default;
    void release() noexcept;

    rhi::Device* m_device = nullptr;
    renderer::TextureCache m_textures;
    renderer::QuadBatch m_batch;
    assets::AssetId m_texture_id;
    /// Rotation is reported the first time it is dropped, not every frame.
    bool m_warned_rotation = false;

    scene::Scene m_scene;
    std::filesystem::path m_save_path;
    std::size_t m_saved_bytes = 0;

    math::OrthoCamera m_camera;
    std::vector<scene::StableId> m_orbiting;
    bool m_dragging = false;
};

}  // namespace atlas::sandbox
