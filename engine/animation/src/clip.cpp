// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/animation/clip.hpp>

#include <algorithm>
#include <cmath>

namespace atlas::animation {
namespace {

/// Linear blend, exact at both endpoints by construction.
///
/// At `t == 0` this is `1 * a + 0 * b` and at `t == 1` it is `0 * a + 1 * b`, so each end
/// returns its own key for every pair of values, without depending on them.
///
/// The usual form, `a + (b - a) * t`, is exact at zero for the same reason and exact at one for
/// most pairs but not as a guarantee, because it reaches `b` by adding a difference that had to
/// be rounded first. **A mutation swapping the two survives every test here**, and that is
/// recorded rather than papered over: the difference does not show at the magnitudes these
/// clips use. The form is chosen because it needs no such argument, not because a test proves
/// it necessary.
[[nodiscard]] float blend(float a, float b, float t) noexcept {
    return ((1.0F - t) * a) + (t * b);
}

[[nodiscard]] math::Vec2 blend(math::Vec2 a, math::Vec2 b, float t) noexcept {
    return math::Vec2{.x = blend(a.x, b.x, t), .y = blend(a.y, b.y, t)};
}

}  // namespace

std::string_view to_string(LoopMode mode) noexcept {
    switch (mode) {
    case LoopMode::Once: return "once";
    case LoopMode::Loop: return "loop";
    case LoopMode::PingPong: return "ping-pong";
    case LoopMode::Count: break;
    }
    return "unrecognised";
}

std::string_view to_string(Easing easing) noexcept {
    switch (easing) {
    case Easing::Linear: return "linear";
    case Easing::Step: return "step";
    case Easing::QuadraticIn: return "quadratic-in";
    case Easing::QuadraticOut: return "quadratic-out";
    case Easing::QuadraticInOut: return "quadratic-in-out";
    case Easing::CubicIn: return "cubic-in";
    case Easing::CubicOut: return "cubic-out";
    case Easing::CubicInOut: return "cubic-in-out";
    case Easing::Count: break;
    }
    return "unrecognised";
}

float ease(Easing easing, float t) noexcept {
    const float x = std::clamp(t, 0.0F, 1.0F);
    switch (easing) {
    case Easing::Linear: return x;
    // Step holds this key's value for the whole segment, so it is zero until the next key is
    // reached. The endpoint is exactly one because the next segment starts there, not because
    // this one arrives.
    case Easing::Step: return x >= 1.0F ? 1.0F : 0.0F;
    case Easing::QuadraticIn: return x * x;
    case Easing::QuadraticOut: return 1.0F - ((1.0F - x) * (1.0F - x));
    case Easing::QuadraticInOut:
        return x < 0.5F ? 2.0F * x * x : 1.0F - (2.0F * (1.0F - x) * (1.0F - x));
    case Easing::CubicIn: return x * x * x;
    case Easing::CubicOut: {
        const float inverted = 1.0F - x;
        return 1.0F - (inverted * inverted * inverted);
    }
    case Easing::CubicInOut: {
        if (x < 0.5F) {
            return 4.0F * x * x * x;
        }
        const float inverted = 1.0F - x;
        return 1.0F - (4.0F * inverted * inverted * inverted);
    }
    case Easing::Count: break;
    }
    return x;
}

std::uint64_t clip_time(std::uint64_t elapsed_ns, std::uint64_t duration_ns,
                        LoopMode loop) noexcept {
    if (duration_ns == 0) {
        return 0;
    }
    if (elapsed_ns < duration_ns) {
        return elapsed_ns;
    }

    switch (loop) {
    case LoopMode::Once:
        // Held at the end, not one nanosecond past it: the last key's value is what a
        // finished clip shows, for ever.
        return duration_ns;
    case LoopMode::Loop:
        // Modulo rather than repeated subtraction, so this is exact however long the clock has
        // been running, and constant time after a pause of any length.
        return elapsed_ns % duration_ns;
    case LoopMode::PingPong: {
        // Two durations make a there-and-back cycle. The second half is the first reversed,
        // and the turning points land exactly on the ends rather than one step past them.
        const std::uint64_t cycle = duration_ns * 2;
        const std::uint64_t within = elapsed_ns % cycle;
        return within < duration_ns ? within : cycle - within;
    }
    case LoopMode::Count: break;
    }
    return elapsed_ns % duration_ns;
}

Sample sample(const Clip& clip, std::uint64_t time_ns) noexcept {
    Sample result;

    if (!clip.transform_keys.empty()) {
        const auto& keys = clip.transform_keys;
        if (time_ns <= keys.front().time_ns) {
            result.position_offset = keys.front().position_offset;
            result.rotation_offset = keys.front().rotation_offset;
            result.scale_factor = keys.front().scale_factor;
        } else if (time_ns >= keys.back().time_ns) {
            result.position_offset = keys.back().position_offset;
            result.rotation_offset = keys.back().rotation_offset;
            result.scale_factor = keys.back().scale_factor;
        } else {
            // The first key strictly after the time, so the one before it is the segment's
            // start. Keys are ordered and the import refuses them otherwise, which is what
            // makes a binary search correct here rather than merely fast.
            const auto next = std::ranges::upper_bound(
                keys, time_ns, {}, [](const TransformKey& key) { return key.time_ns; });
            const auto current = std::prev(next);

            const std::uint64_t span = next->time_ns - current->time_ns;
            // Exactly on a key, or a zero-length segment the import allowed: take the key.
            const float t = span == 0 ? 0.0F
                                      : static_cast<float>(time_ns - current->time_ns) /
                                            static_cast<float>(span);
            const float eased = ease(current->easing, t);

            result.position_offset = blend(current->position_offset, next->position_offset, eased);
            result.rotation_offset = blend(current->rotation_offset, next->rotation_offset, eased);
            result.scale_factor = blend(current->scale_factor, next->scale_factor, eased);
        }
    }

    if (!clip.frame_keys.empty()) {
        // A frame track never interpolates: a sprite sheet shows one cell or another, and
        // there is nothing between two drawings. The key in force is the last one reached.
        const auto& keys = clip.frame_keys;
        const auto next = std::ranges::upper_bound(keys, time_ns, {},
                                                   [](const FrameKey& key) { return key.time_ns; });
        const auto current = next == keys.begin() ? keys.begin() : std::prev(next);
        result.cell = current->cell;
        result.has_cell = true;
    }

    return result;
}

math::Rect cell_uv(const FrameGrid& grid, std::uint32_t cell) noexcept {
    if (grid.columns == 0 || grid.rows == 0) {
        return math::Rect{.position = {.x = 0.0F, .y = 0.0F}, .size = {.x = 1.0F, .y = 1.0F}};
    }

    const std::uint32_t count = grid.columns * grid.rows;
    // Wrapped rather than clamped or refused. A cell past the end of the grid is a clip that
    // disagrees with its sheet, which import already refuses; if one reaches here anyway,
    // showing the wrong frame is a better failure than reading outside the texture.
    const std::uint32_t index = count == 0 ? 0 : cell % count;
    const std::uint32_t column = index % grid.columns;
    const std::uint32_t row = index / grid.columns;

    const float width = 1.0F / static_cast<float>(grid.columns);
    const float height = 1.0F / static_cast<float>(grid.rows);
    return math::Rect{
        .position = {.x = static_cast<float>(column) * width,
                     .y = static_cast<float>(row) * height},
        .size = {.x = width, .y = height},
    };
}

}  // namespace atlas::animation
