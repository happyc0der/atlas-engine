// SPDX-License-Identifier: GPL-3.0-or-later
// The statistics panel, and whether its text reached the catalog at all.
//
// M16 routed every string the overlay shows through `text::Catalog` and shipped with the panel
// titles, the statistic labels and the mode buttons still showing their keys. `--text-check`
// passed, because it checks that every key resolves and not that every widget asks. Nothing
// here reads pixels; what it reads is the catalog's own count of how often it was asked, which
// a panel that resolves nothing cannot fake.
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/text/catalog.hpp>
#include <atlas/tools/debug_ui.hpp>
#include <atlas/tools/panels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::rhi::Device;
using atlas::text::Catalog;
using atlas::tools::DebugUi;
using atlas::tools::SimulationControlsView;
using atlas::tools::Stat;

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

constexpr std::string_view kTitle = "test.title";
constexpr std::array<std::string_view, 3> kLabels{"test.stat.one", "test.stat.two",
                                                  "test.stat.three"};

/// One title and three labels: the number of keys the panel is handed, and therefore the
/// number of lookups a frame must make. Values are the application's to compose and are shown
/// as given, so they are not counted.
constexpr std::size_t kKeysPerFrame = 1 + kLabels.size();

void draw_once(Harness& harness, DebugUi& overlay) {
    const std::array<Stat, 3> stats{
        Stat{.label = kLabels[0], .value = "1"},
        Stat{.label = kLabels[1], .value = "2"},
        Stat{.label = kLabels[2], .value = "3"},
    };
    overlay.begin_frame(1.0F / 60.0F, 320, 240);
    overlay.stats_panel(kTitle, stats);
    auto gpu_frame = harness.device.begin_frame();
    REQUIRE(gpu_frame.has_value());
    (void)overlay.end_frame(*gpu_frame);
    REQUIRE(harness.device.end_frame(std::move(*gpu_frame)).has_value());
}

}  // namespace

TEST_CASE("the statistics panel asks the catalog for its title and every label", "[tools][gpu]") {
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    auto overlay = DebugUi::create(harness->device, harness->window);
    if (!overlay) {
        SKIP("the overlay could not be created on this device");
    }

    Catalog catalog;
    REQUIRE(catalog.insert(kTitle, "Title").has_value());
    for (const std::string_view label : kLabels) {
        REQUIRE(catalog.insert(label, "Label").has_value());
    }
    overlay->set_catalog(&catalog);

    draw_once(*harness, *overlay);

    // `misses() == 0` was true of the M16 overlay too, because it never asked. The lookup
    // count is the assertion with teeth: exactly one per key handed to the panel, and a title
    // built from the raw key, or a label shown unresolved, is one lookup short.
    CHECK(catalog.misses() == 0);
    CHECK(catalog.lookups() == kKeysPerFrame);

    draw_once(*harness, *overlay);
    CHECK(catalog.lookups() == 2 * kKeysPerFrame);

    REQUIRE(harness->device.wait_idle().has_value());
}

TEST_CASE("against an empty catalog every key the panel shows is a distinct miss", "[tools][gpu]") {
    // The mirror image: with nothing loaded, each key is looked up, missed, and remembered
    // once. Four distinct misses says the four keys reached the catalog; a panel that showed
    // one of them raw would record three.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    auto overlay = DebugUi::create(harness->device, harness->window);
    if (!overlay) {
        SKIP("the overlay could not be created on this device");
    }

    Catalog catalog;
    overlay->set_catalog(&catalog);

    draw_once(*harness, *overlay);
    draw_once(*harness, *overlay);

    CHECK(catalog.distinct_misses() == kKeysPerFrame);
    CHECK(catalog.misses() == 2 * kKeysPerFrame);
    CHECK(catalog.lookups() == 2 * kKeysPerFrame);

    REQUIRE(harness->device.wait_idle().has_value());
}

TEST_CASE("the controls panel resolves each display-mode name through the catalog",
          "[tools][gpu]") {
    // The mode buttons were the third thing M16 left raw. The controls panel resolves many
    // strings of its own, so the exact count is not asserted; what is asserted is the
    // difference each mode makes to the distinct misses against an empty catalog.
    //
    // None against one, deliberately, and the count is two: the "display mode" heading is
    // drawn only when there are any modes, and the first mode's name. A draft of this test
    // compared one mode against two and a raw button survived it, because the panel also
    // looks up the *next* mode's name to decide whether to wrap, and that lookup resolved the
    // second name whether or not the button did. The first mode has no predecessor, so its
    // name reaches the catalog only through the button.
    auto harness = make_harness();
    if (!harness) {
        SKIP("no graphics device available");
    }
    auto overlay = DebugUi::create(harness->device, harness->window);
    if (!overlay) {
        SKIP("the overlay could not be created on this device");
    }

    constexpr std::array<std::string_view, 2> kModes{"test.mode.first", "test.mode.second"};

    const auto draw_controls = [&](Catalog& catalog, std::span<const std::string_view> modes) {
        overlay->set_catalog(&catalog);
        overlay->begin_frame(1.0F / 60.0F, 640, 480);
        (void)overlay->simulation_controls_panel(kTitle, SimulationControlsView{
                                                             .speed = {},
                                                             .tick = 0,
                                                             .modes = modes,
                                                             .mode_index = 0,
                                                         });
        auto gpu_frame = harness->device.begin_frame();
        REQUIRE(gpu_frame.has_value());
        (void)overlay->end_frame(*gpu_frame);
        REQUIRE(harness->device.end_frame(std::move(*gpu_frame)).has_value());
    };

    Catalog no_modes;
    draw_controls(no_modes, {});
    Catalog one_mode;
    draw_controls(one_mode, std::span{kModes}.first(1));
    Catalog two_modes;
    draw_controls(two_modes, kModes);

    CHECK(one_mode.distinct_misses() == no_modes.distinct_misses() + 2);
    CHECK(two_modes.distinct_misses() == one_mode.distinct_misses() + 1);
    // And the title reached it: the panel's own keys are many, but this one is the one M16
    // missed, so it is named.
    CHECK(no_modes.lookups() >= 1);

    REQUIRE(harness->device.wait_idle().has_value());
}
