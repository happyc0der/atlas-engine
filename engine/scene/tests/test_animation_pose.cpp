// SPDX-License-Identifier: GPL-3.0-or-later
// The pose a clip writes, and the line between it and what a person authored.
//
// This is the whole of the two-writer arrangement, tested before anything writes a pose for
// real. What it has to establish is not that composition works — the hierarchy tests already
// cover that — but that the two writers never reach each other's field, and that playback is
// invisible to the file.
#include <atlas/scene/scene.hpp>
#include <atlas/scene/serialization.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using atlas::scene::AnimationPose;
using atlas::scene::LocalTransform;
using atlas::scene::Scene;
using atlas::scene::SpriteRenderData;
using atlas::scene::StableId;
using Catch::Approx;

namespace {

/// The composed translation of an entity, which is what a draw path reads.
[[nodiscard]] std::pair<float, float> world_position(const Scene& scene, StableId id) {
    const auto* world = scene.world_transform(id);
    REQUIRE(world != nullptr);
    return {world->matrix.at(0, 3), world->matrix.at(1, 3)};
}

}  // namespace

TEST_CASE("an entity with no pose composes exactly as before", "[scene][animation]") {
    // The identity case, and the one that would catch a pose applied where none exists — an
    // additive scale, say, which would shrink every unanimated entity to nothing.
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id, LocalTransform{.position = {.x = 3.0F, .y = 4.0F}});
    scene.update_transforms();

    const auto [x, y] = world_position(scene, id);
    CHECK(x == Approx(3.0F));
    CHECK(y == Approx(4.0F));
    CHECK(scene.animation_pose(id) == nullptr);
    CHECK(scene.animated().empty());
}

TEST_CASE("a pose offsets the authored transform without changing it", "[scene][animation]") {
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id, LocalTransform{.position = {.x = 10.0F, .y = 0.0F}});
    scene.set_animation_pose(id, AnimationPose{.position_offset = {.x = 5.0F, .y = -2.0F}});
    scene.update_transforms();

    const auto [x, y] = world_position(scene, id);
    CHECK(x == Approx(15.0F));
    CHECK(y == Approx(-2.0F));

    // The authored value is untouched. This is what the inspector shows and what a save
    // records, and a design that wrote through it would fail here.
    const auto* local = scene.local_transform(id);
    REQUIRE(local != nullptr);
    CHECK(local->position.x == Approx(10.0F));
    CHECK(local->position.y == Approx(0.0F));
}

TEST_CASE("an identity pose leaves an entity exactly where it was", "[scene][animation]") {
    // A default-constructed pose must be the identity in all three channels, which is why
    // scale multiplies and the other two add. Getting that backwards is silent and total.
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id, LocalTransform{.position = {.x = 7.0F, .y = -3.0F},
                                                 .rotation = 0.5F,
                                                 .scale = {.x = 2.0F, .y = 3.0F}});
    scene.update_transforms();
    const auto [before_x, before_y] = world_position(scene, id);
    const auto* before = scene.world_transform(id);
    const float before_00 = before->matrix.at(0, 0);

    scene.set_animation_pose(id, AnimationPose{});
    scene.update_transforms();

    const auto [after_x, after_y] = world_position(scene, id);
    CHECK(after_x == Approx(before_x));
    CHECK(after_y == Approx(before_y));
    CHECK(scene.world_transform(id)->matrix.at(0, 0) == Approx(before_00));
}

TEST_CASE("a pose's scale multiplies and its rotation adds", "[scene][animation]") {
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id,
                              LocalTransform{.rotation = 0.0F, .scale = {.x = 2.0F, .y = 2.0F}});
    scene.set_animation_pose(id, AnimationPose{.rotation_offset = std::numbers::pi_v<float> / 2.0F,
                                               .scale_factor = {.x = 3.0F, .y = 0.5F}});
    scene.update_transforms();

    const auto* world = scene.world_transform(id);
    REQUIRE(world != nullptr);
    // A quarter turn takes the x axis onto the y axis, so the top-left term goes to zero and
    // the one below it carries the whole of the x scale: 2 authored times 3 posed.
    CHECK(world->matrix.at(0, 0) == Approx(0.0F).margin(1e-5));
    CHECK(world->matrix.at(1, 0) == Approx(6.0F).margin(1e-5));
    // And the y scale is 2 times a half.
    CHECK(world->matrix.at(0, 1) == Approx(-1.0F).margin(1e-5));
}

TEST_CASE("a posed parent moves its children", "[scene][animation]") {
    // The property the sandbox's demonstration exists to show, now driven by a pose rather
    // than by writing the parent's authored position.
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId child = scene.create("child");
    REQUIRE(scene.set_parent(child, parent).has_value());
    scene.set_local_transform(child, LocalTransform{.position = {.x = 4.0F, .y = 0.0F}});
    scene.update_transforms();
    const auto [before_x, before_y] = world_position(scene, child);

    scene.set_animation_pose(parent, AnimationPose{.position_offset = {.x = 0.0F, .y = 9.0F}});
    scene.update_transforms();

    const auto [after_x, after_y] = world_position(scene, child);
    CHECK(after_x == Approx(before_x));
    CHECK(after_y == Approx(before_y + 9.0F));
}

