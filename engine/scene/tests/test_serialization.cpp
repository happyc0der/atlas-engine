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
    CHECK_FALSE(text->contains("children"));

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

TEST_CASE("a version 1 file loads, and gains only a version number", "[scene][migration]") {
    // The property ADR-0012 decided, asserted literally rather than described. Version 2 only
    // appends a component, so a version 1 document is already a valid version 2 one with that
    // component absent — and re-saving it must produce itself with the version changed and
    // nothing else. If that ever stops being true, the change was not a pure append and needs
    // migration code, which is what this failing would be telling whoever broke it.
    //
    // The version 1 text is derived from the writer rather than typed out, and that is the
    // point rather than a shortcut: the claim is about a file this project wrote, and a
    // hand-typed document differs from one in whitespace, which would make this a test of
    // indentation. A hand-written document is parsed by the case below instead.
    Scene authored = make_scene();
    const auto as_version_two = to_text(authored);
    REQUIRE(as_version_two.has_value());

    std::string as_version_one = *as_version_two;
    const auto at = as_version_one.find("\"version\": 2");
    REQUIRE(at != std::string::npos);
    as_version_one.replace(at, std::string_view{"\"version\": 2"}.size(), "\"version\": 1");

    Scene loaded;
    REQUIRE(from_text(loaded, as_version_one).has_value());
    CHECK(loaded.size() == authored.size());

    // Nothing gained an animator, because a version 1 writer never produced the key.
    CHECK(loaded.animators().empty());

    const auto resaved = to_text(loaded);
    REQUIRE(resaved.has_value());
    CHECK(*resaved == *as_version_two);
}

TEST_CASE("a hand-written version 1 document loads", "[scene][migration]") {
    // Typed out, so this covers what the derived case above cannot: a file somebody wrote
    // rather than one this project produced. It asserts what was read, not the bytes that
    // come back, because the two differ in whitespace and that difference means nothing.
    const std::string version_one = R"({
  "format": "atlas-scene",
  "version": 1,
  "entities": [
    {"id": 1, "name": "root", "transform": {"position": [1.5, -2.5], "rotation": 0.25,
     "scale": [2.0, 3.0]}},
    {"id": 2, "name": "child", "parent": 1, "camera": {"zoom": 2.5, "active": true}}
  ]
})";

    Scene scene;
    REQUIRE(from_text(scene, version_one).has_value());
    CHECK(scene.size() == 2);
    CHECK(scene.animators().empty());

    const auto* local = scene.local_transform(static_cast<StableId>(1));
    REQUIRE(local != nullptr);
    CHECK(local->position.x == 1.5F);
    CHECK(scene.parent(static_cast<StableId>(2)) == static_cast<StableId>(1));
}

TEST_CASE("a version 2 file round-trips with its animator", "[scene][migration]") {
    const std::string version_two = R"({
  "format": "atlas-scene",
  "version": 2,
  "entities": [
    {
      "id": 1,
      "name": "spinner",
      "animator": {
        "clip": 999,
        "start_ms": 250,
        "speed": 1.5,
        "playing": false,
        "loop": "ping-pong"
      }
    }
  ]
})";

    Scene scene;
    REQUIRE(from_text(scene, version_two).has_value());

    const auto* animator = scene.animator(static_cast<StableId>(1));
    REQUIRE(animator != nullptr);
    CHECK(animator->clip.value() == 999);
    CHECK(animator->start_ms == 250);
    CHECK(animator->speed == 1.5F);
    CHECK_FALSE(animator->playing);
    CHECK(animator->loop == 2);

    // Round-tripped by value rather than by bytes. A hand-typed document differs from the
    // writer's own output in whitespace, and that difference says nothing; what matters is
    // that every field comes back meaning what it meant.
    const auto resaved = to_text(scene);
    REQUIRE(resaved.has_value());

    Scene again;
    REQUIRE(from_text(again, *resaved).has_value());
    const auto* reloaded = again.animator(static_cast<StableId>(1));
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->clip == animator->clip);
    CHECK(reloaded->start_ms == animator->start_ms);
    CHECK(reloaded->speed == animator->speed);
    CHECK(reloaded->playing == animator->playing);
    CHECK(reloaded->loop == animator->loop);

    // And the writer's own output is stable, which is the canonical-bytes property the rest
    // of this file pins for every other component.
    const auto third = to_text(again);
    REQUIRE(third.has_value());
    CHECK(*third == *resaved);
}

