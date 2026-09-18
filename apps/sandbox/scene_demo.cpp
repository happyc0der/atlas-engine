// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene_demo.hpp"

#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/scene/serialization.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <numbers>
#include <utility>
#include <variant>

namespace atlas::sandbox {
namespace {

constexpr log::Category kApp = log::category::kApp;

constexpr float kParentSize = 24.0F;
constexpr float kChildSize = 10.0F;
constexpr float kOrbitRadius = 40.0F;
constexpr std::size_t kChildCount = 6;

/// The most one frame may advance a clip.
///
/// A frame that took a second — a debugger stopped at a breakpoint, a lid closed and opened —
/// would otherwise move every clip on by a second the moment the process is running again,
/// which looks like a glitch and is one. A quarter of a second is far longer than any frame
/// this application draws and far shorter than any pause worth noticing.
constexpr std::uint64_t kMaxFrameStepNs = 250'000'000;

/// Build the hierarchy that gets saved.
///
/// Built once, in code, and then never drawn from: the copy that is drawn comes back from the
/// file. That is what makes the demonstration worth anything.
[[nodiscard]] Result<scene::Scene> build_scene(const DemoAssets& ids) {
    scene::Scene built;

    const scene::StableId camera = built.create("camera");
    built.set_camera(camera, scene::Camera{.zoom = 6.0F, .active = true});

    const scene::StableId parent = built.create("parent");
    built.set_local_transform(parent, scene::LocalTransform{.position = {.x = 0.0F, .y = 0.0F}});
    built.set_sprite(parent, scene::SpriteRenderData{
                                 .texture = ids.texture,
                                 .size = {.x = kParentSize, .y = kParentSize},
                                 .tint = {.r = 1.0F, .g = 0.85F, .b = 0.35F, .a = 1.0F},
                                 .layer = 1,
                             });
    // The whole of what used to be a hand-written orbit in `tick`. The clip moves and turns,
    // and because every other sprite hangs off this entity, one revolution here turns nine
    // quads — which is what puts the renderer's rotated path on an ordinary frame rather than
    // only in a test.
    //
    // The authored transform is left at the origin and the clip's keys are offsets from it, so
    // dragging this entity through the inspector moves the whole orbit and the file keeps
    // working. That is the two-writer split of ADR-0012, visible.
    built.set_animator(parent, scene::Animator{.clip = ids.orbit_clip, .loop = 1});

    for (std::size_t i = 0; i < kChildCount; ++i) {
        const float turn = (static_cast<float>(i) / static_cast<float>(kChildCount)) * 2.0F *
                           std::numbers::pi_v<float>;

        const scene::StableId child = built.create(std::format("child {}", i));
        // Positioned relative to the parent, which is the whole point: the parent moves and
        // these follow without any of them being told.
        built.set_local_transform(
            child, scene::LocalTransform{.position = {.x = std::cos(turn) * kOrbitRadius,
                                                      .y = std::sin(turn) * kOrbitRadius}});
        built.set_sprite(child, scene::SpriteRenderData{
                                    .texture = ids.texture,
                                    .size = {.x = kChildSize, .y = kChildSize},
                                    .tint = {.r = 0.35F,
                                             .g = 0.55F + (0.4F * static_cast<float>(i) /
                                                           static_cast<float>(kChildCount)),
                                             .b = 0.9F,
                                             .a = 1.0F},
                                    .layer = 0,
                                });

        if (auto status = built.set_parent(child, parent); !status) {
            return std::unexpected(std::move(status).error().context("building the hierarchy"));
        }

        // One grandchild, so the demonstration covers composition more than one level deep.
        if (i == 0) {
            const scene::StableId descendant = built.create("grandchild");
            built.set_local_transform(descendant,
                                      scene::LocalTransform{.position = {.x = 14.0F, .y = 0.0F},
                                                            .scale = {.x = 0.6F, .y = 0.6F}});
            // A different texture from every other sprite here, which is why the draw path
            // sets one per sprite instead of once per frame. Its tint is white: a sheet whose
            // cells are told apart by colour would be told apart by the tint instead.
            built.set_sprite(descendant, scene::SpriteRenderData{
                                             .texture = ids.sheet,
                                             .size = {.x = kChildSize, .y = kChildSize},
                                             .tint = {.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F},
                                             .layer = 2,
                                         });
            // Frames only. This clip changes which cell of the sheet is shown and moves the
            // entity not at all, which is the half of animation that costs the renderer
            // nothing: a sprite already carries a rectangle.
            built.set_animator(descendant, scene::Animator{.clip = ids.cycle_clip, .loop = 1});
            // Not swapped: the parameters are (child, parent), and a grandchild's parent is
            // a child, so the names genuinely line up that way.
            // NOLINTNEXTLINE(readability-suspicious-call-argument)
            if (auto status = built.set_parent(descendant, child); !status) {
                return std::unexpected(std::move(status).error().context("building the hierarchy"));
            }
        }
    }

    built.update_transforms();
    return built;
}

[[nodiscard]] Status write_file(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(
                Error(ErrorCode::IoFailure,
                      std::format("could not create '{}': {}", parent.string(), ec.message())));
        }
    }

