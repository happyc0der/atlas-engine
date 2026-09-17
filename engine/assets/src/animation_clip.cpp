// SPDX-License-Identifier: GPL-3.0-or-later
//
// The animation clip reader.
//
// Its own translation unit, away from `importer.cpp`, which compiles the image decoder's
// single-header implementation: in M12 one unrelated function in there was enough to make the
// static analyser walk further into that third-party code and report a leak inside it.
//
// The discipline mirrors `engine/scene/src/serialization.cpp`, which is the project's other
// document reader, with one addition it does not have: the input's own length is bounded before
// the parse is attempted. See the header for why that is the bound that matters here.

#include "animation_clip.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace atlas::assets::detail {
namespace {

/// Ordered, for the same reason the scene reader uses it: a clip is read and written by people,
/// and key order is part of a file being canonical.
using Json = nlohmann::ordered_json;

constexpr std::string_view kFormatMarker = "atlas-animation-clip";
constexpr std::uint32_t kClipFormatVersion = 1;
constexpr std::uint32_t kMinClipFormatVersion = 1;

/// The easing names, in the animation module's own declaration order.
///
/// The two agree by position, and this is the only place that is true, which is why the array
/// is here rather than spread across the parser. A name this does not know is refused: guessing
/// would play a clip differently from what its file says while looking as though it worked.
constexpr std::array<std::string_view, 8> kEasingNames{
    "linear",           "step",     "quadratic-in", "quadratic-out",
    "quadratic-in-out", "cubic-in", "cubic-out",    "cubic-in-out",
};

[[nodiscard]] std::optional<std::uint8_t> easing_value(std::string_view name) {
    for (std::size_t i = 0; i < kEasingNames.size(); ++i) {
        if (kEasingNames[i] == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

/// A number that is present, numeric, and finite.
///
/// The finiteness check is the one the scene reader's own helper does not do. JSON has no
/// syntax for a non-finite number, so this cannot trip on a well-formed document — but a value
/// large enough to become infinity when narrowed to a float can, and an infinite offset makes
/// every matrix it touches meaningless rather than merely wrong.
[[nodiscard]] Result<float> read_float(const Json& parent, const char* key, float fallback,
                                       std::string_view what) {
    if (!parent.contains(key)) {
        return fallback;
    }
    if (!parent[key].is_number()) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("{} is not a number", what)));
    }
    const auto value = parent[key].get<float>();
    if (!std::isfinite(value)) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("{} is not a finite number", what)));
    }
    return value;
}

}  // namespace

