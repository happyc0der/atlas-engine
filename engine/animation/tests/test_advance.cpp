// SPDX-License-Identifier: GPL-3.0-or-later
// Advancing a scene, which is where clip evaluation meets the two-writer split.
//
// No clock anywhere: the step is whatever the test hands over, which is what lets a thousand
// frames run in no time and give the same answer twice.
#include <atlas/animation/advance.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/scene/serialization.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using atlas::animation::advance;
using atlas::animation::Clip;
using atlas::animation::ClipCache;
using atlas::animation::LoopMode;
using atlas::animation::TransformKey;
using atlas::assets::AssetId;
using atlas::assets::AssetType;
using atlas::scene::Animator;
using atlas::scene::LocalTransform;
using atlas::scene::Scene;
using atlas::scene::StableId;
using Catch::Approx;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000ULL;
constexpr std::uint64_t kFrameNs = 16'666'667ULL;

/// The scene asserts main-thread affinity on every mutation, and nothing here creates a
/// platform to claim it. Same pattern as the asset and audio tests.
const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

[[nodiscard]] AssetId clip_id() {
    return AssetId::from("animation/test.clip.json", AssetType::Unknown);
}

/// A clip that slides ten to the right over one second.
[[nodiscard]] Clip slide() {
    Clip clip;
    clip.duration_ns = kSecond;
    clip.transform_keys = {
        TransformKey{.time_ns = 0, .position_offset = {.x = 0.0F, .y = 0.0F}},
        TransformKey{.time_ns = kSecond, .position_offset = {.x = 10.0F, .y = 0.0F}},
    };
    return clip;
}

/// A scene with one animated entity at the origin.
struct Fixture {
    Scene scene;
    ClipCache clips;
    StableId id;

    Fixture() : id(scene.create("thing")) {
        scene.set_local_transform(id, LocalTransform{});
        scene.set_animator(id, Animator{.clip = clip_id(),
                                        .speed = 1.0F,
                                        .playing = true,
                                        .loop = static_cast<std::uint8_t>(LoopMode::Loop)});
        clips.insert(clip_id(), slide());
    }

    [[nodiscard]] float offset() const {
        const auto* pose = scene.animation_pose(id);
        return pose == nullptr ? 0.0F : pose->position_offset.x;
    }
};

}  // namespace

TEST_CASE("advancing writes a pose and nothing else", "[animation][advance]") {
    Fixture fixture;
    const auto before = atlas::scene::to_text(fixture.scene);
    REQUIRE(before.has_value());

    const auto report = advance(fixture.scene, fixture.clips, kSecond / 2);
    CHECK(report.advanced == 1);
    CHECK(report.missing_clips == 0);
    CHECK(fixture.offset() == Approx(5.0F).margin(1e-3));

    // The authored scene is untouched. This is the property the whole design exists for, and
    // it is asserted here as well as in the scene's own tests because this is the code that
    // would break it.
    const auto after = atlas::scene::to_text(fixture.scene);
    REQUIRE(after.has_value());
    CHECK(*after == *before);
}

TEST_CASE("the same steps give the same poses twice", "[animation][advance]") {
    // Not a determinism claim about the engine — this is presentation and is hashed nowhere.
    // It is what makes a recorded table of poses a usable acceptance check at all.
    const auto run = [] {
        Fixture fixture;
        std::vector<float> offsets;
        for (int frame = 0; frame < 300; ++frame) {
            advance(fixture.scene, fixture.clips, kFrameNs);
            offsets.push_back(fixture.offset());
        }
        return offsets;
    };
    CHECK(run() == run());
}

TEST_CASE("a paused animator does not move", "[animation][advance]") {
    Fixture fixture;
    advance(fixture.scene, fixture.clips, kSecond / 2);
    const float moved = fixture.offset();

    auto paused = *fixture.scene.animator(fixture.id);
    paused.playing = false;
    fixture.scene.set_animator(fixture.id, paused);

    for (int frame = 0; frame < 100; ++frame) {
        advance(fixture.scene, fixture.clips, kFrameNs);
    }
    // Still posed, still where it was. A paused clip holds rather than clearing, because the
    // thing a person pauses is meant to stay on screen.
    CHECK(fixture.offset() == Approx(moved).margin(1e-4));
}

