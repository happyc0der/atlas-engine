// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Recognising a lost graphics device, and remembering that it was lost.
///
/// A device can stop working underneath a running process: a driver resets, an external
/// display is unplugged, a graphics processor is removed or hangs. What follows is a cascade
/// of unrelated-looking failures, because every later call fails too. This exists so the
/// first one is named correctly and the rest are not reported at all.
///
/// **Atlas does not recover from device loss.** Recovering means recreating the device and
/// every resource on it, which is a large amount of code that cannot be tested on the
/// hardware this project has. The charter puts recovery out of scope for v0.1; detecting it
/// and failing with a clear reason is in scope, and that is what this is.
///
/// **What can actually be detected, by backend.** The graphics library exposes no
/// device-lost query of any kind, so this works by reading the message it leaves behind.
///
/// - **Vulkan**: reliable. The library formats the driver's result code into the message
///   verbatim, so a lost device always says `VK_ERROR_DEVICE_LOST`.
/// - **Direct3D 12**: best effort. The library asks the system to format the removal reason
///   into prose, so the wording depends on the reason and on the system's language. The
///   phrases matched below cover the common English cases and nothing more.
/// - **Metal**: not detected. Metal has no device-lost notion that reaches the library, so on
///   this backend a lost device presents as ordinary failures and `DeviceHealth` never
///   latches. This is a real limitation and not a gap waiting to be filled.
///
/// Nothing here touches the graphics library, so it is testable without a device.

#include <atlas/core/error.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace atlas::rhi::detail {

/// Whether a message from the graphics library describes a lost or removed device.
///
/// Matching is case-insensitive, because only some of the sources are machine-generated
/// tokens; the rest is prose from the operating system.
[[nodiscard]] bool is_device_lost_message(std::string_view message) noexcept;

/// Whether the device is still usable, and why not if it is not.
///
/// Latches: once a device is lost it stays lost. There is no sequence of later successes
/// that makes a lost device safe to use again, and a flag that could flicker back would let
/// a caller retry into undefined behaviour.
class DeviceHealth {
  public:
    /// Classify one failure and remember it if it means the device is gone.
    ///
    /// `fallback` is the code to use when the failure is an ordinary one. Returns the code
    /// the caller should report, which is `fallback` unless the device was lost.
    ErrorCode record_failure(ErrorCode fallback, std::string_view message) noexcept;

    [[nodiscard]] bool lost() const noexcept { return m_lost; }

    /// The message that first said the device was gone. Empty while it is healthy.
    ///
    /// The first one, not the most recent: later failures are consequences, and reporting
    /// them would bury the cause. Truncated to fit, because this is recorded on a path where
    /// the graphics device has just died and allocating there is a second way to fail.
    [[nodiscard]] std::string_view reason() const noexcept {
        return std::string_view{m_reason.data(), m_reason_length};
    }

    /// Longest reason kept. Enough for the messages the backends actually produce.
    static constexpr std::size_t kMaxReason = 256;

  private:
    bool m_lost = false;
    std::array<char, kMaxReason> m_reason{};
    std::size_t m_reason_length = 0;
};

}  // namespace atlas::rhi::detail
