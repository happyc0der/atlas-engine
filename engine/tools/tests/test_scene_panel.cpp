// SPDX-License-Identifier: GPL-3.0-or-later
// The overlay's first tests. It had none before M9, because nothing in it could be exercised
// without a graphics device; these carry the "gpu" label and skip when there is none, which is
// what that label means.
#include <atlas/edit/history.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/scene/scene.hpp>
#include <atlas/tools/debug_ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <utility>

using atlas::edit::History;
using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::rhi::Device;
using atlas::scene::Scene;
using atlas::scene::StableId;
using atlas::tools::DebugUi;

namespace {

struct Harness {
    Platform platform;
    Window window;
    Device device;
};

[[nodiscard]] std::optional<Harness> make_harness() {
    auto platform = Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    auto window = platform->create_window(
        {.title = "Atlas tools test", .width = 320, .height = 240, .hidden = true});
    if (!window) {
        return std::nullopt;
    }
    auto device = Device::create({.debug = true}, *window);
    if (!device) {
        return std::nullopt;
    }
    return Harness{std::move(*platform), std::move(*window), std::move(*device)};
}

struct Fixture {
    Scene scene;
    StableId root = StableId::None;
    StableId child = StableId::None;
};

[[nodiscard]] Fixture make_scene() {
    Fixture fixture;
    fixture.root = fixture.scene.create("root");
    fixture.child = fixture.scene.create("child");
    REQUIRE(fixture.scene.set_parent(fixture.child, fixture.root).has_value());
    fixture.scene.set_local_transform(fixture.child, {.position = {.x = 2.0F, .y = 3.0F}});
    fixture.scene.set_sprite(fixture.child, {});
    fixture.scene.update_transforms();
    return fixture;
}

}  // namespace

TEST_CASE("the scene panel draws a history over several frames and leaks nothing", "[tools][gpu]") {
    // What this catches that a compile cannot: a mismatched Begin/End pair in the panel, which
    // Dear ImGui reports by aborting inside its own assertion rather than by returning an
    // error. The panel gained a nested child region and a pinned row of controls in M9, so the
    // pairing is no longer trivially obvious by reading it.
    //
    // It also checks the overlay's own graphics resources, which nothing did before: the
    // device reports anything still alive when it shuts down, so comparing the counts before
    // creating the overlay and after destroying it is a leak test for the overlay itself.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }

    const auto baseline = harness->device.resource_counts();

    {
        auto overlay = DebugUi::create(harness->device, harness->window);
        if (!overlay) {
            SKIP("the overlay could not be created on this device");
        }

        Fixture fixture = make_scene();
        History history{fixture.scene};
        overlay->select_entity(fixture.child);

        for (int frame = 0; frame < 3; ++frame) {
            overlay->begin_frame(1.0F / 60.0F, 320, 240);
            overlay->scene_panel("Scene", history);
            auto gpu_frame = harness->device.begin_frame();
            REQUIRE(gpu_frame.has_value());
            const auto prepared = overlay->end_frame(*gpu_frame);
            // Whether a swapchain image exists depends on the window, which is hidden here;
            // the panel has already been built either way, which is what is under test.
            (void)prepared;
            REQUIRE(harness->device.end_frame(std::move(*gpu_frame)).has_value());
        }

        // The selection survives the frames and is readable from outside, which is how the
        // application knows what the user picked.
        REQUIRE(overlay->selected_entity().has_value());
        CHECK(*overlay->selected_entity() == fixture.child);

        // Nothing was dragged, so nothing was edited. A panel that emitted a command per
        // frame from an untouched widget would show up here rather than as a user noticing
        // their undo stack full of edits they never made.
        CHECK(history.undo_depth() == 0);
        CHECK(history.revision() == 0);
    }

    REQUIRE(harness->device.wait_idle().has_value());
    const auto after = harness->device.resource_counts();
    CHECK(after.buffers == baseline.buffers);
    CHECK(after.textures == baseline.textures);
    CHECK(after.samplers == baseline.samplers);
    CHECK(after.pipelines == baseline.pipelines);
}

TEST_CASE("the panel drops a selection whose entity has been destroyed", "[tools][gpu]") {
    // The panel does not own the scene and is not told when an entity goes away, so a stale
    // selection is a normal thing to hold. It must be dropped rather than passed to an
    // inspector that would then read a component of nothing.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }

    auto overlay = DebugUi::create(harness->device, harness->window);
    if (!overlay) {
        SKIP("the overlay could not be created on this device");
    }

    Fixture fixture = make_scene();
    History history{fixture.scene};
    overlay->select_entity(fixture.child);

    REQUIRE(history.apply(std::make_unique<atlas::edit::Destroy>(fixture.child)).has_value());

    overlay->begin_frame(1.0F / 60.0F, 320, 240);
    overlay->scene_panel("Scene", history);
    auto gpu_frame = harness->device.begin_frame();
    REQUIRE(gpu_frame.has_value());
    (void)overlay->end_frame(*gpu_frame);
    REQUIRE(harness->device.end_frame(std::move(*gpu_frame)).has_value());

    CHECK_FALSE(overlay->selected_entity().has_value());

    // And an undo brings the entity back without bringing the selection back: selection is the
    // panel's state, not the scene's, and the panel was never told what the undo restored.
    CHECK(history.undo());
    CHECK(fixture.scene.contains(fixture.child));
    CHECK_FALSE(overlay->selected_entity().has_value());

    REQUIRE(harness->device.wait_idle().has_value());
}