TEST_CASE("speed scales the passage of time", "[animation][advance]") {
    Fixture single;
    advance(single.scene, single.clips, kSecond / 4);

    Fixture doubled;
    auto faster = *doubled.scene.animator(doubled.id);
    faster.speed = 2.0F;
    doubled.scene.set_animator(doubled.id, faster);
    advance(doubled.scene, doubled.clips, kSecond / 8);

    // Half the step at twice the speed reaches the same place.
    CHECK(doubled.offset() == Approx(single.offset()).margin(1e-3));
}

TEST_CASE("a speed of zero freezes without pausing", "[animation][advance]") {
    Fixture fixture;
    auto frozen = *fixture.scene.animator(fixture.id);
    frozen.speed = 0.0F;
    fixture.scene.set_animator(fixture.id, frozen);

    for (int frame = 0; frame < 100; ++frame) {
        advance(fixture.scene, fixture.clips, kFrameNs);
    }
    CHECK(fixture.offset() == Approx(0.0F).margin(1e-5));
}

TEST_CASE("speed is clamped at both ends", "[animation][advance]") {
    // The upper half is what this build can see. A speed of two hundred behaves like a
    // hundred, so the clamp is observable rather than merely present.
    Fixture fast;
    auto quick = *fast.scene.animator(fast.id);
    quick.speed = 200.0F;
    fast.scene.set_animator(fast.id, quick);
    advance(fast.scene, fast.clips, kSecond / 1000);

    Fixture capped;
    auto limit = *capped.scene.animator(capped.id);
    limit.speed = 100.0F;
    capped.scene.set_animator(capped.id, limit);
    advance(capped.scene, capped.clips, kSecond / 1000);

    CHECK(fast.offset() == Approx(capped.offset()).margin(1e-4));

    // The lower half cannot be seen here, and the reason is worth writing down. Reverse
    // playback is a different feature with a different name; what a negative speed would
    // actually do is multiply the step by it and convert the negative result to an unsigned
    // counter, and **converting a negative floating-point value to an unsigned integer is
    // undefined**. On this machine it saturates to zero, which is the same answer the clamp
    // gives, so removing the clamp changes nothing this assertion can observe.
    //
    // What does observe it is the undefined-behaviour sanitizer, which aborts on the
    // unclamped version. That was verified by removing the clamp and running this case under
    // the sanitizer preset. So the guard is covered — by a build, not by an assertion.
    Fixture reversed_fixture;
    auto reversed = *reversed_fixture.scene.animator(reversed_fixture.id);
    reversed.speed = -4.0F;
    reversed_fixture.scene.set_animator(reversed_fixture.id, reversed);

    for (int frame = 0; frame < 10; ++frame) {
        advance(reversed_fixture.scene, reversed_fixture.clips, kFrameNs);
    }
    CHECK(reversed_fixture.offset() == Approx(0.0F).margin(1e-5));
}

TEST_CASE("playback starts where the author said", "[animation][advance]") {
    Fixture fixture;
    auto started = *fixture.scene.animator(fixture.id);
    started.start_ms = 500;  // half way along a one-second clip
    fixture.scene.set_animator(fixture.id, started);

    // The first advance begins from the authored start rather than from zero, so a scrub takes
    // effect on the next frame and not a whole clip later.
    advance(fixture.scene, fixture.clips, 0);
    CHECK(fixture.offset() == Approx(5.0F).margin(1e-3));
}

