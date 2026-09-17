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

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <thread>
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

TEST_CASE("typing into the name field renames through the history", "[tools][gpu]") {
    // The whole path in one test: a platform event reaches the bridge, the bridge feeds the
    // overlay, a widget the test did not place receives the characters, and the edit arrives
    // as one undoable command. Every piece of that is new in M11 and none of it can be seen
    // from a unit test, because the widget only exists while a frame is being built.
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

    // Draw once to learn where the field is. The panel reports its own geometry precisely so
    // a caller can reach a widget it did not position.
    const auto draw_once = [&](float dt) {
        // Large enough to contain the panel where it places itself: the scene panel
        // defaults to y=320 and is 520 tall, so a short display clips the inspector
        // away and no field is drawn at all.
        overlay->begin_frame(dt, 1280, 900);
        auto report = overlay->scene_panel("Scene", history);
        auto gpu_frame = harness->device.begin_frame();
        REQUIRE(gpu_frame.has_value());
        (void)overlay->end_frame(*gpu_frame);
        REQUIRE(harness->device.end_frame(std::move(*gpu_frame)).has_value());
        return report;
    };

    const auto first = draw_once(1.0F / 60.0F);
    REQUIRE(first.name_field.has_value());
    const auto centre = first.name_field->centre();

    // Click into it. The overlay trickles a press and release that arrive together across
    // frames, so the click needs more than one frame to take effect.
    (void)overlay->handle_event(atlas::platform::MouseMoved{
        .position = {.x = centre.x, .y = centre.y}, .delta_x = 0.0F, .delta_y = 0.0F});
    (void)overlay->handle_event(
        atlas::platform::MouseButtonPressed{.button = atlas::platform::MouseButton::Left});
    draw_once(1.0F / 60.0F);
    (void)overlay->handle_event(
        atlas::platform::MouseButtonReleased{.button = atlas::platform::MouseButton::Left});
    draw_once(1.0F / 60.0F);

    // A focused text field is exactly what the application watches to decide whether to
    // engage the platform's text input.
    CHECK(overlay->wants_text_input());

    // Select everything, then type over it. This is also what proves modifiers reach the
    // overlay: without them the shortcut is dead and the text would be appended instead.
    //
    // Sent as two separate presses rather than one with both modifiers held, because the
    // overlay compares the held set for exact equality — Control on most systems, Command on
    // Apple ones — so a chord with both matches neither, which is correct and is what an
    // earlier version of this test got wrong. One of the two does nothing on any given
    // platform; whichever owns the shortcut selects.
    //
    // The trailing frames are not padding. The overlay trickles its input queue so that a
    // press and a release queued together cannot be seen in one frame, so a shortcut needs
    // several frames to land.
    const auto press_select_all = [&](atlas::platform::KeyModifiers modifiers,
                                      atlas::platform::Key modifier_key) {
        (void)overlay->handle_event(
            atlas::platform::KeyPressed{.key = atlas::platform::Key::A, .modifiers = modifiers});
        (void)overlay->handle_event(
            atlas::platform::KeyReleased{.key = atlas::platform::Key::A, .modifiers = modifiers});
        (void)overlay->handle_event(
            atlas::platform::KeyReleased{.key = modifier_key, .modifiers = {}});
        for (int frame = 0; frame < 4; ++frame) {
            draw_once(1.0F / 60.0F);
        }
    };
    press_select_all({.control = true}, atlas::platform::Key::LeftControl);
    press_select_all({.super = true}, atlas::platform::Key::LeftSuper);

    atlas::platform::TextInput typed;
    const std::string_view name = "renamed";
    std::ranges::copy(name, typed.bytes.begin());
    typed.length = static_cast<std::uint8_t>(name.size());
    (void)overlay->handle_event(typed);
    draw_once(1.0F / 60.0F);

    // Enter commits. Several frames, because the overlay trickles its input queue: a key
    // event queued behind characters is deferred to a later frame on purpose, so that a
    // press and a release in one frame cannot be seen together.
    (void)overlay->handle_event(atlas::platform::KeyPressed{.key = atlas::platform::Key::Enter});
    (void)overlay->handle_event(atlas::platform::KeyReleased{.key = atlas::platform::Key::Enter});
    for (int frame = 0; frame < 4; ++frame) {
        draw_once(1.0F / 60.0F);
    }

    CHECK(fixture.scene.name(fixture.child) == "renamed");
    REQUIRE(history.undo_depth() == 1);
    CHECK(history.undo_label() == "rename");

    // And it is a real history entry, not a direct write: undo restores the old name.
    CHECK(history.undo());
    CHECK(fixture.scene.name(fixture.child) == "child");

    REQUIRE(harness->device.wait_idle().has_value());
}

