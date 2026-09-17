// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/platform/platform.hpp>

#include "sdl_error.hpp"
#include "sdl_gamepadmap.hpp"
#include "sdl_keymap.hpp"
#include "text_split.hpp"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace atlas::platform {
namespace {

constexpr log::Category kPlatform{"platform"};

/// Events the queue can hold before a pump reallocates. A frame produces a handful; this is
/// sized so that even a burst of motion events does not allocate after warm-up.
constexpr std::size_t kEventReserve = 256;

[[nodiscard]] KeyModifiers translate_modifiers(SDL_Keymod mod) noexcept {
    return KeyModifiers{
        .shift = (mod & SDL_KMOD_SHIFT) != 0,
        .control = (mod & SDL_KMOD_CTRL) != 0,
        .alt = (mod & SDL_KMOD_ALT) != 0,
        .super = (mod & SDL_KMOD_GUI) != 0,
    };
}

[[nodiscard]] MouseButton translate_mouse_button(std::uint8_t button) noexcept {
    switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::Left;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    case SDL_BUTTON_RIGHT: return MouseButton::Right;
    case SDL_BUTTON_X1: return MouseButton::X1;
    case SDL_BUTTON_X2: return MouseButton::X2;
    default: return MouseButton::Count;  // Filtered out by the caller.
    }
}

}  // namespace

Result<Platform> Platform::create(const PlatformConfig& config) {
    // Whichever thread creates the platform is the main thread from here on. Doing it here
    // rather than asking the application to remember means the affinity assertions are
    // always meaningful.
    mark_main_thread();

    if (!config.video_driver.empty()) {
        const std::string driver{config.video_driver};
        if (!SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver.c_str())) {
            ATLAS_LOG_WARN(kPlatform, "could not request video driver '{}'; using the default",
                           driver);
        }
    }

    // SDL's default response to a failed internal assertion is a modal dialog with Retry,
    // Break, Abort and Ignore. In an automated run there is nobody to click it, so the
    // process hangs until something times out, which is a far worse failure than a crash:
    // it hides what went wrong behind a stall. Aborting fails loudly and leaves a core
    // dump. SDL_SetHint respects an existing environment override, so a developer who
    // wants the dialog can still ask for it.
    SDL_SetHint(SDL_HINT_ASSERT, "abort");

    if (!config.app_name.empty()) {
        const std::string name{config.app_name};
        SDL_SetHint(SDL_HINT_APP_NAME, name.c_str());
    }

    // Events are always needed. Video is what a headless run does without, and skipping it
    // is what lets the process run where there is no display at all.
    SDL_InitFlags flags = SDL_INIT_EVENTS;
    if (config.video) {
        flags |= SDL_INIT_VIDEO;
    }

    if (!SDL_Init(flags)) {
        return std::unexpected(detail::sdl_error(ErrorCode::PlatformInitFailed,
                                                 config.video ? "initialising SDL with video failed"
                                                              : "initialising SDL failed"));
    }

    Platform platform;
    platform.m_initialised = true;

    if (config.gamepad) {
        // A separate subsystem rather than a flag on the main init, so that a machine with no
        // input devices, or one that refuses to enumerate them, still gets its window.
        if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            platform.m_gamepad = true;
        } else {
            ATLAS_LOG_WARN(kPlatform, "the gamepad subsystem did not start: {}", SDL_GetError());
        }
    }
    platform.m_video = config.video;
    platform.m_events.reserve(kEventReserve);

    if (config.video) {
        const char* driver = SDL_GetCurrentVideoDriver();
        platform.m_video_driver = (driver != nullptr) ? driver : "";
        ATLAS_LOG_INFO(kPlatform, "platform ready: video driver '{}'", platform.m_video_driver);
    } else {
        ATLAS_LOG_INFO(kPlatform, "platform ready: headless, no video subsystem");
    }

    return platform;
}

Platform::~Platform() {
    // Deliberately silent. Formatting a log message allocates, so logging here would give
    // the destructor a path that can throw, and a throwing destructor during unwinding ends
    // the process. Platform lifetime is already visible from the "platform ready" line at
    // creation and from whatever the application logs as it shuts down.
    if (m_initialised) {
        // Closed before SDL_Quit so that an open pad is released rather than torn down with
        // the subsystem, and so a moved-from platform does not close a handle it gave away.
        for (void*& handle : m_gamepad_handle) {
            if (handle != nullptr) {
                SDL_CloseGamepad(static_cast<SDL_Gamepad*>(handle));
                handle = nullptr;
            }
        }
        SDL_Quit();
        m_initialised = false;
    }
}

