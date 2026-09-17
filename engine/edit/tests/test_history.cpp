// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/edit/history.hpp>
#include <atlas/scene/serialization.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using atlas::edit::Coalesce;
using atlas::edit::Destroy;
using atlas::edit::History;
using atlas::edit::Rename;
using atlas::edit::Reparent;
using atlas::edit::SetLocalTransform;
using atlas::scene::Scene;
using atlas::scene::StableId;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

[[nodiscard]] std::string bytes(const Scene& scene) {
    auto text = atlas::scene::to_text(scene);
    REQUIRE(text.has_value());
    return *text;
}

struct Fixture {
    Scene scene;
    StableId root = StableId::None;
    StableId child = StableId::None;
};

[[nodiscard]] Fixture make_fixture() {
    Fixture fixture;
    fixture.root = fixture.scene.create("root");
    fixture.child = fixture.scene.create("child");
    REQUIRE(fixture.scene.set_parent(fixture.child, fixture.root).has_value());
    return fixture;
}

[[nodiscard]] std::unique_ptr<SetLocalTransform> move_to(StableId id, float x) {
    return std::make_unique<SetLocalTransform>(
        id, atlas::scene::LocalTransform{.position = {.x = x, .y = 0.0F}});
}

[[nodiscard]] float position_of(const Scene& scene, StableId id) {
    const auto* transform = scene.local_transform(id);
    REQUIRE(transform != nullptr);
    return transform->position.x;
}

}  // namespace

TEST_CASE("applying pushes, and undo and redo move between the stacks", "[edit][history]") {
    Fixture fixture = make_fixture();
    const std::string before = bytes(fixture.scene);
    History history{fixture.scene};

    CHECK_FALSE(history.can_undo());
    CHECK_FALSE(history.can_redo());
    CHECK(history.revision() == 0);

    REQUIRE(history.apply(std::make_unique<Rename>(fixture.child, "renamed")).has_value());
    CHECK(history.can_undo());
    CHECK_FALSE(history.can_redo());
    CHECK(history.revision() == 1);
    CHECK(history.undo_label() == "rename");

    CHECK(history.undo());
    CHECK(bytes(fixture.scene) == before);
    CHECK_FALSE(history.can_undo());
    CHECK(history.can_redo());
    CHECK(history.revision() == 2);
    CHECK(history.redo_label() == "rename");

    CHECK(history.redo());
    CHECK(fixture.scene.name(fixture.child) == "renamed");
    CHECK(history.can_undo());
    CHECK_FALSE(history.can_redo());
    CHECK(history.revision() == 3);
}

TEST_CASE("undo and redo on an empty history do nothing and are not errors", "[edit][history]") {
    // Pressing undo when there is nothing to undo is an ordinary thing to do, so it returns
    // false rather than producing an error a caller would have to display.
    Fixture fixture = make_fixture();
    History history{fixture.scene};
    const std::string before = bytes(fixture.scene);

    CHECK_FALSE(history.undo());
    CHECK_FALSE(history.redo());
    CHECK(history.revision() == 0);
    CHECK(bytes(fixture.scene) == before);
    CHECK_FALSE(history.last_error().has_value());
    CHECK(history.undo_label().empty());
    CHECK(history.redo_label().empty());
}

TEST_CASE("a new command clears the redo stack", "[edit][history]") {
    Fixture fixture = make_fixture();
    History history{fixture.scene};

    REQUIRE(history.apply(std::make_unique<Rename>(fixture.child, "first")).has_value());
    CHECK(history.undo());
    REQUIRE(history.can_redo());

    REQUIRE(history.apply(std::make_unique<Rename>(fixture.child, "second")).has_value());
    CHECK_FALSE(history.can_redo());
    CHECK_FALSE(history.redo());
    CHECK(fixture.scene.name(fixture.child) == "second");
}

TEST_CASE("a failed apply leaves the history exactly as it was", "[edit][history]") {
    // Set up with something on both stacks, so this proves the failure disturbs neither. A
    // widget calls apply on every frame of a drag; one that refused an edit and cleared redo
    // would destroy work the user could otherwise get back.
    Fixture fixture = make_fixture();
    History history{fixture.scene};
    REQUIRE(history.apply(std::make_unique<Rename>(fixture.child, "kept")).has_value());
    REQUIRE(history.apply(std::make_unique<Rename>(fixture.child, "undone")).has_value());
    CHECK(history.undo());

    const std::string before = bytes(fixture.scene);
    const std::size_t undo_depth = history.undo_depth();
    const std::size_t redo_depth = history.redo_depth();
    const std::uint64_t revision = history.revision();

    // A cycle: Scene::set_parent refuses it before detaching anything.
    const auto status = history.apply(std::make_unique<Reparent>(fixture.root, fixture.child));
    REQUIRE_FALSE(status.has_value());

    CHECK(bytes(fixture.scene) == before);
    CHECK(history.undo_depth() == undo_depth);
    CHECK(history.redo_depth() == redo_depth);
    CHECK(history.revision() == revision);
}