TEST_CASE("typing into the log filter changes what the console shows", "[tools][gpu]") {
    // The same path against the field that has existed since M9 and has never been able to
    // receive a character. If this passes, the log console's filter works for the first time.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    auto overlay = DebugUi::create(harness->device, harness->window);
    if (!overlay) {
        SKIP("the overlay could not be created on this device");
    }

    atlas::log::LogBuffer buffer{64};
    const auto record = [](std::string_view category, std::string_view message) {
        return atlas::log::Record{.timestamp = std::chrono::system_clock::now(),
                                  .thread = std::this_thread::get_id(),
                                  .category = atlas::log::Category{category},
                                  .severity = atlas::log::Severity::Info,
                                  .message = message,
                                  .where = std::source_location::current()};
    };
    buffer.push(record("alpha", "one"));
    buffer.push(record("alpha", "two"));
    buffer.push(record("beta", "three"));

    const auto draw_once = [&] {
        // The log console defaults to y=510 and is 280 tall; see the note above.
        overlay->begin_frame(1.0F / 60.0F, 1280, 900);
        auto report = overlay->log_console_panel("Log", buffer);
        auto gpu_frame = harness->device.begin_frame();
        REQUIRE(gpu_frame.has_value());
        (void)overlay->end_frame(*gpu_frame);
        REQUIRE(harness->device.end_frame(std::move(*gpu_frame)).has_value());
        return report;
    };

    const auto unfiltered = draw_once();
    CHECK(unfiltered.shown == 3);
    CHECK(unfiltered.hidden == 0);
    REQUIRE(unfiltered.filter_field.has_value());
    const auto centre = unfiltered.filter_field->centre();

    (void)overlay->handle_event(atlas::platform::MouseMoved{
        .position = {.x = centre.x, .y = centre.y}, .delta_x = 0.0F, .delta_y = 0.0F});
    (void)overlay->handle_event(
        atlas::platform::MouseButtonPressed{.button = atlas::platform::MouseButton::Left});
    draw_once();
    (void)overlay->handle_event(
        atlas::platform::MouseButtonReleased{.button = atlas::platform::MouseButton::Left});
    draw_once();

    atlas::platform::TextInput typed;
    const std::string_view filter = "alp";
    std::ranges::copy(filter, typed.bytes.begin());
    typed.length = static_cast<std::uint8_t>(filter.size());
    (void)overlay->handle_event(typed);

    const auto filtered = draw_once();
    CHECK(filtered.shown == 2);
    CHECK(filtered.hidden == 1);

    REQUIRE(harness->device.wait_idle().has_value());
}

TEST_CASE("the overlay reports where an input method should put its candidate list",
          "[tools][gpu]") {
    // What can be checked without an input method: that focusing a field makes the overlay
    // ask for the candidate list, near the field, and that losing focus withdraws the ask.
    // Whether the operating system then puts the list there is a manual check, recorded in
    // the milestone report, because no runner has an input method installed.
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

    const auto draw_once = [&] {
        overlay->begin_frame(1.0F / 60.0F, 1280, 900);
        auto report = overlay->scene_panel("Scene", history);
        auto gpu_frame = harness->device.begin_frame();
        REQUIRE(gpu_frame.has_value());
        (void)overlay->end_frame(*gpu_frame);
        REQUIRE(harness->device.end_frame(std::move(*gpu_frame)).has_value());
        return report;
    };

    const auto first = draw_once();
    REQUIRE(first.name_field.has_value());
    // Nothing focused, so nothing is asked for.
    CHECK_FALSE(overlay->ime_request().visible);

    const auto centre = first.name_field->centre();
    (void)overlay->handle_event(atlas::platform::MouseMoved{
        .position = {.x = centre.x, .y = centre.y}, .delta_x = 0.0F, .delta_y = 0.0F});
    (void)overlay->handle_event(
        atlas::platform::MouseButtonPressed{.button = atlas::platform::MouseButton::Left});
    draw_once();
    (void)overlay->handle_event(
        atlas::platform::MouseButtonReleased{.button = atlas::platform::MouseButton::Left});
    draw_once();

    const auto request = overlay->ime_request();
    CHECK(request.visible);
    CHECK(request.line_height > 0.0F);
    // Near the field it belongs to. Loose bounds on purpose: the exact caret offset is the
    // library's business and would make this a test of its text layout.
    CHECK(request.y >= first.name_field->y - 4.0F);
    CHECK(request.y <= first.name_field->y + first.name_field->height + 4.0F);
    CHECK(request.x >= first.name_field->x - 4.0F);

    // Clicking away withdraws the request, which is what stops a candidate list lingering
    // over a panel nobody is typing into.
    (void)overlay->handle_event(atlas::platform::MouseMoved{
        .position = {.x = 5.0F, .y = 5.0F}, .delta_x = 0.0F, .delta_y = 0.0F});
    (void)overlay->handle_event(
        atlas::platform::MouseButtonPressed{.button = atlas::platform::MouseButton::Left});
    draw_once();
    (void)overlay->handle_event(
        atlas::platform::MouseButtonReleased{.button = atlas::platform::MouseButton::Left});
    draw_once();
    draw_once();

    CHECK_FALSE(overlay->ime_request().visible);

    REQUIRE(harness->device.wait_idle().has_value());
}
