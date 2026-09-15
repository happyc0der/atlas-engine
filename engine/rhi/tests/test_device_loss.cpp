// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>

#include "device_loss.hpp"
#include <catch2/catch_test_macros.hpp>

#include <string>

using atlas::ErrorCode;
using atlas::rhi::detail::DeviceHealth;
using atlas::rhi::detail::is_device_lost_message;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

}  // namespace

TEST_CASE("a lost Vulkan device is recognised", "[rhi][device-loss]") {
    // The graphics library formats the driver's result code into the message verbatim, so
    // this is the exact text a lost Vulkan device produces. If this stops matching, device
    // loss stops being detected at all on the one backend where it is reliable.
    CHECK(is_device_lost_message("vkQueueSubmit VK_ERROR_DEVICE_LOST"));
    CHECK(is_device_lost_message("vkAcquireNextImageKHR VK_ERROR_SURFACE_LOST_KHR"));
}

TEST_CASE("a removed Direct3D device is recognised", "[rhi][device-loss]") {
    CHECK(is_device_lost_message("DXGI_ERROR_DEVICE_REMOVED"));
    CHECK(is_device_lost_message("DXGI_ERROR_DEVICE_RESET"));
    CHECK(is_device_lost_message("The GPU device instance has been suspended: device removed"));
}

TEST_CASE("matching ignores case", "[rhi][device-loss]") {
    // Half the sources are machine-generated tokens and half are prose from the operating
    // system, so the case is not something to rely on.
    CHECK(is_device_lost_message("vk_error_device_lost"));
    CHECK(is_device_lost_message("Vk_Error_Device_Lost"));
    CHECK(is_device_lost_message("DEVICE REMOVED"));
    CHECK(is_device_lost_message("Device Removed"));

    // Case is ignored; spacing and punctuation are not. Matching loosely here would start
    // catching unrelated prose.
    CHECK_FALSE(is_device_lost_message("device  removed"));
    CHECK_FALSE(is_device_lost_message("device-removed"));
}

TEST_CASE("an ordinary failure is not device loss", "[rhi][device-loss]") {
    // This is the direction that matters most. A false positive latches and takes the rest
    // of the run down with it, so an out-of-memory or a bad parameter must not look like a
    // dead device.
    CHECK_FALSE(is_device_lost_message(""));
    CHECK_FALSE(is_device_lost_message("VK_ERROR_OUT_OF_DEVICE_MEMORY"));
    CHECK_FALSE(is_device_lost_message("VK_ERROR_OUT_OF_HOST_MEMORY"));
    CHECK_FALSE(is_device_lost_message("vkCreateBuffer VK_ERROR_INITIALIZATION_FAILED"));
    CHECK_FALSE(is_device_lost_message("Failed to create shader"));
    CHECK_FALSE(is_device_lost_message("no swapchain image available"));
    CHECK_FALSE(is_device_lost_message("the device is busy"));
}

TEST_CASE("a message longer than any phrase is still matched", "[rhi][device-loss]") {
    const std::string padded =
        std::string(4000, 'x') + " VK_ERROR_DEVICE_LOST " + std::string(4000, 'y');
    CHECK(is_device_lost_message(padded));
}

TEST_CASE("health starts good and stays good through ordinary failures", "[rhi][device-loss]") {
    DeviceHealth health;
    CHECK_FALSE(health.lost());
    CHECK(health.reason().empty());

    CHECK(health.record_failure(ErrorCode::ResourceCreationFailed, "out of memory") ==
          ErrorCode::ResourceCreationFailed);
    CHECK(health.record_failure(ErrorCode::Internal, "VK_ERROR_OUT_OF_DEVICE_MEMORY") ==
          ErrorCode::Internal);

    CHECK_FALSE(health.lost());
}

TEST_CASE("a device-lost failure changes the reported code", "[rhi][device-loss]") {
    DeviceHealth health;

    // The caller asked for Internal and gets DeviceLost instead, which is the whole point:
    // the first failure is named correctly rather than as a generic one.
    CHECK(health.record_failure(ErrorCode::Internal, "vkQueueSubmit VK_ERROR_DEVICE_LOST") ==
          ErrorCode::DeviceLost);
    CHECK(health.lost());
    CHECK(health.reason() == "vkQueueSubmit VK_ERROR_DEVICE_LOST");
}

TEST_CASE("the latch is sticky and keeps the first reason", "[rhi][device-loss]") {
    // Later failures are consequences of the first. Reporting the most recent one would bury
    // the cause under whatever happened to fail next.
    DeviceHealth health;
    REQUIRE(health.record_failure(ErrorCode::Internal, "VK_ERROR_DEVICE_LOST") ==
            ErrorCode::DeviceLost);

    CHECK(health.record_failure(ErrorCode::ResourceCreationFailed, "could not create a buffer") ==
          ErrorCode::DeviceLost);
    CHECK(health.record_failure(ErrorCode::Internal, "") == ErrorCode::DeviceLost);

    CHECK(health.lost());
    CHECK(health.reason() == "VK_ERROR_DEVICE_LOST");
}

TEST_CASE("an over-long reason is truncated rather than dropped", "[rhi][device-loss]") {
    // Recorded on a path where the device has just died, so the reason is copied into a
    // fixed buffer rather than allocated.
    DeviceHealth health;
    const std::string message = "VK_ERROR_DEVICE_LOST " + std::string(2000, 'z');
    REQUIRE(health.record_failure(ErrorCode::Internal, message) == ErrorCode::DeviceLost);

    CHECK(health.reason().size() == DeviceHealth::kMaxReason);
    CHECK(health.reason().starts_with("VK_ERROR_DEVICE_LOST"));
}
