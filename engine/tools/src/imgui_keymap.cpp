// SPDX-License-Identifier: GPL-3.0-or-later
#include "imgui_keymap.hpp"

namespace atlas::tools::detail {

ImGuiKey to_imgui_key(platform::Key key) noexcept {
    using platform::Key;
    switch (key) {
    // Letters and digits, so text-field shortcuts work. The characters themselves arrive as
    // TextInput; these are what Ctrl and Cmd combine with.
    case Key::A: return ImGuiKey_A;
    case Key::B: return ImGuiKey_B;
    case Key::C: return ImGuiKey_C;
    case Key::D: return ImGuiKey_D;
    case Key::E: return ImGuiKey_E;
    case Key::F: return ImGuiKey_F;
    case Key::G: return ImGuiKey_G;
    case Key::H: return ImGuiKey_H;
    case Key::I: return ImGuiKey_I;
    case Key::J: return ImGuiKey_J;
    case Key::K: return ImGuiKey_K;
    case Key::L: return ImGuiKey_L;
    case Key::M: return ImGuiKey_M;
    case Key::N: return ImGuiKey_N;
    case Key::O: return ImGuiKey_O;
    case Key::P: return ImGuiKey_P;
    case Key::Q: return ImGuiKey_Q;
    case Key::R: return ImGuiKey_R;
    case Key::S: return ImGuiKey_S;
    case Key::T: return ImGuiKey_T;
    case Key::U: return ImGuiKey_U;
    case Key::V: return ImGuiKey_V;
    case Key::W: return ImGuiKey_W;
    case Key::X: return ImGuiKey_X;
    case Key::Y: return ImGuiKey_Y;
    case Key::Z: return ImGuiKey_Z;
    case Key::Num0: return ImGuiKey_0;
    case Key::Num1: return ImGuiKey_1;
    case Key::Num2: return ImGuiKey_2;
    case Key::Num3: return ImGuiKey_3;
    case Key::Num4: return ImGuiKey_4;
    case Key::Num5: return ImGuiKey_5;
    case Key::Num6: return ImGuiKey_6;
    case Key::Num7: return ImGuiKey_7;
    case Key::Num8: return ImGuiKey_8;
    case Key::Num9: return ImGuiKey_9;

    case Key::F1: return ImGuiKey_F1;
    case Key::F2: return ImGuiKey_F2;
    case Key::F3: return ImGuiKey_F3;
    case Key::F4: return ImGuiKey_F4;
    case Key::F5: return ImGuiKey_F5;
    case Key::F6: return ImGuiKey_F6;
    case Key::F7: return ImGuiKey_F7;
    case Key::F8: return ImGuiKey_F8;
    case Key::F9: return ImGuiKey_F9;
    case Key::F10: return ImGuiKey_F10;
    case Key::F11: return ImGuiKey_F11;
    case Key::F12: return ImGuiKey_F12;

    case Key::Escape: return ImGuiKey_Escape;
    case Key::Enter: return ImGuiKey_Enter;
    case Key::Space: return ImGuiKey_Space;
    case Key::Tab: return ImGuiKey_Tab;
    case Key::Backspace: return ImGuiKey_Backspace;
    case Key::Delete: return ImGuiKey_Delete;
    case Key::Insert: return ImGuiKey_Insert;
    case Key::Home: return ImGuiKey_Home;
    case Key::End: return ImGuiKey_End;
    case Key::PageUp: return ImGuiKey_PageUp;
    case Key::PageDown: return ImGuiKey_PageDown;
    case Key::Left: return ImGuiKey_LeftArrow;
    case Key::Right: return ImGuiKey_RightArrow;
    case Key::Up: return ImGuiKey_UpArrow;
    case Key::Down: return ImGuiKey_DownArrow;

    case Key::LeftShift: return ImGuiKey_LeftShift;
    case Key::RightShift: return ImGuiKey_RightShift;
    case Key::LeftControl: return ImGuiKey_LeftCtrl;
    case Key::RightControl: return ImGuiKey_RightCtrl;
    case Key::LeftAlt: return ImGuiKey_LeftAlt;
    case Key::RightAlt: return ImGuiKey_RightAlt;
    case Key::LeftSuper: return ImGuiKey_LeftSuper;
    case Key::RightSuper: return ImGuiKey_RightSuper;

    // Punctuation, for the editing shortcuts that use it and so a field sees the same key the
    // user pressed.
    case Key::Minus: return ImGuiKey_Minus;
    case Key::Equals: return ImGuiKey_Equal;
    case Key::LeftBracket: return ImGuiKey_LeftBracket;
    case Key::RightBracket: return ImGuiKey_RightBracket;
    case Key::Backslash: return ImGuiKey_Backslash;
    case Key::Semicolon: return ImGuiKey_Semicolon;
    case Key::Apostrophe: return ImGuiKey_Apostrophe;
    case Key::Grave: return ImGuiKey_GraveAccent;
    case Key::Comma: return ImGuiKey_Comma;
    case Key::Period: return ImGuiKey_Period;
    case Key::Slash: return ImGuiKey_Slash;

    // The sentinels. Named rather than left to a default, so that adding a key to the platform
    // enum fails to compile here instead of silently mapping to nothing.
    case Key::Unknown:
    case Key::Count: return ImGuiKey_None;
    }
    return ImGuiKey_None;
}

int to_imgui_button(platform::MouseButton button) noexcept {
    switch (button) {
    case platform::MouseButton::Left: return 0;
    case platform::MouseButton::Right: return 1;
    case platform::MouseButton::Middle: return 2;
    // The extra buttons are modelled by the platform and not by the overlay, which has three.
    case platform::MouseButton::X1:
    case platform::MouseButton::X2:
    case platform::MouseButton::Count: return -1;
    }
    return -1;
}

}  // namespace atlas::tools::detail