    // Binary, so the bytes on disk are the bytes produced. A text-mode write would translate
    // line endings on some platforms and the round-trip comparison would fail there and only
    // there.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(Error(ErrorCode::IoFailure,
                                     std::format("could not open '{}' to write", path.string())));
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        return std::unexpected(
            Error(ErrorCode::IoFailure, std::format("could not write '{}'", path.string())));
    }
    return {};
}

[[nodiscard]] Result<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("could not open '{}' to read", path.string())));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        return std::unexpected(
            Error(ErrorCode::IoFailure, std::format("could not read '{}'", path.string())));
    }
    return text;
}

/// Request one asset by path, so a failure names which one rather than "an asset".
[[nodiscard]] Status request_asset(assets::Registry& registry, std::string_view path,
                                   assets::AssetType type, assets::AssetId expected) {
    auto parsed = assets::VirtualPath::parse(path);
    if (!parsed) {
        return std::unexpected(
            std::move(parsed).error().context(std::format("the path '{}'", path)));
    }
    auto id = registry.request(*parsed, type);
    if (!id) {
        return std::unexpected(std::move(id).error().context(std::format("requesting '{}'", path)));
    }
    // The scene's components were built from `AssetId::from` on the same path and type. If the
    // registry derived a different identifier the components would name assets nothing loads,
    // and the symptom would be a magenta sprite that never animates rather than an error.
    if (*id != expected) {
        return std::unexpected(Error(
            ErrorCode::Internal,
            std::format("'{}' resolved to a different identifier than the scene records", path)));
    }
    return {};
}

}  // namespace

Placement decompose(const math::Mat4& matrix) noexcept {
    // Column-major, because that is what `uniform_elements` produces and what a shader reads.
    // For a rotation composed with a scale, the first column is (cos * sx, sin * sx) and the
    // second is (-sin * sy, cos * sy), so the axis lengths give the scales and the first
    // column's angle gives the rotation.
    const auto m = matrix.uniform_elements();
    return Placement{
        .position = {.x = m[12], .y = m[13]},
        .scale = {.x = std::hypot(m[0], m[1]), .y = std::hypot(m[4], m[5])},
        .rotation = std::atan2(m[1], m[0]),
    };
}

Result<scene::Scene> SceneDemo::build_demo_scene(const DemoAssets& ids) {
    return build_scene(ids);
}