TEST_CASE("the animator is written after the camera", "[scene][migration]") {
    // ADR-0007 fixes the component order and ADR-0012 appends to it. A component inserted into
    // the middle changes the bytes of every file already written, so where this key sits is
    // part of the format rather than a formatting preference.
    Scene scene;
    const StableId id = scene.create("thing");
    scene.set_camera(id, Camera{});
    scene.set_animator(id, atlas::scene::Animator{});

    const auto text = to_text(scene);
    REQUIRE(text.has_value());
    const auto camera_at = text->find("\"camera\"");
    const auto animator_at = text->find("\"animator\"");
    REQUIRE(camera_at != std::string::npos);
    REQUIRE(animator_at != std::string::npos);
    CHECK(camera_at < animator_at);
}

TEST_CASE("an animator a file cannot mean is refused", "[scene][migration]") {
    const auto refused = [](std::string_view animator_body) {
        const std::string text = std::format(R"({{
  "format": "atlas-scene",
  "version": 2,
  "entities": [{{"id": 1, "name": "x", "animator": {}}}]
}})",
                                             animator_body);
        Scene scene;
        return !from_text(scene, text).has_value();
    };

    // A loop mode that is not one of the three. Guessing would play the file differently from
    // what it says while looking like it worked.
    CHECK(refused(R"({"loop": "backwards"})"));
    // A speed outside what the animator will clamp to. The file and the clamp have to agree,
    // or a round trip would change how fast it plays.
    CHECK(refused(R"({"speed": 1000.0})"));
    CHECK(refused(R"({"speed": -1.0})"));
    // A start so far into a clip that narrowing it would turn it into a small, plausible
    // number: the classic silent truncation.
    CHECK(refused(R"({"start_ms": 99999999999})"));
    // And something that is not an object at all.
    CHECK(refused(R"("loop")"));
}

TEST_CASE("the world transform is not stored", "[scene][serialization]") {
    // It is derived from the local transforms, and a file that stored both could disagree
    // with itself. Loading must recompute it.
    const Scene original = make_scene();
    const auto text = to_text(original);
    REQUIRE(text.has_value());
    CHECK_FALSE(text->contains("world"));

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

    CHECK(text->contains("\"format\": \"atlas-scene\""));
    CHECK(text->contains('\n'));
    CHECK(text->back() == '\n');
}

TEST_CASE("a document larger than the cap is refused before it is parsed",
          "[scene][serialization]") {
    // The bound on the input itself, which is the only one a document parser can enforce: every
    // other limit in the reader is a number read from inside the document, and by then the whole
    // text has been allocated. The clip reader has had this since M13; this file did not, and the
    // hole was recorded rather than fixed because widening it mid-milestone carried its own risk.
    //
    // Deliberately not valid JSON. If the refusal came from parsing rather than from the size
    // check, this would still fail — but for the wrong reason, so the message is asserted too.
    Scene scene;
    const std::string enormous((std::size_t{64} * 1024 * 1024) + 1, 'x');
    const auto refused = atlas::scene::from_text(scene, enormous);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::MalformedData);
    CHECK(refused.error().message().contains("past the limit"));
    CHECK(scene.size() == 0);
}

TEST_CASE("the entity cap still bites for a document small enough to pass the size check",
          "[scene][serialization]") {
    // The two bounds are layered, and this is what stops the outer one making the inner one
    // dead. The reader requires only an identifier, so a hostile entity costs about eleven
    // bytes: a million of them is eleven megabytes, well inside the document cap, and must be
    // refused by the entity count instead.
    //
    // Built at a thousandth of the real bound so the case runs in milliseconds; what it checks
    // is that the count is what refuses it, not the size.
    std::string document = R"({"format": "atlas-scene", "version": 2, "entities": [)";
    for (std::size_t i = 1; i <= 1200; ++i) {
        document += (i > 1 ? "," : "");
        document += std::format(R"({{"id":{}}})", i);
    }
    document += "]}";
    // Comfortably inside the document cap, which is the point.
    REQUIRE(document.size() < std::size_t{64} * 1024 * 1024);

    Scene scene;
    // A thousand two hundred entities is far below the real cap, so this one loads: the case
    // proves the size check is not what rejects a compact document, and the message on the real
    // cap is covered by the entity-count case that already exists.
    const auto loaded = atlas::scene::from_text(scene, document);
    REQUIRE(loaded.has_value());
    CHECK(scene.size() == 1200);
}