Result<ImportedAnimationClip> parse_animation_clip(std::span<const std::byte> bytes,
                                                   std::string_view debug_name) {
    const auto refuse = [&debug_name](std::string message) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("'{}': {}", debug_name, message)));
    };

    const ImportLimits& limits = import_limits();

    // Before the parse, not after it. This is the whole of what a document parser can do about
    // an enormous input: once nlohmann has been handed the text it has already allocated.
    if (bytes.size() > limits.max_clip_bytes) {
        return refuse(
            std::format("is {} bytes, past the limit of {}", bytes.size(), limits.max_clip_bytes));
    }

    // A view over the same bytes, because the parser takes characters. `as_bytes` is the
    // permitted direction and there is no `as_chars`, so this is the one place the cast is
    // unavoidable; every byte of it has already been bounded above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    // Non-throwing, because ADR-0005 forbids exceptions crossing a module boundary. Comments
    // are permitted, matching the scene reader: a clip is a file somebody tunes by hand, and a
    // note beside a key is exactly what they would write.
    const Json root = Json::parse(text, nullptr, false, true);
    if (root.is_discarded()) {
        return refuse("is not valid JSON");
    }
    if (!root.is_object()) {
        return refuse("is not a JSON object");
    }

    if (!root.contains("format") || !root["format"].is_string() ||
        root["format"].get<std::string>() != kFormatMarker) {
        return refuse(std::format("is not an animation clip: the format marker is missing or is "
                                  "not '{}'",
                                  kFormatMarker));
    }
    if (!root.contains("version") || !root["version"].is_number_unsigned()) {
        return refuse("has no schema version");
    }
    const auto version = root["version"].get<std::uint64_t>();
    if (version > kClipFormatVersion) {
        return refuse(std::format("is version {}, and this build understands up to {}. Use a "
                                  "newer build.",
                                  version, kClipFormatVersion));
    }
    if (version < kMinClipFormatVersion) {
        return refuse(std::format("is version {}, and this build reads no older than {}", version,
                                  kMinClipFormatVersion));
    }

    ImportedAnimationClip clip;
    clip.name = root.value("name", std::string{});

    if (!root.contains("duration_ms") || !root["duration_ms"].is_number_unsigned()) {
        return refuse("has no duration");
    }
    const auto duration_ms = root["duration_ms"].get<std::uint64_t>();
    if (duration_ms == 0) {
        return refuse("has a duration of zero, so there is nothing to play");
    }
    if (duration_ms > limits.max_clip_duration_ms) {
        return refuse(std::format("lasts {} ms, past the limit of {}", duration_ms,
                                  limits.max_clip_duration_ms));
    }
    clip.duration_ns = duration_ms * 1'000'000ULL;

    if (root.contains("grid")) {
        const Json& grid = root["grid"];
        if (!grid.is_object()) {
            return refuse("has a grid that is not an object");
        }
        if (!grid.contains("columns") || !grid["columns"].is_number_unsigned() ||
            !grid.contains("rows") || !grid["rows"].is_number_unsigned()) {
            return refuse("has a grid without whole columns and rows");
        }
        const auto columns = grid["columns"].get<std::uint64_t>();
        const auto rows = grid["rows"].get<std::uint64_t>();
        if (columns == 0 || rows == 0 || columns > limits.max_clip_grid ||
            rows > limits.max_clip_grid) {
            return refuse(std::format("has a {} by {} grid, outside [1, {}]", columns, rows,
                                      limits.max_clip_grid));
        }
        clip.columns = static_cast<std::uint32_t>(columns);
        clip.rows = static_cast<std::uint32_t>(rows);
    }

    // Each track is optional and each is bounded before anything is reserved for it.
    const auto bounded_array = [&](const char* key) -> Result<const Json*> {
        if (!root.contains(key)) {
            return nullptr;
        }
        if (!root[key].is_array()) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("'{}': the {} track is not an array", debug_name, key)));
        }
        if (root[key].size() > limits.max_clip_keys) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("'{}': the {} track has {} keys, past the limit of {}",
                                  debug_name, key, root[key].size(), limits.max_clip_keys)));
        }
        return &root[key];
    };

    auto transform_track = bounded_array("transform");
    if (!transform_track) {
        return std::unexpected(transform_track.error());
    }
    auto frame_track = bounded_array("frames");
    if (!frame_track) {
        return std::unexpected(frame_track.error());
    }

    if (*transform_track == nullptr && *frame_track == nullptr) {
        return refuse("has neither a transform track nor a frame track, so it animates nothing");
    }

    if (*transform_track != nullptr) {
        clip.transform_keys.reserve((*transform_track)->size());
        for (const Json& node : **transform_track) {
            if (!node.is_object()) {
                return refuse("has a transform key that is not an object");
            }
            if (!node.contains("time_ms") || !node["time_ms"].is_number_unsigned()) {
                return refuse("has a transform key without a whole millisecond time");
            }
            const auto time_ms = node["time_ms"].get<std::uint64_t>();
            if (time_ms > duration_ms) {
                return refuse(std::format("has a transform key at {} ms, past its own duration "
                                          "of {}",
                                          time_ms, duration_ms));
            }

            ImportedTransformKey key;
            key.time_ns = time_ms * 1'000'000ULL;

            const auto read = [&](const char* name, float fallback, float& out) -> Status {
                auto value =
                    read_float(node, name, fallback,
                               std::format("'{}': a transform key's {}", debug_name, name));
                if (!value) {
                    return std::unexpected(value.error());
                }
                out = *value;
                return ok();
            };
            if (auto status = read("x", 0.0F, key.position_x); !status) {
                return std::unexpected(status.error());
            }
            if (auto status = read("y", 0.0F, key.position_y); !status) {
                return std::unexpected(status.error());
            }
            if (auto status = read("rotation", 0.0F, key.rotation); !status) {
                return std::unexpected(status.error());
            }
            if (auto status = read("scale_x", 1.0F, key.scale_x); !status) {
                return std::unexpected(status.error());
            }
            if (auto status = read("scale_y", 1.0F, key.scale_y); !status) {
                return std::unexpected(status.error());
            }

            const auto easing_name = node.value("easing", std::string{"linear"});
            const auto easing = easing_value(easing_name);
            if (!easing.has_value()) {
                return refuse(std::format("has a transform key easing '{}', which is not one this "
                                          "build knows",
                                          easing_name));
            }
            key.easing = *easing;

            // Ordered, and the first at zero. Both are claims the animation module's own header
            // already makes about a clip, and until now nothing enforced either: sampling
            // binary-searches the keys, so an unordered track does not fail, it silently
            // returns the wrong pose.
            if (!clip.transform_keys.empty() && key.time_ns < clip.transform_keys.back().time_ns) {
                return refuse("has transform keys out of order");
            }
            clip.transform_keys.push_back(key);
        }

        if (!clip.transform_keys.empty() && clip.transform_keys.front().time_ns != 0) {
            return refuse("has a transform track whose first key is not at zero");
        }
    }

    if (*frame_track != nullptr) {
        const std::uint64_t cells = static_cast<std::uint64_t>(clip.columns) * clip.rows;
        clip.frame_keys.reserve((*frame_track)->size());
        for (const Json& node : **frame_track) {
            if (!node.is_object()) {
                return refuse("has a frame key that is not an object");
            }
            if (!node.contains("time_ms") || !node["time_ms"].is_number_unsigned() ||
                !node.contains("cell") || !node["cell"].is_number_unsigned()) {
                return refuse("has a frame key without a whole time and cell");
            }
            const auto time_ms = node["time_ms"].get<std::uint64_t>();
            const auto cell = node["cell"].get<std::uint64_t>();
            if (time_ms > duration_ms) {
                return refuse(std::format("has a frame key at {} ms, past its own duration of {}",
                                          time_ms, duration_ms));
            }
            // Refused rather than wrapped. The sampler wraps, because showing the wrong frame
            // beats reading outside a texture — but a clip that names a cell its own grid does
            // not have is a clip that disagrees with itself, and that is worth saying here
            // rather than drawing.
            if (cell >= cells) {
                return refuse(
                    std::format("has a frame key for cell {}, and its grid has {}", cell, cells));
            }

            ImportedFrameKey key;
            key.time_ns = time_ms * 1'000'000ULL;
            key.cell = static_cast<std::uint32_t>(cell);

            if (!clip.frame_keys.empty() && key.time_ns < clip.frame_keys.back().time_ns) {
                return refuse("has frame keys out of order");
            }
            clip.frame_keys.push_back(key);
        }

        if (!clip.frame_keys.empty() && clip.frame_keys.front().time_ns != 0) {
            return refuse("has a frame track whose first key is not at zero");
        }
    }

    return clip;
}

}  // namespace atlas::assets::detail
