// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The sandbox's scene-graph demonstration.
///
/// What M5 exists to show, end to end in one run: a small hierarchy is built in code, saved
/// to text, loaded back, and drawn from the loaded copy. The original is kept only so the two
/// can be compared; the pictures on screen come from the file.
///
/// Since M13 the movement comes from a file too. The parent root and one grandchild carry an
/// `Animator` naming a committed clip, and `animation::advance` writes the pose those clips
/// produce. Nothing here animates by hand any more, and there is no pause key, because there
/// is nothing left to pause for: the animator writes only the derived pose, so an entity can
/// be dragged through the inspector in the same frame a clip is moving it.
///
/// It is a handful of squares around a moving parent. It is not a map and not a game: the
/// entities carry no meaning beyond being arranged in a tree.

#include <atlas/animation/advance.hpp>
#include <atlas/app/camera_controller.hpp>
#include <atlas/assets/asset_id.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/core/result.hpp>
#include <atlas/edit/history.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/math/matrix.hpp>
#include <atlas/math/vector.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/renderer/texture_cache.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/scene/scene.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace atlas::sandbox {

/// The asset paths the demonstration scene refers to.
///
/// Constants rather than literals at each use, because the identifier in a saved scene is a
/// hash of the path and the type: a check that built the scene from a different spelling would
/// be checking a different scene and would not say so.
inline constexpr std::string_view kDemoTexturePath = "textures/tile.png";
inline constexpr std::string_view kDemoSheetPath = "textures/sheet.png";
inline constexpr std::string_view kDemoOrbitClipPath = "animation/orbit.clip.json";
inline constexpr std::string_view kDemoCycleClipPath = "animation/cycle.clip.json";

/// The identifiers the demonstration scene's components carry.
///
/// Derived from the paths above, which is the same derivation `Registry::request` performs, so
/// a headless check that never opens a registry still builds the scene the application shows
/// rather than one that merely resembles it.
struct DemoAssets {
    assets::AssetId texture = assets::AssetId::from(kDemoTexturePath, assets::AssetType::Texture);
    assets::AssetId sheet = assets::AssetId::from(kDemoSheetPath, assets::AssetType::Texture);
    assets::AssetId orbit_clip =
        assets::AssetId::from(kDemoOrbitClipPath, assets::AssetType::AnimationClip);
    assets::AssetId cycle_clip =
        assets::AssetId::from(kDemoCycleClipPath, assets::AssetType::AnimationClip);
};

/// Position, scale and rotation, pulled back out of a composed matrix.
///
/// The scene composes a tree of transforms into one matrix per entity, and the batcher wants
/// the three parts back. Reading them out is four lines of trigonometry, and it lives here —
/// named, in a header — because the headless animation check asserts against the same numbers
/// the draw path uses. Two call sites with two copies of an `atan2` is how a sign convention
/// comes to differ between what is checked and what is drawn.
///
/// Shear is not recovered, and the scene cannot express it: a local transform is a rotation
/// and a non-uniform scale, and composing those can shear only if a parent scales
/// non-uniformly about a rotated child. The demonstration does not, and a sheared sprite would
/// be drawn as the nearest unsheared one.
struct Placement {
    math::Vec2 position;
    math::Vec2 scale{.x = 1.0F, .y = 1.0F};
    /// Radians, counter-clockwise, unwrapped to (-pi, pi].
    float rotation = 0.0F;
};

[[nodiscard]] Placement decompose(const math::Mat4& matrix) noexcept;

class SceneDemo {
  public:
    struct Config {
        std::string_view shader_directory = "assets/cooked/shaders";
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

    /// Build the demonstration scene on its own, with no device and no assets loaded.
    ///
    /// Exposed so a headless check can exercise the edit and animation paths against the same
    /// scene the application shows, rather than against one invented for the test. The asset
    /// identifiers are only recorded on the components; nothing here loads anything.
    [[nodiscard]] static Result<scene::Scene> build_demo_scene(const DemoAssets& ids = {});

    ~SceneDemo();

    SceneDemo(const SceneDemo&) = delete;
    SceneDemo& operator=(const SceneDemo&) = delete;
    SceneDemo(SceneDemo&& other) noexcept;
    SceneDemo& operator=(SceneDemo&& other) noexcept;

    /// Pan and zoom from the pointer.
    ///
    /// `display_scale` converts the platform's logical pointer coordinates into the pixels
    /// the camera's viewport is measured in. They differ on a high-density display, where
    /// omitting it pans at half speed and anchors a zoom to the wrong point.
    void update(const platform::InputState& input, std::span<const platform::Event> events,
                float display_scale, float dt_seconds, bool mouse_allowed);

    /// Move every animator on by one frame.
    ///
    /// Writes `scene::AnimationPose` and nothing else, so this and the edit history can both
    /// run in the same frame without either overwriting the other. Driven by frame time rather
    /// than by ticks: animation is presentation, it reaches no simulation state, and nothing it
    /// produces is hashed.
    ///
    /// The step is clamped, because a frame that took a second — a debugger stopped at a
    /// breakpoint, a laptop lid closed — would otherwise jump every clip forward by a second
    /// on the frame after it.
    void advance(std::uint64_t frame_ns);

    /// What the last `advance` did, for the overlay and for noticing a clip that never loaded.
    [[nodiscard]] const animation::AdvanceReport& advance_report() const noexcept {
        return m_advance;
    }

    void resize(std::uint32_t pixel_width, std::uint32_t pixel_height);

    /// Claim everything the registry has decoded for this demonstration: textures and clips.
    ///
    /// Two finalisers over one registry, each filtering by type. Returns the total, which for
    /// a steady frame is zero.
    std::size_t finalise_assets(assets::Registry& registry);

    /// Rebuild every world transform from the authored transforms and any poses.
    ///
    /// Called once per frame by the application, after the panels have applied this frame's
    /// edits and after `advance` has written this frame's poses, and before anything reads a
    /// composed matrix. Cheap here — the demonstration has nine entities — and correct
    /// regardless of what else ran, which is the property that matters: the edit history does
    /// not recompose, and something that recomposes only sometimes recomposes wrongly.
    void recompose();

    [[nodiscard]] renderer::BatchStats draw(rhi::RenderPass& pass);

    /// The scene, for looking at.
    ///
    /// Const, and it stays const: every change goes through `history()`. Reaching the scene
    /// through the history is what stops a panel from having a second way in.
    [[nodiscard]] const scene::Scene& scene() const noexcept { return m_history->scene(); }

    /// The edit history, which is the only thing that may change an authored component.
    [[nodiscard]] edit::History& history() noexcept { return *m_history; }

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
    DemoAssets m_assets;

    // Heap-allocated so the scene keeps its address when a SceneDemo is moved, which it is:
    // create returns one through Result into an optional. The history holds a reference to it.
    // Declared before the history so it outlives it.
    std::unique_ptr<scene::Scene> m_scene;
    std::optional<edit::History> m_history;
    animation::ClipCache m_clips;
    animation::AdvanceReport m_advance;
    std::filesystem::path m_save_path;
    std::size_t m_saved_bytes = 0;

    math::OrthoCamera m_camera;
    app::CameraControls m_camera_controls{.drag_button = platform::MouseButton::Left};
    app::CameraController m_camera_controller{m_camera_controls};
};

}  // namespace atlas::sandbox
