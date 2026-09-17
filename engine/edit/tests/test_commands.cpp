// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/edit/command.hpp>
#include <atlas/scene/serialization.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using atlas::edit::Create;
using atlas::edit::Destroy;
using atlas::edit::RemoveAnimator;
using atlas::edit::RemoveCamera;
using atlas::edit::RemoveSprite;
using atlas::edit::Rename;
using atlas::edit::Reparent;
using atlas::edit::SetAnimator;
using atlas::edit::SetCamera;
using atlas::edit::SetLocalTransform;
using atlas::edit::SetSprite;
using atlas::scene::Animator;
using atlas::scene::Scene;
using atlas::scene::StableId;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// The scene as text, which is what "unchanged" means here.
///
/// `scene::to_text` writes entities in identifier order with components in a fixed order, so
/// two scenes with the same content produce the same bytes however they were assembled. It
/// deliberately writes neither the next-identifier counter nor world transforms, which is what
/// makes it the right comparison for undo: identifiers are never reused, so the counter has
/// moved on after a destroy and restore, and that is correct rather than a difference.
[[nodiscard]] std::string bytes(const Scene& scene) {
    auto text = atlas::scene::to_text(scene);
    REQUIRE(text.has_value());
    return *text;
}

struct Fixture {
    Scene scene;
    StableId root = StableId::None;
    StableId child = StableId::None;
    StableId grandchild = StableId::None;
    StableId sibling = StableId::None;
};

/// A small tree with components on it, built so that the identifier order of the siblings is
/// the reverse of the order they are attached in. Restoring the tree in creation order would
/// still pass a test whose siblings were attached in identifier order; this one would fail.
[[nodiscard]] Fixture make_fixture() {
    Fixture fixture;
    fixture.root = fixture.scene.create("root");
    fixture.sibling = fixture.scene.create("sibling");
    fixture.child = fixture.scene.create("child");
    fixture.grandchild = fixture.scene.create("grandchild");

    REQUIRE(fixture.scene.set_parent(fixture.child, fixture.root).has_value());
    REQUIRE(fixture.scene.set_parent(fixture.sibling, fixture.root).has_value());
    REQUIRE(fixture.scene.set_parent(fixture.grandchild, fixture.child).has_value());

    fixture.scene.set_local_transform(fixture.child,
                                      {.position = {.x = 3.0F, .y = 4.0F}, .rotation = 0.5F});
    fixture.scene.set_sprite(fixture.grandchild, {.size = {.x = 2.0F, .y = 2.0F}, .layer = 3});
    fixture.scene.set_camera(fixture.root, {});
    return fixture;
}

/// Apply, check it took effect, revert, and require the scene to be byte-identical again.
/// Then apply a second time, which is what a redo does, and require the same after-image:
/// a command that overwrote its before-image on the second apply would pass the first half of
/// this and fail here.
void check_round_trip(Scene& scene, atlas::edit::Command& command) {
    const std::string before = bytes(scene);
    REQUIRE(command.apply(scene).has_value());
    const std::string after = bytes(scene);
    CHECK(after != before);

    REQUIRE(command.revert(scene).has_value());
    CHECK(bytes(scene) == before);

    REQUIRE(command.apply(scene).has_value());
    CHECK(bytes(scene) == after);

    REQUIRE(command.revert(scene).has_value());
    CHECK(bytes(scene) == before);
}

}  // namespace

TEST_CASE("renaming round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    Rename command{fixture.child, "renamed"};
    check_round_trip(fixture.scene, command);
}

TEST_CASE("moving round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    SetLocalTransform command{fixture.child,
                              {.position = {.x = -9.0F, .y = 11.0F}, .rotation = 1.25F}};
    check_round_trip(fixture.scene, command);
}

TEST_CASE("setting a sprite where there was none round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    REQUIRE(fixture.scene.sprite(fixture.child) == nullptr);
    SetSprite command{fixture.child, {.size = {.x = 5.0F, .y = 6.0F}, .layer = 2}};
    check_round_trip(fixture.scene, command);
    // The undo removed a sprite rather than restoring one, which is the branch that a fixture
    // with a sprite everywhere would never reach.
    CHECK(fixture.scene.sprite(fixture.child) == nullptr);
}

TEST_CASE("replacing an existing sprite round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    REQUIRE(fixture.scene.sprite(fixture.grandchild) != nullptr);
    SetSprite command{fixture.grandchild, {.size = {.x = 7.0F, .y = 8.0F}, .layer = 9}};
    check_round_trip(fixture.scene, command);
    const auto* restored = fixture.scene.sprite(fixture.grandchild);
    REQUIRE(restored != nullptr);
    CHECK(restored->layer == 3);
}

TEST_CASE("removing a sprite round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    RemoveSprite command{fixture.grandchild};
    check_round_trip(fixture.scene, command);
}

TEST_CASE("removing a sprite that is not there is refused", "[edit][commands]") {
    // Refused rather than treated as a no-op: undoing a no-op would have to invent a sprite
    // the entity never had, and a command that does nothing has no business in the history.
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);
    RemoveSprite command{fixture.child};
    const auto status = command.apply(fixture.scene);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == atlas::ErrorCode::NotFound);
    CHECK(bytes(fixture.scene) == before);
}

