# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::audio — the output device, the mixer, and the voices sounding on it.
#
# Depends on platform because the platform owns SDL's lifetime and brings the audio subsystem
# up; this module opens a device on it, exactly as rhi opens a graphics device on a window it
# did not create. It does not depend on renderer, scene or simulation, and must not: a sound is
# triggered by whoever observes state, never by state itself, and that separation is what keeps
# audio out of every hash.
#
# The mixer and the generator are pure functions in their own headers, so the arithmetic is
# tested with no device on every machine rather than only on one with a sound card.
find_package(SDL3 CONFIG REQUIRED)
atlas_define_sdl3_target()

atlas_add_module(audio
    SOURCES
        src/mix.cpp
        src/synth.cpp
        src/device.cpp
    HEADERS
        include/atlas/audio/device.hpp
        include/atlas/audio/mix.hpp
        include/atlas/audio/synth.hpp
    DEPENDS
        core
        assets
        platform
    PRIVATE_DEPS
        atlas::sdl3
)

add_subdirectory(tests)
