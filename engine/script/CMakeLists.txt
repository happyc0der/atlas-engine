# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::script — the sandbox untrusted mods run in (ADR-0015), and nothing else.
#
# The one module in this engine that embeds a third-party runtime whose whole purpose is to run
# code nobody here wrote. Everything it exposes is about refusing: a size, a validation, an
# import list, an export list, a memory ceiling and an instruction budget, in that order,
# because each is cheaper than the one after it and none of them is useful once the guest has
# started.
#
# WAMR links PRIVATE and no WebAssembly type appears in any header here. That is what keeps the
# overlay port replaceable: ADR-0015 names Luau as a fallback with a trigger, and a fallback is
# only real if taking it does not change this module's public surface.
find_package(iwasm CONFIG REQUIRED)

atlas_add_module(script
    SOURCES
        src/runtime.cpp
        src/mod.cpp
        src/mod_host.cpp
    HEADERS
        include/atlas/script/atlas_mod.h
        include/atlas/script/limits.hpp
        include/atlas/script/mod.hpp
        include/atlas/script/mod_host.hpp
        include/atlas/script/runtime.hpp
    DEPENDS
        core
        simulation
        assets
    PRIVATE_DEPS
        iwasm::vmlib
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
