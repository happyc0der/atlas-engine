// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene_demo.hpp"

#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/scene/serialization.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <numbers>
#include <utility>
#include <variant>

namespace atlas::sandbox {
namespace {

constexpr log::Category kApp = log::category::kApp;

// A composed matrix's off-diagonal terms are exactly zero for pure scale, but arrive through
// float multiplication; anything under this fraction of the axis length is arithmetic noise.
constexpr float kRotationTolerance = 1e-4F;

constexpr float kParentSize = 24.0F;
constexpr float kChildSize = 10.0F;
constexpr float kOrbitRadius = 40.0F;
constexpr std::size_t kChildCount = 6;

/// Build the hierarchy that gets saved.
///
/// Built once, in code, and then never drawn from: the copy that is drawn comes back from the
/// file. That is what makes the demonstration worth anything.
[[nodiscard]] Result<scene::Scene> build_scene(assets::AssetId texture) {
    scene::Scene built;

    const scene::StableId camera = built.create("camera");
    built.set_camera(camera, scene::Camera{.zoom = 6.0F, .active = true});

    const scene::StableId parent = built.create("parent");
    built.set_local_transform(parent, scene::LocalTransform{.position = {.x = 0.0F, .y = 0.0F}});
    built.set_sprite(parent, scene::SpriteRenderData{
                                 .texture = texture,
                                 .size = {.x = kParentSize, .y = kParentSize},
                                 .tint = {.r = 1.0F, .g = 0.85F, .b = 0.35F, .a = 1.0F},
                                 .layer = 1,
                             });

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
                                    .texture = texture,
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
            built.set_sprite(descendant, scene::SpriteRenderData{
                                             .texture = texture,
                                             .size = {.x = kChildSize, .y = kChildSize},
                                             .tint = {.r = 1.0F, .g = 0.45F, .b = 0.45F, .a = 1.0F},
                                             .layer = 2,
                                         });
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

}  // namespace

Result<scene::Scene> SceneDemo::build_demo_scene(assets::AssetId texture) {
    return build_scene(texture);
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

    auto path = assets::VirtualPath::parse(config.texture_path);
    if (!path) {
        return std::unexpected(std::move(path).error().context("the scene texture path"));
    }
    auto texture_id = registry.request(*path, assets::AssetType::Texture);
    if (!texture_id) {
        return std::unexpected(
            std::move(texture_id).error().context("requesting the scene texture"));
    }
    demo.m_texture_id = *texture_id;

    auto batch =
        renderer::QuadBatch::create(device, {.capacity = renderer::QuadBatch::kDefaultCapacity,
                                             .shader_directory = config.shader_directory});
    if (!batch) {
        return std::unexpected(std::move(batch).error());
    }
    demo.m_batch = std::move(*batch);

    demo.m_scene = std::make_unique<scene::Scene>();
    auto built = build_scene(demo.m_texture_id);
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

    // The history takes the scene by reference and is the only thing permitted to write to
    // it from here on, the demonstration animation aside. Constructed after the load, so an
    // undo can never reach behind the state the application started from.
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

    for (const auto& view : (*demo.m_scene).entities()) {
        if (view.parent != scene::StableId::None && (*demo.m_scene).sprite(view.id) != nullptr) {
            demo.m_orbiting.push_back(view.id);
        }
    }

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
            const auto m = placement->matrix.uniform_elements();
            demo.m_camera.set_centre({m[12], m[13]});
        }
    } else {
        ATLAS_LOG_WARN(kApp, "the loaded scene has no active camera; using a default view");
        demo.m_camera.set_centre({0.0F, 0.0F});
        demo.m_camera.set_zoom(6.0F);
    }

