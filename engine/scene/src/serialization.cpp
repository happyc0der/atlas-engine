// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/scene/serialization.hpp>

#include <nlohmann/json.hpp>

#include <format>
#include <limits>
#include <string>
#include <vector>

namespace atlas::scene {
namespace {

constexpr log::Category kScene{"scene"};

/// Ordered rather than plain: key order is part of the file being canonical, and the plain
/// type sorts keys by whatever its hash produces.
using Json = nlohmann::ordered_json;

/// Bounds on what will be read.
///
/// A scene file is untrusted input. A count claiming millions of entities is either a mistake
/// or an attack, and either way it should be refused before anything is allocated rather than
/// after.
constexpr std::size_t kMaxEntities = 1'000'000;
constexpr std::size_t kMaxNameLength = 1024;

[[nodiscard]] Json write_vec2(const math::Vec2& v) {
    return Json::array({v.x, v.y});
}

[[nodiscard]] Result<math::Vec2> read_vec2(const Json& node, std::string_view what) {
    if (!node.is_array() || node.size() != 2 || !node[0].is_number() || !node[1].is_number()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("{} should be two numbers", what)));
    }
    return math::Vec2{.x = node[0].get<float>(), .y = node[1].get<float>()};
}

[[nodiscard]] Result<float> read_number(const Json& parent, const char* key,
                                        std::string_view what) {
    if (!parent.contains(key) || !parent[key].is_number()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("{} is missing or not a number", what)));
    }
    return parent[key].get<float>();
}

/// Put arrays of plain numbers back on one line.
///
/// The JSON writer indents every array element, which turns a two-number position into four
/// lines. The file is meant to be read and compared by a person, and moving one entity should
/// show up as one changed line rather than as a paragraph. Arrays of objects, such as the
/// entity list, are left alone: those genuinely want a line each.
///
/// Safe to do textually because only the numeric arrays this writer emits can match. A JSON
/// number contains no bracket, comma or quote, so an array whose span holds nothing but digits
/// and number punctuation cannot contain a nested array, an object, or a string.
[[nodiscard]] std::string compact_number_arrays(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '[') {
            out.push_back(text[i]);
            continue;
        }

        const std::size_t close = text.find(']', i);
        if (close == std::string::npos) {
            out.push_back(text[i]);
            continue;
        }

        const std::string_view span{text.data() + i + 1, close - i - 1};
        const bool numeric_only =
            !span.empty() &&
            span.find_first_not_of("0123456789+-.eE, \n\t\r") == std::string_view::npos;
        if (!numeric_only) {
            out.push_back(text[i]);
            continue;
        }

        out.push_back('[');
        bool after_comma = false;
        for (const char c : span) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                continue;
            }
            if (after_comma) {
                out.push_back(' ');
                after_comma = false;
            }
            out.push_back(c);
            if (c == ',') {
                after_comma = true;
            }
        }
        out.push_back(']');
        i = close;
    }

    return out;
}

}  // namespace

Result<std::string> to_text(const Scene& scene) {
    ATLAS_ZONE_NAMED("scene::to_text");

    Json root;
    root["format"] = "atlas-scene";
    root["version"] = kSceneFormatVersion;

    Json entities = Json::array();

    // entities() is ordered by identifier, which is what makes the output canonical.
    for (const auto& view : scene.entities()) {
        Json entry;
        entry["id"] = static_cast<std::uint64_t>(view.id);
        entry["name"] = std::string{view.name};

        if (view.parent != StableId::None) {
            entry["parent"] = static_cast<std::uint64_t>(view.parent);
        }

        // Components in a fixed order, so two saves of the same scene match byte for byte.
        if (const auto* local = scene.local_transform(view.id)) {
            Json transform;
            transform["position"] = write_vec2(local->position);
            transform["rotation"] = local->rotation;
            transform["scale"] = write_vec2(local->scale);
            entry["transform"] = std::move(transform);
        }

        if (const auto* sprite = scene.sprite(view.id)) {
            Json node;
            node["texture"] = sprite->texture.value();
            node["size"] = write_vec2(sprite->size);
            node["uv_position"] = write_vec2(sprite->uv.position);
            node["uv_size"] = write_vec2(sprite->uv.size);
            node["tint"] =
                Json::array({sprite->tint.r, sprite->tint.g, sprite->tint.b, sprite->tint.a});
            node["layer"] = sprite->layer;
            node["visible"] = sprite->visible;
            entry["sprite"] = std::move(node);
        }

        if (const auto* camera = scene.camera(view.id)) {
            Json node;
            node["zoom"] = camera->zoom;
            node["active"] = camera->active;
            entry["camera"] = std::move(node);
        }

        // The world transform is deliberately absent: it is derived from the local ones, and
        // a file that stored both could disagree with itself.
        entities.push_back(std::move(entry));
    }

    root["entities"] = std::move(entities);

    // Two-space indent and a trailing newline, so the output is readable and diffs cleanly.
    return compact_number_arrays(root.dump(2)) + "\n";
}

