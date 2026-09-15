// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/key.hpp>

namespace atlas::platform {

std::string_view to_string(Key key) noexcept {
    switch (key) {
    case Key::Unknown: return "Unknown";
    case Key::A: return "A";
    case Key::B: return "B";
    case Key::C: return "C";
    case Key::D: return "D";
    case Key::E: return "E";
    case Key::F: return "F";
    case Key::G: return "G";
    case Key::H: return "H";
    case Key::I: return "I";
    case Key::J: return "J";
    case Key::K: return "K";
    case Key::L: return "L";
    case Key::M: return "M";
    case Key::N: return "N";
    case Key::O: return "O";
    case Key::P: return "P";
    case Key::Q: return "Q";
    case Key::R: return "R";
    case Key::S: return "S";
    case Key::T: return "T";
    case Key::U: return "U";
    case Key::V: return "V";
    case Key::W: return "W";
    case Key::X: return "X";
    case Key::Y: return "Y";
    case Key::Z: return "Z";
    case Key::Num0: return "Num0";
    case Key::Num1: return "Num1";
    case Key::Num2: return "Num2";
    case Key::Num3: return "Num3";
    case Key::Num4: return "Num4";
    case Key::Num5: return "Num5";
    case Key::Num6: return "Num6";
    case Key::Num7: return "Num7";
    case Key::Num8: return "Num8";
    case Key::Num9: return "Num9";
    case Key::F1: return "F1";
    case Key::F2: return "F2";
    case Key::F3: return "F3";
    case Key::F4: return "F4";
    case Key::F5: return "F5";
    case Key::F6: return "F6";
    case Key::F7: return "F7";
    case Key::F8: return "F8";
    case Key::F9: return "F9";
    case Key::F10: return "F10";
    case Key::F11: return "F11";
    case Key::F12: return "F12";
    case Key::Escape: return "Escape";
    case Key::Enter: return "Enter";
    case Key::Space: return "Space";
    case Key::Tab: return "Tab";
    case Key::Backspace: return "Backspace";
    case Key::Delete: return "Delete";
    case Key::Insert: return "Insert";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::PageUp: return "PageUp";
    case Key::PageDown: return "PageDown";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::LeftShift: return "LeftShift";
    case Key::RightShift: return "RightShift";
    case Key::LeftControl: return "LeftControl";
    case Key::RightControl: return "RightControl";
    case Key::LeftAlt: return "LeftAlt";
    case Key::RightAlt: return "RightAlt";
    case Key::LeftSuper: return "LeftSuper";
    case Key::RightSuper: return "RightSuper";
    case Key::Minus: return "Minus";
    case Key::Equals: return "Equals";
    case Key::LeftBracket: return "LeftBracket";
    case Key::RightBracket: return "RightBracket";
    case Key::Backslash: return "Backslash";
    case Key::Semicolon: return "Semicolon";
    case Key::Apostrophe: return "Apostrophe";
    case Key::Grave: return "Grave";
    case Key::Comma: return "Comma";
    case Key::Period: return "Period";
    case Key::Slash: return "Slash";
    case Key::Count: return "Count";
    }
    return "Unrecognised";
}

std::string_view to_string(MouseButton button) noexcept {
    switch (button) {
    case MouseButton::Left: return "Left";
    case MouseButton::Middle: return "Middle";
    case MouseButton::Right: return "Right";
    case MouseButton::X1: return "X1";
    case MouseButton::X2: return "X2";
    case MouseButton::Count: return "Count";
    }
    return "Unrecognised";
}

}  // namespace atlas::platform