Result<SceneDemo> SceneDemo::create(rhi::Device& device, assets::Registry& registry,
                                    const Config& config) {
    SceneDemo demo;
    demo.m_device = &device;

    auto textures = renderer::TextureCache::create(device);
    if (!textures) {
        return std::unexpected(std::move(textures).error().context("creating the texture cache"));
    }
    demo.m_textures = std::move(*textures);

    // Both textures and both clips, so that what the scene names is what the registry is
    // loading. A clip that is still decoding leaves its entity at the identity pose for a few
    // frames, which looks like a still picture rather than like a failure.
    if (auto status = request_asset(registry, kDemoTexturePath, assets::AssetType::Texture,
                                    demo.m_assets.texture);
        !status) {
        return std::unexpected(std::move(status).error());
    }
    if (auto status = request_asset(registry, kDemoSheetPath, assets::AssetType::Texture,
                                    demo.m_assets.sheet);
        !status) {
        return std::unexpected(std::move(status).error());
    }
    if (auto status = request_asset(registry, kDemoOrbitClipPath, assets::AssetType::AnimationClip,
                                    demo.m_assets.orbit_clip);
        !status) {
        return std::unexpected(std::move(status).error());
    }
    if (auto status = request_asset(registry, kDemoCycleClipPath, assets::AssetType::AnimationClip,
                                    demo.m_assets.cycle_clip);
        !status) {
        return std::unexpected(std::move(status).error());
    }

    auto batch =
        renderer::QuadBatch::create(device, {.capacity = renderer::QuadBatch::kDefaultCapacity,
                                             .shader_directory = config.shader_directory});
    if (!batch) {
        return std::unexpected(std::move(batch).error());
    }
    demo.m_batch = std::move(*batch);

    demo.m_scene = std::make_unique<scene::Scene>();
    auto built = build_scene(demo.m_assets);
    if (!built) {
        return std::unexpected(std::move(built).error());
    }

    auto saved = scene::to_text(*built);
    if (!saved) {
        return std::unexpected(std::move(saved).error().context("saving the scene"));
    }

    demo.m_save_path = config.save_path;
    if (auto status = write_file(demo.m_save_path, *saved); !status) {
        return std::unexpected(std::move(status).error());
    }

    // Read the file back rather than reusing the string in hand, so the path being exercised
    // is the one a real load would take, including whatever the file system did to the bytes.
    auto from_disk = read_file(demo.m_save_path);
    if (!from_disk) {
        return std::unexpected(std::move(from_disk).error());
    }

    if (auto status = scene::from_text((*demo.m_scene), *from_disk); !status) {
        return std::unexpected(std::move(status).error().context("loading the scene back"));
    }

    // The history takes the scene by reference and is the only thing permitted to write an
    // authored component from here on. `animation::advance` writes the derived pose and
    // nothing else, so the two never contend. Constructed after the load, so an undo can never
    // reach behind the state the application started from.
    demo.m_history.emplace(*demo.m_scene);

    auto resaved = scene::to_text((*demo.m_scene));
    if (!resaved) {
        return std::unexpected(std::move(resaved).error().context("re-saving the loaded scene"));
    }

    if (*resaved != *saved) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("the scene did not survive a round trip: {} bytes written, {} bytes "
                              "after loading and saving again",
                              saved->size(), resaved->size())));
    }
    demo.m_saved_bytes = saved->size();

    // The view starts from the scene's own active camera rather than from a constant here.
    // Otherwise the Camera component would be written to the file, read back, and then
    // ignored, which is a component that looks supported and is not. After this the view is
    // the user's: panning and zooming are not written back to the scene.
    if (const auto active = (*demo.m_scene).active_camera()) {
        const auto* component = (*demo.m_scene).camera(*active);
        const auto* placement = (*demo.m_scene).world_transform(*active);
        if (component != nullptr) {
            demo.m_camera.set_zoom(component->zoom);
        }
        if (placement != nullptr) {
            demo.m_camera.set_centre(decompose(placement->matrix).position);
        }
    } else {
        ATLAS_LOG_WARN(kApp, "the loaded scene has no active camera; using a default view");
        demo.m_camera.set_centre({0.0F, 0.0F});
        demo.m_camera.set_zoom(6.0F);
    }

    ATLAS_LOG_INFO(kApp,
                   "scene demo ready: {} entities, {} animated, saved to '{}' ({} bytes), "
                   "reloaded and re-saved identically",
                   (*demo.m_scene).size(), (*demo.m_scene).animated().size(),
                   demo.m_save_path.string(), demo.m_saved_bytes);
    return demo;
}

void SceneDemo::release() noexcept {
    if (m_device == nullptr) {
        return;
    }
    m_batch = renderer::QuadBatch{};
    m_textures = renderer::TextureCache{};
    m_device = nullptr;
}

SceneDemo::~SceneDemo() {
    release();
}