TEST_CASE("an edit and a pose coexist on the same entity", "[scene][animation]") {
    // The case the whole design is for, and the one a replacement-style animator cannot pass:
    // dragging an entity while it plays moves it by the drag, on top of the playback.
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id, LocalTransform{.position = {.x = 0.0F, .y = 0.0F}});
    scene.set_animation_pose(id, AnimationPose{.position_offset = {.x = 100.0F, .y = 0.0F}});
    scene.update_transforms();
    CHECK(world_position(scene, id).first == Approx(100.0F));

    // The author drags it ten to the right. The pose is untouched.
    scene.set_local_transform(id, LocalTransform{.position = {.x = 10.0F, .y = 0.0F}});
    scene.update_transforms();
    CHECK(world_position(scene, id).first == Approx(110.0F));

    // Playback moves on. The drag is still there.
    scene.set_animation_pose(id, AnimationPose{.position_offset = {.x = 200.0F, .y = 0.0F}});
    scene.update_transforms();
    CHECK(world_position(scene, id).first == Approx(210.0F));
}

TEST_CASE("a pose is invisible to the saved file", "[scene][animation]") {
    // The byte test. Playback must leave no trace in what a save records, or a scene that has
    // been watched for a while stops being the scene that was authored.
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_local_transform(id, LocalTransform{.position = {.x = 1.0F, .y = 2.0F}});
    scene.set_sprite(id, SpriteRenderData{});

    const auto before = atlas::scene::to_text(scene);
    REQUIRE(before.has_value());

    for (int step = 0; step < 100; ++step) {
        scene.set_animation_pose(
            id, AnimationPose{.position_offset = {.x = static_cast<float>(step), .y = 0.0F},
                              .rotation_offset = static_cast<float>(step) * 0.1F,
                              .scale_factor = {.x = 1.0F + static_cast<float>(step), .y = 1.0F},
                              .elapsed_ns = static_cast<std::uint64_t>(step) * 16'666'667ULL});
        scene.update_transforms();
    }

    const auto after = atlas::scene::to_text(scene);
    REQUIRE(after.has_value());
    CHECK(*after == *before);
    CHECK_FALSE(after->contains("pose"));
    CHECK_FALSE(after->contains("elapsed"));
}

TEST_CASE("a pose does not survive a round trip through a file", "[scene][animation]") {
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_animation_pose(id, AnimationPose{.position_offset = {.x = 5.0F, .y = 5.0F}});

    const auto text = atlas::scene::to_text(scene);
    REQUIRE(text.has_value());

    Scene loaded;
    REQUIRE(atlas::scene::from_text(loaded, *text).has_value());
    // Nothing was animating the loaded scene, so nothing in it is posed. A clip starts it
    // again from the beginning, which is what an author expects of a file.
    CHECK(loaded.animated().empty());
}

TEST_CASE("the animated list is ordered and reflects clearing", "[scene][animation]") {
    Scene scene;
    const StableId first = scene.create("a");
    const StableId second = scene.create("b");
    const StableId third = scene.create("c");

    scene.set_animation_pose(third, AnimationPose{});
    scene.set_animation_pose(first, AnimationPose{});
    scene.set_animation_pose(second, AnimationPose{});

    // Identifier order, not the order they were added: the storage's own order is arbitrary
    // and anything observable has to be sorted on the way out.
    const auto animated = scene.animated();
    REQUIRE(animated.size() == 3);
    CHECK(animated[0] == first);
    CHECK(animated[1] == second);
    CHECK(animated[2] == third);

    scene.clear_animation_pose(second);
    CHECK(scene.animated().size() == 2);
    CHECK(scene.animation_pose(second) == nullptr);
}

TEST_CASE("a pose on an entity that does not exist does nothing", "[scene][animation]") {
    // Matching every other setter on Scene: silently nothing for a missing entity, because
    // validation is the edit layer's job. A pose is not an edit, so nothing validates it —
    // which is exactly why it must not create anything either.
    Scene scene;
    scene.set_animation_pose(static_cast<StableId>(999), AnimationPose{});
    CHECK(scene.animated().empty());
    CHECK(scene.size() == 0);
    scene.clear_animation_pose(static_cast<StableId>(999));
}

TEST_CASE("destroying an entity takes its pose with it", "[scene][animation]") {
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_animation_pose(id, AnimationPose{});
    REQUIRE(scene.animated().size() == 1);

    CHECK(scene.destroy(id));
    CHECK(scene.animated().empty());
    CHECK(scene.animation_pose(id) == nullptr);
}