TEST_CASE("setting and removing a camera round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    SECTION("set on an entity with none") {
        SetCamera command{fixture.child, {}};
        check_round_trip(fixture.scene, command);
    }
    SECTION("remove an existing one") {
        RemoveCamera command{fixture.root};
        check_round_trip(fixture.scene, command);
    }
    SECTION("removing one that is not there is refused") {
        RemoveCamera command{fixture.child};
        const auto status = command.apply(fixture.scene);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code() == atlas::ErrorCode::NotFound);
    }
}

TEST_CASE("setting an animator where there was none round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    REQUIRE(fixture.scene.animator(fixture.child) == nullptr);
    SetAnimator command{fixture.child, Animator{.start_ms = 250, .speed = 2.0F}};
    check_round_trip(fixture.scene, command);
    // The undo removed an animator rather than restoring one, which is the branch a fixture
    // with an animator everywhere would never reach.
    CHECK(fixture.scene.animator(fixture.child) == nullptr);
}

TEST_CASE("removing an animator round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    fixture.scene.set_animator(fixture.child, Animator{.start_ms = 100});
    RemoveAnimator command{fixture.child};
    check_round_trip(fixture.scene, command);
}

TEST_CASE("removing an animator that is not there is refused", "[edit][commands]") {
    // An undo of this would have to invent an animator the entity never had, which is the
    // same reasoning as the sprite it mirrors.
    Fixture fixture = make_fixture();
    RemoveAnimator command{fixture.child};
    const auto status = command.apply(fixture.scene);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == atlas::ErrorCode::NotFound);
}

TEST_CASE("animator edits merge into one step", "[edit][commands]") {
    // A scrub is a drag: dozens of values a second, and one thing the person did. Merging is
    // what makes a single undo put it back where it started.
    const Fixture fixture = make_fixture();
    SetAnimator first{fixture.child, Animator{.start_ms = 10}};
    const SetAnimator second{fixture.child, Animator{.start_ms = 20}};
    CHECK(first.merge(second));

    // But not across entities: two different things were adjusted, and one undo must not
    // revert both.
    const SetAnimator elsewhere{fixture.root, Animator{.start_ms = 30}};
    CHECK_FALSE(first.merge(elsewhere));
}

TEST_CASE("destroying an animated entity keeps its animator", "[edit][commands]") {
    // The failure this guards against is silent. Destroy captures a fixed list of components,
    // and a component added to the scene without being added to that list is simply lost when
    // an entity is destroyed and the destroy is undone.
    Fixture fixture = make_fixture();
    fixture.scene.set_animator(fixture.child,
                               Animator{.start_ms = 750, .speed = 0.5F, .playing = false});

    Destroy command{fixture.child};
    check_round_trip(fixture.scene, command);

    const auto* restored = fixture.scene.animator(fixture.child);
    REQUIRE(restored != nullptr);
    CHECK(restored->start_ms == 750);
    CHECK(restored->speed == 0.5F);
    CHECK_FALSE(restored->playing);
}

TEST_CASE("reparenting round-trips", "[edit][commands]") {
    Fixture fixture = make_fixture();
    SECTION("to another parent") {
        Reparent command{fixture.grandchild, fixture.sibling};
        check_round_trip(fixture.scene, command);
        CHECK(fixture.scene.parent(fixture.grandchild) == fixture.child);
    }
    SECTION("to the root") {
        Reparent command{fixture.child, StableId::None};
        check_round_trip(fixture.scene, command);
        CHECK(fixture.scene.parent(fixture.child) == fixture.root);
    }
}

TEST_CASE("reparenting into its own descendant is refused", "[edit][commands]") {
    // Scene::set_parent checks for a cycle before it detaches anything, so the refusal leaves
    // the scene untouched and this command never reaches the history.
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);
    Reparent command{fixture.root, fixture.grandchild};
    const auto status = command.apply(fixture.scene);
    REQUIRE_FALSE(status.has_value());
    CHECK(bytes(fixture.scene) == before);
}

TEST_CASE("creating round-trips and keeps its identifier", "[edit][commands]") {
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);

    Create command{"new entity", fixture.root};
    REQUIRE(command.apply(fixture.scene).has_value());
    const StableId created = command.id();
    CHECK(atlas::scene::valid(created));
    CHECK(fixture.scene.parent(created) == fixture.root);

    REQUIRE(command.revert(fixture.scene).has_value());
    CHECK(bytes(fixture.scene) == before);
    CHECK_FALSE(fixture.scene.contains(created));

    // The same identifier on redo, not a fresh one. A selection or a later command holding it
    // must still mean this entity after an undo and a redo.
    REQUIRE(command.apply(fixture.scene).has_value());
    CHECK(command.id() == created);
    CHECK(fixture.scene.contains(created));
    CHECK(fixture.scene.parent(created) == fixture.root);
}

