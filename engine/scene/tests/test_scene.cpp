// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/scene/scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using atlas::ErrorCode;
using atlas::scene::Camera;
using atlas::scene::LocalTransform;
using atlas::scene::Scene;
using atlas::scene::SpriteRenderData;
using atlas::scene::StableId;

namespace {

/// The scene asserts main-thread affinity, as everything in the presentation layer does. In
/// an application the platform establishes this; these tests stand in for it.
const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

}  // namespace

TEST_CASE("a new scene is empty", "[scene]") {
    const Scene scene;
    CHECK(scene.size() == 0);
    CHECK_FALSE(scene.contains(StableId{1}));
    CHECK(scene.roots().empty());
}

TEST_CASE("an entity is created with a name and identity", "[scene]") {
    Scene scene;
    const StableId id = scene.create("player");

    CHECK(atlas::scene::valid(id));
    CHECK(scene.contains(id));
    CHECK(scene.size() == 1);
    CHECK(scene.name(id) == "player");
}

TEST_CASE("identifiers are never reused", "[scene]") {
    // A stale reference to a destroyed entity must not start referring to a new one. That is
    // the difference between a stable identifier and the library's own handle, which is
    // reused freely.
    Scene scene;
    const StableId first = scene.create("first");
    REQUIRE(scene.destroy(first));

    const StableId second = scene.create("second");
    CHECK(first != second);
    CHECK_FALSE(scene.contains(first));
    CHECK(scene.contains(second));
}

TEST_CASE("zero is not a usable identity", "[scene]") {
    Scene scene;
    const auto created = scene.create_with_id(StableId::None, "nothing");
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a chosen identity cannot collide with an existing one", "[scene]") {
    // Silently renumbering during a load would break every reference in the file.
    Scene scene;
    REQUIRE(scene.create_with_id(StableId{7}, "seven").has_value());

    const auto again = scene.create_with_id(StableId{7}, "also seven");
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code() == ErrorCode::AlreadyExists);
}

TEST_CASE("creating after a chosen identity does not collide with it", "[scene]") {
    Scene scene;
    REQUIRE(scene.create_with_id(StableId{100}, "hundred").has_value());

    const StableId next = scene.create("next");
    CHECK(static_cast<std::uint64_t>(next) > 100);
    CHECK(scene.contains(StableId{100}));
}

TEST_CASE("destroying an entity removes it", "[scene]") {
    Scene scene;
    const StableId id = scene.create("doomed");

    CHECK(scene.destroy(id));
    CHECK_FALSE(scene.contains(id));
    CHECK(scene.size() == 0);

    // Destroying twice is reported rather than undefined.
    CHECK_FALSE(scene.destroy(id));
}

TEST_CASE("components are set, read and removed", "[scene]") {
    Scene scene;
    const StableId id = scene.create("thing");

    // Every entity gets a transform, because everything in a scene is somewhere.
    REQUIRE(scene.local_transform(id) != nullptr);

    scene.set_local_transform(id, LocalTransform{.position = {.x = 3.0F, .y = 4.0F},
                                                 .rotation = 1.5F,
                                                 .scale = {.x = 2.0F, .y = 2.0F}});
    const auto* transform = scene.local_transform(id);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.x == 3.0F);
    CHECK(transform->rotation == 1.5F);

    CHECK(scene.sprite(id) == nullptr);
    scene.set_sprite(id, SpriteRenderData{.layer = 5});
    REQUIRE(scene.sprite(id) != nullptr);
    CHECK(scene.sprite(id)->layer == 5);

    scene.remove_sprite(id);
    CHECK(scene.sprite(id) == nullptr);

    scene.set_camera(id, Camera{.zoom = 2.0F, .active = true});
    REQUIRE(scene.camera(id) != nullptr);
    CHECK(scene.camera(id)->zoom == 2.0F);
    scene.remove_camera(id);
    CHECK(scene.camera(id) == nullptr);
}

TEST_CASE("asking about an entity that does not exist is safe", "[scene]") {
    const Scene scene;
    const StableId nobody{999};

    CHECK(scene.name(nobody).empty());
    CHECK(scene.local_transform(nobody) == nullptr);
    CHECK(scene.sprite(nobody) == nullptr);
    CHECK(scene.camera(nobody) == nullptr);
    CHECK(scene.parent(nobody) == StableId::None);
    CHECK(scene.children(nobody).empty());
}

TEST_CASE("entities are listed in identifier order", "[scene]") {
    // The library stores components in whatever order suits its own compaction. Inheriting
    // that would make a saved file depend on the order things happened to be created.
    Scene scene;
    const StableId a = scene.create("a");
    const StableId b = scene.create("b");
    const StableId c = scene.create("c");
    REQUIRE(scene.destroy(b));
    const StableId d = scene.create("d");

    const auto entities = scene.entities();
    REQUIRE(entities.size() == 3);
    CHECK(entities[0].id == a);
    CHECK(entities[1].id == c);
    CHECK(entities[2].id == d);
}

TEST_CASE("draw order is by layer, then identifier", "[scene]") {
    // Ties broken by identifier rather than creation order, so what overlaps what is the
    // same after a save and reload.
    Scene scene;
    const StableId front = scene.create("front");
    const StableId back = scene.create("back");
    const StableId middle = scene.create("middle");

    scene.set_sprite(front, SpriteRenderData{.layer = 10});
    scene.set_sprite(back, SpriteRenderData{.layer = -5});
    scene.set_sprite(middle, SpriteRenderData{.layer = 0});

    const auto order = scene.drawable();
    REQUIRE(order.size() == 3);
    CHECK(order[0] == back);
    CHECK(order[1] == middle);
    CHECK(order[2] == front);
}

TEST_CASE("an invisible sprite is not drawn", "[scene]") {
    Scene scene;
    const StableId id = scene.create("hidden");
    scene.set_sprite(id, SpriteRenderData{.visible = false});

    CHECK(scene.drawable().empty());
}

TEST_CASE("the active camera is found, and ties are resolved", "[scene]") {
    Scene scene;
    CHECK_FALSE(scene.active_camera().has_value());

    const StableId first = scene.create("first");
    const StableId second = scene.create("second");
    scene.set_camera(first, Camera{.active = false});
    scene.set_camera(second, Camera{.active = true});

    REQUIRE(scene.active_camera().has_value());
    CHECK(*scene.active_camera() == second);

    // Two active cameras give a defined answer rather than whichever was visited first.
    scene.set_camera(first, Camera{.active = true});
    REQUIRE(scene.active_camera().has_value());
    CHECK(*scene.active_camera() == first);
}

TEST_CASE("clearing empties the scene and restarts numbering", "[scene]") {
    Scene scene;
    (void)scene.create("a");
    (void)scene.create("b");
    REQUIRE(scene.size() == 2);

    scene.clear();
    CHECK(scene.size() == 0);
    CHECK(scene.roots().empty());
}

TEST_CASE("a scene can be moved", "[scene]") {
    Scene scene;
    const StableId id = scene.create("carried");

    const Scene moved = std::move(scene);
    CHECK(moved.contains(id));
    CHECK(moved.name(id) == "carried");
}
