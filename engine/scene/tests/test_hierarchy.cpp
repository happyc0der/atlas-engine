// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/scene/scene.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>
#include <string>
#include <vector>

using atlas::ErrorCode;
using atlas::math::Vec4;
using atlas::scene::LocalTransform;
using atlas::scene::Scene;
using atlas::scene::StableId;
using Catch::Approx;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// Where an entity's origin ends up in world space.
[[nodiscard]] Vec4 world_origin(const Scene& scene, StableId id) {
    const auto* world = scene.world_transform(id);
    REQUIRE(world != nullptr);
    return world->matrix * Vec4{0.0F, 0.0F, 0.0F, 1.0F};
}

}  // namespace

TEST_CASE("a new entity is a root", "[scene][hierarchy]") {
    Scene scene;
    const StableId id = scene.create("alone");

    CHECK(scene.parent(id) == StableId::None);
    CHECK(scene.children(id).empty());
    REQUIRE(scene.roots().size() == 1);
    CHECK(scene.roots()[0] == id);
}

TEST_CASE("attaching and detaching a child", "[scene][hierarchy]") {
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId child = scene.create("child");

    REQUIRE(scene.set_parent(child, parent).has_value());
    CHECK(scene.parent(child) == parent);
    REQUIRE(scene.children(parent).size() == 1);
    CHECK(scene.children(parent)[0] == child);
    REQUIRE(scene.roots().size() == 1);

    REQUIRE(scene.set_parent(child, StableId::None).has_value());
    CHECK(scene.parent(child) == StableId::None);
    CHECK(scene.children(parent).empty());
    CHECK(scene.roots().size() == 2);
}

TEST_CASE("reparenting removes the child from its old parent", "[scene][hierarchy]") {
    // Leaving it in both lists would make the tree a graph, and the transform update would
    // visit it twice.
    Scene scene;
    const StableId first = scene.create("first");
    const StableId second = scene.create("second");
    const StableId child = scene.create("child");

    REQUIRE(scene.set_parent(child, first).has_value());
    REQUIRE(scene.set_parent(child, second).has_value());

    CHECK(scene.children(first).empty());
    REQUIRE(scene.children(second).size() == 1);
    CHECK(scene.parent(child) == second);
}

TEST_CASE("siblings are kept in identifier order", "[scene][hierarchy]") {
    // So that drawing and serialisation do not depend on the order things were attached.
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId a = scene.create("a");
    const StableId b = scene.create("b");
    const StableId c = scene.create("c");

    REQUIRE(scene.set_parent(c, parent).has_value());
    REQUIRE(scene.set_parent(a, parent).has_value());
    REQUIRE(scene.set_parent(b, parent).has_value());

    const auto children = scene.children(parent);
    REQUIRE(children.size() == 3);
    CHECK(children[0] == a);
    CHECK(children[1] == b);
    CHECK(children[2] == c);
}

TEST_CASE("an entity cannot be its own parent", "[scene][hierarchy]") {
    Scene scene;
    const StableId id = scene.create("self");

    const auto status = scene.set_parent(id, id);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a cycle is refused", "[scene][hierarchy]") {
    // Not merely invalid: a cycle is unwalkable, and the transform update would not
    // terminate. It is checked at the only moment one can be created.
    Scene scene;
    const StableId a = scene.create("a");
    const StableId b = scene.create("b");
    const StableId c = scene.create("c");

    REQUIRE(scene.set_parent(b, a).has_value());
    REQUIRE(scene.set_parent(c, b).has_value());

    // a -> b -> c, so making a a child of c would close the loop.
    const auto status = scene.set_parent(a, c);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::InvalidArgument);
    CHECK(status.error().message().contains("cycle"));

    // And the tree is unchanged.
    CHECK(scene.parent(a) == StableId::None);
    CHECK(scene.parent(b) == a);
}

TEST_CASE("parenting to something that does not exist is refused", "[scene][hierarchy]") {
    Scene scene;
    const StableId child = scene.create("child");

    const auto status = scene.set_parent(child, StableId{999});
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::NotFound);
}

TEST_CASE("destroying a parent destroys its children", "[scene][hierarchy]") {
    // Rather than orphaning them: a child whose parent is gone has a transform relative to
    // nothing, and leaving that reachable means every reader has to handle it.
    Scene scene;
    const StableId root = scene.create("root");
    const StableId child = scene.create("child");
    const StableId grandchild = scene.create("grandchild");

    REQUIRE(scene.set_parent(child, root).has_value());
    // set_parent takes (child, parent), so this reads as it means: the grandchild's parent
    // is the child. The check sees two names it thinks are the wrong way round.
    // NOLINTNEXTLINE(readability-suspicious-call-argument)
    REQUIRE(scene.set_parent(grandchild, child).has_value());
    REQUIRE(scene.size() == 3);

    REQUIRE(scene.destroy(root));
    CHECK(scene.size() == 0);
    CHECK_FALSE(scene.contains(child));
    CHECK_FALSE(scene.contains(grandchild));
}

