// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What a clip is once it has been read, and how it is sampled.
///
/// The evaluation is pure functions over plain data, separated from anything that owns a scene
/// for the same reason the mixer was separated from the audio device in M12 and the camera
/// arithmetic from the platform in M11: the part that can be wrong is arithmetic, and
/// arithmetic tested through a scene is tested at one remove from where it lives.
///
/// **Time is integer nanoseconds throughout.** Milliseconds in the file, because that is what a
/// person tunes and what every sprite tool exports; nanoseconds in memory, because both
/// applications already produce a frame time in nanoseconds and truncating a 16.67 ms frame to
/// milliseconds loses two-thirds of a millisecond every frame. Integer at both ends, so a clip
/// looping for an hour is exact at the end of it — there is no float accumulation anywhere in
/// the clock, which is what makes the question of wrapping versus accumulating moot.
///
/// **Thread affinity: none.** Nothing here touches shared state.

#include <atlas/math/vector.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::animation {

/// What happens when a clip reaches its end.
enum class LoopMode : std::uint8_t {
    /// Hold the last keys for ever.
    Once,
    /// Start again from the beginning.
    Loop,
    /// Run backwards to the beginning, then forwards again.
    PingPong,
    Count,
};

[[nodiscard]] std::string_view to_string(LoopMode mode) noexcept;

/// How a key reaches the next one.
///
/// A fixed set rather than arbitrary curves. Every one of these is exact at both endpoints —
/// `ease(0) == 0` and `ease(1) == 1` to the bit — which is what lets a test assert a key's own
/// value with `==` rather than with a tolerance, and what stops a clip drifting away from the
/// pose its author typed.
enum class Easing : std::uint8_t {
    /// Straight line.
    Linear,
    /// Hold this key's value until the next one. For anything that should snap.
    Step,
    QuadraticIn,
    QuadraticOut,
    QuadraticInOut,
    CubicIn,
    CubicOut,
    CubicInOut,
    Count,
};

[[nodiscard]] std::string_view to_string(Easing easing) noexcept;

/// Apply an easing to a normalised position along a segment.
///
/// `t` outside [0, 1] is clamped rather than extrapolated: a curve continued past its endpoints
/// is a different curve, and every caller here has already bounded it.
[[nodiscard]] float ease(Easing easing, float t) noexcept;

/// One key on the transform track.
///
/// The values are **offsets from the authored pose**, not absolute placements: position and
/// rotation are added to what the author typed and scale is multiplied by it. So a clip is a
/// description of movement rather than of position, and the same clip can be attached to two
/// entities in different places.
struct TransformKey {
    /// When this key is reached, from the start of the clip.
    std::uint64_t time_ns = 0;
    math::Vec2 position_offset;
    /// Radians, and **not** wrapped to a turn: a key at two pi means a full revolution, not
    /// zero. Wrapping would make a spin impossible to express.
    float rotation_offset = 0.0F;
    math::Vec2 scale_factor{.x = 1.0F, .y = 1.0F};
    /// How this key reaches the next one. The last key's easing is unused.
    Easing easing = Easing::Linear;
};

/// One key on the frame track, addressing a cell of a uniform grid.
struct FrameKey {
    std::uint64_t time_ns = 0;
    /// Index into the sheet, counted left to right then top to bottom.
    std::uint32_t cell = 0;
};

/// How a sheet is divided. Frames address cells of a uniform grid.
///
/// A grid rather than normalised rectangles or pixel rectangles. Normalised rectangles are
/// hostile to write by hand and change meaning if the sheet is re-exported at another size;
/// pixel rectangles need a texture size, which this module never sees because it does not know
/// what a texture is. A grid needs neither and is what a person would write.
struct FrameGrid {
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
};

/// A clip, ready to sample.
struct Clip {
    std::string name;
    /// Total length. A clip shorter than its last key is refused at import.
    std::uint64_t duration_ns = 0;
    FrameGrid grid;
    /// Ordered by time, first key at zero. Either track may be empty.
    std::vector<TransformKey> transform_keys;
    std::vector<FrameKey> frame_keys;

    [[nodiscard]] bool empty() const noexcept {
        return transform_keys.empty() && frame_keys.empty();
    }
};

/// What sampling a clip produced.
struct Sample {
    math::Vec2 position_offset;
    float rotation_offset = 0.0F;
    math::Vec2 scale_factor{.x = 1.0F, .y = 1.0F};
    /// The frame cell, when the clip has a frame track.
    std::uint32_t cell = 0;
    bool has_cell = false;
};

/// Where a clock sits inside a clip, once looping has been accounted for.
///
/// Separated from sampling so it can be tested on its own: the mapping from elapsed time to a
/// position inside the clip is where every looping bug lives, and it needs no keys at all.
[[nodiscard]] std::uint64_t clip_time(std::uint64_t elapsed_ns, std::uint64_t duration_ns,
                                      LoopMode loop) noexcept;

/// Sample `clip` at `time_ns`, which must already be inside the clip.
///
/// Before the first key, the first key's value. After the last, the last key's. An empty track
/// contributes its identity: zero offsets and a scale of one, so a clip with only a frame track
/// leaves the transform alone.
[[nodiscard]] Sample sample(const Clip& clip, std::uint64_t time_ns) noexcept;

/// The normalised rectangle of `cell` in a grid, or the whole texture for an empty grid.
[[nodiscard]] math::Rect cell_uv(const FrameGrid& grid, std::uint32_t cell) noexcept;

}  // namespace atlas::animation
