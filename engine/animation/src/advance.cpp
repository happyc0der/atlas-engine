// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/animation/advance.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/profile.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace atlas::animation {
namespace {

/// A clip's loop mode from the animator's stored byte.
///
/// Stored narrowly because it reaches a file, and converted here rather than at the boundary so
/// that an out-of-range value from a scene somebody hand-edited plays once instead of reading
/// off the end of a switch.
[[nodiscard]] LoopMode loop_of(std::uint8_t stored) noexcept {
    return stored < static_cast<std::uint8_t>(LoopMode::Count) ? static_cast<LoopMode>(stored)
                                                               : LoopMode::Once;
}

}  // namespace

void ClipCache::insert(assets::AssetId id, Clip clip) {
    m_clips[id] = std::make_shared<const Clip>(std::move(clip));
}

std::size_t ClipCache::finalise_pending(assets::Registry& registry) {
    ATLAS_ASSERT_MAIN_THREAD();
    ATLAS_ZONE_NAMED("ClipCache::finalise_pending");

    std::size_t created = 0;
    for (const auto id : registry.pending_finalisation()) {
        // Skipped rather than relied on: `take_animation_clip` returns nothing for an asset
        // with no clip payload, so removing this line changes the work done and not the result.
        // The same arrangement the texture cache has had since M4, and the same honest caveat
        // the audio device's finaliser records.
        if (id.type() != assets::AssetType::AnimationClip) {
            continue;
        }

        auto decoded = registry.take_animation_clip(id);
        if (!decoded) {
            // Another finaliser took it, or it was taken already. Not an error: the payload is
            // moved out precisely so this is the safe outcome rather than a race.
            continue;
        }

        Clip clip;
        clip.name = std::move(decoded->name);
        clip.duration_ns = decoded->duration_ns;
        clip.grid = FrameGrid{.columns = decoded->columns, .rows = decoded->rows};

        clip.transform_keys.reserve(decoded->transform_keys.size());
        for (const auto& key : decoded->transform_keys) {
            clip.transform_keys.push_back(TransformKey{
                .time_ns = key.time_ns,
                .position_offset = {.x = key.position_x, .y = key.position_y},
                .rotation_offset = key.rotation,
                .scale_factor = {.x = key.scale_x, .y = key.scale_y},
                // The importer refuses a name it does not know, so the index is in range by
                // the time it reaches here. Guarded anyway, because the two enumerations agree
                // by position and nothing but this line and a comment says so.
                .easing = key.easing < static_cast<std::uint8_t>(Easing::Count)
                              ? static_cast<Easing>(key.easing)
                              : Easing::Linear,
            });
        }

        clip.frame_keys.reserve(decoded->frame_keys.size());
        for (const auto& key : decoded->frame_keys) {
            clip.frame_keys.push_back(FrameKey{.time_ns = key.time_ns, .cell = key.cell});
        }

        // Replaces whatever was there. A playing entity's clock lives in its pose and is
        // untouched, so an edited clip is picked up from where playback had reached.
        insert(id, std::move(clip));
        registry.mark_ready(id);
        ++created;
    }
    return created;
}

const Clip* ClipCache::find(assets::AssetId id) const noexcept {
    const auto it = m_clips.find(id);
    return it == m_clips.end() ? nullptr : it->second.get();
}

AdvanceReport advance(scene::Scene& scene, const ClipCache& clips, std::uint64_t dt_ns) {
    ATLAS_ZONE_NAMED("animation::advance");
    ATLAS_ASSERT_MAIN_THREAD();

    AdvanceReport report;

    // Identifier order, because `animators()` sorts. Nothing here depends on the order — no
    // entity reads another's pose — but an order that varies between runs is the kind of thing
    // that makes a difference impossible to bisect later.
    for (const scene::StableId id : scene.animators()) {
        const auto* animator = scene.animator(id);
        if (animator == nullptr) {
            continue;
        }

        const Clip* clip = clips.find(animator->clip);
        if (clip == nullptr || clip->duration_ns == 0) {
            // The identity pose, not the last one. A clip that failed to load then looks like
            // no animation rather than like an animation that has frozen, and the difference
            // between those two is most of the time somebody would spend finding out.
            scene.set_animation_pose(id, scene::AnimationPose{});
            ++report.missing_clips;
            continue;
        }

        scene::AnimationPose pose;
        if (const auto* existing = scene.animation_pose(id)) {
            pose = *existing;
        } else {
            // First frame for this entity: start where the author said, not at zero.
            pose.elapsed_ns = static_cast<std::uint64_t>(animator->start_ms) * 1'000'000ULL;
        }

        if (animator->playing) {
            // Scaled in integers. The step is nanoseconds and the speed is a float, so this is
            // the one place the clock meets floating point; it is rounded to a whole number of
            // nanoseconds immediately, so nothing accumulates a fraction.
            const float speed = std::clamp(animator->speed, 0.0F, 100.0F);
            const auto scaled = static_cast<std::uint64_t>(
                std::llround(static_cast<double>(dt_ns) * static_cast<double>(speed)));
            pose.elapsed_ns += scaled;
        }

        const LoopMode loop = loop_of(animator->loop);
        if (loop == LoopMode::Once && pose.elapsed_ns >= clip->duration_ns) {
            ++report.finished;
        }

        const std::uint64_t time = clip_time(pose.elapsed_ns, clip->duration_ns, loop);
        const Sample sampled = sample(*clip, time);

        pose.position_offset = sampled.position_offset;
        pose.rotation_offset = sampled.rotation_offset;
        pose.scale_factor = sampled.scale_factor;
        pose.frame_uv =
            sampled.has_cell ? std::optional{cell_uv(clip->grid, sampled.cell)} : std::nullopt;

        scene.set_animation_pose(id, pose);
        ++report.advanced;
    }

    return report;
}

}  // namespace atlas::animation