TEST_CASE("a missing clip gives the identity pose, not the last one", "[animation][advance]") {
    // A clip that failed to load must look like no animation, rather than like an animation
    // that has frozen. Telling those two apart afterwards is most of the time somebody would
    // spend finding out what went wrong.
    Fixture fixture;
    advance(fixture.scene, fixture.clips, kSecond / 2);
    REQUIRE(fixture.offset() > 1.0F);

    fixture.clips.clear();
    const auto report = advance(fixture.scene, fixture.clips, kFrameNs);

    CHECK(report.missing_clips == 1);
    CHECK(report.advanced == 0);
    CHECK(fixture.offset() == 0.0F);
    // Still posed, so the pose is the identity rather than absent: an entity that was
    // animating does not suddenly lose a component it had.
    CHECK(fixture.scene.animation_pose(fixture.id) != nullptr);
}

TEST_CASE("a clip that does not loop finishes and is counted", "[animation][advance]") {
    Fixture fixture;
    auto once = *fixture.scene.animator(fixture.id);
    once.loop = static_cast<std::uint8_t>(LoopMode::Once);
    fixture.scene.set_animator(fixture.id, once);

    advance(fixture.scene, fixture.clips, kSecond / 2);
    CHECK(advance(fixture.scene, fixture.clips, kSecond / 2).finished == 1);
    CHECK(fixture.offset() == Approx(10.0F).margin(1e-3));

    // And stays there, however long it runs on.
    for (int frame = 0; frame < 100; ++frame) {
        advance(fixture.scene, fixture.clips, kFrameNs);
    }
    CHECK(fixture.offset() == Approx(10.0F).margin(1e-3));
}

TEST_CASE("an out-of-range loop mode plays once", "[animation][advance]") {
    // The stored value reaches a file, so a hand-edited scene can carry anything. Playing once
    // is the safe reading: it stops, rather than looping something that was never meant to.
    Fixture fixture;
    auto odd = *fixture.scene.animator(fixture.id);
    odd.loop = 99;
    fixture.scene.set_animator(fixture.id, odd);

    advance(fixture.scene, fixture.clips, 2 * kSecond);
    CHECK(fixture.offset() == Approx(10.0F).margin(1e-3));
}

TEST_CASE("a scene with nothing animated is left alone", "[animation][advance]") {
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id, LocalTransform{});
    const ClipCache clips;

    const auto report = advance(scene, clips, kSecond);
    CHECK(report.advanced == 0);
    CHECK(report.missing_clips == 0);
    CHECK(scene.animation_pose(id) == nullptr);
}

TEST_CASE("the cache replaces a clip rather than accumulating", "[animation][advance]") {
    ClipCache clips;
    clips.insert(clip_id(), slide());
    CHECK(clips.size() == 1);

    Clip replacement = slide();
    replacement.transform_keys.back().position_offset.x = 99.0F;
    clips.insert(clip_id(), std::move(replacement));
    CHECK(clips.size() == 1);

    const auto* found = clips.find(clip_id());
    REQUIRE(found != nullptr);
    CHECK(found->transform_keys.back().position_offset.x == 99.0F);
    CHECK(clips.find(AssetId::from("nothing", AssetType::Unknown)) == nullptr);
}

TEST_CASE("a reload picks up from where the clock had reached", "[animation][advance]") {
    // Editing a clip while watching it should change what happens next, not restart it. That
    // is what an author expects, and it falls out of keeping the clock in the pose rather than
    // in the clip.
    Fixture fixture;
    advance(fixture.scene, fixture.clips, kSecond / 2);
    const auto* pose = fixture.scene.animation_pose(fixture.id);
    REQUIRE(pose != nullptr);
    const std::uint64_t elapsed = pose->elapsed_ns;

    Clip edited = slide();
    edited.transform_keys.back().position_offset.x = 100.0F;
    fixture.clips.insert(clip_id(), std::move(edited));

    advance(fixture.scene, fixture.clips, 0);
    CHECK(fixture.scene.animation_pose(fixture.id)->elapsed_ns == elapsed);
    CHECK(fixture.offset() == Approx(50.0F).margin(1e-2));
}