Platform::Platform(Platform&& other) noexcept
    : m_initialised(std::exchange(other.m_initialised, false)),
      m_video(std::exchange(other.m_video, false)),
      m_quit_requested(std::exchange(other.m_quit_requested, false)),
      m_gamepad(std::exchange(other.m_gamepad, false)),
      m_video_driver(std::move(other.m_video_driver)), m_events(std::move(other.m_events)),
      m_input(other.m_input),
      // Taken, not copied: two platforms holding the same open pad would close it twice.
      m_gamepad_instance(std::exchange(other.m_gamepad_instance, {})),
      m_gamepad_handle(std::exchange(other.m_gamepad_handle, {})) {}

Platform& Platform::operator=(Platform&& other) noexcept {
    if (this != &other) {
        if (m_initialised) {
            for (void*& handle : m_gamepad_handle) {
                if (handle != nullptr) {
                    SDL_CloseGamepad(static_cast<SDL_Gamepad*>(handle));
                    handle = nullptr;
                }
            }
            SDL_Quit();
        }
        m_initialised = std::exchange(other.m_initialised, false);
        m_video = std::exchange(other.m_video, false);
        m_quit_requested = std::exchange(other.m_quit_requested, false);
        m_video_driver = std::move(other.m_video_driver);
        m_gamepad = std::exchange(other.m_gamepad, false);
        m_events = std::move(other.m_events);
        m_input = other.m_input;
        m_gamepad_instance = std::exchange(other.m_gamepad_instance, {});
        m_gamepad_handle = std::exchange(other.m_gamepad_handle, {});
    }
    return *this;
}

// NOLINTNEXTLINE(readability-make-member-function-const): see the header.
Result<Window> Platform::create_window(const WindowDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (!m_video) {
        // A stub window that silently does nothing would turn a configuration mistake into
        // a mystery later. Failing here says exactly what is wrong.
        return std::unexpected(
            Error(ErrorCode::DisplayUnavailable,
                  "cannot create a window: this platform was created without video. "
                  "Set PlatformConfig::video, or run headless without windows."));
    }

    if (desc.width == 0 || desc.height == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("window size must be non-zero, got {}x{}", desc.width, desc.height)));
    }

    SDL_WindowFlags flags = 0;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.high_dpi) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (desc.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    const std::string title{desc.title};
    SDL_Window* handle = SDL_CreateWindow(title.c_str(), static_cast<int>(desc.width),
                                          static_cast<int>(desc.height), flags);
    if (handle == nullptr) {
        return std::unexpected(
            detail::sdl_error(ErrorCode::WindowCreationFailed, "creating the window failed"));
    }

    Window window;
    window.m_handle = handle;
    window.m_id = static_cast<WindowId>(SDL_GetWindowID(handle));
    window.m_title = title;

    ATLAS_LOG_INFO(kPlatform, "window '{}' created: id={} logical={}x{} pixels={}x{} scale={}",
                   title, window.id(), window.size().width, window.size().height,
                   window.pixel_size().width, window.pixel_size().height, window.display_scale());

    return window;
}

std::size_t Platform::gamepad_slot(std::uint32_t instance) const noexcept {
    for (std::size_t slot = 0; slot < kMaxGamepads; ++slot) {
        if (m_gamepad_handle[slot] != nullptr && m_gamepad_instance[slot] == instance) {
            return slot;
        }
    }
    return kMaxGamepads;
}

