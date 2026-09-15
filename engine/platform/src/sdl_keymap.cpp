// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_keymap.hpp"

#include <array>
#include <utility>

namespace atlas::platform::detail {
namespace {

/// The single source of truth for the mapping, in both directions.
///
/// A table rather than two switch statements: two switches drift apart, and the drift is
/// invisible until a key stops working. The bijection test walks this table.
constexpr std::array kKeyTable = std::to_array<std::pair<Key, SDL_Scancode>>({
    {Key::A, SDL_SCANCODE_A},
    {Key::B, SDL_SCANCODE_B},
    {Key::C, SDL_SCANCODE_C},
    {Key::D, SDL_SCANCODE_D},
    {Key::E, SDL_SCANCODE_E},
    {Key::F, SDL_SCANCODE_F},
    {Key::G, SDL_SCANCODE_G},
    {Key::H, SDL_SCANCODE_H},
    {Key::I, SDL_SCANCODE_I},
    {Key::J, SDL_SCANCODE_J},
    {Key::K, SDL_SCANCODE_K},
    {Key::L, SDL_SCANCODE_L},
    {Key::M, SDL_SCANCODE_M},
    {Key::N, SDL_SCANCODE_N},
    {Key::O, SDL_SCANCODE_O},
    {Key::P, SDL_SCANCODE_P},
    {Key::Q, SDL_SCANCODE_Q},
    {Key::R, SDL_SCANCODE_R},
    {Key::S, SDL_SCANCODE_S},
    {Key::T, SDL_SCANCODE_T},
    {Key::U, SDL_SCANCODE_U},
    {Key::V, SDL_SCANCODE_V},
    {Key::W, SDL_SCANCODE_W},
    {Key::X, SDL_SCANCODE_X},
    {Key::Y, SDL_SCANCODE_Y},
    {Key::Z, SDL_SCANCODE_Z},
    {Key::Num0, SDL_SCANCODE_0},
    {Key::Num1, SDL_SCANCODE_1},
    {Key::Num2, SDL_SCANCODE_2},
    {Key::Num3, SDL_SCANCODE_3},
    {Key::Num4, SDL_SCANCODE_4},
    {Key::Num5, SDL_SCANCODE_5},
    {Key::Num6, SDL_SCANCODE_6},
    {Key::Num7, SDL_SCANCODE_7},
    {Key::Num8, SDL_SCANCODE_8},
    {Key::Num9, SDL_SCANCODE_9},
    {Key::F1, SDL_SCANCODE_F1},
    {Key::F2, SDL_SCANCODE_F2},
    {Key::F3, SDL_SCANCODE_F3},
    {Key::F4, SDL_SCANCODE_F4},
    {Key::F5, SDL_SCANCODE_F5},
    {Key::F6, SDL_SCANCODE_F6},
    {Key::F7, SDL_SCANCODE_F7},
    {Key::F8, SDL_SCANCODE_F8},
    {Key::F9, SDL_SCANCODE_F9},
    {Key::F10, SDL_SCANCODE_F10},
    {Key::F11, SDL_SCANCODE_F11},
    {Key::F12, SDL_SCANCODE_F12},
    {Key::Escape, SDL_SCANCODE_ESCAPE},
    {Key::Enter, SDL_SCANCODE_RETURN},
    {Key::Space, SDL_SCANCODE_SPACE},
    {Key::Tab, SDL_SCANCODE_TAB},
    {Key::Backspace, SDL_SCANCODE_BACKSPACE},
    {Key::Delete, SDL_SCANCODE_DELETE},
    {Key::Insert, SDL_SCANCODE_INSERT},
    {Key::Home, SDL_SCANCODE_HOME},
    {Key::End, SDL_SCANCODE_END},
    {Key::PageUp, SDL_SCANCODE_PAGEUP},
    {Key::PageDown, SDL_SCANCODE_PAGEDOWN},
    {Key::Left, SDL_SCANCODE_LEFT},
    {Key::Right, SDL_SCANCODE_RIGHT},
    {Key::Up, SDL_SCANCODE_UP},
    {Key::Down, SDL_SCANCODE_DOWN},
    {Key::LeftShift, SDL_SCANCODE_LSHIFT},
    {Key::RightShift, SDL_SCANCODE_RSHIFT},
    {Key::LeftControl, SDL_SCANCODE_LCTRL},
    {Key::RightControl, SDL_SCANCODE_RCTRL},
    {Key::LeftAlt, SDL_SCANCODE_LALT},
    {Key::RightAlt, SDL_SCANCODE_RALT},
    {Key::LeftSuper, SDL_SCANCODE_LGUI},
    {Key::RightSuper, SDL_SCANCODE_RGUI},
    {Key::Minus, SDL_SCANCODE_MINUS},
    {Key::Equals, SDL_SCANCODE_EQUALS},
    {Key::LeftBracket, SDL_SCANCODE_LEFTBRACKET},
    {Key::RightBracket, SDL_SCANCODE_RIGHTBRACKET},
    {Key::Backslash, SDL_SCANCODE_BACKSLASH},
    {Key::Semicolon, SDL_SCANCODE_SEMICOLON},
    {Key::Apostrophe, SDL_SCANCODE_APOSTROPHE},
    {Key::Grave, SDL_SCANCODE_GRAVE},
    {Key::Comma, SDL_SCANCODE_COMMA},
    {Key::Period, SDL_SCANCODE_PERIOD},
    {Key::Slash, SDL_SCANCODE_SLASH},
});

}  // namespace

SDL_Scancode to_sdl_scancode(Key key) noexcept {
    for (const auto& [atlas_key, scancode] : kKeyTable) {
        if (atlas_key == key) {
            return scancode;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

Key from_sdl_scancode(SDL_Scancode scancode) noexcept {
    for (const auto& [atlas_key, code] : kKeyTable) {
        if (code == scancode) {
            return atlas_key;
        }
    }
    return Key::Unknown;
}

}  // namespace atlas::platform::detail
