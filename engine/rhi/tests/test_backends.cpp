// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether this build can draw Atlas's shaders at all, asked without a graphics device
// (ADR-0025 D3).
//
// Until M28 the device asked the library for DXIL, which Atlas has never shipped, and the
// library on Windows was built with Direct3D 12 as its only GPU backend. Windows therefore got a
// backend on which no Atlas shader could load. Every test that would have shown it needs a GPU,
// and no lane with a GPU runs Windows, so it went unseen from M2 to M28. These cases read what
// the library was compiled with, which is the same with or without a device, so every lane runs
// them: a hosted Windows runner fails here on the build that could not draw.

#include "backends.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using atlas::rhi::kShippedShaderFormats;
using atlas::rhi::ShaderFormat;
using atlas::rhi::detail::backend_shader_formats;
using atlas::rhi::detail::compiled_backends;
using atlas::rhi::detail::requested_shader_formats;

namespace {

[[nodiscard]] bool shipped(ShaderFormat format) {
    return std::ranges::find(kShippedShaderFormats, format) != kShippedShaderFormats.end();
}

[[nodiscard]] std::string names(const std::vector<std::string>& backends) {
    std::string joined;
    for (const auto& name : backends) {
        joined += joined.empty() ? name : ", " + name;
    }
    return joined.empty() ? "none" : joined;
}

}  // namespace

TEST_CASE("the device asks only for shader formats Atlas ships", "[rhi][backends]") {
    // A format asked for and not shipped lets the library choose a backend no shader loads on,
    // which is exactly how Windows came to be given Direct3D 12.
    const auto requested = requested_shader_formats();
    REQUIRE_FALSE(requested.empty());
    for (const ShaderFormat format : requested) {
        INFO("requested " << atlas::rhi::to_string(format));
        CHECK(shipped(format));
    }
}

TEST_CASE("this build of the library has a backend that takes a format Atlas ships",
          "[rhi][backends]") {
    const auto backends = compiled_backends();
    INFO("the library was built with: " << names(backends));
    const auto requested = requested_shader_formats();
    const bool drawable = std::ranges::any_of(backends, [&](const std::string& backend) {
        return std::ranges::any_of(backend_shader_formats(backend), [&](ShaderFormat format) {
            return std::ranges::find(requested, format) != requested.end();
        });
    });
    CHECK(drawable);
}

TEST_CASE("every backend the library was built with is one Atlas has a name for",
          "[rhi][backends]") {
    // A backend the table below does not know takes formats nobody wrote down, and the case
    // above would treat it as taking none. Adding it to the table is a decision; this makes
    // sure it is made rather than inherited from a library update.
    for (const auto& backend : compiled_backends()) {
        INFO("backend " << backend);
        CHECK_FALSE(backend_shader_formats(backend).empty());
    }
}