TEST_CASE("the history is bounded and drops the oldest", "[edit][history]") {
    Fixture fixture = make_fixture();
    History history{fixture.scene, 2};

    REQUIRE(history.apply(move_to(fixture.child, 1.0F)).has_value());
    REQUIRE(history.apply(move_to(fixture.child, 2.0F)).has_value());
    REQUIRE(history.apply(move_to(fixture.child, 3.0F)).has_value());
    CHECK(history.undo_depth() == 2);

    CHECK(history.undo());
    CHECK(position_of(fixture.scene, fixture.child) == 2.0F);
    CHECK(history.undo());
    CHECK(position_of(fixture.scene, fixture.child) == 1.0F);

    // The first edit is beyond the bound: it stays applied rather than being undone into a
    // state the history can no longer describe.
    CHECK_FALSE(history.undo());
    CHECK(position_of(fixture.scene, fixture.child) == 1.0F);
}

TEST_CASE("coalescing folds a drag into one undo step", "[edit][history]") {
    Fixture fixture = make_fixture();
    History history{fixture.scene};
    const float origin = position_of(fixture.scene, fixture.child);

    REQUIRE(history.apply(move_to(fixture.child, 1.0F), Coalesce::WithPrevious).has_value());
    REQUIRE(history.apply(move_to(fixture.child, 2.0F), Coalesce::WithPrevious).has_value());
    REQUIRE(history.apply(move_to(fixture.child, 3.0F), Coalesce::WithPrevious).has_value());

    CHECK(history.undo_depth() == 1);
    CHECK(position_of(fixture.scene, fixture.child) == 3.0F);

    // One undo returns to where the drag started, not to the previous frame of it.
    CHECK(history.undo());
    CHECK(position_of(fixture.scene, fixture.child) == origin);
    CHECK(history.redo());
    CHECK(position_of(fixture.scene, fixture.child) == 3.0F);
}

TEST_CASE("coalescing stops at a different entity, an uncoalesced edit, or a break",
          "[edit][history]") {
    SECTION("a different entity is a different step") {
        Fixture fixture = make_fixture();
        History history{fixture.scene};
        REQUIRE(history.apply(move_to(fixture.child, 1.0F), Coalesce::WithPrevious).has_value());
        REQUIRE(history.apply(move_to(fixture.root, 1.0F), Coalesce::WithPrevious).has_value());
        CHECK(history.undo_depth() == 2);
    }

    SECTION("an uncoalesced edit closes the group") {
        Fixture fixture = make_fixture();
        History history{fixture.scene};
        REQUIRE(history.apply(move_to(fixture.child, 1.0F), Coalesce::WithPrevious).has_value());
        REQUIRE(history.apply(std::make_unique<Rename>(fixture.child, "x")).has_value());
        REQUIRE(history.apply(move_to(fixture.child, 2.0F), Coalesce::WithPrevious).has_value());
        CHECK(history.undo_depth() == 3);
    }

    SECTION("break_coalescing starts a new group") {
        Fixture fixture = make_fixture();
        History history{fixture.scene};
        REQUIRE(history.apply(move_to(fixture.child, 1.0F), Coalesce::WithPrevious).has_value());
        history.break_coalescing();
        REQUIRE(history.apply(move_to(fixture.child, 2.0F), Coalesce::WithPrevious).has_value());
        CHECK(history.undo_depth() == 2);
    }

    SECTION("an undo closes the group, so the next edit cannot merge into what was undone") {
        Fixture fixture = make_fixture();
        History history{fixture.scene};
        REQUIRE(history.apply(move_to(fixture.child, 1.0F), Coalesce::WithPrevious).has_value());
        REQUIRE(history.apply(move_to(fixture.child, 2.0F), Coalesce::WithPrevious).has_value());
        CHECK(history.undo_depth() == 1);
        CHECK(history.undo());
        REQUIRE(history.apply(move_to(fixture.child, 5.0F), Coalesce::WithPrevious).has_value());
        CHECK(history.undo_depth() == 1);
        CHECK_FALSE(history.can_redo());
    }
}

TEST_CASE("a history that cannot reproduce the scene discards itself", "[edit][history]") {
    // The one test that breaks the contract on purpose. History holds a reference to a scene
    // and documents that nothing else may write to it; here something does, by recreating an
    // identifier the pending undo is holding. The undo then cannot restore the subtree, and
    // rather than leaving a half-restored scene and a stack that no longer describes it, the
    // history reports the failure and throws itself away.
    Fixture fixture = make_fixture();
    History history{fixture.scene};
    REQUIRE(history.apply(std::make_unique<Rename>(fixture.root, "kept")).has_value());
    REQUIRE(history.apply(std::make_unique<Destroy>(fixture.child)).has_value());
    REQUIRE_FALSE(fixture.scene.contains(fixture.child));

    // Behind the history's back.
    REQUIRE(fixture.scene.create_with_id(fixture.child, "impostor").has_value());

    CHECK_FALSE(history.undo());
    REQUIRE(history.last_error().has_value());
    CHECK(history.last_error()->code() == atlas::ErrorCode::AlreadyExists);
    CHECK_FALSE(history.can_undo());
    CHECK_FALSE(history.can_redo());
}

TEST_CASE("the scene is reachable only for reading", "[edit][history]") {
    // Not a behaviour test: a statement of the property the whole module exists for. If a
    // mutable accessor is ever added, this comment is where someone should have to argue with
    // it. static_assert rather than a runtime check, because it is about the type.
    Fixture fixture = make_fixture();
    const History history{fixture.scene};
    static_assert(std::is_same_v<decltype(history.scene()), const Scene&>,
                  "History must expose the scene only as const; a panel holding a History must "
                  "not be able to reach a mutable Scene.");
    CHECK(history.scene().size() == 2);
}
