// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/scene/serialization.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <string>

using atlas::ErrorCode;
using atlas::assets::AssetId;
using atlas::assets::AssetType;
using atlas::scene::Camera;
using atlas::scene::from_text;
using atlas::scene::kSceneFormatVersion;
using atlas::scene::LocalTransform;
using atlas::scene::Scene;
using atlas::scene::SpriteRenderData;
using atlas::scene::StableId;
using atlas::scene::to_text;
using Catch::Approx;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// A scene with one of everything, for round-trip tests.
[[nodiscard]] Scene make_scene() {
    Scene scene;

    const StableId root = scene.create("root");
    const StableId child = scene.create("child");
    const StableId eye = scene.create("camera");

    REQUIRE(scene.set_parent(child, root).has_value());

    scene.set_local_transform(root, LocalTransform{.position = {.x = 10.0F, .y = -5.5F},
                                                   .rotation = 0.25F,
                                                   .scale = {.x = 2.0F, .y = 3.0F}});
    scene.set_local_transform(child, LocalTransform{.position = {.x = 1.0F, .y = 2.0F}});

    scene.set_sprite(
        child, SpriteRenderData{
                   .texture = AssetId::from("textures/tile.png", AssetType::Texture),
                   .size = {.x = 4.0F, .y = 6.0F},
                   .uv = {.position = {.x = 0.25F, .y = 0.5F}, .size = {.x = 0.5F, .y = 0.25F}},
                   .tint = {.r = 0.1F, .g = 0.2F, .b = 0.3F, .a = 0.4F},
                   .layer = -3,
                   .visible = true,
               });

    scene.set_camera(eye, Camera{.zoom = 2.5F, .active = true});
    scene.update_transforms();
    return scene;
}

}  // namespace

TEST_CASE("an empty scene round-trips", "[scene][serialization]") {
    const Scene empty;
    const auto text = to_text(empty);
    REQUIRE(text.has_value());

    Scene loaded;
    REQUIRE(from_text(loaded, *text).has_value());
    CHECK(loaded.size() == 0);
}

TEST_CASE("a scene round-trips with every component intact", "[scene][serialization]") {
    const Scene original = make_scene();
    const auto text = to_text(original);
    REQUIRE(text.has_value());

    Scene loaded;
    REQUIRE(from_text(loaded, *text).has_value());

    REQUIRE(loaded.size() == original.size());

    const auto originals = original.entities();
    const auto reloaded = loaded.entities();
    REQUIRE(reloaded.size() == originals.size());

    for (std::size_t i = 0; i < originals.size(); ++i) {
        INFO("entity " << static_cast<std::uint64_t>(originals[i].id));
        CHECK(reloaded[i].id == originals[i].id);
        CHECK(reloaded[i].name == originals[i].name);
        CHECK(reloaded[i].parent == originals[i].parent);
        CHECK(reloaded[i].has_sprite == originals[i].has_sprite);
        CHECK(reloaded[i].has_camera == originals[i].has_camera);
    }
}

TEST_CASE("transform values survive a round trip", "[scene][serialization]") {
    const Scene original = make_scene();
    const auto text = to_text(original);
    REQUIRE(text.has_value());

    Scene loaded;
    REQUIRE(from_text(loaded, *text).has_value());

    const auto id = original.entities().front().id;
    const auto* before = original.local_transform(id);
    const auto* after = loaded.local_transform(id);
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);

    CHECK(after->position.x == Approx(before->position.x));
    CHECK(after->position.y == Approx(before->position.y));
    CHECK(after->rotation == Approx(before->rotation));
    CHECK(after->scale.x == Approx(before->scale.x));
    CHECK(after->scale.y == Approx(before->scale.y));
}

TEST_CASE("sprite values survive a round trip", "[scene][serialization]") {
    const Scene original = make_scene();
    const auto text = to_text(original);
    REQUIRE(text.has_value());

    Scene loaded;
    REQUIRE(from_text(loaded, *text).has_value());

    // The child is the one with a sprite.
    StableId with_sprite = StableId::None;
    for (const auto& view : original.entities()) {
        if (view.has_sprite) {
            with_sprite = view.id;
        }
    }
    REQUIRE(atlas::scene::valid(with_sprite));

    const auto* before = original.sprite(with_sprite);
    const auto* after = loaded.sprite(with_sprite);
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);

    // The asset identifier in particular: a sprite that lost its texture reference on load
    // would draw the fallback and look like an asset problem.
    CHECK(after->texture == before->texture);
    CHECK(after->size.x == Approx(before->size.x));
    CHECK(after->uv.position.x == Approx(before->uv.position.x));
    CHECK(after->uv.size.y == Approx(before->uv.size.y));
    CHECK(after->tint.r == Approx(before->tint.r));
    CHECK(after->tint.a == Approx(before->tint.a));
    CHECK(after->layer == before->layer);
    CHECK(after->visible == before->visible);
}