Status from_text(Scene& scene, std::string_view text) {
    ATLAS_ZONE_NAMED("scene::from_text");

    // The non-throwing parse: ADR-0005 forbids exceptions crossing a module boundary, and a
    // malformed file is an expected outcome rather than an exceptional one.
    const Json root = Json::parse(text, nullptr, false, true);
    if (root.is_discarded()) {
        return std::unexpected(Error(ErrorCode::MalformedData, "the scene file is not valid JSON"));
    }
    if (!root.is_object()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "a scene file must be a JSON object"));
    }

    if (!root.contains("format") || !root["format"].is_string() ||
        root["format"].get<std::string>() != "atlas-scene") {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  "this is not an Atlas scene file: the format marker is missing or wrong"));
    }

    if (!root.contains("version") || !root["version"].is_number_unsigned()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "the scene file has no schema version"));
    }
    const auto version = root["version"].get<std::uint64_t>();

    // A newer file is refused rather than half-understood. Reading one with this build would
    // silently drop whatever the newer version added, and the first sign would be data
    // quietly disappearing on the next save.
    if (version > kSceneFormatVersion) {
        return std::unexpected(
            Error(ErrorCode::VersionMismatch,
                  std::format("the scene file is version {}, and this build understands up to {}. "
                              "Use a newer build.",
                              version, kSceneFormatVersion)));
    }
    if (version == 0) {
        return std::unexpected(
            Error(ErrorCode::VersionMismatch, "schema version 0 is not a version"));
    }

    if (!root.contains("entities") || !root["entities"].is_array()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "the scene file has no entity list"));
    }
    const Json& entities = root["entities"];

    if (entities.size() > kMaxEntities) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("the scene file claims {} entities, beyond the limit of {}",
                              entities.size(), kMaxEntities)));
    }

    // Built aside and only committed once everything has validated, so a failure leaves the
    // caller's scene empty rather than half-populated.
    Scene loaded;
    std::vector<std::pair<StableId, StableId>> parentage;
    parentage.reserve(entities.size());

    for (const Json& entry : entities) {
        if (!entry.is_object()) {
            return std::unexpected(
                Error(ErrorCode::MalformedData, "an entity entry is not an object"));
        }
        if (!entry.contains("id") || !entry["id"].is_number_unsigned()) {
            return std::unexpected(Error(ErrorCode::MalformedData, "an entity has no identifier"));
        }

        const auto raw_id = entry["id"].get<std::uint64_t>();
        if (raw_id == 0) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      "an entity claims identifier 0, which is reserved for 'no entity'"));
        }
        const auto id = static_cast<StableId>(raw_id);

        std::string name;
        if (entry.contains("name")) {
            if (!entry["name"].is_string()) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("entity {} has a name that is not text", raw_id)));
            }
            name = entry["name"].get<std::string>();
            if (name.size() > kMaxNameLength) {
                return std::unexpected(Error(
                    ErrorCode::MalformedData,
                    std::format("entity {} has a name of {} characters, beyond the limit of {}",
                                raw_id, name.size(), kMaxNameLength)));
            }
        }

        auto created = loaded.create_with_id(id, name);
        if (!created) {
            return std::unexpected(std::move(created).error());
        }

        if (entry.contains("parent")) {
            if (!entry["parent"].is_number_unsigned()) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("entity {} has a parent reference that is not an identifier",
                                      raw_id)));
            }
            const auto parent_id = entry["parent"].get<std::uint64_t>();
            if (parent_id != 0) {
                parentage.emplace_back(id, static_cast<StableId>(parent_id));
            }
        }

        if (entry.contains("transform")) {
            const Json& node = entry["transform"];
            if (!node.is_object()) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("entity {} has a transform that is not an object", raw_id)));
            }

            auto position = read_vec2(node.value("position", Json::array({0.0F, 0.0F})),
                                      std::format("entity {} transform position", raw_id));
            if (!position) {
                return std::unexpected(std::move(position).error());
            }
            auto scale = read_vec2(node.value("scale", Json::array({1.0F, 1.0F})),
                                   std::format("entity {} transform scale", raw_id));
            if (!scale) {
                return std::unexpected(std::move(scale).error());
            }
            auto rotation =
                read_number(node, "rotation", std::format("entity {} transform rotation", raw_id));
            if (!rotation) {
                return std::unexpected(std::move(rotation).error());
            }

            loaded.set_local_transform(
                id, LocalTransform{.position = *position, .rotation = *rotation, .scale = *scale});
        }

        if (entry.contains("sprite")) {
            const Json& node = entry["sprite"];
            if (!node.is_object()) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("entity {} has a sprite that is not an object", raw_id)));
            }

            SpriteRenderData sprite;

            if (node.contains("texture") && node["texture"].is_number_unsigned()) {
                sprite.texture = assets::AssetId::from_raw(node["texture"].get<std::uint64_t>(),
                                                           assets::AssetType::Texture);
            }

            auto size = read_vec2(node.value("size", Json::array({1.0F, 1.0F})),
                                  std::format("entity {} sprite size", raw_id));
            if (!size) {
                return std::unexpected(std::move(size).error());
            }
            sprite.size = *size;

            auto uv_position = read_vec2(node.value("uv_position", Json::array({0.0F, 0.0F})),
                                         std::format("entity {} sprite uv position", raw_id));
            if (!uv_position) {
                return std::unexpected(std::move(uv_position).error());
            }
            auto uv_size = read_vec2(node.value("uv_size", Json::array({1.0F, 1.0F})),
                                     std::format("entity {} sprite uv size", raw_id));
            if (!uv_size) {
                return std::unexpected(std::move(uv_size).error());
            }
            sprite.uv = math::Rect{.position = *uv_position, .size = *uv_size};

            if (node.contains("tint")) {
                const Json& tint = node["tint"];
                if (!tint.is_array() || tint.size() != 4) {
                    return std::unexpected(Error(
                        ErrorCode::MalformedData,
                        std::format("entity {} has a tint that is not four numbers", raw_id)));
                }
                sprite.tint = rhi::Colour{.r = tint[0].get<float>(),
                                          .g = tint[1].get<float>(),
                                          .b = tint[2].get<float>(),
                                          .a = tint[3].get<float>()};
            }

            if (node.contains("layer")) {
                if (!node["layer"].is_number_integer()) {
                    return std::unexpected(Error(
                        ErrorCode::MalformedData,
                        std::format("entity {} has a layer that is not a whole number", raw_id)));
                }
                const auto layer = node["layer"].get<std::int64_t>();
                if (layer < std::numeric_limits<std::int32_t>::min() ||
                    layer > std::numeric_limits<std::int32_t>::max()) {
                    return std::unexpected(
                        Error(ErrorCode::MalformedData,
                              std::format("entity {} has a layer outside the representable range",
                                          raw_id)));
                }
                sprite.layer = static_cast<std::int32_t>(layer);
            }

            sprite.visible = node.value("visible", true);
            loaded.set_sprite(id, sprite);
        }

        if (entry.contains("camera")) {
            const Json& node = entry["camera"];
            if (!node.is_object()) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("entity {} has a camera that is not an object", raw_id)));
            }
            Camera camera;
            camera.zoom = node.value("zoom", 1.0F);
            camera.active = node.value("active", false);
            loaded.set_camera(id, camera);
        }
    }

    // Parentage after every entity exists, because a child may be written before its parent
    // and a forward reference must still resolve.
    for (const auto& [child, parent] : parentage) {
        if (auto status = loaded.set_parent(child, parent); !status) {
            return std::unexpected(std::move(status).error().context(
                std::format("linking entity {} to parent {}", static_cast<std::uint64_t>(child),
                            static_cast<std::uint64_t>(parent))));
        }
    }

    loaded.update_transforms();
    scene = std::move(loaded);

    ATLAS_LOG_DEBUG(kScene, "loaded a scene of {} entities at schema version {}", scene.size(),
                    version);
    return ok();
}

}  // namespace atlas::scene
