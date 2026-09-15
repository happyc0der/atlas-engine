# SPDX-License-Identifier: GPL-3.0-or-later

# Pure tests: no SDL, no display, no platform instance.
atlas_add_test(platform_input
    SOURCES
        test_input.cpp
        test_event.cpp
        test_key.cpp
    DEPENDS
        atlas::platform
    LABELS
        unit
)

# The scancode mapping sits directly on the SDL boundary, so testing it needs SDL and the
# private header. Legitimate for a test; it is not a public header.
atlas_add_test(platform_keymap
    SOURCES
        test_keymap.cpp
    DEPENDS
        atlas::platform
    PRIVATE_DEPS
        atlas::sdl3
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    LABELS
        unit
)

# Creates a real platform, and under the dummy video driver a real window, with no display
# attached. Labelled "display" so that it can be excluded where even that is unavailable,
# though it is expected to run everywhere including continuous integration.
atlas_add_test(platform_lifetime
    SOURCES
        test_platform.cpp
        test_window_dummy.cpp
        test_input_events.cpp
    DEPENDS
        atlas::platform
    PRIVATE_DEPS
        atlas::sdl3
    INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    LABELS
        "display"
)