TEST_CASE("saving twice produces identical bytes", "[scene][serialization]") {
    // Necessary for canonical output, though not sufficient on its own: any fixed order
    // satisfies it. The insertion-order test below is the one that pins the order down.
    const Scene original = make_scene();

    const auto first = to_text(original);
    REQUIRE(first.has_value());

    Scene loaded;
    REQUIRE(from_text(loaded, *first).has_value());

    const auto second = to_text(loaded);
    REQUIRE(second.has_value());

    CHECK(*first == *second);
}

TEST_CASE("output does not depend on the order entities were created", "[scene][serialization]") {
    // This is the canonical-output property that matters. Two scenes holding the same
    // entities must produce the same file whatever order they were built in, or a
    // version-control diff of a scene is noise and any future hash of scene state depends on
    // how the scene happened to be assembled.
    const auto build = [](bool ascending) {
        Scene scene;
        const std::array<std::uint64_t, 4> ids{7, 2, 9, 4};

        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::size_t at = ascending ? i : ids.size() - 1 - i;
            const auto id = StableId{ids[at]};
            REQUIRE(scene.create_with_id(id, std::format("entity {}", ids[at])).has_value());
            scene.set_local_transform(
                id, LocalTransform{.position = {.x = static_cast<float>(ids[at]), .y = 0.0F}});
        }
        return scene;
    };

    const auto forwards = to_text(build(true));
    const auto backwards = to_text(build(false));
    REQUIRE(forwards.has_value());
    REQUIRE(backwards.has_value());

    CHECK(*forwards == *backwards);

    // And the order is by identifier specifically, not merely consistent, so a person reading
    // the file can find an entity in it.
    CHECK(forwards->find("entity 2") < forwards->find("entity 4"));
    CHECK(forwards->find("entity 4") < forwards->find("entity 7"));
    CHECK(forwards->find("entity 7") < forwards->find("entity 9"));
}

TEST_CASE("parentage is recorded once, on the child", "[scene][serialization]") {
    // The file names each entity's parent and never lists children. Storing both would let a
    // file disagree with itself, and the loader would have to pick a winner. Sibling order is
    // reconstructed from identifiers on load, which is why it is not written.
    Scene scene;
    const auto parent = StableId{1};
    REQUIRE(scene.create_with_id(parent, "parent").has_value());
    for (const std::uint64_t id : {5U, 3U, 8U}) {
        REQUIRE(scene.create_with_id(StableId{id}, std::format("child {}", id)).has_value());
        REQUIRE(scene.set_parent(StableId{id}, parent).has_value());
    }

    const auto text = to_text(scene);
    REQUIRE(text.has_value());
    CHECK(text->find("children") == std::string::npos);

    Scene loaded;
    REQUIRE(from_text(loaded, *text).has_value());

    const auto children = loaded.children(parent);
    REQUIRE(children.size() == 3);
    CHECK(children[0] == StableId{3});
    CHECK(children[1] == StableId{5});
    CHECK(children[2] == StableId{8});
}

TEST_CASE("a forward reference to a parent resolves", "[scene][serialization]") {
    // A child may be written before its parent, so parentage is linked only once every
    // entity exists.
    const std::string text = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [
    {"id": 1, "name": "child", "parent": 2},
    {"id": 2, "name": "parent"}
  ]
})";

    Scene scene;
    REQUIRE(from_text(scene, text).has_value());
    CHECK(scene.parent(StableId{1}) == StableId{2});
}

TEST_CASE("a file from a newer build is refused", "[scene][serialization]") {
    // Reading it would silently drop whatever the newer version added, and the first sign
    // would be data quietly disappearing on the next save.
    const std::string text = std::format(R"({{
  "format": "atlas-scene",
  "version": {},
  "entities": []
}})",
                                         kSceneFormatVersion + 1);

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::VersionMismatch);
}

