// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A platform, a window and a device, as the renderer's own GPU tests build them. A test that
// cannot get a device is skipped rather than failed, which is what the "gpu" label means.

#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include <optional>
#include <utility>

namespace atlas::lab::testing {

struct GpuHarness {
    platform::Platform platform;
    platform::Window window;
    rhi::Device device;

    GpuHarness(platform::Platform p, platform::Window w, rhi::Device d)
        : platform(std::move(p)), window(std::move(w)), device(std::move(d)) {}

    GpuHarness(const GpuHarness&) = delete;
    GpuHarness& operator=(const GpuHarness&) = delete;
    GpuHarness(GpuHarness&&) = delete;
    GpuHarness& operator=(GpuHarness&&) = delete;
    ~GpuHarness() = default;
};

[[nodiscard]] inline std::optional<GpuHarness> make_gpu_harness(std::uint32_t width = 128,
                                                                std::uint32_t height = 128) {
    auto platform = platform::Platform::create({.video = true});
    if (!platform) {
        return std::nullopt;
    }
    auto window = platform->create_window(
        {.title = "Atlas lab test", .width = width, .height = height, .resizable = false});
    if (!window) {
        return std::nullopt;
    }
    auto device = rhi::Device::create({.debug = true}, *window);
    if (!device) {
        return std::nullopt;
    }
    return std::make_optional<GpuHarness>(std::move(*platform), std::move(*window),
                                          std::move(*device));
}

}  // namespace atlas::lab::testing