TEST_CASE("destroying a subtree restores every entity, component and child order",
          "[edit][commands]") {
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);
    const auto root_children_before = std::vector<StableId>{
        fixture.scene.children(fixture.root).begin(), fixture.scene.children(fixture.root).end()};
    const auto active_camera_before = fixture.scene.active_camera();

    Destroy command{fixture.root};
    REQUIRE(command.apply(fixture.scene).has_value());
    // The whole subtree went, not just the entity named.
    CHECK_FALSE(fixture.scene.contains(fixture.root));
    CHECK_FALSE(fixture.scene.contains(fixture.child));
    CHECK_FALSE(fixture.scene.contains(fixture.grandchild));
    CHECK_FALSE(fixture.scene.contains(fixture.sibling));
    CHECK(fixture.scene.size() == 0);

    REQUIRE(command.revert(fixture.scene).has_value());
    CHECK(bytes(fixture.scene) == before);

    // Byte equality already covers most of this; these say which property broke when it does.
    const auto root_children_after = fixture.scene.children(fixture.root);
    CHECK(std::vector<StableId>{root_children_after.begin(), root_children_after.end()} ==
          root_children_before);
    CHECK(fixture.scene.parent(fixture.grandchild) == fixture.child);
    CHECK(fixture.scene.active_camera() == active_camera_before);
    const auto* sprite = fixture.scene.sprite(fixture.grandchild);
    REQUIRE(sprite != nullptr);
    CHECK(sprite->layer == 3);
    const auto* transform = fixture.scene.local_transform(fixture.child);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.x == 3.0F);
}

TEST_CASE("destroying a leaf leaves its siblings alone", "[edit][commands]") {
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);
    Destroy command{fixture.grandchild};
    check_round_trip(fixture.scene, command);
    CHECK(bytes(fixture.scene) == before);
}

TEST_CASE("every command refuses an entity that is not in the scene", "[edit][commands]") {
    // Scene's component setters return void and silently do nothing for an unknown entity, so
    // without the check each command makes, an edit built from a stale selection would report
    // success and enter the history with nothing to undo. This is that check, for all of them.
    Fixture fixture = make_fixture();
    const auto absent = static_cast<StableId>(999'999);
    const std::string before = bytes(fixture.scene);

    std::vector<std::unique_ptr<atlas::edit::Command>> commands;
    commands.push_back(std::make_unique<Rename>(absent, "x"));
    commands.push_back(std::make_unique<SetLocalTransform>(absent, atlas::scene::LocalTransform{}));
    commands.push_back(std::make_unique<SetSprite>(absent, atlas::scene::SpriteRenderData{}));
    commands.push_back(std::make_unique<RemoveSprite>(absent));
    commands.push_back(std::make_unique<SetCamera>(absent, atlas::scene::Camera{}));
    commands.push_back(std::make_unique<RemoveCamera>(absent));
    commands.push_back(std::make_unique<Reparent>(absent, fixture.root));
    commands.push_back(std::make_unique<Destroy>(absent));
    commands.push_back(std::make_unique<Create>("orphan", absent));

    for (const auto& command : commands) {
        INFO(command->label());
        const auto status = command->apply(fixture.scene);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code() == atlas::ErrorCode::NotFound);
        CHECK(bytes(fixture.scene) == before);
    }
}

TEST_CASE("applying twice without reverting keeps the original before-image", "[edit][commands]") {
    // The property the capture-once flag exists for, and the round-trip helper cannot see it:
    // after a revert the scene is back at the before-image, so a command that recaptured on
    // every apply would capture the same value again and pass.
    //
    // This is the sequence that tells them apart. Two applies with no revert between means a
    // command that recaptures would take the after-image of the first apply as its
    // before-image, and one undo would then leave the entity where the first edit put it
    // instead of where the user started. History never does this, but Command is a public type
    // and cannot rely on its only current caller's discipline.
    Fixture fixture = make_fixture();
    const float origin = fixture.scene.local_transform(fixture.child)->position.x;

    SetLocalTransform command{fixture.child,
                              atlas::scene::LocalTransform{.position = {.x = 5.0F, .y = 0.0F}}};
    REQUIRE(command.apply(fixture.scene).has_value());
    REQUIRE(command.apply(fixture.scene).has_value());
    REQUIRE(command.revert(fixture.scene).has_value());

    CHECK(fixture.scene.local_transform(fixture.child)->position.x == origin);
}

TEST_CASE("destroying twice without reverting keeps the original subtree", "[edit][commands]") {
    // The same property for Destroy, whose capture is the whole subtree rather than one value.
    // A second capture would run against a scene the first destroy has already emptied and
    // record nothing, so the undo would restore nothing and report success.
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);

    Destroy command{fixture.root};
    REQUIRE(command.apply(fixture.scene).has_value());
    // The second apply finds nothing to destroy and says so, which is itself correct; what
    // matters is that it has not discarded the capture from the first.
    CHECK_FALSE(command.apply(fixture.scene).has_value());
    REQUIRE(command.revert(fixture.scene).has_value());

    CHECK(bytes(fixture.scene) == before);
}