TEST_CASE("a file that is not a scene is refused", "[scene][serialization]") {
    Scene scene;
    for (const auto* text :
         {R"({"format": "something-else", "version": 1, "entities": []})",
          R"({"version": 1, "entities": []})", R"({"format": "atlas-scene", "entities": []})",
          R"({"format": "atlas-scene", "version": 1})"}) {
        INFO("input " << text);
        CHECK_FALSE(from_text(scene, text).has_value());
    }
}

TEST_CASE("malformed text is refused rather than throwing", "[scene][serialization]") {
    // A scene file may come from anywhere, and parsing must not throw across this boundary.
    Scene scene;
    for (const auto* text : {"", "not json at all", "{", "[]", R"({"format":)",
                             R"({"format": "atlas-scene", "version": 1, "entities": "no"})"}) {
        INFO("input " << text);
        const auto status = from_text(scene, text);
        CHECK_FALSE(status.has_value());
    }
}

TEST_CASE("a reserved identifier is refused", "[scene][serialization]") {
    const std::string text = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [{"id": 0, "name": "nothing"}]
})";

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("a duplicate identifier is refused", "[scene][serialization]") {
    const std::string text = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [{"id": 5, "name": "a"}, {"id": 5, "name": "b"}]
})";

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::AlreadyExists);
}

TEST_CASE("a parent reference to nothing is refused", "[scene][serialization]") {
    const std::string text = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [{"id": 1, "name": "orphan", "parent": 99}]
})";

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::NotFound);
}

TEST_CASE("a file describing a cycle is refused", "[scene][serialization]") {
    // The cycle check runs on load as well as on edit, because a file is untrusted and a
    // cyclic one would make the transform update not terminate.
    const std::string text = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [
    {"id": 1, "name": "a", "parent": 2},
    {"id": 2, "name": "b", "parent": 1}
  ]
})";

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
}

TEST_CASE("a failed load leaves the scene empty rather than half-populated",
          "[scene][serialization]") {
    // A partially loaded scene is harder to reason about than an empty one, and every caller
    // would have to handle it.
    Scene scene;
    const StableId existing = scene.create("was here");
    REQUIRE(scene.contains(existing));

    const std::string bad = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [{"id": 1, "name": "fine"}, {"id": 0, "name": "broken"}]
})";

    REQUIRE_FALSE(from_text(scene, bad).has_value());

    // Unchanged: the load built its result aside and never committed it.
    CHECK(scene.contains(existing));
    CHECK(scene.size() == 1);
}

TEST_CASE("a layer outside the representable range is refused", "[scene][serialization]") {
    const std::string text = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [{"id": 1, "sprite": {"layer": 99999999999}}]
})";

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("an over-long name is refused", "[scene][serialization]") {
    // Names come from a file and are bounded, so one field cannot make a scene arbitrarily
    // large. The companion entity-count limit is not tested here: a file large enough to
    // exceed it would itself be too large to keep in the repository.
    const std::string long_name(2000, 'x');
    const std::string text = std::format(
        R"({{"format": "atlas-scene", "version": 1, "entities": [{{"id": 1, "name": "{}"}}]}})",
        long_name);

    Scene scene;
    const auto status = from_text(scene, text);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("the world transform is not stored", "[scene][serialization]") {
    // It is derived from the local transforms, and a file that stored both could disagree
    // with itself. Loading must recompute it.
    const Scene original = make_scene();
    const auto text = to_text(original);
    REQUIRE(text.has_value());
    CHECK(text->find("world") == std::string::npos);

    Scene loaded;
    REQUIRE(from_text(loaded, *text).has_value());

    // Recomputed on load, so it is available immediately without the caller asking.
    const auto id = original.entities().front().id;
    CHECK(loaded.world_transform(id) != nullptr);
}

TEST_CASE("the output is readable", "[scene][serialization]") {
    // A scene file is meant to be read and diffed by a person, which is why it is indented
    // text rather than something compact.
    const Scene scene = make_scene();
    const auto text = to_text(scene);
    REQUIRE(text.has_value());

    CHECK(text->find("\"format\": \"atlas-scene\"") != std::string::npos);
    CHECK(text->find("\n") != std::string::npos);
    CHECK(text->back() == '\n');
}