std::span<const Event> Platform::pump() {
    ATLAS_ZONE_NAMED("Platform::pump");
    ATLAS_ASSERT_MAIN_THREAD();

    m_events.clear();
    m_input.begin_frame();

    SDL_Event sdl_event;
    while (SDL_PollEvent(&sdl_event)) {
        switch (sdl_event.type) {
        case SDL_EVENT_QUIT:
            m_quit_requested = true;
            m_events.emplace_back(QuitRequested{});
            break;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_events.emplace_back(WindowCloseRequested{.window = sdl_event.window.windowID});
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            // Both the logical and the pixel size are reported, because a caller that
            // needs one almost always needs the other, and fetching the second one
            // later is where the high-DPI bugs come from.
            SDL_Window* window = SDL_GetWindowFromID(sdl_event.window.windowID);
            Extent2D logical;
            Extent2D pixels;
            if (window != nullptr) {
                int width = 0;
                int height = 0;
                if (SDL_GetWindowSize(window, &width, &height)) {
                    logical = {.width = static_cast<std::uint32_t>(width),
                               .height = static_cast<std::uint32_t>(height)};
                }
                if (SDL_GetWindowSizeInPixels(window, &width, &height)) {
                    pixels = {.width = static_cast<std::uint32_t>(width),
                              .height = static_cast<std::uint32_t>(height)};
                }
            }
            m_events.emplace_back(WindowResized{
                .window = sdl_event.window.windowID, .size = logical, .pixel_size = pixels});
            break;
        }

        case SDL_EVENT_WINDOW_MINIMIZED:
            m_events.emplace_back(WindowMinimized{.window = sdl_event.window.windowID});
            break;

        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            m_events.emplace_back(WindowRestored{.window = sdl_event.window.windowID});
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            m_events.emplace_back(WindowFocusGained{.window = sdl_event.window.windowID});
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            m_events.emplace_back(WindowFocusLost{.window = sdl_event.window.windowID});
            break;

        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
            SDL_Window* window = SDL_GetWindowFromID(sdl_event.window.windowID);
            const float scale = (window != nullptr) ? SDL_GetWindowDisplayScale(window) : 1.0F;
            m_events.emplace_back(WindowDisplayScaleChanged{.window = sdl_event.window.windowID,
                                                            .scale = scale > 0.0F ? scale : 1.0F});
            break;
        }

        case SDL_EVENT_KEY_DOWN: {
            const Key key = detail::from_sdl_scancode(sdl_event.key.scancode);
            const KeyModifiers modifiers = translate_modifiers(sdl_event.key.mod);
            m_input.m_modifiers = modifiers;
            if (key != Key::Unknown) {
                m_input.set_key(key, true);
                m_events.emplace_back(
                    KeyPressed{.key = key, .modifiers = modifiers, .repeat = sdl_event.key.repeat});
            }
            break;
        }

        case SDL_EVENT_KEY_UP: {
            const Key key = detail::from_sdl_scancode(sdl_event.key.scancode);
            const KeyModifiers modifiers = translate_modifiers(sdl_event.key.mod);
            m_input.m_modifiers = modifiers;
            if (key != Key::Unknown) {
                m_input.set_key(key, false);
                m_events.emplace_back(KeyReleased{.key = key, .modifiers = modifiers});
            }
            break;
        }

        case SDL_EVENT_GAMEPAD_ADDED: {
            // The window system sends button and axis events only for a gamepad that has been
            // opened, so opening it here is what makes the rest of this work rather than an
            // optimisation.
            const std::uint32_t instance = sdl_event.gdevice.which;
            std::size_t slot = kMaxGamepads;
            for (std::size_t candidate = 0; candidate < kMaxGamepads; ++candidate) {
                if (m_gamepad_handle[candidate] == nullptr) {
                    slot = candidate;
                    break;
                }
            }
            if (slot == kMaxGamepads) {
                ATLAS_LOG_WARN(kPlatform, "a gamepad was ignored: all {} slots are taken",
                               kMaxGamepads);
                break;
            }
            SDL_Gamepad* pad = SDL_OpenGamepad(instance);
            if (pad == nullptr) {
                ATLAS_LOG_WARN(kPlatform, "a gamepad could not be opened: {}", SDL_GetError());
                break;
            }
            m_gamepad_handle[slot] = pad;
            m_gamepad_instance[slot] = instance;
            m_input.set_gamepad_connected(static_cast<GamepadId>(slot), true);
            const char* name = SDL_GetGamepadName(pad);
            ATLAS_LOG_INFO(kPlatform, "gamepad '{}' connected in slot {}",
                           name != nullptr ? name : "unknown", slot);
            m_events.emplace_back(GamepadConnected{.gamepad = static_cast<GamepadId>(slot)});
            break;
        }

        case SDL_EVENT_GAMEPAD_REMOVED: {
            const std::uint32_t instance = sdl_event.gdevice.which;
            for (std::size_t slot = 0; slot < kMaxGamepads; ++slot) {
                if (m_gamepad_handle[slot] == nullptr || m_gamepad_instance[slot] != instance) {
                    continue;
                }
                SDL_CloseGamepad(static_cast<SDL_Gamepad*>(m_gamepad_handle[slot]));
                m_gamepad_handle[slot] = nullptr;
                m_gamepad_instance[slot] = 0;
                // Clears whatever the pad was holding, so an unplugged stick stops moving.
                m_input.set_gamepad_connected(static_cast<GamepadId>(slot), false);
                ATLAS_LOG_INFO(kPlatform, "the gamepad in slot {} was disconnected", slot);
                m_events.emplace_back(GamepadDisconnected{.gamepad = static_cast<GamepadId>(slot)});
                break;
            }
            break;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const auto slot = gamepad_slot(sdl_event.gbutton.which);
            const GamepadButton button =
                detail::from_sdl_button(static_cast<SDL_GamepadButton>(sdl_event.gbutton.button));
            if (slot == kMaxGamepads || button == GamepadButton::Count) {
                break;
            }
            const bool down = sdl_event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            m_input.set_gamepad_button(static_cast<GamepadId>(slot), button, down);
            const auto id = static_cast<GamepadId>(slot);
            if (down) {
                m_events.emplace_back(GamepadButtonPressed{.gamepad = id, .button = button});
            } else {
                m_events.emplace_back(GamepadButtonReleased{.gamepad = id, .button = button});
            }
            break;
        }

        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const auto slot = gamepad_slot(sdl_event.gaxis.which);
            const GamepadAxis axis =
                detail::from_sdl_axis(static_cast<SDL_GamepadAxis>(sdl_event.gaxis.axis));
            if (slot == kMaxGamepads || axis == GamepadAxis::Count) {
                break;
            }
            // State only: there is no axis event, for the reason event.hpp gives.
            m_input.set_gamepad_axis(static_cast<GamepadId>(slot), axis, sdl_event.gaxis.value);
            break;
        }

        case SDL_EVENT_TEXT_INPUT:
            // The library owns this string and releases it after delivery, so the bytes are
            // copied here rather than referenced. Splitting is in text_split.hpp, where it can
            // be tested without a window system.
            if (sdl_event.text.text != nullptr) {
                detail::append_text_input(m_events, sdl_event.text.text);
            }
            break;

        case SDL_EVENT_TEXT_EDITING: {
            // A composition replaces the previous one, so this is truncated rather than split.
            TextEditing editing;
            const std::string_view text = sdl_event.edit.text != nullptr
                                              ? std::string_view{sdl_event.edit.text}
                                              : std::string_view{};
            const std::size_t take = detail::utf8_prefix_length(text, TextInput::kCapacity);
            std::ranges::copy(text.substr(0, take), editing.bytes.begin());
            editing.length = static_cast<std::uint8_t>(take);
            editing.truncated = take < text.size();
            editing.selection_start = sdl_event.edit.start;
            editing.selection_length = sdl_event.edit.length;
            m_events.emplace_back(editing);
            break;
        }

        case SDL_EVENT_MOUSE_MOTION: {
            const Point2D position{.x = sdl_event.motion.x, .y = sdl_event.motion.y};
            m_input.m_mouse_position = position;
            m_input.m_mouse_delta_x += sdl_event.motion.xrel;
            m_input.m_mouse_delta_y += sdl_event.motion.yrel;
            m_events.emplace_back(MouseMoved{.position = position,
                                             .delta_x = sdl_event.motion.xrel,
                                             .delta_y = sdl_event.motion.yrel});
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const MouseButton button = translate_mouse_button(sdl_event.button.button);
            if (button != MouseButton::Count) {
                m_input.set_mouse_button(button, true);
                m_events.emplace_back(MouseButtonPressed{
                    .button = button,
                    .position = {.x = sdl_event.button.x, .y = sdl_event.button.y},
                    .clicks = sdl_event.button.clicks});
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const MouseButton button = translate_mouse_button(sdl_event.button.button);
            if (button != MouseButton::Count) {
                m_input.set_mouse_button(button, false);
                m_events.emplace_back(MouseButtonReleased{
                    .button = button,
                    .position = {.x = sdl_event.button.x, .y = sdl_event.button.y}});
            }
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
            m_input.m_wheel_delta_x += sdl_event.wheel.x;
            m_input.m_wheel_delta_y += sdl_event.wheel.y;
            m_events.emplace_back(
                MouseWheel{.delta_x = sdl_event.wheel.x, .delta_y = sdl_event.wheel.y});
            break;

        default:
            // Deliberately ignored. SDL reports far more than Atlas currently models,
            // and translating events nothing consumes would be work without a caller.
            break;
        }
    }

    return m_events;
}

}  // namespace atlas::platform