SceneDemo::SceneDemo(SceneDemo&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_textures(std::move(other.m_textures)),
      m_batch(std::move(other.m_batch)), m_assets(other.m_assets),
      m_scene(std::move(other.m_scene)), m_history(std::move(other.m_history)),
      m_clips(std::move(other.m_clips)), m_advance(other.m_advance),
      m_save_path(std::move(other.m_save_path)), m_saved_bytes(other.m_saved_bytes),
      m_camera(other.m_camera), m_camera_controls(other.m_camera_controls),
      m_camera_controller(other.m_camera_controller) {}

SceneDemo& SceneDemo::operator=(SceneDemo&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_textures = std::move(other.m_textures);
        m_batch = std::move(other.m_batch);
        m_assets = other.m_assets;
        m_scene = std::move(other.m_scene);
        m_history = std::move(other.m_history);
        m_clips = std::move(other.m_clips);
        m_advance = other.m_advance;
        m_save_path = std::move(other.m_save_path);
        m_saved_bytes = other.m_saved_bytes;
        m_camera = other.m_camera;
        m_camera_controls = other.m_camera_controls;
        m_camera_controller = other.m_camera_controller;
    }
    return *this;
}

std::size_t SceneDemo::finalise_assets(assets::Registry& registry) {
    // Two finalisers over one registry, each filtering by type, exactly as the audio device's
    // sits beside the texture cache's. A reload replaces a clip and leaves every playing
    // entity's clock alone, so editing a clip file while watching continues from where
    // playback had reached rather than restarting.
    return m_textures.finalise_pending(registry) + m_clips.finalise_pending(registry);
}

void SceneDemo::resize(std::uint32_t pixel_width, std::uint32_t pixel_height) {
    m_camera.set_viewport(static_cast<float>(pixel_width), static_cast<float>(pixel_height));
}

void SceneDemo::advance(std::uint64_t frame_ns) {
    ATLAS_ZONE_NAMED("scene demo advance");
    m_advance = animation::advance(*m_scene, m_clips, std::min(frame_ns, kMaxFrameStepNs));
}

void SceneDemo::recompose() {
    m_scene->update_transforms();
}

renderer::BatchStats SceneDemo::draw(rhi::RenderPass& pass) {
    ATLAS_ZONE_NAMED("scene demo draw");

    m_batch.begin(pass, m_camera.view_projection());

    // The texture is set per sprite rather than once per frame, because the grandchild draws
    // from the sprite sheet and everything else from the tile. `set_texture` flushes when the
    // texture actually changes and does nothing when it does not, so a scene using one texture
    // still costs one draw call; this one costs two, because `drawable()` is ordered by layer
    // and the sheet is on the topmost.
    assets::AssetId bound;

    // drawable() is already in draw order: by layer, then by identifier. Sorting here as well
    // would be a second ordering rule that could drift from the first.
    for (const scene::StableId id : m_scene->drawable()) {
        const auto* sprite = m_scene->sprite(id);
        const auto* world = m_scene->world_transform(id);
        if (sprite == nullptr || world == nullptr || !sprite->visible) {
            continue;
        }

        if (sprite->texture != bound) {
            bound = sprite->texture;
            m_batch.set_texture(m_textures.texture_for(bound), m_textures.sampler());
        }

        // Rotation is drawn rather than dropped and warned about, which is what M13 bought.
        // The composed matrix carries whatever the tree and the pose between them produced,
        // and the batcher turns the quad about its own centre.
        const Placement placement = decompose(world->matrix);
        const float width = sprite->size.x * placement.scale.x;
        const float height = sprite->size.y * placement.scale.y;

        // A pose's frame rectangle wins over the sprite's own when a clip has set one. That
        // single line is the whole of frame animation: a sprite already carries a rectangle,
        // so cycling a sheet costs the renderer nothing at all.
        const auto* pose = m_scene->animation_pose(id);
        const math::Rect uv =
            (pose != nullptr && pose->frame_uv.has_value()) ? *pose->frame_uv : sprite->uv;

        m_batch.add(renderer::Quad{
            .bounds = {.position = {placement.position.x - (width * 0.5F),
                                    placement.position.y - (height * 0.5F)},
                       .size = {width, height}},
            .uv = uv,
            .colour = sprite->tint,
            .rotation = placement.rotation,
        });
    }

    return m_batch.end();
}

void SceneDemo::update(const platform::InputState& input, std::span<const platform::Event> events,
                       float display_scale, float dt_seconds, bool mouse_allowed) {
    ATLAS_ZONE_NAMED("scene demo update");

    // One controller rather than a fourth copy of this: the same twenty lines lived in
    // three files, and M9 fixed one bug in four places because of it.
    m_camera_controller.apply(
        m_camera, app::sample_camera_input(input, events, m_camera_controls, mouse_allowed),
        display_scale, dt_seconds);
}

}  // namespace atlas::sandbox
