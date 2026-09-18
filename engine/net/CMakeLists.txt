# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::net — deterministic lockstep over the command queue, and nothing else (ADR-0014).
#
# Holds what is genuinely about peers: a bounded inbox, the message codec, an in-memory link,
# and a session. The turn gate and the command-source interface live in simulation instead,
# because a mod is also a command source and must not depend on networking for an interface
# about the command queue.
#
# No transport, by decision. There is no socket in this milestone and nothing here reaches
# vcpkg.json; what this module provides is the shape a transport would plug into, and a
# loopback link that exercises it hard enough to be worth something.
atlas_add_module(net
    SOURCES
        src/inbox.cpp
        src/loopback.cpp
        src/session.cpp
        src/message.cpp
        src/protocol.cpp
    HEADERS
        include/atlas/net/inbox.hpp
        include/atlas/net/loopback.hpp
        include/atlas/net/message.hpp
        include/atlas/net/protocol.hpp
        include/atlas/net/session.hpp
    DEPENDS
        core
        simulation
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