    ATLAS_LOG_INFO(kApp,
                   "scene demo ready: {} entities, saved to '{}' ({} bytes), reloaded and "
                   "re-saved identically",
                   (*demo.m_scene).size(), demo.m_save_path.string(), demo.m_saved_bytes);
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
      m_batch(std::move(other.m_batch)), m_texture_id(std::exchange(other.m_texture_id, {})),
      m_scene(std::move(other.m_scene)), m_history(std::move(other.m_history)),
      m_animating(other.m_animating), m_save_path(std::move(other.m_save_path)),
      m_saved_bytes(other.m_saved_bytes), m_camera(other.m_camera),
      m_orbiting(std::move(other.m_orbiting)), m_dragging(other.m_dragging) {}

SceneDemo& SceneDemo::operator=(SceneDemo&& other) noexcept {
    if (this != &other) {
        release();
        m_device = std::exchange(other.m_device, nullptr);
        m_textures = std::move(other.m_textures);
        m_batch = std::move(other.m_batch);
        m_texture_id = std::exchange(other.m_texture_id, {});
        m_scene = std::move(other.m_scene);
        m_history = std::move(other.m_history);
        m_animating = other.m_animating;
        m_save_path = std::move(other.m_save_path);
        m_saved_bytes = other.m_saved_bytes;
        m_camera = other.m_camera;
        m_orbiting = std::move(other.m_orbiting);
        m_dragging = other.m_dragging;
    }
    return *this;
}

std::size_t SceneDemo::finalise_assets(assets::Registry& registry) {
    return m_textures.finalise_pending(registry);
}

void SceneDemo::resize(std::uint32_t pixel_width, std::uint32_t pixel_height) {
    m_camera.set_viewport(static_cast<float>(pixel_width), static_cast<float>(pixel_height));
}

void SceneDemo::tick(std::uint64_t tick_index, std::uint64_t ticks_per_second) {
    ATLAS_ZONE_NAMED("scene demo tick");

    if (ticks_per_second == 0 || !m_animating) {
        return;
    }

    const float seconds = static_cast<float>(tick_index) / static_cast<float>(ticks_per_second);

    // Only the roots move. Everything below them follows because their transforms are
    // relative, which is the property being demonstrated; nothing here touches a child.
    //
    // This is the one writer to the scene that is not the edit history, and it writes straight
    // through the pointer rather than through a command: an animation frame is not an
    // authoring step and has no business on an undo stack. It also means editing a
    // sprite-bearing root while the animation runs is pointless, because the next tick
    // overwrites it. Space pauses it, which is why that key exists.
    for (const scene::StableId root : m_scene->roots()) {
        if (m_scene->sprite(root) == nullptr) {
            continue;
        }
        const float angle = seconds * 0.6F;
        m_scene->set_local_transform(
            root, scene::LocalTransform{.position = {.x = std::cos(angle) * 30.0F,
                                                     .y = std::sin(angle * 1.3F) * 18.0F}});
    }

    m_scene->update_transforms();
}

renderer::BatchStats SceneDemo::draw(rhi::RenderPass& pass) {
    ATLAS_ZONE_NAMED("scene demo draw");

    m_batch.begin(pass, m_camera.view_projection());
    m_batch.set_texture(m_textures.texture_for(m_texture_id), m_textures.sampler());

    // drawable() is already in draw order: by layer, then by identifier. Sorting here as well
    // would be a second ordering rule that could drift from the first.
    for (const scene::StableId id : m_scene->drawable()) {
        const auto* sprite = m_scene->sprite(id);
        const auto* world = m_scene->world_transform(id);
        if (sprite == nullptr || world == nullptr || !sprite->visible) {
            continue;
        }

        // The batcher draws axis-aligned rectangles, so a composed rotation cannot be shown.
        // Position and scale are taken from the composed matrix and rotation is dropped. A
        // scene that expresses one is told so once, rather than drawn quietly wrong; the
        // re-deferral and its reason are in docs/DEFERRED.md under M3.
        const auto m = world->matrix.uniform_elements();
        const float scale_x = std::hypot(m[0], m[1]);
        const float scale_y = std::hypot(m[4], m[5]);
        if (!m_warned_rotation && (std::abs(m[1]) > kRotationTolerance * scale_x ||
                                   std::abs(m[4]) > kRotationTolerance * scale_y)) {
            m_warned_rotation = true;
            ATLAS_LOG_WARN(kApp, "a sprite carries a rotation the batcher cannot draw; it is drawn "
                                 "axis-aligned, and this is reported once per scene");
        }
        const float width = sprite->size.x * scale_x;
        const float height = sprite->size.y * scale_y;

        m_batch.add(renderer::Quad{
            .bounds = {.position = {m[12] - (width * 0.5F), m[13] - (height * 0.5F)},
                       .size = {width, height}},
            .uv = sprite->uv,
            .colour = sprite->tint,
        });
    }

    return m_batch.end();
}

void SceneDemo::update(const platform::InputState& input, std::span<const platform::Event> events,
                       float display_scale) {
    ATLAS_ZONE_NAMED("scene demo update");

    if (input.was_pressed(platform::MouseButton::Left)) {
        m_dragging = true;
    }
    if (input.was_released(platform::MouseButton::Left)) {
        m_dragging = false;
    }

    if (m_dragging) {
        const float zoom = m_camera.zoom();
        m_camera.pan({-input.mouse_delta_x() * display_scale / zoom,
                      -input.mouse_delta_y() * display_scale / zoom});
    }

    for (const auto& event : events) {
        const auto* wheel = std::get_if<platform::MouseWheel>(&event);
        if (wheel == nullptr || wheel->delta_y == 0.0F) {
            continue;
        }
        const float factor = std::pow(1.15F, wheel->delta_y);
        const auto pointer = input.mouse_position();
        m_camera.zoom_about(factor,
                            math::Vec2{pointer.x * display_scale, pointer.y * display_scale});
    }
}

}  // namespace atlas::sandbox
