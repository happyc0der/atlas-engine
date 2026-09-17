// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/event.hpp>

#include <array>
#include <cstddef>
#include <type_traits>
#include <variant>

namespace atlas::platform {
namespace {

/// Names, positionally matched to the alternatives of Event.
///
/// A table indexed by `Event::index()` rather than a `std::visit`: visit is declared to
/// throw if the variant is valueless, which cannot happen here but which the compiler and
/// the analyser cannot know, and `event_name` is declared noexcept. The static assertions
/// below make the positional match safe against both additions and reordering, which is
/// the only thing a table would otherwise get wrong.
constexpr std::array<std::string_view, std::variant_size_v<Event>> kEventNames{
    "QuitRequested",  "WindowCloseRequested", "WindowResized",       "WindowMinimized",
    "WindowRestored", "WindowFocusGained",    "WindowFocusLost",     "WindowDisplayScaleChanged",
    "KeyPressed",     "KeyReleased",          "TextInput",           "TextEditing",
    "MouseMoved",     "MouseButtonPressed",   "MouseButtonReleased", "MouseWheel",
};

template <std::size_t Index, typename T>
inline constexpr bool kAlternativeIs = std::is_same_v<std::variant_alternative_t<Index, Event>, T>;

// Adding, removing or reordering an alternative breaks one of these at compile time.
static_assert(kAlternativeIs<0, QuitRequested>);
static_assert(kAlternativeIs<1, WindowCloseRequested>);
static_assert(kAlternativeIs<2, WindowResized>);
static_assert(kAlternativeIs<3, WindowMinimized>);
static_assert(kAlternativeIs<4, WindowRestored>);
static_assert(kAlternativeIs<5, WindowFocusGained>);
static_assert(kAlternativeIs<6, WindowFocusLost>);
static_assert(kAlternativeIs<7, WindowDisplayScaleChanged>);
static_assert(kAlternativeIs<8, KeyPressed>);
static_assert(kAlternativeIs<9, KeyReleased>);
static_assert(kAlternativeIs<10, TextInput>);
static_assert(kAlternativeIs<11, TextEditing>);
static_assert(kAlternativeIs<12, MouseMoved>);
static_assert(kAlternativeIs<13, MouseButtonPressed>);
static_assert(kAlternativeIs<14, MouseButtonReleased>);
static_assert(kAlternativeIs<15, MouseWheel>);

// The property the valueless check below relies on, and which the text events were designed
// around: an alternative that owned memory would make a valueless variant reachable and would
// allocate inside pump(), which promises it does not.
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace

std::string_view event_name(const Event& event) noexcept {
    // Every alternative is trivially copyable, so a valueless variant is not reachable.
    // Handling it anyway costs one comparison and removes the last way this could surprise.
    if (event.valueless_by_exception()) {
        return "Valueless";
    }
    return kEventNames[event.index()];
}

}  // namespace atlas::platform
