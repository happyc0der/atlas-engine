// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_loss.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace atlas::rhi::detail {
namespace {

/// What a lost device looks like in a message, across the backends that say so at all.
///
/// Deliberately short. Every entry here is a phrase that has been traced to a place the
/// graphics library actually produces it; guessing at additional wording would turn ordinary
/// failures into false reports of a dead device, which is worse than missing one, because a
/// false positive latches and takes the process down with it.
constexpr std::array<std::string_view, 6> kDeviceLostPhrases{
    // Vulkan: the library formats the driver's result code in verbatim.
    "vk_error_device_lost",
    "vk_error_surface_lost_khr",
    // Direct3D 12: the library formats the removal reason as system prose.
    "dxgi_error_device_removed",
    "dxgi_error_device_reset",
    "device removed",
    "device was removed",
};

[[nodiscard]] char lowered(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool contains_ignoring_case(std::string_view haystack,
                                          std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const auto found = std::ranges::search(haystack, needle,
                                           [](char a, char b) { return lowered(a) == lowered(b); });
    return !found.empty();
}

}  // namespace

bool is_device_lost_message(std::string_view message) noexcept {
    return std::ranges::any_of(kDeviceLostPhrases, [message](std::string_view phrase) {
        return contains_ignoring_case(message, phrase);
    });
}

ErrorCode DeviceHealth::record_failure(ErrorCode fallback, std::string_view message) noexcept {
    if (m_lost) {
        // Already gone. Every later failure is a consequence of the first one.
        return ErrorCode::DeviceLost;
    }

    if (!is_device_lost_message(message)) {
        return fallback;
    }

    m_lost = true;
    m_reason_length = std::min(message.size(), kMaxReason);
    std::copy_n(message.begin(), m_reason_length, m_reason.begin());
    return ErrorCode::DeviceLost;
}

}  // namespace atlas::rhi::detail