TEST_CASE("destroying a child leaves its parent intact", "[scene][hierarchy]") {
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId child = scene.create("child");
    REQUIRE(scene.set_parent(child, parent).has_value());

    REQUIRE(scene.destroy(child));
    CHECK(scene.contains(parent));
    CHECK(scene.children(parent).empty());
}

TEST_CASE("a world transform composes with its parent", "[scene][hierarchy]") {
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId child = scene.create("child");
    REQUIRE(scene.set_parent(child, parent).has_value());

    scene.set_local_transform(parent, LocalTransform{.position = {.x = 10.0F, .y = 20.0F}});
    scene.set_local_transform(child, LocalTransform{.position = {.x = 3.0F, .y = 4.0F}});
    scene.update_transforms();

    const Vec4 parent_origin = world_origin(scene, parent);
    CHECK(parent_origin.x == Approx(10.0F));
    CHECK(parent_origin.y == Approx(20.0F));

    // The child's own offset, applied in the parent's frame.
    const Vec4 child_origin = world_origin(scene, child);
    CHECK(child_origin.x == Approx(13.0F));
    CHECK(child_origin.y == Approx(24.0F));
}

TEST_CASE("a parent's scale scales its child's offset", "[scene][hierarchy]") {
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId child = scene.create("child");
    REQUIRE(scene.set_parent(child, parent).has_value());

    scene.set_local_transform(parent, LocalTransform{.scale = {.x = 2.0F, .y = 3.0F}});
    scene.set_local_transform(child, LocalTransform{.position = {.x = 5.0F, .y = 5.0F}});
    scene.update_transforms();

    const Vec4 child_origin = world_origin(scene, child);
    CHECK(child_origin.x == Approx(10.0F));
    CHECK(child_origin.y == Approx(15.0F));
}

TEST_CASE("a parent's rotation rotates its child", "[scene][hierarchy]") {
    Scene scene;
    const StableId parent = scene.create("parent");
    const StableId child = scene.create("child");
    REQUIRE(scene.set_parent(child, parent).has_value());

    // A quarter turn, so the child at (1, 0) should end up at (0, 1).
    scene.set_local_transform(parent, LocalTransform{.rotation = std::numbers::pi_v<float> / 2.0F});
    scene.set_local_transform(child, LocalTransform{.position = {.x = 1.0F, .y = 0.0F}});
    scene.update_transforms();

    const Vec4 child_origin = world_origin(scene, child);
    CHECK(child_origin.x == Approx(0.0F).margin(1e-5));
    CHECK(child_origin.y == Approx(1.0F));
}

TEST_CASE("transforms compose through three levels", "[scene][hierarchy]") {
    Scene scene;
    const StableId a = scene.create("a");
    const StableId b = scene.create("b");
    const StableId c = scene.create("c");
    REQUIRE(scene.set_parent(b, a).has_value());
    REQUIRE(scene.set_parent(c, b).has_value());

    scene.set_local_transform(a, LocalTransform{.position = {.x = 1.0F, .y = 0.0F}});
    scene.set_local_transform(b, LocalTransform{.position = {.x = 2.0F, .y = 0.0F}});
    scene.set_local_transform(c, LocalTransform{.position = {.x = 4.0F, .y = 0.0F}});
    scene.update_transforms();

    CHECK(world_origin(scene, c).x == Approx(7.0F));
}

TEST_CASE("a depth-first walk visits parents before children", "[scene][hierarchy]") {
    Scene scene;
    const StableId root = scene.create("root");
    const StableId first = scene.create("first");
    const StableId second = scene.create("second");
    const StableId grandchild = scene.create("grandchild");

    REQUIRE(scene.set_parent(first, root).has_value());
    REQUIRE(scene.set_parent(second, root).has_value());
    REQUIRE(scene.set_parent(grandchild, first).has_value());

    std::vector<StableId> visited;
    std::vector<std::size_t> depths;
    scene.visit_depth_first([&](StableId id, std::size_t depth) {
        visited.push_back(id);
        depths.push_back(depth);
    });

    REQUIRE(visited.size() == 4);
    CHECK(visited[0] == root);
    CHECK(visited[1] == first);
    CHECK(visited[2] == grandchild);
    CHECK(visited[3] == second);
    CHECK(depths == std::vector<std::size_t>{0, 1, 2, 1});
}

TEST_CASE("a deep hierarchy does not run out of stack", "[scene][hierarchy]") {
    // A scene file is untrusted input, and a deeply nested one would otherwise recurse until
    // the stack ran out. The depth limit turns that into a bounded, reported outcome.
    Scene scene;
    StableId previous = scene.create("level0");
    for (int i = 1; i < 400; ++i) {
        const StableId next = scene.create("level");
        REQUIRE(scene.set_parent(next, previous).has_value());
        previous = next;
    }

    scene.update_transforms();
    SUCCEED("a four-hundred-deep hierarchy was handled without crashing");
}
